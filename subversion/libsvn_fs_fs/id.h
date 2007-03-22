/* id.h : interface to node ID functions, private to libsvn_fs_fs
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

#ifndef SVN_LIBSVN_FS_FS_ID_H
#define SVN_LIBSVN_FS_FS_ID_H

#include "svn_fs.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/*** ID accessor functions. ***/

/* Get the "node id" portion of ID. */
const char *svn_fs_fs__id_node_id(const svn_fs_id_t *id);

/* Get the "copy id" portion of ID. */
const char *svn_fs_fs__id_copy_id(const svn_fs_id_t *id);

/* Get the "txn id" portion of ID, or NULL if it is a permanent ID. */
const char *svn_fs_fs__id_txn_id(const svn_fs_id_t *id);

/* Get the "rev" portion of ID, or SVN_INVALID_REVNUM if it is a
   transaction ID. */
svn_revnum_t svn_fs_fs__id_rev(const svn_fs_id_t *id);

/* Access the "offset" portion of the ID, or -1 if it is a transaction
   ID. */
apr_off_t svn_fs_fs__id_offset(const svn_fs_id_t *id);

/* Convert ID into string form, allocated in POOL. */
svn_string_t *svn_fs_fs__id_unparse(const svn_fs_id_t *id,
                                    apr_pool_t *pool);

/* Return true if A and B are equal. */
svn_boolean_t svn_fs_fs__id_eq(const svn_fs_id_t *a,
                               const svn_fs_id_t *b);

/* Return true if A and B are related. */
svn_boolean_t svn_fs_fs__id_check_related(const svn_fs_id_t *a,
                                          const svn_fs_id_t *b);

/* Return 0 if A and B are equal, 1 if they are related, -1 otherwise. */
int svn_fs_fs__id_compare(const svn_fs_id_t *a,
                          const svn_fs_id_t *b);

/* Create an ID within a transaction based on NODE_ID, COPY_ID, and
   TXN_ID, allocated in POOL. */
svn_fs_id_t *svn_fs_fs__id_txn_create(const char *node_id,
                                      const char *copy_id,
                                      const char *txn_id,
                                      apr_pool_t *pool);

/* Create a permanent ID based on NODE_ID, COPY_ID, REV, and OFFSET,
   allocated in POOL. */
svn_fs_id_t *svn_fs_fs__id_rev_create(const char *node_id,
                                      const char *copy_id,
                                      svn_revnum_t rev,
                                      apr_off_t offset,
                                      apr_pool_t *pool);

/* Return a copy of ID, allocated from POOL. */
svn_fs_id_t *svn_fs_fs__id_copy(const svn_fs_id_t *id, 
                                apr_pool_t *pool);

/* Return an ID resulting from parsing the string DATA (with length
   LEN), or NULL if DATA is an invalid ID string. */
svn_fs_id_t *svn_fs_fs__id_parse(const char *data,
                                 apr_size_t len,
                                 apr_pool_t *pool);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_FS_ID_H */
