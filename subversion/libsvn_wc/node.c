/*
 * node.c:  routines for getting information about nodes in the working copy.
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

/* A note about these functions:

   We aren't really sure yet which bits of data libsvn_client needs about
   nodes.  In wc-1, we just grab the entry, and then use whatever we want
   from it.  Such a pattern is Bad.

   This file is intended to hold functions which retrieve specific bits of
   information about a node, and will hopefully give us a better idea about
   what data libsvn_client needs, and how to best provide that data in 1.7
   final.  As such, these functions should only be called from outside
   libsvn_wc; any internal callers are encouraged to use the appropriate
   information fetching function, such as svn_wc__db_read_info().
*/

#include <apr_pools.h>
#include <apr_time.h>

#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_hash.h"
#include "svn_types.h"

#include "wc.h"
#include "lock.h"
#include "props.h"
#include "log.h"
#include "entries.h"
#include "wc_db.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"
#include "private/svn_debug.h"


svn_error_t *
svn_wc__node_get_children(const apr_array_header_t **children,
                          svn_wc_context_t *wc_ctx,
                          const char *dir_abspath,
                          svn_boolean_t show_hidden,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  const apr_array_header_t *rel_children;
  apr_array_header_t *childs;
  int i;

  SVN_ERR(svn_wc__db_read_children(&rel_children, wc_ctx->db, dir_abspath,
                                   scratch_pool, scratch_pool));

  childs = apr_array_make(result_pool, rel_children->nelts,
                             sizeof(const char *));
  for (i = 0; i < rel_children->nelts; i++)
    {
      const char *child_abspath = svn_dirent_join(dir_abspath,
                                                  APR_ARRAY_IDX(rel_children,
                                                                i,
                                                                const char *),
                                                  result_pool);

      /* Don't add hidden nodes to *CHILDREN if we don't want them. */
      if (!show_hidden)
        {
          svn_boolean_t child_is_hidden;

          SVN_ERR(svn_wc__db_node_hidden(&child_is_hidden, wc_ctx->db,
                                         child_abspath, scratch_pool));
          if (child_is_hidden)
            continue;
        }

      APR_ARRAY_PUSH(childs, const char *) = child_abspath;
    }

  *children = childs;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__node_get_repos_info(const char **repos_root_url,
                            const char **repos_uuid,
                            svn_wc_context_t *wc_ctx,
                            const char * local_abspath,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  
  err = svn_wc__db_read_info(NULL, NULL, NULL, NULL,
                             repos_root_url, repos_uuid,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL,
                             wc_ctx->db, local_abspath, result_pool,
                             scratch_pool);

  if (err && (err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND
            || err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY))
    {
      svn_error_clear(err);
      err = SVN_NO_ERROR;
      if (repos_root_url)
        *repos_root_url = NULL;
      if (repos_uuid)
        *repos_uuid = NULL;
    }

  return err;
}

svn_error_t *
svn_wc__node_get_kind(svn_node_kind_t *kind,
                      svn_wc_context_t *wc_ctx,
                      const char *abspath,
                      svn_boolean_t show_hidden,
                      apr_pool_t *scratch_pool)
{
  svn_wc__db_kind_t db_kind;

  SVN_ERR(svn_wc__db_read_kind(&db_kind, wc_ctx->db, abspath, TRUE,
                               scratch_pool));
  switch (db_kind)
    {
      case svn_wc__db_kind_file:
        *kind = svn_node_file;
        break;
      case svn_wc__db_kind_dir:
        *kind = svn_node_dir;
        break;
      case svn_wc__db_kind_symlink:
        *kind = svn_node_file;
        break;
      case svn_wc__db_kind_unknown:
        *kind = svn_node_unknown;  /* ### should probably be svn_node_none  */
        break;
      default:
        SVN_ERR_MALFUNCTION();
    }

  /* If we found a svn_node_file or svn_node_dir, but it is hidden,
     then consider *KIND to be svn_node_none unless SHOW_HIDDEN is true. */
  if (! show_hidden
      && (*kind == svn_node_file || *kind == svn_node_dir))
    {
      svn_boolean_t hidden;

      SVN_ERR(svn_wc__db_node_hidden(&hidden, wc_ctx->db, abspath,
                                     scratch_pool));
      if (hidden)
        *kind = svn_node_none;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_get_depth(svn_depth_t *depth,
                       svn_wc_context_t *wc_ctx,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool)
{
  return svn_error_return(
    svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                         NULL, NULL, NULL, depth, NULL, NULL, NULL,
                         NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                         NULL, NULL, NULL,
                         wc_ctx->db, local_abspath, scratch_pool,
                         scratch_pool));
}

svn_error_t *
svn_wc__node_get_changed_info(svn_revnum_t *changed_rev,
                              apr_time_t *changed_date,
                              const char **changed_author,
                              svn_wc_context_t *wc_ctx,
                              const char *local_abspath,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  return svn_error_return(
    svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL, changed_rev,
                         changed_date, changed_author, NULL, NULL, NULL,
                         NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                         NULL, NULL, NULL, NULL,
                         wc_ctx->db, local_abspath, result_pool,
                         scratch_pool));
}

svn_error_t *
svn_wc__node_get_changelist(const char **changelist,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  svn_error_t *err;

  err = svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             changelist,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL,
                             wc_ctx->db, local_abspath, result_pool,
                             scratch_pool);

  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      svn_error_clear(err);
      err = SVN_NO_ERROR;
      *changelist = NULL;
    }

  return svn_error_return(err);
}

