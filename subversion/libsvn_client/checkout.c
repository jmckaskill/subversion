/*
 * checkout.c:  wrappers around wc checkout functionality
 *
 * ====================================================================
 *    Licensed to the Subversion Corporation (SVN Corp.) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The SVN Corp. licenses this file
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
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_ra.h"
#include "svn_types.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_io.h"
#include "svn_opt.h"
#include "svn_time.h"
#include "client.h"

#include "private/svn_wc_private.h"

#include "svn_private_config.h"


/*** Public Interfaces. ***/


svn_error_t *
svn_client__checkout_internal(svn_revnum_t *result_rev,
                              const char *url,
                              const char *path,
                              const svn_opt_revision_t *peg_revision,
                              const svn_opt_revision_t *revision,
                              const svn_client__ra_session_from_path_results *ra_cache,
                              svn_depth_t depth,
                              svn_boolean_t ignore_externals,
                              svn_boolean_t allow_unver_obstructions,
                              svn_boolean_t innercheckout,
                              svn_boolean_t *timestamp_sleep,
                              svn_client_ctx_t *ctx,
                              apr_pool_t *pool)
{
  svn_error_t *err = NULL;
  svn_revnum_t revnum;
  svn_boolean_t sleep_here = FALSE;
  svn_boolean_t *use_sleep = timestamp_sleep ? timestamp_sleep : &sleep_here;
  const char *session_url;
  svn_node_kind_t kind;
  const char *uuid, *repos_root;
  const char *local_abspath;

  /* Sanity check.  Without these, the checkout is meaningless. */
  SVN_ERR_ASSERT(path != NULL);
  SVN_ERR_ASSERT(url != NULL);
  
  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));

  /* Fulfill the docstring promise of svn_client_checkout: */
  if ((revision->kind != svn_opt_revision_number)
      && (revision->kind != svn_opt_revision_date)
      && (revision->kind != svn_opt_revision_head))
    return svn_error_create(SVN_ERR_CLIENT_BAD_REVISION, NULL, NULL);

  /* Canonicalize the URL. */
  url = svn_path_canonicalize(url, pool);

  {
    svn_boolean_t have_repos_root_url;
    svn_boolean_t have_repos_uuid;
    svn_boolean_t have_session_url;
    svn_boolean_t have_revnum;
    svn_boolean_t have_kind;

    if ((have_repos_root_url = (ra_cache && ra_cache->repos_root_url)))
      repos_root = ra_cache->repos_root_url;

    if ((have_repos_uuid = (ra_cache && ra_cache->repos_uuid)))
      uuid = ra_cache->repos_uuid;

    if ((have_session_url = (ra_cache && ra_cache->ra_session_url)))
      session_url = ra_cache->ra_session_url;

    if ((have_revnum = (ra_cache && SVN_IS_VALID_REVNUM(ra_cache->ra_revnum))))
      revnum = ra_cache->ra_revnum;

    if ((have_kind = (ra_cache && ra_cache->kind_p)))
      kind = *(ra_cache->kind_p);

    if (! have_repos_root_url || ! have_repos_uuid || ! have_session_url ||
        ! have_revnum || ! have_kind)
      {
        apr_pool_t *session_pool = svn_pool_create(pool);
        svn_ra_session_t *ra_session;
        svn_revnum_t tmp_revnum;
        const char *tmp_session_url;

        /* Get the RA connection. */
        SVN_ERR(svn_client__ra_session_from_path(&ra_session, &tmp_revnum,
                                                 &tmp_session_url, url, NULL,
                                                 peg_revision, revision, ctx,
                                                 session_pool));

        if (! have_repos_root_url)
          SVN_ERR(svn_ra_get_repos_root2(ra_session, &repos_root, pool));

        if (! have_repos_uuid)
          SVN_ERR(svn_ra_get_uuid2(ra_session, &uuid, pool));

        if (! have_session_url)
          session_url = apr_pstrdup(pool, tmp_session_url);

        if (! have_revnum)
          revnum = tmp_revnum;

        if (! have_kind)
          SVN_ERR(svn_ra_check_path(ra_session, "", revnum, &kind, pool));

        svn_pool_destroy(session_pool);
      }
  }

  if (kind == svn_node_none)
    return svn_error_createf(SVN_ERR_RA_ILLEGAL_URL, NULL,
                             _("URL '%s' doesn't exist"), session_url);
  else if (kind == svn_node_file)
    return svn_error_createf
      (SVN_ERR_UNSUPPORTED_FEATURE , NULL,
       _("URL '%s' refers to a file, not a directory"), session_url);

  SVN_ERR(svn_io_check_path(path, &kind, pool));

  if (kind == svn_node_none)
    {
      /* Bootstrap: create an incomplete working-copy root dir.  Its
         entries file should only have an entry for THIS_DIR with a
         URL, revnum, and an 'incomplete' flag.  */
      SVN_ERR(svn_io_make_dir_recursively(path, pool));
      goto initialize_area;
    }
  else if (kind == svn_node_dir)
    {
      int wc_format;
      const svn_wc_entry_t *entry;

      SVN_ERR(svn_wc_check_wc(path, &wc_format, pool));
      if (! wc_format)
        {
        initialize_area:

          if (depth == svn_depth_unknown)
            depth = svn_depth_infinity;

          /* Make the unversioned directory into a versioned one.  */
          SVN_ERR(svn_wc_ensure_adm4(ctx->wc_ctx, local_abspath, uuid,
                                     session_url, repos_root, revnum, depth,
                                     pool));
          /* Have update fix the incompleteness. */
          err = svn_client__update_internal(result_rev, path, revision,
                                            depth, TRUE, ignore_externals,
                                            allow_unver_obstructions,
                                            use_sleep, FALSE, innercheckout,
                                            ctx, pool);
          goto done;
        }

      /* Get PATH's entry. */
      SVN_ERR(svn_wc__get_entry_versioned(&entry, ctx->wc_ctx, local_abspath,
                                          svn_node_unknown, FALSE, FALSE,
                                          pool, pool));

      /* If PATH's existing URL matches the incoming one, then
         just update.  This allows 'svn co' to restart an
         interrupted checkout. */
      if (entry->url && (strcmp(entry->url, session_url) == 0))
        {
          err = svn_client__update_internal(result_rev, path, revision,
                                            depth, TRUE, ignore_externals,
                                            allow_unver_obstructions,
                                            use_sleep, FALSE, innercheckout,
                                            ctx, pool);
        }
      else
        {
          const char *errmsg;
          errmsg = apr_psprintf
            (pool,
             _("'%s' is already a working copy for a different URL"),
             svn_dirent_local_style(path, pool));
          if (entry->incomplete)
            errmsg = apr_pstrcat
              (pool, errmsg, _("; run 'svn update' to complete it"), NULL);

          return svn_error_create(SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
                                  errmsg);
        }
    }
  else
    {
      return svn_error_createf
        (SVN_ERR_WC_NODE_KIND_CHANGE, NULL,
         _("'%s' already exists and is not a directory"),
         svn_dirent_local_style(path, pool));
    }

 done:
  if (err)
    {
      /* Don't rely on the error handling to handle the sleep later, do
         it now */
      svn_io_sleep_for_timestamps(path, pool);
      return svn_error_return(err);
    }
  *use_sleep = TRUE;

  if (sleep_here)
    svn_io_sleep_for_timestamps(path, pool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_checkout3(svn_revnum_t *result_rev,
                     const char *URL,
                     const char *path,
                     const svn_opt_revision_t *peg_revision,
                     const svn_opt_revision_t *revision,
                     svn_depth_t depth,
                     svn_boolean_t ignore_externals,
                     svn_boolean_t allow_unver_obstructions,
                     svn_client_ctx_t *ctx,
                     apr_pool_t *pool)
{
  return svn_client__checkout_internal(result_rev, URL, path, peg_revision,
                                       revision, NULL, depth, ignore_externals,
                                       allow_unver_obstructions, FALSE, NULL,
                                       ctx, pool);
}
