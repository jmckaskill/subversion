/* id.h : interface to node ID functions, private to libsvn_fs
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

#ifndef SVN_LIBSVN_FS_ID_H
#define SVN_LIBSVN_FS_ID_H

#include "svn_fs.h"


/* Return true iff PARENT is a direct parent of CHILD.  */
int svn_fs__is_parent (const svn_fs_id_t *parent,
                       const svn_fs_id_t *child);


#endif /* SVN_LIBSVN_FS_ID_H */



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
