/*
 * commit_util.c:  Driver for the WC commit process.
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
#include <apr_hash.h>
#include <apr_md5.h>

#include "client.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_types.h"
#include "svn_pools.h"
#include "svn_props.h"
#include "svn_iter.h"
#include "svn_subst.h"
#include "svn_hash.h"

#include <assert.h>
#include <stdlib.h>  /* for qsort() */

#include "svn_private_config.h"
#include "private/svn_wc_private.h"
#include "private/svn_client_private.h"

/*** Uncomment this to turn on commit driver debugging. ***/
/*
#define SVN_CLIENT_COMMIT_DEBUG
*/

/* Wrap an RA error in a nicer error if one is available. */
static svn_error_t *
fixup_commit_error(const char *local_abspath,
                   const char *repos_root,
                   const char *repos_relpath,
                   svn_node_kind_t kind,
                   svn_error_t *err,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *scratch_pool)
{
  if (err->apr_err == SVN_ERR_FS_NOT_FOUND
      || err->apr_err == SVN_ERR_FS_ALREADY_EXISTS
      || err->apr_err == SVN_ERR_FS_TXN_OUT_OF_DATE
      || err->apr_err == SVN_ERR_RA_DAV_PATH_NOT_FOUND
      || err->apr_err == SVN_ERR_RA_DAV_ALREADY_EXISTS
      || svn_error_find_cause(err, SVN_ERR_RA_OUT_OF_DATE))
    {
      if (ctx->notify_func2)
        {
          svn_wc_notify_t *notify;

          if (local_abspath)
            notify = svn_wc_create_notify(local_abspath,
                                          svn_wc_notify_failed_out_of_date,
                                          scratch_pool);
          else
            notify = svn_wc_create_notify_url(
                                svn_path_url_add_component2(repos_root,
                                                            repos_relpath,
                                                            scratch_pool),
                                svn_wc_notify_failed_out_of_date,
                                scratch_pool);

          notify->kind = kind;
          notify->err = err;

          ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
        }

      return svn_error_createf(SVN_ERR_WC_NOT_UP_TO_DATE, err,
                               (kind == svn_node_dir
                                 ? _("Directory '%s' is out of date")
                                 : _("File '%s' is out of date")),
                               local_abspath
                                  ? svn_dirent_local_style(local_abspath,
                                                           scratch_pool)
                                  : svn_path_url_add_component2(repos_root,
                                                                repos_relpath,
                                                                scratch_pool));
    }
  else if (svn_error_find_cause(err, SVN_ERR_FS_NO_LOCK_TOKEN)
           || err->apr_err == SVN_ERR_FS_LOCK_OWNER_MISMATCH
           || err->apr_err == SVN_ERR_RA_NOT_LOCKED)
    {
      if (ctx->notify_func2)
        {
          svn_wc_notify_t *notify;

          if (local_abspath)
            notify = svn_wc_create_notify(local_abspath,
                                          svn_wc_notify_failed_locked,
                                          scratch_pool);
          else
            notify = svn_wc_create_notify_url(
                                svn_path_url_add_component2(repos_root,
                                                            repos_relpath,
                                                            scratch_pool),
                                svn_wc_notify_failed_locked,
                                scratch_pool);

          notify->kind = kind;
          notify->err = err;

          ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
        }

      return svn_error_createf(SVN_ERR_CLIENT_NO_LOCK_TOKEN, err,
                   (kind == svn_node_dir
                     ? _("Directory '%s' is locked in another working copy")
                     : _("File '%s' is locked in another working copy")),
                   local_abspath
                      ? svn_dirent_local_style(local_abspath,
                                               scratch_pool)
                      : svn_path_url_add_component2(repos_root,
                                                    repos_relpath,
                                                    scratch_pool));
    }
  else if (svn_error_find_cause(err, SVN_ERR_RA_DAV_FORBIDDEN)
           || err->apr_err == SVN_ERR_AUTHZ_UNWRITABLE)
    {
      if (ctx->notify_func2)
        {
          svn_wc_notify_t *notify;

          if (local_abspath)
            notify = svn_wc_create_notify(
                                    local_abspath,
                                    svn_wc_notify_failed_forbidden_by_server,
                                    scratch_pool);
          else
            notify = svn_wc_create_notify_url(
                                svn_path_url_add_component2(repos_root,
                                                            repos_relpath,
                                                            scratch_pool),
                                svn_wc_notify_failed_forbidden_by_server,
                                scratch_pool);

          notify->kind = kind;
          notify->err = err;

          ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
        }

      return svn_error_createf(SVN_ERR_CLIENT_FORBIDDEN_BY_SERVER, err,
                   (kind == svn_node_dir
                     ? _("Changing directory '%s' is forbidden by the server")
                     : _("Changing file '%s' is forbidden by the server")),
                   local_abspath
                      ? svn_dirent_local_style(local_abspath,
                                               scratch_pool)
                      : svn_path_url_add_component2(repos_root,
                                                    repos_relpath,
                                                    scratch_pool));
    }
  else
    return svn_error_createf(err->apr_err, err,
                             _("Error while committing '%s':"),
                             local_abspath
                                ? svn_dirent_local_style(local_abspath,
                                                         scratch_pool)
                                : svn_path_url_add_component2(repos_root,
                                                              repos_relpath,
                                                              scratch_pool));

}


/*** Harvesting Commit Candidates ***/


/* Add a new commit candidate (described by all parameters except
   `COMMITTABLES') to the COMMITTABLES hash.  All of the commit item's
   members are allocated out of RESULT_POOL. */
static svn_error_t *
add_committable(svn_client__committables_t *committables,
                const char *local_abspath,
                svn_node_kind_t kind,
                const char *repos_root_url,
                const char *repos_relpath,
                svn_revnum_t revision,
                const char *copyfrom_relpath,
                svn_revnum_t copyfrom_rev,
                apr_byte_t state_flags,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  apr_array_header_t *array;
  svn_client_commit_item3_t *new_item;

  /* Sanity checks. */
  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(repos_root_url && repos_relpath);

  /* ### todo: Get the canonical repository for this item, which will
     be the real key for the COMMITTABLES hash, instead of the above
     bogosity. */
  array = apr_hash_get(committables->by_repository,
                       repos_root_url,
                       APR_HASH_KEY_STRING);

  /* E-gads!  There is no array for this repository yet!  Oh, no
     problem, we'll just create (and add to the hash) one. */
  if (array == NULL)
    {
      array = apr_array_make(result_pool, 1, sizeof(new_item));
      apr_hash_set(committables->by_repository,
                   apr_pstrdup(result_pool, repos_root_url),
                   APR_HASH_KEY_STRING, array);
    }

  /* Now update pointer values, ensuring that their allocations live
     in POOL. */
  new_item = svn_client_commit_item3_create(result_pool);
  new_item->path           = apr_pstrdup(result_pool, local_abspath);
  new_item->kind           = kind;
  new_item->url            = svn_path_url_add_component2(repos_root_url,
                                                         repos_relpath,
                                                         result_pool);
  new_item->revision       = revision;
  new_item->copyfrom_url   = copyfrom_relpath
                                ? svn_path_url_add_component2(repos_root_url,
                                                              copyfrom_relpath,
                                                              result_pool)
                                : NULL;
  new_item->copyfrom_rev   = copyfrom_rev;
  new_item->state_flags    = state_flags;
  new_item->incoming_prop_changes = apr_array_make(result_pool, 1,
                                                   sizeof(svn_prop_t *));

  /* Now, add the commit item to the array. */
  APR_ARRAY_PUSH(array, svn_client_commit_item3_t *) = new_item;

  /* ... and to the hash. */
  apr_hash_set(committables->by_path,
               new_item->path,
               APR_HASH_KEY_STRING,
               new_item);

  return SVN_NO_ERROR;
}

/* If there is a commit item for PATH in COMMITTABLES, return it, else
   return NULL.  Use POOL for temporary allocation only. */
static svn_client_commit_item3_t *
look_up_committable(svn_client__committables_t *committables,
                    const char *path,
                    apr_pool_t *pool)
{
  return (svn_client_commit_item3_t *)
      apr_hash_get(committables->by_path, path, APR_HASH_KEY_STRING);
}

/* Helper function for svn_client__harvest_committables().
 * Determine whether we are within a tree-conflicted subtree of the
 * working copy and return an SVN_ERR_WC_FOUND_CONFLICT error if so. */
