/*
 * get_editor.c :  routines for update and checkout
 *
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */



#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_tables.h>
#include <apr_file_io.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>

#include "svn_types.h"
#include "svn_delta.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_xml.h"
#include "svn_error.h"
#include "svn_io.h"
#include "svn_hash.h"
#include "svn_wc.h"
#include "config.h"

#include "wc.h"



/*** batons ***/

struct edit_baton
{
  svn_string_t *dest_dir;
  svn_revnum_t target_revision;

  /* These used only in checkouts. */
  svn_boolean_t is_checkout;
  svn_string_t *ancestor_path;
  svn_string_t *repository;

  apr_pool_t *pool;
};


struct dir_baton
{
  /* The path to this directory. */
  svn_string_t *path;

  /* Basename of this directory. */
  svn_string_t *name;

  /* The number of other changes associated with this directory in the
     delta (typically, the number of files being changed here, plus
     this dir itself).  BATON->ref_count starts at 1, is incremented
     for each entity being changed, and decremented for each
     completion of one entity's changes.  When the ref_count is 0, the
     directory may be safely set to the target revision, and this baton
     freed. */
  int ref_count;

  /* The global edit baton. */
  struct edit_baton *edit_baton;

  /* Baton for this directory's parent, or NULL if this is the root
     directory. */
  struct dir_baton *parent_baton;

  /* Gets set iff there's a change to this directory's properties, to
     guide us when syncing adm files later. */
  svn_boolean_t prop_changed;

  /* An array of (svn_prop_t *)'s, representing all the property
     changes to be applied to this file. */
  apr_array_header_t *propchanges;

  /* The pool in which this baton itself is allocated. */
  apr_pool_t *pool;
};


struct handler_baton
{
  apr_file_t *source;
  apr_file_t *dest;
  svn_txdelta_window_handler_t *apply_handler;
  void *apply_baton;
  apr_pool_t *pool;
  struct file_baton *fb;
};

/* Create a new dir_baton for subdir NAME in PARENT_PATH with
 * EDIT_BATON, using a new subpool of POOL.
 *
 * The new baton's ref_count is 1.
 *
 * NAME and PARENT_BATON can be null, meaning this is the root baton.
 */
static struct dir_baton *
make_dir_baton (svn_string_t *name,
                struct edit_baton *edit_baton,
                struct dir_baton *parent_baton,
                apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  struct dir_baton *d = apr_pcalloc (subpool, sizeof (*d));
  svn_string_t *parent_path
    = parent_baton ? parent_baton->path : edit_baton->dest_dir;
  svn_string_t *path = svn_string_dup (parent_path, subpool);

  if (name)
    svn_path_add_component (path, name, svn_path_local_style);

  d->path         = path;
  d->edit_baton   = edit_baton;
  d->parent_baton = parent_baton;
  d->ref_count    = 1;
  d->pool         = subpool;
  d->propchanges  = apr_make_array (subpool, 1, sizeof(svn_prop_t *));

  if (name)
    d->name = svn_string_dup (name, subpool);

  if (parent_baton)
    parent_baton->ref_count++;

  return d;
}


/* Avoid the circular prototypes problem. */
static svn_error_t *decrement_ref_count (struct dir_baton *d);


static svn_error_t *
free_dir_baton (struct dir_baton *dir_baton)
{
  svn_error_t *err;
  struct dir_baton *parent = dir_baton->parent_baton;

  /* Bump this dir to the new revision. */
  err = svn_wc__entry_merge_sync (dir_baton->path,
                                  NULL,
                                  dir_baton->edit_baton->target_revision,
                                  svn_node_dir,
                                  0,
                                  0,
                                  0,
                                  dir_baton->pool,
                                  NULL,
                                  NULL);
   if (err)
     return err;

  /* After we destroy DIR_BATON->pool, DIR_BATON itself is lost. */
  apr_destroy_pool (dir_baton->pool);

  /* We've declared this directory done, so decrement its parent's ref
     count too. */ 
  if (parent)
    {
      err = decrement_ref_count (parent);
      if (err)
        return err;
    }

  return SVN_NO_ERROR;
}


/* Decrement DIR_BATON's ref count, and if the count hits 0, call
 * free_dir_baton().
 *
 * Note: There is no corresponding function for incrementing the
 * ref_count.  As far as we know, nothing special depends on that, so
 * it's always done inline.
 */
static svn_error_t *
decrement_ref_count (struct dir_baton *d)
{
  d->ref_count--;

  if (d->ref_count == 0)
    return free_dir_baton (d);

  return SVN_NO_ERROR;
}


struct file_baton
{
  /* Baton for this file's parent directory. */
  struct dir_baton *dir_baton;

  /* Pool specific to this file_baton. */
  apr_pool_t *pool;

  /* Name of this file (its entry in the directory). */
  const svn_string_t *name;

  /* Path to this file, either abs or relative to the change-root. */
  svn_string_t *path;

  /* This gets set if the file underwent a text change, which guides
     the code that syncs up the adm dir and working copy. */
  svn_boolean_t text_changed;

  /* This gets set if there's a conflict while merging the
     repository's file into the locally changed working file */
  svn_boolean_t text_conflict;

  /* This gets set if the file underwent a prop change, which guides
     the code that syncs up the adm dir and working copy. */
  svn_boolean_t prop_changed;

  /* This gets set if there's a conflict when merging a prop-delta
     into the locally modified props.  */
  svn_boolean_t prop_conflict;

