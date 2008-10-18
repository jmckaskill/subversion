/* checksum-reps-table.h : internal interface to ops on `checksum-reps' table
 *
 * ====================================================================
 * Copyright (c) 2007 CollabNet.  All rights reserved.
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

#ifndef SVN_LIBSVN_FS_CHECKSUM_REPS_TABLE_H
#define SVN_LIBSVN_FS_CHECKSUM_REPS_TABLE_H

#include "svn_fs.h"
#include "svn_error.h"
#include "svn_checksum.h"
#include "../trail.h"
#include "../fs.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* Open a `checksum-reps' table in ENV.  If CREATE is non-zero, create
   one if it doesn't exist.  Set *CHECKSUM_REPS_P to the new table.
   Return a Berkeley DB error code.  */
int svn_fs_bdb__open_checksum_reps_table(DB **checksum_reps_p,
                                         DB_ENV *env,
                                         svn_boolean_t create);

/* Set *REP_KEY to the representation key stored as the value of key
   CHECKSUM in the `checksum-reps' table.  Do this as part of TRAIL.
   Use POOL for allocations.

   If no such node revision ID is stored for CHECKSUM, return
   SVN_ERR_FS_NO_SUCH_CHECKSUM_REP.  */
svn_error_t *svn_fs_bdb__get_checksum_rep(const char **rep_key,
                                          svn_fs_t *fs,
                                          svn_checksum_t *checksum,
                                          trail_t *trail,
                                          apr_pool_t *pool);

/* Store in the `checksum-reps' table a mapping of CHECKSUM to
   representation key REP_KEY in FS.  Do this as part of TRAIL.  Use
   POOL for temporary allocations. 

   WARNING: NEVER store a record that maps a checksum to a mutable
   representation.  Ever.  Under pain of dismemberment and death.  */
svn_error_t *svn_fs_bdb__set_checksum_rep(svn_fs_t *fs,
                                          svn_checksum_t *checksum,
                                          const char *rep_key,
                                          trail_t *trail,
                                          apr_pool_t *pool);

/* Delete from the `checksum-reps' table the mapping of CHECKSUM to a
   representation key in FS.  Do this as part of TRAIL.  Use POOL for
   temporary allocations.  */
svn_error_t *svn_fs_bdb__delete_checksum_rep(svn_fs_t *fs,
                                             svn_checksum_t *checksum,
                                             trail_t *trail,
                                             apr_pool_t *pool);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_CHECKSUM_REPS_TABLE_H */
