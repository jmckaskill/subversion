/*
 * adm_crawler.c:  report local WC mods to an Editor.
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


#include <string.h>

#include <apr_pools.h>
#include <apr_file_io.h>
#include <apr_hash.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_wc.h"
#include "svn_io.h"
#include "svn_base64.h"
#include "svn_delta.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"

#include "private/svn_wc_private.h"

#include "wc.h"
#include "adm_files.h"
#include "props.h"
#include "translate.h"
#include "workqueue.h"
#include "conflicts.h"

#include "svn_private_config.h"


/* Helper for report_revisions_and_depths().

   Perform an atomic restoration of the file LOCAL_ABSPATH; that is, copy
   the file's text-base to the administrative tmp area, and then move
   that file to LOCAL_ABSPATH with possible translations/expansions.  If
   USE_COMMIT_TIMES is set, then set working file's timestamp to
   last-commit-time.  Either way, set entry-timestamp to match that of
   the working file when all is finished.

   Not that a valid access baton with a write lock to the directory of
   LOCAL_ABSPATH must be available in DB.*/
static svn_error_t *
restore_file(svn_wc__db_t *db,
             const char *local_abspath,
             svn_boolean_t use_commit_times,
             apr_pool_t *scratch_pool)
{
  svn_skel_t *work_item;

  SVN_ERR(svn_wc__wq_build_file_install(&work_item,
                                        db, local_abspath,
                                        NULL /* source_abspath */,
                                        use_commit_times,
                                        TRUE /* record_fileinfo */,
                                        scratch_pool, scratch_pool));
  /* ### we need an existing path for wq_add. not entirely WRI_ABSPATH yet  */
  SVN_ERR(svn_wc__db_wq_add(db,
                            svn_dirent_dirname(local_abspath, scratch_pool),
                            work_item, scratch_pool));

  /* Run the work item immediately.  */
  SVN_ERR(svn_wc__wq_run(db, local_abspath,
                         NULL, NULL, /* ### nice to have cancel_func/baton */
                         scratch_pool));

  /* Remove any text conflict */
  return svn_error_return(svn_wc__resolve_text_conflict(db, local_abspath,
                                                        scratch_pool));
}

/* Try to restore LOCAL_ABSPATH of node type KIND and if successfull,
   notify that the node is restored.  Use DB for accessing the working copy.
   If USE_COMMIT_TIMES is set, then set working file's timestamp to
   last-commit-time.

   Set RESTORED to TRUE if the node is successfull restored. RESTORED will
   be FALSE if restoring this node is not supported.

   This function does all temporary allocations in SCRATCH_POOL
 */
static svn_error_t *
restore_node(svn_boolean_t *restored,
             svn_wc__db_t *db,
             const char *local_abspath,
             svn_wc__db_kind_t kind,
             svn_boolean_t use_commit_times,
             svn_wc_notify_func2_t notify_func,
             void *notify_baton,
             apr_pool_t *scratch_pool)
{
  *restored = FALSE;

  /* Currently we can only restore files, but we will be able to restore
     directories after we move to a single database and pristine store. */
  if (kind == svn_wc__db_kind_file || kind == svn_wc__db_kind_symlink)
    {
      /* ... recreate file from text-base, and ... */
      SVN_ERR(restore_file(db, local_abspath, use_commit_times,
                           scratch_pool));

      *restored = TRUE;
      /* ... report the restoration to the caller.  */
      if (notify_func != NULL)
        {
          svn_wc_notify_t *notify = svn_wc_create_notify(
                                            local_abspath,
                                            svn_wc_notify_restore,
                                            scratch_pool);
          notify->kind = svn_node_file;
          (*notify_func)(notify_baton, notify, scratch_pool);
        }
    }

  return SVN_NO_ERROR;
}

/* Check if there is an externals definition stored on LOCAL_ABSPATH
   using DB.  In that case send the externals definition and DEPTH to
   EXTERNAL_FUNC.  Use SCRATCH_POOL for temporary allocations. */
