/* fs.h : interface to Subversion filesystem, private to libsvn_fs
 *
 * ====================================================================
 * Copyright (c) 2000-2007 CollabNet.  All rights reserved.
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

#ifndef SVN_LIBSVN_FS_FS_H
#define SVN_LIBSVN_FS_FS_H

#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_md5.h>
#include <apr_thread_mutex.h>
#include <apr_network_io.h>

#include "svn_fs.h"
#include "private/svn_fs_private.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/*** The filesystem structure.  ***/

/* Following are defines that specify the textual elements of the
   native filesystem directories and revision files. */

/* Names of special files in the fs_fs filesystem. */
#define PATH_FORMAT           "format"           /* Contains format number */
#define PATH_UUID             "uuid"             /* Contains UUID */
#define PATH_CURRENT          "current"          /* Youngest revision */
#define PATH_LOCK_FILE        "write-lock"       /* Revision lock file */
#define PATH_REVS_DIR         "revs"             /* Directory of revisions */
#define PATH_REVPROPS_DIR     "revprops"         /* Directory of revprops */
#define PATH_TXNS_DIR         "transactions"     /* Directory of transactions */
#define PATH_TXN_CURRENT      "transaction-current" /* File with next txn key */
#define PATH_TXN_CURRENT_LOCK "txn-current-lock" /* Lock for txn-current */
#define PATH_LOCKS_DIR         "locks"           /* Directory of locks */

/* Names of special files and file extensions for transactions */
#define PATH_CHANGES       "changes"       /* Records changes made so far */
#define PATH_TXN_PROPS     "props"         /* Transaction properties */
#define PATH_NEXT_IDS      "next-ids"      /* Next temporary ID assignments */
#define PATH_REV           "rev"           /* Proto rev file */
#define PATH_REV_LOCK      "rev-lock"      /* Proto rev (write) lock file */
#define PATH_TXN_MERGEINFO "mergeinfo"     /* Transaction mergeinfo props */
#define PATH_PREFIX_NODE   "node."         /* Prefix for node filename */
#define PATH_EXT_TXN       ".txn"          /* Extension of txn dir */
#define PATH_EXT_CHILDREN  ".children"     /* Extension for dir contents */
#define PATH_EXT_PROPS     ".props"        /* Extension for node props */

/* The format number of this filesystem.
   This is independent of the repository format number, and
   independent of any other FS back ends. */
#define SVN_FS_FS__FORMAT_NUMBER   3

/* The minimum format number that supports svndiff version 1.  */
#define SVN_FS_FS__MIN_SVNDIFF1_FORMAT 2

/* The minimum format number that supports transaction ID generation
   using a transaction sequence in the transaction-current file. */
#define SVN_FS_FS__MIN_TXN_CURRENT_FORMAT 3

/* The minimum format number that supports the "layout" filesystem
   format option. */
#define SVN_FS_FS__MIN_LAYOUT_FORMAT_OPTION_FORMAT 3

/* Maximum number of directories to cache dirents for.
   This *must* be a power of 2 for DIR_CACHE_ENTRIES_MASK
   to work.  */
#define NUM_DIR_CACHE_ENTRIES 128
#define DIR_CACHE_ENTRIES_MASK(x) ((x) & (NUM_DIR_CACHE_ENTRIES - 1))

/* Maximum number of revroot ids to cache dirents for at a time. */
#define NUM_RRI_CACHE_ENTRIES 4096

/* Private FSFS-specific data shared between all svn_txn_t objects that
   relate to a particular transaction in a filesystem (as identified
   by transaction id and filesystem UUID).  Objects of this type are
   allocated in their own subpool of the common pool. */
struct fs_fs_shared_txn_data_t;
typedef struct fs_fs_shared_txn_data_t
{
  /* The next transaction in the list, or NULL if there is no following
     transaction. */
  struct fs_fs_shared_txn_data_t *next;

  /* This transaction's ID.  For repositories whose format is less
     than SVN_FS_FS__MIN_TXN_CURRENT_FORMAT, the ID is in the form
     <rev>-<uniqueifier>, where <uniqueifier> runs from 0-99999 (see
     create_txn_dir_pre_1_5() in fs_fs.c).  For newer repositories,
     the form is <rev>-<200 digit base 36 number> (see
     create_txn_dir() in fs_fs.c). */
  char txn_id[SVN_FS__TXN_MAX_LEN+1];

  /* Whether the transaction's prototype revision file is locked for
     writing by any thread in this process (including the current
     thread; recursive locks are not permitted).  This is effectively
     a non-recursive mutex. */
  svn_boolean_t being_written;

  /* The pool in which this object has been allocated; a subpool of the
     common pool. */
  apr_pool_t *pool;
} fs_fs_shared_txn_data_t;


/* Private FSFS-specific data shared between all svn_fs_t objects that
   relate to a particular filesystem, as identified by filesystem UUID.
   Objects of this type are allocated in the common pool. */
typedef struct
{
  /* A list of shared transaction objects for each transaction that is
     currently active, or NULL if none are.  All access to this list,
     including the contents of the objects stored in it, is synchronised
     under TXN_LIST_LOCK. */
  fs_fs_shared_txn_data_t *txns;

  /* A free transaction object, or NULL if there is no free object.
     Access to this object is synchronised under TXN_LIST_LOCK. */
  fs_fs_shared_txn_data_t *free_txn;

#if APR_HAS_THREADS
  /* A lock for intra-process synchronization when accessing the TXNS list. */
  apr_thread_mutex_t *txn_list_lock;

  /* A lock for intra-process synchronization when grabbing the
     repository write lock. */
  apr_thread_mutex_t *fs_write_lock;

  /* A lock for intra-process synchronization when locking the
     transaction-current file. */
  apr_thread_mutex_t *txn_current_lock;
#endif

  /* The common pool, under which this object is allocated, subpools
     of which are used to allocate the transaction objects. */
  apr_pool_t *common_pool;
} fs_fs_shared_data_t;

