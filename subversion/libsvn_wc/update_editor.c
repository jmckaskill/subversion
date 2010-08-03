/*
 * update_editor.c :  main editor for checkouts and updates
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



#include <stdlib.h>
#include <string.h>

#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_md5.h>
#include <apr_tables.h>
#include <apr_file_io.h>
#include <apr_strings.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_delta.h"
#include "svn_string.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_xml.h"
#include "svn_error.h"
#include "svn_io.h"
#include "svn_private_config.h"
#include "svn_time.h"
#include "svn_config.h"
#include "svn_iter.h"

#include "wc.h"
#include "log.h"
#include "adm_files.h"
#include "entries.h"
#include "lock.h"
#include "translate.h"
#include "tree_conflicts.h"
#include "workqueue.h"

#include "private/svn_wc_private.h"

/* Checks whether a svn_wc__db_status_t indicates whether a node is
   present in a working copy. Used by the editor implementation */
#define IS_NODE_PRESENT(status)                             \
           ((status) != svn_wc__db_status_absent &&         \
            (status) != svn_wc__db_status_excluded &&       \
            (status) != svn_wc__db_status_not_present)


/*
 * This code handles "checkout" and "update" and "switch".
 * A checkout is similar to an update that is only adding new items.
 *
 * The intended behaviour of "update" and "switch", focusing on the checks
 * to be made before applying a change, is:
 *
 *   For each incoming change:
 *     if target is already in conflict or obstructed:
 *       skip this change
 *     else
 *     if this action will cause a tree conflict:
 *       record the tree conflict
 *       skip this change
 *     else:
 *       make this change
 *
 * In more detail:
 *
 *   For each incoming change:
 *
 *   1.   if  # Incoming change is inside an item already in conflict:
 *    a.    tree/text/prop change to node beneath tree-conflicted dir
 *        then  # Skip all changes in this conflicted subtree [*1]:
 *          do not update the Base nor the Working
 *          notify "skipped because already in conflict" just once
 *            for the whole conflicted subtree
 *
 *        if  # Incoming change affects an item already in conflict:
 *    b.    tree/text/prop change to tree-conflicted dir/file, or
 *    c.    tree change to a text/prop-conflicted file/dir, or
 *    d.    text/prop change to a text/prop-conflicted file/dir [*2], or
 *    e.    tree change to a dir tree containing any conflicts,
 *        then  # Skip this change [*1]:
 *          do not update the Base nor the Working
 *          notify "skipped because already in conflict"
 *
 *   2.   if  # Incoming change affects an item that's "obstructed":
 *    a.    on-disk node kind doesn't match recorded Working node kind
 *            (including an absence/presence mis-match),
 *        then  # Skip this change [*1]:
 *          do not update the Base nor the Working
 *          notify "skipped because obstructed"
 *
 *   3.   if  # Incoming change raises a tree conflict:
 *    a.    tree/text/prop change to node beneath sched-delete dir, or
 *    b.    tree/text/prop change to sched-delete dir/file, or
 *    c.    text/prop change to tree-scheduled dir/file,
 *        then  # Skip this change:
 *          do not update the Base nor the Working [*3]
 *          notify "tree conflict"
 *
 *   4.   Apply the change:
 *          update the Base
 *          update the Working, possibly raising text/prop conflicts
 *          notify
 *
 * Notes:
 *
 *      "Tree change" here refers to an add or delete of the target node,
 *      including the add or delete part of a copy or move or rename.
 *
 * [*1] We should skip changes to an entire node, as the base revision number
 *      applies to the entire node. Not sure how this affects attempts to
 *      handle text and prop changes separately.
 *
 * [*2] Details of which combinations of property and text changes conflict
 *      are not specified here.
 *
 * [*3] For now, we skip the update, and require the user to:
 *        - Modify the WC to be compatible with the incoming change;
 *        - Mark the conflict as resolved;
 *        - Repeat the update.
 *      Ideally, it would be possible to resolve any conflict without
 *      repeating the update. To achieve this, we would have to store the
 *      necessary data at conflict detection time, and delay the update of
 *      the Base until the time of resolving.
 */


/*** batons ***/

struct edit_baton
{
  /* For updates, the "destination" of the edit is ANCHOR_ABSPATH, the
     directory containing TARGET_ABSPATH. If ANCHOR_ABSPATH itself is the
     target, the values are identical.

     TARGET_BASENAME is the name of TARGET_ABSPATH in ANCHOR_ABSPATH, or "" if
     ANCHOR_ABSPATH is the target */
  const char *target_basename;

  /* Absolute variants of ANCHOR and TARGET */
  const char *anchor_abspath;
  const char *target_abspath;

  /* The DB handle for managing the working copy state.  */
  svn_wc__db_t *db;
  svn_wc_context_t *wc_ctx;

  /* Array of file extension patterns to preserve as extensions in
     generated conflict files. */
  const apr_array_header_t *ext_patterns;

  /* The revision we're targeting...or something like that.  This
     starts off as a pointer to the revision to which we are updating,
     or SVN_INVALID_REVNUM, but by the end of the edit, should be
     pointing to the final revision. */
  svn_revnum_t *target_revision;

  /* The requested depth of this edit. */
  svn_depth_t requested_depth;

  /* Is the requested depth merely an operational limitation, or is
     also the new sticky ambient depth of the update target? */
  svn_boolean_t depth_is_sticky;

  /* Need to know if the user wants us to overwrite the 'now' times on
     edited/added files with the last-commit-time. */
  svn_boolean_t use_commit_times;

  /* Was the root actually opened (was this a non-empty edit)? */
  svn_boolean_t root_opened;

  /* Was the update-target deleted?  This is a special situation. */
  svn_boolean_t target_deleted;

  /* Allow unversioned obstructions when adding a path. */
  svn_boolean_t allow_unver_obstructions;

  /* If this is a 'switch' operation, the new relpath of target_abspath,
     else NULL. */
  const char *switch_relpath;

  /* The URL to the root of the repository. */
  const char *repos_root;

  /* The UUID of the repos, or NULL. */
  const char *repos_uuid;

  /* External diff3 to use for merges (can be null, in which case
     internal merge code is used). */
  const char *diff3_cmd;

  /* Externals handler */
  svn_wc_external_update_t external_func;
  void *external_baton;

  /* This editor sends back notifications as it edits. */
  svn_wc_notify_func2_t notify_func;
  void *notify_baton;

  /* This editor is normally wrapped in a cancellation editor anyway,
     so it doesn't bother to check for cancellation itself.  However,
     it needs a cancel_func and cancel_baton available to pass to
     long-running functions. */
  svn_cancel_func_t cancel_func;
  void *cancel_baton;

  /* This editor will invoke a interactive conflict-resolution
     callback, if available. */
  svn_wc_conflict_resolver_func_t conflict_func;
  void *conflict_baton;

  /* If the server sends add_file(copyfrom=...) and we don't have the
     copyfrom file in the working copy, we use this callback to fetch
     it directly from the repository. */
  svn_wc_get_file_t fetch_func;
  void *fetch_baton;

  /* Subtrees that were skipped during the edit, and therefore shouldn't
     have their revision/url info updated at the end.  If a path is a
     directory, its descendants will also be skipped.  The keys are absolute
     pathnames and the values unspecified. */
  apr_hash_t *skipped_trees;

  apr_pool_t *pool;
};


/* Record in the edit baton EB that LOCAL_ABSPATH's base version is not being
 * updated.
 *
 * Add to EB->skipped_trees a copy (allocated in EB->pool) of the string
 * LOCAL_ABSPATH.
 */
static svn_error_t *
remember_skipped_tree(struct edit_baton *eb, const char *local_abspath)
{
  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  apr_hash_set(eb->skipped_trees,  apr_pstrdup(eb->pool, local_abspath),
               APR_HASH_KEY_STRING, (void*)1);

  return SVN_NO_ERROR;
}

struct dir_baton
{
  /* Basename of this directory. */
  const char *name;

  /* Absolute path of this directory */
  const char *local_abspath;

  /* The repository relative path this directory will correspond to. */
  const char *new_relpath;

  /* The revision of the directory before updating */
  svn_revnum_t old_revision;

  /* The global edit baton. */
  struct edit_baton *edit_baton;

  /* Baton for this directory's parent, or NULL if this is the root
     directory. */
  struct dir_baton *parent_baton;

  /* Set if updates to this directory are skipped */
  svn_boolean_t skip_this;

  /* Set if updates to all descendants of this directory are skipped */
  svn_boolean_t skip_descendants;

  /* Set if there was a previous notification for this directory */
  svn_boolean_t already_notified;

  /* Set if this directory is being added during this editor drive. */
  svn_boolean_t adding_dir;

  /* Set on a node and its descendants when a node gets tree conflicted
     and descendants should still be updated (not skipped).
     These nodes should all be marked as deleted. */
  svn_boolean_t in_deleted_and_tree_conflicted_subtree;

  /* Set if an unversioned dir of the same name already existed in
     this directory. */
  svn_boolean_t obstruction_found;

  /* Set if a dir of the same name already exists and is
     scheduled for addition without history. */
  svn_boolean_t add_existed;

  /* An array of svn_prop_t structures, representing all the property
     changes to be applied to this directory. */
  apr_array_header_t *propchanges;

  /* The bump information for this directory. */
  struct bump_dir_info *bump_info;

  /* The depth of the directory in the wc (or inferred if added).  Not
     used for filtering; we have a separate wrapping editor for that. */
  svn_depth_t ambient_depth;

  /* Was the directory marked as incomplete before the update?
     (In other words, are we resuming an interrupted update?)

     If WAS_INCOMPLETE is set to TRUE we expect to receive all child nodes
     and properties for/of the directory. If WAS_INCOMPLETE is FALSE then
     we only receive the changes in/for children and properties.*/
  svn_boolean_t was_incomplete;

  /* The pool in which this baton itself is allocated. */
  apr_pool_t *pool;
};


/* The bump information is tracked separately from the directory batons.
   This is a small structure kept in the edit pool, while the heavier
   directory baton is managed by the editor driver.

   In a postfix delta case, the directory batons are going to disappear.
   The files will refer to these structures, rather than the full
   directory baton.  */
struct bump_dir_info
{
  /* ptr to the bump information for the parent directory */
  struct bump_dir_info *parent;

  /* how many entries are referring to this bump information? */
  int ref_count;

  /* the absolute path of the directory to bump */
  const char *local_abspath;

  /* Set if this directory is skipped due to prop or tree conflicts.
     This does NOT mean that children are skipped. */
  svn_boolean_t skipped;

  /* Pool that should be cleared after the dir is bumped */
  apr_pool_t *pool;
};


struct handler_baton
{
  svn_txdelta_window_handler_t apply_handler;
  void *apply_baton;
  apr_pool_t *pool;
  struct file_baton *fb;

  /* Where we are assembling the new file. */
  const char *new_text_base_tmp_abspath;

    /* The expected MD5 checksum of the text source or NULL if no base
     checksum is available */
  svn_checksum_t *expected_source_md5_checksum;

  /* Why two checksums?
     The editor currently provides an md5 which we use to detect corruption
     during transmission.  We use the sha1 inside libsvn_wc both for pristine
     handling and corruption detection.  In the future, the editor will also
     provide a sha1, so we may not have to calculate both, but for the time
     being, that's the way it is. */

  /* The calculated checksum of the text source or NULL if the acual
     checksum is not being calculated */
  svn_checksum_t *actual_source_md5_checksum;

  /* The stream used to calculate the source checksums */
  svn_stream_t *source_checksum_stream;

  /* A calculated MD5 digest of NEW_TEXT_BASE_TMP_ABSPATH.
     This is initialized to all zeroes when the baton is created, then
     populated with the MD5 digest of the resultant fulltext after the
     last window is handled by the handler returned from
     apply_textdelta(). */
  unsigned char new_text_base_md5_digest[APR_MD5_DIGESTSIZE];

  /* A calculated SHA-1 of NEW_TEXT_BASE_TMP_ABSPATH, which we'll use for
     eventually writing the pristine. */
  svn_checksum_t * new_text_base_sha1_checksum;
};


/* Get an empty file in the temporary area for WRI_ABSPATH.  The file will
   not be set for automatic deletion, and the name will be returned in
   TMP_FILENAME.

   This implementation creates a new empty file with a unique name.

   ### This is inefficient for callers that just want an empty file to read
   ### from.  There could be (and there used to be) a permanent, shared
   ### empty file for this purpose.

   ### This is inefficient for callers that just want to reserve a unique
   ### file name to create later.  A better way may not be readily available.
 */
static svn_error_t *
get_empty_tmp_file(const char **tmp_filename,
                   svn_wc__db_t *db,
                   const char *wri_abspath,
                   apr_pool_t *result_pool,
                   apr_pool_t *scratch_pool)
{
  const char *temp_dir_path;
  apr_file_t *file;

  SVN_ERR(svn_wc__db_temp_wcroot_tempdir(&temp_dir_path, db, wri_abspath,
                                         scratch_pool, scratch_pool));
  SVN_ERR(svn_io_open_unique_file3(&file, tmp_filename, temp_dir_path,
                                   svn_io_file_del_none,
                                   scratch_pool, scratch_pool));
  SVN_ERR(svn_io_file_close(file, scratch_pool));

  return svn_error_return(svn_dirent_get_absolute(tmp_filename, *tmp_filename,
                                                  result_pool));
}


/* Return the repository relative path for LOCAL_ABSPATH allocated in
 * RESULT_POOL, or NULL if unable to obtain.
 *
 * Use DB to retrieve information on LOCAL_ABSPATH, and do all temporary
 * allocation in SCRATCH_POOL.
 */
static const char *
node_get_relpath_ignore_errors(svn_wc__db_t *db,
                               const char *local_abspath,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  svn_error_t *err;
  const char *relpath = NULL;

  err = svn_wc__db_read_info(&status, NULL, NULL, &relpath, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL,
                             db, local_abspath, result_pool, scratch_pool);

  if (err)
    {
      svn_error_clear(err);
      return NULL;
    }

  if (relpath)
    return relpath;

  if (status == svn_wc__db_status_added ||
      status == svn_wc__db_status_obstructed_add)
    {
      svn_error_clear(svn_wc__db_scan_addition(NULL, NULL, &relpath, NULL,
                                               NULL, NULL, NULL, NULL, NULL,
                                               db, local_abspath,
                                               result_pool, scratch_pool));
    }
  else if (status != svn_wc__db_status_deleted &&
           status != svn_wc__db_status_obstructed_delete)
    {
      svn_error_clear(svn_wc__db_scan_base_repos(&relpath, NULL, NULL,
                                                 db, local_abspath,
                                                 result_pool, scratch_pool));
    }

  return relpath;
}


/* An APR pool cleanup handler.  This runs the log file for a
   directory baton. */
static apr_status_t
cleanup_dir_baton(void *dir_baton)
{
  struct dir_baton *db = dir_baton;
  struct edit_baton *eb = db->edit_baton;
  svn_error_t *err;
  apr_pool_t *pool = apr_pool_parent_get(db->pool);

  err = svn_wc__wq_run(eb->db, db->local_abspath,
                       eb->cancel_func, eb->cancel_baton,
                       pool);

  if (err)
    {
      apr_status_t apr_err = err->apr_err;
      svn_error_clear(err);
      return apr_err;
    }
  return APR_SUCCESS;
}

/* An APR pool cleanup handler.  This is a child handler, it removes
   the mail pool handler.
   <stsp> mail pool?
   <hwright> that's where the missing commit mails are going!  */
static apr_status_t
cleanup_dir_baton_child(void *dir_baton)
{
  struct dir_baton *db = dir_baton;
  apr_pool_cleanup_kill(db->pool, db, cleanup_dir_baton);
  return APR_SUCCESS;
}


/* Return a new dir_baton to represent NAME (a subdirectory of
   PARENT_BATON).  If PATH is NULL, this is the root directory of the
   edit. ADDING should be TRUE if we are adding this directory.  */
static svn_error_t *
make_dir_baton(struct dir_baton **d_p,
               const char *path,
               struct edit_baton *eb,
               struct dir_baton *pb,
               svn_boolean_t adding,
               apr_pool_t *scratch_pool)
{
  apr_pool_t *dir_pool;
  struct dir_baton *d;
  struct bump_dir_info *bdi;

  if (pb != NULL)
    dir_pool = svn_pool_create(pb->pool);
  else
    dir_pool = svn_pool_create(eb->pool);

  SVN_ERR_ASSERT(path || (! pb));

  /* Okay, no easy out, so allocate and initialize a dir baton. */
  d = apr_pcalloc(dir_pool, sizeof(*d));

  /* Construct the PATH and baseNAME of this directory. */
  if (path)
    {
      d->name = svn_dirent_basename(path, dir_pool);
      d->local_abspath = svn_dirent_join(pb->local_abspath, d->name, dir_pool);
      d->in_deleted_and_tree_conflicted_subtree =
          pb->in_deleted_and_tree_conflicted_subtree;
    }
  else
    {
      /* This is the root baton. */
      d->name = NULL;
      d->local_abspath = eb->anchor_abspath;
    }

  /* Figure out the new_relpath for this directory. */
  if (eb->switch_relpath)
    {
      /* Handle switches... */

      if (pb == NULL)
        {
          if (*eb->target_basename == '\0')
            {
              /* No parent baton and target_basename=="" means that we are
                 the target of the switch. Thus, our NEW_RELPATH will be
                 the SWITCH_RELPATH.  */
              d->new_relpath = eb->switch_relpath;
            }
          else
            {
              /* This node is NOT the target of the switch (one of our
                 children is the target); therefore, it must already exist.
                 Get its old REPOS_RELPATH, as it won't be changing.  */
              SVN_ERR(svn_wc__db_scan_base_repos(&d->new_relpath, NULL, NULL,
                                                 eb->db, d->local_abspath,
                                                 dir_pool, scratch_pool));
            }
        }
      else
        {
          /* This directory is *not* the root (has a parent). If there is
             no grandparent, then we may have anchored at the parent,
             and self is the target. If we match the target, then set
             NEW_RELPATH to the SWITCH_RELPATH.

             Otherwise, we simply extend NEW_RELPATH from the parent.  */
          if (pb->parent_baton == NULL
              && strcmp(eb->target_basename, d->name) == 0)
            d->new_relpath = eb->switch_relpath;
          else
            d->new_relpath = svn_relpath_join(pb->new_relpath, d->name,
                                              dir_pool);
        }
    }
  else  /* must be an update */
    {
      /* If we are adding the node, then simply extend the parent's
         relpath for our own.  */
      if (adding)
        {
          SVN_ERR_ASSERT(pb != NULL);
          d->new_relpath = svn_relpath_join(pb->new_relpath, d->name,
                                            dir_pool);
        }
      else
        {
          /* Get the original REPOS_RELPATH. An update will not be
             changing its value.  */
          SVN_ERR(svn_wc__db_scan_base_repos(&d->new_relpath, NULL, NULL,
                                             eb->db, d->local_abspath,
                                             dir_pool, scratch_pool));
        }
    }

  /* the bump information lives in the edit pool */
  bdi = apr_pcalloc(dir_pool, sizeof(*bdi));
  bdi->parent = pb ? pb->bump_info : NULL;
  bdi->ref_count = 1;
  bdi->local_abspath = apr_pstrdup(eb->pool, d->local_abspath);
  bdi->skipped = FALSE;
  bdi->pool = dir_pool;

  /* the parent's bump info has one more referer */
  if (pb)
    ++bdi->parent->ref_count;

  d->edit_baton   = eb;
  d->parent_baton = pb;
  d->pool         = dir_pool;
  d->propchanges  = apr_array_make(dir_pool, 1, sizeof(svn_prop_t));
  d->obstruction_found = FALSE;
  d->add_existed  = FALSE;
  d->bump_info    = bdi;
  d->old_revision = SVN_INVALID_REVNUM;
  d->adding_dir   = adding;

  /* The caller of this function needs to fill these in. */
  d->ambient_depth = svn_depth_unknown;
  d->was_incomplete = FALSE;

  apr_pool_cleanup_register(dir_pool, d, cleanup_dir_baton,
                            cleanup_dir_baton_child);

  *d_p = d;
  return SVN_NO_ERROR;
}


/* Forward declarations. */
static svn_error_t *
do_entry_deletion(struct edit_baton *eb,
                  const char *local_abspath,
                  const char *their_url,
                  svn_boolean_t in_deleted_and_tree_conflicted_subtree,
                  apr_pool_t *pool);

static svn_error_t *
already_in_a_tree_conflict(svn_boolean_t *conflicted,
                           svn_wc__db_t *db,
                           const char *local_abspath,
                           apr_pool_t *scratch_pool);


static void
do_notification(const struct edit_baton *eb,
                const char *local_abspath,
                svn_node_kind_t kind,
                svn_wc_notify_action_t action,
                apr_pool_t *scratch_pool)
{
  svn_wc_notify_t *notify;

  if (eb->notify_func == NULL)
    return;

  notify = svn_wc_create_notify(local_abspath, action, scratch_pool);
  notify->kind = kind;

  (*eb->notify_func)(eb->notify_baton, notify, scratch_pool);
}


/* Helper for maybe_bump_dir_info():

   In a single atomic action, (1) remove any 'deleted' entries from a
   directory, (2) remove any 'absent' entries whose revision numbers
   are different from the parent's new target revision, (3) remove any
   'missing' dir entries, and (4) remove the directory's 'incomplete'
   flag. */
static svn_error_t *
complete_directory(struct edit_baton *eb,
                   const char *local_abspath,
                   svn_boolean_t is_root_dir,
                   apr_pool_t *pool)
{
  const apr_array_header_t *children;
  int i;
  apr_pool_t *iterpool;

  /* If this is the root directory and there is a target, we can't
     mark this directory complete. */
  if (is_root_dir && *eb->target_basename != '\0')
    {
      /* ### obsolete comment?
         Before we can finish, we may need to clear the exclude flag for
         target. Also give a chance to the target that is explicitly pulled
         in. */
      svn_wc__db_kind_t kind;
      svn_wc__db_status_t status;
      svn_error_t *err;

      SVN_ERR_ASSERT(strcmp(local_abspath, eb->anchor_abspath) == 0);

      /* Note: we are fetching information about the *target*, not self.
         There is no guarantee that the target has a BASE node. Two examples:

           1. the node was present, but the update deleted it
           2. the node was not present in BASE, but locally-added, and the
              update did not create a new BASE node "under" the local-add.

         If there is no BASE node for the target, then we certainly don't
         have to worry about removing it.  */
      err = svn_wc__db_base_get_info(&status, &kind, NULL,
                                     NULL, NULL, NULL,
                                     NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL, NULL, NULL,
                                     eb->db, eb->target_abspath,
                                     pool, pool);
      if (err)
        {
          if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
            return svn_error_return(err);

          svn_error_clear(err);
          return SVN_NO_ERROR;
        }

      if (status == svn_wc__db_status_excluded)
        {
          /* ### obsolete comment?
             There is a small chance that the target is gone in the
             repository.  If so, we should get rid of the entry now. */

          if (kind == svn_wc__db_kind_dir &&
              svn_wc__adm_missing(eb->db, eb->target_abspath, pool))
            {
              /* ### obsolete comment?
               * Still passing NULL for THEIR_URL. A case where THEIR_URL
               * is needed in this call is rare or even non-existant.
               * ### TODO: Construct a proper THEIR_URL anyway. See also
               * NULL handling code in do_entry_deletion(). */
              SVN_ERR(do_entry_deletion(eb, eb->target_abspath,
                                        NULL, FALSE, pool));
            }
        }

      return SVN_NO_ERROR;
    }

  iterpool = svn_pool_create(pool);

  /* Mark THIS_DIR complete. */
  SVN_ERR(svn_wc__db_temp_op_set_base_incomplete(eb->db, local_abspath, FALSE,
                                                 iterpool));

  if (eb->depth_is_sticky)
    {
      svn_depth_t depth;

      /* ### obsolete comment?
         ### We should specifically check BASE_NODE here and then only remove
             the BASE_NODE if there is a WORKING_NODE. */

      SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       NULL, &depth, NULL, NULL, NULL, NULL,
                                       eb->db, local_abspath,
                                       iterpool, iterpool));

      if (depth != eb->requested_depth)
        {
          /* After a depth upgrade the entry must reflect the new depth.
             Upgrading to infinity changes the depth of *all* directories,
             upgrading to something else only changes the target. */

          if (eb->requested_depth == svn_depth_infinity
              || (strcmp(local_abspath, eb->target_abspath) == 0
                  && eb->requested_depth > depth))
            {
              SVN_ERR(svn_wc__db_temp_op_set_dir_depth(eb->db,
                                                       local_abspath,
                                                       eb->requested_depth,
                                                       iterpool));
            }
        }
    }

  /* Remove any deleted or missing entries. */

  SVN_ERR(svn_wc__db_base_get_children(&children, eb->db, local_abspath,
                                       pool, iterpool));
  for (i = 0; i < children->nelts; i++)
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);
      const char *node_abspath;
      svn_wc__db_status_t status;
      svn_wc__db_kind_t kind;
      svn_revnum_t revnum;
      svn_error_t *err;

      svn_pool_clear(iterpool);

      node_abspath = svn_dirent_join(local_abspath, name, iterpool);

#ifndef SVN_WC__SINGLE_DB
      /* ### there is an edge case that we can run into right now: this
         ### dir can have a "subdir" node in the BASE_NODE, but the
         ### actual subdir does NOT have a record.
         ###
         ### in particular, copy_tests 21 and schedule_tests 10 can create
         ### this situation. in short: the subdir is rm'd on the disk, and
         ### a deletion of that directory is committed. a local-add then
         ### reintroduces the directory and metadata (within WORKING).
         ### before or after an update, the parent dir has the "subdir"
         ### BASE_NODE and it is missing in the child. asking for the BASE
         ### won't return status_obstructed since there is a true subdir
         ### there now.
         ###
         ### at some point in the control flow, we should have removed
         ### the "subdir" record. maybe there is a good place to remove
         ### that record (or wait for single-dir). for now, we can correct
         ### it when we detect it.  */
#endif
      err = svn_wc__db_base_get_info(&status, &kind, &revnum,
                                     NULL, NULL, NULL,
                                     NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL, NULL, NULL,
                                     eb->db, node_abspath,
                                     iterpool, iterpool);
#ifdef SVN_WC__SINGLE_DB
      SVN_ERR(err);
#else
      if (err)
        {
          if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
            return svn_error_return(err);

          svn_error_clear(err);

          SVN_ERR(svn_wc__db_temp_remove_subdir_record(eb->db, node_abspath,
                                                       iterpool));
          continue;
        }
#endif

      /* ### obsolete comment?
         Any entry still marked as deleted (and not schedule add) can now
         be removed -- if it wasn't undeleted by the update, then it
         shouldn't stay in the updated working set.  Schedule add items
         should remain.

         An absent entry might have been reconfirmed as absent, and the way
         we can tell is by looking at its revision number: a revision
         number different from the target revision of the update means the
         update never mentioned the item, so the entry should be
         removed. */
      if (status == svn_wc__db_status_not_present)
        {
          /* "Usually", not_present nodes indicate that an 'svn delete' was
           * committed and its parent has not been updated yet. We have just
           * updated the parent and so the not_present BASE_NODE should go
           * away.
           * However, not_present can also mean that 'update' wanted to add a
           * node and found an unversioned obstruction at that path. We don't
           * want to remove such not_present state, so check if there is a
           * tree conflict flagged against an unversioned node and leave the
           * BASE_NODE alone if so.
           * Note that add_file() automatically fixes such an
           * added-not_present node when it finds the obstruction gone. */
          const svn_wc_conflict_description2_t *tree_conflict;
          SVN_ERR(svn_wc__get_tree_conflict(&tree_conflict,
                                            eb->wc_ctx,
                                            node_abspath,
                                            iterpool, iterpool));
          if (!tree_conflict
              || tree_conflict->reason != svn_wc_conflict_reason_unversioned)
            SVN_ERR(svn_wc__db_base_remove(eb->db, node_abspath, iterpool));
        }
      else if (status == svn_wc__db_status_absent
               && revnum != *eb->target_revision)
        {
          SVN_ERR(svn_wc__db_base_remove(eb->db, node_abspath, iterpool));
        }
      else if (kind == svn_wc__db_kind_dir
               && svn_wc__adm_missing(eb->db, node_abspath, iterpool)
               && status != svn_wc__db_status_absent)
        {
          SVN_ERR(svn_wc__db_temp_op_remove_entry(eb->db, node_abspath,
                                                  iterpool));

          do_notification(eb, node_abspath, svn_wc_notify_update_delete,
                          (kind == svn_wc__db_kind_dir)
                            ? svn_node_dir
                            : svn_node_file,
                          iterpool);
        }
    }

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}



