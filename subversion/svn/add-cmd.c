/*
 * add-cmd.c -- Subversion add/unadd commands
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
#define APR_WANT_STDIO
#include <apr_want.h>

#include "svn_client.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "cl.h"



/*** Code. ***/

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__add(apr_getopt_t *os,
            void *baton,
            apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_array_header_t *targets;
  int i;
  apr_pool_t *subpool;

  SVN_ERR(svn_opt_args_to_target_array2(&targets, os, 
                                        opt_state->targets, pool));

  if (! targets->nelts)
    return svn_error_create(SVN_ERR_CL_INSUFFICIENT_ARGS, 0, NULL);
      
  if (! opt_state->quiet)
    svn_cl__get_notifier(&ctx->notify_func2, &ctx->notify_baton2, FALSE,
                         FALSE, FALSE, pool);

  subpool = svn_pool_create(pool);
  for (i = 0; i < targets->nelts; i++)
    {
      const char *target = ((const char **) (targets->elts))[i];

      svn_pool_clear(subpool);
      SVN_ERR(svn_cl__check_cancel(ctx->cancel_baton));
      SVN_ERR(svn_cl__try 
              (svn_client_add3(target, (! opt_state->nonrecursive), 
                               opt_state->force, opt_state->no_ignore,
                               ctx, subpool),
               NULL, opt_state->quiet,
               SVN_ERR_ENTRY_EXISTS,
               SVN_ERR_WC_PATH_NOT_FOUND,
               SVN_NO_ERROR));
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}
