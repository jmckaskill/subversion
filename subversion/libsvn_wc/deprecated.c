/*
 * deprecated.c:  holding file for all deprecated APIs.
 *                "we can't lose 'em, but we can shun 'em!"
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

/* We define this here to remove any further warnings about the usage of
   deprecated functions in this file. */
#define SVN_DEPRECATED

#include "svn_wc.h"
#include "svn_subst.h"
#include "svn_pools.h"
#include "svn_props.h"
#include "svn_dirent_uri.h"

#include "wc.h"
#include "lock.h"
#include "props.h"

#include "svn_private_config.h"




/*** From adm_crawler.c ***/

/*** Compatibility wrapper: turns an svn_ra_reporter2_t into an
     svn_ra_reporter3_t.

     This code looks like it duplicates code in libsvn_ra/ra_loader.c,
     but it does not.  That code makes an new thing look like an old
     thing; this code makes an old thing look like a new thing. ***/

struct wrap_3to2_report_baton {
  const svn_ra_reporter2_t *reporter;
  void *baton;
};

static svn_error_t *wrap_3to2_set_path(void *report_baton,
                                       const char *path,
                                       svn_revnum_t revision,
                                       svn_depth_t depth,
                                       svn_boolean_t start_empty,
                                       const char *lock_token,
                                       apr_pool_t *pool)
{
  struct wrap_3to2_report_baton *wrb = report_baton;

  return wrb->reporter->set_path(wrb->baton, path, revision, start_empty,
                                 lock_token, pool);
}

static svn_error_t *wrap_3to2_delete_path(void *report_baton,
                                          const char *path,
                                          apr_pool_t *pool)
{
  struct wrap_3to2_report_baton *wrb = report_baton;

  return wrb->reporter->delete_path(wrb->baton, path, pool);
}

static svn_error_t *wrap_3to2_link_path(void *report_baton,
                                        const char *path,
                                        const char *url,
                                        svn_revnum_t revision,
                                        svn_depth_t depth,
                                        svn_boolean_t start_empty,
                                        const char *lock_token,
                                        apr_pool_t *pool)
{
  struct wrap_3to2_report_baton *wrb = report_baton;

  return wrb->reporter->link_path(wrb->baton, path, url, revision,
                                  start_empty, lock_token, pool);
}

static svn_error_t *wrap_3to2_finish_report(void *report_baton,
                                            apr_pool_t *pool)
{
  struct wrap_3to2_report_baton *wrb = report_baton;

  return wrb->reporter->finish_report(wrb->baton, pool);
}

static svn_error_t *wrap_3to2_abort_report(void *report_baton,
                                           apr_pool_t *pool)
{
  struct wrap_3to2_report_baton *wrb = report_baton;

  return wrb->reporter->abort_report(wrb->baton, pool);
}

static const svn_ra_reporter3_t wrap_3to2_reporter = {
  wrap_3to2_set_path,
  wrap_3to2_delete_path,
  wrap_3to2_link_path,
  wrap_3to2_finish_report,
  wrap_3to2_abort_report
};

svn_error_t *
svn_wc_crawl_revisions3(const char *path,
                        svn_wc_adm_access_t *adm_access,
                        const svn_ra_reporter3_t *reporter,
                        void *report_baton,
                        svn_boolean_t restore_files,
                        svn_depth_t depth,
                        svn_boolean_t depth_compatibility_trick,
                        svn_boolean_t use_commit_times,
                        svn_wc_notify_func2_t notify_func,
                        void *notify_baton,
                        svn_wc_traversal_info_t *traversal_info,
                        apr_pool_t *pool)
{
  return svn_wc_crawl_revisions4(path,
                                 adm_access,
                                 reporter, report_baton,
                                 restore_files,
                                 depth,
                                 FALSE,
                                 depth_compatibility_trick,
                                 use_commit_times,
                                 notify_func,
                                 notify_baton,
                                 traversal_info,
                                 pool);
}

svn_error_t *
svn_wc_crawl_revisions2(const char *path,
                        svn_wc_adm_access_t *adm_access,
                        const svn_ra_reporter2_t *reporter,
                        void *report_baton,
                        svn_boolean_t restore_files,
                        svn_boolean_t recurse,
                        svn_boolean_t use_commit_times,
                        svn_wc_notify_func2_t notify_func,
                        void *notify_baton,
                        svn_wc_traversal_info_t *traversal_info,
                        apr_pool_t *pool)
{
  struct wrap_3to2_report_baton wrb;
  wrb.reporter = reporter;
  wrb.baton = report_baton;

  return svn_wc_crawl_revisions3(path,
                                 adm_access,
                                 &wrap_3to2_reporter, &wrb,
                                 restore_files,
                                 SVN_DEPTH_INFINITY_OR_FILES(recurse),
                                 FALSE,
                                 use_commit_times,
                                 notify_func,
                                 notify_baton,
                                 traversal_info,
                                 pool);
}


/*** Compatibility wrapper: turns an svn_ra_reporter_t into an
     svn_ra_reporter2_t.

     This code looks like it duplicates code in libsvn_ra/ra_loader.c,
     but it does not.  That code makes an new thing look like an old
     thing; this code makes an old thing look like a new thing. ***/

struct wrap_2to1_report_baton {
  const svn_ra_reporter_t *reporter;
  void *baton;
};

static svn_error_t *wrap_2to1_set_path(void *report_baton,
                                       const char *path,
                                       svn_revnum_t revision,
                                       svn_boolean_t start_empty,
                                       const char *lock_token,
                                       apr_pool_t *pool)
{
  struct wrap_2to1_report_baton *wrb = report_baton;

  return wrb->reporter->set_path(wrb->baton, path, revision, start_empty,
                                 pool);
}

static svn_error_t *wrap_2to1_delete_path(void *report_baton,
                                          const char *path,
                                          apr_pool_t *pool)
{
  struct wrap_2to1_report_baton *wrb = report_baton;

  return wrb->reporter->delete_path(wrb->baton, path, pool);
}

static svn_error_t *wrap_2to1_link_path(void *report_baton,
                                        const char *path,
                                        const char *url,
                                        svn_revnum_t revision,
                                        svn_boolean_t start_empty,
                                        const char *lock_token,
                                        apr_pool_t *pool)
{
  struct wrap_2to1_report_baton *wrb = report_baton;

  return wrb->reporter->link_path(wrb->baton, path, url, revision,
                                  start_empty, pool);
}

static svn_error_t *wrap_2to1_finish_report(void *report_baton,
                                            apr_pool_t *pool)
{
  struct wrap_2to1_report_baton *wrb = report_baton;

  return wrb->reporter->finish_report(wrb->baton, pool);
}

static svn_error_t *wrap_2to1_abort_report(void *report_baton,
                                           apr_pool_t *pool)
{
  struct wrap_2to1_report_baton *wrb = report_baton;

  return wrb->reporter->abort_report(wrb->baton, pool);
}

static const svn_ra_reporter2_t wrap_2to1_reporter = {
  wrap_2to1_set_path,
  wrap_2to1_delete_path,
  wrap_2to1_link_path,
  wrap_2to1_finish_report,
  wrap_2to1_abort_report
};

svn_error_t *
svn_wc_crawl_revisions(const char *path,
                       svn_wc_adm_access_t *adm_access,
                       const svn_ra_reporter_t *reporter,
                       void *report_baton,
                       svn_boolean_t restore_files,
                       svn_boolean_t recurse,
                       svn_boolean_t use_commit_times,
                       svn_wc_notify_func_t notify_func,
                       void *notify_baton,
                       svn_wc_traversal_info_t *traversal_info,
                       apr_pool_t *pool)
{
  struct wrap_2to1_report_baton wrb;
  svn_wc__compat_notify_baton_t nb;

  wrb.reporter = reporter;
  wrb.baton = report_baton;

  nb.func = notify_func;
  nb.baton = notify_baton;

  return svn_wc_crawl_revisions2(path, adm_access, &wrap_2to1_reporter, &wrb,
                                 restore_files, recurse, use_commit_times,
                                 svn_wc__compat_call_notify_func, &nb,
                                 traversal_info,
                                 pool);
}

