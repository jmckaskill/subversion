/*
 * copy.c:  wc 'copy' functionality.
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

#include <string.h>
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"

#include "wc.h"
#include "log.h"
#include "workqueue.h"
#include "adm_files.h"
#include "props.h"
#include "translate.h"
#include "entries.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"


/*** Code. ***/

/* Make a copy of SRC_ABSPATH under a temporary name in the directory
   TMPDIR_ABSPATH and return the absolute path of the copy in
   *DST_ABSPATH.  Return the node kind of SRC_ABSPATH in *KIND.  If
   SRC_ABSPATH doesn't exist then set *DST_ABSPATH to NULL to indicate
   that no copy was made. */
static svn_error_t *
copy_to_tmpdir(const char **dst_abspath,
               svn_node_kind_t *kind,
               const char *src_abspath,
               const char *tmpdir_abspath,
               svn_boolean_t recursive,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               apr_pool_t *scratch_pool)
{
  svn_boolean_t is_special;
  svn_io_file_del_t delete_when;

  SVN_ERR(svn_io_check_special_path(src_abspath, kind, &is_special,
                                    scratch_pool));
  if (*kind == svn_node_none)
    {
      *dst_abspath = NULL;
      return SVN_NO_ERROR;
    }
  else if (*kind == svn_node_unknown)
    {
      return svn_error_createf(SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                               _("Source '%s' is unexpected kind"),
                               svn_dirent_local_style(src_abspath,
                                                      scratch_pool));
    }
  else if (*kind == svn_node_dir || is_special)
    delete_when = svn_io_file_del_on_close;
  else if (*kind == svn_node_file)
    delete_when = svn_io_file_del_none;

  /* ### Do we need a pool cleanup to remove the copy?  We can't use
     ### svn_io_file_del_on_pool_cleanup above because a) it won't
     ### handle the directory case and b) we need to be able to remove
     ### the cleanup before queueing the move work item. */

  SVN_ERR(svn_io_open_unique_file3(NULL, dst_abspath, tmpdir_abspath,
                                   delete_when, scratch_pool, scratch_pool));

  if (*kind == svn_node_dir)
    {
      if (recursive)
        SVN_ERR(svn_io_copy_dir_recursively(src_abspath,
                                            tmpdir_abspath,
                                            svn_dirent_basename(*dst_abspath,
                                                                scratch_pool),
                                            TRUE, /* copy_perms */
                                            cancel_func, cancel_baton,
                                            scratch_pool));
      else
        SVN_ERR(svn_io_dir_make(*dst_abspath, APR_OS_DEFAULT, scratch_pool));
    }
  else if (!is_special)
    SVN_ERR(svn_io_copy_file(src_abspath, *dst_abspath, TRUE, /* copy_perms */
                             scratch_pool));
  else
    SVN_ERR(svn_io_copy_link(src_abspath, *dst_abspath, scratch_pool));
    

  return SVN_NO_ERROR;
}


/* A replacement for both copy_file_administratively and
   copy_added_file_administratively.  Not yet fully working.  Relies
   on in-db-props.  SRC_ABSPATH is a versioned file but the filesystem
   node might not be a file.

   This also works for versioned symlinks that are stored in the db as
   svn_wc__db_kind_file with svn:special set. */
