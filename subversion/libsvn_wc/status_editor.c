/*
 * status_editor.c :  editor that implement a 'dry run' update
 *                    and tweaks status structures accordingly.
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
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



#include <apr_pools.h>
#include <apr_hash.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_delta.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_error.h"
#include "svn_io.h"
#include "svn_hash.h"
#include "svn_wc.h"
#include "svn_private_config.h"

#include "wc.h"



struct edit_baton
{
  /* For status, the "destination" of the edit  and whether to honor
     any paths that are 'below'.  */
  svn_stringbuf_t *path;
  svn_boolean_t descend;

  /* The youngest revision in the repository.  This is a reference
     because this editor returns youngest rev to the driver directly,
     as well as in each statushash entry. */
  svn_revnum_t *youngest_revision;

  /* The hash of status structures we're editing. */
  apr_hash_t *statushash;

  /* The pool that will be used to add new structures to the hash,
     presumably the same one it's already been using. */
  apr_pool_t *hashpool;

  /* The pool which the editor uses for the whole tree-walk.*/
  apr_pool_t *pool;
};





/*** Helper ***/


/* Look up the key PATH in EDIT_BATON->STATUSHASH.

   If the value doesn't yet exist, create a new status struct using
   EDIT_BATON->HASHPOOL.

   Set the status structure's "network" fields to REPOS_TEXT_STATUS,
   REPOS_PROP_STATUS.  If either of these fields is 0, it will be
   ignored.  */
static svn_error_t *
tweak_statushash (void *edit_baton,
                  const char *path,
                  enum svn_wc_status_kind repos_text_status,
                  enum svn_wc_status_kind repos_prop_status)
{
  svn_wc_status_t *statstruct;
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  apr_hash_t *statushash = eb->statushash;
  apr_pool_t *pool = eb->hashpool;

  /* If you want temporary debugging info... */
  /* {
     apr_hash_index_t *hi;
     char buf[200];
     
     printf("---Tweaking statushash:  editing path `%s'\n", path);
     
     for (hi = apr_hash_first (pool, statushash); 
     hi; 
     hi = apr_hash_next (hi))
     {
     const void *key;
     void *val;
     apr_size_t klen;
         
     apr_hash_this (hi, &key, &klen, &val);
     snprintf(buf, klen+1, (const char *)key);
     printf("    %s\n", buf);
     }
     fflush(stdout);
     }
  */
  
  /* Is PATH already a hash-key? */
  statstruct = (svn_wc_status_t *) apr_hash_get (statushash, path,
                                                 APR_HASH_KEY_STRING);
  /* If not, make it so. */
  if (! statstruct)
    {
      svn_stringbuf_t *pathkey = svn_stringbuf_create (path, pool);
        
      /* Use the public API to get a statstruct: */
      SVN_ERR (svn_wc_status (&statstruct, pathkey, pool));

      /* Put the path/struct into the hash. */
      apr_hash_set (statushash, pathkey->data, pathkey->len, statstruct);
    }

  /* Tweak the structure's repos fields. */
  if (repos_text_status)
    statstruct->repos_text_status = repos_text_status;
  if (repos_prop_status)
    statstruct->repos_prop_status = repos_prop_status;
  
  return SVN_NO_ERROR;
}




/*** batons ***/

struct dir_baton
{
  /* The path to this directory. */
  svn_stringbuf_t *path;

  /* Basename of this directory. */
  svn_stringbuf_t *name;

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

  /* 'svn status' shouldn't print status lines for things that are
     added;  we're only interest in asking if objects that the user
     *already* has are up-to-date or not.  Thus if this flag is set,
     the next two will be ignored.  :-)  */
  svn_boolean_t added;

  /* Gets set iff there's a change to this directory's properties, to
     guide us when syncing adm files later. */
  svn_boolean_t prop_changed;

  /* This means (in terms of 'svn status') that some child was deleted
     or added to the directory */
  svn_boolean_t text_changed;

  /* The pool in which this baton itself is allocated. */
  apr_pool_t *pool;
};



/* Create a new dir_baton for subdir NAME in PARENT_PATH with
 * EDIT_BATON, using a new subpool of POOL.
 *
 * The new baton's ref_count is 1.
 *
 * NAME and PARENT_BATON can be null, meaning this is the root baton.
 */
