#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Poiters number */
#define PTRS_PER_BLOCK 128

#define DIRECT_POINTER_NUM 12
#define SINGLE_POINTER_NUM 1
#define DOUBLE_POINTER_NUM 1
#define TOTAL_POINTER_NUM (DIRECT_POINTER_NUM + SINGLE_POINTER_NUM + DOUBLE_POINTER_NUM)

#define MAX_FILE_SIZE 8460288 // in bytes

off_t inode_extend(struct inode *inode, off_t new_length);
size_t inode_expand_single_block(struct inode *inode, size_t needed_allocated_sectors);
size_t inode_expand_double_block(struct inode *inode, size_t needed_allocated_sectors);
size_t inode_expand_double_block2(struct inode *inode, size_t needed_allocated_sectors,
        block_sector_t *level1_block);
void deallocate_inode(struct inode *inode);
void inode_dealloc_double_indirect_block(block_sector_t *ptr, size_t level1_sectors, 
      size_t level0_sectors);
void inode_dealloc_indirect_block(block_sector_t *ptr, size_t data_ptrs);











/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    block_sector_t pointers[TOTAL_POINTER_NUM];

    uint32_t level0_ptr_index;                  /* index of the pointer list */
    uint32_t level1_ptr_index;               /* index of the level 1 pointer table */
    uint32_t level2_ptr_index;               /* index of the level 2 pointer table */
    uint32_t is_file;                    /* 1 for file, 0 for dir */
    uint32_t unused[122 - TOTAL_POINTER_NUM];               /* Not used. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* added by Lu*/
static size_t
bytes_to_indirect_sectors(off_t size)
{
  if (size <= BLOCK_SECTOR_SIZE * DIRECT_POINTER_NUM)
  {
    return 0;
  }
  size -= BLOCK_SECTOR_SIZE * DIRECT_POINTER_NUM;
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE * PTRS_PER_BLOCK);
}

static size_t bytes_to_double_indirect_sector(off_t size)
{
  if (size <= BLOCK_SECTOR_SIZE * (DIRECT_POINTER_NUM + PTRS_PER_BLOCK))
  {
    return 0;
  }
  return 1;
}

/* NEED: return the correct sector with indirect map */ 

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */

    struct lock extend_lock;
    size_t level0_ptr_index;                  /* index of the pointer list */
    size_t level1_ptr_index;               /* index of the level 1 pointer table */
    size_t level2_ptr_index;               /* index of the level 2 pointer table */
    off_t length;                       /* File size in bytes. */
    off_t length_for_read; 
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);

  if(pos < inode->data.length) {
    if(pos < DIRECT_POINTER_NUM * BLOCK_SECTOR_SIZE) 
    {
      return inode->data.pointers[pos / BLOCK_SECTOR_SIZE];
    } 
    else if (pos < (PTRS_PER_BLOCK + DIRECT_POINTER_NUM) * BLOCK_SECTOR_SIZE) 
    {
      uint32_t level1_index;
      uint32_t level1_table[PTRS_PER_BLOCK];
      pos -= DIRECT_POINTER_NUM * BLOCK_SECTOR_SIZE;
      level1_index = pos / BLOCK_SECTOR_SIZE;
      block_read(fs_device, inode->data.pointers[TOTAL_POINTER_NUM - 2],
                 &level1_table);
      return level1_table[level1_index];
    }
    else 
    {
      uint32_t level_index;
      uint32_t level_table[PTRS_PER_BLOCK];
      // read the first level pointer table
      block_read(fs_device, inode->data.pointers[TOTAL_POINTER_NUM - 1],
                 &level_table);
      pos -= (DIRECT_POINTER_NUM + PTRS_PER_BLOCK) * BLOCK_SECTOR_SIZE;
      level_index = pos / (PTRS_PER_BLOCK * BLOCK_SECTOR_SIZE);
      // read the second level pointer table
      block_read(fs_device, level_table[level_index], &level_table);
      pos -= level_index * (PTRS_PER_BLOCK * BLOCK_SECTOR_SIZE);
      return level_table[pos / BLOCK_SECTOR_SIZE];
    }
  }
  else 
  {
    return -1;
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, uint32_t is_file)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);
  ASSERT (length <= MAX_FILE_SIZE)
  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;

      disk_inode->is_file = is_file;

      if (allocate_inode(disk_inode)) {
        block_write(fs_device, sector, disk_inode);
      }

      // if (free_map_allocate (sectors, &disk_inode->start)) 
      //   {
      //     block_write (fs_device, sector, disk_inode);
      //     if (sectors > 0) 
      //       {
      //         static char zeros[BLOCK_SECTOR_SIZE];
      //         size_t i;
              
      //         for (i = 0; i < sectors; i++) 
      //           block_write (fs_device, disk_inode->start + i, zeros);
      //       }
      //     success = true; 
      //   } 


      free (disk_inode);
    }
  return success;
}




