/*
 * diff.c: comparing and merging
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
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



/*** Includes. ***/

#include <apr_strings.h>
#include <apr_pools.h>
#include <apr_hash.h>
#include "svn_wc.h"
#include "svn_delta.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_test.h"
#include "svn_io.h"
#include "svn_pools.h"
#include "client.h"
#include <assert.h>


/*-----------------------------------------------------------------*/

/* Utilities */

/* A helper func that writes out verbal descriptions of property diffs
   to FILE.   Of course, the apr_file_t will probably be the 'outfile'
   passed to svn_client_diff, which is probably stdout. */
static svn_error_t *
display_prop_diffs (const apr_array_header_t *propchanges,
                    apr_hash_t *original_props,
                    const char *path,
                    apr_file_t *file,
                    apr_pool_t *pool)
{
  int i;

  apr_file_printf (file, "\nProperty changes on: %s\n", path);
  apr_file_printf (file,
     "___________________________________________________________________\n");

  for (i = 0; i < propchanges->nelts; i++)
    {
      const svn_prop_t *propchange
        = &APR_ARRAY_IDX(propchanges, i, svn_prop_t);

      const svn_stringbuf_t *original_value;

      if (original_props)
        original_value =
          apr_hash_get (original_props, propchange->name, APR_HASH_KEY_STRING);
      else
        original_value = NULL;

      apr_file_printf (file, "Name: %s\n", propchange->name);

      if (original_value != NULL)
        apr_file_printf (file, "   - %s\n", original_value->data);

      if (propchange->value != NULL)
        apr_file_printf (file, "   + %s\n", propchange->value->data);
    }

  apr_file_printf (file, "\n");

  return SVN_NO_ERROR;
}




/*-----------------------------------------------------------------*/

/*** Callbacks for 'svn diff', invoked by the repos-diff editor. ***/


struct diff_cmd_baton {
  const apr_array_header_t *options;
  apr_pool_t *pool;
  apr_file_t *outfile;
  apr_file_t *errfile;
};


/* The main workhorse, which invokes an external 'diff' program on the
   two temporary files.   The path is the "true" label to use in the
   diff output, and the revnums are ignored. */
static svn_error_t *
diff_file_changed (const char *path,
                   const char *tmpfile1,
                   const char *tmpfile2,
                   svn_revnum_t rev1,
                   svn_revnum_t rev2,
                   void *diff_baton)
{
  struct diff_cmd_baton *diff_cmd_baton = diff_baton;
  const char **args = NULL;
  int nargs, exitcode;
  apr_file_t *outfile = diff_cmd_baton->outfile;
  apr_file_t *errfile = diff_cmd_baton->errfile;
  apr_pool_t *subpool = svn_pool_create (diff_cmd_baton->pool);
  const char *label = path;

  /* Execute local diff command on these two paths, print to stdout. */
  nargs = diff_cmd_baton->options->nelts;
  if (nargs)
    {
      int i;
      args = apr_palloc (subpool, nargs * sizeof (char *));
      for (i = 0; i < diff_cmd_baton->options->nelts; i++)
        {
          args[i] =
            ((svn_stringbuf_t **)(diff_cmd_baton->options->elts))[i]->data;
        }
      assert (i == nargs);
    }

  /* Print out the diff header. */
  apr_file_printf (outfile, "Index: %s\n", label ? label : tmpfile1);
  apr_file_printf (outfile,
     "===================================================================\n");

  SVN_ERR (svn_io_run_diff (".", args, nargs, path,
                            tmpfile1, tmpfile2,
                            &exitcode, outfile, errfile, subpool));

  /* ### todo: Handle exit code == 2 (i.e. errors with diff) here */

  /* ### todo: someday we'll need to worry about whether we're going
     to need to write a diff plug-in mechanism that makes use of the
     two paths, instead of just blindly running SVN_CLIENT_DIFF.  */

  /* Destroy the subpool. */
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}

/* The because the repos-diff editor passes at least one empty file to
   each of these next two functions, they can be dumb wrappers around
   the main workhorse routine. */
static svn_error_t *
diff_file_added (const char *path,
                 const char *tmpfile1,
                 const char *tmpfile2,
                 void *diff_baton)
{
  return diff_file_changed (path, tmpfile1, tmpfile2,
                            SVN_INVALID_REVNUM, SVN_INVALID_REVNUM,
                            diff_baton);
}

static svn_error_t *
diff_file_deleted (const char *path,
                   const char *tmpfile1,
                   const char *tmpfile2,
                   void *diff_baton)
{
  return diff_file_changed (path, tmpfile1, tmpfile2,
                            SVN_INVALID_REVNUM, SVN_INVALID_REVNUM,
                            diff_baton);
}

/* For now, let's have 'svn diff' send feedback to the top-level
   application, so that something reasonable about directories and
   propsets gets printed to stdout. */
static svn_error_t *
diff_dir_added (const char *path,
                void *diff_baton)
{
  /* ### todo:  send feedback to app */
  return SVN_NO_ERROR;
}

static svn_error_t *
diff_dir_deleted (const char *path,
                  void *diff_baton)
{
  /* ### todo:  send feedback to app */
  return SVN_NO_ERROR;
}

