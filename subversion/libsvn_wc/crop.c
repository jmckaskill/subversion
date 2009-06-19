/*
 * crop.c: Cropping the WC
 *
 * ====================================================================
 * Copyright (c) 2008 CollabNet.  All rights reserved.
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

#include "svn_wc.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_client.h"
#include "svn_error_codes.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"

#include "lock.h"
#include "entries.h"

#include "svn_private_config.h"

/* Evaluate EXPR.  If it returns an error, return that error, unless
   the error's code is SVN_ERR_WC_LEFT_LOCAL_MOD, in which case clear
   the error and do not return. */
#define IGNORE_LOCAL_MOD(expr)                                   \
  do {                                                           \
    svn_error_t *__temp = (expr);                                \
    if (__temp)                                                  \
      {                                                          \
        if (__temp->apr_err == SVN_ERR_WC_LEFT_LOCAL_MOD)        \
          svn_error_clear(__temp);                               \
        else                                                     \
          return __temp;                                         \
      }                                                          \
  } while (0)

/* Helper function that crops the children of the DIR_PATH, under the constraint
 * of DEPTH. The DIR_PATH itself will never be cropped. The ADM_ACCESS is the
 * access baton that contains DIR_PATH. And the whole subtree should have been
 * locked.
 *
 * If NOTIFY_FUNC is not null, each file and ROOT of subtree will be reported
 * upon remove.
 */