static svn_error_t *
copy_versioned_file(svn_wc__db_t *db,
                    const char *src_abspath,
                    const char *dst_abspath,
                    svn_cancel_func_t cancel_func,
                    void *cancel_baton,
                    svn_wc_notify_func2_t notify_func,
                    void *notify_baton,
                    apr_pool_t *scratch_pool)
{
  svn_skel_t *work_items = NULL;
  const char *dir_abspath = svn_dirent_dirname(dst_abspath, scratch_pool);
  const char *tmpdir_abspath;
  svn_stream_t *src_pristine;
  const char *tmp_dst_abspath;
  svn_node_kind_t kind;

  SVN_ERR(svn_wc__db_temp_wcroot_tempdir(&tmpdir_abspath, db,
                                         dst_abspath,
                                         scratch_pool, scratch_pool));

  
  /* This goes away when we stop using revert bases. */
  {
    svn_wc__db_status_t dst_status; 
    svn_boolean_t will_replace;
    svn_error_t *err;

    err = svn_wc__db_read_info(&dst_status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               db, dst_abspath,
                               scratch_pool, scratch_pool);
    if (err && err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
      return svn_error_return(err);
    will_replace = (!err && dst_status == svn_wc__db_status_deleted);
    svn_error_clear(err);
    if (will_replace)
      SVN_ERR(svn_wc__wq_prepare_revert_files(db, dst_abspath,
                                              scratch_pool));
  }

  /* This goes away when we centralise, but until then we might need
     to do a cross-db pristine copy. */
  if (strcmp(svn_dirent_dirname(src_abspath, scratch_pool),
             svn_dirent_dirname(dst_abspath, scratch_pool)))
    {
      const svn_checksum_t *checksum;

      SVN_ERR(svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL,
                                   &checksum,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL,
                                   db, src_abspath,
                                   scratch_pool, scratch_pool));
      if (checksum)
        {
          svn_stream_t *tmp_pristine;
          const char *tmp_pristine_abspath;
          const svn_checksum_t *sha1_checksum, *md5_checksum;

          if (checksum->kind == svn_checksum_md5)
            {
              md5_checksum = checksum;
              SVN_ERR(svn_wc__db_pristine_get_sha1(&sha1_checksum, db,
                                                   src_abspath, checksum,
                                                   scratch_pool, scratch_pool));
            }
          else
            {
              sha1_checksum = checksum;
              SVN_ERR(svn_wc__db_pristine_get_md5(&md5_checksum, db,
                                                  src_abspath, checksum,
                                                  scratch_pool, scratch_pool));
            }
          SVN_ERR(svn_wc__db_pristine_read(&src_pristine, db,
                                           src_abspath, sha1_checksum,
                                           scratch_pool, scratch_pool));
          SVN_ERR(svn_stream_open_unique(&tmp_pristine, &tmp_pristine_abspath,
                                         tmpdir_abspath, svn_io_file_del_none,
                                         scratch_pool, scratch_pool));
          SVN_ERR(svn_stream_copy3(src_pristine, tmp_pristine,
                                   cancel_func, cancel_baton, scratch_pool));
          SVN_ERR(svn_wc__db_pristine_install(db, tmp_pristine_abspath,
                                              sha1_checksum, md5_checksum,
                                              scratch_pool));
        }
    }

#if (SVN_WC__VERSION < SVN_WC__PROPS_IN_DB)
  /* This goes away when we move to in-db-props. */
  {
    apr_hash_t *src_props;

    SVN_ERR(svn_wc__get_pristine_props(&src_props, db, src_abspath,
                                       scratch_pool, scratch_pool));
    if (src_props && apr_hash_count(src_props))
      {
        svn_skel_t *work_item;
        const char *props_abspath;

        SVN_ERR(svn_wc__prop_path(&props_abspath, dst_abspath, 
                                  svn_wc__db_kind_file, svn_wc__props_base,
                                  scratch_pool));
        SVN_ERR(svn_wc__wq_build_write_old_props(&work_item, props_abspath,
                                                 src_props, scratch_pool));
        work_items = svn_wc__wq_merge(work_items, work_item, scratch_pool);
      }

    SVN_ERR(svn_wc__get_actual_props(&src_props, db, src_abspath,
                                     scratch_pool, scratch_pool));
    if (src_props && apr_hash_count(src_props))
      {
        svn_skel_t *work_item;
        const char *props_abspath;

        SVN_ERR(svn_wc__prop_path(&props_abspath, dst_abspath, 
                                  svn_wc__db_kind_file, svn_wc__props_working,
                                  scratch_pool));
        SVN_ERR(svn_wc__wq_build_write_old_props(&work_item, props_abspath,
                                                 src_props, scratch_pool));
        work_items = svn_wc__wq_merge(work_items, work_item, scratch_pool);
      }
  }
#endif

  SVN_ERR(copy_to_tmpdir(&tmp_dst_abspath, &kind, src_abspath, tmpdir_abspath,
                         TRUE, /* recursive */
                         cancel_func, cancel_baton, scratch_pool));
  if (tmp_dst_abspath)
    {
      svn_skel_t *work_item;

      SVN_ERR(svn_wc__wq_build_file_move(&work_item, db,
                                         tmp_dst_abspath, dst_abspath,
                                         scratch_pool, scratch_pool));
      work_items = svn_wc__wq_merge(work_items, work_item, scratch_pool);
    }

  SVN_ERR(svn_wc__db_op_copy(db, src_abspath, dst_abspath,
                             work_items, scratch_pool));
  SVN_ERR(svn_wc__wq_run(db, dir_abspath,
                         cancel_func, cancel_baton, scratch_pool));

  if (notify_func)
    {
      svn_wc_notify_t *notify
        = svn_wc_create_notify(dst_abspath, svn_wc_notify_add,
                               scratch_pool);
      notify->kind = svn_node_file;
      (*notify_func)(notify_baton, notify, scratch_pool);
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
copy_versioned_dir(svn_wc__db_t *db,
                   const char *src_abspath,
                   const char *dst_abspath,
                   svn_cancel_func_t cancel_func,
                   void *cancel_baton,
                   svn_wc_notify_func2_t notify_func,
                   void *notify_baton,
                   apr_pool_t *scratch_pool)
{
  svn_skel_t *work_items = NULL;
  const char *dir_abspath = svn_dirent_dirname(dst_abspath, scratch_pool);
  const char *tmpdir_abspath;
  const char *tmp_dst_abspath;
  const apr_array_header_t *versioned_children;
  apr_hash_t *children;
  svn_node_kind_t kind;
  apr_pool_t *iterpool;
  int i;

  SVN_ERR(svn_wc__db_temp_wcroot_tempdir(&tmpdir_abspath, db,
                                         dst_abspath,
                                         scratch_pool, scratch_pool));

  SVN_ERR(copy_to_tmpdir(&tmp_dst_abspath, &kind, src_abspath, tmpdir_abspath,
                         FALSE, /* recursive */
                         cancel_func, cancel_baton, scratch_pool));
  if (tmp_dst_abspath)
    {
      svn_skel_t *work_item;

      SVN_ERR(svn_wc__wq_build_file_move(&work_item, db,
                                         tmp_dst_abspath, dst_abspath,
                                         scratch_pool, scratch_pool));
      work_items = svn_wc__wq_merge(work_items, work_item, scratch_pool);

#ifndef SVN_WC__SINGLE_DB
      if (kind == svn_node_dir)
        {
          /* Create the per-directory db in the copied directory.  The
             copy is not yet connected to the parent so we don't need
             to use a workqueue.  This will be removed when we
             centralise. */
          const char *dst_parent_abspath, *name;
          const char *repos_root_url, *repos_uuid;
          svn_revnum_t revision;
          svn_depth_t depth;
          svn_wc__db_status_t status;
          svn_boolean_t have_base;

          svn_dirent_split(&dst_parent_abspath, &name, dst_abspath,
                           scratch_pool);

          SVN_ERR(svn_wc__db_read_info(&status,
                                       NULL, /* kind */
                                       &revision,
                                       NULL, /* repos_relpath */
                                       &repos_root_url,
                                       &repos_uuid,
                                       NULL, /* changed_rev */
                                       NULL, /* changed_date */
                                       NULL, /* changed_author */
                                       NULL, /* last_mod_time */
                                       &depth,
                                       NULL, /* checksum */
                                       NULL, /* translated_size */
                                       NULL, /* target */
                                       NULL, /* changelist */
                                       NULL, /* original_repos_relpath */
                                       NULL, /* original_root_url */
                                       NULL, /* original_uuid */
                                       NULL, /* original_revision */
                                       NULL, /* props_mod */
                                       &have_base,
                                       NULL, /* have_work */
                                       NULL, /* conflicted */
                                       NULL, /* lock */
                                       db, src_abspath,
                                       scratch_pool, scratch_pool));

          if (!repos_root_url)
            {
              if (status == svn_wc__db_status_deleted)
                {
                  const char *work_del_abspath;

                  SVN_ERR(svn_wc__db_scan_deletion(NULL, NULL, NULL,
                                                   &work_del_abspath,
                                                   db, src_abspath,
                                                   scratch_pool, scratch_pool));
                  if (work_del_abspath)
                    {
                      const char *parent_del_abspath
                        = svn_dirent_dirname(work_del_abspath, scratch_pool);

                      SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, NULL,
                                                       &repos_root_url,
                                                       &repos_uuid,
                                                       NULL, NULL, NULL, NULL,
                                                       db, parent_del_abspath,
                                                       scratch_pool,
                                                       scratch_pool));
                    }
                  else
                    SVN_ERR(svn_wc__db_scan_base_repos(NULL, &repos_root_url,
                                                       &repos_uuid,
                                                       db, src_abspath,
                                                       scratch_pool,
                                                       scratch_pool));
                }
              else if (status == svn_wc__db_status_added || !have_base)
                SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, NULL,
                                                 &repos_root_url, &repos_uuid,
                                                 NULL, NULL, NULL, NULL,
                                                 db, src_abspath,
                                                 scratch_pool, scratch_pool));
              else
                 SVN_ERR(svn_wc__db_scan_base_repos(NULL, &repos_root_url,
                                                    &repos_uuid,
                                                    db, src_abspath,
                                                    scratch_pool, scratch_pool));
            }

          /* Use the repos_root as root node url, because we are going to
             remove the node directly anyway. */
          SVN_ERR(svn_wc__internal_ensure_adm(db, tmp_dst_abspath,
                                              repos_root_url, repos_root_url,
                                              repos_uuid, revision, depth,
                                              scratch_pool));

          /* That creates a base node which we do not want so delete it. */
          SVN_ERR(svn_wc__db_base_remove(db, tmp_dst_abspath,
                                         scratch_pool));

          /* ### Need to close the database so that Windows can move
             ### the directory. */
          SVN_ERR(svn_wc__db_temp_forget_directory(db, tmp_dst_abspath,
                                                   scratch_pool));
        }