/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;

  /* added by Lu*/
  lock_init(&inode->extend_lock);
  block_read (fs_device, inode->sector, &inode->data);
  inode->length = inode->data.length;
  inode->level0_ptr_index = inode->data.level0_ptr_index;
  inode->level1_ptr_index = inode->data.level1_ptr_index;
  inode->level2_ptr_index = inode->data.level2_ptr_index;


  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          deallocate_inode (inode);
        }
        else 
        {
      //           struct inode_disk disk_inode = {
      //     .length = inode->length,
      //     .magic = INODE_MAGIC,
      //     .direct_index = inode->direct_index,
      //     .indirect_index = inode->indirect_index,
      //     .double_indirect_index = inode->double_indirect_index,
      //     .isdir = inode->isdir,
      //     .parent = inode->parent,
      // };
      // memcpy(&disk_inode.ptr, &inode->ptr,
      //        INODE_BLOCK_PTRS * sizeof(block_sector_t));
      // block_write(fs_device, inode->sector, &disk_inode);
          block_write(fs_device, inode->sector, &inode->data);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.)
   implement now by Lu */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  // extend the file
  if (offset + size > inode_length(inode))
  {
    if (inode->data.is_file)
    {
      lock_acquire(&inode->extend_lock);
    }
    inode->length = inode_extend(inode, offset + size);
    inode->data.length = inode->length;
    if (inode->data.is_file)
    {
      lock_release(&inode->extend_lock);
    }
  }

  while (size > 0)
  {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode->length - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    struct cache_entry *c = filesys_cache_block_get(sector_idx, true);
    memcpy((uint8_t *)&c->block + sector_ofs, buffer + bytes_written,
           chunk_size);
    c->accessed = true;
    c->dirty = true;
    c->open_cnt--;

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }

  inode->length_for_read = inode->length;
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

/** extend the file size to NEW_LENGTH (in bytes) 
*   return NEW_LENGTH if allocated success
*/
off_t inode_extend(struct inode *inode, off_t new_length)
{
  static char zeros[BLOCK_SECTOR_SIZE];
  size_t needed_allocated_sectors = bytes_to_data_sectors(new_length) -
                            bytes_to_data_sectors(inode->length);

  if (needed_allocated_sectors == 0)
  {
    return new_length;
  }

  /* allocate for the sector that direct pointer points to */
  while (inode->level0_ptr_index < DIRECT_POINTER_NUM)
  {
    free_map_allocate(1, &inode->data.pointers[inode->level0_ptr_index]);
    block_write(fs_device, inode->data.pointers[inode->level0_ptr_index], zeros);
    inode->level0_ptr_index ++;
    needed_allocated_sectors--;
    if (needed_allocated_sectors == 0)
    {
      return new_length;
    }
  }
  /* allocate for the sector of single indirect pointers */
  if (inode->level0_ptr_index == DIRECT_POINTER_NUM)
  {
    needed_allocated_sectors = inode_expand_single_block(inode, needed_allocated_sectors);
    if (needed_allocated_sectors == 0)
    {
      return new_length;
    }
  }
  if (inode->level0_ptr_index == DIRECT_POINTER_NUM + SINGLE_POINTER_NUM)
  {
    needed_allocated_sectors = inode_expand_double_block(inode, needed_allocated_sectors);
  }
  return new_length - needed_allocated_sectors * BLOCK_SECTOR_SIZE;
}

size_t inode_expand_single_block(struct inode *inode, size_t needed_allocated_sectors)
{
  static char zeros[BLOCK_SECTOR_SIZE];
  block_sector_t ptr_block[PTRS_PER_BLOCK];
  if (inode->level1_ptr_index == 0)
  {
    free_map_allocate(1, &inode->data.pointers[inode->level0_ptr_index]);
  }
  else
  {
    block_read(fs_device, inode->data.pointers[inode->level0_ptr_index], &ptr_block);
  }
  while (inode->level1_ptr_index < PTRS_PER_BLOCK)
  {
    free_map_allocate(1, &ptr_block[inode->level1_ptr_index]);
    block_write(fs_device, ptr_block[inode->level1_ptr_index], zeros);
    inode->level1_ptr_index ++;
    needed_allocated_sectors--;
    if (needed_allocated_sectors == 0)
    {
      break;
    }
  }
  block_write(fs_device, inode->data.pointers[inode->level1_ptr_index], &ptr_block);
  if (inode->level1_ptr_index == PTRS_PER_BLOCK)
  {
    inode->level1_ptr_index = 0;
    inode->level0_ptr_index ++;
  }
  return needed_allocated_sectors;
}

