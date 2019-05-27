#include "filesys/cache.h"
#include <debug.h>
#include <string.h>
#include "filesys/filesys.h"
#include "devices/timer.h"
#include "threads/malloc.h"

#include "threads/thread.h"


#define INVALID_SECTOR ((block_sector_t) -1)

#define CACHE_SIZE 64
struct cache_block cache[CACHE_SIZE];

/* Cache lock.

   Required to allocate a cache block to a sector, to prevent a
   single sector being allocated two different cache blocks.

   Required to search the cache for a sector, to prevent the
   sector from being added while the search is ongoing.

   Protects hand. */
struct lock cache_sync;

/* Cache eviction hand.
   Protected by cache_sync. */
static int hand = 0;

static void flush_d_init (void);
static void read_ahead_d_init (void);
static void read_ahead_d_submit (block_sector_t sector);

/* Initializes cache. */
void
cache_init (void) 
{
  int i;
  
  lock_init (&cache_sync);
  for (i = 0; i < CACHE_SIZE; i++) 
    {
      struct cache_block *b = &cache[i];
      lock_init (&b->block_lock);
      cond_init (&b->no_readers_or_writers);
      cond_init (&b->no_writers);
      b->readers = b->read_waiters = 0;
      b->writers = b->write_waiters = 0;
      b->sector = INVALID_SECTOR;
      lock_init (&b->data_lock);
    }

  flush_d_init ();
  read_ahead_d_init ();
}

/* Flushes cache to disk. */
void
cache_flush (void) 
{
  int i;
  
  for (i = 0; i < CACHE_SIZE; i++)
    {
      struct cache_block *b = &cache[i];
      block_sector_t sector;
      
      lock_acquire (&b->block_lock);
      sector = b->sector;
      lock_release (&b->block_lock);

      if (sector == INVALID_SECTOR)
        continue;

      b = cache_lock (sector, EXCLUSIVE);
      if (b->up_to_date && b->dirty) 
        {
          block_write (fs_device, b->sector, b->data);
          b->dirty = false; 
        }
      cache_unlock (b);
    }
}

/* Locks the given SECTOR into the cache and returns the cache
   block.
   If TYPE is EXCLUSIVE, then the block returned will be locked
   only by the caller.  The calling thread must not already
   have any lock on the block.
   If TYPE is NON_EXCLUSIVE, then block returned may be locked by
   any number of other callers.  The calling thread may already
   have any number of non-exclusive locks on the block. */