static svn_error_t *
diff_props_changed (const char *path,
                    const apr_array_header_t *propchanges,
                    apr_hash_t *original_props,
                    void *diff_baton)
{
  struct diff_cmd_baton *diff_cmd_baton = diff_baton;
  apr_array_header_t *entry_props, *wc_props, *regular_props;
  apr_pool_t *subpool = svn_pool_create (diff_cmd_baton->pool);

  SVN_ERR (svn_categorize_props (propchanges,
                                 &entry_props, &wc_props, &regular_props,
                                 subpool));

  if (regular_props->nelts > 0)
    SVN_ERR (display_prop_diffs (regular_props,
                                 original_props,
                                 path,
                                 diff_cmd_baton->outfile,
                                 subpool));

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}

/* The main callback table for 'svn diff'.  */
static const svn_diff_callbacks_t
diff_callbacks =
  {
    diff_file_changed,
    diff_file_added,
    diff_file_deleted,
    diff_dir_added,
    diff_dir_deleted,
    diff_props_changed
  };


/*-----------------------------------------------------------------*/

/*** Callbacks for 'svn merge', invoked by the repos-diff editor. ***/


struct merge_cmd_baton {
  svn_boolean_t force;
  apr_pool_t *pool;
};


static svn_error_t *
merge_file_changed (const char *mine,
                    const char *older,
                    const char *yours,
                    svn_revnum_t older_rev,
                    svn_revnum_t yours_rev,
                    void *baton)
{
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create (merge_b->pool);
  const char *target_label = ".working";
  const char *left_label = apr_psprintf (subpool, ".r%" SVN_REVNUM_T_FMT,
                                         older_rev);
  const char *right_label = apr_psprintf (subpool, ".r%" SVN_REVNUM_T_FMT,
                                          yours_rev);
  svn_error_t *err;

  /* This callback is essentially no more than a wrapper around
     svn_wc_merge().  Thank goodness that all the
     diff-editor-mechanisms are doing the hard work of getting the
     fulltexts! */

  err = svn_wc_merge (older, yours, mine,
                      left_label, right_label, target_label,
                      subpool);
  if (err && (err->apr_err != SVN_ERR_WC_CONFLICT))
    return err;

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}

static svn_error_t *
merge_file_added (const char *mine,
                  const char *older,
                  const char *yours,
                  void *baton)
{
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create (merge_b->pool);
  enum svn_node_kind kind;

  SVN_ERR (svn_io_check_path (mine, &kind, subpool));
  switch (kind)
    {
    case svn_node_none:
      SVN_ERR (svn_io_copy_file (yours, mine, TRUE, subpool));
      SVN_ERR (svn_client_add (svn_stringbuf_create (mine, subpool),
                               FALSE, NULL, NULL, subpool));
      break;
    case svn_node_dir:
      /* ### create a .drej conflict or something someday? */
      return svn_error_createf (SVN_ERR_WC_NOT_FILE, 0, NULL, subpool,
                                "Cannot create file '%s' for addition, "
                                "because a directory by that name "
                                "already exists.", mine);
    case svn_node_file:
      {
        /* file already exists, is it under version control? */
        svn_wc_entry_t *entry;
        SVN_ERR (svn_wc_entry (&entry, svn_stringbuf_create (mine, subpool),
                               FALSE, subpool));
        if (entry)
          {
            if (entry->schedule == svn_wc_schedule_delete)
              {
                /* If already scheduled for deletion, then carry out
                   an add, which is really a (R)eplacement.  */
                SVN_ERR (svn_io_copy_file (yours, mine, TRUE, subpool));
                SVN_ERR (svn_client_add (svn_stringbuf_create (mine, subpool),
                                         FALSE, NULL, NULL, subpool));
              }
            else
              {
                svn_error_t *err;
                err = svn_wc_merge (older, yours, mine,
                                    ".older", ".yours", ".working", /* ###? */
                                    subpool);
                if (err && (err->apr_err != SVN_ERR_WC_CONFLICT))
                  return err;
              }
          }
        else
          return svn_error_createf (SVN_ERR_WC_OBSTRUCTED_UPDATE, 0, NULL,
                                    subpool,
                                    "Cannot create file '%s' for addition, "
                                    "because an unversioned file by that name "
                                    "already exists.", mine);
        break;
      }
    default:
      break;
    }

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}

static svn_error_t *
merge_file_deleted (const char *mine,
                    const char *older,
                    const char *yours,
                    void *baton)
{
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create (merge_b->pool);
  enum svn_node_kind kind;

  SVN_ERR (svn_io_check_path (mine, &kind, subpool));
  switch (kind)
    {
    case svn_node_file:
      SVN_ERR (svn_client_delete (NULL,
                                  svn_stringbuf_create (mine, subpool),
                                  merge_b->force,
                                  NULL, NULL, NULL, NULL, NULL, subpool));
      break;
    case svn_node_dir:
      /* ### create a .drej conflict or something someday? */
      return svn_error_createf (SVN_ERR_WC_NOT_FILE, 0, NULL, subpool,
                                "Cannot schedule file '%s' for deletion, "
                                "because a directory by that name "
                                "already exists.", mine);
    case svn_node_none:
      /* file is already non-existent, this is a no-op. */
      break;
    default:
      break;
    }

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}

