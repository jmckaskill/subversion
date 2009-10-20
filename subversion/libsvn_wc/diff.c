/*
 * diff.c -- The diff editor for comparing the working copy against the
 *           repository.
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

/*
 * This code uses an svn_delta_editor_t editor driven by
 * svn_wc_crawl_revisions (like the update command) to retrieve the
 * differences between the working copy and the requested repository
 * version. Rather than updating the working copy, this new editor creates
 * temporary files that contain the pristine repository versions. When the
 * crawler closes the files the editor calls back to a client layer
 * function to compare the working copy and the temporary file. There is
 * only ever one temporary file in existence at any time.
 *
 * When the crawler closes a directory, the editor then calls back to the
 * client layer to compare any remaining files that may have been modified
 * locally. Added directories do not have corresponding temporary
 * directories created, as they are not needed.
 *
 * ### TODO: Replacements where the node kind changes needs support. It
 * mostly works when the change is in the repository, but not when it is
 * in the working copy.
 *
 * ### TODO: Do we need to support copyfrom?
 *
 */

#include <apr_hash.h>

#include "svn_error.h"
#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_hash.h"

#include "private/svn_wc_private.h"

#include "wc.h"
#include "props.h"
#include "adm_files.h"
#include "lock.h"
#include "entries.h"
#include "translate.h"

#include "svn_private_config.h"


/*-------------------------------------------------------------------------*/
/* A little helper function.

   You see, when we ask the server to update us to a certain revision,
   we construct the new fulltext, and then run

         'diff <repos_fulltext> <working_fulltext>'

   which is, of course, actually backwards from the repository's point
   of view.  It thinks we want to move from working->repos.

   So when the server sends property changes, they're effectively
   backwards from what we want.  We don't want working->repos, but
   repos->working.  So this little helper "reverses" the value in
   BASEPROPS and PROPCHANGES before we pass them off to the
   prop_changed() diff-callback.  */
static void
reverse_propchanges(apr_hash_t *baseprops,
                    apr_array_header_t *propchanges,
                    apr_pool_t *pool)
{
  int i;

  /* ### todo: research lifetimes for property values below */

  for (i = 0; i < propchanges->nelts; i++)
    {
      svn_prop_t *propchange
        = &APR_ARRAY_IDX(propchanges, i, svn_prop_t);

      const svn_string_t *original_value =
        apr_hash_get(baseprops, propchange->name, APR_HASH_KEY_STRING);

      if ((original_value == NULL) && (propchange->value != NULL))
        {
          /* found an addition.  make it look like a deletion. */
          apr_hash_set(baseprops, propchange->name, APR_HASH_KEY_STRING,
                       svn_string_dup(propchange->value, pool));
          propchange->value = NULL;
        }

      else if ((original_value != NULL) && (propchange->value == NULL))
        {
          /* found a deletion.  make it look like an addition. */
          propchange->value = svn_string_dup(original_value, pool);
          apr_hash_set(baseprops, propchange->name, APR_HASH_KEY_STRING,
                       NULL);
        }

      else if ((original_value != NULL) && (propchange->value != NULL))
        {
          /* found a change.  just swap the values.  */
          const svn_string_t *str = svn_string_dup(propchange->value, pool);
          propchange->value = svn_string_dup(original_value, pool);
          apr_hash_set(baseprops, propchange->name, APR_HASH_KEY_STRING, str);
        }
    }
}


/*-------------------------------------------------------------------------*/


/* Overall crawler editor baton.
 */
struct edit_baton {
  /* A wc db. */
  svn_wc__db_t *db;

  /* ANCHOR/TARGET represent the base of the hierarchy to be compared. */
  const char *anchor_path;
  const char *target;

  /* The absolute path of the anchor directory */
  const char *anchor_abspath;

  /* Target revision */
  svn_revnum_t revnum;

  /* Was the root opened? */
  svn_boolean_t root_opened;

  /* The callbacks and callback argument that implement the file comparison
     functions */
  const svn_wc_diff_callbacks4_t *callbacks;
  void *callback_baton;

  /* How does this diff descend? */
  svn_depth_t depth;

  /* Should this diff ignore node ancestry? */
  svn_boolean_t ignore_ancestry;

  /* Should this diff not compare copied files with their source? */
  svn_boolean_t show_copies_as_adds;

  /* Possibly diff repos against text-bases instead of working files. */
  svn_boolean_t use_text_base;

  /* Possibly show the diffs backwards. */
  svn_boolean_t reverse_order;

  /* Empty file used to diff adds / deletes */
  const char *empty_file;

  /* Hash whose keys are const char * changelist names. */
  apr_hash_t *changelist_hash;

  /* Cancel function/baton */
  svn_cancel_func_t cancel_func;
  void *cancel_baton;

  apr_pool_t *pool;
};

/* Directory level baton.
 */
struct dir_baton {
  /* Gets set if the directory is added rather than replaced/unchanged. */
  svn_boolean_t added;

  /* The depth at which this directory should be diffed. */
  svn_depth_t depth;

  /* The name and path of this directory as if they would be/are in the
      local working copy. */
  const char *name;
  const char *local_abspath;

  /* The "correct" path of the directory, but it may not exist in the
     working copy. */
  const char *path;

  /* Identifies those directory elements that get compared while running
     the crawler.  These elements should not be compared again when
     recursively looking for local modifications.

     This hash maps the full path of the entry to an unimportant value
     (presence in the hash is the important factor here, not the value
     itself).

     If the directory's properties have been compared, an item with hash
     key of "" (an empty string) will be present in the hash. */
  apr_hash_t *compared;

  /* The baton for the parent directory, or null if this is the root of the
     hierarchy to be compared. */
  struct dir_baton *parent_baton;

  /* The list of incoming BASE->repos propchanges. */
  apr_array_header_t *propchanges;

  /* The overall crawler editor baton. */
  struct edit_baton *eb;

  apr_pool_t *pool;
};

/* File level baton.
 */
struct file_baton {
  /* Gets set if the file is added rather than replaced. */
  svn_boolean_t added;

  /* The name and path of this file as if they would be/are in the
      local working copy. */
  const char *name;
  const char *local_abspath;

  /* PATH is the "correct" path of the file, but it may not exist in the
     working copy.  WC_PATH is a path we can use to make temporary files
     or open empty files; it doesn't necessarily exist either, but the
     directory part of it does. */
  const char *path;
  const char *wc_path;

 /* When constructing the requested repository version of the file, we
    drop the result into a file at TEMP_FILE_PATH. */
  const char *temp_file_path;

  /* The list of incoming BASE->repos propchanges. */
  apr_array_header_t *propchanges;

  /* APPLY_HANDLER/APPLY_BATON represent the delta applcation baton. */
  svn_txdelta_window_handler_t apply_handler;
  void *apply_baton;

  /* The overall crawler editor baton. */
  struct edit_baton *eb;

  struct dir_baton *parent_baton;

  apr_pool_t *pool;
};

/* Create a new edit baton. TARGET_PATH/ANCHOR are working copy paths
 * that describe the root of the comparison. CALLBACKS/CALLBACK_BATON
 * define the callbacks to compare files. DEPTH defines if and how to
 * descend into subdirectories; see public doc string for exactly how.
 * IGNORE_ANCESTRY defines whether to utilize node ancestry when
 * calculating diffs.  USE_TEXT_BASE defines whether to compare
 * against working files or text-bases.  REVERSE_ORDER defines which
 * direction to perform the diff.
 *
 * CHANGELISTS is a list of const char * changelist names, used to
 * filter diff output responses to only those items in one of the
 * specified changelists, empty (or NULL altogether) if no changelist
 * filtering is requested.
 */
