/*
 * depth_filter_editor.c -- provide a svn_delta_editor_t which wraps
 *                          another editor and provides depth-based filtering
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

#include "svn_delta.h"


/*** Batons, and the Toys That Create Them ***/

struct edit_baton
{
  const svn_delta_editor_t *wrapped_editor;
  void *wrapped_edit_baton;
  svn_depth_t requested_depth;
  svn_boolean_t has_target;
  int actual_depth;
};

struct node_baton
{
  svn_boolean_t filtered;
  void *edit_baton;
  void *wrapped_baton;
};

/* Allocate and return a new node_baton structure, populated via the
   the input to this helper function. */
static struct node_baton *
make_node_baton(void *edit_baton,
                svn_boolean_t filtered,
                apr_pool_t *pool)
{
  struct node_baton *b = apr_palloc(pool, sizeof(*b));
  b->edit_baton = edit_baton;
  b->wrapped_baton = NULL;
  b->filtered = filtered;
  return b;
}

static svn_boolean_t 
okay_to_edit(struct edit_baton *eb,
             struct node_baton *pb,
             svn_node_kind_t kind)
{
  int effective_depth;

  if (pb->filtered)
    return FALSE;

  effective_depth = eb->actual_depth - (eb->has_target ? 1 : 0);
  switch (eb->requested_depth)
    {
    case svn_depth_infinity:
      return TRUE;
    case svn_depth_empty:
      return (kind == svn_node_dir && effective_depth <= 0);
    case svn_depth_files:
      return ((effective_depth <= 0)
              || (kind == svn_node_file && effective_depth == 1));
    case svn_depth_immediates:
      return (effective_depth <= 1);
    default:
      break;
    }
  abort();
}


/*** Editor Functions ***/

static svn_error_t *
set_target_revision(void *edit_baton,
                    svn_revnum_t target_revision,
                    apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;

  /* Nothing depth-y to filter here. */ 
 return eb->wrapped_editor->set_target_revision(eb->wrapped_edit_baton,
                                                target_revision, pool);
}

static svn_error_t *
open_root(void *edit_baton,
          svn_revnum_t base_revision,
          apr_pool_t *pool,
          void **root_baton)
{
  struct edit_baton *eb = edit_baton;
  struct node_baton *b;

  /* The root node always gets through cleanly. */
  b = make_node_baton(edit_baton, FALSE, pool);
  SVN_ERR(eb->wrapped_editor->open_root(eb->wrapped_edit_baton, base_revision,
                                        pool, &b->wrapped_baton));

  eb->actual_depth++;
  *root_baton = b;
  return SVN_NO_ERROR;
}

