/* rev-table.h : internal interface to revision table operations
 *
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

#ifndef SVN_LIBSVN_FS_REV_TABLE_H
#define SVN_LIBSVN_FS_REV_TABLE_H

#include <db.h>
#include "svn_fs.h"
#include "skel.h"
#include "trail.h"


/* Creating and opening the `revisions' table.  */

/* Open a `revisions' table in ENV.  If CREATE is non-zero, create one
   if it doesn't exist.  Set *REVS_P to the new table.  Return a
   Berkeley DB error code.  */
int svn_fs__open_revisions_table (DB **revisions_p,
                                  DB_ENV *env,
                                  int create);



/* Storing and retrieving filesystem revisions.  */


/* Set *SKEL_P to point to the REVISION skel for the filesystem
   revision REV in FS, as part of TRAIL.
   Allocate the skel and the data it points into in TAIL->pool.

   This verifies that *SKEL_P is a well-formed REVISION skel.  */
svn_error_t *svn_fs__get_rev (skel_t **skel_p,
                              svn_fs_t *fs,
                              svn_revnum_t rev,
                              trail_t *trail);

/* Store SKEL as the REVISION skel in FS as part of TRAIL, and return
   the new filesystem revision number in *REV.  Do any necessary
   temporary allocation in TRAIL->pool.

   This verifies that SKEL is a well-formed REVISION skel.  */
svn_error_t *svn_fs__put_rev (svn_revnum_t *rev,
                              svn_fs_t *fs,
                              skel_t *skel,
                              trail_t *trail);


/* Set *ROOT_ID_P to the ID of the root directory of revision REV in FS,
   as part of TRAIL.  Allocate the ID in TRAIL->pool.  */
svn_error_t *svn_fs__rev_get_root (svn_fs_id_t **root_id_p,
                                   svn_fs_t *fs,
                                   svn_revnum_t rev,
                                   trail_t *trail);


/* Set *YOUNGEST_P to the youngest revision in filesystem FS,
   as part of TRAIL.  Use TRAIL->pool for all temporary allocation. */
svn_error_t *svn_fs__youngest_rev (svn_revnum_t *youngest_p,
                                   svn_fs_t *fs,
                                   trail_t *trail);



#endif /* SVN_LIBSVN_FS_REV_TABLE_H */



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