static svn_error_t *
make_edit_baton(struct edit_baton **edit_baton,
                svn_wc__db_t *db,
                const char *anchor_path,
                const char *target,
                const svn_wc_diff_callbacks4_t *callbacks,
                void *callback_baton,
                svn_depth_t depth,
                svn_boolean_t ignore_ancestry,
                svn_boolean_t show_copies_as_adds,
                svn_boolean_t use_text_base,
                svn_boolean_t reverse_order,
                const apr_array_header_t *changelists,
                svn_cancel_func_t cancel_func,
                void *cancel_baton,
                apr_pool_t *pool)
{
  apr_hash_t *changelist_hash = NULL;
  struct edit_baton *eb;

  if (changelists && changelists->nelts)
    SVN_ERR(svn_hash_from_cstring_keys(&changelist_hash, changelists, pool));

  eb = apr_pcalloc(pool, sizeof(*eb));
  eb->db = db;
  eb->anchor_path = anchor_path;
  SVN_ERR(svn_dirent_get_absolute(&eb->anchor_abspath, eb->anchor_path, pool));
  eb->target = apr_pstrdup(pool, target);
  eb->callbacks = callbacks;
  eb->callback_baton = callback_baton;
  eb->depth = depth;
  eb->ignore_ancestry = ignore_ancestry;
  eb->show_copies_as_adds = show_copies_as_adds;
  eb->use_text_base = use_text_base;
  eb->reverse_order = reverse_order;
  eb->changelist_hash = changelist_hash;
  eb->cancel_func = cancel_func;
  eb->cancel_baton = cancel_baton;  
  eb->pool = pool;

  *edit_baton = eb;
  return SVN_NO_ERROR;
}

/* Create a new directory baton.  PATH is the directory path,
 * including anchor_path.  ADDED is set if this directory is being
 * added rather than replaced.  PARENT_BATON is the baton of the
 * parent directory, it will be null if this is the root of the
 * comparison hierarchy.  The directory and its parent may or may not
 * exist in the working copy.  EDIT_BATON is the overall crawler
 * editor baton.
 */
static struct dir_baton *
make_dir_baton(const char *path,
               struct dir_baton *parent_baton,
               struct edit_baton *eb,
               svn_boolean_t added,
               svn_depth_t depth,
               apr_pool_t *pool)
{
  struct dir_baton *db = apr_pcalloc(pool, sizeof(*db));

  db->eb = eb;
  db->parent_baton = parent_baton;
  db->added = added;
  db->depth = depth;
  db->pool = pool;
  db->propchanges = apr_array_make(pool, 1, sizeof(svn_prop_t));
  db->compared = apr_hash_make(db->pool);
  db->path = path;

  db->name = svn_dirent_basename(path, NULL);

  if (parent_baton != NULL)
    db->local_abspath = svn_dirent_join(parent_baton->local_abspath, db->name,
                                        pool);
  else
    db->local_abspath = apr_pstrdup(pool, eb->anchor_abspath);

  return db;
}

/* Create a new file baton.  PATH is the file path, including
 * anchor_path.  ADDED is set if this file is being added rather than
 * replaced.  PARENT_BATON is the baton of the parent directory.
 * The directory and its parent may or may not exist in the working copy.
 */
static struct file_baton *
make_file_baton(const char *path,
                svn_boolean_t added,
                struct dir_baton *parent_baton,
                apr_pool_t *pool)
{
  struct file_baton *fb = apr_pcalloc(pool, sizeof(*fb));
  struct edit_baton *eb = parent_baton->eb;

  fb->eb = eb;
  fb->parent_baton = parent_baton;
  fb->added = added;
  fb->pool = pool;
  fb->propchanges  = apr_array_make(pool, 1, sizeof(svn_prop_t));
  fb->path = path;

  fb->name = svn_dirent_basename(path, NULL);
  fb->local_abspath = svn_dirent_join(parent_baton->local_abspath, fb->name,
                                      pool);

  /* If the parent directory is added rather than replaced it does not
     exist in the working copy.  Determine a working copy path whose
     directory part does exist; we can use that to create temporary
     files.  It doesn't matter whether the file part exists in the
     directory. */
  if (parent_baton->added)
    {
      struct dir_baton *wc_dir_baton = parent_baton;

      /* Ascend until a directory is not being added, this will be a
         directory that does exist. This must terminate since the root of
         the comparison cannot be added. */
      while (wc_dir_baton->added)
        wc_dir_baton = wc_dir_baton->parent_baton;

      fb->wc_path = svn_dirent_join(wc_dir_baton->path, "unimportant",
                                    fb->pool);
    }
  else
    {
      fb->wc_path = fb->path;
    }

  return fb;
}

/* Get the empty file associated with the edit baton. This is cached so
 * that it can be reused, all empty files are the same.
 */
static svn_error_t *
get_empty_file(struct edit_baton *b,
               const char **empty_file)
{
  /* Create the file if it does not exist */
  /* Note that we tried to use /dev/null in r17220, but
     that won't work on Windows: it's impossible to stat NUL */
  if (!b->empty_file)
    {
      SVN_ERR(svn_io_open_unique_file3(NULL, &b->empty_file, NULL,
                                       svn_io_file_del_on_pool_cleanup,
                                       b->pool, b->pool));
    }

  *empty_file = b->empty_file;

  return SVN_NO_ERROR;
}


/* Return the value of the svn:mime-type property held in PROPS, or NULL
   if no such property exists. */
static const char *
get_prop_mimetype(apr_hash_t *props)
{
  const svn_string_t *mimetype_val;

  mimetype_val = apr_hash_get(props,
                              SVN_PROP_MIME_TYPE,
                              strlen(SVN_PROP_MIME_TYPE));
  return (mimetype_val) ? mimetype_val->data : NULL;
}


/* Set *MIMETYPE to the BASE version of the svn:mime-type property of
   file LOCAL_ABSPATH, using DB, or to NULL if no such property exists.
   BASEPROPS is optional: if present, use it to cache the BASE properties
   of the file.

   Return the property value and property hash allocated in RESULT_POOL,
   use SCRATCH_POOL for temporary accesses.
*/
static svn_error_t *
get_base_mimetype(const char **mimetype,
                  apr_hash_t **baseprops,
                  svn_wc__db_t *db,
                  const char *local_abspath,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  apr_hash_t *props = NULL;

  if (baseprops == NULL)
    baseprops = &props;

  if (*baseprops == NULL)
    SVN_ERR(svn_wc__internal_propdiff(NULL, baseprops, db, local_abspath,
                                      result_pool, scratch_pool));

  *mimetype = get_prop_mimetype(*baseprops);

  return SVN_NO_ERROR;
}


/* Set *MIMETYPE to the WORKING version of the svn:mime-type property
   of file PATH, using ADM_ACCESS, or to NULL if no such property exists.
   WORKINGPROPS is optional: if present, use it to cache the WORKING
   properties of the file.

   Return the property value and property hash allocated in POOL.
*/
static svn_error_t *
get_working_mimetype(const char **mimetype,
                     apr_hash_t **workingprops,
                     const char *local_abspath,
                     svn_wc__db_t *db,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  apr_hash_t *props = NULL;

  if (workingprops == NULL)
    workingprops = &props;

  if (*workingprops == NULL)
    SVN_ERR(svn_wc__load_props(NULL, workingprops, NULL, db, local_abspath,
                               result_pool, scratch_pool));

  *mimetype = get_prop_mimetype(*workingprops);

  return SVN_NO_ERROR;
}