  /* An array of (svn_prop_t *)'s, representing all the property
     changes to be applied to this file. */
  apr_array_header_t *propchanges;

};


/* Make a file baton, using a new subpool of PARENT_DIR_BATON's pool.
   NAME is just one component, not a path. */
static struct file_baton *
make_file_baton (struct dir_baton *parent_dir_baton, svn_string_t *name)
{
  apr_pool_t *subpool = svn_pool_create (parent_dir_baton->pool);
  struct file_baton *f = apr_pcalloc (subpool, sizeof (*f));
  svn_string_t *path = svn_string_dup (parent_dir_baton->path,
                                       subpool);

  /* Make the file's on-disk name. */
  svn_path_add_component (path,
                          name,
                          svn_path_local_style);

  f->pool       = subpool;
  f->dir_baton  = parent_dir_baton;
  f->name       = name;
  f->path       = path;
  f->propchanges = apr_make_array (subpool, 1, sizeof(svn_prop_t *));

  parent_dir_baton->ref_count++;

  return f;
}


static svn_error_t *
free_file_baton (struct file_baton *fb)
{
  struct dir_baton *parent = fb->dir_baton;
  apr_destroy_pool (fb->pool);
  return decrement_ref_count (parent);
}



/*** Helpers for the editor callbacks. ***/

static svn_error_t *
read_from_file (void *baton, char *buffer, apr_size_t *len, apr_pool_t *pool)
{
  apr_file_t *fp = baton;
  apr_status_t status;

  if (fp == NULL)
    {
      *len = 0;
      return SVN_NO_ERROR;
    }
  status = apr_full_read (fp, buffer, *len, len);
  if (status && !APR_STATUS_IS_EOF(status))
    return svn_error_create (status, 0, NULL, pool, "Can't read base file");
  return SVN_NO_ERROR;
}


static svn_error_t *
write_to_file (void *baton, const char *data, apr_size_t *len,
               apr_pool_t *pool)
{
  apr_file_t *fp = baton;
  apr_status_t status;

  status = apr_full_write (fp, data, *len, len);
  if (status)
    return svn_error_create (status, 0, NULL, pool,
                             "Can't write new base file");
  return SVN_NO_ERROR;
}


static svn_error_t *
window_handler (svn_txdelta_window_t *window, void *baton)
{
  struct handler_baton *hb = baton;
  struct file_baton *fb = hb->fb;
  svn_error_t *err = NULL, *err2 = NULL;

  /* Apply this window.  We may be done at that point.  */
  err = hb->apply_handler (window, hb->apply_baton);
  if (window != NULL && err == SVN_NO_ERROR)
    return err;

  /* Either we're done (window is NULL) or we had an error.  In either
     case, clean up the handler.  */
  if ((! fb->dir_baton->edit_baton->is_checkout) && hb->source)
    {
      err2 = svn_wc__close_text_base (hb->source, fb->path, 0, fb->pool);
      if (err2 != SVN_NO_ERROR && err == SVN_NO_ERROR)
        err = err2;
    }
  err2 = svn_wc__close_text_base (hb->dest, fb->path, 0, fb->pool);
  if (err2 != SVN_NO_ERROR && err == SVN_NO_ERROR)
    err = err2;
  apr_destroy_pool (hb->pool);

  if (err != SVN_NO_ERROR)
    {
      /* We failed to apply the patch; clean up the temporary file.  */
      apr_pool_t *pool = svn_pool_create (fb->pool);
      svn_string_t *tmppath = svn_wc__text_base_path (fb->path, TRUE, pool);

      apr_remove_file (tmppath->data, pool);
      apr_destroy_pool (pool);
    }
  else
    {
      /* Leave a note in the baton indicating that there's new text to
         sync up.  */
      fb->text_changed = 1;
    }

  return err;
}


/* Prepare directory PATH for updating or checking out.
 *
 * If FORCE is non-zero, then the directory will definitely exist
 * after this call, else the directory must exist already.
 *
 * If the path already exists, but is not a working copy for
 * DIRECTORY, then an error will be returned. 
 */
static svn_error_t *
prep_directory (svn_string_t *path,
                svn_string_t *repository,
                svn_string_t *ancestor_path,
                svn_revnum_t ancestor_revision,
                svn_boolean_t force,
                apr_pool_t *pool)
{
  svn_error_t *err;

  /* kff todo: how about a sanity check that it's not a dir of the
     same name from a different repository or something? 
     Well, that will be later on down the line... */

  if (force)   /* Make sure the directory exists. */
    {
      err = svn_wc__ensure_directory (path, pool);
      if (err)
        return err;
    }

  /* Make sure it's the right working copy, either by creating it so,
     or by checking that it is so already. */
  err = svn_wc__ensure_wc (path,
                           repository,
                           ancestor_path,
                           ancestor_revision,
                           pool);
  if (err)
    return err;

  return SVN_NO_ERROR;
}



/*** The callbacks we'll plug into an svn_delta_edit_fns_t structure. ***/

