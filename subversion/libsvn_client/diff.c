/*
 * diff.c: Compare working copy with text-base or repository.
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

struct diff_cmd_baton {
  const apr_array_header_t *options;
  apr_pool_t *pool;
  apr_file_t *outfile;
  apr_file_t *errfile;
};

/* This is an svn_wc_diff_cmd_t callback */
static svn_error_t *
diff_cmd (svn_stringbuf_t *path1,
          svn_stringbuf_t *path2,
          svn_stringbuf_t *label,
          void *baton)
{
  struct diff_cmd_baton *diff_cmd_baton = baton;
  const char **args = NULL;
  int nargs, exitcode;
  apr_file_t *outfile = diff_cmd_baton->outfile;
  apr_file_t *errfile = diff_cmd_baton->errfile;
  apr_pool_t *subpool = svn_pool_create (diff_cmd_baton->pool);

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
  apr_file_printf (outfile, "Index: %s\n", label ? label->data : path1->data);
  apr_file_printf (outfile, 
     "===================================================================\n");

  SVN_ERR (svn_io_run_diff (".", args, nargs, 
                            label ? label->data : NULL, 
                            path1->data, path2->data, 
                            &exitcode, outfile, errfile, subpool));

  /* ### todo: Handle exit code == 2 (i.e. errors with diff) here */
  
  /* ### todo: someday we'll need to worry about whether we're going
     to need to write a diff plug-in mechanism that makes use of the
     two paths, instead of just blindly running SVN_CLIENT_DIFF.  */

  /* Destroy the subpool. */
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}



/*** Public Interface. ***/

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



/* Hi!  I'm a soon-to-be-out-of-date comment regarding a
   soon-to-be-rewritten svn_client_diff() function!

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
svn_error_t *
svn_client_diff (const apr_array_header_t *diff_options,
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
  svn_revnum_t start_revnum, end_revnum;
  svn_string_t path_str;
  svn_boolean_t path_is_url;
  svn_stringbuf_t *anchor = NULL, *target = NULL;
  svn_wc_entry_t *entry;
  svn_stringbuf_t *URL = path1;
  void *ra_baton, *session;
  svn_ra_plugin_t *ra_lib;
  const svn_ra_reporter_t *reporter;
  void *report_baton;
  const svn_delta_edit_fns_t *diff_editor;
  void *diff_edit_baton;
  struct diff_cmd_baton diff_cmd_baton;

  /* Return an error if PATH1 and PATH2 aren't the same (for now). */
  if (! svn_stringbuf_compare (path1, path2))
    return svn_error_createf (SVN_ERR_UNSUPPORTED_FEATURE, 0, NULL, pool,
                              "Multi-path diff is currently unsupprted");

  /* Sanity check -- ensure that we have valid revisions to look at. */
  if ((revision1->kind == svn_client_revision_unspecified)
      || (revision2->kind == svn_client_revision_unspecified))
    {
      return svn_error_create
        (SVN_ERR_CLIENT_BAD_REVISION, 0, NULL, pool,
         "svn_client_diff: caller failed to specify any revisions");
    }

  /* Populate the DIFF_CMD_BATON. */
  diff_cmd_baton.options = diff_options;
  diff_cmd_baton.pool = pool;
  diff_cmd_baton.outfile = outfile;
  diff_cmd_baton.errfile = errfile;

  /* Determine if the target we have been given is a path or an URL.
     If it is a working copy path, we'll need to extract the URL from
     the entry for that path. */
  path_str.data = path1->data;
  path_str.len = path1->len;
  if (! ((path_is_url = svn_path_is_url (&path_str))))
    {
      SVN_ERR (svn_wc_get_actual_target (path1, &anchor, &target, pool));
      SVN_ERR (svn_wc_entry (&entry, anchor, pool));
      URL = svn_stringbuf_create (entry->url->data, pool);
    }

  /* If we are diffing a working copy path, simply use this 'quick'
     diff that does not contact the repository and simply uses the
     text base. */
  if ((! path_is_url)
      && ((revision1->kind == svn_client_revision_committed)
          || (revision1->kind == svn_client_revision_base))
      && (revision2->kind == svn_client_revision_working))
    {
      return svn_wc_diff (anchor, target, diff_cmd, &diff_cmd_baton,
                          recurse, pool);
    }

  /* Else we must contact the repository. */

  /* Establish RA session */
  SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, URL->data, pool));

  /* ### TODO: We have to pass null for the base_dir here, since the
     working copy does not match the requested revision.  It might be
     possible to have a special ra session for diff, where get_wc_prop
     cooperates with the editor and returns values when the file is in the
     wc, and null otherwise. */
  SVN_ERR (svn_client__open_ra_session (&session, ra_lib, URL, NULL,
                                        FALSE, FALSE, auth_baton, pool));

  /* ### todo: later, when we take two (or N) targets and a more
     sophisticated indication of their interaction with start and end,
     these calls will need to be rearranged. */
  SVN_ERR (svn_client__get_revision_number
           (&start_revnum, ra_lib, session, revision1, path1->data, pool));
  SVN_ERR (svn_client__get_revision_number
           (&end_revnum, ra_lib, session, revision2, path1->data, pool));

  /* ### todo: For the moment, we'll maintain the two distinct diff
     editors, svn_wc vs svn_client, in order to get this change done
     in a finite amount of time.  Next the two editors will be unified
     into one "lazy" editor that uses local data whenever possible. */

  if (revision2->kind == svn_client_revision_working)
    {
      /* The working copy is involved in this case. */
      SVN_ERR (svn_wc_get_diff_editor (anchor, target,
                                       diff_cmd, &diff_cmd_baton,
                                       recurse,
                                       &diff_editor, &diff_edit_baton,
                                       pool));

      SVN_ERR (ra_lib->do_update (session,
                                  &reporter, &report_baton,
                                  start_revnum,
                                  target,
                                  recurse,
                                  diff_editor, diff_edit_baton));

      SVN_ERR (svn_wc_crawl_revisions (path1, reporter, report_baton,
                                       FALSE, recurse,
                                       NULL, NULL, /* notification is N/A */
                                       pool));
    }
  else   /* ### todo: there may be uncovered cases remaining */
    {
      /* Pure repository comparison. */

      /* Open a second session used to request individual file
         contents. Although a session can be used for multiple requests, it
         appears that they must be sequential. Since the first request, for
         the diff, is still being processed the first session cannot be
         reused. This applies to ra_dav, ra_local does not appears to have
         this limitation. */
      void *session2;

      /* ### TODO: Forcing the base_dir to null. It might be possible to
         use a special ra session that cooperates with the editor to enable
         get_wc_prop to return values when the file is in the wc */
      SVN_ERR (svn_client__open_ra_session (&session2, ra_lib, URL,
                                            NULL,
                                            FALSE, FALSE,
                                            auth_baton, pool));

      SVN_ERR (svn_client__get_diff_editor (target,
                                            diff_cmd,
                                            &diff_cmd_baton,
                                            recurse,
                                            ra_lib, session2,
                                            start_revnum,
                                            &diff_editor, &diff_edit_baton,
                                            pool));

      SVN_ERR (ra_lib->do_update (session,
                                  &reporter, &report_baton,
                                  end_revnum,
                                  target,
                                  recurse,
                                  diff_editor, diff_edit_baton));

      SVN_ERR (reporter->set_path (report_baton, "", start_revnum));

      SVN_ERR (reporter->finish_report (report_baton));

      SVN_ERR (ra_lib->close (session2));
    }

  SVN_ERR (ra_lib->close (session));

  return SVN_NO_ERROR;
}



/* --------------------------------------------------------------
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end: */


