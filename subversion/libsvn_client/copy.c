/*
 * copy.c:  copy/move wrappers around wc 'copy' functionality.
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
#include "svn_client.h"
#include "svn_error.h"
#include "svn_error_codes.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_opt.h"
#include "svn_time.h"
#include "svn_props.h"
#include "svn_mergeinfo.h"
#include "svn_pools.h"

#include "client.h"
#include "mergeinfo.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"
#include "private/svn_mergeinfo_private.h"


/*
 * OUR BASIC APPROACH TO COPIES
 * ============================
 *
 * for each source/destination pair
 *   if (not exist src_path)
 *     return ERR_BAD_SRC error
 *
 *   if (exist dst_path)
 *     return ERR_OBSTRUCTION error
 *   else
 *     copy src_path into parent_of_dst_path as basename (dst_path)
 *
 *   if (this is a move)
 *     delete src_path
 */



/*** Code. ***/

/* Obtain the implied mergeinfo and the existing mergeinfo of the
   source path, combine them and return the result in
   *TARGET_MERGEINFO.  One of LOCAL_ABSPATH and SRC_URL must be valid,
   the other must be NULL. */
static svn_error_t *
calculate_target_mergeinfo(svn_ra_session_t *ra_session,
                           apr_hash_t **target_mergeinfo,
                           const char *local_abspath,
                           const char *src_url,
                           svn_revnum_t src_revnum,
                           svn_client_ctx_t *ctx,
                           apr_pool_t *pool)
{
  svn_boolean_t locally_added = FALSE;
  apr_hash_t *src_mergeinfo = NULL;

  SVN_ERR_ASSERT((local_abspath && !src_url) || (!local_abspath && src_url));

  /* If we have a schedule-add WC path (which was not copied from
     elsewhere), it doesn't have any repository mergeinfo, so don't
     bother checking. */
  if (local_abspath)
    {
      svn_boolean_t is_added;
      const char *copyfrom_url;

      SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

      SVN_ERR(svn_wc__node_is_added(&is_added, ctx->wc_ctx,
                                    local_abspath, pool));
      if (is_added)
        SVN_ERR(svn_wc__node_get_copyfrom_info(&copyfrom_url, NULL, NULL,
                                               ctx->wc_ctx, local_abspath,
                                               pool, pool));

      if (is_added && !copyfrom_url)
        {
          locally_added = TRUE;
        }
      else
        {
          SVN_ERR(svn_client__entry_location(&src_url, &src_revnum,
                                             ctx->wc_ctx, local_abspath,
                                             svn_opt_revision_working,
                                             pool, pool));
        }
    }

  if (! locally_added)
    {
      /* Fetch any existing (explicit) mergeinfo.  We'll temporarily
         reparent to the target URL here, just to keep the code simple.
         We could, as an alternative, first see if the target URL was a
         child of the session URL and use the relative "remainder",
         falling back to this reparenting as necessary.  */
      const char *old_session_url = NULL;
      SVN_ERR(svn_client__ensure_ra_session_url(&old_session_url,
                                                ra_session, src_url, pool));
      SVN_ERR(svn_client__get_repos_mergeinfo(ra_session, &src_mergeinfo,
                                              "", src_revnum,
                                              svn_mergeinfo_inherited,
                                              TRUE, pool));
      if (old_session_url)
        SVN_ERR(svn_ra_reparent(ra_session, old_session_url, pool));
    }

  *target_mergeinfo = src_mergeinfo;
  return SVN_NO_ERROR;
}

/* Extend the mergeinfo for the single WC path TARGET_WCPATH, adding
   MERGEINFO to any mergeinfo pre-existing in the WC. */
static svn_error_t *
extend_wc_mergeinfo(const char *target_abspath,
                    apr_hash_t *mergeinfo,
                    svn_client_ctx_t *ctx,
                    apr_pool_t *pool)
{
  apr_hash_t *wc_mergeinfo;

  /* Get a fresh copy of the pre-existing state of the WC's mergeinfo
     updating it. */
  SVN_ERR(svn_client__parse_mergeinfo(&wc_mergeinfo, ctx->wc_ctx,
                                      target_abspath, pool, pool));

  /* Combine the provided mergeinfo with any mergeinfo from the WC. */
  if (wc_mergeinfo && mergeinfo)
    SVN_ERR(svn_mergeinfo_merge(wc_mergeinfo, mergeinfo, pool));
  else if (! wc_mergeinfo)
    wc_mergeinfo = mergeinfo;

  return svn_error_return(
    svn_client__record_wc_mergeinfo(target_abspath, wc_mergeinfo,
                                    FALSE, ctx, pool));
}

/* Find the longest common ancestor of paths in COPY_PAIRS.  If
   SRC_ANCESTOR is NULL, ignore source paths in this calculation.  If
   DST_ANCESTOR is NULL, ignore destination paths in this calculation.
   COMMON_ANCESTOR will be the common ancestor of both the
   SRC_ANCESTOR and DST_ANCESTOR, and will only be set if it is not
   NULL.
 */
static svn_error_t *
get_copy_pair_ancestors(const apr_array_header_t *copy_pairs,
                        const char **src_ancestor,
                        const char **dst_ancestor,
                        const char **common_ancestor,
                        apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create(pool);
  svn_client__copy_pair_t *first;
  const char *first_dst;
  const char *first_src;
  const char *top_dst;
  svn_boolean_t src_is_url;
  svn_boolean_t dst_is_url;
  char *top_src;
  int i;

  first = APR_ARRAY_IDX(copy_pairs, 0, svn_client__copy_pair_t *);

  /* Because all the destinations are in the same directory, we can easily
     determine their common ancestor. */
  first_dst = first->dst_abspath_or_url;
  dst_is_url = svn_path_is_url(first_dst);

  if (copy_pairs->nelts == 1)
    top_dst = apr_pstrdup(subpool, first_dst);
  else
    top_dst = dst_is_url ? svn_uri_dirname(first_dst, subpool)
                         : svn_dirent_dirname(first_dst, subpool);

  /* Sources can came from anywhere, so we have to actually do some
     work for them.  */
  first_src = first->src_abspath_or_url;
  src_is_url = svn_path_is_url(first_src);
  top_src = apr_pstrdup(subpool, first_src);
  for (i = 1; i < copy_pairs->nelts; i++)
    {
      /* We don't need to clear the subpool here for several reasons:
         1)  If we do, we can't use it to allocate the initial versions of
             top_src and top_dst (above).
         2)  We don't return any errors in the following loop, so we
             are guanteed to destroy the subpool at the end of this function.
         3)  The number of iterations is likely to be few, and the loop will
             be through quickly, so memory leakage will not be significant,
             in time or space.
      */
      const svn_client__copy_pair_t *pair =
        APR_ARRAY_IDX(copy_pairs, i, svn_client__copy_pair_t *);

      top_src = src_is_url
        ? svn_uri_get_longest_ancestor(top_src, pair->src_abspath_or_url,
                                       subpool)
        : svn_dirent_get_longest_ancestor(top_src, pair->src_abspath_or_url,
                                          subpool);
    }

  if (src_ancestor)
    *src_ancestor = apr_pstrdup(pool, top_src);

  if (dst_ancestor)
    *dst_ancestor = apr_pstrdup(pool, top_dst);

  if (common_ancestor)
    *common_ancestor =
               src_is_url
                    ? svn_uri_get_longest_ancestor(top_src, top_dst, pool)
                    : svn_dirent_get_longest_ancestor(top_src, top_dst, pool);

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}


struct do_wc_to_wc_copies_with_write_lock_baton {
  const apr_array_header_t *copy_pairs;
  svn_client_ctx_t *ctx;
  const char *dst_parent;
};