static svn_error_t *
replace_root (void *edit_baton,
              void **dir_baton)
{
  struct edit_baton *eb = edit_baton;
  struct dir_baton *d;
  svn_error_t *err;
  svn_string_t *ancestor_path;
  svn_revnum_t ancestor_revision;

  *dir_baton = d = make_dir_baton (NULL, eb, NULL, eb->pool);

  if (eb->is_checkout)
    {
      ancestor_path = eb->ancestor_path;
      ancestor_revision = eb->target_revision;
      
      err = prep_directory (d->path,
                            eb->repository,
                            ancestor_path,
                            ancestor_revision,
                            1, /* force */
                            d->pool);
      if (err)
        return err;
    }

  return SVN_NO_ERROR;
}


static svn_error_t *
delete_item (svn_string_t *name, void *parent_baton)
{
  svn_error_t *err;
  struct dir_baton *parent_dir_baton = parent_baton;
  apr_status_t apr_err;
  apr_file_t *log_fp = NULL;
  svn_string_t *log_item = svn_string_create ("", parent_dir_baton->pool);

  err = svn_wc__lock (parent_dir_baton->path, 0, parent_dir_baton->pool);
  if (err)
    return err;

    err = svn_wc__open_adm_file (&log_fp,
                                 parent_dir_baton->path,
                                 SVN_WC__ADM_LOG,
                                 (APR_WRITE | APR_CREATE), /* not excl */
                                 parent_dir_baton->pool);
    if (err)
      return err;

    svn_xml_make_open_tag (&log_item,
                           parent_dir_baton->pool,
                           svn_xml_self_closing,
                           SVN_WC__LOG_DELETE_ENTRY,
                           SVN_WC__LOG_ATTR_NAME,
                           name,
                           NULL);

    apr_err = apr_full_write (log_fp, log_item->data, log_item->len, NULL);
    if (apr_err)
      {
        apr_close (log_fp);
        return svn_error_createf (apr_err, 0, NULL, parent_dir_baton->pool,
                                  "delete error writing %s's log file",
                                  parent_dir_baton->path->data);
      }

    err = svn_wc__close_adm_file (log_fp,
                                  parent_dir_baton->path,
                                  SVN_WC__ADM_LOG,
                                  1, /* sync */
                                  parent_dir_baton->pool);
    if (err)
      return err;
    
    err = svn_wc__run_log (parent_dir_baton->path, parent_dir_baton->pool);
    if (err)
      return err;

    err = svn_wc__unlock (parent_dir_baton->path, parent_dir_baton->pool);
    if (err)
      return err;

  return SVN_NO_ERROR;
}


static svn_error_t *
add_directory (svn_string_t *name,
               void *parent_baton,
               svn_string_t *ancestor_path,
               svn_revnum_t ancestor_revision,
               void **child_baton)
{
  svn_error_t *err;
  struct dir_baton *parent_dir_baton = parent_baton;

  struct dir_baton *this_dir_baton
    = make_dir_baton (name,
                      parent_dir_baton->edit_baton,
                      parent_dir_baton,
                      parent_dir_baton->pool);

  /* Notify the parent that this child dir exists.  This can happen
     right away, there is no need to wait until the child is done. */
  err = svn_wc__entry_merge_sync (parent_dir_baton->path,
                                  this_dir_baton->name,
                                  SVN_INVALID_REVNUM,
                                  svn_node_dir,
                                  0,
                                  0,
                                  0,
                                  parent_dir_baton->pool,
                                  NULL,
                                  NULL);
  if (err)
    return err;


  err = prep_directory (this_dir_baton->path,
                        this_dir_baton->edit_baton->repository,
                        ancestor_path,
                        ancestor_revision,
                        1, /* force */
                        this_dir_baton->pool);
  if (err)
    return (err);

  *child_baton = this_dir_baton;

  return SVN_NO_ERROR;
}


static svn_error_t *
replace_directory (svn_string_t *name,
                   void *parent_baton,
                   svn_string_t *ancestor_path,
                   svn_revnum_t ancestor_revision,
                   void **child_baton)
{
  struct dir_baton *parent_dir_baton = parent_baton;

  /* kff todo: check that the dir exists locally, find it somewhere if
     its not there?  Yes, all this and more...  And ancestor_path and
     ancestor_revision need to get used. */

  struct dir_baton *this_dir_baton
    = make_dir_baton (name,
                      parent_dir_baton->edit_baton,
                      parent_dir_baton,
                      parent_dir_baton->pool);

  *child_baton = this_dir_baton;

  return SVN_NO_ERROR;
}


static svn_error_t *
change_dir_prop (void *dir_baton,
                 svn_string_t *name,
                 svn_string_t *value)
{
  svn_string_t *local_name, *local_value;
  svn_prop_t *propchange, **receiver;
  struct dir_baton *db = dir_baton;

  /* Duplicate storage of name/value pair;  they should live in the
     dir baton's pool, not some pool within the editor's driver. :)
  */
  local_name = svn_string_dup (name, db->pool);
  if (value)
    /* remember that value could be NULL, signifying a property
       `delete' */
    local_value = svn_string_dup (value, db->pool);
  else
    local_value = NULL;
  
  /* Build propchange object */
  propchange = apr_pcalloc (db->pool, sizeof(*propchange));
  propchange->name = local_name;
  propchange->value = local_value;

  /* Push the object to the file baton's array of propchanges */
  receiver = (svn_prop_t **) apr_push_array (db->propchanges);
  *receiver = propchange;

  /* Let close_dir() know that propchanges are waiting to be
     applied. */
  db->prop_changed = 1;

  return SVN_NO_ERROR;
}





