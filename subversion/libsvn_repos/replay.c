/*
 * replay.c:   an editor driver for changes made in a given revision
 *             or transaction
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


#include <assert.h>
#include <apr_hash.h>
#include <apr_md5.h>

#include "svn_types.h"
#include "svn_delta.h"
#include "svn_fs.h"
#include "svn_repos.h"
#include "svn_props.h"
#include "svn_pools.h"
#include "svn_path.h"
#include "svn_private_config.h"


/*** Backstory ***/

/* The year was 2003.  Subversion usage was rampant in the world, and
   there was a rapidly growing issues database to prove it.  To make
   matters worse, svn_repos_dir_delta() had simply outgrown itself.
   No longer content to simply describe the differences between two
   trees, the function had been slowly bearing the added
   responsibility of representing the actions that had been taken to
   cause those differences -- a burden it was never meant to bear.
   Now grown into a twisted mess of razor-sharp metal and glass, and
   trembling with a sort of momentarily stayed spring force,
   svn_repos_dir_delta was a timebomb poised for total annihilation of
   the American Midwest.
   
   Subversion needed a change.

   Changes, in fact.  And not just in the literary segue sense.  What
   Subversion desperately needed was a new mechanism solely
   responsible for replaying repository actions back to some
   interested party -- to translate and retransmit the contents of the
   Berkeley 'changes' database file. */

/*** Overview ***/

/* The filesystem keeps a record of high-level actions that affect the
   files and directories in itself.  The 'changes' table records
   additions, deletions, textual and property modifications, and so
   on.  The goal of the functions in this file is to examine those
   change records, and use them to drive an editor interface in such a
   way as to effectively replay those actions.

   This is critically different than what svn_repos_dir_delta() was
   designed to do.  That function describes, in the simplest way it
   can, how to transform one tree into another.  It doesn't care
   whether or not this was the same way a user might have done this
   transformation.  More to the point, it doesn't care if this is how
   those differences *did* come into being.  And it is for this reason
   that it cannot be relied upon for tasks such as the repository
   dumpfile-generation code, which is supposed to represent not
   changes, but actions that cause changes.

   So, what's the plan here?

   First, we fetch the changes for a particular revision or
   transaction.  We get these as an array, sorted chronologically.
   From this array we will build a hash, keyed on the path associated
   with each change item, and whose values are arrays of changes made
   to that path, again preserving the chronological ordering.

   Once our hash it built, we then sort all the keys of the hash (the
   paths) using a depth-first directory sort routine.

   Finally, we drive an editor, moving down our list of sorted paths,
   and manufacturing any intermediate editor calls (directory openings
   and closures) needed to navigate between each successive path.  For
   each path, we replay the sorted actions that occurred at that path.

   We we've finished the editor drive, we should have fully replayed
   the filesystem events that occurred in that revision or transactions
   (though not necessarily in the same order in which they
   occurred). */
   


/*** Helper functions. ***/


struct path_driver_cb_baton
{
  const svn_delta_editor_t *editor;
  void *edit_baton;

  /* The root of the revision we're replaying. */
  svn_fs_root_t *root;

  /* The root of the previous revision.  If this is non-NULL it means that
     we are supposed to generate props and text deltas relative to it. */
  svn_fs_root_t *compare_root;

  apr_hash_t *changed_paths;

  svn_repos_authz_func_t authz_read_func;
  void *authz_read_baton;

  const char *base_path;
  int base_path_len;

  svn_revnum_t low_water_mark;
};

/* Recursively traverse PATH (as it exists under SOURCE_ROOT) emitting
   the appropriate editor calls to add it and its children without any
   history.  This is meant to be used when either a subset of the tree
   has been ignored and we need to copy something from that subset to
   the part of the tree we do care about, or if a subset of the tree is
   unavailable because of authz and we need to use it as the source of
   a copy. */
