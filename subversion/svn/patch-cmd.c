/*
 * patch-cmd.c -- Apply changes to a working copy.
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

#include "svn_client.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_error.h"
#include "svn_types.h"
#include "cl.h"

#include "svn_private_config.h"


/*** Code. ***/


/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__patch(apr_getopt_t *os,
              void *baton,
              apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state;
  svn_client_ctx_t *ctx;
  apr_array_header_t *args;
  apr_array_header_t *targets;
  const char *abs_patch_path;
  const char *abs_target_path;

  opt_state = ((svn_cl__cmd_baton_t *)baton)->opt_state;
  ctx = ((svn_cl__cmd_baton_t *)baton)->ctx;

  SVN_ERR(svn_opt_parse_num_args(&args, os, 1, pool));
  SVN_ERR(svn_dirent_get_absolute(&abs_patch_path,
                                  APR_ARRAY_IDX(args, 0, const char *),
                                  pool));

  SVN_ERR(svn_client_args_to_target_array(&targets, os, opt_state->targets,
                                          ctx, pool));
  if (targets->nelts > 1)
    return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, 0, NULL);

  svn_opt_push_implicit_dot_target(targets, pool);
  SVN_ERR(svn_opt_eat_peg_revisions(&targets, targets, pool));
  SVN_ERR(svn_dirent_get_absolute(&abs_target_path,
                                  APR_ARRAY_IDX(targets, 0, const char *),
                                  pool));

  if (! opt_state->quiet)
    SVN_ERR(svn_cl__get_notifier(&ctx->notify_func2, &ctx->notify_baton2,
                                 FALSE, FALSE, FALSE, pool));

  SVN_ERR(svn_client_patch(abs_patch_path, abs_target_path,
                           opt_state->dry_run, opt_state->strip_count,
                           opt_state->reverse_diff,
                           opt_state->include_patterns,
                           opt_state->exclude_patterns,
                           NULL, NULL, 
                           opt_state->ignore_whitespaces, ctx, pool, pool));


  if (! opt_state->quiet)
    SVN_ERR(svn_cl__print_conflict_stats(ctx->notify_baton2, pool));

  return SVN_NO_ERROR;
}