struct cache_block *
cache_lock (block_sector_t sector, enum lock_type type) 
{
  int i;

 try_again:
  lock_acquire (&cache_sync);

  /* Is the block already in-cache? */
  for (i = 0; i < CACHE_SIZE; i++)
    {
      /* Skip any blocks that don't hold SECTOR. */
      struct cache_block *b = &cache[i];
      lock_acquire (&b->block_lock);
      if (b->sector != sector) 
        {
          lock_release (&b->block_lock);
          continue;
        }
      lock_release (&cache_sync);

      /* Get read or write lock. */
      if (type == NON_EXCLUSIVE) 
        {
          /* Lock for read. */
          b->read_waiters++;
          if (b->writers || b->write_waiters)
            do {
              cond_wait (&b->no_writers, &b->block_lock);
            } while (b->writers);
          b->readers++;
          b->read_waiters--;
        }
      else 
        {
          /* Lock for write. */
          b->write_waiters++;
          if (b->readers || b->read_waiters || b->writers)
            do {
              cond_wait (&b->no_readers_or_writers, &b->block_lock);
            } while (b->readers || b->writers);
          b->writers++;
          b->write_waiters--;
        }
      lock_release (&b->block_lock);

      /* Our sector should have been pinned in the cache while we
         were waiting.  Make sure. */
      ASSERT (b->sector == sector);

      return b;
    }

  /* Not in cache.  Find empty slot.
     We hold cache_sync. */
  for (i = 0; i < CACHE_SIZE; i++)
    {
      struct cache_block *b = &cache[i];
      lock_acquire (&b->block_lock);
      if (b->sector == INVALID_SECTOR) 
        {
          /* Drop block_lock, which is no longer needed because
             this is the only code that allocates free blocks,
             and we still have cache_sync.

             We can't drop cache_sync yet because someone else
             might try to allocate this same block (or read from
             it) while we're still initializing the block. */
          lock_release (&b->block_lock);

          b->sector = sector;
          b->up_to_date = false;
          ASSERT (b->readers == 0);
          ASSERT (b->writers == 0);
          if (type == NON_EXCLUSIVE)
            b->readers = 1;
          else
            b->writers = 1;
          lock_release (&cache_sync);
          return b;
        }
      lock_release (&b->block_lock); 
    }

  /* No empty slots.  Evict something.
     We hold cache_sync. */
  for (i = 0; i < CACHE_SIZE; i++)
    {
      struct cache_block *b = &cache[hand];
      if (++hand >= CACHE_SIZE)
        hand = 0;

      /* Try to grab exclusive write access to block. */
      lock_acquire (&b->block_lock);
      if (b->readers || b->writers || b->read_waiters || b->write_waiters) 
        {
          lock_release (&b->block_lock);
          continue;
        }
      b->writers = 1;
      lock_release (&b->block_lock);

      lock_release (&cache_sync);

      /* Write block to disk if dirty. */
      if (b->up_to_date && b->dirty) 
        {
          block_write (fs_device, b->sector, b->data);
          b->dirty = false;
        }

      /* Remove block from cache, if possible: someone might have
         started waiting on it while the lock was released. */
      lock_acquire (&b->block_lock);
      b->writers = 0;
      if (!b->read_waiters && !b->write_waiters) 
        {
          /* No one is waiting for it, so we can free it. */
          b->sector = INVALID_SECTOR; 
        }
      else 
        {
          /* There is a waiter.  Give it the block. */
          if (b->read_waiters)
            cond_broadcast (&b->no_writers, &b->block_lock);
          else
            cond_signal (&b->no_readers_or_writers, &b->block_lock);
        }
      lock_release (&b->block_lock);

      /* Try again. */
      goto try_again;
    }

  /* Wait for cache contention to die down. */
  lock_release (&cache_sync);
  timer_msleep (1000);
  goto try_again;
}

/* Bring block B up-to-date, by reading it from disk if
   necessary, and return a pointer to its data.
   The caller must have an exclusive or non-exclusive lock on
   B. */
void *
cache_read (struct cache_block *b) 
{
  lock_acquire (&b->data_lock);
  if (!b->up_to_date) 
    {
      block_read (fs_device, b->sector, b->data);
      b->up_to_date = true;
      b->dirty = false; 
    }
  lock_release (&b->data_lock);

  return b->data;
}

/* Zero out block B, without reading it from disk, and return a
   pointer to the zeroed data.
   The caller must have an exclusive lock on B. */
void *
cache_zero (struct cache_block *b) 
{
  ASSERT (b->writers);
  memset (b->data, 0, BLOCK_SECTOR_SIZE);
  b->up_to_date = true;
  b->dirty = true;

  return b->data;
}

/* Marks block B as dirty, so that it will be written back to
   disk before eviction.
   The caller must have a read or write lock on B,
   and B must be up-to-date. */
void
cache_dirty (struct cache_block *b) 
{
  ASSERT (b->up_to_date);
  b->dirty = true;
}

/* Unlocks block B.
   If B is no longer locked by any thread, then it becomes a
   candidate for immediate eviction. */
void
cache_unlock (struct cache_block *b) 
{
  lock_acquire (&b->block_lock);
  if (b->readers) 
    {
      ASSERT (b->writers == 0);
      if (--b->readers == 0)
        cond_signal (&b->no_readers_or_writers, &b->block_lock);
    }
  else if (b->writers)
    {
      ASSERT (b->readers == 0);
      ASSERT (b->writers == 1);
      b->writers--;
      if (b->read_waiters)
        cond_broadcast (&b->no_writers, &b->block_lock);
      else
        cond_signal (&b->no_readers_or_writers, &b->block_lock);
    }
  else
    NOT_REACHED ();
  lock_release (&b->block_lock);
}