/* Return the property hash resulting from combining PROPS and PROPCHANGES.
 *
 * A note on pool usage: The returned hash and hash keys are allocated in
 * the same pool as PROPS, but the hash values will be taken directly from
 * either PROPS or PROPCHANGES, as appropriate.  Caller must therefore
 * ensure that the returned hash is only used for as long as PROPS and
 * PROPCHANGES remain valid.
 */
static apr_hash_t *
apply_propchanges(apr_hash_t *props,
                  apr_array_header_t *propchanges)
{
  apr_hash_t *newprops = apr_hash_copy(apr_hash_pool_get(props), props);
  int i;

  for (i = 0; i < propchanges->nelts; ++i)
    {
      const svn_prop_t *prop = &APR_ARRAY_IDX(propchanges, i, svn_prop_t);
      apr_hash_set(newprops, prop->name, APR_HASH_KEY_STRING, prop->value);
    }

  return newprops;
}


/* Called by directory_elements_diff when a file is to be compared. At this
 * stage we are dealing with a file that does exist in the working copy.
 *
 * DIR_BATON is the parent directory baton, PATH is the path to the file to
 * be compared.
 *
 * Do all allocation in POOL.
 *
 * ### TODO: Need to work on replace if the new filename used to be a
 * directory.
 */
static svn_error_t *
file_diff(struct dir_baton *db,
          const char *path,
          apr_pool_t *pool)
{
  struct edit_baton *eb = db->eb;
  const char *textbase;
  const char *empty_file;
  svn_boolean_t replaced;
  svn_wc__db_status_t status;
  svn_revnum_t revision;
  apr_array_header_t *propchanges = NULL;
  apr_hash_t *baseprops = NULL;
  const char *local_abspath;

  SVN_ERR_ASSERT(! eb->use_text_base);

  local_abspath = svn_dirent_join(db->local_abspath,
                                  svn_dirent_basename(path, pool),
                                  pool);

  /* If the item is not a member of a specified changelist (and there are
     some specified changelists), skip it. */
  if (! svn_wc__internal_changelist_match(eb->db, local_abspath,
                                          eb->changelist_hash, pool))
    return SVN_NO_ERROR;

  SVN_ERR(svn_wc__db_read_info(&status, NULL, &revision, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, eb->db, local_abspath,
                               pool, pool));

  if (status == svn_wc__db_status_added)
    SVN_ERR(svn_wc__db_scan_addition(&status, NULL, NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, eb->db, local_abspath,
                                     pool, pool));

  /* Prep these two paths early. */
  SVN_ERR(svn_wc__text_base_path(&textbase, eb->db, local_abspath, FALSE,
                                 pool));

  /* If the regular text base is not there, we fall back to the revert
     text base (if that's not present either, we'll error later).  But
     the logic here is subtler than one might at first expect.

     When the file has some non-replacement scheduling, then it can be
     expected to still have its regular text base.  But what about
     when it's replaced or replaced-with-history?  In both cases, a
     revert text-base will be present; in the latter case only, a
     regular text-base be present as well.  So which text-base do we
     want to use for the diff?

     One could argue that we should never diff against the revert
     base, and instead diff against the empty-file for both types of
     replacement.  After all, there is no ancestry relationship
     between the working file and the base file.  But my guess is that
     in practice, users want to see the diff between their working
     file and "the nearest versioned thing", whatever that is.  I'm
     not 100% sure this is the right decision, but it at least seems
     to match our test suite's expectations. */
  {
    svn_node_kind_t kind;
    SVN_ERR(svn_io_check_path(textbase, &kind, pool));
    if (kind == svn_node_none)
      SVN_ERR(svn_wc__text_revert_path(&textbase, eb->db, local_abspath,
                                       pool));
  }

  SVN_ERR(get_empty_file(eb, &empty_file));

  /* Get property diffs if this is not schedule delete. */
  if (status != svn_wc__db_status_deleted)
    {
      svn_boolean_t modified;

      SVN_ERR(svn_wc__props_modified(&modified, eb->db, local_abspath,
                                     pool));
      if (modified)
        SVN_ERR(svn_wc__internal_propdiff(&propchanges, &baseprops, eb->db,
                                          local_abspath, pool, pool));
      else
        propchanges = apr_array_make(pool, 1, sizeof(svn_prop_t));
    }
  else
    {
      SVN_ERR(svn_wc__internal_propdiff(NULL, &baseprops, eb->db,
                                        local_abspath, pool, pool));
    }

  SVN_ERR(svn_wc__internal_is_replaced(&replaced, eb->db, local_abspath,
                                       pool));

  /* Delete compares text-base against empty file, modifications to the
   * working-copy version of the deleted file are not wanted.
   * Replace is treated like a delete plus an add: two comparisons are
   * generated, first one for the delete and then one for the add.
   * However, if this file was replaced and we are ignoring ancestry,
   * report it as a normal file modification instead. */
  if ((! replaced && status == svn_wc__db_status_deleted) ||
      (replaced && ! eb->ignore_ancestry))
    {
      const char *base_mimetype;

      /* Get svn:mime-type from BASE props of PATH. */
      SVN_ERR(get_base_mimetype(&base_mimetype, &baseprops, eb->db,
                                local_abspath, pool, pool));

      SVN_ERR(eb->callbacks->file_deleted(NULL, NULL, NULL, path,
                                          textbase,
                                          empty_file,
                                          base_mimetype,
                                          NULL,
                                          baseprops,
                                          eb->callback_baton,
                                          pool));

      if (! (replaced && ! eb->ignore_ancestry))
        {
          /* We're here only for showing a delete, so we're done. */
          return SVN_NO_ERROR;
        }
    }

 /* Now deal with showing additions, or the add-half of replacements.
  * If the item is schedule-add *with history*, then we usually want
  * to see the usual working vs. text-base comparison, which will show changes
  * made since the file was copied.  But in case we're showing copies as adds,
  * we need to compare the copied file to the empty file. */
  if ((! replaced && status == svn_wc__db_status_added) ||
     (replaced && ! eb->ignore_ancestry) ||
     ((status == svn_wc__db_status_copied ||
       status == svn_wc__db_status_moved_here) && eb->show_copies_as_adds))
    {
      const char *translated = NULL;
      const char *working_mimetype;

      /* Get svn:mime-type from working props of PATH. */
      SVN_ERR(get_working_mimetype(&working_mimetype, NULL, local_abspath,
                                   eb->db, pool, pool));

      SVN_ERR(svn_wc__internal_translated_file(
              &translated, local_abspath, eb->db, local_abspath,
              SVN_WC_TRANSLATE_TO_NF | SVN_WC_TRANSLATE_USE_GLOBAL_TMP,
              pool, pool));

      SVN_ERR(eb->callbacks->file_added(NULL, NULL, NULL, NULL, path,
                                        empty_file,
                                        translated,
                                        0, revision,
                                        NULL,
                                        working_mimetype,
                                        NULL, SVN_INVALID_REVNUM,
                                        propchanges, baseprops,
                                        eb->callback_baton,
                                        pool));
    }
  else
    {
      svn_boolean_t modified;
      const char *translated = NULL;

      /* Here we deal with showing pure modifications. */

      SVN_ERR(svn_wc__internal_text_modified_p(&modified, eb->db,
                                               local_abspath, FALSE, TRUE,
                                               pool));
      if (modified)
        {
          /* Note that this might be the _second_ time we translate
             the file, as svn_wc__text_modified_internal_p() might have used a
             tmp translated copy too.  But what the heck, diff is
             already expensive, translating twice for the sake of code
             modularity is liveable. */
          SVN_ERR(svn_wc__internal_translated_file(
                   &translated, local_abspath, eb->db, local_abspath,
                   SVN_WC_TRANSLATE_TO_NF | SVN_WC_TRANSLATE_USE_GLOBAL_TMP,
                   pool, pool));
        }

      if (modified || propchanges->nelts > 0)
        {
          const char *base_mimetype;
          const char *working_mimetype;

          /* Get svn:mime-type for both base and working file. */
          SVN_ERR(get_base_mimetype(&base_mimetype, &baseprops, eb->db,
                                    local_abspath, pool, pool));
          SVN_ERR(get_working_mimetype(&working_mimetype, NULL, local_abspath,
                                       eb->db, pool, pool));

          SVN_ERR(eb->callbacks->file_changed(NULL, NULL, NULL, NULL,
                                              path,
                                              modified ? textbase : NULL,
                                              translated,
                                              revision,
                                              SVN_INVALID_REVNUM,
                                              base_mimetype,
                                              working_mimetype,
                                              propchanges, baseprops,
                                              eb->callback_baton,
                                              pool));
        }
    }

  return SVN_NO_ERROR;
}