/* Decrement the bump_dir_info's reference count. If it hits zero,
   then this directory is "done". This means it is safe to remove the
   'incomplete' flag attached to the THIS_DIR entry.

   In addition, when the directory is "done", we loop onto the parent's
   bump information to possibly mark it as done, too.
*/
static svn_error_t *
maybe_bump_dir_info(struct edit_baton *eb,
                    struct bump_dir_info *bdi,
                    apr_pool_t *pool)
{
  /* Keep moving up the tree of directories until we run out of parents,
     or a directory is not yet "done".  */
  while (bdi != NULL)
    {
      if (--bdi->ref_count > 0)
        return SVN_NO_ERROR;    /* directory isn't done yet */

      /* The refcount is zero, so we remove any 'dead' entries from
         the directory and mark it 'complete'.  */
      if (! bdi->skipped)
        SVN_ERR(complete_directory(eb, bdi->local_abspath,
                                   bdi->parent == NULL, pool));
      bdi = bdi->parent;
    }
  /* we exited the for loop because there are no more parents */

  return SVN_NO_ERROR;
}

struct file_baton
{
  /* Pool specific to this file_baton. */
  apr_pool_t *pool;

  /* Name of this file (its entry in the directory). */
  const char *name;

  /* Absolute path to this file */
  const char *local_abspath;

  /* The repository relative path this file will correspond to. */
  const char *new_relpath;

  /* The revision of the file before updating */
  svn_revnum_t old_revision;

  /* The global edit baton. */
  struct edit_baton *edit_baton;

  /* The parent directory of this file. */
  struct dir_baton *dir_baton;

  /* Set if updates to this directory are skipped */
  svn_boolean_t skip_this;

  /* Set if there was a previous notification  */
  svn_boolean_t already_notified;

  /* Set if this file is new. */
  svn_boolean_t adding_file;

  /* Set if this file is new with history. */
  svn_boolean_t added_with_history;

  /* Set if an unversioned file of the same name already existed in
     this directory. */
  svn_boolean_t obstruction_found;

  /* Set if a file of the same name already exists and is
     scheduled for addition without history. */
  svn_boolean_t add_existed;

  /* Set if this file is locally deleted or is being added
     within a locally deleted tree. */
  svn_boolean_t deleted;

  /* If there are file content changes, these are the checksums of the
     resulting new text base, which is in the pristine store, else NULL. */
  svn_checksum_t *new_text_base_md5_checksum;
  svn_checksum_t *new_text_base_sha1_checksum;

  /* If this file was added with history, these are the checksums of the
     copy-from text base, which is in the pristine store, else NULL. */
  svn_checksum_t *copied_text_base_md5_checksum;
  svn_checksum_t *copied_text_base_sha1_checksum;

  /* If this file was added with history, and the copyfrom had local
     mods, this is the path to a copy of the user's version with local
     mods (in the temporary area). */
  const char *copied_working_text;

  /* If this file was added with history, this hash contains the base
     properties of the copied file. */
  apr_hash_t *copied_base_props;

  /* If this file was added with history, this hash contains the working
     properties of the copied file. */
  apr_hash_t *copied_working_props;

  /* Set if we've received an apply_textdelta for this file. */
  svn_boolean_t received_textdelta;

  /* An array of svn_prop_t structures, representing all the property
     changes to be applied to this file.  Once a file baton is
     initialized, this is never NULL, but it may have zero elements.  */
  apr_array_header_t *propchanges;

  /* The last-changed-date of the file.  This is actually a property
     that comes through as an 'entry prop', and will be used to set
     the working file's timestamp if it's added. 

     Will be NULL unless eb->use_commit_times is TRUE. */
  const char *last_changed_date;

  /* Bump information for the directory this file lives in */
  struct bump_dir_info *bump_info;

  /* This is set when there is an incoming add of a file/symlink node onto a
   * locally added node of different identity (add-vs-add tree conflict). */
  svn_boolean_t adding_base_under_local_add;

};


/* Make a new file baton in the provided POOL, with PB as the parent baton.
 * PATH is relative to the root of the edit. ADDING tells whether this file
 * is being added. */
static svn_error_t *
make_file_baton(struct file_baton **f_p,
                struct dir_baton *pb,
                const char *path,
                svn_boolean_t adding,
                apr_pool_t *scratch_pool)
{
  apr_pool_t *file_pool = svn_pool_create(pb->pool);

  struct file_baton *f = apr_pcalloc(file_pool, sizeof(*f));

  SVN_ERR_ASSERT(path);

  /* Make the file's on-disk name. */
  f->name = svn_dirent_basename(path, file_pool);
  f->old_revision = SVN_INVALID_REVNUM;
  f->local_abspath = svn_dirent_join(pb->local_abspath, f->name, file_pool);

  /* Figure out the new_URL for this file. */
  if (pb->edit_baton->switch_relpath)
    f->new_relpath = svn_relpath_join(pb->new_relpath, f->name, file_pool);
  else
    f->new_relpath = node_get_relpath_ignore_errors(pb->edit_baton->db,
                                                    f->local_abspath,
                                                    file_pool, scratch_pool);

  /* ### why the complicated logic above. isn't it always this way?
     ### file externals are probably special/different?  */
  if (f->new_relpath == NULL)
    f->new_relpath = svn_relpath_join(pb->new_relpath, f->name, file_pool);

  f->pool              = file_pool;
  f->edit_baton        = pb->edit_baton;
  f->propchanges       = apr_array_make(file_pool, 1, sizeof(svn_prop_t));
  f->bump_info         = pb->bump_info;
  f->adding_file       = adding;
  f->obstruction_found = FALSE;
  f->add_existed       = FALSE;
  f->deleted           = FALSE;
  f->dir_baton         = pb;

  /* the directory's bump info has one more referer now */
  ++f->bump_info->ref_count;

  *f_p = f;
  return SVN_NO_ERROR;
}


/* Handle the next delta window of the file described by BATON.  If it is
 * the end (WINDOW == NULL), then check the checksum, store the text in the
 * pristine store and write its details into BATON->fb->new_text_base_*. */
static svn_error_t *
window_handler(svn_txdelta_window_t *window, void *baton)
{
  struct handler_baton *hb = baton;
  struct file_baton *fb = hb->fb;
  svn_wc__db_t *db = fb->edit_baton->db;
  svn_error_t *err;

  /* Apply this window.  We may be done at that point.  */
  err = hb->apply_handler(window, hb->apply_baton);
  if (window != NULL && !err)
    return SVN_NO_ERROR;

  if (hb->expected_source_md5_checksum)
    {
      /* Close the stream to calculate HB->actual_source_md5_checksum. */
      svn_error_t *err2 = svn_stream_close(hb->source_checksum_stream);

      if (!err2 && !svn_checksum_match(hb->expected_source_md5_checksum,
                                       hb->actual_source_md5_checksum))
        {
          err = svn_error_createf(SVN_ERR_WC_CORRUPT_TEXT_BASE, err,
                    _("Checksum mismatch while updating '%s':\n"
                      "   expected:  %s\n"
                      "     actual:  %s\n"),
                    svn_dirent_local_style(fb->local_abspath, hb->pool),
                    svn_checksum_to_cstring(hb->expected_source_md5_checksum,
                                            hb->pool),
                    svn_checksum_to_cstring(hb->actual_source_md5_checksum,
                                            hb->pool));
        }

      err = svn_error_compose_create(err, err2);
    }

  if (err)
    {
      /* We failed to apply the delta; clean up the temporary file.  */
      svn_error_clear(svn_io_remove_file2(hb->new_text_base_tmp_abspath, TRUE,
                                          hb->pool));
    }
  else
    {
      /* Tell the file baton about the new text base's checksums. */
      fb->new_text_base_md5_checksum =
        svn_checksum__from_digest(hb->new_text_base_md5_digest,
                                  svn_checksum_md5, fb->pool);
      fb->new_text_base_sha1_checksum =
        svn_checksum_dup(hb->new_text_base_sha1_checksum, fb->pool);

      /* Store the new pristine text in the pristine store now.  Later, in a
         single transaction we will update the BASE_NODE to include a
         reference to this pristine text's checksum. */
      SVN_ERR(svn_wc__db_pristine_install(db, hb->new_text_base_tmp_abspath,
                                          fb->new_text_base_sha1_checksum,
                                          fb->new_text_base_md5_checksum,
                                          hb->pool));
    }

  svn_pool_destroy(hb->pool);

  return err;
}


/* Prepare directory for dir_baton DB for updating or checking out.
 * Give it depth DEPTH.
 *
 * If the path already exists, but is not a working copy for
 * ANCESTOR_URL and ANCESTOR_REVISION, then an error will be returned.
 */
static svn_error_t *
prep_directory(struct dir_baton *db,
               const char *ancestor_url,
               svn_revnum_t ancestor_revision,
               apr_pool_t *pool)
{
  const char *dir_abspath;
#ifndef SINGLE_DB
  const char *repos_root;
  svn_boolean_t locked_here;
#endif

  dir_abspath = db->local_abspath;

  /* Make sure the directory exists. */
  SVN_ERR(svn_wc__ensure_directory(dir_abspath, pool));

#ifndef SINGLE_DB
  /* Use the repository root of the anchor, but only if it actually is an
     ancestor of the URL of this directory. */
  if (svn_uri_is_ancestor(db->edit_baton->repos_root, ancestor_url))
    repos_root = db->edit_baton->repos_root;
  else
    repos_root = NULL;

  /* Make sure it's the right working copy, either by creating it so,
     or by checking that it is so already. */
  SVN_ERR(svn_wc__internal_ensure_adm(db->edit_baton->db, dir_abspath,
                                      ancestor_url, repos_root,
                                      db->edit_baton->repos_uuid,
                                      ancestor_revision,
                                      db->ambient_depth, pool));

  SVN_ERR(svn_wc_locked2(&locked_here, NULL, db->edit_baton->wc_ctx,
                         dir_abspath, pool));
  if (!locked_here)
    /* Recursive lock release on parent will release this lock. */
    SVN_ERR(svn_wc__acquire_write_lock(NULL, db->edit_baton->wc_ctx,
                                       dir_abspath, FALSE, pool, pool));
#endif

  return SVN_NO_ERROR;
}


/* Find the last-change info within ENTRY_PROPS, and return then in the
   CHANGED_* parameters. Each parameter will be initialized to its "none"
   value, and will contain the relavent info if found.

   CHANGED_AUTHOR will be allocated in RESULT_POOL. SCRATCH_POOL will be
   used for some temporary allocations.
*/
static svn_error_t *
accumulate_last_change(svn_revnum_t *changed_rev,
                       apr_time_t *changed_date,
                       const char **changed_author,
                       svn_wc__db_t *db,
                       const char *local_abspath,
                       const apr_array_header_t *entry_props,
                       apr_pool_t *scratch_pool,
                       apr_pool_t *result_pool)
{
  int i;

  *changed_rev = SVN_INVALID_REVNUM;
  *changed_date = 0;
  *changed_author = NULL;

  for (i = 0; i < entry_props->nelts; ++i)
    {
      const svn_prop_t *prop = &APR_ARRAY_IDX(entry_props, i, svn_prop_t);

      /* A prop value of NULL means the information was not
         available.  We don't remove this field from the entries
         file; we have convention just leave it empty.  So let's
         just skip those entry props that have no values. */
      if (! prop->value)
        continue;

      if (! strcmp(prop->name, SVN_PROP_ENTRY_LAST_AUTHOR))
        *changed_author = apr_pstrdup(result_pool, prop->value->data);
      else if (! strcmp(prop->name, SVN_PROP_ENTRY_COMMITTED_REV))
        *changed_rev = SVN_STR_TO_REV(prop->value->data);
      else if (! strcmp(prop->name, SVN_PROP_ENTRY_COMMITTED_DATE))
        SVN_ERR(svn_time_from_cstring(changed_date, prop->value->data,
                                      scratch_pool));

      /* Starting with Subversion 1.7 we ignore the SVN_PROP_ENTRY_UUID
         property here. */
    }

  return SVN_NO_ERROR;
}


/* Check that when ADD_PATH is joined to BASE_PATH, the resulting path
 * is still under BASE_PATH in the local filesystem.  If not, return
 * SVN_ERR_WC_OBSTRUCTED_UPDATE; else return success.
 *
 * This is to prevent the situation where the repository contains,
 * say, "..\nastyfile".  Although that's perfectly legal on some
 * systems, when checked out onto Win32 it would cause "nastyfile" to
 * be created in the parent of the current edit directory.
 *
 * (http://cve.mitre.org/cgi-bin/cvename.cgi?name=2007-3846)
 */
static svn_error_t *
check_path_under_root(const char *base_path,
                      const char *add_path,
                      apr_pool_t *pool)
{
  const char *full_path;
  svn_boolean_t under_root;

  SVN_ERR(svn_dirent_is_under_root(&under_root, &full_path, base_path, add_path, pool));

  if (! under_root)
    {
      return svn_error_createf(
          SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
         _("Path '%s' is not in the working copy"),
         /* Not using full_path here because it might be NULL or
            undefined, since apr_filepath_merge() returned error.
            (Pity we can't pass NULL for &full_path in the first place,
            but the APR docs don't bless that.) */
         svn_dirent_local_style(svn_dirent_join(base_path, add_path, pool),
                                pool));
    }

  return SVN_NO_ERROR;
}


/*** The callbacks we'll plug into an svn_delta_editor_t structure. ***/

