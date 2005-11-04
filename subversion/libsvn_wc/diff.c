/*
 * diff.c -- The diff editor for comparing the working copy against the
 *           repository.
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
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
 * ### TODO: It might be better if the temporary files were not created in
 * the admin's temp area, but in a more general area (/tmp, $TMPDIR) as
 * then diff could be run on a read-only working copy.
 *
 * ### TODO: Replacements where the node kind changes needs support. It
 * mostly works when the change is in the repository, but not when it is
 * in the working copy.
 *
 * ### TODO: Do we need to support copyfrom?
 *
 */

#include <assert.h>

#include <apr_hash.h>

#include "svn_pools.h"
#include "svn_path.h"

#include "wc.h"
#include "props.h"
#include "adm_files.h"


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
reverse_propchanges (apr_hash_t *baseprops,
                     apr_array_header_t *propchanges,
                     apr_pool_t *pool)
{
  int i;

  /* ### todo: research lifetimes for property values below */

  for (i = 0; i < propchanges->nelts; i++)
    {
      svn_prop_t *propchange
        = &APR_ARRAY_IDX (propchanges, i, svn_prop_t);
      
      const svn_string_t *original_value =
        apr_hash_get (baseprops, propchange->name, APR_HASH_KEY_STRING);
     
      if ((original_value == NULL) && (propchange->value != NULL)) 
        {
          /* found an addition.  make it look like a deletion. */
          apr_hash_set (baseprops, propchange->name, APR_HASH_KEY_STRING,
                        svn_string_dup (propchange->value, pool));
          propchange->value = NULL;
        }

      else if ((original_value != NULL) && (propchange->value == NULL)) 
        {
          /* found a deletion.  make it look like an addition. */
          propchange->value = svn_string_dup (original_value, pool);
          apr_hash_set (baseprops, propchange->name, APR_HASH_KEY_STRING,
                        NULL);
        }

      else if ((original_value != NULL) && (propchange->value != NULL)) 
        {
          /* found a change.  just swap the values.  */
          const svn_string_t *str = svn_string_dup (propchange->value, pool);
          propchange->value = svn_string_dup (original_value, pool);
          apr_hash_set (baseprops, propchange->name, APR_HASH_KEY_STRING, str);
        }
    }
}

/*-------------------------------------------------------------------------*/

/* Overall crawler editor baton.
 */
struct edit_baton {
  /* ANCHOR/TARGET represent the base of the hierarchy to be compared. */
  svn_wc_adm_access_t *anchor;
  const char *anchor_path;
  const char *target;

  /* Target revision */
  svn_revnum_t revnum;

  /* Was the root opened? */
  svn_boolean_t root_opened;

  /* The callbacks and callback argument that implement the file comparison
     functions */
  const svn_wc_diff_callbacks2_t *callbacks;
  void *callback_baton;

  /* Flags whether to diff recursively or not. If set the diff is
     recursive. */
  svn_boolean_t recurse;

  /* Should this diff ignore node ancestry. */
  svn_boolean_t ignore_ancestry;

  /* Possibly diff repos against text-bases instead of working files. */
  svn_boolean_t use_text_base;

  /* Possibly show the diffs backwards. */
  svn_boolean_t reverse_order;

  /* Empty file used to diff adds / deletes */
  const char *empty_file;

  apr_pool_t *pool;
};

/* Directory level baton.
 */
struct dir_baton {
  /* Gets set if the directory is added rather than replaced/unchanged. */
  svn_boolean_t added;

  /* The "correct" path of the directory, but it may not exist in the
     working copy. */
  const char *path;

 /* Identifies those directory elements that get compared while running the
    crawler. These elements should not be compared again when recursively
    looking for local only diffs. */
  apr_hash_t *compared;

  /* The baton for the parent directory, or null if this is the root of the
     hierarchy to be compared. */
  struct dir_baton *dir_baton;

  /* The original property hash, and the list of incoming propchanges. */
  apr_hash_t *baseprops;
  apr_array_header_t *propchanges;
  svn_boolean_t fetched_baseprops; /* did we get the working props yet? */

  /* The overall crawler editor baton. */
  struct edit_baton *edit_baton;

  apr_pool_t *pool;
};

/* File level baton.
 */
struct file_baton {
  /* Gets set if the file is added rather than replaced. */
  svn_boolean_t added;

  /* PATH is the "correct" path of the file, but it may not exist in the
     working copy.  WC_PATH is a path we can use to make temporary files
     or open empty files; it doesn't necessarily exist either, but the
     directory part of it does. */
  const char *path;
  const char *wc_path;

 /* When constructing the requested repository version of the file,
    ORIGINAL_FILE is version of the file in the working copy. TEMP_FILE is
    the pristine repository file obtained by applying the repository diffs
    to ORIGINAL_FILE. */
  apr_file_t *original_file;
  apr_file_t *temp_file;

  /* The original property hash, and the list of incoming propchanges. */
  apr_hash_t *baseprops;
  apr_array_header_t *propchanges;
  svn_boolean_t fetched_baseprops; /* did we get the working props yet? */

  /* APPLY_HANDLER/APPLY_BATON represent the delta applcation baton. */
  svn_txdelta_window_handler_t apply_handler;
  void *apply_baton;

  /* Is this file scheduled to be deleted? */
  svn_boolean_t schedule_delete;

  /* The overall crawler editor baton. */
  struct edit_baton *edit_baton;

  apr_pool_t *pool;
};

/* Used to wrap svn_wc_diff_callbacks_t. */
struct callbacks_wrapper_baton {
  const svn_wc_diff_callbacks_t *callbacks;
  void *baton;
};

/* Create a new edit baton. TARGET/ANCHOR are working copy paths that
 * describe the root of the comparison. CALLBACKS/CALLBACK_BATON
 * define the callbacks to compare files. RECURSE defines whether to
 * descend into subdirectories.  IGNORE_ANCESTRY defines whether to
 * utilize node ancestry when calculating diffs.  USE_TEXT_BASE
 * defines whether to compare against working files or text-bases.
 * REVERSE_ORDER defines which direction to perform the diff.
 */