svn_error_t *
svn_wc__node_get_url(const char **url,
                     svn_wc_context_t *wc_ctx,
                     const char *local_abspath,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  const char *repos_relpath;
  const char *repos_root_url;

  SVN_ERR(svn_wc__db_read_info(&status, NULL, NULL, &repos_relpath,
                               &repos_root_url,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL,
                               wc_ctx->db, local_abspath, scratch_pool,
                               scratch_pool));

  if (repos_relpath == NULL)
    {
      if (status == svn_wc__db_status_normal
            || status == svn_wc__db_status_incomplete)
        {
          SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath, &repos_root_url,
                                             NULL, wc_ctx->db, local_abspath,
                                             scratch_pool, scratch_pool));
        }
      else if (status == svn_wc__db_status_added
                || status == svn_wc__db_status_obstructed_add)
        {
          SVN_ERR(svn_wc__db_scan_addition(NULL, NULL, &repos_relpath,
                                           &repos_root_url, NULL, NULL, NULL,
                                           NULL, NULL, wc_ctx->db,
                                           local_abspath, scratch_pool,
                                           scratch_pool));
        }
      else
        {
          *url = NULL;
          return SVN_NO_ERROR;
        }
    }

  SVN_ERR_ASSERT(repos_root_url != NULL && repos_relpath != NULL);
  *url = svn_path_url_add_component2(repos_root_url, repos_relpath,
                                     result_pool);

  return SVN_NO_ERROR;
}