/* Called when the directory is closed to compare any elements that have
 * not yet been compared.  This identifies local, working copy only
 * changes.  At this stage we are dealing with files/directories that do
 * exist in the working copy.
 *
 * DIR_BATON is the baton for the directory.
 */
static svn_error_t *
directory_elements_diff(struct dir_baton *db)
{
  const apr_array_header_t *children;
  int i;
  svn_boolean_t in_anchor_not_target;
  apr_pool_t *iterpool;
  struct edit_baton *eb = db->eb;

  /* This directory should have been unchanged or replaced, not added,
     since an added directory can only contain added files and these will
     already have been compared. */
  SVN_ERR_ASSERT(!db->added);

  /* Everything we do below is useless if we are comparing to BASE. */
  if (eb->use_text_base)
    return SVN_NO_ERROR;

  /* Determine if this is the anchor directory if the anchor is different
     to the target. When the target is a file, the anchor is the parent
     directory and if this is that directory the non-target entries must be
     skipped. */
  in_anchor_not_target =
    (*eb->target
      && (! svn_path_compare_paths(db->path, eb->anchor_path)));

  /* Check for local property mods on this directory, if we haven't
     already reported them and we aren't changelist-filted.
     ### it should be noted that we do not currently allow directories
     ### to be part of changelists, so if a changelist is provided, the
     ### changelist check will always fail. */
  if (svn_wc__internal_changelist_match(eb->db, db->local_abspath,
                                        eb->changelist_hash, db->pool)
      && (! in_anchor_not_target)
      && (! apr_hash_get(db->compared, "", 0)))
    {
      svn_boolean_t modified;

      SVN_ERR(svn_wc__props_modified(&modified, eb->db, db->local_abspath,
                                     db->pool));
      if (modified)
        {
          apr_array_header_t *propchanges;
          apr_hash_t *baseprops;

          SVN_ERR(svn_wc__internal_propdiff(&propchanges, &baseprops,
                                            eb->db, db->local_abspath,
                                            db->pool, db->pool));

          SVN_ERR(eb->callbacks->dir_props_changed(db->local_abspath,
                                                   NULL, NULL,
                                                   db->path,
                                                   propchanges, baseprops,
                                                   eb->callback_baton,
                                                   db->pool));
        }
    }

  if (db->depth == svn_depth_empty && !in_anchor_not_target)
    return SVN_NO_ERROR;

  iterpool = svn_pool_create(db->pool);

  SVN_ERR(svn_wc__db_read_children(&children, eb->db, db->local_abspath,
                                   db->pool, iterpool));

  for (i = 0; i < children->nelts; i++)
    {
      const char *name = APR_ARRAY_IDX(children, i, const char*);
      const svn_wc_entry_t *entry;
      struct dir_baton *subdir_baton;
      const char *child_abpath, *path;
      svn_boolean_t hidden;

      svn_pool_clear(iterpool);

      if (eb->cancel_func)
        SVN_ERR(eb->cancel_func(eb->cancel_baton));

      child_abpath = svn_dirent_join(db->local_abspath, name, iterpool);

      SVN_ERR(svn_wc__db_node_hidden(&hidden, eb->db, child_abpath, iterpool));

      if (hidden)
        continue;

      /* In the anchor directory, if the anchor is not the target then all
         entries other than the target should not be diff'd. Running diff
         on one file in a directory should not diff other files in that
         directory. */
      if (in_anchor_not_target && strcmp(eb->target, name))
        continue;

      path = svn_dirent_join(db->path, name, iterpool);

      /* Skip entry if it is in the list of entries already diff'd. */
      if (apr_hash_get(db->compared, path, APR_HASH_KEY_STRING))
        continue;

      SVN_ERR(svn_wc__get_entry(&entry, eb->db, child_abpath, FALSE,
                                svn_node_unknown, FALSE, iterpool, iterpool));

      switch (entry->kind)
        {
        case svn_node_file:
          SVN_ERR(file_diff(db, path, iterpool));
          break;

        case svn_node_dir:
          if (entry->schedule == svn_wc_schedule_replace)
            {
              /* ### TODO: Don't know how to do this bit. How do I get
                 information about what is being replaced? If it was a
                 directory then the directory elements are also going to be
                 deleted. We need to show deletion diffs for these
                 files. If it was a file we need to show a deletion diff
                 for that file. */
            }

          /* Check the subdir if in the anchor (the subdir is the target), or
             if recursive */
          if (in_anchor_not_target
              || (db->depth > svn_depth_files)
              || (db->depth == svn_depth_unknown))
            {
              svn_depth_t depth_below_here = db->depth;

              if (depth_below_here == svn_depth_immediates)
                depth_below_here = svn_depth_empty;

              subdir_baton = make_dir_baton(path, db,
                                            db->eb,
                                            FALSE,
                                            depth_below_here,
                                            iterpool);

              SVN_ERR(directory_elements_diff(subdir_baton));
            }
          break;

        default:
          break;
        }
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Report an existing file in the working copy (either in BASE or WORKING)
 * as having been added.
 *
 * DIR_BATON is the parent directory baton, ADM_ACCESS/PATH is the path
 * to the file to be compared.
 *
 * Do all allocation in POOL.
 */
static svn_error_t *
report_wc_file_as_added(struct dir_baton *db,
                        const char *path,
                        apr_pool_t *pool)
{
  struct edit_baton *eb = db->eb;
  apr_hash_t *emptyprops;
  const char *mimetype;
  apr_hash_t *wcprops = NULL;
  apr_array_header_t *propchanges;
  const char *empty_file;
  const char *source_file;
  const char *translated_file;
  const char *local_abspath;
  svn_wc__db_status_t status;
  svn_revnum_t revision;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));

  /* If this entry is filtered by changelist specification, do nothing. */
  if (! svn_wc__internal_changelist_match(eb->db, local_abspath,
                                          eb->changelist_hash, pool))
    return SVN_NO_ERROR;

  SVN_ERR(get_empty_file(eb, &empty_file));

  SVN_ERR(svn_wc__db_read_info(&status, &revision, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, eb->db, local_abspath,
                               pool, pool));

  if (status == svn_wc__db_status_added)
    SVN_ERR(svn_wc__db_scan_addition(&status, NULL, NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, eb->db, local_abspath,
                                     pool, pool));

  /* We can't show additions for files that don't exist. */
  SVN_ERR_ASSERT(status != svn_wc__db_status_deleted || eb->use_text_base);

  /* If the file was added *with history*, then we don't want to
     see a comparison to the empty file;  we want the usual working
     vs. text-base comparison. */
  if (status == svn_wc__db_status_copied ||
      status == svn_wc__db_status_moved_here)
    {
      /* Don't show anything if we're comparing to BASE, since by
         definition there can't be any local modifications. */
      if (eb->use_text_base)
        return SVN_NO_ERROR;

      /* Otherwise show just the local modifications. */
      return file_diff(db, path, pool);
    }

  emptyprops = apr_hash_make(pool);

  if (eb->use_text_base)
    SVN_ERR(get_base_mimetype(&mimetype, &wcprops, eb->db, local_abspath,
                              pool, pool));
  else
    SVN_ERR(get_working_mimetype(&mimetype, &wcprops, local_abspath,
                                 eb->db, pool, pool));

  SVN_ERR(svn_prop_diffs(&propchanges,
                         wcprops, emptyprops, pool));


  if (eb->use_text_base)
    SVN_ERR(svn_wc__text_base_path(&source_file, eb->db, local_abspath, FALSE,
                                   pool));
  else
    source_file = path;

  SVN_ERR(svn_wc__internal_translated_file(
           &translated_file, source_file, eb->db, local_abspath,
           SVN_WC_TRANSLATE_TO_NF | SVN_WC_TRANSLATE_USE_GLOBAL_TMP,
           pool, pool));

  SVN_ERR(eb->callbacks->file_added(db->local_abspath,
                                    NULL, NULL, NULL,
                                    path,
                                    empty_file, translated_file,
                                    0, revision,
                                    NULL, mimetype,
                                    NULL, SVN_INVALID_REVNUM,
                                    propchanges, emptyprops,
                                    eb->callback_baton,
                                    pool));

  return SVN_NO_ERROR;
}