/* The guts of do_wc_to_wc_copies */
static svn_error_t *
do_wc_to_wc_copies_with_write_lock(void *baton,
                                   apr_pool_t *result_pool,
                                   apr_pool_t *scratch_pool)
{
  struct do_wc_to_wc_copies_with_write_lock_baton *b = baton;
  int i;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  svn_error_t *err = SVN_NO_ERROR;

  for (i = 0; i < b->copy_pairs->nelts; i++)
    {
      const char *dst_abspath;
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(b->copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      svn_pool_clear(iterpool);

      /* Check for cancellation */
      if (b->ctx->cancel_func)
        SVN_ERR(b->ctx->cancel_func(b->ctx->cancel_baton));

      /* Perform the copy */
      dst_abspath = svn_dirent_join(pair->dst_parent_abspath, pair->base_name,
                                    iterpool);
      err = svn_wc_copy3(b->ctx->wc_ctx, pair->src_abspath_or_url, dst_abspath,
                         b->ctx->cancel_func, b->ctx->cancel_baton,
                         b->ctx->notify_func2, b->ctx->notify_baton2, iterpool);
      if (err)
        break;
    }
  svn_pool_destroy(iterpool);

  svn_io_sleep_for_timestamps(b->dst_parent, scratch_pool);
  SVN_ERR(err);
  return SVN_NO_ERROR;
}

/* Copy each COPY_PAIR->SRC into COPY_PAIR->DST.  Use POOL for temporary
   allocations. */
static svn_error_t *
do_wc_to_wc_copies(const apr_array_header_t *copy_pairs,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool)
{
  const char *dst_parent, *dst_parent_abspath;
  struct do_wc_to_wc_copies_with_write_lock_baton baton;

  get_copy_pair_ancestors(copy_pairs, NULL, &dst_parent, NULL, pool);
  if (copy_pairs->nelts == 1)
    dst_parent = svn_dirent_dirname(dst_parent, pool);

  SVN_ERR(svn_dirent_get_absolute(&dst_parent_abspath, dst_parent, pool));

  baton.copy_pairs = copy_pairs;
  baton.ctx = ctx;
  baton.dst_parent = dst_parent;
  SVN_ERR(svn_wc__call_with_write_lock(do_wc_to_wc_copies_with_write_lock,
                                       &baton, ctx->wc_ctx, dst_parent_abspath,
                                       pool, pool));

  return SVN_NO_ERROR;
}

struct do_wc_to_wc_moves_with_locks_baton {
  svn_client_ctx_t *ctx;
  svn_client__copy_pair_t *pair;
  const char *dst_parent_abspath;
  svn_boolean_t lock_src;
  svn_boolean_t lock_dst;
};

/* The locked bit of do_wc_to_wc_moves. */
static svn_error_t *
do_wc_to_wc_moves_with_locks2(void *baton,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  struct do_wc_to_wc_moves_with_locks_baton *b = baton;
  const char *dst_abspath;

  dst_abspath = svn_dirent_join(b->dst_parent_abspath, b->pair->base_name,
                                scratch_pool);

  SVN_ERR(svn_wc_copy3(b->ctx->wc_ctx, b->pair->src_abspath_or_url,
                       dst_abspath,
                       b->ctx->cancel_func, b->ctx->cancel_baton,
                       b->ctx->notify_func2, b->ctx->notify_baton2,
                       scratch_pool));

  SVN_ERR(svn_wc_delete4(b->ctx->wc_ctx, b->pair->src_abspath_or_url,
                         FALSE, FALSE,
                         b->ctx->cancel_func, b->ctx->cancel_baton,
                         b->ctx->notify_func2, b->ctx->notify_baton2,
                         scratch_pool));

  return SVN_NO_ERROR;
}

/* Wrapper to add an optional second lock */
static svn_error_t *
do_wc_to_wc_moves_with_locks1(void *baton,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  struct do_wc_to_wc_moves_with_locks_baton *b = baton;

  if (b->lock_dst)
    SVN_ERR(svn_wc__call_with_write_lock(do_wc_to_wc_moves_with_locks2, b,
                                         b->ctx->wc_ctx, b->dst_parent_abspath,
                                         result_pool, scratch_pool));
  else
    SVN_ERR(do_wc_to_wc_moves_with_locks2(b, result_pool, scratch_pool));

  return SVN_NO_ERROR;
}

/* Move each COPY_PAIR->SRC into COPY_PAIR->DST, deleting COPY_PAIR->SRC
   afterwards.  Use POOL for temporary allocations. */
static svn_error_t *
do_wc_to_wc_moves(const apr_array_header_t *copy_pairs,
                  const char *dst_path,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool)
{
  int i;
  apr_pool_t *iterpool = svn_pool_create(pool);
  svn_error_t *err = SVN_NO_ERROR;

  for (i = 0; i < copy_pairs->nelts; i++)
    {
      const char *src_parent_abspath;
      struct do_wc_to_wc_moves_with_locks_baton baton;

      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      svn_pool_clear(iterpool);

      /* Check for cancellation */
      if (ctx->cancel_func)
        SVN_ERR(ctx->cancel_func(ctx->cancel_baton));

      src_parent_abspath = svn_dirent_dirname(pair->src_abspath_or_url,
                                              iterpool);

      /* We now need to lock the right combination of batons.
         Four cases:
           1) src_parent == dst_parent
           2) src_parent is parent of dst_parent
           3) dst_parent is parent of src_parent
           4) src_parent and dst_parent are disjoint
         We can handle 1) as either 2) or 3) */
      if (strcmp(src_parent_abspath, pair->dst_parent_abspath) == 0
          || svn_dirent_is_child(src_parent_abspath, pair->dst_parent_abspath,
                                 iterpool))
        {
          baton.lock_src = TRUE;
          baton.lock_dst = FALSE;
        }
      else if (svn_dirent_is_child(pair->dst_parent_abspath, src_parent_abspath,
                                   iterpool))
        {
          baton.lock_src = FALSE;
          baton.lock_dst = TRUE;
        }
      else
        {
          baton.lock_src = TRUE;
          baton.lock_dst = TRUE;
        }

      /* Perform the copy and then the delete. */
      baton.ctx = ctx;
      baton.pair = pair;
      baton.dst_parent_abspath = pair->dst_parent_abspath;
      if (baton.lock_src)
        SVN_ERR(svn_wc__call_with_write_lock(do_wc_to_wc_moves_with_locks1,
                                             &baton,
                                             ctx->wc_ctx, src_parent_abspath,
                                             iterpool, iterpool));
      else
        SVN_ERR(do_wc_to_wc_moves_with_locks1(&baton, iterpool, iterpool));

    }
  svn_pool_destroy(iterpool);

  svn_io_sleep_for_timestamps(dst_path, pool);

  return svn_error_return(err);
}


static svn_error_t *
wc_to_wc_copy(const apr_array_header_t *copy_pairs,
              const char *dst_path,
              svn_boolean_t is_move,
              svn_boolean_t make_parents,
              svn_client_ctx_t *ctx,
              apr_pool_t *pool)
{
  int i;
  apr_pool_t *iterpool = svn_pool_create(pool);

  /* Check that all of our SRCs exist, and all the DSTs don't. */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      svn_node_kind_t dst_kind, dst_parent_kind;

      svn_pool_clear(iterpool);

      /* Verify that SRC_PATH exists. */
      SVN_ERR(svn_io_check_path(pair->src_abspath_or_url, &pair->src_kind,
                                iterpool));
      if (pair->src_kind == svn_node_none)
        return svn_error_createf(
          SVN_ERR_NODE_UNKNOWN_KIND, NULL,
          _("Path '%s' does not exist"),
          svn_dirent_local_style(pair->src_abspath_or_url, pool));

      /* If DST_PATH does not exist, then its basename will become a new
         file or dir added to its parent (possibly an implicit '.').
         Else, just error out. */
      SVN_ERR(svn_io_check_path(pair->dst_abspath_or_url, &dst_kind,
                                iterpool));
      if (dst_kind != svn_node_none)
        return svn_error_createf(
          SVN_ERR_ENTRY_EXISTS, NULL,
          _("Path '%s' already exists"),
          svn_dirent_local_style(pair->dst_abspath_or_url, pool));

      svn_dirent_split(&pair->dst_parent_abspath, &pair->base_name,
                       pair->dst_abspath_or_url, pool);

      /* Make sure the destination parent is a directory and produce a clear
         error message if it is not. */
      SVN_ERR(svn_io_check_path(pair->dst_parent_abspath, &dst_parent_kind,
                                iterpool));
      if (make_parents && dst_parent_kind == svn_node_none)
        {
          SVN_ERR(svn_client__make_local_parents(pair->dst_parent_abspath,
                                                 TRUE, ctx, iterpool));
        }
      else if (dst_parent_kind != svn_node_dir)
        {
          return svn_error_createf(SVN_ERR_WC_NOT_WORKING_COPY, NULL,
                                   _("Path '%s' is not a directory"),
                                   svn_dirent_local_style(
                                     pair->dst_parent_abspath, pool));
        }
    }

  svn_pool_destroy(iterpool);

  /* Copy or move all targets. */
  if (is_move)
    return do_wc_to_wc_moves(copy_pairs, dst_path, ctx, pool);
  else
    return do_wc_to_wc_copies(copy_pairs, ctx, pool);
}


/* Path-specific state used as part of path_driver_cb_baton. */
typedef struct
{
  const char *src_url;
  const char *src_path;
  const char *dst_path;
  svn_node_kind_t src_kind;
  svn_revnum_t src_revnum;
  svn_boolean_t resurrection;
  svn_boolean_t dir_add;
  svn_string_t *mergeinfo;  /* the new mergeinfo for the target */
} path_driver_info_t;


/* The baton used with the path_driver_cb_func() callback for a copy
   or move operation. */
struct path_driver_cb_baton
{
  /* The editor (and its state) used to perform the operation. */
  const svn_delta_editor_t *editor;
  void *edit_baton;

  /* A hash of path -> path_driver_info_t *'s. */
  apr_hash_t *action_hash;

  /* Whether the operation is a move or copy. */
  svn_boolean_t is_move;
};

static svn_error_t *
path_driver_cb_func(void **dir_baton,
                    void *parent_baton,
                    void *callback_baton,
                    const char *path,
                    apr_pool_t *pool)
{
  struct path_driver_cb_baton *cb_baton = callback_baton;
  svn_boolean_t do_delete = FALSE, do_add = FALSE;
  path_driver_info_t *path_info = apr_hash_get(cb_baton->action_hash,
                                               path,
                                               APR_HASH_KEY_STRING);

  /* Initialize return value. */
  *dir_baton = NULL;

  /* This function should never get an empty PATH.  We can neither
     create nor delete the empty PATH, so if someone is calling us
     with such, the code is just plain wrong. */
  SVN_ERR_ASSERT(! svn_path_is_empty(path));

  /* Check to see if we need to add the path as a directory. */
  if (path_info->dir_add)
    {
      return cb_baton->editor->add_directory(path, parent_baton, NULL,
                                             SVN_INVALID_REVNUM, pool,
                                             dir_baton);
    }

  /* If this is a resurrection, we know the source and dest paths are
     the same, and that our driver will only be calling us once.  */
  if (path_info->resurrection)
    {
      /* If this is a move, we do nothing.  Otherwise, we do the copy.  */
      if (! cb_baton->is_move)
        do_add = TRUE;
    }
  /* Not a resurrection. */
  else
    {
      /* If this is a move, we check PATH to see if it is the source
         or the destination of the move. */
      if (cb_baton->is_move)
        {
          if (strcmp(path_info->src_path, path) == 0)
            do_delete = TRUE;
          else
            do_add = TRUE;
        }
      /* Not a move?  This must just be the copy addition. */
      else
        {
          do_add = TRUE;
        }
    }

  if (do_delete)
    {
      SVN_ERR(cb_baton->editor->delete_entry(path, SVN_INVALID_REVNUM,
                                             parent_baton, pool));
    }
  if (do_add)
    {
      SVN_ERR(svn_path_check_valid(path, pool));

      if (path_info->src_kind == svn_node_file)
        {
          void *file_baton;
          SVN_ERR(cb_baton->editor->add_file(path, parent_baton,
                                             path_info->src_url,
                                             path_info->src_revnum,
                                             pool, &file_baton));
          if (path_info->mergeinfo)
            SVN_ERR(cb_baton->editor->change_file_prop(file_baton,
                                                       SVN_PROP_MERGEINFO,
                                                       path_info->mergeinfo,
                                                       pool));
          SVN_ERR(cb_baton->editor->close_file(file_baton, NULL, pool));
        }
      else
        {
          SVN_ERR(cb_baton->editor->add_directory(path, parent_baton,
                                                  path_info->src_url,
                                                  path_info->src_revnum,
                                                  pool, dir_baton));
          if (path_info->mergeinfo)
            SVN_ERR(cb_baton->editor->change_dir_prop(*dir_baton,
                                                      SVN_PROP_MERGEINFO,
                                                      path_info->mergeinfo,
                                                      pool));
        }
    }
  return SVN_NO_ERROR;
}


/* Starting with the path DIR relative to the RA_SESSION's session
   URL, work up through DIR's parents until an existing node is found.
   Push each nonexistent path onto the array NEW_DIRS, allocating in
   POOL.  Raise an error if the existing node is not a directory.

   ### Multiple requests for HEAD (SVN_INVALID_REVNUM) make this
   ### implementation susceptible to race conditions.  */
