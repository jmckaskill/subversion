/*
 * commit-cmd.c -- Check changes into the repository.
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
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

#include <apr_general.h>
#include <apr_file_info.h>
#include <apr_lib.h>

#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_error.h"
#include "svn_sorts.h"
#include "cl.h"

#include "client_errors.h"



/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__commit (apr_getopt_t *os,
                void *baton,
                apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_array_header_t *targets;
  apr_array_header_t *condensed_targets;
  const char *base_dir;
  svn_client_commit_info_t *commit_info = NULL;

  SVN_ERR (svn_opt_args_to_target_array (&targets, os, 
                                         opt_state->targets,
                                         &(opt_state->start_revision),
                                         &(opt_state->end_revision),
                                         FALSE, pool));

  /* Add "." if user passed 0 arguments. */
  svn_opt_push_implicit_dot_target (targets, pool);

  /* Condense the targets (like commit does)... */
  SVN_ERR (svn_path_condense_targets (&base_dir,
                                      &condensed_targets,
                                      targets,
                                      TRUE,
                                      pool));

  if ((! condensed_targets) || (! condensed_targets->nelts))
    {
      const char *parent_dir, *base_name;

      SVN_ERR (svn_wc_get_actual_target (base_dir, &parent_dir, 
                                         &base_name, pool));
      if (base_name)
        base_dir = apr_pstrdup (pool, parent_dir);
    }

  if (! opt_state->quiet)
    svn_cl__get_notifier (&ctx->notify_func, &ctx->notify_baton, FALSE, FALSE,
                          FALSE, pool);

  /* We're creating a new log message baton because we can use our base_dir 
     to store the temp file, instead of the current working directory.  The 
     client might not have write access to their working directory, but they 
     better have write access to the directory they're committing.  */
  SVN_ERR (svn_cl__make_log_msg_baton (&(ctx->log_msg_baton),
                                       opt_state, base_dir, 
                                       ctx->config, pool));

  /* Commit. */
  SVN_ERR (svn_cl__cleanup_log_msg
           (ctx->log_msg_baton, svn_client_commit (&commit_info,
                                                   targets,
                                                   opt_state->nonrecursive,
                                                   ctx,
                                                   pool)));
  if (commit_info && ! opt_state->quiet)
    svn_cl__print_commit_info (commit_info);

  return SVN_NO_ERROR;
}
