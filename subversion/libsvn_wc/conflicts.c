/*
 * conflicts.c: routines for managing conflict data.
 *            NOTE: this code doesn't know where the conflict is
 *            actually stored. 
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



#include <string.h>

#include <apr_pools.h>
#include <apr_tables.h>
#include <apr_hash.h>
#include <apr_file_io.h>
#include <apr_time.h>
#include <apr_errno.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_wc.h"
#include "svn_io.h"
#include "svn_diff.h"

#include "wc.h"
#include "log.h"
#include "adm_ops.h"
#include "props.h"
#include "tree_conflicts.h"
#include "workqueue.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"

struct svn_wc_conflict_t
{
  /* Pool conflict is allocated in */
  apr_pool_t *pool;

  /* ### kind + property name are the primary keys of a conflict */
  /* The kind of conflict recorded */
  svn_wc_conflict_kind_t kind;

  /* When describing a property conflict the property name
     or "" when no property name is available. (Upgrade from old WC or 
     raised via compatibility apis). */
  const char *property_name;

  /* ### TODO: Add more fields */
};

static svn_error_t *
conflict_alloc(svn_wc_conflict_t **conflict, apr_pool_t *result_pool)
{
  svn_wc_conflict_t *c = apr_pcalloc(result_pool, sizeof(*c));

  c->pool = result_pool;

  *conflict = c;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_conflict_dup(svn_wc_conflict_t **duplicate,
                    const svn_wc_conflict_t *base,
                    apr_pool_t *result_pool)
{
  svn_wc_conflict_t *c;
  if (result_pool == base->pool)
    {
      /* No need to duplicate; base has the same liftime and its inner
         values can't change */
      *duplicate = base;
      return SVN_NO_ERROR;
    }
  
  SVN_ERR(conflict_alloc(&c, result_pool));

  c->kind = base->kind;
  c->property_name = base->property_name
                          ? apr_pstrdup(result_pool, base->property_name)
                          : NULL;

  *duplicate = c;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_create_property_conflict(svn_wc_conflict_t **conflict,
                                const char *property_name,
                                const svn_wc_conflict_version_t *older_version,
                                const svn_wc_conflict_version_t *left_version,
                                const svn_wc_conflict_version_t *right_version,
                                const svn_string_t *older_value,
                                const svn_string_t *left_value,
                                const svn_string_t *right_value,
                                const char *marker_abspath,
                                svn_wc_operation_t operation,
                                apr_pool_t *result_pool,
                                apr_pool_t *scratch_pool)
{
  SVN_ERR_MALFUNCTION(); /* ### Not implemented yet */
}

svn_error_t *
svn_wc_create_text_conflict(svn_wc_conflict_t **conflict,
                            const svn_wc_conflict_version_t *older_version,
                            const svn_wc_conflict_version_t *left_version,
                            const svn_wc_conflict_version_t *right_version,
                            const char *older_abspath,
                            const char *left_abspath,
                            const char *right_abspath,
                            svn_wc_operation_t operation,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  SVN_ERR_MALFUNCTION(); /* ### Not implemented yet */
}

svn_error_t *
svn_wc_create_tree_conflict(svn_wc_conflict_t **conflict,
                            const svn_wc_conflict_version_t *older_version,
                            const svn_wc_conflict_version_t *left_version,
                            const svn_wc_conflict_version_t *right_version,
                            svn_wc_conflict_action_t action,
                            svn_wc_conflict_reason_t reason,
                            svn_wc_operation_t operation,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  SVN_ERR_MALFUNCTION(); /* ### Not implemented yet */
}

svn_error_t *
svn_wc_get_conflict_info(svn_wc_conflict_kind_t *kind,
                         const char **property_name,
                         svn_wc_conflict_action_t *action,
                         svn_wc_conflict_reason_t *reason,
                         svn_wc_operation_t *operation,
                         svn_boolean_t *conflict_resolved,
                         svn_wc_context_t *wc_ctx,
                         const char *local_abspath,
                         svn_wc_conflict_t *conflict,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  SVN_ERR_MALFUNCTION(); /* ### Not implemented yet */
}


svn_error_t *
svn_wc_get_conflict_marker_files(const char **older_abspath,
                                 const char **left_abspath,
                                 const char **right_abspath,
                                 svn_wc_context_t *wc_ctx,
                                 const char *local_abspath,
                                 svn_wc_conflict_t *conflict,
                                 apr_pool_t *result_pool,
                                 apr_pool_t *scratch_pool)
{
  SVN_ERR_MALFUNCTION(); /* ### Not implemented yet */
}

svn_error_t *
svn_wc_get_conflict_sources(const svn_wc_conflict_version_t **older_version,
                            const svn_wc_conflict_version_t **left_version,
                            const svn_wc_conflict_version_t **right_version,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            svn_wc_conflict_t *conflict,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  SVN_ERR_MALFUNCTION(); /* ### Not implemented yet */
}

svn_error_t *
svn_wc_get_property_conflict_data(const svn_string_t **older_value,
                                  const svn_string_t **left_value,
                                  const svn_string_t **right_value,
                                  svn_wc_context_t *wc_ctx,
                                  const char *local_abspath,
                                  svn_wc_conflict_t *conflict,
                                  apr_pool_t *result_pool,
                                  apr_pool_t *scratch_pool)
{
  SVN_ERR_MALFUNCTION(); /* ### Not implemented yet */
}


/*** Resolving a conflict automatically ***/


/* Helper for resolve_conflict_on_entry.  Delete the file BASE_NAME in
   PARENT_DIR if it exists.  Set WAS_PRESENT to TRUE if the file existed,
   and leave it UNTOUCHED otherwise. */
static svn_error_t *
attempt_deletion(const char *parent_dir,
                 const char *base_name,
                 svn_boolean_t *was_present,
                 apr_pool_t *scratch_pool)
{
  const char *full_path;
  svn_error_t *err;

  if (base_name == NULL)
    return SVN_NO_ERROR;

  full_path = svn_dirent_join(parent_dir, base_name, scratch_pool);
  err = svn_io_remove_file2(full_path, FALSE, scratch_pool);

  if (err == NULL || !APR_STATUS_IS_ENOENT(err->apr_err))
    {
      *was_present = TRUE;
      return svn_error_return(err);
    }

  svn_error_clear(err);
  return SVN_NO_ERROR;
}


/* Conflict resolution involves removing the conflict files, if they exist,
   and clearing the conflict filenames from the entry.  The latter needs to
   be done whether or not the conflict files exist.

   Tree conflicts are not resolved here, because the data stored in one
   entry does not refer to that entry but to children of it.

   PATH is the path to the item to be resolved, BASE_NAME is the basename
   of PATH, and CONFLICT_DIR is the access baton for PATH.  ORIG_ENTRY is
   the entry prior to resolution. RESOLVE_TEXT and RESOLVE_PROPS are TRUE
   if text and property conflicts respectively are to be resolved.

   If this call marks any conflict as resolved, set *DID_RESOLVE to true,
   else do not change *DID_RESOLVE.

   See svn_wc_resolved_conflict5() for how CONFLICT_CHOICE behaves.

   ### FIXME: This function should be loggy, otherwise an interruption can
   ### leave, for example, one of the conflict artifact files deleted but
   ### the entry still referring to it and trying to use it for the next
   ### attempt at resolving.
*/
static svn_error_t *
resolve_conflict_on_node(svn_wc__db_t *db,
                         const char *local_abspath,
                         svn_boolean_t resolve_text,
                         svn_boolean_t resolve_props,
                         svn_wc_conflict_choice_t conflict_choice,
                         svn_boolean_t *did_resolve,
                         apr_pool_t *pool)
{
  svn_boolean_t found_file;
  const char *conflict_old = NULL;
  const char *conflict_new = NULL;
  const char *conflict_working = NULL;
  const char *prop_reject_file = NULL;
  svn_wc__db_kind_t kind;
  int i;
  const apr_array_header_t *conflicts = NULL;
  const char *conflict_dir_abspath;

  SVN_ERR(svn_wc__db_read_info(NULL, &kind, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL,
                               db, local_abspath, pool, pool));
  SVN_ERR(svn_wc__db_read_conflicts(&conflicts, db, local_abspath,
                                    pool, pool));

  for (i = 0; i < conflicts->nelts; i++)
    {
      const svn_wc_conflict_description2_t *desc;

      desc = APR_ARRAY_IDX(conflicts, i,
                           const svn_wc_conflict_description2_t*);

      if (desc->kind == svn_wc_conflict_kind_text)
        {
          conflict_old = desc->base_file;
          conflict_new = desc->their_file;
          conflict_working = desc->my_file;
        }
      else if (desc->kind == svn_wc_conflict_kind_property)
        prop_reject_file = desc->their_file;
    }

  if (kind == svn_wc__db_kind_dir)
    conflict_dir_abspath = local_abspath;
  else
    conflict_dir_abspath = svn_dirent_dirname(local_abspath, pool);

  if (resolve_text)
    {
      const char *auto_resolve_src;

      /* Handle automatic conflict resolution before the temporary files are
       * deleted, if necessary. */
      switch (conflict_choice)
        {
        case svn_wc_conflict_choose_base:
          auto_resolve_src = conflict_old;
          break;
        case svn_wc_conflict_choose_mine_full:
          auto_resolve_src = conflict_working;
          break;
        case svn_wc_conflict_choose_theirs_full:
          auto_resolve_src = conflict_new;
          break;
        case svn_wc_conflict_choose_merged:
          auto_resolve_src = NULL;
          break;
        case svn_wc_conflict_choose_theirs_conflict:
        case svn_wc_conflict_choose_mine_conflict:
          {
            if (conflict_old && conflict_working && conflict_new)
              {
                apr_file_t *tmp_f;
                svn_stream_t *tmp_stream;
                svn_diff_t *diff;
                svn_diff_conflict_display_style_t style =
                  conflict_choice == svn_wc_conflict_choose_theirs_conflict
                  ? svn_diff_conflict_display_latest
                  : svn_diff_conflict_display_modified;

                SVN_ERR(svn_wc_create_tmp_file2(&tmp_f,
                                                &auto_resolve_src,
                                                conflict_dir_abspath,
                                                svn_io_file_del_none,
                                                pool));
                tmp_stream = svn_stream_from_aprfile2(tmp_f, FALSE, pool);
                SVN_ERR(svn_diff_file_diff3_2(&diff,
                                              conflict_old,
                                              conflict_working,
                                              conflict_new,
                                              svn_diff_file_options_create(pool),
                                              pool));
                SVN_ERR(svn_diff_file_output_merge2(tmp_stream, diff,
                                                    conflict_old,
                                                    conflict_working,
                                                    conflict_new,
                                                    /* markers ignored */
                                                    NULL, NULL, NULL, NULL,
                                                    style,
                                                    pool));
                SVN_ERR(svn_stream_close(tmp_stream));
              }
            else
              auto_resolve_src = NULL;
            break;
          }
        default:
          return svn_error_create(SVN_ERR_INCORRECT_PARAMS, NULL,
                                  _("Invalid 'conflict_result' argument"));
        }

      if (auto_resolve_src)
        SVN_ERR(svn_io_copy_file(
          svn_dirent_join(conflict_dir_abspath, auto_resolve_src, pool),
          local_abspath, TRUE, pool));
    }

  /* Records whether we found any of the conflict files.  */
  found_file = FALSE;

  if (resolve_text)
    {
      SVN_ERR(attempt_deletion(conflict_dir_abspath, conflict_old,
                               &found_file, pool));
      SVN_ERR(attempt_deletion(conflict_dir_abspath, conflict_new,
                               &found_file, pool));
      SVN_ERR(attempt_deletion(conflict_dir_abspath, conflict_working,
                               &found_file, pool));
      resolve_text = conflict_old || conflict_new || conflict_working;
    }
  if (resolve_props)
    {
      if (prop_reject_file != NULL)
        SVN_ERR(attempt_deletion(conflict_dir_abspath, prop_reject_file,
                                 &found_file, pool));
      else
        resolve_props = FALSE;
    }

  if (resolve_text || resolve_props)
    {
      SVN_ERR(svn_wc__db_op_mark_resolved(db, local_abspath,
                                          resolve_text, resolve_props,
                                          FALSE, pool));

      /* No feedback if no files were deleted and all we did was change the
         entry, such a file did not appear as a conflict */
      if (found_file)
        *did_resolve = TRUE;
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
resolve_one_conflict(svn_wc__db_t *db,
                     const char *local_abspath,
                     svn_boolean_t resolve_text,
                     const char *resolve_prop,
                     svn_boolean_t resolve_tree,
                     svn_wc_conflict_choice_t conflict_choice,
                     svn_cancel_func_t cancel_func,
                     void *cancel_baton,
                     svn_wc_notify_func2_t notify_func,
                     void *notify_baton,
                     apr_pool_t *scratch_pool)
{
  const apr_array_header_t *conflicts;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  int i;
  svn_boolean_t resolved;

  SVN_ERR(svn_wc__db_read_conflicts(&conflicts, db, local_abspath,
                                    scratch_pool, iterpool));

  for (i = 0; i < conflicts->nelts; i++)
    {
      const svn_wc_conflict_description2_t *cd;
      svn_boolean_t did_resolve;

      cd = APR_ARRAY_IDX(conflicts, i, const svn_wc_conflict_description2_t *);

      svn_pool_clear(iterpool);

      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      switch (cd->kind)
        {
          case svn_wc_conflict_kind_tree:
            if (!resolve_tree)
              break;

            /* For now, we only clear tree conflict information and resolve
             * to the working state. There is no way to pick theirs-full
             * or mine-full, etc. Throw an error if the user expects us
             * to be smarter than we really are. */
            if (conflict_choice != svn_wc_conflict_choose_merged)
            {
              return svn_error_createf(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE,
                                       NULL,
                                       _("Tree conflicts can only be resolved "
                                         "to 'working' state; "
                                         "'%s' not resolved"),
                                       svn_dirent_local_style(local_abspath,
                                                              iterpool));
            }

            SVN_ERR(svn_wc__db_op_set_tree_conflict(db, local_abspath, NULL,
                                                    iterpool));

            resolved = TRUE;
            break;

          case svn_wc_conflict_kind_text:
            if (!resolve_text)
              break;

            SVN_ERR(resolve_conflict_on_node(db,
                                             local_abspath,
                                             TRUE,
                                             FALSE,
                                             conflict_choice,
                                             &did_resolve,
                                             iterpool));

            if (did_resolve)
              resolved = TRUE;
            break;

          case svn_wc_conflict_kind_property:
            if (!resolve_prop)
              break;

            if (*resolve_prop != '\0' &&
                strcmp(resolve_prop, cd->property_name) != 0)
              {
                break; /* Skip this property conflict */
              }


            /* We don't have property name handling here yet :( */
            SVN_ERR(resolve_conflict_on_node(db,
                                             local_abspath,
                                             FALSE,
                                             TRUE,
                                             conflict_choice,
                                             &did_resolve,
                                             iterpool));

            if (did_resolve)
              resolved = TRUE;
            break;

          default:
            /* We can't resolve other conflict types */
            break;
        }
    }

  /* Notify */
  if (notify_func && resolved)
    notify_func(notify_baton,
                svn_wc_create_notify(local_abspath, svn_wc_notify_resolved,
                                     iterpool),
                iterpool);

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

static svn_error_t *
recursive_resolve_conflict(svn_wc__db_t *db,
                           const char *local_abspath,
                           svn_depth_t depth,
                           svn_boolean_t resolve_text,
                           const char *resolve_prop,
                           svn_boolean_t resolve_tree,
                           svn_wc_conflict_choice_t conflict_choice,
                           svn_cancel_func_t cancel_func,
                           void *cancel_baton,
                           svn_wc_notify_func2_t notify_func,
                           void *notify_baton,
                           apr_pool_t *scratch_pool)
{
  svn_boolean_t conflicted;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  const apr_array_header_t *children;
  apr_hash_t *visited = apr_hash_make(scratch_pool);
  svn_depth_t child_depth;
  svn_error_t *err;
  int i;

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  err = svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, &conflicted,
                             NULL,
                             db, local_abspath,
                             iterpool, iterpool);

  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    { /* Would be nice if we could just call svn_wc__db_read_info on
         conflict victims */
      svn_error_clear(err);
      conflicted = TRUE; /* Just resolve it */
    }
  else
    SVN_ERR(err);

  if (conflicted)
    {
      SVN_ERR(resolve_one_conflict(db,
                                   local_abspath,
                                   resolve_text,
                                   resolve_prop,
                                   resolve_tree,
                                   conflict_choice,
                                   cancel_func, cancel_baton,
                                   notify_func, notify_baton,
                                   iterpool));
    }

  if (depth < svn_depth_files)
    return SVN_NO_ERROR;

  child_depth = (depth < svn_depth_infinity) ? svn_depth_empty : depth;

  SVN_ERR(svn_wc__db_read_conflict_victims(&children, db, local_abspath,
                                           scratch_pool, iterpool));

  for (i = 0; i < children->nelts; i++)
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);
      const char *child_abspath;
      svn_wc__db_kind_t kind;

      svn_pool_clear(iterpool);

      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      child_abspath = svn_dirent_join(local_abspath, name, iterpool);

      SVN_ERR(svn_wc__db_read_kind(&kind, db, child_abspath, TRUE, iterpool));

      apr_hash_set(visited, name, APR_HASH_KEY_STRING, name);

      if (kind == svn_wc__db_kind_dir && depth < svn_depth_immediates)
        continue;

      SVN_ERR(recursive_resolve_conflict(db,
                                         child_abspath,
                                         child_depth,
                                         resolve_text,
                                         resolve_prop,
                                         resolve_tree,
                                         conflict_choice,
                                         cancel_func, cancel_baton,
                                         notify_func, notify_baton,
                                         iterpool));
    }

  SVN_ERR(svn_wc__db_read_children(&children, db, local_abspath,
                                   scratch_pool, iterpool));

  for (i = 0; i < children->nelts; i++)
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);
      const char *child_abspath;
      svn_wc__db_kind_t kind;
      svn_boolean_t hidden;

      svn_pool_clear(iterpool);

      if (apr_hash_get(visited, name, APR_HASH_KEY_STRING) != NULL)
        continue; /* Already visited */

      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      child_abspath = svn_dirent_join(local_abspath, name, iterpool);

      SVN_ERR(svn_wc__db_node_hidden(&hidden, db, child_abspath, iterpool));

      if (hidden)
        continue;

      SVN_ERR(svn_wc__db_read_kind(&kind, db, child_abspath, TRUE, iterpool));

      if (kind == svn_wc__db_kind_dir && depth < svn_depth_immediates)
        continue;

      SVN_ERR(recursive_resolve_conflict(db,
                                         child_abspath,
                                         child_depth,
                                         resolve_text,
                                         resolve_prop,
                                         resolve_tree,
                                         conflict_choice,
                                         cancel_func, cancel_baton,
                                         notify_func, notify_baton,
                                         iterpool));
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__internal_resolved_conflict(svn_wc__db_t *db,
                                   const char *local_abspath,
                                   svn_depth_t depth,
                                   svn_boolean_t resolve_text,
                                   const char *resolve_prop,
                                   svn_boolean_t resolve_tree,
                                   svn_wc_conflict_choice_t conflict_choice,
                                   svn_cancel_func_t cancel_func,
                                   void *cancel_baton,
                                   svn_wc_notify_func2_t notify_func,
                                   void *notify_baton,
                                   apr_pool_t *scratch_pool)
{
  /* When the implementation still used the entry walker, depth
     unknown was translated to infinity. */
  if (depth == svn_depth_unknown)
    depth = svn_depth_infinity;

  return svn_error_return(
    recursive_resolve_conflict(db,
                               local_abspath,
                               depth,
                               resolve_text,
                               resolve_prop,
                               resolve_tree,
                               conflict_choice,
                               cancel_func, cancel_baton,
                               notify_func, notify_baton,
                               scratch_pool));
}

/* The public function */
svn_error_t *
svn_wc_resolved_conflict5(svn_wc_context_t *wc_ctx,
                          const char *local_abspath,
                          svn_depth_t depth,
                          svn_boolean_t resolve_text,
                          const char *resolve_prop,
                          svn_boolean_t resolve_tree,
                          svn_wc_conflict_choice_t conflict_choice,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          apr_pool_t *scratch_pool)
{
  return svn_error_return(
    svn_wc__internal_resolved_conflict(wc_ctx->db,
                                       local_abspath,
                                       depth,
                                       resolve_text,
                                       resolve_prop,
                                       resolve_tree,
                                       conflict_choice,
                                       cancel_func, cancel_baton,
                                       notify_func, notify_baton,
                                       scratch_pool));
}