svn_error_t *
svn_wc_transmit_text_deltas2(const char **tempfile,
                             unsigned char digest[],
                             const char *path,
                             svn_wc_adm_access_t *adm_access,
                             svn_boolean_t fulltext,
                             const svn_delta_editor_t *editor,
                             void *file_baton,
                             apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc_context_t *wc_ctx;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access),
                                         pool));

  SVN_ERR(svn_wc_transmit_text_deltas3(tempfile, digest, wc_ctx,
                                       local_abspath, fulltext, editor,
                                       file_baton, pool, pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}

svn_error_t *
svn_wc_transmit_text_deltas(const char *path,
                            svn_wc_adm_access_t *adm_access,
                            svn_boolean_t fulltext,
                            const svn_delta_editor_t *editor,
                            void *file_baton,
                            const char **tempfile,
                            apr_pool_t *pool)
{
  return svn_wc_transmit_text_deltas2(tempfile, NULL, path, adm_access,
                                      fulltext, editor, file_baton, pool);
}

svn_error_t *
svn_wc_transmit_prop_deltas(const char *path,
                            svn_wc_adm_access_t *adm_access,
                            const svn_wc_entry_t *entry,
                            const svn_delta_editor_t *editor,
                            void *baton,
                            const char **tempfile,
                            apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc_context_t *wc_ctx;

  if (tempfile)
    *tempfile = NULL;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access),
                                         pool));

  SVN_ERR(svn_wc_transmit_prop_deltas2(wc_ctx, local_abspath, editor, baton, 
                                       pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}

/*** From adm_files.c ***/
svn_error_t *
svn_wc_ensure_adm2(const char *path,
                   const char *uuid,
                   const char *url,
                   const char *repos,
                   svn_revnum_t revision,
                   apr_pool_t *pool)
{
  return svn_wc_ensure_adm3(path, uuid, url, repos, revision,
                            svn_depth_infinity, pool);
}


svn_error_t *
svn_wc_ensure_adm(const char *path,
                  const char *uuid,
                  const char *url,
                  svn_revnum_t revision,
                  apr_pool_t *pool)
{
  return svn_wc_ensure_adm2(path, uuid, url, NULL, revision, pool);
}

svn_error_t *
svn_wc_create_tmp_file(apr_file_t **fp,
                       const char *path,
                       svn_boolean_t delete_on_close,
                       apr_pool_t *pool)
{
  return svn_wc_create_tmp_file2(fp, NULL, path,
                                 delete_on_close
                                 ? svn_io_file_del_on_close
                                 : svn_io_file_del_none,
                                 pool);
}


/*** From adm_ops.c ***/
svn_error_t *
svn_wc_process_committed3(const char *path,
                          svn_wc_adm_access_t *adm_access,
                          svn_boolean_t recurse,
                          svn_revnum_t new_revnum,
                          const char *rev_date,
                          const char *rev_author,
                          apr_array_header_t *wcprop_changes,
                          svn_boolean_t remove_lock,
                          const unsigned char *digest,
                          apr_pool_t *pool)
{
  return svn_wc_process_committed4(path, adm_access, recurse, new_revnum,
                                   rev_date, rev_author, wcprop_changes,
                                   remove_lock, FALSE, digest, pool);
}

svn_error_t *
svn_wc_process_committed2(const char *path,
                          svn_wc_adm_access_t *adm_access,
                          svn_boolean_t recurse,
                          svn_revnum_t new_revnum,
                          const char *rev_date,
                          const char *rev_author,
                          apr_array_header_t *wcprop_changes,
                          svn_boolean_t remove_lock,
                          apr_pool_t *pool)
{
  return svn_wc_process_committed3(path, adm_access, recurse, new_revnum,
                                   rev_date, rev_author, wcprop_changes,
                                   remove_lock, NULL, pool);
}

svn_error_t *
svn_wc_process_committed(const char *path,
                         svn_wc_adm_access_t *adm_access,
                         svn_boolean_t recurse,
                         svn_revnum_t new_revnum,
                         const char *rev_date,
                         const char *rev_author,
                         apr_array_header_t *wcprop_changes,
                         apr_pool_t *pool)
{
  return svn_wc_process_committed2(path, adm_access, recurse, new_revnum,
                                   rev_date, rev_author, wcprop_changes,
                                   FALSE, pool);
}

svn_error_t *
svn_wc_delete2(const char *path,
               svn_wc_adm_access_t *adm_access,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               svn_wc_notify_func2_t notify_func,
               void *notify_baton,
               apr_pool_t *pool)
{
  return svn_wc_delete3(path, adm_access, cancel_func, cancel_baton,
                        notify_func, notify_baton, FALSE, pool);
}

svn_error_t *
svn_wc_delete(const char *path,
              svn_wc_adm_access_t *adm_access,
              svn_cancel_func_t cancel_func,
              void *cancel_baton,
              svn_wc_notify_func_t notify_func,
              void *notify_baton,
              apr_pool_t *pool)
{
  svn_wc__compat_notify_baton_t nb;

  nb.func = notify_func;
  nb.baton = notify_baton;

  return svn_wc_delete2(path, adm_access, cancel_func, cancel_baton,
                        svn_wc__compat_call_notify_func, &nb, pool);
}

svn_error_t *
svn_wc_add2(const char *path,
            svn_wc_adm_access_t *parent_access,
            const char *copyfrom_url,
            svn_revnum_t copyfrom_rev,
            svn_cancel_func_t cancel_func,
            void *cancel_baton,
            svn_wc_notify_func2_t notify_func,
            void *notify_baton,
            apr_pool_t *pool)
{
  return svn_wc_add3(path, parent_access, svn_depth_infinity,
                     copyfrom_url, copyfrom_rev,
                     cancel_func, cancel_baton,
                     notify_func, notify_baton, pool);
}

svn_error_t *
svn_wc_add(const char *path,
           svn_wc_adm_access_t *parent_access,
           const char *copyfrom_url,
           svn_revnum_t copyfrom_rev,
           svn_cancel_func_t cancel_func,
           void *cancel_baton,
           svn_wc_notify_func_t notify_func,
           void *notify_baton,
           apr_pool_t *pool)
{
  svn_wc__compat_notify_baton_t nb;

  nb.func = notify_func;
  nb.baton = notify_baton;

  return svn_wc_add2(path, parent_access, copyfrom_url, copyfrom_rev,
                     cancel_func, cancel_baton,
                     svn_wc__compat_call_notify_func, &nb, pool);
}

svn_error_t *
svn_wc_revert2(const char *path,
               svn_wc_adm_access_t *parent_access,
               svn_boolean_t recursive,
               svn_boolean_t use_commit_times,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               svn_wc_notify_func2_t notify_func,
               void *notify_baton,
               apr_pool_t *pool)
{
  return svn_wc_revert3(path, parent_access,
                        SVN_DEPTH_INFINITY_OR_EMPTY(recursive),
                        use_commit_times, NULL, cancel_func, cancel_baton,
                        notify_func, notify_baton, pool);
}

svn_error_t *
svn_wc_revert(const char *path,
              svn_wc_adm_access_t *parent_access,
              svn_boolean_t recursive,
              svn_boolean_t use_commit_times,
              svn_cancel_func_t cancel_func,
              void *cancel_baton,
              svn_wc_notify_func_t notify_func,
              void *notify_baton,
              apr_pool_t *pool)
{
  svn_wc__compat_notify_baton_t nb;

  nb.func = notify_func;
  nb.baton = notify_baton;

  return svn_wc_revert2(path, parent_access, recursive, use_commit_times,
                        cancel_func, cancel_baton,
                        svn_wc__compat_call_notify_func, &nb, pool);
}

svn_error_t *
svn_wc_resolved_conflict(const char *path,
                         svn_wc_adm_access_t *adm_access,
                         svn_boolean_t resolve_text,
                         svn_boolean_t resolve_props,
                         svn_boolean_t recurse,
                         svn_wc_notify_func_t notify_func,
                         void *notify_baton,
                         apr_pool_t *pool)
{
  svn_wc__compat_notify_baton_t nb;

  nb.func = notify_func;
  nb.baton = notify_baton;

  return svn_wc_resolved_conflict2(path, adm_access,
                                   resolve_text, resolve_props, recurse,
                                   svn_wc__compat_call_notify_func, &nb,
                                   NULL, NULL, pool);

}

svn_error_t *
svn_wc_resolved_conflict2(const char *path,
                          svn_wc_adm_access_t *adm_access,
                          svn_boolean_t resolve_text,
                          svn_boolean_t resolve_props,
                          svn_boolean_t recurse,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          apr_pool_t *pool)
{
  return svn_wc_resolved_conflict3(path, adm_access, resolve_text,
                                   resolve_props,
                                   SVN_DEPTH_INFINITY_OR_EMPTY(recurse),
                                   svn_wc_conflict_choose_merged,
                                   notify_func, notify_baton, cancel_func,
                                   cancel_baton, pool);
}

svn_error_t *
svn_wc_resolved_conflict3(const char *path,
                          svn_wc_adm_access_t *adm_access,
                          svn_boolean_t resolve_text,
                          svn_boolean_t resolve_props,
                          svn_depth_t depth,
                          svn_wc_conflict_choice_t conflict_choice,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          apr_pool_t *pool)
{
  return svn_wc_resolved_conflict4(path, adm_access, resolve_text,
                                   resolve_props, FALSE, depth,
                                   svn_wc_conflict_choose_merged,
                                   notify_func, notify_baton, cancel_func,
                                   cancel_baton, pool);
}

svn_error_t *
svn_wc_add_lock(const char *path,
                const svn_lock_t *lock,
                svn_wc_adm_access_t *adm_access,
                apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc_context_t *wc_ctx;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access),
                                         pool));

  return svn_error_return(svn_wc_add_lock2(wc_ctx, local_abspath, lock, pool));
}

svn_error_t *
svn_wc_remove_lock(const char *path,
                   svn_wc_adm_access_t *adm_access,
                   apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc_context_t *wc_ctx;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access),
                                         pool));

  return svn_error_return(svn_wc_remove_lock2(wc_ctx, local_abspath, pool));
}

/*** From diff.c ***/
/* Used to wrap svn_wc_diff_callbacks_t. */
struct diff_callbacks_wrapper_baton {
  const svn_wc_diff_callbacks_t *callbacks;
  void *baton;
};

/* An svn_wc_diff_callbacks3_t function for wrapping svn_wc_diff_callbacks_t. */
static svn_error_t *
wrap_3to1_file_changed(svn_wc_adm_access_t *adm_access,
                       svn_wc_notify_state_t *contentstate,
                       svn_wc_notify_state_t *propstate,
                       svn_boolean_t *tree_conflicted,
                       const char *path,
                       const char *tmpfile1,
                       const char *tmpfile2,
                       svn_revnum_t rev1,
                       svn_revnum_t rev2,
                       const char *mimetype1,
                       const char *mimetype2,
                       const apr_array_header_t *propchanges,
                       apr_hash_t *originalprops,
                       void *diff_baton)
{
  struct diff_callbacks_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  if (tmpfile2 != NULL)
    SVN_ERR(b->callbacks->file_changed(adm_access, contentstate, path,
                                       tmpfile1, tmpfile2,
                                       rev1, rev2, mimetype1, mimetype2,
                                       b->baton));
  if (propchanges->nelts > 0)
    SVN_ERR(b->callbacks->props_changed(adm_access, propstate, path,
                                        propchanges, originalprops,
                                        b->baton));

  return SVN_NO_ERROR;
}

