/* lock.c :  functions for manipulating filesystem locks.
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */


#include <assert.h>
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_fs.h"
#include "svn_hash.h"
#include "svn_time.h"
#include "svn_utf.h"
#include "svn_md5.h"

#include "apr_uuid.h"
#include "apr_file_io.h"
#include "apr_file_info.h"
#include "apr_md5.h"

#include "lock.h"
#include "tree.h"
#include "err.h"
#include "fs_fs.h"
#include "../libsvn_fs/fs-loader.h"

#include "svn_private_config.h"

/* Names of special lock directories in the fs_fs filesystem. */
#define LOCK_ROOT_DIR "locks"

/* Names of hash keys used to store a lock for writing to disk. */
#define PATH_KEY "path"
#define TOKEN_KEY "token"
#define OWNER_KEY "owner"
#define CREATION_DATE_KEY "creation_date"
#define EXPIRATION_DATE_KEY "expiration_date"
#define COMMENT_KEY "comment"
#define IS_XML_COMMENT_KEY "is_xml_comment"
#define CHILDREN_KEY "children"

/* Number of characters from the head of a digest file name used to
   calculate a subdirectory in which to drop that file. */
#define DIGEST_SUBDIR_LEN 3



/*** Generic helper functions. ***/

/* Return the MD5 hash of STR. */
static const char *
make_digest (const char *str,
             apr_pool_t *pool)
{
  unsigned char digest[APR_MD5_DIGESTSIZE];

  apr_md5 (digest, str, strlen(str));
  return svn_md5_digest_to_cstring (digest, pool);
}


/* Set the value of KEY (whose size is KEY_LEN, or APR_HASH_KEY_STRING
   if unknown) to an svn_string_t-ized version of VALUE (whose size is
   VALUE_LEN, or APR_HASH_KEY_STRING if unknown) in HASH.  The value
   will be allocated in POOL; KEY will not be duped.  If either KEY or VALUE
   is NULL, this function will do nothing. */
static void
hash_store (apr_hash_t *hash,
            const char *key,
            apr_ssize_t key_len,
            const char *value,
            apr_ssize_t value_len,
            apr_pool_t *pool)
{
  if (! (key && value))
    return;
  if (value_len == APR_HASH_KEY_STRING)
    value_len = strlen (value);
  apr_hash_set (hash, key, key_len, 
                svn_string_ncreate (value, value_len, pool));
}


/* Fetch the value of KEY from HASH, returning only the cstring data
   of that value (if it exists). */
static const char *
hash_fetch (apr_hash_t *hash,
            const char *key,
            apr_pool_t *pool)
{
  svn_string_t *str = apr_hash_get (hash, key, APR_HASH_KEY_STRING);
  return str ? str->data : NULL;
}



/*** Digest file handling functions. ***/

/* Return the path of the lock/entries file for which DIGEST is the
   hashed repository relative path. */
static const char *
digest_path_from_digest (svn_fs_t *fs,
                         const char *digest,
                         apr_pool_t *pool)
{
  return svn_path_join_many (pool, fs->path, LOCK_ROOT_DIR, 
                             apr_pstrmemdup (pool, digest, DIGEST_SUBDIR_LEN), 
                             digest, NULL);
}


/* Return the path to the lock/entries digest file associate with
   PATH, where PATH is the path to the lock file or lock entries file
   in FS. */
static const char *
digest_path_from_path (svn_fs_t *fs,
                       const char *path,
                       apr_pool_t *pool)
{
  const char *digest = make_digest (path, pool);
  return svn_path_join_many (pool, fs->path, LOCK_ROOT_DIR, 
                             apr_pstrmemdup (pool, digest, DIGEST_SUBDIR_LEN), 
                             digest, NULL);
}


/* If directory PATH does not exist, create it and give it the same
   permissions as FS->path.*/