typedef struct dag_node_t dag_node_t;

/* Structure for DAG-node cache.  Cache items are arranged in a
   circular LRU list with a dummy entry, and also indexed with a hash
   table.  Transaction nodes are cached within the individual txn
   roots; revision nodes are cached together within the FS object. */
typedef struct dag_node_cache_t
{
  const char *key;                /* Lookup key for cached node: path
                                     for txns; rev catenated with path
                                     for revs */
  dag_node_t *node;               /* Cached node */
  struct dag_node_cache_t *prev;  /* Next node in LRU list */
  struct dag_node_cache_t *next;  /* Previous node in LRU list */
  apr_pool_t *pool;               /* Pool in which node is allocated */
} dag_node_cache_t;


/* Private (non-shared) FSFS-specific data for each svn_fs_t object. */
typedef struct
{
  /* A cache of the last directory opened within the filesystem. */
  svn_fs_id_t *dir_cache_id[NUM_DIR_CACHE_ENTRIES];
  apr_hash_t *dir_cache[NUM_DIR_CACHE_ENTRIES];
  apr_pool_t *dir_cache_pool[NUM_DIR_CACHE_ENTRIES];

  /* The format number of this FS. */
  int format;
  /* The maximum number of files to store per directory (for sharded
     layouts) or zero (for linear layouts). */
  int max_files_per_dir;

  /* The uuid of this FS. */
  const char *uuid;

  /* Caches of immutable data.
     
     Both of these could be moved to fs_fs_shared_data_t to make them
     last longer; on the other hand, this would require adding mutexes
     for threaded builds.
  */

  /* A cache of revision root IDs, allocated in this subpool.  (IDs
     are so small that one pool per ID would be overkill;
     unfortunately, this means the only way we expire cache entries is
     by wiping the whole cache.) */
  apr_hash_t *rev_root_id_cache;
  apr_pool_t *rev_root_id_cache_pool;

  /* DAG node cache for immutable nodes */
  dag_node_cache_t rev_node_list;
  apr_hash_t *rev_node_cache;

  /* Data shared between all svn_fs_t objects for a given filesystem. */
  fs_fs_shared_data_t *shared;
} fs_fs_data_t;


/*** Filesystem Transaction ***/
typedef struct
{
  /* property list (const char * name, svn_string_t * value).
     may be NULL if there are no properties.  */
  apr_hash_t *proplist;

  /* node revision id of the root node.  */
  const svn_fs_id_t *root_id;

  /* node revision id of the node which is the root of the revision
     upon which this txn is base.  (unfinished only) */
  const svn_fs_id_t *base_id;

  /* copies list (const char * copy_ids), or NULL if there have been
     no copies in this transaction.  */
  apr_array_header_t *copies;

} transaction_t;


/*** Representation ***/
/* If you add fields to this, check to see if you need to change
 * svn_fs_fs__rep_copy. */
typedef struct
{
  /* MD5 checksum for the contents produced by this representation.
     This checksum is for the contents the rep shows to consumers,
     regardless of how the rep stores the data under the hood.  It is
     independent of the storage (fulltext, delta, whatever).

     If all the bytes are 0, then for compatibility behave as though
     this checksum matches the expected checksum. */
  unsigned char checksum[APR_MD5_DIGESTSIZE];

  /* Revision where this representation is located. */
  svn_revnum_t revision;

  /* Offset into the revision file where it is located. */
  apr_off_t offset;

  /* The size of the representation in bytes as seen in the revision
     file. */
  svn_filesize_t size;

  /* The size of the fulltext of the representation. */
  svn_filesize_t expanded_size;

  /* Is this representation a transaction? */
  const char *txn_id;

} representation_t;


/*** Node-Revision ***/
/* If you add fields to this, check to see if you need to change
 * copy_node_revision in dag.c. */
typedef struct
{
  /* node kind */
  svn_node_kind_t kind;

  /* The node-id for this node-rev. */
  const svn_fs_id_t *id;

  /* predecessor node revision id, or NULL if there is no predecessor
     for this node revision */
  const svn_fs_id_t *predecessor_id;

  /* If this node-rev is a copy, where was it copied from? */
  const char *copyfrom_path;
  svn_revnum_t copyfrom_rev;

  /* Helper for history tracing, root of the parent tree from whence
     this node-rev was copied. */
  svn_revnum_t copyroot_rev;
  const char *copyroot_path;

  /* number of predecessors this node revision has (recursively), or
     -1 if not known (for backward compatibility). */
  int predecessor_count;

  /* representation key for this node's properties.  may be NULL if
     there are no properties.  */
  representation_t *prop_rep;

  /* representation for this node's data.  may be NULL if there is
     no data. */
  representation_t *data_rep;

  /* path at which this node first came into existence.  */
  const char *created_path;

  /* is this the unmodified root of a transaction? */
  svn_boolean_t is_fresh_txn_root;

} node_revision_t;


/*** Change ***/
typedef struct
{
  /* Path of the change. */
  const char *path;

  /* Node revision ID of the change. */
  const svn_fs_id_t *noderev_id;

  /* The kind of change. */
  svn_fs_path_change_kind_t kind;

  /* Text or property mods? */
  svn_boolean_t text_mod;
  svn_boolean_t prop_mod;

  /* Copyfrom revision and path. */
  svn_revnum_t copyfrom_rev;
  const char * copyfrom_path;

} change_t;


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_FS_H */
