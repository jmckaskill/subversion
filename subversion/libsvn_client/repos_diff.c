/*
 * repos_diff.c -- The diff editor for comparing two repository versions
 *
 * ====================================================================
 * Copyright (c) 2002 CollabNet.  All rights reserved.
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

/* This code uses an svn_delta_edit_fns_t editor driven by a tree delta
 * between two repository revisions (REV1 and REV2). For each file
 * encountered in the delta the editor constructs two temporary files, one
 * for each revision. This necessitates a separate request for the REV1
 * version of the file when the delta shows the file being modified or
 * deleted. Files that are added by the delta do not require a separate
 * request, the REV1 version is empty and the delta is sufficient to
 * construct the REV2 version. When both versions of each file have been
 * created the diff callback is invoked to display the difference between
 * the two files.
 */

#include "svn_client.h"
#include "svn_wc.h"
#include "svn_pools.h"
#include "svn_path.h"

#include "client.h"

/* Overall crawler editor baton.
 */
struct edit_baton {
  /* TARGET represent the base of the hierarchy to be compared. */
  svn_stringbuf_t *target;

  /* The callback and calback argument that implement the file comparison
     function */
  svn_wc_diff_cmd_t diff_cmd;
  void *diff_cmd_baton;

  /* Flags whether to diff recursively or not. If set the diff is
     recursive. */
  svn_boolean_t recurse;

  /* RA_LIB is the vtable for making requests to the RA layer, RA_SESSION
     is the open session for these requests */
  svn_ra_plugin_t *ra_lib;
  void *ra_session;

  /* The rev1 from the '-r Rev1:Rev2' command line option */
  svn_revnum_t revision;

  /* A temporary empty file. Used for add/delete differences. This is
     cached here so that it can be reused, all empty files are the same. */
  svn_stringbuf_t *empty_file;

  apr_pool_t *pool;
};

/* Directory level baton.
 */
struct dir_baton {
  /* Gets set if the directory is added rather than replaced/unchanged. */
  svn_boolean_t added;

  /* The path of the directory within the repository */
  svn_stringbuf_t *path;

  /* The baton for the parent directory, or null if this is the root of the
     hierarchy to be compared. */
  struct dir_baton *dir_baton;

  /* The overall crawler editor baton. */
  struct edit_baton *edit_baton;

  apr_pool_t *pool;
};

/* File level baton.
 */
struct file_baton {
  /* Gets set if the file is added rather than replaced. */
  svn_boolean_t added;

  /* The path of the file within the repository */
  svn_stringbuf_t *path;

  /* The path and APR file handle to the temporary file that contains the
     first repository version */
  svn_stringbuf_t *path_start_revision;
  apr_file_t *file_start_revision;

  /* The path and APR file handle to the temporary file that contains the
     second repository version.  These fields are set when processing
     textdelta and file deletion, and will be NULL if there's no
     textual difference between the two revisions. */
  svn_stringbuf_t *path_end_revision;
  apr_file_t *file_end_revision;

  /* APPLY_HANDLER/APPLY_BATON represent the delta applcation baton. */
  svn_txdelta_window_handler_t apply_handler;
  void *apply_baton;

  /* The baton for the parent directory. */
  struct dir_baton *dir_baton;

  /* The overall crawler editor baton. */
  struct edit_baton *edit_baton;

  apr_pool_t *pool;
};

/* Data used by the apr pool temp file cleanup handler */
struct temp_file_cleanup_s {
  /* The path to the file to be deleted */
  svn_stringbuf_t *path;
  /* The pool to which the deletion of the file is linked. */
  apr_pool_t *pool;
};

/* Create a new directory baton. NAME is the directory name sans
 * path. ADDED is set if this directory is being added rather than
 * replaced. PARENT_BATON is the baton of the parent directory, it will be
 * null if this is the root of the comparison hierarchy. The directory and
 * its parent may or may not exist in the working copy. EDIT_BATON is the
 * overall crawler editor baton.
 */
static struct dir_baton *
make_dir_baton (const svn_stringbuf_t *name,
                struct dir_baton *parent_baton,
                struct edit_baton *edit_baton,
                svn_boolean_t added,
                apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  struct dir_baton *dir_baton = apr_pcalloc (subpool, sizeof (*dir_baton));

  dir_baton->dir_baton = parent_baton;
  dir_baton->edit_baton = edit_baton;
  dir_baton->added = added;
  dir_baton->pool = subpool;

  if (parent_baton)
    {
      dir_baton->path = svn_stringbuf_dup (parent_baton->path, dir_baton->pool);
    }
  else
    {
      dir_baton->path = svn_stringbuf_create ("", dir_baton->pool);
    }

  if (name)
    svn_path_add_component (dir_baton->path, name);

  return dir_baton;
}