static svn_error_t *
ensure_dir_exists (const char *path,
                   svn_fs_t *fs,
                   apr_pool_t *pool)
{
  svn_error_t *err = svn_io_dir_make (path, APR_OS_DEFAULT, pool);
  if (err && APR_STATUS_IS_EEXIST (err->apr_err))
    {
      svn_error_clear (err);
      return SVN_NO_ERROR;
    }
  SVN_ERR (err);

  /* We successfully created a new directory.  Dup the permissions
     from FS->path. */
  SVN_ERR (svn_fs_fs__dup_perms (path, fs->path, pool));

  return SVN_NO_ERROR;
}


/* Write to DIGEST_PATH a representation of CHILDREN (which may be
   empty, if the versioned path in FS represented by DIGEST_PATH has
   no children) and LOCK (which may be NULL if that versioned path is
   lock itself locked).  Use POOL for all allocations. */
static svn_error_t *
write_digest_file (apr_hash_t *children,
                   svn_lock_t *lock,
                   svn_fs_t *fs,
                   const char *digest_path,
                   apr_pool_t *pool)
{
  svn_error_t *err = SVN_NO_ERROR;
  apr_file_t *fd;
  apr_hash_index_t *hi;
  apr_hash_t *hash = apr_hash_make (pool);
  const char *tmp_path;

  SVN_ERR (ensure_dir_exists (svn_path_join (fs->path, LOCK_ROOT_DIR, pool), 
                              fs, pool));
  SVN_ERR (ensure_dir_exists (svn_path_dirname (digest_path, pool), fs, pool));
  SVN_ERR (svn_io_open_unique_file 
           (&fd, &tmp_path, digest_path, ".tmp", FALSE, pool));

  if (lock)
    {
      const char *creation_date = NULL, *expiration_date = NULL;
      if (lock->creation_date)
        creation_date = svn_time_to_cstring (lock->creation_date, pool);
      if (lock->expiration_date)
        expiration_date = svn_time_to_cstring (lock->expiration_date, pool);
      hash_store (hash, PATH_KEY, sizeof(PATH_KEY)-1,
                  lock->path, APR_HASH_KEY_STRING, pool); 
      hash_store (hash, TOKEN_KEY, sizeof(TOKEN_KEY)-1,
                  lock->token, APR_HASH_KEY_STRING, pool); 
      hash_store (hash, OWNER_KEY, sizeof(OWNER_KEY)-1,
                  lock->owner, APR_HASH_KEY_STRING, pool); 
      hash_store (hash, COMMENT_KEY, sizeof(COMMENT_KEY)-1,
                  lock->comment, APR_HASH_KEY_STRING, pool); 
      hash_store (hash, IS_XML_COMMENT_KEY, sizeof(IS_XML_COMMENT_KEY)-1,
                  lock->xml_comment ? "1" : "0", 1, pool);
      hash_store (hash, CREATION_DATE_KEY, sizeof(CREATION_DATE_KEY)-1,
                  creation_date, APR_HASH_KEY_STRING, pool);
      hash_store (hash, EXPIRATION_DATE_KEY, sizeof(EXPIRATION_DATE_KEY)-1,
                  expiration_date, APR_HASH_KEY_STRING, pool);
    }
  if (apr_hash_count (children))
    {
      svn_stringbuf_t *children_list = svn_stringbuf_create ("", pool);
      for (hi = apr_hash_first(pool, children); hi; hi = apr_hash_next(hi)) 
        {
          const void *key;
          apr_ssize_t klen;
          apr_hash_this (hi, &key, &klen, NULL);
          svn_stringbuf_appendbytes (children_list, key, klen);
          svn_stringbuf_appendbytes (children_list, "\n", 1);
        }
      hash_store (hash, CHILDREN_KEY, sizeof(CHILDREN_KEY)-1,
                  children_list->data, children_list->len, pool);
    } 

  if ((err = svn_hash_write2 (hash, 
                              svn_stream_from_aprfile (fd, pool),
                              SVN_HASH_TERMINATOR, pool)))
    {
      (void) svn_io_file_close (fd, pool); /* error is relatively unexciting */
      return svn_error_createf (err->apr_err,
                                err,
                                _("Cannot write lock/entries hashfile '%s'"),
                                svn_path_local_style (tmp_path, pool));
    }

  SVN_ERR (svn_io_file_close (fd, pool));
  SVN_ERR (svn_io_file_rename (tmp_path, digest_path, pool));
  SVN_ERR (svn_fs_fs__dup_perms 
           (digest_path, svn_fs_fs__path_rev (fs, 0, pool), pool));

  return SVN_NO_ERROR;
}