static svn_error_t *
merge_dir_added (const char *path,
                 void *baton)
{
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create (merge_b->pool);
  svn_stringbuf_t *path_s = svn_stringbuf_create (path, subpool);
  enum svn_node_kind kind;
  svn_boolean_t is_wc;

  SVN_ERR (svn_io_check_path (path, &kind, subpool));
  switch (kind)
    {
    case svn_node_none:
      SVN_ERR (svn_client_mkdir (NULL, path_s, NULL, NULL, NULL,
                                 NULL, NULL, subpool));
      break;
    case svn_node_dir:
      /* dir already exists.  make sure it's under version control. */
      SVN_ERR (svn_wc_check_wc (path_s, &is_wc, subpool));
      if (! is_wc)
        SVN_ERR (svn_client_add (path_s, FALSE, NULL, NULL, subpool));
      break;
    case svn_node_file:
      /* ### create a .drej conflict or something someday? */
      return svn_error_createf (SVN_ERR_WC_NOT_DIRECTORY, 0, NULL, subpool,
                                "Cannot create directory '%s' for addition, "
                                "because a file by that name "
                                "already exists.", path);
      break;
    default:
      break;
    }

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}

static svn_error_t *
merge_dir_deleted (const char *path,
                   void *baton)
{
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create (merge_b->pool);
  enum svn_node_kind kind;

  SVN_ERR (svn_io_check_path (path, &kind, subpool));
  switch (kind)
    {
    case svn_node_dir:
      SVN_ERR (svn_client_delete (NULL,
                                  svn_stringbuf_create (path, subpool),
                                  merge_b->force,
                                  NULL, NULL, NULL, NULL, NULL, subpool));
      break;
    case svn_node_file:
      /* ### create a .drej conflict or something someday? */
      return svn_error_createf (SVN_ERR_WC_NOT_DIRECTORY, 0, NULL, subpool,
                                "Cannot schedule directory '%s' for deletion, "
                                "because a file by that name "
                                "already exists.", path);
    case svn_node_none:
      /* dir is already non-existent, this is a no-op. */
      break;
    default:
      break;
    }

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}

static svn_error_t *
merge_props_changed (const char *path,
                     const apr_array_header_t *propchanges,
                     apr_hash_t *original_props,
                     void *baton)
{
  apr_array_header_t *entry_props, *wc_props, *regular_props;
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create (merge_b->pool);

  SVN_ERR (svn_categorize_props (propchanges,
                                 &entry_props, &wc_props, &regular_props,
                                 subpool));

  /* We only want to merge "regular" version properties:  by
     definition, 'svn merge' shouldn't touch any data within .svn/  */
  if (regular_props)
    SVN_ERR (svn_wc_merge_prop_diffs (path, regular_props, subpool));

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}

/* The main callback table for 'svn merge'.  */
static const svn_diff_callbacks_t
merge_callbacks =
  {
    merge_file_changed,
    merge_file_added,
    merge_file_deleted,
    merge_dir_added,
    merge_dir_deleted,
    merge_props_changed
  };


/*-----------------------------------------------------------------------*/

/** The logic behind 'svn diff' and 'svn merge'.  */


/* Hi!  This is a comment left behind by Karl, and Ben is too afraid
   to erase it at this time, because he's not fully confident that all
   this knowledge has been grokked yet.

   There are five cases:
      1. path is not an URL and start_revision != end_revision
      2. path is not an URL and start_revision == end_revision
      3. path is an URL and start_revision != end_revision
      4. path is an URL and start_revision == end_revision
      5. path is not an URL and no revisions given

   With only one distinct revision the working copy provides the
   other.  When path is an URL there is no working copy. Thus

     1: compare repository versions for URL coresponding to working copy
     2: compare working copy against repository version
     3: compare repository versions for URL
     4: nothing to do.
     5: compare working copy against text-base

   Case 4 is not as stupid as it looks, for example it may occur if
   the user specifies two dates that resolve to the same revision.  */




/* Helper function: given a working-copy PATH, return its associated
   url in *URL, allocated in POOL.  If PATH is *already* a URL, that's
   fine, just set *URL = PATH. */
static svn_error_t *
convert_to_url (svn_stringbuf_t **url,
                svn_stringbuf_t *path,
                apr_pool_t *pool)
{
  svn_string_t path_str;
  svn_boolean_t path_is_url;

  path_str.data = path->data;
  path_str.len = path->len;

  path_is_url = svn_path_is_url (&path_str);

  if (path_is_url)
    *url = path;
  else
    {
      svn_wc_entry_t *entry;
      SVN_ERR (svn_wc_entry (&entry, path, FALSE, pool));
      if (entry)
        *url = svn_stringbuf_dup (entry->url, pool);
      else
        return svn_error_createf
          (SVN_ERR_ENTRY_NOT_FOUND, 0, NULL, pool,
           "convert_to_url: %s is not versioned", path->data);
    }

  return SVN_NO_ERROR;
}



/* PATH1, PATH2, and TARGET_WCPATH all better be directories.   For
   the single file case, the caller do the merging manually. */
