/*
 * switch.c:  implement 'switch' feature via WC & RA interfaces.
 *
 * ====================================================================
 * Copyright (c) 2000-2007 CollabNet.  All rights reserved.
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

#include "svn_client.h"
#include "svn_error.h"
#include "svn_time.h"
#include "svn_path.h"
#include "svn_config.h"
#include "svn_pools.h"
#include "client.h"
#include "mergeinfo.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"
#include "private/svn_mergeinfo_private.h"


/*** Code. ***/

/* This feature is essentially identical to 'svn update' (see
   ./update.c), but with two differences:

     - the reporter->finish_report() routine needs to make the server
       run delta_dirs() on two *different* paths, rather than on two
       identical paths.

     - after the update runs, we need to more than just
       ensure_uniform_revision;  we need to rewrite all the entries'
       URL attributes.
*/


svn_error_t *
svn_client__switch_internal(svn_revnum_t *result_rev,
                            const char *path,
                            const char *switch_url,
                            const svn_opt_revision_t *peg_revision,
                            const svn_opt_revision_t *revision,
                            svn_depth_t depth,
                            svn_boolean_t *timestamp_sleep,
                            svn_boolean_t ignore_externals,
                            svn_boolean_t allow_unver_obstructions,
                            svn_client_ctx_t *ctx,
                            apr_pool_t *pool)
{
  const svn_ra_reporter3_t *reporter;
  void *report_baton;
  const svn_wc_entry_t *entry;
  const char *URL, *anchor, *target, *source_root;
  const char *tmp_url;
  svn_ra_session_t *ra_session;
  svn_revnum_t revnum;
  svn_node_kind_t switch_url_kind;
  svn_error_t *err = SVN_NO_ERROR;
  svn_wc_adm_access_t *adm_access, *dir_access;
  const char *diff3_cmd;
  svn_boolean_t use_commit_times;
  svn_boolean_t sleep_here;
  svn_boolean_t *use_sleep = timestamp_sleep ? timestamp_sleep : &sleep_here;
  const svn_delta_editor_t *switch_editor;
  void *switch_edit_baton;
  svn_wc_traversal_info_t *traversal_info = svn_wc_init_traversal_info(pool);
  const char *preserved_exts_str;
  apr_array_header_t *preserved_exts;
  svn_boolean_t server_supports_depth;
  svn_config_t *cfg = ctx->config ? apr_hash_get(ctx->config,
                                                 SVN_CONFIG_CATEGORY_CONFIG,
                                                 APR_HASH_KEY_STRING)
                                  : NULL;

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

  /* Sanity check.  Without these, the switch is meaningless. */
  assert(path);
  assert(switch_url && (switch_url[0] != '\0'));

  /* ### Need to lock the whole target tree to invalidate wcprops. Does
     non-recursive switch really need to invalidate the whole tree? */
  SVN_ERR(svn_wc_adm_open_anchor(&adm_access, &dir_access, &target, path,
                                 TRUE, -1, ctx->cancel_func,
                                 ctx->cancel_baton, pool));
  anchor = svn_wc_adm_access_path(adm_access);

  SVN_ERR(svn_wc__entry_versioned(&entry, anchor, adm_access, FALSE, pool));
  if (! entry->url)
    return svn_error_createf(SVN_ERR_ENTRY_MISSING_URL, NULL,
                             _("Directory '%s' has no URL"),
                             svn_path_local_style(anchor, pool));

  URL = apr_pstrdup(pool, entry->url);

  /* Get revnum set to something meaningful, so we can fetch the
     switch editor. */
  if (revision->kind == svn_opt_revision_number)
    revnum = revision->value.number; /* do the trivial conversion manually */
  else
    revnum = SVN_INVALID_REVNUM; /* no matter, do real conversion later */

  /* Open an RA session to 'source' URL */
  SVN_ERR(svn_client__ra_session_from_path(&ra_session, &revnum, &tmp_url,
                                           URL, peg_revision, revision,
                                           ctx, pool));
  SVN_ERR(svn_ra_get_repos_root(ra_session, &source_root, pool));

  /* Disallow a switch operation to change the repository root of the
     target. */
  if (! svn_path_is_ancestor(source_root, switch_url))
    return svn_error_createf
      (SVN_ERR_WC_INVALID_SWITCH, NULL,
       _("'%s'\n"
         "is not the same repository as\n"
         "'%s'"), switch_url, source_root);

  /* Check to make sure that the switch target actually exists. */
  SVN_ERR(svn_ra_reparent(ra_session, source_root, pool));
  SVN_ERR(svn_ra_check_path(ra_session,
                            svn_path_is_child(source_root, switch_url, pool),
                            revnum,
                            &switch_url_kind,
                            pool));

  if (switch_url_kind == svn_node_none)
    return svn_error_createf
      (SVN_ERR_WC_INVALID_SWITCH, NULL,
       _("Destination does not exist: '%s'"), switch_url);

  SVN_ERR(svn_ra_reparent(ra_session, URL, pool));


  /* Fetch the switch (update) editor.  If REVISION is invalid, that's
     okay; the RA driver will call editor->set_target_revision() later on. */
  SVN_ERR(svn_wc_get_switch_editor3(&revnum, adm_access, target,
                                    switch_url, use_commit_times, depth,
                                    allow_unver_obstructions,
                                    ctx->notify_func2, ctx->notify_baton2,
                                    ctx->cancel_func, ctx->cancel_baton,
                                    ctx->conflict_func, ctx->conflict_baton,
                                    diff3_cmd, preserved_exts,
                                    &switch_editor, &switch_edit_baton,
                                    traversal_info, pool));

  /* Tell RA to do an update of URL+TARGET to REVISION; if we pass an
     invalid revnum, that means RA will use the latest revision. */
  SVN_ERR(svn_ra_do_switch2(ra_session, &reporter, &report_baton, revnum,
                            target, depth, switch_url,
                            switch_editor, switch_edit_baton, pool));

  SVN_ERR(svn_ra_has_capability(ra_session, &server_supports_depth,
                                SVN_RA_CAPABILITY_DEPTH, pool));

  /* Drive the reporter structure, describing the revisions within
     PATH.  When we call reporter->finish_report, the update_editor
     will be driven by svn_repos_dir_delta2.

     We pass NULL for traversal_info because this is a switch, not an
     update, and therefore we don't want to handle any externals
     except the ones directly affected by the switch. */
  err = svn_wc_crawl_revisions3(path, dir_access, reporter, report_baton,
                                TRUE, depth, (! server_supports_depth),
                                use_commit_times,
                                ctx->notify_func2, ctx->notify_baton2,
                                NULL, /* no traversal info */
                                pool);

  if (err)
    {
      /* Don't rely on the error handling to handle the sleep later, do
         it now */
      svn_sleep_for_timestamps();
      return err;
    }
  *use_sleep = TRUE;

  /* We handle externals after the switch is complete, so that
     handling external items (and any errors therefrom) doesn't delay
     the primary operation. */
  if (SVN_DEPTH_IS_RECURSIVE(depth) && (! ignore_externals))
    err = svn_client__handle_externals(traversal_info, path, switch_url,
                                       source_root, depth, FALSE, use_sleep,
                                       ctx, pool);

  if (!err)
    {
      /* Check if any mergeinfo on PATH or any its children elides as a
         result of the switch. */
      apr_hash_t *children_with_mergeinfo = apr_hash_make(pool);
      svn_wc_adm_access_t *path_adm_access;
      SVN_ERR(svn_wc_adm_probe_retrieve(&path_adm_access, adm_access, path,
                                        pool));
      err = svn_client__get_prop_from_wc(children_with_mergeinfo,
                                         SVN_PROP_MERGE_INFO, path, FALSE,
                                         entry, path_adm_access,
                                         depth, ctx, pool);
      if (err)
        {
          if (err->apr_err == SVN_ERR_UNVERSIONED_RESOURCE)
            {
              svn_error_clear(err);
              err = SVN_NO_ERROR;
            }
          /* Any other error is not returned until after we sleep. */
        }
      else
        {
          SVN_ERR(svn_client__elide_mergeinfo_for_tree(children_with_mergeinfo,
                                                       adm_access, ctx, pool));
        }
    }

  /* Sleep to ensure timestamp integrity (we do this regardless of
     errors in the actual switch operation(s)). */
  if (sleep_here)
    svn_sleep_for_timestamps();

  /* Return errors we might have sustained. */
  if (err)
    return err;

  SVN_ERR(svn_wc_adm_close(adm_access));

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
svn_client_switch2(svn_revnum_t *result_rev,
                   const char *path,
                   const char *switch_url,
                   const svn_opt_revision_t *peg_revision,
                   const svn_opt_revision_t *revision,
                   svn_depth_t depth,
                   svn_boolean_t ignore_externals,
                   svn_boolean_t allow_unver_obstructions,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool)
{
  return svn_client__switch_internal(result_rev, path, switch_url,
                                     peg_revision, revision,
                                     depth, NULL, ignore_externals,
                                     allow_unver_obstructions, ctx, pool);
}

svn_error_t *
svn_client_switch(svn_revnum_t *result_rev,
                  const char *path,
                  const char *switch_url,
                  const svn_opt_revision_t *revision,
                  svn_boolean_t recurse,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool)
{
  svn_opt_revision_t peg_revision;
  peg_revision.kind = svn_opt_revision_unspecified;
  return svn_client__switch_internal(result_rev, path, switch_url, 
                                     &peg_revision, revision,
                                     SVN_DEPTH_INFINITY_OR_FILES(recurse),
                                     NULL, FALSE, FALSE, ctx, pool);
}