/* Create a new file baton. NAME is the directory name sans path. ADDED is
 * set if this file is being added rather than replaced. PARENT_BATON is
 * the baton of the parent directory.  The directory and its parent may or
 * may not exist in the working copy.
 */
static struct file_baton *
make_file_baton (const svn_stringbuf_t *name,
                 svn_boolean_t added,
                 struct dir_baton *parent_baton)
{
  apr_pool_t *subpool = svn_pool_create (parent_baton->pool);
  struct file_baton *file_baton = apr_pcalloc (subpool, sizeof (*file_baton));

  file_baton->dir_baton = parent_baton;
  file_baton->edit_baton = parent_baton->edit_baton;
  file_baton->added = added;
  file_baton->pool = subpool;

  file_baton->path = svn_stringbuf_dup (parent_baton->path, file_baton->pool);
  svn_path_add_component (file_baton->path, name);

  return file_baton;
}

/* An apr pool cleanup handler, this deletes one of the temporary files.
 */
static apr_status_t
temp_file_plain_cleanup_handler (void *arg)
{
  struct temp_file_cleanup_s *s = arg;

  return apr_file_remove (s->path->data, s->pool);
}

/* An apr pool cleanup handler, this removes a cleanup handler.
 */
static apr_status_t
temp_file_child_cleanup_handler (void *arg)
{
  struct temp_file_cleanup_s *s = arg;

  apr_pool_cleanup_kill (s->pool, s, temp_file_plain_cleanup_handler);

  return APR_SUCCESS;
}

/* Register a pool cleanup to delete PATH when POOL is destroyed.
 *
 * The main "gotcha" is that if the process forks a child by calling
 * apr_proc_create, then the child's copy of the cleanup handler will run
 * and delete the file while the parent still expects it to be around. To
 * avoid this a child cleanup handler is also installed to kill the plain
 * cleanup handler in the child.
 *
 * ### TODO: This a candidate to be a general utility function.
 */
static svn_error_t *
temp_file_cleanup_register (svn_stringbuf_t *path,
                            apr_pool_t *pool)
{
  struct temp_file_cleanup_s *s = apr_palloc (pool, sizeof (*s));
  s->path = path;
  s->pool = pool;
  apr_pool_cleanup_register (s->pool, s, temp_file_plain_cleanup_handler,
                             temp_file_child_cleanup_handler);
  return SVN_NO_ERROR;
}


/* Get the repository version of a file. This makes an RA request to
 * retrieve the file contents. A pool cleanup handler is installed to
 * delete this file.
 *
 * ### TODO: The editor calls this function to get REV1 of the file. Can we
 * get the file props as well?  Then get_wc_prop() could return them later
 * on enabling the REV1:REV2 request to send diffs.
 */
static svn_error_t *
get_file_from_ra (struct file_baton *b)
{
  apr_status_t status;
  apr_file_t *file;
  svn_stream_t *fstream;
  svn_stringbuf_t *tmp_name;

  /* ### TODO: Need some apr temp file support */
  tmp_name = svn_stringbuf_create ("tmp", b->pool);
  SVN_ERR (svn_io_open_unique_file (&file, &b->path_start_revision,
                                    tmp_name, "", FALSE, b->pool));

  /* Install a pool cleanup handler to delete the file */
  SVN_ERR (temp_file_cleanup_register (b->path_start_revision, b->pool));

  fstream = svn_stream_from_aprfile (file, b->pool);
  SVN_ERR (b->edit_baton->ra_lib->get_file (b->edit_baton->ra_session,
                                            b->path->data,
                                            b->edit_baton->revision,
                                            fstream, NULL, NULL));

  status = apr_file_close (file);
  if (status)
    return svn_error_createf (status, 0, NULL, b->pool,
                              "failed to close file '%s'",
                              b->path_start_revision->data);

  return SVN_NO_ERROR;
}

/* Create an empty file, the path to the file is returned in EMPTY_FILE
 */
static svn_error_t *
create_empty_file (svn_stringbuf_t **empty_file,
                   apr_pool_t *pool)
{
  apr_status_t status;
  apr_file_t *file;
  svn_stringbuf_t *tmp_name;

  /* ### TODO: Need some apr temp file support */
  tmp_name = svn_stringbuf_create ("tmp", pool);
  SVN_ERR (svn_io_open_unique_file (&file, empty_file, tmp_name, "", FALSE,
                                    pool));

  status = apr_file_close (file);
  if (status)
    return svn_error_createf (status, 0, NULL, pool,
                              "failed to create empty file '%s'",
                              (*empty_file)->data);
  return SVN_NO_ERROR;
}