static svn_error_t *
do_merge (const svn_delta_editor_t *after_editor,
          void *after_edit_baton,
          svn_client_auth_baton_t *auth_baton,
          svn_stringbuf_t *path1,
          const svn_client_revision_t *revision1,
          svn_stringbuf_t *path2,
          const svn_client_revision_t *revision2,
          svn_stringbuf_t *target_wcpath,
          svn_boolean_t recurse,
          const svn_diff_callbacks_t *callbacks,
          void *callback_baton,
          apr_pool_t *pool)
{
  svn_revnum_t start_revnum, end_revnum;
  svn_stringbuf_t *URL1, *URL2;
  void *ra_baton, *session, *session2;
  svn_ra_plugin_t *ra_lib;
  const svn_ra_reporter_t *reporter;
  void *report_baton;
  const svn_delta_edit_fns_t *diff_editor;
  const svn_delta_editor_t *composed_editor, *new_diff_editor;
  void *diff_edit_baton, *composed_edit_baton, *new_diff_edit_baton;

  /* Sanity check -- ensure that we have valid revisions to look at. */
  if ((revision1->kind == svn_client_revision_unspecified)
      || (revision2->kind == svn_client_revision_unspecified))
    {
      return svn_error_create
        (SVN_ERR_CLIENT_BAD_REVISION, 0, NULL, pool,
         "do_merge: caller failed to specify all revisions");
    }

  /* Make sure we have two URLs ready to go.*/
  SVN_ERR (convert_to_url (&URL1, path1, pool));
  SVN_ERR (convert_to_url (&URL2, path2, pool));

  /* Establish first RA session to URL1. */
  SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, URL1->data, pool));
  SVN_ERR (svn_client__open_ra_session (&session, ra_lib, URL1, NULL,
                                        NULL, FALSE, FALSE, TRUE,
                                        auth_baton, pool));
  /* Resolve the revision numbers. */
  SVN_ERR (svn_client__get_revision_number
           (&start_revnum, ra_lib, session, revision1, path1->data, pool));
  SVN_ERR (svn_client__get_revision_number
           (&end_revnum, ra_lib, session, revision2, path2->data, pool));

  /* Open a second session used to request individual file
     contents. Although a session can be used for multiple requests, it
     appears that they must be sequential. Since the first request, for
     the diff, is still being processed the first session cannot be
     reused. This applies to ra_dav, ra_local does not appears to have
     this limitation. */
  SVN_ERR (svn_client__open_ra_session (&session2, ra_lib, URL1, NULL,
                                        NULL, FALSE, FALSE, TRUE,
                                        auth_baton, pool));

  SVN_ERR (svn_client__get_diff_editor (target_wcpath,
                                        callbacks,
                                        callback_baton,
                                        recurse,
                                        ra_lib, session2,
                                        start_revnum,
                                        &new_diff_editor,
                                        &new_diff_edit_baton,
                                        pool));

  if (after_editor)
    {
      svn_delta_compose_editors (&composed_editor, &composed_edit_baton,
                                 new_diff_editor, new_diff_edit_baton,
                                 after_editor, after_edit_baton, pool);
    }
  else
    {
      composed_editor = new_diff_editor;
      composed_edit_baton = new_diff_edit_baton;
    }

  /* ### Make composed editor look "old" style.  Remove someday. */
  svn_delta_compat_wrap (&diff_editor, &diff_edit_baton,
                         composed_editor, composed_edit_baton, pool);

  SVN_ERR (ra_lib->do_switch (session,
                              &reporter, &report_baton,
                              end_revnum,
                              NULL,
                              recurse,
                              URL2,
                              diff_editor, diff_edit_baton));

  SVN_ERR (reporter->set_path (report_baton, "", start_revnum));

  SVN_ERR (reporter->finish_report (report_baton));

  SVN_ERR (ra_lib->close (session2));

  SVN_ERR (ra_lib->close (session));

  return SVN_NO_ERROR;
}