static svn_error_t *
add_subdir (svn_fs_root_t *source_root,
            svn_fs_root_t *target_root,
            const svn_delta_editor_t *editor,
            void *edit_baton,
            const char *path,
            void *parent_baton,
            const char *source_path,
            svn_repos_authz_func_t authz_read_func,
            void *authz_read_baton,
            apr_pool_t *pool,
            void **dir_baton)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  apr_hash_index_t *hi;
  apr_hash_t *dirents;

  SVN_ERR (editor->add_directory (path, parent_baton, NULL,
                                  SVN_INVALID_REVNUM, pool, dir_baton));

  SVN_ERR (svn_fs_dir_entries (&dirents, source_root, source_path, pool));

  for (hi = apr_hash_first (pool, dirents); hi; hi = apr_hash_next (hi))
    {
      svn_boolean_t readable = TRUE;
      svn_fs_dirent_t *dent;
      const char *new_path;
      void *val;

      svn_pool_clear (subpool);

      apr_hash_this (hi, NULL, NULL, &val);

      dent = val;

      new_path = svn_path_join (path, dent->name, subpool);

      if (authz_read_func)
        SVN_ERR (authz_read_func (&readable, target_root, new_path,
                                  authz_read_baton, pool));

      if (! readable)
        continue;

      if (dent->kind == svn_node_dir)
        {
          void *new_dir_baton;

          SVN_ERR (add_subdir (source_root, target_root, editor, edit_baton,
                               new_path, *dir_baton,
                               svn_path_join (source_path, dent->name,
                                              subpool),
                               authz_read_func, authz_read_baton,
                               subpool, &new_dir_baton));

          SVN_ERR (editor->close_directory (new_dir_baton, subpool));
        }
      else if (dent->kind == svn_node_file)
        {
          svn_txdelta_window_handler_t delta_handler;
          void *delta_handler_baton, *file_baton;
          svn_txdelta_stream_t *delta_stream;

          SVN_ERR (editor->add_file (svn_path_join (path, dent->name, subpool),
                                     *dir_baton, NULL, SVN_INVALID_REVNUM,
                                     pool, &file_baton));

          SVN_ERR (editor->apply_textdelta (file_baton, NULL, pool, 
                                            &delta_handler, 
                                            &delta_handler_baton));

          SVN_ERR (svn_fs_get_file_delta_stream
                     (&delta_stream, NULL, NULL, source_root,
                      svn_path_join (source_path, dent->name, subpool),
                      pool));

          SVN_ERR (svn_txdelta_send_txstream (delta_stream,
                                              delta_handler,
                                              delta_handler_baton,
                                              pool));

          SVN_ERR (editor->close_file (file_baton, NULL, pool));
        }
      else
        abort ();
    }

  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}

static svn_boolean_t
is_within_base_path (const char *path, const char *base_path, int base_len)
{
  if (base_path[0] == '\0')
    return TRUE;

  if (strncmp (base_path, path, base_len) == 0
      && (path[base_len] == '/' || path[base_len] == '\0'))
    return TRUE;

  return FALSE;
}