/* If SECTOR is in the cache, evicts it immediately without
   writing it back to disk (even if dirty).
   The block must be entirely unused. */
void
cache_free (block_sector_t sector) 
{
  int i;
  
  lock_acquire (&cache_sync);
  for (i = 0; i < CACHE_SIZE; i++)
    {
      struct cache_block *b = &cache[i];

      lock_acquire (&b->block_lock);
      if (b->sector == sector) 
        {
          lock_release (&cache_sync);

          /* Only invalidate the block if it's unused.  That
             should be the normal case, but it could be part of a
             read-ahead (in readaheadd()) or write-behind (in
             cache_flush()). */
          if (b->readers == 0 && b->read_waiters == 0
              && b->writers == 0 && b->write_waiters == 0) 
            b->sector = INVALID_SECTOR; 

          lock_release (&b->block_lock);
          return;
        }
      lock_release (&b->block_lock);
    }
  lock_release (&cache_sync);
}

void
cache_read_ahead (block_sector_t sector) 
{
  read_ahead_d_submit (sector);
}

/* Flush daemon. */

static void flush_d (void *aux);

/* Initializes flush daemon. */
static void
flush_d_init (void) 
{
  thread_create ("flushd", PRI_MIN, flush_d, NULL);
}

/* Flush daemon thread. */
static void
flush_d (void *aux UNUSED) 
{
  for (;;) 
    {
      timer_msleep (30 * 1000);
      cache_flush ();
    }
}

/* A block to read ahead. */
struct read_ahead_block 
  {
    struct list_elem list_elem;         /* readahead_list element. */
    block_sector_t sector;              /* Sector to read. */
  };

/* Protects readahead_list.
   Monitor lock for readahead_list_nonempty. */
static struct lock read_ahead_lock;

/* Signaled when a block is added to readahead_list. */
static struct condition read_ahead_list_nonempty;

/* List of blocks for read-ahead. */
static struct list read_ahead_list;

static void read_ahead_d (void *aux);

/* Initialize read-ahead daemon. */
static void
read_ahead_d_init (void) 
{
  lock_init (&read_ahead_lock);
  cond_init (&read_ahead_list_nonempty);
  list_init (&read_ahead_list);
  thread_create ("readaheadd", PRI_MIN, read_ahead_d, NULL);
}

/* Adds SECTOR to the read-ahead queue. */
static void
read_ahead_d_submit (block_sector_t sector) 
{
  /* Allocate readahead block. */
  struct read_ahead_block *block = malloc (sizeof *block);
  if (block == NULL)
    return;
  block->sector = sector;

  /* Add block to list. */
  lock_acquire (&read_ahead_lock);
  list_push_back (&read_ahead_list, &block->list_elem);
  cond_signal (&read_ahead_list_nonempty, &read_ahead_lock);
  lock_release (&read_ahead_lock);
}

/* Read-ahead daemon. */
static void
read_ahead_d (void *aux UNUSED) 
{
  for (;;) 
    {
      struct read_ahead_block *ra_block;
      struct cache_block *cache_block;

      /* Get readahead block from list. */
      lock_acquire (&read_ahead_lock);
      while (list_empty (&read_ahead_list)) 
        cond_wait (&read_ahead_list_nonempty, &read_ahead_lock);
      ra_block = list_entry (list_pop_front (&read_ahead_list),
                             struct read_ahead_block, list_elem);
      lock_release (&read_ahead_lock);

      /* Read block into cache. */
      cache_block = cache_lock (ra_block->sector, NON_EXCLUSIVE);
      cache_read (cache_block);
      cache_unlock (cache_block);
      free (ra_block);
    }
}