static struct edit_baton *
make_editor_baton (svn_wc_adm_access_t *anchor,
                   const char *target,
                   const svn_wc_diff_callbacks2_t *callbacks,
                   void *callback_baton,
                   svn_boolean_t recurse,
                   svn_boolean_t ignore_ancestry,
                   svn_boolean_t use_text_base,
                   svn_boolean_t reverse_order,
                   apr_pool_t *pool)
{
  struct edit_baton *eb = apr_pcalloc (pool, sizeof (*eb));

  eb->anchor = anchor;
  eb->anchor_path = svn_wc_adm_access_path (anchor);
  eb->target = apr_pstrdup (pool, target);
  eb->callbacks = callbacks;
  eb->callback_baton = callback_baton;
  eb->recurse = recurse;
  eb->ignore_ancestry = ignore_ancestry;
  eb->use_text_base = use_text_base;
  eb->reverse_order = reverse_order;
  eb->pool = pool;

  return eb;
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
make_dir_baton (const char *path,
                struct dir_baton *parent_baton,
                struct edit_baton *edit_baton,
                svn_boolean_t added,
                apr_pool_t *pool)
{
  struct dir_baton *dir_baton = apr_pcalloc (pool, sizeof (*dir_baton));

  dir_baton->dir_baton = parent_baton;
  dir_baton->edit_baton = edit_baton;
  dir_baton->added = added;
  dir_baton->pool = pool;
  dir_baton->baseprops = apr_hash_make (dir_baton->pool);
  dir_baton->propchanges = apr_array_make (pool, 1, sizeof (svn_prop_t));
  dir_baton->compared = apr_hash_make (dir_baton->pool);
  dir_baton->path = path;

  return dir_baton;
}

/* Create a new file baton.  PATH is the file path, including
 * anchor_path.  ADDED is set if this file is being added rather than
 * replaced.  PARENT_BATON is the baton of the parent directory.  The
 * directory and its parent may or may not exist in the working copy.
 */
static struct file_baton *
make_file_baton (const char *path,
                 svn_boolean_t added,
                 struct dir_baton *parent_baton,
                 apr_pool_t *pool)
{
  struct file_baton *file_baton = apr_pcalloc (pool, sizeof (*file_baton));
  struct edit_baton *edit_baton = parent_baton->edit_baton;

  file_baton->edit_baton = edit_baton;
  file_baton->added = added;
  file_baton->pool = pool;
  file_baton->baseprops = apr_hash_make (file_baton->pool);
  file_baton->propchanges  = apr_array_make (pool, 1, sizeof (svn_prop_t));
  file_baton->path = path;
  file_baton->schedule_delete = FALSE;

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
        wc_dir_baton = wc_dir_baton->dir_baton;

      file_baton->wc_path = svn_path_join (wc_dir_baton->path, "unimportant",
                                           file_baton->pool);
    }
  else
    {
      file_baton->wc_path = file_baton->path;
    }

  return file_baton;
}

/* Get the empty file associated with the edit baton. This is cached so
 * that it can be reused, all empty files are the same.
 */
static svn_error_t *
get_empty_file (struct edit_baton *b,
                const char **empty_file)
{
  /* Create the file if it does not exist */
  if (!b->empty_file)
    {
      const char *temp_dir;

      SVN_ERR (svn_io_temp_dir (&temp_dir, b->pool));
      SVN_ERR (svn_io_open_unique_file2
               (NULL, &(b->empty_file),
                svn_path_join (temp_dir, "tmp", b->pool),
                "", svn_io_file_del_on_pool_cleanup,
                b->pool));
    }

  *empty_file = b->empty_file;

  return SVN_NO_ERROR;
}

/* Helper function:  load a file_baton's base_props. */
static svn_error_t *
load_base_props (struct file_baton *b)
{
  /* the 'base' props to compare against, in this case, are
     actually the working props.  that's what we do with texts,
     anyway, in the 'svn diff -rN foo' case.  */
  
  /* This path might not exist in the working copy, in which case
     the baseprops remains an empty hash. */
  SVN_ERR (svn_wc_prop_list (&(b->baseprops), b->path,
                             b->edit_baton->anchor, b->pool));
  b->fetched_baseprops = TRUE;

  return SVN_NO_ERROR;
}


/* Helper function for retrieving svn:mime-type properties, if
   present, on file PATH.  File baton B is optional:  if present,
   assume it refers to PATH, and use its caching abilities.

   If PRISTINE_MIMETYPE is non-NULL, then set *PRISTINE_MIMETYPE to
   the value of svn:mime-type if available.  Else set to NULL.  Search
   for the property in B->PROPCHANGES first (if B is non-NULL), then
   search in the pristine properties of PATH, using ADM_ACCESS.

   If WORKING_MIMETYPE is non-NULL, then set *WORKING_MIMETYPE to
   the value of svn:mime-type if available.  Else set to NULL.  Search
   in the working properties of PATH, using ADM_ACCESS.

   Return the property value allocated in POOL, or if B is non-NULL,
   whatever pool B is allocated in.
*/
static svn_error_t *
get_local_mimetypes (const char **pristine_mimetype,
                     const char **working_mimetype,
                     struct file_baton *b,
                     svn_wc_adm_access_t *adm_access,
                     const char *path,
                     apr_pool_t *pool)
{
  const svn_string_t *working_val;

  if (working_mimetype)
    {
      if (b)
        {
          /* If we have the file_baton, try to use its working props. */
          if (! b->fetched_baseprops)
            SVN_ERR (load_base_props (b));

          working_val = apr_hash_get (b->baseprops, SVN_PROP_MIME_TYPE,
                                      strlen(SVN_PROP_MIME_TYPE));
        }
      else
        {
          /* else use the public API */
          SVN_ERR (svn_wc_prop_get (&working_val, SVN_PROP_MIME_TYPE,
                                    path, adm_access, pool));
        }

      *working_mimetype = working_val ? working_val->data : NULL;
    }

  if (pristine_mimetype)
    {
      const svn_string_t *pristine_val = NULL;

      if (b && b->propchanges)
        {
          /* first search any new propchanges from the repository */
          int i;
          svn_prop_t *propchange;
                    
          for (i = 0; i < b->propchanges->nelts; i++)
            {
              propchange = &APR_ARRAY_IDX(b->propchanges, i, svn_prop_t);
              if (strcmp (propchange->name, SVN_PROP_MIME_TYPE) == 0)
                {
                  pristine_val = propchange->value;
                  break;
                }
            }          
        }
      if (! pristine_val)
        {
          /* otherwise, try looking in the pristine props in the wc */
          const char *props_base_path;
          apr_hash_t *baseprops = apr_hash_make (pool);

          SVN_ERR (svn_wc__prop_base_path (&props_base_path, path, adm_access,
                                           FALSE, pool));
          SVN_ERR (svn_wc__load_prop_file (props_base_path, baseprops, pool));
          pristine_val = apr_hash_get (baseprops, SVN_PROP_MIME_TYPE,
                                       strlen(SVN_PROP_MIME_TYPE));
        }

      *pristine_mimetype = pristine_val ? pristine_val->data : NULL;
    }

  return SVN_NO_ERROR;
}