static svn_error_t *
find_absent_parents1(svn_ra_session_t *ra_session,
                     const char *dir,
                     apr_array_header_t *new_dirs,
                     apr_pool_t *pool)
{
  svn_node_kind_t kind;
  apr_pool_t *iterpool = svn_pool_create(pool);

  SVN_ERR(svn_ra_check_path(ra_session, dir, SVN_INVALID_REVNUM, &kind,
                            iterpool));

  while (kind == svn_node_none)
    {
      svn_pool_clear(iterpool);

      APR_ARRAY_PUSH(new_dirs, const char *) = dir;
      dir = svn_dirent_dirname(dir, pool);

      SVN_ERR(svn_ra_check_path(ra_session, dir, SVN_INVALID_REVNUM,
                                &kind, iterpool));
    }

  if (kind != svn_node_dir)
    return svn_error_createf(SVN_ERR_FS_ALREADY_EXISTS, NULL,
                             _("Path '%s' already exists, but is not a "
                               "directory"), dir);

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

/* Starting with the URL *TOP_DST_URL which is also the root of
   RA_SESSION, work up through its parents until an existing node is
   found. Push each nonexistent URL onto the array NEW_DIRS,
   allocating in POOL.  Raise an error if the existing node is not a
   directory.

   Set *TOP_DST_URL and the RA session's root to the existing node's URL.

   ### Multiple requests for HEAD (SVN_INVALID_REVNUM) make this
   ### implementation susceptible to race conditions.  */
static svn_error_t *
find_absent_parents2(svn_ra_session_t *ra_session,
                     const char **top_dst_url,
                     apr_array_header_t *new_dirs,
                     apr_pool_t *pool)
{
  const char *root_url = *top_dst_url;
  svn_node_kind_t kind;

  SVN_ERR(svn_ra_check_path(ra_session, "", SVN_INVALID_REVNUM, &kind,
                            pool));

  while (kind == svn_node_none)
    {
      APR_ARRAY_PUSH(new_dirs, const char *) = root_url;
      root_url = svn_uri_dirname(root_url, pool);

      SVN_ERR(svn_ra_reparent(ra_session, root_url, pool));
      SVN_ERR(svn_ra_check_path(ra_session, "", SVN_INVALID_REVNUM, &kind,
                                pool));
    }

  if (kind != svn_node_dir)
    return svn_error_createf(SVN_ERR_FS_ALREADY_EXISTS, NULL,
                _("Path '%s' already exists, but is not a directory"),
                root_url);

  *top_dst_url = root_url;
  return SVN_NO_ERROR;
}

static svn_error_t *
repos_to_repos_copy(svn_commit_info_t **commit_info_p,
                    const apr_array_header_t *copy_pairs,
                    svn_boolean_t make_parents,
                    const apr_hash_t *revprop_table,
                    svn_client_ctx_t *ctx,
                    svn_boolean_t is_move,
                    apr_pool_t *pool)
{
  svn_error_t *err;
  apr_array_header_t *paths = apr_array_make(pool, 2 * copy_pairs->nelts,
                                             sizeof(const char *));
  apr_hash_t *action_hash = apr_hash_make(pool);
  apr_array_header_t *path_infos;
  const char *top_url, *top_url_all, *top_url_dst;
  const char *message, *repos_root, *ignored_url;
  svn_revnum_t youngest = SVN_INVALID_REVNUM;
  svn_ra_session_t *ra_session = NULL;
  const svn_delta_editor_t *editor;
  void *edit_baton;
  void *commit_baton;
  struct path_driver_cb_baton cb_baton;
  apr_array_header_t *new_dirs = NULL;
  apr_hash_t *commit_revprops;
  int i;
  svn_client__copy_pair_t *first_pair =
    APR_ARRAY_IDX(copy_pairs, 0, svn_client__copy_pair_t *);

  /* Open an RA session to the first copy pair's destination.  We'll
     be verifying that every one of our copy source and destination
     URLs is or is beneath this sucker's repository root URL as a form
     of a cheap(ish) sanity check.  */
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session,
                                               first_pair->src_abspath_or_url,
                                               NULL, NULL, FALSE, TRUE,
                                               ctx, pool));
  SVN_ERR(svn_ra_get_repos_root2(ra_session, &repos_root, pool));

  /* Verify that sources and destinations are all at or under
     REPOS_ROOT.  While here, create a path_info struct for each
     src/dst pair and initialize portions of it with normalized source
     location information.  */
  path_infos = apr_array_make(pool, copy_pairs->nelts,
                              sizeof(path_driver_info_t *));
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      path_driver_info_t *info = apr_pcalloc(pool, sizeof(*info));
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      apr_hash_t *mergeinfo;
      svn_opt_revision_t *src_rev, *ignored_rev, dead_end_rev;
      dead_end_rev.kind = svn_opt_revision_unspecified;

      /* Are the source and destination URLs at or under REPOS_ROOT? */
      if (! (svn_uri_is_ancestor(repos_root, pair->src_abspath_or_url)
             && svn_uri_is_ancestor(repos_root, pair->dst_abspath_or_url)))
        return svn_error_create
          (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
           _("Source and destination URLs appear not to all point to the "
             "same repository."));

      /* Resolve revision keywords and such into real revision number,
         passing NULL for the path (to ensure error if trying to get a
         revision based on the working copy). */
      SVN_ERR(svn_client__get_revision_number(&pair->src_revnum, &youngest,
                                              ctx->wc_ctx, NULL,
                                              ra_session,
                                              &pair->src_op_revision, pool));

      /* Run the history function to get the source's URL in the
         operational revision. */
      SVN_ERR(svn_client__ensure_ra_session_url(&ignored_url, ra_session,
                                                pair->src_abspath_or_url,
                                                pool));
      SVN_ERR(svn_client__repos_locations(&pair->src_abspath_or_url, &src_rev,
                                          &ignored_url, &ignored_rev,
                                          ra_session,
                                          pair->src_abspath_or_url,
                                          &pair->src_peg_revision,
                                          &pair->src_op_revision,
                                          &dead_end_rev, ctx, pool));

      /* Go ahead and grab mergeinfo from the source, too. */
      SVN_ERR(svn_client__ensure_ra_session_url(&ignored_url, ra_session,
                                                pair->src_abspath_or_url,
                                                pool));
      SVN_ERR(calculate_target_mergeinfo(ra_session, &mergeinfo, NULL,
                                         pair->src_abspath_or_url,
                                         pair->src_revnum, ctx, pool));
      if (mergeinfo)
        SVN_ERR(svn_mergeinfo_to_string(&info->mergeinfo, mergeinfo, pool));

      /* Plop an INFO structure onto our array thereof. */
      info->src_url = pair->src_abspath_or_url;
      info->src_revnum = pair->src_revnum;
      info->resurrection = FALSE;
      APR_ARRAY_PUSH(path_infos, path_driver_info_t *) = info;
    }

  /* If this is a move, we have to open our session to the longest
     path common to all SRC_URLS and DST_URLS in the repository so we
     can do existence checks on all paths, and so we can operate on
     all paths in the case of a move.  But if this is *not* a move,
     then opening our session at the longest path common to sources
     *and* destinations might be an optimization when the user is
     authorized to access all that stuff, but could cause the
     operation to fail altogether otherwise.  See issue #3242.  */
  get_copy_pair_ancestors(copy_pairs, NULL, &top_url_dst, &top_url_all, pool);
  top_url = is_move ? top_url_all : top_url_dst;

  /* Check each src/dst pair for resurrection, and verify that TOP_URL
     is anchored high enough to cover all the editor_t activities
     required for this operation.  */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      path_driver_info_t *info = APR_ARRAY_IDX(path_infos, i,
                                               path_driver_info_t *);

      /* Source and destination are the same?  It's a resurrection. */
      if (strcmp(pair->src_abspath_or_url, pair->dst_abspath_or_url) == 0)
        info->resurrection = TRUE;

      /* We need to add each dst_URL, and (in a move) we'll need to
         delete each src_URL.  Our selection of TOP_URL so far ensures
         that all our destination URLs (and source URLs, for moves)
         are at least as deep as TOP_URL, but we need to make sure
         that TOP_URL is an *ancestor* of all our to-be-edited paths.

         Issue #683 is demonstrates this scenario.  If you're
         resurrecting a deleted item like this: 'svn cp -rN src_URL
         dst_URL', then src_URL == dst_URL == top_url.  In this
         situation, we want to open an RA session to be at least the
         *parent* of all three. */
      if ((strcmp(top_url, pair->dst_abspath_or_url) == 0)
          && (strcmp(top_url, repos_root) != 0))
        {
          top_url = svn_uri_dirname(top_url, pool);
        }
      if (is_move
          && (strcmp(top_url, pair->src_abspath_or_url) == 0)
          && (strcmp(top_url, repos_root) != 0))
        {
          top_url = svn_uri_dirname(top_url, pool);
        }
    }

  /* Point the RA session to our current TOP_URL. */
  SVN_ERR(svn_client__ensure_ra_session_url(&ignored_url, ra_session,
                                            top_url, pool));

  /* If we're allowed to create nonexistent parent directories of our
     destinations, then make a list in NEW_DIRS of the parent
     directories of the destination that don't yet exist.  */
  if (make_parents)
    {
      const char *dir;

      new_dirs = apr_array_make(pool, 0, sizeof(const char *));

      /* If this is a move, TOP_URL is at least the common ancestor of
         all the paths (sources and destinations) involved.  Assuming
         the sources exist (which is fair, because if they don't, this
         whole operation will fail anyway), TOP_URL must also exist.
         So it's the paths between TOP_URL and the destinations which
         we have to check for existence.  But here, we take advantage
         of the knowledge of our caller.  We know that if there are
         multiple copy/move operations being requested, then the
         destinations of the copies/moves will all be siblings of one
         another.  Therefore, we need only to check for the
         nonexistent paths between TOP_URL and *one* of our
         destinations to find nonexistent parents of all of them.  */
      if (is_move)
        {
          /* Imagine a situation where the user tries to copy an
             existing source directory to nonexistent directory with
             --parents options specified:

                svn copy --parents URL/src URL/dst

             where src exists and dst does not.  The svn_uri_dirname()
             call above will produce a string equivalent to TOP_URL,
             which means svn_uri_is_child() will return NULL.  In this
             case, do not try to add dst to the NEW_DIRS list since it
             will be added to the commit items array later in this
             function. */
          dir = svn_uri_is_child(
                  top_url,
                  svn_uri_dirname(first_pair->dst_abspath_or_url, pool),
                  pool);
          if (dir)
            SVN_ERR(find_absent_parents1(ra_session,
                                         svn_path_uri_decode(dir, pool),
                                         new_dirs, pool));
        }
      /* If, however, this is *not* a move, TOP_URL only points to the
         common ancestor of our destination path(s), or possibly one
         level higher.  We'll need to do an existence crawl toward the
         root of the repository, starting with one of our destinations
         (see "... take advantage of the knowledge of our caller ..."
         above), and possibly adjusting TOP_URL as we go. */
      else
        {
          apr_array_header_t *new_urls =
            apr_array_make(pool, 0, sizeof(const char *));
          SVN_ERR(find_absent_parents2(ra_session, &top_url, new_urls, pool));

          /* Convert absolute URLs into URLs relative to TOP_URL. */
          for (i = 0; i < new_urls->nelts; i++)
            {
              const char *new_url = APR_ARRAY_IDX(new_urls, i, const char *);
              dir = svn_uri_is_child(top_url, new_url, pool);
              APR_ARRAY_PUSH(new_dirs, const char *) = dir ? dir : "";
            }
        }
    }

  /* For each src/dst pair, check to see if that SRC_URL is a child of
     the DST_URL (excepting the case where DST_URL is the repo root).
     If it is, and the parent of DST_URL is the current TOP_URL, then we
     need to reparent the session one directory higher, the parent of
     the DST_URL. */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      path_driver_info_t *info = APR_ARRAY_IDX(path_infos, i,
                                               path_driver_info_t *);

      if ((strcmp(pair->dst_abspath_or_url, repos_root) != 0)
          && (svn_uri_is_child(pair->dst_abspath_or_url,
                               pair->src_abspath_or_url, pool) != NULL))
        {
          info->resurrection = TRUE;
          top_url = svn_uri_dirname(top_url, pool);
          SVN_ERR(svn_ra_reparent(ra_session, top_url, pool));
        }
    }

  /* Get the portions of the SRC and DST URLs that are relative to
     TOP_URL (URI-decoding them while we're at it), verify that the
     source exists and the proposed destination does not, and toss
     what we've learned into the INFO array.  (For copies -- that is,
     non-moves -- the relative source URL NULL because it isn't a
     child of the TOP_URL at all.  That's okay, we'll deal with
     it.)  */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      svn_client__copy_pair_t *pair =
        APR_ARRAY_IDX(copy_pairs, i, svn_client__copy_pair_t *);
      path_driver_info_t *info =
        APR_ARRAY_IDX(path_infos, i, path_driver_info_t *);
      svn_node_kind_t dst_kind;
      const char *src_rel, *dst_rel;

      src_rel = svn_uri_is_child(top_url, pair->src_abspath_or_url, pool);
      if (src_rel)
        {
          src_rel = svn_path_uri_decode(src_rel, pool);
          SVN_ERR(svn_ra_check_path(ra_session, src_rel, pair->src_revnum,
                                    &info->src_kind, pool));
        }
      else if (strcmp(pair->src_abspath_or_url, top_url) == 0)
        {
          if (is_move)
            return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                     _("Cannot move URL '%s' into itself"),
                                     pair->src_abspath_or_url);
          src_rel = "";
          SVN_ERR(svn_ra_check_path(ra_session, src_rel, pair->src_revnum,
                                    &info->src_kind, pool));
        }
      else
        {
          const char *old_url = NULL;

          src_rel = NULL;
          SVN_ERR_ASSERT(! is_move);

          SVN_ERR(svn_client__ensure_ra_session_url(&old_url, ra_session,
                                                    pair->src_abspath_or_url,
                                                    pool));
          SVN_ERR(svn_ra_check_path(ra_session, "", pair->src_revnum,
                                    &info->src_kind, pool));
          SVN_ERR(svn_ra_reparent(ra_session, old_url, pool));
        }
      if (info->src_kind == svn_node_none)
        return svn_error_createf(SVN_ERR_FS_NOT_FOUND, NULL,
                                 _("Path '%s' does not exist in revision %ld"),
                                 pair->src_abspath_or_url, pair->src_revnum);

      /* Figure out the basename that will result from this operation,
         and ensure that we aren't trying to overwrite existing paths.  */
      dst_rel = svn_uri_is_child(top_url, pair->dst_abspath_or_url, pool);
      if (dst_rel)
        dst_rel = svn_path_uri_decode(dst_rel, pool);
      else
        dst_rel = "";
      SVN_ERR(svn_ra_check_path(ra_session, dst_rel, youngest,
                                &dst_kind, pool));
      if (dst_kind != svn_node_none)
        return svn_error_createf(SVN_ERR_FS_ALREADY_EXISTS, NULL,
                                 _("Path '%s' already exists"), dst_rel);

      /* More info for our INFO structure.  */
      info->src_path = src_rel;
      info->dst_path = dst_rel;
    }

  if (SVN_CLIENT__HAS_LOG_MSG_FUNC(ctx))
    {
      /* Produce a list of new paths to add, and provide it to the
         mechanism used to acquire a log message. */
      svn_client_commit_item3_t *item;
      const char *tmp_file;
      apr_array_header_t *commit_items
        = apr_array_make(pool, 2 * copy_pairs->nelts, sizeof(item));

      /* Add any intermediate directories to the message */
      if (make_parents)
        {
          for (i = 0; i < new_dirs->nelts; i++)
            {
              const char *url = APR_ARRAY_IDX(new_dirs, i, const char *);

              item = svn_client_commit_item3_create(pool);
              item->url = svn_uri_join(top_url, url, pool);
              item->state_flags = SVN_CLIENT_COMMIT_ITEM_ADD;
              APR_ARRAY_PUSH(commit_items, svn_client_commit_item3_t *) = item;
            }
        }

      for (i = 0; i < path_infos->nelts; i++)
        {
          path_driver_info_t *info = APR_ARRAY_IDX(path_infos, i,
                                                   path_driver_info_t *);

          item = svn_client_commit_item3_create(pool);
          item->url = svn_uri_join(top_url, info->dst_path, pool);
          item->state_flags = SVN_CLIENT_COMMIT_ITEM_ADD;
          APR_ARRAY_PUSH(commit_items, svn_client_commit_item3_t *) = item;
          apr_hash_set(action_hash, info->dst_path, APR_HASH_KEY_STRING,
                       info);

          if (is_move && (! info->resurrection))
            {
              item = apr_pcalloc(pool, sizeof(*item));
              item->url = svn_uri_join(top_url, info->src_path, pool);
              item->state_flags = SVN_CLIENT_COMMIT_ITEM_DELETE;
              APR_ARRAY_PUSH(commit_items, svn_client_commit_item3_t *) = item;
              apr_hash_set(action_hash, info->src_path, APR_HASH_KEY_STRING,
                           info);
            }
        }

      SVN_ERR(svn_client__get_log_msg(&message, &tmp_file, commit_items,
                                      ctx, pool));
      if (! message)
        return SVN_NO_ERROR;
    }
  else
    message = "";

  /* Setup our PATHS for the path-based editor drive. */
  /* First any intermediate directories. */
  if (make_parents)
    {
      for (i = 0; i < new_dirs->nelts; i++)
        {
          const char *url = APR_ARRAY_IDX(new_dirs, i, const char *);
          path_driver_info_t *info = apr_pcalloc(pool, sizeof(*info));

          info->dst_path = url;
          info->dir_add = TRUE;

          APR_ARRAY_PUSH(paths, const char *) = url;
          apr_hash_set(action_hash, url, APR_HASH_KEY_STRING, info);
        }
    }

  /* Then our copy destinations and move sources (if any). */
  for (i = 0; i < path_infos->nelts; i++)
    {
      path_driver_info_t *info = APR_ARRAY_IDX(path_infos, i,
                                               path_driver_info_t *);

      APR_ARRAY_PUSH(paths, const char *) = info->dst_path;
      if (is_move && (! info->resurrection))
        APR_ARRAY_PUSH(paths, const char *) = info->src_path;
    }

  SVN_ERR(svn_client__ensure_revprop_table(&commit_revprops, revprop_table,
                                           message, ctx, pool));

  /* Fetch RA commit editor. */
  SVN_ERR(svn_client__commit_get_baton(&commit_baton, commit_info_p, pool));
  SVN_ERR(svn_ra_get_commit_editor3(ra_session, &editor, &edit_baton,
                                    commit_revprops,
                                    svn_client__commit_callback,
                                    commit_baton,
                                    NULL, TRUE, /* No lock tokens */
                                    pool));

  /* Setup the callback baton. */
  cb_baton.editor = editor;
  cb_baton.edit_baton = edit_baton;
  cb_baton.action_hash = action_hash;
  cb_baton.is_move = is_move;

  /* Call the path-based editor driver. */
  err = svn_delta_path_driver(editor, edit_baton, youngest, paths,
                              path_driver_cb_func, &cb_baton, pool);
  if (err)
    {
      /* At least try to abort the edit (and fs txn) before throwing err. */
      svn_error_clear(editor->abort_edit(edit_baton, pool));
      return svn_error_return(err);
    }

  /* Close the edit. */
  return svn_error_return(editor->close_edit(edit_baton, pool));
}