static svn_error_t *
close_directory (void *dir_baton)
{
  struct dir_baton *db = dir_baton;
  svn_error_t *err = NULL;

  /* kff todo: now that the child is finished, we should make an entry
     in the parent's base-tree (although frankly I'm beginning to
     wonder if child directories should be recorded anywhere but in
     themselves; perhaps that would be best, and just let the parent
     deduce their existence.  We can still tell when an update of the
     parent is complete, by refcounting.) */

  /* If this directory has property changes stored up, now is the time
     to deal with them. */
  if (db->prop_changed)
    {
      svn_boolean_t prop_modified;
      apr_status_t apr_err;
      char *revision_str;
      apr_file_t *log_fp = NULL;

      /* to hold log messages: */
      svn_string_t *entry_accum = svn_string_create ("", db->pool);

      /* Lock down the administrative area */
      err = svn_wc__lock (db->path, 0, db->pool);
      if (err)
        return err;
      
      /* Open log file */
      err = svn_wc__open_adm_file (&log_fp,
                                   db->path,
                                   SVN_WC__ADM_LOG,
                                   (APR_WRITE | APR_CREATE), /* not excl */
                                   db->pool);
      if (err) return err;

      /* Merge pending properties into temporary files and detect
         conflicts. */
      err = svn_wc__do_property_merge (db->path, NULL,
                                       db->propchanges, db->pool,
                                       &entry_accum);
      if (err) 
        return 
          svn_error_quick_wrap (err, "close_dir: couldn't do prop merge.");

      /* Set revision. */
      revision_str = apr_psprintf (db->pool,
                                   "%d",
                                   db->edit_baton->target_revision);
      
      /* Write a log entry to bump the directory's revision. */
      svn_xml_make_open_tag (&entry_accum,
                             db->pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_MODIFY_ENTRY,
                             SVN_WC__LOG_ATTR_NAME,
                             svn_string_create
                             (SVN_WC_ENTRY_THIS_DIR, db->pool),
                             SVN_WC_ENTRY_ATTR_REVISION,
                             svn_string_create (revision_str, db->pool),
                             NULL);


      /* Are the directory's props locally modified? */
      err = svn_wc_props_modified_p (&prop_modified,
                                     db->path,
                                     db->pool);
      if (err) return err;

      /* Log entry which sets a new property timestamp, but *only* if
         there are no local changes to the props. */
      if (! prop_modified)
        svn_xml_make_open_tag (&entry_accum,
                               db->pool,
                               svn_xml_self_closing,
                               SVN_WC__LOG_MODIFY_ENTRY,
                               SVN_WC__LOG_ATTR_NAME,
                               svn_string_create
                               (SVN_WC_ENTRY_THIS_DIR, db->pool),
                               SVN_WC_ENTRY_ATTR_PROP_TIME,
                               /* use wfile time */
                               svn_string_create (SVN_WC_TIMESTAMP_WC,
                                                  db->pool),
                               NULL);

      
      /* Write our accumulation of log entries into a log file */
      apr_err = apr_full_write (log_fp, entry_accum->data,
                                entry_accum->len, NULL);
      if (apr_err)
        {
          apr_close (log_fp);
          return svn_error_createf (apr_err, 0, NULL, db->pool,
                                    "close_dir: error writing %s's log file",
                                    db->path->data);
        }
      
      /* The log is ready to run, close it. */
      err = svn_wc__close_adm_file (log_fp,
                                    db->path,
                                    SVN_WC__ADM_LOG,
                                    1, /* sync */
                                    db->pool);
      if (err) return err;

      /* Run the log. */
      err = svn_wc__run_log (db->path, db->pool);
      if (err) return err;

      /* Unlock, we're done modifying directory props. */
      err = svn_wc__unlock (db->path, db->pool);
      if (err) return err;            
    }


  /* We're truly done with this directory now.  decrement_ref_count
  will actually destroy dir_baton if the ref count reaches zero, so we
  call this LAST. */
  err = decrement_ref_count (db);
  if (err)
    return err;

  return SVN_NO_ERROR;
}



/* Common code for add_file() and replace_file(). */
static svn_error_t *
add_or_replace_file (svn_string_t *name,
                     void *parent_baton,
                     svn_string_t *ancestor_path,
                     svn_revnum_t ancestor_revision,
                     void **file_baton,
                     svn_boolean_t adding)  /* 0 if replacing */
{
  struct dir_baton *parent_dir_baton = parent_baton;
  struct file_baton *fb;
  svn_error_t *err;
  apr_hash_t *entries = NULL;
  svn_wc_entry_t *entry;
  svn_boolean_t is_wc;

  err = svn_wc__entries_read (&entries,
                              parent_dir_baton->path,
                              parent_dir_baton->pool);
  if (err)
    return err;

  entry = apr_hash_get (entries, name->data, name->len);

  /* kff todo: if file is marked as removed by user, then flag a
     conflict in the entry and proceed.  Similarly if it has changed
     kind. */

  /* Sanity checks. */
  if (adding && entry)
    return svn_error_createf (SVN_ERR_WC_ENTRY_EXISTS, 0, NULL,
                              parent_dir_baton->pool,
                              "trying to add versioned file "
                              "%s in directory %s",
                              name->data, parent_dir_baton->path->data);
  else if ((! adding) && (! entry))
    return svn_error_createf (SVN_ERR_WC_ENTRY_NOT_FOUND, 0, NULL,
                              parent_dir_baton->pool,
                              "trying to replace non-versioned file "
                              "%s in directory %s",
                              name->data, parent_dir_baton->path->data);

        
  /* Make sure we've got a working copy to put the file in. */
  /* kff todo: need stricter logic here */
  err = svn_wc_check_wc (parent_dir_baton->path, &is_wc,
                         parent_dir_baton->pool);
  if (err)
    return err;
  else if (! is_wc)
    return svn_error_createf
      (SVN_ERR_WC_OBSTRUCTED_UPDATE, 0, NULL, parent_dir_baton->pool,
       "add_or_replace_file: %s is not a working copy directory",
       parent_dir_baton->path->data);

  /* Set up the file's baton. */
  fb = make_file_baton (parent_dir_baton, name);
  *file_baton = fb;

  return SVN_NO_ERROR;
}


