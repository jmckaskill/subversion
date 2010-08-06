/*
 *  load_editor.c: The svn_delta_editor_t editor used by svnrdump to
 *  load revisions.
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

#include "svn_cmdline.h"
#include "svn_pools.h"
#include "svn_delta.h"
#include "svn_repos.h"
#include "svn_props.h"
#include "svn_path.h"
#include "svn_ra.h"
#include "svn_io.h"

#include "load_editor.h"

#ifdef SVN_DEBUG
#define LDR_DBG(x) SVN_DBG(x)
#else
#define LDR_DBG(x) while(0)
#endif

static svn_error_t *
commit_callback(const svn_commit_info_t *commit_info,
                void *baton,
                apr_pool_t *pool)
{
  SVN_ERR(svn_cmdline_printf(pool, "* Loaded revision %ld\n",
                             commit_info->revision));
  return SVN_NO_ERROR;
}


static svn_error_t *
new_revision_record(void **revision_baton,
		    apr_hash_t *headers,
		    void *parse_baton,
		    apr_pool_t *pool)
{
  struct revision_baton *rb;
  struct parse_baton *pb;
  apr_hash_index_t *hi;

  rb = apr_pcalloc(pool, sizeof(*rb));
  pb = parse_baton;
  rb->pool = svn_pool_create(pool);
  rb->pb = pb;

  for (hi = apr_hash_first(pool, headers); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      void *val;
      const char *hname, *hval;

      apr_hash_this(hi, &key, NULL, &val);
      hname = key;
      hval = val;

      if (strcmp(hname, SVN_REPOS_DUMPFILE_REVISION_NUMBER) == 0)
        rb->rev = atoi(hval);
    }

  /* Set the commit_editor/ commit_edit_baton to NULL and wait for
     them to be created in new_node_record */
  rb->pb->commit_editor = NULL;
  rb->pb->commit_edit_baton = NULL;
  rb->revprop_table = apr_hash_make(rb->pool);

  *revision_baton = rb;
  return SVN_NO_ERROR;
}

static svn_error_t *
uuid_record(const char *uuid,
            void *parse_baton,
            apr_pool_t *pool)
{
  struct parse_baton *pb;
  pb = parse_baton;
  pb->uuid = apr_pstrdup(pool, uuid);
  return SVN_NO_ERROR;
}

