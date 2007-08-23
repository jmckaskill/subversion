/* revs-txns.h : internal interface to revision and transactions operations
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

#ifndef SVN_LIBSVN_FS_REVS_TXNS_H
#define SVN_LIBSVN_FS_REVS_TXNS_H

#define APU_WANT_DB
#include <apu_want.h>

#include "svn_fs.h"

#include "fs.h"
#include "trail.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */



/*** Revisions ***/

/* Set *ROOT_ID_P to the ID of the root directory of revision REV in FS,
   as part of TRAIL.  Allocate the ID in POOL.  */
svn_error_t *svn_fs_base__rev_get_root(const svn_fs_id_t **root_id_p,
                                       svn_fs_t *fs,
                                       svn_revnum_t rev,
                                       trail_t *trail,
                                       apr_pool_t *pool);


/* Set *TXN_ID_P to the ID of the transaction that was committed to
   create REV in FS, as part of TRAIL.  Allocate the ID in POOL.  */
svn_error_t *svn_fs_base__rev_get_txn_id(const char **txn_id_p,
                                         svn_fs_t *fs,
                                         svn_revnum_t rev,
                                         trail_t *trail,
                                         apr_pool_t *pool);


/* Set property NAME to VALUE on REV in FS, as part of TRAIL.  */
svn_error_t *svn_fs_base__set_rev_prop(svn_fs_t *fs,
                                       svn_revnum_t rev,
                                       const char *name,
                                       const svn_string_t *value,
                                       trail_t *trail,
                                       apr_pool_t *pool);



/*** Transactions ***/

/* Convert the unfinished transaction in FS named TXN_NAME to a
   committed transaction that refers to REVISION as part of TRAIL.

   Returns SVN_ERR_FS_TRANSACTION_NOT_MUTABLE if TXN_NAME refers to a
   transaction that has already been committed.  */
svn_error_t *svn_fs_base__txn_make_committed(svn_fs_t *fs,
                                             const char *txn_name,
                                             svn_revnum_t revision,
                                             trail_t *trail,
                                             apr_pool_t *pool);


/* Set *REVISION to the revision which was created when FS transaction
   TXN_NAME was committed, or to SVN_INVALID_REVNUM if the transaction
   has not been committed.  Do all of this as part of TRAIL.  */
svn_error_t *svn_fs_base__txn_get_revision(svn_revnum_t *revision,
                                           svn_fs_t *fs,
                                           const char *txn_name,
                                           trail_t *trail,
                                           apr_pool_t *pool);


/* Retrieve information about the Subversion transaction SVN_TXN from
   the `transactions' table of FS, as part of TRAIL.
   Set *ROOT_ID_P to the ID of the transaction's root directory.
   Set *BASE_ROOT_ID_P to the ID of the root directory of the
   transaction's base revision.

   If there is no such transaction, SVN_ERR_FS_NO_SUCH_TRANSACTION is
   the error returned.

   Returns SVN_ERR_FS_TRANSACTION_NOT_MUTABLE if TXN_NAME refers to a
   transaction that has already been committed.

   Allocate *ROOT_ID_P and *BASE_ROOT_ID_P in POOL.  */
svn_error_t *svn_fs_base__get_txn_ids(const svn_fs_id_t **root_id_p,
                                      const svn_fs_id_t **base_root_id_p,
                                      svn_fs_t *fs,
                                      const char *txn_name,
                                      trail_t *trail,
                                      apr_pool_t *pool);


/* Set the root directory of the Subversion transaction TXN_NAME in FS
   to ROOT_ID, as part of TRAIL.  Do any necessary temporary
   allocation in POOL.

   Returns SVN_ERR_FS_TRANSACTION_NOT_MUTABLE if TXN_NAME refers to a
   transaction that has already been committed.  */
svn_error_t *svn_fs_base__set_txn_root(svn_fs_t *fs,
                                       const char *txn_name,
                                       const svn_fs_id_t *root_id,
                                       trail_t *trail,
                                       apr_pool_t *pool);


/* Add COPY_ID to the list of copies made under the Subversion
   transaction TXN_NAME in FS as part of TRAIL.

   Returns SVN_ERR_FS_TRANSACTION_NOT_MUTABLE if TXN_NAME refers to a
   transaction that has already been committed.  */
