/*
 * adm_crawler.c:  report local WC mods to an Editor.
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

/* ==================================================================== */


#include <string.h>

#include "apr_pools.h"
#include "apr_file_io.h"
#include "apr_hash.h"
#include "apr_fnmatch.h"

#include "wc.h"
#include "svn_types.h"
#include "svn_pools.h"
#include "svn_wc.h"
#include "svn_io.h"
#include "svn_sorts.h"
#include "svn_delta.h"

#include <assert.h>

static void add_default_ignores (apr_array_header_t *patterns)
{
  static const char *ignores[] = 
  {
    "*.o", "*.lo", "*.la", "#*#", "*.rej", "*~", ".#*",
    /* what else? */
    NULL
  };
  int i;
  
  for (i = 0; ignores[i] != NULL; i++)
    {
      const char **ent = apr_array_push(patterns);
      *ent = ignores[i];
    }

}

/* Helper routine: try to read the contents of DIRPATH/.svnignore.  If
   no such file exists, then set *PATTERNS to NULL.  Otherwise, set
   *PATTERNS to a list of patterns to match;  *PATTERNS will contain
   an array of (const char *) objects. */
static svn_error_t *
load_ignore_file (const char *dirpath,
                  apr_array_header_t *patterns,
                  apr_pool_t *pool)
{
  apr_file_t *fp;
  apr_status_t status;
  char buf[100];
  apr_size_t sz = 100;

  /* Try to load the .svnignore file. */
  svn_stringbuf_t *path = svn_stringbuf_create (dirpath, pool);
  svn_path_add_component_nts (path, SVN_WC_SVNIGNORE, svn_path_local_style);
  if (apr_file_open (&fp, path->data, APR_READ | APR_BUFFERED, 
                     APR_OS_DEFAULT, pool))
    {
      return SVN_NO_ERROR;
    }

  /* Now that it's open, read one line at a time into the array. */
  while (1)
    {
      status = svn_io_read_length_line (fp, buf, &sz);
      if (status == APR_EOF)
        break;
      else if (status)
        return svn_error_createf(status, 0, NULL, pool,
                                 "error reading %s", path->data);

      (*((const char **) apr_array_push (patterns))) = 
        apr_pstrndup (pool, buf, sz);

      sz = 100;
    }

  return SVN_NO_ERROR;
}                  



/* The values stored in `affected_targets' hashes are of this type.
 *
 * Ben: I think this is the start of a larger change, in which all
 * entries affected by the commit -- dirs and files alike -- are
 * stored in the affected_targets hash, and their entries are recorded
 * along with the baton that needs to be passed to the editor
 * callbacks.  
 * 
 * At that point, stack_object would hold a (struct target_baton *)
 * instead of an entry and an editor baton, and push_stack() would
 * take a struct (target_baton *).  The other changes follow from
 * there, etc.
 *
 * However, since directory adds/deletes are not supported, I've not
 * started storing directories in the affected_targets hash.
 */
struct target_baton
{
  svn_wc_entry_t *entry;
  void *editor_baton;

  svn_boolean_t text_modified_p;
};


/* Local "stack" objects used by the crawler to keep track of dir
   batons. */
struct stack_object
{
  svn_stringbuf_t *path;      /* A working copy directory */
  void *baton;                /* An associated dir baton, if any exists yet. */
  svn_wc_entry_t *this_dir;   /* All entry info about this directory */
  apr_pool_t *pool;

  struct stack_object *next;
  struct stack_object *previous;
};




/* Create a new stack object containing {PATH, BATON, ENTRY} and push
   it on top of STACK. */
static void
push_stack (struct stack_object **stack,
            svn_stringbuf_t *path,
            void *baton,
            svn_wc_entry_t *entry,
            apr_pool_t *pool)
{
  struct stack_object *new_top;
  apr_pool_t *my_pool;

  if (*stack == NULL)
    my_pool = svn_pool_create (pool);
  else
    my_pool = svn_pool_create ((*stack)->pool);

  /* Store path and baton in a new stack object */
  new_top = apr_pcalloc (my_pool, sizeof (*new_top));
  new_top->path = svn_stringbuf_dup (path, pool);
  new_top->baton = baton;
  new_top->this_dir = entry;
  new_top->next = NULL;
  new_top->previous = NULL;
  new_top->pool = my_pool;

  if (*stack == NULL)
    {
      /* This will be the very first object on the stack. */
      *stack = new_top;
    }
  else 
    {
      /* The stack already exists, so create links both ways, new_top
         becomes the top of the stack.  */
      (*stack)->next = new_top;
      new_top->previous = *stack;
      *stack = new_top;
    }
}


/* Remove youngest stack object from STACK. */
static void
pop_stack (struct stack_object **stack)
{
  struct stack_object *new_top;
  struct stack_object *old_top;

  old_top = *stack;
  if ((*stack)->previous)
    {
      new_top = (*stack)->previous;
      new_top->next = NULL;
      *stack = new_top;
    }
  svn_pool_destroy (old_top->pool);
}



/* Remove administrative-area locks on each path in LOCKS hash */
static svn_error_t *
remove_all_locks (apr_hash_t *locks, apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  
  for (hi = apr_hash_first (pool, locks); hi; hi = apr_hash_next (hi))
    {
      svn_error_t *err;
      const void *key;
      void *val;
      apr_size_t klen;
      svn_stringbuf_t *unlock_path;
      
      apr_hash_this (hi, &key, &klen, &val);
      unlock_path = svn_stringbuf_create ((char *)key, pool);
      
      err = svn_wc__unlock (unlock_path, pool);
      if (err) 
        {
          char *message =
            apr_psprintf (pool,
                          "remove_all_locks:  couldn't unlock %s",
                          unlock_path->data);
          return svn_error_quick_wrap (err, message);
        }          
    }

  return SVN_NO_ERROR;
}



/* Attempt to grab a lock in PATH.  If we succeed, store PATH in LOCKS
   and return success.  If we fail to grab a lock, remove all locks in
   LOCKS and return error. */
static svn_error_t *
do_lock (svn_stringbuf_t *path, apr_hash_t *locks, apr_pool_t *pool)
{
  svn_error_t *err, *err2;

  /* See if this directory is locked already.  If so, there's not
     really much to do here.  ### todo: Should we do check the actual
     working copy admin area with a call to svn_wc_locked() here? */
  if (apr_hash_get (locks, path->data, APR_HASH_KEY_STRING) != NULL)
    return SVN_NO_ERROR;

  err = svn_wc__lock (path, 0, pool);
  if (err)
    {
      /* Couldn't lock: */
      
      /* Remove _all_ previous commit locks */
      err2 = remove_all_locks (locks, pool);
      if (err2) 
        {
          /* If this also errored, put the original error inside it. */
          err2->child = err;
          return err2;
        }
      
      return err;
    }
  
  /* Lock succeeded */
  apr_hash_set (locks, path->data, APR_HASH_KEY_STRING, "(locked)");

  return SVN_NO_ERROR;
}






/* Given the path on the top of STACK, store (and return) NEWEST_BATON
   -- which allows one to edit entries there.  Fetch and store (in
   STACK) any previous directory batons necessary to create the one
   for path (..using calls from EDITOR.)  For every directory baton
   generated, lock the directory as well and store in LOCKS using
   TOP_POOL.  */
