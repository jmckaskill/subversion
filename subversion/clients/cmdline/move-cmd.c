/*
 * move-cmd.c -- Subversion move command
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
svn_cl__move (apr_getopt_t *os,
              svn_cl__opt_state_t *opt_state,
              apr_pool_t *pool)
{
  apr_array_header_t *targets;
  svn_stringbuf_t *src_path, *dst_path;
  svn_client_auth_baton_t *auth_baton = NULL;
  svn_stringbuf_t *message = NULL;
  svn_client_commit_info_t *commit_info = NULL;

  targets = svn_cl__args_to_target_array (os, pool);

  if (targets->nelts != 2)
    {
      svn_cl__subcommand_help ("move", pool);
      return svn_error_create (SVN_ERR_CL_ARG_PARSING_ERROR, 0, 0, pool, "");
    }

  /* Take our message from ARGV or a FILE */
  if (opt_state->filedata) 
    message = opt_state->filedata;
  else
    message = opt_state->message;
  
  /* Build an authentication object to give to libsvn_client. */
  auth_baton = svn_cl__make_auth_baton (opt_state, pool);

  src_path = ((svn_stringbuf_t **) (targets->elts))[0];
  dst_path = ((svn_stringbuf_t **) (targets->elts))[1];
  
  SVN_ERR (svn_client_move 
           (&commit_info, 
            src_path, opt_state->start_revision, dst_path, auth_baton, 
            message ? message : svn_stringbuf_create ("", pool), pool));

  if (commit_info)
    svn_cl__print_commit_info (commit_info);

  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../../svn-dev.el")
 * end: 
 */