svn_error_t *svn_fs_base__add_txn_copy(svn_fs_t *fs,
                                       const char *txn_name,
                                       const char *copy_id,
                                       trail_t *trail,
                                       apr_pool_t *pool);


/* Set the base root directory of TXN_NAME in FS to NEW_ID, as part of
   TRAIL.  Do any necessary temporary allocation in POOL.

   Returns SVN_ERR_FS_TRANSACTION_NOT_MUTABLE if TXN_NAME refers to a
   transaction that has already been committed.  */
svn_error_t *svn_fs_base__set_txn_base(svn_fs_t *fs,
                                       const char *txn_name,
                                       const svn_fs_id_t *new_id,
                                       trail_t *trail,
                                       apr_pool_t *pool);


/* Set a PATH to VALUE for the SVN_FS_PROP_TXN_MERGEINFO property (a
   serialized hash table) on transaction TXN_NAME's proplist in FS as
   part of TRAIL.  Use POOL for any necessary allocations.

   Return SVN_ERR_FS_TRANSACTION_NOT_MUTABLE if TXN_NAME refers to a
   transaction that has already been committed.  */
svn_error_t *
svn_fs_base__set_txn_mergeinfo(svn_fs_t *fs,
                               const char *txn_name,
                               const char *path,
                               const svn_string_t *value,
                               trail_t *trail,
                               apr_pool_t *pool);

/* Set a property NAME to VALUE on transaction TXN_NAME in FS as part
   of TRAIL.  Use POOL for any necessary allocations.

   Returns SVN_ERR_FS_TRANSACTION_NOT_MUTABLE if TXN_NAME refers to a
   transaction that has already been committed.  */
svn_error_t *svn_fs_base__set_txn_prop(svn_fs_t *fs,
                                       const char *txn_name,
                                       const char *name,
                                       const svn_string_t *value,
                                       trail_t *trail,
                                       apr_pool_t *pool);


/* These functions implement some of the calls in the FS loader
   library's fs and txn vtables. */

svn_error_t *svn_fs_base__youngest_rev(svn_revnum_t *youngest_p, svn_fs_t *fs,
                                       apr_pool_t *pool);

svn_error_t *svn_fs_base__revision_prop(svn_string_t **value_p, svn_fs_t *fs,
                                        svn_revnum_t rev,
                                        const char *propname,
                                        apr_pool_t *pool);

svn_error_t *svn_fs_base__revision_proplist(apr_hash_t **table_p,
                                            svn_fs_t *fs,
                                            svn_revnum_t rev,
                                            apr_pool_t *pool);

svn_error_t *svn_fs_base__change_rev_prop(svn_fs_t *fs, svn_revnum_t rev,
                                          const char *name,
                                          const svn_string_t *value,
                                          apr_pool_t *pool);

svn_error_t *svn_fs_base__begin_txn(svn_fs_txn_t **txn_p, svn_fs_t *fs,
                                    svn_revnum_t rev, apr_uint32_t flags,
                                    apr_pool_t *pool);

svn_error_t *svn_fs_base__open_txn(svn_fs_txn_t **txn, svn_fs_t *fs,
                                   const char *name, apr_pool_t *pool);

svn_error_t *svn_fs_base__purge_txn(svn_fs_t *fs, const char *txn_id,
                                    apr_pool_t *pool);

svn_error_t *svn_fs_base__list_transactions(apr_array_header_t **names_p,
                                            svn_fs_t *fs, apr_pool_t *pool);

svn_error_t *svn_fs_base__abort_txn(svn_fs_txn_t *txn, apr_pool_t *pool);

svn_error_t *svn_fs_base__txn_prop(svn_string_t **value_p, svn_fs_txn_t *txn,
                                   const char *propname, apr_pool_t *pool);

svn_error_t *svn_fs_base__txn_proplist(apr_hash_t **table_p,
                                       svn_fs_txn_t *txn,
                                       apr_pool_t *pool);

/* Helper func:  variant of __txn_proplist that uses an existing TRAIL. 
 * TXN_ID identifies the transaction.
 * *TABLE_P will be non-null upon success. 
 */
svn_error_t *svn_fs_base__txn_proplist_in_trail(apr_hash_t **table_p,
                                                const char *txn_id,
                                                trail_t *trail);

svn_error_t *svn_fs_base__change_txn_prop(svn_fs_txn_t *txn, const char *name,
                                          const svn_string_t *value,
                                          apr_pool_t *pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_REVS_TXNS_H */