/* Get the empty file associated with the edit baton. This is cached so
 * that it can be reused, all empty files are the same.
 */
static svn_error_t *
get_empty_file (struct edit_baton *b,
                svn_stringbuf_t **empty_file)
{
  /* Create the file if it does not exist */
  if (!b->empty_file)
    {
      SVN_ERR (create_empty_file (&b->empty_file, b->pool));

      /* Install a pool cleanup handler to delete the file */
      SVN_ERR (temp_file_cleanup_register (b->empty_file, b->pool));
    }

  *empty_file = b->empty_file;

  return SVN_NO_ERROR;
}

/* Runs the diff callback to display the difference for a single file. At
 * this stage both versions of the file exist as temporary files. 
 */
static svn_error_t *
run_diff_cmd (struct file_baton *b)
{
  svn_wc_diff_cmd_t diff_cmd = b->edit_baton->diff_cmd;

  SVN_ERR (diff_cmd (b->path_start_revision, b->path_end_revision, b->path,
                     b->edit_baton->diff_cmd_baton));

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function. The root of the comparison
 * hierarchy
 */
static svn_error_t *
set_target_revision (void *edit_baton, svn_revnum_t target_revision)
{
  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function. The root of the comparison
 * hierarchy
 */
static svn_error_t *
open_root (void *edit_baton,
           svn_revnum_t base_revision,
           void **root_baton)
{
  struct edit_baton *eb = edit_baton;
  struct dir_baton *b;

  b = make_dir_baton (NULL, NULL, eb, FALSE, eb->pool);
  *root_baton = b;

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function.
 */
static svn_error_t *
delete_entry (svn_stringbuf_t *name,
              svn_revnum_t base_revision,
              void *parent_baton)
{
  struct dir_baton *pb = parent_baton;
  apr_pool_t *pool = svn_pool_create (pb->pool);
  svn_stringbuf_t *path = svn_stringbuf_dup (pb->path, pool);
  svn_node_kind_t kind;

  svn_path_add_component (path, name);

  /* We need to know if this is a directory or a file */
  SVN_ERR (pb->edit_baton->ra_lib->check_path (&kind,
                                               pb->edit_baton->ra_session,
                                               path->data,
                                               pb->edit_baton->revision));

  switch (kind)
    {
    case svn_node_file:
      {
        /* Compare a file being deleted against an empty file */
        struct file_baton *b = make_file_baton (name, FALSE, pb);
        SVN_ERR (get_file_from_ra (b));
        SVN_ERR (get_empty_file(b->edit_baton, &b->path_end_revision));
        SVN_ERR (run_diff_cmd (b));
        svn_pool_destroy (b->pool);
      }
      break;
    case svn_node_dir:
      /* ### TODO: need to get the directory entries to show
         deleted files */
      break;
    default:
      break;
    }

  svn_pool_destroy (pool);

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function.
 */
static svn_error_t *
add_directory (svn_stringbuf_t *name,
               void *parent_baton,
               svn_stringbuf_t *copyfrom_path,
               svn_revnum_t copyfrom_revision,
               void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct dir_baton *b;

  /* ### TODO: support copyfrom? */

  b = make_dir_baton (name, pb, pb->edit_baton, TRUE, pb->pool);
  *child_baton = b;

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function.
 */
static svn_error_t *
open_directory (svn_stringbuf_t *name,
                void *parent_baton,
                svn_revnum_t base_revision,
                void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct dir_baton *b;

  b = make_dir_baton (name, pb, pb->edit_baton, FALSE, pb->pool);
  *child_baton = b;

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function.  When a directory is closed,
 * all the directory elements that have been added or replaced will already
 * have been diff'd. However there may be other elements in the working
 * copy that have not yet been considered.
 */
static svn_error_t *
close_directory (void *dir_baton)
{
  struct dir_baton *b = dir_baton;

  svn_pool_destroy (b->pool);

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function.
 */
static svn_error_t *
add_file (svn_stringbuf_t *name,
          void *parent_baton,
          svn_stringbuf_t *copyfrom_path,
          svn_revnum_t copyfrom_revision,
          void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *b;

  /* ### TODO: support copyfrom? */

  b = make_file_baton (name, TRUE, pb);
  *file_baton = b;

  SVN_ERR (get_empty_file (b->edit_baton, &b->path_start_revision));

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function.
 */
static svn_error_t *
open_file (svn_stringbuf_t *name,
           void *parent_baton,
           svn_revnum_t base_revision,
           void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *b;

  b = make_file_baton (name, FALSE, pb);
  *file_baton = b;

  SVN_ERR (get_file_from_ra (b));

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function.  Do the work of applying the
 * text delta.
 */
static svn_error_t *
window_handler (svn_txdelta_window_t *window,
                void *window_baton)
{
  struct file_baton *b = window_baton;

  SVN_ERR (b->apply_handler (window, b->apply_baton));

  if (!window)
    {
      apr_status_t status;

      status = apr_file_close (b->file_start_revision);
      if (status)
        return svn_error_createf (status, 0, NULL, b->pool,
                                  "failed to close file '%s'",
                                  b->path_start_revision->data);

      status = apr_file_close (b->file_end_revision);
      if (status)
        return svn_error_createf (status, 0, NULL, b->pool,
                                  "failed to close file '%s'",
                                  b->path_end_revision->data);
    }

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function.
 */
static svn_error_t *
apply_textdelta (void *file_baton,
                 svn_txdelta_window_handler_t *handler,
                 void **handler_baton)
{
  struct file_baton *b = file_baton;
  apr_status_t status;

  /* Open the file to be used as the base for second revision */
  status = apr_file_open (&b->file_start_revision, b->path_start_revision->data,
                          APR_READ, APR_OS_DEFAULT, b->pool);
  if (status)
    return svn_error_createf (status, 0, NULL, b->pool,
                              "failed to open file '%s'",
                              b->path_start_revision->data);

  /* Open the file that will become the second revision after applying the
     text delta, it starts empty */
  SVN_ERR (create_empty_file (&b->path_end_revision, b->pool));
  SVN_ERR (temp_file_cleanup_register (b->path_end_revision, b->pool));
  status = apr_file_open (&b->file_end_revision, b->path_end_revision->data,
                          APR_WRITE, APR_OS_DEFAULT, b->pool);
  if (status)
    return svn_error_createf (status, 0, NULL, b->pool,
                              "failed to open file '%s'",
                              b->path_end_revision->data);

  svn_txdelta_apply (svn_stream_from_aprfile (b->file_start_revision, b->pool),
                     svn_stream_from_aprfile (b->file_end_revision, b->pool),
                     b->pool,
                     &b->apply_handler, &b->apply_baton);

  *handler = window_handler;
  *handler_baton = file_baton;

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function.  When the file is closed we
 * have a temporary file containing a pristine version of the repository
 * file. This can be compared against the working copy.
 */
static svn_error_t *
close_file (void *file_baton)
{
  struct file_baton *b = file_baton;

  /* Maybe only the properties changed, not the text content.  In that
     case, no difference is reported, and no need to run diff.
     ### todo: of course, if some day we have an extended diff format
     that shows property differences, then there will be appropriate
     changes to the file_baton, and probably more code here.  */
  if (b->path_end_revision)
    SVN_ERR (run_diff_cmd (b));

  svn_pool_destroy (b->pool);

  return SVN_NO_ERROR;
}

/* An svn_delta_edit_fns_t editor function.
 */
static svn_error_t *
close_edit (void *edit_baton)
{
  struct edit_baton *eb = edit_baton;

  svn_pool_destroy (eb->pool);

  return SVN_NO_ERROR;
}

/* Create a repository diff editor and baton.
 */
svn_error_t *
svn_client__get_diff_editor (svn_stringbuf_t *target,
                             svn_wc_diff_cmd_t diff_cmd,
                             void *diff_cmd_baton,
                             svn_boolean_t recurse,
                             svn_ra_plugin_t *ra_lib,
                             void *ra_session,
                             svn_revnum_t revision,
                             const svn_delta_edit_fns_t **editor,
                             void **edit_baton,
                             apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  svn_delta_edit_fns_t *tree_editor = svn_delta_default_editor (subpool);
  struct edit_baton *eb = apr_palloc (subpool, sizeof (*eb));

  eb->target = target;
  eb->diff_cmd = diff_cmd;
  eb->diff_cmd_baton = diff_cmd_baton;
  eb->recurse = recurse;
  eb->ra_lib = ra_lib;
  eb->ra_session = ra_session;
  eb->revision = revision;
  eb->empty_file = NULL;
  eb->pool = subpool;

  tree_editor->set_target_revision = set_target_revision;
  tree_editor->open_root = open_root;
  tree_editor->delete_entry = delete_entry;
  tree_editor->add_directory = add_directory;
  tree_editor->open_directory = open_directory;
  tree_editor->close_directory = close_directory;
  tree_editor->add_file = add_file;
  tree_editor->open_file = open_file;
  tree_editor->apply_textdelta = apply_textdelta;
  tree_editor->close_file = close_file;
  tree_editor->close_edit = close_edit;

  *edit_baton = eb;
  *editor = tree_editor;

  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: */