#endif
    }

#if (SVN_WC__VERSION < SVN_WC__PROPS_IN_DB)
  /* This goes away when we move to in-db-props. */
  {
    apr_hash_t *src_props;

    SVN_ERR(svn_wc__get_pristine_props(&src_props, db, src_abspath,
                                       scratch_pool, scratch_pool));
    if (src_props && apr_hash_count(src_props))
      {
        svn_skel_t *work_item;
        const char *props_abspath;

        SVN_ERR(svn_wc__prop_path(&props_abspath, dst_abspath, 
                                  svn_wc__db_kind_dir, svn_wc__props_base,
                                  scratch_pool));
        SVN_ERR(svn_wc__wq_build_write_old_props(&work_item, props_abspath,
                                                 src_props, scratch_pool));
        work_items = svn_wc__wq_merge(work_items, work_item, scratch_pool);
      }

    SVN_ERR(svn_wc__get_actual_props(&src_props, db, src_abspath,
                                     scratch_pool, scratch_pool));
    if (src_props && apr_hash_count(src_props))
      {
        svn_skel_t *work_item;
        const char *props_abspath;

        SVN_ERR(svn_wc__prop_path(&props_abspath, dst_abspath, 
                                  svn_wc__db_kind_dir, svn_wc__props_working,
                                  scratch_pool));
        SVN_ERR(svn_wc__wq_build_write_old_props(&work_item, props_abspath,
                                                 src_props, scratch_pool));
        work_items = svn_wc__wq_merge(work_items, work_item, scratch_pool);
      }
  }