/* ### Copy ...
 * COMMIT_INFO_P is ...
 * COPY_PAIRS is ...
 * MAKE_PARENTS is ...
 * REVPROP_TABLE is ...
 * CTX is ... */
static svn_error_t *
wc_to_repos_copy(svn_commit_info_t **commit_info_p,
                 const apr_array_header_t *copy_pairs,
                 svn_boolean_t make_parents,
                 const apr_hash_t *revprop_table,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool)
{
  const char *message;
  const char *top_src_path, *top_dst_url;
  const char *top_src_abspath;
  svn_ra_session_t *ra_session;
  const svn_delta_editor_t *editor;
  void *edit_baton;
  void *commit_baton;
  apr_hash_t *committables;
  apr_array_header_t *commit_items;
  apr_pool_t *iterpool;
  apr_array_header_t *new_dirs = NULL;
  apr_hash_t *commit_revprops;
  svn_client__copy_pair_t *first_pair;
  int i;

  /* Find the common root of all the source paths */
  get_copy_pair_ancestors(copy_pairs, &top_src_path, NULL, NULL, pool);

  /* Do we need to lock the working copy?  1.6 didn't take a write
     lock, but what happens if the working copy changes during the copy
     operation? */

  iterpool = svn_pool_create(pool);

  /* Verify that all the source paths exist, are versioned, etc.
     We'll do so by querying the base revisions of those things (which
     we'll need to know later anyway). */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      svn_pool_clear(iterpool);

      SVN_ERR(svn_wc__node_get_base_rev(&pair->src_revnum, ctx->wc_ctx,
                                        pair->src_abspath_or_url, iterpool));
    }

  /* Determine the longest common ancestor for the destinations, and open an RA
     session to that location. */
  /* ### But why start by getting the _parent_ of the first one? */
  /* --- That works because multiple destinations always point to the same
   *     directory. I'm rather wondering why we need to find a common
   *     destination parent here at all, instead of simply getting
   *     top_dst_url from get_copy_pair_ancestors() above?
   *     It looks like the entire block of code hanging off this comment
   *     is redundant. */
  first_pair = APR_ARRAY_IDX(copy_pairs, 0, svn_client__copy_pair_t *);
  top_dst_url = svn_uri_dirname(first_pair->dst_abspath_or_url, pool);
  for (i = 1; i < copy_pairs->nelts; i++)
    {
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      top_dst_url = svn_uri_get_longest_ancestor(top_dst_url,
                                                 pair->dst_abspath_or_url,
                                                 pool);
    }

  SVN_ERR(svn_dirent_get_absolute(&top_src_abspath, top_src_path, pool));
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, top_dst_url,
                                               top_src_abspath, NULL, TRUE,
                                               TRUE, ctx, pool));

  /* If requested, determine the nearest existing parent of the destination,
     and reparent the ra session there. */
  if (make_parents)
    {
      new_dirs = apr_array_make(pool, 0, sizeof(const char *));
      SVN_ERR(find_absent_parents2(ra_session, &top_dst_url, new_dirs, pool));
    }

  /* Figure out the basename that will result from each copy and check to make
     sure it doesn't exist already. */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      svn_node_kind_t dst_kind;
      const char *dst_rel;
      svn_client__copy_pair_t *pair =
        APR_ARRAY_IDX(copy_pairs, i, svn_client__copy_pair_t *);

      svn_pool_clear(iterpool);
      dst_rel = svn_path_uri_decode(svn_uri_is_child(top_dst_url,
                                                     pair->dst_abspath_or_url,
                                                     iterpool),
                                    iterpool);
      SVN_ERR(svn_ra_check_path(ra_session, dst_rel, SVN_INVALID_REVNUM,
                                &dst_kind, iterpool));
      if (dst_kind != svn_node_none)
        {
          return svn_error_createf(SVN_ERR_FS_ALREADY_EXISTS, NULL,
                                   _("Path '%s' already exists"),
                                   pair->dst_abspath_or_url);
        }
    }

  if (SVN_CLIENT__HAS_LOG_MSG_FUNC(ctx))
    {
      /* Produce a list of new paths to add, and provide it to the
         mechanism used to acquire a log message. */
      svn_client_commit_item3_t *item;
      const char *tmp_file;
      commit_items = apr_array_make(pool, copy_pairs->nelts, sizeof(item));

      /* Add any intermediate directories to the message */
      if (make_parents)
        {
          for (i = 0; i < new_dirs->nelts; i++)
            {
              const char *url = APR_ARRAY_IDX(new_dirs, i, const char *);

              item = svn_client_commit_item3_create(pool);
              item->url = url;
              item->state_flags = SVN_CLIENT_COMMIT_ITEM_ADD;
              APR_ARRAY_PUSH(commit_items, svn_client_commit_item3_t *) = item;
            }
        }

      for (i = 0; i < copy_pairs->nelts; i++)
        {
          svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                            svn_client__copy_pair_t *);

          item = svn_client_commit_item3_create(pool);
          item->url = pair->dst_abspath_or_url;
          item->state_flags = SVN_CLIENT_COMMIT_ITEM_ADD;
          APR_ARRAY_PUSH(commit_items, svn_client_commit_item3_t *) = item;
        }

      SVN_ERR(svn_client__get_log_msg(&message, &tmp_file, commit_items,
                                      ctx, pool));
      if (! message)
        {
          svn_pool_destroy(iterpool);
          return SVN_NO_ERROR;
        }
    }
  else
    message = "";

  SVN_ERR(svn_client__ensure_revprop_table(&commit_revprops, revprop_table,
                                           message, ctx, pool));

  /* Crawl the working copy for commit items. */
  SVN_ERR(svn_client__get_copy_committables(&committables,
                                            copy_pairs,
                                            ctx, pool));

  /* ### todo: There should be only one hash entry, which currently
     has a hacked name until we have the entries files storing
     canonical repository URLs.  Then, the hacked name can go away and
     be replaced with a entry->repos (or wherever the entry's
     canonical repos URL is stored). */
  if (! (commit_items = apr_hash_get(committables,
                                     SVN_CLIENT__SINGLE_REPOS_NAME,
                                     APR_HASH_KEY_STRING)))
    {
      return SVN_NO_ERROR;
    }

  /* If we are creating intermediate directories, tack them onto the list
     of committables. */
  if (make_parents)
    {
      for (i = 0; i < new_dirs->nelts; i++)
        {
          const char *url = APR_ARRAY_IDX(new_dirs, i, const char *);
          svn_client_commit_item3_t *item;

          item = svn_client_commit_item3_create(pool);
          item->url = url;
          item->state_flags = SVN_CLIENT_COMMIT_ITEM_ADD;
          item->incoming_prop_changes = apr_array_make(pool, 1,
                                                       sizeof(svn_prop_t *));
          APR_ARRAY_PUSH(commit_items, svn_client_commit_item3_t *) = item;
        }
    }

  /* ### TODO: This extra loop would be unnecessary if this code lived
     ### in svn_client__get_copy_committables(), which is incidentally
     ### only used above (so should really be in this source file). */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      apr_hash_t *mergeinfo, *wc_mergeinfo;
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      svn_client_commit_item3_t *item =
        APR_ARRAY_IDX(commit_items, i, svn_client_commit_item3_t *);

      svn_pool_clear(iterpool);

      /* Set the mergeinfo for the destination to the combined merge
         info known to the WC and the repository. */
      item->outgoing_prop_changes = apr_array_make(pool, 1,
                                                   sizeof(svn_prop_t *));
      SVN_ERR(calculate_target_mergeinfo(ra_session, &mergeinfo,
                                         pair->src_abspath_or_url,
                                         NULL, SVN_INVALID_REVNUM,
                                         ctx, iterpool));
      SVN_ERR(svn_client__parse_mergeinfo(&wc_mergeinfo, ctx->wc_ctx,
                                          pair->src_abspath_or_url,
                                          iterpool, iterpool));
      if (wc_mergeinfo && mergeinfo)
        SVN_ERR(svn_mergeinfo_merge(mergeinfo, wc_mergeinfo, iterpool));
      else if (! mergeinfo)
        mergeinfo = wc_mergeinfo;
      if (mergeinfo)
        {
          /* Push a mergeinfo prop representing MERGEINFO onto the
           * OUTGOING_PROP_CHANGES array. */

          svn_prop_t *mergeinfo_prop
            = apr_palloc(item->outgoing_prop_changes->pool,
                         sizeof(svn_prop_t));
          svn_string_t *prop_value;

          SVN_ERR(svn_mergeinfo_to_string(&prop_value, mergeinfo,
                                          item->outgoing_prop_changes->pool));

          mergeinfo_prop->name = SVN_PROP_MERGEINFO;
          mergeinfo_prop->value = prop_value;
          APR_ARRAY_PUSH(item->outgoing_prop_changes, svn_prop_t *)
            = mergeinfo_prop;
        }
    }

  /* Sort and condense our COMMIT_ITEMS. */
  SVN_ERR(svn_client__condense_commit_items(&top_dst_url,
                                            commit_items, pool));

  /* Open an RA session to DST_URL. */
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, top_dst_url,
                                               NULL, commit_items,
                                               FALSE, FALSE, ctx, pool));

  /* Fetch RA commit editor. */
  SVN_ERR(svn_client__commit_get_baton(&commit_baton, commit_info_p, pool));
  SVN_ERR(svn_ra_get_commit_editor3(ra_session, &editor, &edit_baton,
                                    commit_revprops,
                                    svn_client__commit_callback,
                                    commit_baton, NULL,
                                    TRUE, /* No lock tokens */
                                    pool));

  /* Perform the commit. */
  SVN_ERR_W(svn_client__do_commit(top_dst_url, commit_items,
                                  editor, edit_baton,
                                  0, /* ### any notify_path_offset needed? */
                                  NULL, NULL, NULL, ctx, pool),
            _("Commit failed (details follow):"));

  /* Sleep to ensure timestamp integrity. */
  svn_io_sleep_for_timestamps(top_src_path, pool);

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Peform each individual copy operation for a repos -> wc copy.  A
   helper for repos_to_wc_copy(). */