static svn_error_t *
do_dir_replaces (void **newest_baton,
                 struct stack_object *stack,
                 const svn_delta_edit_fns_t *editor,
                 void *edit_baton,
                 apr_hash_t *locks,
                 apr_pool_t *top_pool)
{
  struct stack_object *stackptr;  /* current stack object we're examining */

  /* Start at the top of the stack */
  stackptr = stack;  
  
  /* Walk down the stack until we find a non-NULL dir baton. */
  while (1)  
    {
      if (stackptr->baton != NULL) 
        /* Found an existing directory baton! */
        break;
      
      if (stackptr->previous)
        {
          /* There's a previous stack frame, so descend. */
          stackptr = stackptr->previous;
        }
      else
        {
          /* Can't descend?  We must be at stack bottom, fetch the
             root baton. */
          void *root_baton;

          SVN_ERR (editor->replace_root (edit_baton,
                                         stackptr->this_dir->revision, 
                                         &root_baton));  
          /* Store it */
          stackptr->baton = root_baton;
          break;
        }
    }

  /* Now that we're outside the while() loop, our stackptr is pointing
     to the frame with the "youngest" directory baton. */

  /* Now walk _up_ the stack, creating & storing new batons. */
  while (1)  
    {
      if (stackptr->next)
        {
          svn_stringbuf_t *dirname;
          void *dir_baton;

          /* Move up the stack */
          stackptr = stackptr->next;

          /* We only want the last component of the path; that's what
             the editor's replace_directory() expects from us. */
          dirname = svn_path_last_component (stackptr->path,
                                             svn_path_local_style, 
                                             stackptr->pool);

          /* Get a baton for this directory */
          SVN_ERR (editor->replace_directory 
                   (dirname, /* current dir */
                    stackptr->previous->baton, /* parent */
                    stackptr->this_dir->revision,
                    &dir_baton));

          /* Store it */
          stackptr->baton = dir_baton;
        }
      else 
        {
          /* Can't move up the stack anymore?  We must be at the top
             of the stack.  We're all done. */
          break;
        }
    }

  /* Return (by reference) the youngest directory baton, the one that
     goes with our youngest PATH */
  *newest_baton = stackptr->baton;

  /* Lock this youngest directory */
  SVN_ERR (do_lock 
           (svn_stringbuf_dup (stackptr->path, top_pool), locks, top_pool));
  
  return SVN_NO_ERROR;
}




/* Remove stackframes from STACK until the top points to DESIRED_PATH.
   Before stack frames are popped, call EDITOR->close_directory() on
   any non-null batons. */
static svn_error_t *
do_dir_closures (svn_stringbuf_t *desired_path,
                 struct stack_object **stack,
                 const svn_delta_edit_fns_t *editor)
{
   while (svn_path_compare_paths (desired_path, (*stack)->path,
                                  svn_path_local_style))
    {
      if ((*stack)->baton)
        SVN_ERR (editor->close_directory ((*stack)->baton));
      
      pop_stack (stack);
    }

  return SVN_NO_ERROR;
}





/* Examine both the local and text-base copies of a file FILENAME, and
   push a text-delta to EDITOR using the already-opened FILE_BATON.
   (FILENAME is presumed to be a full path ending with a filename.) */
static svn_error_t *
do_apply_textdelta (svn_stringbuf_t *filename,
                    const svn_delta_edit_fns_t *editor,
                    struct target_baton *tb,
                    apr_pool_t *pool)
{
  apr_status_t status;
  svn_txdelta_window_handler_t window_handler;
  void *window_handler_baton;
  svn_txdelta_stream_t *txdelta_stream;
  svn_txdelta_window_t *txdelta_window;
  apr_file_t *localfile = NULL;
  apr_file_t *textbasefile = NULL;
  svn_stringbuf_t *local_tmp_path;

  /* Tell the editor that we're about to apply a textdelta to the file
     baton; the editor returns to us a window consumer routine and
     baton. */
  SVN_ERR (editor->apply_textdelta (tb->editor_baton,
                                    &window_handler,
                                    &window_handler_baton));

  /* Copy the local file to the administrative temp area. */
  local_tmp_path = svn_wc__text_base_path (filename, TRUE, pool);
  SVN_ERR (svn_io_copy_file (filename, local_tmp_path, pool));

  /* Open a filehandle for tmp local file, and one for text-base if
     applicable. */
  status = apr_file_open (&localfile, local_tmp_path->data,
                          APR_READ, APR_OS_DEFAULT, pool);
  if (status)
    return svn_error_createf (status, 0, NULL, pool,
                              "do_apply_textdelta: error opening '%s'",
                              local_tmp_path->data);

  if ((! (tb->entry->schedule == svn_wc_schedule_add))
      && (! (tb->entry->schedule == svn_wc_schedule_replace)))
    {
      SVN_ERR (svn_wc__open_text_base (&textbasefile, filename, 
                                       APR_READ, pool));
    }

  /* Create a text-delta stream object that pulls data out of the two
     files. */
  svn_txdelta (&txdelta_stream,
               svn_stream_from_aprfile (textbasefile, pool),
               svn_stream_from_aprfile (localfile, pool),
               pool);
  
  /* Grab a window from the stream, "push" it at the consumer routine,
     then free it.  (When we run out of windows, TXDELTA_WINDOW will
     be set to NULL, and then still passed to window_handler(),
     thereby notifying window_handler that we're all done.)  */
  do
    {
      SVN_ERR (svn_txdelta_next_window (&txdelta_window, txdelta_stream));
      SVN_ERR ((* (window_handler)) (txdelta_window, window_handler_baton));
      svn_txdelta_free_window (txdelta_window);

    } while (txdelta_window);


  /* Free the stream */
  svn_txdelta_free (txdelta_stream);

  /* Close the two files */
  status = apr_file_close (localfile);
  if (status)
    return svn_error_create (status, 0, NULL, pool,
                             "do_apply_textdelta: error closing local file");

  if (textbasefile)
    SVN_ERR (svn_wc__close_text_base (textbasefile, filename, 0, pool));

  return SVN_NO_ERROR;
}


/* Loop over AFFECTED_TARGETS, calling do_apply_textdelta().
   AFFECTED_TARGETS, if non-empty, contains a mapping of full file
   paths to still-open file_batons.  After sending each text-delta,
   close each file_baton. */ 
static svn_error_t *
do_postfix_text_deltas (apr_hash_t *affected_targets,
                        const svn_delta_edit_fns_t *editor,
                        apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  svn_stringbuf_t *entrypath;
  struct target_baton *tb;
  const void *key;
  void *val;
  size_t keylen;

  for (hi = apr_hash_first (pool, affected_targets); 
       hi; 
       hi = apr_hash_next (hi))
    {
      apr_hash_this (hi, &key, &keylen, &val);
      tb = val;

      if (tb->text_modified_p)
        {
          entrypath = svn_stringbuf_create ((char *) key, pool);
          SVN_ERR (do_apply_textdelta (entrypath, editor, tb, pool));
          SVN_ERR (editor->close_file (tb->editor_baton));
        }
    }

  return SVN_NO_ERROR;
}



/* Given a full PATH to a particular ENTRY, apply all local property
   changes via EDITOR callbacks with the appropriate file or directory
   BATON. */
static svn_error_t *
do_prop_deltas (svn_stringbuf_t *path,
                svn_wc_entry_t *entry,
                const svn_delta_edit_fns_t *editor,
                void *baton,
                apr_pool_t *pool)
{
  int i;
  svn_stringbuf_t *prop_path, *prop_base_path, *tmp_prop_path;
  apr_array_header_t *local_propchanges;
  apr_hash_t *localprops = apr_hash_make (pool);
  apr_hash_t *baseprops = apr_hash_make (pool);
  
  /* First, get the prop_path from the original path */
  SVN_ERR (svn_wc__prop_path (&prop_path, path, 0, pool));
  
  /* Get the full path of the prop-base `pristine' file */
  SVN_ERR (svn_wc__prop_base_path (&prop_base_path, path, 0, pool));

  /* Copy the local prop file to the administrative temp area */
  SVN_ERR (svn_wc__prop_path (&tmp_prop_path, path, 1, pool));
  SVN_ERR (svn_io_copy_file (prop_path, tmp_prop_path, pool));

  /* Load all properties into hashes */
  SVN_ERR (svn_wc__load_prop_file (tmp_prop_path, localprops, pool));
  SVN_ERR (svn_wc__load_prop_file (prop_base_path, baseprops, pool));
  
  /* Get an array of local changes by comparing the hashes. */
  SVN_ERR (svn_wc__get_local_propchanges 
           (&local_propchanges, localprops, baseprops, pool));
  
  /* Apply each local change to the baton */
  for (i = 0; i < local_propchanges->nelts; i++)
    {
      svn_prop_t *change;

      change = (((svn_prop_t **)(local_propchanges)->elts)[i]);
      
      if (entry->kind == svn_node_file)
        SVN_ERR (editor->change_file_prop (baton,
                                           change->name,
                                           change->value));
      else
        SVN_ERR (editor->change_dir_prop (baton,
                                          change->name,
                                          change->value));
    }

  return SVN_NO_ERROR;
}




