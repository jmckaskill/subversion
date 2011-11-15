/*
 * merge-cmd.c -- Merging changes into a working copy.
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

/* A message to print after reintegration */
const char *keep_alive_message = N_(
  "To continue using the source branch after this reintegration, note the\n"
  "new revision number REV created by the commit and perform the following\n"
  "command in a working copy of that branch:\n"
  "\n"
  "  svn merge --record-only --change REV ^/%s .\n"
  "\n"
  "See \"Keeping a Reintegrated Branch Alive\" in the Svn Book here:\n"
  "<http://svnbook.red-bean.com/en/1.6/svn-book.html#svn.branchmerge.advanced.reintegratetwice>\n"
);

/* Set *REPOS_RELPATH to the repository path of WC_PATH relative to the
 * repository root. */
static svn_error_t *
get_repos_relpath(const char **repos_relpath,
                  const char *wc_path,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool)
{
  const char *wc_abspath, *url, *repos_url;

  SVN_ERR(svn_dirent_get_absolute(&wc_abspath, wc_path, pool));
  SVN_ERR(svn_client_url_from_path2(&url, wc_abspath,
                                    ctx, pool, pool));
  SVN_ERR(svn_client_get_repos_root(&repos_url, NULL, wc_abspath,
                                    ctx, pool, pool));
  *repos_relpath = svn_uri_skip_ancestor(repos_url, url, pool);
  return SVN_NO_ERROR;
}

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__merge(apr_getopt_t *os,
              void *baton,
              apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_array_header_t *targets;
  const char *sourcepath1 = NULL, *sourcepath2 = NULL, *targetpath = "";
  svn_boolean_t two_sources_specified = TRUE;
  svn_error_t *err;
  svn_opt_revision_t first_range_start, first_range_end, peg_revision1,
    peg_revision2;
  apr_array_header_t *options, *ranges_to_merge = opt_state->revision_ranges;

  /* Merge doesn't support specifying a revision or revision range
     when using --reintegrate. */
  if (opt_state->reintegrate
      && opt_state->start_revision.kind != svn_opt_revision_unspecified)
    {
      return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                              _("-r and -c can't be used with --reintegrate"));
    }

  SVN_ERR(svn_cl__args_to_target_array_print_reserved(&targets, os,
                                                      opt_state->targets,
                                                      ctx, FALSE, pool));

  /* For now, we require at least one source.  That may change in
     future versions of Subversion, for example if we have support for
     negated mergeinfo.  See this IRC conversation:

       <bhuvan>   kfogel: yeah, i think you are correct; we should
                  specify the source url

       <kfogel>   bhuvan: I'll change the help output and propose for
                  backport.  Thanks.

       <bhuvan>   kfogel: np; while we are at it, 'svn merge' simply
                  returns nothing; i think we should say: """svn: Not
                  enough arguments provided; try 'svn help' for more
                  info"""

       <kfogel>   good idea

       <kfogel>   (in the future, 'svn merge' might actually do
                  something, but that's all the more reason to make
                  sure it errors now)

       <cmpilato> actually, i'm pretty sure 'svn merge' does something

       <cmpilato> it says "please merge any unmerged changes from
                  myself to myself."

       <cmpilato> :-)

       <kfogel>   har har

       <cmpilato> kfogel: i was serious.

       <kfogel>   cmpilato: urrr, uh.  Is that meaningful?  Is there
                  ever a reason for a user to run it?

       <cmpilato> kfogel: not while we don't have support for negated
                  mergeinfo.

       <kfogel>   cmpilato: do you concur that until it does something
                  useful it should error?

       <cmpilato> kfogel: yup.

       <kfogel>   cool
  */
  if (targets->nelts < 1)
    {
      /* Infer the merge source automatically, assuming a sync merge. */
      svn_client_peg_t *target_peg, *source_peg;

      SVN_ERR(svn_client_peg_create(&target_peg, "", NULL, pool));
      SVN_ERR(svn_cl__find_merge_source_branch(&source_peg, target_peg, ctx, pool));
      /*SVN_ERR(svn_client__resolve_target_location_from_peg(&source, source_peg, NULL, ctx, pool));*/
      printf("Assuming source branch is copy-source of target branch: '%s'\n",
             svn_cl__peg_for_display(source_peg, pool));
      peg_revision1 = source_peg->peg_revision;
      sourcepath1 = source_peg->path_or_url;
    }
  else  /* Parse at least one, and possible two, sources. */
    {
      SVN_ERR(svn_opt_parse_path(&peg_revision1, &sourcepath1,
                                 APR_ARRAY_IDX(targets, 0, const char *),
                                 pool));
      if (targets->nelts >= 2)
        SVN_ERR(svn_opt_parse_path(&peg_revision2, &sourcepath2,
                                   APR_ARRAY_IDX(targets, 1, const char *),
                                   pool));
    }

  /* We could have one or two sources.  Deliberately written to stay
     correct even if we someday permit implied merge source. */
  if (targets->nelts <= 1)
    {
      two_sources_specified = FALSE;
    }
  else if (targets->nelts == 2)
    {
      if (svn_path_is_url(sourcepath1) && !svn_path_is_url(sourcepath2))
        two_sources_specified = FALSE;
    }

  if (opt_state->revision_ranges->nelts > 0)
    {
      first_range_start = APR_ARRAY_IDX(opt_state->revision_ranges, 0,
                                        svn_opt_revision_range_t *)->start;
      first_range_end = APR_ARRAY_IDX(opt_state->revision_ranges, 0,
                                      svn_opt_revision_range_t *)->end;
    }
  else
    {
      first_range_start.kind = first_range_end.kind =
        svn_opt_revision_unspecified;
    }

  /* If revision_ranges has at least one real range at this point, then
     we know the user must have used the '-r' and/or '-c' switch(es).
     This means we're *not* doing two distinct sources. */
  if (first_range_start.kind != svn_opt_revision_unspecified)
    {
      /* A revision *range* is required. */
      if (first_range_end.kind == svn_opt_revision_unspecified)
        return svn_error_create(SVN_ERR_CL_INSUFFICIENT_ARGS, 0,
                                _("Second revision required"));

      two_sources_specified = FALSE;
    }

  if (! two_sources_specified) /* TODO: Switch order of if */
    {
      if (targets->nelts > 2)
        return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                                _("Too many arguments given"));

      /* Set the default value for unspecified paths and peg revision. */
      /* targets->nelts is 1 ("svn merge SOURCE") or 2 ("svn merge
         SOURCE WCPATH") here. */
      sourcepath2 = sourcepath1;

      if (peg_revision1.kind == svn_opt_revision_unspecified)
        peg_revision1.kind = svn_path_is_url(sourcepath1)
          ? svn_opt_revision_head : svn_opt_revision_working;

      if (targets->nelts == 2)
        {
          targetpath = APR_ARRAY_IDX(targets, 1, const char *);
          if (svn_path_is_url(targetpath))
            return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                                    _("Cannot specify a revision range "
                                      "with two URLs"));
        }
    }
  else /* using @rev syntax */
    {
      if (targets->nelts < 2)
        return svn_error_create(SVN_ERR_CL_INSUFFICIENT_ARGS, NULL, NULL);
      if (targets->nelts > 3)
        return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                                _("Too many arguments given"));

      first_range_start = peg_revision1;
      first_range_end = peg_revision2;

      /* Catch 'svn merge wc_path1 wc_path2 [target]' without explicit
         revisions--since it ignores local modifications it may not do what
         the user expects.  Forcing the user to specify a repository
         revision should avoid any confusion. */
      if ((first_range_start.kind == svn_opt_revision_unspecified
           && ! svn_path_is_url(sourcepath1))
          ||
          (first_range_end.kind == svn_opt_revision_unspecified
           && ! svn_path_is_url(sourcepath2)))
        return svn_error_create
          (SVN_ERR_CLIENT_BAD_REVISION, 0,
           _("A working copy merge source needs an explicit revision"));

      /* Default peg revisions to each URL's youngest revision. */
      if (first_range_start.kind == svn_opt_revision_unspecified)
        first_range_start.kind = svn_opt_revision_head;
      if (first_range_end.kind == svn_opt_revision_unspecified)
        first_range_end.kind = svn_opt_revision_head;

      /* Decide where to apply the delta (defaulting to "."). */
      if (targets->nelts == 3)
        targetpath = APR_ARRAY_IDX(targets, 2, const char *);
    }

  /* If no targetpath was specified, see if we can infer it from the
     sourcepaths. */
  if (sourcepath1 && sourcepath2 && strcmp(targetpath, "") == 0)
    {
      /* If the sourcepath is a URL, it can only refer to a target in
         the current working directory.  However, if the sourcepath is
         a local path, it can refer to a target somewhere deeper in
         the directory structure. */
      if (svn_path_is_url(sourcepath1))
        {
          const char *sp1_basename = svn_uri_basename(sourcepath1, pool);
          const char *sp2_basename = svn_uri_basename(sourcepath2, pool);

          if (strcmp(sp1_basename, sp2_basename) == 0)
            {
              svn_node_kind_t kind;

              SVN_ERR(svn_io_check_path(sp1_basename, &kind, pool));
              if (kind == svn_node_file)
                {
                  targetpath = sp1_basename;
                }
            }
        }
      else if (strcmp(sourcepath1, sourcepath2) == 0)
        {
          svn_node_kind_t kind;
          const char *decoded_path = svn_path_uri_decode(sourcepath1, pool);
          SVN_ERR(svn_io_check_path(decoded_path, &kind, pool));
          if (kind == svn_node_file)
            {
              targetpath = decoded_path;
            }
        }
    }

  if (opt_state->extensions)
    options = svn_cstring_split(opt_state->extensions, " \t\n\r", TRUE, pool);
  else
    options = NULL;

  /* More input validation. */
  if (opt_state->reintegrate)
    {
      if (opt_state->depth != svn_depth_unknown)
        return svn_error_create(SVN_ERR_CL_MUTUALLY_EXCLUSIVE_ARGS, NULL,
                                _("--depth cannot be used with "
                                  "--reintegrate"));

      if (opt_state->force)
        return svn_error_create(SVN_ERR_CL_MUTUALLY_EXCLUSIVE_ARGS, NULL,
                                _("--force cannot be used with "
                                  "--reintegrate"));

      if (two_sources_specified)
        return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                                _("--reintegrate can only be used with "
                                  "a single merge source"));
      if (opt_state->allow_mixed_rev)
        return svn_error_create(SVN_ERR_CL_MUTUALLY_EXCLUSIVE_ARGS, NULL,
                                _("--allow-mixed-revisions cannot be used "
                                  "with --reintegrate"));
    }

  if (opt_state->dry_run)
    printf(_("This is a dry-run merge: the working copy will not be changed.\n"));

  if (opt_state->reintegrate)
    {
      printf(_("Reintegrate merge\n"));
      printf(_("  from '%s' into '%s'\n"),
             sourcepath1, targetpath);
      err = svn_client_merge_reintegrate(sourcepath1,
                                         &peg_revision1,
                                         targetpath,
                                         opt_state->dry_run,
                                         options, ctx, pool);
      
      /* Tell the user how to keep the source branch alive. */
      {
        const char *tgt_repos_relpath;

        SVN_ERR(get_repos_relpath(&tgt_repos_relpath, targetpath, ctx, pool));
        printf(_(keep_alive_message), tgt_repos_relpath);
      }
    }
  else if (! two_sources_specified)
    {
      /* If we don't have at least one valid revision range, pick a
         good one that spans the entire set of revisions on our
         source. */
      if ((first_range_start.kind == svn_opt_revision_unspecified)
          && (first_range_end.kind == svn_opt_revision_unspecified))
        {
          svn_opt_revision_range_t *range = apr_pcalloc(pool, sizeof(*range));

          ranges_to_merge = apr_array_make(pool, 1, sizeof(range));
          range->start.kind = svn_opt_revision_number;
          range->start.value.number = 1;
          range->end = peg_revision1;
          APR_ARRAY_PUSH(ranges_to_merge, svn_opt_revision_range_t *) = range;

          printf(_("Sync merge\n"));
        }
      else
        {
          printf(_("Cherry-pick merge\n"));
        }
      printf(_("  from '%s' to '%s'\n"),
             sourcepath1, targetpath);
      err = svn_client_merge_peg4(sourcepath1,
                                  ranges_to_merge,
                                  &peg_revision1,
                                  targetpath,
                                  opt_state->depth,
                                  opt_state->ignore_ancestry,
                                  opt_state->force,
                                  opt_state->record_only,
                                  opt_state->dry_run,
                                  opt_state->allow_mixed_rev,
                                  options,
                                  ctx,
                                  pool);
    }
  else
    {
      if (svn_path_is_url(sourcepath1) != svn_path_is_url(sourcepath2))
        return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                                _("Merge sources must both be "
                                  "either paths or URLs"));
      printf(_("Two-URL merge\n"));
      printf(_("  from diff between '%s' and '%s' into '%s'\n"),
             sourcepath1, sourcepath2, targetpath);
      err = svn_client_merge4(sourcepath1,
                              &first_range_start,
                              sourcepath2,
                              &first_range_end,
                              targetpath,
                              opt_state->depth,
                              opt_state->ignore_ancestry,
                              opt_state->force,
                              opt_state->record_only,
                              opt_state->dry_run,
                              opt_state->allow_mixed_rev,
                              options,
                              ctx,
                              pool);
    }

  if (! opt_state->quiet)
    SVN_ERR(svn_cl__print_conflict_stats(ctx->notify_baton2, pool));

  if (err)
    {
      if(err->apr_err == SVN_ERR_CLIENT_INVALID_MERGEINFO_NO_MERGETRACKING)
        {
          err = svn_error_quick_wrap(
            err,
            _("Merge tracking not possible, use --ignore-ancestry or\n"
              "fix invalid mergeinfo in target with 'svn propset'"));
        }
      else if (! opt_state->reintegrate)
        {
          return svn_cl__may_need_force(err);
        }
    }

  return svn_error_trace(err);
}