/* The single-file, simplified version of do_merge. */
static svn_error_t *
do_single_file_merge (const svn_delta_editor_t *after_editor,
                      void *after_edit_baton,
                      svn_client_auth_baton_t *auth_baton,
                      svn_stringbuf_t *path1,
                      const svn_client_revision_t *revision1,
                      svn_stringbuf_t *path2,
                      const svn_client_revision_t *revision2,
                      svn_stringbuf_t *target_wcpath,
                      apr_pool_t *pool)
{
  apr_status_t status;
  svn_error_t *err;
  apr_file_t *fp1 = NULL, *fp2 = NULL;
  svn_stringbuf_t *tmpfile1, *tmpfile2, *URL1, *URL2;
  svn_stream_t *fstream1, *fstream2;
  const char *oldrev_str, *newrev_str;
  svn_revnum_t rev1, rev2;
  apr_hash_t *props1, *props2;
  apr_array_header_t *propchanges;
  void *ra_baton, *session1, *session2;
  svn_ra_plugin_t *ra_lib;

  props1 = apr_hash_make (pool);
  props2 = apr_hash_make (pool);

  /* Create two temporary files that contain the fulltexts of
     PATH1@REV1 and PATH2@REV2. */
  SVN_ERR (svn_io_open_unique_file (&fp1, &tmpfile1,
                                    target_wcpath->data, ".tmp",
                                    FALSE, pool));
  SVN_ERR (svn_io_open_unique_file (&fp2, &tmpfile2,
                                    target_wcpath->data, ".tmp",
                                    FALSE, pool));
  fstream1 = svn_stream_from_aprfile (fp1, pool);
  fstream2 = svn_stream_from_aprfile (fp2, pool);

  SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));

  SVN_ERR (convert_to_url (&URL1, path1, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, URL1->data, pool));
  SVN_ERR (svn_client__open_ra_session (&session1, ra_lib, URL1, NULL,
                                        NULL, FALSE, FALSE, TRUE,
                                        auth_baton, pool));
  SVN_ERR (svn_client__get_revision_number
           (&rev1, ra_lib, session1, revision1, path1->data, pool));
  SVN_ERR (ra_lib->get_file (session1, "", rev1, fstream1, NULL, &props1));
  SVN_ERR (ra_lib->close (session1));

  /* ### heh, funny.  we could be fetching two fulltexts from two
     *totally* different repositories here.  :-) */

  SVN_ERR (convert_to_url (&URL2, path2, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, URL2->data, pool));
  SVN_ERR (svn_client__open_ra_session (&session2, ra_lib, URL2, NULL,
                                        NULL, FALSE, FALSE, TRUE,
                                        auth_baton, pool));
  SVN_ERR (svn_client__get_revision_number
           (&rev2, ra_lib, session2, revision2, path2->data, pool));
  SVN_ERR (ra_lib->get_file (session2, "", rev2, fstream2, NULL, &props2));
  SVN_ERR (ra_lib->close (session2));

  status = apr_file_close (fp1);
  if (status)
    return svn_error_createf (status, 0, NULL, pool, "failed to close '%s'.",
                              tmpfile1->data);
  status = apr_file_close (fp2);
  if (status)
    return svn_error_createf (status, 0, NULL, pool, "failed to close '%s'.",
                              tmpfile2->data);

  /* Perform a 3-way merge between the temporary fulltexts and the
     current working file. */
  oldrev_str = apr_psprintf (pool, ".r%" SVN_REVNUM_T_FMT, rev1);
  newrev_str = apr_psprintf (pool, ".r%" SVN_REVNUM_T_FMT, rev2);
  err = svn_wc_merge (tmpfile1->data, tmpfile2->data,
                      target_wcpath->data,
                      oldrev_str, newrev_str, ".working", pool);
  if (err && (err->apr_err != SVN_ERR_WC_CONFLICT))
    return err;

  SVN_ERR (svn_io_remove_file (tmpfile1->data, pool));
  SVN_ERR (svn_io_remove_file (tmpfile2->data, pool));

  /* Deduce property diffs, and merge those too. */
  SVN_ERR (svn_wc_get_local_propchanges (&propchanges,
                                         props1, props2, pool));
  SVN_ERR (svn_wc_merge_prop_diffs (target_wcpath->data,
                                    propchanges, pool));

  /* Let's be nice and give feedback via the 'after' editor, just like
     the recursive-directory do_merge() routine would do, by virtue of
     being driven by dir_delta().  ### someday we'll use only the
     notification vtable for this, instead of a trace editor. */
  if (after_editor)
    {
      void *root_baton, *file_baton, *handler_baton;
      svn_stringbuf_t *parent, *basename;
      svn_txdelta_window_handler_t handler;
      svn_path_split (target_wcpath, &parent, &basename, pool);

      SVN_ERR (after_editor->open_root (after_edit_baton,
                                        SVN_INVALID_REVNUM, pool,
                                        &root_baton));
      SVN_ERR (after_editor->open_file (basename->data, root_baton,
                                        SVN_INVALID_REVNUM, pool,
                                        &file_baton));
      SVN_ERR (after_editor->apply_textdelta (file_baton,
                                              &handler, &handler_baton));
      if (propchanges->nelts > 0)
        {
          apr_array_header_t *entry_props, *wc_props, *regular_props;
          SVN_ERR (svn_categorize_props (propchanges,
                                         &entry_props,
                                         &wc_props,
                                         &regular_props,
                                         pool));

          /* ### this is so silly; we just want the trace-update editor
             to print a UU or _U here.... it ignores the prop. */
          if (regular_props->nelts > 0)
            SVN_ERR (after_editor->change_file_prop
                     (file_baton,
                      "foo",
                      svn_string_create ("foo", pool), pool));
        }
      SVN_ERR (after_editor->close_file (file_baton));
      SVN_ERR (after_editor->close_edit (after_edit_baton));
    }

  return SVN_NO_ERROR;
}