#endif

  SVN_ERR(svn_wc__db_op_copy(db, src_abspath, dst_abspath,
                             work_items, scratch_pool));
  SVN_ERR(svn_wc__wq_run(db, dir_abspath,
                         cancel_func, cancel_baton, scratch_pool));

  if (kind == svn_node_dir)
    {
      /* The first copy only does the parent stub, this second copy
         does the full node but can only happen after the workqueue
         has move the destination into place. */
      SVN_ERR(svn_wc__db_op_copy(db, src_abspath, dst_abspath,
                                 NULL, scratch_pool));
    }

  if (notify_func)
    {
      svn_wc_notify_t *notify
        = svn_wc_create_notify(dst_abspath, svn_wc_notify_add,
                               scratch_pool);
      notify->kind = svn_node_dir;
      (*notify_func)(notify_baton, notify, scratch_pool);
    }

  if (kind == svn_node_dir)
    /* All children, versioned and unversioned.  We're only interested in the
       names of the children, so we can pass TRUE as the only_check_type
       param. */
    SVN_ERR(svn_io_get_dirents3(&children, src_abspath, TRUE,
                                scratch_pool, scratch_pool));

  /* Copy all the versioned children */
  SVN_ERR(svn_wc__db_read_children(&versioned_children, db, src_abspath,
                                   scratch_pool, scratch_pool));
  iterpool = svn_pool_create(scratch_pool);
  for (i = 0; i < versioned_children->nelts; ++i)
    {
      const char *child_name, *child_src_abspath, *child_dst_abspath;
      svn_wc__db_kind_t child_kind;

      svn_pool_clear(iterpool);
      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      child_name = APR_ARRAY_IDX(versioned_children, i, const char *);
      child_src_abspath = svn_dirent_join(src_abspath, child_name, iterpool);
      child_dst_abspath = svn_dirent_join(dst_abspath, child_name, iterpool);

      SVN_ERR(svn_wc__db_read_kind(&child_kind, db, child_src_abspath,
                                   TRUE, iterpool));

      if (child_kind == svn_wc__db_kind_file)
        SVN_ERR(copy_versioned_file(db,
                                    child_src_abspath, child_dst_abspath,
                                    cancel_func, cancel_baton, NULL, NULL,
                                    iterpool));
      else if (child_kind == svn_wc__db_kind_dir)
        SVN_ERR(copy_versioned_dir(db,
                                   child_src_abspath, child_dst_abspath,
                                   cancel_func, cancel_baton, NULL, NULL,
                                   iterpool));
      else
        return svn_error_createf(SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                                 _("cannot handle node kind for '%s'"),
                                 svn_dirent_local_style(child_src_abspath,
                                                        scratch_pool));

      if (kind == svn_node_dir)
        /* Remove versioned child as it has been handled */
        apr_hash_set(children, child_name, APR_HASH_KEY_STRING, NULL);
    }

  if (kind == svn_node_dir)
    {
      /* All the remaining children are unversioned. */
      apr_hash_index_t *hi;

      for (hi = apr_hash_first(scratch_pool, children); hi;
           hi = apr_hash_next(hi))
        {
          const char *name = svn__apr_hash_index_key(hi);
          const char *unver_src_abspath, *unver_dst_abspath;

          if (svn_wc_is_adm_dir(name, iterpool))
            continue;

          svn_pool_clear(iterpool);
          if (cancel_func)
            SVN_ERR(cancel_func(cancel_baton));

          unver_src_abspath = svn_dirent_join(src_abspath, name, iterpool);
          unver_dst_abspath = svn_dirent_join(dst_abspath, name, iterpool);

          SVN_ERR(copy_to_tmpdir(&tmp_dst_abspath, &kind, unver_src_abspath,
                                 tmpdir_abspath,
                                 TRUE, /* recursive */
                                 cancel_func, cancel_baton, iterpool));
          if (tmp_dst_abspath)
            {
              svn_skel_t *work_item;
              SVN_ERR(svn_wc__wq_build_file_move(&work_item, db,
                                                 tmp_dst_abspath,
                                                 unver_dst_abspath,
                                                 iterpool, iterpool));
              SVN_ERR(svn_wc__db_wq_add(db, dst_abspath, work_item,
                                        iterpool));
            }

        }
      SVN_ERR(svn_wc__wq_run(db, dst_abspath, cancel_func, cancel_baton,
                             scratch_pool));
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}