static svn_error_t *
add_file (svn_string_t *name,
          void *parent_baton,
          svn_string_t *ancestor_path,
          svn_revnum_t ancestor_revision,
          void **file_baton)
{
  return add_or_replace_file
    (name, parent_baton, ancestor_path, ancestor_revision, file_baton, 1);
}


static svn_error_t *
replace_file (svn_string_t *name,
              void *parent_baton,
              svn_string_t *ancestor_path,
              svn_revnum_t ancestor_revision,
              void **file_baton)
{
  return add_or_replace_file
    (name, parent_baton, ancestor_path, ancestor_revision, file_baton, 0);
}


static svn_error_t *
apply_textdelta (void *file_baton, 
                 svn_txdelta_window_handler_t **handler,
                 void **handler_baton)
{
  struct file_baton *fb = file_baton;
  apr_pool_t *subpool = svn_pool_create (fb->pool);
  struct handler_baton *hb = apr_palloc (subpool, sizeof (*hb));
  svn_error_t *err;

  /* Open the text base for reading, unless this is a checkout. */
  hb->source = NULL;
  if (! fb->dir_baton->edit_baton->is_checkout)
    {
      /* 
         kff todo: what we really need to do here is:
         
         1. See if there's a file or dir by this name already here.
         2. See if it's under revision control.
         3. If both are true, open text-base.
         4. If only 1 is true, bail, because we can't go destroying
            user's files (or as an alternative to bailing, move it to
            some tmp name and somehow tell the user, but communicating
            with the user without erroring is a whole callback system
            we haven't finished inventing yet.)
      */

      err = svn_wc__open_text_base (&hb->source, fb->path, APR_READ, subpool);
      if (err && !APR_STATUS_IS_ENOENT(err->apr_err))
        {
          if (hb->source)
            svn_wc__close_text_base (hb->source, fb->path, 0, subpool);
          apr_destroy_pool (subpool);
          return err;
        }
      else if (err)
        hb->source = NULL;  /* make sure */
    }

  /* Open the text base for writing (this will get us a temporary file).  */
  hb->dest = NULL;
  err = svn_wc__open_text_base (&hb->dest, fb->path,
                                (APR_WRITE | APR_TRUNCATE | APR_CREATE),
                                subpool);
  if (err)
    {
      if (hb->dest)
        svn_wc__close_text_base (hb->dest, fb->path, 0, subpool);
      apr_destroy_pool (subpool);
      return err;
    }
  
  /* Prepare to apply the delta.  */
  svn_txdelta_apply (read_from_file, hb->source, write_to_file, hb->dest,
                     subpool, &hb->apply_handler, &hb->apply_baton);
  
  hb->pool = subpool;
  hb->fb = fb;
  
  /* We're all set.  */
  *handler_baton = hb;
  *handler = window_handler;

  return SVN_NO_ERROR;
}




static svn_error_t *
change_file_prop (void *file_baton,
                  svn_string_t *name,
                  svn_string_t *value)
{
  struct file_baton *fb = file_baton;
  svn_string_t *local_name, *local_value;
  svn_prop_t *propchange, **receiver;

  /* Duplicate storage of name/value pair;  they should live in the
     file baton's pool, not some pool within the editor's driver. :)
  */
  local_name = svn_string_dup (name, fb->pool);
  if (value)
    /* remember that value could be NULL, signifying a property
       `delete' */
    local_value = svn_string_dup (value, fb->pool);
  else
    local_value = NULL;

  /* Build propchange object */
  propchange = apr_pcalloc (fb->pool, sizeof(*propchange));
  propchange->name = local_name;
  propchange->value = local_value;

  /* Push the object to the file baton's array of propchanges */
  receiver = (svn_prop_t **) apr_push_array (fb->propchanges);
  *receiver = propchange;

  /* Let close_file() know that propchanges are waiting to be
     applied. */
  fb->prop_changed = 1;

  return SVN_NO_ERROR;
}