static svn_error_t *
new_node_record(void **node_baton,
                apr_hash_t *headers,
                void *revision_baton,
                apr_pool_t *pool)
{
  const struct svn_delta_editor_t *commit_editor;
  struct node_baton *nb;
  struct revision_baton *rb;
  struct directory_baton *child_db;
  apr_hash_index_t *hi;
  void *child_baton;
  void *commit_edit_baton;
  char *ancestor_path;
  apr_array_header_t *residual_path;
  int i;

  rb = revision_baton;
  nb = apr_pcalloc(rb->pool, sizeof(*nb));
  nb->rb = rb;

  nb->copyfrom_path = NULL;
  nb->copyfrom_rev = SVN_INVALID_REVNUM;

  commit_editor = rb->pb->commit_editor;
  commit_edit_baton = rb->pb->commit_edit_baton;

  /* If the creation of commit_editor is pending, create it now and
     open_root on it; also create a top-level directory baton. */
  if (!commit_editor) {
      SVN_ERR(svn_ra_get_commit_editor3(rb->pb->session, &commit_editor,
                                        &commit_edit_baton, rb->revprop_table,
                                        commit_callback, NULL, NULL, FALSE,
                                        rb->pool));

      rb->pb->commit_editor = commit_editor;
      rb->pb->commit_edit_baton = commit_edit_baton;

      SVN_ERR(commit_editor->open_root(commit_edit_baton, rb->rev - 1,
                                       rb->pool, &child_baton));

      LDR_DBG(("Opened root %p\n", child_baton));

      /* child_db corresponds to the root directory baton here */
      child_db = apr_pcalloc(rb->pool, sizeof(*child_db));
      child_db->baton = child_baton;
      child_db->depth = 0;
      child_db->parent = NULL;
      child_db->relpath = svn_relpath_canonicalize("/", rb->pool);
      rb->db = child_db;
  }

  for (hi = apr_hash_first(rb->pool, headers); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      void *val;
      const char *hname, *hval;

      apr_hash_this(hi, &key, NULL, &val);
      hname = key;
      hval = val;

      /* Parse the different kinds of headers we can encounter and
         stuff them into the node_baton for writing later */
      if (strcmp(hname, SVN_REPOS_DUMPFILE_NODE_PATH) == 0)
        nb->path = apr_pstrdup(rb->pool, hval);
      if (strcmp(hname, SVN_REPOS_DUMPFILE_NODE_KIND) == 0)
        nb->kind = strcmp(hval, "file") == 0 ? svn_node_file : svn_node_dir;
      if (strcmp(hname, SVN_REPOS_DUMPFILE_NODE_ACTION) == 0)
        {
          if (strcmp(hval, "add") == 0)
            nb->action = svn_node_action_add;
          if (strcmp(hval, "change") == 0)
            nb->action = svn_node_action_change;
          if (strcmp(hval, "delete") == 0)
            nb->action = svn_node_action_delete;
          if (strcmp(hval, "replace") == 0)
            nb->action = svn_node_action_replace;
        }
      if (strcmp(hname, SVN_REPOS_DUMPFILE_NODE_COPYFROM_REV) == 0)
        nb->copyfrom_rev = atoi(hval);
      if (strcmp(hname, SVN_REPOS_DUMPFILE_NODE_COPYFROM_PATH) == 0)
          nb->copyfrom_path =
            svn_path_url_add_component2(rb->pb->root_url,
                                        apr_pstrdup(rb->pool, hval),
                                        rb->pool);
    }

  if (svn_path_compare_paths(svn_relpath_dirname(nb->path, pool),
                             rb->db->baton) != 0)
    {
      /* Before attempting to handle the action, call open_directory
         for all the path components and set the directory baton
         accordingly */
      ancestor_path =
        svn_relpath_get_longest_ancestor(nb->path,
                                         rb->db->relpath, pool);
      residual_path =
        svn_path_decompose(svn_relpath_skip_ancestor(nb->path,
                                                     ancestor_path),
                           rb->pool);

      /* First close all as many directories as there are after
         skip_ancestor, and then open fresh directories */
      for (i = 0; i < residual_path->nelts; i ++)
        {
          /* Don't worry about destroying the actual rb->db object,
             since the pool we're using has the lifetime of one
             revision anyway */
          LDR_DBG(("Closing dir %p\n", rb->db->baton));
          SVN_ERR(commit_editor->close_directory(rb->db->baton, rb->pool));
          rb->db = rb->db->parent;
        }
        
      for (i = 0; i < residual_path->nelts; i ++)
        {
          SVN_ERR(commit_editor->open_directory(residual_path->elts + i,
                                                rb->db->baton,
                                                rb->rev - 1,
                                                rb->pool, &child_baton));
          LDR_DBG(("Opened dir %p\n", child_baton));
          child_db = apr_pcalloc(rb->pool, sizeof(*child_db));
          child_db->baton = child_baton;
          child_db->depth = rb->db->depth + 1;
          child_db->relpath = svn_relpath_join(rb->db->relpath,
                                               residual_path->elts + i,
                                               rb->pool);
          rb->db = child_db;
        }

    }

  switch (nb->action)
    {
    case svn_node_action_add:
      switch (nb->kind)
        {
        case svn_node_file:
          SVN_ERR(commit_editor->add_file(nb->path, rb->db->baton,
                                          nb->copyfrom_path,
                                          nb->copyfrom_rev,
                                          rb->pool, &(nb->file_baton)));
          LDR_DBG(("Adding file %s to dir %p as %p\n", nb->path, rb->db->baton, nb->file_baton));
          break;
        case svn_node_dir:
          SVN_ERR(commit_editor->add_directory(nb->path, rb->db->baton,
                                               nb->copyfrom_path,
                                               nb->copyfrom_rev,
                                               rb->pool, &child_baton));
          LDR_DBG(("Adding dir %s to dir %p as %p\n", nb->path, rb->db->baton, child_baton));
          child_db = apr_pcalloc(rb->pool, sizeof(*child_db));
          child_db->baton = child_baton;
          child_db->depth = rb->db->depth + 1;
          child_db->relpath = svn_relpath_join(rb->db->relpath,
                                               residual_path->elts + i,
                                               rb->pool);
          rb->db = child_db;
          break;
        default:
          break;
        }
    case svn_node_action_change:
      /* Handled in set_node_property/ delete_node_property */
      break;
    case svn_node_action_delete:
      switch (nb->kind)
        {
        case svn_node_file:
          LDR_DBG(("Deleting file %s in %p\n", nb->path, rb->db->baton));
          SVN_ERR(commit_editor->delete_entry(nb->path, rb->rev,
                                              rb->db->baton, rb->pool));
          break;
        case svn_node_dir:
          LDR_DBG(("Deleting dir %s in %p\n", nb->path, rb->db->baton));
          SVN_ERR(commit_editor->delete_entry(nb->path, rb->rev,
                                              rb->db->baton, rb->pool));
          break;
        default:
          break;
        }
    case svn_node_action_replace:
      /* Absent in dumpstream; represented as a delete + add */
      break;
    }

  *node_baton = nb;
  return SVN_NO_ERROR;
}