/* An svn_delta_editor_t function. */
static svn_error_t *
set_target_revision(void *edit_baton,
                    svn_revnum_t target_revision,
                    apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;

  /* Stashing a target_revision in the baton */
  *(eb->target_revision) = target_revision;
  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
open_root(void *edit_baton,
          svn_revnum_t base_revision, /* This is ignored in co */
          apr_pool_t *pool,
          void **dir_baton)
{
  struct edit_baton *eb = edit_baton;
  struct dir_baton *db;
  svn_boolean_t already_conflicted;
  svn_wc__db_kind_t kind;
  svn_error_t *err;

  /* Note that something interesting is actually happening in this
     edit run. */
  eb->root_opened = TRUE;

  SVN_ERR(make_dir_baton(&db, NULL, eb, NULL, FALSE, pool));
  *dir_baton = db;

  SVN_ERR(svn_wc__db_read_kind(&kind, eb->db, db->local_abspath, TRUE, pool));

  if (kind == svn_wc__db_kind_dir)
    {
      err = already_in_a_tree_conflict(&already_conflicted, eb->db,
                                       db->local_abspath, pool);

      if (err && err->apr_err == SVN_ERR_WC_MISSING)
        {
          svn_error_clear(err);
          already_conflicted = FALSE;
        }
      else
        SVN_ERR(err);
    }
  else
    already_conflicted = FALSE;

  if (already_conflicted)
    {
      db->skip_this = TRUE;
      db->skip_descendants = TRUE;
      db->already_notified = TRUE;
      db->bump_info->skipped = TRUE;

      /* Notify that we skipped the target, while we actually skipped
         the anchor */
      do_notification(eb, eb->target_abspath, svn_node_unknown,
                      svn_wc_notify_skip, pool);

      return SVN_NO_ERROR;
    }

  if (! *eb->target_basename)
    {
      /* For an update with a NULL target, this is equivalent to open_dir(): */
      svn_depth_t depth;
      svn_wc__db_status_t status;

      /* Read the depth from the entry. */
      SVN_ERR(svn_wc__db_base_get_info(&status, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, &depth, NULL, NULL, NULL,
                                   NULL, eb->db, db->local_abspath, pool, pool));
      db->ambient_depth = depth;
      db->was_incomplete = (status == svn_wc__db_status_incomplete);

      /* ### TODO: Skip if inside a conflicted tree. */

      SVN_ERR(svn_wc__db_temp_op_start_directory_update(eb->db,
                                                        db->local_abspath,
                                                        db->new_relpath,
                                                        *eb->target_revision,
                                                        pool));
    }

  return SVN_NO_ERROR;
}


/* Helper for delete_entry() and do_entry_deletion().

   If the error chain ERR contains evidence that a local mod was left
   (an SVN_ERR_WC_LEFT_LOCAL_MOD error), clear ERR.  Otherwise, return ERR.
*/
static svn_error_t *
leftmod_error_chain(svn_error_t *err)
{
  svn_error_t *tmp_err;

  if (! err)
    return SVN_NO_ERROR;

  /* Advance TMP_ERR to the part of the error chain that reveals that
     a local mod was left, or to the NULL end of the chain. */
  for (tmp_err = err; tmp_err; tmp_err = tmp_err->child)
    if (tmp_err->apr_err == SVN_ERR_WC_LEFT_LOCAL_MOD)
      {
        /* We just found a "left a local mod" error, so tolerate it
           and clear the whole error. In that case we continue with
           modified files left on the disk. */
        svn_error_clear(err);
        return SVN_NO_ERROR;
      }

  /* Otherwise, we just return our top-most error. */
  return err;
}


/* ===================================================================== */
/* Checking for local modifications. */

/* Set *MODIFIED to true iff the item described by (LOCAL_ABSPATH, KIND)
 * has local modifications. For a file, this means text mods or property mods.
 * For a directory, this means property mods.
 *
 * Use SCRATCH_POOL for temporary allocations.
 */
static svn_error_t *
entry_has_local_mods(svn_boolean_t *modified,
                     svn_wc__db_t *db,
                     const char *local_abspath,
                     svn_wc__db_kind_t kind,
                     apr_pool_t *scratch_pool)
{
  svn_boolean_t text_modified;
  svn_boolean_t props_modified;

  /* Check for text modifications */
  if (kind == svn_wc__db_kind_file
      || kind == svn_wc__db_kind_symlink)
    SVN_ERR(svn_wc__internal_text_modified_p(&text_modified, db, local_abspath,
                                             FALSE, TRUE, scratch_pool));
  else
    text_modified = FALSE;

  /* Check for property modifications */
  SVN_ERR(svn_wc__props_modified(&props_modified, db, local_abspath,
                                 scratch_pool));

  *modified = (text_modified || props_modified);

  return SVN_NO_ERROR;
}

/* A baton for use with modcheck_found_entry(). */
typedef struct modcheck_baton_t {
  svn_wc__db_t *db;         /* wc_db to access nodes */
  svn_boolean_t found_mod;  /* whether a modification has been found */
  svn_boolean_t all_edits_are_deletes;  /* If all the mods found, if any,
                                          were deletes.  If FOUND_MOD is false
                                          then this field has no meaning. */
} modcheck_baton_t;

/* */
static svn_error_t *
modcheck_found_node(const char *local_abspath,
                    void *walk_baton,
                    apr_pool_t *scratch_pool)
{
  modcheck_baton_t *baton = walk_baton;
  svn_wc__db_kind_t kind;
  svn_wc__db_status_t status;
  svn_boolean_t modified;

  SVN_ERR(svn_wc__db_read_info(&status, &kind, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL,
                               baton->db, local_abspath, scratch_pool,
                               scratch_pool));

  if (status != svn_wc__db_status_normal)
    modified = TRUE;
  else
    SVN_ERR(entry_has_local_mods(&modified, baton->db, local_abspath,
                                 kind, scratch_pool));

  if (modified)
    {
      baton->found_mod = TRUE;
      if (status != svn_wc__db_status_deleted)
        baton->all_edits_are_deletes = FALSE;
    }

  return SVN_NO_ERROR;
}


/* Set *MODIFIED to true iff there are any local modifications within the
 * tree rooted at LOCAL_ABSPATH, using DB. If *MODIFIED
 * is set to true and all the local modifications were deletes then set
 * *ALL_EDITS_ARE_DELETES to true, set it to false otherwise.  LOCAL_ABSPATH
 * may be a file or a directory. */
static svn_error_t *
tree_has_local_mods(svn_boolean_t *modified,
                    svn_boolean_t *all_edits_are_deletes,
                    svn_wc__db_t *db,
                    const char *local_abspath,
                    svn_cancel_func_t cancel_func,
                    void *cancel_baton,
                    apr_pool_t *pool)
{
  modcheck_baton_t modcheck_baton = { NULL, FALSE, TRUE };

  modcheck_baton.db = db;

  /* Walk the WC tree to its full depth, looking for any local modifications.
   * If it's a "sparse" directory, that's OK: there can be no local mods in
   * the pieces that aren't present in the WC. */

  SVN_ERR(svn_wc__internal_walk_children(db, local_abspath,
                                         FALSE /* show_hidden */,
                                         modcheck_found_node, &modcheck_baton,
                                         svn_depth_infinity, cancel_func,
                                         cancel_baton, pool));

  *modified = modcheck_baton.found_mod;
  *all_edits_are_deletes = modcheck_baton.all_edits_are_deletes;
  return SVN_NO_ERROR;
}


/* Indicates an unset svn_wc_conflict_reason_t. */
#define SVN_WC_CONFLICT_REASON_NONE (svn_wc_conflict_reason_t)(-1)

/* Create a tree conflict struct.
 *
 * The REASON is stored directly in the tree conflict info.
 *
 * All temporary allocactions are be made in SCRATCH_POOL, while allocations
 * needed for the returned conflict struct are made in RESULT_POOL.
 *
 * All other parameters are identical to and described by
 * check_tree_conflict(), with the slight modification that this function
 * relies on the reason passed in REASON instead of actively looking for one. */
static svn_error_t *
create_tree_conflict(svn_wc_conflict_description2_t **pconflict,
                     struct edit_baton *eb,
                     const char *local_abspath,
                     svn_wc_conflict_reason_t reason,
                     svn_wc_conflict_action_t action,
                     svn_node_kind_t their_node_kind,
                     const char *their_relpath,
                     apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  const char *repos_root_url = NULL;
  const char *left_repos_relpath;
  svn_revnum_t left_revision;
  svn_node_kind_t left_kind;
  const char *right_repos_relpath;
  const char *added_repos_relpath = NULL;
  svn_node_kind_t conflict_node_kind;
  svn_wc_conflict_version_t *src_left_version;
  svn_wc_conflict_version_t *src_right_version;

  *pconflict = NULL;

  SVN_ERR_ASSERT(reason != SVN_WC_CONFLICT_REASON_NONE);

  /* Get the source-left information, i.e. the local state of the node
   * before any changes were made to the working copy, i.e. the state the
   * node would have if it was reverted. */
  if (reason == svn_wc_conflict_reason_added)
    {
      svn_wc__db_status_t added_status;

      /* ###TODO: It would be nice to tell the user at which URL and
       * ### revision source-left was empty, which could be quite difficult
       * ### to code, and is a slight theoretical leap of the svn mind.
       * ### Update should show
       * ###   URL: svn_wc__db_scan_addition( &repos_relpath )
       * ###   REV: The base revision of the parent of before this update
       * ###        started
       * ###        ### BUT what if parent was updated/switched away with
       * ###        ### depth=empty after this node was added?
       * ### Switch should show
       * ###   URL: scan_addition URL of before this switch started
       * ###   REV: same as above */

      /* In case of a local addition, source-left is non-existent / empty. */
      left_kind = svn_node_none;
      left_revision = SVN_INVALID_REVNUM;
      left_repos_relpath = NULL;

      /* Still get the repository root needed by both 'update' and 'switch',
       * and the would-be repos_relpath needed to construct the source-right
       * in case of an 'update'. Check sanity while we're at it. */
      SVN_ERR(svn_wc__db_scan_addition(&added_status, NULL,
                                       &added_repos_relpath,
                                       &repos_root_url,
                                       NULL, NULL, NULL, NULL, NULL,
                                       eb->db, local_abspath,
                                       result_pool, scratch_pool));

      /* This better really be an added status. */
      SVN_ERR_ASSERT(added_status == svn_wc__db_status_added
                     || added_status == svn_wc__db_status_obstructed_add
                     || added_status == svn_wc__db_status_copied
                     || added_status == svn_wc__db_status_moved_here);
    }
  else if (reason == svn_wc_conflict_reason_unversioned)
    {
      /* Obstructed by an unversioned node. Source-left is
       * non-existent/empty. */
      left_kind = svn_node_none;
      left_revision = SVN_INVALID_REVNUM;
      left_repos_relpath = NULL;
      repos_root_url = eb->repos_root;
    }
  else
    {
      /* A BASE node should exist. */
      svn_wc__db_kind_t base_kind;

      /* If anything else shows up, then this assertion is probably naive
       * and that other case should also be handled. */
      SVN_ERR_ASSERT(reason == svn_wc_conflict_reason_edited
                     || reason == svn_wc_conflict_reason_deleted
                     || reason == svn_wc_conflict_reason_replaced
                     || reason == svn_wc_conflict_reason_obstructed);

      SVN_ERR(svn_wc__db_base_get_info(NULL, &base_kind,
                                       &left_revision,
                                       &left_repos_relpath,
                                       &repos_root_url,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL,
                                       eb->db,
                                       local_abspath,
                                       result_pool,
                                       scratch_pool));
      /* Translate the node kind. */
      if (base_kind == svn_wc__db_kind_file
          || base_kind == svn_wc__db_kind_symlink)
        left_kind = svn_node_file;
      else if (base_kind == svn_wc__db_kind_dir)
        left_kind = svn_node_dir;
      else
        SVN_ERR_MALFUNCTION();
    }

  SVN_ERR_ASSERT(strcmp(repos_root_url, eb->repos_root) == 0);

  /* Find the source-right information, i.e. the state in the repository
   * to which we would like to update. */
  if (eb->switch_relpath)
    {
      /* If this is a 'switch' operation, try to construct the switch
       * target's REPOS_RELPATH. */
      if (their_relpath != NULL)
        right_repos_relpath = their_relpath;
      else
        {
          /* The complete source-right URL is not available, but it
           * is somewhere below the SWITCH_URL. For now, just go
           * without it.
           * ### TODO: Construct a proper THEIR_URL in some of the
           * delete cases that still pass NULL for THEIR_URL when
           * calling this function. Do that on the caller's side. */
          right_repos_relpath = eb->switch_relpath;
          right_repos_relpath = apr_pstrcat(result_pool, right_repos_relpath,
                                            "_THIS_IS_INCOMPLETE", NULL);
        }
    }
  else
    {
      /* This is an 'update', so REPOS_RELPATH would be the same as for
       * source-left. However, we don't have a source-left for locally
       * added files. */
      right_repos_relpath = (reason == svn_wc_conflict_reason_added ?
                             added_repos_relpath : left_repos_relpath);
      if (! right_repos_relpath)
        right_repos_relpath = their_relpath;
    }

  SVN_ERR_ASSERT(right_repos_relpath != NULL);

  /* Determine PCONFLICT's overall node kind, which is not allowed to be
   * svn_node_none. We give it the source-right revision (THEIR_NODE_KIND)
   * -- unless source-right is deleted and hence == svn_node_none, in which
   * case we take it from source-left, which has to be the node kind that
   * was deleted. */
  conflict_node_kind = (action == svn_wc_conflict_action_delete ?
                        left_kind : their_node_kind);
  SVN_ERR_ASSERT(conflict_node_kind == svn_node_file
                 || conflict_node_kind == svn_node_dir);


  /* Construct the tree conflict info structs. */

  if (left_repos_relpath == NULL)
    /* A locally added or unversioned path in conflict with an incoming add.
     * Send an 'empty' left revision. */
    src_left_version = NULL;
  else
    src_left_version = svn_wc_conflict_version_create(repos_root_url,
                                                      left_repos_relpath,
                                                      left_revision,
                                                      left_kind,
                                                      result_pool);

  src_right_version = svn_wc_conflict_version_create(repos_root_url,
                                                     right_repos_relpath,
                                                     *eb->target_revision,
                                                     their_node_kind,
                                                     result_pool);

  *pconflict = svn_wc_conflict_description_create_tree2(
                   local_abspath, conflict_node_kind,
                   eb->switch_relpath ?
                     svn_wc_operation_switch : svn_wc_operation_update,
                   src_left_version, src_right_version, result_pool);
  (*pconflict)->action = action;
  (*pconflict)->reason = reason;

  return SVN_NO_ERROR;
}


/* Check whether the incoming change ACTION on FULL_PATH would conflict with
 * LOCAL_ABSPATH's scheduled change. If so, then raise a tree conflict with
 * LOCAL_ABSPATH as the victim.
 *
 * The edit baton EB gives information including whether the operation is
 * an update or a switch.
 *
 * If a tree conflict reason was found for the incoming action, the resulting
 * tree conflict info is returned in *PCONFLICT. PCONFLICT must be non-NULL,
 * while *PCONFLICT is always overwritten.
 *
 * THEIR_NODE_KIND should be the node kind reflected by the incoming edit
 * function. E.g. dir_opened() should pass svn_node_dir, etc.
 * In some cases of delete, svn_node_none may be used here.
 *
 * THEIR_RELPATH should be the involved node's repository-relative path on the
 * source-right side, the side that the target should become after the update.
 * Simply put, that's the URL obtained from the node's dir_baton->new_relpath
 * or file_baton->new_relpath (but it's more complex for a delete).
 *
 * All allocations are made in POOL.
 */
static svn_error_t *
check_tree_conflict(svn_wc_conflict_description2_t **pconflict,
                    struct edit_baton *eb,
                    const char *local_abspath,
                    svn_wc_conflict_action_t action,
                    svn_node_kind_t their_node_kind,
                    const char *their_relpath,
                    apr_pool_t *pool)
{
  svn_wc__db_status_t status;
  svn_wc__db_kind_t db_node_kind;
  svn_boolean_t have_base;
  svn_wc_conflict_reason_t reason = SVN_WC_CONFLICT_REASON_NONE;
  svn_boolean_t locally_replaced = FALSE;
  svn_boolean_t modified = FALSE;
  svn_boolean_t all_mods_are_deletes = FALSE;

  *pconflict = NULL;

  SVN_ERR(svn_wc__db_read_info(&status,
                               &db_node_kind,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL,
                               &have_base,
                               NULL, NULL, NULL,
                               eb->db,
                               local_abspath,
                               pool,
                               pool));

  /* Find out if there are any local changes to this node that may
   * be the "reason" of a tree-conflict with the incoming "action". */
  switch (status)
    {
      case svn_wc__db_status_added:
      case svn_wc__db_status_obstructed_add:
      case svn_wc__db_status_moved_here:
      case svn_wc__db_status_copied:
        /* Is it a replace? */
        if (have_base)
          {
            svn_wc__db_status_t base_status;
            SVN_ERR(svn_wc__db_base_get_info(&base_status, NULL, NULL,
                                             NULL, NULL, NULL, NULL, NULL,
                                             NULL, NULL, NULL, NULL, NULL,
                                             NULL, NULL,
                                             eb->db, local_abspath,
                                             pool,
                                             pool));
            if (base_status != svn_wc__db_status_not_present)
              locally_replaced = TRUE;
          }

        if (!locally_replaced)
          {
            /* The node is locally added, and it did not exist before.  This
             * is an 'update', so the local add can only conflict with an
             * incoming 'add'.  In fact, if we receive anything else than an
             * svn_wc_conflict_action_add (which includes 'added',
             * 'copied-here' and 'moved-here') during update on a node that
             * did not exist before, then something is very wrong.
             * Note that if there was no action on the node, this code
             * would not have been called in the first place. */
            SVN_ERR_ASSERT(action == svn_wc_conflict_action_add);

            reason = svn_wc_conflict_reason_added;
          }
        else
          {
            /* The node is locally replaced. */
            reason = svn_wc_conflict_reason_replaced;
          }
        break;


      case svn_wc__db_status_deleted:
      case svn_wc__db_status_obstructed_delete:
        /* The node is locally deleted. */
        reason = svn_wc_conflict_reason_deleted;
        break;

      case svn_wc__db_status_incomplete:
        /* We used svn_wc__db_read_info(), so 'incomplete' means
         * - there is no node in the WORKING tree
         * - a BASE node is known to exist
         * So the node exists and is essentially 'normal'. We still need to
         * check prop and text mods, and those checks will retrieve the
         * missing information (hopefully). */
      case svn_wc__db_status_obstructed:
        /* Tree-conflicts during update are only concerned with local
         * modifications. We can safely update BASE, disregarding the
         * obstruction. So let's treat this as normal. */
      case svn_wc__db_status_normal:
        if (action == svn_wc_conflict_action_edit)
          /* An edit onto a local edit or onto *no* local changes is no
           * tree-conflict. (It's possibly a text- or prop-conflict,
           * but we don't handle those here.) */
          return SVN_NO_ERROR;

        /* Check if the update wants to delete or replace a locally
         * modified node. */
        switch (db_node_kind)
          {
            case svn_wc__db_kind_file:
            case svn_wc__db_kind_symlink:
              all_mods_are_deletes = FALSE;
              SVN_ERR(entry_has_local_mods(&modified, eb->db, local_abspath,
                                           db_node_kind, pool));
              break;

            case svn_wc__db_kind_dir:
              /* We must detect deep modifications in a directory tree,
               * but the update editor will not visit the subdirectories
               * of a directory that it wants to delete.  Therefore, we
               * need to start a separate crawl here. */
              if (!svn_wc__adm_missing(eb->db, local_abspath, pool))
                SVN_ERR(tree_has_local_mods(&modified, &all_mods_are_deletes,
                                            eb->db, local_abspath,
                                            eb->cancel_func, eb->cancel_baton,
                                            pool));
              break;

            default:
              /* It's supposed to be in 'normal' status. So how can it be
               * neither file nor folder? */
              SVN_ERR_MALFUNCTION();
              break;
          }

        if (modified)
          {
            if (all_mods_are_deletes)
              reason = svn_wc_conflict_reason_deleted;
            else
              reason = svn_wc_conflict_reason_edited;
          }
        break;

      case svn_wc__db_status_absent:
        /* Not allowed to view the node. Not allowed to report tree
         * conflicts. */
      case svn_wc__db_status_excluded:
        /* Locally marked as excluded. No conflicts wanted. */
      case svn_wc__db_status_not_present:
        /* A committed delete (but parent not updated). The delete is
           committed, so no conflict possible during update. */
        return SVN_NO_ERROR;

      case svn_wc__db_status_base_deleted:
        /* An internal status. Should never show up here. */
        SVN_ERR_MALFUNCTION();
        break;

    }

  if (reason == SVN_WC_CONFLICT_REASON_NONE)
    /* No conflict with the current action. */
    return SVN_NO_ERROR;


  /* Sanity checks. Note that if there was no action on the node, this function
   * would not have been called in the first place.*/
  if (reason == svn_wc_conflict_reason_edited
      || reason == svn_wc_conflict_reason_deleted
      || reason == svn_wc_conflict_reason_replaced)
    /* When the node existed before (it was locally deleted, replaced or
     * edited), then 'update' cannot add it "again". So it can only send
     * _action_edit, _delete or _replace. */
    SVN_ERR_ASSERT(action == svn_wc_conflict_action_edit
                   || action == svn_wc_conflict_action_delete
                   || action == svn_wc_conflict_action_replace);
  else if (reason == svn_wc_conflict_reason_added)
    /* When the node did not exist before (it was locally added), then 'update'
     * cannot want to modify it in any way. It can only send _action_add. */
    SVN_ERR_ASSERT(action == svn_wc_conflict_action_add);


  /* A conflict was detected. Append log commands to the log accumulator
   * to record it. */
  return svn_error_return(create_tree_conflict(pconflict, eb, local_abspath,
                                               reason, action, their_node_kind,
                                               their_relpath, pool, pool));
}


/* If LOCAL_ABSPATH is inside a conflicted tree, set *CONFLICTED to TRUE,
 * Otherwise set *CONFLICTED to FALSE.  Use SCRATCH_POOL for temporary
 * allocations.
 *
 * The search begins at the working copy root, returning the first
 * ("highest") tree conflict victim, which may be LOCAL_ABSPATH itself.
 *
 * ### this function MAY not cache 'entries' (lack of access batons), so
 * ### it will re-read the entries file for ancestor directories for
 * ### every path encountered during the update. however, the DB param
 * ### may have directories with access batons, holding the entries. it
 * ### depends on whether the update was done from the wcroot or not.
 */
static svn_error_t *
already_in_a_tree_conflict(svn_boolean_t *conflicted,
                           svn_wc__db_t *db,
                           const char *local_abspath,
                           apr_pool_t *scratch_pool)
{
  const char *ancestor_abspath = local_abspath;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  *conflicted = FALSE;

  while (TRUE)
    {
      svn_wc__db_kind_t kind;
      svn_boolean_t hidden;
      svn_boolean_t is_wc_root;
      svn_error_t *err;
      const svn_wc_conflict_description2_t *conflict;

      svn_pool_clear(iterpool);

      err = svn_wc__db_read_kind(&kind, db, ancestor_abspath, TRUE,
                                 iterpool);

      if (err)
        {
          if (! SVN_WC__ERR_IS_NOT_CURRENT_WC(err))
            return svn_error_return(err);

          svn_error_clear(err);
          break;
        }

      if (kind == svn_wc__db_kind_unknown)
        break;

      SVN_ERR(svn_wc__db_node_hidden(&hidden, db, ancestor_abspath, iterpool));

      if (hidden)
        break;

      SVN_ERR(svn_wc__db_op_read_tree_conflict(&conflict, db, ancestor_abspath,
                                               iterpool, iterpool));

      if (conflict != NULL)
        {
          *conflicted = TRUE;
          break;
        }

      if (svn_dirent_is_root(ancestor_abspath, strlen(ancestor_abspath)))
        break;

      err = svn_wc__check_wc_root(&is_wc_root, NULL, NULL,
                                  db, ancestor_abspath, iterpool);

      if (err
          && (err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND
              || err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY))
        {
          svn_error_clear(err);
          return SVN_NO_ERROR;
        }
      else
        SVN_ERR(err);

      ancestor_abspath = svn_dirent_dirname(ancestor_abspath, scratch_pool);
    }

  svn_pool_clear(iterpool);

  return SVN_NO_ERROR;
}

/* Temporary helper until the new conflict handling is in place */
static svn_error_t *
node_already_conflicted(svn_boolean_t *conflicted,
                        svn_wc__db_t *db,
                        const char *local_abspath,
                        apr_pool_t *scratch_pool)
{
  const apr_array_header_t *conflicts;
  int i;

  SVN_ERR(svn_wc__db_read_conflicts(&conflicts, db, local_abspath,
                                    scratch_pool, scratch_pool));

  *conflicted = FALSE;

  for (i = 0; i < conflicts->nelts; i++)
    {
      const svn_wc_conflict_description2_t *cd;
      cd = APR_ARRAY_IDX(conflicts, i, const svn_wc_conflict_description2_t *);

      if (cd->kind == svn_wc_conflict_kind_tree)
        {
          *conflicted = TRUE;
          return SVN_NO_ERROR;
        }
      else if (cd->kind == svn_wc_conflict_kind_property ||
               cd->kind == svn_wc_conflict_kind_text)
        {
          svn_boolean_t text_conflicted, prop_conflicted, tree_conflicted;
          SVN_ERR(svn_wc__internal_conflicted_p(&text_conflicted,
                                                &prop_conflicted,
                                                &tree_conflicted,
                                                db, local_abspath,
                                                scratch_pool));

          *conflicted = (text_conflicted || prop_conflicted
                            || tree_conflicted);
          return SVN_NO_ERROR;
        }
    }

  return SVN_NO_ERROR;
}


/* Delete PATH from its immediate parent PARENT_PATH, in the edit
 * represented by EB. PATH is relative to EB->anchor.
 * PARENT_PATH is relative to the current working directory.
 *
 * THEIR_RELPATH is the deleted node's repository relative path on the
 * source-right side, the side that the target should become after the
 * update. In other words, that's the new URL the node would have if it
 * were not deleted.
 *
 * Perform all allocations in POOL.
 */
static svn_error_t *
do_entry_deletion(struct edit_baton *eb,
                  const char *local_abspath,
                  const char *their_relpath,
                  svn_boolean_t in_deleted_and_tree_conflicted_subtree,
                  apr_pool_t *pool)
{
  svn_wc__db_kind_t kind;
  svn_boolean_t already_conflicted;
  svn_wc_conflict_description2_t *tree_conflict = NULL;
  const char *dir_abspath = svn_dirent_dirname(local_abspath, pool);
  svn_boolean_t hidden;
  svn_skel_t *work_item;

  SVN_ERR(svn_wc__db_read_kind(&kind, eb->db, local_abspath, FALSE, pool));

  /* Is this path a conflict victim? */
  SVN_ERR(node_already_conflicted(&already_conflicted, eb->db,
                                  local_abspath, pool));
  if (already_conflicted)
    {
      SVN_ERR(remember_skipped_tree(eb, local_abspath));

      /* ### TODO: Also print victim_path in the skip msg. */
      do_notification(eb, local_abspath, svn_node_unknown, svn_wc_notify_skip,
                      pool);

      return SVN_NO_ERROR;
    }

    /* Receive the remote removal of excluded/absent/not present node.
       Do not notify. */
  SVN_ERR(svn_wc__db_node_hidden(&hidden, eb->db, local_abspath, pool));
  if (hidden)
    {
      SVN_ERR(svn_wc__db_base_remove(eb->db, local_abspath, pool));

      if (strcmp(local_abspath, eb->target_abspath) == 0)
        eb->target_deleted = TRUE;

      return SVN_NO_ERROR;
    }

  /* Is this path the victim of a newly-discovered tree conflict?  If so,
   * remember it and notify the client. Then (if it was existing and
   * modified), re-schedule the node to be added back again, as a (modified)
   * copy of the previous base version.  */

  /* Check for conflicts only when we haven't already recorded
   * a tree-conflict on a parent node. */
  if (!in_deleted_and_tree_conflicted_subtree)
    SVN_ERR(check_tree_conflict(&tree_conflict, eb, local_abspath,
                                svn_wc_conflict_action_delete, svn_node_none,
                                their_relpath, pool));

  if (tree_conflict != NULL)
    {
      /* When we raise a tree conflict on a directory, we want to avoid
       * making any changes inside it. (Will an update ever try to make
       * further changes to or inside a directory it's just deleted?) */
      SVN_ERR(svn_wc__loggy_add_tree_conflict(&work_item, eb->db, dir_abspath,
                                              tree_conflict, pool));
      SVN_ERR(svn_wc__db_wq_add(eb->db, dir_abspath, work_item, pool));

      SVN_ERR(remember_skipped_tree(eb, local_abspath));

      do_notification(eb, local_abspath, svn_node_unknown,
                      svn_wc_notify_tree_conflict, pool);

      if (tree_conflict->reason == svn_wc_conflict_reason_edited)
        {
          /* The item exists locally and has some sort of local mod.
           * It no longer exists in the repository at its target URL@REV.
           * (### If its WC parent was not updated similarly, then it needs to
           * be marked 'deleted' in its WC parent.)
           * To prepare the "accept mine" resolution for the tree conflict,
           * we must schedule the existing content for re-addition as a copy
           * of what it was, but with its local modifications preserved. */

          /* Run the log in the parent dir, to record the tree conflict.
           * Do this before schedule_existing_item_for_re_add(), in case
           * that needs to modify the same entries. */
          SVN_ERR(svn_wc__wq_run(eb->db, dir_abspath,
                                 eb->cancel_func, eb->cancel_baton,
                                 pool));

          SVN_ERR(svn_wc__db_temp_op_make_copy(eb->db, local_abspath, TRUE,
                                               pool));

          return SVN_NO_ERROR;
        }
      else if (tree_conflict->reason == svn_wc_conflict_reason_deleted)
        {
          /* The item does not exist locally (except perhaps as a skeleton
           * directory tree) because it was already scheduled for delete.
           * We must complete the deletion, leaving the tree conflict info
           * as the only difference from a normal deletion. */

          /* Fall through to the normal "delete" code path. */
        }
      else if (tree_conflict->reason == svn_wc_conflict_reason_replaced)
        {
          /* The item was locally replaced with something else. We should
           * keep the existing item schedule-replace, but we also need to
           * update the BASE rev of the item to the revision we are updating
           * to. Otherwise, the replace cannot be committed because the item
           * is considered out-of-date, and it cannot be updated either because
           * we're here to do just that. */

          /* Run the log in the parent dir, to record the tree conflict.
           * Do this before schedule_existing_item_for_re_add(), in case
           * that needs to modify the same entries. */
          SVN_ERR(svn_wc__wq_run(eb->db, dir_abspath,
                                 eb->cancel_func, eb->cancel_baton,
                                 pool));

          SVN_ERR(svn_wc__db_temp_op_make_copy(eb->db, local_abspath, TRUE,
                                               pool));

          return SVN_NO_ERROR;
        }
      else
        SVN_ERR_MALFUNCTION();  /* other reasons are not expected here */
    }

  /* Issue a loggy command to delete the entry from version control and to
     delete it from disk if unmodified, but leave any modified files on disk
     unversioned.

     If the thing being deleted is the *target* of this update, then
     we need to recreate a 'deleted' entry, so that the parent can give
     accurate reports about itself in the future. */
  if (strcmp(local_abspath, eb->target_abspath) != 0)
    {
      /* Delete, and do not leave a not-present node.  */
      SVN_ERR(svn_wc__loggy_delete_entry(&work_item,
                                         eb->db, dir_abspath, local_abspath,
                                         SVN_INVALID_REVNUM,
                                         svn_wc__db_kind_unknown,
                                         pool));
      SVN_ERR(svn_wc__db_wq_add(eb->db, dir_abspath, work_item, pool));
    }
  else
    {
      /* Delete, leaving a not-present node.  */
      SVN_ERR(svn_wc__loggy_delete_entry(&work_item,
                                         eb->db, dir_abspath, local_abspath,
                                         *eb->target_revision,
                                         kind,
                                         pool));
      SVN_ERR(svn_wc__db_wq_add(eb->db, dir_abspath, work_item, pool));
      eb->target_deleted = TRUE;
    }

  if (eb->switch_relpath)
    {
      /* The SVN_WC__LOG_DELETE_ENTRY log item will cause
       * svn_wc_remove_from_revision_control() to be run.  But that
       * function checks whether the deletion target's URL is child of
       * its parent directory's URL, and if it's not, then the entry
       * in parent won't be deleted (because presumably the child
       * represents a disjoint working copy, i.e., it is a wc_root).
       *
       * However, during a switch this works against us, because by
       * the time we get here, the parent's URL has already been
       * changed.  So we manually remove the child from revision
       * control after the delete-entry item has been written in the
       * parent's log, but before it is run, so the only work left for
       * the log item is to remove the entry in the parent directory.
       */

      if (kind == svn_wc__db_kind_dir)
        {
          SVN_ERR(leftmod_error_chain(
                    svn_wc__internal_remove_from_revision_control(
                      eb->db,
                      local_abspath,
                      TRUE, /* destroy */
                      FALSE, /* instant error */
                      eb->cancel_func,
                      eb->cancel_baton,
                      pool)));
        }
    }

  /* Note: these two lines are duplicated in the tree-conflicts bail out
   * above. */
  SVN_ERR(svn_wc__wq_run(eb->db, dir_abspath,
                         eb->cancel_func, eb->cancel_baton,
                         pool));

  /* Notify. (If tree_conflict, we've already notified.) */
  if (tree_conflict == NULL)
    do_notification(eb, local_abspath, svn_node_unknown,
                    svn_wc_notify_update_delete, pool);

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
static svn_error_t *
delete_entry(const char *path,
             svn_revnum_t revision,
             void *parent_baton,
             apr_pool_t *pool)
{
  struct dir_baton *pb = parent_baton;
  const char *base = svn_relpath_basename(path, pool);
  const char *local_abspath;
  const char *their_relpath;

  local_abspath = svn_dirent_join(pb->local_abspath, base, pool);

  if (pb->skip_descendants)
    {
      if (!pb->skip_this)
        SVN_ERR(remember_skipped_tree(pb->edit_baton, local_abspath));

      return SVN_NO_ERROR;
    }

  SVN_ERR(check_path_under_root(pb->local_abspath, base, pool));

  their_relpath = svn_relpath_join(pb->new_relpath, base, pool);

  return do_entry_deletion(pb->edit_baton, local_abspath,
                           their_relpath,
                           pb->in_deleted_and_tree_conflicted_subtree,
                           pool);
}


/* An svn_delta_editor_t function. */
static svn_error_t *
add_directory(const char *path,
              void *parent_baton,
              const char *copyfrom_path,
              svn_revnum_t copyfrom_revision,
              apr_pool_t *pool,
              void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  struct dir_baton *db;
  svn_node_kind_t kind;
  svn_wc__db_status_t status;
  svn_wc__db_kind_t wc_kind;
  svn_boolean_t already_conflicted;
  svn_boolean_t versioned_locally_and_present;
  svn_wc_conflict_description2_t *tree_conflict = NULL;
  svn_error_t *err;

  /* Semantic check.  Either both "copyfrom" args are valid, or they're
     NULL and SVN_INVALID_REVNUM.  A mixture is illegal semantics. */
  SVN_ERR_ASSERT((copyfrom_path && SVN_IS_VALID_REVNUM(copyfrom_revision))
                 || (!copyfrom_path &&
                     !SVN_IS_VALID_REVNUM(copyfrom_revision)));
  if (copyfrom_path != NULL)
    {
      /* ### todo: for now, this editor doesn't know how to deal with
         copyfrom args.  Someday it will interpet them as an update
         optimization, and actually copy one part of the wc to another.
         Then it will recursively "normalize" all the ancestry in the
         copied tree.  Someday!

         Note from the future: if someday it does, we'll probably want
         to tweak libsvn_ra_neon/fetch.c:validate_element() to accept
         that an add-dir element can contain a delete-entry element
         (because the dir might be added with history).  Currently
         that combination will not validate.  See r30161, and see the
         thread in which this message appears:

      http://subversion.tigris.org/servlets/ReadMsg?list=dev&msgNo=136879
      From: "David Glasser" <glasser@davidglasser.net>
      To: "Karl Fogel" <kfogel@red-bean.com>, dev@subversion.tigris.org
      Cc: "Arfrever Frehtes Taifersar Arahesis" <arfrever.fta@gmail.com>,
          glasser@tigris.org
      Subject: Re: svn commit: r30161 - in trunk/subversion: \
               libsvn_ra_neon tests/cmdline
      Date: Fri, 4 Apr 2008 14:47:06 -0700
      Message-ID: <1ea387f60804041447q3aea0bbds10c2db3eacaf73e@mail.gmail.com>

      */
      return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                               _("Failed to add directory '%s': "
                                 "copyfrom arguments not yet supported"),
                               svn_dirent_local_style(path, pool));
    }

  SVN_ERR(make_dir_baton(&db, path, eb, pb, TRUE, pool));
  *child_baton = db;

  if (pb->skip_descendants)
    {
      if (!pb->skip_this)
        SVN_ERR(remember_skipped_tree(eb, db->local_abspath));

      db->skip_this = TRUE;
      db->skip_descendants = TRUE;
      db->already_notified = TRUE;

      return SVN_NO_ERROR;
    }

  SVN_ERR(check_path_under_root(pb->local_abspath, db->name, pool));

  if (strcmp(eb->target_abspath, db->local_abspath) == 0)
    {
      /* The target of the edit is being added, give it the requested
         depth of the edit (but convert svn_depth_unknown to
         svn_depth_infinity). */
      db->ambient_depth = (eb->requested_depth == svn_depth_unknown)
        ? svn_depth_infinity : eb->requested_depth;
    }
  else if (eb->requested_depth == svn_depth_immediates
           || (eb->requested_depth == svn_depth_unknown
               && pb->ambient_depth == svn_depth_immediates))
    {
      db->ambient_depth = svn_depth_empty;
    }
  else
    {
      db->ambient_depth = svn_depth_infinity;
    }

  /* It may not be named the same as the administrative directory. */
  if (svn_wc_is_adm_dir(db->name, pool))
    return svn_error_createf(
       SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
       _("Failed to add directory '%s': object of the same name as the "
         "administrative directory"),
       svn_dirent_local_style(db->local_abspath, pool));

  SVN_ERR(svn_io_check_path(db->local_abspath, &kind, db->pool));

  err = svn_wc__db_read_info(&status, &wc_kind, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL,
                             eb->db, db->local_abspath, db->pool, db->pool);
  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
        return svn_error_return(err);

      svn_error_clear(err);
      wc_kind = svn_wc__db_kind_unknown;
      status = svn_wc__db_status_normal;

      versioned_locally_and_present = FALSE;
    }
  else
    versioned_locally_and_present = IS_NODE_PRESENT(status);

  /* Is this path a conflict victim? */
  SVN_ERR(node_already_conflicted(&already_conflicted, eb->db,
                                  db->local_abspath, pool));
  if (already_conflicted
      && status == svn_wc__db_status_not_present
      && kind == svn_node_none)
    {
      /* A conflict is flagged. Now let's do some user convenience.
       * When we flagged a tree conflict for a local unversioned node
       * vs. an incoming add, and we find that this unversioned node is
       * no longer in the way, automatically pull in the versioned node
       * and remove the conflict marker.
       * Right, the node status matches (not_present) and there is no
       * unversioned obstruction in the file system (anymore?). If it
       * has a tree conflict with reason 'unversioned', remove that. */
      const svn_wc_conflict_description2_t *previous_tc;
      SVN_ERR(svn_wc__get_tree_conflict(&previous_tc,
                                        eb->wc_ctx,
                                        db->local_abspath,
                                        pool, pool));
      if (previous_tc
          && previous_tc->reason == svn_wc_conflict_reason_unversioned)
        {
          /* Remove tree conflict. */
          SVN_ERR(svn_wc__db_op_set_tree_conflict(eb->db,
                                                  db->local_abspath,
                                                  NULL, pool));
          /* Don't skip this path after all. */
          already_conflicted = FALSE;
        }
    }

  /* Now the "usual" behaviour if already conflicted. Skip it. */
  if (already_conflicted)
    {
      /* Record this conflict so that its descendants are skipped silently. */
      SVN_ERR(remember_skipped_tree(eb, db->local_abspath));

      db->skip_this = TRUE;
      db->skip_descendants = TRUE;
      db->already_notified = TRUE;

      /* ### TODO: Also print victim_path in the skip msg. */
      do_notification(eb, db->local_abspath, svn_node_unknown,
                      svn_wc_notify_skip, pool);
      return SVN_NO_ERROR;
    }


  if (versioned_locally_and_present)
    {
      /* What to do with a versioned or schedule-add dir:

         A dir already added without history is OK.  Set add_existed
         so that user notification is delayed until after any prop
         conflicts have been found.

         An existing versioned dir is an error.  In the future we may
         relax this restriction and simply update such dirs.

         A dir added with history is a tree conflict. */

      svn_boolean_t local_is_dir;
      svn_boolean_t local_is_non_dir;
      const char *local_is_copy = NULL;

      /* Is the local add a copy? */
      if (status == svn_wc__db_status_added)
        SVN_ERR(svn_wc__node_get_copyfrom_info(&local_is_copy,
                                               NULL, NULL, NULL, NULL,
                                               eb->wc_ctx,
                                               db->local_abspath,
                                               pool, pool));


      /* Is there something that is a file? */
      local_is_dir = (wc_kind == svn_wc__db_kind_dir
                      && status != svn_wc__db_status_deleted);

      /* Is there *something* that is not a dir? */
      local_is_non_dir = (wc_kind != svn_wc__db_kind_dir
                          && status != svn_wc__db_status_deleted);

      if (local_is_dir)
        {
          svn_boolean_t wc_root;
          svn_boolean_t switched;

          SVN_ERR(svn_wc__check_wc_root(&wc_root, NULL, &switched, eb->db,
                                        db->local_abspath, pool));

          err = NULL;

          if (wc_root)
            {
              /* ### In 1.6 we provided a bit more information on
                     what kind of working copy was found */
              err = svn_error_createf(
                         SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
                         _("Failed to add directory '%s': a separate "
                           "working copy with the same name already exists"),
                         svn_dirent_local_style(db->local_abspath, pool));
            }

          if (!err && switched && !eb->switch_relpath)
            {
              err = svn_error_createf(
                         SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
                         _("Switched directory '%s' does not match "
                           "expected URL '%s'"),
                         svn_dirent_local_style(db->local_abspath, pool),
                         svn_path_url_add_component2(eb->repos_root,
                                                     db->new_relpath, pool));
            }

          if (err != NULL)
            {
              db->already_notified = TRUE;
              do_notification(eb, db->local_abspath, svn_node_dir,
                              svn_wc_notify_update_obstruction, pool);

              return svn_error_return(err);
            }
        }

      /* We can't properly handle add vs. add with mismatching
       * node kinds before single db. */
      if (local_is_non_dir)
        {
          db->already_notified = TRUE;
          do_notification(eb, db->local_abspath, svn_node_dir,
                          svn_wc_notify_update_obstruction, pool);
          return svn_error_createf(
                   SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
                   _("Failed to add directory '%s': a non-directory object "
                     "of the same name already exists"),
                   svn_dirent_local_style(db->local_abspath,
                                          pool));
        }

      /* Do tree conflict checking if
       *  - if there is a local copy.
       *  - if this is a switch operation
       *  - the node kinds mismatch (when single db is here)
       *
       * During switch, local adds at the same path as incoming adds get
       * "lost" in that switching back to the original will no longer have the
       * local add. So switch always alerts the user with a tree conflict.
       *
       * Allow pulling absent/exluded/not_present nodes back in.
       *
       * ### This code is already gearing up to single db with respect to
       * add-vs.-add conflicts with mismatching node kinds. But before single
       * db, we cannot deal with mismatching node kinds properly.
       *
       * ### We would also like to be checking copyfrom infos to not flag tree
       * conflicts on two copies with identical history. But at the time of
       * writing, add_directory() does not get any copyfrom information. */
      if (! pb->in_deleted_and_tree_conflicted_subtree
          && (eb->switch_relpath != NULL
              || local_is_non_dir
              || local_is_copy
             )
         )
        {
          SVN_ERR(check_tree_conflict(&tree_conflict, eb,
                                      db->local_abspath,
                                      svn_wc_conflict_action_add,
                                      svn_node_dir, db->new_relpath, pool));
        }


      if (tree_conflict == NULL)
        {
          /* We have a node in WORKING and we've decided not to flag a
           * conflict, so merge it with the incoming add. */
          db->add_existed = TRUE;

          /* Pre-single-db, a dir that was OS-deleted from the working copy
           * along with its .svn folder is seen 'obstructed' in this code
           * path. The particular situation however better matches the word
           * 'missing'. We do add_existed to avoid spurious errors where other
           * code relies on add_existed to be TRUE when there is a node
           * record (schedule_tests delete_redelete_fudgery used to XFail).
           * Still, let's notify 'A' as the old client did. Ultimately, this
           * should probably say 'Restored' instead of 'A', like with file. */
          if (status == svn_wc__db_status_obstructed
              || status == svn_wc__db_status_obstructed_add
              || status == svn_wc__db_status_obstructed_delete)
            {
              db->already_notified = TRUE;
              do_notification(eb, db->local_abspath, svn_node_dir,
                              svn_wc_notify_add, pool);
            }
        }
    }
  else if (kind != svn_node_none)
    {
      /* There's an unversioned node at this path. */
      db->obstruction_found = TRUE;

      /* Unversioned, obstructing dirs are handled by prop merge/conflict,
       * if unversioned obstructions are allowed. */
      if (! (kind == svn_node_dir && eb->allow_unver_obstructions))
        {
          /* ### Instead of skipping, this should bring in the BASE node
           * and mark some sort of obstruction-conflict. Come, o single-db! */
          db->skip_this = TRUE;

          /* If we are skipping an add, we need to tell the WC that
           * there's a node supposed to be here which we don't have. */
          SVN_ERR(svn_wc__db_base_add_absent_node(eb->db, db->local_abspath,
                                                  db->new_relpath,
                                                  eb->repos_root,
                                                  eb->repos_uuid,
                                                  (eb->target_revision?
                                                   *eb->target_revision
                                                   : SVN_INVALID_REVNUM),
                                                  svn_wc__db_kind_dir,
                                                  svn_wc__db_status_not_present,
                                                  NULL, NULL, pool));
          SVN_ERR(remember_skipped_tree(eb, db->local_abspath));

          /* Mark a conflict */
          SVN_ERR(create_tree_conflict(&tree_conflict, eb,
                                       db->local_abspath,
                                       svn_wc_conflict_reason_unversioned,
                                       svn_wc_conflict_action_add,
                                       svn_node_dir,
                                       db->new_relpath, pool, pool));
          SVN_ERR_ASSERT(tree_conflict != NULL);
        }
    }

  if (tree_conflict != NULL)
    {
      svn_skel_t *work_item;

      /* Queue this conflict in the parent so that its descendants
         are skipped silently. */
      SVN_ERR(svn_wc__loggy_add_tree_conflict(&work_item,
                                              eb->db,
                                              pb->local_abspath,
                                              tree_conflict,
                                              pool));
      SVN_ERR(svn_wc__db_wq_add(eb->db, pb->local_abspath,
                                work_item, pool));

      SVN_ERR(remember_skipped_tree(eb, db->local_abspath));

      db->skip_this = TRUE;
      db->skip_descendants = TRUE;
      db->already_notified = TRUE;

      do_notification(eb, db->local_abspath, svn_node_unknown,
                      svn_wc_notify_tree_conflict, pool);
      return SVN_NO_ERROR;
    }


#ifdef SINGLE_DB
  SVN_ERR(svn_wc__db_temp_op_set_new_dir_to_incomplete(eb->db,
                                                       db->local_abspath,
                                                       db->new_relpath,
                                                       eb->repos_root,
                                                       eb->repos_uuid,
                                                       *eb->target_revision,
                                                       db->ambient_depth,
                                                       pool));
#else
    {
      /* Immediately create an entry for the new directory in the parent.
         Note that the parent must already be either added or opened, and
         thus it's in an 'incomplete' state just like the new dir.
         The entry may already exist if the new directory is already
         scheduled for addition without history, in that case set
         its schedule to normal. */
      SVN_ERR(svn_wc__db_temp_set_parent_stub_to_normal(eb->db,
                                                        db->local_abspath,
                                                        db->add_existed,
                                                        pool));

      if (db->add_existed)
        {
          /* Immediately tweak the schedule for "this dir" so it too
             is no longer scheduled for addition.  Change rev from 0
             to the target revision allowing prep_directory() to do
             its thing without error. 

             ### In the future this should probably become a proper
             ### tree conflict and just handled by putting a base
             ### directory below the existing working node.
             */
          SVN_ERR(svn_wc__db_temp_op_set_new_dir_to_incomplete(
                                                  eb->db,
                                                  db->local_abspath,
                                                  db->new_relpath,
                                                  eb->repos_root,
                                                  eb->repos_uuid,
                                                  *eb->target_revision,
                                                  db->ambient_depth,
                                                  pool));

          SVN_ERR(svn_wc__db_temp_set_parent_stub_to_normal(eb->db,
                                                            db->local_abspath,
                                                            TRUE, pool));
        }
    }
#endif

  SVN_ERR(prep_directory(db,
                         svn_path_url_add_component2(eb->repos_root,
                                                     db->new_relpath, pool),
                         *(eb->target_revision),
                         db->pool));

  /* If PATH is within a locally deleted tree then make it also
     scheduled for deletion.  We must do this after the call to
     prep_directory() otherwise the administrative area for DB->PATH
     is not present, nor is there an entry for DB->PATH in DB->PATH's
     entries. */
  if (pb->in_deleted_and_tree_conflicted_subtree)
    {
      SVN_ERR(svn_wc__db_temp_op_delete(eb->db, db->local_abspath, pool));
    }

  /* If this add was obstructed by dir scheduled for addition without
     history let close_file() handle the notification because there
     might be properties to deal with.  If PATH was added inside a locally
     deleted tree, then suppress notification, a tree conflict was already
     issued. */
  if (eb->notify_func && !db->already_notified && !db->add_existed)
    {
      svn_wc_notify_action_t action;

      if (db->in_deleted_and_tree_conflicted_subtree)
        action = svn_wc_notify_update_add_deleted;
      else if (db->obstruction_found)
        action = svn_wc_notify_exists;
      else
        action = svn_wc_notify_update_add;

      db->already_notified = TRUE;

      do_notification(eb, db->local_abspath, svn_node_dir, action, pool);
    }

  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
open_directory(const char *path,
               void *parent_baton,
               svn_revnum_t base_revision,
               apr_pool_t *pool,
               void **child_baton)
{
  struct dir_baton *db, *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  svn_boolean_t have_work;
  svn_boolean_t already_conflicted;
  svn_wc_conflict_description2_t *tree_conflict = NULL;
  svn_wc__db_status_t status, base_status;

  SVN_ERR(make_dir_baton(&db, path, eb, pb, FALSE, pool));
  *child_baton = db;

  /* We should have a write lock on every directory touched.  */
  SVN_ERR(svn_wc__write_check(eb->db, db->local_abspath, pool));

  if (pb->skip_descendants)
    {
      if (!pb->skip_this)
        SVN_ERR(remember_skipped_tree(eb, db->local_abspath));

      db->skip_this = TRUE;
      db->skip_descendants = TRUE;
      db->already_notified = TRUE;

      db->bump_info->skipped = TRUE;

      return SVN_NO_ERROR;
    }

  SVN_ERR(check_path_under_root(pb->local_abspath, db->name, pool));

  SVN_ERR(svn_wc__db_read_info(&status, NULL, &db->old_revision, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL,
                               &db->ambient_depth, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL,
                               NULL, &have_work, NULL, NULL,
                               eb->db, db->local_abspath, pool, pool));

  if (!have_work)
    base_status = status;
  else
    SVN_ERR(svn_wc__db_base_get_info(&base_status, NULL, &db->old_revision,
                                     NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                     &db->ambient_depth, NULL, NULL, NULL,
                                     NULL,
                                     eb->db, db->local_abspath, pool, pool));

  db->was_incomplete = (base_status == svn_wc__db_status_incomplete);

  /* Is this path a conflict victim? */
  SVN_ERR(node_already_conflicted(&already_conflicted, eb->db,
                                  db->local_abspath, pool));
  if (already_conflicted)
    {
      SVN_ERR(remember_skipped_tree(eb, db->local_abspath));

      db->skip_this = TRUE;
      db->skip_descendants = TRUE;
      db->already_notified = TRUE;

      do_notification(eb, db->local_abspath, svn_node_unknown,
                      svn_wc_notify_skip, pool);

      return SVN_NO_ERROR;
    }

  /* Is this path a fresh tree conflict victim?  If so, skip the tree
     with one notification. */

  /* Check for conflicts only when we haven't already recorded
   * a tree-conflict on a parent node. */
  if (!db->in_deleted_and_tree_conflicted_subtree)
    SVN_ERR(check_tree_conflict(&tree_conflict, eb, db->local_abspath,
                                svn_wc_conflict_action_edit, svn_node_dir,
                                db->new_relpath, pool));

  /* Remember the roots of any locally deleted trees. */
  if (tree_conflict != NULL)
    {
      svn_skel_t *work_item;

      /* Place a tree conflict into the parent work queue.  */
      SVN_ERR(svn_wc__loggy_add_tree_conflict(&work_item,
                                              eb->db, pb->local_abspath,
                                              tree_conflict, pool));
      SVN_ERR(svn_wc__db_wq_add(eb->db, pb->local_abspath, work_item, pool));

      do_notification(eb, db->local_abspath, svn_node_dir,
                      svn_wc_notify_tree_conflict, pool);
      db->already_notified = TRUE;

      /* Even if PATH is locally deleted we still need mark it as being
         at TARGET_REVISION, so fall through to the code below to do just
         that. */
      if (tree_conflict->reason != svn_wc_conflict_reason_deleted &&
          tree_conflict->reason != svn_wc_conflict_reason_replaced)
        {
          SVN_ERR(remember_skipped_tree(eb, db->local_abspath));
          db->skip_descendants = TRUE;
          db->skip_this = TRUE;

          return SVN_NO_ERROR;
        }
      else
        db->in_deleted_and_tree_conflicted_subtree = TRUE;
    }

  /* Mark directory as being at target_revision and URL, but incomplete. */
  SVN_ERR(svn_wc__db_temp_op_start_directory_update(eb->db, db->local_abspath,
                                                    db->new_relpath,
                                                    *eb->target_revision,
                                                    pool));

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
static svn_error_t *
change_dir_prop(void *dir_baton,
                const char *name,
                const svn_string_t *value,
                apr_pool_t *pool)
{
  svn_prop_t *propchange;
  struct dir_baton *db = dir_baton;

  if (db->skip_this)
    return SVN_NO_ERROR;

  propchange = apr_array_push(db->propchanges);
  propchange->name = apr_pstrdup(db->pool, name);
  propchange->value = value ? svn_string_dup(value, db->pool) : NULL;

  return SVN_NO_ERROR;
}

/* If any of the svn_prop_t objects in PROPCHANGES represents a change
   to the SVN_PROP_EXTERNALS property, return that change, else return
   null.  If PROPCHANGES contains more than one such change, return
   the first. */
static const svn_prop_t *
externals_prop_changed(const apr_array_header_t *propchanges)
{
  int i;

  for (i = 0; i < propchanges->nelts; i++)
    {
      const svn_prop_t *p = &(APR_ARRAY_IDX(propchanges, i, svn_prop_t));
      if (strcmp(p->name, SVN_PROP_EXTERNALS) == 0)
        return p;
    }

  return NULL;
}


/* Create in POOL a name->value hash from PROP_LIST, and return it. */
static apr_hash_t *
prop_hash_from_array(const apr_array_header_t *prop_list,
                     apr_pool_t *pool)
{
  int i;
  apr_hash_t *prop_hash = apr_hash_make(pool);

  for (i = 0; i < prop_list->nelts; i++)
    {
      const svn_prop_t *prop = &APR_ARRAY_IDX(prop_list, i, svn_prop_t);
      apr_hash_set(prop_hash, prop->name, APR_HASH_KEY_STRING, prop->value);
    }

  return prop_hash;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
close_directory(void *dir_baton,
                apr_pool_t *pool)
{
  struct dir_baton *db = dir_baton;
  struct edit_baton *eb = db->edit_baton;
  svn_wc_notify_state_t prop_state = svn_wc_notify_state_unknown;
  apr_array_header_t *entry_props, *dav_props, *regular_props;
  apr_hash_t *base_props;
  apr_hash_t *actual_props;
  apr_hash_t *new_base_props = NULL, *new_actual_props = NULL;
  struct bump_dir_info *bdi;
  svn_revnum_t new_changed_rev;
  apr_time_t new_changed_date;
  const char *new_changed_author;

  /* Skip if we're in a conflicted tree. */
  if (db->skip_this)
    {
      db->bump_info->skipped = TRUE;

      /* ### hopefully this directory's queue is empty, cuz we're not
         ### going to be running it!  */

      /* Allow the parent to complete its update. */
      SVN_ERR(maybe_bump_dir_info(eb, db->bump_info, db->pool));

      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_categorize_props(db->propchanges, &entry_props, &dav_props,
                               &regular_props, pool));

  /* Fetch the existing properties.  */
  SVN_ERR(svn_wc__get_pristine_props(&base_props,
                                     eb->db, db->local_abspath,
                                     pool, pool));
  SVN_ERR(svn_wc__get_actual_props(&actual_props,
                                   eb->db, db->local_abspath,
                                   pool, pool));

  /* Local-add nodes have no pristines. Incoming-adds have no actuals.  */
  if (base_props == NULL)
    base_props = apr_hash_make(pool);
  if (actual_props == NULL)
    actual_props = apr_hash_make(pool);

  /* An incomplete directory might have props which were supposed to be
     deleted but weren't.  Because the server sent us all the props we're
     supposed to have, any previous base props not in this list must be
     deleted (issue #1672). */
  if (db->was_incomplete)
    {
      int i;
      apr_hash_t *props_to_delete;
      apr_hash_index_t *hi;

      /* In a copy of the BASE props, remove every property that we see an
         incoming change for. The remaining unmentioned properties are those
         which need to be deleted.  */
      props_to_delete = apr_hash_copy(pool, base_props);
      for (i = 0; i < regular_props->nelts; i++)
        {
          const svn_prop_t *prop;
          prop = &APR_ARRAY_IDX(regular_props, i, svn_prop_t);
          apr_hash_set(props_to_delete, prop->name,
                       APR_HASH_KEY_STRING, NULL);
        }

      /* Add these props to the incoming propchanges (in regular_props).  */
      for (hi = apr_hash_first(pool, props_to_delete);
           hi != NULL;
           hi = apr_hash_next(hi))
        {
          const char *propname = svn__apr_hash_index_key(hi);
          svn_prop_t *prop = apr_array_push(regular_props);

          /* Record a deletion for PROPNAME.  */
          prop->name = propname;
          prop->value = NULL;
        }
    }

  /* If this directory has property changes stored up, now is the time
     to deal with them. */
  if (regular_props->nelts || entry_props->nelts || dav_props->nelts)
    {
      if (regular_props->nelts)
        {
          /* If recording traversal info, then see if the
             SVN_PROP_EXTERNALS property on this directory changed,
             and record before and after for the change. */
            if (eb->external_func)
            {
              const svn_prop_t *change = externals_prop_changed(regular_props);

              if (change)
                {
                  const svn_string_t *new_val_s = change->value;
                  const svn_string_t *old_val_s;

                  SVN_ERR(svn_wc__internal_propget(
                           &old_val_s, eb->db, db->local_abspath,
                           SVN_PROP_EXTERNALS, db->pool, db->pool));

                  if ((new_val_s == NULL) && (old_val_s == NULL))
                    ; /* No value before, no value after... so do nothing. */
                  else if (new_val_s && old_val_s
                           && (svn_string_compare(old_val_s, new_val_s)))
                    ; /* Value did not change... so do nothing. */
                  else if (old_val_s || new_val_s)
                    /* something changed, record the change */
                    {
                      SVN_ERR((eb->external_func)(
                                           eb->external_baton,
                                           db->local_abspath,
                                           old_val_s,
                                           new_val_s,
                                           db->ambient_depth,
                                           db->pool));
                    }
                }
            }

          /* Merge pending properties into temporary files (ignoring
             conflicts). */
          SVN_ERR_W(svn_wc__merge_props(&prop_state,
                                        &new_base_props,
                                        &new_actual_props,
                                        eb->db,
                                        db->local_abspath,
                                        svn_wc__db_kind_dir,
                                        NULL, /* left_version */
                                        NULL, /* right_version */
                                        NULL /* use baseprops */,
                                        base_props,
                                        actual_props,
                                        regular_props,
                                        TRUE /* base_merge */,
                                        FALSE /* dry_run */,
                                        eb->conflict_func,
                                        eb->conflict_baton,
                                        eb->cancel_func,
                                        eb->cancel_baton,
                                        db->pool,
                                        pool),
                    _("Couldn't do property merge"));
          /* After a (not-dry-run) merge, we ALWAYS have props to save.  */
          SVN_ERR_ASSERT(new_base_props != NULL && new_actual_props != NULL);
        }

      SVN_ERR(accumulate_last_change(&new_changed_rev,
                                     &new_changed_date,
                                     &new_changed_author,
                                     eb->db, db->local_abspath, entry_props,
                                     pool, pool));
    }

  /* If this directory is merely an anchor for a targeted child, then we
     should not be updating the node at all.  */
  if (db->parent_baton == NULL
      && *eb->target_basename != '\0')
    {
      /* And we should not have received any changes!  */
      SVN_ERR_ASSERT(db->propchanges->nelts == 0);
      /* ... which also implies NEW_CHANGED_* are not set,
         and NEW_BASE_PROPS == NULL.  */
    }
  else
    {
      svn_depth_t depth;
      svn_revnum_t changed_rev;
      apr_time_t changed_date;
      const char *changed_author;
      apr_hash_t *props;

      /* ### we know a base node already exists. it was created in
         ### open_directory or add_directory.  let's just preserve the
         ### existing DEPTH value, and possibly CHANGED_*.  */
      SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       &changed_rev,
                                       &changed_date,
                                       &changed_author,
                                       NULL, &depth, NULL, NULL, NULL, NULL,
                                       eb->db, db->local_abspath,
                                       pool, pool));

      /* If we received any changed_* values, then use them.  */
      if (SVN_IS_VALID_REVNUM(new_changed_rev))
        changed_rev = new_changed_rev;
      if (new_changed_date != 0)
        changed_date = new_changed_date;
      if (new_changed_author != NULL)
        changed_author = new_changed_author;

#ifdef SVN_WC__SINGLE_DB
      /* If no depth is set yet, set to infinity. */
      if (depth == svn_depth_unknown)
        depth = svn_depth_infinity;
#endif

      /* Do we have new properties to install? Or shall we simply retain
         the prior set of properties? If we're installing new properties,
         then we also want to write them to an old-style props file.  */
      props = new_base_props;
      if (props == NULL)
        SVN_ERR(svn_wc__db_base_get_props(&props, eb->db, db->local_abspath,
                                          pool, pool));

      /* ### NOTE: from this point onwards, we make TWO changes to the
         ### database in a non-transactional way. some kind of revamp
         ### needs to happend to bring this down to a single DB transaction
         ### to perform the changes and install all the needed work items.  */

      SVN_ERR(svn_wc__db_base_add_directory(
                eb->db, db->local_abspath,
                db->new_relpath,
                eb->repos_root, eb->repos_uuid,
                *eb->target_revision,
                props,
                changed_rev, changed_date, changed_author,
                NULL /* children */,
                depth,
                (dav_props && dav_props->nelts > 0)
                    ? prop_hash_from_array(dav_props, pool)
                    : NULL,
                NULL /* conflict */,
                NULL /* work_items */,
                pool));

      /* If we updated the BASE properties, then we also have ACTUAL
         properties to update. Do that now, along with queueing a work
         item to write out an old-style props file.  */
      if (new_base_props != NULL)
        {
          apr_array_header_t *prop_diffs;

          SVN_ERR_ASSERT(new_actual_props != NULL);

          /* If the ACTUAL props are the same as the BASE props, then we
             should "write" a NULL. This will remove the props from the
             ACTUAL_NODE row, and remove the old-style props file, indicating
             "no change".  */
          props = new_actual_props;
          SVN_ERR(svn_prop_diffs(&prop_diffs, new_actual_props, new_base_props,
                                 pool));
          if (prop_diffs->nelts == 0)
            props = NULL;

          SVN_ERR(svn_wc__db_op_set_props(eb->db, db->local_abspath,
                                          props,
                                          NULL /* conflict */,
                                          NULL /* work_items */,
                                          pool));
        }
    }

  /* Process all of the queued work items for this directory.  */
  SVN_ERR(svn_wc__wq_run(eb->db, db->local_abspath,
                         eb->cancel_func, eb->cancel_baton,
                         pool));

  /* We're done with this directory, so remove one reference from the
     bump information. This may trigger a number of actions. See
     maybe_bump_dir_info() for more information.  */
  SVN_ERR(maybe_bump_dir_info(eb, db->bump_info, db->pool));

  /* Notify of any prop changes on this directory -- but do nothing if
     it's an added or skipped directory, because notification has already
     happened in that case - unless the add was obstructed by a dir
     scheduled for addition without history, in which case we handle
     notification here). */
  if (!db->already_notified && eb->notify_func)
    {
      svn_wc_notify_t *notify;
      svn_wc_notify_action_t action;

      if (db->in_deleted_and_tree_conflicted_subtree)
        action = svn_wc_notify_update_update_deleted;
      else if (db->obstruction_found || db->add_existed)
        action = svn_wc_notify_exists;
      else
        action = svn_wc_notify_update_update;

      notify = svn_wc_create_notify(db->local_abspath, action, pool);
      notify->kind = svn_node_dir;
      notify->prop_state = prop_state;
      notify->revision = *eb->target_revision;
      notify->old_revision = db->old_revision;

      eb->notify_func(eb->notify_baton, notify, pool);
    }

  bdi = db->bump_info;
  while (bdi && !bdi->ref_count)
    {
      apr_pool_t *destroy_pool = bdi->pool;
      apr_pool_cleanup_kill(destroy_pool, db, cleanup_dir_baton);
      bdi = bdi->parent;
      svn_pool_destroy(destroy_pool);
    }

  return SVN_NO_ERROR;
}


/* Common code for 'absent_file' and 'absent_directory'. */
static svn_error_t *
absent_file_or_dir(const char *path,
                   svn_node_kind_t kind,
                   void *parent_baton,
                   apr_pool_t *pool)
{
  const char *name = svn_dirent_basename(path, pool);
  const char *local_abspath;
  struct dir_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  const char *repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_boolean_t is_added;
  svn_node_kind_t existing_kind;
  svn_wc__db_kind_t db_kind
    = kind == svn_node_dir ? svn_wc__db_kind_dir : svn_wc__db_kind_file;

  local_abspath = svn_dirent_join(pb->local_abspath, name, pool);

  /* If an item by this name is scheduled for addition that's a
     genuine tree-conflict.  */
  SVN_ERR(svn_wc_read_kind(&existing_kind, eb->wc_ctx, local_abspath, TRUE,
                           pool));
  if (existing_kind != svn_node_none)
    {
      SVN_ERR(svn_wc__node_is_added(&is_added, eb->wc_ctx, local_abspath,
                                    pool));
      if (is_added)
        return svn_error_createf(
         SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
         _("Failed to mark '%s' absent: item of the same name is already "
           "scheduled for addition"),
         svn_dirent_local_style(path, pool));
    }

  SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath, &repos_root_url,
                                     &repos_uuid, eb->db, pb->local_abspath,
                                     pool, pool));
  repos_relpath = svn_dirent_join(repos_relpath, name, pool);

  SVN_ERR(svn_wc__db_base_add_absent_node(eb->db, local_abspath,
                                          repos_relpath, repos_root_url,
                                          repos_uuid, *(eb->target_revision),
                                          db_kind, svn_wc__db_status_absent,
                                          NULL, NULL,
                                          pool));

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
static svn_error_t *
absent_file(const char *path,
            void *parent_baton,
            apr_pool_t *pool)
{
  return absent_file_or_dir(path, svn_node_file, parent_baton, pool);
}


