/*
 * update.c:  wrappers around wc update functionality
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

#include <assert.h>

#include "svn_wc.h"
#include "svn_client.h"
#include "svn_error.h"
#include "svn_config.h"
#include "svn_time.h"
#include "svn_path.h"
#include "client.h"

#include "svn_private_config.h"


/*** Code. ***/

svn_error_t *
svn_client__update_internal (svn_revnum_t *result_rev,
                             const char *path,
                             const svn_opt_revision_t *revision,
                             svn_boolean_t recurse,
                             svn_boolean_t *timestamp_sleep,
                             svn_client_ctx_t *ctx,
                             apr_pool_t *pool)
{
  const svn_delta_editor_t *update_editor;
  void *update_edit_baton;
  const svn_ra_reporter_t *reporter;
  void *report_baton;
  const svn_wc_entry_t *entry;
  const char *URL, *anchor, *target;
  svn_error_t *err;
  svn_revnum_t revnum;
  svn_wc_traversal_info_t *traversal_info = svn_wc_init_traversal_info (pool);
  svn_wc_adm_access_t *adm_access;
  svn_boolean_t use_commit_times;
  svn_boolean_t sleep_here = FALSE;
  svn_boolean_t *use_sleep = timestamp_sleep ? timestamp_sleep : &sleep_here;
  const char *diff3_cmd;
  void *ra_baton, *session;
  svn_ra_plugin_t *ra_lib;
  svn_wc_adm_access_t *dir_access;
  svn_config_t *cfg = ctx->config ? apr_hash_get (ctx->config, 
                                                  SVN_CONFIG_CATEGORY_CONFIG,
                                                  APR_HASH_KEY_STRING) : NULL;
  
  /* Sanity check.  Without this, the update is meaningless. */
  assert (path);

  /* Use PATH to get the update's anchor and targets and get a write lock */
  SVN_ERR (svn_wc_adm_open_anchor (&adm_access, &dir_access, &target, path,
                                   TRUE, recurse ? -1 : 0, pool));
  anchor = svn_wc_adm_access_path (adm_access);

  /* Get full URL from the ANCHOR. */
  SVN_ERR (svn_wc_entry (&entry, anchor, adm_access, FALSE, pool));
  if (! entry)
    return svn_error_createf (SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL, 
                              _("'%s' is not under version control"),
                              svn_path_local_style (anchor, pool));
  if (! entry->url)
    return svn_error_createf (SVN_ERR_ENTRY_MISSING_URL, NULL,
                              _("Entry '%s' has no URL"),
                              svn_path_local_style (anchor, pool));
  URL = apr_pstrdup (pool, entry->url);

  /* Get revnum set to something meaningful, so we can fetch the
     update editor. */
  if (revision->kind == svn_opt_revision_number)
    revnum = revision->value.number;
  else
    revnum = SVN_INVALID_REVNUM;

  /* Get the external diff3, if any. */    
  svn_config_get (cfg, &diff3_cmd, SVN_CONFIG_SECTION_HELPERS,
                  SVN_CONFIG_OPTION_DIFF3_CMD, NULL);

  /* See if the user wants last-commit timestamps instead of current ones. */
  SVN_ERR (svn_config_get_bool (cfg, &use_commit_times,
                                SVN_CONFIG_SECTION_MISCELLANY,
                                SVN_CONFIG_OPTION_USE_COMMIT_TIMES, FALSE));

  /* Get the RA vtable that matches URL. */
  SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, URL, pool));

  /* Open an RA session for the URL */
  SVN_ERR (svn_client__open_ra_session (&session, ra_lib, URL, anchor, 
                                        adm_access, NULL, TRUE, TRUE, 
                                        ctx, pool));

  /* ### todo: shouldn't svn_client__get_revision_number be able
     to take a URL as easily as a local path?  */
  SVN_ERR (svn_client__get_revision_number
           (&revnum, ra_lib, session, revision, path, pool));

  /* Fetch the update editor.  If REVISION is invalid, that's okay;
     the RA driver will call editor->set_target_revision later on. */
  SVN_ERR (svn_wc_get_update_editor (&revnum, adm_access, target,
                                     use_commit_times, recurse,
                                     ctx->notify_func, ctx->notify_baton,
                                     ctx->cancel_func, ctx->cancel_baton,
                                     diff3_cmd,
                                     &update_editor, &update_edit_baton,
                                     traversal_info,
                                     pool));

  /* Tell RA to do an update of URL+TARGET to REVISION; if we pass an
     invalid revnum, that means RA will use the latest revision.  */
  SVN_ERR (ra_lib->do_update (session,
                              &reporter, &report_baton,
                              revnum,
                              target,
                              recurse,
                              update_editor, update_edit_baton, pool));

  /* Drive the reporter structure, describing the revisions within
     PATH.  When we call reporter->finish_report, the
     update_editor will be driven by svn_repos_dir_delta. */
  err = svn_wc_crawl_revisions (path, dir_access, reporter, report_baton,
                                TRUE, recurse, use_commit_times,
                                ctx->notify_func, ctx->notify_baton,
                                traversal_info, pool);
      
  if (err)
    {
      /* Don't rely on the error handling to handle the sleep later, do
         it now */
      svn_sleep_for_timestamps ();
      return err;
    }
  *use_sleep = TRUE;
  
  /* We handle externals after the update is complete, so that
     handling external items (and any errors therefrom) doesn't delay
     the primary operation.  */
  if (recurse)
    SVN_ERR (svn_client__handle_externals (traversal_info, 
                                           TRUE, /* update unchanged ones */
                                           use_sleep, ctx, pool));

  if (sleep_here)
    svn_sleep_for_timestamps ();

  SVN_ERR (svn_wc_adm_close (adm_access));

  /* Let everyone know we're finished here. */
  if (ctx->notify_func)
    (*ctx->notify_func) (ctx->notify_baton,
                         anchor,
                         svn_wc_notify_update_completed,
                         svn_node_none,
                         NULL,
                         svn_wc_notify_state_inapplicable,
                         svn_wc_notify_state_inapplicable,
                         revnum);

  /* If the caller wants the result revision, give it to them. */
  if (result_rev)
    *result_rev = revnum;
  
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_update (svn_revnum_t *result_rev,
                   const char *path,
                   const svn_opt_revision_t *revision,
                   svn_boolean_t recurse,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool)
{
  return svn_client__update_internal (result_rev, path, revision, recurse, 
                                      NULL, ctx, pool);
}
