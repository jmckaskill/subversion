/*
 * svnadmin.h :  shared declarations between 'svnadmin' C files.
 *
  * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
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



#include <apr_general.h>
#include <apr_pools.h>

#define APR_WANT_STDIO
#define APR_WANT_STRFUNC
#include <apr_want.h>
#include <apr_strings.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_io.h"
#include "svn_path.h"
#include "svn_fs.h"
#include "svn_repos.h"

#include "db.h"



/* Run an interactive shell that will explore already-opened FS. */
svn_error_t *
svnadmin_run_shell (svn_fs_t *fs, apr_pool_t *pool);



/* Context indicating the 'location' of the user in the filesystem.*/
typedef struct shcxt_t
{
  /* the filesystem we're exploring */
  svn_fs_t *fs;

  /* the current working revision */
  svn_revnum_t current_rev;

  /* the root object of the current working revision */
  svn_fs_root_t *root;

  /* the current working directory */
  svn_stringbuf_t *cwd;

  /* top-level pool, where cwd is allocated */
  apr_pool_t *pool;

} shcxt_t;






/* 
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end:
 */