static svn_error_t *
crop_children(svn_wc__db_t *db,
              const char *dir_path,
              svn_depth_t depth,
              svn_wc_notify_func2_t notify_func,
              void *notify_baton,
              svn_cancel_func_t cancel_func,
              void *cancel_baton,
              apr_pool_t *pool)
{
  const char *local_dir_abspath;
  const apr_array_header_t *children;
  svn_depth_t dir_depth;
  apr_pool_t *iterpool;
  int i;

  SVN_ERR_ASSERT(depth != svn_depth_exclude);

  SVN_ERR(svn_dirent_get_absolute(&local_dir_abspath, dir_path, pool));

  SVN_ERR(svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, &dir_depth,
                               NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL,
                               db, local_dir_abspath, pool, pool));

  /* Update the depth of target first, if needed. */
  if (dir_depth > depth)
    {
      SVN_ERR(svn_wc__set_depth(db, local_dir_abspath, depth, pool));
    }

  /* Looping over current directory's SVN entries: */
  SVN_ERR(svn_wc__db_read_children(&children, db, local_dir_abspath, pool,
                                   pool));
  iterpool = svn_pool_create(pool);

  for (i = 0; i < children->nelts; i++)
    {
      const char *child_name = APR_ARRAY_IDX(children, i, const char *);
      const char *child_abspath;
      svn_wc__db_kind_t kind;
      svn_depth_t child_depth;

      svn_pool_clear(iterpool);

      /* Get the next entry */
      child_abspath = svn_dirent_join(local_dir_abspath, child_name, iterpool);

      SVN_ERR(svn_wc__db_read_info(NULL, &kind, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, &child_depth,
                                   NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL,
                                   db, child_abspath, iterpool, iterpool));

      if (kind == svn_wc__db_kind_file)
        {
          /* We currently crop on a directory basis. So don't worry about
             svn_depth_exclude here. And even we permit excluding a single
             file in the future, svn_wc_remove_from_revision_control() can
             also handle it. We only need to skip the notification in that
             case. */
          svn_wc_adm_access_t *dir_access = svn_wc__adm_retrieve_internal2(
                                                  db, local_dir_abspath, pool);
          SVN_ERR_ASSERT(dir_access != NULL);
             
          if (depth == svn_depth_empty)
            IGNORE_LOCAL_MOD
              (svn_wc_remove_from_revision_control(dir_access, child_name,
                                                   TRUE, /* destroy */
                                                   FALSE, /* instant error */
                                                   cancel_func, cancel_baton,
                                                   iterpool));
          else
            continue;

        }
      else if (kind == svn_wc__db_kind_dir)
        {
          const char *this_path = svn_dirent_join(dir_path, child_name,
                                                  iterpool);

          if (child_depth == svn_depth_exclude)
            {
              /* Preserve the excluded entry if the parent need it.
                 Anyway, don't report on excluded subdir, since they are
                 logically not exist. */
              if (depth < svn_depth_immediates)
                SVN_ERR(svn_wc__entry_remove(db, child_abspath, iterpool));
              continue;
            }
          else if (depth < svn_depth_immediates)
            {
              svn_wc_adm_access_t *child_access;
              svn_wc_adm_access_t *dir_access = svn_wc__adm_retrieve_internal2(
                                                  db, local_dir_abspath, pool);
              SVN_ERR_ASSERT(dir_access != NULL);
              SVN_ERR(svn_wc_adm_retrieve(&child_access, dir_access,
                                          this_path, iterpool));

              IGNORE_LOCAL_MOD
                (svn_wc_remove_from_revision_control(child_access,
                                                     SVN_WC_ENTRY_THIS_DIR,
                                                     TRUE, /* destroy */
                                                     FALSE, /* instant error */
                                                     cancel_func,
                                                     cancel_baton,
                                                     iterpool));
            }
          else
            {
              SVN_ERR(crop_children(db,
                                    this_path,
                                    svn_depth_empty,
                                    notify_func,
                                    notify_baton,
                                    cancel_func,
                                    cancel_baton,
                                    iterpool));
              continue;
            }
        }
      else
        {
          return svn_error_createf
            (SVN_ERR_NODE_UNKNOWN_KIND, NULL, _("Unknown entry kind for '%s'"),
             svn_path_local_style(child_abspath, iterpool));
        }

      if (notify_func)
        {
          svn_wc_notify_t *notify;
          notify = svn_wc_create_notify(child_abspath,
                                        svn_wc_notify_delete,
                                        iterpool);
          (*notify_func)(notify_baton, notify, iterpool);
        }
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_crop_tree(svn_wc_adm_access_t *anchor,
                 const char *target,
                 svn_depth_t depth,
                 svn_wc_notify_func2_t notify_func,
                 void *notify_baton,
                 svn_cancel_func_t cancel_func,
                 void *cancel_baton,
                 apr_pool_t *pool)
{
  svn_wc__db_t *db = svn_wc__adm_get_db(anchor);
  const svn_wc_entry_t *entry;
  const char *full_path;
  svn_wc_adm_access_t *dir_access;

  /* Only makes sense when the depth is restrictive. */
  if (depth == svn_depth_infinity)
    return SVN_NO_ERROR; /* Nothing to crop */
  if (!(depth >= svn_depth_exclude && depth < svn_depth_infinity))
    return svn_error_create(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
      _("Can only crop a working copy with a restrictive depth"));

  /* Only makes sense to crop a dir target. */
  full_path = svn_dirent_join(svn_wc_adm_access_path(anchor), target, pool);
  SVN_ERR(svn_wc_entry(&entry, full_path, anchor, FALSE, pool));
  if (!entry || entry->kind != svn_node_dir)
    return svn_error_create(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
      _("Can only crop directories"));

  /* Don't bother to crop if the target is scheduled delete. */
  if (entry->schedule == svn_wc_schedule_delete)
    return svn_error_createf
      (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
       _("Cannot crop '%s': it is going to be removed from repository."
         " Try commit instead"),
       svn_path_local_style(full_path, pool));

  /* Crop the target itself if we are requested to. */
  if (depth == svn_depth_exclude)
    {
      svn_boolean_t entry_in_repos;
      const svn_wc_entry_t *parent_entry = NULL;
      const char *local_dir_abspath;

      /* Safeguard on bad target. */
      if (*full_path == 0)
        return svn_error_createf
          (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
           _("Cannot exclude current directory"));

      if (svn_dirent_is_root(full_path, strlen(full_path)))
        return svn_error_createf
          (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
           _("Cannot exclude root directory"));

      SVN_ERR(svn_dirent_get_absolute(&local_dir_abspath, full_path, pool));

      /* This simulates the logic of svn_wc_is_wc_root(). */
        {
          const char *parent_abspath;
          const char *bname;
          svn_error_t *err;

          svn_dirent_split(local_dir_abspath, &parent_abspath, &bname, pool);
          err = svn_wc__get_entry(&parent_entry, db, parent_abspath, FALSE,
                                  svn_node_dir, FALSE, pool, pool);
          if (err)
            {
              /* Probably fell off the top of the working copy?  */
              svn_error_clear(err);
              parent_entry = NULL;
            }

          /* The server simply do not accept excluded link_path and thus
             switched path cannot be excluded. Just completely prohibit
             this situation. */
          if (entry->url
              && parent_entry
              && (strcmp(entry->url,
                         svn_path_url_add_component2(parent_entry->url, bname,
                                                     pool))))
            {
              return svn_error_createf
                (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                 _("Cannot crop '%s': it is a switched path"),
                 svn_path_local_style(full_path, pool));
            }
        }

      /* If the target entry is just added without history, it does not exist
         in the repos (in which case we won't exclude it). */
      entry_in_repos
        = ! ((entry->schedule == svn_wc_schedule_add
              || entry->schedule == svn_wc_schedule_replace)
             && ! entry->copied);

      /* Mark the target as excluded, if the parent requires it by
         default. */
      if (parent_entry && entry_in_repos
          && (parent_entry->depth > svn_depth_files))
        {
          SVN_ERR(svn_wc__set_depth(db, local_dir_abspath, svn_depth_exclude,
                                    pool));
        }

      /* TODO(#2843): Do we need to restore the modified depth if the user
         cancel this operation? */
      SVN_ERR(svn_wc_adm_retrieve(&dir_access, anchor, full_path, pool));
      IGNORE_LOCAL_MOD
        (svn_wc_remove_from_revision_control(dir_access,
                                             SVN_WC_ENTRY_THIS_DIR,
                                             TRUE, /* destroy */
                                             FALSE, /* instant error */
                                             cancel_func,
                                             cancel_baton,
                                             pool));

      if (notify_func)
        {
          svn_wc_notify_t *notify;
          notify = svn_wc_create_notify(full_path,
                                        svn_wc_notify_delete,
                                        pool);
          (*notify_func)(notify_baton, notify, pool);
        }
      return SVN_NO_ERROR;
    }

  return crop_children(db, full_path, depth,
                       notify_func, notify_baton,
                       cancel_func, cancel_baton, pool);
}