static svn_error_t *
repos_to_wc_copy_single(svn_client__copy_pair_t *pair,
                        svn_boolean_t same_repositories,
                        svn_boolean_t ignore_externals,
                        svn_ra_session_t *ra_session,
                        svn_client_ctx_t *ctx,
                        apr_pool_t *pool)
{
  svn_revnum_t src_revnum = pair->src_revnum;
  apr_hash_t *src_mergeinfo;
  const char *dst_abspath = pair->dst_abspath_or_url;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(dst_abspath));

  if (pair->src_kind == svn_node_dir)
    {
      SVN_ERR(svn_client__checkout_internal(NULL, pair->src_original,
                                            pair->dst_abspath_or_url,
                                            &pair->src_peg_revision,
                                            &pair->src_op_revision, NULL,
                                            svn_depth_infinity,
                                            ignore_externals, FALSE, TRUE,
                                            NULL, ctx, pool));

      /* Rewrite URLs recursively, remove wcprops, and mark everything
         as 'copied' -- assuming that the src and dst are from the
         same repository.  (It's kind of weird that svn_wc_add3() is the
         way to do this; see its doc for more about the controversy.) */
      if (same_repositories)
        {
          if (pair->src_op_revision.kind == svn_opt_revision_head)
            {
              /* If we just checked out from the "head" revision,
                 that's fine, but we don't want to pass a '-1' as a
                 copyfrom_rev to svn_wc_add3().  That function will
                 dump it right into the entry, and when we try to
                 commit later on, the 'add-dir-with-history' step will
                 be -very- unhappy; it only accepts specific
                 revisions.

                 On the other hand, we *could* say that -1 is a
                 legitimate copyfrom_rev, but I think that's bogus.
                 Somebody made a copy from a particular revision; if
                 they wait a long time to commit, it would be terrible
                 if the copied happened from a newer revision!! */

              /* We just did a checkout; whatever revision we just
                 got, that should be the copyfrom_revision when we
                 commit later. */
              SVN_ERR(svn_wc__node_get_base_rev(&src_revnum, ctx->wc_ctx,
                                                dst_abspath, pool));
            }

          /* Schedule dst_path for addition in parent, with copy history.
             (This function also recursively puts a 'copied' flag on every
             entry). */
          SVN_ERR(svn_wc_add4(ctx->wc_ctx, dst_abspath, svn_depth_infinity,
                              pair->src_abspath_or_url, src_revnum,
                              ctx->cancel_func, ctx->cancel_baton,
                              ctx->notify_func2, ctx->notify_baton2, pool));

          /* ### Recording of implied mergeinfo should really occur
             ### *before* the notification callback is invoked by
             ### svn_wc_add4(), but can't occur before we add the new
             ### source path. */
          SVN_ERR(calculate_target_mergeinfo(ra_session, &src_mergeinfo, NULL,
                                             pair->src_abspath_or_url,
                                             src_revnum, ctx, pool));
          SVN_ERR(extend_wc_mergeinfo(dst_abspath, src_mergeinfo, ctx, pool));
        }
      else  /* different repositories */
        {
          /* ### Someday, we would just call svn_wc_add4(), as above,
             but with no copyfrom args.  I.e. in the
             directory-foreign-UUID case, we still want everything
             scheduled for addition, URLs rewritten, and wcprop cache
             deleted, but WITHOUT any copied flags or copyfrom urls.
             Unfortunately, svn_wc_add4() is such a mess that it chokes
             at the moment when we pass a NULL copyfromurl. */

          return svn_error_createf
            (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
             _("Source URL '%s' is from foreign repository; "
               "leaving it as a disjoint WC"), pair->src_abspath_or_url);
        }
    } /* end directory case */

  else if (pair->src_kind == svn_node_file)
    {
      svn_stream_t *fstream;
      svn_revnum_t real_rev;
      const char *new_text_path;
      apr_hash_t *new_props;
      const char *src_rel;
      svn_stream_t *new_base_contents;

      SVN_ERR(svn_stream_open_unique(&fstream, &new_text_path, NULL,
                                     svn_io_file_del_on_pool_cleanup, pool,
                                     pool));

      SVN_ERR(svn_ra_get_path_relative_to_session(ra_session, &src_rel,
                                                  pair->src_abspath_or_url,
                                                  pool));
      SVN_ERR(svn_ra_get_file(ra_session, src_rel, src_revnum, fstream,
                              &real_rev, &new_props, pool));
      SVN_ERR(svn_stream_close(fstream));

      /* If SRC_REVNUM is invalid (HEAD), then REAL_REV is now the
         revision that was actually retrieved.  This is the value we
         want to use as 'copyfrom_rev' below. */
      if (! SVN_IS_VALID_REVNUM(src_revnum))
        src_revnum = real_rev;

      SVN_ERR(svn_stream_open_readonly(&new_base_contents, new_text_path,
                                       pool, pool));
      SVN_ERR(svn_wc_add_repos_file4(
         ctx->wc_ctx, dst_abspath,
         new_base_contents, NULL, new_props, NULL,
         same_repositories ? pair->src_abspath_or_url : NULL,
         same_repositories ? src_revnum : SVN_INVALID_REVNUM,
         ctx->cancel_func, ctx->cancel_baton,
         ctx->notify_func2, ctx->notify_baton2,
         pool));

      SVN_ERR(calculate_target_mergeinfo(ra_session, &src_mergeinfo,
                                         NULL, pair->src_abspath_or_url,
                                         src_revnum, ctx, pool));
      SVN_ERR(extend_wc_mergeinfo(dst_abspath, src_mergeinfo, ctx, pool));

      /* Ideally, svn_wc_add_repos_file3() would take a notify function
         and baton, and we wouldn't have to make this call here.
         However, the situation is... complicated.  See issue #1552
         for the full story. */
      if (ctx->notify_func2)
        {
          svn_wc_notify_t *notify = svn_wc_create_notify(
                                      pair->dst_abspath_or_url,
                                      svn_wc_notify_add, pool);
          notify->kind = pair->src_kind;
          (*ctx->notify_func2)(ctx->notify_baton2, notify, pool);
        }

      svn_io_sleep_for_timestamps(pair->dst_abspath_or_url, pool);
    }

  return SVN_NO_ERROR;
}