static svn_error_t *
delete_entry(const char *path,
             svn_revnum_t base_revision,
             void *parent_baton,
             apr_pool_t *pool)
{
  struct node_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;

  /* Don't delete children of filtered nodes. */
  if (pb->filtered)
    return SVN_NO_ERROR;

  /* ### FIXME: Because we don't know the type of the entry, we don't
     know if it's okay to really delete it or not.  For example, if
     we're filtering for depth=files and this is a delete of a
     subdirectory, we don't want to really delete the thing. */
  if (eb->requested_depth == svn_depth_infinity)
    SVN_ERR(eb->wrapped_editor->delete_entry(path, base_revision,
                                             pb->wrapped_baton, pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
add_directory(const char *path,
              void *parent_baton,
              const char *copyfrom_path,
              svn_revnum_t copyfrom_revision,
              apr_pool_t *pool,
              void **child_baton)
{
  struct node_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  struct node_baton *b = NULL;

  /* Check for sufficient depth. */
  if (okay_to_edit(eb, pb, svn_node_dir))
    {
      b = make_node_baton(eb, FALSE, pool);
      SVN_ERR(eb->wrapped_editor->add_directory(path, pb->wrapped_baton,
                                                copyfrom_path, 
                                                copyfrom_revision,
                                                pool, &b->wrapped_baton));
    }
  else
    {
      b = make_node_baton(eb, TRUE, pool);
    }

  eb->actual_depth++;
  *child_baton = b;
  return SVN_NO_ERROR;
}

static svn_error_t *
open_directory(const char *path,
               void *parent_baton,
               svn_revnum_t base_revision,
               apr_pool_t *pool,
               void **child_baton)
{
  struct node_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  struct node_baton *b;

  /* Check for sufficient depth. */
  if (okay_to_edit(eb, pb, svn_node_dir))
    {
      b = make_node_baton(eb, FALSE, pool);
      SVN_ERR(eb->wrapped_editor->open_directory(path, pb->wrapped_baton,
                                                 base_revision, pool,
                                                 &b->wrapped_baton));
    }
  else
    {
      b = make_node_baton(eb, TRUE, pool);
    }

  eb->actual_depth++;
  *child_baton = b;
  return SVN_NO_ERROR;
}

static svn_error_t *
add_file(const char *path,
         void *parent_baton,
         const char *copyfrom_path,
         svn_revnum_t copyfrom_revision,
         apr_pool_t *pool,
         void **child_baton)
{
  struct node_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  struct node_baton *b = NULL;

  /* Check for sufficient depth. */
  if (okay_to_edit(eb, pb, svn_node_file))
    {
      b = make_node_baton(eb, FALSE, pool);
      SVN_ERR(eb->wrapped_editor->add_file(path, pb->wrapped_baton,
                                           copyfrom_path, copyfrom_revision,
                                           pool, &b->wrapped_baton));
    }
  else
    {
      b = make_node_baton(eb, TRUE, pool);
    }

  *child_baton = b;
  return SVN_NO_ERROR;
}

static svn_error_t *
open_file(const char *path,
          void *parent_baton,
          svn_revnum_t base_revision,
          apr_pool_t *pool,
          void **child_baton)
{
  struct node_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  struct node_baton *b;

  /* Check for sufficient depth. */
  if (okay_to_edit(eb, pb, svn_node_file))
    {
      b = make_node_baton(eb, FALSE, pool);
      SVN_ERR(eb->wrapped_editor->open_file(path, pb->wrapped_baton,
                                            base_revision, pool,
                                            &b->wrapped_baton));
    }
  else
    {
      b = make_node_baton(eb, TRUE, pool);
    }

  *child_baton = b;
  return SVN_NO_ERROR;
}

static svn_error_t *
apply_textdelta(void *file_baton,
                const char *base_checksum,
                apr_pool_t *pool,
                svn_txdelta_window_handler_t *handler,
                void **handler_baton)
{
  struct node_baton *fb = file_baton;
  struct edit_baton *eb = fb->edit_baton;

  /* For filtered files, we just consume the textdelta. */
  if (fb->filtered)
    {
      *handler = svn_delta_noop_window_handler;
      *handler_baton = NULL;
    }
  else
    {
      SVN_ERR(eb->wrapped_editor->apply_textdelta(fb->wrapped_baton,
                                                  base_checksum, pool,
                                                  handler, handler_baton));
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
close_file(void *file_baton,
           const char *text_checksum,
           apr_pool_t *pool)
{
  struct node_baton *fb = file_baton;
  struct edit_baton *eb = fb->edit_baton;

  /* Don't close filtered files. */
  if (! fb->filtered)
    SVN_ERR(eb->wrapped_editor->close_file(fb->wrapped_baton, 
                                           text_checksum, pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
absent_file(const char *path,
            void *parent_baton,
            apr_pool_t *pool)
{
  struct node_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;

  /* Don't report absent items in filtered directories. */
  if (! pb->filtered)
    SVN_ERR(eb->wrapped_editor->absent_file(path, pb->wrapped_baton, pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
close_directory(void *dir_baton,
                apr_pool_t *pool)
{
  struct node_baton *db = dir_baton;
  struct edit_baton *eb = db->edit_baton;

  /* Don't close filtered directories. */
  if (! db->filtered)
    SVN_ERR(eb->wrapped_editor->close_directory(db->wrapped_baton, pool));

  eb->actual_depth--;
  return SVN_NO_ERROR;
}

static svn_error_t *
absent_directory(const char *path,
                 void *parent_baton,
                 apr_pool_t *pool)
{
  struct node_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;

  /* Don't report absent items in filtered directories. */
  if (! pb->filtered)
    SVN_ERR(eb->wrapped_editor->absent_directory(path, pb->wrapped_baton, 
                                                 pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
change_file_prop(void *file_baton,
                 const char *name,
                 const svn_string_t *value,
                 apr_pool_t *pool)
{
  struct node_baton *fb = file_baton;
  struct edit_baton *eb = fb->edit_baton;

  /* No propchanges on filtered files. */
  if (! fb->filtered)
    SVN_ERR(eb->wrapped_editor->change_file_prop(fb->wrapped_baton,
                                                 name, value, pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
change_dir_prop(void *dir_baton,
                const char *name,
                const svn_string_t *value,
                apr_pool_t *pool)
{
  struct node_baton *db = dir_baton;
  struct edit_baton *eb = db->edit_baton;

  /* No propchanges on filtered nodes. */
  if (! db->filtered)
    SVN_ERR(eb->wrapped_editor->change_dir_prop(db->wrapped_baton,
                                                name, value, pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
close_edit(void *edit_baton,
           apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;
  return eb->wrapped_editor->close_edit(eb->wrapped_edit_baton, pool);
}

svn_error_t *
svn_delta_depth_filter_editor(const svn_delta_editor_t **editor,
                              void **edit_baton,
                              const svn_delta_editor_t *wrapped_editor,
                              void *wrapped_edit_baton,
                              svn_depth_t requested_depth,
                              svn_boolean_t has_target,
                              apr_pool_t *pool)
{
  svn_delta_editor_t *depth_filter_editor;
  struct edit_baton *eb;

  /* Easy out: if the caller wants infinite depth, there's nothing to
     filter, so just return the editor we were supposed to wrap. */
  if (requested_depth == svn_depth_infinity)
    {
      *editor = wrapped_editor;
      *edit_baton = wrapped_edit_baton;
      return SVN_NO_ERROR;
    }

  depth_filter_editor = svn_delta_default_editor(pool);
  depth_filter_editor->set_target_revision = set_target_revision;
  depth_filter_editor->open_root = open_root;
  depth_filter_editor->delete_entry = delete_entry;
  depth_filter_editor->add_directory = add_directory;
  depth_filter_editor->open_directory = open_directory;
  depth_filter_editor->change_dir_prop = change_dir_prop;
  depth_filter_editor->close_directory = close_directory;
  depth_filter_editor->absent_directory = absent_directory;
  depth_filter_editor->add_file = add_file;
  depth_filter_editor->open_file = open_file;
  depth_filter_editor->apply_textdelta = apply_textdelta;
  depth_filter_editor->change_file_prop = change_file_prop;
  depth_filter_editor->close_file = close_file;
  depth_filter_editor->absent_file = absent_file;
  depth_filter_editor->close_edit = close_edit;

  eb = apr_palloc(pool, sizeof(*eb));
  eb->wrapped_editor = wrapped_editor;
  eb->wrapped_edit_baton = wrapped_edit_baton;
  eb->actual_depth = 0;
  eb->has_target = has_target;
  eb->requested_depth = requested_depth;

  *editor = depth_filter_editor;
  *edit_baton = eb;

  return SVN_NO_ERROR;
}