static svn_error_t *
path_driver_cb_func (void **dir_baton,
                     void *parent_baton,
                     void *callback_baton,
                     const char *path,
                     apr_pool_t *pool)
{
  struct path_driver_cb_baton *cb = callback_baton;
  const svn_delta_editor_t *editor = cb->editor;
  void *edit_baton = cb->edit_baton;
  svn_fs_root_t *root = cb->root;
  svn_fs_path_change_t *change;
  svn_boolean_t do_add = FALSE, do_delete = FALSE;
  svn_node_kind_t kind;
  void *file_baton = NULL;
  const char *copyfrom_path = NULL;
  svn_revnum_t copyfrom_rev;
  svn_boolean_t src_readable = TRUE;
  svn_fs_root_t *source_root = cb->compare_root;
  const char *base_path = cb->base_path;
  int base_path_len = cb->base_path_len;

  *dir_baton = NULL;

  change = apr_hash_get (cb->changed_paths, path, APR_HASH_KEY_STRING);
  switch (change->change_kind)
    {
    case svn_fs_path_change_add:
      do_add = TRUE;
      break;

    case svn_fs_path_change_delete:
      do_delete = TRUE;
      break;

    case svn_fs_path_change_replace:
      do_add = TRUE;
      do_delete = TRUE;
      break;

    case svn_fs_path_change_modify:
    default:
      /* do nothing */
      break;
    }

  /* Handle any deletions. */
  if (do_delete)
    SVN_ERR (editor->delete_entry (path, SVN_INVALID_REVNUM, 
                                   parent_baton, pool));

  /* Fetch the node kind if it makes sense to do so. */
  if (! do_delete || do_add)
    {
      SVN_ERR (svn_fs_check_path (&kind, root, path, pool));
      if ((kind != svn_node_dir) && (kind != svn_node_file))
        return svn_error_createf 
          (SVN_ERR_FS_NOT_FOUND, NULL, 
           _("Filesystem path '%s' is neither a file nor a directory"), path);
    }

  /* Handle any adds/opens. */
  if (do_add)
    {
      /* Was this node copied? */
      SVN_ERR (svn_fs_copied_from (&copyfrom_rev, &copyfrom_path,
                                   root, path, pool));

      if (copyfrom_path && SVN_IS_VALID_REVNUM (copyfrom_rev))
        {
          /* Save the source_root so that we can use it later, when we
             need to generate text deltas. */

          SVN_ERR (svn_fs_revision_root (&source_root,
                                         svn_fs_root_fs (root),
                                         copyfrom_rev, pool));

          if (cb->authz_read_func)
            {
              SVN_ERR (cb->authz_read_func (&src_readable, source_root,
                                            copyfrom_path,
                                            cb->authz_read_baton, pool));
            }
        }

      /* Do the right thing based on the path KIND. */
      if (kind == svn_node_dir)
        {
          /* If there is a copyfrom path, and we're either not allowed to see
             it or we're explicitly ignoring (i.e. the base_path doesn't match
             the copyfrom path) or the copyfrom rev is prior to the low water
             mark then we just do a recursive add of the source path contents.
           */
          if (copyfrom_path
              && (! src_readable 
                  || ! is_within_base_path (copyfrom_path+1, base_path,
                                            base_path_len)
                  || cb->low_water_mark > copyfrom_rev))
            {
              SVN_ERR (add_subdir (source_root, root, editor, edit_baton,
                                   path, parent_baton, copyfrom_path,
                                   cb->authz_read_func, cb->authz_read_baton,
                                   pool, dir_baton));
            }
          else
            {
              SVN_ERR (editor->add_directory (path, parent_baton,
                                              copyfrom_path, copyfrom_rev,
                                              pool, dir_baton));
            }
        }
      else
        {
          /* If we have a copyfrom path, and we can't read it or we're just
             ignoring it, or the copyfrom rev is prior to the low water mark
             then we just null them out and do a raw add with no history at
             all. */
          if (copyfrom_path
              && (! src_readable
                  || ! is_within_base_path (copyfrom_path+1, base_path,
                                            base_path_len)
                  || cb->low_water_mark > copyfrom_rev))
            {
              copyfrom_path = NULL;
              copyfrom_rev = SVN_INVALID_REVNUM;
            }

          SVN_ERR (editor->add_file (path, parent_baton, copyfrom_path,
                                     copyfrom_rev, pool, &file_baton));
        }
    }
  else if (! do_delete)
    {
      /* Do the right thing based on the path KIND (and the presence
         of a PARENT_BATON). */
      if (kind == svn_node_dir)
        {
          if (parent_baton)
            {
              SVN_ERR (editor->open_directory (path, parent_baton, 
                                               SVN_INVALID_REVNUM,
                                               pool, dir_baton));
            }
          else
            {
              SVN_ERR (editor->open_root (edit_baton, SVN_INVALID_REVNUM, 
                                          pool, dir_baton));
            }
        }
      else
        {
          SVN_ERR (editor->open_file (path, parent_baton, SVN_INVALID_REVNUM,
                                      pool, &file_baton));
        }
    }

  /* Handle property modifications. */
  if (! do_delete || do_add)
    {
      if (change->prop_mod)
        {
          if (cb->compare_root)
            {
              apr_array_header_t *prop_diffs;
              apr_hash_t *old_props;
              apr_hash_t *new_props;
              int i;

              if (change->change_kind != svn_fs_path_change_add)
                SVN_ERR (svn_fs_node_proplist
                           (&old_props, cb->compare_root,
                            copyfrom_path ? copyfrom_path : path, pool));
              else
                old_props = apr_hash_make (pool);

              SVN_ERR (svn_fs_node_proplist (&new_props, root, path, pool));

              SVN_ERR (svn_prop_diffs (&prop_diffs, new_props, old_props,
                                       pool));

              for (i = 0; i < prop_diffs->nelts; ++i)
                {
                   svn_prop_t *pc = &APR_ARRAY_IDX (prop_diffs, i, svn_prop_t);
                   if (kind == svn_node_dir)
                     SVN_ERR (editor->change_dir_prop (*dir_baton, pc->name,
                                                       pc->value, pool));
                   else if (kind == svn_node_file)
                     SVN_ERR (editor->change_file_prop (file_baton, pc->name,
                                                        pc->value, pool));
                }
            }
          else
            {
              if (kind == svn_node_dir)
                SVN_ERR (editor->change_dir_prop (*dir_baton, "", NULL,
                                                  pool));
              else if (kind == svn_node_file)
                SVN_ERR (editor->change_file_prop (file_baton, "", NULL,
                                                   pool));
            }
        }

      /* Handle textual modifications.

         Note that this needs to happen in the "copy from a file we
         aren't allowed to see" case since otherwise the caller will
         have no way to actually get the new file's contents, which
         they are apparently allowed to see. */
      if (change->text_mod || (copyfrom_path && ! src_readable))
        {
          svn_txdelta_window_handler_t delta_handler;
          void *delta_handler_baton;
          SVN_ERR (editor->apply_textdelta (file_baton, NULL, pool, 
                                            &delta_handler, 
                                            &delta_handler_baton));
          if (cb->compare_root)
            {
              svn_txdelta_stream_t *delta_stream;

              SVN_ERR (svn_fs_get_file_delta_stream
                         (&delta_stream,
                          (copyfrom_path && src_readable) ? source_root
                                                          : NULL,
                          (copyfrom_path && src_readable) ? copyfrom_path
                                                          : path,
                          root, path, pool));

              SVN_ERR (svn_txdelta_send_txstream (delta_stream,
                                                  delta_handler,
                                                  delta_handler_baton,
                                                  pool));
            }
          else
            SVN_ERR (delta_handler (NULL, delta_handler_baton));
        }
    }

  /* Close the file baton if we opened it. */
  if (file_baton)
    SVN_ERR (editor->close_file (file_baton, NULL, pool));

  return SVN_NO_ERROR;
}




