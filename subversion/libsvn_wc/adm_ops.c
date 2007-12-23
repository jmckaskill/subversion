/*
 * adm_ops.c: routines for affecting working copy administrative
 *            information.  NOTE: this code doesn't know where the adm
 *            info is actually stored.  Instead, generic handles to
 *            adm data are requested via a reference to some PATH
 *            (PATH being a regular, non-administrative directory or
 *            file in the working copy).
 *
 * ====================================================================
 * Copyright (c) 2000-2007 CollabNet.  All rights reserved.
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



#include <string.h>

#include <apr_pools.h>
#include <apr_tables.h>
#include <apr_hash.h>
#include <apr_md5.h>
#include <apr_file_io.h>
#include <apr_time.h>
#include <apr_errno.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_wc.h"
#include "svn_io.h"
#include "svn_md5.h"
#include "svn_xml.h"
#include "svn_time.h"

#include "wc.h"
#include "log.h"
#include "adm_files.h"
#include "adm_ops.h"
#include "entries.h"
#include "lock.h"
#include "props.h"
#include "translate.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"


/*** Finishing updates and commits. ***/


/* The main body of svn_wc__do_update_cleanup. */
static svn_error_t *
tweak_entries(svn_wc_adm_access_t *dirpath,
              const char *base_url,
              const char *repos,
              svn_revnum_t new_rev,
              svn_wc_notify_func2_t notify_func,
              void *notify_baton,
              svn_boolean_t remove_missing_dirs,
              svn_depth_t depth,
              apr_hash_t *exclude_paths,
              apr_pool_t *pool)
{
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  apr_pool_t *subpool = svn_pool_create(pool);
  svn_boolean_t write_required = FALSE;
  svn_wc_notify_t *notify;

  /* Read DIRPATH's entries. */
  SVN_ERR(svn_wc_entries_read(&entries, dirpath, TRUE, pool));

  /* Tweak "this_dir" */
  if (! apr_hash_get(exclude_paths, svn_wc_adm_access_path(dirpath),
                     APR_HASH_KEY_STRING))
    SVN_ERR(svn_wc__tweak_entry(entries, SVN_WC_ENTRY_THIS_DIR,
                                base_url, repos, new_rev, FALSE,
                                &write_required,
                                svn_wc_adm_access_pool(dirpath)));

  if (depth == svn_depth_unknown)
    depth = svn_depth_infinity;

  if (depth > svn_depth_empty)
    {
      for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
        {
          const void *key;
          void *val;
          const char *name;
          svn_wc_entry_t *current_entry;
          const char *child_path;
          const char *child_url = NULL;
          svn_boolean_t excluded;

          svn_pool_clear(subpool);

          apr_hash_this(hi, &key, NULL, &val);
          name = key;
          current_entry = val;

          /* Ignore the "this dir" entry. */
          if (! strcmp(name, SVN_WC_ENTRY_THIS_DIR))
            continue;

          /* Derive the new URL for the current (child) entry */
          if (base_url)
            child_url = svn_path_url_add_component(base_url, name, subpool);

          child_path = svn_path_join(svn_wc_adm_access_path(dirpath), name,
                                     subpool);
          excluded = (apr_hash_get(exclude_paths, child_path,
                                   APR_HASH_KEY_STRING) != NULL);

          /* If a file, or deleted or absent dir, then tweak the entry
             but don't recurse. */
          if ((current_entry->kind == svn_node_file)
              || (current_entry->deleted || current_entry->absent))
            {
              if (! excluded)
                SVN_ERR(svn_wc__tweak_entry(entries, name,
                                            child_url, repos, new_rev, TRUE,
                                            &write_required,
                                            svn_wc_adm_access_pool(dirpath)));
            }

          /* If a directory and recursive... */
          else if ((depth == svn_depth_infinity
                    || depth == svn_depth_immediates)
                   && (current_entry->kind == svn_node_dir))
            {
              svn_depth_t depth_below_here = depth;

              if (depth == svn_depth_immediates)
                depth_below_here = svn_depth_empty;

              /* If the directory is 'missing', remove it.  This is safe as
                 long as this function is only called as a helper to
                 svn_wc__do_update_cleanup, since the update will already have
                 restored any missing items that it didn't want to delete. */
              if (remove_missing_dirs
                  && svn_wc__adm_missing(dirpath, child_path))
                {
                  if (current_entry->schedule != svn_wc_schedule_add
                      && !excluded)
                    {
                      svn_wc__entry_remove(entries, name);
                      if (notify_func)
                        {
                          notify = svn_wc_create_notify(child_path,
                                                        svn_wc_notify_delete,
                                                        subpool);
                          notify->kind = current_entry->kind;
                          (* notify_func)(notify_baton, notify, subpool);
                        }
                    }
                  /* Else if missing item is schedule-add, do nothing. */
                }

              /* Not missing, deleted, or absent, so recurse. */
              else
                {
                  svn_wc_adm_access_t *child_access;
                  SVN_ERR(svn_wc_adm_retrieve(&child_access, dirpath,
                                              child_path, subpool));
                  SVN_ERR(tweak_entries
                          (child_access, child_url, repos, new_rev,
                           notify_func, notify_baton, remove_missing_dirs,
                           depth_below_here, exclude_paths, subpool));
                }
            }
        }
    }

  /* Write a shiny new entries file to disk. */
  if (write_required)
    SVN_ERR(svn_wc__entries_write(entries, dirpath, subpool));

  /* Cleanup */
  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

/* Helper for svn_wc_process_committed2. */
static svn_error_t *
remove_revert_file(svn_stringbuf_t **logtags,
                   svn_wc_adm_access_t *adm_access,
                   const char *path,
                   svn_boolean_t is_prop,
                   apr_pool_t * pool)
{
  const char *revert_file;
  svn_node_kind_t kind;

  if (is_prop)
    SVN_ERR(svn_wc__loggy_props_delete(logtags, path, svn_wc__props_revert,
                                       adm_access, pool));
  else
    {
      revert_file = svn_wc__text_revert_path(path, FALSE, pool);

      SVN_ERR(svn_io_check_path(revert_file, &kind, pool));

      if (kind == svn_node_file)
        SVN_ERR(svn_wc__loggy_remove(logtags, adm_access, revert_file, pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__do_update_cleanup(const char *path,
                          svn_wc_adm_access_t *adm_access,
                          svn_depth_t depth,
                          const char *base_url,
                          const char *repos,
                          svn_revnum_t new_revision,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          svn_boolean_t remove_missing_dirs,
                          apr_hash_t *exclude_paths,
                          apr_pool_t *pool)
{
  apr_hash_t *entries;
  const svn_wc_entry_t *entry;

  SVN_ERR(svn_wc_entry(&entry, path, adm_access, TRUE, pool));
  if (entry == NULL)
    return SVN_NO_ERROR;

  if (entry->kind == svn_node_file
      || (entry->kind == svn_node_dir && (entry->deleted || entry->absent)))
    {
      const char *parent, *base_name;
      svn_wc_adm_access_t *dir_access;
      svn_boolean_t write_required = FALSE;
      if (apr_hash_get(exclude_paths, path, APR_HASH_KEY_STRING))
        return SVN_NO_ERROR;
      svn_path_split(path, &parent, &base_name, pool);
      SVN_ERR(svn_wc_adm_retrieve(&dir_access, adm_access, parent, pool));
      SVN_ERR(svn_wc_entries_read(&entries, dir_access, TRUE, pool));
      SVN_ERR(svn_wc__tweak_entry(entries, base_name,
                                  base_url, repos, new_revision,
                                  FALSE, /* Parent not updated so don't
                                            remove PATH entry */
                                  &write_required,
                                  svn_wc_adm_access_pool(dir_access)));
      if (write_required)
        SVN_ERR(svn_wc__entries_write(entries, dir_access, pool));
    }

  else if (entry->kind == svn_node_dir)
    {
      svn_wc_adm_access_t *dir_access;
      SVN_ERR(svn_wc_adm_retrieve(&dir_access, adm_access, path, pool));

      SVN_ERR(tweak_entries(dir_access, base_url, repos, new_revision,
                            notify_func, notify_baton, remove_missing_dirs,
                            depth, exclude_paths, pool));
    }

  else
    return svn_error_createf(SVN_ERR_NODE_UNKNOWN_KIND, NULL,
                             _("Unrecognized node kind: '%s'"),
                             svn_path_local_style(path, pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_maybe_set_repos_root(svn_wc_adm_access_t *adm_access,
                            const char *path,
                            const char *repos,
                            apr_pool_t *pool)
{
  apr_hash_t *entries;
  svn_boolean_t write_required = FALSE;
  const svn_wc_entry_t *entry;
  const char *base_name;
  svn_wc_adm_access_t *dir_access;

  SVN_ERR(svn_wc_entry(&entry, path, adm_access, FALSE, pool));
  if (! entry)
    return SVN_NO_ERROR;

  if (entry->kind == svn_node_file)
    {
      const char *parent;

      svn_path_split(path, &parent, &base_name, pool);
      SVN_ERR(svn_wc__adm_retrieve_internal(&dir_access, adm_access,
                                            parent, pool));
    }
  else
    {
      base_name = SVN_WC_ENTRY_THIS_DIR;
      SVN_ERR(svn_wc__adm_retrieve_internal(&dir_access, adm_access,
                                            path, pool));
    }

  if (! dir_access)
    return SVN_NO_ERROR;

  SVN_ERR(svn_wc_entries_read(&entries, dir_access, TRUE, pool));

  SVN_ERR(svn_wc__tweak_entry(entries, base_name,
                              NULL, repos, SVN_INVALID_REVNUM, FALSE,
                              &write_required,
                              svn_wc_adm_access_pool(dir_access)));

  if (write_required)
    SVN_ERR(svn_wc__entries_write(entries, dir_access, pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
process_committed_leaf(int log_number,
                       const char *path,
                       svn_wc_adm_access_t *adm_access,
                       svn_boolean_t *recurse,
                       svn_revnum_t new_revnum,
                       const char *rev_date,
                       const char *rev_author,
                       apr_array_header_t *wcprop_changes,
                       svn_boolean_t remove_lock,
                       svn_boolean_t remove_changelist,
                       const unsigned char *digest,
                       apr_pool_t *pool)
{
  const char *base_name;
  const char *hex_digest = NULL;
  svn_wc_entry_t tmp_entry;
  apr_uint64_t modify_flags = 0;
  svn_stringbuf_t *logtags = svn_stringbuf_create("", pool);

  SVN_ERR(svn_wc__adm_write_check(adm_access));

  /* Set PATH's working revision to NEW_REVNUM; if REV_DATE and
     REV_AUTHOR are both non-NULL, then set the 'committed-rev',
     'committed-date', and 'last-author' entry values; and set the
     checksum if a file. */

  base_name = svn_path_is_child(svn_wc_adm_access_path(adm_access), path,
                                pool);
  if (base_name)
    {
      /* If the props or text revert file exists it needs to be deleted when
       * the file is committed. */
      SVN_ERR(remove_revert_file(&logtags, adm_access, path, FALSE, pool));
      SVN_ERR(remove_revert_file(&logtags, adm_access, path, TRUE, pool));

      if (digest)
        hex_digest = svn_md5_digest_to_cstring(digest, pool);
      else
        {
          /* There may be a new text base sitting in the adm tmp area
             by now, because the commit succeeded.  A file that is
             copied, but not otherwise modified, doesn't have a new
             text base, so we use the unmodified text base.

             ### Does this mean that a file committed with only prop mods
             ### will still get its text base checksum recomputed?  Yes it
             ### does, sadly.  But it's not enough to just check for that
             ### condition, because in the case of an added file, there
             ### may not be a pre-existing checksum in the entry.
             ### Probably the best solution is to compute (or copy) the
             ### checksum at 'svn add' (or 'svn cp') time, instead of
             ### waiting until commit time.
          */
          const char *latest_base;
          svn_error_t *err;
          unsigned char local_digest[APR_MD5_DIGESTSIZE];

          latest_base = svn_wc__text_base_path(path, TRUE, pool);
          err = svn_io_file_checksum(local_digest, latest_base, pool);

          if (err && APR_STATUS_IS_ENOENT(err->apr_err))
            {
              svn_error_clear(err);
              latest_base = svn_wc__text_base_path(path, FALSE, pool);
              err = svn_io_file_checksum(local_digest, latest_base, pool);
            }

          if (! err)
            hex_digest = svn_md5_digest_to_cstring(local_digest, pool);
          else if (APR_STATUS_IS_ENOENT(err->apr_err))
            svn_error_clear(err);
          else
            return err;
        }

      /* Oh, and recursing at this point isn't really sensible. */
      if (recurse)
        *recurse = FALSE;
    }
  else
    {
      /* PATH must be a dir */
      base_name = SVN_WC_ENTRY_THIS_DIR;
    }


  /* Append a log command to set (overwrite) the 'committed-rev',
     'committed-date', 'last-author', and possibly 'checksum'
     attributes in the entry.

     Note: it's important that this log command come *before* the
     LOG_COMMITTED command, because log_do_committed() might actually
     remove the entry! */
  if (rev_date)
    {
      tmp_entry.cmt_rev = new_revnum;
      SVN_ERR(svn_time_from_cstring(&tmp_entry.cmt_date, rev_date, pool));
      modify_flags |= SVN_WC__ENTRY_MODIFY_CMT_REV
        | SVN_WC__ENTRY_MODIFY_CMT_DATE;
    }

  if (rev_author)
    {
      tmp_entry.cmt_rev = new_revnum;
      tmp_entry.cmt_author = rev_author;
      modify_flags |= SVN_WC__ENTRY_MODIFY_CMT_REV
        | SVN_WC__ENTRY_MODIFY_CMT_AUTHOR;
    }

  if (hex_digest)
    {
      tmp_entry.checksum = hex_digest;
      modify_flags |= SVN_WC__ENTRY_MODIFY_CHECKSUM;
    }

  SVN_ERR(svn_wc__loggy_entry_modify(&logtags, adm_access,
                                     path, &tmp_entry, modify_flags, pool));

  if (remove_lock)
    SVN_ERR(svn_wc__loggy_delete_lock(&logtags, adm_access, path, pool));

  if (remove_changelist)
    SVN_ERR(svn_wc__loggy_delete_changelist(&logtags, adm_access, path, pool));

  /* Regardless of whether it's a file or dir, the "main" logfile
     contains a command to bump the revision attribute (and
     timestamp). */
  SVN_ERR(svn_wc__loggy_committed(&logtags, adm_access,
                                  path, new_revnum, pool));


  /* Do wcprops in the same log txn as revision, etc. */
  if (wcprop_changes && (wcprop_changes->nelts > 0))
    {
      int i;

      for (i = 0; i < wcprop_changes->nelts; i++)
        {
          svn_prop_t *prop = APR_ARRAY_IDX(wcprop_changes, i, svn_prop_t *);

          SVN_ERR(svn_wc__loggy_modify_wcprop
                  (&logtags, adm_access,
                   path, prop->name,
                   prop->value ? prop->value->data : NULL,
                   pool));
        }
    }

  /* Write our accumulation of log entries into a log file */
  SVN_ERR(svn_wc__write_log(adm_access, log_number, logtags, pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
process_committed_internal(int *log_number,
                           const char *path,
                           svn_wc_adm_access_t *adm_access,
                           svn_boolean_t recurse,
                           svn_revnum_t new_revnum,
                           const char *rev_date,
                           const char *rev_author,
                           apr_array_header_t *wcprop_changes,
                           svn_boolean_t remove_lock,
                           svn_boolean_t remove_changelist,
                           const unsigned char *digest,
                           apr_pool_t *pool)
{
  SVN_ERR(process_committed_leaf((*log_number)++, path, adm_access, &recurse,
                                 new_revnum, rev_date, rev_author,
                                 wcprop_changes,
                                 remove_lock, remove_changelist,
                                 digest, pool));

  if (recurse)
    {
      apr_hash_t *entries;
      apr_hash_index_t *hi;
      apr_pool_t *subpool = svn_pool_create(pool);

      /* Read PATH's entries;  this is the absolute path. */
      SVN_ERR(svn_wc_entries_read(&entries, adm_access, TRUE, pool));

      /* Recursively loop over all children. */
      for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
        {
          const void *key;
          void *val;
          const char *name;
          const svn_wc_entry_t *current_entry;
          const char *this_path;
          svn_wc_adm_access_t *child_access;

          svn_pool_clear(subpool);

          apr_hash_this(hi, &key, NULL, &val);
          name = key;
          current_entry = val;

          /* Ignore the "this dir" entry. */
          if (! strcmp(name, SVN_WC_ENTRY_THIS_DIR))
            continue;

          /* Create child path by telescoping the main path. */
          this_path = svn_path_join(path, name, subpool);

          if (current_entry->kind == svn_node_dir)
            SVN_ERR(svn_wc_adm_retrieve(&child_access, adm_access, this_path,
                                        subpool));
          else
             child_access = adm_access;

          /* Recurse, but only allow further recursion if the child is
             a directory.  Pass null for wcprop_changes, because the
             ones present in the current call are only applicable to
             this one committed item. */
          if (current_entry->kind == svn_node_dir)
            SVN_ERR(svn_wc_process_committed4
                    (this_path, child_access,
                     TRUE,
                     new_revnum, rev_date, rev_author, NULL, FALSE,
                     remove_changelist, NULL, subpool));
          else
            {
              /* Suppress log creation for deleted entries in a replaced
                 directory.  By the time any log we create here is run,
                 those entries will already have been removed (as a result
                 of running the log for the replaced directory that was
                 created at the start of this function). */
              if (current_entry->schedule == svn_wc_schedule_delete)
                {
                  svn_wc_entry_t *parent_entry;

                  parent_entry = apr_hash_get(entries, SVN_WC_ENTRY_THIS_DIR,
                                              APR_HASH_KEY_STRING);
                  if (parent_entry->schedule == svn_wc_schedule_replace)
                    continue;
                }
              SVN_ERR(process_committed_leaf
                      ((*log_number)++, this_path, adm_access, NULL,
                       new_revnum, rev_date, rev_author, NULL, FALSE,
                       remove_changelist, NULL, subpool));
            }
        }

      svn_pool_destroy(subpool);
   }

  return SVN_NO_ERROR;
}


struct svn_wc_committed_queue_t
{
  apr_pool_t *pool;
  apr_array_header_t *queue;
};

typedef struct committed_queue_item_t
{
  const char *path;
  svn_wc_adm_access_t *adm_access;
  svn_boolean_t recurse;
  svn_boolean_t remove_lock;
  svn_boolean_t remove_changelist;
  apr_array_header_t *wcprop_changes;
  const unsigned char *digest;
} committed_queue_item_t;


svn_wc_committed_queue_t *
svn_wc_committed_queue_create(apr_pool_t *pool)
{
  svn_wc_committed_queue_t *q;

  q = apr_palloc(pool, sizeof(*q));
  q->pool = pool;
  q->queue = apr_array_make(pool, 1, sizeof(committed_queue_item_t *));

  return q;
}

svn_error_t *
svn_wc_queue_committed(svn_wc_committed_queue_t **queue,
                       const char *path,
                       svn_wc_adm_access_t *adm_access,
                       svn_boolean_t recurse,
                       apr_array_header_t *wcprop_changes,
                       svn_boolean_t remove_lock,
                       svn_boolean_t remove_changelist,
                       const unsigned char *digest,
                       apr_pool_t *pool)
{
  committed_queue_item_t *cqi;

  /* Use the same pool as the one *QUEUE was allocated in,
     to prevent lifetime issues.  Intermediate operations
     should use POOL. */

  /* Add to the array with paths and options */
  cqi = apr_palloc((*queue)->pool, sizeof(*cqi));
  cqi->path = path;
  cqi->adm_access = adm_access;
  cqi->recurse = recurse;
  cqi->remove_lock = remove_lock;
  cqi->remove_changelist = remove_changelist;
  cqi->wcprop_changes = wcprop_changes;
  cqi->digest = digest;

  APR_ARRAY_PUSH((*queue)->queue, committed_queue_item_t *) = cqi;

  return SVN_NO_ERROR;
}

typedef struct affected_adm_t
{
  int next_log;
  svn_wc_adm_access_t *adm_access;
} affected_adm_t;


/* Return TRUE if any item of QUEUE
   is a parent of ITEM and will be processed recursively,
   return FALSE otherwise.

   If HAVE_ANY_RECURSIVE is FALSE, exit early returning FALSE.
   Recalculate its value otherwise, changing it to FALSE
   iff no recursive items are found.
*/
static svn_boolean_t
have_recursive_parent(svn_boolean_t *have_any_recursive,
                      apr_array_header_t *queue,
                      int item,
                      apr_pool_t *pool)
{
  int i;
  svn_boolean_t found_recursive = FALSE;
  const char *path
    = APR_ARRAY_IDX(queue, item, committed_queue_item_t *)->path;

  if (! *have_any_recursive)
    return FALSE;

  for (i = 0; i < queue->nelts; i++)
    {
      committed_queue_item_t *qi
        = APR_ARRAY_IDX(queue, i, committed_queue_item_t *);

      found_recursive |= qi->recurse;

      if (i == item)
        continue;

      if (qi->recurse
          && svn_path_is_child(qi->path, path, pool))
        return TRUE;
    }

  /* Now we walked the entire array, change the cached value
     to reflect what we found. */
  *have_any_recursive = found_recursive;

  return FALSE;
}

svn_error_t *
svn_wc_process_committed_queue(svn_wc_committed_queue_t *queue,
                               svn_wc_adm_access_t *adm_access,
                               svn_revnum_t new_revnum,
                               const char *rev_date,
                               const char *rev_author,
                               apr_pool_t *pool)
{
  int i;
  apr_hash_index_t *hi;
  apr_hash_t *updated_adms = apr_hash_make(pool);
  apr_pool_t *iterpool = svn_pool_create(pool);
  svn_boolean_t have_any_recursive = TRUE;
  /* Assume we do have recursive items queued:
     we need to search for recursive parents until proven otherwise */


  /* Now, we write all log files,
     collecting the affected adms in the process ... */
  for (i = 0; i < queue->queue->nelts; i++)
    {
      affected_adm_t *affected_adm;
      const char *adm_path;
      committed_queue_item_t *cqi
        = APR_ARRAY_IDX(queue->queue,
                        i, committed_queue_item_t *);

      svn_pool_clear(iterpool);

      if (have_recursive_parent(&have_any_recursive,
                                queue->queue,
                                i, iterpool))
        continue;

      adm_path = svn_wc_adm_access_path(cqi->adm_access);
      affected_adm = apr_hash_get(updated_adms,
                                  adm_path, APR_HASH_KEY_STRING);
      if (! affected_adm)
        {
          /* allocate in pool instead of iterpool:
             we don't want this cleared at the next iteration */
          affected_adm = apr_palloc(pool, sizeof(*affected_adm));
          affected_adm->next_log = 0;
          affected_adm->adm_access = cqi->adm_access;
          apr_hash_set(updated_adms, adm_path, APR_HASH_KEY_STRING,
                       affected_adm);
        }

      SVN_ERR(process_committed_internal(&affected_adm->next_log, cqi->path,
                                         cqi->adm_access, cqi->recurse,
                                         new_revnum, rev_date, rev_author,
                                         cqi->wcprop_changes,
                                         cqi->remove_lock,
                                         cqi->remove_changelist,
                                         cqi->digest, iterpool));
    }

  /* ... and then we run them; all at once.

         This prevents writing the entries file
         more than once per adm area */
  for (hi = apr_hash_first(pool, updated_adms); hi; hi = apr_hash_next(hi))
    {
      void *val;
      affected_adm_t *this_adm;

      svn_pool_clear(iterpool);

      apr_hash_this(hi, NULL, NULL, &val);
      this_adm = val;

      SVN_ERR(svn_wc__run_log(this_adm->adm_access, NULL, iterpool));
    }

  queue->queue->nelts = 0;

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_process_committed4(const char *path,
                          svn_wc_adm_access_t *adm_access,
                          svn_boolean_t recurse,
                          svn_revnum_t new_revnum,
                          const char *rev_date,
                          const char *rev_author,
                          apr_array_header_t *wcprop_changes,
                          svn_boolean_t remove_lock,
                          svn_boolean_t remove_changelist,
                          const unsigned char *digest,
                          apr_pool_t *pool)
{
  int log_number = 0;

  SVN_ERR(process_committed_internal(&log_number,
                                     path, adm_access, recurse,
                                     new_revnum, rev_date, rev_author,
                                     wcprop_changes, remove_lock,
                                     remove_changelist, digest, pool));

  /* Run the log file(s) we just created. */
  SVN_ERR(svn_wc__run_log(adm_access, NULL, pool));

  return SVN_NO_ERROR;
}

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

/* Remove FILE if it exists and is a file.  If it does not exist, do
   nothing.  If it is not a file, error. */
static svn_error_t *
remove_file_if_present(const char *file, apr_pool_t *pool)
{
  svn_error_t *err;

  /* Try to remove the file. */
  err = svn_io_remove_file(file, pool);

  /* Ignore file not found error. */
  if (err && APR_STATUS_IS_ENOENT(err->apr_err))
    {
      svn_error_clear(err);
      err = SVN_NO_ERROR;
    }

  return err;
}


/* Recursively mark a tree ADM_ACCESS with a SCHEDULE, COPIED and/or KEEP_LOCAL
   flag, depending on the state of MODIFY_FLAGS (which may contain only a
   subset of the possible modification flags, namely, those indicating a change
   to one of the three flags mentioned above). */
static svn_error_t *
mark_tree(svn_wc_adm_access_t *adm_access,
          apr_uint64_t modify_flags,
          svn_wc_schedule_t schedule,
          svn_boolean_t copied,
          svn_boolean_t keep_local,
          svn_cancel_func_t cancel_func,
          void *cancel_baton,
          svn_wc_notify_func2_t notify_func,
          void *notify_baton,
          apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create(pool);
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  const svn_wc_entry_t *entry;
  svn_wc_entry_t tmp_entry;
  apr_uint64_t this_dir_flags;

  /* Read the entries file for this directory. */
  SVN_ERR(svn_wc_entries_read(&entries, adm_access, FALSE, pool));

  /* Mark each entry in the entries file. */
  for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
    {
      const char *fullpath;
      const void *key;
      void *val;
      const char *base_name;

      /* Clear our per-iteration pool. */
      svn_pool_clear(subpool);

      /* Get the next entry */
      apr_hash_this(hi, &key, NULL, &val);
      entry = val;

      /* Skip "this dir".  */
      if (! strcmp((const char *)key, SVN_WC_ENTRY_THIS_DIR))
        continue;

      base_name = key;
      fullpath = svn_path_join(svn_wc_adm_access_path(adm_access), base_name,
                               subpool);

      /* If this is a directory, recurse. */
      if (entry->kind == svn_node_dir)
        {
          svn_wc_adm_access_t *child_access;
          SVN_ERR(svn_wc_adm_retrieve(&child_access, adm_access, fullpath,
                                      subpool));
          SVN_ERR(mark_tree(child_access, modify_flags,
                            schedule, copied, keep_local,
                            cancel_func, cancel_baton,
                            notify_func, notify_baton,
                            subpool));
        }

      tmp_entry.schedule = schedule;
      tmp_entry.copied = copied;
      SVN_ERR(svn_wc__entry_modify
              (adm_access, base_name, &tmp_entry,
               modify_flags & (SVN_WC__ENTRY_MODIFY_SCHEDULE
                               | SVN_WC__ENTRY_MODIFY_COPIED),
               TRUE, subpool));

      if (copied)
        /* Remove now obsolete wcprops */
        SVN_ERR(svn_wc__props_delete(fullpath, svn_wc__props_wcprop,
                                     adm_access, subpool));

      /* Tell someone what we've done. */
      if (schedule == svn_wc_schedule_delete && notify_func != NULL)
        (*notify_func)(notify_baton,
                       svn_wc_create_notify(fullpath, svn_wc_notify_delete,
                                            subpool), pool);
    }

  /* Handle "this dir" for states that need it done post-recursion. */
  entry = apr_hash_get(entries, SVN_WC_ENTRY_THIS_DIR, APR_HASH_KEY_STRING);
  this_dir_flags = 0;

  /* Uncommitted directories (schedule add) that are to be scheduled for
     deletion are a special case, they don't need to be changed as they
     will be removed from their parent's entry list. */
  if (! (entry->schedule == svn_wc_schedule_add
         && schedule == svn_wc_schedule_delete))
  {
    if (modify_flags & SVN_WC__ENTRY_MODIFY_SCHEDULE)
      {
        tmp_entry.schedule = schedule;
        this_dir_flags |= SVN_WC__ENTRY_MODIFY_SCHEDULE;
      }

    if (modify_flags & SVN_WC__ENTRY_MODIFY_COPIED)
      {
        tmp_entry.copied = copied;
        this_dir_flags |= SVN_WC__ENTRY_MODIFY_COPIED;
      }
  }

  /* Set keep_local on the "this dir", if requested. */
  if (modify_flags & SVN_WC__ENTRY_MODIFY_KEEP_LOCAL)
    {
      tmp_entry.keep_local = keep_local;
      this_dir_flags |= SVN_WC__ENTRY_MODIFY_KEEP_LOCAL;
    }

  /* Modify this_dir entry if requested. */
  if (this_dir_flags)
    SVN_ERR(svn_wc__entry_modify(adm_access, NULL, &tmp_entry, this_dir_flags,
                                 TRUE, subpool));

  /* Destroy our per-iteration pool. */
  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* Remove/erase PATH from the working copy. This involves deleting PATH
 * from the physical filesystem. PATH is assumed to be an unversioned file
 * or directory.
 *
 * If CANCEL_FUNC is non-null, invoke it with CANCEL_BATON at various
 * points, return any error immediately.
 */
static svn_error_t *
erase_unversioned_from_wc(const char *path,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          apr_pool_t *pool)
{
  svn_error_t *err;

  /* Optimize the common case: try to delete the file */
  err = svn_io_remove_file(path, pool);
  if (err)
    {
      /* Then maybe it was a directory? */
      svn_error_clear(err);

      err = svn_io_remove_dir2(path, FALSE, cancel_func, cancel_baton, pool);

      if (err)
        {
          /* We're unlikely to end up here. But we need this fallback
             to make sure we report the right error *and* try the
             correct deletion at least once. */
          svn_node_kind_t kind;

          svn_error_clear(err);
          SVN_ERR(svn_io_check_path(path, &kind, pool));
          if (kind == svn_node_file)
            SVN_ERR(svn_io_remove_file(path, pool));
          else if (kind == svn_node_dir)
            SVN_ERR(svn_io_remove_dir2(path, FALSE,
                                       cancel_func, cancel_baton, pool));
          else if (kind == svn_node_none)
            return svn_error_createf(SVN_ERR_BAD_FILENAME, NULL,
                                     _("'%s' does not exist"),
                                     svn_path_local_style(path, pool));
          else
            return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                     _("Unsupported node kind for path '%s'"),
                                     svn_path_local_style(path, pool));

        }
    }

  return SVN_NO_ERROR;
}

/* Remove/erase PATH from the working copy. For files this involves
 * deletion from the physical filesystem.  For directories it involves the
 * deletion from the filesystem of all unversioned children, and all
 * versioned children that are files. By the time we get here, added but
 * not committed items will have been scheduled for deletion which means
 * they have become unversioned.
 *
 * The result is that all that remains are versioned directories, each with
 * its .svn directory and .svn contents.
 *
 * If CANCEL_FUNC is non-null, invoke it with CANCEL_BATON at various
 * points, return any error immediately.
 *
 * KIND is the node kind appropriate for PATH
 */
static svn_error_t *
erase_from_wc(const char *path,
              svn_wc_adm_access_t *adm_access,
              svn_node_kind_t kind,
              svn_cancel_func_t cancel_func,
              void *cancel_baton,
              apr_pool_t *pool)
{
  const svn_wc_entry_t *entry;

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  if (kind == svn_node_file)
    SVN_ERR(remove_file_if_present(path, pool));

  else if (kind == svn_node_dir)
    /* This must be a directory or absent */
    {
      apr_hash_t *ver, *unver;
      apr_hash_index_t *hi;
      svn_wc_adm_access_t *dir_access;
      svn_error_t *err;

      /* ### Suspect that an iteration or recursion subpool would be
         good here. */

      /* First handle the versioned items, this is better (probably) than
         simply using svn_io_get_dirents2 for everything as it avoids the
         need to do svn_io_check_path on each versioned item */
      err = svn_wc_adm_retrieve(&dir_access, adm_access, path, pool);

      /* If there's no on-disk item, be sure to exit early and
         not to return an error */
      if (err)
        {
          svn_node_kind_t wc_kind;
          svn_error_t *err2 = svn_io_check_path(path, &wc_kind, pool);

          if (err2)
            {
              svn_error_clear(err);
              return err2;
            }

          if (wc_kind != svn_node_none)
            return err;

          svn_error_clear(err);
          return SVN_NO_ERROR;
        }
      SVN_ERR(svn_wc_entries_read(&ver, dir_access, FALSE, pool));
      for (hi = apr_hash_first(pool, ver); hi; hi = apr_hash_next(hi))
        {
          const void *key;
          void *val;
          const char *name;
          const char *down_path;

          apr_hash_this(hi, &key, NULL, &val);
          name = key;
          entry = val;

          if (!strcmp(name, SVN_WC_ENTRY_THIS_DIR))
            continue;

          down_path = svn_path_join(path, name, pool);
          SVN_ERR(erase_from_wc(down_path, adm_access, entry->kind,
                                cancel_func, cancel_baton, pool));
        }

      /* Now handle any remaining unversioned items */
      SVN_ERR(svn_io_get_dirents2(&unver, path, pool));
      for (hi = apr_hash_first(pool, unver); hi; hi = apr_hash_next(hi))
        {
          const void *key;
          const char *name;
          const char *down_path;

          apr_hash_this(hi, &key, NULL, NULL);
          name = key;

          /* The admin directory will show up, we don't want to delete it */
          if (svn_wc_is_adm_dir(name, pool))
            continue;

          /* Versioned directories will show up, don't delete those either */
          if (apr_hash_get(ver, name, APR_HASH_KEY_STRING))
            continue;

          down_path = svn_path_join(path, name, pool);
          SVN_ERR(erase_unversioned_from_wc
                  (down_path, cancel_func, cancel_baton, pool));
        }
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_delete3(const char *path,
               svn_wc_adm_access_t *adm_access,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               svn_wc_notify_func2_t notify_func,
               void *notify_baton,
               svn_boolean_t keep_local,
               apr_pool_t *pool)
{
  svn_wc_adm_access_t *dir_access;
  const svn_wc_entry_t *entry;
  const char *parent, *base_name;
  svn_boolean_t was_schedule;
  svn_node_kind_t was_kind;
  svn_boolean_t was_copied;
  svn_boolean_t was_deleted = FALSE; /* Silence a gcc uninitialized warning */

  SVN_ERR(svn_wc_adm_probe_try3(&dir_access, adm_access, path,
                                TRUE, -1, cancel_func, cancel_baton, pool));
  if (dir_access)
    SVN_ERR(svn_wc_entry(&entry, path, dir_access, FALSE, pool));
  else
    entry = NULL;

  if (!entry)
    return erase_unversioned_from_wc(path, cancel_func, cancel_baton, pool);

  /* Note: Entries caching?  What happens to this entry when the entries
     file is updated?  Lets play safe and copy the values */
  was_schedule = entry->schedule;
  was_kind = entry->kind;
  was_copied = entry->copied;

  svn_path_split(path, &parent, &base_name, pool);

  if (was_kind == svn_node_dir)
    {
      svn_wc_adm_access_t *parent_access;
      apr_hash_t *entries;
      const svn_wc_entry_t *entry_in_parent;

      /* The deleted state is only available in the entry in parent's
         entries file */
      SVN_ERR(svn_wc_adm_retrieve(&parent_access, adm_access, parent, pool));
      SVN_ERR(svn_wc_entries_read(&entries, parent_access, TRUE, pool));
      entry_in_parent = apr_hash_get(entries, base_name, APR_HASH_KEY_STRING);
      was_deleted = entry_in_parent ? entry_in_parent->deleted : FALSE;

      if (was_schedule == svn_wc_schedule_add && !was_deleted)
        {
          /* Deleting a directory that has been added but not yet
             committed is easy, just remove the administrative dir. */

          if (dir_access != adm_access)
            {
              SVN_ERR(svn_wc_remove_from_revision_control
                      (dir_access, SVN_WC_ENTRY_THIS_DIR, FALSE, FALSE,
                       cancel_func, cancel_baton, pool));
            }
          else
            {
              /* adm_probe_retrieve returned the parent access baton,
                 which is the same access baton that we came in here
                 with!  this means we're dealing with a missing item
                 that's scheduled for addition.  Easiest to just
                 remove the entry.  */
              svn_wc__entry_remove(entries, base_name);
              SVN_ERR(svn_wc__entries_write(entries, parent_access, pool));
            }
        }
      else
        {
          /* if adm_probe_retrieve returned the parent access baton,
             (which is the same access baton that we came in here
             with), this means we're dealing with a missing directory.
             So there's no tree to mark for deletion.  Instead, the
             next phase of code will simply schedule the directory for
             deletion in its parent. */
          if (dir_access != adm_access)
            {
              /* Recursively mark a whole tree for deletion. */
              SVN_ERR(mark_tree(dir_access,
                                SVN_WC__ENTRY_MODIFY_SCHEDULE
                                | SVN_WC__ENTRY_MODIFY_KEEP_LOCAL,
                                svn_wc_schedule_delete, FALSE, keep_local,
                                cancel_func, cancel_baton,
                                notify_func, notify_baton,
                                pool));
            }
        }
    }

  if (!(was_kind == svn_node_dir && was_schedule == svn_wc_schedule_add
        && !was_deleted))
    {
      /* We need to mark this entry for deletion in its parent's entries
         file, so we split off base_name from the parent path, then fold in
         the addition of a delete flag. */
      svn_stringbuf_t *log_accum = svn_stringbuf_create("", pool);
      svn_wc_entry_t tmp_entry;

      /* Edit the entry to reflect the now deleted state.
         entries.c:fold_entry() clears the values of copied, copyfrom_rev
         and copyfrom_url. */
      tmp_entry.schedule = svn_wc_schedule_delete;
      SVN_ERR(svn_wc__loggy_entry_modify(&log_accum, adm_access,
                                         path, &tmp_entry,
                                         SVN_WC__ENTRY_MODIFY_SCHEDULE,
                                         pool));

      /* is it a replacement with history? */
      if (was_schedule == svn_wc_schedule_replace && was_copied)
        {
          const char *text_base =
            svn_wc__text_base_path(path, FALSE, pool);
          const char *text_revert =
            svn_wc__text_revert_path(path, FALSE, pool);

          if (was_kind != svn_node_dir) /* Dirs don't have text-bases */
            /* Restore the original text-base */
            SVN_ERR(svn_wc__loggy_move(&log_accum, NULL, adm_access,
                                       text_revert, text_base,
                                       FALSE, pool));

          SVN_ERR(svn_wc__loggy_revert_props_restore(&log_accum,
                                                     path, adm_access, pool));
        }
      if (was_schedule == svn_wc_schedule_add)
        SVN_ERR(svn_wc__loggy_props_delete(&log_accum, path,
                                           svn_wc__props_base,
                                           adm_access, pool));

      SVN_ERR(svn_wc__write_log(adm_access, 0, log_accum, pool));

      SVN_ERR(svn_wc__run_log(adm_access, NULL, pool));

    }

  /* Report the deletion to the caller. */
  if (notify_func != NULL)
    (*notify_func)(notify_baton,
                   svn_wc_create_notify(path, svn_wc_notify_delete,
                                        pool), pool);

  /* By the time we get here, anything that was scheduled to be added has
     become unversioned */
  if (!keep_local)
    {
      if (was_schedule == svn_wc_schedule_add)
        SVN_ERR(erase_unversioned_from_wc
                (path, cancel_func, cancel_baton, pool));
      else
        SVN_ERR(erase_from_wc(path, adm_access, was_kind,
                              cancel_func, cancel_baton, pool));
    }

  return SVN_NO_ERROR;
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
svn_wc_get_ancestry(char **url,
                    svn_revnum_t *rev,
                    const char *path,
                    svn_wc_adm_access_t *adm_access,
                    apr_pool_t *pool)
{
  const svn_wc_entry_t *ent;

  SVN_ERR(svn_wc__entry_versioned(&ent, path, adm_access, FALSE, pool));

  if (url)
    *url = apr_pstrdup(pool, ent->url);

  if (rev)
    *rev = ent->revision;

  return SVN_NO_ERROR;
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
  const char *parent_dir, *base_name;
  const svn_wc_entry_t *orig_entry, *parent_entry;
  svn_wc_entry_t tmp_entry;
  svn_boolean_t is_replace = FALSE;
  svn_node_kind_t kind;
  apr_uint64_t modify_flags = 0;
  svn_wc_adm_access_t *adm_access;

  SVN_ERR(svn_path_check_valid(path, pool));

  /* Make sure something's there. */
  SVN_ERR(svn_io_check_path(path, &kind, pool));
  if (kind == svn_node_none)
    return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                             _("'%s' not found"),
                             svn_path_local_style(path, pool));
  if (kind == svn_node_unknown)
    return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                             _("Unsupported node kind for path '%s'"),
                             svn_path_local_style(path, pool));

  /* Get the original entry for this path if one exists (perhaps
     this is actually a replacement of a previously deleted thing).

     Note that this is one of the few functions that is allowed to see
     'deleted' entries;  it's totally fine to have an entry that is
     scheduled for addition and still previously 'deleted'.  */
  SVN_ERR(svn_wc_adm_probe_try3(&adm_access, parent_access, path,
                                TRUE, copyfrom_url != NULL ? -1 : 0,
                                cancel_func, cancel_baton, pool));
  if (adm_access)
    SVN_ERR(svn_wc_entry(&orig_entry, path, adm_access, TRUE, pool));
  else
    orig_entry = NULL;

  /* You can only add something that is not in revision control, or
     that is slated for deletion from revision control, or has been
     previously 'deleted', unless, of course, you're specifying an
     addition with -history-; then it's okay for the object to be
     under version control already; it's not really new.  */
  if (orig_entry)
    {
      if ((! copyfrom_url)
          && (orig_entry->schedule != svn_wc_schedule_delete)
          && (! orig_entry->deleted))
        {
          return svn_error_createf
            (SVN_ERR_ENTRY_EXISTS, NULL,
             _("'%s' is already under version control"),
             svn_path_local_style(path, pool));
        }
      else if (orig_entry->kind != kind)
        {
          /* ### todo: At some point, we obviously don't want to block
             replacements where the node kind changes.  When this
             happens, svn_wc_revert3() needs to learn how to revert
             this situation.  At present we are using a specific node-change
             error so that clients can detect it. */
          return svn_error_createf
            (SVN_ERR_WC_NODE_KIND_CHANGE, NULL,
             _("Can't replace '%s' with a node of a differing type; "
               "the deletion must be committed and the parent updated "
               "before adding '%s'"),
             svn_path_local_style(path, pool),
             svn_path_local_style(path, pool));
        }
      if (orig_entry->schedule == svn_wc_schedule_delete)
        is_replace = TRUE;
    }

  /* Split off the base_name from the parent directory. */
  svn_path_split(path, &parent_dir, &base_name, pool);
  SVN_ERR(svn_wc_entry(&parent_entry, parent_dir, parent_access, FALSE,
                       pool));
  if (! parent_entry)
    return svn_error_createf
      (SVN_ERR_ENTRY_NOT_FOUND, NULL,
       _("Can't find parent directory's entry while trying to add '%s'"),
       svn_path_local_style(path, pool));
  if (parent_entry->schedule == svn_wc_schedule_delete)
    return svn_error_createf
      (SVN_ERR_WC_SCHEDULE_CONFLICT, NULL,
       _("Can't add '%s' to a parent directory scheduled for deletion"),
       svn_path_local_style(path, pool));

  /* Init the modify flags. */
  modify_flags = SVN_WC__ENTRY_MODIFY_SCHEDULE | SVN_WC__ENTRY_MODIFY_KIND;
  if (! (is_replace || copyfrom_url))
    modify_flags |= SVN_WC__ENTRY_MODIFY_REVISION;

  /* If a copy ancestor was given, make sure the copyfrom URL is in the same
     repository (if possible) and put the proper ancestry info in the new
     entry */
  if (copyfrom_url)
    {
      if (parent_entry->repos
          && ! svn_path_is_ancestor(parent_entry->repos, copyfrom_url))
        return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                 _("The URL '%s' has a different repository "
                                   "root than its parent"), copyfrom_url);
      tmp_entry.copyfrom_url = copyfrom_url;
      tmp_entry.copyfrom_rev = copyfrom_rev;
      tmp_entry.copied = TRUE;
      modify_flags |= SVN_WC__ENTRY_MODIFY_COPYFROM_URL;
      modify_flags |= SVN_WC__ENTRY_MODIFY_COPYFROM_REV;
      modify_flags |= SVN_WC__ENTRY_MODIFY_COPIED;
    }

  /* If this is a replacement we want to remove the checksum and the property
     flags so they are not set to their respective old values. */
  if (is_replace)
    {
      tmp_entry.checksum = NULL;
      modify_flags |= SVN_WC__ENTRY_MODIFY_CHECKSUM;

      tmp_entry.has_props = FALSE;
      tmp_entry.has_prop_mods = FALSE;
      modify_flags |= SVN_WC__ENTRY_MODIFY_HAS_PROPS;
      modify_flags |= SVN_WC__ENTRY_MODIFY_HAS_PROP_MODS;
    }

  tmp_entry.revision = 0;
  tmp_entry.kind = kind;
  tmp_entry.schedule = svn_wc_schedule_add;

  /* Now, add the entry for this item to the parent_dir's
     entries file, marking it for addition. */
  SVN_ERR(svn_wc__entry_modify(parent_access, base_name, &tmp_entry,
                               modify_flags, TRUE, pool));


  /* If this is a replacement without history, we need to reset the
     properties for PATH. */
  if (orig_entry && (! copyfrom_url))
    SVN_ERR(svn_wc__props_delete(path, svn_wc__props_working,
                                 adm_access, pool));

  if (kind == svn_node_dir) /* scheduling a directory for addition */
    {
      /* Note that both calls to svn_wc_ensure_adm3() below pass
         svn_depth_infinity.  Even if 'svn add' were invoked with some
         other depth, we'd want to create the adm area with
         svn_depth_infinity, because when the user passes add a depth,
         that's just a way of telling Subversion what items to add,
         not a way of telling Subversion what depth the resultant
         newly-versioned directory should have. */

      if (! copyfrom_url)
        {
          const svn_wc_entry_t *p_entry; /* ### why not use parent_entry? */
          const char *new_url;

          /* Get the entry for this directory's parent.  We need to snatch
             the ancestor path out of there. */
          SVN_ERR(svn_wc_entry(&p_entry, parent_dir, parent_access, FALSE,
                               pool));

          /* Derive the parent path for our new addition here. */
          new_url = svn_path_url_add_component(p_entry->url, base_name, pool);

          /* Make sure this new directory has an admistrative subdirectory
             created inside of it */
          SVN_ERR(svn_wc_ensure_adm3(path, NULL, new_url, p_entry->repos,
                                     0, svn_depth_infinity, pool));
        }
      else
        {
          /* When we are called with the copyfrom arguments set and with
             the admin directory already in existence, then the dir will
             contain the copyfrom settings.  So we need to pass the
             copyfrom arguments to the ensure call. */
          SVN_ERR(svn_wc_ensure_adm3(path, NULL, copyfrom_url,
                                     parent_entry->repos, copyfrom_rev,
                                     svn_depth_infinity, pool));
        }

      /* We want the locks to persist, so use the access baton's pool */
      if (! orig_entry || orig_entry->deleted)
        {
          apr_pool_t* access_pool = svn_wc_adm_access_pool(parent_access);
          SVN_ERR(svn_wc_adm_open3(&adm_access, parent_access, path,
                                   TRUE, copyfrom_url != NULL ? -1 : 0,
                                   cancel_func, cancel_baton,
                                   access_pool));
        }

      /* We're making the same mods we made above, but this time we'll
         force the scheduling.  Also make sure to undo the
         'incomplete' flag which svn_wc_ensure_adm3 sets by default. */
      modify_flags |= SVN_WC__ENTRY_MODIFY_FORCE;
      modify_flags |= SVN_WC__ENTRY_MODIFY_INCOMPLETE;
      tmp_entry.schedule = is_replace
                           ? svn_wc_schedule_replace
                           : svn_wc_schedule_add;
      tmp_entry.incomplete = FALSE;
      SVN_ERR(svn_wc__entry_modify(adm_access, NULL, &tmp_entry,
                                   modify_flags, TRUE, pool));

      if (copyfrom_url)
        {
          /* If this new directory has ancestry, it's not enough to
             schedule it for addition with copyfrom args.  We also
             need to rewrite its ancestor-url, and rewrite the
             ancestor-url of ALL its children!

             We're doing this because our current commit model (for
             hysterical raisins, presumably) assumes an entry's URL is
             correct before commit -- i.e. the URL is not tweaked in
             the post-commit bumping process.  We might want to change
             this model someday. */

          /* Figure out what the new url should be. */
          const char *new_url =
            svn_path_url_add_component(parent_entry->url, base_name, pool);

          /* Change the entry urls recursively (but not the working rev). */
          SVN_ERR(svn_wc__do_update_cleanup(path, adm_access,
                                            svn_depth_infinity, new_url,
                                            parent_entry->repos,
                                            SVN_INVALID_REVNUM, NULL,
                                            NULL, FALSE, apr_hash_make(pool),
                                            pool));

          /* Recursively add the 'copied' existence flag as well!  */
          SVN_ERR(mark_tree(adm_access, SVN_WC__ENTRY_MODIFY_COPIED,
                            svn_wc_schedule_normal, TRUE, FALSE,
                            cancel_func,
                            cancel_baton,
                            NULL, NULL, /* N/A cuz we aren't deleting */
                            pool));

          /* Clean out the now-obsolete wcprops. */
          SVN_ERR(svn_wc__props_delete(path, svn_wc__props_wcprop,
                                       adm_access, pool));
        }
    }

  /* Report the addition to the caller. */
  if (notify_func != NULL)
    {
      svn_wc_notify_t *notify = svn_wc_create_notify(path, svn_wc_notify_add,
                                                     pool);
      notify->kind = kind;
      (*notify_func)(notify_baton, notify, pool);
    }

  return SVN_NO_ERROR;
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
/* Thoughts on Reversion.

    What does is mean to revert a given PATH in a tree?  We'll
    consider things by their modifications.

    Adds

    - For files, svn_wc_remove_from_revision_control(), baby.

    - Added directories may contain nothing but added children, and
      reverting the addition of a directory necessarily means reverting
      the addition of all the directory's children.  Again,
      svn_wc_remove_from_revision_control() should do the trick.

    Deletes

    - Restore properties to their unmodified state.

    - For files, restore the pristine contents, and reset the schedule
      to 'normal'.

    - For directories, reset the schedule to 'normal'.  All children
      of a directory marked for deletion must also be marked for
      deletion, but it's okay for those children to remain deleted even
      if their parent directory is restored.  That's what the
      recursive flag is for.

    Replaces

    - Restore properties to their unmodified state.

    - For files, restore the pristine contents, and reset the schedule
      to 'normal'.

    - For directories, reset the schedule to normal.  A replaced
      directory can have deleted children (left over from the initial
      deletion), replaced children (children of the initial deletion
      now re-added), and added children (new entries under the
      replaced directory).  Since this is technically an addition, it
      necessitates recursion.

    Modifications

    - Restore properties and, for files, contents to their unmodified
      state.

*/


/* Revert ENTRY for NAME in directory represented by ADM_ACCESS. Sets
   *REVERTED to TRUE if something actually is reverted.

   Use SVN_WC_ENTRY_THIS_DIR as NAME for reverting ADM_ACCESS directory
   itself.

   Use POOL for any temporary allocations.*/
static svn_error_t *
revert_admin_things(svn_wc_adm_access_t *adm_access,
                    const char *name,
                    const svn_wc_entry_t *entry,
                    svn_boolean_t *reverted,
                    svn_boolean_t use_commit_times,
                    apr_pool_t *pool)
{
  const char *fullpath;
  /* If true, force reinstallation of working file. */
  svn_boolean_t reinstall_working = FALSE;
  svn_wc_entry_t tmp_entry;
  apr_uint64_t flags = 0;
  svn_stringbuf_t *log_accum = svn_stringbuf_create("", pool);
  apr_hash_t *baseprops = NULL;
  svn_boolean_t revert_base = FALSE;

  /* Build the full path of the thing we're reverting. */
  fullpath = svn_wc_adm_access_path(adm_access);
  if (strcmp(name, SVN_WC_ENTRY_THIS_DIR) != 0)
    fullpath = svn_path_join(fullpath, name, pool);

  /* Deal with properties. */
  if (entry->schedule == svn_wc_schedule_replace)
    {
       revert_base = entry->copied;
      /* Use the revertpath as the new propsbase if it exists. */

      baseprops = apr_hash_make(pool);
      SVN_ERR(svn_wc__load_props((! revert_base) ? &baseprops : NULL,
                                 NULL,
                                 revert_base ? &baseprops : NULL,
                                 adm_access, fullpath, pool));

      /* Ensure the revert propfile gets removed. */
      if (revert_base)
        SVN_ERR(svn_wc__loggy_props_delete(&log_accum,
                                           fullpath, svn_wc__props_revert,
                                           adm_access, pool));
      *reverted = TRUE;
    }

  /* If not schedule replace, or no revert props, use the normal
     base-props and working props. */
  if (! baseprops)
    {
      svn_boolean_t modified;

      /* Check for prop changes. */
      SVN_ERR(svn_wc_props_modified_p(&modified, fullpath, adm_access,
                                      pool));
      if (modified)
        {
          apr_array_header_t *propchanges;

          /* Get the full list of property changes and see if any magic
             properties were changed. */
          SVN_ERR(svn_wc_get_prop_diffs(&propchanges, &baseprops, fullpath,
                                        adm_access, pool));

          /* Determine if any of the propchanges are the "magic" ones that
             might require changing the working file. */
          reinstall_working = svn_wc__has_magic_property(propchanges);

        }
    }

  /* Reinstall props if we need to.  Only rewrite the baseprops,
     if we're reverting a replacement.  This is just an optimization. */
  if (baseprops)
    {
      SVN_ERR(svn_wc__install_props(&log_accum, adm_access, fullpath, baseprops,
                                    baseprops,
                                    revert_base,
                                    pool));
      *reverted = TRUE;
    }

  /* Deal with the contents. */

  if (entry->kind == svn_node_file)
    {
      svn_node_kind_t base_kind;
      const char *base_thing;
      svn_boolean_t tgt_modified;

      /* If the working file is missing, we need to reinstall it. */
      if (! reinstall_working)
        {
          svn_node_kind_t kind;
          SVN_ERR(svn_io_check_path(fullpath, &kind, pool));
          if (kind == svn_node_none)
            reinstall_working = TRUE;
        }

      base_thing = svn_wc__text_base_path(fullpath, FALSE, pool);

      /* Check for text base presence. */
      SVN_ERR(svn_io_check_path(base_thing, &base_kind, pool));

      if (base_kind != svn_node_file)
        return svn_error_createf(APR_ENOENT, NULL,
                                 _("Error restoring text for '%s'"),
                                 svn_path_local_style(fullpath, pool));

      /* Look for a revert base file.  If it exists use it for
       * the text base for the file.  If it doesn't use the normal
       * text base. */
      SVN_ERR(svn_wc__loggy_move
              (&log_accum, &tgt_modified, adm_access,
               svn_wc__text_revert_path(fullpath, FALSE, pool), base_thing,
               FALSE, pool));
      reinstall_working = reinstall_working || tgt_modified;

      /* A shortcut: since we will translate when reinstall_working, we
         don't need to check if the working file is modified
      */
      if (! reinstall_working)
        SVN_ERR(svn_wc__text_modified_internal_p(&reinstall_working,
                                                 fullpath, FALSE, adm_access,
                                                 FALSE, pool));

      if (reinstall_working)
        {
          /* If there are textual mods (or if the working file is
             missing altogether), copy the text-base out into
             the working copy, and update the timestamp in the entries
             file. */
          SVN_ERR(svn_wc__loggy_copy(&log_accum, NULL, adm_access,
                                     svn_wc__copy_translate,
                                     base_thing, fullpath, FALSE, pool));

          /* Possibly set the timestamp to last-commit-time, rather
             than the 'now' time that already exists. */
          if (use_commit_times && entry->cmt_date)
            SVN_ERR(svn_wc__loggy_set_timestamp
                    (&log_accum, adm_access, fullpath,
                     svn_time_to_cstring(entry->cmt_date, pool),
                     pool));

          SVN_ERR(svn_wc__loggy_set_entry_timestamp_from_wc
                  (&log_accum, adm_access,
                   fullpath, SVN_WC__ENTRY_ATTR_TEXT_TIME, pool));
          SVN_ERR(svn_wc__loggy_set_entry_working_size_from_wc
                  (&log_accum, adm_access, fullpath, pool));

          *reverted = TRUE;
        }
    }

  /* Remove conflict state (and conflict files), if any.
     Handle the three possible text conflict files. */
  if (entry->conflict_old)
    {
      flags |= SVN_WC__ENTRY_MODIFY_CONFLICT_OLD;
      tmp_entry.conflict_old = NULL;
      SVN_ERR(svn_wc__loggy_remove
              (&log_accum, adm_access,
               svn_path_join(svn_wc_adm_access_path(adm_access),
                             entry->conflict_old, pool), pool));
    }

  if (entry->conflict_new)
    {
      flags |= SVN_WC__ENTRY_MODIFY_CONFLICT_NEW;
      tmp_entry.conflict_new = NULL;
      SVN_ERR(svn_wc__loggy_remove
              (&log_accum, adm_access,
               svn_path_join(svn_wc_adm_access_path(adm_access),
                             entry->conflict_new, pool), pool));
    }

  if (entry->conflict_wrk)
    {
      flags |= SVN_WC__ENTRY_MODIFY_CONFLICT_WRK;
      tmp_entry.conflict_wrk = NULL;
      SVN_ERR(svn_wc__loggy_remove
              (&log_accum, adm_access,
               svn_path_join(svn_wc_adm_access_path(adm_access),
                             entry->conflict_wrk, pool), pool));
    }

  /* Remove the prej-file if the entry lists one (and it exists) */
  if (entry->prejfile)
    {
      flags |= SVN_WC__ENTRY_MODIFY_PREJFILE;
      tmp_entry.prejfile = NULL;
      SVN_ERR(svn_wc__loggy_remove
              (&log_accum, adm_access,
               svn_path_join(svn_wc_adm_access_path(adm_access),
                             entry->prejfile, pool), pool));
    }

  /* Clean up the copied state if this is a replacement. */
  if (entry->schedule == svn_wc_schedule_replace)
    {
      flags |= SVN_WC__ENTRY_MODIFY_COPIED |
          SVN_WC__ENTRY_MODIFY_COPYFROM_URL |
          SVN_WC__ENTRY_MODIFY_COPYFROM_REV;
      tmp_entry.copied = FALSE;

      /* Reset the checksum if this is a replace-with-history. */
      if (entry->kind == svn_node_file && entry->copyfrom_url)
        {
          const char *base_path;
          unsigned char digest[APR_MD5_DIGESTSIZE];

          base_path = svn_wc__text_revert_path(fullpath, FALSE, pool);
          SVN_ERR(svn_io_file_checksum(digest, base_path, pool));
          tmp_entry.checksum = svn_md5_digest_to_cstring(digest, pool);
          flags |= SVN_WC__ENTRY_MODIFY_CHECKSUM;
        }

      /* Set this to the empty string, because NULL values will disappear
         in the XML log file. */
      tmp_entry.copyfrom_url = "";
      tmp_entry.copyfrom_rev = SVN_INVALID_REVNUM;
    }

  /* Reset schedule attribute to svn_wc_schedule_normal. */
  if (entry->schedule != svn_wc_schedule_normal)
    {
      flags |= SVN_WC__ENTRY_MODIFY_SCHEDULE;
      tmp_entry.schedule = svn_wc_schedule_normal;
      *reverted = TRUE;
    }

  SVN_ERR(svn_wc__loggy_entry_modify(&log_accum, adm_access, fullpath,
                                     &tmp_entry, flags, pool));

  /* Don't run log if nothing to change. */
  if (!svn_stringbuf_isempty(log_accum))
    {
      SVN_ERR(svn_wc__write_log(adm_access, 0, log_accum, pool));
      SVN_ERR(svn_wc__run_log(adm_access, NULL, pool));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_revert3(const char *path,
               svn_wc_adm_access_t *parent_access,
               svn_depth_t depth,
               svn_boolean_t use_commit_times,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               svn_wc_notify_func2_t notify_func,
               void *notify_baton,
               apr_pool_t *pool)
{
  svn_node_kind_t kind;
  const char *p_dir = NULL, *bname = NULL;
  const svn_wc_entry_t *entry;
  svn_boolean_t wc_root = FALSE, reverted = FALSE;
  svn_wc_adm_access_t *dir_access;

  /* Check cancellation here, so recursive calls get checked early. */
  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  SVN_ERR(svn_wc_adm_probe_retrieve(&dir_access, parent_access, path, pool));

  /* Safeguard 1:  is this a versioned resource? */
  SVN_ERR_W(svn_wc__entry_versioned(&entry, path, dir_access, FALSE, pool),
            _("Cannot revert"));

  /* Safeguard 1.5: is this a missing versioned directory? */
  if (entry->kind == svn_node_dir)
    {
      svn_node_kind_t disk_kind;
      SVN_ERR(svn_io_check_path(path, &disk_kind, pool));
      if ((disk_kind != svn_node_dir)
          && (entry->schedule != svn_wc_schedule_add))
        {
          /* When the directory itself is missing, we can't revert
             without hitting the network.  Someday a '--force' option
             will make this happen.  For now, send notification of the
             failure. */
          if (notify_func != NULL)
            (*notify_func)
              (notify_baton,
               svn_wc_create_notify(path, svn_wc_notify_failed_revert,
                                    pool), pool);
          return SVN_NO_ERROR;
        }
    }

  /* Safeguard 2:  can we handle this node kind? */
  if ((entry->kind != svn_node_file) && (entry->kind != svn_node_dir))
    return svn_error_createf
      (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
       _("Cannot revert '%s': unsupported entry node kind"),
       svn_path_local_style(path, pool));

  /* Safeguard 3:  can we deal with the node kind of PATH current in
     the working copy? */
  SVN_ERR(svn_io_check_path(path, &kind, pool));
  if ((kind != svn_node_none)
      && (kind != svn_node_file)
      && (kind != svn_node_dir))
    return svn_error_createf
      (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
       _("Cannot revert '%s': unsupported node kind in working copy"),
       svn_path_local_style(path, pool));

  /* For directories, determine if PATH is a WC root so that we can
     tell if it is safe to split PATH into a parent directory and
     basename.  For files, we always do this split.  */
  if (kind == svn_node_dir)
    SVN_ERR(svn_wc_is_wc_root(&wc_root, path, dir_access, pool));
  if (! wc_root)
    {
      /* Split the base_name from the parent path. */
      svn_path_split(path, &p_dir, &bname, pool);
    }


  /* Additions. */
  if (entry->schedule == svn_wc_schedule_add)
    {
      /* Before removing item from revision control, notice if the
         entry is in a 'deleted' state; this is critical for
         directories, where this state only exists in its parent's
         entry. */
      svn_boolean_t was_deleted = FALSE;
      const char *parent, *basey;

      svn_path_split(path, &parent, &basey, pool);
      if (entry->kind == svn_node_file)
        {
          was_deleted = entry->deleted;
          SVN_ERR(svn_wc_remove_from_revision_control(parent_access, bname,
                                                      FALSE, FALSE,
                                                      cancel_func,
                                                      cancel_baton,
                                                      pool));
        }
      else if (entry->kind == svn_node_dir)
        {
          apr_hash_t *entries;
          const svn_wc_entry_t *parents_entry;

          /*
           * We are trying to revert the current directory which is
           * scheduled for addition. This is supposed to fail (Issue #854)
           * */
          if (path[0] == '\0')
            {
              return svn_error_create(SVN_ERR_WC_INVALID_OP_ON_CWD, NULL,
                                      _("Cannot revert addition of current directory; "
                                        "please try again from the parent directory"));
            }

          SVN_ERR(svn_wc_entries_read(&entries, parent_access, TRUE, pool));
          parents_entry = apr_hash_get(entries, basey, APR_HASH_KEY_STRING);
          if (parents_entry)
            was_deleted = parents_entry->deleted;
          if (kind == svn_node_none
              || svn_wc__adm_missing(parent_access, path))
            {
              /* Schedule add but missing, just remove the entry
                 or it's missing an adm area in which case
                 svn_wc_adm_probe_retrieve() returned the parent's
                 adm_access, for which we definitely can't use the 'else'
                 code path (as it will remove the parent from version
                 control... (See issue 2425) */
              svn_wc__entry_remove(entries, basey);
              SVN_ERR(svn_wc__entries_write(entries, parent_access, pool));
            }
          else
            SVN_ERR(svn_wc_remove_from_revision_control
                    (dir_access, SVN_WC_ENTRY_THIS_DIR, FALSE, FALSE,
                     cancel_func, cancel_baton, pool));
        }
      else  /* Else it's `none', or something exotic like a symlink... */
        return svn_error_createf
          (SVN_ERR_NODE_UNKNOWN_KIND, NULL,
           _("Unknown or unexpected kind for path '%s'"),
           svn_path_local_style(path, pool));

      /* Recursivity is taken care of by svn_wc_remove_from_revision_control,
         and we've definitely reverted PATH at this point. */
      depth = svn_depth_empty;
      reverted = TRUE;

      /* If the removed item was *also* in a 'deleted' state, make
         sure we leave just a plain old 'deleted' entry behind in the
         parent. */
      if (was_deleted)
        {
          svn_wc_entry_t *tmpentry;
          tmpentry = apr_pcalloc(pool, sizeof(*tmpentry));
          tmpentry->kind = entry->kind;
          tmpentry->deleted = TRUE;

          if (entry->kind == svn_node_dir)
            SVN_ERR(svn_wc__entry_modify(parent_access, basey, tmpentry,
                                         SVN_WC__ENTRY_MODIFY_KIND
                                         | SVN_WC__ENTRY_MODIFY_DELETED,
                                         TRUE, pool));
          else
            SVN_ERR(svn_wc__entry_modify(parent_access, bname, tmpentry,
                                         SVN_WC__ENTRY_MODIFY_KIND
                                         | SVN_WC__ENTRY_MODIFY_DELETED,
                                         TRUE, pool));
        }
    }
  /* Regular prop and text edit. */
  /* Deletions and replacements. */
  else if (entry->schedule == svn_wc_schedule_normal
           || entry->schedule == svn_wc_schedule_delete
           || entry->schedule == svn_wc_schedule_replace)
    {
      /* Revert the prop and text mods (if any). */
      switch (entry->kind)
        {
        case svn_node_file:
          SVN_ERR(revert_admin_things(parent_access, bname, entry,
                                      &reverted, use_commit_times, pool));
          break;

        case svn_node_dir:
          SVN_ERR(revert_admin_things(dir_access, SVN_WC_ENTRY_THIS_DIR, entry,
                                      &reverted, use_commit_times, pool));

          /* Also revert the entry in the parent (issue #2804). */
          if (reverted && bname)
            {
              svn_boolean_t dummy_reverted;
              svn_wc_entry_t *entry_in_parent;
              apr_hash_t *entries;

              SVN_ERR(svn_wc_entries_read(&entries, parent_access, TRUE,
                                          pool));
              entry_in_parent = apr_hash_get(entries, bname,
                                             APR_HASH_KEY_STRING);
              SVN_ERR(revert_admin_things(parent_access, bname,
                                          entry_in_parent, &dummy_reverted,
                                          use_commit_times, pool));
            }

          /* Force recursion on replaced directories. */
          if (entry->schedule == svn_wc_schedule_replace)
            depth = svn_depth_infinity;
          break;

        default:
          /* No op? */
          break;
        }
    }

  /* If PATH was reverted, tell our client that. */
  if ((notify_func != NULL) && reverted)
    (*notify_func)(notify_baton,
                   svn_wc_create_notify(path, svn_wc_notify_revert, pool),
                   pool);

  /* Finally, recurse if requested. */
  if (entry->kind == svn_node_dir && depth > svn_depth_empty)
    {
      apr_hash_t *entries;
      apr_hash_index_t *hi;
      apr_pool_t *subpool = svn_pool_create(pool);

      SVN_ERR(svn_wc_entries_read(&entries, dir_access, FALSE, pool));
      for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
        {
          const void *key;
          const char *keystring;
          const char *full_entry_path;
          svn_depth_t depth_under_here = depth;

          if (depth == svn_depth_files || depth == svn_depth_immediates)
            depth_under_here = svn_depth_empty;

          svn_pool_clear(subpool);

          /* Get the next entry */
          apr_hash_this(hi, &key, NULL, NULL);
          keystring = key;

          /* Skip "this dir" */
          if (! strcmp(keystring, SVN_WC_ENTRY_THIS_DIR))
            continue;

          /* Add the entry name to FULL_ENTRY_PATH. */
          full_entry_path = svn_path_join(path, keystring, subpool);

          /* Revert the entry. */
          SVN_ERR(svn_wc_revert3(full_entry_path, dir_access,
                                 depth_under_here, use_commit_times,
                                 cancel_func, cancel_baton,
                                 notify_func, notify_baton, subpool));
        }

      svn_pool_destroy(subpool);
    }

  return SVN_NO_ERROR;
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
  return svn_wc_revert3(path,
                        parent_access,
                        recursive ? svn_depth_infinity : svn_depth_empty,
                        use_commit_times,
                        cancel_func,
                        cancel_baton,
                        notify_func,
                        notify_baton,
                        pool);
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
svn_wc_get_pristine_copy_path(const char *path,
                              const char **pristine_path,
                              apr_pool_t *pool)
{
  *pristine_path = svn_wc__text_base_path(path, FALSE, pool);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_remove_from_revision_control(svn_wc_adm_access_t *adm_access,
                                    const char *name,
                                    svn_boolean_t destroy_wf,
                                    svn_boolean_t instant_error,
                                    svn_cancel_func_t cancel_func,
                                    void *cancel_baton,
                                    apr_pool_t *pool)
{
  svn_error_t *err;
  svn_boolean_t is_file;
  svn_boolean_t left_something = FALSE;
  apr_hash_t *entries = NULL;
  const char *full_path = apr_pstrdup(pool,
                                      svn_wc_adm_access_path(adm_access));

  /* Check cancellation here, so recursive calls get checked early. */
  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  /* NAME is either a file's basename or SVN_WC_ENTRY_THIS_DIR. */
  is_file = (strcmp(name, SVN_WC_ENTRY_THIS_DIR)) ? TRUE : FALSE;

  if (is_file)
    {
      svn_node_kind_t kind;
      svn_boolean_t wc_special, local_special;
      svn_boolean_t text_modified_p;
      full_path = svn_path_join(full_path, name, pool);

      /* Only check if the file was modified when it wasn't overwritten with a
         special file */
      SVN_ERR(svn_wc__get_special(&wc_special, full_path, adm_access, pool));
      SVN_ERR(svn_io_check_special_path(full_path, &kind, &local_special,
                                        pool));
      if (wc_special || ! local_special)
        {
          /* Check for local mods. before removing entry */
          SVN_ERR(svn_wc_text_modified_p(&text_modified_p, full_path,
                  FALSE, adm_access, pool));
          if (text_modified_p && instant_error)
            return svn_error_createf(SVN_ERR_WC_LEFT_LOCAL_MOD, NULL,
                   _("File '%s' has local modifications"),
                   svn_path_local_style(full_path, pool));
        }

      /* Remove the wcprops. */
      SVN_ERR(svn_wc__props_delete(full_path, svn_wc__props_wcprop,
                                   adm_access, pool));
      /* Remove prop/NAME, prop-base/NAME.svn-base. */
      SVN_ERR(svn_wc__props_delete(full_path, svn_wc__props_working,
                                   adm_access, pool));
      SVN_ERR(svn_wc__props_delete(full_path, svn_wc__props_base,
                                   adm_access, pool));

      /* Remove NAME from PATH's entries file: */
      SVN_ERR(svn_wc_entries_read(&entries, adm_access, TRUE, pool));
      svn_wc__entry_remove(entries, name);
      SVN_ERR(svn_wc__entries_write(entries, adm_access, pool));

      /* Remove text-base/NAME.svn-base */
      SVN_ERR(remove_file_if_present(svn_wc__text_base_path(full_path, 0, pool),
                                     pool));

      /* If we were asked to destroy the working file, do so unless
         it has local mods. */
      if (destroy_wf)
        {
          /* Don't kill local mods. */
          if (text_modified_p || (! wc_special && local_special))
            return svn_error_create(SVN_ERR_WC_LEFT_LOCAL_MOD, NULL, NULL);
          else  /* The working file is still present; remove it. */
            SVN_ERR(remove_file_if_present(full_path, pool));
        }

    }  /* done with file case */

  else /* looking at THIS_DIR */
    {
      apr_pool_t *subpool = svn_pool_create(pool);
      apr_hash_index_t *hi;
      svn_wc_entry_t incomplete_entry;

      /* ### sanity check:  check 2 places for DELETED flag? */

      /* Before we start removing entries from this dir's entries
         file, mark this directory as "incomplete".  This allows this
         function to be interruptible and the wc recoverable by 'svn
         up' later on. */
      incomplete_entry.incomplete = TRUE;
      SVN_ERR(svn_wc__entry_modify(adm_access,
                                   SVN_WC_ENTRY_THIS_DIR,
                                   &incomplete_entry,
                                   SVN_WC__ENTRY_MODIFY_INCOMPLETE,
                                   TRUE, /* sync to disk immediately */
                                   pool));

      /* Get rid of all the wcprops in this directory.  This avoids rewriting
         the wcprops file over and over (meaning O(n^2) complexity)
         below. */
      SVN_ERR(svn_wc__props_delete(full_path, svn_wc__props_wcprop,
                                   adm_access, pool));

      /* Walk over every entry. */
      SVN_ERR(svn_wc_entries_read(&entries, adm_access, FALSE, pool));

      for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
        {
          const void *key;
          void *val;
          const char *current_entry_name;
          const svn_wc_entry_t *current_entry;

          svn_pool_clear(subpool);

          apr_hash_this(hi, &key, NULL, &val);
          current_entry = val;
          if (! strcmp(key, SVN_WC_ENTRY_THIS_DIR))
            current_entry_name = NULL;
          else
            current_entry_name = key;

          if (current_entry->kind == svn_node_file)
            {
              err = svn_wc_remove_from_revision_control
                (adm_access, current_entry_name, destroy_wf, instant_error,
                 cancel_func, cancel_baton, subpool);

              if (err && (err->apr_err == SVN_ERR_WC_LEFT_LOCAL_MOD))
                {
                  if (instant_error)
                    {
                      return err;
                    }
                  else
                    {
                      svn_error_clear(err);
                      left_something = TRUE;
                    }
                }
              else if (err)
                return err;
            }
          else if (current_entry_name && (current_entry->kind == svn_node_dir))
            {
              svn_wc_adm_access_t *entry_access;
              const char *entrypath
                = svn_path_join(svn_wc_adm_access_path(adm_access),
                                current_entry_name,
                                subpool);

              if (svn_wc__adm_missing(adm_access, entrypath))
                {
                  /* The directory is already missing, so don't try to
                     recurse, just delete the entry in the parent
                     directory. */
                  svn_wc__entry_remove(entries, current_entry_name);
                }
              else
                {
                  SVN_ERR(svn_wc_adm_retrieve(&entry_access, adm_access,
                                              entrypath, subpool));

                  err = svn_wc_remove_from_revision_control
                    (entry_access, SVN_WC_ENTRY_THIS_DIR, destroy_wf,
                     instant_error, cancel_func, cancel_baton, subpool);

                  if (err && (err->apr_err == SVN_ERR_WC_LEFT_LOCAL_MOD))
                    {
                      if (instant_error)
                        {
                          return err;
                        }
                      else
                        {
                          svn_error_clear(err);
                          left_something = TRUE;
                        }
                    }
                  else if (err)
                    return err;
                }
            }
        }

      /* At this point, every directory below this one has been
         removed from revision control. */

      /* Remove self from parent's entries file, but only if parent is
         a working copy.  If it's not, that's fine, we just move on. */
      {
        const char *parent_dir, *base_name;
        svn_boolean_t is_root;

        SVN_ERR(svn_wc_is_wc_root(&is_root, full_path, adm_access, pool));

        /* If full_path is not the top of a wc, then its parent
           directory is also a working copy and has an entry for
           full_path.  We need to remove that entry: */
        if (! is_root)
          {
            svn_wc_adm_access_t *parent_access;

            svn_path_split(full_path, &parent_dir, &base_name, pool);

            SVN_ERR(svn_wc_adm_retrieve(&parent_access, adm_access,
                                        parent_dir, pool));
            SVN_ERR(svn_wc_entries_read(&entries, parent_access, TRUE,
                                        pool));
            svn_wc__entry_remove(entries, base_name);
            SVN_ERR(svn_wc__entries_write(entries, parent_access, pool));
          }
      }

      /* Remove the entire administrative .svn area, thereby removing
         _this_ dir from revision control too.  */
      SVN_ERR(svn_wc__adm_destroy(adm_access, subpool));

      /* If caller wants us to recursively nuke everything on disk, go
         ahead, provided that there are no dangling local-mod files
         below */
      if (destroy_wf && (! left_something))
        {
          /* If the dir is *truly* empty (i.e. has no unversioned
             resources, all versioned files are gone, all .svn dirs are
             gone, and contains nothing but empty dirs), then a
             *non*-recursive dir_remove should work.  If it doesn't,
             no big deal.  Just assume there are unversioned items in
             there and set "left_something" */
          err = svn_io_dir_remove_nonrecursive
            (svn_wc_adm_access_path(adm_access), subpool);
          if (err)
            {
              left_something = TRUE;
              svn_error_clear(err);
            }
        }

      svn_pool_destroy(subpool);

    }  /* end of directory case */

  if (left_something)
    return svn_error_create(SVN_ERR_WC_LEFT_LOCAL_MOD, NULL, NULL);
  else
    return SVN_NO_ERROR;
}



/*** Resolving a conflict automatically ***/


/* Helper for resolve_conflict_on_entry.  Delete the file BASE_NAME in
   PARENT_DIR if it exists.  Set WAS_PRESENT to TRUE if the file existed,
   and to FALSE otherwise. */
static svn_error_t *
attempt_deletion(const char *parent_dir,
                 const char *base_name,
                 svn_boolean_t *was_present,
                 apr_pool_t *pool)
{
  const char *full_path = svn_path_join(parent_dir, base_name, pool);
  svn_error_t *err = svn_io_remove_file(full_path, pool);

  *was_present = ! err || ! APR_STATUS_IS_ENOENT(err->apr_err);
  if (*was_present)
    return err;

  svn_error_clear(err);
  return SVN_NO_ERROR;
}


/* Conflict resolution involves removing the conflict files, if they exist,
   and clearing the conflict filenames from the entry.  The latter needs to
   be done whether or not the conflict files exist.  If @a conflict_choice
   is svn_wc_conflict_choose_base, resolve the conflict with the old
   file contents; if svn_wc_conflict_choose_mine, use the original
   working contents; if svn_wc_conflict_choose_theirs, the new
   contents; and if svn_wc_conflict_choose_merged, don't change the
   contents at all, just remove the conflict status (i.e. pre-1.5 behavior).

   @since 1.5 Automatic Conflict Resolution (Issue 2784)

   PATH is the path to the item to be resolved, BASE_NAME is the basename
   of PATH, and CONFLICT_DIR is the access baton for PATH.  ORIG_ENTRY is
   the entry prior to resolution. RESOLVE_TEXT and RESOLVE_PROPS are TRUE
   if text and property conficts respectively are to be resolved. */
static svn_error_t *
resolve_conflict_on_entry(const char *path,
                          const svn_wc_entry_t *orig_entry,
                          svn_wc_adm_access_t *conflict_dir,
                          const char *base_name,
                          svn_boolean_t resolve_text,
                          svn_boolean_t resolve_props,
                          svn_wc_conflict_choice_t conflict_choice,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          apr_pool_t *pool)
{
  svn_boolean_t was_present, need_feedback = FALSE;
  apr_uint64_t modify_flags = 0;
  svn_wc_entry_t *entry = svn_wc_entry_dup(orig_entry, pool);
  const char *auto_resolve_src;

  /* Handle automatic conflict resolution before the temporary files are
   * deleted, if necessary. */
  switch (conflict_choice)
    {
    case svn_wc_conflict_choose_base:
      auto_resolve_src = entry->conflict_old;
      break;
    case svn_wc_conflict_choose_mine:
      auto_resolve_src = entry->conflict_wrk;
      break;
    case svn_wc_conflict_choose_theirs:
      auto_resolve_src = entry->conflict_new;
      break;
    case svn_wc_conflict_choose_merged:
      auto_resolve_src = NULL;
      break;
    default:
      return svn_error_create(SVN_ERR_INCORRECT_PARAMS, NULL,
                              _("Invalid 'conflict_result' argument"));
    }

    if (auto_resolve_src)
      SVN_ERR(svn_io_copy_file(
        svn_path_join(svn_wc_adm_access_path(conflict_dir), auto_resolve_src,
                      pool),
        path, TRUE, pool));

  /* Yes indeed, being able to map a function over a list would be nice. */
  if (resolve_text && entry->conflict_old)
    {
      SVN_ERR(attempt_deletion(svn_wc_adm_access_path(conflict_dir),
                               entry->conflict_old, &was_present, pool));
      modify_flags |= SVN_WC__ENTRY_MODIFY_CONFLICT_OLD;
      entry->conflict_old = NULL;
      need_feedback |= was_present;
    }
  if (resolve_text && entry->conflict_new)
    {
      SVN_ERR(attempt_deletion(svn_wc_adm_access_path(conflict_dir),
                               entry->conflict_new, &was_present, pool));
      modify_flags |= SVN_WC__ENTRY_MODIFY_CONFLICT_NEW;
      entry->conflict_new = NULL;
      need_feedback |= was_present;
    }
  if (resolve_text && entry->conflict_wrk)
    {
      SVN_ERR(attempt_deletion(svn_wc_adm_access_path(conflict_dir),
                               entry->conflict_wrk, &was_present, pool));
      modify_flags |= SVN_WC__ENTRY_MODIFY_CONFLICT_WRK;
      entry->conflict_wrk = NULL;
      need_feedback |= was_present;
    }
  if (resolve_props && entry->prejfile)
    {
      SVN_ERR(attempt_deletion(svn_wc_adm_access_path(conflict_dir),
                               entry->prejfile, &was_present, pool));
      modify_flags |= SVN_WC__ENTRY_MODIFY_PREJFILE;
      entry->prejfile = NULL;
      need_feedback |= was_present;
    }

  if (modify_flags)
    {
      /* Although removing the files is sufficient to indicate that the
         conflict is resolved, if we update the entry as well future checks
         for conflict state will be more efficient. */
      SVN_ERR(svn_wc__entry_modify
              (conflict_dir,
               (entry->kind == svn_node_dir ? NULL : base_name),
               entry, modify_flags, TRUE, pool));

      /* No feedback if no files were deleted and all we did was change the
         entry, such a file did not appear as a conflict */
      if (need_feedback && notify_func)
        {
          /* Sanity check:  see if libsvn_wc *still* thinks this item is in a
             state of conflict that we have asked to resolve.  If not, report
             the successful resolution.  */
          svn_boolean_t text_conflict, prop_conflict;
          SVN_ERR(svn_wc_conflicted_p(&text_conflict, &prop_conflict,
                                      svn_wc_adm_access_path(conflict_dir),
                                      entry, pool));
          if ((! (resolve_text && text_conflict))
              && (! (resolve_props && prop_conflict)))
            (*notify_func)(notify_baton,
                           svn_wc_create_notify(path, svn_wc_notify_resolved,
                                                pool), pool);
        }
    }

  return SVN_NO_ERROR;
}

/* Machinery for an automated entries walk... */

struct resolve_callback_baton
{
  /* TRUE if text conflicts are to be resolved. */
  svn_boolean_t resolve_text;
  /* TRUE if property conflicts are to be resolved. */
  svn_boolean_t resolve_props;
  /* The type of automatic conflict resolution to perform */
  svn_wc_conflict_choice_t conflict_choice;
  /* An access baton for the tree, with write access */
  svn_wc_adm_access_t *adm_access;
  /* Notification function and baton */
  svn_wc_notify_func2_t notify_func;
  void *notify_baton;
};

static svn_error_t *
resolve_found_entry_callback(const char *path,
                             const svn_wc_entry_t *entry,
                             void *walk_baton,
                             apr_pool_t *pool)
{
  struct resolve_callback_baton *baton = walk_baton;
  const char *conflict_dir, *base_name = NULL;
  svn_wc_adm_access_t *adm_access;

  /* We're going to receive dirents twice;  we want to ignore the
     first one (where it's a child of a parent dir), and only print
     the second one (where we're looking at THIS_DIR). */
  if ((entry->kind == svn_node_dir)
      && (strcmp(entry->name, SVN_WC_ENTRY_THIS_DIR)))
    return SVN_NO_ERROR;

  /* Figger out the directory in which the conflict resides. */
  if (entry->kind == svn_node_dir)
    conflict_dir = path;
  else
    svn_path_split(path, &conflict_dir, &base_name, pool);
  SVN_ERR(svn_wc_adm_retrieve(&adm_access, baton->adm_access, conflict_dir,
                              pool));

  return resolve_conflict_on_entry(path, entry, adm_access, base_name,
                                   baton->resolve_text, baton->resolve_props,
                                   baton->conflict_choice, baton->notify_func,
                                   baton->notify_baton, pool);
}

static const svn_wc_entry_callbacks2_t
resolve_walk_callbacks =
  {
    resolve_found_entry_callback,
    svn_wc__walker_default_error_handler
  };


/* The public function */
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
                                   resolve_props, recurse,
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
  struct resolve_callback_baton *baton = apr_pcalloc(pool, sizeof(*baton));

  baton->resolve_text = resolve_text;
  baton->resolve_props = resolve_props;
  baton->adm_access = adm_access;
  baton->notify_func = notify_func;
  baton->notify_baton = notify_baton;
  baton->conflict_choice = conflict_choice;

  if (depth == svn_depth_empty)
    {
      const svn_wc_entry_t *entry;
      SVN_ERR(svn_wc__entry_versioned(&entry, path, adm_access, FALSE, pool));

      SVN_ERR(resolve_found_entry_callback(path, entry, baton, pool));
    }
  else
    {
      SVN_ERR(svn_wc_walk_entries3(path, adm_access,
                                   &resolve_walk_callbacks, baton, depth,
                                   FALSE, cancel_func, cancel_baton, pool));

    }

  return SVN_NO_ERROR;
}

svn_error_t *svn_wc_add_lock(const char *path, const svn_lock_t *lock,
                             svn_wc_adm_access_t *adm_access, apr_pool_t *pool)
{
  const svn_wc_entry_t *entry;
  svn_wc_entry_t newentry;

  SVN_ERR(svn_wc__entry_versioned(&entry, path, adm_access, FALSE, pool));


  newentry.lock_token = lock->token;
  newentry.lock_owner = lock->owner;
  newentry.lock_comment = lock->comment;
  newentry.lock_creation_date = lock->creation_date;

  SVN_ERR(svn_wc__entry_modify(adm_access, entry->name, &newentry,
                               SVN_WC__ENTRY_MODIFY_LOCK_TOKEN
                               | SVN_WC__ENTRY_MODIFY_LOCK_OWNER
                               | SVN_WC__ENTRY_MODIFY_LOCK_COMMENT
                               | SVN_WC__ENTRY_MODIFY_LOCK_CREATION_DATE,
                               TRUE, pool));

  { /* if svn:needs-lock is present, then make the file read-write. */
    const svn_string_t *needs_lock;

    SVN_ERR(svn_wc_prop_get(&needs_lock, SVN_PROP_NEEDS_LOCK,
                            path, adm_access, pool));
    if (needs_lock)
      SVN_ERR(svn_io_set_file_read_write(path, FALSE, pool));
  }

  return SVN_NO_ERROR;
}

svn_error_t *svn_wc_remove_lock(const char *path,
                             svn_wc_adm_access_t *adm_access, apr_pool_t *pool)
{
  const svn_wc_entry_t *entry;
  svn_wc_entry_t newentry;

  SVN_ERR(svn_wc__entry_versioned(&entry, path, adm_access, FALSE, pool));

  newentry.lock_token = newentry.lock_owner = newentry.lock_comment = NULL;
  newentry.lock_creation_date = 0;
  SVN_ERR(svn_wc__entry_modify(adm_access, entry->name, &newentry,
                               SVN_WC__ENTRY_MODIFY_LOCK_TOKEN
                               | SVN_WC__ENTRY_MODIFY_LOCK_OWNER
                               | SVN_WC__ENTRY_MODIFY_LOCK_COMMENT
                               | SVN_WC__ENTRY_MODIFY_LOCK_CREATION_DATE,
                               TRUE, pool));

  { /* if svn:needs-lock is present, then make the file read-only. */
    const svn_string_t *needs_lock;

    SVN_ERR(svn_wc_prop_get(&needs_lock, SVN_PROP_NEEDS_LOCK,
                            path, adm_access, pool));
    if (needs_lock)
      SVN_ERR(svn_io_set_file_read_only(path, FALSE, pool));
  }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_set_changelist(const apr_array_header_t *paths,
                      const char *changelist,
                      const char *matching_changelist,
                      svn_cancel_func_t cancel_func,
                      void *cancel_baton,
                      svn_wc_notify_func2_t notify_func,
                      void *notify_baton,
                      apr_pool_t *pool)
{
  int i;
  apr_pool_t *iterpool = svn_pool_create(pool);

  for (i = 0; i < paths->nelts; i++)
    {
      const char *path = APR_ARRAY_IDX(paths, i, const char *);
      svn_wc_adm_access_t *adm_access;
      const svn_wc_entry_t *entry;
      svn_wc_entry_t newentry;
      svn_wc_notify_t *notify;

      svn_pool_clear(iterpool);

      /* Check for cancellation */
      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      SVN_ERR(svn_wc_adm_probe_open3(&adm_access, NULL, path,
                                     TRUE, /* get write lock */
                                     0, /* levels to lock */
                                     NULL, NULL, iterpool));

      SVN_ERR(svn_wc_entry(&entry, path, adm_access, FALSE, iterpool));

      /* Is this an unversioned path?  Skip it. */
      if (! entry)
        {
          if (notify_func)
            {
              svn_error_t *unversioned_err =
                  svn_error_createf(SVN_ERR_UNVERSIONED_RESOURCE, NULL,
                                    _("'%s' is not under version control"),
                                    path);
              notify = svn_wc_create_notify(path,
                                            svn_wc_notify_changelist_failed,
                                            iterpool);
              notify->err = unversioned_err;
              notify_func(notify_baton, notify, iterpool);
            }
          continue;
        }

      /* Is path a directory?  Skip it.

         ### TODO(sussman): we may want to allow directories to be
             members of changelists one day, but we'll have to make
             them take --depth arguments or something, to Do It Right.
      */
      if (entry->kind == svn_node_dir)
        {
          if (notify_func)
            {
              svn_error_t *unversioned_err =
                  svn_error_createf(SVN_ERR_CLIENT_IS_DIRECTORY, NULL,
                                    _("'%s' is a directory, and thus cannot"
                                      " be a member of a changelist"),
                                    path);
              notify = svn_wc_create_notify(path,
                                            svn_wc_notify_changelist_failed,
                                            iterpool);
              notify->err = unversioned_err;
              notify_func(notify_baton, notify, iterpool);
            }
          continue;
        }

      /* If the path is already assigned to the changelist we're
         trying to assign, or if we're trying to remove a changelist
         assignment from a path that has none, there's nothing to
         do. */
      if (entry->changelist 
          && changelist 
          && strcmp(entry->changelist, changelist) == 0)
        continue;
      if (! (entry->changelist || changelist))
        continue;

      /* Possibly enforce matching with an existing changelist. */
      if (matching_changelist
          && entry->changelist 
          && strcmp(entry->changelist, matching_changelist) != 0)
        {
          if (notify_func)
            {
              svn_error_t *mismatch_err =
                svn_error_createf(SVN_ERR_WC_MISMATCHED_CHANGELIST, NULL,
                                  _("'%s' is not currently a member of "
                                    "changelist '%s'."),
                                  path, matching_changelist);
              notify = svn_wc_create_notify(path,
                                            svn_wc_notify_changelist_failed,
                                            iterpool);
              notify->err = mismatch_err;
              notify_func(notify_baton, notify, iterpool);
            }
          continue;
        }

      /* If the path is already a member of a changelist, warn the
         user about this, but still allow the reassignment to happen.
      */
      if (entry->changelist && changelist)
        {
          if (notify_func)
            {
              svn_error_t *unversioned_err =
                  svn_error_createf(SVN_ERR_WC_CHANGELIST_MOVE, NULL,
                                    _("Removing '%s' from"
                                      " changelist '%s'."),
                                    path, entry->changelist);
              notify = svn_wc_create_notify(path,
                                            svn_wc_notify_changelist_moved,
                                            iterpool);
              notify->err = unversioned_err;
              notify_func(notify_baton, notify, iterpool);
            }
        }

      newentry.changelist = changelist;

      SVN_ERR(svn_wc__entry_modify(adm_access, entry->name, &newentry,
                                   SVN_WC__ENTRY_MODIFY_CHANGELIST,
                                   TRUE, iterpool));
      SVN_ERR(svn_wc_adm_close(adm_access));

      if (notify_func)
        {
          notify = svn_wc_create_notify(path,
                                        changelist
                                        ? svn_wc_notify_changelist_set
                                        : svn_wc_notify_changelist_clear,
                                        iterpool);
          notify->changelist_name = changelist;
          notify_func(notify_baton, notify, iterpool);
        }
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}