/* Decide if the file or dir represented by ENTRY continues to exist
   in a state of conflict.  If so, aid in the bailout of the current
   commit by unlocking all admin-area locks in LOCKS and returning an
   error.
   
   Obviously, this routine should only be called on entries who have
   the `conflicted' flag bit set.  */
static svn_error_t *
bail_if_unresolved_conflict (svn_stringbuf_t *full_path,
                             svn_wc_entry_t *entry,
                             apr_hash_t *locks,
                             apr_pool_t *pool)
{
  if (entry->conflicted)
    {
      /* We must decide if either component is "conflicted", based
         on whether reject files are mentioned and/or continue to
         exist.  Luckily, we have a function to do this.  :) */
      svn_boolean_t text_conflict_p, prop_conflict_p;
      svn_stringbuf_t *parent_dir;
      
      if (entry->kind == svn_node_file)
        {
          parent_dir = svn_stringbuf_dup (full_path, pool);
          svn_path_remove_component (parent_dir, svn_path_local_style);
        }
      else if (entry->kind == svn_node_dir)
        parent_dir = full_path;
      
      SVN_ERR (svn_wc_conflicted_p (&text_conflict_p,
                                    &prop_conflict_p,
                                    parent_dir,
                                    entry,
                                    pool));

      if ((! text_conflict_p) && (! prop_conflict_p))
        return SVN_NO_ERROR;

      else /* a tracked .rej or .prej file still exists */
        {
          svn_error_t *err;
          svn_error_t *final_err;
          
          final_err = svn_error_createf 
            (SVN_ERR_WC_FOUND_CONFLICT, 0, NULL, pool,
             "Aborting commit: '%s' remains in conflict.",
             full_path->data);
          
          err = remove_all_locks (locks, pool);
          if (err)
            final_err->child = err; /* nestle them */
          
          return final_err;
        }
    }
  
  return SVN_NO_ERROR;
}