/* An svn_wc_diff_callbacks3_t function for wrapping svn_wc_diff_callbacks_t. */
static svn_error_t *
wrap_3to1_file_added(svn_wc_adm_access_t *adm_access,
                     svn_wc_notify_state_t *contentstate,
                     svn_wc_notify_state_t *propstate,
                     svn_boolean_t *tree_conflicted,
                     const char *path,
                     const char *tmpfile1,
                     const char *tmpfile2,
                     svn_revnum_t rev1,
                     svn_revnum_t rev2,
                     const char *mimetype1,
                     const char *mimetype2,
                     const apr_array_header_t *propchanges,
                     apr_hash_t *originalprops,
                     void *diff_baton)
{
  struct diff_callbacks_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  SVN_ERR(b->callbacks->file_added(adm_access, contentstate, path,
                                   tmpfile1, tmpfile2, rev1, rev2,
                                   mimetype1, mimetype2, b->baton));
  if (propchanges->nelts > 0)
    SVN_ERR(b->callbacks->props_changed(adm_access, propstate, path,
                                        propchanges, originalprops,
                                        b->baton));

  return SVN_NO_ERROR;
}

/* An svn_wc_diff_callbacks3_t function for wrapping svn_wc_diff_callbacks_t. */
static svn_error_t *
wrap_3to1_file_deleted(svn_wc_adm_access_t *adm_access,
                       svn_wc_notify_state_t *state,
                       svn_boolean_t *tree_conflicted,
                       const char *path,
                       const char *tmpfile1,
                       const char *tmpfile2,
                       const char *mimetype1,
                       const char *mimetype2,
                       apr_hash_t *originalprops,
                       void *diff_baton)
{
  struct diff_callbacks_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  SVN_ERR_ASSERT(originalprops);

  return b->callbacks->file_deleted(adm_access, state, path,
                                    tmpfile1, tmpfile2, mimetype1, mimetype2,
                                    b->baton);
}

/* An svn_wc_diff_callbacks3_t function for wrapping svn_wc_diff_callbacks_t. */
static svn_error_t *
wrap_3to1_dir_added(svn_wc_adm_access_t *adm_access,
                    svn_wc_notify_state_t *state,
                    svn_boolean_t *tree_conflicted,
                    const char *path,
                    svn_revnum_t rev,
                    void *diff_baton)
{
  struct diff_callbacks_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  return b->callbacks->dir_added(adm_access, state, path, rev, b->baton);
}

/* An svn_wc_diff_callbacks3_t function for wrapping svn_wc_diff_callbacks_t. */
static svn_error_t *
wrap_3to1_dir_deleted(svn_wc_adm_access_t *adm_access,
                      svn_wc_notify_state_t *state,
                      svn_boolean_t *tree_conflicted,
                      const char *path,
                      void *diff_baton)
{
  struct diff_callbacks_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  return b->callbacks->dir_deleted(adm_access, state, path, b->baton);
}

/* An svn_wc_diff_callbacks3_t function for wrapping svn_wc_diff_callbacks_t. */
static svn_error_t *
wrap_3to1_dir_props_changed(svn_wc_adm_access_t *adm_access,
                            svn_wc_notify_state_t *state,
                            svn_boolean_t *tree_conflicted,
                            const char *path,
                            const apr_array_header_t *propchanges,
                            apr_hash_t *originalprops,
                            void *diff_baton)
{
  struct diff_callbacks_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  return b->callbacks->props_changed(adm_access, state, path, propchanges,
                                     originalprops, b->baton);
}

/* An svn_wc_diff_callbacks3_t function for wrapping svn_wc_diff_callbacks_t
   and svn_wc_diff_callbacks2_t. */
static svn_error_t *
wrap_3to1or2_dir_opened(svn_wc_adm_access_t *adm_access,
                        svn_boolean_t *tree_conflicted,
                        const char *path,
                        svn_revnum_t rev,
                        void *diff_baton)
{
  if (tree_conflicted)
    *tree_conflicted = FALSE;
  /* Do nothing. */
  return SVN_NO_ERROR;
}

/* An svn_wc_diff_callbacks3_t function for wrapping svn_wc_diff_callbacks_t
   and svn_wc_diff_callbacks2_t. */
static svn_error_t *
wrap_3to1or2_dir_closed(svn_wc_adm_access_t *adm_access,
                        svn_wc_notify_state_t *propstate,
                        svn_wc_notify_state_t *contentstate,
                        svn_boolean_t *tree_conflicted,
                        const char *path,
                        void *diff_baton)
{
  if (contentstate)
    *contentstate = svn_wc_notify_state_unknown;
  if (propstate)
    *propstate = svn_wc_notify_state_unknown;
  if (tree_conflicted)
    *tree_conflicted = FALSE;
  /* Do nothing. */
  return SVN_NO_ERROR;
}

/* Used to wrap svn_diff_callbacks_t as an svn_wc_diff_callbacks3_t. */
static struct svn_wc_diff_callbacks3_t diff_callbacks_wrapper = {
  wrap_3to1_file_changed,
  wrap_3to1_file_added,
  wrap_3to1_file_deleted,
  wrap_3to1_dir_added,
  wrap_3to1_dir_deleted,
  wrap_3to1_dir_props_changed,
  wrap_3to1or2_dir_opened,
  wrap_3to1or2_dir_closed
};



/* Used to wrap svn_wc_diff_callbacks2_t. */
struct diff_callbacks2_wrapper_baton {
  const svn_wc_diff_callbacks2_t *callbacks2;
  void *baton;
};

/* An svn_wc_diff_callbacks3_t function for wrapping
 * svn_wc_diff_callbacks2_t. */
static svn_error_t *
wrap_3to2_file_changed(svn_wc_adm_access_t *adm_access,
                       svn_wc_notify_state_t *contentstate,
                       svn_wc_notify_state_t *propstate,
                       svn_boolean_t *tree_conflicted,
                       const char *path,
                       const char *tmpfile1,
                       const char *tmpfile2,
                       svn_revnum_t rev1,
                       svn_revnum_t rev2,
                       const char *mimetype1,
                       const char *mimetype2,
                       const apr_array_header_t *propchanges,
                       apr_hash_t *originalprops,
                       void *diff_baton)
{
  struct diff_callbacks2_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  return b->callbacks2->file_changed(adm_access, contentstate, propstate,
                                     path, tmpfile1, tmpfile2,
                                     rev1, rev2, mimetype1, mimetype2,
                                     propchanges, originalprops, b->baton);
}

/* An svn_wc_diff_callbacks3_t function for wrapping
 * svn_wc_diff_callbacks2_t. */
static svn_error_t *
wrap_3to2_file_added(svn_wc_adm_access_t *adm_access,
                     svn_wc_notify_state_t *contentstate,
                     svn_wc_notify_state_t *propstate,
                     svn_boolean_t *tree_conflicted,
                     const char *path,
                     const char *tmpfile1,
                     const char *tmpfile2,
                     svn_revnum_t rev1,
                     svn_revnum_t rev2,
                     const char *mimetype1,
                     const char *mimetype2,
                     const apr_array_header_t *propchanges,
                     apr_hash_t *originalprops,
                     void *diff_baton)
{
  struct diff_callbacks2_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  return b->callbacks2->file_added(adm_access, contentstate, propstate, path,
                                   tmpfile1, tmpfile2, rev1, rev2,
                                   mimetype1, mimetype2, propchanges,
                                   originalprops, b->baton);
}

/* An svn_wc_diff_callbacks3_t function for wrapping
 * svn_wc_diff_callbacks2_t. */
static svn_error_t *
wrap_3to2_file_deleted(svn_wc_adm_access_t *adm_access,
                       svn_wc_notify_state_t *state,
                       svn_boolean_t *tree_conflicted,
                       const char *path,
                       const char *tmpfile1,
                       const char *tmpfile2,
                       const char *mimetype1,
                       const char *mimetype2,
                       apr_hash_t *originalprops,
                       void *diff_baton)
{
  struct diff_callbacks2_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  return b->callbacks2->file_deleted(adm_access, state, path,
                                     tmpfile1, tmpfile2, mimetype1, mimetype2,
                                     originalprops, b->baton);
}

/* An svn_wc_diff_callbacks3_t function for wrapping
 * svn_wc_diff_callbacks2_t. */
static svn_error_t *
wrap_3to2_dir_added(svn_wc_adm_access_t *adm_access,
                    svn_wc_notify_state_t *state,
                    svn_boolean_t *tree_conflicted,
                    const char *path,
                    svn_revnum_t rev,
                    void *diff_baton)
{
  struct diff_callbacks2_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  return b->callbacks2->dir_added(adm_access, state, path, rev, b->baton);
}

/* An svn_wc_diff_callbacks3_t function for wrapping
 * svn_wc_diff_callbacks2_t. */
static svn_error_t *
wrap_3to2_dir_deleted(svn_wc_adm_access_t *adm_access,
                      svn_wc_notify_state_t *state,
                      svn_boolean_t *tree_conflicted,
                      const char *path,
                      void *diff_baton)
{
  struct diff_callbacks2_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  return b->callbacks2->dir_deleted(adm_access, state, path, b->baton);
}

/* An svn_wc_diff_callbacks3_t function for wrapping
 * svn_wc_diff_callbacks2_t. */
static svn_error_t *
wrap_3to2_dir_props_changed(svn_wc_adm_access_t *adm_access,
                            svn_wc_notify_state_t *state,
                            svn_boolean_t *tree_conflicted,
                            const char *path,
                            const apr_array_header_t *propchanges,
                            apr_hash_t *originalprops,
                            void *diff_baton)
{
  struct diff_callbacks2_wrapper_baton *b = diff_baton;

  if (tree_conflicted)
    *tree_conflicted = FALSE;

  return b->callbacks2->dir_props_changed(adm_access, state, path, propchanges,
                                          originalprops, b->baton);
}