static svn_error_t *
bail_on_tree_conflicted_ancestor(svn_wc_context_t *wc_ctx,
                                 const char *local_abspath,
                                 svn_wc_notify_func2_t notify_func,
                                 void *notify_baton,
                                 apr_pool_t *scratch_pool)
{
  const char *wcroot_abspath;

  SVN_ERR(svn_wc__get_wc_root(&wcroot_abspath, wc_ctx, local_abspath,
                              scratch_pool, scratch_pool));

  local_abspath = svn_dirent_dirname(local_abspath, scratch_pool);

  while(svn_dirent_is_ancestor(wcroot_abspath, local_abspath))
    {
      svn_boolean_t tree_conflicted;

      /* Check if the parent has tree conflicts */
      SVN_ERR(svn_wc_conflicted_p3(NULL, NULL, &tree_conflicted,
                                   wc_ctx, local_abspath, scratch_pool));
      if (tree_conflicted)
        {
          if (notify_func != NULL)
            {
              notify_func(notify_baton,
                          svn_wc_create_notify(local_abspath,
                                               svn_wc_notify_failed_conflict,
                                               scratch_pool),
                          scratch_pool);
            }

          return svn_error_createf(
                   SVN_ERR_WC_FOUND_CONFLICT, NULL,
                   _("Aborting commit: '%s' remains in tree-conflict"),
                   svn_dirent_local_style(local_abspath, scratch_pool));
        }

      /* Step outwards */
      if (svn_dirent_is_root(local_abspath, strlen(local_abspath)))
        break;
      else
        local_abspath = svn_dirent_dirname(local_abspath, scratch_pool);
    }

  return SVN_NO_ERROR;
}


/* Recursively search for commit candidates in (and under) LOCAL_ABSPATH using
   WC_CTX and add those candidates to COMMITTABLES.  If in ADDS_ONLY modes,
   only new additions are recognized.

   DEPTH indicates how to treat files and subdirectories of LOCAL_ABSPATH
   when LOCAL_ABSPATH is itself a directory; see
   svn_client__harvest_committables() for its behavior.

   Lock tokens of candidates will be added to LOCK_TOKENS, if
   non-NULL.  JUST_LOCKED indicates whether to treat non-modified items with
   lock tokens as commit candidates.

   If COMMIT_RELPATH is not NULL, treat not-added nodes as if it is destined to
   be added as COMMIT_RELPATH, and add 'deleted' entries to COMMITTABLES as
   items to delete in the copy destination.  COPY_MODE_ROOT should be set TRUE
   for the first call for which COPY_MODE is TRUE, i.e. not for the
   recursive calls, and FALSE otherwise.

   If CHANGELISTS is non-NULL, it is a hash whose keys are const char *
   changelist names used as a restrictive filter
   when harvesting committables; that is, don't add a path to
   COMMITTABLES unless it's a member of one of those changelists.

   IS_EXPLICIT_TARGET should always be passed as TRUE, except when
   harvest_committables() calls itself in recursion. This provides a way to
   tell whether LOCAL_ABSPATH was an original target or whether it was reached
   by recursing deeper into a dir target. (This is used to skip all file
   externals that aren't explicit commit targets.)

   DANGLERS is a hash table mapping const char* absolute paths of a parent
   to a const char * absolute path of a child. See the comment about
   danglers at the top of svn_client__harvest_committables().

   If CANCEL_FUNC is non-null, call it with CANCEL_BATON to see
   if the user has cancelled the operation.

   Any items added to COMMITTABLES are allocated from the COMITTABLES
   hash pool, not POOL.  SCRATCH_POOL is used for temporary allocations. */

struct harvest_baton
{
  /* Static data */
  const char *root_abspath;
  svn_client__committables_t *committables;
  apr_hash_t *lock_tokens;
  const char *commit_relpath; /* Valid for the harvest root */
  svn_depth_t depth;
  svn_boolean_t just_locked;
  apr_hash_t *changelists;
  apr_hash_t *danglers;
  svn_client__check_url_kind_t check_url_func;
  void *check_url_baton;
  svn_wc_notify_func2_t notify_func;
  void *notify_baton;
  svn_wc_context_t *wc_ctx;
  apr_pool_t *result_pool;

  /* Harvester state */
  const char *skip_below_abspath; /* If non-NULL, skip everything below */
};

static svn_error_t *
harvest_status_callback(void *status_baton,
                        const char *local_abspath,
                        const svn_wc_status3_t *status,
                        apr_pool_t *scratch_pool);