/* Called by directory_elements_diff when a file is to be compared. At this
 * stage we are dealing with a file that does exist in the working copy.
 *
 * DIR_BATON is the parent directory baton, PATH is the path to the file to
 * be compared. ENTRY is the working copy entry for the file. ADDED forces
 * the file to be treated as added.
 *
 * Do all allocation in POOL.
 *
 * ### TODO: Need to work on replace if the new filename used to be a
 * directory.
 */
static svn_error_t *
file_diff (struct dir_baton *dir_baton,
           const char *path,
           const svn_wc_entry_t *entry,
           svn_boolean_t added,
           apr_pool_t *pool)
{
  struct edit_baton *eb = dir_baton->edit_baton;
  const char *pristine_copy, *empty_file;
  svn_boolean_t modified;
  enum svn_wc_schedule_t schedule = entry->schedule;
  svn_boolean_t copied = entry->copied;
  svn_wc_adm_access_t *adm_access;
  const char *pristine_mimetype, *working_mimetype;
  const char *translated = NULL;
  apr_array_header_t *propchanges = NULL;
  apr_hash_t *baseprops = NULL;

  assert (! eb->use_text_base);

  SVN_ERR (svn_wc_adm_retrieve (&adm_access, dir_baton->edit_baton->anchor,
                                dir_baton->path, pool));

  /* If the directory is being added, then this file will need to be
     added. */
  if (added)
    schedule = svn_wc_schedule_add;

  /* If the item is schedule-add *with history*, then we don't want to
     see a comparison to the empty file;  we want the usual working
     vs. text-base comparision. */
  if (copied)
    schedule = svn_wc_schedule_normal;

  /* If this was scheduled replace and we are ignoring ancestry,
     report it as a normal file modification. */
  if (eb->ignore_ancestry && (schedule == svn_wc_schedule_replace))
    schedule = svn_wc_schedule_normal;

  /* Prep these two paths early. */
  pristine_copy = svn_wc__text_base_path (path, FALSE, pool);
  SVN_ERR (get_empty_file (eb, &empty_file));

  /* Get property diffs if this is not schedule delete. */
  if (schedule != svn_wc_schedule_delete)
    {
      SVN_ERR (svn_wc_props_modified_p (&modified, path, adm_access, pool));
      if (modified)
        SVN_ERR (svn_wc_get_prop_diffs (&propchanges, &baseprops, path,
                                        adm_access, pool));
      else
        propchanges = apr_array_make (pool, 1, sizeof (svn_prop_t));
    }
  else
    {
      /* Get pristine properties. */
      SVN_ERR (svn_wc_get_prop_diffs (NULL, &baseprops, path,
                                      adm_access, pool));
    }

  switch (schedule)
    {
      /* Replace is treated like a delete plus an add: two
         comparisons are generated, first one for the delete and
         then one for the add. */
    case svn_wc_schedule_replace:
    case svn_wc_schedule_delete:
      /* Delete compares text-base against empty file, modifications to the
         working-copy version of the deleted file are not wanted. */

      /* Get svn:mime-type from pristine props of PATH. */
      SVN_ERR (get_local_mimetypes (&pristine_mimetype, NULL, NULL,
                                    adm_access, path, pool));

      SVN_ERR (dir_baton->edit_baton->callbacks->file_deleted
               (NULL, NULL, path, 
                pristine_copy, 
                empty_file,
                pristine_mimetype,
                NULL,
                baseprops,
                dir_baton->edit_baton->callback_baton));

      /* Replace will fallthrough! */
      if (schedule == svn_wc_schedule_delete)
        break;

    case svn_wc_schedule_add:
      /* Get svn:mime-type from working props of PATH. */
      SVN_ERR (get_local_mimetypes (NULL, &working_mimetype, NULL,
                                    adm_access, path, pool));

      SVN_ERR (svn_wc_translated_file (&translated, path, adm_access,
                                       TRUE, pool));

      SVN_ERR (dir_baton->edit_baton->callbacks->file_added
               (NULL, NULL, NULL, path,
                empty_file,
                translated,
                0, entry->revision,
                NULL,
                working_mimetype,
                propchanges, baseprops,
                dir_baton->edit_baton->callback_baton));

      break;

    default:
      SVN_ERR (svn_wc_text_modified_p (&modified, path, FALSE, 
                                       adm_access, pool));
      if (modified)
        {
          /* Note that this might be the _second_ time we translate
             the file, as svn_wc_text_modified_p() might have used a
             tmp translated copy too.  But what the heck, diff is
             already expensive, translating twice for the sake of code
             modularity is liveable. */
          SVN_ERR (svn_wc_translated_file (&translated, path, adm_access,
                                           TRUE, pool));
        }

      if (modified || propchanges->nelts > 0)
        {
          svn_error_t *err, *err2 = SVN_NO_ERROR;

          /* Get svn:mime-type for both pristine and working file. */
          SVN_ERR (get_local_mimetypes (&pristine_mimetype, &working_mimetype,
                                        NULL, adm_access, path, pool));

          err = dir_baton->edit_baton->callbacks->file_changed
            (NULL, NULL, NULL,
             path,
             modified ? pristine_copy : NULL,
             translated,
             entry->revision,
             SVN_INVALID_REVNUM,
             pristine_mimetype,
             working_mimetype,
             propchanges, baseprops,
             dir_baton->edit_baton->callback_baton);
          
          if (translated && translated != path)
            err2 = svn_io_remove_file (translated, pool);

          if (err && err2)
            {
              svn_error_compose (err, err2);
              return err;
            }
          if (err)
            return err;
          if (err2)
            return err2;
        }
    }
  return SVN_NO_ERROR;
}