static svn_error_t *
close_file (void *file_baton)
{
  struct file_baton *fb = file_baton;
  apr_file_t *log_fp = NULL;
  svn_error_t *err;
  apr_status_t apr_err;
  char *revision_str = NULL;
  svn_string_t *entry_accum;

  err = svn_wc__lock (fb->dir_baton->path, 0, fb->pool);
  if (err)
    return err;

  /*
     When we reach close_file() for file `F', the following are
     true:

         - The new pristine text of F, if any, is present in
           SVN/tmp/text-base/F, and the file_baton->text_changed is
           set if necessary.

         - The new pristine props for F, if any, are present in
           the file_baton->propchanges array, and
           file_baton->prop_changed is set.

         - The SVN/entries file still reflects the old F.

         - SVN/text-base/F is the old pristine F.

         - SVN/prop-base/F is the old pristine F props.

      The goal is to update the local working copy of F to reflect
      the changes received from the repository, preserving any local
      modifications, in an interrupt-safe way.  So we first write our
      intentions to SVN/log, then run over the log file doing each
      operation in turn.  For a given operation, you can tell by
      inspection whether or not it has already been done; thus, those
      that have already been done are no-ops, and when we reach the
      end of the log file, we remove it.

      Because we must preserve local changes, the actual order of
      operations to update F is this:

         1. receive svndiff data D
         2. svnpatch SVN/text-base/F < D > SVN/tmp/text-base/F
         3. gdiff -c SVN/text-base/F SVN/tmp/text-base/F > SVN/tmp/F.blah.tmp
         4. cp SVN/tmp/text-base/F SVN/text-base/F
         5. gpatch F < SVN/tmp/F.tmpfile
              ==> possibly producing F.blah.rej

  */

  /** Write out the appropriate log entries. 
      This is safe because the adm area is locked right now. **/ 
      
  err = svn_wc__open_adm_file (&log_fp,
                               fb->dir_baton->path,
                               SVN_WC__ADM_LOG,
                               (APR_WRITE | APR_CREATE), /* not excl */
                               fb->pool);
  if (err)
    return err;

  entry_accum = svn_string_create ("", fb->pool);

  if (fb->text_changed)
    {
      enum svn_node_kind wfile_kind = svn_node_unknown;
      svn_string_t *tmp_txtb = svn_wc__text_base_path (fb->name, 1, fb->pool);
      svn_string_t *txtb     = svn_wc__text_base_path (fb->name, 0, fb->pool);
      svn_string_t *received_diff_filename;
      
      err = svn_io_check_path (fb->path, &wfile_kind, fb->pool);
      if (err)
        return err;
      
      if (wfile_kind == svn_node_file)
        {
          /* To preserve local changes dominantly over received
             changes, we record the received changes as a diff, to be
             applied over the working file.  Rejected hunks will be from
             the received changes, not the user's changes. */
          
          /* diff -c SVN/text-base/F SVN/tmp/text-base/F > SVN/tmp/F.blah.diff
           */
          
          /* kff todo: need to handle non-text formats here, and support
             other merge programs.  And quote the arguments like civilized
             programmers. */
          
          apr_proc_t diff_proc;
          apr_procattr_t *diffproc_attr;
          const char *diff_args[6];

          apr_file_t *received_diff_file;
          svn_string_t *tmp_txtb_full_path
            = svn_wc__text_base_path (fb->path, 1, fb->pool);
          svn_string_t *txtb_full_path
            = svn_wc__text_base_path (fb->path, 0, fb->pool);
          svn_string_t *tmp_loc
            = svn_wc__adm_path (fb->dir_baton->path, 1, fb->pool, 
                                fb->name->data, NULL);
          
          err = svn_io_open_unique_file (&received_diff_file,
                                         &received_diff_filename,
                                         tmp_loc,
                                         SVN_WC__DIFF_EXT,
                                         fb->pool);
          if (err)
            return err;
          
          /* Create the process attributes. */
          apr_err = apr_createprocattr_init (&diffproc_attr, fb->pool); 
          if (! APR_STATUS_IS_SUCCESS (apr_err))
            return svn_error_create 
              (apr_err, 0, NULL, fb->pool,
               "close_file: error creating diff process attributes");
          
          /* Make sure we invoke diff directly, not through a shell. */
          apr_err = apr_setprocattr_cmdtype (diffproc_attr, APR_PROGRAM);
          if (! APR_STATUS_IS_SUCCESS (apr_err))
            return svn_error_create 
              (apr_err, 0, NULL, fb->pool,
               "close_file: error setting diff process cmdtype");
          
          /* Set io style. */
          apr_err = apr_setprocattr_io (diffproc_attr, 0, 
                                        APR_CHILD_BLOCK, APR_CHILD_BLOCK);
          if (! APR_STATUS_IS_SUCCESS (apr_err))
            return svn_error_create
              (apr_err, 0, NULL, fb->pool,
               "close_file: error setting diff process io attributes");
          
          /* Tell it to send output to the diff file. */
          apr_err = apr_setprocattr_childout (diffproc_attr,
                                              received_diff_file,
                                              NULL);
          if (! APR_STATUS_IS_SUCCESS (apr_err))
            return svn_error_create 
              (apr_err, 0, NULL, fb->pool,
               "close_file: error setting diff process child output");
          
          /* Build the diff command. */
          diff_args[0] = "diff";
          diff_args[1] = "-c";
          diff_args[2] = "--";
          diff_args[3] = txtb_full_path->data;
          diff_args[4] = tmp_txtb_full_path->data;
          diff_args[5] = NULL;
          
          /* Start the diff command.  kff todo: path to diff program
             should be determined through various levels of fallback,
             of course, not hardcoded. */ 
          apr_err = apr_create_process (&diff_proc,
                                        SVN_CLIENT_DIFF,
                                        diff_args,
                                        NULL,
                                        diffproc_attr,
                                        fb->pool);
          if (! APR_STATUS_IS_SUCCESS (apr_err))
            return svn_error_createf 
              (apr_err, 0, NULL, fb->pool,
               "close_file: error starting diff process");
          
          /* Wait for the diff command to finish. */
          apr_err = apr_wait_proc (&diff_proc, APR_WAIT);
          if (APR_STATUS_IS_CHILD_NOTDONE (apr_err))
            return svn_error_createf
              (apr_err, 0, NULL, fb->pool,
               "close_file: error waiting for diff process");
        }
      
      /* Move new text base over old text base. */
      svn_xml_make_open_tag (&entry_accum,
                             fb->pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_MV,
                             SVN_WC__LOG_ATTR_NAME,
                             tmp_txtb,
                             SVN_WC__LOG_ATTR_DEST,
                             txtb,
                             NULL);
      
      if (wfile_kind == svn_node_none)
        {
          /* Copy the new base text to the working file. */
          svn_xml_make_open_tag (&entry_accum,
                                 fb->pool,
                                 svn_xml_self_closing,
                                 SVN_WC__LOG_CP,
                                 SVN_WC__LOG_ATTR_NAME,
                                 txtb,
                                 SVN_WC__LOG_ATTR_DEST,
                                 fb->name,
                                 NULL);
        }
      else if (wfile_kind == svn_node_file)
        {
          /* Patch repos changes into an existing local file. */
          svn_string_t *patch_cmd = svn_string_create (SVN_CLIENT_PATCH,
                                                       fb->pool);
          apr_file_t *reject_file = NULL;
          svn_string_t *reject_filename = NULL;
          
          /* Get the reject file ready. */
          /* kff todo: code dup with above, abstract it? */
          err = svn_io_open_unique_file (&reject_file,
                                         &reject_filename,
                                         fb->path,
                                         SVN_WC__TEXT_REJ_EXT,
                                         fb->pool);
          if (err)
            return err;
          else
            {
              apr_err = apr_close (reject_file);
              if (apr_err)
                return svn_error_createf (apr_err, 0, NULL, fb->pool,
                                          "close_file: error closing %s",
                                          reject_filename->data);
            }

          /* Paths need to be relative to the working dir that uses
             this log file, so we chop the prefix.

             kff todo: maybe this should be abstracted into
             svn_path_whatever, but it's so simple I'm inclined not
             to.  On the other hand, the +1/-1's are for slashes, and
             technically only svn_path should know such dirty details.
             On the third hand, whatever the separator char is, it's
             still likely to be one char, so the code would work even
             if it weren't slash.

             Sometimes I think I think too much.  I think.
          */ 
          reject_filename = svn_string_ncreate
            (reject_filename->data + fb->dir_baton->path->len + 1,
             reject_filename->len - fb->dir_baton->path->len - 1,
             fb->pool);

          received_diff_filename = svn_string_ncreate
            (received_diff_filename->data + fb->dir_baton->path->len + 1,
             received_diff_filename->len - fb->dir_baton->path->len - 1,
             fb->pool);

          /* Log the patch command. */
          /* kff todo: these options will have to be portablized too.
             Even if we know we're doing a plaintext patch, not all
             patch programs support these args. */
          svn_xml_make_open_tag (&entry_accum,
                                 fb->pool,
                                 svn_xml_self_closing,
                                 SVN_WC__LOG_RUN_CMD,
                                 SVN_WC__LOG_ATTR_NAME,
                                 patch_cmd,
                                 SVN_WC__LOG_ATTR_ARG_1,
                                 svn_string_create ("-r", fb->pool),
                                 SVN_WC__LOG_ATTR_ARG_2,
                                 reject_filename,
                                 SVN_WC__LOG_ATTR_ARG_3,
                                 svn_string_create ("--", fb->pool),
                                 SVN_WC__LOG_ATTR_ARG_4,
                                 fb->name,
                                 SVN_WC__LOG_ATTR_INFILE,
                                 received_diff_filename,
                                 NULL);

          /* Remove the diff file that patch will have used. */
          svn_xml_make_open_tag (&entry_accum,
                                 fb->pool,
                                 svn_xml_self_closing,
                                 SVN_WC__LOG_RM,
                                 SVN_WC__LOG_ATTR_NAME,
                                 received_diff_filename,
                                 NULL);

          /* Remove the reject file that patch will have used, IFF the
           reject file is empty (zero bytes) -- implying that there
           was no conflict.  If the reject file is nonzero, then mark
           the entry as conflicted!  */
          svn_xml_make_open_tag (&entry_accum,
                                 fb->pool,
                                 svn_xml_self_closing,
                                 SVN_WC__LOG_DETECT_CONFLICT,
                                 SVN_WC__LOG_ATTR_NAME,
                                 fb->name,
                                 SVN_WC_ENTRY_ATTR_REJFILE,
                                 reject_filename,
                                 NULL);

        }
      else
        {
          /* kff todo: handle edge cases */
        }
    }

  /* MERGE ANY PROPERTY CHANGES, if they exist... */
  if (fb->prop_changed)
    {
      err = svn_wc__do_property_merge (fb->dir_baton->path, fb->name,
                                       fb->propchanges, fb->pool,
                                       &entry_accum);
      if (err) 
        return
          svn_error_quick_wrap (err, "close_file: couldn't do prop merge.");
    }

  /* Set revision. */
  revision_str = apr_psprintf (fb->pool,
                              "%d",
                              fb->dir_baton->edit_baton->target_revision);

  /* Write log entry which will bump the revision number:  */
  svn_xml_make_open_tag (&entry_accum,
                         fb->pool,
                         svn_xml_self_closing,
                         SVN_WC__LOG_MODIFY_ENTRY,
                         SVN_WC__LOG_ATTR_NAME,
                         fb->name,
                         SVN_WC_ENTRY_ATTR_REVISION,
                         svn_string_create (revision_str, fb->pool),
                         NULL);

  if (fb->text_changed)
    {
      svn_boolean_t text_modified;

      /* Is the working file's text locally modified? */
      err = svn_wc_text_modified_p (&text_modified,
                                    fb->path,
                                    fb->pool);
      if (err) return err;

      /* Log entry which sets a new textual timestamp, but only if
         there are no local changes to the text. */
      if (! text_modified)
        svn_xml_make_open_tag (&entry_accum,
                               fb->pool,
                               svn_xml_self_closing,
                               SVN_WC__LOG_MODIFY_ENTRY,
                               SVN_WC__LOG_ATTR_NAME,
                               fb->name,
                               SVN_WC_ENTRY_ATTR_TEXT_TIME,
                               /* use wfile time */
                               svn_string_create (SVN_WC_TIMESTAMP_WC,
                                                  fb->pool),
                               NULL);
    }

  if (fb->prop_changed)
    {
      svn_boolean_t prop_modified;

      /* Are the working file's props locally modified? */
      err = svn_wc_props_modified_p (&prop_modified,
                                     fb->path,
                                     fb->pool);
      if (err) return err;

      /* Log entry which sets a new property timestamp, but only if
         there are no local changes to the props. */
      if (! prop_modified)
        svn_xml_make_open_tag (&entry_accum,
                               fb->pool,
                               svn_xml_self_closing,
                               SVN_WC__LOG_MODIFY_ENTRY,
                               SVN_WC__LOG_ATTR_NAME,
                               fb->name,
                               SVN_WC_ENTRY_ATTR_PROP_TIME,
                               /* use wfile time */
                               svn_string_create (SVN_WC_TIMESTAMP_WC,
                                                  fb->pool),
                               NULL);
    }

  /* Write our accumulation of log entries into a log file */
  apr_err = apr_full_write (log_fp, entry_accum->data, entry_accum->len, NULL);
  if (apr_err)
    {
      apr_close (log_fp);
      return svn_error_createf (apr_err, 0, NULL, fb->pool,
                                "close_file: error writing %s's log file",
                                fb->path->data);
    }

  /* The log is ready to run, close it. */
  err = svn_wc__close_adm_file (log_fp,
                                fb->dir_baton->path,
                                SVN_WC__ADM_LOG,
                                1, /* sync */
                                fb->pool);
  if (err)
    return err;

  /* Run the log. */
  err = svn_wc__run_log (fb->dir_baton->path, fb->pool);
  if (err)
    return err;

  /* Unlock, we're done with this whole file-update. */
  err = svn_wc__unlock (fb->dir_baton->path, fb->pool);
  if (err)
    return err;

  /* Tell the directory it has one less thing to worry about. */
  err = free_file_baton (fb);
  if (err)
    return err;

  return SVN_NO_ERROR;
}