/* An svn_delta_editor_t function. */
static svn_error_t *
absent_directory(const char *path,
                 void *parent_baton,
                 apr_pool_t *pool)
{
  return absent_file_or_dir(path, svn_node_dir, parent_baton, pool);
}


/* Beginning at DIR_ABSPATH within a working copy, search the working copy
   copy for a pre-existing versioned file which is exactly equal to
   COPYFROM_PATH@COPYFROM_REV.

   The current implementation does this by taking the repos_relpath of
   dir_abspath and copyfrom_relpath to calculate where in the working copy
   repos_relpath would be and then tries to confirm its guess.

   1) When it finds a copied file there, it looks for its origin to see
   if the origin matches the copied file good enough to use it as new base
   contents and properties. If that is the case set NEW_BASE_CONTENTS
   and NEW_BASE_PROPS to the found restult.

   If the new base information is found check if the node is tree-conflicted,
   and when that is the case use its in-wc contents and actual properties
   to set NEW_CONTENTS and NEW_PROPS.

   (If new base info is found, return)

   2) If the node's BASE information matches the expected origin matches the the
   copy origin good enough use it as NEW_BASE_CONTENTS and NEW_BASE_PROPS.

   If the new base information is found and the db_status of the node is normal,
   then set NEW_CONTENTS and NEW_PROPS with the found values.

   If data is not found, its values will be set to NULL. 

   Allocate the return values in RESULT_POOL, but perform all temporary allocations
   in SCRATCH_POOL.

   ### With a centralized datastore this becomes much easier. For now we
   ### keep the old algorithm because the result is also used for copying
   ### local changes. This support can probably be removed once we have real
   ### local file moves.
*/
static svn_error_t *
locate_copyfrom(svn_stream_t **new_base_contents,
                svn_stream_t **new_contents,
                apr_hash_t **new_base_props,
                apr_hash_t **new_props,
                svn_wc__db_t *db,
                const char *dir_abspath,
                const char *copyfrom_relpath,
                svn_revnum_t copyfrom_rev,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  const char *ancestor_abspath, *ancestor_relpath;
  const char *dir_repos_relpath, *dir_repos_root_url, *dir_repos_uuid;
  const char *repos_relpath, *repos_root_url, *repos_uuid;
  const char *local_abspath;

  apr_size_t levels_up;
  svn_error_t *err;

  SVN_ERR_ASSERT(copyfrom_relpath[0] != '/');

  SVN_ERR(svn_wc__db_scan_base_repos(&dir_repos_relpath, &dir_repos_root_url,
                                     &dir_repos_uuid,
                                     db, dir_abspath,
                                     scratch_pool, scratch_pool));

  /* Be pessimistic.  This function is basically a series of tests
     that gives dozens of ways to fail our search, returning
     SVN_NO_ERROR in each case.  If we make it all the way to the
     bottom, we have a real discovery to return. */
  *new_base_contents = NULL;
  *new_contents = NULL;
  *new_base_props = NULL;
  *new_props = NULL;

  /* Subtract the dest_dir's URL from the repository "root" URL to get
     the absolute FS path represented by dest_dir. */

  /* Find nearest FS ancestor dir of current FS path and copyfrom_parent */
  ancestor_relpath = svn_relpath_get_longest_ancestor(dir_repos_relpath,
                                                      copyfrom_relpath,
                                                      scratch_pool);

  /* Move 'up' the working copy to what ought to be the common ancestor dir. */
  levels_up = svn_path_component_count(dir_repos_relpath)
              - svn_path_component_count(ancestor_relpath);

  /* Walk up the path dirent safe */
  ancestor_abspath = dir_abspath;
  while (levels_up-- > 0)
    ancestor_abspath = svn_dirent_dirname(ancestor_abspath, scratch_pool);

  /* Verify hypothetical ancestor */
  err = svn_wc__db_scan_base_repos(&repos_relpath, &repos_root_url,
                                   &repos_uuid,
                                   db, ancestor_abspath,
                                   scratch_pool, scratch_pool);

  if (err && ((err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY) ||
              (err->apr_err == SVN_ERR_WC_PATH_FOUND)))
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  else
    SVN_ERR(err);

  /* If we got this far, we know that the ancestor dir exists, and
     that it's a working copy too.  But is it from the same
     repository?  And does it represent the URL we expect it to? */
  if ((strcmp(dir_repos_uuid, repos_uuid) != 0)
      || (strcmp(dir_repos_root_url, repos_root_url) != 0)
      || (strcmp(ancestor_relpath, repos_relpath) != 0))
    return SVN_NO_ERROR;

  /* Add the remaining components to cwd, then add the remaining relpath to
     where we hope the copyfrom_relpath file exists. */
  local_abspath = svn_dirent_join(ancestor_abspath,
                                 svn_dirent_skip_ancestor(ancestor_relpath,
                                                          copyfrom_relpath),
                                 scratch_pool);

  /* Verify file in expected location */
  {
    svn_revnum_t rev, changed_rev;
    svn_wc__db_status_t status, base_status;
    svn_boolean_t conflicted, have_base;
    const svn_checksum_t *checksum;

    err = svn_wc__db_read_info(&status, NULL, &rev, &repos_relpath,
                               &repos_root_url, &repos_uuid, &changed_rev,
                               NULL, NULL, NULL, NULL, &checksum, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, &have_base,
                               NULL, &conflicted, NULL,
                               db, local_abspath, scratch_pool, scratch_pool);

    if (err && ((err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY ||
                (err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND))))
      {
        svn_error_clear(err);
        return SVN_NO_ERROR;
      }

    /* Check if we have an added node with the right copyfrom information, as
       this is what you would see on a file move. */
       
    if (status == svn_wc__db_status_added)
      {
        const char *op_root_abspath;
        const char *original_repos_relpath, *original_root_url;
        const char *original_uuid;
        svn_revnum_t original_rev;

        SVN_ERR(svn_wc__db_scan_addition(&status, &op_root_abspath,
                                         &repos_relpath, &repos_root_url,
                                         &repos_uuid, &original_repos_relpath,
                                         &original_root_url, &original_uuid,
                                         &original_rev,
                                         db, local_abspath,
                                         scratch_pool, scratch_pool));

        if (status == svn_wc__db_status_copied
            || status == svn_wc__db_status_moved_here)
          {
            original_repos_relpath = svn_relpath_join(
                                    original_repos_relpath,
                                    svn_dirent_skip_ancestor(op_root_abspath,
                                                             local_abspath),
                                    scratch_pool);

            /* If the repository location matches our exact guess and
               the file's recorded revisions tell us that the file had the
               same contents at the copyfrom_revision, we can use this
               data as new_base */
            if (strcmp(original_repos_relpath, copyfrom_relpath) == 0
                && strcmp(original_root_url, dir_repos_root_url) == 0
                && strcmp(original_uuid, dir_repos_uuid) == 0
                && strcmp(repos_relpath, copyfrom_relpath) == 0
                && strcmp(repos_root_url, dir_repos_root_url) == 0
                && strcmp(repos_uuid, dir_repos_uuid) == 0

                && SVN_IS_VALID_REVNUM(changed_rev)
                && changed_rev <= copyfrom_rev
                && copyfrom_rev <= original_rev)
              {
                svn_node_kind_t kind;
                svn_boolean_t text_changed;

                /* WORKING_NODE has the right new-BASE information,
                   so we have at least a partial result. */
                SVN_ERR(svn_wc__db_pristine_read(new_base_contents,
                                                 db, local_abspath, checksum,
                                                 result_pool, scratch_pool));
                SVN_ERR(svn_wc__get_pristine_props(new_base_props,
                                                   db, local_abspath,
                                                   result_pool, scratch_pool));

                /* If the node is conflicted, that might have happened because
                   the node was deleted. Which might indicate that we have
                   a file move. In this case we like the real file data */
                if (!conflicted
                    && status == svn_wc__db_status_copied)
                  return SVN_NO_ERROR; /* A local copy is no local modification
                                          that we should keep */

                /* ### TODO: Add verification to check that the conflict
                       tells us that this is the right thing to do.

                   ### Pre 1.7 we just assumed that it is ok without checking for
                       conflicts, so this is not a regression */

                SVN_ERR(svn_io_check_path(local_abspath, &kind, scratch_pool));

                if (kind != svn_node_file)
                  return SVN_NO_ERROR; /* Nothing to copy */

                SVN_ERR(svn_wc__internal_text_modified_p(&text_changed, db,
                                                         local_abspath, FALSE,
                                                         TRUE, scratch_pool));

                if (!text_changed)
                  return SVN_NO_ERROR; /* Take the easy route */

                SVN_ERR(svn_stream_open_readonly(new_contents, local_abspath,
                                                 result_pool, scratch_pool));

                SVN_ERR(svn_wc__get_actual_props(new_props, db, local_abspath,
                                                 result_pool, scratch_pool));

                return SVN_NO_ERROR;
              }
          }
      }

    if (!have_base)
      return SVN_NO_ERROR;

    base_status = status;

    if (status != svn_wc__db_status_normal)
      SVN_ERR(svn_wc__db_base_get_info(&base_status, NULL, &rev,
                                       &repos_relpath, &repos_root_url,
                                       &repos_uuid, &changed_rev, NULL,
                                       NULL, NULL, NULL, &checksum, NULL,
                                       NULL, NULL,
                                       db, local_abspath,
                                       scratch_pool, scratch_pool));

    if (base_status != svn_wc__db_status_normal)
      return SVN_NO_ERROR; /* No interesting BASE_NODE */

    if (!repos_relpath || !repos_root_url || !repos_uuid)
      SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath, &repos_root_url,
                                         &repos_uuid,
                                         db, local_abspath,
                                         scratch_pool, scratch_pool));

    /* Is it from the same repository */
    if ((strcmp(dir_repos_uuid, repos_uuid) != 0)
        || (strcmp(dir_repos_root_url, repos_root_url) != 0)
        || (strcmp(copyfrom_relpath, repos_relpath) != 0))
      return SVN_NO_ERROR;

    /* Ok, we know that we look at the right node, but do we have the
       right revision?

       To be sure that the base node has the right properties and text,
       the node must be the same in copyfrom_rev and changed_rev, which
       is only true within this specific range
       */
    if (!(SVN_IS_VALID_REVNUM(changed_rev)
          && changed_rev <= copyfrom_rev
          && copyfrom_rev <= rev))
      {
        return SVN_NO_ERROR;
      }

    /* BASE_NODE has the right new-BASE information,
       so we have at least a partial result. */
    SVN_ERR(svn_wc__db_pristine_read(new_base_contents,
                                     db, local_abspath, checksum,
                                     result_pool, scratch_pool));

    SVN_ERR(svn_wc__db_base_get_props(new_base_props,
                                      db, local_abspath, result_pool,
                                      scratch_pool));

    /* If the node is in status normal, the user probably intended to make
       a copy of this in-wc node, so copy its local changes over to
       the new file. */
    if (status == svn_wc__db_status_normal)
      {
        svn_node_kind_t kind;
        svn_boolean_t text_changed;

        SVN_ERR(svn_io_check_path(local_abspath, &kind, scratch_pool));

        if (kind != svn_node_file)
          return SVN_NO_ERROR; /* Nothing to copy */

        SVN_ERR(svn_wc__internal_text_modified_p(&text_changed, db,
                                                 local_abspath, FALSE,
                                                 TRUE, scratch_pool));

        if (!text_changed)
            return SVN_NO_ERROR; /* Take the easy route */

        SVN_ERR(svn_stream_open_readonly(new_contents, local_abspath,
                                         result_pool, scratch_pool));

        SVN_ERR(svn_wc__get_actual_props(new_props, db, local_abspath,
                                         result_pool, scratch_pool));
      }
  }
  return SVN_NO_ERROR;
}


