/*
 * propedit-cmd.c -- Edit properties of files/dirs using $EDITOR
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
svn_cl__propedit (apr_getopt_t *os,
                  svn_cl__opt_state_t *opt_state,
                  apr_pool_t *pool)
{
  svn_stringbuf_t *propname;
  apr_array_header_t *targets;
  int i;
  const char *editor_cmd;

  /* ### todo:  remove this once the feature is completed. */
  return svn_error_create 
    (SVN_ERR_UNSUPPORTED_FEATURE, 0, NULL, pool, 
     "Property editing (via `propedit') has not been implemented.");

  SVN_ERR (svn_cl__parse_num_args (os, opt_state, "propedit", 1, pool));

  /* Get the property's name. */
  propname = ((svn_stringbuf_t **) (opt_state->args->elts))[0];

  /* Suck up all the remaining arguments into a targets array */
  targets = svn_cl__args_to_target_array (os, pool);

  /* Add "." if user passed 0 file arguments */
  svn_cl__push_implicit_dot_target (targets, pool);

  /* Get the EDITOR environment variable. */
  editor_cmd = getenv ("EDITOR");
  if (! editor_cmd)
    return svn_error_create (SVN_ERR_MISSING_ENV_VARIABLE, 0, NULL, 
                             pool, "EDITOR");

  /* For each target, edit the property PNAME. */
  for (i = 0; i < targets->nelts; i++)
    {
      apr_hash_t *props;
      svn_stringbuf_t *target = ((svn_stringbuf_t **) (targets->elts))[i];

      /* Fetch the current property. */
      SVN_ERR (svn_client_propget (&props, propname->data, target->data,
                                   FALSE, pool));

      /* ### todo: all the following: */

      /* Dump its contents to a temporary file. */
      /* Throw the file into the EDITOR. */
      /* Now, re-read the contents of the file... */
      /* ...and re-set the property's value accordingly. */
    }

  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../../../tools/dev/svn-dev.el")
 * end: 
 */
