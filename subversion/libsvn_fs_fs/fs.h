/* fs.h : interface to Subversion filesystem, private to libsvn_fs
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

#ifndef SVN_LIBSVN_FS_FS_H
#define SVN_LIBSVN_FS_FS_H

#define APU_WANT_DB
#include <apu_want.h>

#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_md5.h>
#include "svn_fs.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/*** The filesystem structure.  ***/

struct svn_fs_t 
{
  /* A pool managing this filesystem.  Freeing this pool must
     completely clean up the filesystem, including any database
     or system resources it holds.  */
  apr_pool_t *pool;

  /* The path to the repository's top-level directory. */
  char *path;

  /* A callback function for printing warning messages, and a baton to
     pass through to it.  */
  svn_fs_warning_callback_t warning;
  void *warning_baton;

  /* The filesystem configuration. */
  apr_hash_t *config;

  /* The filesystem UUID (or NULL if not-yet-known; see svn_fs_get_uuid). */
  const char *uuid;
};


/* Return a canonicalized version of a filesystem PATH, allocated in
   POOL.  While the filesystem API is pretty flexible about the
   incoming paths (they must be UTF-8 with '/' as separators, but they
   don't have to begin with '/', and multiple contiguous '/'s are
   ignored) we want any paths that are physically stored in the
   underlying database to look consistent.  Specifically, absolute
   filesystem paths should begin with '/', and all redundant and trailing '/'
   characters be removed.  */
const char *
svn_fs__canonicalize_abspath (const char *path, apr_pool_t *pool);


/*** Filesystem Revision ***/
typedef struct
{
  /* id of the transaction that was committed to create this
     revision. */
  const char *txn_id;

} svn_fs__revision_t;


/*** Transaction Kind ***/
typedef enum
{
  svn_fs__transaction_kind_normal = 1,  /* normal, uncommitted */
  svn_fs__transaction_kind_committed,   /* committed */
  svn_fs__transaction_kind_dead         /* uncommitted and dead */

} svn_fs__transaction_kind_t;


/*** Filesystem Transaction ***/
typedef struct
{
  /* kind of transaction. */
  svn_fs__transaction_kind_t kind;

  /* revision which this transaction was committed to create, or an
     invalid revision number if this transaction was never committed. */
  svn_revnum_t revision;

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

} svn_fs__transaction_t;


/*** Node-Revision ***/
typedef struct
{
  /* node kind */
  svn_node_kind_t kind;

  /* predecessor node revision id, or NULL if there is no predecessor
     for this node revision */
  const svn_fs_id_t *predecessor_id;

  /* number of predecessors this node revision has (recursively), or
     -1 if not known (for backward compatibility). */
  int predecessor_count;

  /* representation key for this node's properties.  may be NULL if
     there are no properties.  */
  const char *prop_key;

  /* representation key for this node's text data (files) or entries
     list (dirs).  may be NULL if there are no contents.  */
  const char *data_key;

  /* representation key for this node's text-data-in-progess (files
     only).  NULL if no edits are currently in-progress.  This field
     is always NULL for kinds other than "file".  */
  const char *edit_key;

  /* path at which this node first came into existence.  */
  const char *created_path;

} svn_fs__node_revision_t;


/*** Representation Kind ***/
typedef enum
{
  svn_fs__rep_kind_fulltext = 1, /* fulltext */
  svn_fs__rep_kind_delta         /* delta */

} svn_fs__rep_kind_t;


/*** "Delta" Offset/Window Chunk ***/
typedef struct 
{
  /* diff format version number ### at this point, "svndiff" is the
     only format used. */
  apr_byte_t version;

  /* starting offset of the data represented by this chunk */
  svn_filesize_t offset;

  /* string-key to which this representation points. */
  const char *string_key; 

  /* size of the fulltext data represented by this delta window. */
  apr_size_t size;

  /* represenatation-key to use when needed source data for
     undeltification. */
  const char *rep_key;

  /* apr_off_t rep_offset;  ### not implemented */

} svn_fs__rep_delta_chunk_t;


/*** Representation ***/
typedef struct
{
  /* representation kind */
  svn_fs__rep_kind_t kind;

  /* transaction ID under which representation was created (used as a
     mutability flag when compared with a current editing
     transaction). */
  const char *txn_id;

  /* MD5 checksum for the contents produced by this representation.
     This checksum is for the contents the rep shows to consumers,
     regardless of how the rep stores the data under the hood.  It is
     independent of the storage (fulltext, delta, whatever). 

     If all the bytes are 0, then for compatibility behave as though
     this checksum matches the expected checksum. */
  unsigned char checksum[APR_MD5_DIGESTSIZE];

  /* kind-specific stuff */
  union 
  {
    /* fulltext stuff */
    struct
    {
      /* string-key which holds the fulltext data */
      const char *string_key;

    } fulltext;

    /* delta stuff */
    struct
    {
      /* an array of svn_fs__rep_delta_chunk_t * chunks of delta
         information */
      apr_array_header_t *chunks;

    } delta;
  } contents;
} svn_fs__representation_t;


/*** Copy Kind ***/
typedef enum
{
  svn_fs__copy_kind_real = 1, /* real copy */
  svn_fs__copy_kind_soft      /* soft copy */

} svn_fs__copy_kind_t;


/*** Copy ***/
typedef struct
{
  /* What kind of copy occurred. */
  svn_fs__copy_kind_t kind;

  /* Path of copy source. */
  const char *src_path;

  /* Transaction id of copy source. */
  const char *src_txn_id;

  /* Node-revision of copy destination. */
  const svn_fs_id_t *dst_noderev_id;

} svn_fs__copy_t;


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

} svn_fs__change_t;


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_FS_H */