/* Given a set of properties PROPS_IN, find all regular properties
   and shallowly copy them into a new set (allocate the new set in
   POOL, but the set's members retain their original allocations). */
static apr_hash_t *
copy_regular_props(apr_hash_t *props_in,
                   apr_pool_t *pool)
{
  apr_hash_t *props_out = apr_hash_make(pool);
  apr_hash_index_t *hi;

  for (hi = apr_hash_first(pool, props_in); hi; hi = apr_hash_next(hi))
    {
      const char *propname = svn__apr_hash_index_key(hi);
      const svn_string_t *propval = svn__apr_hash_index_val(hi);

      if (svn_wc_is_normal_prop(propname))
        apr_hash_set(props_out, propname, APR_HASH_KEY_STRING, propval);
    }
  return props_out;
}


/* Do the "with history" part of add_file().

   Attempt to locate COPYFROM_PATH@COPYFROM_REV within the existing working
   copy.  If a node with such a base is found, copy the base *and working*
   text and properties from there.  If not found, fetch the text and
   properties from the repository by calling PB->edit_baton->fetch_func.

   Store the copied base and working text in new temporary files in the adm
   tmp area of the parent directory, whose baton is PB.  Set
   TFB->copied_text_base_* and TFB->copied_working_text to their paths and
   checksums.  Set TFB->copied_*_props to the copied properties.

   After this function returns, subsequent apply_textdelta() commands coming
   from the server may further alter the file before it is installed.

   Ensure the resulting text base is in the pristine store, and set
   TFB->copied_text_base_* to its readable abspath and checksums.
*/
static svn_error_t *
add_file_with_history(struct dir_baton *pb,
                      const char *copyfrom_path,
                      svn_revnum_t copyfrom_rev,
                      struct file_baton *tfb,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  struct edit_baton *eb = pb->edit_baton;
  svn_stream_t *copied_stream;
  const char *copied_text_base_tmp_abspath;
  svn_wc__db_t *db = eb->db;
  svn_stream_t *new_base_contents, *new_contents;
  apr_hash_t *new_base_props, *new_props;

  SVN_ERR_ASSERT(copyfrom_path[0] == '/');

  tfb->added_with_history = TRUE;

  /* Attempt to locate the copyfrom_path in the working copy first. */
  SVN_ERR(locate_copyfrom(&new_base_contents, &new_contents,
                          &new_base_props, &new_props,
                          db, pb->local_abspath,
                          copyfrom_path+1, /* Create repos_relpath */
                          copyfrom_rev, result_pool, scratch_pool));

  /* Open the text base for writing (this will get us a temporary file).  */
  SVN_ERR(svn_wc__open_writable_base(&copied_stream,
                                     &copied_text_base_tmp_abspath,
  /* Compute an MD5 checksum for the stream as we write stuff into it.
     ### this is temporary. in many cases, we already *know* the checksum
     ### since it is a copy. */
                                     &tfb->copied_text_base_md5_checksum,
                                     &tfb->copied_text_base_sha1_checksum,
                                     db, pb->local_abspath,
                                     result_pool, scratch_pool));

  if (new_base_contents && new_base_props)
    {
      /* Copy the existing file's text-base over to the (temporary)
         new text-base, where the file baton expects it to be.  Get
         the text base and props from the usual place or from the
         revert place, depending on scheduling. */
      SVN_ERR(svn_stream_copy3(new_base_contents, copied_stream,
                               eb->cancel_func, eb->cancel_baton,
                               scratch_pool));

      if (!new_props)
        new_props = new_base_props;
    }
  else  /* Couldn't find a file to copy  */
    {
      /* Fall back to fetching it from the repository instead. */

      if (! eb->fetch_func)
        return svn_error_create(SVN_ERR_WC_INVALID_OP_ON_CWD, NULL,
                                _("No fetch_func supplied to update_editor"));

      /* Fetch the repository file's text-base and base-props;
         svn_stream_close() automatically closes the text-base file for us. */

      /* copyfrom_path is a absolute path, fetch_func requires a path relative
         to the root of the repository so skip the first '/'. */
      SVN_ERR(eb->fetch_func(eb->fetch_baton, copyfrom_path + 1, copyfrom_rev,
                             copied_stream,
                             NULL, &new_base_props, scratch_pool));
      SVN_ERR(svn_stream_close(copied_stream));

      /* Filter out wc-props */
      /* ### Do we get new values as modification or should these really
             be installed? */
      new_base_props = svn_prop_hash_dup(copy_regular_props(new_base_props,
                                                             scratch_pool),
                                         result_pool);

      new_props = new_base_props;
    }

  SVN_ERR(svn_wc__db_pristine_install(db, copied_text_base_tmp_abspath,
                                      tfb->copied_text_base_sha1_checksum,
                                      tfb->copied_text_base_md5_checksum,
                                      scratch_pool));

  tfb->copied_base_props = new_base_props;

  if (new_contents)
    {
      /* If we copied an existing file over, we need to copy its
         working text too, to preserve any local mods.  (We already
         read its working *props* into tfb->copied_working_props.) */
      const char *temp_dir_abspath;
      svn_stream_t *tmp_contents;

        /* Make a unique file name for the copied working text. */
      SVN_ERR(svn_wc__db_temp_wcroot_tempdir(&temp_dir_abspath,
                                             db, pb->local_abspath,
                                             scratch_pool, scratch_pool));

      SVN_ERR(svn_stream_open_unique(&tmp_contents, &tfb->copied_working_text,
                                     temp_dir_abspath, svn_io_file_del_none,
                                     result_pool, scratch_pool));

      SVN_ERR(svn_stream_copy3(new_contents, tmp_contents, eb->cancel_func,
                               eb->cancel_baton,
                               scratch_pool));

       tfb->copied_working_props = new_props;
    }

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
static svn_error_t *
add_file(const char *path,
         void *parent_baton,
         const char *copyfrom_path,
         svn_revnum_t copyfrom_rev,
         apr_pool_t *pool,
         void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  struct file_baton *fb;
  svn_node_kind_t kind;
  svn_wc__db_kind_t wc_kind;
  svn_wc__db_status_t status;
  apr_pool_t *subpool;
  svn_boolean_t already_conflicted;
  svn_boolean_t versioned_locally_and_present;
  svn_error_t *err;
  svn_wc_conflict_description2_t *tree_conflict = NULL;

  /* Skip the initial '/' */
  const char *copyfrom_relpath = (copyfrom_path && copyfrom_path[0] ?
                                  copyfrom_path+1 : copyfrom_path);

  /* Semantic check.  Either both "copyfrom" args are valid, or they're
     NULL and SVN_INVALID_REVNUM.  A mixture is illegal semantics. */
  SVN_ERR_ASSERT((copyfrom_path && SVN_IS_VALID_REVNUM(copyfrom_rev))
                 || (!copyfrom_path &&
                     !SVN_IS_VALID_REVNUM(copyfrom_rev)));

  SVN_ERR(make_file_baton(&fb, pb, path, TRUE, pool));
  *file_baton = fb;

  if (pb->skip_descendants)
    {
      if (!pb->skip_this)
        SVN_ERR(remember_skipped_tree(eb, fb->local_abspath));

      fb->skip_this = TRUE;
      fb->already_notified = TRUE;

      return SVN_NO_ERROR;
    }

  SVN_ERR(check_path_under_root(pb->local_abspath, fb->name, pool));

  fb->deleted = pb->in_deleted_and_tree_conflicted_subtree;

  /* The file_pool can stick around for a *long* time, so we want to
     use a subpool for any temporary allocations. */
  subpool = svn_pool_create(pool);


  /* It may not be named the same as the administrative directory. */
  if (svn_wc_is_adm_dir(fb->name, pool))
    return svn_error_createf(
       SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
       _("Failed to add file '%s': object of the same name as the "
         "administrative directory"),
       svn_dirent_local_style(fb->local_abspath, pool));

  SVN_ERR(svn_io_check_path(fb->local_abspath, &kind, subpool));

  err = svn_wc__db_read_info(&status, &wc_kind, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL,
                             eb->db, fb->local_abspath, subpool, subpool);

  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND)
        return svn_error_return(err);

      svn_error_clear(err);
      wc_kind = svn_wc__db_kind_unknown;
      status = svn_wc__db_status_normal;

      versioned_locally_and_present = FALSE;
    }
  else
    versioned_locally_and_present = IS_NODE_PRESENT(status);


  /* Is this path a conflict victim? */
  SVN_ERR(node_already_conflicted(&already_conflicted, eb->db,
                                  fb->local_abspath, subpool));
  if (already_conflicted)
    {
      svn_boolean_t do_skip = TRUE;

      /* A conflict is flagged. Now let's do some user convenience.
       * When we flagged a tree conflict for a local unversioned node
       * vs. an incoming add, and we find that this unversioned node is
       * no longer in the way, automatically pull in the versioned node
       * and remove the conflict marker. */
      if (status == svn_wc__db_status_not_present
          && kind == svn_node_none)
        {
          /* Right, the node status matches (not_present) and there is no
           * unversioned obstruction in the file system (anymore?). If it
           * has a tree conflict with reason 'unversioned', remove that. */
          const svn_wc_conflict_description2_t *previous_tc;
          SVN_ERR(svn_wc__get_tree_conflict(&previous_tc,
                                            eb->wc_ctx,
                                            fb->local_abspath,
                                            subpool, subpool));
          if (previous_tc
              && previous_tc->reason == svn_wc_conflict_reason_unversioned)
            {
              do_skip = FALSE;

              /* Remove tree conflict. */
              SVN_ERR(svn_wc__db_op_set_tree_conflict(eb->db,
                                                      fb->local_abspath,
                                                      NULL, subpool));
            }
        }

      if (do_skip)
        {
          SVN_ERR(remember_skipped_tree(eb, fb->local_abspath));

          fb->skip_this = TRUE;
          fb->already_notified = TRUE;

          do_notification(eb, fb->local_abspath, svn_node_unknown,
                          svn_wc_notify_skip, subpool);

          svn_pool_destroy(subpool);

          return SVN_NO_ERROR;
        }
    }


  if (versioned_locally_and_present)
    {
      /* What to do with a versioned or schedule-add file:

         If the UUID doesn't match the parent's, or the URL isn't a child of
         the parent dir's URL, it's an error.

         A file with matching history is OK.  Set add_existed so that
         user notification is delayed until after any text or prop conflicts
         have been found.

         Whether the incoming add is a symlink or a file will only be known in
         close_file(), when the props are known. So with a locally added file
         or symlink, let close_file() check for a tree conflict.

         We will never see missing files here, because these would be
         re-added during the crawler phase. */
      svn_boolean_t local_is_file;
      svn_boolean_t local_is_non_file;
      svn_boolean_t is_file_external;
      const char *local_copyfrom_repos_relpath = NULL;
      svn_revnum_t local_copyfrom_rev = SVN_INVALID_REVNUM;

      /* Is the local add a copy, and where from? */
      if (status == svn_wc__db_status_added)
        SVN_ERR(svn_wc__node_get_copyfrom_info(NULL,
                                               &local_copyfrom_repos_relpath,
                                               NULL,
                                               &local_copyfrom_rev,
                                               NULL,
                                               eb->wc_ctx,
                                               fb->local_abspath,
                                               subpool, subpool));


      /* Is there something that is a file? */
      local_is_file = ((wc_kind == svn_wc__db_kind_file
                        || wc_kind == svn_wc__db_kind_symlink) 
                       && status != svn_wc__db_status_deleted);

      /* Is there *something* that is not a file? */
      local_is_non_file = ((wc_kind == svn_wc__db_kind_dir
                            || wc_kind == svn_wc__db_kind_unknown)
                           && status != svn_wc__db_status_deleted);

      if (local_is_file)
        {
          svn_boolean_t wc_root, switched;

          SVN_ERR(svn_wc__check_wc_root(&wc_root, NULL, &switched,
                                        eb->db, fb->local_abspath, pool));

          err = NULL;

          if (wc_root)
            {
              err = svn_error_createf(
                         SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
                         _("Failed to add file '%s': a file "
                           "from another repository with the same name "
                           "already exists"),
                         svn_dirent_local_style(fb->local_abspath, pool));
            }

          if (switched && !eb->switch_relpath)
            {
              err = svn_error_createf(
                         SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
                         _("Switched file '%s' does not match "
                           "expected URL '%s'"),
                         svn_dirent_local_style(fb->local_abspath, pool),
                         svn_path_url_add_component2(eb->repos_root,
                                                     fb->new_relpath, pool));
            }

          if (err != NULL)
            {
              fb->already_notified = TRUE;
              do_notification(eb, fb->local_abspath, svn_node_file,
                              svn_wc_notify_update_obstruction, pool);

              return svn_error_return(err);
            }
        }

      /* We can't properly handle add vs. add with mismatching
       * node kinds before single db. */
      if (local_is_non_file)
        {
          return svn_error_createf(
                           SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
                           _("Failed to add file '%s': a non-file object "
                             "of the same name already exists"),
                           svn_dirent_local_style(fb->local_abspath,
                                                  pool));
        }

      /* Find out if this is a file external, because we want to allow pulling
       * in a file external onto an existing node -- because that's how
       * externals are currently implemented. :( */
      err = svn_wc__node_is_file_external(&is_file_external, eb->wc_ctx,
                                          fb->local_abspath, subpool);
      if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
        {
          svn_error_clear(err);
          is_file_external = FALSE;
        }
      else
        SVN_ERR(err);

      /* Do tree conflict checking if
       *  - if a copy is involved on either side, except if both are copies
       *    from the same URL and revnum.
       *  - if this is a switch operation
       *  - if we are not busy fetching externals
       *  - the node kinds mismatch (when single db is here)
       * IOW, do no tree conflict checking if during update both sides'
       * histories match (both simple adds or both copies from the same
       * URL@REV); don't flag tree conflicts on externals, being handled
       * elsewhere.
       *
       * During switch, local adds at the same path as incoming adds get
       * "lost" in that switching back to the original will no longer have the
       * local add. So switch always alerts the user with a tree conflict.
       *
       * Allow pulling absent/exluded/not_present nodes back in.
       *
       * ### This code is already gearing up to single db with respect to
       * add-vs.-add conflicts with mismatching node kinds. But before single
       * db, we cannot deal with mismatching node kinds properly.
       *
       * ### Checking the copyfrom_path is bogus during checkout and when
       * contacting older servers, because then, there is no incoming copy
       * information. In those cases svn still acts like 1.6 did, like it used
       * to -- fails to flag same-kind tree conflicts with local-add vs.
       * incoming-copy, and fails to not flag conflicts on matching copies. */
      if (! pb->in_deleted_and_tree_conflicted_subtree
          && ! is_file_external
          && (eb->switch_relpath != NULL
              || local_is_non_file
              || ((copyfrom_path != NULL 
                   || local_copyfrom_repos_relpath != NULL)
                  &&
                  ! (copyfrom_path != NULL
                     && local_copyfrom_repos_relpath != NULL
                     && strcmp(local_copyfrom_repos_relpath,
                               copyfrom_relpath) == 0
                     && local_copyfrom_rev == copyfrom_rev)
                 )
             )
         )
        {
          SVN_ERR(check_tree_conflict(&tree_conflict, eb,
                                      fb->local_abspath,
                                      svn_wc_conflict_action_add,
                                      svn_node_file, fb->new_relpath,
                                      subpool));
        }


      if (tree_conflict == NULL)
        /* We have a node in WORKING and we've decided not to flag a
         * conflict, so merge it with the incoming add. */
        fb->add_existed = TRUE;
      else
        /* We have a tree conflict of a local add vs. an incoming add.
         * We want to update BASE only, scheduling WORKING as a replace
         * of BASE so that WORKING/ACTUAL stay unchanged. */
        fb->adding_base_under_local_add = TRUE;

    }
  else if (kind != svn_node_none)
    {
      /* There's an unversioned node at this path. */
      fb->obstruction_found = TRUE;

      /* Unversioned, obstructing files are handled by text merge/conflict,
       * if unversioned obstructions are allowed. */
      if (! (kind == svn_node_file && eb->allow_unver_obstructions))
        {
          /* ### Instead of skipping, this should bring in the BASE node
           * and mark some sort of obstruction-conflict. Come, o single-db! */
          fb->skip_this = TRUE;

          /* If we are skipping an add, we need to tell the WC that
           * there's a node supposed to be here which we don't have. */
          SVN_ERR(svn_wc__db_base_add_absent_node(eb->db, fb->local_abspath,
                                                  fb->new_relpath,
                                                  eb->repos_root,
                                                  eb->repos_uuid,
                                                  (eb->target_revision?
                                                   *eb->target_revision
                                                   : SVN_INVALID_REVNUM),
                                                  svn_wc__db_kind_file,
                                                  svn_wc__db_status_not_present,
                                                  NULL, NULL, subpool));
          SVN_ERR(remember_skipped_tree(eb, fb->local_abspath));

          /* Mark a conflict */
          SVN_ERR(create_tree_conflict(&tree_conflict, eb,
                                       fb->local_abspath,
                                       svn_wc_conflict_reason_unversioned,
                                       svn_wc_conflict_action_add,
                                       svn_node_file,
                                       fb->new_relpath, subpool, subpool));
          SVN_ERR_ASSERT(tree_conflict != NULL);
        }
    }

  if (tree_conflict != NULL)
    {
      svn_skel_t *work_item;

      fb->obstruction_found = TRUE;

      SVN_ERR(svn_wc__loggy_add_tree_conflict(&work_item,
                                              eb->db,
                                              pb->local_abspath,
                                              tree_conflict,
                                              subpool));
      SVN_ERR(svn_wc__db_wq_add(eb->db, pb->local_abspath,
                                work_item, pool));

      fb->already_notified = TRUE;
      do_notification(eb, fb->local_abspath, svn_node_unknown,
                      svn_wc_notify_tree_conflict, subpool);
    }

  /* Now, if this is an add with history, do the history part. */
  if (copyfrom_path && !fb->skip_this)
    {
      SVN_ERR(add_file_with_history(pb, copyfrom_path, copyfrom_rev,
                                    fb, pool, subpool));
    }

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
static svn_error_t *
open_file(const char *path,
          void *parent_baton,
          svn_revnum_t base_revision,
          apr_pool_t *pool,
          void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  struct file_baton *fb;
  svn_node_kind_t kind;
  svn_boolean_t already_conflicted;
  svn_wc_conflict_description2_t *tree_conflict = NULL;

  /* the file_pool can stick around for a *long* time, so we want to use
     a subpool for any temporary allocations. */
  apr_pool_t *subpool = svn_pool_create(pool);

  SVN_ERR(make_file_baton(&fb, pb, path, FALSE, pool));
  *file_baton = fb;

  if (pb->skip_descendants)
    {
      if (!pb->skip_this)
        SVN_ERR(remember_skipped_tree(eb, fb->local_abspath));

      fb->skip_this = TRUE;
      fb->already_notified = TRUE;

      return SVN_NO_ERROR;
    }

  SVN_ERR(check_path_under_root(pb->local_abspath, fb->name, subpool));

  SVN_ERR(svn_io_check_path(fb->local_abspath, &kind, subpool));

  /* Sanity check. */

  /* If replacing, make sure the .svn entry already exists. */
  SVN_ERR(svn_wc__db_read_info(NULL, NULL, &fb->old_revision, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL,
                               eb->db, fb->local_abspath, subpool, subpool));

  /* Is this path a conflict victim? */
  SVN_ERR(node_already_conflicted(&already_conflicted, eb->db,
                                  fb->local_abspath, pool));
  if (already_conflicted)
    {
      SVN_ERR(remember_skipped_tree(eb, fb->local_abspath));

      fb->skip_this = TRUE;
      fb->already_notified = TRUE;

      do_notification(eb, fb->local_abspath, svn_node_unknown,
                      svn_wc_notify_skip, subpool);

      svn_pool_destroy(subpool);

      return SVN_NO_ERROR;
    }

  fb->deleted = pb->in_deleted_and_tree_conflicted_subtree;

  /* Check for conflicts only when we haven't already recorded
   * a tree-conflict on a parent node. */
  if (!pb->in_deleted_and_tree_conflicted_subtree)
    SVN_ERR(check_tree_conflict(&tree_conflict, eb, fb->local_abspath,
                                svn_wc_conflict_action_edit, svn_node_file,
                                fb->new_relpath, pool));

  /* Is this path the victim of a newly-discovered tree conflict? */
  if (tree_conflict)
    {
      svn_skel_t *work_item;

      SVN_ERR(svn_wc__loggy_add_tree_conflict(&work_item,
                                              eb->db, pb->local_abspath,
                                              tree_conflict, pool));
      SVN_ERR(svn_wc__db_wq_add(eb->db, pb->local_abspath, work_item, pool));

      if (tree_conflict->reason == svn_wc_conflict_reason_deleted ||
          tree_conflict->reason == svn_wc_conflict_reason_replaced)
        {
          fb->deleted = TRUE;
        }
      else
        SVN_ERR(remember_skipped_tree(eb, fb->local_abspath));

      if (!fb->deleted)
        fb->skip_this = TRUE;

      fb->already_notified = TRUE;
      do_notification(eb, fb->local_abspath, svn_node_unknown,
                      svn_wc_notify_tree_conflict, pool);
    }

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
apply_textdelta(void *file_baton,
                const char *expected_base_checksum,
                apr_pool_t *pool,
                svn_txdelta_window_handler_t *handler,
                void **handler_baton)
{
  struct file_baton *fb = file_baton;
  apr_pool_t *handler_pool = svn_pool_create(fb->pool);
  struct handler_baton *hb = apr_pcalloc(handler_pool, sizeof(*hb));
  svn_error_t *err;
  const char *recorded_base_checksum;
  svn_stream_t *source;
  svn_stream_t *target;

  if (fb->skip_this)
    {
      *handler = svn_delta_noop_window_handler;
      *handler_baton = NULL;
      return SVN_NO_ERROR;
    }

  fb->received_textdelta = TRUE;

  /* Before applying incoming svndiff data to text base, make sure
     text base hasn't been corrupted, and that its checksum
     matches the expected base checksum. */

  /* The incoming delta is targeted against EXPECTED_BASE_CHECKSUM. Find and
     check our RECORDED_BASE_CHECKSUM.  (In WC-1, we could not do this test
     for replaced nodes because we didn't store the checksum of the "revert
     base".  In WC-NG, we do and we can.) */
  {
    const svn_checksum_t *checksum;

    SVN_ERR(svn_wc__get_ultimate_base_checksums(NULL, &checksum,
                                                fb->edit_baton->db,
                                                fb->local_abspath,
                                                pool, pool));
    recorded_base_checksum
      = checksum ? svn_checksum_to_cstring(checksum, pool) : NULL;
    if (recorded_base_checksum && expected_base_checksum
        && strcmp(expected_base_checksum, recorded_base_checksum) != 0)
      return svn_error_createf(SVN_ERR_WC_CORRUPT_TEXT_BASE, NULL,
                     _("Checksum mismatch for '%s':\n"
                       "   expected:  %s\n"
                       "   recorded:  %s\n"),
                     svn_dirent_local_style(fb->local_abspath, pool),
                     expected_base_checksum, recorded_base_checksum);
  }

  /* Open the text base for reading, unless this is an added file. */

  /*
     kff todo: what we really need to do here is:

     1. See if there's a file or dir by this name already here.
     2. See if it's under revision control.
     3. If both are true, open text-base.
     4. If only 1 is true, bail, because we can't go destroying user's
        files (or as an alternative to bailing, move it to some tmp
        name and somehow tell the user, but communicating with the
        user without erroring is a whole callback system we haven't
        finished inventing yet.)
  */

  if (! fb->adding_file)
    {
      SVN_ERR(svn_wc__get_ultimate_base_contents(&source, fb->edit_baton->db,
                                                 fb->local_abspath,
                                                 handler_pool, handler_pool));
      if (source == NULL)
        source = svn_stream_empty(handler_pool);
    }
  else
    {
      if (fb->copied_text_base_sha1_checksum)
        SVN_ERR(svn_wc__db_pristine_read(&source, fb->edit_baton->db,
                                         fb->local_abspath,
                                         fb->copied_text_base_sha1_checksum,
                                         handler_pool, handler_pool));
      else
        source = svn_stream_empty(handler_pool);
    }

  /* If we don't have a recorded checksum, use the ra provided checksum */
  if (!recorded_base_checksum)
    recorded_base_checksum = expected_base_checksum;

  /* Checksum the text base while applying deltas */
  if (recorded_base_checksum)
    {
      SVN_ERR(svn_checksum_parse_hex(&hb->expected_source_md5_checksum,
                                     svn_checksum_md5, recorded_base_checksum,
                                     handler_pool));

      /* Wrap stream and store reference to allow calculating the md5 */
      source = svn_stream_checksummed2(source,
                                       &hb->actual_source_md5_checksum,
                                       NULL, svn_checksum_md5,
                                       TRUE, handler_pool);
      hb->source_checksum_stream = source;
    }

  /* Open the text base for writing (this will get us a temporary file).  */
  err = svn_wc__open_writable_base(&target, &hb->new_text_base_tmp_abspath,
                                   NULL, &hb->new_text_base_sha1_checksum,
                                   fb->edit_baton->db, fb->local_abspath,
                                   handler_pool, pool);
  if (err)
    {
      svn_pool_destroy(handler_pool);
      return svn_error_return(err);
    }

  /* Prepare to apply the delta.  */
  svn_txdelta_apply(source, target,
                    hb->new_text_base_md5_digest,
                    hb->new_text_base_tmp_abspath /* error_info */,
                    handler_pool,
                    &hb->apply_handler, &hb->apply_baton);

  hb->pool = handler_pool;
  hb->fb = fb;

  /* We're all set.  */
  *handler_baton = hb;
  *handler = window_handler;

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
static svn_error_t *
change_file_prop(void *file_baton,
                 const char *name,
                 const svn_string_t *value,
                 apr_pool_t *scratch_pool)
{
  struct file_baton *fb = file_baton;
  struct edit_baton *eb = fb->edit_baton;
  svn_prop_t *propchange;

  if (fb->skip_this)
    return SVN_NO_ERROR;

  /* Push a new propchange to the file baton's array of propchanges */
  propchange = apr_array_push(fb->propchanges);
  propchange->name = apr_pstrdup(fb->pool, name);
  propchange->value = value ? svn_string_dup(value, fb->pool) : NULL;

  /* Special case: If use-commit-times config variable is set we
     cache the last-changed-date propval so we can use it to set
     the working file's timestamp. */
  if (value
      && eb->use_commit_times
      && (strcmp(name, SVN_PROP_ENTRY_COMMITTED_DATE) == 0))
    {
      /* propchange is already in the right pool */
      fb->last_changed_date = propchange->value->data;
    }

  return SVN_NO_ERROR;
}


/* This is the small planet.  It has the complex responsibility of
 * "integrating" a new revision of a file into a working copy.
 *
 * Given a file_baton FB for a file either already under version control, or
 * prepared (see below) to join version control, fully install a
 * new revision of the file.
 *
 * ### transitional: installation of the working file will be handled
 * ### by the *INSTALL_PRISTINE flag.
 *
 * By "install", we mean: create a new text-base and prop-base, merge
 * any textual and property changes into the working file, and finally
 * update all metadata so that the working copy believes it has a new
 * working revision of the file.  All of this work includes being
 * sensitive to eol translation, keyword substitution, and performing
 * all actions accumulated the parent directory's work queue.
 *
 * If there's a new text base, it must be in the pristine store and
 * NEW_TEXT_BASE_SHA1_CHECKSUM must be its SHA-1 checksum (else NULL).
 * After this function returns, the caller should install it as the new
 * text base for this file.
 *
 * Set *CONTENT_STATE to the state of the contents after the
 * installation.
 *
 * Return values are allocated in RESULT_POOL and temporary allocations
 * are performed in SCRATCH_POOL.
 */
static svn_error_t *
merge_file(svn_skel_t **work_items,
           svn_boolean_t *install_pristine,
           const char **install_from,
           svn_wc_notify_state_t *content_state,
           struct file_baton *fb,
           const svn_checksum_t *new_text_base_sha1_checksum,
           apr_pool_t *result_pool,
           apr_pool_t *scratch_pool)
{
  struct edit_baton *eb = fb->edit_baton;
  struct dir_baton *pb = fb->dir_baton;
  svn_boolean_t is_locally_modified;
  svn_boolean_t is_replaced = FALSE;
  svn_boolean_t magic_props_changed;
  enum svn_wc_merge_outcome_t merge_outcome = svn_wc_merge_unchanged;
  svn_skel_t *work_item;
  const char *new_text_base_tmp_abspath;
  svn_wc__db_t *db = eb->db;
  apr_pool_t *pool = result_pool;
  svn_boolean_t file_exists = TRUE;
  svn_wc__db_status_t status;
  svn_boolean_t have_base;
  svn_revnum_t revision;
  const char *file_external = NULL;
  svn_error_t *err;

  /*
     When this function is called on file F, we assume the following
     things are true:

         - The new pristine text of F is present in the pristine store
           iff NEW_TEXT_BASE_SHA1_CHECKSUM is not NULL.

         - The WC metadata still reflects the old version of F.
           (We can still access the old pristine base text of F.)

     The goal is to update the local working copy of F to reflect
     the changes received from the repository, preserving any local
     modifications.
  */

  *work_items = NULL;
  *install_pristine = FALSE;
  *install_from = NULL;

  if (new_text_base_sha1_checksum != NULL)
    SVN_ERR(svn_wc__db_pristine_get_path(&new_text_base_tmp_abspath,
                                         eb->db, fb->local_abspath,
                                         new_text_base_sha1_checksum,
                                         pool, scratch_pool));
  else
    new_text_base_tmp_abspath = NULL;

  err = svn_wc__db_read_info(&status, NULL, &revision, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, &have_base, NULL,
                             NULL, NULL,
                             db, fb->local_abspath,
                             scratch_pool, scratch_pool);

  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      svn_error_clear(err);
      file_exists = FALSE;

      status = svn_wc__db_status_not_present;
      revision = SVN_INVALID_REVNUM;
      have_base = FALSE;
    }
  else
    SVN_ERR(err);

  if (file_exists)
    SVN_ERR(svn_wc__db_temp_get_file_external(&file_external, eb->db,
                                              fb->local_abspath,
                                              scratch_pool, scratch_pool));

  /* Start by splitting the file path, getting an access baton for the parent,
     and an entry for the file if any. */

  /* Determine if any of the propchanges are the "magic" ones that
     might require changing the working file. */
  magic_props_changed = svn_wc__has_magic_property(fb->propchanges);

  /* Has the user made local mods to the working file?
     Note that this compares to the current pristine file, which is
     different from fb->old_text_base_path if we have a replaced-with-history
     file.  However, in the case we had an obstruction, we check against the
     new text base. (And if we're doing an add-with-history and we've already
     saved a copy of a locally-modified file, then there certainly are mods.)

     Special case: The working file is referring to a file external? If so
                   then we must mark it as unmodified in order to avoid bogus
                   conflicts, since this file was added as a place holder to
                   merge externals item from the repository.

     ### Newly added file externals have a svn_wc_schedule_add here. */
  if (fb->copied_working_text)
    {
      /* The file was copied here, and it came with both a (new) pristine
         and a working file. Presumably, the working file is modified
         relative to the new pristine.

         The new pristine is in NEW_TEXT_BASE_TMP_ABSPATH, which should also
         be FB->COPIED_TEXT_BASE_ABSPATH.  */
      is_locally_modified = TRUE;
    }
  else if (file_external &&
           status ==svn_wc__db_status_added)
    {
      is_locally_modified = FALSE; /* ### Or a conflict will be raised */
    }
  else if (! fb->obstruction_found)
    {
      /* The working file is not an obstruction. So: is the file modified,
         relative to its ORIGINAL pristine?  */
      SVN_ERR(svn_wc__internal_text_modified_p(&is_locally_modified, eb->db,
                                               fb->local_abspath,
                                               FALSE /* force_comparison */,
                                               FALSE /* compare_textbases */,
                                               pool));
    }
  else if (new_text_base_sha1_checksum && !fb->obstruction_found)
    {
      svn_stream_t *pristine_stream;

      /* We have a new pristine to install. Is the file modified relative
         to this new pristine?  */
      SVN_ERR(svn_wc__db_pristine_read(&pristine_stream,
                                       eb->db, fb->local_abspath,
                                       new_text_base_sha1_checksum,
                                       pool, pool));
      SVN_ERR(svn_wc__internal_versioned_file_modcheck(&is_locally_modified,
                                                       eb->db,
                                                       fb->local_abspath,
                                                       pristine_stream,
                                                       FALSE, pool));
    }
  else
    {
      /* No other potential changes, so the working file is NOT modified.
         Except when we have a local obstruction! */

      if (fb->obstruction_found)
        is_locally_modified = TRUE;
      else
        is_locally_modified = FALSE;
    }

  if (have_base)
    {
      svn_wc__db_status_t base_status;

      SVN_ERR(svn_wc__db_base_get_info(&base_status, NULL, &revision, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL,
                                       db, fb->local_abspath,
                                       scratch_pool, scratch_pool));

      if (status == svn_wc__db_status_added
          && base_status != svn_wc__db_status_not_present)
        is_replaced = TRUE;
    }

  /* For 'textual' merging, we implement this matrix.

                                 Text file                  Binary File
                               -----------------------------------------------
    "Local Mods" &&            | svn_wc_merge uses diff3, | svn_wc_merge     |
    (!fb->obstruction_found || | possibly makes backups & | makes backups,   |
     fb->add_existed)          | marks file as conflicted.| marks conflicted |
                               -----------------------------------------------
    "Local Mods" &&            |        Just leave obstructing file as-is.   |
    fb->obstruction_found      |                                             |
                               -----------------------------------------------
    No Mods                    |        Just overwrite working file.         |
                               -----------------------------------------------
    File is Locally            |        Same as if 'No Mods' except we       |
    Deleted                    |        don't copy the new text base to      |
                               |        the working file location.           |
                               -----------------------------------------------
    File is Locally            |        Install the new text base.           |
    Replaced                   |        Leave working file alone.            |
                               -----------------------------------------------

   So the first thing we do is figure out where we are in the
   matrix. */

  if (new_text_base_sha1_checksum)
    {
      if (is_replaced)
        {
          /* Nothing to do, the delete half of the local replacement will
             have already raised a tree conflict.  So we will just fall
             through to the installation of the new textbase. */
        }
      else if (! is_locally_modified)
        {
          if (!fb->deleted)
            {
              /* If there are no local mods, who cares whether it's a text
                 or binary file!  Just write a log command to overwrite
                 any working file with the new text-base.  If newline
                 conversion or keyword substitution is activated, this
                 will happen as well during the copy.
                 For replaced files, though, we want to merge in the changes
                 even if the file is not modified compared to the (non-revert)
                 text-base. */

              *install_pristine = TRUE;

              /* ### sheesh. for file externals, there is a WORKING_NODE
                 ### row (during this transitional state), which means the
                 ### node is reported as "added". further, this means
                 ### that the text base will be dropped into the "revert
                 ### base". even after everything stabilizes, the file
                 ### external's base will continue to reside in the revert
                 ### base, but the rest of libsvn_wc appears to compensate
                 ### for this fact (even tho it is schedule_normal!!).
                 ### in any case, let's do the working copy file install
                 ### from the revert base for file externals.  */
              if (file_external)
                {
                  SVN_ERR_ASSERT(status == svn_wc__db_status_added);

                  /* The revert-base will be installed later in this function.
                     To tell the caller to install the new working text from
                     the (revert-)base file, we leave INSTALL_FROM as NULL. */
                }
            }
        }
      else   /* working file or obstruction is locally modified... */
        {
          svn_node_kind_t wfile_kind;

          SVN_ERR(svn_io_check_path(fb->local_abspath, &wfile_kind, pool));
          if (wfile_kind == svn_node_none && ! fb->added_with_history)
            {
              /* working file is missing?!
                 Just copy the new text-base to the file. */
              *install_pristine = TRUE;
            }
          else if (! fb->obstruction_found)
            /* Working file exists and has local mods
               or is scheduled for addition but is not an obstruction. */
            {
              /* Now we need to let loose svn_wc__merge_internal() to merge
                 the textual changes into the working file. */
              const char *oldrev_str, *newrev_str, *mine_str;
              const char *merge_left;
              svn_boolean_t delete_left = FALSE;
              const char *path_ext = "";

              /* If we have any file extensions we're supposed to
                 preserve in generated conflict file names, then find
                 this path's extension.  But then, if it isn't one of
                 the ones we want to keep in conflict filenames,
                 pretend it doesn't have an extension at all. */
              if (eb->ext_patterns && eb->ext_patterns->nelts)
                {
                  svn_path_splitext(NULL, &path_ext, fb->local_abspath, pool);
                  if (! (*path_ext
                         && svn_cstring_match_glob_list(path_ext,
                                                        eb->ext_patterns)))
                    path_ext = "";
                }

              /* Create strings representing the revisions of the
                 old and new text-bases. */
              /* Either an old version, or an add-with-history */
              if (fb->added_with_history)
                oldrev_str = apr_psprintf(pool, ".copied%s%s",
                                          *path_ext ? "." : "",
                                          *path_ext ? path_ext : "");
              else
                {
                  svn_revnum_t old_rev = revision;

                  /* ### BH: Why is this necessary? */
                  if (!SVN_IS_VALID_REVNUM(old_rev))
                    old_rev = 0;

                  oldrev_str = apr_psprintf(pool, ".r%ld%s%s",
                                            old_rev,
                                            *path_ext ? "." : "",
                                            *path_ext ? path_ext : "");
                }
              newrev_str = apr_psprintf(pool, ".r%ld%s%s",
                                        *eb->target_revision,
                                        *path_ext ? "." : "",
                                        *path_ext ? path_ext : "");
              mine_str = apr_psprintf(pool, ".mine%s%s",
                                      *path_ext ? "." : "",
                                      *path_ext ? path_ext : "");

              if (fb->add_existed && ! is_replaced)
                {
                  SVN_ERR(get_empty_tmp_file(&merge_left, eb->db,
                                             pb->local_abspath,
                                             pool, pool));
                  delete_left = TRUE;
                }
              else if (fb->copied_text_base_sha1_checksum)
                SVN_ERR(svn_wc__db_pristine_get_path(&merge_left, eb->db,
                                                     fb->local_abspath,
                                                     fb->copied_text_base_sha1_checksum,
                                                     pool, pool));
              else
                SVN_ERR(svn_wc__ultimate_base_text_path_to_read(
                  &merge_left, eb->db, fb->local_abspath, pool, pool));

              /* Merge the changes from the old textbase to the new
                 textbase into the file we're updating.
                 Remember that this function wants full paths! */
              /* ### TODO: Pass version info here. */
              /* ### NOTE: if this call bails out, then we must ensure
                 ###   that no work items have been queued which might
                 ###   place this file into an inconsistent state.
                 ###   in the future, all the state changes should be
                 ###   made atomically.  */
              SVN_ERR(svn_wc__internal_merge(
                        &work_item,
                        &merge_outcome,
                        eb->db,
                        merge_left, NULL,
                        new_text_base_tmp_abspath, NULL,
                        fb->local_abspath,
                        fb->copied_working_text,
                        oldrev_str, newrev_str, mine_str,
                        FALSE /* dry_run */,
                        eb->diff3_cmd, NULL, fb->propchanges,
                        eb->conflict_func, eb->conflict_baton,
                        eb->cancel_func, eb->cancel_baton,
                        pool, pool));
              *work_items = svn_wc__wq_merge(*work_items, work_item, pool);

              /* If we created a temporary left merge file, get rid of it. */
              if (delete_left)
                {
                  SVN_ERR(svn_wc__wq_build_file_remove(&work_item,
                                                       eb->db, merge_left,
                                                       pool, pool));
                  *work_items = svn_wc__wq_merge(*work_items, work_item, pool);
                }

              /* And clean up add-with-history-related temp file too. */
              if (fb->copied_working_text)
                {
                  SVN_ERR(svn_wc__wq_build_file_remove(&work_item,
                                                       eb->db,
                                                       fb->copied_working_text,
                                                       pool, pool));
                  *work_items = svn_wc__wq_merge(*work_items, work_item, pool);
                }
            } /* end: working file exists and has mods */
        } /* end: working file has mods */
    } /* end: "textual" merging process */
  else
    {
      /* There is no new text base, but let's see if the working file needs
         to be updated for any other reason. */

      apr_hash_t *keywords;

      SVN_ERR(svn_wc__get_translate_info(NULL, NULL,
                                         &keywords,
                                         NULL,
                                         eb->db, fb->local_abspath,
                                         pool, pool));
      if (magic_props_changed || keywords)
        {
          /* Special edge-case: it's possible that this file installation
             only involves propchanges, but that some of those props still
             require a retranslation of the working file.

             OR that the file doesn't involve propchanges which by themselves
             require retranslation, but receiving a change bumps the revision
             number which requires re-expansion of keywords... */

          const char *tmptext;

          /* Copy and DEtranslate the working file to a temp text-base.
             Note that detranslation is done according to the old props. */
          SVN_ERR(svn_wc__internal_translated_file(
                    &tmptext, fb->local_abspath, eb->db, fb->local_abspath,
                    SVN_WC_TRANSLATE_TO_NF
                      | SVN_WC_TRANSLATE_NO_OUTPUT_CLEANUP,
                    eb->cancel_func, eb->cancel_baton,
                    pool, pool));

          /* We always want to reinstall the working file if the magic
             properties have changed, or there are any keywords present.
             Note that TMPTEXT might actually refer to the working file
             itself (the above function skips a detranslate when not
             required). This is acceptable, as we will (re)translate
             according to the new properties into a temporary file (from
             the working file), and then rename the temp into place. Magic!  */
          *install_pristine = TRUE;
          *install_from = tmptext;
        }
    }

  /* Installing from a pristine will handle timestamps and recording.
     However, if we are NOT creating a new working copy file, then create
     work items to handle text-timestamp and working-size.  */
  if (!*install_pristine
      && !is_locally_modified
      && (fb->adding_file || status == svn_wc__db_status_normal))
    {
      apr_time_t set_date = 0;
      /* Adjust working copy file unless this file is an allowed
         obstruction. */
      if (fb->last_changed_date && !fb->obstruction_found)
        {
          /* Ignore invalid dates */
          err = svn_time_from_cstring(&set_date, fb->last_changed_date,
                                      pool);

          if (err)
            {
              svn_error_clear(err);
              set_date = 0;
            }
        }

      /* If this would have been an obstruction, we wouldn't be here,
         because we would have installed an obstruction or tree conflict
         instead */
      SVN_ERR(svn_wc__wq_build_record_fileinfo(&work_item,
                                               fb->local_abspath,
                                               set_date,
                                               pool));
      *work_items = svn_wc__wq_merge(*work_items, work_item, pool);
    }

  /* Set the returned content state. */

  /* This is kind of interesting.  Even if no new text was
     installed (i.e., NEW_TEXT_BASE_ABSPATH was null), we could still
     report a pre-existing conflict state.  Say a file, already
     in a state of textual conflict, receives prop mods during an
     update.  Then we'll notify that it has text conflicts.  This
     seems okay to me.  I guess.  I dunno.  You? */

  if (merge_outcome == svn_wc_merge_conflict)
    *content_state = svn_wc_notify_state_conflicted;
  else if (new_text_base_sha1_checksum)
    {
      if (is_locally_modified)
        *content_state = svn_wc_notify_state_merged;
      else
        *content_state = svn_wc_notify_state_changed;
    }
  else
    *content_state = svn_wc_notify_state_unchanged;

  return SVN_NO_ERROR;
}


/* An svn_delta_editor_t function. */
/* Mostly a wrapper around merge_file. */
static svn_error_t *
close_file(void *file_baton,
           const char *expected_md5_digest,
           apr_pool_t *pool)
{
  struct file_baton *fb = file_baton;
  struct edit_baton *eb = fb->edit_baton;
  svn_wc_notify_state_t content_state, prop_state;
  svn_wc_notify_lock_state_t lock_state;
  svn_checksum_t *expected_md5_checksum = NULL;
  svn_checksum_t *new_text_base_md5_checksum;
  svn_checksum_t *new_text_base_sha1_checksum;
  apr_hash_t *new_base_props = NULL;
  apr_hash_t *new_actual_props = NULL;
  apr_array_header_t *entry_props;
  apr_array_header_t *dav_props;
  apr_array_header_t *regular_props;
  svn_boolean_t install_pristine;
  const char *install_from = NULL;
  apr_hash_t *current_base_props = NULL;
  apr_hash_t *current_actual_props = NULL;
  apr_hash_t *local_actual_props = NULL;
  svn_skel_t *all_work_items = NULL;
  svn_skel_t *work_item;
  svn_revnum_t new_changed_rev;
  apr_time_t new_changed_date;
  svn_node_kind_t kind;
  const char *new_changed_author;
  apr_pool_t *scratch_pool = fb->pool; /* Destroyed at function exit */

  if (fb->skip_this)
    {
      SVN_ERR(maybe_bump_dir_info(eb, fb->bump_info, pool));
      svn_pool_destroy(fb->pool);
      return SVN_NO_ERROR;
    }

  if (expected_md5_digest)
    SVN_ERR(svn_checksum_parse_hex(&expected_md5_checksum, svn_checksum_md5,
                                   expected_md5_digest, pool));

  /* Retrieve the new text-base file's path and checksums.  If it was an
   * add-with-history, with no apply_textdelta, then that means the text-base
   * of the copied file, else the new text-base created by apply_textdelta(),
   * if any. */
  if (fb->received_textdelta)
    {
      new_text_base_md5_checksum = fb->new_text_base_md5_checksum;
      new_text_base_sha1_checksum = fb->new_text_base_sha1_checksum;
      SVN_ERR_ASSERT(new_text_base_md5_checksum &&
                     new_text_base_sha1_checksum);
    }
  else if (fb->added_with_history)
    {
      SVN_ERR_ASSERT(! fb->new_text_base_sha1_checksum);
      new_text_base_md5_checksum = fb->copied_text_base_md5_checksum;
      new_text_base_sha1_checksum = fb->copied_text_base_sha1_checksum;
      SVN_ERR_ASSERT(new_text_base_md5_checksum &&
                     new_text_base_sha1_checksum);
    }
  else
    {
      SVN_ERR_ASSERT(! fb->new_text_base_sha1_checksum
                     && ! fb->copied_text_base_sha1_checksum);
      new_text_base_md5_checksum = NULL;
      new_text_base_sha1_checksum = NULL;
    }

  if (new_text_base_md5_checksum && expected_md5_checksum
      && !svn_checksum_match(expected_md5_checksum, new_text_base_md5_checksum))
    return svn_error_createf(SVN_ERR_CHECKSUM_MISMATCH, NULL,
            _("Checksum mismatch for '%s':\n"
              "   expected:  %s\n"
              "     actual:  %s\n"),
            svn_dirent_local_style(fb->local_abspath, pool),
            expected_md5_digest,
            svn_checksum_to_cstring_display(new_text_base_md5_checksum, pool));

  SVN_ERR(svn_wc_read_kind(&kind, eb->wc_ctx, fb->local_abspath, TRUE, pool));
  if (kind == svn_node_none && ! fb->adding_file)
    return svn_error_createf(SVN_ERR_UNVERSIONED_RESOURCE, NULL,
                             _("'%s' is not under version control"),
                             svn_dirent_local_style(fb->local_abspath, pool));

  /* Gather the changes for each kind of property.  */
  SVN_ERR(svn_categorize_props(fb->propchanges, &entry_props, &dav_props,
                               &regular_props, pool));

  /* Extract the changed_* and lock state information.  */
  SVN_ERR(accumulate_last_change(&new_changed_rev,
                                 &new_changed_date,
                                 &new_changed_author,
                                 eb->db, fb->local_abspath,
                                 entry_props,
                                 pool, pool));

  /* Determine whether the file has become unlocked.  */
  {
    int i;

    lock_state = svn_wc_notify_lock_state_unchanged;

    for (i = 0; i < entry_props->nelts; ++i)
      {
        const svn_prop_t *prop = &APR_ARRAY_IDX(entry_props, i, svn_prop_t);

        /* If we see a change to the LOCK_TOKEN entry prop, then the only
           possible change is its REMOVAL. Thus, the lock has been removed,
           and we should likewise remove our cached copy of it.  */
        if (! strcmp(prop->name, SVN_PROP_ENTRY_LOCK_TOKEN))
          {
            SVN_ERR_ASSERT(prop->value == NULL);
            SVN_ERR(svn_wc__db_lock_remove(eb->db, fb->local_abspath, pool));

            lock_state = svn_wc_notify_lock_state_unlocked;
            break;
          }
      }
  }

  /* Install all kinds of properties.  It is important to do this before
     any file content merging, since that process might expand keywords, in
     which case we want the new entryprops to be in place. */

  /* Write log commands to merge REGULAR_PROPS into the existing
     properties of FB->LOCAL_ABSPATH.  Update *PROP_STATE to reflect
     the result of the regular prop merge.

     BASE_PROPS and WORKING_PROPS are hashes of the base and
     working props of the file; if NULL they are read from the wc.  */

  /* ### some of this feels like voodoo... */

  if (kind != svn_node_none)
    SVN_ERR(svn_wc__get_actual_props(&local_actual_props,
                                     eb->db, fb->local_abspath,
                                     pool, pool));
  if (local_actual_props == NULL)
    local_actual_props = apr_hash_make(pool);


  if (fb->copied_base_props)
    {
      /* The BASE props are given by the source of the copy. We may also
         have some ACTUAL props if the server directed us to copy a path
         located in our WC which had some ACTUAL changes.  */
      current_base_props = fb->copied_base_props;
      current_actual_props = fb->copied_working_props;
    }
  else if (kind != svn_node_none)
    {
      /* This node already exists. Grab its properties. */
      SVN_ERR(svn_wc__get_pristine_props(&current_base_props,
                                         eb->db, fb->local_abspath,
                                         pool, pool));
      current_actual_props = local_actual_props;
    }

  /* Note: even if the node existed before, it may not have
     pristine props (e.g a local-add)  */
  if (current_base_props == NULL)
    current_base_props = apr_hash_make(pool);

  /* And new nodes need an empty set of ACTUAL props.  */
  if (current_actual_props == NULL)
    current_actual_props = apr_hash_make(pool);

  /* Catch symlink-ness change.
   * add_file() doesn't know whether the incoming added node is a file or
   * a symlink, because symlink-ness is saved in a prop :(
   * So add_file() cannot notice when update wants to add a symlink where
   * locally there already is a file scheduled for addition, or vice versa.
   * It sees incoming symlinks as simple files and may wrongly try to offer
   * a text conflict. So flag a tree conflict here. */
  if (fb->adding_file && fb->add_existed)
    {
      int i;
      svn_boolean_t local_is_link = FALSE;
      svn_boolean_t incoming_is_link = FALSE;

      local_is_link = apr_hash_get(local_actual_props,
                                SVN_PROP_SPECIAL,
                                APR_HASH_KEY_STRING) != NULL;

      /* Jump through hoops to get the proper props in case of
       * a copy. (see the fb->copied_base_props condition above) */
      if (fb->copied_base_props)
        {
          incoming_is_link = fb->copied_working_props
                             && apr_hash_get(fb->copied_working_props,
                                             SVN_PROP_SPECIAL,
                                             APR_HASH_KEY_STRING) != NULL;
        }
      else
        {
          for (i = 0; i < regular_props->nelts; ++i)
            {
              const svn_prop_t *prop = &APR_ARRAY_IDX(regular_props, i,
                                                      svn_prop_t);

              if (strcmp(prop->name, SVN_PROP_SPECIAL) == 0)
                {
                  incoming_is_link = TRUE;
                }
            }
        }


      if (local_is_link != incoming_is_link)
        {
          svn_wc_conflict_description2_t *tree_conflict = NULL;

          fb->adding_base_under_local_add = TRUE;
          fb->obstruction_found = TRUE;
          fb->add_existed = FALSE;

          SVN_ERR(check_tree_conflict(&tree_conflict, eb, fb->local_abspath,
                                      svn_wc_conflict_action_add, svn_node_file,
                                      fb->new_relpath, pool));
          SVN_ERR_ASSERT(tree_conflict != NULL);
          SVN_ERR(svn_wc__loggy_add_tree_conflict(&work_item,
                                                  eb->db,
                                                  fb->dir_baton->local_abspath,
                                                  tree_conflict,
                                                  pool));
          SVN_ERR(svn_wc__db_wq_add(eb->db, fb->dir_baton->local_abspath,
                                    work_item, pool));

          fb->already_notified = TRUE;
          do_notification(eb, fb->local_abspath, svn_node_unknown,
                          svn_wc_notify_tree_conflict, pool);
        }
    }

  prop_state = svn_wc_notify_state_unknown;

  if (! fb->adding_base_under_local_add)
    {
      /* Merge the 'regular' props into the existing working proplist. */
      /* This will merge the old and new props into a new prop db, and
         write <cp> commands to the logfile to install the merged
         props.  */
      SVN_ERR(svn_wc__merge_props(&prop_state,
                                  &new_base_props,
                                  &new_actual_props,
                                  eb->db,
                                  fb->local_abspath,
                                  svn_wc__db_kind_file,
                                  NULL /* left_version */,
                                  NULL /* right_version */,
                                  NULL /* server_baseprops (update, not merge)  */,
                                  current_base_props,
                                  current_actual_props,
                                  regular_props, /* propchanges */
                                  TRUE /* base_merge */,
                                  FALSE /* dry_run */,
                                  eb->conflict_func, eb->conflict_baton,
                                  eb->cancel_func, eb->cancel_baton,
                                  pool,
                                  pool));
      /* We will ALWAYS have properties to save (after a not-dry-run merge).  */
      SVN_ERR_ASSERT(new_base_props != NULL && new_actual_props != NULL);

    /* Merge the text. This will queue some additional work.  */
    SVN_ERR(merge_file(&all_work_items, &install_pristine, &install_from,
                       &content_state, fb, new_text_base_sha1_checksum,
                       pool, scratch_pool));

    if (install_pristine)
      {
        svn_boolean_t record_fileinfo;

        /* If we are installing from the pristine contents, then go ahead and
           record the fileinfo. That will be the "proper" values. Installing
           from some random file means the fileinfo does NOT correspond to
           the pristine (in which case, the fileinfo will be cleared for
           safety's sake).  */
        record_fileinfo = install_from == NULL;

        SVN_ERR(svn_wc__wq_build_file_install(&work_item,
                                              eb->db,
                                              fb->local_abspath,
                                              install_from,
                                              eb->use_commit_times,
                                              record_fileinfo,
                                              pool, pool));
        all_work_items = svn_wc__wq_merge(all_work_items, work_item, pool);
      }

    }
  else
    {
      /* Adding a BASE node under a locally added node.
       * The incoming add becomes the revert-base! */
      svn_wc_notify_state_t no_prop_state;
      apr_hash_t *copied_base_props;
      apr_hash_t *no_new_actual_props = NULL;
      apr_hash_t *no_working_props = apr_hash_make(pool);

      copied_base_props = fb->copied_base_props;
      if (! copied_base_props)
        copied_base_props = apr_hash_make(pool);


      /* Store the incoming props (sent as propchanges) in new_base_props.
       * Keep the actual props unchanged. */
      SVN_ERR(svn_wc__merge_props(&no_prop_state,
                                  &new_base_props,
                                  &no_new_actual_props,
                                  eb->db,
                                  fb->local_abspath,
                                  svn_wc__db_kind_file,
                                  NULL /* left_version */,
                                  NULL /* right_version */,
                                  NULL /* server_baseprops (update, not merge)  */,
                                  copied_base_props,
                                  no_working_props,
                                  regular_props, /* propchanges */
                                  TRUE /* base_merge */,
                                  FALSE /* dry_run */,
                                  eb->conflict_func, eb->conflict_baton,
                                  eb->cancel_func, eb->cancel_baton,
                                  pool,
                                  pool));

      prop_state = svn_wc_notify_state_unchanged;
      new_actual_props = local_actual_props;
    }

  /* Now that all the state has settled, should we update the readonly
     status of the working file? The LOCK_STATE will signal what we should
     do for this node.  */
  if (new_text_base_sha1_checksum == NULL
      && lock_state == svn_wc_notify_lock_state_unlocked)
    {
      /* If a lock was removed and we didn't update the text contents, we
         might need to set the file read-only.

         Note: this will also update the executable flag, but ... meh.  */
      SVN_ERR(svn_wc__wq_build_sync_file_flags(&work_item, eb->db,
                                               fb->local_abspath,
                                               pool, pool));
      all_work_items = svn_wc__wq_merge(all_work_items, work_item, pool);
    }

  /* Clean up any temporary files.  */

  /* Remove the INSTALL_FROM file, as long as it doesn't refer to the
     working file.  */
  if (install_from != NULL
      && strcmp(install_from, fb->local_abspath) != 0)
    {
      SVN_ERR(svn_wc__wq_build_file_remove(&work_item, eb->db, install_from,
                                           pool, pool));
      all_work_items = svn_wc__wq_merge(all_work_items, work_item, pool);
    }

  /* Remove the copied text base file if we're no longer using it. */
  if (fb->copied_text_base_sha1_checksum)
    {
      /* ### TODO: Add a WQ item to remove this pristine if unreferenced:
         svn_wc__wq_build_pristine_remove(&work_item,
                                          eb->db, fb->local_abspath,
                                          fb->copied_text_base_sha1_checksum,
                                          pool);
         all_work_items = svn_wc__wq_merge(all_work_items, work_item, pool);
      */
    }

  /* ### NOTE: from this point onwards, we make several changes to the
     ### database in a non-transactional way. we also queue additional
     ### work after these changes. some revamps need to be performed to
     ### bring this down to a single DB transaction to perform all the
     ### changes and to install all the needed work items.  */

  /* Insert/replace the BASE node with all of the new metadata.  */
  {
      /* Set the 'checksum' column of the file's BASE_NODE row to
       * NEW_TEXT_BASE_SHA1_CHECKSUM.  The pristine text identified by that
       * checksum is already in the pristine store. */
    const svn_checksum_t *new_checksum = new_text_base_sha1_checksum;
    const char *serialised;

    /* If we don't have a NEW checksum, then the base must not have changed.
       Just carry over the old checksum.  */
    if (new_checksum == NULL)
      {
        SVN_ERR(svn_wc__db_base_get_info(NULL, NULL, NULL,
                                         NULL, NULL, NULL,
                                         NULL, NULL, NULL,
                                         NULL, NULL,
                                         &new_checksum, NULL, NULL, NULL,
                                         eb->db, fb->local_abspath,
                                         pool, pool));
      }

    if (kind != svn_node_none)
      SVN_ERR(svn_wc__db_temp_get_file_external(&serialised,
                                                eb->db, fb->local_abspath,
                                                pool, pool));

    SVN_ERR(svn_wc__db_base_add_file(eb->db, fb->local_abspath,
                                     fb->new_relpath,
                                     eb->repos_root, eb->repos_uuid,
                                     *eb->target_revision,
                                     new_base_props,
                                     new_changed_rev,
                                     new_changed_date,
                                     new_changed_author,
                                     new_checksum,
                                     SVN_INVALID_FILESIZE,
                                     (dav_props && dav_props->nelts > 0)
                                        ? prop_hash_from_array(dav_props, pool)
                                        : NULL,
                                     NULL /* conflict */,
                                     all_work_items,
                                     pool));

    /* ### ugh. deal with preserving the file external value in the database.
       ### there is no official API, so we do it this way. maybe we should
       ### have a temp API into wc_db.  */
    if (kind != svn_node_none && serialised)
      {
        const char *file_external_repos_relpath;
        svn_opt_revision_t file_external_peg_rev, file_external_rev;

        SVN_ERR(svn_wc__unserialize_file_external(&file_external_repos_relpath,
                                                  &file_external_peg_rev,
                                                  &file_external_rev,
                                                  serialised, pool));

        SVN_ERR(svn_wc__db_temp_op_set_file_external(
                                                  eb->db, fb->local_abspath,
                                                  file_external_repos_relpath,
                                                  &file_external_peg_rev,
                                                  &file_external_rev,
                                                  pool));
      }
  }

  /* Deal with the WORKING tree, based on updates to the BASE tree.  */

  /* An ancestor was locally-deleted. This file is being added within
     that tree. We need to schedule this file for deletion.  */
  if (fb->dir_baton->in_deleted_and_tree_conflicted_subtree && fb->adding_file)
    {
      SVN_ERR(svn_wc__db_temp_op_delete(eb->db, fb->local_abspath, pool));
    }

  /* If this file was locally-added and is now being added by the update, we
     can toss the local-add, turning this into a local-edit.  */
  if (fb->add_existed && fb->adding_file)
    {
      SVN_ERR(svn_wc__db_temp_op_remove_working(eb->db, fb->local_abspath,
                                                pool));
    }

  /* Now we need to update the ACTUAL tree, with the result of the
     properties merge. */
  if (! fb->adding_base_under_local_add)
    {
      apr_hash_t *props;
      apr_array_header_t *prop_diffs;

      SVN_ERR_ASSERT(new_actual_props != NULL);

      /* If the ACTUAL props are the same as the BASE props, then we
         should "write" a NULL. This will remove the props from the
         ACTUAL_NODE row, and remove the old-style props file, indicating
         "no change".  */
      props = new_actual_props;
      SVN_ERR(svn_prop_diffs(&prop_diffs, new_actual_props, new_base_props,
                             pool));
      if (prop_diffs->nelts == 0)
        props = NULL;

      SVN_ERR(svn_wc__db_op_set_props(eb->db, fb->local_abspath,
                                      props,
                                      NULL /* conflict */,
                                      NULL /* work_item */,
                                      pool));
    }

  /* ### we may as well run whatever is in the queue right now. this
     ### starts out with some crap node data via construct_base_node(),
     ### so we can't really monkey things up too badly here. all tests
     ### continue to pass, so this also gives us a better insight into
     ### doing things more immediately, rather than queuing to run at
     ### some future point in time.  */
  SVN_ERR(svn_wc__wq_run(eb->db, fb->dir_baton->local_abspath,
                         eb->cancel_func, eb->cancel_baton,
                         pool));

  /* We have one less referrer to the directory's bump information. */
  SVN_ERR(maybe_bump_dir_info(eb, fb->bump_info, pool));

  /* Send a notification to the callback function.  (Skip notifications
     about files which were already notified for another reason.) */
  if (eb->notify_func && !fb->already_notified)
    {
      const svn_string_t *mime_type;
      svn_wc_notify_t *notify;
      svn_wc_notify_action_t action = svn_wc_notify_update_update;

      if (fb->deleted)
        action = svn_wc_notify_update_add_deleted;
      else if (fb->obstruction_found || fb->add_existed)
        {
          if (content_state != svn_wc_notify_state_conflicted)
            action = svn_wc_notify_exists;
        }
      else if (fb->adding_file)
        {
          action = svn_wc_notify_update_add;
        }

      notify = svn_wc_create_notify(fb->local_abspath, action, pool);
      notify->kind = svn_node_file;
      notify->content_state = content_state;
      notify->prop_state = prop_state;
      notify->lock_state = lock_state;
      notify->revision = *eb->target_revision;
      notify->old_revision = fb->old_revision;

      /* Fetch the mimetype */
      SVN_ERR(svn_wc__internal_propget(&mime_type, eb->db, fb->local_abspath,
                                       SVN_PROP_MIME_TYPE, pool, pool));
      notify->mime_type = mime_type == NULL ? NULL : mime_type->data;

      eb->notify_func(eb->notify_baton, notify, pool);
    }

  svn_pool_destroy(fb->pool);

  return SVN_NO_ERROR;
}

/* Helper for svn_wc__do_update_cleanup().
 *
 * Tweak the information for LOCAL_ABSPATH in DB.  If NEW_REPOS_RELPATH is
 * non-NULL update the entry to the new url specified by NEW_REPOS_RELPATH,
 * NEW_REPOS_ROOT_URL, NEW_REPOS_UUID..  If NEW_REV is valid, make this the
 * node's working revision.
 *
 * If ALLOW_REMOVAL is TRUE the tweaks might cause the node for
 * LOCAL_ABSPATH to be removed from the WC; if ALLOW_REMOVAL is FALSE this
 * will not happen.
 */
static svn_error_t *
tweak_node(svn_wc__db_t *db,
           const char *local_abspath,
           svn_wc__db_kind_t kind,
           svn_boolean_t parent_stub,
           const char *new_repos_relpath,
           const char *new_repos_root_url,
           const char *new_repos_uuid,
           svn_revnum_t new_rev,
           svn_boolean_t allow_removal,
           apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  svn_wc__db_kind_t db_kind;
  svn_revnum_t revision;
  const char *repos_relpath, *repos_root_url, *repos_uuid;
  svn_boolean_t set_repos_relpath = FALSE;
  svn_error_t *err;

  err = svn_wc__db_base_get_info(&status, &db_kind, &revision,
                                 &repos_relpath, &repos_root_url,
                                 &repos_uuid, NULL, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, db, local_abspath,
                                 scratch_pool, scratch_pool);

  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      /* ### Tweaking should never be necessary for nodes that don't
         ### have a base node, but we still get here from many tests */
      svn_error_clear(err);
      return SVN_NO_ERROR; /* No BASE_NODE -> Added node */
    }
  else
    SVN_ERR(err);

  SVN_ERR_ASSERT(db_kind == kind);

  /* As long as this function is only called as a helper to
     svn_wc__do_update_cleanup, then it's okay to remove any entry
     under certain circumstances:

     If the entry is still marked 'deleted', then the server did not
     re-add it.  So it's really gone in this revision, thus we remove
     the entry.

     If the entry is still marked 'absent' and yet is not the same
     revision as new_rev, then the server did not re-add it, nor
     re-absent it, so we can remove the entry.

     ### This function cannot always determine whether removal is
     ### appropriate, hence the ALLOW_REMOVAL flag.  It's all a bit of a
     ### mess. */
  if (allow_removal
      && (status == svn_wc__db_status_not_present
          || (status == svn_wc__db_status_absent && revision != new_rev)))
    {
      return svn_error_return(
                svn_wc__db_temp_op_remove_entry(db, local_abspath,
                                                scratch_pool));

    }

  if (new_repos_relpath != NULL)
    {
      if (!repos_relpath)
        SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath, &repos_root_url,
                                           &repos_uuid, db, local_abspath,
                                           scratch_pool, scratch_pool));

      if (strcmp(repos_relpath, new_repos_relpath))
          set_repos_relpath = TRUE;
    }

  if (SVN_IS_VALID_REVNUM(new_rev) && new_rev == revision)
    new_rev = SVN_INVALID_REVNUM;

  if (SVN_IS_VALID_REVNUM(new_rev) || set_repos_relpath)
    {
      svn_boolean_t update_stub = 
            (db_kind == svn_wc__db_kind_dir && parent_stub);

      SVN_ERR(svn_wc__db_temp_op_set_rev_and_repos_relpath(db, local_abspath,
                                                          new_rev,
                                                          set_repos_relpath,
                                                          new_repos_relpath,
                                                          repos_root_url,
                                                          repos_uuid,
                                                          update_stub,
                                                          scratch_pool));
    }

  return SVN_NO_ERROR;
}