/* Called when the directory is closed to compare any elements that have
 * not yet been compared.  This identifies local, working copy only
 * changes.  At this stage we are dealing with files/directories that do
 * exist in the working copy.
 *
 * DIR_BATON is the baton for the directory. ADDED forces the directory
 * to be treated as added.
 */
static svn_error_t *
directory_elements_diff (struct dir_baton *dir_baton,
                         svn_boolean_t added)
{
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  svn_boolean_t in_anchor_not_target;
  apr_pool_t *subpool;
  svn_wc_adm_access_t *adm_access;

  /* This directory should have been unchanged or replaced, not added,
     since an added directory can only contain added files and these will
     already have been compared. (Note: the ADDED flag is used to simulate
     added directories, these are *not* scheduled to be added in the
     working copy.) */
  assert (!dir_baton->added);

  /* Everything we do below is useless if we are comparing to BASE. */
  if (dir_baton->edit_baton->use_text_base)
    return SVN_NO_ERROR;

  /* Determine if this is the anchor directory if the anchor is different
     to the target. When the target is a file, the anchor is the parent
     directory and if this is that directory the non-target entries must be
     skipped. */
  in_anchor_not_target =
    (*dir_baton->edit_baton->target
     && (! svn_path_compare_paths
         (dir_baton->path,
          svn_wc_adm_access_path (dir_baton->edit_baton->anchor))));

  SVN_ERR (svn_wc_adm_retrieve (&adm_access, dir_baton->edit_baton->anchor,
                                dir_baton->path, dir_baton->pool));

  /* Check for property mods on this directory. */
  if (!in_anchor_not_target)
    {
      svn_boolean_t modified;

      SVN_ERR (svn_wc_props_modified_p (&modified, dir_baton->path, adm_access,
                                        dir_baton->pool));
      if (modified)
        {
          apr_array_header_t *propchanges;
          apr_hash_t *baseprops;

          SVN_ERR (svn_wc_get_prop_diffs (&propchanges, &baseprops,
                                          dir_baton->path, adm_access,
                                          dir_baton->pool));
              
          SVN_ERR (dir_baton->edit_baton->callbacks->dir_props_changed
                   (NULL, NULL,
                    dir_baton->path,
                    propchanges, baseprops,
                    dir_baton->edit_baton->callback_baton));
        }
    }

  SVN_ERR (svn_wc_entries_read (&entries, adm_access, FALSE, dir_baton->pool));

  subpool = svn_pool_create (dir_baton->pool);

  for (hi = apr_hash_first (dir_baton->pool, entries); hi;
       hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;
      const svn_wc_entry_t *entry;
      struct dir_baton *subdir_baton;
      const char *name, *path;

      svn_pool_clear (subpool);

      apr_hash_this (hi, &key, NULL, &val);
      name = key;
      entry = val;
      
      /* Skip entry for the directory itself. */
      if (strcmp (key, SVN_WC_ENTRY_THIS_DIR) == 0)
        continue;

      /* In the anchor directory, if the anchor is not the target then all
         entries other than the target should not be diff'd. Running diff
         on one file in a directory should not diff other files in that
         directory. */
      if (in_anchor_not_target
          && strcmp (dir_baton->edit_baton->target, name))
        continue;

      path = svn_path_join (dir_baton->path, name, subpool);

      /* Skip entry if it is in the list of entries already diff'd. */
      if (apr_hash_get (dir_baton->compared, path, APR_HASH_KEY_STRING))
        continue;

      switch (entry->kind)
        {
        case svn_node_file:
          SVN_ERR (file_diff (dir_baton, path, entry, added, subpool));
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
          if (in_anchor_not_target || dir_baton->edit_baton->recurse)
            {
              subdir_baton = make_dir_baton (path, dir_baton,
                                             dir_baton->edit_baton,
                                             FALSE,
                                             subpool);

              SVN_ERR (directory_elements_diff (subdir_baton, added));
            }
          break;

        default:
          break;
        }
    }

  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
set_target_revision (void *edit_baton, 
                     svn_revnum_t target_revision,
                     apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;
  eb->revnum = target_revision;

  return SVN_NO_ERROR;
}

/* An editor function. The root of the comparison hierarchy */
static svn_error_t *
open_root (void *edit_baton,
           svn_revnum_t base_revision,
           apr_pool_t *dir_pool,
           void **root_baton)
{
  struct edit_baton *eb = edit_baton;
  struct dir_baton *b;

  eb->root_opened = TRUE;
  b = make_dir_baton (eb->anchor_path, NULL, eb, FALSE, dir_pool);
  *root_baton = b;

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
delete_entry (const char *path,
              svn_revnum_t base_revision,
              void *parent_baton,
              apr_pool_t *pool)
{
  struct dir_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  const svn_wc_entry_t *entry;
  struct dir_baton *b;
  const char *empty_file;
  const char *full_path = svn_path_join (pb->edit_baton->anchor_path, path,
                                         pb->pool);
  svn_wc_adm_access_t *adm_access;
  const char *working_mimetype, *pristine_mimetype;
  apr_hash_t *baseprops;

  SVN_ERR (svn_wc_adm_probe_retrieve (&adm_access, pb->edit_baton->anchor,
                                      full_path, pool));
  SVN_ERR (svn_wc_entry (&entry, full_path, adm_access, FALSE, pool));
  SVN_ERR (get_empty_file (pb->edit_baton, &empty_file));
  switch (entry->kind)
    {
    case svn_node_file:
      /* A delete is required to change working-copy into requested
         revision, so diff should show this as an add. Thus compare
         the empty file against the current working copy.  If
         'reverse_order' is set, then show a deletion. */

      SVN_ERR (get_local_mimetypes (&pristine_mimetype, &working_mimetype,
                                    NULL, adm_access, full_path, pool));

      if (eb->reverse_order)
        {
          /* Whenever showing a deletion, we show the text-base vanishing. */
          const char *textbase = svn_wc__text_base_path (full_path,
                                                         FALSE, pool);

          SVN_ERR (svn_wc_get_prop_diffs (NULL, &baseprops, full_path,
                                          adm_access, pool));
          SVN_ERR (pb->edit_baton->callbacks->file_deleted
                   (NULL, NULL, full_path,
                    textbase,
                    empty_file,
                    pristine_mimetype,
                    NULL,
                    baseprops,
                    pb->edit_baton->callback_baton));
        }
      else
        {
          /* Or normally, show the working file being added. */
          /* ### Show the properties as well. */
          SVN_ERR (pb->edit_baton->callbacks->file_added
                   (NULL, NULL, NULL, full_path,
                    empty_file,
                    full_path,
                    0, entry->revision,
                    NULL,
                    working_mimetype,
                    apr_array_make (pool, 1, sizeof (svn_prop_t)), NULL,
                    pb->edit_baton->callback_baton));
        }

      apr_hash_set (pb->compared, full_path, APR_HASH_KEY_STRING, "");
      break;

    case svn_node_dir:
      b = make_dir_baton (full_path, pb, pb->edit_baton, FALSE, pool);
      /* A delete is required to change working-copy into requested
         revision, so diff should show this as and add. Thus force the
         directory diff to treat this as added. */
      SVN_ERR (directory_elements_diff (b, TRUE));
      break;

    default:
      break;
    }

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
add_directory (const char *path,
               void *parent_baton,
               const char *copyfrom_path,
               svn_revnum_t copyfrom_revision,
               apr_pool_t *dir_pool,
               void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct dir_baton *b;
  const char *full_path;

  /* ### TODO: support copyfrom? */

  full_path = svn_path_join (pb->edit_baton->anchor_path, path, dir_pool);
  b = make_dir_baton (full_path, pb, pb->edit_baton, TRUE, dir_pool);
  *child_baton = b;

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
open_directory (const char *path,
                void *parent_baton,
                svn_revnum_t base_revision,
                apr_pool_t *dir_pool,
                void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct dir_baton *b;
  const char *full_path;

  /* Allocate path from the parent pool since the memory is used in the
     parent's compared hash */
  full_path = svn_path_join (pb->edit_baton->anchor_path, path, pb->pool);
  b = make_dir_baton (full_path, pb, pb->edit_baton, FALSE, dir_pool);
  *child_baton = b;

  return SVN_NO_ERROR;
}

/* An editor function.  When a directory is closed, all the directory
 * elements that have been added or replaced will already have been
 * diff'd. However there may be other elements in the working copy
 * that have not yet been considered.  */
static svn_error_t *
close_directory (void *dir_baton,
                 apr_pool_t *pool)
{
  struct dir_baton *b = dir_baton;

  /* Skip added directories, they can only contain added elements all of
     which have already been diff'd. */
  if (!b->added)
    SVN_ERR (directory_elements_diff (dir_baton, FALSE));

  /* Mark this directory as compared in the parent directory's baton. */
  if (b->dir_baton)
    {
      apr_hash_set (b->dir_baton->compared, b->path, APR_HASH_KEY_STRING,
                    "");
    }

  if (b->propchanges->nelts > 0)
    {
      if (! b->edit_baton->reverse_order)
        reverse_propchanges (b->baseprops, b->propchanges, b->pool);

      SVN_ERR (b->edit_baton->callbacks->dir_props_changed
               (NULL, NULL,
                b->path,
                b->propchanges,
                b->baseprops,
                b->edit_baton->callback_baton));
    }

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
add_file (const char *path,
          void *parent_baton,
          const char *copyfrom_path,
          svn_revnum_t copyfrom_revision,
          apr_pool_t *file_pool,
          void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *b;
  const char *full_path;

  /* ### TODO: support copyfrom? */

  full_path = svn_path_join (pb->edit_baton->anchor_path, path, file_pool);
  b = make_file_baton (full_path, TRUE, pb, file_pool);
  *file_baton = b;

  /* Add this filename to the parent directory's list of elements that
     have been compared. */
  apr_hash_set (pb->compared, apr_pstrdup(pb->pool, full_path),
                APR_HASH_KEY_STRING, "");

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
open_file (const char *path,
           void *parent_baton,
           svn_revnum_t base_revision,
           apr_pool_t *file_pool,
           void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *b;
  const char *full_path;

  full_path = svn_path_join (pb->edit_baton->anchor_path, path, file_pool);
  b = make_file_baton (full_path, FALSE, pb, file_pool);
  *file_baton = b;

  /* Add this filename to the parent directory's list of elements that
     have been compared. */
  apr_hash_set (pb->compared, apr_pstrdup(pb->pool, full_path),
                APR_HASH_KEY_STRING, "");

  return SVN_NO_ERROR;
}

/* This is an apr cleanup handler. It is called whenever the associated
 * pool is cleared or destroyed. It is installed when the temporary file is
 * created, and removes the file when the file pool is deleted, whether in
 * the normal course of events, or if an error occurs.
 *
 * ARG is the file baton for the working copy file associated with the
 * temporary file.
 */
static apr_status_t
temp_file_cleanup_handler (void *arg)
{
  struct file_baton *b = arg;
  svn_error_t *err;
  apr_status_t status;

  /* The path to the temporary copy of the pristine repository version. */
  const char *temp_file_path
    = svn_wc__text_base_path (b->wc_path, TRUE, b->pool);

  err = svn_io_remove_file (temp_file_path, b->pool);
  if (err)
    {
      status = err->apr_err;
      svn_error_clear (err);
    }
  else
    status = APR_SUCCESS;

  return status;
}

/* This removes the temp_file_cleanup_handler in the child process before
 * exec'ing diff.
 */
static apr_status_t
temp_file_cleanup_handler_remover (void *arg)
{
  struct file_baton *b = arg;
  apr_pool_cleanup_kill (b->pool, b, temp_file_cleanup_handler);
  return APR_SUCCESS;
}

/* An editor function.  Do the work of applying the text delta. */
static svn_error_t *
window_handler (svn_txdelta_window_t *window,
                void *window_baton)
{
  struct file_baton *b = window_baton;

  SVN_ERR (b->apply_handler (window, b->apply_baton));

  if (!window)
    {
      SVN_ERR (svn_wc__close_text_base (b->temp_file, b->wc_path, 0, b->pool));

      if (b->added)
        SVN_ERR (svn_io_file_close (b->original_file, b->pool));
      else
        {
          SVN_ERR (svn_wc__close_text_base (b->original_file, b->wc_path, 0,
                                            b->pool));
        }
    }

  return SVN_NO_ERROR;
}

/* An editor function. */
static svn_error_t *
apply_textdelta (void *file_baton,
                 const char *base_checksum,
                 apr_pool_t *pool,
                 svn_txdelta_window_handler_t *handler,
                 void **handler_baton)
{
  struct file_baton *b = file_baton;
  struct edit_baton *eb = b->edit_baton;
  const svn_wc_entry_t *entry;

  SVN_ERR (svn_wc_entry (&entry, b->wc_path, eb->anchor, FALSE, b->pool));
  
  /* Check to see if there is a schedule-add with history entry in
     the current working copy.  If so, then this is not actually
     an add, but instead a modification.*/
  if (entry && entry->copyfrom_url)
    b->added = FALSE;

  /* Check to see if this entry is scheduled to be deleted, if so,
     then we need to remember so that we don't attempt to open the
     non-existent file when doing the final diff. */
  if (entry && entry->schedule == svn_wc_schedule_delete)
    b->schedule_delete = TRUE;

  if (b->added)
    {
      /* An empty file is the starting point if the file is being added */
      const char *empty_file;

      SVN_ERR (get_empty_file (eb, &empty_file));
      SVN_ERR (svn_io_file_open (&b->original_file, empty_file,
                                 APR_READ, APR_OS_DEFAULT, pool));
    }
  else
    {
      /* The current text-base is the starting point if replacing */
      SVN_ERR (svn_wc__open_text_base (&b->original_file, b->wc_path,
                                       APR_READ, b->pool));
    }

  /* This is the file that will contain the pristine repository version. It
     is created in the admin temporary area. This file continues to exists
     until after the diff callback is run, at which point it is deleted. */ 
  SVN_ERR (svn_wc__open_text_base (&b->temp_file, b->wc_path,
                                   (APR_WRITE | APR_TRUNCATE | APR_CREATE),
                                   b->pool));

  /* Need to ensure that the above file gets removed if the program aborts
     with some error. So register a pool cleanup handler to delete the
     file. This handler is removed just before deleting the file. */
  apr_pool_cleanup_register (b->pool, file_baton, temp_file_cleanup_handler,
                             temp_file_cleanup_handler_remover);

  {
    const char *tmp_path;

    apr_file_name_get (&tmp_path, b->temp_file);
    svn_txdelta_apply (svn_stream_from_aprfile (b->original_file, b->pool),
                       svn_stream_from_aprfile (b->temp_file, b->pool),
                       NULL,
                       tmp_path,
                       b->pool,
                       &b->apply_handler, &b->apply_baton);
  }

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
close_file (void *file_baton,
            const char *text_checksum,
            apr_pool_t *pool)
{
  struct file_baton *b = file_baton;
  struct edit_baton *eb = b->edit_baton;
  svn_wc_adm_access_t *adm_access;
  const svn_wc_entry_t *entry;
  const char *pristine_mimetype, *working_mimetype;
  const char *empty_file;

  /* The path to the temporary copy of the pristine repository version. */
  const char *temp_file_path;

  SVN_ERR (svn_wc_adm_probe_retrieve (&adm_access, b->edit_baton->anchor,
                                      b->wc_path, b->pool));
  SVN_ERR (svn_wc_entry (&entry, b->wc_path, adm_access, FALSE, b->pool));

  SVN_ERR (get_empty_file (b->edit_baton, &empty_file));
  /* We want to figure out if the file from the repository has an
     svn:mime-type.  So first look for svn:mime-type in
     b->propchanges... if not there, look for it in the *pristine*
     properties of path. */
  SVN_ERR (get_local_mimetypes (&pristine_mimetype, &working_mimetype,
                                b, adm_access, b->wc_path, pool));

  if (b->added)
    {
      temp_file_path = svn_wc__text_base_path (b->wc_path, TRUE, b->pool);

      /* Remember that the default diff order is to show repos->wc,
         but we ask the server for a wc->repos diff.  So if
         'reverse_order' is TRUE, then we do what the server says:
         show an add. */
      if (eb->reverse_order)
        /* ### Show the properties as well. */
        SVN_ERR (b->edit_baton->callbacks->file_added
                 (NULL, NULL, NULL, b->path,
                  empty_file,
                  temp_file_path,
                  0,
                  entry ? entry->revision : SVN_INVALID_REVNUM,
                  NULL,
                  pristine_mimetype,
                  apr_array_make (pool, 1, sizeof (svn_prop_t)), NULL,
                  b->edit_baton->callback_baton));
      else
        {
          apr_hash_t *props = apr_hash_make (pool);
          int i;

          /* Convert the propchanges to a hash table. */
          for (i = 0; i < b->propchanges->nelts; ++i)
            {
              const svn_prop_t *prop = &APR_ARRAY_IDX (b->propchanges, i,
                                                       svn_prop_t);
              apr_hash_set (props, prop->name, APR_HASH_KEY_STRING,
                            prop->value);
            }

          /* Add is required to change working-copy into requested revision, so
             diff should show this as a delete. Thus compare the repository
             file against the empty file. */
          SVN_ERR (b->edit_baton->callbacks->file_deleted
                   (NULL, NULL, b->path,
                    temp_file_path,
                    empty_file,
                    pristine_mimetype,
                    NULL,
                    props,
                    b->edit_baton->callback_baton));
        }
    }
  else
    {
      /* Be careful with errors to ensure that the temporary translated
         file is deleted. */
      svn_error_t *err1, *err2 = SVN_NO_ERROR;
      const char *localfile = NULL;

      if (b->temp_file) /* A props-only change will not have opened a file */
        {
          if (eb->use_text_base)
            localfile = svn_wc__text_base_path (b->path, FALSE, b->pool);
          else if (b->schedule_delete)
            localfile = empty_file;
          else
            /* a detranslated version of the working file */
            SVN_ERR (svn_wc_translated_file (&localfile, b->path, adm_access,
                                             TRUE, b->pool));
          temp_file_path = svn_wc__text_base_path (b->wc_path, TRUE, b->pool);
        }
      else
        localfile = temp_file_path = NULL;
      
      if (b->propchanges->nelts > 0
          && ! eb->reverse_order)
        reverse_propchanges (b->baseprops, b->propchanges, b->pool);

      if (localfile || b->propchanges->nelts > 0)
        {
          err1 = b->edit_baton->callbacks->file_changed
            (NULL, NULL, NULL,
             b->path,
             eb->reverse_order ? localfile : temp_file_path,
             eb->reverse_order ? temp_file_path : localfile,
             eb->reverse_order ? SVN_INVALID_REVNUM : b->edit_baton->revnum,
             eb->reverse_order ? b->edit_baton->revnum : SVN_INVALID_REVNUM,
             eb->reverse_order ? working_mimetype : pristine_mimetype,
             eb->reverse_order ? pristine_mimetype : working_mimetype,
             b->propchanges, b->baseprops,
             b->edit_baton->callback_baton);
      
          if (localfile && (! eb->use_text_base) && (! b->schedule_delete)
              && localfile != b->path)
            err2 = svn_io_remove_file (localfile, b->pool);

          if (err1 || err2)
            {
              if (err1 && err2)
                svn_error_clear (err2);
              return err1 ? err1 : err2;
            }
        }
    }

  return SVN_NO_ERROR;
}


static svn_error_t *
change_file_prop (void *file_baton,
                  const char *name,
                  const svn_string_t *value,
                  apr_pool_t *pool)
{
  struct file_baton *b = file_baton;
  svn_prop_t *propchange;

  propchange = apr_array_push (b->propchanges);
  propchange->name = apr_pstrdup (b->pool, name);
  propchange->value = value ? svn_string_dup (value, b->pool) : NULL;
  
  /* Read the baseprops if you haven't already. */
  if (! b->fetched_baseprops)
    SVN_ERR (load_base_props (b));

  return SVN_NO_ERROR;
}


/* An editor function. */
static svn_error_t *
change_dir_prop (void *dir_baton,
                 const char *name,
                 const svn_string_t *value,
                 apr_pool_t *pool)
{
  struct dir_baton *db = dir_baton;
  svn_prop_t *propchange;

  propchange = apr_array_push (db->propchanges);
  propchange->name = apr_pstrdup (db->pool, name);
  propchange->value = value ? svn_string_dup (value, db->pool) : NULL;

  /* Read the baseprops if you haven't already. */
  if (! db->fetched_baseprops)
    {
      /* the 'base' props to compare against, in this case, are
         actually the working props.  that's what we do with texts,
         anyway, in the 'svn diff -rN foo' case.  */

      /* This path might not exist in the working copy, in which case
         the baseprops remains an empty hash. */
      SVN_ERR (svn_wc_prop_list (&(db->baseprops), db->path,
                                 db->edit_baton->anchor, db->pool));
      db->fetched_baseprops = TRUE;
    }

  return SVN_NO_ERROR;
}


/* An editor function. */
static svn_error_t *
close_edit (void *edit_baton,
            apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;

  if (!eb->root_opened)
    {
      struct dir_baton *b;

      b = make_dir_baton (eb->anchor_path, NULL, eb, FALSE, eb->pool);
      SVN_ERR (directory_elements_diff (b, FALSE));
    }

  return SVN_NO_ERROR;
}

/* An svn_wc_diff_callbacks2_t function. */
static svn_error_t *
file_changed (svn_wc_adm_access_t *adm_access,
              svn_wc_notify_state_t *contentstate,
              svn_wc_notify_state_t *propstate,
              const char *path,
              const char *tmpfile1,
              const char *tmpfile2,
              svn_revnum_t rev1,
              svn_revnum_t rev2,
              const char *mimetype1,
              const char *mimetype2,
              const apr_array_header_t *propchanges,
              apr_hash_t *originalprops,
              void *diff_baton)
{
  struct callbacks_wrapper_baton *b = diff_baton;
  if (tmpfile2 != NULL)
    SVN_ERR (b->callbacks->file_changed (adm_access, contentstate, path,
                                         tmpfile1, tmpfile2,
                                         rev1, rev2, mimetype1, mimetype2,
                                         b->baton));
  if (propchanges->nelts > 0)
    SVN_ERR (b->callbacks->props_changed (adm_access, propstate, path,
                                          propchanges, originalprops,
                                          b->baton));

  return SVN_NO_ERROR;
}

/* An svn_wc_diff_callbacks2_t function. */
static svn_error_t *
file_added (svn_wc_adm_access_t *adm_access,
            svn_wc_notify_state_t *contentstate,
            svn_wc_notify_state_t *propstate,
            const char *path,
            const char *tmpfile1,
            const char *tmpfile2,
            svn_revnum_t rev1,
            svn_revnum_t rev2,
            const char *mimetype1,
            const char *mimetype2,
            const apr_array_header_t *propchanges,
            apr_hash_t *originalprops,
            void *diff_baton)
{
  struct callbacks_wrapper_baton *b = diff_baton;
  SVN_ERR (b->callbacks->file_added (adm_access, contentstate, path,
                                     tmpfile1, tmpfile2, rev1, rev2,
                                     mimetype1, mimetype2, b->baton));
  if (propchanges->nelts > 0)
    SVN_ERR (b->callbacks->props_changed (adm_access, propstate, path,
                                          propchanges, originalprops,
                                          b->baton));

  return SVN_NO_ERROR;
}

/* A diff_callbakcs2 function. */
static svn_error_t *
file_deleted (svn_wc_adm_access_t *adm_access,
              svn_wc_notify_state_t *state,
              const char *path,
              const char *tmpfile1,
              const char *tmpfile2,
              const char *mimetype1,
              const char *mimetype2,
              apr_hash_t *originalprops,
              void *diff_baton)
{
  struct callbacks_wrapper_baton *b = diff_baton;

  assert (originalprops);

  return b->callbacks->file_deleted (adm_access, state, path,
                                     tmpfile1, tmpfile2, mimetype1, mimetype2,
                                     b->baton);
}
  
/* An svn_wc_diff_callbacks2_t function. */
static svn_error_t *
dir_added (svn_wc_adm_access_t *adm_access,
           svn_wc_notify_state_t *state,
           const char *path,
           svn_revnum_t rev,
           void *diff_baton)
{
  struct callbacks_wrapper_baton *b = diff_baton;

  return b->callbacks->dir_added (adm_access, state, path, rev, b->baton);
}
  
/* An svn_wc_diff_callbacks2_t function. */
static svn_error_t *
dir_deleted (svn_wc_adm_access_t *adm_access,
             svn_wc_notify_state_t *state,
             const char *path,
             void *diff_baton)
{
  struct callbacks_wrapper_baton *b = diff_baton;

  return b->callbacks->dir_deleted (adm_access, state, path, b->baton);
}
  
/* An svn_wc_diff_callbacks2_t function. */
static svn_error_t *
dir_props_changed (svn_wc_adm_access_t *adm_access,
                   svn_wc_notify_state_t *state,
                   const char *path,
                   const apr_array_header_t *propchanges,
                   apr_hash_t *originalprops,
                   void *diff_baton)
{
  struct callbacks_wrapper_baton *b = diff_baton;
  return b->callbacks->props_changed (adm_access, state, path, propchanges,
                                      originalprops, b->baton);
}

/* Used to wrap svn_diff_callbacks_t as an svn_wc_diff_callbacks_2t. */
static struct svn_wc_diff_callbacks2_t callbacks_wrapper = {
  file_changed,
  file_added,
  file_deleted,
  dir_added,
  dir_deleted,
  dir_props_changed

};

/* Public Interface */


/* Create a diff editor and baton. */
svn_error_t *
svn_wc_get_diff_editor3 (svn_wc_adm_access_t *anchor,
                         const char *target,
                         const svn_wc_diff_callbacks2_t *callbacks,
                         void *callback_baton,
                         svn_boolean_t recurse,
                         svn_boolean_t ignore_ancestry,
                         svn_boolean_t use_text_base,
                         svn_boolean_t reverse_order,
                         svn_cancel_func_t cancel_func,
                         void *cancel_baton,
                         const svn_delta_editor_t **editor,
                         void **edit_baton,
                         apr_pool_t *pool)
{
  struct edit_baton *eb;
  svn_delta_editor_t *tree_editor;

  eb = make_editor_baton (anchor, target, callbacks, callback_baton,
                          recurse, ignore_ancestry, use_text_base,
                          reverse_order, pool);
  tree_editor = svn_delta_default_editor (eb->pool);

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

  SVN_ERR (svn_delta_get_cancellation_editor (cancel_func,
                                              cancel_baton,
                                              tree_editor,
                                              eb,
                                              editor,
                                              edit_baton,
                                              pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_get_diff_editor2 (svn_wc_adm_access_t *anchor,
                         const char *target,
                         const svn_wc_diff_callbacks_t *callbacks,
                         void *callback_baton,
                         svn_boolean_t recurse,
                         svn_boolean_t ignore_ancestry,
                         svn_boolean_t use_text_base,
                         svn_boolean_t reverse_order,
                         svn_cancel_func_t cancel_func,
                         void *cancel_baton,
                         const svn_delta_editor_t **editor,
                         void **edit_baton,
                         apr_pool_t *pool)
{
  struct callbacks_wrapper_baton *b = apr_pcalloc (pool, sizeof (*b));
  b->callbacks = callbacks;
  b->baton = callback_baton;
  return svn_wc_get_diff_editor3 (anchor, target, &callbacks_wrapper, b,
                                  recurse, ignore_ancestry, use_text_base,
                                  reverse_order, cancel_func, cancel_baton,
                                  editor, edit_baton, pool);
}

svn_error_t *
svn_wc_get_diff_editor (svn_wc_adm_access_t *anchor,
                        const char *target,
                        const svn_wc_diff_callbacks_t *callbacks,
                        void *callback_baton,
                        svn_boolean_t recurse,
                        svn_boolean_t use_text_base,
                        svn_boolean_t reverse_order,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        const svn_delta_editor_t **editor,
                        void **edit_baton,
                        apr_pool_t *pool)
{
  return svn_wc_get_diff_editor2 (anchor, target, callbacks, callback_baton,
                                  recurse, FALSE, use_text_base, reverse_order,
                                  cancel_func, cancel_baton,
                                  editor, edit_baton, pool);
}


/* Compare working copy against the text-base. */
svn_error_t *
svn_wc_diff3 (svn_wc_adm_access_t *anchor,
              const char *target,
              const svn_wc_diff_callbacks2_t *callbacks,
              void *callback_baton,
              svn_boolean_t recurse,
              svn_boolean_t ignore_ancestry,
              apr_pool_t *pool)
{
  struct edit_baton *eb;
  struct dir_baton *b;
  const svn_wc_entry_t *entry;
  const char *target_path;
  svn_wc_adm_access_t *adm_access;

  eb = make_editor_baton (anchor, target, callbacks, callback_baton,
                          recurse, ignore_ancestry, FALSE, FALSE, pool);

  target_path = svn_path_join (svn_wc_adm_access_path (anchor), target,
                               eb->pool);

  SVN_ERR (svn_wc_adm_probe_retrieve (&adm_access, anchor, target_path,
                                      eb->pool));
  SVN_ERR (svn_wc_entry (&entry, target_path, adm_access, FALSE, eb->pool));

  if (entry->kind == svn_node_dir)
    b = make_dir_baton (target_path, NULL, eb, FALSE, eb->pool);
  else
    b = make_dir_baton (eb->anchor_path, NULL, eb, FALSE, eb->pool);

  SVN_ERR (directory_elements_diff (b, FALSE));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_diff2 (svn_wc_adm_access_t *anchor,
              const char *target,
              const svn_wc_diff_callbacks_t *callbacks,
              void *callback_baton,
              svn_boolean_t recurse,
              svn_boolean_t ignore_ancestry,
              apr_pool_t *pool)
{
  struct callbacks_wrapper_baton *b = apr_pcalloc (pool, sizeof (*b));
  b->callbacks = callbacks;
  b->baton = callback_baton;
  return svn_wc_diff3 (anchor, target, &callbacks_wrapper, b,
                       recurse, ignore_ancestry, pool);
}

svn_error_t *
svn_wc_diff (svn_wc_adm_access_t *anchor,
             const char *target,
             const svn_wc_diff_callbacks_t *callbacks,
             void *callback_baton,
             svn_boolean_t recurse,
             apr_pool_t *pool)
{
  return svn_wc_diff2 (anchor, target, callbacks, callback_baton,
                       recurse, FALSE, pool);
}