/* Public Interface */

svn_error_t *
svn_wc_copy3(svn_wc_context_t *wc_ctx,
             const char *src_abspath,
             const char *dst_abspath,
             svn_cancel_func_t cancel_func,
             void *cancel_baton,
             svn_wc_notify_func2_t notify_func,
             void *notify_baton,
             apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db = wc_ctx->db;
  svn_node_kind_t src_kind;
  svn_wc__db_kind_t src_db_kind;
  const char *dstdir_abspath;
  
  SVN_ERR_ASSERT(svn_dirent_is_absolute(src_abspath));
  SVN_ERR_ASSERT(svn_dirent_is_absolute(dst_abspath));
  
  dstdir_abspath = svn_dirent_dirname(dst_abspath, scratch_pool);

  {
    svn_wc__db_status_t src_status, dstdir_status;
    const char *src_repos_root_url, *dst_repos_root_url;
    const char *src_repos_uuid, *dst_repos_uuid;
    svn_error_t *err;

    err = svn_wc__db_read_info(&src_status, &src_db_kind, NULL, NULL,
                               &src_repos_root_url, &src_repos_uuid, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL,
                               db, src_abspath, scratch_pool, scratch_pool);

    if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
      {
        /* Replicate old error code and text */
        svn_error_clear(err);
        return svn_error_createf(SVN_ERR_ENTRY_NOT_FOUND, NULL,
                                 _("'%s' is not under version control"),
                                 svn_dirent_local_style(src_abspath,
                                                        scratch_pool));
      }
    else
      SVN_ERR(err);

    SVN_ERR(svn_wc__db_read_info(&dstdir_status, NULL, NULL, NULL,
                                 &dst_repos_root_url, &dst_repos_uuid, NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL,
                                 db, dstdir_abspath, scratch_pool, scratch_pool));

    if (!src_repos_root_url)
      {
        if (src_status == svn_wc__db_status_added)
          SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, NULL,
                                           &src_repos_root_url,
                                           &src_repos_uuid, NULL, NULL, NULL,
                                           NULL,
                                           db, src_abspath,
                                           scratch_pool, scratch_pool));
        else
          /* If not added, the node must have a base or we can't copy */
          SVN_ERR(svn_wc__db_scan_base_repos(NULL, &src_repos_root_url,
                                             &src_repos_uuid,
                                             db, src_abspath,
                                             scratch_pool, scratch_pool));
      }

    if (!dst_repos_root_url)
      {
        if (dstdir_status == svn_wc__db_status_added)
          SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, NULL,
                                           &dst_repos_root_url,
                                           &dst_repos_uuid, NULL, NULL, NULL,
                                           NULL,
                                           db, dstdir_abspath,
                                           scratch_pool, scratch_pool));
        else
          /* If not added, the node must have a base or we can't copy */
          SVN_ERR(svn_wc__db_scan_base_repos(NULL, &dst_repos_root_url,
                                             &dst_repos_uuid,
                                             db, dstdir_abspath,
                                             scratch_pool, scratch_pool));
      }

    if (strcmp(src_repos_root_url, dst_repos_root_url) != 0
        || strcmp(src_repos_uuid, dst_repos_uuid) != 0)
      return svn_error_createf(
         SVN_ERR_WC_INVALID_SCHEDULE, NULL,
         _("Cannot copy to '%s', as it is not from repository '%s'; "
           "it is from '%s'"),
         svn_dirent_local_style(dst_abspath, scratch_pool),
         src_repos_root_url, dst_repos_root_url);

    if (dstdir_status == svn_wc__db_status_deleted)
      return svn_error_createf(
         SVN_ERR_WC_INVALID_SCHEDULE, NULL,
         _("Cannot copy to '%s' as it is scheduled for deletion"),
         svn_dirent_local_style(dst_abspath, scratch_pool));
  }

  /* TODO(#2843): Rework the error report. */
  /* Check if the copy target is missing or hidden and thus not exist on the
     disk, before actually doing the file copy. */
  {
    svn_wc__db_status_t dst_status;
    svn_error_t *err;

    err = svn_wc__db_read_info(&dst_status, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               db, dst_abspath, scratch_pool, scratch_pool);

    if (err && err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
      return svn_error_return(err);

    svn_error_clear(err);

    if (!err)
      switch (dst_status)
        {
          case svn_wc__db_status_excluded:
            return svn_error_createf(
                     SVN_ERR_ENTRY_EXISTS, NULL,
                     _("'%s' is already under version control "
                       "but is excluded."),
                     svn_dirent_local_style(dst_abspath, scratch_pool));
          case svn_wc__db_status_absent:
            return svn_error_createf(
                     SVN_ERR_ENTRY_EXISTS, NULL,
                     _("'%s' is already under version control"),
                     svn_dirent_local_style(dst_abspath, scratch_pool));

          case svn_wc__db_status_deleted:
          case svn_wc__db_status_obstructed_delete:
          case svn_wc__db_status_not_present:
            break; /* OK to add */

          default:
            return svn_error_createf(SVN_ERR_ENTRY_EXISTS, NULL,
                               _("There is already a versioned item '%s'"),
                               svn_dirent_local_style(dst_abspath,
                                                      scratch_pool));
        }
  }

  SVN_ERR(svn_io_check_path(src_abspath, &src_kind, scratch_pool));

#ifndef SINGLE_DB
  if (src_kind == svn_node_file ||
      (src_kind == svn_node_none
        && (src_db_kind == svn_wc__db_kind_file
            || src_db_kind == svn_wc__db_kind_symlink)))
#endif
    {
      svn_node_kind_t dst_kind;

      /* This is the error checking from copy_file_administratively
         but converted to wc-ng.  It's not in copy_file since this
         checking only needs to happen at the root of the copy and not
         when called recursively. */
      SVN_ERR(svn_io_check_path(dst_abspath, &dst_kind, scratch_pool));
      if (dst_kind != svn_node_none)
        return svn_error_createf(SVN_ERR_ENTRY_EXISTS, NULL,
                                 _("'%s' already exists and is in the way"),
                                 svn_dirent_local_style(dst_abspath,
                                                        scratch_pool));
    }

  if (src_db_kind == svn_wc__db_kind_file
      || src_db_kind == svn_wc__db_kind_symlink)
    {
      SVN_ERR(copy_versioned_file(db, src_abspath, dst_abspath,
                                  cancel_func, cancel_baton,
                                  notify_func, notify_baton,
                                  scratch_pool));
    }
  else
    {
      SVN_ERR(copy_versioned_dir(db, src_abspath, dst_abspath,
                                 cancel_func, cancel_baton,
                                 notify_func, notify_baton,
                                 scratch_pool));
    }

  return SVN_NO_ERROR;
}