/* A Theoretical Note From Ben, regarding do_diff().

   This function is really svn_client_diff().  If you read the public
   API description for svn_client_diff, it sounds quite Grand.  It
   sounds really generalized and abstract and beautiful: that it will
   diff any two paths, be they working-copy paths or URLs, at any two
   revisions.

   Now, the *reality* is that we have exactly three 'tools' for doing
   diffing, and thus this routine is built around the use of the three
   tools.  Here they are, for clarity:

     - svn_wc_diff:  assumes both paths are the same wcpath.
                     compares wcpath@BASE vs. wcpath@WORKING

     - svn_wc_get_diff_editor:  compares some URL@REV vs. wcpath@WORKING

     - svn_client__get_diff_editor:  compares some URL1@REV1 vs. URL2@REV2

   So the truth of the matter is, if the caller's arguments can't be
   pigeonholed into one of these three use-cases, we currently bail
   with a friendly apology.

   Perhaps someday a brave soul will truly make svn_client_diff
   perfectly general.  For now, we live with the 90% case.  Certainly,
   the commandline client only calls this function in legal ways.
   When there are other users of svn_client.h, maybe this will become
   a more pressing issue.
 */
static svn_error_t *
do_diff (const apr_array_header_t *options,
         svn_client_auth_baton_t *auth_baton,
         svn_stringbuf_t *path1,
         const svn_client_revision_t *revision1,
         svn_stringbuf_t *path2,
         const svn_client_revision_t *revision2,
         svn_boolean_t recurse,
         const svn_diff_callbacks_t *callbacks,
         void *callback_baton,
         apr_pool_t *pool)
{
  svn_error_t *err;
  svn_revnum_t start_revnum, end_revnum;
  svn_string_t path_str;
  svn_stringbuf_t *anchor = NULL, *target = NULL;
  svn_wc_entry_t *entry;
  void *ra_baton, *session;
  svn_ra_plugin_t *ra_lib;
  const svn_ra_reporter_t *reporter;
  void *report_baton;
  const svn_delta_edit_fns_t *diff_editor;
  void *diff_edit_baton;

  /* Sanity check -- ensure that we have valid revisions to look at. */
  if ((revision1->kind == svn_client_revision_unspecified)
      || (revision2->kind == svn_client_revision_unspecified))
    return svn_error_create (SVN_ERR_CLIENT_BAD_REVISION, 0, NULL, pool,
                             "do_diff: not all revisions are specified.");

  /* The simplest use-case.  No repository contact required. */
  if ((revision1->kind == svn_client_revision_base)
      && (revision2->kind == svn_client_revision_working))
    {
      /* Sanity check -- path1 and path2 are the same working-copy path. */
      if (! svn_stringbuf_compare (path1, path2))
        {
          err = svn_error_create (SVN_ERR_INCORRECT_PARAMS, 0, NULL, pool,
                                  "do_diff: paths aren't equal!");
          goto polite_error;
        }
      path_str.data = path1->data;
      path_str.len = path1->len;
      if (svn_path_is_url (&path_str))
        {
          return svn_error_create (SVN_ERR_INCORRECT_PARAMS, 0, NULL, pool,
                                   "do_diff: path isn't a working-copy path.");
          goto polite_error;
        }

      SVN_ERR (svn_wc_get_actual_target (path1, &anchor, &target, pool));
      SVN_ERR (svn_wc_entry (&entry, anchor, FALSE, pool));
      SVN_ERR (svn_wc_diff (anchor, target, callbacks, callback_baton,
                            recurse, pool));
    }

  /* Next use-case:  some repos-revision compared against wcpath@WORKING */
  else if ((revision2->kind == svn_client_revision_working)
           && (revision1->kind != svn_client_revision_working)
           && (revision1->kind != svn_client_revision_base))
    {
      svn_stringbuf_t *URL1;
      svn_stringbuf_t *url_anchor, *url_target;

      /* Sanity check -- path2 better be a working-copy path. */
      path_str.data = path2->data;
      path_str.len = path2->len;
      if (svn_path_is_url (&path_str))
        {
          return svn_error_create (SVN_ERR_INCORRECT_PARAMS, 0, NULL, pool,
                                   "do_diff: path isn't a working-copy path.");
          goto polite_error;
        }

      /* Extract a URL and revision from path1 (if not already a URL) */
      SVN_ERR (convert_to_url (&URL1, path1, pool));

      /* Trickiness:  possibly split up path2 into anchor/target.  If
         we do so, then we must split URL1 as well.  We shouldn't go
         assuming that URL1 is equal to path2's URL, as we used to. */
      SVN_ERR (svn_wc_get_actual_target (path2, &anchor, &target, pool));
      if (target)
        svn_path_split (URL1, &url_anchor, &url_target, pool);
      else
        {
          url_anchor = URL1;
          url_target = NULL;
        }

      /* Establish RA session to URL1's anchor */
      SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
      SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton,
                                      url_anchor->data, pool));
      SVN_ERR (svn_client__open_ra_session (&session, ra_lib, url_anchor,
                                            NULL, NULL, FALSE, FALSE, TRUE,
                                            auth_baton, pool));

      /* Set up diff editor according to path2's anchor/target. */
      SVN_ERR (svn_wc_get_diff_editor (anchor, target,
                                       callbacks, callback_baton,
                                       recurse,
                                       &diff_editor, &diff_edit_baton,
                                       pool));

      /* Tell the RA layer we want a delta to change our txn to URL1 */
      SVN_ERR (svn_client__get_revision_number
               (&start_revnum, ra_lib, session, revision1, path1->data, pool));
      SVN_ERR (ra_lib->do_update (session,
                                  &reporter, &report_baton,
                                  start_revnum,
                                  url_target,
                                  recurse,
                                  diff_editor, diff_edit_baton));

      /* Create a txn mirror of path2;  the diff editor will print
         diffs in reverse.  :-)  */
      SVN_ERR (svn_wc_crawl_revisions (path2, reporter, report_baton,
                                       FALSE, recurse,
                                       NULL, NULL, /* notification is N/A */
                                       pool));
    }

  /* Last use-case:  comparing path1@rev1 and path2@rev2, where both revs
     require repository contact.  */
  else if ((revision2->kind != svn_client_revision_working)
           && (revision2->kind != svn_client_revision_base)
           && (revision1->kind != svn_client_revision_working)
           && (revision1->kind != svn_client_revision_base))
    {
      const svn_delta_editor_t *new_diff_editor;
      void *new_diff_edit_baton;
      svn_string_t path1_s, path2_s;
      svn_stringbuf_t *URL1, *URL2;
      svn_stringbuf_t *anchor1, *target1, *anchor2, *target2;
      svn_boolean_t path1_is_url, path2_is_url;
      enum svn_node_kind path2_kind;
      void *session2;

      /* The paths could be *either* wcpaths or urls... */
      SVN_ERR (convert_to_url (&URL1, path1, pool));
      SVN_ERR (convert_to_url (&URL2, path2, pool));

      path1_s.data = path1->data;
      path1_s.len = path1->len;
      path2_s.data = path2->data;
      path2_s.len = path2->len;
      path1_is_url = svn_path_is_url (&path1_s);
      path2_is_url = svn_path_is_url (&path2_s);

      /* Open temporary RA sessions to each URL. */
      SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
      SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, URL1->data, pool));
      SVN_ERR (svn_client__open_ra_session (&session, ra_lib, URL1, NULL,
                                            NULL, FALSE, FALSE, TRUE,
                                            auth_baton, pool));
      SVN_ERR (svn_client__open_ra_session (&session2, ra_lib, URL2, NULL,
                                            NULL, FALSE, FALSE, TRUE,
                                            auth_baton, pool));

      /* Do the right thing in resolving revisions;  if the caller
         does something foolish like pass in URL@committed, then they
         should rightfully get an error when we pass a NULL path below. */
      SVN_ERR (svn_client__get_revision_number
               (&start_revnum, ra_lib, session, revision1,
                path1_is_url ? NULL : path1->data,
                pool));
      SVN_ERR (svn_client__get_revision_number
               (&end_revnum, ra_lib, session2, revision2,
                path2_is_url ? NULL : path2->data,
                pool));

      /* Now down to the -real- business.  We gotta figure out anchors
         and targets, whether things are urls or wcpaths.

         Like we do in the 2nd use-case, we have path1 follow path2's
         lead.  If path2 is split into anchor/target, then so must
         path1 (URL1) be. */
      if (path2_is_url)
        {
          SVN_ERR (ra_lib->check_path (&path2_kind, session2,
                                       "", end_revnum));
          switch (path2_kind)
            {
            case svn_node_file:
              svn_path_split (path2, &anchor2, &target2, pool);
              break;
            case svn_node_dir:
              anchor2 = path2;
              target2 = NULL;
              break;
            default:
              return svn_error_createf (SVN_ERR_FS_NOT_FOUND, 0, NULL, pool,
                                        "'%s' at rev %" SVN_REVNUM_T_FMT
                                        " wasn't found in repository.",
                                        path2->data, end_revnum);
            }
        }
      else
        {
          SVN_ERR (svn_wc_get_actual_target (path2, &anchor2, &target2, pool));
        }

      if (target2)
        {
          svn_path_split (URL1, &anchor1, &target1, pool);
        }
      else
        {
          anchor1 = URL1;
          target1 = NULL;
        }

      SVN_ERR (ra_lib->close (session));
      SVN_ERR (ra_lib->close (session2));

      /* The main session is opened to the anchor of URL1. */
      SVN_ERR (svn_client__open_ra_session (&session, ra_lib, anchor1, NULL,
                                            NULL, FALSE, FALSE, TRUE,
                                            auth_baton, pool));


      /* Open a second session used to request individual file
         contents from URL1's anchor.  */
      SVN_ERR (svn_client__open_ra_session (&session2, ra_lib, anchor1,
                                            NULL, NULL, FALSE, FALSE, TRUE,
                                            auth_baton, pool));

      /* Set up the repos_diff editor on path2's anchor.  */
      SVN_ERR (svn_client__get_diff_editor (anchor2,
                                            callbacks,
                                            callback_baton,
                                            recurse,
                                            ra_lib, session2,
                                            start_revnum,
                                            &new_diff_editor,
                                            &new_diff_edit_baton,
                                            pool));

      /* ### Make diff editor look "old" style.  Remove someday. */
      svn_delta_compat_wrap (&diff_editor, &diff_edit_baton,
                             new_diff_editor, new_diff_edit_baton, pool);

      /* We want to switch our txn into URL2 */
      SVN_ERR (ra_lib->do_switch (session,
                                  &reporter, &report_baton,
                                  end_revnum,
                                  target1,
                                  recurse,
                                  URL2,
                                  diff_editor, diff_edit_baton));

      SVN_ERR (reporter->set_path (report_baton, "", start_revnum));
      SVN_ERR (reporter->finish_report (report_baton));

      SVN_ERR (ra_lib->close (session2));
      SVN_ERR (ra_lib->close (session));
    }

  else
    {
      /* can't pigeonhole our inputs into one of the three use-cases. */
      err = NULL;
      goto polite_error;
    }

  return SVN_NO_ERROR;

 polite_error:
  return svn_error_create (SVN_ERR_INCORRECT_PARAMS, 0, err, pool,
                           "Sorry, svn_client_diff was called in a way "
                           "that is not yet supported.");
}




