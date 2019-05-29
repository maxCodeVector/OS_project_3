#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* add by hya: to change a file/dir name to coresponding inode */
static struct inode * name_to_inode (const char *name);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  cache_init();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  cache_flush ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
// bool
// filesys_create (const char *name, off_t initial_size) 
// {
//   block_sector_t inode_sector = 0;
//   struct dir *dir = dir_open_root ();
//   bool success = (dir != NULL
//                   && free_map_allocate (1, &inode_sector)
//                   && inode_create (inode_sector, initial_size, FILE_TYPE)
//                   && dir_add (dir, name, inode_sector));
//   if (!success && inode_sector != 0) 
//     free_map_release (inode_sector, 1);
//   dir_close (dir);

//   return success;
// }

/* hya add: Extracts a file name part from *SRCP into PART,
   and updates *SRCP so that the next call will return the next
   file name part.
   Returns 1 if successful, 0 at end of string, -1 for a too-long
   file name part. */
static int
get_next_part (char part[NAME_MAX], const char **srcp)
{
  const char *src = *srcp;
  char *dst = part;

  /* Skip leading slashes.
     If it's all slashes, we're done. */
  while (*src == '/')
    src++;
  if (*src == '\0')
    return 0;

  /* Copy up to NAME_MAX character from SRC to DST.
     Add null terminator. */
  while (*src != '/' && *src != '\0') 
    {
      if (dst < part + NAME_MAX)
        *dst++ = *src;
      else
        return -1;
      src++; 
    }
  *dst = '\0';

  /* Advance source pointer. */
  *srcp = src;
  return 1;
}

/* hya add: Resolves relative or absolute file NAME.
   Returns true if successful, false on failure.
   Stores the directory corresponding to the name into *DIRP,
   and the file name part into BASE_NAME.*/
static bool
resolve_name_to_entry (const char *name,
                       struct dir **dirp, char base_name[NAME_MAX + 1]) 
{
  struct dir *dir = NULL;
  struct inode *inode;
  const char *cp;
  char part[NAME_MAX + 1], next_part[NAME_MAX + 1];
  int ok;
  
  /* Find starting directory. */
  if (name[0] == '/' || thread_current ()->wd == NULL)
    dir = dir_open_root ();
  else
    dir = dir_reopen (thread_current ()->wd);
  if (dir == NULL){  
    /* Return failure. */
    dir_close (dir);
    *dirp = NULL;
    base_name[0] = '\0';
    return false;
  }

  /* Get first name part. */
  cp = name;
  if (get_next_part (part, &cp) <= 0){
    /* Return failure. */
    dir_close (dir);
    *dirp = NULL;
    base_name[0] = '\0';
    return false;
  }

  /* As long as another part follows the current one,
     traverse down another directory. */
  while ((ok = get_next_part (next_part, &cp)) > 0)
    {
      if (!dir_lookup (dir, part, &inode)){
        /* Return failure. */
        dir_close (dir);
        *dirp = NULL;
        base_name[0] = '\0';
        return false;

      }

      dir_close (dir);
      dir = dir_open (inode);
      if (dir == NULL){
        /* Return failure. */
        dir_close (dir);
        *dirp = NULL;
        base_name[0] = '\0';
        return false;
      }

      strlcpy (part, next_part, NAME_MAX + 1);
    }
  if (ok < 0){
    /* Return failure. */
    dir_close (dir);
    *dirp = NULL;
    base_name[0] = '\0';
    return false;
  }

  /* Return our results. */
  *dirp = dir;
  strlcpy (base_name, part, NAME_MAX + 1);
  return true;
}


/* add by hya: parse path and create the file*/
bool
filesys_create (const char *name, off_t initial_size) 
{
  struct dir *dir;
  char base_name[NAME_MAX + 1];
  block_sector_t inode_sector;  // new mallocate sector space to store new dir

  bool success = (resolve_name_to_entry (name, &dir, base_name)
                  && free_map_allocate (1, &inode_sector));
  if (success) 
    {
      struct inode *inode;
      inode = file_create (inode_sector, initial_size); 
      if (inode != NULL)
        {
          success = dir_add (dir, base_name, inode_sector);
          if (!success)
            inode_remove (inode);
          inode_close (inode);
        }
      else
        success = false;
        
    }
  dir_close (dir);

  return success;
}

/* add by hya: parse path and create the dir */
bool
filesys_dir_create (const char *name, off_t initial_size) 
{
  struct dir *dir;
  char base_name[NAME_MAX + 1];
  block_sector_t inode_sector;  // new mallocate sector space to store new dir

  bool success = (resolve_name_to_entry (name, &dir, base_name)
                  && free_map_allocate (1, &inode_sector));
  if (success) 
    {
      struct inode *inode;
      inode = dir_create (inode_sector,
                            inode_get_inumber (dir_get_inode (dir))); 
      if (inode != NULL)
        {
          success = dir_add (dir, base_name, inode_sector);
          if (!success)
            inode_remove (inode);
          inode_close (inode);
        }
      else
        success = false;
        
    }
  dir_close (dir);

  return success;
}


/* modified by hya:, Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{


   if (name[0] == '/' && name[strspn (name, "/")] == '\0') 
    {
      /* The name represents the root directory.
         There's no name part at all, so resolve_name_to_entry()
         would reject it entirely.
         Special case it. */
      return file_open(inode_open (ROOT_DIR_SECTOR));
    }
  else 
    {
      struct dir *dir;
      char base_name[NAME_MAX + 1];

      // struct dir *dir = dir_open_root ();
      // struct inode *inode = NULL;

      // if (dir != NULL){
      //   dir_lookup (dir, name, &inode);
      // }
      // dir_close (dir);

      // return file_open (inode);

      if (resolve_name_to_entry (name, &dir, base_name)) 
        {
          struct inode *inode;
          dir_lookup (dir, base_name, &inode);
          dir_close (dir);
          return file_open(inode); 
        }
      else
        return NULL;
    }
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir = dir_open_root ();
  bool success = dir != NULL && dir_remove (dir, name);
  dir_close (dir); 

  return success;
}

/* modified by hya: Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  // if (!dir_create_root (ROOT_DIR_SECTOR, 16))
  //   PANIC ("root directory creation failed");


  /* Set up root directory. */
  struct inode *inode = dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR);
  if (inode == NULL)
    PANIC ("root directory creation failed");
  inode_close (inode);

  free_map_close ();
  printf ("done.\n");
}

/* add by hya: Change current directory to NAME.
   Return true if successful, false on failure. */
bool
filesys_chdir (const char *name) 
{
  struct dir *dir = dir_open (name_to_inode (name));
  if (dir != NULL) 
    {
      dir_close (thread_current ()->wd);
      thread_current ()->wd = dir;
      return true;
    }
  else
    return false;
}


/* add by hya:  Resolves relative or absolute file NAME to an inode.
   Returns an inode if successful, or a null pointer on failure.
   The caller is responsible for closing the returned inode. */
static struct inode *
name_to_inode (const char *name)
{
  if (name[0] == '/' && name[strspn (name, "/")] == '\0') 
    {
      /* The name represents the root directory.
         There's no name part at all, so resolve_name_to_entry()
         would reject it entirely.
         Special case it. */
      return inode_open (ROOT_DIR_SECTOR);
    }
  else 
    {
      struct dir *dir;
      char base_name[NAME_MAX + 1];

      if (resolve_name_to_entry (name, &dir, base_name)) 
        {
          struct inode *inode;
          dir_lookup (dir, base_name, &inode);
          dir_close (dir);
          return inode; 
        }
      else
        return NULL;
    }
}