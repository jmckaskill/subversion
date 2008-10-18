/* tree.h : internal interface to tree node functions
 *
 * ====================================================================
 * Copyright (c) 2000-2006 CollabNet.  All rights reserved.
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

#ifndef SVN_LIBSVN_FS_TREE_H
#define SVN_LIBSVN_FS_TREE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "svn_props.h"



/* These functions implement some of the calls in the FS loader
   library's fs and txn vtables. */

svn_error_t *svn_fs_base__revision_root(svn_fs_root_t **root_p, svn_fs_t *fs,
                                        svn_revnum_t rev, apr_pool_t *pool);

svn_error_t *svn_fs_base__deltify(svn_fs_t *fs, svn_revnum_t rev,
                                  apr_pool_t *pool);

svn_error_t *svn_fs_base__commit_txn(const char **conflict_p,
                                     svn_revnum_t *new_rev, svn_fs_txn_t *txn,
                                     apr_pool_t *pool);

svn_error_t *svn_fs_base__txn_root(svn_fs_root_t **root_p, svn_fs_txn_t *txn,
                                   apr_pool_t *pool);



/* Inserting and retrieving miscellany records in the fs */

/* Set the value of miscellaneous records KEY to VAL in FS.  To remove
   a value altogether, pass NULL for VAL.
   
   KEY and VAL should be NULL-terminated strings. */
svn_error_t *
svn_fs_base__miscellaneous_set(svn_fs_t *fs,
                               const char *key,
                               const char *val,
                               apr_pool_t *pool);

/* Retrieve the miscellany records for KEY into *VAL for FS, allocated
   in POOL.  If the fs doesn't support miscellany storage, or the value
   does not exist, *VAL is set to NULL.
   
   KEY should be a NULL-terminated string. */
svn_error_t *
svn_fs_base__miscellaneous_get(const char **val,
                               svn_fs_t *fs,
                               const char *key,
                               apr_pool_t *pool);





/* Helper func: in the context of TRAIL, return the KIND of PATH in
   head revision.   If PATH doesn't exist, set *KIND to svn_node_none.*/
svn_error_t *svn_fs_base__get_path_kind(svn_node_kind_t *kind,
                                        const char *path,
                                        trail_t *trail,
                                        apr_pool_t *pool);

/* Helper func: in the context of TRAIL, set *REV to the created-rev
   of PATH in head revision.  If PATH doesn't exist, set *REV to
   SVN_INVALID_REVNUM. */
svn_error_t *svn_fs_base__get_path_created_rev(svn_revnum_t *rev,
                                               const char *path,
                                               trail_t *trail,
                                               apr_pool_t *pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_TREE_H */