/*----------------------------------------------------------------------- */

/*** Public Interfaces. ***/

/* Display context diffs between two PATH/REVISION pairs.  Each of
   these input will be one of the following:

   - a repository URL at a given revision.
   - a working copy path, ignoring no local mods.
   - a working copy path, including local mods.

   We can establish a matrix that shows the nine possible types of
   diffs we expect to support.


      ` .     DST ||  URL:rev   | WC:base    | WC:working |
          ` .     ||            |            |            |
      SRC     ` . ||            |            |            |
      ============++============+============+============+
       URL:rev    || (*)        | (*)        | (*)        |
                  ||            |            |            |
                  ||            |            |            |
                  ||            |            |            |
      ------------++------------+------------+------------+
       WC:base    || (*)        |                         |
                  ||            | New svn_wc_diff which   |
                  ||            | is smart enough to      |
                  ||            | handle two WC paths     |
      ------------++------------+ and their related       +
       WC:working || (*)        | text-bases and working  |
                  ||            | files.  This operation  |
                  ||            | is entirely local.      |
                  ||            |                         |
      ------------++------------+------------+------------+
      * These cases require server communication.

   Svn_client_diff() is the single entry point for all of the diff
   operations, and will be in charge of examining the inputs and
   making decisions about how to accurately report contextual diffs.

   NOTE:  In the near future, svn_client_diff() will likely only
   continue to report textual differences in files.  Property diffs
   are important, too, and will need to be supported in some fashion
   so that this code can be re-used for svn_client_merge().
*/
svn_error_t *
svn_client_diff (const apr_array_header_t *options,
                 svn_client_auth_baton_t *auth_baton,
                 svn_stringbuf_t *path1,
                 const svn_client_revision_t *revision1,
                 svn_stringbuf_t *path2,
                 const svn_client_revision_t *revision2,
                 svn_boolean_t recurse,
                 apr_file_t *outfile,
                 apr_file_t *errfile,
                 apr_pool_t *pool)
{
  struct diff_cmd_baton diff_cmd_baton;

  diff_cmd_baton.options = options;
  diff_cmd_baton.pool = pool;
  diff_cmd_baton.outfile = outfile;
  diff_cmd_baton.errfile = errfile;

  return do_diff (options,
                  auth_baton,
                  path1, revision1,
                  path2, revision2,
                  recurse,
                  &diff_callbacks, &diff_cmd_baton,
                  pool);
}