static svn_error_t *
harvest_committables(const char *local_abspath,
                     svn_client__committables_t *committables,
                     apr_hash_t *lock_tokens,
                     const char *copy_mode_relpath,
                     svn_depth_t depth,
                     svn_boolean_t just_locked,
                     apr_hash_t *changelists,
                     apr_hash_t *danglers,
                     svn_client__check_url_kind_t check_url_func,
                     void *check_url_baton,
                     svn_cancel_func_t cancel_func,
                     void *cancel_baton,
                     svn_wc_notify_func2_t notify_func,
                     void *notify_baton,
                     svn_wc_context_t *wc_ctx,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  struct harvest_baton baton;

  baton.root_abspath = local_abspath;
  baton.committables = committables;
  baton.lock_tokens = lock_tokens;
  baton.commit_relpath = copy_mode_relpath;
  baton.depth = depth;
  baton.just_locked = just_locked;
  baton.changelists = changelists;
  baton.danglers = danglers;
  baton.check_url_func = check_url_func;
  baton.check_url_baton = check_url_baton;
  baton.notify_func = notify_func;
  baton.notify_baton = notify_baton;
  baton.wc_ctx = wc_ctx;
  baton.result_pool = result_pool;

  baton.skip_below_abspath = NULL;

  SVN_ERR(svn_wc_walk_status(wc_ctx,
                             local_abspath,
                             depth,
                             (copy_mode_relpath != NULL) /* get_all */,
                             FALSE /* no_ignore */,
                             FALSE /* ignore_text_mods */,
                             NULL /* ignore_patterns */,
                             harvest_status_callback,
                             &baton,
                             cancel_func, cancel_baton,
                             scratch_pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
harvest_not_present_for_copy(svn_wc_context_t *wc_ctx,
                             const char *local_abspath,
                             svn_client__committables_t *committables,
                             const char *repos_root_url,
                             const char *commit_relpath,
                             svn_client__check_url_kind_t check_url_func,
                             void *check_url_baton,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  const apr_array_header_t *children;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  int i;

  /* A function to retrieve not present children would be nice to have */
  SVN_ERR(svn_wc__node_get_children_of_working_node(
                                    &children, wc_ctx, local_abspath, TRUE,
                                    scratch_pool, iterpool));
      
  for (i = 0; i < children->nelts; i++)
    {
      const char *this_abspath = APR_ARRAY_IDX(children, i, const char *);
      const char *name = svn_dirent_basename(this_abspath, NULL);
      const char *this_commit_relpath;
      svn_boolean_t not_present;
      svn_node_kind_t kind;

      svn_pool_clear(iterpool);

      SVN_ERR(svn_wc__node_is_status_not_present(&not_present, wc_ctx,
                                                  this_abspath, scratch_pool));

      if (!not_present)
        continue;

      if (commit_relpath == NULL)
        this_commit_relpath = NULL;
      else
        this_commit_relpath = svn_relpath_join(commit_relpath, name,
                                              iterpool);

      /* We should check if we should really add a delete operation */
      if (check_url_func)
        {
          svn_revnum_t parent_rev;
          const char *parent_repos_relpath;
          const char *parent_repos_root_url;
          const char *node_url;

          /* Determine from what parent we would be the deleted child */
          SVN_ERR(svn_wc__node_get_origin(
                              NULL, &parent_rev, &parent_repos_relpath,
                              &parent_repos_root_url, NULL, NULL,
                              wc_ctx,
                              svn_dirent_dirname(this_abspath,
                                                  scratch_pool),
                              FALSE, scratch_pool, scratch_pool));

          node_url = svn_path_url_add_component2(
                        svn_path_url_add_component2(parent_repos_root_url,
                                                    parent_repos_relpath,
                                                    scratch_pool),
                        svn_dirent_basename(this_abspath, NULL),
                        iterpool);

          SVN_ERR(check_url_func(check_url_baton, &kind,
                                 node_url, parent_rev, iterpool));

          if (kind == svn_node_none)
            continue; /* This node can't be deleted */
        }
      else
        SVN_ERR(svn_wc_read_kind(&kind, wc_ctx, this_abspath, TRUE,
                                 scratch_pool));

      SVN_ERR(add_committable(committables, this_abspath, kind,
                              repos_root_url,
                              this_commit_relpath,
                              SVN_INVALID_REVNUM,
                              NULL /* copyfrom_relpath */,
                              SVN_INVALID_REVNUM /* copyfrom_rev */,
                              SVN_CLIENT_COMMIT_ITEM_DELETE,
                              result_pool, scratch_pool));
    }

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

/* Implements svn_wc_status_func4_t */
static svn_error_t *
harvest_status_callback(void *status_baton,
                        const char *local_abspath,
                        const svn_wc_status3_t *status,
                        apr_pool_t *scratch_pool)
{
  apr_byte_t state_flags = 0;
  svn_revnum_t node_rev;
  const char *cf_relpath = NULL;
  svn_revnum_t cf_rev = SVN_INVALID_REVNUM;
  svn_boolean_t matches_changelists;
  svn_boolean_t is_added;
  svn_boolean_t is_deleted;
  svn_boolean_t is_replaced;
  svn_boolean_t is_op_root;
  svn_boolean_t is_update_root;
  svn_revnum_t original_rev;
  const char *original_relpath;
  svn_boolean_t copy_mode;

  struct harvest_baton *baton = status_baton;
  svn_boolean_t is_harvest_root = 
                (strcmp(baton->root_abspath, local_abspath) == 0);
  svn_client__committables_t *committables = baton->committables;
  apr_hash_t *lock_tokens = baton->lock_tokens;
  const char *repos_root_url = status->repos_root_url;
  const char *commit_relpath = NULL;
  svn_boolean_t copy_mode_root = (baton->commit_relpath && is_harvest_root);
  svn_boolean_t just_locked = baton->just_locked;
  apr_hash_t *changelists = baton->changelists;
  apr_hash_t *danglers = baton->danglers;
  svn_wc_notify_func2_t notify_func = baton->notify_func;
  void *notify_baton = baton->notify_baton;
  svn_wc_context_t *wc_ctx = baton->wc_ctx;
  apr_pool_t *result_pool = baton->result_pool;

  if (baton->commit_relpath)
    commit_relpath = svn_relpath_join(
                        baton->commit_relpath,
                        svn_dirent_skip_ancestor(baton->root_abspath,
                                                 local_abspath),
                        scratch_pool);

  copy_mode = (commit_relpath != NULL);

  if (baton->skip_below_abspath
      && svn_dirent_is_ancestor(baton->skip_below_abspath, local_abspath))
    {
      return SVN_NO_ERROR;
    }
  else
    baton->skip_below_abspath = NULL; /* We have left the skip tree */

  /* Return early for nodes that don't have a committable status */
  switch (status->node_status)
    {
      case svn_wc_status_unversioned:
      case svn_wc_status_ignored:
      case svn_wc_status_external:
      case svn_wc_status_none:
        /* Unversioned nodes aren't committable, but are reported by the status
           walker.
           But if the unversioned node is the root of the walk, we have a user
           error */
        if (is_harvest_root)
          return svn_error_createf(
                       SVN_ERR_ILLEGAL_TARGET, NULL,
                       _("'%s' is not under version control"),
                       svn_dirent_local_style(local_abspath, scratch_pool));
        return SVN_NO_ERROR;
      case svn_wc_status_normal:
        /* Status normal nodes aren't modified, so we don't have to commit them
           when we perform a normal commit. But if a node is conflicted we want
           to stop the commit and if we are collecting lock tokens we want to
           look further anyway.

           When in copy mode we need to compare the revision of the node against
           the parent node to copy mixed-revision base nodes properly */
        if (!copy_mode && !status->conflicted
            && !(just_locked && status->lock))
          return SVN_NO_ERROR;
        break;
      default:
        /* Fall through */
        break;
    }

  /* Early out if the item is already marked as committable. */
  if (look_up_committable(committables, local_abspath, scratch_pool))
    return SVN_NO_ERROR;

  SVN_ERR_ASSERT((copy_mode && commit_relpath)
                 || (! copy_mode && ! commit_relpath));
  SVN_ERR_ASSERT((copy_mode_root && copy_mode) || ! copy_mode_root);
  SVN_ERR_ASSERT((just_locked && lock_tokens) || !just_locked);

  /* Save the result for reuse. */
  matches_changelists = ((changelists == NULL)
                         || (status->changelist != NULL
                             && apr_hash_get(changelists, status->changelist,
                                             APR_HASH_KEY_STRING) != NULL));

  /* Early exit. */
  if (status->kind != svn_node_dir && ! matches_changelists)
    {
      return SVN_NO_ERROR;
    }

  /* If NODE is in our changelist, then examine it for conflicts. We
     need to bail out if any conflicts exist.
     The status walker checked for conflict marker removal. */
  if (status->conflicted && matches_changelists)
    {
      if (notify_func != NULL)
        {
          notify_func(notify_baton,
                      svn_wc_create_notify(local_abspath,
                                           svn_wc_notify_failed_conflict,
                                           scratch_pool),
                      scratch_pool);
        }

      return svn_error_createf(
            SVN_ERR_WC_FOUND_CONFLICT, NULL,
            _("Aborting commit: '%s' remains in conflict"),
            svn_dirent_local_style(local_abspath, scratch_pool));
    }
  else if (status->node_status == svn_wc_status_obstructed)
    {
      /* A node's type has changed before attempting to commit.
         This also catches symlink vs non symlink changes */

      if (notify_func != NULL)
        {
          notify_func(notify_baton,
                      svn_wc_create_notify(local_abspath,
                                           svn_wc_notify_failed_obstruction,
                                           scratch_pool),
                      scratch_pool);
        }

      return svn_error_createf(
                    SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                    _("Node '%s' has unexpectedly changed kind"),
                    svn_dirent_local_style(local_abspath, scratch_pool));
    }

  if (status->conflicted && status->kind == svn_node_unknown)
    return SVN_NO_ERROR; /* Ignore delete-delete conflict */

  /* Return error on unknown path kinds.  We check both the entry and
     the node itself, since a path might have changed kind since its
     entry was written. */
  SVN_ERR(svn_wc__node_get_commit_status(&is_added, &is_deleted,
                                         &is_replaced,
                                         &is_op_root,
                                         &node_rev,
                                         &original_rev, &original_relpath,
                                         &is_update_root,
                                         wc_ctx, local_abspath,
                                         scratch_pool, scratch_pool));

  /* Handle file externals.
   * (IS_UPDATE_ROOT is more generally defined, but at the moment this
   * condition matches only file externals.)
   *
   * Don't copy files that svn:externals brought into the WC. So in copy_mode,
   * even explicit targets are skipped.
   *
   * Hande file externals only when passed as explicit target. Note that
   * svn_client_commit6() passes all committable externals in as explicit
   * targets iff they count.
   */
  if (is_update_root
      && status->kind == svn_node_file
      && (copy_mode || ! is_harvest_root))
    {
      return SVN_NO_ERROR;
    }

  if (status->node_status == svn_wc_status_missing && matches_changelists)
    {
      /* Added files and directories must exist. See issue #3198. */
      if (is_added && is_op_root)
        {
          if (notify_func != NULL)
            {
              notify_func(notify_baton,
                          svn_wc_create_notify(local_abspath,
                                               svn_wc_notify_failed_missing,
                                               scratch_pool),
                          scratch_pool);
            }
          return svn_error_createf(
             SVN_ERR_WC_PATH_NOT_FOUND, NULL,
             _("'%s' is scheduled for addition, but is missing"),
             svn_dirent_local_style(local_abspath, scratch_pool));
        }

      return SVN_NO_ERROR;
    }

  if (is_deleted && !is_op_root /* && !is_added */)
    return SVN_NO_ERROR; /* Not an operational delete and not an add. */

  /* Check for the deletion case.
     * We delete explicitly deleted nodes (duh!)
     * We delete not-present children of copies
     * We delete nodes that directly replace a node in its ancestor
   */

  if (is_deleted || is_replaced)
    state_flags |= SVN_CLIENT_COMMIT_ITEM_DELETE;

  /* Check for adds and copies */
  if (is_added && is_op_root)
    {
      /* Root of local add or copy */
      state_flags |= SVN_CLIENT_COMMIT_ITEM_ADD;

      if (original_relpath)
        {
          /* Root of copy */
          state_flags |= SVN_CLIENT_COMMIT_ITEM_IS_COPY;
          cf_relpath = original_relpath;
          cf_rev = original_rev;
        }
    }

  /* Further copies may occur in copy mode. */
  else if (copy_mode
           && !(state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE))
    {
      svn_revnum_t dir_rev;

      if (!copy_mode_root && !status->switched)
        SVN_ERR(svn_wc__node_get_base(&dir_rev, NULL, NULL, NULL, wc_ctx,
                                      svn_dirent_dirname(local_abspath,
                                                         scratch_pool),
                                      scratch_pool, scratch_pool));

      if (copy_mode_root || status->switched || node_rev != dir_rev)
        {
          state_flags |= (SVN_CLIENT_COMMIT_ITEM_ADD
                          | SVN_CLIENT_COMMIT_ITEM_IS_COPY);

          if (status->copied)
            {
              /* Copy from original location */
              cf_rev = original_rev;
              cf_relpath = original_relpath;
            }
          else
            {
              /* Copy BASE location, to represent a mixed-rev or switch copy */
              cf_rev = status->revision;
              cf_relpath = status->repos_relpath;
            }
        }
    }

  if (!(state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE)
      || (state_flags & SVN_CLIENT_COMMIT_ITEM_ADD))
    {
      svn_boolean_t text_mod = FALSE;
      svn_boolean_t prop_mod = FALSE;

      if (status->kind == svn_node_file)
        {
          /* Check for text modifications on files */
          if ((state_flags & SVN_CLIENT_COMMIT_ITEM_ADD)
              && ! (state_flags & SVN_CLIENT_COMMIT_ITEM_IS_COPY))
            {
              text_mod = TRUE; /* Local added files are always modified */
            }
          else
            text_mod = (status->text_status != svn_wc_status_normal);
        }

      prop_mod = (status->prop_status != svn_wc_status_normal
                  && status->prop_status != svn_wc_status_none);

      /* Set text/prop modification flags accordingly. */
      if (text_mod)
        state_flags |= SVN_CLIENT_COMMIT_ITEM_TEXT_MODS;
      if (prop_mod)
        state_flags |= SVN_CLIENT_COMMIT_ITEM_PROP_MODS;
    }

  /* If the entry has a lock token and it is already a commit candidate,
     or the caller wants unmodified locked items to be treated as
     such, note this fact. */
  if (status->lock && lock_tokens && (state_flags || just_locked))
    {
      state_flags |= SVN_CLIENT_COMMIT_ITEM_LOCK_TOKEN;
    }

  /* Now, if this is something to commit, add it to our list. */
  if (state_flags)
    {
      if (matches_changelists)
        {
          /* Finally, add the committable item. */
          SVN_ERR(add_committable(committables, local_abspath,
                                  status->kind,
                                  repos_root_url,
                                  copy_mode
                                      ? commit_relpath
                                      : status->repos_relpath,
                                  copy_mode
                                      ? SVN_INVALID_REVNUM
                                      : node_rev,
                                  cf_relpath,
                                  cf_rev,
                                  state_flags,
                                  result_pool, scratch_pool));
          if (state_flags & SVN_CLIENT_COMMIT_ITEM_LOCK_TOKEN)
            apr_hash_set(lock_tokens,
                         svn_path_url_add_component2(
                             repos_root_url, status->repos_relpath,
                             result_pool),
                         APR_HASH_KEY_STRING,
                         apr_pstrdup(result_pool,
                                     status->lock->token));
        }
    }

    /* Fetch lock tokens for descendants of deleted nodes. */
  if (lock_tokens
      && (state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE))
    {
      apr_hash_t *local_relpath_tokens;
      apr_hash_index_t *hi;
      apr_pool_t *token_pool = apr_hash_pool_get(lock_tokens);

      SVN_ERR(svn_wc__node_get_lock_tokens_recursive(
                  &local_relpath_tokens, wc_ctx, local_abspath,
                  token_pool, scratch_pool));

      /* Add tokens to existing hash. */
      for (hi = apr_hash_first(scratch_pool, local_relpath_tokens);
           hi;
           hi = apr_hash_next(hi))
        {
          const void *key;
          apr_ssize_t klen;
          void * val;

          apr_hash_this(hi, &key, &klen, &val);

          apr_hash_set(lock_tokens, key, klen, val);
        }
    }

  /* Make sure we check for dangling children on additions */
  if (state_flags && is_added && is_harvest_root && danglers)
    {
      /* If a node is added, it's parent must exist in the repository at the
         time of committing */

      svn_boolean_t parent_added;
      const char *parent_abspath = svn_dirent_dirname(local_abspath,
                                                      scratch_pool);

      SVN_ERR(svn_wc__node_is_added(&parent_added, wc_ctx, parent_abspath,
                                    scratch_pool));

      if (parent_added)
        {
          const char *copy_root_abspath;
          svn_boolean_t parent_is_copy;

          /* The parent is added, so either it is a copy, or a locally added
           * directory. In either case, we require the op-root of the parent
           * to be part of the commit. See issue #4059. */
          SVN_ERR(svn_wc__node_get_origin(&parent_is_copy, NULL, NULL, NULL,
                                          NULL, &copy_root_abspath,
                                          wc_ctx, parent_abspath,
                                          FALSE, scratch_pool, scratch_pool));

          if (parent_is_copy)
            parent_abspath = copy_root_abspath;

          if (!apr_hash_get(danglers, parent_abspath, APR_HASH_KEY_STRING))
            {
              apr_hash_set(danglers,
                           apr_pstrdup(result_pool, parent_abspath),
                           APR_HASH_KEY_STRING,
                           apr_pstrdup(result_pool, local_abspath));
            }
        }
    }

  if ((state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE)
      && !(state_flags & SVN_CLIENT_COMMIT_ITEM_ADD))
    {
      /* Skip all descendants */
      if (status->kind == svn_node_dir)
        baton->skip_below_abspath = apr_pstrdup(baton->result_pool,
                                                local_abspath);
      return SVN_NO_ERROR;
    }

  /* Recursively handle each node according to depth, except when the
     node is only being deleted, or is in an added tree (as added trees
     use the normal commit handling). */
  if (copy_mode && !is_added && !is_deleted && status->kind == svn_node_dir)
    {
      SVN_ERR(harvest_not_present_for_copy(wc_ctx, local_abspath, committables,
                                           repos_root_url, commit_relpath,
                                           baton->check_url_func,
                                           baton->check_url_baton,
                                           result_pool, scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Baton for handle_descendants */
struct handle_descendants_baton
{
  svn_wc_context_t *wc_ctx;
  svn_cancel_func_t cancel_func;
  void *cancel_baton;
  svn_client__check_url_kind_t check_url_func;
  void *check_url_baton;
};

/* Helper for the commit harvesters */
static svn_error_t *
handle_descendants(void *baton,
                       const void *key, apr_ssize_t klen, void *val,
                       apr_pool_t *pool)
{
  struct handle_descendants_baton *hdb = baton;
  apr_array_header_t *commit_items = val;
  apr_pool_t *iterpool = svn_pool_create(pool);
  int i;

  for (i = 0; i < commit_items->nelts; i++)
    {
      svn_client_commit_item3_t *item =
        APR_ARRAY_IDX(commit_items, i, svn_client_commit_item3_t *);
      const apr_array_header_t *absent_descendants;
      int j;

      /* Is this a copy operation? */
      if (!(item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD)
          || ! item->copyfrom_url)
        continue;

      if (hdb->cancel_func)
        SVN_ERR(hdb->cancel_func(hdb->cancel_baton));

      svn_pool_clear(iterpool);

      SVN_ERR(svn_wc__get_not_present_descendants(&absent_descendants,
                                                  hdb->wc_ctx, item->path,
                                                  iterpool, iterpool));

      for (j = 0; j < absent_descendants->nelts; j++)
        {
          int k;
          svn_boolean_t found_item = FALSE;
          svn_node_kind_t kind;
          const char *relpath = APR_ARRAY_IDX(absent_descendants, j,
                                              const char *);
          const char *local_abspath = svn_dirent_join(item->path, relpath,
                                                      iterpool);

          /* If the path has a commit operation, we do nothing.
             (It will be deleted by the operation) */
          for (k = 0; k < commit_items->nelts; k++)
            {
              svn_client_commit_item3_t *cmt_item =
                 APR_ARRAY_IDX(commit_items, k, svn_client_commit_item3_t *);

              if (! strcmp(cmt_item->path, local_abspath))
                {
                  found_item = TRUE;
                  break;
                }
            }

          if (found_item)
            continue; /* We have an explicit delete or replace for this path */

          /* ### Need a sub-iterpool? */

          if (hdb->check_url_func)
            {
              const char *from_url = svn_path_url_add_component2(
                                                item->copyfrom_url, relpath,
                                                iterpool);

              SVN_ERR(hdb->check_url_func(hdb->check_url_baton,
                                          &kind, from_url, item->copyfrom_rev,
                                          iterpool));

              if (kind == svn_node_none)
                continue; /* This node is already deleted */
            }
          else
            kind = svn_node_unknown; /* 'Ok' for a delete of something */

          {
            /* Add a new commit item that describes the delete */
            apr_pool_t *result_pool = commit_items->pool;
            svn_client_commit_item3_t *new_item
                  = svn_client_commit_item3_create(result_pool);

            new_item->path = svn_dirent_join(item->path, relpath,
                                             result_pool);
            new_item->kind = kind;
            new_item->url = svn_path_url_add_component2(item->url, relpath,
                                                        result_pool);
            new_item->revision = SVN_INVALID_REVNUM;
            new_item->state_flags = SVN_CLIENT_COMMIT_ITEM_DELETE;
            new_item->incoming_prop_changes = apr_array_make(result_pool, 1,
                                                 sizeof(svn_prop_t *));

            APR_ARRAY_PUSH(commit_items, svn_client_commit_item3_t *)
                  = new_item;
          }
        }
      }

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

/* Allocate and initialize the COMMITTABLES structure from POOL.
 */
static void
create_committables(svn_client__committables_t **committables,
                    apr_pool_t *pool)
{
  *committables = apr_palloc(pool, sizeof(**committables));

  (*committables)->by_repository = apr_hash_make(pool);
  (*committables)->by_path = apr_hash_make(pool);
}

svn_error_t *
svn_client__harvest_committables(svn_client__committables_t **committables,
                                 apr_hash_t **lock_tokens,
                                 const char *base_dir_abspath,
                                 const apr_array_header_t *targets,
                                 svn_depth_t depth,
                                 svn_boolean_t just_locked,
                                 const apr_array_header_t *changelists,
                                 svn_client__check_url_kind_t check_url_func,
                                 void *check_url_baton,
                                 svn_client_ctx_t *ctx,
                                 apr_pool_t *result_pool,
                                 apr_pool_t *scratch_pool)
{
  int i;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_hash_t *changelist_hash = NULL;
  struct handle_descendants_baton hdb;
  apr_hash_index_t *hi;

  /* It's possible that one of the named targets has a parent that is
   * itself scheduled for addition or replacement -- that is, the
   * parent is not yet versioned in the repository.  This is okay, as
   * long as the parent itself is part of this same commit, either
   * directly, or by virtue of a grandparent, great-grandparent, etc,
   * being part of the commit.
   *
   * Since we don't know what's included in the commit until we've
   * harvested all the targets, we can't reliably check this as we
   * go.  So in `danglers', we record named targets whose parents
   * do not yet exist in the repository. Then after harvesting the total
   * commit group, we check to make sure those parents are included.
   *
   * Each key of danglers is a parent which does not exist in the
   * repository.  The (const char *) value is one of that parent's
   * children which is named as part of the commit; the child is
   * included only to make a better error message.
   *
   * (The reason we don't bother to check unnamed -- i.e, implicit --
   * targets is that they can only join the commit if their parents
   * did too, so this situation can't arise for them.)
   */
  apr_hash_t *danglers = apr_hash_make(scratch_pool);

  SVN_ERR_ASSERT(svn_dirent_is_absolute(base_dir_abspath));

  /* Create the COMMITTABLES structure. */
  create_committables(committables, result_pool);

  /* And the LOCK_TOKENS dito. */
  *lock_tokens = apr_hash_make(result_pool);

  /* If we have a list of changelists, convert that into a hash with
     changelist keys. */
  if (changelists && changelists->nelts)
    SVN_ERR(svn_hash_from_cstring_keys(&changelist_hash, changelists,
                                       scratch_pool));

  for (i = 0; i < targets->nelts; ++i)
    {
      const char *target_abspath;

      svn_pool_clear(iterpool);

      /* Add the relative portion to the base abspath.  */
      target_abspath = svn_dirent_join(base_dir_abspath,
                                       APR_ARRAY_IDX(targets, i, const char *),
                                       iterpool);

      /* Handle our TARGET. */
      /* Make sure this isn't inside a working copy subtree that is
       * marked as tree-conflicted. */
      SVN_ERR(bail_on_tree_conflicted_ancestor(ctx->wc_ctx, target_abspath,
                                               ctx->notify_func2,
                                               ctx->notify_baton2,
                                               iterpool));

      SVN_ERR(harvest_committables(target_abspath,
                                   *committables, *lock_tokens,
                                   NULL /* COPY_MODE_RELPATH */,
                                   depth, just_locked, changelist_hash,
                                   danglers,
                                   check_url_func, check_url_baton,
                                   ctx->cancel_func, ctx->cancel_baton,
                                   ctx->notify_func2, ctx->notify_baton2,
                                   ctx->wc_ctx, result_pool, iterpool));
    }

  hdb.wc_ctx = ctx->wc_ctx;
  hdb.cancel_func = ctx->cancel_func;
  hdb.cancel_baton = ctx->cancel_baton;
  hdb.check_url_func = check_url_func;
  hdb.check_url_baton = check_url_baton;

  SVN_ERR(svn_iter_apr_hash(NULL, (*committables)->by_repository,
                            handle_descendants, &hdb, iterpool));

  /* Make sure that every path in danglers is part of the commit. */
  for (hi = apr_hash_first(scratch_pool, danglers); hi; hi = apr_hash_next(hi))
    {
      const char *dangling_parent = svn__apr_hash_index_key(hi);

      svn_pool_clear(iterpool);

      if (! look_up_committable(*committables, dangling_parent, iterpool))
        {
          const char *dangling_child = svn__apr_hash_index_val(hi);

          if (ctx->notify_func2 != NULL)
            {
              svn_wc_notify_t *notify;

              notify = svn_wc_create_notify(dangling_child,
                                            svn_wc_notify_failed_no_parent,
                                            scratch_pool);

              ctx->notify_func2(ctx->notify_baton2, notify, iterpool);
            }

          return svn_error_createf(
                           SVN_ERR_ILLEGAL_TARGET, NULL,
                           _("'%s' is not known to exist in the repository "
                             "and is not part of the commit, "
                             "yet its child '%s' is part of the commit"),
                           /* Probably one or both of these is an entry, but
                              safest to local_stylize just in case. */
                           svn_dirent_local_style(dangling_parent, iterpool),
                           svn_dirent_local_style(dangling_child, iterpool));
        }
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

struct copy_committables_baton
{
  svn_client_ctx_t *ctx;
  svn_client__committables_t *committables;
  apr_pool_t *result_pool;
  svn_client__check_url_kind_t check_url_func;
  void *check_url_baton;
};

static svn_error_t *
harvest_copy_committables(void *baton, void *item, apr_pool_t *pool)
{
  struct copy_committables_baton *btn = baton;
  svn_client__copy_pair_t *pair = *(svn_client__copy_pair_t **)item;
  const char *repos_root_url;
  const char *commit_relpath;
  struct handle_descendants_baton hdb;

  /* Read the entry for this SRC. */
  SVN_ERR_ASSERT(svn_dirent_is_absolute(pair->src_abspath_or_url));

  SVN_ERR(svn_wc__node_get_repos_info(&repos_root_url, NULL, btn->ctx->wc_ctx,
                                      pair->src_abspath_or_url,
                                      pool, pool));

  commit_relpath = svn_uri_skip_ancestor(repos_root_url,
                                         pair->dst_abspath_or_url, pool);

  /* Handle this SRC. */
  SVN_ERR(harvest_committables(pair->src_abspath_or_url,
                               btn->committables, NULL,
                               commit_relpath,
                               svn_depth_infinity,
                               FALSE,  /* JUST_LOCKED */
                               NULL /* changelists */,
                               NULL,
                               btn->check_url_func,
                               btn->check_url_baton,
                               btn->ctx->cancel_func,
                               btn->ctx->cancel_baton,
                               btn->ctx->notify_func2,
                               btn->ctx->notify_baton2,
                               btn->ctx->wc_ctx, btn->result_pool, pool));

  hdb.wc_ctx = btn->ctx->wc_ctx;
  hdb.cancel_func = btn->ctx->cancel_func;
  hdb.cancel_baton = btn->ctx->cancel_baton;
  hdb.check_url_func = btn->check_url_func;
  hdb.check_url_baton = btn->check_url_baton;

  SVN_ERR(svn_iter_apr_hash(NULL, btn->committables->by_repository,
                            handle_descendants, &hdb, pool));

  return SVN_NO_ERROR;
}



svn_error_t *
svn_client__get_copy_committables(svn_client__committables_t **committables,
                                  const apr_array_header_t *copy_pairs,
                                  svn_client__check_url_kind_t check_url_func,
                                  void *check_url_baton,
                                  svn_client_ctx_t *ctx,
                                  apr_pool_t *result_pool,
                                  apr_pool_t *scratch_pool)
{
  struct copy_committables_baton btn;

  /* Create the COMMITTABLES structure. */
  create_committables(committables, result_pool);

  btn.ctx = ctx;
  btn.committables = *committables;
  btn.result_pool = result_pool;

  btn.check_url_func = check_url_func;
  btn.check_url_baton = check_url_baton;

  /* For each copy pair, harvest the committables for that pair into the
     committables hash. */
  return svn_iter_apr_array(NULL, copy_pairs,
                            harvest_copy_committables, &btn, scratch_pool);
}


int svn_client__sort_commit_item_urls(const void *a, const void *b)
{
  const svn_client_commit_item3_t *item1
    = *((const svn_client_commit_item3_t * const *) a);
  const svn_client_commit_item3_t *item2
    = *((const svn_client_commit_item3_t * const *) b);
  return svn_path_compare_paths(item1->url, item2->url);
}



svn_error_t *
svn_client__condense_commit_items(const char **base_url,
                                  apr_array_header_t *commit_items,
                                  apr_pool_t *pool)
{
  apr_array_header_t *ci = commit_items; /* convenience */
  const char *url;
  svn_client_commit_item3_t *item, *last_item = NULL;
  int i;

  SVN_ERR_ASSERT(ci && ci->nelts);

  /* Sort our commit items by their URLs. */
  qsort(ci->elts, ci->nelts,
        ci->elt_size, svn_client__sort_commit_item_urls);

  /* Loop through the URLs, finding the longest usable ancestor common
     to all of them, and making sure there are no duplicate URLs.  */
  for (i = 0; i < ci->nelts; i++)
    {
      item = APR_ARRAY_IDX(ci, i, svn_client_commit_item3_t *);
      url = item->url;

      if ((last_item) && (strcmp(last_item->url, url) == 0))
        return svn_error_createf
          (SVN_ERR_CLIENT_DUPLICATE_COMMIT_URL, NULL,
           _("Cannot commit both '%s' and '%s' as they refer to the same URL"),
           svn_dirent_local_style(item->path, pool),
           svn_dirent_local_style(last_item->path, pool));

      /* In the first iteration, our BASE_URL is just our only
         encountered commit URL to date.  After that, we find the
         longest ancestor between the current BASE_URL and the current
         commit URL.  */
      if (i == 0)
        *base_url = apr_pstrdup(pool, url);
      else
        *base_url = svn_uri_get_longest_ancestor(*base_url, url, pool);

      /* If our BASE_URL is itself a to-be-committed item, and it is
         anything other than an already-versioned directory with
         property mods, we'll call its parent directory URL the
         BASE_URL.  Why?  Because we can't have a file URL as our base
         -- period -- and all other directory operations (removal,
         addition, etc.) require that we open that directory's parent
         dir first.  */
      /* ### I don't understand the strlen()s here, hmmm.  -kff */
      if ((strlen(*base_url) == strlen(url))
          && (! ((item->kind == svn_node_dir)
                 && item->state_flags == SVN_CLIENT_COMMIT_ITEM_PROP_MODS)))
        *base_url = svn_uri_dirname(*base_url, pool);

      /* Stash our item here for the next iteration. */
      last_item = item;
    }

  /* Now that we've settled on a *BASE_URL, go hack that base off
     of all of our URLs and store it as session_relpath. */
  for (i = 0; i < ci->nelts; i++)
    {
      svn_client_commit_item3_t *this_item
        = APR_ARRAY_IDX(ci, i, svn_client_commit_item3_t *);

      this_item->session_relpath = svn_uri_skip_ancestor(*base_url,
                                                         this_item->url, pool);
    }
#ifdef SVN_CLIENT_COMMIT_DEBUG
  /* ### TEMPORARY CODE ### */
  SVN_DBG(("COMMITTABLES: (base URL=%s)\n", *base_url));
  SVN_DBG(("   FLAGS     REV  REL-URL (COPY-URL)\n"));
  for (i = 0; i < ci->nelts; i++)
    {
      svn_client_commit_item3_t *this_item
        = APR_ARRAY_IDX(ci, i, svn_client_commit_item3_t *);
      char flags[6];
      flags[0] = (this_item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD)
                   ? 'a' : '-';
      flags[1] = (this_item->state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE)
                   ? 'd' : '-';
      flags[2] = (this_item->state_flags & SVN_CLIENT_COMMIT_ITEM_TEXT_MODS)
                   ? 't' : '-';
      flags[3] = (this_item->state_flags & SVN_CLIENT_COMMIT_ITEM_PROP_MODS)
                   ? 'p' : '-';
      flags[4] = (this_item->state_flags & SVN_CLIENT_COMMIT_ITEM_IS_COPY)
                   ? 'c' : '-';
      flags[5] = '\0';
      SVN_DBG(("   %s  %6ld  '%s' (%s)\n",
               flags,
               this_item->revision,
               this_item->url ? this_item->url : "",
               this_item->copyfrom_url ? this_item->copyfrom_url : "none"));
    }
#endif /* SVN_CLIENT_COMMIT_DEBUG */

  return SVN_NO_ERROR;
}


struct file_mod_t
{
  const svn_client_commit_item3_t *item;
  void *file_baton;
};


/* A baton for use while driving a path-based editor driver for commit */
struct item_commit_baton
{
  const svn_delta_editor_t *editor;    /* commit editor */
  void *edit_baton;                    /* commit editor's baton */
  apr_hash_t *file_mods;               /* hash: path->file_mod_t */
  const char *notify_path_prefix;      /* notification path prefix
                                          (NULL is okay, else abs path) */
  svn_client_ctx_t *ctx;               /* client context baton */
  apr_hash_t *commit_items;            /* the committables */
  const char *base_url;                /* The session url for the commit */
};


/* Drive CALLBACK_BATON->editor with the change described by the item in
 * CALLBACK_BATON->commit_items that is keyed by PATH.  If the change
 * includes a text mod, however, call the editor's file_open() function
 * but do not send the text mod to the editor; instead, add a mapping of
 * "item-url => (commit-item, file-baton)" into CALLBACK_BATON->file_mods.
 *
 * Before driving the editor, call the cancellation and notification
 * callbacks in CALLBACK_BATON->ctx, if present.
 *
 * This implements svn_delta_path_driver_cb_func_t. */
static svn_error_t *
do_item_commit(svn_client_commit_item3_t *item,
               svn_editor_t *editor,
               const char *notify_path_prefix,
               const char *repos_root,
               apr_hash_t *checksums,
               apr_hash_t *new_children,
               svn_client_ctx_t *ctx,
               apr_pool_t *scratch_pool)
{
  svn_node_kind_t kind = item->kind;
  const char *local_abspath = NULL;
  apr_hash_t *props = NULL;
  svn_checksum_t *sha1_checksum = NULL;
  svn_checksum_t *md5_checksum = NULL;
  svn_stream_t *contents = NULL;
  svn_revnum_t replaces_rev = SVN_INVALID_REVNUM;
  const char *repos_relpath = svn_uri_skip_ancestor(repos_root, item->url,
                                                    scratch_pool);

  /* Do some initializations. */
  if (item->kind != svn_node_none && item->path)
    {
      /* We always get an absolute path, see svn_client_commit_item3_t. */
      SVN_ERR_ASSERT(svn_dirent_is_absolute(item->path));
      local_abspath = item->path;
    }

  /* Validation. */
  if (item->state_flags & SVN_CLIENT_COMMIT_ITEM_IS_COPY)
    {
      if (! item->copyfrom_url)
        return svn_error_createf
          (SVN_ERR_BAD_URL, NULL,
           _("Commit item '%s' has copy flag but no copyfrom URL"),
           svn_dirent_local_style(item->path, scratch_pool));
      if (! SVN_IS_VALID_REVNUM(item->copyfrom_rev))
        return svn_error_createf
          (SVN_ERR_CLIENT_BAD_REVISION, NULL,
           _("Commit item '%s' has copy flag but an invalid revision"),
           svn_dirent_local_style(item->path, scratch_pool));
    }

  /* If a feedback table was supplied by the application layer,
     describe what we're about to do to this item. */
  if (ctx->notify_func2 && item->path)
    {
      const char *npath = item->path;
      svn_wc_notify_t *notify;

      if ((item->state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE)
          && (item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD))
        {
          /* We don't print the "(bin)" notice for binary files when
             replacing, only when adding.  So we don't bother to get
             the mime-type here. */
          if (item->copyfrom_url)
            notify = svn_wc_create_notify(npath,
                                          svn_wc_notify_commit_copied_replaced,
                                          scratch_pool);
          else
            notify = svn_wc_create_notify(npath, svn_wc_notify_commit_replaced,
                                          scratch_pool);

        }
      else if (item->state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE)
        {
          notify = svn_wc_create_notify(npath, svn_wc_notify_commit_deleted,
                                        scratch_pool);
        }
      else if (item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD)
        {
          if (item->copyfrom_url)
            notify = svn_wc_create_notify(npath, svn_wc_notify_commit_copied,
                                          scratch_pool);
          else
            notify = svn_wc_create_notify(npath, svn_wc_notify_commit_added,
                                          scratch_pool);

          if (item->kind == svn_node_file)
            {
              const svn_string_t *propval;

              SVN_ERR(svn_wc_prop_get2(&propval, ctx->wc_ctx, local_abspath,
                                       SVN_PROP_MIME_TYPE, scratch_pool,
                                       scratch_pool));

              if (propval)
                notify->mime_type = propval->data;
            }
        }
      else if ((item->state_flags & SVN_CLIENT_COMMIT_ITEM_TEXT_MODS)
               || (item->state_flags & SVN_CLIENT_COMMIT_ITEM_PROP_MODS))
        {
          notify = svn_wc_create_notify(npath, svn_wc_notify_commit_modified,
                                        scratch_pool);
          if (item->state_flags & SVN_CLIENT_COMMIT_ITEM_TEXT_MODS)
            notify->content_state = svn_wc_notify_state_changed;
          else
            notify->content_state = svn_wc_notify_state_unchanged;
          if (item->state_flags & SVN_CLIENT_COMMIT_ITEM_PROP_MODS)
            notify->prop_state = svn_wc_notify_state_changed;
          else
            notify->prop_state = svn_wc_notify_state_unchanged;
        }
      else
        notify = NULL;

      if (notify)
        {
          notify->kind = item->kind;
          notify->path_prefix = notify_path_prefix;
          (*ctx->notify_func2)(ctx->notify_baton2, notify, scratch_pool);
        }
    }

  /* If this item is supposed to be deleted, do so. */
  if ((item->state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE)
        && !(item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD))
    {
      SVN_ERR(svn_editor_delete(editor, repos_relpath, item->revision));

      return SVN_NO_ERROR;
    }

/*  if ((item->state_flags & SVN_CLIENT_COMMIT_ITEM_PROP_MODS) ||
        (item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD))*/
    {
      if (item->path)
        SVN_ERR(svn_wc_prop_list2(&props, ctx->wc_ctx, item->path,
                                  scratch_pool, scratch_pool));
      else
        props = apr_hash_make(scratch_pool);
    }

  if ((kind == svn_node_file)
      && (item->state_flags & SVN_CLIENT_COMMIT_ITEM_TEXT_MODS))
    {
      const char *pristine_tempdir;
      const char *pristine_temppath;
      svn_stream_t *tmp_stream;

      /* Get a de-translated stream of the working contents, along with an
         appropriate checksum. */
      SVN_ERR(svn_client__get_detranslated_stream(&contents, &sha1_checksum,
                                                  &md5_checksum,
                                                  item->path, props,
                                                  scratch_pool,
                                                  scratch_pool));

      /* This is all messed up.
         Pristine installation has traditionally happened during the commit,
         as libsvn_wc was transmitting deltas.  We don't do that anymore, so
         we have to install pristines elsewhere.

         Ideally, we'd do it post commit, so that we don't have non-used
         pristines just laying around in the case of error during
         transmission.  Also ideally, we'd detranslate the file directly to
         disk, and then just move that into place.

         Unfortunately, we aren't yet ideal, so the following will have to
         suffice. */
      SVN_ERR(svn_wc__node_pristine_get_tempdir(&pristine_tempdir,
                                                ctx->wc_ctx, item->path,
                                                scratch_pool, scratch_pool));
      SVN_ERR(svn_stream_open_unique(&tmp_stream, &pristine_temppath,
                                     pristine_tempdir,
                                     svn_io_file_del_none, scratch_pool,
                                     scratch_pool));
      SVN_ERR(svn_stream_copy3(contents, tmp_stream, ctx->cancel_func,
                               ctx->cancel_baton, scratch_pool));

      SVN_ERR(svn_wc__node_pristine_install(ctx->wc_ctx, pristine_temppath,
                                            sha1_checksum, md5_checksum,
                                            scratch_pool));

      SVN_ERR(svn_wc__get_pristine_contents_by_checksum(&contents,
                                                        ctx->wc_ctx,
                                                        item->path,
                                                        sha1_checksum,
                                                        scratch_pool,
                                                        scratch_pool));
    }

  if ((item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD) &&
        (item->state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE))
    {
      replaces_rev = item->revision;
    }

  /* If this item is supposed to be added, do so. */
  if ((item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD) &&
        !(item->state_flags & SVN_CLIENT_COMMIT_ITEM_IS_COPY))
    {
      if (item->kind == svn_node_file)
        {
          SVN_ERR_ASSERT(props != NULL);
          SVN_ERR_ASSERT(contents != NULL);
          SVN_ERR_ASSERT(sha1_checksum != NULL);

          SVN_ERR(svn_editor_add_file(editor, repos_relpath,
                                      sha1_checksum, contents, props,
                                      replaces_rev));
        }
      else /* May be svn_node_none when adding parent dirs for a copy. */
        {
          apr_array_header_t *children;

          SVN_ERR_ASSERT(props != NULL);

          children = apr_hash_get(new_children, item->session_relpath,
                                  APR_HASH_KEY_STRING);
          if (!children)
            children = apr_array_make(scratch_pool, 0, sizeof(const char *));

          SVN_ERR(svn_editor_add_directory(editor, repos_relpath,
                                           children, props,
                                           replaces_rev));
        }
    }

  if (item->state_flags & SVN_CLIENT_COMMIT_ITEM_IS_COPY)
    {
      const char *src_relpath = svn_uri_skip_ancestor(repos_root,
                                                      item->copyfrom_url,
                                                      scratch_pool);
      SVN_ERR(svn_editor_copy(editor, src_relpath, item->copyfrom_rev,
                              repos_relpath, replaces_rev));
    }

  if ((props || contents)
        && !(item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD))
    {
      if (item->kind == svn_node_file)
        {
          SVN_ERR(svn_editor_alter_file(editor, repos_relpath,
                                        item->revision, props,
                                        sha1_checksum, contents));
        }
      else
        {
          SVN_ERR(svn_editor_alter_directory(editor, repos_relpath,
                                             item->revision, NULL, props));
        }
    }

  if (checksums && sha1_checksum)
    {
      apr_hash_set(checksums, item->path, APR_HASH_KEY_STRING,
                   svn_checksum_dup(sha1_checksum,
                                    apr_hash_pool_get(checksums)));
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
do_item_commit_wrap_error(svn_client_commit_item3_t *item,
                          svn_editor_t *editor,
                          const char *notify_path_prefix,
                          const char *repos_root,
                          apr_hash_t *checksums,
                          apr_hash_t *new_children,
                          svn_client_ctx_t *ctx,
                          apr_pool_t *scratch_pool)
{
  svn_error_t *err = do_item_commit(item, editor, notify_path_prefix,
                                    repos_root, checksums, new_children, ctx,
                                    scratch_pool);

  if (err)
    {
      const char *repos_relpath = svn_uri_skip_ancestor(repos_root, item->url,
                                                        scratch_pool);

      return svn_error_trace(fixup_commit_error(item->path, repos_root,
                                                repos_relpath, item->kind,
                                                err, ctx, scratch_pool));
    }
  else
    return SVN_NO_ERROR;
}

svn_error_t *
svn_client__do_commit(const char *repos_root,
                      const apr_array_header_t *commit_items,
                      svn_editor_t *editor,
                      const char *notify_path_prefix,
                      apr_hash_t **sha1_checksums,
                      svn_client_ctx_t *ctx,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  apr_array_header_t *file_mods = apr_array_make(scratch_pool, 1,
                                         sizeof(svn_client_commit_item3_t *));
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_hash_t *checksums = apr_hash_make(result_pool);
  apr_hash_t *new_children = apr_hash_make(scratch_pool);
  int i;

  /* Loop to look for children of newly-added directories.
     
     ### This information is probably available earlier in the commit
     ### process, but we just don't capture it.  If/when we rework
     ### the commit item struct, we should include children as well. */
  for (i = 0; i < commit_items->nelts; i++)
    {
      svn_client_commit_item3_t *item =
        APR_ARRAY_IDX(commit_items, i, svn_client_commit_item3_t *);
      apr_array_header_t *children;
      const char *parent_relpath = svn_relpath_dirname(item->session_relpath,
                                                       scratch_pool);

      children = apr_hash_get(new_children, parent_relpath,
                              APR_HASH_KEY_STRING);
      if (!children)
        {
          children = apr_array_make(scratch_pool, 1, sizeof(const char *));
          apr_hash_set(new_children, parent_relpath, APR_HASH_KEY_STRING,
                       children);
        }
    
      APR_ARRAY_PUSH(children, const char *) =
            svn_relpath_basename(item->session_relpath, scratch_pool);
    }

  /* Build a hash from our COMMIT_ITEMS array, keyed on the
     relative paths (which come from the item URLs).  And
     keep an array of those decoded paths, too.  */
  for (i = 0; i < commit_items->nelts; i++)
    {
      svn_client_commit_item3_t *item =
        APR_ARRAY_IDX(commit_items, i, svn_client_commit_item3_t *);

      svn_pool_clear(iterpool);

      /* Call the cancellation function. */
      if (ctx->cancel_func)
        SVN_ERR(ctx->cancel_func(ctx->cancel_baton));

      SVN_ERR(do_item_commit_wrap_error(item, editor, notify_path_prefix,
                                        repos_root, checksums, new_children,
                                        ctx, iterpool));

      if ((item->kind == svn_node_file)
            && (item->state_flags & SVN_CLIENT_COMMIT_ITEM_TEXT_MODS))
        APR_ARRAY_PUSH(file_mods, svn_client_commit_item3_t *) = item;
    }

  /* This is the old habit of doing things, so right now we just spool several
     additional notifications to the client, saying we transmitted contents,
     even though it has already happened. */
  for (i = 0; i < file_mods->nelts; i++)
    {
      const svn_client_commit_item3_t *item =
                    APR_ARRAY_IDX(file_mods, i, svn_client_commit_item3_t *);
      svn_pool_clear(iterpool);

      if (ctx->notify_func2)
        {
          svn_wc_notify_t *notify;
          notify = svn_wc_create_notify(item->path,
                                        svn_wc_notify_commit_postfix_txdelta,
                                        iterpool);
          notify->kind = svn_node_file;
          notify->path_prefix = notify_path_prefix;
          ctx->notify_func2(ctx->notify_baton2, notify, iterpool);
        }

    }

  svn_pool_destroy(iterpool);

  if (sha1_checksums)
    *sha1_checksums = checksums;

  /* Close the edit. */
  return svn_error_trace(svn_editor_complete(editor));
}


svn_error_t *
svn_client__get_log_msg(const char **log_msg,
                        const char **tmp_file,
                        const apr_array_header_t *commit_items,
                        svn_client_ctx_t *ctx,
                        apr_pool_t *pool)
{
  if (ctx->log_msg_func3)
    {
      /* The client provided a callback function for the current API.
         Forward the call to it directly. */
      return (*ctx->log_msg_func3)(log_msg, tmp_file, commit_items,
                                   ctx->log_msg_baton3, pool);
    }
  else if (ctx->log_msg_func2 || ctx->log_msg_func)
    {
      /* The client provided a pre-1.5 (or pre-1.3) API callback
         function.  Convert the commit_items list to the appropriate
         type, and forward call to it. */
      svn_error_t *err;
      apr_pool_t *scratch_pool = svn_pool_create(pool);
      apr_array_header_t *old_commit_items =
        apr_array_make(scratch_pool, commit_items->nelts, sizeof(void*));

      int i;
      for (i = 0; i < commit_items->nelts; i++)
        {
          svn_client_commit_item3_t *item =
            APR_ARRAY_IDX(commit_items, i, svn_client_commit_item3_t *);

          if (ctx->log_msg_func2)
            {
              svn_client_commit_item2_t *old_item =
                apr_pcalloc(scratch_pool, sizeof(*old_item));

              old_item->path = item->path;
              old_item->kind = item->kind;
              old_item->url = item->url;
              old_item->revision = item->revision;
              old_item->copyfrom_url = item->copyfrom_url;
              old_item->copyfrom_rev = item->copyfrom_rev;
              old_item->state_flags = item->state_flags;
              old_item->wcprop_changes = item->incoming_prop_changes;

              APR_ARRAY_PUSH(old_commit_items, svn_client_commit_item2_t *) =
                old_item;
            }
          else /* ctx->log_msg_func */
            {
              svn_client_commit_item_t *old_item =
                apr_pcalloc(scratch_pool, sizeof(*old_item));

              old_item->path = item->path;
              old_item->kind = item->kind;
              old_item->url = item->url;
              /* The pre-1.3 API used the revision field for copyfrom_rev
                 and revision depeding of copyfrom_url. */
              old_item->revision = item->copyfrom_url ?
                item->copyfrom_rev : item->revision;
              old_item->copyfrom_url = item->copyfrom_url;
              old_item->state_flags = item->state_flags;
              old_item->wcprop_changes = item->incoming_prop_changes;

              APR_ARRAY_PUSH(old_commit_items, svn_client_commit_item_t *) =
                old_item;
            }
        }

      if (ctx->log_msg_func2)
        err = (*ctx->log_msg_func2)(log_msg, tmp_file, old_commit_items,
                                    ctx->log_msg_baton2, pool);
      else
        err = (*ctx->log_msg_func)(log_msg, tmp_file, old_commit_items,
                                   ctx->log_msg_baton, pool);
      svn_pool_destroy(scratch_pool);
      return err;
    }
  else
    {
      /* No log message callback was provided by the client. */
      *log_msg = "";
      *tmp_file = NULL;
      return SVN_NO_ERROR;
    }
}

svn_error_t *
svn_client__ensure_revprop_table(apr_hash_t **revprop_table_out,
                                 const apr_hash_t *revprop_table_in,
                                 const char *log_msg,
                                 svn_client_ctx_t *ctx,
                                 apr_pool_t *pool)
{
  apr_hash_t *new_revprop_table;
  if (revprop_table_in)
    {
      if (svn_prop_has_svn_prop(revprop_table_in, pool))
        return svn_error_create(SVN_ERR_CLIENT_PROPERTY_NAME, NULL,
                                _("Standard properties can't be set "
                                  "explicitly as revision properties"));
      new_revprop_table = apr_hash_copy(pool, revprop_table_in);
    }
  else
    {
      new_revprop_table = apr_hash_make(pool);
    }
  apr_hash_set(new_revprop_table, SVN_PROP_REVISION_LOG, APR_HASH_KEY_STRING,
               svn_string_create(log_msg, pool));
  *revprop_table_out = new_revprop_table;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__get_detranslated_stream(svn_stream_t **fstream,
                                    svn_checksum_t **sha1_checksum,
                                    svn_checksum_t **md5_checksum,
                                    const char *local_abspath,
                                    apr_hash_t *properties,
                                    apr_pool_t *result_pool,
                                    apr_pool_t *scratch_pool)
{
  svn_stream_t *contents;
  const svn_string_t *eol_style_val = NULL;
  const svn_string_t *keywords_val = NULL;
  svn_boolean_t special = FALSE;
  svn_subst_eol_style_t eol_style;
  const char *eol;
  apr_hash_t *keywords;

  /* If there are properties, look for EOL-style and keywords ones. */
  if (properties)
    {
      eol_style_val = apr_hash_get(properties, SVN_PROP_EOL_STYLE,
                                   sizeof(SVN_PROP_EOL_STYLE) - 1);
      keywords_val = apr_hash_get(properties, SVN_PROP_KEYWORDS,
                                  sizeof(SVN_PROP_KEYWORDS) - 1);
      if (apr_hash_get(properties, SVN_PROP_SPECIAL, APR_HASH_KEY_STRING))
        special = TRUE;
    }

  if (eol_style_val)
    svn_subst_eol_style_from_value(&eol_style, &eol, eol_style_val->data);
  else
    {
      eol = NULL;
      eol_style = svn_subst_eol_style_none;
    }

  if (keywords_val)
    SVN_ERR(svn_subst_build_keywords2(&keywords, keywords_val->data,
                                      APR_STRINGIFY(SVN_INVALID_REVNUM),
                                      "", 0, "", scratch_pool));
  else
    keywords = NULL;

  if (special)
    {
      SVN_ERR(svn_subst_read_specialfile(&contents, local_abspath,
                                         scratch_pool, scratch_pool));
    }
  else
    {
      /* Open the working copy file. */
      SVN_ERR(svn_stream_open_readonly(&contents, local_abspath, scratch_pool,
                                       scratch_pool));

      /* If we have EOL styles or keywords, then detranslate the file. */
      if (svn_subst_translation_required(eol_style, eol, keywords,
                                         FALSE, TRUE))
        {
          if (eol_style == svn_subst_eol_style_unknown)
            return svn_error_createf(SVN_ERR_IO_UNKNOWN_EOL, NULL,
                                    _("%s property on '%s' contains "
                                      "unrecognized EOL-style '%s'"),
                                    SVN_PROP_EOL_STYLE,
                                    svn_dirent_local_style(local_abspath,
                                                           scratch_pool),
                                    eol_style_val->data);

          /* We're importing, so translate files with 'native' eol-style to
           * repository-normal form, not to this platform's native EOL. */
          if (eol_style == svn_subst_eol_style_native)
            eol = SVN_SUBST_NATIVE_EOL_STR;

          /* Wrap the working copy stream with a filter to detranslate it. */
          contents = svn_subst_stream_translated(contents,
                                                 eol,
                                                 FALSE /* repair */,
                                                 keywords,
                                                 FALSE /* expand */,
                                                 scratch_pool);
        }
    }

  if (sha1_checksum)
    contents = svn_stream_checksummed2(contents, sha1_checksum, NULL,
                                       svn_checksum_sha1, TRUE, scratch_pool);
  if (md5_checksum)
    contents = svn_stream_checksummed2(contents, md5_checksum, NULL,
                                       svn_checksum_md5, TRUE, scratch_pool);

  *fstream = svn_stream_buffered(result_pool);
  SVN_ERR(svn_stream_copy3(contents, svn_stream_disown(*fstream, result_pool),
                           NULL, NULL, scratch_pool));

  return SVN_NO_ERROR;
}