static svn_error_t *
set_revision_property(void *baton,
                      const char *name,
                      const svn_string_t *value)
{
  struct revision_baton *rb;
  rb = baton;

  if (rb->rev > 0)
    apr_hash_set(rb->revprop_table, apr_pstrdup(rb->pool, name),
                 APR_HASH_KEY_STRING, svn_string_dup(value, rb->pool));
  else
    /* Special handling for revision 0; this is safe because the
       commit_editor hasn't been created yet. */
    svn_ra_change_rev_prop(rb->pb->session, rb->rev, name, value,
                           rb->pool);

  /* Remember any datestamp/ author that passes through (see comment
     in close_revision). */
  if (!strcmp(name, SVN_PROP_REVISION_DATE))
    rb->datestamp = svn_string_dup(value, rb->pool);
  if (!strcmp(name, SVN_PROP_REVISION_AUTHOR))
    rb->author = svn_string_dup(value, rb->pool);

  return SVN_NO_ERROR;
}

static svn_error_t *
set_node_property(void *baton,
                  const char *name,
                  const svn_string_t *value)
{
  struct node_baton *nb;
  const struct svn_delta_editor_t *commit_editor;
  apr_pool_t *pool;
  nb = baton;
  commit_editor = nb->rb->pb->commit_editor;
  pool = nb->rb->pool;

  LDR_DBG(("Applying properties on %p\n", nb->file_baton));
  if (nb->kind == svn_node_file)
    SVN_ERR(commit_editor->change_file_prop(nb->file_baton, name,
                                            value, pool));
  else
    SVN_ERR(commit_editor->change_dir_prop(nb->rb->db->baton, name,
                                           value, pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
delete_node_property(void *baton,
                     const char *name)
{
  struct node_baton *nb;
  const struct svn_delta_editor_t *commit_editor;
  apr_pool_t *pool;
  nb = baton;
  commit_editor = nb->rb->pb->commit_editor;
  pool = nb->rb->pool;

  if (nb->kind == svn_node_file)
    SVN_ERR(commit_editor->change_file_prop(nb->file_baton, name,
                                            NULL, pool));
  else
    SVN_ERR(commit_editor->change_dir_prop(nb->rb->db->baton, name,
                                           NULL, pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
remove_node_props(void *baton)
{
  /* ### Not implemented */
  return SVN_NO_ERROR;
}

static svn_error_t *
set_fulltext(svn_stream_t **stream,
             void *node_baton)
{
  /* ### Not implemented */
  return SVN_NO_ERROR;
}

static svn_error_t *
apply_textdelta(svn_txdelta_window_handler_t *handler,
                void **handler_baton,
                void *node_baton)
{
  struct node_baton *nb;
  const struct svn_delta_editor_t *commit_editor;
  apr_pool_t *pool;
  nb = node_baton;
  commit_editor = nb->rb->pb->commit_editor;
  pool = nb->rb->pool;
  SVN_ERR(commit_editor->apply_textdelta(nb->file_baton, NULL /* base_checksum */, 
                                         pool, handler, handler_baton));
  SVN_ERR(commit_editor->close_file(nb->file_baton, NULL, pool));
  LDR_DBG(("Closing file %p\n", nb->file_baton));

  return SVN_NO_ERROR;
}

static svn_error_t *
close_node(void *baton)
{
  /* Nothing to do */
  return SVN_NO_ERROR;
}

static svn_error_t *
close_revision(void *baton)
{
  struct revision_baton *rb;
  const svn_delta_editor_t *commit_editor;
  void *commit_edit_baton;
  rb = baton;

  commit_editor = rb->pb->commit_editor;
  commit_edit_baton = rb->pb->commit_edit_baton;

  /* r0 doesn't have a corresponding commit_editor; we fake it */
  if (rb->rev == 0)
    SVN_ERR(svn_cmdline_printf(rb->pool, "* Loaded revision 0\n"));
  else {
    /* Close all pending open directories, and then close the edit
       session itself */
    while (rb->db && rb->db->parent)
      {
        LDR_DBG(("Closing dir %p\n", rb->db->baton));
        SVN_ERR(commit_editor->close_directory(rb->db->baton, rb->pool));
        rb->db = rb->db->parent;
      }
    LDR_DBG(("Closing edit on %p\n", commit_edit_baton));
    SVN_ERR(commit_editor->close_edit(commit_edit_baton, rb->pool));
  }

  /* svn_fs_commit_txn rewrites the datestamp/ author property-
     rewrite it by hand after closing the commit_editor. */
  SVN_ERR(svn_ra_change_rev_prop(rb->pb->session, rb->rev,
                                 SVN_PROP_REVISION_DATE,
                                 rb->datestamp, rb->pool));
  SVN_ERR(svn_ra_change_rev_prop(rb->pb->session, rb->rev,
                                 SVN_PROP_REVISION_AUTHOR,
                                 rb->author, rb->pool));

  svn_pool_destroy(rb->pool);

  return SVN_NO_ERROR;
}

svn_error_t *
get_dumpstream_loader(const svn_repos_parse_fns2_t **parser,
                      void **parse_baton,
                      svn_ra_session_t *session,
                      apr_pool_t *pool)
{
  svn_repos_parse_fns2_t *pf;
  struct parse_baton *pb;

  pf = apr_pcalloc(pool, sizeof(*pf));
  pf->new_revision_record = new_revision_record;
  pf->uuid_record = uuid_record;
  pf->new_node_record = new_node_record;
  pf->set_revision_property = set_revision_property;
  pf->set_node_property = set_node_property;
  pf->delete_node_property = delete_node_property;
  pf->remove_node_props = remove_node_props;
  pf->set_fulltext = set_fulltext;
  pf->apply_textdelta = apply_textdelta;
  pf->close_node = close_node;
  pf->close_revision = close_revision;

  pb = apr_pcalloc(pool, sizeof(*pb));
  pb->session = session;

  *parser = pf;
  *parse_baton = pb;

  return SVN_NO_ERROR;
}

svn_error_t *
drive_dumpstream_loader(svn_stream_t *stream,
                        const svn_repos_parse_fns2_t *parser,
                        void *parse_baton,
                        svn_ra_session_t *session,
                        apr_pool_t *pool)
{
  struct parse_baton *pb;
  pb = parse_baton;

  SVN_ERR(svn_ra_get_repos_root2(session, &(pb->root_url), pool));
  SVN_ERR(svn_repos_parse_dumpstream2(stream, parser, parse_baton,
                                      NULL, NULL, pool));

  return SVN_NO_ERROR;
}