/* Parse the file at DIGEST_PATH, populating the lock LOCK_P in that
   file (if it exists, and if *LOCK_P is non-NULL) and the hash of
   CHILDREN_P (if any exist, and if *CHILDREN_P is non-NULL).  Use POOL
   for all allocations.  */
static svn_error_t *
read_digest_file (apr_hash_t **children_p,
                  svn_lock_t **lock_p,
                  svn_fs_t *fs,
                  const char *digest_path,
                  apr_pool_t *pool)
{
  svn_error_t *err = SVN_NO_ERROR;
  svn_lock_t *lock;
  apr_hash_t *hash;
  apr_file_t *fd;
  const char *val;

  if (lock_p)
    *lock_p = NULL;
  if (children_p)
    *children_p = apr_hash_make (pool);

  err = svn_io_file_open (&fd, digest_path, APR_READ, APR_OS_DEFAULT, pool);
  if (err && APR_STATUS_IS_ENOENT (err->apr_err))
    {
      svn_error_clear (err);
      return SVN_NO_ERROR;
    }
  SVN_ERR (err);

  /* If our caller doesn't care about anything but the presence of the
     file... whatever. */
  if (! (lock_p || children_p))
    return svn_io_file_close (fd, pool);

  hash = apr_hash_make (pool);
  if ((err = svn_hash_read2 (hash, 
                             svn_stream_from_aprfile (fd, pool),
                             SVN_HASH_TERMINATOR, pool)))
    {
      (void) svn_io_file_close (fd, pool); /* error is relatively unexciting */
      return svn_error_createf (err->apr_err,
                                err,
                                _("Can't parse lock/entries hashfile '%s'"),
                                svn_path_local_style (digest_path, pool));
    }
  SVN_ERR (svn_io_file_close (fd, pool));

  /* If our caller cares, see if we have a lock path in our hash. If
     so, we'll assume we have a lock here. */
  val = hash_fetch (hash, PATH_KEY, pool);
  if (val && lock_p)
    {
      const char *path = val;

      /* Create our lock and load it up. */
      lock = svn_lock_create (pool);
      lock->path = path;

      if (! ((lock->token = hash_fetch (hash, TOKEN_KEY, pool))))
        return svn_fs_fs__err_corrupt_lockfile (fs, path);

      if (! ((lock->owner = hash_fetch (hash, OWNER_KEY, pool))))
        return svn_fs_fs__err_corrupt_lockfile (fs, path);

      if (! ((val = hash_fetch (hash, IS_XML_COMMENT_KEY, pool))))
        return svn_fs_fs__err_corrupt_lockfile (fs, path);
      lock->xml_comment = (val[0] == '1') ? TRUE : FALSE;

      if (! ((val = hash_fetch (hash, CREATION_DATE_KEY, pool))))
        return svn_fs_fs__err_corrupt_lockfile (fs, path);
      SVN_ERR (svn_time_from_cstring (&(lock->creation_date), val, pool));

      if ((val = hash_fetch (hash, EXPIRATION_DATE_KEY, pool)))
        SVN_ERR (svn_time_from_cstring (&(lock->expiration_date), val, pool));

      lock->comment = hash_fetch (hash, COMMENT_KEY, pool);

      *lock_p = lock;
    }

  /* If our caller cares, see if we have any children for this path. */
  val = hash_fetch (hash, CHILDREN_KEY, pool);
  if (val && children_p)
    {
      apr_array_header_t *kiddos = svn_cstring_split (val, "\n", FALSE, pool);
      int i;

      for (i = 0; i < kiddos->nelts; i++)
        {
          apr_hash_set (*children_p, APR_ARRAY_IDX (kiddos, i, const char *),
                        APR_HASH_KEY_STRING, (void *)1);
        }
    }
  return SVN_NO_ERROR;
}