static svn_error_t*
repos_to_wc_copy_locked(const apr_array_header_t *copy_pairs,
                        const char *top_dst_path,
                        svn_boolean_t ignore_externals,
                        svn_ra_session_t *ra_session,
                        svn_client_ctx_t *ctx,
                        apr_pool_t *scratch_pool)
{
  int i;
  const char *src_uuid = NULL, *dst_uuid = NULL;
  svn_boolean_t same_repositories;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  /* We've already checked for physical obstruction by a working file.
     But there could also be logical obstruction by an entry whose
     working file happens to be missing.*/
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      svn_node_kind_t kind;
      svn_depth_t node_depth;
      svn_boolean_t is_absent;

      svn_pool_clear(iterpool);

      SVN_ERR(svn_wc_read_kind(&kind, ctx->wc_ctx, pair->dst_abspath_or_url,
                               FALSE, iterpool));
      if (kind == svn_node_none)
        continue;

      /* ### TODO(#2843): Rework these error report. Maybe we can
         ### simplify the conditions? */

      /* Hidden by client exclusion */
      SVN_ERR(svn_wc__node_get_depth(&node_depth, ctx->wc_ctx, 
                                     pair->dst_abspath_or_url, iterpool));
      if (node_depth == svn_depth_exclude)
        {
          return svn_error_createf
            (SVN_ERR_ENTRY_EXISTS,
             NULL, _("'%s' is already under version control"),
             svn_dirent_local_style(pair->dst_abspath_or_url, iterpool));
        }

      /* Hidden by server exclusion (absent) */
      SVN_ERR(svn_wc__node_is_status_absent(&is_absent, ctx->wc_ctx,
                                            pair->dst_abspath_or_url,
                                            iterpool));
      if (is_absent)
        {
          return svn_error_createf
            (SVN_ERR_ENTRY_EXISTS,
             NULL, _("'%s' is already under version control"),
             svn_dirent_local_style(pair->dst_abspath_or_url, iterpool));
        }

      /* Working file missing to something other than being scheduled
         for addition or in "deleted" state. */
      if (kind != svn_node_dir)
        {
          svn_boolean_t is_deleted;
          svn_boolean_t is_present;

          SVN_ERR(svn_wc__node_is_status_deleted(&is_deleted, ctx->wc_ctx,
                                                 pair->dst_abspath_or_url,
                                                 iterpool));
          SVN_ERR(svn_wc__node_is_status_present(&is_present, 
                                                 ctx->wc_ctx,
                                                 pair->dst_abspath_or_url,
                                                 iterpool));
          if ((! is_deleted) && is_present)
            return svn_error_createf
              (SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
               _("Entry for '%s' exists (though the working file is missing)"),
               svn_dirent_local_style(pair->dst_abspath_or_url, iterpool));
        }
    }

  /* Decide whether the two repositories are the same or not. */
  {
    svn_error_t *src_err, *dst_err;
    const char *parent;
    const char *parent_abspath;

    /* Get the repository uuid of SRC_URL */
    src_err = svn_ra_get_uuid2(ra_session, &src_uuid, scratch_pool);
    if (src_err && src_err->apr_err != SVN_ERR_RA_NO_REPOS_UUID)
      return svn_error_return(src_err);

    /* Get repository uuid of dst's parent directory, since dst may
       not exist.  ### TODO:  we should probably walk up the wc here,
       in case the parent dir has an imaginary URL.  */
    if (copy_pairs->nelts == 1)
      parent = svn_dirent_dirname(top_dst_path, scratch_pool);
    else
      parent = top_dst_path;

    SVN_ERR(svn_dirent_get_absolute(&parent_abspath, parent, scratch_pool));
    dst_err = svn_client_uuid_from_path2(&dst_uuid, parent_abspath, ctx,
                                         scratch_pool, scratch_pool);
    if (dst_err && dst_err->apr_err != SVN_ERR_RA_NO_REPOS_UUID)
      return dst_err;

    /* If either of the UUIDs are nonexistent, then at least one of
       the repositories must be very old.  Rather than punish the
       user, just assume the repositories are different, so no
       copy-history is attempted. */
    if (src_err || dst_err || (! src_uuid) || (! dst_uuid))
      same_repositories = FALSE;

    else
      same_repositories = (strcmp(src_uuid, dst_uuid) == 0);
  }

  /* Perform the move for each of the copy_pairs. */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      /* Check for cancellation */
      if (ctx->cancel_func)
        SVN_ERR(ctx->cancel_func(ctx->cancel_baton));

      svn_pool_clear(iterpool);

      SVN_ERR(repos_to_wc_copy_single(APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *),
                                      same_repositories,
                                      ignore_externals,
                                      ra_session, ctx, iterpool));
    }
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

struct repos_to_wc_copy_baton {
  const apr_array_header_t *copy_pairs;
  const char *top_dst_path;
  svn_boolean_t ignore_externals;
  svn_ra_session_t *ra_session;
  svn_client_ctx_t *ctx;
};

