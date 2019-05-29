#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "devices/timer.h"
#include "threads/synch.h"
#include <list.h>

#define WRITE_BACK_WAIT_TIME 5*TIMER_FREQ
#define MAX_FILESYS_CACHE_SIZE 64                       /* maximum cache size of pintos */

struct list filesys_cache;                              /* cache list */
uint32_t filesys_cache_size;                            /* current cache number of pintos */
struct lock filesys_cache_lock;                         

/** cache block
 * 
 * 
 * */
struct cache_entry {
  uint8_t block[BLOCK_SECTOR_SIZE];                     /* actual data from disk */
  block_sector_t sector;                                /* sector on disk where the data resides */
  bool dirty;                                           /* dirty flag, true if the data was changed */
  bool accessed;                                        /* show if the cache is readed */
  int open_cnt;     
  struct list_elem elem;                                /* list element for filesys_cache */
};

void filesys_cache_init (void);
struct cache_entry *block_in_cache (block_sector_t sector);
struct cache_entry* filesys_cache_block_get (block_sector_t sector,
					     bool dirty);
struct cache_entry* filesys_cache_block_evict (block_sector_t sector,
					       bool dirty);

struct cache_entry *find_replace();


void filesys_cache_write_to_disk (bool halt);
void write_cache_back_loop (void *aux);
void thread_func_read_ahead (void *aux);
void spawn_thread_read_ahead (block_sector_t sector);

#endif /* filesys/cache.h */