/* Report an existing directory in the working copy (either in BASE
 * or WORKING) as having been added.  If recursing, also report any
 * subdirectories as added.
 *
 * DIR_BATON is the baton for the directory.
 *
 * Do all allocation in POOL.
 */
static svn_error_t *
report_wc_directory_as_added(struct dir_baton *db,
                             apr_pool_t *pool)
{
  struct edit_baton *eb = db->eb;
  apr_hash_t *emptyprops = apr_hash_make(pool), *wcprops = NULL;
  apr_array_header_t *propchanges;
  const apr_array_header_t *children;
  int i;
  apr_pool_t *iterpool;
  const char *dir_abspath;

  SVN_ERR(svn_dirent_get_absolute(&dir_abspath, db->path, pool));

  /* If this directory passes changelist filtering, get its BASE or
     WORKING properties, as appropriate, and simulate their
     addition.
     ### it should be noted that we do not currently allow directories
     ### to be part of changelists, so if a changelist is provided, this
     ### check will always fail. */
  if (svn_wc__internal_changelist_match(eb->db, dir_abspath,
                                        eb->changelist_hash, pool))
    {
      if (eb->use_text_base)
        SVN_ERR(svn_wc__internal_propdiff(NULL, &wcprops, eb->db, dir_abspath,
                                          pool, pool));
      else
        SVN_ERR(svn_wc__load_props(NULL, &wcprops, NULL, eb->db, dir_abspath,
                                   pool, pool));

      SVN_ERR(svn_prop_diffs(&propchanges, wcprops, emptyprops, pool));

      if (propchanges->nelts > 0)
        SVN_ERR(eb->callbacks->dir_props_changed(db->local_abspath,
                                                 NULL, NULL,
                                                 db->path,
                                                 propchanges, emptyprops,
                                                 eb->callback_baton,
                                                 pool));
    }

  /* Report the addition of the directory's contents. */
  iterpool = svn_pool_create(pool);

  SVN_ERR(svn_wc__db_read_children(&children, eb->db, dir_abspath,
                                   pool, iterpool));

  for (i = 0; i < children->nelts; i++)
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);
      const char *child_abspath, *path;
      const svn_wc_entry_t *entry;
      svn_boolean_t hidden;

      svn_pool_clear(iterpool);

      if (eb->cancel_func)
        SVN_ERR(eb->cancel_func(eb->cancel_baton));

      child_abspath = svn_dirent_join(dir_abspath, name, iterpool);

      SVN_ERR(svn_wc__db_node_hidden(&hidden, eb->db, child_abspath,
                                     iterpool));

      if (hidden)
        continue;

      SVN_ERR(svn_wc__get_entry(&entry, eb->db, child_abspath, FALSE,
                                svn_node_unknown, FALSE, iterpool, iterpool));

      /* If comparing against WORKING, skip entries that are
         schedule-deleted - they don't really exist. */
      if (!eb->use_text_base && entry->schedule == svn_wc_schedule_delete)
        continue;

      path = svn_dirent_join(db->path, name, iterpool);

      switch (entry->kind)
        {
        case svn_node_file:
          SVN_ERR(report_wc_file_as_added(db, path, iterpool));
          break;

        case svn_node_dir:
          if (db->depth > svn_depth_files || db->depth == svn_depth_unknown)
            {
              svn_depth_t depth_below_here = db->depth;
              struct dir_baton *subdir_baton;

              if (depth_below_here == svn_depth_immediates)
                depth_below_here = svn_depth_empty;

              subdir_baton = make_dir_baton(path, db, eb, FALSE,
                                            depth_below_here,
                                            iterpool);

              SVN_ERR(report_wc_directory_as_added(subdir_baton, iterpool));
            }
          break;

        default:
          break;
        }
    }
  
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


/* An editor function. */
static svn_error_t *
set_target_revision(void *edit_baton,
                    svn_revnum_t target_revision,
                    apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;
  eb->revnum = target_revision;

  return SVN_NO_ERROR;
}

