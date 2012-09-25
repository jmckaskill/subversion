/*
 * revert-cmd.c -- Subversion revert command
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

#include "svn_client.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "cl.h"



/*** Code. ***/

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__revert (apr_getopt_t *os,
                void *baton,
                apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_array_header_t *targets;
  int i;
  svn_boolean_t recursive = opt_state->recursive;
  apr_pool_t *subpool;

  SVN_ERR (svn_opt_args_to_target_array (&targets, os, 
                                         opt_state->targets,
                                         &(opt_state->start_revision),
                                         &(opt_state->end_revision),
                                         FALSE, pool));

  /* Revert has no implicit dot-target `.', so don't you put that code here! */
  if (! targets->nelts)
    return svn_error_create (SVN_ERR_CL_ARG_PARSING_ERROR, 0, "");

  if (! opt_state->quiet)
    svn_cl__get_notifier (&ctx->notify_func, &ctx->notify_baton, FALSE, FALSE,
                          FALSE, pool);

  subpool = svn_pool_create (pool);
  for (i = 0; i < targets->nelts; i++)
    {
      const char *target = ((const char **) (targets->elts))[i];
      svn_error_t *err;

      err = svn_client_revert (target, recursive, ctx, subpool);
      if (err)
        {
          if (err->apr_err == SVN_ERR_ENTRY_NOT_FOUND)
            {
              if (!opt_state->quiet)
                {
                  svn_handle_warning (stderr, err);
                }
              svn_error_clear (err);
              continue;
            }
          else
            return err;
        }

      SVN_ERR (svn_cl__check_cancel (ctx->cancel_baton));
      svn_pool_clear (subpool);
    }
  
  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}