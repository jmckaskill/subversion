/*
 * proplist-cmd.c -- Display status information in current directory
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

/* ==================================================================== */



/*** Includes. ***/

#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_error.h"
#include "cl.h"


/*** Code. ***/

svn_error_t *
svn_cl__proplist (svn_cl__opt_state_t *opt_state,
                  apr_array_header_t *targets,
                  apr_pool_t *pool)
{
  svn_error_t *err;
  int i;

  /* Add "." if user passed 0 arguments */
  push_implicit_dot_target(targets, pool);

  for (i = 0; i < targets->nelts; i++)
    {
      svn_string_t *target = ((svn_string_t **) (targets->elts))[i];
      apr_hash_t *prop_hash = apr_hash_make (pool);

      err = svn_wc_prop_list (&prop_hash, target, pool);
      if (err)
        return err;

      svn_cl__print_prop_hash (prop_hash, pool);
    }

  return SVN_NO_ERROR;
}


/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: 
 */