/* An editor function. The root of the comparison hierarchy */
static svn_error_t *
open_root(void *edit_baton,
          svn_revnum_t base_revision,
          apr_pool_t *dir_pool,
          void **root_baton)
{
  struct edit_baton *eb = edit_baton;
  struct dir_baton *db;

  eb->root_opened = TRUE;
  db = make_dir_baton(eb->anchor_path, NULL, eb, FALSE, eb->depth, dir_pool);
  *root_baton = db;

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
delete_entry(const char *path,
             svn_revnum_t base_revision,
             void *parent_baton,
             apr_pool_t *pool)
{
  struct dir_baton *pb = parent_baton;
  struct edit_baton *eb = pb->eb;
  const svn_wc_entry_t *entry;
  struct dir_baton *db;
  const char *empty_file;
  const char *full_path = svn_dirent_join(eb->anchor_path, path,
                                          pb->pool);
  const char *name = svn_dirent_basename(path, pool);
  const char *local_abspath = svn_dirent_join(pb->local_abspath, name, pool);

  SVN_ERR(svn_wc__get_entry(&entry, eb->db, local_abspath, TRUE,
                            svn_node_unknown, FALSE, pool, pool));

  /* So, it turns out that this can be NULL in at least one actual case,
     if you do a nonrecursive checkout and the diff involves the addition
     of one of the directories that is not present due to the fact that
     your checkout is nonrecursive.  There isn't really a good way to be
     sure though, since nonrecursive checkouts suck, and don't leave any
     indication in .svn/entries that the directories in question are just
     missing. */
  if (! entry)
    return SVN_NO_ERROR;

  /* Mark this entry as compared in the parent directory's baton. */
  apr_hash_set(pb->compared, full_path, APR_HASH_KEY_STRING, "");

  /* If comparing against WORKING, skip entries that are schedule-deleted
     - they don't really exist. */
  if (!eb->use_text_base && entry->schedule == svn_wc_schedule_delete)
    return SVN_NO_ERROR;

  SVN_ERR(get_empty_file(pb->eb, &empty_file));
  switch (entry->kind)
    {
    case svn_node_file:
      /* A delete is required to change working-copy into requested
         revision, so diff should show this as an add. Thus compare
         the empty file against the current working copy.  If
         'reverse_order' is set, then show a deletion. */

      if (eb->reverse_order)
        {
          /* Whenever showing a deletion, we show the text-base vanishing. */
          /* ### This is wrong if we're diffing WORKING->repos. */
          const char *textbase;

          apr_hash_t *baseprops = NULL;
          const char *base_mimetype;

          SVN_ERR(svn_wc__text_base_path(&textbase, eb->db, local_abspath,
                                         FALSE, pool));

          SVN_ERR(get_base_mimetype(&base_mimetype, &baseprops, eb->db,
                                    local_abspath, pool, pool));

          SVN_ERR(eb->callbacks->file_deleted(NULL, NULL, NULL, full_path,
                                              textbase,
                                              empty_file,
                                              base_mimetype,
                                              NULL,
                                              baseprops,
                                              eb->callback_baton,
                                              pool));
        }
      else
        {
          /* Or normally, show the working file being added. */
          SVN_ERR(report_wc_file_as_added(pb, full_path, pool));
        }
      break;
    case svn_node_dir:
      db = make_dir_baton(full_path, pb, pb->eb, FALSE,
                          svn_depth_infinity, pool);
      /* A delete is required to change working-copy into requested
         revision, so diff should show this as an add. */
      SVN_ERR(report_wc_directory_as_added(db, pool));

    default:
      break;
    }

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
add_directory(const char *path,
              void *parent_baton,
              const char *copyfrom_path,
              svn_revnum_t copyfrom_revision,
              apr_pool_t *dir_pool,
              void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct dir_baton *db;
  const char *full_path;
  svn_depth_t subdir_depth = (pb->depth == svn_depth_immediates)
                              ? svn_depth_empty : pb->depth;

  /* ### TODO: support copyfrom? */

  full_path = svn_dirent_join(pb->eb->anchor_path, path, dir_pool);
  db = make_dir_baton(full_path, pb, pb->eb, TRUE, subdir_depth,
                      dir_pool);
  *child_baton = db;

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
open_directory(const char *path,
               void *parent_baton,
               svn_revnum_t base_revision,
               apr_pool_t *dir_pool,
               void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct dir_baton *db;
  const char *full_path;
  svn_depth_t subdir_depth = (pb->depth == svn_depth_immediates)
                              ? svn_depth_empty : pb->depth;

  /* Allocate path from the parent pool since the memory is used in the
     parent's compared hash */
  full_path = svn_dirent_join(pb->eb->anchor_path, path, pb->pool);
  db = make_dir_baton(full_path, pb, pb->eb, FALSE, subdir_depth, dir_pool);
  *child_baton = db;

  return SVN_NO_ERROR;
}


/* An editor function.  When a directory is closed, all the directory
 * elements that have been added or replaced will already have been
 * diff'd. However there may be other elements in the working copy
 * that have not yet been considered.  */
static svn_error_t *
close_directory(void *dir_baton,
                apr_pool_t *pool)
{
  struct dir_baton *db = dir_baton;
  struct dir_baton *pb = db->parent_baton;
  struct edit_baton *eb = db->eb;

  /* Report the property changes on the directory itself, if necessary. */
  if (db->propchanges->nelts > 0)
    {
      /* The working copy properties at the base of the wc->repos comparison:
         either BASE or WORKING. */
      apr_hash_t *originalprops;

      if (db->added)
        {
          originalprops = apr_hash_make(db->pool);
        }
      else
        {
          if (db->eb->use_text_base)
            {
              SVN_ERR(svn_wc__internal_propdiff(NULL, &originalprops, eb->db,
                                                db->local_abspath, pool, pool));
            }
          else
            {
              apr_hash_t *base_props, *repos_props;

              SVN_ERR(svn_wc__load_props(NULL, &originalprops, NULL,
                                         eb->db, db->local_abspath, pool, pool));

              /* Load the BASE and repository directory properties. */
              SVN_ERR(svn_wc__internal_propdiff(NULL, &base_props, eb->db,
                                                db->local_abspath, pool, pool));

              repos_props = apply_propchanges(base_props, db->propchanges);

              /* Recalculate b->propchanges as the change between WORKING
                 and repos. */
              SVN_ERR(svn_prop_diffs(&db->propchanges,
                                     repos_props, originalprops, db->pool));
            }
        }

      if (!eb->reverse_order)
        reverse_propchanges(originalprops, db->propchanges, db->pool);

      SVN_ERR(eb->callbacks->dir_props_changed(NULL, NULL, NULL,
                                               db->path,
                                               db->propchanges,
                                               originalprops,
                                               eb->callback_baton,
                                               pool));

      /* Mark the properties of this directory as having already been
         compared so that we know not to show any local modifications
         later on. */
      apr_hash_set(db->compared, "", 0, "");
    }

  /* Report local modifications for this directory.  Skip added
     directories since they can only contain added elements, all of
     which have already been diff'd. */
  if (!db->added)
    SVN_ERR(directory_elements_diff(db));

  /* Mark this directory as compared in the parent directory's baton,
     unless this is the root of the comparison. */
  if (pb)
    apr_hash_set(pb->compared, db->path, APR_HASH_KEY_STRING, "");

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
add_file(const char *path,
         void *parent_baton,
         const char *copyfrom_path,
         svn_revnum_t copyfrom_revision,
         apr_pool_t *file_pool,
         void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *fb;
  const char *full_path;

  /* ### TODO: support copyfrom? */

  full_path = svn_dirent_join(pb->eb->anchor_path, path, file_pool);
  fb = make_file_baton(full_path, TRUE, pb, file_pool);
  *file_baton = fb;

  /* Add this filename to the parent directory's list of elements that
     have been compared. */
  apr_hash_set(pb->compared, apr_pstrdup(pb->pool, full_path),
               APR_HASH_KEY_STRING, "");

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
open_file(const char *path,
          void *parent_baton,
          svn_revnum_t base_revision,
          apr_pool_t *file_pool,
          void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *fb;
  const char *full_path;

  full_path = svn_dirent_join(pb->eb->anchor_path, path, file_pool);
  fb = make_file_baton(full_path, FALSE, pb, file_pool);
  *file_baton = fb;

  /* Add this filename to the parent directory's list of elements that
     have been compared. */
  apr_hash_set(pb->compared, apr_pstrdup(pb->pool, full_path),
               APR_HASH_KEY_STRING, "");

  return SVN_NO_ERROR;
}

/* Do the work of applying the text delta. */
static svn_error_t *
window_handler(svn_txdelta_window_t *window,
               void *window_baton)
{
  struct file_baton *fb = window_baton;

  return fb->apply_handler(window, fb->apply_baton);
}

/* An editor function. */
static svn_error_t *
apply_textdelta(void *file_baton,
                const char *base_checksum,
                apr_pool_t *pool,
                svn_txdelta_window_handler_t *handler,
                void **handler_baton)
{
  struct file_baton *fb = file_baton;
  struct edit_baton *eb = fb->eb;
  const svn_wc_entry_t *entry;
  const char *parent, *base_name;
  const char *temp_dir;
  svn_stream_t *source;
  svn_stream_t *temp_stream;
  svn_error_t *err;

  err = svn_wc__get_entry(&entry, eb->db, fb->local_abspath, TRUE,
                          svn_node_file, FALSE, pool, pool);

  if (err && err->apr_err == SVN_ERR_WC_MISSING)
    {
      svn_error_clear(err);
      entry = NULL;
    }
  else
    SVN_ERR(err);

  svn_dirent_split(fb->wc_path, &parent, &base_name, fb->pool);

  /* Check to see if there is a schedule-add with history entry in
     the current working copy.  If so, then this is not actually
     an add, but instead a modification.*/
  if (entry && entry->copyfrom_url)
    fb->added = FALSE;

  if (fb->added)
    {
      source = svn_stream_empty(pool);
    }
  else
    {
      /* The current text-base is the starting point if replacing */
      SVN_ERR(svn_wc__get_pristine_contents(&source, eb->db, fb->local_abspath,
                                            fb->pool, fb->pool));
    }

  /* This is the file that will contain the pristine repository version. It
     is created in the admin temporary area. This file continues to exists
     until after the diff callback is run, at which point it is deleted. */
  SVN_ERR(svn_wc__db_temp_wcroot_tempdir(&temp_dir, eb->db, fb->local_abspath,
                                         pool, pool));
  SVN_ERR(svn_stream_open_unique(&temp_stream, &fb->temp_file_path,
                                 temp_dir, svn_io_file_del_on_pool_cleanup,
                                 fb->pool, fb->pool));

  svn_txdelta_apply(source, temp_stream,
                    NULL,
                    fb->temp_file_path /* error_info */,
                    fb->pool,
                    &fb->apply_handler, &fb->apply_baton);

  *handler = window_handler;
  *handler_baton = file_baton;
  return SVN_NO_ERROR;
}

/* An editor function.  When the file is closed we have a temporary
 * file containing a pristine version of the repository file. This can
 * be compared against the working copy.
 *
 * Ignore TEXT_CHECKSUM.
 */
static svn_error_t *
close_file(void *file_baton,
           const char *text_checksum,
           apr_pool_t *pool)
{
  struct file_baton *fb = file_baton;
  struct edit_baton *eb = fb->eb;
  svn_wc__db_status_t status;
  const char *repos_mimetype;
  const char *empty_file;
  svn_error_t *err;

  /* The BASE and repository properties of the file. */
  apr_hash_t *base_props;
  apr_hash_t *repos_props;

  /* The path to the wc file: either BASE or WORKING. */
  const char *localfile;
  /* The path to the temporary copy of the pristine repository version. */
  const char *temp_file_path;
  const char *temp_file_abspath;
  svn_boolean_t modified;
  /* The working copy properties at the base of the wc->repos
     comparison: either BASE or WORKING. */
  apr_hash_t *originalprops;

  err = svn_wc__db_read_info(&status, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, eb->db, fb->local_abspath, pool, pool);
  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    svn_error_clear(err);
  else
    SVN_ERR(err);

  if (status == svn_wc__db_status_added)
    SVN_ERR(svn_wc__db_scan_addition(&status, NULL, NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, eb->db,
                                     fb->local_abspath, pool, pool));
    
  SVN_ERR(get_empty_file(eb, &empty_file));

  /* Load the BASE and repository file properties. */
  if (fb->added)
    base_props = apr_hash_make(pool);
  else
    SVN_ERR(svn_wc__internal_propdiff(NULL, &base_props, eb->db,
                                      fb->local_abspath, pool, pool));

  repos_props = apply_propchanges(base_props, fb->propchanges);

  repos_mimetype = get_prop_mimetype(repos_props);

  /* The repository version of the file is in the temp file we applied
     the BASE->repos delta to.  If we haven't seen any changes, it's
     the same as BASE. */
  temp_file_path = fb->temp_file_path;
  if (!temp_file_path)
    SVN_ERR(svn_wc__text_base_path(&temp_file_path, eb->db, fb->local_abspath,
                                   FALSE, fb->pool));
  SVN_ERR(svn_dirent_get_absolute(&temp_file_abspath, temp_file_path,
                                  fb->pool));

  /* If the file isn't in the working copy (either because it was added
     in the BASE->repos diff or because we're diffing against WORKING
     and it was marked as schedule-deleted), we show either an addition
     or a deletion of the complete contents of the repository file,
     depending upon the direction of the diff. */
  if (fb->added || (!eb->use_text_base && status == svn_wc__db_status_deleted))
    {
      if (eb->reverse_order)
        return eb->callbacks->file_added(NULL, NULL, NULL, NULL, fb->path,
                                         empty_file,
                                         temp_file_path,
                                         0,
                                         eb->revnum,
                                         NULL,
                                         repos_mimetype,
                                         NULL, SVN_INVALID_REVNUM,
                                         fb->propchanges,
                                         apr_hash_make(pool),
                                         eb->callback_baton,
                                         pool);
      else
        return eb->callbacks->file_deleted(NULL, NULL, NULL, fb->path,
                                           temp_file_path,
                                           empty_file,
                                           repos_mimetype,
                                           NULL,
                                           repos_props,
                                           eb->callback_baton,
                                           pool);
    }

  /* If the file was locally added with history, and we want to show copies
   * as added, diff the file with the empty file. */
  if ((status == svn_wc__db_status_copied ||
       status == svn_wc__db_status_moved_here) && eb->show_copies_as_adds)
    return eb->callbacks->file_added(NULL, NULL, NULL, NULL, fb->path,
                                     empty_file,
                                     fb->local_abspath,
                                     0,
                                     eb->revnum,
                                     NULL,
                                     repos_mimetype,
                                     NULL, SVN_INVALID_REVNUM,
                                     fb->propchanges,
                                     apr_hash_make(pool),
                                     eb->callback_baton,
                                     pool);
    
  /* If we didn't see any content changes between the BASE and repository
     versions (i.e. we only saw property changes), then, if we're diffing
     against WORKING, we also need to check whether there are any local
     (BASE:WORKING) modifications. */
  modified = (fb->temp_file_path != NULL);
  if (!modified && !eb->use_text_base)
    SVN_ERR(svn_wc__internal_text_modified_p(&modified, eb->db,
                                             fb->local_abspath,
                                             FALSE, TRUE, pool));

  if (modified)
    {
      if (eb->use_text_base)
        SVN_ERR(svn_wc__text_base_path(&localfile, eb->db, fb->local_abspath,
                                       FALSE, fb->pool));
      else
        /* a detranslated version of the working file */
        SVN_ERR(svn_wc__internal_translated_file(
                 &localfile, fb->local_abspath, eb->db, fb->local_abspath,
                 SVN_WC_TRANSLATE_TO_NF | SVN_WC_TRANSLATE_USE_GLOBAL_TMP,
                 pool, pool));
    }
  else
    localfile = temp_file_path = NULL;

  if (eb->use_text_base)
    {
      originalprops = base_props;
    }
  else
    {
      SVN_ERR(svn_wc__load_props(NULL, &originalprops, NULL, eb->db,
                                 fb->local_abspath, pool, pool));

      /* We have the repository properties in repos_props, and the
         WORKING properties in originalprops.  Recalculate
         fb->propchanges as the change between WORKING and repos. */
      SVN_ERR(svn_prop_diffs(&fb->propchanges,
                             repos_props, originalprops, fb->pool));
    }

  if (localfile || fb->propchanges->nelts > 0)
    {
      const char *original_mimetype = get_prop_mimetype(originalprops);

      if (fb->propchanges->nelts > 0
          && ! eb->reverse_order)
        reverse_propchanges(originalprops, fb->propchanges, fb->pool);

      SVN_ERR(eb->callbacks->file_changed(NULL, NULL, NULL, NULL,
                                          fb->path,
                                          eb->reverse_order ? localfile
                                                            : temp_file_path,
                                           eb->reverse_order
                                                          ? temp_file_path
                                                          : localfile,
                                           eb->reverse_order
                                                          ? SVN_INVALID_REVNUM
                                                          : eb->revnum,
                                           eb->reverse_order
                                                          ? eb->revnum
                                                          : SVN_INVALID_REVNUM,
                                           eb->reverse_order
                                                          ? original_mimetype
                                                          : repos_mimetype,
                                           eb->reverse_order
                                                          ? repos_mimetype
                                                          : original_mimetype,
                                           fb->propchanges, originalprops,
                                           eb->callback_baton,
                                           pool));
    }

  return SVN_NO_ERROR;
}


/* An editor function. */
static svn_error_t *
change_file_prop(void *file_baton,
                 const char *name,
                 const svn_string_t *value,
                 apr_pool_t *pool)
{
  struct file_baton *fb = file_baton;
  svn_prop_t *propchange;

  propchange = apr_array_push(fb->propchanges);
  propchange->name = apr_pstrdup(fb->pool, name);
  propchange->value = value ? svn_string_dup(value, fb->pool) : NULL;

  return SVN_NO_ERROR;
}


/* An editor function. */
static svn_error_t *
change_dir_prop(void *dir_baton,
                const char *name,
                const svn_string_t *value,
                apr_pool_t *pool)
{
  struct dir_baton *db = dir_baton;
  svn_prop_t *propchange;

  propchange = apr_array_push(db->propchanges);
  propchange->name = apr_pstrdup(db->pool, name);
  propchange->value = value ? svn_string_dup(value, db->pool) : NULL;

  return SVN_NO_ERROR;
}


/* An editor function. */
static svn_error_t *
close_edit(void *edit_baton,
           apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;

  if (!eb->root_opened)
    {
      struct dir_baton *db;

      db = make_dir_baton(eb->anchor_path, NULL, eb, FALSE, eb->depth,
                          eb->pool);
      SVN_ERR(directory_elements_diff(db));
    }

  return SVN_NO_ERROR;
}

/* Public Interface */


/* Create a diff editor and baton. */
svn_error_t *
svn_wc_get_diff_editor6(const svn_delta_editor_t **editor,
                        void **edit_baton,
                        svn_wc_context_t *wc_ctx,
                        const char *anchor_path,
                        const char *target,
                        const svn_wc_diff_callbacks4_t *callbacks,
                        void *callback_baton,
                        svn_depth_t depth,
                        svn_boolean_t ignore_ancestry,
                        svn_boolean_t show_copies_as_adds,
                        svn_boolean_t use_text_base,
                        svn_boolean_t reverse_order,
                        const apr_array_header_t *changelists,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool)
{
  struct edit_baton *eb;
  void *inner_baton;
  svn_delta_editor_t *tree_editor;
  const svn_delta_editor_t *inner_editor;

  SVN_ERR(make_edit_baton(&eb,
                          wc_ctx->db,
                          anchor_path, target,
                          callbacks, callback_baton,
                          depth, ignore_ancestry, show_copies_as_adds,
                          use_text_base, reverse_order, changelists,
                          cancel_func, cancel_baton,
                          result_pool));

  tree_editor = svn_delta_default_editor(eb->pool);

  tree_editor->set_target_revision = set_target_revision;
  tree_editor->open_root = open_root;
  tree_editor->delete_entry = delete_entry;
  tree_editor->add_directory = add_directory;
  tree_editor->open_directory = open_directory;
  tree_editor->close_directory = close_directory;
  tree_editor->add_file = add_file;
  tree_editor->open_file = open_file;
  tree_editor->apply_textdelta = apply_textdelta;
  tree_editor->change_file_prop = change_file_prop;
  tree_editor->change_dir_prop = change_dir_prop;
  tree_editor->close_file = close_file;
  tree_editor->close_edit = close_edit;

  inner_editor = tree_editor;
  inner_baton = eb;

  if (depth == svn_depth_unknown)
    SVN_ERR(svn_wc__ambient_depth_filter_editor(&inner_editor,
                                                &inner_baton,
                                                inner_editor,
                                                inner_baton,
                                                anchor_path,
                                                target,
                                                wc_ctx->db,
                                                result_pool));

  return svn_delta_get_cancellation_editor(cancel_func,
                                           cancel_baton,
                                           inner_editor,
                                           inner_baton,
                                           editor,
                                           edit_baton,
                                           result_pool);
}


/* Compare working copy against the text-base. */
svn_error_t *
svn_wc_diff6(svn_wc_context_t *wc_ctx,
             const char *target_path,
             const svn_wc_diff_callbacks4_t *callbacks,
             void *callback_baton,
             svn_depth_t depth,
             svn_boolean_t ignore_ancestry,
             svn_boolean_t show_copies_as_adds,
             const apr_array_header_t *changelists,
             svn_cancel_func_t cancel_func,
             void *cancel_baton,
             apr_pool_t *pool)
{
  struct edit_baton *eb;
  struct dir_baton *db;
  const char *target;
  const char *target_abspath;
  const char *anchor_path;
  svn_wc__db_kind_t kind;

  SVN_ERR(svn_dirent_get_absolute(&target_abspath, target_path, pool));
  SVN_ERR(svn_wc__db_read_kind(&kind, wc_ctx->db, target_abspath, FALSE,
                               pool));

  if (kind == svn_wc__db_kind_dir)
    {
      anchor_path = target_path;
      target = "";
    }
  else
    svn_dirent_split(target_path, &anchor_path, &target, pool);

  SVN_ERR(make_edit_baton(&eb,
                          wc_ctx->db,
                          anchor_path,
                          target, 
                          callbacks, callback_baton,
                          depth, ignore_ancestry, show_copies_as_adds,
                          FALSE, FALSE, changelists,
                          cancel_func, cancel_baton,
                          pool));

  db = make_dir_baton(anchor_path, NULL, eb, FALSE, depth, eb->pool);

  SVN_ERR(directory_elements_diff(db));

  return SVN_NO_ERROR;
}