/*** Lock helper functions (path here are still FS paths, not on-disk
     schema-supporting paths) ***/


/* Write LOCK in FS to the actual OS filesystem. */
static svn_error_t *
set_lock (svn_fs_t *fs,
          svn_lock_t *lock,
          apr_pool_t *pool)
{
  svn_stringbuf_t *this_path = svn_stringbuf_create (lock->path, pool);
  svn_stringbuf_t *last_child = svn_stringbuf_create ("", pool);
  apr_pool_t *subpool;

  assert (lock);

  /* Iterate in reverse, creating the lock for LOCK->path, and then
     just adding entries for its parent, until we reach a parent
     that's already listed in *its* parent. */ 
  subpool = svn_pool_create (pool);
  while (1729)
    {
      const char *digest_path, *parent_dir, *digest_file;
      apr_hash_t *this_children;
      svn_lock_t *this_lock;

      svn_pool_clear (subpool);

      /* Calculate the DIGEST_PATH for the currently FS path, and then
         split it into a PARENT_DIR and DIGEST_FILE basename. */
      digest_path = digest_path_from_path (fs, this_path->data, subpool);
      svn_path_split (digest_path, &parent_dir, &digest_file, subpool);

      SVN_ERR (read_digest_file (&this_children, &this_lock, fs, 
                                 digest_path, subpool));

      /* We're either writing a new lock (first time through only) or
         a new entry (every time but the first). */
      if (lock)
        {
          this_lock = lock;
          lock = NULL;
          svn_stringbuf_set (last_child, digest_file);
        }
      else
        {
          /* If we already have an entry for this path, we're done. */
          if (apr_hash_get (this_children, last_child->data, last_child->len))
            break;
          apr_hash_set (this_children, last_child->data, 
                        last_child->len, (void *)1);
        }
      SVN_ERR (write_digest_file (this_children, this_lock, fs, 
                                  digest_path, subpool));

      /* Prep for next iteration, or bail if we're done. */
      if ((this_path->len == 1) && (this_path->data[0] == '/'))
        break;
      svn_stringbuf_set (this_path, 
                         svn_path_dirname (this_path->data, subpool));
    }

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}

/* Delete LOCK from FS in the actual OS filesystem. */
static svn_error_t *
delete_lock (svn_fs_t *fs, 
             svn_lock_t *lock,
             apr_pool_t *pool)
{
  svn_stringbuf_t *this_path = svn_stringbuf_create (lock->path, pool);
  svn_stringbuf_t *child_to_kill = svn_stringbuf_create ("", pool);
  apr_pool_t *subpool;

  assert (lock);

  /* Iterate in reverse, deleting the lock for LOCK->path, and then
     pruning entries from its parents. */
  subpool = svn_pool_create (pool);
  while (1729)
    {
      const char *digest_path, *parent_dir, *digest_file;
      apr_hash_t *this_children;
      svn_lock_t *this_lock;

      svn_pool_clear (subpool);

      /* Calculate the DIGEST_PATH for the currently FS path, and then
         split it into a PARENT_DIR and DIGEST_FILE basename. */
      digest_path = digest_path_from_path (fs, this_path->data, subpool);
      svn_path_split (digest_path, &parent_dir, &digest_file, subpool);

      SVN_ERR (read_digest_file (&this_children, &this_lock, fs, 
                                 digest_path, subpool));

      /* If we are supposed to drop the last entry from this path's
         children list, do so. */
      if (child_to_kill->len)
        apr_hash_set (this_children, child_to_kill->data, 
                      child_to_kill->len, NULL);
        
      /* Delete the lock (first time through only). */
      if (lock)
        {
          this_lock = NULL;
          lock = NULL;
        }

      if (! (this_lock || apr_hash_count (this_children) != 0))
        {
          /* Special case:  no goodz, no file.  And remember to nix
             the entry for it in its parent. */
          svn_stringbuf_set (child_to_kill, 
                             svn_path_basename (digest_path, subpool));
          SVN_ERR (svn_io_remove_file (digest_path, subpool));
        }
      else
        {
          SVN_ERR (write_digest_file (this_children, this_lock, fs, 
                                      digest_path, subpool));
          svn_stringbuf_setempty (child_to_kill);
        }

      /* Prep for next iteration, or bail if we're done. */
      if ((this_path->len == 1) && (this_path->data[0] == '/'))
        break;
      svn_stringbuf_set (this_path, 
                         svn_path_dirname (this_path->data, subpool));
    }

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}