/* The main body of svn_wc__do_update_cleanup. */
static svn_error_t *
tweak_entries(svn_wc__db_t *db,
              const char *dir_abspath,
              const char *new_repos_relpath,
              const char *new_repos_root_url,
              const char *new_repos_uuid,
              svn_revnum_t new_rev,
              svn_wc_notify_func2_t notify_func,
              void *notify_baton,
              svn_depth_t depth,
              apr_hash_t *exclude_paths,
              apr_pool_t *pool)
{
  apr_pool_t *iterpool;
  svn_wc_notify_t *notify;
  const apr_array_header_t *children;
  int i;

  /* Skip an excluded path and its descendants. */
  if (apr_hash_get(exclude_paths, dir_abspath, APR_HASH_KEY_STRING))
    return SVN_NO_ERROR;

  iterpool = svn_pool_create(pool);

  /* Tweak "this_dir" */
  SVN_ERR(tweak_node(db, dir_abspath, svn_wc__db_kind_dir, FALSE,
                     new_repos_relpath, new_repos_root_url, new_repos_uuid,
                     new_rev, FALSE /* allow_removal */, iterpool));

  if (depth == svn_depth_unknown)
    depth = svn_depth_infinity;

  /* Early out */
  if (depth <= svn_depth_empty)
    return SVN_NO_ERROR;

  SVN_ERR(svn_wc__db_base_get_children(&children, db, dir_abspath,
                                       pool, iterpool));
  for (i = 0; i < children->nelts; i++)
    {
      const char *child_basename = APR_ARRAY_IDX(children, i, const char *);
      const char *child_abspath;
      svn_wc__db_kind_t kind;
      svn_wc__db_status_t status;

      const char *child_repos_relpath = NULL;
      svn_boolean_t excluded;

      svn_pool_clear(iterpool);

      /* Derive the new URL for the current (child) entry */
      if (new_repos_relpath)
        child_repos_relpath = svn_relpath_join(new_repos_relpath,
                                               child_basename, iterpool);

      child_abspath = svn_dirent_join(dir_abspath, child_basename, iterpool);
      excluded = (apr_hash_get(exclude_paths, child_abspath,
                               APR_HASH_KEY_STRING) != NULL);

      SVN_ERR(svn_wc__db_read_info(&status, &kind, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL,
                                   db, child_abspath, iterpool, iterpool));

      /* If a file, or deleted, excluded or absent dir, then tweak the
         entry but don't recurse.

         ### how does this translate into wc_db land? */
      if (kind == svn_wc__db_kind_file
            || status == svn_wc__db_status_not_present
            || status == svn_wc__db_status_absent
            || status == svn_wc__db_status_excluded)
        {
          if (excluded)
            continue;

          if (kind == svn_wc__db_kind_dir)
            SVN_ERR(tweak_node(db, child_abspath, svn_wc__db_kind_dir, TRUE,
                               child_repos_relpath, new_repos_root_url,
                               new_repos_uuid, new_rev,
                               TRUE /* allow_removal */, iterpool));
          else
            SVN_ERR(tweak_node(db, child_abspath, kind, FALSE,
                               child_repos_relpath, new_repos_root_url,
                               new_repos_uuid, new_rev,
                               TRUE /* allow_removal */, iterpool));
        }

      /* If a directory and recursive... */
      else if ((depth == svn_depth_infinity
                || depth == svn_depth_immediates)
               && (kind == svn_wc__db_kind_dir))
        {
          svn_depth_t depth_below_here = depth;

          if (depth == svn_depth_immediates)
            depth_below_here = svn_depth_empty;

          /* If the directory is 'missing', remove it.  This is safe as
             long as this function is only called as a helper to
             svn_wc__do_update_cleanup, since the update will already have
             restored any missing items that it didn't want to delete. */
          if (svn_wc__adm_missing(db, child_abspath, iterpool))
            {
              if ( (status == svn_wc__db_status_added
                    || status == svn_wc__db_status_obstructed_add)
                  && !excluded)
                {
                  SVN_ERR(svn_wc__db_temp_op_remove_entry(db, child_abspath,
                                                          iterpool));

                  if (notify_func)
                    {
                      notify = svn_wc_create_notify(child_abspath,
                                                    svn_wc_notify_delete,
                                                    iterpool);

                      if (kind == svn_wc__db_kind_dir)
                        notify->kind = svn_node_dir;
                      else if (kind == svn_wc__db_kind_file)
                        notify->kind = svn_node_file;
                      else
                        notify->kind = svn_node_unknown;

                      (* notify_func)(notify_baton, notify, iterpool);
                    }
                }
              /* Else if missing item is schedule-add, do nothing. */
            }

          /* Not missing, deleted, or absent, so recurse. */
          else
            {
              SVN_ERR(tweak_entries(db, child_abspath, child_repos_relpath,
                                    new_repos_root_url, new_repos_uuid,
                                    new_rev, notify_func, notify_baton,
                                    depth_below_here,
                                    exclude_paths, iterpool));
            }
        }
    }

  /* Cleanup */
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Modify the entry of working copy LOCAL_ABSPATH, presumably after an update
   completes.   If LOCAL_ABSPATH doesn't exist, this routine does nothing.

   Set the entry's 'url' and 'working revision' fields to BASE_URL and
   NEW_REVISION.  If BASE_URL is null, the url field is untouched; if
   NEW_REVISION in invalid, the working revision field is untouched.
   The modifications are mutually exclusive.

   If REPOS is non-NULL, set the repository root of the entry to REPOS, but
   only if REPOS is an ancestor of the entries URL (after possibly modifying
   it).  In addition to that requirement, if the LOCAL_ABSPATH refers to a
   directory, the repository root is only set if REPOS is an ancestor of the
   URLs all file entries which don't already have a repository root set.  This
   prevents the entries file from being corrupted by this operation.

   If LOCAL_ABSPATH is a directory, then, walk entries below LOCAL_ABSPATH
   according to DEPTH thusly:

   If DEPTH is svn_depth_infinity, perform the following actions on
   every entry below PATH; if svn_depth_immediates, svn_depth_files,
   or svn_depth_empty, perform them only on LOCAL_ABSPATH.

   If NEW_REVISION is valid, then tweak every entry to have this new
   working revision (excluding files that are scheduled for addition
   or replacement.)  Likewise, if BASE_URL is non-null, then rewrite
   all urls to be "telescoping" children of the base_url.

   EXCLUDE_PATHS is a hash containing const char * pathnames.  Entries
   for pathnames contained in EXCLUDE_PATHS are not touched by this
   function.  These pathnames should be absolute paths.
*/
static svn_error_t *
do_update_cleanup(svn_wc__db_t *db,
                          const char *local_abspath,
                          svn_depth_t depth,
                          const char *new_repos_relpath,
                          const char *new_repos_root_url,
                          const char *new_repos_uuid,
                          svn_revnum_t new_revision,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          apr_hash_t *exclude_paths,
                          apr_pool_t *pool)
{
  svn_wc__db_status_t status;
  svn_wc__db_kind_t kind;
  svn_error_t *err;

  if (apr_hash_get(exclude_paths, local_abspath, APR_HASH_KEY_STRING))
    return SVN_NO_ERROR;

  err = svn_wc__db_read_info(&status, &kind, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL,
                             db, local_abspath, pool, pool);
  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  else
    SVN_ERR(err);

  switch (status)
    {
      case svn_wc__db_status_excluded:
      case svn_wc__db_status_absent:
      case svn_wc__db_status_not_present:
        return SVN_NO_ERROR;
      case svn_wc__db_status_obstructed:
      case svn_wc__db_status_obstructed_add:
      case svn_wc__db_status_obstructed_delete:
        /* There is only a parent stub. That's fine... just tweak it
           and avoid directory recursion.  */
        SVN_ERR(tweak_node(db, local_abspath, svn_wc__db_kind_dir, TRUE,
                           new_repos_relpath, new_repos_root_url,
                           new_repos_uuid, new_revision,
                           FALSE /* allow_removal */, pool));
        return SVN_NO_ERROR;

      /* Explicitly ignore other statii */
      default:
        break;
    }

  if (kind == svn_wc__db_kind_file || kind == svn_wc__db_kind_symlink)
    {
      /* Parent not updated so don't remove PATH entry.  */
      SVN_ERR(tweak_node(db, local_abspath, kind, FALSE,
                         new_repos_relpath, new_repos_root_url, new_repos_uuid,
                         new_revision, FALSE /* allow_removal */, pool));
    }
  else if (kind == svn_wc__db_kind_dir)
    {
      SVN_ERR(tweak_entries(db, local_abspath, new_repos_relpath,
                            new_repos_root_url, new_repos_uuid, new_revision,
                            notify_func, notify_baton,
                            depth, exclude_paths, pool));
    }
  else
    return svn_error_createf(SVN_ERR_NODE_UNKNOWN_KIND, NULL,
                             _("Unrecognized node kind: '%s'"),
                             svn_dirent_local_style(local_abspath, pool));

  return SVN_NO_ERROR;
}

/* An svn_delta_editor_t function. */
static svn_error_t *
close_edit(void *edit_baton,
           apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;

  /* If there is a target and that target is missing, then it
     apparently wasn't re-added by the update process, so we'll
     pretend that the editor deleted the entry.  The helper function
     do_entry_deletion() will take care of the necessary steps.  */
  if ((*eb->target_basename) &&
      svn_wc__adm_missing(eb->db, eb->target_abspath, pool))
    {
      /* Still passing NULL for THEIR_URL. A case where THEIR_URL
       * is needed in this call is rare or even non-existant.
       * ### TODO: Construct a proper THEIR_URL anyway. See also
       * NULL handling code in do_entry_deletion(). */
      SVN_ERR(do_entry_deletion(eb, eb->target_abspath, NULL, FALSE, pool));
    }

  /* The editor didn't even open the root; we have to take care of
     some cleanup stuffs. */
  if (! eb->root_opened)
    {
      /* We need to "un-incomplete" the root directory. */
      SVN_ERR(complete_directory(eb, eb->anchor_abspath, TRUE, pool));
    }


  /* By definition, anybody "driving" this editor for update or switch
     purposes at a *minimum* must have called set_target_revision() at
     the outset, and close_edit() at the end -- even if it turned out
     that no changes ever had to be made, and open_root() was never
     called.  That's fine.  But regardless, when the edit is over,
     this editor needs to make sure that *all* paths have had their
     revisions bumped to the new target revision. */

  /* Make sure our update target now has the new working revision.
     Also, if this was an 'svn switch', then rewrite the target's
     url.  All of this tweaking might happen recursively!  Note
     that if eb->target is NULL, that's okay (albeit "sneaky",
     some might say).  */

  /* Extra check: if the update did nothing but make its target
     'deleted', then do *not* run cleanup on the target, as it
     will only remove the deleted entry!  */
  if (! eb->target_deleted)
    {
      SVN_ERR(do_update_cleanup(eb->db,
                                eb->target_abspath,
                                eb->requested_depth,
                                eb->switch_relpath,
                                eb->repos_root,
                                eb->repos_uuid,
                                *(eb->target_revision),
                                eb->notify_func,
                                eb->notify_baton,
                                eb->skipped_trees,
                                eb->pool));
    }

  /* The edit is over, free its pool.
     ### No, this is wrong.  Who says this editor/baton won't be used
     again?  But the change is not merely to remove this call.  We
     should also make eb->pool not be a subpool (see make_editor),
     and change callers of svn_client_{checkout,update,switch} to do
     better pool management. ### */
  svn_pool_destroy(eb->pool);

  return SVN_NO_ERROR;
}



/*** Returning editors. ***/

/* Helper for the three public editor-supplying functions. */
static svn_error_t *
make_editor(svn_revnum_t *target_revision,
            svn_wc_context_t *wc_ctx,
            const char *anchor_abspath,
            const char *target_basename,
            svn_boolean_t use_commit_times,
            const char *switch_url,
            svn_depth_t depth,
            svn_boolean_t depth_is_sticky,
            svn_boolean_t allow_unver_obstructions,
            svn_wc_notify_func2_t notify_func,
            void *notify_baton,
            svn_cancel_func_t cancel_func,
            void *cancel_baton,
            svn_wc_conflict_resolver_func_t conflict_func,
            void *conflict_baton,
            svn_wc_external_update_t external_func,
            void *external_baton,
            svn_wc_get_file_t fetch_func,
            void *fetch_baton,
            const char *diff3_cmd,
            const apr_array_header_t *preserved_exts,
            const svn_delta_editor_t **editor,
            void **edit_baton,
            apr_pool_t *result_pool,
            apr_pool_t *scratch_pool)
{
  struct edit_baton *eb;
  void *inner_baton;
  apr_pool_t *edit_pool = svn_pool_create(result_pool);
  svn_delta_editor_t *tree_editor = svn_delta_default_editor(edit_pool);
  const svn_delta_editor_t *inner_editor;
  const char *repos_root, *repos_uuid;

  /* An unknown depth can't be sticky. */
  if (depth == svn_depth_unknown)
    depth_is_sticky = FALSE;

  /* Get the anchor's repository root and uuid. */
  SVN_ERR(svn_wc__db_read_info(NULL,NULL, NULL, NULL, &repos_root, &repos_uuid,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, wc_ctx->db, anchor_abspath,
                               result_pool, scratch_pool));

  /* With WC-NG we need a valid repository root */
  SVN_ERR_ASSERT(repos_root != NULL && repos_uuid != NULL);

  /* Disallow a switch operation to change the repository root of the target,
     if that is known. */
  if (switch_url && !svn_uri_is_ancestor(repos_root, switch_url))
    return svn_error_createf(
       SVN_ERR_WC_INVALID_SWITCH, NULL,
       _("'%s'\n"
         "is not the same repository as\n"
         "'%s'"), switch_url, repos_root);

  /* Construct an edit baton. */
  eb = apr_pcalloc(edit_pool, sizeof(*eb));
  eb->pool                     = edit_pool;
  eb->use_commit_times         = use_commit_times;
  eb->target_revision          = target_revision;
  eb->repos_root               = repos_root;
  eb->repos_uuid               = repos_uuid;
  eb->db                       = wc_ctx->db;
  eb->wc_ctx                   = wc_ctx;
  eb->target_basename          = target_basename;
  eb->anchor_abspath           = anchor_abspath;

  if (switch_url)
    eb->switch_relpath         = svn_path_uri_decode(
                                    svn_uri_skip_ancestor(repos_root,
                                                          switch_url),
                                    scratch_pool);
  else
    eb->switch_relpath         = NULL;

  if (svn_path_is_empty(target_basename))
    eb->target_abspath = eb->anchor_abspath;
  else
    eb->target_abspath = svn_dirent_join(eb->anchor_abspath, target_basename,
                                         edit_pool);

  eb->requested_depth          = depth;
  eb->depth_is_sticky          = depth_is_sticky;
  eb->notify_func              = notify_func;
  eb->notify_baton             = notify_baton;
  eb->external_func            = external_func;
  eb->external_baton           = external_baton;
  eb->diff3_cmd                = diff3_cmd;
  eb->cancel_func              = cancel_func;
  eb->cancel_baton             = cancel_baton;
  eb->conflict_func            = conflict_func;
  eb->conflict_baton           = conflict_baton;
  eb->fetch_func               = fetch_func;
  eb->fetch_baton              = fetch_baton;
  eb->allow_unver_obstructions = allow_unver_obstructions;
  eb->skipped_trees            = apr_hash_make(edit_pool);
  eb->ext_patterns             = preserved_exts;

  /* Construct an editor. */
  tree_editor->set_target_revision = set_target_revision;
  tree_editor->open_root = open_root;
  tree_editor->delete_entry = delete_entry;
  tree_editor->add_directory = add_directory;
  tree_editor->open_directory = open_directory;
  tree_editor->change_dir_prop = change_dir_prop;
  tree_editor->close_directory = close_directory;
  tree_editor->absent_directory = absent_directory;
  tree_editor->add_file = add_file;
  tree_editor->open_file = open_file;
  tree_editor->apply_textdelta = apply_textdelta;
  tree_editor->change_file_prop = change_file_prop;
  tree_editor->close_file = close_file;
  tree_editor->absent_file = absent_file;
  tree_editor->close_edit = close_edit;

  /* Fiddle with the type system. */
  inner_editor = tree_editor;
  inner_baton = eb;

  /* We need to limit the scope of our operation to the ambient depths
     present in the working copy already, but only if the requested
     depth is not sticky. If a depth was explicitly requested,
     libsvn_delta/depth_filter_editor.c will ensure that we never see
     editor calls that extend beyond the scope of the requested depth.
     But even what we do so might extend beyond the scope of our
     ambient depth.  So we use another filtering editor to avoid
     modifying the ambient working copy depth when not asked to do so.
     (This can also be skipped if the server understands depth; consider
     letting the depth RA capability percolate down to this level.) */
  if (!depth_is_sticky)
    SVN_ERR(svn_wc__ambient_depth_filter_editor(&inner_editor,
                                                &inner_baton,
                                                wc_ctx->db,
                                                anchor_abspath,
                                                target_basename,
                                                TRUE /* read_base */,
                                                inner_editor,
                                                inner_baton,
                                                result_pool));

  return svn_delta_get_cancellation_editor(cancel_func,
                                           cancel_baton,
                                           inner_editor,
                                           inner_baton,
                                           editor,
                                           edit_baton,
                                           result_pool);
}


svn_error_t *
svn_wc_get_update_editor4(const svn_delta_editor_t **editor,
                          void **edit_baton,
                          svn_revnum_t *target_revision,
                          svn_wc_context_t *wc_ctx,
                          const char *anchor_abspath,
                          const char *target_basename,
                          svn_boolean_t use_commit_times,
                          svn_depth_t depth,
                          svn_boolean_t depth_is_sticky,
                          svn_boolean_t allow_unver_obstructions,
                          const char *diff3_cmd,
                          const apr_array_header_t *preserved_exts,
                          svn_wc_get_file_t fetch_func,
                          void *fetch_baton,
                          svn_wc_conflict_resolver_func_t conflict_func,
                          void *conflict_baton,
                          svn_wc_external_update_t external_func,
                          void *external_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  return make_editor(target_revision, wc_ctx, anchor_abspath,
                     target_basename, use_commit_times,
                     NULL, depth, depth_is_sticky, allow_unver_obstructions,
                     notify_func, notify_baton,
                     cancel_func, cancel_baton,
                     conflict_func, conflict_baton,
                     external_func, external_baton,
                     fetch_func, fetch_baton,
                     diff3_cmd, preserved_exts, editor, edit_baton,
                     result_pool, scratch_pool);
}

svn_error_t *
svn_wc_get_switch_editor4(const svn_delta_editor_t **editor,
                          void **edit_baton,
                          svn_revnum_t *target_revision,
                          svn_wc_context_t *wc_ctx,
                          const char *anchor_abspath,
                          const char *target_basename,
                          const char *switch_url,
                          svn_boolean_t use_commit_times,
                          svn_depth_t depth,
                          svn_boolean_t depth_is_sticky,
                          svn_boolean_t allow_unver_obstructions,
                          const char *diff3_cmd,
                          const apr_array_header_t *preserved_exts,
                          svn_wc_get_file_t fetch_func,
                          void *fetch_baton,
                          svn_wc_conflict_resolver_func_t conflict_func,
                          void *conflict_baton,
                          svn_wc_external_update_t external_func,
                          void *external_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(switch_url && svn_uri_is_canonical(switch_url, scratch_pool));

  return make_editor(target_revision, wc_ctx, anchor_abspath,
                     target_basename, use_commit_times,
                     switch_url,
                     depth, depth_is_sticky, allow_unver_obstructions,
                     notify_func, notify_baton,
                     cancel_func, cancel_baton,
                     conflict_func, conflict_baton,
                     external_func, external_baton,
                     fetch_func, fetch_baton,
                     diff3_cmd, preserved_exts,
                     editor, edit_baton,
                     result_pool, scratch_pool);
}

/* ABOUT ANCHOR AND TARGET, AND svn_wc_get_actual_target2()

   THE GOAL

   Note the following actions, where X is the thing we wish to update,
   P is a directory whose repository URL is the parent of
   X's repository URL, N is directory whose repository URL is *not*
   the parent directory of X (including the case where N is not a
   versioned resource at all):

      1.  `svn up .' from inside X.
      2.  `svn up ...P/X' from anywhere.
      3.  `svn up ...N/X' from anywhere.

   For the purposes of the discussion, in the '...N/X' situation, X is
   said to be a "working copy (WC) root" directory.

   Now consider the four cases for X's type (file/dir) in the working
   copy vs. the repository:

      A.  dir in working copy, dir in repos.
      B.  dir in working copy, file in repos.
      C.  file in working copy, dir in repos.
      D.  file in working copy, file in repos.

   Here are the results we expect for each combination of the above:

      1A. Successfully update X.
      1B. Error (you don't want to remove your current working
          directory out from underneath the application).
      1C. N/A (you can't be "inside X" if X is a file).
      1D. N/A (you can't be "inside X" if X is a file).

      2A. Successfully update X.
      2B. Successfully update X.
      2C. Successfully update X.
      2D. Successfully update X.

      3A. Successfully update X.
      3B. Error (you can't create a versioned file X inside a
          non-versioned directory).
      3C. N/A (you can't have a versioned file X in directory that is
          not its repository parent).
      3D. N/A (you can't have a versioned file X in directory that is
          not its repository parent).

   To summarize, case 2 always succeeds, and cases 1 and 3 always fail
   (or can't occur) *except* when the target is a dir that remains a
   dir after the update.

   ACCOMPLISHING THE GOAL

   Updates are accomplished by driving an editor, and an editor is
   "rooted" on a directory.  So, in order to update a file, we need to
   break off the basename of the file, rooting the editor in that
   file's parent directory, and then updating only that file, not the
   other stuff in its parent directory.

   Secondly, we look at the case where we wish to update a directory.
   This is typically trivial.  However, one problematic case, exists
   when we wish to update a directory that has been removed from the
   repository and replaced with a file of the same name.  If we root
   our edit at the initial directory, there is no editor mechanism for
   deleting that directory and replacing it with a file (this would be
   like having an editor now anchored on a file, which is disallowed).

   All that remains is to have a function with the knowledge required
   to properly decide where to root our editor, and what to act upon
   with that now-rooted editor.  Given a path to be updated, this
   function should conditionally split that path into an "anchor" and
   a "target", where the "anchor" is the directory at which the update
   editor is rooted (meaning, editor->open_root() is called with
   this directory in mind), and the "target" is the actual intended
   subject of the update.

   svn_wc_get_actual_target2() is that function.

   So, what are the conditions?

   Case I: Any time X is '.' (implying it is a directory), we won't
   lop off a basename.  So we'll root our editor at X, and update all
   of X.

   Cases II & III: Any time we are trying to update some path ...N/X,
   we again will not lop off a basename.  We can't root an editor at
   ...N with X as a target, either because ...N isn't a versioned
   resource at all (Case II) or because X is X is not a child of ...N
   in the repository (Case III).  We root at X, and update X.

   Cases IV-???: We lop off a basename when we are updating a
   path ...P/X, rooting our editor at ...P and updating X, or when X
   is missing from disk.

   These conditions apply whether X is a file or directory.

   ---

   As it turns out, commits need to have a similar check in place,
   too, specifically for the case where a single directory is being
   committed (we have to anchor at that directory's parent in case the
   directory itself needs to be modified).
*/


svn_error_t *
svn_wc__check_wc_root(svn_boolean_t *wc_root,
                      svn_wc__db_kind_t *kind,
                      svn_boolean_t *switched,
                      svn_wc__db_t *db,
                      const char *local_abspath,
                      apr_pool_t *scratch_pool)
{
  const char *parent_abspath, *name;
  const char *repos_relpath, *repos_root, *repos_uuid;
  svn_wc__db_status_t status;
  svn_wc__db_kind_t my_kind;

  /* Go ahead and initialize our return value to the most common
     (code-wise) values. */
  if (!kind)
    kind = &my_kind;

  *wc_root = TRUE;
  if (switched)
    *switched = FALSE;

  SVN_ERR(svn_wc__db_read_info(&status, kind, NULL, &repos_relpath,
                               &repos_root, &repos_uuid, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               db, local_abspath,
                               scratch_pool, scratch_pool));

  if (repos_relpath == NULL)
    {
      /* If we inherit our URL, then we can't be a root, nor switched.  */
      *wc_root = FALSE;
      return SVN_NO_ERROR;
    }
  if (*kind != svn_wc__db_kind_dir)
    {
      /* File/symlinks cannot be a root.  */
      *wc_root = FALSE;
    }
  else if (status == svn_wc__db_status_added
           || status == svn_wc__db_status_deleted)
    {
      *wc_root = FALSE;
    }
  else if (status == svn_wc__db_status_absent
           || status == svn_wc__db_status_excluded
           || status == svn_wc__db_status_not_present)
    {
      return svn_error_createf(
                    SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                    _("The node '%s' was not found."),
                    svn_dirent_local_style(local_abspath, scratch_pool));
    }
  else if (svn_dirent_is_root(local_abspath, strlen(local_abspath)))
    return SVN_NO_ERROR;

  if (!*wc_root && switched == NULL )
    return SVN_NO_ERROR; /* No more info needed */

  svn_dirent_split(&parent_abspath, &name, local_abspath, scratch_pool);

  /* Check if the node is recorded in the parent */
  if (*wc_root)
    {
      svn_boolean_t is_root;
      SVN_ERR(svn_wc__db_is_wcroot(&is_root, db, local_abspath, scratch_pool));

      if (is_root)
        {
          /* We're not in the (versioned) parent directory's list of
             children, so we must be the root of a distinct working copy.  */
          return SVN_NO_ERROR;
        }
    }

  {
    const char *parent_repos_root;
    const char *parent_repos_relpath;
    const char *parent_repos_uuid;

    SVN_ERR(svn_wc__db_scan_base_repos(&parent_repos_relpath,
                                       &parent_repos_root,
                                       &parent_repos_uuid,
                                       db, parent_abspath,
                                       scratch_pool, scratch_pool));

    if (strcmp(repos_root, parent_repos_root) != 0
        || strcmp(repos_uuid, parent_repos_uuid) != 0)
      {
        /* This should never happen (### until we get mixed-repos working
           copies). If we're in the parent, then we should be from the
           same repository. For this situation, just declare us the root
           of a separate, unswitched working copy.  */
        return SVN_NO_ERROR;
      }

    *wc_root = FALSE;

    if (switched)
      {
        const char *expected_relpath = svn_relpath_join(parent_repos_relpath,
                                                        name, scratch_pool);

        *switched = (strcmp(expected_relpath, repos_relpath) != 0);
      }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_is_wc_root2(svn_boolean_t *wc_root,
                   svn_wc_context_t *wc_ctx,
                   const char *local_abspath,
                   apr_pool_t *scratch_pool)
{
  svn_boolean_t is_root;
  svn_boolean_t is_switched;
  svn_wc__db_kind_t kind;
  svn_error_t *err;
  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  err = svn_wc__check_wc_root(&is_root, &kind, &is_switched,
                              wc_ctx->db, local_abspath, scratch_pool);

  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND &&
          err->apr_err != SVN_ERR_WC_NOT_WORKING_COPY)
        return svn_error_return(err);

      return svn_error_create(SVN_ERR_ENTRY_NOT_FOUND, err, err->message);
    }

  *wc_root = is_root || (kind == svn_wc__db_kind_dir && is_switched);

  return SVN_NO_ERROR;
}


svn_error_t*
svn_wc__strictly_is_wc_root(svn_boolean_t *wc_root,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            apr_pool_t *scratch_pool)
{
  return svn_error_return(
             svn_wc__check_wc_root(wc_root, NULL, NULL,
                                   wc_ctx->db, local_abspath,
                                   scratch_pool));
}


svn_error_t *
svn_wc_get_actual_target2(const char **anchor,
                          const char **target,
                          svn_wc_context_t *wc_ctx,
                          const char *path,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  svn_boolean_t is_wc_root, is_switched;
  svn_wc__db_kind_t kind;
  const char *local_abspath;
  svn_error_t *err;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, scratch_pool));

  err = svn_wc__check_wc_root(&is_wc_root, &kind, &is_switched,
                              wc_ctx->db, local_abspath,
                              scratch_pool);

  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_PATH_NOT_FOUND &&
          err->apr_err != SVN_ERR_WC_NOT_WORKING_COPY)
        return svn_error_return(err);

      svn_error_clear(err);
      is_wc_root = FALSE;
      is_switched = FALSE;
    }

  /* If PATH is not a WC root, or if it is a file, lop off a basename. */
  if (!(is_wc_root || is_switched) || (kind != svn_wc__db_kind_dir))
    {
      svn_dirent_split(anchor, target, path, result_pool);
    }
  else
    {
      *anchor = apr_pstrdup(result_pool, path);
      *target = "";
    }

  return SVN_NO_ERROR;
}