/* Used to wrap svn_diff_callbacks2_t as an svn_wc_diff_callbacks3_t. */
static struct svn_wc_diff_callbacks3_t diff_callbacks2_wrapper = {
  wrap_3to2_file_changed,
  wrap_3to2_file_added,
  wrap_3to2_file_deleted,
  wrap_3to2_dir_added,
  wrap_3to2_dir_deleted,
  wrap_3to2_dir_props_changed,
  wrap_3to1or2_dir_opened,
  wrap_3to1or2_dir_closed
};



/* Used to wrap svn_wc_diff_callbacks3_t. */
struct diff_callbacks3_wrapper_baton {
  const svn_wc_diff_callbacks3_t *callbacks3;
  void *baton;
};

/* An svn_wc_diff_callbacks4_t function for wrapping
 * svn_wc_diff_callbacks3_t. */
static svn_error_t *
wrap_4to3_file_changed(svn_wc_adm_access_t *adm_access,
                       svn_wc_notify_state_t *contentstate,
                       svn_wc_notify_state_t *propstate,
                       svn_boolean_t *tree_conflicted,
                       const char *path,
                       const char *tmpfile1,
                       const char *tmpfile2,
                       svn_revnum_t rev1,
                       svn_revnum_t rev2,
                       const char *mimetype1,
                       const char *mimetype2,
                       const apr_array_header_t *propchanges,
                       apr_hash_t *originalprops,
                       void *diff_baton)
{
  struct diff_callbacks3_wrapper_baton *b = diff_baton;

  return b->callbacks3->file_changed(adm_access, contentstate, propstate,
                                     tree_conflicted, path, tmpfile1, tmpfile2,
                                     rev1, rev2, mimetype1, mimetype2,
                                     propchanges, originalprops, b->baton);
}

/* An svn_wc_diff_callbacks4_t function for wrapping
 * svn_wc_diff_callbacks3_t. */
static svn_error_t *
wrap_4to3_file_added(svn_wc_adm_access_t *adm_access,
                     svn_wc_notify_state_t *contentstate,
                     svn_wc_notify_state_t *propstate,
                     svn_boolean_t *tree_conflicted,
                     const char *path,
                     const char *tmpfile1,
                     const char *tmpfile2,
                     svn_revnum_t rev1,
                     svn_revnum_t rev2,
                     const char *mimetype1,
                     const char *mimetype2,
                     const char *copyfrom_path,
                     svn_revnum_t copyfrom_revision,
                     const apr_array_header_t *propchanges,
                     apr_hash_t *originalprops,
                     void *diff_baton)
{
  struct diff_callbacks3_wrapper_baton *b = diff_baton;

  return b->callbacks3->file_added(adm_access, contentstate, propstate,
                                   tree_conflicted, path, tmpfile1, tmpfile2,
                                   rev1, rev2, mimetype1, mimetype2,
                                   propchanges, originalprops, b->baton);
}

/* An svn_wc_diff_callbacks4_t function for wrapping
 * svn_wc_diff_callbacks3_t. */
static svn_error_t *
wrap_4to3_file_deleted(svn_wc_adm_access_t *adm_access,
                       svn_wc_notify_state_t *state,
                       svn_boolean_t *tree_conflicted,
                       const char *path,
                       const char *tmpfile1,
                       const char *tmpfile2,
                       const char *mimetype1,
                       const char *mimetype2,
                       apr_hash_t *originalprops,
                       void *diff_baton)
{
  struct diff_callbacks3_wrapper_baton *b = diff_baton;

  return b->callbacks3->file_deleted(adm_access, state, tree_conflicted,
                                     path, tmpfile1, tmpfile2,
                                     mimetype1, mimetype2, originalprops,
                                     b->baton);
}

/* An svn_wc_diff_callbacks4_t function for wrapping
 * svn_wc_diff_callbacks3_t. */
static svn_error_t *
wrap_4to3_dir_added(svn_wc_adm_access_t *adm_access,
                    svn_wc_notify_state_t *state,
                    svn_boolean_t *tree_conflicted,
                    const char *path,
                    svn_revnum_t rev,
                    const char *copyfrom_path,
                    svn_revnum_t copyfrom_revision,
                    void *diff_baton)
{
  struct diff_callbacks3_wrapper_baton *b = diff_baton;

  return b->callbacks3->dir_added(adm_access, state, tree_conflicted, path, rev, b->baton);
}

/* An svn_wc_diff_callbacks4_t function for wrapping
 * svn_wc_diff_callbacks3_t. */
static svn_error_t *
wrap_4to3_dir_deleted(svn_wc_adm_access_t *adm_access,
                      svn_wc_notify_state_t *state,
                      svn_boolean_t *tree_conflicted,
                      const char *path,
                      void *diff_baton)
{
  struct diff_callbacks3_wrapper_baton *b = diff_baton;

  return b->callbacks3->dir_deleted(adm_access, state, tree_conflicted,
                                    path, b->baton);
}

/* An svn_wc_diff_callbacks4_t function for wrapping
 * svn_wc_diff_callbacks3_t. */
static svn_error_t *
wrap_4to3_dir_props_changed(svn_wc_adm_access_t *adm_access,
                            svn_wc_notify_state_t *propstate,
                            svn_boolean_t *tree_conflicted,
                            const char *path,
                            const apr_array_header_t *propchanges,
                            apr_hash_t *original_props,
                            void *diff_baton)
{
  struct diff_callbacks3_wrapper_baton *b = diff_baton;

  return b->callbacks3->dir_props_changed(adm_access, propstate,
                                          tree_conflicted, path,
                                          propchanges, original_props,
                                          b->baton);
}

/* An svn_wc_diff_callbacks4_t function for wrapping
 * svn_wc_diff_callbacks3_t. */
static svn_error_t *
wrap_4to3_dir_opened(svn_wc_adm_access_t *adm_access,
                     svn_boolean_t *tree_conflicted,
                     const char *path,
                     svn_revnum_t rev,
                     void *diff_baton)
{
  struct diff_callbacks3_wrapper_baton *b = diff_baton;

  return b->callbacks3->dir_opened(adm_access, tree_conflicted, path, rev,
                                   b->baton);
}

/* An svn_wc_diff_callbacks4_t function for wrapping
 * svn_wc_diff_callbacks3_t. */
static svn_error_t *
wrap_4to3_dir_closed(svn_wc_adm_access_t *adm_access,
                     svn_wc_notify_state_t *contentstate,
                     svn_wc_notify_state_t *propstate,
                     svn_boolean_t *tree_conflicted,
                     const char *path,
                     void *diff_baton)
{
  struct diff_callbacks3_wrapper_baton *b = diff_baton;

  return b->callbacks3->dir_closed(adm_access, contentstate, propstate,
                                   tree_conflicted, path, b->baton);
}


/* Used to wrap svn_diff_callbacks3_t as an svn_wc_diff_callbacks4_t. */
static struct svn_wc_diff_callbacks4_t diff_callbacks3_wrapper = {
  wrap_4to3_file_changed,
  wrap_4to3_file_added,
  wrap_4to3_file_deleted,
  wrap_4to3_dir_added,
  wrap_4to3_dir_deleted,
  wrap_4to3_dir_props_changed,
  wrap_4to3_dir_opened,
  wrap_4to3_dir_closed
};

svn_error_t *
svn_wc_get_diff_editor5(svn_wc_adm_access_t *anchor,
                        const char *target,
                        const svn_wc_diff_callbacks3_t *callbacks,
                        void *callback_baton,
                        svn_depth_t depth,
                        svn_boolean_t ignore_ancestry,
                        svn_boolean_t use_text_base,
                        svn_boolean_t reverse_order,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        const apr_array_header_t *changelists,
                        const svn_delta_editor_t **editor,
                        void **edit_baton,
                        apr_pool_t *pool)
{
  struct diff_callbacks3_wrapper_baton *b = apr_palloc(pool, sizeof(*b));
  b->callbacks3 = callbacks;
  b->baton = callback_baton;
  return svn_wc_get_diff_editor6(anchor,
                                 target,
                                 &diff_callbacks3_wrapper,
                                 b,
                                 depth,
                                 ignore_ancestry,
                                 use_text_base,
                                 reverse_order,
                                 cancel_func,
                                 cancel_baton,
                                 changelists,
                                 editor,
                                 edit_baton,
                                 NULL,
                                 pool);
}

svn_error_t *
svn_wc_get_diff_editor4(svn_wc_adm_access_t *anchor,
                        const char *target,
                        const svn_wc_diff_callbacks2_t *callbacks,
                        void *callback_baton,
                        svn_depth_t depth,
                        svn_boolean_t ignore_ancestry,
                        svn_boolean_t use_text_base,
                        svn_boolean_t reverse_order,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        const apr_array_header_t *changelists,
                        const svn_delta_editor_t **editor,
                        void **edit_baton,
                        apr_pool_t *pool)
{
  struct diff_callbacks2_wrapper_baton *b = apr_palloc(pool, sizeof(*b));
  b->callbacks2 = callbacks;
  b->baton = callback_baton;
  return svn_wc_get_diff_editor5(anchor,
                                 target,
                                 &diff_callbacks2_wrapper,
                                 b,
                                 depth,
                                 ignore_ancestry,
                                 use_text_base,
                                 reverse_order,
                                 cancel_func,
                                 cancel_baton,
                                 changelists,
                                 editor,
                                 edit_baton,
                                 pool);
}

svn_error_t *
svn_wc_get_diff_editor3(svn_wc_adm_access_t *anchor,
                        const char *target,
                        const svn_wc_diff_callbacks2_t *callbacks,
                        void *callback_baton,
                        svn_boolean_t recurse,
                        svn_boolean_t ignore_ancestry,
                        svn_boolean_t use_text_base,
                        svn_boolean_t reverse_order,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        const svn_delta_editor_t **editor,
                        void **edit_baton,
                        apr_pool_t *pool)
{
  return svn_wc_get_diff_editor4(anchor,
                                 target,
                                 callbacks,
                                 callback_baton,
                                 SVN_DEPTH_INFINITY_OR_FILES(recurse),
                                 ignore_ancestry,
                                 use_text_base,
                                 reverse_order,
                                 cancel_func,
                                 cancel_baton,
                                 NULL,
                                 editor,
                                 edit_baton,
                                 pool);
}