static struct dir_baton *
make_dir_baton (svn_stringbuf_t *name,
                struct edit_baton *edit_baton,
                struct dir_baton *parent_baton,
                apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;
  apr_pool_t *subpool = svn_pool_create (pool);
  struct dir_baton *d = apr_pcalloc (subpool, sizeof (*d));
  svn_stringbuf_t *path;

  if (parent_baton)
    {
      /* I, the baton-in-creation, have a parent, so base my path on
         that of my parent. */
      path = svn_stringbuf_dup (parent_baton->path, subpool);
    }
  else
    {
      /* I am Adam.  All my base are belong to me. */
      path = svn_stringbuf_dup (eb->path, subpool);
    }

  if (name)
    {
      d->name = svn_stringbuf_dup (name, subpool);
      svn_path_add_component (path, name, svn_path_local_style);
    }

  d->path         = path;
  d->edit_baton   = edit_baton;
  d->parent_baton = parent_baton;
  d->ref_count    = 1;
  d->pool         = subpool;

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

  /* After we destroy DIR_BATON->pool, DIR_BATON itself is lost. */
  svn_pool_destroy (dir_baton->pool);

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
  const svn_stringbuf_t *name;

  /* Path to this file, either abs or relative to the change-root. */
  svn_stringbuf_t *path;

  /* 'svn status' shouldn't print status lines for things that are
     added;  we're only interest in asking if objects that the user
     *already* has are up-to-date or not.  Thus if this flag is set,
     the next two will be ignored.  :-)  */
  svn_boolean_t added;

  /* This gets set if the file underwent a text change, which guides
     the code that syncs up the adm dir and working copy. */
  svn_boolean_t text_changed;

  /* This gets set if the file underwent a prop change, which guides
     the code that syncs up the adm dir and working copy. */
  svn_boolean_t prop_changed;

};


/* Make a file baton, using a new subpool of PARENT_DIR_BATON's pool.
   NAME is just one component, not a path. */
static struct file_baton *
make_file_baton (struct dir_baton *parent_dir_baton, svn_stringbuf_t *name)
{
  apr_pool_t *subpool = svn_pool_create (parent_dir_baton->pool);
  struct file_baton *f = apr_pcalloc (subpool, sizeof (*f));
  svn_stringbuf_t *path = svn_stringbuf_dup (parent_dir_baton->path,
                                       subpool);

  svn_path_add_component (path,
                          name,
                          svn_path_local_style);

  f->pool       = subpool;
  f->dir_baton  = parent_dir_baton;
  f->name       = name;
  f->path       = path;

  parent_dir_baton->ref_count++;

  return f;
}


static svn_error_t *
free_file_baton (struct file_baton *fb)
{
  struct dir_baton *parent = fb->dir_baton;
  svn_pool_destroy (fb->pool);
  return decrement_ref_count (parent);
}



/*** Helpers for the editor callbacks. ***/

static svn_error_t *
window_handler (svn_txdelta_window_t *window, void *baton)
{
  /* This is a deliberate no-op.  In theory, this function should only
     receive a single empty window from svn_repos_dir_delta. */
  return SVN_NO_ERROR;
}

/*----------------------------------------------------------------------*/

/*** The callbacks we'll plug into an svn_delta_edit_fns_t structure. ***/

static svn_error_t *
set_target_revision (void *edit_baton, svn_revnum_t target_revision)
{
  struct edit_baton *eb = edit_baton;

  *(eb->youngest_revision) = target_revision;

  return SVN_NO_ERROR;
}


static svn_error_t *
open_root (void *edit_baton,
           svn_revnum_t base_revision, /* This is ignored in co */
           void **dir_baton)
{
  struct edit_baton *eb = edit_baton;
  struct dir_baton *d;

  *dir_baton = d = make_dir_baton (NULL, eb, NULL, eb->pool);

  return SVN_NO_ERROR;
}


static svn_error_t *
delete_entry (svn_stringbuf_t *name, svn_revnum_t revision, void *parent_baton)
{
  struct dir_baton *db = parent_baton;
  apr_hash_t *entries;

  /* Note:  when something is deleted, it's okay to tweak the
     statushash immediately.  No need to wait until close_file or
     close_dir, because there's no risk of having to honor the 'added'
     flag.  We already know this item exists in the working copy. */

  /* Mark the deleted object as such. */
  svn_stringbuf_t *deleted_path = svn_stringbuf_dup (db->path, db->pool);
  svn_path_add_component (deleted_path, name, svn_path_local_style);

  /* Read the parent's entries file.  If the deleted thing is not
     versioned in this working copy, it was probably deleted via this
     working copy.  No need to report such a thing. */
  SVN_ERR (svn_wc_entries_read (&entries, db->path, db->pool));
  if (apr_hash_get (entries, name->data, name->len))
    {
      SVN_ERR (tweak_statushash (db->edit_baton,
                                 deleted_path->data,
                                 svn_wc_status_deleted, 0));

    }

  /* Mark the parent dir regardless -- it lost an entry. */
  SVN_ERR (tweak_statushash (db->edit_baton,
                             db->path->data,
                             svn_wc_status_modified, 0));

  return SVN_NO_ERROR;
}