static svn_error_t *
close_edit (void *edit_baton)
{
  struct edit_baton *eb = edit_baton;

  /* The edit is over, free its pool. */
  apr_destroy_pool (eb->pool);

  /* kff todo:  Wow.  Is there _anything_ else that needs to be done? */

  return SVN_NO_ERROR;
}



/*** Returning editors. ***/

static const svn_delta_edit_fns_t tree_editor =
{
  replace_root,
  delete_item,
  add_directory,
  replace_directory,
  change_dir_prop,
  close_directory,
  add_file,
  replace_file,
  apply_textdelta,
  change_file_prop,
  close_file,
  close_edit
};


/* Helper for the two public editor-supplying functions. */
static svn_error_t *
make_editor (svn_string_t *dest,
             svn_revnum_t target_revision,
             svn_boolean_t is_checkout,
             svn_string_t *repos,
             svn_string_t *ancestor_path,
             const svn_delta_edit_fns_t **editor,
             void **edit_baton,
             apr_pool_t *pool)
{
  struct edit_baton *eb;
  apr_pool_t *subpool = svn_pool_create (pool);

  /* Else nothing in the way, so continue. */

  *editor = &tree_editor;

  if (is_checkout)
    {
      assert (ancestor_path != NULL);
      assert (repos != NULL);
    }

  eb = apr_palloc (subpool, sizeof (*eb));
  eb->dest_dir       = dest;
  eb->pool           = subpool;
  eb->is_checkout    = is_checkout;
  eb->ancestor_path  = ancestor_path;
  eb->repository     = repos;
  eb->target_revision = target_revision;

  *edit_baton = eb;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_get_update_editor (svn_string_t *dest,
                          svn_revnum_t target_revision,
                          const svn_delta_edit_fns_t **editor,
                          void **edit_baton,
                          apr_pool_t *pool)
{
  return
    make_editor (dest, target_revision,
                 0, NULL, NULL,
                 editor, edit_baton, pool);
}


svn_error_t *
svn_wc_get_checkout_editor (svn_string_t *dest,
                            svn_string_t *repos,
                            svn_string_t *ancestor_path,
                            svn_revnum_t target_revision,
                            const svn_delta_edit_fns_t **editor,
                            void **edit_baton,
                            apr_pool_t *pool)
{
  return make_editor (dest, target_revision,
                      1, repos, ancestor_path,
                      editor, edit_baton, pool);
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