/* Implements svn_wc__with_write_lock_func_t. */
static svn_error_t *
repos_to_wc_copy_cb(void *baton,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool)
{
  struct repos_to_wc_copy_baton *b = baton;

  SVN_ERR(repos_to_wc_copy_locked(b->copy_pairs, b->top_dst_path,
                                  b->ignore_externals, b->ra_session,
                                  b->ctx, scratch_pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
repos_to_wc_copy(const apr_array_header_t *copy_pairs,
                 svn_boolean_t make_parents,
                 svn_boolean_t ignore_externals,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool)
{
  svn_ra_session_t *ra_session;
  const char *top_src_url, *top_dst_path;
  apr_pool_t *iterpool = svn_pool_create(pool);
  const char *lock_abspath;
  struct repos_to_wc_copy_baton baton;
  int i;

  /* Get the real path for the source, based upon its peg revision. */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      const char *src, *ignored_url;
      svn_opt_revision_t *new_rev, *ignored_rev, dead_end_rev;

      svn_pool_clear(iterpool);
      dead_end_rev.kind = svn_opt_revision_unspecified;

      SVN_ERR(svn_client__repos_locations(&src, &new_rev,
                                          &ignored_url, &ignored_rev,
                                          NULL,
                                          pair->src_abspath_or_url,
                                          &pair->src_peg_revision,
                                          &pair->src_op_revision,
                                          &dead_end_rev,
                                          ctx, iterpool));

      pair->src_original = pair->src_abspath_or_url;
      pair->src_abspath_or_url = apr_pstrdup(pool, src);
    }

  get_copy_pair_ancestors(copy_pairs, &top_src_url, &top_dst_path, NULL, pool);
  lock_abspath = top_dst_path;
  if (copy_pairs->nelts == 1)
    {
      svn_node_kind_t kind;
      top_src_url = svn_uri_dirname(top_src_url, pool);
      SVN_ERR(svn_wc_read_kind(&kind, ctx->wc_ctx, top_dst_path, FALSE, pool));
      if (kind != svn_node_dir)
        lock_abspath = svn_dirent_dirname(top_dst_path, pool);
    }

  /* Open a repository session to the longest common src ancestor.  We do not
     (yet) have a working copy, so we don't have a corresponding path and
     tempfiles cannot go into the admin area. */
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, top_src_url, NULL,
                                               NULL, FALSE, TRUE, ctx, pool));

  /* Pass null for the path, to ensure error if trying to get a
     revision based on the working copy.  */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);

      SVN_ERR(svn_client__get_revision_number(&pair->src_revnum, NULL,
                                              ctx->wc_ctx, NULL, ra_session,
                                              &pair->src_op_revision, pool));
    }

  /* Get the correct src path for the peg revision used, and verify that we
     aren't overwriting an existing path. */
  for (i = 0; i < copy_pairs->nelts; i++)
    {
      svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);
      svn_node_kind_t dst_parent_kind, dst_kind;
      const char *dst_parent;
      const char *src_rel;

      svn_pool_clear(iterpool);

      /* Next, make sure that the path exists in the repository. */
      SVN_ERR(svn_ra_get_path_relative_to_session(ra_session, &src_rel,
                                                  pair->src_abspath_or_url,
                                                  iterpool));
      SVN_ERR(svn_ra_check_path(ra_session, src_rel, pair->src_revnum,
                                &pair->src_kind, pool));
      if (pair->src_kind == svn_node_none)
        {
          if (SVN_IS_VALID_REVNUM(pair->src_revnum))
            return svn_error_createf
              (SVN_ERR_FS_NOT_FOUND, NULL,
               _("Path '%s' not found in revision %ld"),
               pair->src_abspath_or_url, pair->src_revnum);
          else
            return svn_error_createf
              (SVN_ERR_FS_NOT_FOUND, NULL,
               _("Path '%s' not found in head revision"),
               pair->src_abspath_or_url);
        }

      /* Figure out about dst. */
      SVN_ERR(svn_io_check_path(pair->dst_abspath_or_url, &dst_kind,
                                iterpool));
      if (dst_kind != svn_node_none)
        {
          return svn_error_createf(
            SVN_ERR_ENTRY_EXISTS, NULL,
            _("Path '%s' already exists"),
            svn_dirent_local_style(pair->dst_abspath_or_url, pool));
        }

      /* Make sure the destination parent is a directory and produce a clear
         error message if it is not. */
      dst_parent = svn_dirent_dirname(pair->dst_abspath_or_url, iterpool);
      SVN_ERR(svn_io_check_path(dst_parent, &dst_parent_kind, iterpool));
      if (make_parents && dst_parent_kind == svn_node_none)
        {
          SVN_ERR(svn_client__make_local_parents(dst_parent, TRUE, ctx,
                                                 iterpool));
        }
      else if (dst_parent_kind != svn_node_dir)
        {
          return svn_error_createf(SVN_ERR_WC_NOT_WORKING_COPY, NULL,
                                   _("Path '%s' is not a directory"),
                                   svn_dirent_local_style(dst_parent, pool));
        }
    }
  svn_pool_destroy(iterpool);

  baton.copy_pairs = copy_pairs;
  baton.top_dst_path = top_dst_path;
  baton.ignore_externals = ignore_externals;
  baton.ra_session = ra_session;
  baton.ctx = ctx;

  SVN_ERR(svn_wc__call_with_write_lock(repos_to_wc_copy_cb, &baton,
                                       ctx->wc_ctx, lock_abspath,
                                       pool, pool));
  return SVN_NO_ERROR;
}

#define NEED_REPOS_REVNUM(revision) \
        ((revision.kind != svn_opt_revision_unspecified) \
          && (revision.kind != svn_opt_revision_working))

/* Perform all allocations in POOL. */
static svn_error_t *
try_copy(svn_commit_info_t **commit_info_p,
         const apr_array_header_t *sources,
         const char *dst_path_in,
         svn_boolean_t is_move,
         svn_boolean_t force,
         svn_boolean_t make_parents,
         svn_boolean_t ignore_externals,
         const apr_hash_t *revprop_table,
         svn_client_ctx_t *ctx,
         apr_pool_t *pool)
{
  apr_array_header_t *copy_pairs =
                        apr_array_make(pool, sources->nelts,
                                       sizeof(svn_client__copy_pair_t *));
  svn_boolean_t srcs_are_urls, dst_is_url;
  int i;

  /* Are either of our paths URLs?  Just check the first src_path.  If
     there are more than one, we'll check for homogeneity among them
     down below. */
  srcs_are_urls = svn_path_is_url(APR_ARRAY_IDX(sources, 0,
                                  svn_client_copy_source_t *)->path);
  dst_is_url = svn_path_is_url(dst_path_in);
  if (!dst_is_url)
    SVN_ERR(svn_dirent_get_absolute(&dst_path_in, dst_path_in, pool));

  /* If we have multiple source paths, it implies the dst_path is a
     directory we are moving or copying into.  Populate the COPY_PAIRS
     array to contain a destination path for each of the source paths. */
  if (sources->nelts > 1)
    {
      apr_pool_t *iterpool = svn_pool_create(pool);

      for (i = 0; i < sources->nelts; i++)
        {
          svn_client_copy_source_t *source = APR_ARRAY_IDX(sources, i,
                                               svn_client_copy_source_t *);
          svn_client__copy_pair_t *pair = apr_palloc(pool, sizeof(*pair));
          const char *src_basename;
          svn_boolean_t src_is_url = svn_path_is_url(source->path);

          svn_pool_clear(iterpool);

          if (src_is_url)
            pair->src_abspath_or_url = apr_pstrdup(pool, source->path);
          else
            SVN_ERR(svn_dirent_get_absolute(&pair->src_abspath_or_url,
                                            source->path, pool));
          pair->src_op_revision = *source->revision;
          pair->src_peg_revision = *source->peg_revision;

          SVN_ERR(svn_opt_resolve_revisions(&pair->src_peg_revision,
                                            &pair->src_op_revision,
                                            src_is_url,
                                            TRUE,
                                            iterpool));
          if (src_is_url)
            src_basename = svn_uri_basename(pair->src_abspath_or_url,
                                            iterpool);
          else
            src_basename = svn_dirent_basename(pair->src_abspath_or_url,
                                               iterpool);
          if (srcs_are_urls && ! dst_is_url)
            src_basename = svn_path_uri_decode(src_basename, iterpool);

          /* Check to see if all the sources are urls or all working copy
           * paths. */
          if (src_is_url != srcs_are_urls)
            return svn_error_create
              (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
               _("Cannot mix repository and working copy sources"));

          if (dst_is_url)
            pair->dst_abspath_or_url = svn_uri_join(dst_path_in,
                                                    src_basename, pool);
          else
            pair->dst_abspath_or_url = svn_dirent_join(dst_path_in,
                                                       src_basename, pool);
          APR_ARRAY_PUSH(copy_pairs, svn_client__copy_pair_t *) = pair;
        }

      svn_pool_destroy(iterpool);
    }
  else
    {
      /* Only one source path. */
      svn_client__copy_pair_t *pair = apr_palloc(pool, sizeof(*pair));
      svn_client_copy_source_t *source =
        APR_ARRAY_IDX(sources, 0, svn_client_copy_source_t *);
      svn_boolean_t src_is_url = svn_path_is_url(source->path);

      if (src_is_url)
        pair->src_abspath_or_url = apr_pstrdup(pool, source->path);
      else
        SVN_ERR(svn_dirent_get_absolute(&pair->src_abspath_or_url,
                                        source->path, pool));
      pair->src_op_revision = *source->revision;
      pair->src_peg_revision = *source->peg_revision;

      SVN_ERR(svn_opt_resolve_revisions(&pair->src_peg_revision,
                                        &pair->src_op_revision,
                                        src_is_url, TRUE, pool));

      pair->dst_abspath_or_url = dst_path_in;
      APR_ARRAY_PUSH(copy_pairs, svn_client__copy_pair_t *) = pair;
    }

  if (!srcs_are_urls && !dst_is_url)
    {
      apr_pool_t *iterpool = svn_pool_create(pool);

      for (i = 0; i < copy_pairs->nelts; i++)
        {
          svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                            svn_client__copy_pair_t *);

          svn_pool_clear(iterpool);

          if (svn_dirent_is_child(pair->src_abspath_or_url,
                                  pair->dst_abspath_or_url, iterpool))
            return svn_error_createf
              (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
               _("Cannot copy path '%s' into its own child '%s'"),
               svn_dirent_local_style(pair->src_abspath_or_url, pool),
               svn_dirent_local_style(pair->dst_abspath_or_url, pool));
        }

      svn_pool_destroy(iterpool);
    }

  /* A file external should not be moved since the file external is
     implemented as a switched file and it would delete the file the
     file external is switched to, which is not the behavior the user
     would probably want. */
  if (is_move && !srcs_are_urls)
    {
      apr_pool_t *iterpool = svn_pool_create(pool);

      for (i = 0; i < copy_pairs->nelts; i++)
        {
          svn_client__copy_pair_t *pair =
            APR_ARRAY_IDX(copy_pairs, i, svn_client__copy_pair_t *);
          svn_boolean_t is_file_external;

          svn_pool_clear(iterpool);

          SVN_ERR_ASSERT(svn_dirent_is_absolute(pair->src_abspath_or_url));
          SVN_ERR(svn_wc__node_is_file_external(&is_file_external, ctx->wc_ctx,
                                                pair->src_abspath_or_url,
                                                iterpool));
          if (is_file_external)
            return svn_error_createf(
                     SVN_ERR_WC_CANNOT_MOVE_FILE_EXTERNAL,
                     NULL,
                     _("Cannot move the file external at '%s'; please "
                       "propedit the svn:externals description that "
                       "created it"),
                     svn_dirent_local_style(pair->src_abspath_or_url, pool));
        }
      svn_pool_destroy(iterpool);
    }

  if (is_move)
    {
      if (srcs_are_urls == dst_is_url)
        {
          for (i = 0; i < copy_pairs->nelts; i++)
            {
              svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                svn_client__copy_pair_t *);

              if (strcmp(pair->src_abspath_or_url,
                         pair->dst_abspath_or_url) == 0)
                return svn_error_createf
                  (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                   _("Cannot move path '%s' into itself"),
                   svn_dirent_local_style(pair->src_abspath_or_url, pool));
            }
        }
      else
        {
          /* Disallow moves between the working copy and the repository. */
          return svn_error_create
            (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
             _("Moves between the working copy and the repository are not "
               "supported"));
        }
    }
  else
    {
      if (!srcs_are_urls)
        {
          /* If we are doing a wc->* move, but with an operational revision
             other than the working copy revision, we are really doing a
             repo->* move, because we're going to need to get the rev from the
             repo. */

          svn_boolean_t need_repos_op_rev = FALSE;
          svn_boolean_t need_repos_peg_rev = FALSE;

          /* Check to see if any revision is something other than
             svn_opt_revision_unspecified or svn_opt_revision_working. */
          for (i = 0; i < copy_pairs->nelts; i++)
            {
              svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                svn_client__copy_pair_t *);

              if (NEED_REPOS_REVNUM(pair->src_op_revision))
                need_repos_op_rev = TRUE;

              if (NEED_REPOS_REVNUM(pair->src_peg_revision))
                need_repos_peg_rev = TRUE;

              if (need_repos_op_rev || need_repos_peg_rev)
                break;
            }

          if (need_repos_op_rev || need_repos_peg_rev)
            {
              apr_pool_t *iterpool = svn_pool_create(pool);

              for (i = 0; i < copy_pairs->nelts; i++)
                {
                  const char *copyfrom_url, *url;
                  svn_revnum_t base_rev, copyfrom_rev;
                  svn_client__copy_pair_t *pair = APR_ARRAY_IDX(copy_pairs, i,
                                                    svn_client__copy_pair_t *);

                  svn_pool_clear(iterpool);

                  SVN_ERR_ASSERT(svn_dirent_is_absolute(pair->src_abspath_or_url));

                  SVN_ERR(svn_wc__node_get_copyfrom_info(
                    &copyfrom_url, &copyfrom_rev, NULL, ctx->wc_ctx,
                    pair->src_abspath_or_url, pool, iterpool));

                  if (copyfrom_url)
                    {
                      url = copyfrom_url;
                    }
                  else
                    {
                      SVN_ERR(svn_wc__node_get_url(&url, ctx->wc_ctx,
                                                   pair->src_abspath_or_url,
                                                   pool, iterpool));
                      SVN_ERR(svn_wc__node_get_base_rev(
                        &base_rev, ctx->wc_ctx,
                        pair->src_abspath_or_url, iterpool));
                    }

                  if (url == NULL)
                    return svn_error_createf
                      (SVN_ERR_ENTRY_MISSING_URL, NULL,
                       _("'%s' does not have a URL associated with it"),
                       svn_dirent_local_style(pair->src_abspath_or_url, pool));

                  pair->src_abspath_or_url = url;

                  if (!need_repos_peg_rev
                      || pair->src_peg_revision.kind == svn_opt_revision_base)
                    {
                      /* Default the peg revision to that of the WC entry. */
                      pair->src_peg_revision.kind = svn_opt_revision_number;
                      pair->src_peg_revision.value.number =
                        (copyfrom_url ? copyfrom_rev : base_rev);
                    }

                  if (pair->src_op_revision.kind == svn_opt_revision_base)
                    {
                      /* Use the entry's revision as the operational rev. */
                      pair->src_op_revision.kind = svn_opt_revision_number;
                      pair->src_op_revision.value.number =
                        (copyfrom_url ? copyfrom_rev : base_rev);
                    }
                }

              svn_pool_destroy(iterpool);
              srcs_are_urls = TRUE;
            }
        }
    }

  /* Now, call the right handler for the operation. */
  if ((! srcs_are_urls) && (! dst_is_url))
    {
      *commit_info_p = NULL;
      return svn_error_return(
        wc_to_wc_copy(copy_pairs, dst_path_in, is_move, make_parents, ctx,
                      pool));
    }
  else if ((! srcs_are_urls) && (dst_is_url))
    {
      return svn_error_return(
        wc_to_repos_copy(commit_info_p, copy_pairs, make_parents,
                         revprop_table, ctx, pool));
    }
  else if ((srcs_are_urls) && (! dst_is_url))
    {
      *commit_info_p = NULL;
      return svn_error_return(
        repos_to_wc_copy(copy_pairs, make_parents, ignore_externals,
                         ctx, pool));
    }
  else
    {
      return svn_error_return(
        repos_to_repos_copy(commit_info_p, copy_pairs, make_parents,
                            revprop_table, ctx, is_move, pool));
    }
}