static svn_error_t *
add_directory (svn_stringbuf_t *name,
               void *parent_baton,
               svn_stringbuf_t *copyfrom_path,
               svn_revnum_t copyfrom_revision,
               void **child_baton)
{
  struct dir_baton *parent_dir_baton = parent_baton;

  /* Make a new dir baton for the new directory. */
  struct dir_baton *this_dir_baton
    = make_dir_baton (name,
                      parent_dir_baton->edit_baton,
                      parent_dir_baton,
                      parent_dir_baton->pool);

  /* Mark the new directory as "added" */
  this_dir_baton->added = 1;

  /* Mark the parent as changed however;  it gained an entry. */
  parent_dir_baton->text_changed = 1;

  *child_baton = this_dir_baton;

  return SVN_NO_ERROR;
}


static svn_error_t *
open_directory (svn_stringbuf_t *name,
                void *parent_baton,
                svn_revnum_t base_revision,
                void **child_baton)
{
  struct dir_baton *parent_dir_baton = parent_baton;

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
                 svn_stringbuf_t *name,
                 svn_stringbuf_t *value)
{
  struct dir_baton *db = dir_baton;

  if (svn_wc_is_normal_prop (name))    
    db->prop_changed = 1;

  return SVN_NO_ERROR;
}



static svn_error_t *
close_directory (void *dir_baton)
{
  struct dir_baton *db = dir_baton;
  svn_error_t *err = NULL;

  if (db->added 
      || db->prop_changed
      || db->text_changed)
    {
      if (db->added)
        /* add the directory to the status hash */
        SVN_ERR (tweak_statushash (db->edit_baton,
                                   db->path->data,
                                   svn_wc_status_added,
                                   db->prop_changed ? svn_wc_status_added : 0));
      else
        /* mark the existing directory in the statushash */    
        SVN_ERR (tweak_statushash (db->edit_baton,
                                   db->path->data,
                                   db->text_changed ? svn_wc_status_modified : 0,
                                   db->prop_changed ? svn_wc_status_modified : 0));

    }

  /* We're truly done with this directory now.  decrement_ref_count
  will actually destroy dir_baton if the ref count reaches zero, so we
  call this LAST. */
  err = decrement_ref_count (db);
  if (err)
    return err;

  return SVN_NO_ERROR;
}



/* Common code for add_file() and open_file(). */
static svn_error_t *
add_or_open_file (svn_stringbuf_t *name,
                  void *parent_baton,
                  svn_stringbuf_t *ancestor_path,
                  svn_revnum_t ancestor_revision,
                  void **file_baton,
                  svn_boolean_t adding)  /* 0 if replacing */
{
  struct file_baton *this_file_baton
    = make_file_baton (parent_baton, name);

  if (adding)
    this_file_baton->added = 1;
    
  *file_baton = this_file_baton;

  return SVN_NO_ERROR;
}


static svn_error_t *
add_file (svn_stringbuf_t *name,
          void *parent_baton,
          svn_stringbuf_t *copyfrom_path,
          svn_revnum_t copyfrom_revision,
          void **file_baton)
{
  struct dir_baton *parent_dir_baton = parent_baton;

  /* Mark parent dir as changed */  
  parent_dir_baton->text_changed = 1;

  return add_or_open_file
    (name, parent_baton, copyfrom_path, copyfrom_revision, file_baton, 1);
}


static svn_error_t *
open_file (svn_stringbuf_t *name,
           void *parent_baton,
           svn_revnum_t base_revision,
           void **file_baton)
{
  return add_or_open_file
    (name, parent_baton, NULL, base_revision, file_baton, 0);
}