/* A recursive node-walker, helper for svn_wc__node_walk_children(). */
static svn_error_t *
walker_helper(svn_wc__db_t *db,
              const char *dir_abspath,
              svn_boolean_t show_hidden,
              svn_wc__node_found_func_t walk_callback,
              void *walk_baton,
              svn_depth_t depth,
              svn_cancel_func_t cancel_func,
              void *cancel_baton,
              apr_pool_t *scratch_pool)
{
  const apr_array_header_t *rel_children;
  apr_pool_t *iterpool;
  int i;

  if (depth == svn_depth_empty)
    return SVN_NO_ERROR;

  SVN_ERR(svn_wc__db_read_children(&rel_children, db, dir_abspath,
                                   scratch_pool, scratch_pool));

  iterpool = svn_pool_create(scratch_pool);
  for (i = 0; i < rel_children->nelts; i++)
    {
      const char *child_abspath;
      svn_wc__db_kind_t child_kind;
      
      svn_pool_clear(iterpool);

      /* See if someone wants to cancel this operation. */
      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      child_abspath = svn_dirent_join(dir_abspath,
                                      APR_ARRAY_IDX(rel_children, i,
                                                    const char *),
                                      iterpool);

      if (!show_hidden)
        {
          svn_boolean_t hidden;

          SVN_ERR(svn_wc__db_node_hidden(&hidden, db, child_abspath, iterpool));
          if (hidden)
            continue;
        }

      SVN_ERR(svn_wc__db_read_info(NULL, &child_kind, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL,
                                   NULL,
                                   db, child_abspath, iterpool, iterpool));

      /* Return the child, if appropriate.  (For a directory,
       * this is the first visit: as a child.) */
      if (child_kind == svn_wc__db_kind_file
            || depth >= svn_depth_immediates)
        {
          SVN_ERR(walk_callback(child_abspath, walk_baton, iterpool));
        }

      /* Recurse into this directory, if appropriate. */
      if (child_kind == svn_wc__db_kind_dir
            && depth >= svn_depth_immediates)
        {
          svn_depth_t depth_below_here = depth;

          if (depth == svn_depth_immediates)
            depth_below_here = svn_depth_empty;

          SVN_ERR(walker_helper(db, child_abspath, show_hidden,
                                walk_callback, walk_baton,
                                depth_below_here, cancel_func, cancel_baton,
                                iterpool));
        }
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__internal_walk_children(svn_wc__db_t *db,
                               const char *local_abspath,
                               svn_boolean_t show_hidden,
                               svn_wc__node_found_func_t walk_callback,
                               void *walk_baton,
                               svn_depth_t walk_depth,
                               svn_cancel_func_t cancel_func,
                               void *cancel_baton,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_kind_t kind;
  svn_depth_t depth;

  SVN_ERR(svn_wc__db_read_info(NULL, &kind, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, &depth, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL,
                               db, local_abspath, scratch_pool, scratch_pool));

  if (kind == svn_wc__db_kind_file || depth == svn_depth_exclude)
    {
      return svn_error_return(
        walk_callback(local_abspath, walk_baton, scratch_pool));
    }

  if (kind == svn_wc__db_kind_dir)
    {
      /* Return the directory first, before starting recursion, since it
         won't get returned as part of the recursion. */
      SVN_ERR(walk_callback(local_abspath, walk_baton, scratch_pool));

      return svn_error_return(
        walker_helper(db, local_abspath, show_hidden, walk_callback, walk_baton,
                      walk_depth, cancel_func, cancel_baton, scratch_pool));
    }

  return svn_error_createf(SVN_ERR_NODE_UNKNOWN_KIND, NULL,
                           _("'%s' has an unrecognized node kind"),
                           svn_dirent_local_style(local_abspath,
                                                  scratch_pool));
}

svn_error_t *
svn_wc__node_walk_children(svn_wc_context_t *wc_ctx,
                           const char *local_abspath,
                           svn_boolean_t show_hidden,
                           svn_wc__node_found_func_t walk_callback,
                           void *walk_baton,
                           svn_depth_t walk_depth,
                           svn_cancel_func_t cancel_func,
                           void *cancel_baton,
                           apr_pool_t *scratch_pool)
{
  return svn_error_return(
    svn_wc__internal_walk_children(wc_ctx->db, local_abspath, show_hidden,
                                   walk_callback, walk_baton, walk_depth,
                                   cancel_func, cancel_baton, scratch_pool));
}

svn_error_t *
svn_wc__node_is_status_delete(svn_boolean_t *is_deleted,
                              svn_wc_context_t *wc_ctx,
                              const char *local_abspath,
                              apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));

  /* ### Do we need to consider svn_wc__db_status_obstructed_delete? */
  *is_deleted = (status == svn_wc__db_status_deleted);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_is_status_obstructed(svn_boolean_t *is_obstructed,
                                  svn_wc_context_t *wc_ctx,
                                  const char *local_abspath,
                                  apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));

  *is_obstructed = (status == svn_wc__db_status_obstructed) ||
                   (status == svn_wc__db_status_obstructed_add) ||
                   (status == svn_wc__db_status_obstructed_delete);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_is_status_absent(svn_boolean_t *is_absent,
                              svn_wc_context_t *wc_ctx,
                              const char *local_abspath,
                              apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));
  *is_absent = (status == svn_wc__db_status_absent);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_is_status_present(svn_boolean_t *is_present,
                               svn_wc_context_t *wc_ctx,
                               const char *local_abspath,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));
  *is_present = (status != svn_wc__db_status_not_present);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_is_status_added(svn_boolean_t *is_added,
                             svn_wc_context_t *wc_ctx,
                             const char *local_abspath,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;

  SVN_ERR(svn_wc__db_read_info(&status,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               wc_ctx->db, local_abspath,
                               scratch_pool, scratch_pool));
  *is_added = (status == svn_wc__db_status_added);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__node_get_base_rev(svn_revnum_t *base_revision,
                          svn_wc_context_t *wc_ctx,
                          const char *local_abspath,
                          svn_boolean_t scan_added,
                          apr_pool_t *scratch_pool)
{
  while (TRUE)
    {
      svn_wc__db_status_t status;
      svn_boolean_t base_shadowed;

      SVN_ERR(svn_wc__db_read_info(&status,
                                   NULL, base_revision,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, &base_shadowed,
                                   NULL, NULL,
                                   wc_ctx->db, local_abspath,
                                   scratch_pool, scratch_pool));

      if (SVN_IS_VALID_REVNUM(*base_revision))
        return SVN_NO_ERROR;

      /* First check if we have a base */
      if (base_shadowed)
        {
          /* The node was replaced with something else. Look at the base */
          return svn_error_return(
              svn_wc__db_base_get_info(NULL, NULL, base_revision, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       wc_ctx->db, local_abspath,
                                       scratch_pool, scratch_pool));
        }

      if (!scan_added)
        return SVN_NO_ERROR;

      /* Ok, and now the fun begins */

      if (status == svn_wc__db_status_added ||
          status == svn_wc__db_status_obstructed_add)
        {
          /* We have an addition. Let's look at the root of the addition */
          const char *check_abspath;
          SVN_ERR(svn_wc__db_scan_addition(NULL, &check_abspath, NULL, NULL,
                                           NULL, NULL, NULL, NULL, NULL,
                                           wc_ctx->db, local_abspath,
                                           scratch_pool, scratch_pool));

          if (check_abspath != NULL &&
              strcmp(check_abspath, local_abspath) != 0)
            {
              /* Check the root of the addition, it might be replaced */
              local_abspath = check_abspath;
            }
          else
            {
              /* The parent was not replaced, check the parent to which this
                 node was added */
              SVN_ERR_ASSERT(!svn_dirent_is_root(local_abspath,
                                                 strlen(local_abspath)));

              local_abspath = svn_dirent_dirname(local_abspath, scratch_pool);
            }

          continue; /* Restart at local_abspath */
        }

      return svn_error_create(SVN_ERR_WC_PATH_UNEXPECTED_STATUS, NULL, NULL);
  }
}
