/*
 * update.c:  wrappers around wc update functionality
 *
 * ====================================================================
 * Copyright (c) 2000-2006 CollabNet.  All rights reserved.
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

#include <assert.h>

#include "svn_wc.h"
#include "svn_client.h"
#include "svn_error.h"
#include "svn_config.h"
#include "svn_time.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_io.h"
#include "client.h"
#include "mergeinfo.h"

#include "svn_private_config.h"
#include "private/svn_mergeinfo_private.h"
#include "private/svn_wc_private.h"


/*** Code. ***/

svn_error_t *
svn_client__update_internal(svn_revnum_t *result_rev,
                            const char *path,
                            const svn_opt_revision_t *revision,
                            svn_depth_t depth,
                            svn_boolean_t ignore_externals,
                            svn_boolean_t allow_unver_obstructions,
                            svn_boolean_t *timestamp_sleep,
                            svn_client_ctx_t *ctx,
                            apr_pool_t *pool)
{
  const svn_delta_editor_t *update_editor;
  void *update_edit_baton;
  const svn_ra_reporter3_t *reporter;
  void *report_baton;
  const svn_wc_entry_t *entry;
  const char *anchor, *target;
  const char *repos_root;
  svn_error_t *err;
  svn_revnum_t revnum;
  int adm_open_depth;
  svn_wc_traversal_info_t *traversal_info = svn_wc_init_traversal_info(pool);
  svn_wc_adm_access_t *adm_access;
  svn_boolean_t use_commit_times;
  svn_boolean_t sleep_here = FALSE;
  svn_boolean_t *use_sleep = timestamp_sleep ? timestamp_sleep : &sleep_here;
  const char *diff3_cmd;
  svn_ra_session_t *ra_session;
  svn_wc_adm_access_t *dir_access;
  svn_wc_adm_access_t *path_adm_access;
  apr_hash_t *children_with_mergeinfo;
  const char *preserved_exts_str;
  apr_array_header_t *preserved_exts;
  svn_config_t *cfg = ctx->config ? apr_hash_get(ctx->config, 
                                                 SVN_CONFIG_CATEGORY_CONFIG,
                                                 APR_HASH_KEY_STRING) : NULL;

  /* ### TODO(sd): Ah, the irony.  We'd like to base our adm_open depth on
     ### the depth we're going to use for the update.  But that
     ### may depend on the depth in the working copy, which we can't
     ### discover without calling adm_open.  We could expend an extra
     ### call, with adm_open_depth=0, to get the real depth (but only
     ### if we need to) and then make the real call... but it's not
     ### worth the complexity right now.  Locking the entire tree when
     ### we didn't need to is a performance hit, but (except for
     ### access contention) not a correctness problem. */

  if (depth == svn_depth_empty
      || depth == svn_depth_files
      || depth == svn_depth_immediates)
    adm_open_depth = 0;
  else
    adm_open_depth = -1;
  
  /* Sanity check.  Without this, the update is meaningless. */
  assert(path);

  if (svn_path_is_url(path))
    return svn_error_createf(SVN_ERR_WC_NOT_DIRECTORY, NULL,
                             _("Path '%s' is not a directory"),
                             path);

  /* Use PATH to get the update's anchor and targets and get a write lock */
  SVN_ERR(svn_wc_adm_open_anchor(&adm_access, &dir_access, &target, path,
                                 TRUE, adm_open_depth,
                                 ctx->cancel_func, ctx->cancel_baton,
                                 pool));
  anchor = svn_wc_adm_access_path(adm_access);

  /* Get full URL from the ANCHOR. */
  SVN_ERR(svn_wc_entry(&entry, anchor, adm_access, FALSE, pool));
  if (! entry->url)
    return svn_error_createf(SVN_ERR_ENTRY_MISSING_URL, NULL,
                             _("Entry '%s' has no URL"),
                             svn_path_local_style(anchor, pool));

  /* Get revnum set to something meaningful, so we can fetch the
     update editor. */
  if (revision->kind == svn_opt_revision_number)
    revnum = revision->value.number;
  else
    revnum = SVN_INVALID_REVNUM;

  /* Get the external diff3, if any. */    
  svn_config_get(cfg, &diff3_cmd, SVN_CONFIG_SECTION_HELPERS,
                 SVN_CONFIG_OPTION_DIFF3_CMD, NULL);

  /* See if the user wants last-commit timestamps instead of current ones. */
  SVN_ERR(svn_config_get_bool(cfg, &use_commit_times,
                              SVN_CONFIG_SECTION_MISCELLANY,
                              SVN_CONFIG_OPTION_USE_COMMIT_TIMES, FALSE));

  /* See which files the user wants to preserve the extension of when
     conflict files are made. */
  svn_config_get(cfg, &preserved_exts_str, SVN_CONFIG_SECTION_MISCELLANY,
                 SVN_CONFIG_OPTION_PRESERVED_CF_EXTS, "");
  preserved_exts = *preserved_exts_str 
    ? svn_cstring_split(preserved_exts_str, "\n\r\t\v ", FALSE, pool)
    : NULL;

  /* Open an RA session for the URL */
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, entry->url,
                                               anchor, adm_access,
                                               NULL, TRUE, TRUE, 
                                               ctx, pool));

  /* ### todo: shouldn't svn_client__get_revision_number be able
     to take a URL as easily as a local path?  */
  SVN_ERR(svn_client__get_revision_number
          (&revnum, ra_session, revision, path, pool));

  /* Take the chance to set the repository root on the target.
     Why do we bother doing this for old working copies?
     There are two reasons: first, it's nice to get this information into
     old WCs so they are "ready" when we start depending on it.  (We can
     never *depend* upon it in a strict sense, however.)
     Second, if people mix old and new clients, this information will
     be dropped by the old clients, which might be annoying. */
  SVN_ERR(svn_ra_get_repos_root(ra_session, &repos_root, pool));
  SVN_ERR(svn_wc_maybe_set_repos_root(dir_access, path, repos_root, pool));

  /* Fetch the update editor.  If REVISION is invalid, that's okay;
     the RA driver will call editor->set_target_revision later on. */
  SVN_ERR(svn_wc_get_update_editor3(&revnum, adm_access, target,
                                    use_commit_times, depth,
                                    allow_unver_obstructions,
                                    ctx->notify_func2, ctx->notify_baton2,
                                    ctx->cancel_func, ctx->cancel_baton,
                                    diff3_cmd, preserved_exts,
                                    &update_editor, &update_edit_baton,
                                    traversal_info,
                                    pool));

  /* Tell RA to do an update of URL+TARGET to REVISION; if we pass an
     invalid revnum, that means RA will use the latest revision.  */
  SVN_ERR(svn_ra_do_update2(ra_session,
                            &reporter, &report_baton,
                            revnum,
                            target,
                            depth,
                            update_editor, update_edit_baton, pool));

  /* Drive the reporter structure, describing the revisions within
     PATH.  When we call reporter->finish_report, the
     update_editor will be driven by svn_repos_dir_delta2. */
  err = svn_wc_crawl_revisions3(path, dir_access, reporter, report_baton,
                                TRUE, depth, use_commit_times,
                                ctx->notify_func2, ctx->notify_baton2,
                                traversal_info, pool);
      
  if (err)
    {
      /* Don't rely on the error handling to handle the sleep later, do
         it now */
      svn_sleep_for_timestamps();
      return err;
    }
  *use_sleep = TRUE;
  
  /* We handle externals after the update is complete, so that
     handling external items (and any errors therefrom) doesn't delay
     the primary operation.  */
  if ((depth == svn_depth_infinity || depth == svn_depth_unknown)
      && (! ignore_externals))
    SVN_ERR(svn_client__handle_externals(traversal_info, 
                                         TRUE, /* update unchanged ones */
                                         use_sleep, ctx, pool));

  if (sleep_here)
    svn_sleep_for_timestamps();

  if (adm_open_depth)
    {
      SVN_ERR(svn_wc_adm_probe_retrieve(&path_adm_access, adm_access, path,
                                        pool));  
    }
  else
    {
      /* A depth other than infinity means we need to open a new
         access to lock PATH's children for possible elision. */
      SVN_ERR(svn_wc_adm_close(adm_access));
      SVN_ERR(svn_wc_adm_open3(&path_adm_access, NULL, path, TRUE, -1,
                               ctx->cancel_func, ctx->cancel_baton,
                               pool));
    }

    /* Check if any mergeinfo on PATH or any its children elides as a
     result of the update. */
  children_with_mergeinfo = apr_hash_make(pool);  
  err = svn_client__get_prop_from_wc(children_with_mergeinfo,
                                     SVN_PROP_MERGE_INFO, path, FALSE,
                                     entry, path_adm_access, TRUE, ctx,
                                     pool);
  if (err)
    {
      if (err->apr_err == SVN_ERR_UNVERSIONED_RESOURCE)
        {
          svn_error_clear(err);
          err = SVN_NO_ERROR;
        }
      else
        {
          return err;
        }
    }
  else
    {
      SVN_ERR(svn_client__elide_mergeinfo_for_tree(children_with_mergeinfo,
                                                   adm_access, ctx, pool));
    }

  SVN_ERR(svn_wc_adm_close(adm_open_depth ? path_adm_access : adm_access));

  /* Let everyone know we're finished here. */
  if (ctx->notify_func2)
    {
      svn_wc_notify_t *notify
        = svn_wc_create_notify(anchor, svn_wc_notify_update_completed, pool);
      notify->kind = svn_node_none;
      notify->content_state = notify->prop_state
        = svn_wc_notify_state_inapplicable;
      notify->lock_state = svn_wc_notify_lock_state_inapplicable;
      notify->revision = revnum;
      (*ctx->notify_func2)(ctx->notify_baton2, notify, pool);
    }

  /* If the caller wants the result revision, give it to them. */
  if (result_rev)
    *result_rev = revnum;
  
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_update3(apr_array_header_t **result_revs,
                   const apr_array_header_t *paths,
                   const svn_opt_revision_t *revision,
                   svn_depth_t depth,
                   svn_boolean_t ignore_externals,
                   svn_boolean_t allow_unver_obstructions,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool)
{
  int i;
  svn_error_t *err = SVN_NO_ERROR;
  apr_pool_t *subpool = svn_pool_create(pool);

  if (result_revs)
    *result_revs = apr_array_make(pool, paths->nelts, sizeof(svn_revnum_t));

  for (i = 0; i < paths->nelts; ++i)
    {
      svn_boolean_t sleep;
      svn_revnum_t result_rev;
      const char *path = APR_ARRAY_IDX(paths, i, const char *);

      svn_pool_clear(subpool);

      if (ctx->cancel_func && (err = ctx->cancel_func(ctx->cancel_baton)))
        break;

      err = svn_client__update_internal(&result_rev, path, revision,
                                        depth, ignore_externals,
                                        allow_unver_obstructions,
                                        &sleep, ctx, subpool);
      if (err && err->apr_err != SVN_ERR_WC_NOT_DIRECTORY)
        {
          return err;
        }
      else if (err)
        {
          /* SVN_ERR_WC_NOT_DIRECTORY: it's not versioned */
          svn_error_clear(err);
          err = SVN_NO_ERROR;
          result_rev = SVN_INVALID_REVNUM;
          if (ctx->notify_func2)
            (*ctx->notify_func2)(ctx->notify_baton2,
                                 svn_wc_create_notify(path,
                                                      svn_wc_notify_skip,
                                                      subpool), subpool);
        }
      if (result_revs)
        APR_ARRAY_PUSH(*result_revs, svn_revnum_t) = result_rev;
    }

  svn_pool_destroy(subpool);
  svn_sleep_for_timestamps();

  return err;
}

svn_error_t *
svn_client_update2(apr_array_header_t **result_revs,
                   const apr_array_header_t *paths,
                   const svn_opt_revision_t *revision,
                   svn_boolean_t recurse,
                   svn_boolean_t ignore_externals,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool)
{
  return svn_client_update3(result_revs, paths, revision,
                            SVN_DEPTH_FROM_RECURSE(recurse),
                            ignore_externals, FALSE, ctx, pool);
}

svn_error_t *
svn_client_update(svn_revnum_t *result_rev,
                  const char *path,
                  const svn_opt_revision_t *revision,
                  svn_boolean_t recurse,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool)
{
  return svn_client__update_internal(result_rev, path, revision,
                                     SVN_DEPTH_FROM_RECURSE(recurse), 
                                     FALSE, FALSE, NULL, ctx, pool);
}
