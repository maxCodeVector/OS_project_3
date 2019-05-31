# OS_project_3
file system





## Task 3

#### Data Structure

In inode_disk, add `is_file` to judge if this is a file or dir.

```c
/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    // uint32_t sectors[TOTAL_POINTER_NUM];               /* Not used. */
    uint32_t is_file;                    /* 1 for file, 0 for dir */
    uint32_t not_used[124];
  };
```







#### Algorithm

**create  new directory**

1. Parse the path, if the path is only "/" or thread's current workspace is NULL, then just open the root directory to look up next level dirctory in path. Else, open the corresponding dir, if new open dir is be removed (in `struct inode`, `removed=true`), return failure.

   ```c
     /* Find starting directory. */
     if (name[0] == '/' || thread_current ()->wd == NULL)
       dir = dir_open_root ();
     else
       dir = dir_reopen (thread_current ()->wd);
     if (dir == NULL || !is_dir_exist(dir)){  // check if this directory has been removed
       /* Return failure. */
       dir_close (dir);
       *dirp = NULL;
       base_name[0] = '\0';
       return false;
     }
   ```

2. Create a new inode with allocated sector. In its `strcut inode_disk data`, will allocate some sector to store real data (will mentioned in 3). And write it in disk.

3. Create two entry `.` and `..` , `dir ..` will point to parent sector number, so we can return to last level dir. Those two entry are stored in allocated sector, this sector number are put in disk_inode's attribute `start`.  Until new, we just allocate 320 byte to store data, so the entry can not so much (about 10 dir_enrty). 

4. Add a new dir_entry with the new dir_name as its name and store it in opened dir (mentoind in 1). This dir_entry was point to the same sector as `.` (mentioned in 3).

**remove a directory**

Before remove, we test if it can be remove. It is a file, we don't care about it. It this file is not empty, then we should not remove it.

If it can be removed, then just remove it.

**open a dirctory**

First, we should parse the path, and if some middle directory is not exist, should not go next.

**close a dirctory**

just invoke`file_close (struct file *file) `is ok, in this project, we treat both dirctory and file as file.

**read a dirctory** 

We just use syscall READDIR to read a dir's name.

```c
  struct file_node * openf = find_file(&thread_current()->files, fd);
  bool ok;
  if(openf!=NULL){
    ok = read_dir_by_file_node(openf->file, dir_name);
  }
  f->eax = ok;
```

And when user want to use syscall READ to read it, it will judge if it is real file, and in that case return -1;

```c
bool is_really_file(struct file* file){
  return file->inode->data.is_file==FILE_TYPE;
}
```

**write a dirctory**

Also, we can not write something to a dirctory, so when syscall WRITE invoke, we need to test it as READ do.

**syscall CHDIR**

close current thread's work directory, and update it by new directory.

**syscall INUMBER**

use inode's sector number to distingush it.

```c
int get_inumber(struct file* file){
  return file->inode->sector;
}
```







### Project 3 Report

1. When one process is actively reading or writing data in a buffer cache block, how are other processes prevented from evicting that block?



I store all my cache_entry in a list, and cache_entry have a attribute `open_cnt` represent how many threads are using it, so when it need to replace one block, it will not replace those none-zero `open_cnt` blocks.



2. During the eviction of a block from the cache, how are other processes prevented from attempting to access the block?



I have a lock called `filesys-cache_lock`(It is a global cache lock). When one thread want to read or write a block, or need to replace a blok, they all need to allocated this lock. So when I have not finish evict a clock, other thread can not get the `filesys_cache_lock`, so they also can not access the block.



3. If a block is currently being loaded into the cache, how are other processes prevented from also loading it into a different cache entry? How are other processes prevented from accessing the block before it is fully loaded?



The cache_entry stores the block sector number. Every time when a thread want to read or write a block, it first look up it in cache by providing the corresponding sector number. Only if didn't found it, the thread will load it from disk. And like previous problem, I will acqure a lock when I load the cache entry, only when I load completely, and finish next write or read, I will release the lock. So by this way, I can prevented other process from accessing the block before it is fully loaded.



4. How will your filesystem take a relative path like ../my_files/notes.txt and locate the corre- sponding directory? Also, how will you locate absolute paths like /cs162/solutions.md?



The directory entry have a sectore number point to the relative inode_disk. For `..`, it  points to its father directory's inode_disk except for root dirctory . So by this way, we can found those kind of relative path file/directory.

For absolute path, it check the first char of path first, if it is '/', then it open root dirctory, the find the next level path.



5. Will a user process be allowed to delete a directory if it is the cwd of a running process? The test suite will accept both “yes” and “no”, but in either case, you must make sure that new files cannot be created in deleted directories.








6. How will your syscall handlers take a file descriptor, like 3, and locate the corresponding file or directory struct?



Each thread have a struct file_node list which store all open file or directrory this thread open, the `strcuct file_node`have a file descriptor which was increased automatically when new open  a file/directory.  Each time the thread provide a descriptor, then syscall will traverse the `struct file_node list`, found the coressponding file_node, then get the dir/file. 



7. You are already familiar with handling memory exhaustion in C, by checking for a NULL return value from malloc. In this project, you will also need to handle diskspace exhaustion. When your filesystem is unable to allocate new disk blocks, you must have a strategy to abort the current operation and rollback to a previous good state.


