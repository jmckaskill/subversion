/*
 * lock.c:  routines for locking working copy subdirectories.
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



#include <apr_pools.h>
#include <apr_time.h>
#include "wc.h"




svn_error_t *
svn_wc__lock (svn_string_t *path, int wait_for, apr_pool_t *pool)
{
  svn_error_t *err;

  do {
    err = svn_wc__make_adm_thing (path, SVN_WC__ADM_LOCK,
                                  svn_node_file, 0, pool);
    if (err && APR_STATUS_IS_EEXIST(err->apr_err))
      {
        svn_error_free (err);
        apr_sleep (1 * APR_USEC_PER_SEC);  /* micro-seconds */
        wait_for--;
      }
    else
      return SVN_NO_ERROR;
  } while (wait_for > 0);

  return svn_error_createf (SVN_ERR_WC_LOCKED, 0, NULL, pool, 
                            "working copy locked: %s", path->data); 
}


svn_error_t *
svn_wc__unlock (svn_string_t *path, apr_pool_t *pool)
{
  return svn_wc__remove_adm_file (path, pool, SVN_WC__ADM_LOCK, NULL);
}


svn_error_t *
svn_wc__locked (svn_boolean_t *locked, svn_string_t *path, apr_pool_t *pool)
{
  svn_error_t *err;
  enum svn_node_kind kind;
  svn_string_t *lockfile
    = svn_wc__adm_path (path, 0, pool, SVN_WC__ADM_LOCK, NULL);
                                             
  err = svn_io_check_path (lockfile, &kind, pool);
  if (err)
    return err;
  else if (kind == svn_node_file)
    *locked = 1;
  else if (kind == svn_node_none)
    *locked = 0;
  else
    return svn_error_createf (SVN_ERR_WC_LOCKED,
                              0,
                              NULL,
                              pool,
                              "svn_wc__locked: "
                              "lock file is not a regular file (%s)",
                              path->data);
    
  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
