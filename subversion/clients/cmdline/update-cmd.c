/*
 * update-cmd.c -- Bring work tree in sync with repository
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

#include "svn_wc.h"
#include "svn_pools.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_error.h"
#include "cl.h"


/*** Code. ***/

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__update (apr_getopt_t *os,
                void *baton,
                apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_array_header_t *targets;
  apr_array_header_t *condensed_targets;
  apr_pool_t *subpool = svn_pool_create (pool);
  int i;

  SVN_ERR (svn_opt_args_to_target_array (&targets, os, 
                                         opt_state->targets,
                                         &(opt_state->start_revision),
                                         &(opt_state->end_revision),
                                         FALSE, pool));

  /* Add "." if user passed 0 arguments */
  svn_opt_push_implicit_dot_target (targets, pool);

  /* Remove redundancies from the target list while preserving order. */
  SVN_ERR (svn_path_remove_redundancies (&condensed_targets,
                                         targets,
                                         pool));

  for (i = 0; i < condensed_targets->nelts; i++)
    {
      const char *target = ((const char **) (condensed_targets->elts))[i];
      svn_error_t *err;

      svn_pool_clear (subpool);
      SVN_ERR (svn_cl__check_cancel (ctx->cancel_baton));

      if (! opt_state->quiet)
        svn_cl__get_notifier (&ctx->notify_func, &ctx->notify_baton, 
                              FALSE, FALSE, FALSE, subpool);

      err = svn_client_update (NULL, target,
                               &(opt_state->start_revision),
                               opt_state->nonrecursive ? FALSE : TRUE,
                               ctx, subpool);
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
    }

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}