svn_error_t *
svn_client_merge (const svn_delta_editor_t *after_editor,
                  void *after_edit_baton,
                  svn_client_auth_baton_t *auth_baton,
                  svn_stringbuf_t *path1,
                  const svn_client_revision_t *revision1,
                  svn_stringbuf_t *path2,
                  const svn_client_revision_t *revision2,
                  svn_stringbuf_t *target_wcpath,
                  svn_boolean_t recurse,
                  svn_boolean_t force,
                  apr_pool_t *pool)
{
  svn_wc_entry_t *entry;

  SVN_ERR (svn_wc_entry (&entry, target_wcpath, FALSE, pool));
  if (entry == NULL)
    return svn_error_createf (SVN_ERR_ENTRY_NOT_FOUND, 0, NULL, pool,
                              "Can't merge changes into '%s':"
                              "it's not under revision control.",
                              target_wcpath->data);

  /* If our target_wcpath is a single file, assume that PATH1 and
     PATH2 are files as well, and do a single-file merge. */
  if (entry->kind == svn_node_file)
    {
      SVN_ERR (do_single_file_merge (after_editor, after_edit_baton,
                                     auth_baton,
                                     path1, revision1,
                                     path2, revision2,
                                     target_wcpath,
                                     pool));
    }

  /* Otherwise, this must be a directory merge.  Do the fancy
     recursive diff-editor thing. */
  else if (entry->kind == svn_node_dir)
    {
      struct merge_cmd_baton merge_cmd_baton;
      merge_cmd_baton.force = force;
      merge_cmd_baton.pool = pool;

      SVN_ERR (do_merge (after_editor, after_edit_baton,
                         auth_baton,
                         path1,
                         revision1,
                         path2,
                         revision2,
                         target_wcpath,
                         recurse,
                         &merge_callbacks,
                         &merge_cmd_baton,
                         pool));
    }

  return SVN_NO_ERROR;
}


/* --------------------------------------------------------------
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end: */