size_t inode_expand_double_block(struct inode *inode,
                                          size_t needed_allocated_sectors)
{
  block_sector_t ptr_block[PTRS_PER_BLOCK];
  if (inode->level2_ptr_index == 0 && inode->level1_ptr_index == 0)
  {
    free_map_allocate(1, &inode->data.pointers[inode->level0_ptr_index]);
  }
  else
  {
    block_read(fs_device, inode->data.pointers[inode->level0_ptr_index], &ptr_block);
  }

  while (inode->level1_ptr_index < PTRS_PER_BLOCK)
  {
    needed_allocated_sectors = inode_expand_double_block2(inode,
                                                                  needed_allocated_sectors, &ptr_block);
    if (needed_allocated_sectors == 0)
    {
      break;
    }
  }
  block_write(fs_device, inode->data.pointers[inode->level0_ptr_index], &ptr_block);
  return needed_allocated_sectors;
}

size_t inode_expand_double_block2(struct inode *inode,
                                                  size_t needed_allocated_sectors,
                                                  block_sector_t *level1_block)
{
  
  static char zeros[BLOCK_SECTOR_SIZE];
  block_sector_t level2_block[PTRS_PER_BLOCK];
  if (inode->level2_ptr_index == 0)
  {
    free_map_allocate(1, &level1_block[inode->level1_ptr_index]);
  }
  else
  {
    block_read(fs_device, level1_block[inode->level1_ptr_index],
               &level2_block);
  }
  while (inode->level2_ptr_index < PTRS_PER_BLOCK)
  {
    free_map_allocate(1, &level2_block[inode->level2_ptr_index]);
    block_write(fs_device, level2_block[inode->level2_ptr_index],
                zeros);
    inode->level2_ptr_index ++;
    needed_allocated_sectors--;
    if (needed_allocated_sectors == 0)
    {
      break;
    }
  }
  block_write(fs_device, level1_block[inode->level1_ptr_index], &level2_block);
  if (inode->level2_ptr_index == PTRS_PER_BLOCK)
  {
    inode->level2_ptr_index = 0;
    inode->level1_ptr_index ++;
  }
  return needed_allocated_sectors;
}


void deallocate_inode(struct inode *inode)
{
  size_t level0_sectors = bytes_to_data_sectors(inode->length);
  size_t level1_sectors = bytes_to_indirect_sectors(inode->length);
  size_t level2_sectors = bytes_to_double_indirect_sector(inode->length);
  unsigned int level0_ptr_index = 0;
  while (level0_sectors && level0_ptr_index < DIRECT_POINTER_NUM)
  {
    free_map_release(inode->data.pointers[level0_ptr_index], 1);
    level0_sectors--;
    level0_ptr_index++;
  }
  if (level1_sectors)
  {
    size_t single_data_ptrs = level0_sectors;
    if (single_data_ptrs > PTRS_PER_BLOCK) {
      single_data_ptrs = PTRS_PER_BLOCK;
    }
    inode_dealloc_indirect_block(&inode->data.pointers[level0_ptr_index], single_data_ptrs);
    level0_sectors -= single_data_ptrs;
    level1_sectors --;
    level0_ptr_index++;
  }
  if (level2_sectors)
  {
    inode_dealloc_double_indirect_block(&inode->data.pointers[level0_ptr_index], 
                                 level1_sectors,level0_sectors);
  }
}

void inode_dealloc_double_indirect_block(block_sector_t *ptr,
                                         size_t level1_sectors,
                                         size_t level0_sectors)
{
  unsigned int i;
  block_sector_t ptr_block[PTRS_PER_BLOCK];
  block_read(fs_device, *ptr, &ptr_block);
  for (i = 0; i < level1_sectors; i++)
  {
    size_t data_per_block = PTRS_PER_BLOCK;
    if (data_per_block > level0_sectors) {
      data_per_block = level0_sectors;
    }
    inode_dealloc_indirect_block(&ptr_block[i], data_per_block);
    level0_sectors -= data_per_block;
  }
  free_map_release(*ptr, 1);
}

void inode_dealloc_indirect_block(block_sector_t *ptr, size_t data_ptrs)
{
  block_sector_t ptr_block[PTRS_PER_BLOCK];
  block_read(fs_device, *ptr, &ptr_block);
  for (int i = 0; i < data_ptrs; i++)
  {
    free_map_release(ptr_block[i], 1);
  }
  free_map_release(*ptr, 1);
}