static svn_error_t *
read_externals_info(svn_wc__db_t *db,
                    const char *local_abspath,
                    svn_wc_external_update_t external_func,
                    void *external_baton,
                    svn_depth_t depth,
                    apr_pool_t *scratch_pool)
{
  const svn_string_t *val;

  SVN_ERR_ASSERT(external_func != NULL);

  SVN_ERR(svn_wc__internal_propget(&val, db, local_abspath,
                                   SVN_PROP_EXTERNALS,
                                   scratch_pool, scratch_pool));

  if (val)
    {
      SVN_ERR((external_func)(external_baton, local_abspath, val, val, depth,
                              scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* The recursive crawler that describes a mixed-revision working
   copy to an RA layer.  Used to initiate updates.

   This is a depth-first recursive walk of DIR_PATH under ANCHOR_ABSPATH,
   using DB.  Look at each entry and check if its revision is different
   than DIR_REV.  If so, report this fact to REPORTER.  If an entry is
   missing from disk, report its absence to REPORTER.  If an entry has
   a different URL than expected, report that to REPORTER.  If an
   entry has a different depth than its parent, report that to
   REPORTER.

   Alternatively, if REPORT_EVERYTHING is set, then report all
   children unconditionally.

   DEPTH is actually the *requested* depth for the update-like
   operation for which we are reporting working copy state.  However,
   certain requested depths affect the depth of the report crawl.  For
   example, if the requested depth is svn_depth_empty, there's no
   point descending into subdirs, no matter what their depths.  So:

   If DEPTH is svn_depth_empty, don't report any files and don't
   descend into any subdirs.  If svn_depth_files, report files but
   still don't descend into subdirs.  If svn_depth_immediates, report
   files, and report subdirs themselves but not their entries.  If
   svn_depth_infinity or svn_depth_unknown, report everything all the
   way down.  (That last sentence might sound counterintuitive, but
   since you can't go deeper than the local ambient depth anyway,
   requesting svn_depth_infinity really means "as deep as the various
   parts of this working copy go".  Of course, the information that
   comes back from the server will be different for svn_depth_unknown
   than for svn_depth_infinity.)

   DEPTH_COMPATIBILITY_TRICK means the same thing here as it does
   in svn_wc_crawl_revisions3().

   If EXTERNAL_FUNC is non-NULL, then send externals information with
   the help of EXTERNAL_BATON

   If RESTORE_FILES is set, then unexpectedly missing working files
   will be restored from text-base and NOTIFY_FUNC/NOTIFY_BATON
   will be called to report the restoration.  USE_COMMIT_TIMES is
   passed to restore_file() helper. */
static svn_error_t *
report_revisions_and_depths(svn_wc__db_t *db,
                            const char *anchor_abspath,
                            const char *dir_path,
                            svn_revnum_t dir_rev,
                            const svn_ra_reporter3_t *reporter,
                            void *report_baton,
                            svn_wc_external_update_t external_func,
                            void *external_baton,
                            svn_wc_notify_func2_t notify_func,
                            void *notify_baton,
                            svn_boolean_t restore_files,
                            svn_depth_t depth,
                            svn_boolean_t honor_depth_exclude,
                            svn_boolean_t depth_compatibility_trick,
                            svn_boolean_t report_everything,
                            svn_boolean_t use_commit_times,
                            apr_pool_t *scratch_pool)
{
  const char *dir_abspath;
  const apr_array_header_t *base_children;
  apr_hash_t *dirents;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  int i;
  const char *dir_repos_root, *dir_repos_relpath;
  svn_depth_t dir_depth;


  /* Get both the SVN Entries and the actual on-disk entries.   Also
     notice that we're picking up hidden entries too (read_children never
     hides children). */
  dir_abspath = svn_dirent_join(anchor_abspath, dir_path, scratch_pool);
  SVN_ERR(svn_wc__db_base_get_children(&base_children, db, dir_abspath,
                                       scratch_pool, iterpool));
  SVN_ERR(svn_io_get_dirents3(&dirents, dir_abspath, TRUE,
                              scratch_pool, scratch_pool));

  /*** Do the real reporting and recursing. ***/

  /* First, look at "this dir" to see what its URL and depth are. */
  SVN_ERR(svn_wc__db_read_info(NULL, NULL, NULL, &dir_repos_relpath,
                               &dir_repos_root, NULL, NULL, NULL, NULL, NULL,
                               &dir_depth, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL,
                               db, dir_abspath,
                               scratch_pool, iterpool));

  /* If the directory has no url, search its parents */
  if (dir_repos_relpath == NULL)
    SVN_ERR(svn_wc__db_scan_base_repos(&dir_repos_relpath, &dir_repos_root,
                                       NULL, db, dir_abspath,
                                       scratch_pool, iterpool));

  /* If "this dir" has "svn:externals" property set on it,
   * call the external_func callback. */
  if (external_func)
    SVN_ERR(read_externals_info(db, dir_abspath, external_func,
                                external_baton, dir_depth, iterpool));

  /* Looping over current directory's BASE children: */
  for (i = 0; i < base_children->nelts; ++i)
    {
      const char *child = APR_ARRAY_IDX(base_children, i, const char *);
      const char *this_path, *this_abspath;
      const char *this_repos_root_url, *this_repos_relpath;
      svn_wc__db_status_t this_status;
      svn_wc__db_kind_t this_kind;
      svn_revnum_t this_rev;
      svn_depth_t this_depth;
      svn_wc__db_lock_t *this_lock;
      svn_boolean_t this_switched;
      svn_error_t *err;

      /* Clear the iteration subpool here because the loop has a bunch
         of 'continue' jump statements. */
      svn_pool_clear(iterpool);

      /* Compute the paths and URLs we need. */
      this_path = svn_dirent_join(dir_path, child, iterpool);
      this_abspath = svn_dirent_join(dir_abspath, child, iterpool);

      err = svn_wc__db_base_get_info(&this_status, &this_kind, &this_rev,
                                     &this_repos_relpath, &this_repos_root_url,
                                     NULL, NULL, NULL, NULL, NULL, &this_depth,
                                     NULL, NULL, NULL, &this_lock,
                                     db, this_abspath, iterpool, iterpool);
      if (err)
        {
          if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
            return svn_error_return(err);

          /* THIS_ABSPATH was listed as a BASE child of DIR_ABSPATH. Yet,
             we just got an error trying to read it. What gives? :-P

             This happens when THIS_ABSPATH is a subdirectory that is
             marked in the parent stub as "not-present". The subdir is
             then removed. Later, an addition is scheduled, putting the
             subdirectory back, but ONLY containing WORKING nodes.

             Thus, the BASE fetch comes out of the subdir, and fails.

             For this case, we go ahead and treat this as a simple
             not-present, and ignore whatever is in the subdirectory.  */
          svn_error_clear(err);

          this_status = svn_wc__db_status_not_present;

          /* Note: the other THIS_* local variables pass to base_get_info
             are NOT set at this point. But we don't need them...  */
        }

      /* Note: some older code would attempt to check the parent stub
         of subdirectories for the not-present state. That check was
         redundant since a not-present directory has no BASE nodes
         within it which may report another status.

         There might be NO BASE node (per the condition above), but the
         typical case is that base_get_info() reads the parent stub
         because there is no subdir (with administrative data). Thus, we
         already have all the information we need. No further testing.  */

      /* First check for exclusion */
      if (this_status == svn_wc__db_status_excluded)
        {
          if (honor_depth_exclude)
            {
              /* Report the excluded path, no matter whether report_everything
                 flag is set.  Because the report_everything flag indicates
                 that the server will treat the wc as empty and thus push
                 full content of the files/subdirs. But we want to prevent the
                 server from pushing the full content of this_path at us. */

              /* The server does not support link_path report on excluded
                 path. We explicitly prohibit this situation in
                 svn_wc_crop_tree(). */
              SVN_ERR(reporter->set_path(report_baton,
                                         this_path,
                                         dir_rev,
                                         svn_depth_exclude,
                                         FALSE,
                                         NULL,
                                         iterpool));
            }
          else
            {
              /* We want to pull in the excluded target. So, report it as
                 deleted, and server will respond properly. */
              if (! report_everything)
                SVN_ERR(reporter->delete_path(report_baton,
                                              this_path, iterpool));
            }
          continue;
        }

      /*** The Big Tests: ***/
      if (this_status == svn_wc__db_status_absent
          || this_status == svn_wc__db_status_not_present)
        {
          /* If the entry is 'absent' or 'not-present', make sure the server
             knows it's gone...
             ...unless we're reporting everything, in which case we're
             going to report it missing later anyway.

             This instructs the server to send it back to us, if it is
             now available (an addition after a not-present state), or if
             it is now authorized (change in authz for the absent item).  */
          if (! report_everything)
            SVN_ERR(reporter->delete_path(report_baton, this_path, iterpool));
          continue;
        }

      /* Is the entry NOT on the disk? We may be able to restore it.  */
      if (apr_hash_get(dirents, child, APR_HASH_KEY_STRING) == NULL)
        {
          svn_boolean_t missing = FALSE;
          svn_wc__db_status_t wrk_status;
          svn_wc__db_kind_t wrk_kind;

          SVN_ERR(svn_wc__db_read_info(&wrk_status, &wrk_kind, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL,
                                       db, this_abspath, iterpool, iterpool));

          if (restore_files && wrk_status != svn_wc__db_status_added
              && wrk_status != svn_wc__db_status_deleted
              && wrk_status != svn_wc__db_status_obstructed_add
              && wrk_status != svn_wc__db_status_obstructed_delete)
            {
              svn_node_kind_t dirent_kind;

              /* It is possible on a case insensitive system that the
                 entry is not really missing, but just cased incorrectly.
                 In this case we can't overwrite it with the pristine
                 version */
              SVN_ERR(svn_io_check_path(this_abspath, &dirent_kind, iterpool));

              if (dirent_kind == svn_node_none)
                {
                  svn_boolean_t restored;

                  SVN_ERR(restore_node(&restored, db, this_abspath, wrk_kind,
                                       use_commit_times, notify_func,
                                       notify_baton, iterpool));
                  if (!restored)
                    missing = TRUE;
                }
            }
          else
            missing = TRUE;

          /* If a node is still missing from disk here, we have no way to
             recreate it locally, so report as missing and move along.
             Again, don't bother if we're reporting everything, because the
             dir is already missing on the server. */
          if (missing && wrk_kind == svn_wc__db_kind_dir
               && (depth > svn_depth_files || depth == svn_depth_unknown))
            {
              if (! report_everything)
                SVN_ERR(reporter->delete_path(report_baton, this_path,
                                              iterpool));
              continue;
            }
        }

      /* And finally prepare for reporting */
      if (!this_repos_relpath)
        {
          this_switched = FALSE;
          this_repos_relpath = svn_relpath_join(dir_repos_relpath, child,
                                                iterpool);
        }
      else
        {
          const char *childname = svn_relpath_is_child(dir_repos_relpath,
                                                       this_repos_relpath,
                                                       NULL);

          if (childname == NULL || strcmp(childname, child) != 0)
            this_switched = TRUE;
          else
            this_switched = FALSE;
        }

      /* Tweak THIS_DEPTH to a useful value.  */
      if (this_depth == svn_depth_unknown)
        this_depth = svn_depth_infinity;

      /* Obstructed nodes might report SVN_INVALID_REVNUM. Tweak it.

         ### it seems that obstructed nodes should be handled quite a
         ### bit differently. maybe reported as missing, like not-present
         ### or absent nodes?  */
      if (!SVN_IS_VALID_REVNUM(this_rev))
        this_rev = dir_rev;

      /*** Files ***/
      if (this_kind == svn_wc__db_kind_file ||
          this_kind == svn_wc__db_kind_symlink)
        {
          if (report_everything)
            {
              /* Report the file unconditionally, one way or another. */
              if (this_switched)
                SVN_ERR(reporter->link_path(report_baton,
                                            this_path,
                                            svn_path_url_add_component2(
                                                dir_repos_root,
                                                this_repos_relpath, iterpool),
                                            this_rev,
                                            this_depth,
                                            FALSE,
                                            this_lock ? this_lock->token : NULL,
                                            iterpool));
              else
                SVN_ERR(reporter->set_path(report_baton,
                                           this_path,
                                           this_rev,
                                           this_depth,
                                           FALSE,
                                           this_lock ? this_lock->token : NULL,
                                           iterpool));
            }

          /* Possibly report a disjoint URL ... */
          else if (this_switched)
            SVN_ERR(reporter->link_path(report_baton,
                                        this_path,
                                        svn_path_url_add_component2(
                                                dir_repos_root,
                                                this_repos_relpath, iterpool),
                                        this_rev,
                                        this_depth,
                                        FALSE,
                                        this_lock ? this_lock->token : NULL,
                                        iterpool));
          /* ... or perhaps just a differing revision or lock token,
             or the mere presence of the file in a depth-empty dir. */
          else if (this_rev != dir_rev
                   || this_lock
                   || dir_depth == svn_depth_empty)
            SVN_ERR(reporter->set_path(report_baton,
                                       this_path,
                                       this_rev,
                                       this_depth,
                                       FALSE,
                                       this_lock ? this_lock->token : NULL,
                                       iterpool));
        } /* end file case */

      /*** Directories (in recursive mode) ***/
      else if (this_kind == svn_wc__db_kind_dir
               && (depth > svn_depth_files
                   || depth == svn_depth_unknown))
        {
          svn_boolean_t is_incomplete;
          svn_boolean_t start_empty;

          /* If the subdir and its administrative area are not present,
             then do NOT bother to report this node, much less recurse
             into the thing.

             Note: if the there is nothing on the disk, then we may have
             reported it missing further above.

             ### hmm. but what if we have a *file* obstructing the dir?
             ### the code above will not report it, and we'll simply
             ### skip it right here. I guess with an obstruction, we
             ### can't really do anything with info the server might
             ### send, so maybe this is just fine.  */
          if (this_status == svn_wc__db_status_obstructed)
            continue;

          is_incomplete = (this_status == svn_wc__db_status_incomplete);
          start_empty = is_incomplete;

          if (depth_compatibility_trick
              && this_depth <= svn_depth_files
              && depth > this_depth)
            {
              start_empty = TRUE;
            }

          if (report_everything)
            {
              /* Report the dir unconditionally, one way or another. */
              if (this_switched)
                SVN_ERR(reporter->link_path(report_baton,
                                            this_path,
                                            svn_path_url_add_component2(
                                                dir_repos_root,
                                                this_repos_relpath, iterpool),
                                            this_rev,
                                            this_depth,
                                            start_empty,
                                            this_lock ? this_lock->token
                                                      : NULL,
                                            iterpool));
              else
                SVN_ERR(reporter->set_path(report_baton,
                                           this_path,
                                           this_rev,
                                           this_depth,
                                           start_empty,
                                           this_lock ? this_lock->token : NULL,
                                           iterpool));
            }

          /* Possibly report a disjoint URL ... */
          else if (this_switched)
            SVN_ERR(reporter->link_path(report_baton,
                                        this_path,
                                        svn_path_url_add_component2(
                                                dir_repos_root,
                                                this_repos_relpath, iterpool),
                                        this_rev,
                                        this_depth,
                                        start_empty,
                                        this_lock ? this_lock->token : NULL,
                                        iterpool));
          /* ... or perhaps just a differing revision, lock token, incomplete
             subdir, the mere presence of the directory in a depth-empty or
             depth-files dir, or if the parent dir is at depth-immediates but
             the child is not at depth-empty.  Also describe shallow subdirs
             if we are trying to set depth to infinity. */
          else if (this_rev != dir_rev
                   || this_lock
                   || is_incomplete
                   || dir_depth == svn_depth_empty
                   || dir_depth == svn_depth_files
                   || (dir_depth == svn_depth_immediates
                       && this_depth != svn_depth_empty)
                   || (this_depth < svn_depth_infinity
                       && depth == svn_depth_infinity))
            SVN_ERR(reporter->set_path(report_baton,
                                       this_path,
                                       this_rev,
                                       this_depth,
                                       start_empty,
                                       this_lock ? this_lock->token : NULL,
                                       iterpool));

          if (SVN_DEPTH_IS_RECURSIVE(depth))
             SVN_ERR(report_revisions_and_depths(db,
                                                anchor_abspath,
                                                this_path,
                                                this_rev,
                                                reporter, report_baton,
                                                external_func, external_baton,
                                                notify_func, notify_baton,
                                                restore_files, depth,
                                                honor_depth_exclude,
                                                depth_compatibility_trick,
                                                start_empty,
                                                use_commit_times,
                                                iterpool));
        } /* end directory case */
    } /* end main entries loop */

  /* We're done examining this dir's entries, so free everything. */
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Helper for svn_wc_crawl_revisions5() that finds a base revision for a node
   that doesn't have one itself. */
static svn_error_t *
find_base_rev(svn_revnum_t *base_rev,
              svn_wc__db_t *db,
              const char *local_abspath,
              const char *top_local_abspath,
              apr_pool_t *pool)
{
  const char *op_root_abspath;
  svn_wc__db_status_t status;
  svn_boolean_t have_base;

  SVN_ERR(svn_wc__db_read_info(&status, NULL, base_rev, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL,
                               &have_base, NULL, NULL, NULL,
                               db, local_abspath, pool, pool));

  if (SVN_IS_VALID_REVNUM(*base_rev))
      return SVN_NO_ERROR;

  if (have_base)
    return svn_error_return(
        svn_wc__db_base_get_info(NULL, NULL, base_rev, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                 NULL,
                                 db, local_abspath, pool, pool));

  if (status == svn_wc__db_status_added)
    {
      SVN_ERR(svn_wc__db_scan_addition(NULL, &op_root_abspath, NULL, NULL,
                                       NULL, NULL, NULL, NULL,  NULL,
                                       db, local_abspath, pool, pool));

      return svn_error_return(
                 find_base_rev(base_rev,
                               db, svn_dirent_dirname(op_root_abspath, pool),
                               top_local_abspath,
                               pool));
    }
  else if (status == svn_wc__db_status_deleted)
    {
      const char *work_del_abspath;
       SVN_ERR(svn_wc__db_scan_deletion(NULL, NULL, NULL, &work_del_abspath,
                                       db, local_abspath, pool, pool));

      if (work_del_abspath != NULL)
        return svn_error_return(
                 find_base_rev(base_rev,
                               db, work_del_abspath,
                               top_local_abspath,
                               pool));
    }

  return svn_error_createf(SVN_ERR_WC_CORRUPT, NULL,
                           _("Can't retrieve base revision for %s"),
                           svn_dirent_local_style(top_local_abspath, pool));
}


/*------------------------------------------------------------------*/
/*** Public Interfaces ***/


svn_error_t *
svn_wc_crawl_revisions5(svn_wc_context_t *wc_ctx,
                        const char *local_abspath,
                        const svn_ra_reporter3_t *reporter,
                        void *report_baton,
                        svn_boolean_t restore_files,
                        svn_depth_t depth,
                        svn_boolean_t honor_depth_exclude,
                        svn_boolean_t depth_compatibility_trick,
                        svn_boolean_t use_commit_times,
                        svn_wc_external_update_t external_func,
                        void *external_baton,
                        svn_wc_notify_func2_t notify_func,
                        void *notify_baton,
                        apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db = wc_ctx->db;
  svn_error_t *fserr, *err;
  svn_revnum_t target_rev = SVN_INVALID_REVNUM;
  svn_boolean_t missing = FALSE;
  svn_boolean_t start_empty;
  svn_wc__db_status_t status;
  svn_wc__db_kind_t target_kind = svn_wc__db_kind_unknown;
  const char *repos_relpath=NULL, *repos_root=NULL;
  svn_depth_t target_depth = svn_depth_unknown;
  svn_wc__db_lock_t *target_lock = NULL;
  svn_boolean_t explicit_rev;
  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  /* The first thing we do is get the base_rev from the working copy's
     ROOT_DIRECTORY.  This is the first revnum that entries will be
     compared to. */
  err = svn_wc__db_base_get_info(&status, &target_kind, &target_rev,
                                 &repos_relpath, &repos_root,
                                 NULL, NULL, NULL, NULL, NULL,
                                 &target_depth, NULL, NULL, NULL,
                                 &target_lock,
                                 db, local_abspath, scratch_pool,
                                 scratch_pool);

  {
    svn_boolean_t has_base = TRUE;

    if (err)
      {
        if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
          return svn_error_return(err);

        svn_error_clear(err);
        has_base = FALSE;
        SVN_ERR(svn_wc__db_read_kind(&target_kind, db, local_abspath, TRUE,
                                     scratch_pool));

        if (target_kind == svn_wc__db_kind_file
            || target_kind == svn_wc__db_kind_symlink)
          status = svn_wc__db_status_absent; /* Crawl via parent dir */
        else
          status = svn_wc__db_status_not_present; /* As checkout */
      }

    /* ### Check the parentstub if we don't find a BASE. But don't
           do this if we already have the info we want or we break
           some copy scenarios. */
    if (!has_base && target_kind == svn_wc__db_kind_dir)
      {
        svn_boolean_t not_present;
        svn_revnum_t rev = SVN_INVALID_REVNUM;
        err = svn_wc__db_temp_is_dir_deleted(&not_present, &rev,
                                             db, local_abspath, scratch_pool);

        if (err && (err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND
                    || err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY))
          {
            svn_error_clear(err);
            not_present = FALSE;
          }
        else
          SVN_ERR(err);

        if (not_present)
          status = svn_wc__db_status_not_present;

        if (!SVN_IS_VALID_REVNUM(target_rev))
          target_rev = rev;
      }
  }

  if ((status == svn_wc__db_status_not_present)
      || (target_kind == svn_wc__db_kind_dir
          && status != svn_wc__db_status_normal
          && status != svn_wc__db_status_incomplete))
    {
      /* The target does not exist or is a local addition */

      if (!SVN_IS_VALID_REVNUM(target_rev))
        target_rev = 0;

      if (depth == svn_depth_unknown)
        depth = svn_depth_infinity;

      SVN_ERR(reporter->set_path(report_baton, "", target_rev, depth,
                                 FALSE,
                                 NULL,
                                 scratch_pool));
      SVN_ERR(reporter->delete_path(report_baton, "", scratch_pool));

      /* Finish the report, which causes the update editor to be
         driven. */
      SVN_ERR(reporter->finish_report(report_baton, scratch_pool));

      return SVN_NO_ERROR;
    }

  if (!repos_root || !repos_relpath)
    {
      err = svn_wc__db_scan_base_repos(&repos_relpath, &repos_root, NULL,
                                      db, local_abspath,
                                      scratch_pool, scratch_pool);

      if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
        svn_error_clear(err);
      else
        SVN_ERR(err);

      /* Ok, that leaves a local addition. Deleted and not existing nodes
         are already handled. */
      if (!repos_root || !repos_relpath)
        SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, &repos_relpath,
                                         &repos_root, NULL, NULL, NULL, NULL,
                                         NULL, db, local_abspath,
                                         scratch_pool, scratch_pool));
    }

  if (!SVN_IS_VALID_REVNUM(target_rev))
    {
      SVN_ERR(find_base_rev(&target_rev, db, local_abspath, local_abspath,
                            scratch_pool));
      explicit_rev = TRUE;
    }
  else
    explicit_rev = FALSE;

  start_empty = (status == svn_wc__db_status_incomplete);
  if (depth_compatibility_trick
      && target_depth <= svn_depth_immediates
      && depth > target_depth)
    {
      start_empty = TRUE;
    }

  if (target_depth == svn_depth_unknown)
    target_depth = svn_depth_infinity;

  /* The first call to the reporter merely informs it that the
     top-level directory being updated is at BASE_REV.  Its PATH
     argument is ignored. */
  SVN_ERR(reporter->set_path(report_baton, "", target_rev, target_depth,
                             start_empty, NULL, scratch_pool));

  /* ### status can NEVER be deleted. should examine why this was
     ### ever here. we may have remapped into wc-ng incorrectly.  */
  if (status != svn_wc__db_status_deleted)
    {
      apr_finfo_t info;
      err = svn_io_stat(&info, local_abspath, APR_FINFO_MIN, scratch_pool);
      if (err)
        {
          if (APR_STATUS_IS_ENOENT(err->apr_err))
            missing = TRUE;
          svn_error_clear(err);
          err = NULL;
        }
    }

  if (missing && restore_files)
    {
      svn_boolean_t restored;

      err = restore_node(&restored, wc_ctx->db, local_abspath,
                         target_kind, use_commit_times,
                         notify_func, notify_baton,
                         scratch_pool);

      if (err)
          goto abort_report;

      if (restored)
        missing = FALSE;
    }

  if (target_kind == svn_wc__db_kind_dir)
    {
      if (missing)
        {
          /* Report missing directories as deleted to retrieve them
             from the repository. */
          err = reporter->delete_path(report_baton, "", scratch_pool);
          if (err)
            goto abort_report;
        }
      else if (depth != svn_depth_empty)
        {
          /* Recursively crawl ROOT_DIRECTORY and report differing
             revisions. */
          err = report_revisions_and_depths(wc_ctx->db,
                                            local_abspath,
                                            "",
                                            target_rev,
                                            reporter, report_baton,
                                            external_func, external_baton,
                                            notify_func, notify_baton,
                                            restore_files, depth,
                                            honor_depth_exclude,
                                            depth_compatibility_trick,
                                            start_empty,
                                            use_commit_times,
                                            scratch_pool);
          if (err)
            goto abort_report;
        }
    }

  else if (target_kind == svn_wc__db_kind_file ||
           target_kind == svn_wc__db_kind_symlink)
    {
      svn_boolean_t skip_set_path  = FALSE;
      const char *parent_abspath, *base;
      svn_wc__db_status_t parent_status;
      const char *parent_repos_relpath;

      svn_dirent_split(&parent_abspath, &base, local_abspath,
                       scratch_pool);

      /* We can assume a file is in the same repository as its parent
         directory, so we only look at the relpath. */
      err = svn_wc__db_base_get_info(&parent_status, NULL, NULL,
                                     &parent_repos_relpath, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL, NULL, NULL,
                                     NULL, NULL,
                                     db, parent_abspath,
                                     scratch_pool, scratch_pool);

      if (err)
        goto abort_report;

      if (!parent_repos_relpath)
        err = svn_wc__db_scan_base_repos(&parent_repos_relpath, NULL,
                                         NULL,
                                         db, parent_abspath,
                                         scratch_pool, scratch_pool);

      if (err)
        goto abort_report;

      if (strcmp(repos_relpath,
                 svn_relpath_join(parent_repos_relpath, base,
                                  scratch_pool)) != 0)
        {
          /* This file is disjoint with respect to its parent
             directory.  Since we are looking at the actual target of
             the report (not some file in a subdirectory of a target
             directory), and that target is a file, we need to pass an
             empty string to link_path. */
          err = reporter->link_path(report_baton,
                                    "",
                                    svn_path_url_add_component2(
                                                    repos_root,
                                                    repos_relpath,
                                                    scratch_pool),
                                    target_rev,
                                    target_depth,
                                    FALSE,
                                    target_lock ? target_lock->token : NULL,
                                    scratch_pool);
          if (err)
            goto abort_report;
          skip_set_path = TRUE;
        }

      if (!skip_set_path && (explicit_rev || target_lock))
        {
          /* If this entry is a file node, we just want to report that
             node's revision.  Since we are looking at the actual target
             of the report (not some file in a subdirectory of a target
             directory), and that target is a file, we need to pass an
             empty string to set_path. */
          err = reporter->set_path(report_baton, "", target_rev,
                                   target_depth,
                                   FALSE,
                                   target_lock ? target_lock->token : NULL,
                                   scratch_pool);
          if (err)
            goto abort_report;
        }
    }

  /* Finish the report, which causes the update editor to be driven. */
  return reporter->finish_report(report_baton, scratch_pool);

 abort_report:
  /* Clean up the fs transaction. */
  if ((fserr = reporter->abort_report(report_baton, scratch_pool)))
    {
      fserr = svn_error_quick_wrap(fserr, _("Error aborting report"));
      svn_error_compose(err, fserr);
    }
  return err;
}

/*** Copying stream ***/

/* A copying stream is a bit like the unix tee utility:
 *
 * It reads the SOURCE when asked for data and while returning it,
 * also writes the same data to TARGET.
 */
struct copying_stream_baton
{
  /* Stream to read input from. */
  svn_stream_t *source;

  /* Stream to write all data read to. */
  svn_stream_t *target;
};


/* */
static svn_error_t *
read_handler_copy(void *baton, char *buffer, apr_size_t *len)
{
  struct copying_stream_baton *btn = baton;

  SVN_ERR(svn_stream_read(btn->source, buffer, len));

  return svn_stream_write(btn->target, buffer, len);
}

/* */
static svn_error_t *
close_handler_copy(void *baton)
{
  struct copying_stream_baton *btn = baton;

  SVN_ERR(svn_stream_close(btn->target));
  return svn_stream_close(btn->source);
}


/* Return a stream - allocated in POOL - which reads its input
 * from SOURCE and, while returning that to the caller, at the
 * same time writes that to TARGET.
 */
static svn_stream_t *
copying_stream(svn_stream_t *source,
               svn_stream_t *target,
               apr_pool_t *pool)
{
  struct copying_stream_baton *baton;
  svn_stream_t *stream;

  baton = apr_palloc(pool, sizeof (*baton));
  baton->source = source;
  baton->target = target;

  stream = svn_stream_create(baton, pool);
  svn_stream_set_read(stream, read_handler_copy);
  svn_stream_set_close(stream, close_handler_copy);

  return stream;
}

svn_error_t *
svn_wc__internal_transmit_text_deltas(const char **tempfile,
                                      const svn_checksum_t **new_text_base_md5_checksum,
                                      const svn_checksum_t **new_text_base_sha1_checksum,
                                      svn_wc__db_t *db,
                                      const char *local_abspath,
                                      svn_boolean_t fulltext,
                                      const svn_delta_editor_t *editor,
                                      void *file_baton,
                                      apr_pool_t *result_pool,
                                      apr_pool_t *scratch_pool)
{
  svn_txdelta_window_handler_t handler;
  void *wh_baton;
  const svn_checksum_t *expected_md5_checksum;
  svn_checksum_t *verify_checksum = NULL;  /* calc'd MD5 of BASE_STREAM */
  svn_checksum_t *local_md5_checksum;  /* calc'd MD5 of LOCAL_STREAM */
  svn_checksum_t *local_sha1_checksum;  /* calc'd SHA1 of LOCAL_STREAM */
  const char *new_pristine_tmp_abspath;
  svn_error_t *err;
  svn_stream_t *base_stream;  /* delta source */
  svn_stream_t *local_stream;  /* delta target: LOCAL_ABSPATH transl. to NF */

  /* Translated input */
  SVN_ERR(svn_wc__internal_translated_stream(&local_stream, db,
                                             local_abspath, local_abspath,
                                             SVN_WC_TRANSLATE_TO_NF,
                                             scratch_pool, scratch_pool));

  /* If the caller wants a copy of the working file translated to
   * repository-normal form, make the copy by tee-ing the stream and set
   * *TEMPFILE to the path to it.  This is only needed for the 1.6 API,
   * 1.7 doesn't set TEMPFILE.  Even when using the 1.6 API this file
   * is not used by the functions that would have used it when using
   * the 1.6 code.  It's possible that 3rd party users (if there are any)
   * might expect this file to be a text-base. */
  if (tempfile)
    {
      svn_stream_t *tempstream;

      /* It can't be the same location as in 1.6 because the admin directory
         no longer exists. */
      SVN_ERR(svn_io_open_unique_file3(NULL, tempfile, NULL, 
                                       svn_io_file_del_none,
                                       result_pool, scratch_pool));

      SVN_ERR(svn_stream_open_writable(&tempstream, *tempfile,
                                       scratch_pool, scratch_pool));

      /* Wrap the translated stream with a new stream that writes the
         translated contents into the new text base file as we read from it.
         Note that the new text base file will be closed when the new stream
         is closed. */
      local_stream = copying_stream(local_stream, tempstream, scratch_pool);
    }
  if (new_text_base_sha1_checksum)
    {
      svn_stream_t *new_pristine_stream;

      SVN_ERR(svn_wc__open_writable_base(&new_pristine_stream,
                                         &new_pristine_tmp_abspath,
                                         NULL, &local_sha1_checksum,
                                         db, local_abspath,
                                         scratch_pool, scratch_pool));
      local_stream = copying_stream(local_stream, new_pristine_stream,
                                    scratch_pool);
    }

  /* Set BASE_STREAM to a stream providing the base (source) content for the
   * delta, which may be an empty stream;
   * set EXPECTED_CHECKSUM to its stored (or possibly calculated) MD5 checksum;
   * and (usually) arrange for its VERIFY_CHECKSUM to be calculated later. */
  if (! fulltext)
    {
      /* Compute delta against the pristine contents */
      SVN_ERR(svn_wc__get_pristine_contents(&base_stream, db, local_abspath,
                                            scratch_pool, scratch_pool));
      if (base_stream == NULL)
        base_stream = svn_stream_empty(scratch_pool);

      SVN_ERR(svn_wc__db_read_info(NULL, NULL, NULL,
                                   NULL, NULL, NULL,
                                   NULL, NULL, NULL,
                                   NULL, NULL,
                                   &expected_md5_checksum, NULL,
                                   NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL,
                                   NULL, NULL, NULL,
                                   db, local_abspath,
                                   scratch_pool, scratch_pool));
      /* SVN_EXPERIMENTAL_PRISTINE:
         If we got a SHA-1, get the corresponding MD-5. */
      if (expected_md5_checksum
          && expected_md5_checksum->kind != svn_checksum_md5)
        SVN_ERR(svn_wc__db_pristine_get_md5(&expected_md5_checksum,
                                            db, local_abspath,
                                            expected_md5_checksum,
                                            scratch_pool, scratch_pool));

      /* ### We want expected_md5_checksum to ALWAYS be present, but on old
         working copies maybe it won't be (unclear?).  If it is there,
         then we can pass it to apply_textdelta() as required, and later on
         we can use it as an expected value to verify against.  Therefore
         we prepare now to calculate (during the later reading of the base
         stream) the actual checksum of the base stream as VERIFY_CHECKSUM.

         If the base checksum was NOT recorded, then we must compute it NOW
         for the apply_textdelta() call.  In this case, we won't bother to
         calculate it a second time during the later reading of the stream
         for the purpose of verification, and will leave VERIFY_CHECKSUM as
         NULL. */
      if (expected_md5_checksum)
        {
          /* Arrange to set VERIFY_CHECKSUM to the MD5 of what is *actually*
             found when the base stream is read. */
          base_stream = svn_stream_checksummed2(base_stream, &verify_checksum,
                                                NULL, svn_checksum_md5, TRUE,
                                                scratch_pool);
        }
      else
        {
          svn_stream_t *p_stream;
          svn_checksum_t *p_checksum;

          /* Set EXPECTED_CHECKSUM to the MD5 checksum of the existing
           * pristine text, by reading the text and calculating it. */
          /* ### we should ALREADY have the checksum for pristine. */
          SVN_ERR(svn_wc__get_pristine_contents(&p_stream, db, local_abspath,
                                                scratch_pool, scratch_pool));
          if (p_stream == NULL)
            p_stream = svn_stream_empty(scratch_pool);

          p_stream = svn_stream_checksummed2(p_stream, &p_checksum,
                                             NULL, svn_checksum_md5, TRUE,
                                             scratch_pool);

          /* Closing this will cause a full read/checksum. */
          SVN_ERR(svn_stream_close(p_stream));

          expected_md5_checksum = p_checksum;
        }
    }
  else
    {
      /* Send a fulltext. */
      base_stream = svn_stream_empty(scratch_pool);
      expected_md5_checksum = NULL;
    }

  /* Tell the editor that we're about to apply a textdelta to the
     file baton; the editor returns to us a window consumer and baton.  */
  {
    /* apply_textdelta() is working against a base with this checksum */
    const char *base_digest_hex = NULL;

    if (expected_md5_checksum)
      /* ### Why '..._display()'?  expected_md5_checksum should never be all-
       * zero, but if it is, we would want to pass NULL not an all-zero
       * digest to apply_textdelta(), wouldn't we? */
      base_digest_hex = svn_checksum_to_cstring_display(expected_md5_checksum,
                                                        scratch_pool);

    SVN_ERR(editor->apply_textdelta(file_baton, base_digest_hex, scratch_pool,
                                    &handler, &wh_baton));
  }

  /* Run diff processing, throwing windows at the handler. */
  err = svn_txdelta_run(base_stream, local_stream,
                        handler, wh_baton,
                        svn_checksum_md5, &local_md5_checksum,
                        NULL, NULL,
                        scratch_pool, scratch_pool);

  /* Close the two streams to force writing the digest,
     if we already have an error, ignore this one. */
  if (err)
    {
      svn_error_clear(svn_stream_close(base_stream));
      svn_error_clear(svn_stream_close(local_stream));
    }
  else
    {
      SVN_ERR(svn_stream_close(base_stream));
      SVN_ERR(svn_stream_close(local_stream));
    }

  /* If we have an error, it may be caused by a corrupt text base.
     Check the checksum and discard `err' if they don't match. */
  if (expected_md5_checksum && verify_checksum
      && !svn_checksum_match(expected_md5_checksum, verify_checksum))
    {
      /* The entry checksum does not match the actual text
         base checksum.  Extreme badness. Of course,
         theoretically we could just switch to
         fulltext transmission here, and everything would
         work fine; after all, we're going to replace the
         text base with a new one in a moment anyway, and
         we'd fix the checksum then.  But it's better to
         error out.  People should know that their text
         bases are getting corrupted, so they can
         investigate.  Other commands could be affected,
         too, such as `svn diff'.  */

      /* Deliberately ignore errors; the error about the
         checksum mismatch is more important to return. */
      svn_error_clear(err);
      if (tempfile)
        svn_error_clear(svn_io_remove_file2(*tempfile, TRUE, scratch_pool));

      return svn_error_createf(SVN_ERR_WC_CORRUPT_TEXT_BASE, NULL,
                               _("Checksum mismatch for text base of '%s':\n"
                                 "   expected:  %s\n"
                                 "     actual:  %s\n"),
                               svn_dirent_local_style(local_abspath,
                                                      scratch_pool),
                               svn_checksum_to_cstring_display(
                                 expected_md5_checksum, scratch_pool),
                               svn_checksum_to_cstring_display(
                                 verify_checksum, scratch_pool));
    }

  /* Now, handle that delta transmission error if any, so we can stop
     thinking about it after this point. */
  SVN_ERR_W(err, apr_psprintf(scratch_pool,
                              _("While preparing '%s' for commit"),
                              svn_dirent_local_style(local_abspath,
                                                     scratch_pool)));

  if (new_text_base_md5_checksum)
    *new_text_base_md5_checksum = svn_checksum_dup(local_md5_checksum,
                                                   result_pool);
  if (new_text_base_sha1_checksum)
    {
      SVN_ERR(svn_wc__db_pristine_install(db, new_pristine_tmp_abspath,
                                          local_sha1_checksum,
                                          local_md5_checksum,
                                          scratch_pool));
      *new_text_base_sha1_checksum = svn_checksum_dup(local_sha1_checksum,
                                                      result_pool);
    }

  /* Close the file baton, and get outta here. */
  return editor->close_file(file_baton,
                            svn_checksum_to_cstring(local_md5_checksum,
                                                    scratch_pool),
                            scratch_pool);
}

svn_error_t *
svn_wc_transmit_text_deltas3(const svn_checksum_t **new_text_base_md5_checksum,
                             const svn_checksum_t **new_text_base_sha1_checksum,
                             svn_wc_context_t *wc_ctx,
                             const char *local_abspath,
                             svn_boolean_t fulltext,
                             const svn_delta_editor_t *editor,
                             void *file_baton,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  return svn_wc__internal_transmit_text_deltas(NULL,
                                               new_text_base_md5_checksum,
                                               new_text_base_sha1_checksum,
                                               wc_ctx->db, local_abspath,
                                               fulltext, editor,
                                               file_baton, result_pool,
                                               scratch_pool);
}

svn_error_t *
svn_wc__internal_transmit_prop_deltas(svn_wc__db_t *db,
                                     const char *local_abspath,
                                     const svn_delta_editor_t *editor,
                                     void *baton,
                                     apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  int i;
  apr_array_header_t *propmods;
  svn_wc__db_kind_t kind;

  SVN_ERR(svn_wc__db_read_kind(&kind, db, local_abspath, FALSE, iterpool));

  /* Get an array of local changes by comparing the hashes. */
  SVN_ERR(svn_wc__internal_propdiff(&propmods, NULL, db, local_abspath,
                                    scratch_pool, iterpool));

  /* Apply each local change to the baton */
  for (i = 0; i < propmods->nelts; i++)
    {
      const svn_prop_t *p = &APR_ARRAY_IDX(propmods, i, svn_prop_t);

      svn_pool_clear(iterpool);

      if (kind == svn_wc__db_kind_file)
        SVN_ERR(editor->change_file_prop(baton, p->name, p->value,
                                         iterpool));
      else
        SVN_ERR(editor->change_dir_prop(baton, p->name, p->value,
                                        iterpool));
    }

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_transmit_prop_deltas2(svn_wc_context_t *wc_ctx,
                             const char *local_abspath,
                             const svn_delta_editor_t *editor,
                             void *baton,
                             apr_pool_t *scratch_pool)
{
  return svn_wc__internal_transmit_prop_deltas(wc_ctx->db, local_abspath,
                                               editor, baton, scratch_pool);
}