svn_error_t *
svn_repos_replay2 (svn_fs_root_t *root,
                   const char *base_path,
                   svn_revnum_t low_water_mark,
                   svn_boolean_t send_deltas,
                   const svn_delta_editor_t *editor,
                   void *edit_baton,
                   svn_repos_authz_func_t authz_read_func,
                   void *authz_read_baton,
                   apr_pool_t *pool)
{
  apr_hash_t *fs_changes;
  apr_hash_t *changed_paths;
  apr_hash_index_t *hi;
  apr_array_header_t *paths;
  struct path_driver_cb_baton cb_baton;
  int base_path_len;

  /* Fetch the paths changed under ROOT. */
  SVN_ERR (svn_fs_paths_changed (&fs_changes, root, pool));

  if (! base_path)
    base_path = "";
  else if (base_path[0] == '/')
    ++base_path;

  base_path_len = strlen (base_path);

  /* Make an array from the keys of our CHANGED_PATHS hash, and copy
     the values into a new hash whose keys have no leading slashes. */
  paths = apr_array_make (pool, apr_hash_count (fs_changes),
                          sizeof (const char *));
  changed_paths = apr_hash_make (pool);
  for (hi = apr_hash_first (pool, fs_changes); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;
      apr_ssize_t keylen;
      const char *path;
      svn_fs_path_change_t *change;
      svn_boolean_t allowed = TRUE;

      apr_hash_this (hi, &key, &keylen, &val);
      path = key;
      change = val;

      if (authz_read_func)
        SVN_ERR (authz_read_func (&allowed, root, path, authz_read_baton,
                                  pool));

      if (allowed)
        {
          if (path[0] == '/')
            {
              path++;
              keylen--;
            }

          /* If the base_path doesn't match the top directory of this path
             we don't want anything to do with it... */
          if (is_within_base_path (path, base_path, base_path_len))
            {
              APR_ARRAY_PUSH (paths, const char *) = path;
              apr_hash_set (changed_paths, path, keylen, change);
            }
        }
    }

  /* If we were not give a low water mark, assume that everything is there,
     all the way back to revision 0. */ 
  if (! SVN_IS_VALID_REVNUM (low_water_mark))
    low_water_mark = 0;
 
  /* Initialize our callback baton. */
  cb_baton.editor = editor;
  cb_baton.edit_baton = edit_baton;
  cb_baton.root = root;
  cb_baton.changed_paths = changed_paths;
  cb_baton.authz_read_func = authz_read_func;
  cb_baton.authz_read_baton = authz_read_baton;
  cb_baton.base_path = base_path;
  cb_baton.base_path_len = base_path_len;
  cb_baton.low_water_mark = low_water_mark;

  if (send_deltas)
    SVN_ERR (svn_fs_revision_root (&cb_baton.compare_root,
                                   svn_fs_root_fs (root),
                                   svn_fs_revision_root_revision (root) - 1,
                                   pool));
  else
    cb_baton.compare_root = NULL;

  /* Determine the revision to use throughout the edit, and call
     EDITOR's set_target_revision() function.  */
  if (svn_fs_is_revision_root (root))
    {
      svn_revnum_t revision = svn_fs_revision_root_revision (root);
      SVN_ERR (editor->set_target_revision (edit_baton, revision, pool));
    }

  /* Call the path-based editor driver. */
  SVN_ERR (svn_delta_path_driver (editor, edit_baton, 
                                  SVN_INVALID_REVNUM, paths, 
                                  path_driver_cb_func, &cb_baton, pool));
  
  return SVN_NO_ERROR;
}

svn_error_t *
svn_repos_replay (svn_fs_root_t *root,
                  const svn_delta_editor_t *editor,
                  void *edit_baton,
                  apr_pool_t *pool)
{
  return svn_repos_replay2 (root,
                            "" /* the whole tree */,
                            SVN_INVALID_REVNUM, /* no low water mark */
                            FALSE /* no text deltas */,
                            editor, edit_baton,
                            NULL /* no authz func */,
                            NULL /* no authz baton */,
                            pool);
}
