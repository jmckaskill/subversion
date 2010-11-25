/*
 * update-cmd.c -- Bring work tree in sync with repository
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

/* ==================================================================== */



/*** Includes. ***/

#include "svn_pools.h"
#include "svn_client.h"
#include "svn_path.h"
#include "svn_error_codes.h"
#include "svn_error.h"
#include "cl.h"

#include "svn_private_config.h"


/*** Code. ***/

/* Print an update summary when there's more than one target to report
   about. */
static svn_error_t *
print_update_summary(apr_array_header_t *targets,
                     apr_array_header_t *result_revs,
                     apr_pool_t *scratch_pool)
{
  int i;
  const char *path_prefix;
  apr_pool_t *iter_pool;

  if (targets->nelts < 2)
    return SVN_NO_ERROR;

  SVN_ERR(svn_dirent_get_absolute(&path_prefix, "", scratch_pool));
  SVN_ERR(svn_cmdline_printf(scratch_pool, _("Summary of updates:\n")));

  iter_pool = svn_pool_create(scratch_pool);

  for (i = 0; i < targets->nelts; i++)
    {
      const char *path = APR_ARRAY_IDX(targets, i, const char *);
      const char *path_local;

      svn_pool_clear(iter_pool);

      /* Convert to an absolute path if it's not already. */
      if (! svn_dirent_is_absolute(path))
        SVN_ERR(svn_dirent_get_absolute(&path, path, iter_pool));

      /* Remove the current working directory prefix from PATH (if
         PATH is at or under $CWD), and convert to local style for
         display. */
      path_local = svn_dirent_skip_ancestor(path_prefix, path);
      path_local = svn_dirent_local_style(path_local, iter_pool);
      
      if (i < result_revs->nelts)
        {
          svn_revnum_t rev = APR_ARRAY_IDX(result_revs, i, svn_revnum_t);

          if (SVN_IS_VALID_REVNUM(rev))
            SVN_ERR(svn_cmdline_printf(iter_pool,
                                       _("  Updated '%s' to r%ld.\n"),
                                       path_local, rev));
        }
      else
        {
          /* ### Notify about targets for which there's no
             ### result_rev?  Can we even get here?  */
        }
    }

  svn_pool_destroy(iter_pool);
  return SVN_NO_ERROR;
}

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__update(apr_getopt_t *os,
               void *baton,
               apr_pool_t *scratch_pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_array_header_t *targets;
  svn_depth_t depth;
  svn_boolean_t depth_is_sticky;
  struct svn_cl__check_externals_failed_notify_baton nwb;
  apr_array_header_t *result_revs;

  SVN_ERR(svn_cl__args_to_target_array_print_reserved(&targets, os,
                                                      opt_state->targets,
                                                      ctx, scratch_pool));

  /* Add "." if user passed 0 arguments */
  svn_opt_push_implicit_dot_target(targets, scratch_pool);

  SVN_ERR(svn_cl__eat_peg_revisions(&targets, targets, scratch_pool));

  /* If using changelists, convert targets into a set of paths that
     match the specified changelist(s). */
  if (opt_state->changelists)
    {
      svn_depth_t cl_depth = opt_state->depth;
      if (cl_depth == svn_depth_unknown)
        cl_depth = svn_depth_infinity;
      SVN_ERR(svn_cl__changelist_paths(&targets,
                                       opt_state->changelists, targets,
                                       cl_depth, ctx, scratch_pool,
                                       scratch_pool));
    }

  /* Deal with depthstuffs. */
  if (opt_state->set_depth != svn_depth_unknown)
    {
      depth = opt_state->set_depth;
      depth_is_sticky = TRUE;
    }
  else
    {
      depth = opt_state->depth;
      depth_is_sticky = FALSE;
    }

  nwb.wrapped_func = ctx->notify_func2;
  nwb.wrapped_baton = ctx->notify_baton2;
  nwb.had_externals_error = FALSE;
  ctx->notify_func2 = svn_cl__check_externals_failed_notify_wrapper;
  ctx->notify_baton2 = &nwb;
  
  SVN_ERR(svn_client_update4(&result_revs, targets,
                             &(opt_state->start_revision),
                             depth, depth_is_sticky,
                             opt_state->ignore_externals,
                             opt_state->force, opt_state->parents,
                             ctx, scratch_pool));

  if (! opt_state->quiet)
    {
      SVN_ERR(print_update_summary(targets, result_revs, scratch_pool));

      /* ### Layering problem: This call assumes that the baton we're
       * passing is the one that was originally provided by
       * svn_cl__get_notifier(), but that isn't promised. */
      SVN_ERR(svn_cl__print_conflict_stats(nwb.wrapped_baton, scratch_pool));
    }

  if (nwb.had_externals_error)
    return svn_error_create(SVN_ERR_CL_ERROR_PROCESSING_EXTERNALS, NULL,
                            _("Failure occured processing one or more "
                              "externals definitions"));

  return SVN_NO_ERROR;
}
