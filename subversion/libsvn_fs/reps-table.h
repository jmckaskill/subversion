/* reps-table.h : internal interface to `representations' table
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
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

#ifndef SVN_LIBSVN_FS_REPS_TABLE_H
#define SVN_LIBSVN_FS_REPS_TABLE_H

#include "db.h"
#include "svn_io.h"
#include "svn_fs.h"
#include "fs.h"
#include "trail.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/*** Creating the `representations' table. ***/

/* Open a `representations' table in ENV.  If CREATE is non-zero,
   create one if it doesn't exist.  Set *REPS_P to the new table.
   Return a Berkeley DB error code.  */
int svn_fs__open_reps_table (DB **reps_p, DB_ENV *env, int create);



/*** Storing and retrieving reps.  ***/

/* Set *REP_P to point to the representation for the key KEY in
   FS, as part of TRAIL.  Perform all allocations in TRAIL->pool.

   If KEY is not a representation in FS, the error
   SVN_ERR_FS_NO_SUCH_REPRESENTATION is returned.  */
svn_error_t *svn_fs__read_rep (svn_fs__representation_t **rep_p,
                               svn_fs_t *fs,
                               const char *key,
                               trail_t *trail);


/* Store REP as the representation for KEY in FS, as part of
   TRAIL.  Do any necessary temporary allocation in TRAIL->pool.  */
svn_error_t *svn_fs__write_rep (svn_fs_t *fs,
                                const char *key,
                                svn_fs__representation_t *rep,
                                trail_t *trail);


/* Store REP as a new representation in FS, and the new rep's key in
   *KEY, as part of trail.  The new key is allocated in TRAIL->pool.  */
svn_error_t *svn_fs__write_new_rep (const char **key,
                                    svn_fs_t *fs,
                                    svn_fs__representation_t *rep,
                                    trail_t *trail);

/* Delete representation KEY from FS, as part of TRAIL.
   WARNING: This does not ensure that no one references this
   representation!  Callers should ensure that themselves.  */
svn_error_t *svn_fs__delete_rep (svn_fs_t *fs,
                                 const char *key,
                                 trail_t *trail);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_REPS_TABLE_H */


/* 
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end:
 */