/* Given a directory DIR under revision control with schedule
   SCHEDULE:

   - if SCHEDULE is svn_wc_schedule_delete, all children of this
     directory must have a schedule _delete.

   - else, if SCHEDULE is svn_wc_schedule_replace, all children of
     this directory must have a schedule of either _add or _delete.

   - else, this directory must not be marked for deletion, which is an
     automatic to fail this verifation!
*/
static svn_error_t *
verify_tree_deletion (svn_stringbuf_t *dir,
                      enum svn_wc_schedule_t schedule,
                      apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  svn_stringbuf_t *fullpath = svn_stringbuf_dup (dir, pool);

  if ((schedule != svn_wc_schedule_delete) 
      && (schedule != svn_wc_schedule_replace))
    {
      return svn_error_createf 
        (SVN_ERR_WC_FOUND_CONFLICT, 0, NULL, pool,
         "Aborting commit: '%s' not scheduled for deletion as expected.",
         dir->data);
    }

  /* Read the entries file for this directory. */
  SVN_ERR (svn_wc_entries_read (&entries, dir, pool));

  /* Delete each entry in the entries file. */
  for (hi = apr_hash_first (pool, entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      apr_size_t klen;
      void *val;
      svn_wc_entry_t *entry; 
      int is_this_dir;

      /* Get the next entry */
      apr_hash_this (hi, &key, &klen, &val);
      entry = (svn_wc_entry_t *) val;
      is_this_dir = strcmp (key, SVN_WC_ENTRY_THIS_DIR) == 0;

      /* If the entry's existence is `deleted', skip it. */
      if (entry->existence == svn_wc_existence_deleted)
        continue;

      /* Construct the fullpath of this entry. */
      if (! is_this_dir)
        svn_path_add_component_nts (fullpath, key, svn_path_local_style);

      /* If parent is marked for deletion only, this entry must be
         marked the same way. */
      if ((schedule == svn_wc_schedule_delete)
          && (entry->schedule != svn_wc_schedule_delete))
        {
          return svn_error_createf 
            (SVN_ERR_WC_FOUND_CONFLICT, 0, NULL, pool,
             "Aborting commit: '%s' dangling in deleted directory.",
             fullpath->data);
        }
      /* If parent is marked for both deletion and addition, this
         entry must be marked for either deletion, addition, or
         replacement. */
      if ((schedule == svn_wc_schedule_replace)
          && (! ((entry->schedule == svn_wc_schedule_delete)
                 || (entry->schedule == svn_wc_schedule_add)
                 || (entry->schedule == svn_wc_schedule_replace))))
        {
          return svn_error_createf 
            (SVN_ERR_WC_FOUND_CONFLICT, 0, NULL, pool,
             "Aborting commit: '%s' dangling in replaced directory.",
             fullpath->data);
        }

      /* Recurse on subdirectories. */
      if ((entry->kind == svn_node_dir) && (! is_this_dir))
        SVN_ERR (verify_tree_deletion (fullpath, entry->schedule, subpool));

      /* Reset FULLPATH to just hold this dir's name. */
      svn_stringbuf_set (fullpath, dir->data);

      /* Clear our per-iteration pool. */
      svn_pool_clear (subpool);
    }

  /* Destroy our per-iteration pool. */
  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}


/* Forward declaration for co-dependent recursion. */
static svn_error_t *crawl_dir (svn_stringbuf_t *path,
                               void *dir_baton,
                               const svn_delta_edit_fns_t *editor,
                               void *edit_baton,
                               svn_boolean_t adds_only,
                               struct stack_object **stack,
                               apr_hash_t *affected_targets,
                               apr_hash_t *locks,
                               apr_pool_t *top_pool);


/* Report modifications to file or directory NAME in STACK->path
   (represented by ENTRY).  NAME is NOT SVN_WC_ENTRY_THIS_DIR.
   
   Keep track of modified targets in AFFECTED_TARGETS, and of locked
   directories in LOCKS.

   All reporting is made using calls to EDITOR (using its associated
   EDIT_BATON and a computed DIR_BATON).

   If ADDS_ONLY is not FALSE, this function will only pay attention to
   files and directories schedule for addition.

   Perform all temporary allocation in STACK->pool, and any allocation
   that must outlive the reporting process in TOP_POOL. */
static svn_error_t *
report_single_mod (const char *name,
                   svn_wc_entry_t *entry,
                   struct stack_object **stack,
                   apr_hash_t *affected_targets,
                   apr_hash_t *locks,
                   const svn_delta_edit_fns_t *editor,
                   void *edit_baton,
                   void **dir_baton,
                   svn_boolean_t adds_only,
                   apr_pool_t *top_pool)
{
  svn_stringbuf_t *full_path;
  svn_stringbuf_t *entry_name;
  void *new_dir_baton = NULL;
  svn_boolean_t do_add = FALSE, do_delete = FALSE;
      
  if (! strcmp (name, SVN_WC_ENTRY_THIS_DIR))
    return SVN_NO_ERROR;

  entry_name = svn_stringbuf_create (name, (*stack)->pool);

  /* This entry gets deleted if marked for deletion or replacement,
     and if the algorithm is not in "adds only" mode. */
  if ((! adds_only)
      && ((entry->schedule == svn_wc_schedule_delete)
          || (entry->schedule == svn_wc_schedule_replace)))
    {
      do_delete = TRUE;
    }

  /* This entry gets added if marked for addition or replacement. */
  if ((entry->schedule == svn_wc_schedule_add)
      || (entry->schedule == svn_wc_schedule_replace))
    {
      do_add = TRUE;
    }

  /* If the entry's existence is `deleted' and it's still scheduled
     for addition, do -both- actions, to insure an accurate repos
     transaction. */
  if ((entry->schedule == svn_wc_schedule_add)
      && (entry->existence == svn_wc_existence_deleted))
    {
      do_delete = TRUE;
      do_add = TRUE;
    }

  /* Construct a full path to the current entry */
  full_path = svn_stringbuf_dup ((*stack)->path, (*stack)->pool);
  if (entry_name != NULL)
    svn_path_add_component (full_path, entry_name,
                            svn_path_local_style);

  /* Preemptive strike:  if the current entry is a file in a state
     of conflict that has NOT yet been resolved, we abort the
     entire commit.  */
  SVN_ERR (bail_if_unresolved_conflict (full_path, entry, locks, 
                                        (*stack)->pool));


  /* Here's a guide to the very long logic that extends below.  For
   * each entry in the current dir (STACK->path), the examination
   * looks like this:
   *
   *   if (deleted)...
   *   if (added)...
   *   else if (local mods)...
   *   if (dir)
   *      recurse() */

      
  /* DELETION CHECK */
  if (do_delete)
    {
      svn_stringbuf_t *longpath;
      struct target_baton *tb;

      /* Do what's necessary to get a baton for current directory */
      if (! *dir_baton)
        SVN_ERR (do_dir_replaces (dir_baton,
                                  *stack, editor, edit_baton,
                                  locks, top_pool));
      
      /* If this entry is a directory, we do a sanity check and make
         sure that all the directory's children are also marked for
         deletion.  If not, we're in a screwy state. */
      if (entry->kind == svn_node_dir)
        SVN_ERR (verify_tree_deletion (full_path, entry->schedule, 
                                       (*stack)->pool));

      /* Delete the entry */
      SVN_ERR (editor->delete_entry (entry_name, *dir_baton));
          
      /* Remember that it was affected. */
      tb = apr_pcalloc (top_pool, sizeof (*tb));            
      tb->entry = svn_wc__entry_dup (entry, top_pool);
      longpath = svn_stringbuf_dup (full_path, top_pool);
      apr_hash_set (affected_targets, longpath->data, longpath->len, tb);
    }  
  /* END DELETION CHECK */
  

  /* ADDITION CHECK */
  if (do_add)
    {
      /* Create an affected-target object */
      svn_stringbuf_t *longpath;
      struct target_baton *tb;
      svn_boolean_t prop_modified_p;        

      tb = apr_pcalloc (top_pool, sizeof (*tb));
      tb->entry = svn_wc__entry_dup (entry, top_pool);          
      
      /* Do what's necesary to get a baton for current directory */
      if (! *dir_baton)
        SVN_ERR (do_dir_replaces (dir_baton,
                                  *stack, editor, edit_baton,
                                  locks, top_pool));
      
      /* Adding a new directory: */
      if (entry->kind == svn_node_dir)
        {             
          svn_stringbuf_t *copyfrom_URL = NULL;
          svn_wc_entry_t *subdir_entry;
          
          /* A directory's interesting information is stored in
             its own THIS_DIR entry, so read that to get the real
             data for this directory. */
          SVN_ERR (svn_wc_entry (&subdir_entry, full_path, (*stack)->pool));
          
          /* If the directory is completely new, the wc records
             its pre-committed revision as "0", even though it may
             have a "default" URL listed.  But the delta.h
             docstring for add_directory() says that the copyfrom
             args must be either both valid or both invalid. */
          if (subdir_entry->revision > 0)
            copyfrom_URL = subdir_entry->ancestor;
          
          /* Add the new directory, getting a new dir baton.  */
          SVN_ERR (editor->add_directory (entry_name,
                                          *dir_baton,
                                          copyfrom_URL,
                                          subdir_entry->revision,
                                          &new_dir_baton));
        }
      
      /* Adding a new file: */
      else if (entry->kind == svn_node_file)
        {
          /* Add a new file, getting a file baton */
          SVN_ERR (editor->add_file (entry_name,
                                     *dir_baton,
                                     entry->ancestor,
                                     entry->revision,
                                     &(tb->editor_baton)));
          
          /* This might be a *newly* added file, in which case the
             revision is 0 or invalid; assume that the contents need
             to be sent. */
          if ((entry->revision == 0) 
              || (! SVN_IS_VALID_REVNUM (entry->revision)))
            {
              tb->text_modified_p = TRUE;
            }
          else
            {
              /* This file might be added with history; in this case,
                 we only *might* need to send contents.  Do a real
                 local-mod check on it. */
              SVN_ERR (svn_wc_text_modified_p (&(tb->text_modified_p),
                                               full_path, (*stack)->pool));
            }

          /* Check for local property changes to send */
          SVN_ERR (svn_wc_props_modified_p (&prop_modified_p, full_path, 
                                            (*stack)->pool));

          /* Send propchanges to the editor. */
          if (prop_modified_p)
            SVN_ERR (do_prop_deltas (full_path, entry, editor, 
                                     tb->editor_baton, (*stack)->pool));
        }
      
      /* Store the (added) affected-target for safe keeping (possibly
         to be used later for postfix text-deltas) */
      longpath = svn_stringbuf_dup (full_path, top_pool);
      apr_hash_set (affected_targets, longpath->data, longpath->len, tb);
    } 
  /* END ADDITION CHECK */
  

  /* LOCAL MOD CHECK */
  else if (! adds_only)
    {
      svn_boolean_t text_modified_p, prop_modified_p;
          
      /* Is text modified? */
      SVN_ERR (svn_wc_text_modified_p (&text_modified_p, full_path, 
                                       (*stack)->pool));
          
      /* Only check for local propchanges if we're looking at a file,
         or if we're looking at SVN_WC_ENTRY_THIS_DIR.  Otherwise,
         each directory will end up being checked twice! */
      if (entry->kind == svn_node_dir)
        prop_modified_p = FALSE;
      else
        SVN_ERR (svn_wc_props_modified_p (&prop_modified_p, full_path, 
                                          (*stack)->pool));
      
      if (text_modified_p || prop_modified_p)
        {
          svn_stringbuf_t *longpath;
          struct target_baton *tb;
          
          /* There was a local change.  Build an affected-target
             object in the top-most pool. */
          tb = apr_pcalloc (top_pool, sizeof (*tb));
          tb->entry = svn_wc__entry_dup (entry, top_pool);
          tb->text_modified_p = text_modified_p;
          
          /* Build the full path to this entry, also from the top-pool. */
          longpath = svn_stringbuf_dup (full_path, top_pool);
          
          /* Do what's necesary to get a baton for current directory */
          if (! *dir_baton)
            SVN_ERR (do_dir_replaces (dir_baton,
                                      *stack, editor, edit_baton,
                                      locks, top_pool));
          
          /* Replace a file's text, getting a new file baton */
          if (entry->kind == svn_node_file)
            SVN_ERR (editor->replace_file (entry_name,
                                           *dir_baton,
                                           entry->revision,
                                           &(tb->editor_baton)));
          
          if (prop_modified_p)
            {
              void *baton = (entry->kind == svn_node_file) ?
                tb->editor_baton : *dir_baton;
              
              /* Send propchanges to editor. */
              SVN_ERR (do_prop_deltas (longpath, entry, editor, baton, 
                                       (*stack)->pool));
                  
              /* Very important: if there are *only* propchanges, but
                 not textual ones, close the file here and now.
                 (Otherwise the file will be closed after sending
                 postfix text-deltas.)*/
              if ((entry->kind == svn_node_file) && (! text_modified_p))
                SVN_ERR (editor->close_file (tb->editor_baton));
            }
          
          /* Store the affected-target for safe keeping (possibly to
             be used later for postfix text-deltas) */
          apr_hash_set (affected_targets, longpath->data, longpath->len, tb);
        }
    }  
  /* END LOCAL MOD CHECK */
  

  /* Finally, decide whether or not to recurse.  Recurse only on
     directories that are not scheduled for deletion (add and replace
     are okay). */
  if ((entry->kind == svn_node_dir) 
      && (entry->schedule != svn_wc_schedule_delete))
    {
      /* Recurse, using new_dir_baton, which will most often be NULL
         (unless the entry is a newly added directory.)  Why NULL?
         Because that will later force a call to do_dir_replaces() and
         get the _correct_ dir baton for the child directory. */
      SVN_ERR (crawl_dir (full_path, 
                          new_dir_baton, 
                          editor, 
                          edit_baton, 
                          adds_only,
                          stack,
                          affected_targets, 
                          locks, 
                          top_pool));
    }

  return SVN_NO_ERROR;
}




/* A recursive working-copy "crawler", used to drive commits.

   Enter directory PATH and examine its entries for changes that need
   to be reported to EDITOR (using its associated EDIT_BATON and a
   calculated DIR_BATON).

   The DIR_BATON argument holds the current baton used to commit
   changes from PATH.  It may be NULL.  If it is NULL and a local
   change is discovered, then it (and all parent batons) will be
   automatically generated by do_dir_replaces(). 

   Open file-batons will be stored in AFFECTED_TARGETS using the
   never-changing top-level pool TOP_POOL (for submitting postfix
   text-deltas later.)  Any working copy dirs that are locked are
   appended to LOCKS.

   STACK should begin either as NULL, or pointing at the parent of
   PATH.  Stackframes are automatically pushed/popped as the crawl
   proceeds.  When this function returns, the top of stack will be
   exactly where it was. */
static svn_error_t *
crawl_dir (svn_stringbuf_t *path,
           void *dir_baton,
           const svn_delta_edit_fns_t *editor,
           void *edit_baton,
           svn_boolean_t adds_only,
           struct stack_object **stack,
           apr_hash_t *affected_targets,
           apr_hash_t *locks,
           apr_pool_t *top_pool)
{
  apr_hash_t *entries;            /* all entries in PATH */
  apr_hash_index_t *entry_index;  /* holds loop-state */
  svn_wc_entry_t *this_dir_entry; /* represents current working dir */
  apr_pool_t *subpool;            /* per-recursion pool */
  svn_boolean_t prop_modified_p;

  /* Create the per-recusion subpool. */
  subpool = svn_pool_create (top_pool);

  /* Retrieve _all_ the entries in this subdir into subpool. */
  SVN_ERR (svn_wc_entries_read (&entries, path, subpool));

  /* Grab the entry representing "." */
  this_dir_entry = apr_hash_get (entries, SVN_WC_ENTRY_THIS_DIR, 
                                 APR_HASH_KEY_STRING);
  if (! this_dir_entry)
    return svn_error_createf 
      (SVN_ERR_WC_ENTRY_NOT_FOUND, 0, NULL, top_pool,
       "Can't find `.' entry in %s", path->data);

  /* If the `.' entry is marked with ADD, then we *only* want to
     notice child entries that are also added.  It makes no sense to
     look for deletes or local mods in an added directory. */
  if ((this_dir_entry->schedule == svn_wc_schedule_add)
      || (this_dir_entry->schedule == svn_wc_schedule_replace))
    adds_only = TRUE;

  /* Push the current {path, baton, this_dir} to the top of the stack */
  push_stack (stack, path, dir_baton, this_dir_entry, top_pool);

  /* Take care of any property changes this directory might have
     pending. */
  SVN_ERR (svn_wc_props_modified_p (&prop_modified_p, path, 
                                    (*stack)->pool));

  if (prop_modified_p)
    {
      /* Perform the necessary steps to ensure a dir_baton for this
         directory. */
      if (! dir_baton)
        SVN_ERR (do_dir_replaces (&dir_baton,
                                  *stack, editor, edit_baton,
                                  locks, top_pool));
        
      /* Send propchanges to editor. */
      SVN_ERR (do_prop_deltas (path, this_dir_entry, editor, dir_baton, 
                               (*stack)->pool));
    }

  /* Loop over each entry */
  for (entry_index = apr_hash_first (subpool, entries); entry_index;
       entry_index = apr_hash_next (entry_index))
    {
      const void *key;
      const char *keystring;
      apr_size_t klen;
      void *val;
      svn_wc_entry_t *current_entry; 
      
      /* Get the next entry name (and structure) from the hash */
      apr_hash_this (entry_index, &key, &klen, &val);
      keystring = (const char *) key;

      /* Skip "this dir" */
      if (! strcmp (keystring, SVN_WC_ENTRY_THIS_DIR))
        continue;

      /* Get the entry for this file or directory. */
      current_entry = (svn_wc_entry_t *) val;

      /* If the entry's existence is `deleted', skip it. */
      if ((current_entry->existence == svn_wc_existence_deleted)
          && (current_entry->schedule != svn_wc_schedule_add))
        continue;
      
      /* Report mods for a single entry. */
      SVN_ERR (report_single_mod (keystring,
                                  current_entry,
                                  stack,
                                  affected_targets,
                                  locks,
                                  editor,
                                  edit_baton,
                                  &dir_baton,
                                  adds_only,
                                  top_pool));
      
    }

  /* The presence of a baton in this stack frame means that at some
     point, something was committed in this directory, and means we
     must close that dir baton. */
  if ((*stack)->baton)
    SVN_ERR (editor->close_directory ((*stack)->baton));
  
  /* If stack has no previous pointer, then we'd be removing the base
     stackframe.  We don't want to do this, however;
     svn_wc_crawl_local_mods() needs to examine it to determine if any
     changes were ever made at all. */
  if ((*stack)->previous)
    pop_stack (stack);

  /* Free all memory used when processing this subdir. */
  svn_pool_destroy (subpool);
  
  return SVN_NO_ERROR;
}



/* The main logic of svn_wc_crawl_local_mods(), which is not much more
   than a public wrapper for this routine.  See its docstring.

   The differences between this routine and the public wrapper:

      - assumes that CONDENSED_TARGETS has been sorted (critical!)
      - takes an initialized LOCKED_DIRS hash for storing locked wc dirs.
            
*/
static svn_error_t *
svn_wc__crawl_local_mods (svn_stringbuf_t *parent_dir,
                          apr_array_header_t *condensed_targets,
                          const svn_delta_edit_fns_t *editor,
                          void *edit_baton,
                          apr_hash_t *locked_dirs,
                          apr_pool_t *pool)
{
  svn_error_t *err;
  void *dir_baton = NULL;

  /* A stack that will store all paths and dir_batons as we drive the
     editor depth-first. */
  struct stack_object *stack = NULL;

  /* All the locally modified files which are waiting to be sent as
     postfix-textdeltas. */
  apr_hash_t *affected_targets = apr_hash_make (pool);

  /* No targets at all?  This means we are committing the entries in a
     single directory. */
  if (condensed_targets->nelts == 0)
    {
      /* Do a single crawl from parent_dir, that's it.  Parent_dir
         will be automatically pushed to the empty stack, but not
         removed.  This way we can examine the frame to see if there's
         a root_dir_baton, and thus whether we need to call
         close_edit(). */
      err = crawl_dir (parent_dir,
                       NULL,
                       editor, 
                       edit_baton,
                       FALSE,
                       &stack, 
                       affected_targets, 
                       locked_dirs,
                       pool);

      if (err)        
        return svn_error_quick_wrap 
          (err, "commit failed: while sending tree-delta to repos.");
    }

  /* This is the "multi-arg" commit processing branch.  That's not to
     say that there is necessarily more than one commit target, but
     whatever..." */
  else 
    {
      svn_wc_entry_t *parent_entry, *tgt_entry;
      int i;

      /* To begin, put the grandaddy parent_dir at the base of the stack. */
      SVN_ERR (svn_wc_entry (&parent_entry, parent_dir, pool));
      push_stack (&stack, parent_dir, NULL, parent_entry, pool);

      /* For each target in our CONDENSED_TARGETS list (which are
         given as paths relative to the PARENT_DIR 'grandaddy
         directory'), we pop or push stackframes until the stack is
         pointing to the immediate parent of the target.  From there,
         we can crawl the target for mods. */
      for (i = 0; i < condensed_targets->nelts; i++)
        {
          svn_stringbuf_t *ptarget;
          svn_stringbuf_t *remainder;
          svn_stringbuf_t *target, *subparent;
          svn_stringbuf_t *tgt_name =
            (((svn_stringbuf_t **) condensed_targets->elts)[i]);

          /* Get the full path of the target. */
          target = svn_stringbuf_dup (parent_dir, pool);
          svn_path_add_component (target, tgt_name, svn_path_local_style);
          
          /* Examine top of stack and target, and get a nearer common
             'subparent'. */

          subparent = svn_path_get_longest_ancestor 
            (target, stack->path, svn_path_local_style, pool);
          
          /* If the current stack path is NOT equal to the subparent,
             it must logically be a child of the subparent.  So... */
          if (svn_path_compare_paths (stack->path, subparent,
                                      svn_path_local_style))
            {
              /* ...close directories and remove stackframes until the
                 stack reaches the common parent. */
              err = do_dir_closures (subparent, &stack, editor);         
              if (err)
                return svn_error_quick_wrap 
                  (err, "commit failed: error traversing working copy.");

              /* Reset the dir_baton to NULL; it is of no use to our
                 target (which is not a sibling, or a child of a
                 sibling, to any previous targets we may have
                 processed. */
              dir_baton = NULL;
            }

          /* Push new stackframes to get down to the immediate parent
             of the target PTARGET, which must also be a child of the
             subparent. */
          svn_path_split (target, &ptarget, NULL,
                          svn_path_local_style, pool);
          remainder = svn_path_is_child (stack->path, ptarget, 
                                         svn_path_local_style,
                                         pool);
          
          /* If PTARGET is below the current stackframe, we have to
             push a new stack frame for each directory level between
             them. */
          if (remainder)  
            {
              apr_array_header_t *components;
              int j;
              
              /* Invalidate the dir_baton, because it no longer
                 represents target's immediate parent directory. */
              dir_baton = NULL;

              /* split the remainder into path components. */
              components = svn_path_decompose (remainder,
                                               svn_path_local_style,
                                               pool);
              
              for (j = 0; j < components->nelts; j++)
                {
                  svn_stringbuf_t *new_path;
                  svn_wc_entry_t *new_entry;
                  svn_stringbuf_t *component = 
                    (((svn_stringbuf_t **) components->elts)[j]);

                  new_path = svn_stringbuf_dup (stack->path, pool);
                  svn_path_add_component (new_path, component,
                                          svn_path_local_style);
                  err = svn_wc_entry (&new_entry, new_path, pool);
                  if (err)
                    return svn_error_quick_wrap 
                      (err, "commit failed: looking for next commit target");

                  push_stack (&stack, new_path, NULL, new_entry, pool);
                }
            }
          

          /* NOTE: At this point of processing, the topmost stackframe
           * is GUARANTEED to be the parent of TARGET, regardless of
           * whether TARGET is a file or a directory. 
           */
          

          /* Get the entry for TARGET. */
          err = svn_wc_entry (&tgt_entry, target, pool);
          if (err)
            return svn_error_quick_wrap 
              (err, "commit failed: getting entry of commit target");

          if (tgt_entry)
            {
              apr_pool_t *subpool = svn_pool_create (pool);
              svn_stringbuf_t *basename;
              
              if (tgt_entry->existence == svn_wc_existence_deleted)
                return svn_error_createf
                  (SVN_ERR_WC_ENTRY_NOT_FOUND, 0, NULL, pool,
                   "entry '%s' has already been deleted", target->data);

              basename = svn_path_last_component (target,
                                                  svn_path_local_style, 
                                                  pool);
              
              /* If TARGET is a file, we check that file for mods.  No
                 stackframes will be pushed or popped, since (the file's
                 parent is already on the stack).  No batons will be
                 closed at all (in case we need to commit more files in
                 this parent). */
              err = report_single_mod (basename->data,
                                       tgt_entry,
                                       &stack,
                                       affected_targets,
                                       locked_dirs,
                                       editor,
                                       edit_baton,
                                       &dir_baton,
                                       FALSE,
                                       pool);
              
              svn_pool_destroy (subpool);
              
              if (err)
                return svn_error_quick_wrap 
                  (err, "commit failed: while sending tree-delta.");
            }
          else
            return svn_error_createf
              (SVN_ERR_UNVERSIONED_RESOURCE, 0, NULL, pool,
               "svn_wc_crawl_local_mods: '%s' is not a versioned resource",
               target->data);

        } /*  -- End of main target loop -- */
      
      /* To finish, pop the stack all the way back to the grandaddy
         parent_dir, and call close_dir() on all batons we find. */
      err = do_dir_closures (parent_dir, &stack, editor);
      if (err)
        return svn_error_quick_wrap 
          (err, "commit failed: finishing the crawl");

      /* Don't forget to close the root-dir baton on the bottom
         stackframe, if one exists. */
      if (stack->baton)        
        {
          err = editor->close_directory (stack->baton);
          if (err)
            return svn_error_quick_wrap 
              (err, "commit failed: closing editor's root directory");
        }

    }  /* End of multi-target section */


  /* All crawls are completed, so affected_targets potentially has
     some still-open file batons. Loop through affected_targets, and
     fire off any postfix text-deltas that need to be sent. */
  err = do_postfix_text_deltas (affected_targets, editor, pool);
  if (err)
    return svn_error_quick_wrap 
      (err, "commit failed:  while sending postfix text-deltas.");

  /* Have *any* edits been made at all?  We can tell by looking at the
     foundation stackframe; it might still contain a root-dir baton.
     If so, close the entire edit. */
  if (stack->baton)
    {
      err = editor->close_edit (edit_baton);
      if (err)
        {
          /* Commit failure, though not *necessarily* from the
             repository.  close_edit() does a LOT of things, including
             bumping all working copy revision numbers.  Again, see
             earlier comment.

             The interesting thing here is that the commit might have
             succeeded in the repository, but the WC lib returned a
             revision-bumping or wcprop error. */
          return svn_error_quick_wrap
            (err, "commit failed: while calling close_edit()");
        }
    }

  /* The commit is complete, and revisions have been bumped. */  
  return SVN_NO_ERROR;
}


/* Helper for report_revisions().
   
   Perform an atomic restoration of the file FILE_PATH; that is, copy
   the file's text-base to the administrative tmp area, and then move
   that file to FILE_PATH.  */
static svn_error_t *
restore_file (svn_stringbuf_t *file_path,
              apr_pool_t *pool)
{
  apr_status_t status;
  svn_stringbuf_t *text_base_path, *tmp_text_base_path;

  text_base_path = svn_wc__text_base_path (file_path, 0, pool);
  tmp_text_base_path = svn_wc__text_base_path (file_path, 1, pool);

  SVN_ERR (svn_io_copy_file (text_base_path, tmp_text_base_path, pool));
  status = apr_file_rename (tmp_text_base_path->data, file_path->data, pool);
  if (status)
    return svn_error_createf (status, 0, NULL, pool,
                              "error renaming `%s' to `%s'",
                              tmp_text_base_path->data,
                              file_path->data);

  return SVN_NO_ERROR;
}


/* The recursive crawler that describes a mixed-revision working
   copy to an RA layer.  Used to initiate updates.

   This is a depth-first recursive walk of DIR_PATH under WC_PATH.
   Look at each entry and check if its revision is different than
   DIR_REV.  If so, report this fact to REPORTER.  If an entry is
   missing from disk, report its absence to REPORTER.  

   If PRINT_UNRECOGNIZED is set, then unversioned objects will be
   reported via FBTABLE.   If RESTORE_FILES is set, then unexpectedly
   missing working files will be restored from text-base. */
static svn_error_t *
report_revisions (svn_stringbuf_t *wc_path,
                  svn_stringbuf_t *dir_path,
                  svn_revnum_t dir_rev,
                  const svn_ra_reporter_t *reporter,
                  void *report_baton,
                  svn_pool_feedback_t *fbtable,
                  svn_boolean_t print_unrecognized,
                  svn_boolean_t restore_files,
                  apr_pool_t *pool)
{
  apr_hash_t *entries, *dirents;
  apr_hash_index_t *hi;
  apr_array_header_t *patterns;
  apr_pool_t *subpool = svn_pool_create (pool);

  /* Construct the actual 'fullpath' = wc_path + dir_path */
  svn_stringbuf_t *full_path = svn_stringbuf_dup (wc_path, subpool);
  svn_path_add_component (full_path, dir_path, svn_path_local_style);

  /* Get both the SVN Entries and the actual on-disk entries. */
  SVN_ERR (svn_wc_entries_read (&entries, full_path, subpool));
  SVN_ERR (svn_io_get_dirents (&dirents, full_path, subpool));

  /* Try to load any '.svnignore' file that may be present. */
  patterns = apr_array_make (pool, 1, sizeof(const char *));
  SVN_ERR (load_ignore_file (full_path->data, patterns, subpool));
  add_default_ignores (patterns);

  /* Phase 1:  Print out every unrecognized (unversioned) object. */

  if (print_unrecognized)

    for (hi = apr_hash_first (subpool, dirents); hi; hi = apr_hash_next (hi))
      {
        const void *key;
        apr_size_t klen;
        void *val;
        const char *keystring;
        svn_stringbuf_t *current_entry_name;
        svn_stringbuf_t *printable_path;
        
        apr_hash_this (hi, &key, &klen, &val);
        keystring = (const char *) key;
        
        /* If the dirent isn't in `SVN/entries'... */
        if (! apr_hash_get (entries, key, klen))        
          /* and we're not looking at SVN... */
          if (strcmp (keystring, SVN_WC_ADM_DIR_NAME))
            {
              svn_boolean_t print_item = TRUE;
              apr_status_t status;
              int i;
              
              current_entry_name = svn_stringbuf_create (keystring, subpool);
              
              for (i = 0; i < patterns->nelts; i++)
                {
                  const char *pat = (((const char **) (patterns)->elts))[i];
                  
                  /* Try to match current_entry_name to pat. */
                  status = apr_fnmatch (pat, current_entry_name->data,
                                        FNM_PERIOD);
                  
                  if (status == APR_SUCCESS)
                    {
                      /* APR_SUCCESS means we found a match: */
                      print_item = FALSE;
                      break;
                    }
                }
              
              if (print_item)
                {
                  printable_path = svn_stringbuf_dup (full_path, subpool);
                  svn_path_add_component (printable_path, current_entry_name,
                                          svn_path_local_style);
                  status = 
                    fbtable->report_unversioned_item (printable_path->data);
                  if (status)
                    return 
                      svn_error_createf (status, 0, NULL, subpool,
                                         "error reporting unversioned '%s'",
                                         printable_path->data);
                }
            }
      }  /* end of dirents loop */
  
  
  /* Phase 2:  Do the real reporting and recursing. */

  /* Looping over current directory's SVN entries: */
  for (hi = apr_hash_first (subpool, entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      const char *keystring;
      apr_size_t klen;
      void *val;
      svn_stringbuf_t *current_entry_name;
      svn_wc_entry_t *current_entry; 
      svn_stringbuf_t *full_entry_path;
      enum svn_node_kind *dirent_kind;
      svn_boolean_t missing = FALSE;

      /* Get the next entry */
      apr_hash_this (hi, &key, &klen, &val);
      keystring = (const char *) key;
      current_entry = (svn_wc_entry_t *) val;

      /* Compute the name of the entry.  Skip THIS_DIR altogether. */
      if (! strcmp (keystring, SVN_WC_ENTRY_THIS_DIR))
        continue;
      else
        current_entry_name = svn_stringbuf_create (keystring, subpool);

      /* Compute the complete path of the entry, relative to dir_path. */
      full_entry_path = svn_stringbuf_dup (dir_path, subpool);
      if (current_entry_name)
        svn_path_add_component (full_entry_path, current_entry_name,
                                svn_path_local_style);

      /* The Big Tests: */
      
      /* 1. If the entry is `deleted' already, then we *must* report
         it as missing.  Otherwise, the server may tell us to
         re-remove it (remember that the `deleted' flag means that the
         item is deleted in a later revision of the parent dir.) */
      if (current_entry->existence == svn_wc_existence_deleted)
        {
          SVN_ERR (reporter->delete_path (report_baton, full_entry_path));
          continue;  /* move on to next entry */
        }

      /* 2. Is the entry on disk?  Set a flag if not. */
      dirent_kind = (enum svn_node_kind *) apr_hash_get (dirents, key, klen);
      if (! dirent_kind)
        missing = TRUE;
      
      /* From here on out, ignore any entry scheduled for addition
         or deletion */
      if (current_entry->schedule == svn_wc_schedule_normal)
        /* The entry exists on disk, and isn't `deleted'. */
        {
          if (current_entry->kind == svn_node_file) 
            {
              if (dirent_kind && (*dirent_kind != svn_node_file))
                {
                  /* If the dirent changed kind, report it as missing.
                     Later on, the update editor will return an
                     'obstructed update' error.  :)  */
                  SVN_ERR (reporter->delete_path (report_baton,
                                                full_entry_path));
                  continue;  /* move to next entry */
                }

              if (missing && restore_files)
                {
                  svn_stringbuf_t *long_file_path 
                    = svn_stringbuf_dup (full_path, pool);
                  svn_path_add_component (long_file_path, current_entry_name,
                                          svn_path_local_style);

                  /* Recreate file from text-base. */
                  SVN_ERR (restore_file (long_file_path, pool));

                  /* Tell feedback table. */
                  fbtable->report_restoration (long_file_path->data, pool);
                }

              /* Possibly report a differing revision. */
              if (current_entry->revision !=  dir_rev)                
                SVN_ERR (reporter->set_path (report_baton,
                                             full_entry_path,
                                             current_entry->revision));
            }

          else if (current_entry->kind == svn_node_dir)
            {
              if (missing)
                {
                  /* We can't recreate dirs locally, so report as missing. */
                  SVN_ERR (reporter->delete_path (report_baton,
                                                  full_entry_path));   
                  continue;  /* move on to next entry */
                }

              if (dirent_kind && (*dirent_kind != svn_node_dir))
                /* No excuses here.  If the user changed a
                   revision-controlled directory into something else,
                   the working copy is FUBAR.  It can't receive
                   updates within this dir anymore.  Throw a real
                   error. */
                return svn_error_createf
                  (SVN_ERR_WC_OBSTRUCTED_UPDATE, 0, NULL, subpool,
                   "The entry '%s' is no longer a directory,\n"
                   "which prevents proper updates.\n"
                   "Please remove this entry and try updating again.",
                   full_entry_path->data);
              
              /* Otherwise, possibly report a differing revision, and
                 recurse. */
              {
                svn_wc_entry_t *subdir_entry;
                svn_stringbuf_t *megalong_path = 
                  svn_stringbuf_dup (wc_path, subpool);
                svn_path_add_component (megalong_path, full_entry_path,
                                        svn_path_local_style);
                SVN_ERR (svn_wc_entry (&subdir_entry, megalong_path, subpool));
                
                if (subdir_entry->revision != dir_rev)
                  SVN_ERR (reporter->set_path (report_baton,
                                               full_entry_path,
                                               subdir_entry->revision));
                /* Recurse. */
                SVN_ERR (report_revisions (wc_path,
                                           full_entry_path,
                                           subdir_entry->revision,
                                           reporter, report_baton, fbtable,
                                           print_unrecognized,
                                           restore_files,
                                           subpool));
              }
            } /* end directory case */
        } /* end 'entry exists on disk' */   
    } /* end main entries loop */

  /* We're done examining this dir's entries, so free everything. */
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}





/*------------------------------------------------------------------*/
/*** Public Interfaces ***/


/* This is the main driver of the commit-editor.   It drives the
   editor in postfix-text-delta style. */


/* Fascinating note about the potential values of {parent_dir,
   condensed_targets} coming into this function:

   There are only four possibilities.

    1. No targets.
       parent = /home/sussman, targets = []

    2. One file target.
       parent = /home/sussman, targets = [foo.c]

    3. One directory target.(*)
       parent = /home/sussman, targets = [bar]

    4. Two or more targets of any type.
       parent = /home/sussman, targets = [foo.c, bar, baz, ...]

   (*) While svn_path_condense_targets does not allow for the
   possibility of a single directory target, the caller should have
   used svn_wc_get_actual_target in this case, which would result in
   the {parent_dir, NULL} combination possibly turning into a
   {parent_dir's parent, parent_dir} combination. */
svn_error_t *
svn_wc_crawl_local_mods (svn_stringbuf_t *parent_dir,
                         apr_array_header_t *condensed_targets,
                         const svn_delta_edit_fns_t *editor,
                         void *edit_baton,
                         apr_pool_t *pool)
{
  svn_error_t *err, *err2;

  /* All the wc directories that are "locked" as we commit local
     changes. */
  apr_hash_t *locked_dirs = apr_hash_make (pool);

  /* Sanity check. */
  assert (parent_dir != NULL);
  assert (condensed_targets != NULL);

  /* Sort the condensed targets so that targets which share "common
     sub-parent" directories are all lumped together.  This guarantees
     a depth-first drive of the editor. */
  qsort (condensed_targets->elts,
         condensed_targets->nelts,
         condensed_targets->elt_size,
         svn_sort_compare_strings_as_paths);


  /* Now pass the locked_dirs hash into the *real* routine that does
     the work. */
  err = svn_wc__crawl_local_mods (parent_dir,
                                  condensed_targets,
                                  editor, edit_baton,
                                  locked_dirs,
                                  pool);

  /* Make sure that we always remove the locks that we installed. */
  err2 = remove_all_locks (locked_dirs, pool);

  /* Now deal with the two errors that may have occurred. */
  if (err && err2)
    {
      svn_error_t *scan;

      /* This is tricky... wrap the two errors and concatenate them. */
      err = svn_error_quick_wrap (err, "---- commit error follows:");
      err2 = svn_error_quick_wrap
        (err2, "commit failed (see below); unable to remove all wc locks:");

      /* Hook the commit error to the end of the unlock error. */
      for (scan = err2; scan->child != NULL; scan = scan->child)
        continue;
      scan->child = err;

      /* Return the unlock error; the commit error is at the end. */
      return err2;
    }

  if (err)
    return svn_error_quick_wrap 
      (err, "commit failed: wc locks have been removed.");

  if (err2)
    return svn_error_quick_wrap
      (err, "commit succeeded, but unable to remove all wc locks!");

  return SVN_NO_ERROR;
}



/* This is the main driver of the working copy state "reporter", used
   for updates. */
svn_error_t *
svn_wc_crawl_revisions (svn_stringbuf_t *path,
                        const svn_ra_reporter_t *reporter,
                        void *report_baton,
                        svn_boolean_t print_unrecognized,
                        svn_boolean_t restore_files,
                        apr_pool_t *pool)
{
  svn_error_t *err;
  svn_wc_entry_t *entry;
  svn_revnum_t base_rev = SVN_INVALID_REVNUM;
  svn_pool_feedback_t *fbtable = svn_pool_get_feedback_vtable (pool);
  svn_boolean_t missing = FALSE;

  /* The first thing we do is get the base_rev from the working copy's
     ROOT_DIRECTORY.  This is the first revnum that entries will be
     compared to. */
  SVN_ERR (svn_wc_entry (&entry, path, pool));
  base_rev = entry->revision;
  if (base_rev == SVN_INVALID_REVNUM)
    {
      svn_stringbuf_t *parent_name = svn_stringbuf_dup (path, pool);
      svn_wc_entry_t *parent_entry;
      svn_path_remove_component (parent_name, svn_path_local_style);
      SVN_ERR (svn_wc_entry (&parent_entry, parent_name, pool));
      base_rev = parent_entry->revision;
    }

  /* The first call to the reporter merely informs it that the
     top-level directory being updated is at BASE_REV.  Its PATH
     argument is ignored. */
  SVN_ERR (reporter->set_path (report_baton,
                               svn_stringbuf_create ("", pool),
                               base_rev));

  if (entry->existence != svn_wc_existence_deleted
      && entry->schedule != svn_wc_schedule_delete)
    {
      apr_finfo_t info;
      apr_status_t apr_err;
      apr_err = apr_stat (&info, path->data, APR_FINFO_MIN, pool);
      if (APR_STATUS_IS_ENOENT(apr_err))
        missing = TRUE;
    }

  if (entry->kind == svn_node_dir)
    {
      if (missing)
        {
          /* Always report directories as missing;  we can't recreate
             them locally. */
          err = reporter->delete_path (report_baton,
                                       svn_stringbuf_create ("", pool));
          if (err)
            {
              /* Clean up the fs transaction. */
              svn_error_t *fserr;
              fserr = reporter->abort_report (report_baton);
              if (fserr)
                return svn_error_quick_wrap (fserr, "Error aborting report.");
              else
                return err;
            }
        }

      else 
        {
          /* Recursively crawl ROOT_DIRECTORY and report differing
             revisions. */
          err = report_revisions (path,
                                  svn_stringbuf_create ("", pool),
                                  base_rev,
                                  reporter, report_baton, fbtable, 
                                  print_unrecognized, restore_files, pool);
          if (err)
            {
              /* Clean up the fs transaction. */
              svn_error_t *fserr;
              fserr = reporter->abort_report (report_baton);
              if (fserr)
                return svn_error_quick_wrap (fserr, "Error aborting report.");
              else
                return err;
            }
        }
    }

  else if (entry->kind == svn_node_file)
    {
      if (missing && restore_files)
        {
          /* Recreate file from text-base. */
          SVN_ERR (restore_file (path, pool));
          
          /* Tell feedback table. */
          fbtable->report_restoration (path->data, pool);
        }

      if (entry->revision != base_rev)
        {
          /* If this entry is a file node, we just want to report that
             node's revision.  Since we are looking at the actual target
             of the report (not some file in a subdirectory of a target
             directory), and that target is a file, we need to pass an
             empty string to set_path. */
          err = reporter->set_path (report_baton, 
                                    svn_stringbuf_create ("", pool),
                                    base_rev);
          if (err)
            {
              /* Clean up the fs transaction. */
              svn_error_t *fserr;
              fserr = reporter->abort_report (report_baton);
              if (fserr)
                return svn_error_quick_wrap (fserr, "Error aborting report.");
              else
                return err;
            }
        }
    }

  /* Finish the report, which causes the update editor to be driven. */
  err = reporter->finish_report (report_baton);
  if (err)
    {
      /* Clean up the fs transaction. */
      svn_error_t *fserr;
      fserr = reporter->abort_report (report_baton);
      if (fserr)
        return svn_error_quick_wrap (fserr, "Error aborting report.");
      else
        return err;
    }

  return SVN_NO_ERROR;
}





/* 

   Status of multi-arg commits:
   
   TODO:

   * the "path analysis" phase needs to happen at a high level in the
   client, along with the alphabetization.  Specifically, when the
   client must open an RA session to the *grandparent* dir of all
   commit targets, and use that ra session to fetch the commit
   editor.  It then needs to pass the canonicalized paths to
   crawl_local_mods.

   * must write some python tests for multi-args.

   * secret worry:  do all the new path routines work -- both Kevin
   P-B's as well as my own?
 
 */


/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: */
