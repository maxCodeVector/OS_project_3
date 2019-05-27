#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "threads/synch.h"

/* A cached block. */
struct cache_block 
  {
    /* Locking to prevent eviction. */
    struct lock block_lock;                    /* Protects fields in group. */
    struct condition no_readers_or_writers; /* readers == 0 && writers == 0 */
    struct condition no_writers;                            /* writers == 0 */
    int readers, read_waiters;          /* # of readers, # waiting to read. */
    int writers, write_waiters; /* # of writers (<= 1), # waiting to write. */

    /* Sector number.  INVALID_SECTOR indicates a free cache block.

       Changing from free to allocated requires cache_sync.

       Changing from allocated to free requires block_lock, block
       must be up-to-date and not dirty, and no one may be
       waiting on it. */
    block_sector_t sector;

    /* Is data[] correct?
       Requires write lock or data_lock. */
    bool up_to_date;

    /* Does data[] need to be written back to disk?
       Valid only when up-to-date.
       Requires read lock or write lock or data_lock. */
    bool dirty;

    /* Sector data.
       Access to data[] requires up-to-date and read or write lock.
       Bringing up-to-date requires write lock or data_lock. */
    struct lock data_lock;              /* Protects fields in group. */
    uint8_t data[BLOCK_SECTOR_SIZE];    /* Disk data. */
  };



/* Type of block lock. */
enum lock_type 
  {
    NON_EXCLUSIVE,	/* Any number of lockers. */
    EXCLUSIVE		/* Only one locker. */
  };

void cache_init (void);
void cache_flush (void);
struct cache_block *cache_lock (block_sector_t, enum lock_type);
void *cache_read (struct cache_block *);
void *cache_zero (struct cache_block *);
void cache_dirty (struct cache_block *);
void cache_unlock (struct cache_block *);
void cache_free (block_sector_t);
void cache_read_ahead (block_sector_t);


#endif