svn_error_t *
svn_wc_get_diff_editor2(svn_wc_adm_access_t *anchor,
                        const char *target,
                        const svn_wc_diff_callbacks_t *callbacks,
                        void *callback_baton,
                        svn_boolean_t recurse,
                        svn_boolean_t ignore_ancestry,
                        svn_boolean_t use_text_base,
                        svn_boolean_t reverse_order,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        const svn_delta_editor_t **editor,
                        void **edit_baton,
                        apr_pool_t *pool)
{
  struct diff_callbacks_wrapper_baton *b = apr_palloc(pool, sizeof(*b));
  b->callbacks = callbacks;
  b->baton = callback_baton;
  return svn_wc_get_diff_editor5(anchor, target, &diff_callbacks_wrapper, b,
                                 SVN_DEPTH_INFINITY_OR_FILES(recurse),
                                 ignore_ancestry, use_text_base,
                                 reverse_order, cancel_func, cancel_baton,
                                 NULL, editor, edit_baton, pool);
}

svn_error_t *
svn_wc_get_diff_editor(svn_wc_adm_access_t *anchor,
                       const char *target,
                       const svn_wc_diff_callbacks_t *callbacks,
                       void *callback_baton,
                       svn_boolean_t recurse,
                       svn_boolean_t use_text_base,
                       svn_boolean_t reverse_order,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       const svn_delta_editor_t **editor,
                       void **edit_baton,
                       apr_pool_t *pool)
{
  return svn_wc_get_diff_editor2(anchor, target, callbacks, callback_baton,
                                 recurse, FALSE, use_text_base, reverse_order,
                                 cancel_func, cancel_baton,
                                 editor, edit_baton, pool);
}

svn_error_t *
svn_wc_diff5(svn_wc_adm_access_t *anchor,
             const char *target,
             const svn_wc_diff_callbacks3_t *callbacks,
             void *callback_baton,
             svn_depth_t depth,
             svn_boolean_t ignore_ancestry,
             const apr_array_header_t *changelists,
             apr_pool_t *pool)
{
  struct diff_callbacks3_wrapper_baton *b = apr_palloc(pool, sizeof(*b));
  b->callbacks3 = callbacks;
  b->baton = callback_baton;

  return svn_wc_diff6(anchor, target, &diff_callbacks3_wrapper, b,
                      depth, ignore_ancestry, changelists, NULL, pool);
}

svn_error_t *
svn_wc_diff4(svn_wc_adm_access_t *anchor,
             const char *target,
             const svn_wc_diff_callbacks2_t *callbacks,
             void *callback_baton,
             svn_depth_t depth,
             svn_boolean_t ignore_ancestry,
             const apr_array_header_t *changelists,
             apr_pool_t *pool)
{
  struct diff_callbacks2_wrapper_baton *b = apr_palloc(pool, sizeof(*b));
  b->callbacks2 = callbacks;
  b->baton = callback_baton;

  return svn_wc_diff5(anchor, target, &diff_callbacks2_wrapper, b,
                      depth, ignore_ancestry, changelists, pool);
}

svn_error_t *
svn_wc_diff3(svn_wc_adm_access_t *anchor,
             const char *target,
             const svn_wc_diff_callbacks2_t *callbacks,
             void *callback_baton,
             svn_boolean_t recurse,
             svn_boolean_t ignore_ancestry,
             apr_pool_t *pool)
{
  return svn_wc_diff4(anchor, target, callbacks, callback_baton,
                      SVN_DEPTH_INFINITY_OR_FILES(recurse), ignore_ancestry,
                      NULL, pool);
}

svn_error_t *
svn_wc_diff2(svn_wc_adm_access_t *anchor,
             const char *target,
             const svn_wc_diff_callbacks_t *callbacks,
             void *callback_baton,
             svn_boolean_t recurse,
             svn_boolean_t ignore_ancestry,
             apr_pool_t *pool)
{
  struct diff_callbacks_wrapper_baton *b = apr_pcalloc(pool, sizeof(*b));
  b->callbacks = callbacks;
  b->baton = callback_baton;
  return svn_wc_diff5(anchor, target, &diff_callbacks_wrapper, b,
                      SVN_DEPTH_INFINITY_OR_FILES(recurse), ignore_ancestry,
                      NULL, pool);
}

svn_error_t *
svn_wc_diff(svn_wc_adm_access_t *anchor,
            const char *target,
            const svn_wc_diff_callbacks_t *callbacks,
            void *callback_baton,
            svn_boolean_t recurse,
            apr_pool_t *pool)
{
  return svn_wc_diff2(anchor, target, callbacks, callback_baton,
                      recurse, FALSE, pool);
}

/*** From entries.c ***/
svn_error_t *
svn_wc_walk_entries2(const char *path,
                     svn_wc_adm_access_t *adm_access,
                     const svn_wc_entry_callbacks_t *walk_callbacks,
                     void *walk_baton,
                     svn_boolean_t show_hidden,
                     svn_cancel_func_t cancel_func,
                     void *cancel_baton,
                     apr_pool_t *pool)
{
  svn_wc_entry_callbacks2_t walk_cb2 = { 0 };
  walk_cb2.found_entry = walk_callbacks->found_entry;
  walk_cb2.handle_error = svn_wc__walker_default_error_handler;
  return svn_wc_walk_entries3(path, adm_access,
                              &walk_cb2, walk_baton, svn_depth_infinity,
                              show_hidden, cancel_func, cancel_baton, pool);
}

svn_error_t *
svn_wc_walk_entries(const char *path,
                    svn_wc_adm_access_t *adm_access,
                    const svn_wc_entry_callbacks_t *walk_callbacks,
                    void *walk_baton,
                    svn_boolean_t show_hidden,
                    apr_pool_t *pool)
{
  return svn_wc_walk_entries2(path, adm_access, walk_callbacks,
                              walk_baton, show_hidden, NULL, NULL,
                              pool);
}

/*** From props.c ***/
svn_error_t *
svn_wc_parse_externals_description2(apr_array_header_t **externals_p,
                                    const char *parent_directory,
                                    const char *desc,
                                    apr_pool_t *pool)
{
  apr_array_header_t *list;
  apr_pool_t *subpool = svn_pool_create(pool);
  int i;

  SVN_ERR(svn_wc_parse_externals_description3(externals_p ? &list : NULL,
                                              parent_directory, desc,
                                              TRUE, subpool));

  if (externals_p)
    {
      *externals_p = apr_array_make(pool, list->nelts,
                                    sizeof(svn_wc_external_item_t *));
      for (i = 0; i < list->nelts; i++)
        {
          svn_wc_external_item2_t *item2 = APR_ARRAY_IDX(list, i,
                                             svn_wc_external_item2_t *);
          svn_wc_external_item_t *item = apr_palloc(pool, sizeof (*item));

          if (item2->target_dir)
            item->target_dir = apr_pstrdup(pool, item2->target_dir);
          if (item2->url)
            item->url = apr_pstrdup(pool, item2->url);
          item->revision = item2->revision;

          APR_ARRAY_PUSH(*externals_p, svn_wc_external_item_t *) = item;
        }
    }

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_parse_externals_description(apr_hash_t **externals_p,
                                   const char *parent_directory,
                                   const char *desc,
                                   apr_pool_t *pool)
{
  apr_array_header_t *list;
  int i;

  SVN_ERR(svn_wc_parse_externals_description2(externals_p ? &list : NULL,
                                              parent_directory, desc, pool));

  /* Store all of the items into the hash if that was requested. */
  if (externals_p)
    {
      *externals_p = apr_hash_make(pool);
      for (i = 0; i < list->nelts; i++)
        {
          svn_wc_external_item_t *item;
          item = APR_ARRAY_IDX(list, i, svn_wc_external_item_t *);

          apr_hash_set(*externals_p, item->target_dir,
                       APR_HASH_KEY_STRING, item);
        }
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_prop_set3(const char *name,
                 const svn_string_t *value,
                 const char *path,
                 svn_wc_adm_access_t *adm_access,
                 svn_boolean_t skip_checks,
                 svn_wc_notify_func2_t notify_func,
                 void *notify_baton,
                 apr_pool_t *pool)
{
  svn_wc_context_t *wc_ctx;
  const char *local_abspath;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access), pool));

  SVN_ERR(svn_wc_prop_set4(wc_ctx, local_abspath, name, value, skip_checks,
                           notify_func, notify_baton, pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}

svn_error_t *
svn_wc_prop_set2(const char *name,
                 const svn_string_t *value,
                 const char *path,
                 svn_wc_adm_access_t *adm_access,
                 svn_boolean_t skip_checks,
                 apr_pool_t *pool)
{
  return svn_wc_prop_set3(name, value, path, adm_access, skip_checks,
                          NULL, NULL, pool);
}

svn_error_t *
svn_wc_prop_set(const char *name,
                const svn_string_t *value,
                const char *path,
                svn_wc_adm_access_t *adm_access,
                apr_pool_t *pool)
{
  return svn_wc_prop_set2(name, value, path, adm_access, FALSE, pool);
}

svn_error_t *
svn_wc_prop_list(apr_hash_t **props,
                 const char *path,
                 svn_wc_adm_access_t *adm_access,
                 apr_pool_t *pool)
{
  svn_wc_context_t *wc_ctx;
  const char *local_abspath;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access), pool));

  SVN_ERR(svn_wc_prop_list2(props, wc_ctx, local_abspath, pool, pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}

svn_error_t *
svn_wc_prop_get(const svn_string_t **value,
                const char *name,
                const char *path,
                svn_wc_adm_access_t *adm_access,
                apr_pool_t *pool)
{

  svn_wc_context_t *wc_ctx;
  const char *local_abspath;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access), pool));

  SVN_ERR(svn_wc_prop_get2(value, wc_ctx, local_abspath, name, pool, pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}

svn_error_t *
svn_wc_merge_props(svn_wc_notify_state_t *state,
                   const char *path,
                   svn_wc_adm_access_t *adm_access,
                   apr_hash_t *baseprops,
                   const apr_array_header_t *propchanges,
                   svn_boolean_t base_merge,
                   svn_boolean_t dry_run,
                   apr_pool_t *pool)
{
  return svn_wc_merge_props2(state, path, adm_access, baseprops, propchanges,
                             base_merge, dry_run, NULL, NULL, pool);
}


svn_error_t *
svn_wc_merge_prop_diffs(svn_wc_notify_state_t *state,
                        const char *path,
                        svn_wc_adm_access_t *adm_access,
                        const apr_array_header_t *propchanges,
                        svn_boolean_t base_merge,
                        svn_boolean_t dry_run,
                        apr_pool_t *pool)
{
  /* NOTE: Here, we use implementation knowledge.  The public
     svn_wc_merge_props2 doesn't allow NULL as baseprops argument, but we know
     that it works. */
  return svn_wc_merge_props2(state, path, adm_access, NULL, propchanges,
                             base_merge, dry_run, NULL, NULL, pool);
}

svn_error_t *
svn_wc_get_prop_diffs(apr_array_header_t **propchanges,
                      apr_hash_t **original_props,
                      const char *path,
                      svn_wc_adm_access_t *adm_access,
                      apr_pool_t *pool)
{
  svn_wc_context_t *wc_ctx;
  const char *local_abspath;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access), pool));

  SVN_ERR(svn_wc_get_prop_diffs2(propchanges, original_props, wc_ctx,
                                 local_abspath, pool, pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}


/*** From status.c ***/

struct status4_wrapper_baton
{
  svn_wc_status_func3_t old_func;
  void *old_baton;
};

static svn_error_t *
status4_wrapper_func(void *baton,
                     const char *path,
                     const svn_wc_status2_t *status,
                     apr_pool_t *scratch_pool)
{
  struct status4_wrapper_baton *swb = baton;
  svn_wc_status2_t *dup = svn_wc_dup_status2(status, scratch_pool);

  return (*swb->old_func)(swb->old_baton, path, dup, scratch_pool);
}

svn_error_t *
svn_wc_get_status_editor4(const svn_delta_editor_t **editor,
                          void **edit_baton,
                          void **set_locks_baton,
                          svn_revnum_t *edit_revision,
                          svn_wc_adm_access_t *anchor,
                          const char *target,
                          svn_depth_t depth,
                          svn_boolean_t get_all,
                          svn_boolean_t no_ignore,
                          const apr_array_header_t *ignore_patterns,
                          svn_wc_status_func3_t status_func,
                          void *status_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          svn_wc_traversal_info_t *traversal_info,
                          apr_pool_t *pool)
{
  struct status4_wrapper_baton *swb = apr_palloc(pool, sizeof(*swb));
  svn_wc_context_t *wc_ctx;
  apr_pool_t *scratch_pool = svn_pool_create(pool);
  svn_error_t *err;

  swb->old_func = status_func;
  swb->old_baton = status_baton;

  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(anchor),
                                         scratch_pool));

  err = svn_wc_get_status_editor5(editor, edit_baton, set_locks_baton,
                                  edit_revision, wc_ctx, anchor, target,
                                  depth, get_all, no_ignore, ignore_patterns,
                                  status4_wrapper_func, swb,
                                  cancel_func, cancel_baton, traversal_info,
                                  pool, scratch_pool);

  /* This destroys the context also. */
  svn_pool_destroy(scratch_pool);
  return err;
}

