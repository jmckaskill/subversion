/*
 * move-cmd.c -- Subversion move command
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__move (apr_getopt_t *os,
              void *baton,
              apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_array_header_t *targets;
  const char *src_path, *dst_path;
  svn_client_commit_info_t *commit_info = NULL;
  svn_error_t *err;
  svn_wc_notify_func_t notify_func = NULL;
  void *notify_baton = NULL;
  void *log_msg_baton;

  SVN_ERR (svn_opt_args_to_target_array (&targets, os, 
                                         opt_state->targets,
                                         &(opt_state->start_revision),
                                         &(opt_state->end_revision),
                                         FALSE, pool));

  if (targets->nelts != 2)
    return svn_error_create (SVN_ERR_CL_ARG_PARSING_ERROR, 0, "");

  src_path = ((const char **) (targets->elts))[0];
  dst_path = ((const char **) (targets->elts))[1];
  
  if (! opt_state->quiet)
    svn_cl__get_notifier (&notify_func, &notify_baton, FALSE, FALSE, FALSE,
			  pool);

  log_msg_baton = svn_cl__make_log_msg_baton (opt_state, NULL, pool);
  err = svn_client_move 
           (&commit_info, 
            src_path, &(opt_state->start_revision), dst_path,
            opt_state->force,
            &svn_cl__get_log_message,
            log_msg_baton,
            notify_func, notify_baton, ctx,
            pool);

  if (err)
    err = svn_cl__may_need_force (err);
  SVN_ERR (svn_cl__cleanup_log_msg (log_msg_baton, err));

  if (commit_info && ! opt_state->quiet)
    svn_cl__print_commit_info (commit_info);

  return SVN_NO_ERROR;
}