/* ### Note that this function is completely different from the rest of the
       update editor in what it updates. The update editor changes only BASE
       and ACTUAL and this function just changes WORKING and ACTUAL.

       In the entries world this function shared a lot of code with the
       update editor but in the wonderful new WC-NG world it will probably
       do more and more by itself and would be more logically grouped with
       the add/copy functionality in adm_ops.c and copy.c. */
svn_error_t *
svn_wc_add_repos_file4(svn_wc_context_t *wc_ctx,
                       const char *local_abspath,
                       svn_stream_t *new_base_contents,
                       svn_stream_t *new_contents,
                       apr_hash_t *new_base_props,
                       apr_hash_t *new_props,
                       const char *copyfrom_url,
                       svn_revnum_t copyfrom_rev,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       svn_wc_notify_func2_t notify_func,
                       void *notify_baton,
                       apr_pool_t *pool)
{
  svn_wc__db_t *db = wc_ctx->db;
  const char *dir_abspath = svn_dirent_dirname(local_abspath, pool);
  const char *tmp_text_base_abspath;
  svn_checksum_t *new_text_base_md5_checksum;
  svn_checksum_t *new_text_base_sha1_checksum;
  const char *source_abspath = NULL;
  svn_skel_t *all_work_items = NULL;
  svn_skel_t *work_item;
  const char *original_root_url;
  const char *original_repos_relpath;
  const char *original_uuid;
  apr_hash_t *actual_props;
  svn_revnum_t changed_rev;
  apr_time_t changed_date;
  const char *changed_author;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(new_base_contents != NULL);
  SVN_ERR_ASSERT(new_base_props != NULL);

  /* We should have a write lock on this file's parent directory.  */
  SVN_ERR(svn_wc__write_check(db, dir_abspath, pool));

  /* Fabricate the anticipated new URL of the target and check the
     copyfrom URL to be in the same repository. */
  if (copyfrom_url != NULL)
    {
      const char *relative_url;

      /* Find the repository_root via the parent directory, which
         is always versioned before this function is called */
      SVN_ERR(svn_wc__node_get_repos_info(&original_root_url,
                                          &original_uuid,
                                          wc_ctx,
                                          dir_abspath,
                                          TRUE /* scan_added */,
                                          FALSE /* scan_deleted */,
                                          pool, pool));

      if (!svn_uri_is_ancestor(original_root_url, copyfrom_url))
        return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                 _("Copyfrom-url '%s' has different repository"
                                   " root than '%s'"),
                                 copyfrom_url, original_root_url);

      relative_url = svn_uri_skip_ancestor(original_root_url, copyfrom_url);
      original_repos_relpath = svn_path_uri_decode(relative_url, pool);
    }
  else
    {
      original_root_url = NULL;
      original_repos_relpath = NULL;
      original_uuid = NULL;
      copyfrom_rev = SVN_INVALID_REVNUM;  /* Just to be sure.  */
    }

  /* If we're replacing the file then we need to save the destination file's
     original text base and prop base before replacing it. This allows us to
     revert the entire change.

     Note: We don't do this when the file was already replaced before because
     the revert-base is already present and has the original text base.

     ### This block can be removed once the new pristine store is in place */
  {
    svn_error_t *err;
    svn_wc__db_status_t status;

    err = svn_wc__db_base_get_info(&status, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, db, local_abspath, pool, pool);
    if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
      {
        /* There is no BASE node. Thus, we'll have nothing to save.  */
        svn_error_clear(err);
      }
    else if (err)
      return svn_error_return(err);
  }

  /* Set CHANGED_* to reflect the entry props in NEW_BASE_PROPS, and
     filter NEW_BASE_PROPS so it contains only regular props. */
  {
    apr_array_header_t *regular_props;
    apr_array_header_t *entry_props;

    SVN_ERR(svn_categorize_props(svn_prop_hash_to_array(new_base_props, pool),
                                 &entry_props, NULL, &regular_props,
                                 pool));

    /* Put regular props back into a hash table. */
    new_base_props = prop_hash_from_array(regular_props, pool);

    /* Get the change_* info from the entry props.  */
    SVN_ERR(accumulate_last_change(&changed_rev,
                                   &changed_date,
                                   &changed_author,
                                   db, local_abspath,
                                   entry_props, pool, pool));
  }

  /* Add some work items to install the properties.  */
  {
    if (new_props == NULL)
      {
        actual_props = NULL;
      }
    else
      {
        apr_array_header_t *prop_diffs;

        SVN_ERR(svn_prop_diffs(&prop_diffs, new_props, new_base_props,
                               pool));
        if (prop_diffs->nelts == 0)
          actual_props = NULL;
        else
          actual_props = new_props;
      }
  }

  /* Copy NEW_BASE_CONTENTS into a temporary file so our log can refer to
     it, and set TMP_TEXT_BASE_ABSPATH to its path.  Compute its
     NEW_TEXT_BASE_MD5_CHECKSUM and NEW_TEXT_BASE_SHA1_CHECKSUM as we copy. */
  {
    svn_stream_t *tmp_base_contents;

    SVN_ERR(svn_wc__open_writable_base(&tmp_base_contents,
                                       &tmp_text_base_abspath,
                                       &new_text_base_md5_checksum,
                                       &new_text_base_sha1_checksum,
                                       wc_ctx->db, local_abspath,
                                       pool, pool));
    SVN_ERR(svn_stream_copy3(new_base_contents, tmp_base_contents,
                             cancel_func, cancel_baton, pool));
  }

  /* If the caller gave us a new working file, copy it to a safe (temporary)
     location and set SOURCE_ABSPATH to that path. We'll then translate/copy
     that into place after the node's state has been created.  */
  if (new_contents)
    {
      const char *temp_dir_abspath;
      svn_stream_t *tmp_contents;

      SVN_ERR(svn_wc__db_temp_wcroot_tempdir(&temp_dir_abspath, db,
                                             local_abspath, pool, pool));
      SVN_ERR(svn_stream_open_unique(&tmp_contents, &source_abspath,
                                     temp_dir_abspath, svn_io_file_del_none,
                                     pool, pool));
      SVN_ERR(svn_stream_copy3(new_contents, tmp_contents,
                               cancel_func, cancel_baton, pool));
    }

  /* Install new text base for copied files. Added files do NOT have a
     text base.  */
  if (copyfrom_url != NULL)
    {
      SVN_ERR(svn_wc__db_pristine_install(db, tmp_text_base_abspath,
                                          new_text_base_sha1_checksum,
                                          new_text_base_md5_checksum, pool));
    }
  else
    {
      /* ### There's something wrong around here.  Sometimes (merge from a
         foreign repository, at least) we are called with copyfrom_url =
         NULL and an empty new_base_contents (and an empty set of
         new_base_props).  Why an empty "new base"?

         That happens in merge_tests.py 54,87,88,89,143.

         In that case, having been given this supposed "new base" file, we
         copy it and calculate its checksum but do not install it.  Why?
         That must be wrong.

         To crudely work around one issue with this, that we shouldn't
         record a checksum in the database if we haven't installed the
         corresponding pristine text, for now we'll just set the checksum
         to NULL.

         The proper solution is probably more like: the caller should pass
         NULL for the missing information, and this function should learn to
         handle that. */

      new_text_base_sha1_checksum = NULL;
      new_text_base_md5_checksum = NULL;
    }

  /* For added files without NEW_CONTENTS, then generate the working file
     from the provided "pristine" contents.  */
  if (new_contents == NULL && copyfrom_url == NULL)
    source_abspath = tmp_text_base_abspath;

  {
    svn_boolean_t record_fileinfo;

    /* If new contents were provided, then we do NOT want to record the
       file information. We assume the new contents do not match the
       "proper" values for TRANSLATED_SIZE and LAST_MOD_TIME.  */
    record_fileinfo = new_contents == NULL;

    /* Install the working copy file (with appropriate translation) from
       the appropriate source. SOURCE_ABSPATH will be NULL, indicating an
       installation from the pristine (available for copied/moved files),
       or it will specify a temporary file where we placed a "pristine"
       (for an added file) or a detranslated local-mods file.  */
    SVN_ERR(svn_wc__wq_build_file_install(&work_item,
                                          db, local_abspath,
                                          source_abspath,
                                          FALSE /* use_commit_times */,
                                          record_fileinfo,
                                          pool, pool));
    all_work_items = svn_wc__wq_merge(all_work_items, work_item, pool);

    /* If we installed from somewhere besides the official pristine, then
       it is a temporary file, which needs to be removed.  */
    if (source_abspath != NULL)
      {
        SVN_ERR(svn_wc__wq_build_file_remove(&work_item,
                                             db, source_abspath,
                                             pool, pool));
        all_work_items = svn_wc__wq_merge(all_work_items, work_item, pool);
      }
  }

  /* ### ideally, we would have a single DB operation, and queue the work
     ### items on that. for now, we'll queue them with the second call.  */

  SVN_ERR(svn_wc__db_op_copy_file(db, local_abspath,
                                  new_base_props,
                                  changed_rev,
                                  changed_date,
                                  changed_author,
                                  original_repos_relpath,
                                  original_root_url,
                                  original_uuid,
                                  copyfrom_rev,
                                  new_text_base_sha1_checksum,
                                  NULL /* conflict */,
                                  NULL /* work_items */,
                                  pool));

  /* ### if below fails, then the above db change would remain :-(  */

  SVN_ERR(svn_wc__db_op_set_props(db, local_abspath,
                                  actual_props,
                                  NULL /* conflict */,
                                  all_work_items,
                                  pool));

  return svn_error_return(svn_wc__wq_run(db, dir_abspath,
                                         cancel_func, cancel_baton,
                                         pool));
}