/* Public Interfaces */
svn_error_t *
svn_client_copy5(svn_commit_info_t **commit_info_p,
                 const apr_array_header_t *sources,
                 const char *dst_path,
                 svn_boolean_t copy_as_child,
                 svn_boolean_t make_parents,
                 svn_boolean_t ignore_externals,
                 const apr_hash_t *revprop_table,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool)
{
  svn_error_t *err;
  svn_commit_info_t *commit_info = NULL;
  apr_pool_t *subpool = svn_pool_create(pool);

  if (sources->nelts > 1 && !copy_as_child)
    return svn_error_create(SVN_ERR_CLIENT_MULTIPLE_SOURCES_DISALLOWED,
                            NULL, NULL);

  err = try_copy(&commit_info,
                 sources, dst_path,
                 FALSE /* is_move */,
                 TRUE /* force, set to avoid deletion check */,
                 make_parents,
                 ignore_externals,
                 revprop_table,
                 ctx,
                 subpool);

  /* If the destination exists, try to copy the sources as children of the
     destination. */
  if (copy_as_child && err && (sources->nelts == 1)
        && (err->apr_err == SVN_ERR_ENTRY_EXISTS
            || err->apr_err == SVN_ERR_FS_ALREADY_EXISTS))
    {
      const char *src_path = APR_ARRAY_IDX(sources, 0,
                                           svn_client_copy_source_t *)->path;
      const char *src_basename;
      svn_boolean_t src_is_uri = svn_path_is_url(src_path);
      svn_boolean_t dst_is_uri = svn_path_is_url(dst_path);

      svn_error_clear(err);
      svn_pool_clear(subpool);

      src_basename = src_is_uri ? svn_uri_basename(src_path, subpool)
                                : svn_dirent_basename(src_path, subpool);
      if (svn_path_is_url(src_path) && ! svn_path_is_url(dst_path))
        src_basename = svn_path_uri_decode(src_basename, subpool);

      err = try_copy(&commit_info,
                     sources,
                     dst_is_uri
                         ? svn_uri_join(dst_path, src_basename, subpool)
                         : svn_dirent_join(dst_path, src_basename, subpool),
                     FALSE /* is_move */,
                     TRUE /* force, set to avoid deletion check */,
                     make_parents,
                     ignore_externals,
                     revprop_table,
                     ctx,
                     subpool);
    }

  if (commit_info_p != NULL)
    {
      if (commit_info)
        *commit_info_p = svn_commit_info_dup(commit_info, pool);
      else
        *commit_info_p = NULL;
    }

  svn_pool_destroy(subpool);
  return svn_error_return(err);
}


svn_error_t *
svn_client_move5(svn_commit_info_t **commit_info_p,
                 const apr_array_header_t *src_paths,
                 const char *dst_path,
                 svn_boolean_t force,
                 svn_boolean_t move_as_child,
                 svn_boolean_t make_parents,
                 const apr_hash_t *revprop_table,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool)
{
  svn_commit_info_t *commit_info = NULL;
  const svn_opt_revision_t head_revision
    = { svn_opt_revision_head, { 0 } };
  svn_error_t *err;
  int i;
  apr_pool_t *subpool = svn_pool_create(pool);
  apr_array_header_t *sources = apr_array_make(pool, src_paths->nelts,
                                  sizeof(const svn_client_copy_source_t *));

  if (src_paths->nelts > 1 && !move_as_child)
    return svn_error_create(SVN_ERR_CLIENT_MULTIPLE_SOURCES_DISALLOWED,
                            NULL, NULL);

  for (i = 0; i < src_paths->nelts; i++)
    {
      const char *src_path = APR_ARRAY_IDX(src_paths, i, const char *);
      svn_client_copy_source_t *copy_source = apr_palloc(pool,
                                                         sizeof(*copy_source));

      copy_source->path = src_path;
      copy_source->revision = &head_revision;
      copy_source->peg_revision = &head_revision;

      APR_ARRAY_PUSH(sources, svn_client_copy_source_t *) = copy_source;
    }

  err = try_copy(&commit_info, sources, dst_path,
                 TRUE /* is_move */,
                 force,
                 make_parents,
                 FALSE,
                 revprop_table,
                 ctx,
                 subpool);

  /* If the destination exists, try to move the sources as children of the
     destination. */
  if (move_as_child && err && (src_paths->nelts == 1)
        && (err->apr_err == SVN_ERR_ENTRY_EXISTS
            || err->apr_err == SVN_ERR_FS_ALREADY_EXISTS))
    {
      const char *src_path = APR_ARRAY_IDX(src_paths, 0, const char *);
      const char *src_basename;
      svn_boolean_t src_is_uri = svn_path_is_url(src_path);
      svn_boolean_t dst_is_uri = svn_path_is_url(dst_path);

      svn_error_clear(err);
      svn_pool_clear(subpool);

      src_basename = src_is_uri ? svn_uri_basename(src_path, pool)
                                : svn_dirent_basename(src_path, pool);

      err = try_copy(&commit_info, sources,
                     dst_is_uri
                         ? svn_uri_join(dst_path, src_basename, pool)
                         : svn_dirent_join(dst_path, src_basename, pool),
                     TRUE /* is_move */,
                     force,
                     make_parents,
                     FALSE,
                     revprop_table,
                     ctx,
                     subpool);
    }

  if (commit_info_p != NULL)
    {
      if (commit_info)
        *commit_info_p = svn_commit_info_dup(commit_info, pool);
      else
        *commit_info_p = commit_info;
    }

  svn_pool_destroy(subpool);
  return svn_error_return(err);
}