/* Set *LOCK_P to the lock for PATH in FS.  HAVE_WRITE_LOCK should be
   TRUE if the caller (or one of its callers) has taken out the
   repository-wide write lock, FALSE otherwise.  Use POOL for
   allocations. */
static svn_error_t *
get_lock (svn_lock_t **lock_p,
          svn_fs_t *fs,
          const char *path,
          svn_boolean_t have_write_lock,
          apr_pool_t *pool)
{
  svn_lock_t *lock;
  const char *digest_path = digest_path_from_path (fs, path, pool);

  SVN_ERR (read_digest_file (NULL, &lock, fs, digest_path, pool));
  if (! lock)
    return svn_fs_fs__err_no_such_lock (fs, path);

  /* Possibly auto-expire the lock. */
  if (lock->expiration_date && (apr_time_now() > lock->expiration_date))
    {
      if (have_write_lock)
        SVN_ERR (delete_lock (fs, lock, pool));
      else
        {
          /* Grab the fs write lock. */
          apr_pool_t *subpool = svn_pool_create (pool);
          SVN_ERR (svn_fs_fs__get_write_lock (fs, subpool));
          
          /* Reread the lock to avoid a race. */
          SVN_ERR (read_digest_file (NULL, &lock, fs, digest_path, pool));
          if (! lock)
            {
              svn_pool_destroy (subpool);
              return svn_fs_fs__err_no_such_lock (fs, path);
            }

          /* Check to make sure that the lock that we read the second
             time around is actually expired. */
          if (lock->expiration_date && (apr_time_now() > lock->expiration_date))
            SVN_ERR (delete_lock (fs, lock, pool));
          
          /* Destroy our subpool and release the fs write lock. */
          svn_pool_destroy (subpool);
        }
      *lock_p = NULL;
      return svn_fs_fs__err_lock_expired (fs, lock->token); 
    }
  
  *lock_p = lock;
  return SVN_NO_ERROR;
}


/* Set *LOCK_P to the lock for PATH in FS.  HAVE_WRITE_LOCK should be
   TRUE if the caller (or one of its callers) has taken out the
   repository-wide write lock, FALSE otherwise.  Use POOL for
   allocations. */
static svn_error_t *
get_lock_helper (svn_fs_t *fs,
                 svn_lock_t **lock_p,
                 const char *path,
                 svn_boolean_t have_write_lock,
                 apr_pool_t *pool)
{
  svn_lock_t *lock;
  svn_error_t *err;
  
  err = get_lock (&lock, fs, path, have_write_lock, pool);

  /* We've deliberately decided that this function doesn't tell the
     caller *why* the lock is unavailable.  */
  if (err && ((err->apr_err == SVN_ERR_FS_NO_SUCH_LOCK)
              || (err->apr_err == SVN_ERR_FS_LOCK_EXPIRED)))
    {
      svn_error_clear (err);
      *lock_p = NULL;
      return SVN_NO_ERROR;
    }
  else
    SVN_ERR (err);

  *lock_p = lock;
  return SVN_NO_ERROR;
}


/* A recursive function that calls GET_LOCKS_FUNC/GET_LOCKS_BATON for
   all locks in and under PATH in FS.  */