struct status_editor3_compat_baton
{
  svn_wc_status_func2_t old_func;
  void *old_baton;
};

static svn_error_t *
status_editor3_compat_func(void *baton,
                           const char *path,
                           svn_wc_status2_t *status,
                           apr_pool_t *pool)
{
  struct status_editor3_compat_baton *secb = baton;

  secb->old_func(secb->old_baton, path, status);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_get_status_editor3(const svn_delta_editor_t **editor,
                          void **edit_baton,
                          void **set_locks_baton,
                          svn_revnum_t *edit_revision,
                          svn_wc_adm_access_t *anchor,
                          const char *target,
                          svn_depth_t depth,
                          svn_boolean_t get_all,
                          svn_boolean_t no_ignore,
                          apr_array_header_t *ignore_patterns,
                          svn_wc_status_func2_t status_func,
                          void *status_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          svn_wc_traversal_info_t *traversal_info,
                          apr_pool_t *pool)
{
  struct status_editor3_compat_baton *secb = apr_palloc(pool, sizeof(*secb));
  secb->old_func = status_func;
  secb->old_baton = status_baton;

  return svn_wc_get_status_editor4(editor, edit_baton, set_locks_baton,
                                   edit_revision, anchor, target, depth,
                                   get_all, no_ignore, ignore_patterns,
                                   status_editor3_compat_func, secb,
                                   cancel_func, cancel_baton, traversal_info,
                                   pool);
}

svn_error_t *
svn_wc_get_status_editor2(const svn_delta_editor_t **editor,
                          void **edit_baton,
                          void **set_locks_baton,
                          svn_revnum_t *edit_revision,
                          svn_wc_adm_access_t *anchor,
                          const char *target,
                          apr_hash_t *config,
                          svn_boolean_t recurse,
                          svn_boolean_t get_all,
                          svn_boolean_t no_ignore,
                          svn_wc_status_func2_t status_func,
                          void *status_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          svn_wc_traversal_info_t *traversal_info,
                          apr_pool_t *pool)
{
  apr_array_header_t *ignores;
  SVN_ERR(svn_wc_get_default_ignores(&ignores, config, pool));
  return svn_wc_get_status_editor3(editor,
                                   edit_baton,
                                   set_locks_baton,
                                   edit_revision,
                                   anchor,
                                   target,
                                   SVN_DEPTH_INFINITY_OR_IMMEDIATES(recurse),
                                   get_all,
                                   no_ignore,
                                   ignores,
                                   status_func,
                                   status_baton,
                                   cancel_func,
                                   cancel_baton,
                                   traversal_info,
                                   pool);
}


/* Helpers for deprecated svn_wc_status_editor(), of type
   svn_wc_status_func2_t. */
struct old_status_func_cb_baton
{
  svn_wc_status_func_t original_func;
  void *original_baton;
};

static void old_status_func_cb(void *baton,
                               const char *path,
                               svn_wc_status2_t *status)
{
  struct old_status_func_cb_baton *b = baton;
  svn_wc_status_t *stat = (svn_wc_status_t *) status;

  b->original_func(b->original_baton, path, stat);
}

svn_error_t *
svn_wc_get_status_editor(const svn_delta_editor_t **editor,
                         void **edit_baton,
                         svn_revnum_t *edit_revision,
                         svn_wc_adm_access_t *anchor,
                         const char *target,
                         apr_hash_t *config,
                         svn_boolean_t recurse,
                         svn_boolean_t get_all,
                         svn_boolean_t no_ignore,
                         svn_wc_status_func_t status_func,
                         void *status_baton,
                         svn_cancel_func_t cancel_func,
                         void *cancel_baton,
                         svn_wc_traversal_info_t *traversal_info,
                         apr_pool_t *pool)
{
  struct old_status_func_cb_baton *b = apr_pcalloc(pool, sizeof(*b));
  apr_array_header_t *ignores;
  b->original_func = status_func;
  b->original_baton = status_baton;
  SVN_ERR(svn_wc_get_default_ignores(&ignores, config, pool));
  return svn_wc_get_status_editor3(editor, edit_baton, NULL, edit_revision,
                                   anchor, target,
                                   SVN_DEPTH_INFINITY_OR_IMMEDIATES(recurse),
                                   get_all, no_ignore, ignores,
                                   old_status_func_cb, b,
                                   cancel_func, cancel_baton,
                                   traversal_info, pool);
}

svn_error_t *
svn_wc_status(svn_wc_status_t **status,
              const char *path,
              svn_wc_adm_access_t *adm_access,
              apr_pool_t *pool)
{
  svn_wc_status2_t *stat2;

  SVN_ERR(svn_wc_status2(&stat2, path, adm_access, pool));
  *status = (svn_wc_status_t *) stat2;
  return SVN_NO_ERROR;
}

svn_wc_status_t *
svn_wc_dup_status(const svn_wc_status_t *orig_stat,
                  apr_pool_t *pool)
{
  svn_wc_status_t *new_stat = apr_palloc(pool, sizeof(*new_stat));

  /* Shallow copy all members. */
  *new_stat = *orig_stat;

  /* Now go back and dup the deep item into this pool. */
  if (orig_stat->entry)
    new_stat->entry = svn_wc_entry_dup(orig_stat->entry, pool);

  /* Return the new hotness. */
  return new_stat;
}

svn_error_t *
svn_wc_get_ignores(apr_array_header_t **patterns,
                   apr_hash_t *config,
                   svn_wc_adm_access_t *adm_access,
                   apr_pool_t *pool)
{
  svn_wc_context_t *wc_ctx;
  const char *local_abspath;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath,
                                  svn_wc_adm_access_path(adm_access), pool));

  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access),
                                         pool));

  SVN_ERR(svn_wc_get_ignores2(patterns, wc_ctx, local_abspath, config, pool,
                              pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}

svn_error_t *
svn_wc_status2(svn_wc_status2_t **status,
               const char *path,
               svn_wc_adm_access_t *adm_access,
               apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc_context_t *wc_ctx;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access),
                                         pool));

  SVN_ERR(svn_wc_status3(status, wc_ctx, local_abspath, pool, pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}


/*** From update_editor.c ***/
svn_error_t *
svn_wc_add_repos_file2(const char *dst_path,
                       svn_wc_adm_access_t *adm_access,
                       const char *new_text_base_path,
                       const char *new_text_path,
                       apr_hash_t *new_base_props,
                       apr_hash_t *new_props,
                       const char *copyfrom_url,
                       svn_revnum_t copyfrom_rev,
                       apr_pool_t *pool)
{
  svn_stream_t *new_base_contents;
  svn_stream_t *new_contents = NULL;

  SVN_ERR(svn_stream_open_readonly(&new_base_contents, new_text_base_path,
                                   pool, pool));

  if (new_text_path)
    {
      /* NOTE: the specified path may *not* be under version control.
         It is most likely sitting in .svn/tmp/. Thus, we cannot use the
         typical WC functions to access "special", "keywords" or "EOL"
         information. We need to look at the properties given to us. */

      /* If the new file is special, then we can simply open the given
         contents since it is already in normal form. */
      if (apr_hash_get(new_props,
                       SVN_PROP_SPECIAL, APR_HASH_KEY_STRING) != NULL)
        {
          SVN_ERR(svn_stream_open_readonly(&new_contents, new_text_path,
                                           pool, pool));
        }
      else
        {
          /* The new text contents need to be detrans'd into normal form. */
          svn_subst_eol_style_t eol_style;
          const char *eol_str;
          apr_hash_t *keywords = NULL;
          svn_string_t *list;

          list = apr_hash_get(new_props,
                              SVN_PROP_KEYWORDS, APR_HASH_KEY_STRING);
          if (list != NULL)
            {
              /* Since we are detranslating, all of the keyword values
                 can be "". */
              SVN_ERR(svn_subst_build_keywords2(&keywords,
                                                list->data,
                                                "", "", 0, "",
                                                pool));
              if (apr_hash_count(keywords) == 0)
                keywords = NULL;
            }

          svn_subst_eol_style_from_value(&eol_style, &eol_str,
                                         apr_hash_get(new_props,
                                                      SVN_PROP_EOL_STYLE,
                                                      APR_HASH_KEY_STRING));

          if (svn_subst_translation_required(eol_style, eol_str, keywords,
                                             FALSE, FALSE))
            {
              SVN_ERR(svn_subst_stream_detranslated(&new_contents,
                                                    new_text_path,
                                                    eol_style, eol_str,
                                                    FALSE,
                                                    keywords,
                                                    FALSE,
                                                    pool));
            }
          else
            {
              SVN_ERR(svn_stream_open_readonly(&new_contents, new_text_path,
                                               pool, pool));
            }
        }
    }

  SVN_ERR(svn_wc_add_repos_file3(dst_path, adm_access,
                                 new_base_contents, new_contents,
                                 new_base_props, new_props,
                                 copyfrom_url, copyfrom_rev,
                                 NULL, NULL, NULL, NULL,
                                 pool));

  /* The API contract states that the text files will be removed upon
     successful completion. add_repos_file3() does not remove the files
     since it only has streams on them. Toss 'em now. */
  svn_error_clear(svn_io_remove_file(new_text_base_path, pool));
  if (new_text_path)
    svn_error_clear(svn_io_remove_file(new_text_path, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_add_repos_file(const char *dst_path,
                      svn_wc_adm_access_t *adm_access,
                      const char *new_text_path,
                      apr_hash_t *new_props,
                      const char *copyfrom_url,
                      svn_revnum_t copyfrom_rev,
                      apr_pool_t *pool)
{
  return svn_wc_add_repos_file2(dst_path, adm_access,
                                new_text_path, NULL,
                                new_props, NULL,
                                copyfrom_url, copyfrom_rev,
                                pool);
}

/*** From lock.c ***/

/* To preserve API compatibility with Subversion 1.0.0 */
svn_error_t *
svn_wc_adm_open(svn_wc_adm_access_t **adm_access,
                svn_wc_adm_access_t *associated,
                const char *path,
                svn_boolean_t write_lock,
                svn_boolean_t tree_lock,
                apr_pool_t *pool)
{
  return svn_wc_adm_open3(adm_access, associated, path, write_lock,
                          (tree_lock ? -1 : 0), NULL, NULL, pool);
}

svn_error_t *
svn_wc_adm_open2(svn_wc_adm_access_t **adm_access,
                 svn_wc_adm_access_t *associated,
                 const char *path,
                 svn_boolean_t write_lock,
                 int levels_to_lock,
                 apr_pool_t *pool)
{
  return svn_wc_adm_open3(adm_access, associated, path, write_lock,
                          levels_to_lock, NULL, NULL, pool);
}

svn_error_t *
svn_wc_adm_probe_open(svn_wc_adm_access_t **adm_access,
                      svn_wc_adm_access_t *associated,
                      const char *path,
                      svn_boolean_t write_lock,
                      svn_boolean_t tree_lock,
                      apr_pool_t *pool)
{
  return svn_wc_adm_probe_open3(adm_access, associated, path,
                                write_lock, (tree_lock ? -1 : 0),
                                NULL, NULL, pool);
}


svn_error_t *
svn_wc_adm_probe_open2(svn_wc_adm_access_t **adm_access,
                       svn_wc_adm_access_t *associated,
                       const char *path,
                       svn_boolean_t write_lock,
                       int levels_to_lock,
                       apr_pool_t *pool)
{
  return svn_wc_adm_probe_open3(adm_access, associated, path, write_lock,
                                levels_to_lock, NULL, NULL, pool);
}

svn_error_t *
svn_wc_adm_probe_try(svn_wc_adm_access_t **adm_access,
                     svn_wc_adm_access_t *associated,
                     const char *path,
                     svn_boolean_t write_lock,
                     svn_boolean_t tree_lock,
                     apr_pool_t *pool)
{
  return svn_wc_adm_probe_try3(adm_access, associated, path, write_lock,
                               (tree_lock ? -1 : 0), NULL, NULL, pool);
}

svn_error_t *
svn_wc_adm_close(svn_wc_adm_access_t *adm_access)
{
  /* This is the only pool we have access to. */
  apr_pool_t *scratch_pool = svn_wc_adm_access_pool(adm_access);

  return svn_wc_adm_close2(adm_access, scratch_pool);
}


/*** From translate.c ***/

svn_error_t *
svn_wc_translated_file(const char **xlated_p,
                       const char *vfile,
                       svn_wc_adm_access_t *adm_access,
                       svn_boolean_t force_repair,
                       apr_pool_t *pool)
{
  return svn_wc_translated_file2(xlated_p, vfile, vfile, adm_access,
                                 SVN_WC_TRANSLATE_TO_NF
                                 | (force_repair ?
                                    SVN_WC_TRANSLATE_FORCE_EOL_REPAIR : 0),
                                 pool);
}

svn_error_t *
svn_wc_translated_stream(svn_stream_t **stream,
                         const char *path,
                         const char *versioned_file,
                         svn_wc_adm_access_t *adm_access,
                         apr_uint32_t flags,
                         apr_pool_t *pool)
{
  const char *local_abspath;
  const char *versioned_abspath;
  svn_wc_context_t *wc_ctx;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_dirent_get_absolute(&versioned_abspath, versioned_file, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access),
                                         pool));

  SVN_ERR(svn_wc_translated_stream2(stream, wc_ctx, local_abspath,
                                    versioned_abspath, flags,
                                    pool, pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}

svn_error_t *
svn_wc_translated_file2(const char **xlated_path,
                        const char *src,
                        const char *versioned_file,
                        svn_wc_adm_access_t *adm_access,
                        apr_uint32_t flags,
                        apr_pool_t *pool)
{
  const char *versioned_abspath;
  const char *root;
  const char *tmp_root;
  svn_wc_context_t *wc_ctx;

  SVN_ERR(svn_dirent_get_absolute(&versioned_abspath, versioned_file, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL,
                                         svn_wc__adm_get_db(adm_access),
                                         pool));

  SVN_ERR(svn_wc_translated_file3(xlated_path, src, wc_ctx, versioned_abspath, 
                                  flags, pool, pool));
  if (! svn_dirent_is_absolute(versioned_file))
    {
      SVN_ERR(svn_io_temp_dir(&tmp_root, pool));
      if (! svn_dirent_is_child(tmp_root, *xlated_path, pool))
        {
          SVN_ERR(svn_dirent_get_absolute(&root, "", pool));
          *xlated_path = svn_dirent_is_child(root, *xlated_path, pool);
        }
    }

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}

/*** From relocate.c ***/
svn_error_t *
svn_wc_relocate3(const char *path,
                 svn_wc_adm_access_t *adm_access,
                 const char *from,
                 const char *to,
                 svn_boolean_t recurse,
                 svn_wc_relocation_validator3_t validator,
                 void *validator_baton,
                 apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc_context_t *wc_ctx;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access),
                                         pool));

  SVN_ERR(svn_wc_relocate4(wc_ctx, local_abspath, from, to, recurse,
                           validator, validator_baton, pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}

/* Compatibility baton and wrapper. */
struct compat2_baton {
  svn_wc_relocation_validator2_t validator;
  void *baton;
};

/* Compatibility baton and wrapper. */
struct compat_baton {
  svn_wc_relocation_validator_t validator;
  void *baton;
};

/* This implements svn_wc_relocate_validator3_t. */
static svn_error_t *
compat2_validator(void *baton,
                  const char *uuid,
                  const char *url,
                  const char *root_url,
                  apr_pool_t *pool)
{
  struct compat2_baton *cb = baton;
  /* The old callback type doesn't set root_url. */
  return cb->validator(cb->baton, uuid,
                       (root_url ? root_url : url), (root_url != NULL),
                       pool);
}

/* This implements svn_wc_relocate_validator3_t. */
static svn_error_t *
compat_validator(void *baton,
                 const char *uuid,
                 const char *url,
                 const char *root_url,
                 apr_pool_t *pool)
{
  struct compat_baton *cb = baton;
  /* The old callback type doesn't allow uuid to be NULL. */
  if (uuid)
    return cb->validator(cb->baton, uuid, url);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_relocate2(const char *path,
                 svn_wc_adm_access_t *adm_access,
                 const char *from,
                 const char *to,
                 svn_boolean_t recurse,
                 svn_wc_relocation_validator2_t validator,
                 void *validator_baton,
                 apr_pool_t *pool)
{
  struct compat2_baton cb;

  cb.validator = validator;
  cb.baton = validator_baton;

  return svn_wc_relocate3(path, adm_access, from, to, recurse,
                          compat2_validator, &cb, pool);
}

svn_error_t *
svn_wc_relocate(const char *path,
                svn_wc_adm_access_t *adm_access,
                const char *from,
                const char *to,
                svn_boolean_t recurse,
                svn_wc_relocation_validator_t validator,
                void *validator_baton,
                apr_pool_t *pool)
{
  struct compat_baton cb;

  cb.validator = validator;
  cb.baton = validator_baton;

  return svn_wc_relocate3(path, adm_access, from, to, recurse,
                          compat_validator, &cb, pool);
}


/*** From log.c ***/

svn_error_t *
svn_wc_cleanup2(const char *path,
                const char *diff3_cmd,
                svn_cancel_func_t cancel_func,
                void *cancel_baton,
                apr_pool_t *pool)
{
  svn_wc_context_t *wc_ctx;
  const char *local_abspath;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc_context_create(&wc_ctx, NULL, pool, pool));

  SVN_ERR(svn_wc_cleanup3(wc_ctx, local_abspath, cancel_func, 
                          cancel_baton, pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}

svn_error_t *
svn_wc_cleanup(const char *path,
               svn_wc_adm_access_t *optional_adm_access,
               const char *diff3_cmd,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               apr_pool_t *pool)
{
  return svn_wc_cleanup2(path, diff3_cmd, cancel_func, cancel_baton, pool);
}

/*** From questions.c ***/

svn_error_t *
svn_wc_has_binary_prop(svn_boolean_t *has_binary_prop,
                       const char *path,
                       svn_wc_adm_access_t *adm_access,
                       apr_pool_t *pool)
{
  svn_wc__db_t *db = svn_wc__adm_get_db(adm_access);
  const char *local_abspath;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));

  return svn_error_return(svn_wc__marked_as_binary(has_binary_prop,
                                                   local_abspath, db, pool));
}

svn_error_t *
svn_wc_conflicted_p2(svn_boolean_t *text_conflicted_p,
                     svn_boolean_t *prop_conflicted_p,
                     svn_boolean_t *tree_conflicted_p,
                     const char *path,
                     svn_wc_adm_access_t *adm_access,
                     apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc_context_t *wc_ctx;
  svn_error_t *err;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  SVN_ERR(svn_wc__context_create_with_db(&wc_ctx, NULL /* config */,
                                         svn_wc__adm_get_db(adm_access),
                                         pool));

  err = svn_wc_conflicted_p3(text_conflicted_p, prop_conflicted_p,
                             tree_conflicted_p, wc_ctx, local_abspath, pool);

  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      svn_error_clear(err);

      if (text_conflicted_p)
        *text_conflicted_p = FALSE;
      if (prop_conflicted_p)
        *prop_conflicted_p = FALSE;
      if (tree_conflicted_p)
        *tree_conflicted_p = FALSE;
    }
  else if (err)
    return err;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_conflicted_p(svn_boolean_t *text_conflicted_p,
                    svn_boolean_t *prop_conflicted_p,
                    const char *dir_path,
                    const svn_wc_entry_t *entry,
                    apr_pool_t *pool)
{
  svn_node_kind_t kind;
  const char *path;

  *text_conflicted_p = FALSE;
  *prop_conflicted_p = FALSE;

  if (entry->conflict_old)
    {
      path = svn_dirent_join(dir_path, entry->conflict_old, pool);
      SVN_ERR(svn_io_check_path(path, &kind, pool));
      *text_conflicted_p = (kind == svn_node_file);
    }

  if ((! *text_conflicted_p) && (entry->conflict_new))
    {
      path = svn_dirent_join(dir_path, entry->conflict_new, pool);
      SVN_ERR(svn_io_check_path(path, &kind, pool));
      *text_conflicted_p = (kind == svn_node_file);
    }

  if ((! *text_conflicted_p) && (entry->conflict_wrk))
    {
      path = svn_dirent_join(dir_path, entry->conflict_wrk, pool);
      SVN_ERR(svn_io_check_path(path, &kind, pool));
      *text_conflicted_p = (kind == svn_node_file);
    }

  if (entry->prejfile)
    {
      path = svn_dirent_join(dir_path, entry->prejfile, pool);
      SVN_ERR(svn_io_check_path(path, &kind, pool));
      *prop_conflicted_p = (kind == svn_node_file);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_text_modified_p(svn_boolean_t *modified_p,
                       const char *filename,
                       svn_boolean_t force_comparison,
                       svn_wc_adm_access_t *adm_access,
                       apr_pool_t *pool)
{
  svn_wc_context_t *wc_ctx;
  const char *local_abspath;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, filename, pool));
  SVN_ERR(svn_wc_context_create(&wc_ctx, NULL /* config */, pool, pool));

  SVN_ERR(svn_wc_text_modified_p2(modified_p, wc_ctx, local_abspath,
                                  force_comparison, pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}


/*** From copy.c ***/

svn_error_t *
svn_wc_copy(const char *src_path,
            svn_wc_adm_access_t *dst_parent,
            const char *dst_basename,
            svn_cancel_func_t cancel_func,
            void *cancel_baton,
            svn_wc_notify_func_t notify_func,
            void *notify_baton,
            apr_pool_t *pool)
{
  svn_wc__compat_notify_baton_t nb;

  nb.func = notify_func;
  nb.baton = notify_baton;

  return svn_wc_copy2(src_path, dst_parent, dst_basename, cancel_func,
                      cancel_baton, svn_wc__compat_call_notify_func,
                      &nb, pool);
}


/*** From merge.c ***/

svn_error_t *
svn_wc_merge2(enum svn_wc_merge_outcome_t *merge_outcome,
              const char *left,
              const char *right,
              const char *merge_target,
              svn_wc_adm_access_t *adm_access,
              const char *left_label,
              const char *right_label,
              const char *target_label,
              svn_boolean_t dry_run,
              const char *diff3_cmd,
              const apr_array_header_t *merge_options,
              apr_pool_t *pool)
{
  return svn_wc_merge3(merge_outcome,
                       left, right, merge_target, adm_access,
                       left_label, right_label, target_label,
                       dry_run, diff3_cmd, merge_options, NULL,
                       NULL, NULL, pool);
}

svn_error_t *
svn_wc_merge(const char *left,
             const char *right,
             const char *merge_target,
             svn_wc_adm_access_t *adm_access,
             const char *left_label,
             const char *right_label,
             const char *target_label,
             svn_boolean_t dry_run,
             enum svn_wc_merge_outcome_t *merge_outcome,
             const char *diff3_cmd,
             apr_pool_t *pool)
{
  return svn_wc_merge3(merge_outcome,
                       left, right, merge_target, adm_access,
                       left_label, right_label, target_label,
                       dry_run, diff3_cmd, NULL, NULL, NULL,
                       NULL, pool);
}


/*** From util.c ***/

svn_wc_conflict_description_t *
svn_wc_conflict_description_create_text(const char *path,
                                        svn_wc_adm_access_t *adm_access,
                                        apr_pool_t *pool)
{
  svn_wc_conflict_description_t *conflict;

  conflict = apr_pcalloc(pool, sizeof(*conflict));
  conflict->path = path;
  conflict->node_kind = svn_node_file;
  conflict->kind = svn_wc_conflict_kind_text;
  conflict->access = adm_access;
  conflict->action = svn_wc_conflict_action_edit;
  conflict->reason = svn_wc_conflict_reason_edited;
  return conflict;
}

svn_wc_conflict_description_t *
svn_wc_conflict_description_create_prop(const char *path,
                                        svn_wc_adm_access_t *adm_access,
                                        svn_node_kind_t node_kind,
                                        const char *property_name,
                                        apr_pool_t *pool)
{
  svn_wc_conflict_description_t *conflict;

  conflict = apr_pcalloc(pool, sizeof(*conflict));
  conflict->path = path;
  conflict->node_kind = node_kind;
  conflict->kind = svn_wc_conflict_kind_property;
  conflict->access = adm_access;
  conflict->property_name = property_name;
  return conflict;
}

svn_wc_conflict_description_t *
svn_wc_conflict_description_create_tree(
                            const char *path,
                            svn_wc_adm_access_t *adm_access,
                            svn_node_kind_t node_kind,
                            svn_wc_operation_t operation,
                            svn_wc_conflict_version_t *src_left_version,
                            svn_wc_conflict_version_t *src_right_version,
                            apr_pool_t *pool)
{
  svn_wc_conflict_description_t *conflict;

  conflict = apr_pcalloc(pool, sizeof(*conflict));
  conflict->path = path;
  conflict->node_kind = node_kind;
  conflict->kind = svn_wc_conflict_kind_tree;
  conflict->access = adm_access;
  conflict->operation = operation;
  conflict->src_left_version = src_left_version;
  conflict->src_right_version = src_right_version;
  return conflict;
}


/*** From revision_status.c ***/

svn_error_t *
svn_wc_revision_status(svn_wc_revision_status_t **result_p,
                       const char *wc_path,
                       const char *trail_url,
                       svn_boolean_t committed,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       apr_pool_t *pool)
{
  svn_wc_context_t *wc_ctx;
  const char *local_abspath;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, wc_path, pool));
  SVN_ERR(svn_wc_context_create(&wc_ctx, NULL /* config */, pool, pool));

  SVN_ERR(svn_wc_revision_status2(result_p, wc_ctx, local_abspath, trail_url,
                                  committed, cancel_func, cancel_baton, pool,
                                  pool));

  return svn_error_return(svn_wc_context_destroy(wc_ctx));
}