static svn_error_t *
apply_textdelta (void *file_baton, 
                 svn_txdelta_window_handler_t *handler,
                 void **handler_baton)
{
  struct file_baton *fb = file_baton;
  
  fb->text_changed = 1;

  /* Send back a no-op window handler. */
  *handler_baton = NULL;
  *handler = window_handler;

  return SVN_NO_ERROR;
}


static svn_error_t *
change_file_prop (void *file_baton,
                  svn_stringbuf_t *name,
                  svn_stringbuf_t *value)
{
  struct file_baton *fb = file_baton;

  if (svn_wc_is_normal_prop (name))    
    fb->prop_changed = 1;

  return SVN_NO_ERROR;
}


static svn_error_t *
close_file (void *file_baton)
{
  struct file_baton *fb = file_baton;

  if (fb->added 
      || fb->prop_changed
      || fb->text_changed)
    {
      if (fb->added)
        /* add file to the hash */
        SVN_ERR (tweak_statushash (fb->dir_baton->edit_baton,
                                   fb->path->data,
                                   svn_wc_status_added, 
                                   fb->prop_changed ? svn_wc_status_added : 0));
      else
        /* mark the file in the statushash */
        SVN_ERR (tweak_statushash (fb->dir_baton->edit_baton,
                                   fb->path->data,
                                   fb->text_changed ? svn_wc_status_modified : 0,
                                   fb->prop_changed ? svn_wc_status_modified : 0));
    }

  /* Tell the directory it has one less thing to worry about. */
  SVN_ERR (free_file_baton (fb));

  return SVN_NO_ERROR;
}


static svn_error_t *
close_edit (void *edit_baton)
{
  struct edit_baton *eb = edit_baton;
  apr_hash_index_t *hi;

  /* Loop through the statushash, set the REPOS_REV field in each. (We
     got the youngest revision way back in editor->set_target_revision.)  */
  for (hi = apr_hash_first (eb->pool, eb->statushash); 
       hi; 
       hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;
      apr_ssize_t klen;
      svn_wc_status_t *status;
      
      apr_hash_this (hi, &key, &klen, &val);
      status = (svn_wc_status_t *) val;
      status->repos_rev = *(eb->youngest_revision);
    }
  
  /* The edit is over, free its pool. */
  svn_pool_destroy (eb->pool);
    
  return SVN_NO_ERROR;
}



/*** Returning editors. ***/


/*** Public API ***/

svn_error_t *
svn_wc_get_status_editor (svn_delta_edit_fns_t **editor,
                          void **edit_baton,
                          svn_stringbuf_t *path,
                          svn_boolean_t descend,
                          apr_hash_t *statushash,
                          svn_revnum_t *youngest,
                          apr_pool_t *pool)
{
  struct edit_baton *eb;
  svn_stringbuf_t *anchor, *target, *tempbuf;
  apr_pool_t *subpool = svn_pool_create (pool);
  svn_delta_edit_fns_t *tree_editor = svn_delta_default_editor (pool);

  /* Construct an edit baton. */
  eb = apr_palloc (subpool, sizeof (*eb));
  eb->pool              = subpool;
  eb->hashpool          = pool;
  eb->statushash        = statushash;
  eb->descend           = descend;
  eb->youngest_revision = youngest;

  /* Anchor target analysis, to make this editor able to match
     hash-keys already in the hash.  (svn_wc_statuses is ignorant of
     anchor/target issues.) */
  SVN_ERR (svn_wc_get_actual_target (path, &anchor, &target, pool));
  tempbuf = svn_stringbuf_dup (anchor, pool);
  if (target)
    svn_path_add_component (tempbuf, target, svn_path_local_style);

  if (! svn_stringbuf_compare (path, tempbuf))
    eb->path = svn_stringbuf_create ("", pool);
  else
    eb->path = anchor;

  /* Construct an editor. */
  tree_editor->set_target_revision = set_target_revision;
  tree_editor->open_root = open_root;
  tree_editor->delete_entry = delete_entry;
  tree_editor->add_directory = add_directory;
  tree_editor->open_directory = open_directory;
  tree_editor->change_dir_prop = change_dir_prop;
  tree_editor->close_directory = close_directory;
  tree_editor->add_file = add_file;
  tree_editor->open_file = open_file;
  tree_editor->apply_textdelta = apply_textdelta;
  tree_editor->change_file_prop = change_file_prop;
  tree_editor->close_file = close_file;
  tree_editor->close_edit = close_edit;

  *edit_baton = eb;
  *editor = tree_editor;

  return SVN_NO_ERROR;
}




/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: 
 */