static svn_error_t *
walk_digest_files (svn_fs_t *fs, 
                   const char *digest_path,
                   svn_fs_get_locks_callback_t get_locks_func,
                   void *get_locks_baton,
                   apr_pool_t *pool)
{
  apr_hash_t *children;
  svn_lock_t *lock;
  apr_hash_index_t *hi;
  apr_pool_t *subpool;

  /* First, send up any locks in the current digest file. */
  SVN_ERR (read_digest_file (&children, &lock, fs, digest_path, pool));
  if (lock && get_locks_func)
    SVN_ERR (get_locks_func (get_locks_baton, lock, pool));

  /* Now, recurse on this thing's child entries (if any; bail otherwise). */
  if (! apr_hash_count (children))
    return SVN_NO_ERROR;
  subpool = svn_pool_create (pool);
  for (hi = apr_hash_first(pool, children); hi; hi = apr_hash_next(hi)) 
    {
      const void *key;
      svn_pool_clear (subpool);
      apr_hash_this(hi, &key, NULL, NULL);
      SVN_ERR (walk_digest_files 
               (fs, digest_path_from_digest (fs, key, subpool),
                get_locks_func, get_locks_baton, subpool));
    }
  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}


/* Utility function:  verify that a lock can be used.  Interesting
   errors returned from this function:

      SVN_ERR_FS_NO_USER: No username attached to FS.
      SVN_ERR_FS_LOCK_OWNER_MISMATCH: FS's username doesn't match LOCK's owner.
      SVN_ERR_FS_BAD_LOCK_TOKEN: FS doesn't hold matching lock-token for LOCK.
 */
static svn_error_t *
verify_lock (svn_fs_t *fs,
             svn_lock_t *lock,
             apr_pool_t *pool)
{
  if ((! fs->access_ctx) || (! fs->access_ctx->username))
    return svn_error_createf 
      (SVN_ERR_FS_NO_USER, NULL,
       _("Cannot verify lock on path '%s'; no username available"),
       lock->path);
  
  else if (strcmp (fs->access_ctx->username, lock->owner) != 0)
    return svn_error_createf 
      (SVN_ERR_FS_LOCK_OWNER_MISMATCH, NULL,
       _("User %s does not own lock on path '%s' (currently locked by %s)"),
       fs->access_ctx->username, lock->path, lock->owner);

  else if (apr_hash_get (fs->access_ctx->lock_tokens, lock->token,
                         APR_HASH_KEY_STRING) == NULL)
    return svn_error_createf 
      (SVN_ERR_FS_BAD_LOCK_TOKEN, NULL,
       _("Cannot verify lock on path '%s'; no matching lock-token available"),
       lock->path);
    
  return SVN_NO_ERROR;
}


/* This implements the svn_fs_get_locks_callback_t interface, where
   BATON is just an svn_fs_t object. */
static svn_error_t *
get_locks_callback (void *baton, 
                    svn_lock_t *lock, 
                    apr_pool_t *pool)
{
  return verify_lock (baton, lock, pool);
}


/* The main routine for lock enforcement, used throughout libsvn_fs_fs. */
svn_error_t *
svn_fs_fs__allow_locked_operation (const char *path,
                                   svn_fs_t *fs,
                                   svn_boolean_t recurse,
                                   apr_pool_t *pool)
{
  path = svn_fs_fs__canonicalize_abspath (path, pool);
  if (recurse)
    {
      /* Discover all locks at or below the path. */
      SVN_ERR (svn_fs_fs__get_locks (fs, path, get_locks_callback, fs, pool));
    }
  else 
    {
      /* Discover and verify any lock attached to the path. */
      svn_lock_t *lock;
      SVN_ERR (get_lock_helper (fs, &lock, path, FALSE, pool));
      if (lock)
        SVN_ERR (verify_lock (fs, lock, pool));
    }
  return SVN_NO_ERROR;
}



/*** Public API implementations ***/

