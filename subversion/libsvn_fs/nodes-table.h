/* nodes-table.h : interface to `nodes' table
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

#ifndef SVN_LIBSVN_FS_NODES_TABLE_H
#define SVN_LIBSVN_FS_NODES_TABLE_H

#include "db.h"
#include "svn_fs.h"
#include "skel.h"
#include "trail.h"


/* Creating and opening the `nodes' table.  */


/* Open a `nodes' table in ENV.  If CREATE is non-zero, create
   one if it doesn't exist.  Set *NODES_P to the new table.  
   Return a Berkeley DB error code.  */
int svn_fs__open_nodes_table (DB **nodes_p,
                              DB_ENV *env,
                              int create);


/* Set *SKEL_P to point to the REPRESENTATION skel for the node ID in
   FS, as part of TRAIL.  Allocate the skel and the data it points
   into in TRAIL->pool.

   This verifies that *SKEL_P is a well-formed REPRESENTATION skel.  */
svn_error_t *svn_fs__get_rep (skel_t **skel_p,
                              svn_fs_t *fs,
                              const svn_fs_id_t *id,
                              trail_t *trail);


/* Store SKEL as the REPRESENTATION skel of node ID in FS, as part of
   TRAIL.  Do any necessary temporary allocation in TRAIL->pool.

   This verifies that SKEL is a well-formed REPRESENTATION skel.  */
svn_error_t *svn_fs__put_rep (svn_fs_t *fs,
                              const svn_fs_id_t *id,
                              skel_t *skel,
                              trail_t *trail);


/* Check FS's `nodes' table to find an unused node number, and set
   *ID_P to the ID of the first revision of an entirely new node in
   FS, as part of TRAIL.  Allocate the new ID, and do all temporary
   allocation, in TRAIL->pool.  */
svn_error_t *svn_fs__new_node_id (svn_fs_id_t **id_p,
                                  svn_fs_t *fs,
                                  trail_t *trail);


/* Set *SUCCESSOR_P to the ID of an immediate successor to node
   revision ID in FS that does not exist yet, as part of TRAIL.
   Allocate *SUCCESSOR_P in TRAIL->pool.

   If ID is the youngest revision of its node, then the successor is
   simply ID with its rightmost revision number increased; otherwise,
   the successor is a new branch from ID.  */
svn_error_t *svn_fs__new_successor_id (svn_fs_id_t **successor_p,
                                       svn_fs_t *fs,
                                       const svn_fs_id_t *id,
                                       trail_t *trail);


#endif /* SVN_LIBSVN_FS_NODES_TABLE_H */



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
