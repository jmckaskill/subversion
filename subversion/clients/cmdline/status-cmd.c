/*
 * status-cmd.c -- Display status information in current directory
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
svn_cl__status (apr_getopt_t *os,
                void *baton,
                apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_hash_t *statushash;
  apr_array_header_t *targets;
  int i;
  svn_revnum_t youngest = SVN_INVALID_REVNUM;
  svn_wc_notify_func_t notify_func = NULL;
  void *notify_baton = NULL;

  SVN_ERR (svn_opt_args_to_target_array (&targets, os, 
                                         opt_state->targets,
                                         &(opt_state->start_revision),
                                         &(opt_state->end_revision),
                                         FALSE, pool));

  /* Build an authentication object to give to libsvn_client. */
  svn_client_ctx_set_auth_baton (ctx,
                                 svn_cl__make_auth_baton (opt_state, pool));

  /* The notification callback. */
  svn_cl__get_notifier (&notify_func, &notify_baton, FALSE, FALSE, FALSE, pool);

  /* Add "." if user passed 0 arguments */
  svn_opt_push_implicit_dot_target(targets, pool);

  for (i = 0; i < targets->nelts; i++)
    {
      const char *target = ((const char **) (targets->elts))[i];

      /* Retrieve a hash of status structures with the information
         requested by the user.

         svn_client_status directly understands the three commandline
         switches (-n, -u, -[vV]) : */

      SVN_ERR (svn_client_status (&statushash, &youngest, target,
                                  opt_state->nonrecursive ? FALSE : TRUE,
                                  opt_state->verbose,
                                  opt_state->update,
                                  opt_state->no_ignore,
                                  notify_func, notify_baton,
                                  ctx, pool));

      /* Now print the structures to the screen.
         The flag we pass indicates whether to use the 'detailed'
         output format or not. */
      svn_cl__print_status_list (statushash,
                                 youngest,
                                 (opt_state->verbose || opt_state->update),
                                 opt_state->verbose,
                                 opt_state->quiet,
                                 pool);
    }

  return SVN_NO_ERROR;
}