svn_error_t *
svn_fs_fs__lock (svn_lock_t **lock_p,
                 svn_fs_t *fs,
                 const char *path,
                 const char *token,
                 const char *comment,
                 int timeout,
                 svn_revnum_t current_rev,
                 svn_boolean_t steal_lock,
                 apr_pool_t *pool)
{
  svn_node_kind_t kind;
  svn_lock_t *existing_lock;
  svn_lock_t *lock;
  svn_fs_root_t *root;
  svn_revnum_t youngest;
  apr_pool_t *subpool = svn_pool_create (pool);

  SVN_ERR (svn_fs_fs__check_fs (fs));
  path = svn_fs_fs__canonicalize_abspath (path, pool);

  /* Until we implement directory locks someday, we only allow locks
     on files or non-existent paths. */
  /* Use fs->vtable->foo instead of svn_fs_foo to avoid circular
     library dependencies, which are not portable. */
  SVN_ERR (fs->vtable->youngest_rev (&youngest, fs, pool));
  SVN_ERR (fs->vtable->revision_root (&root, fs, youngest, pool));
  SVN_ERR (svn_fs_fs__check_path (&kind, root, path, pool));
  if (kind == svn_node_dir)
    return svn_fs_fs__err_not_file (fs, path);

  /* While our locking implementation easily supports the locking of
     nonexistent paths, we deliberately choose not to allow such madness. */
  if (kind == svn_node_none)
    return svn_error_createf (SVN_ERR_FS_NOT_FOUND, NULL,
                              "Path '%s' doesn't exist in HEAD revision",
                              path);

  /* We need to have a username attached to the fs. */
  if (!fs->access_ctx || !fs->access_ctx->username)
    return svn_fs_fs__err_no_user (fs);

  SVN_ERR (svn_fs_fs__get_write_lock (fs, subpool));

  /* Is the caller attempting to lock an out-of-date working file? */
  if (SVN_IS_VALID_REVNUM(current_rev))
    {
      svn_revnum_t created_rev;
      SVN_ERR (svn_fs_fs__node_created_rev (&created_rev, root, path, pool));

      /* SVN_INVALID_REVNUM means the path doesn't exist.  So
         apparently somebody is trying to lock something in their
         working copy, but somebody else has deleted the thing
         from HEAD.  That counts as being 'out of date'. */     
      if (! SVN_IS_VALID_REVNUM(created_rev))
        return svn_error_createf (SVN_ERR_FS_OUT_OF_DATE, NULL,
                                  "Path '%s' doesn't exist in HEAD revision.",
                                  path);

      if (current_rev < created_rev)
        return svn_error_createf (SVN_ERR_FS_OUT_OF_DATE, NULL,
                                  "Lock failed: newer version of '%s' exists.",
                                  path);
    }

  /* If the caller provided a TOKEN, we *really* need to see
     if a lock already exists with that token, and if so, verify that
     the lock's path matches PATH.  Otherwise we run the risk of
     breaking the 1-to-1 mapping of lock tokens to locked paths. */
  /* ### TODO:  actually do this check.  This is tough, because the
     schema doesn't supply a lookup-by-token mechanism.
  if (token)
    {
    }
  */

  /* Is the path already locked?   

     Note that this next function call will automatically ignore any
     errors about {the path not existing as a key, the path's token
     not existing as a key, the lock just having been expired}.  And
     that's totally fine.  Any of these three errors are perfectly
     acceptable to ignore; it means that the path is now free and
     clear for locking, because the fsfs funcs just cleared out both
     of the tables for us.   */
  SVN_ERR (get_lock_helper (fs, &existing_lock, path, TRUE, pool));
  if (existing_lock)
    {
      if (! steal_lock)
        {
          /* Sorry, the path is already locked. */
          return svn_fs_fs__err_path_locked (fs, existing_lock);
        }
      else
        {
          /* STEAL_LOCK was passed, so fs_username is "stealing" the
             lock from lock->owner.  Destroy the existing lock. */
          SVN_ERR (delete_lock (fs, existing_lock, pool));
        }          
    }

  /* Create our new lock, and add it to the tables. */    
  lock = svn_lock_create (pool);
  if (token)
    lock->token = apr_pstrdup (pool, token);
  else
    SVN_ERR (svn_fs_fs__generate_lock_token (&(lock->token), fs, pool));
  lock->path = apr_pstrdup (pool, path);
  lock->owner = apr_pstrdup (pool, fs->access_ctx->username);
  lock->comment = apr_pstrdup (pool, comment);
  lock->creation_date = apr_time_now();
  if (timeout)
    lock->expiration_date = lock->creation_date + apr_time_from_sec (timeout);

  SVN_ERR (set_lock (fs, lock, pool));
  *lock_p = lock;

  /* Destroy our subpool and release the fs write lock. */
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__generate_lock_token (const char **token,
                                svn_fs_t *fs,
                                apr_pool_t *pool)
{
  /* Notice that 'fs' is currently unused.  But perhaps someday,
     we'll want to use the fs UUID + some incremented number?  */
  apr_uuid_t uuid;
  char *uuid_str = apr_pcalloc (pool, APR_UUID_FORMATTED_LENGTH + 1);

  SVN_ERR (svn_fs_fs__check_fs (fs));

  apr_uuid_get (&uuid);
  apr_uuid_format (uuid_str, &uuid);

  /* For now, we generate a URI that matches the DAV RFC.  We could
     change this to some other URI schema someday, if we wish. */
  *token = apr_pstrcat (pool, "opaquelocktoken:", uuid_str, NULL);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__unlock (svn_fs_t *fs,
                   const char *path,
                   const char *token,
                   svn_boolean_t break_lock,
                   apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  svn_lock_t *lock;

  SVN_ERR (svn_fs_fs__check_fs (fs));
  path = svn_fs_fs__canonicalize_abspath (path, pool);

  SVN_ERR (svn_fs_fs__get_write_lock (fs, subpool));

  /* This could return SVN_ERR_FS_BAD_LOCK_TOKEN or SVN_ERR_FS_LOCK_EXPIRED. */
  SVN_ERR (get_lock (&lock, fs, path, TRUE, pool));
  
  /* Unless breaking the lock, we do some checks. */
  if (! break_lock)
    {
      /* Sanity check:  the incoming token should match lock->token. */
      if (strcmp (token, lock->token) != 0)
        return svn_fs_fs__err_no_such_lock (fs, lock->path);

      /* There better be a username attached to the fs. */
      if (! (fs->access_ctx && fs->access_ctx->username))
        return svn_fs_fs__err_no_user (fs);

      /* And that username better be the same as the lock's owner. */
      if (strcmp (fs->access_ctx->username, lock->owner) != 0)
        return svn_fs_fs__err_lock_owner_mismatch (fs, 
                                                   fs->access_ctx->username,
                                                   lock->owner);
    }  

  /* Remove lock and lock token files. */
  SVN_ERR (delete_lock (fs, lock, pool));

  /* Destroy our subpool and release the fs write lock. */
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__get_lock (svn_lock_t **lock_p,
                     svn_fs_t *fs,
                     const char *path,
                     apr_pool_t *pool)
{
  SVN_ERR (svn_fs_fs__check_fs (fs));
  path = svn_fs_fs__canonicalize_abspath (path, pool);
  return get_lock_helper (fs, lock_p, path, FALSE, pool);
}


svn_error_t *
svn_fs_fs__get_locks (svn_fs_t *fs,
                      const char *path,
                      svn_fs_get_locks_callback_t get_locks_func,
                      void *get_locks_baton,
                      apr_pool_t *pool)
{
  const char *digest_path;

  SVN_ERR (svn_fs_fs__check_fs (fs));
  path = svn_fs_fs__canonicalize_abspath (path, pool);

  /* Get the top digest path in our tree of interest, and then walk it. */
  digest_path = digest_path_from_path (fs, path, pool);
  return walk_digest_files (fs, digest_path, get_locks_func, 
                            get_locks_baton, pool);
}
