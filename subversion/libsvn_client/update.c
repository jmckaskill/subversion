/*
 * update.c:  wrappers around wc update functionality
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



/*** Includes. ***/

#include <assert.h>

#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "client.h"



/*** Code. ***/

/* Perform an update of PATH (part of a working copy), providing pre-
   and post-checkout hook editors and batons (BEFORE_EDITOR,
   BEFORE_EDIT_BATON / AFTER_EDITOR, AFTER_EDIT_BATON).

   If XML_SRC is NULL, then the update will come from the repository
   that PATH was originally checked-out from.  An invalid REVISION
   will cause the PATH to be updated to the "latest" revision, while a
   valid REVISION will update to a specific tree.  Alternatively, a
   time TM can be used to implicitly select a revision.  TM cannot be
   used at the same time as REVISION.

   If XML_SRC is non-NULL, it is an xml file to update from.  An
   invalid REVISION implies that the revision *must* be present in the
   <delta-pkg> tag, while a valid REVISION will be simply be stored in
   the wc. (Note: a <delta-pkg> revision will *always* override the
   one passed in.)

   This operation will use the provided memory POOL. */
svn_error_t *
svn_client_update (const svn_delta_edit_fns_t *before_editor,
                   void *before_edit_baton,
                   const svn_delta_edit_fns_t *after_editor,
                   void *after_edit_baton,
                   svn_client_auth_baton_t *auth_baton,
                   svn_stringbuf_t *path,
                   svn_stringbuf_t *xml_src,
                   svn_revnum_t revision,
                   apr_time_t tm,
                   apr_pool_t *pool)
{
  const svn_delta_edit_fns_t *update_editor;
  void *update_edit_baton;
  const svn_ra_reporter_t *reporter;
  void *report_baton;
  svn_wc_entry_t *entry;
  svn_stringbuf_t *URL;
  svn_stringbuf_t *anchor, *target;

  /* Sanity check.  Without this, the update is meaningless. */
  assert (path != NULL);
  assert (path->len > 0);

  /* If both REVISION and TM are specified, this is an error.
     They mostly likely contradict one another. */
  if ((revision != SVN_INVALID_REVNUM) && tm)
    return
      svn_error_create(SVN_ERR_CL_MUTUALLY_EXCLUSIVE_ARGS, 0, NULL, pool,
                       "Cannot specify _both_ revision and time.");

  /* Use PATH to get the update's anchor and targets. */
  SVN_ERR (svn_wc_get_actual_target (path, &anchor, &target, pool));

  /* Get full URL from the ANCHOR. */
  SVN_ERR (svn_wc_entry (&entry, anchor, pool));
  if (! entry)
    return svn_error_createf
      (SVN_ERR_WC_OBSTRUCTED_UPDATE, 0, NULL, pool,
       "svn_client_update: %s is not under revision control", anchor->data);
  if (entry->existence == svn_wc_existence_deleted)
    return svn_error_createf
      (SVN_ERR_WC_ENTRY_NOT_FOUND, 0, NULL, pool,
       "svn_client_update: entry '%s' has been deleted", anchor->data);
  if (! entry->ancestor)
    return svn_error_createf
      (SVN_ERR_WC_ENTRY_MISSING_ANCESTRY, 0, NULL, pool,
       "svn_client_update: entry '%s' has no URL", anchor->data);
  URL = svn_stringbuf_create (entry->ancestor->data, pool);

  /* Fetch the update editor.  If REVISION is invalid, that's okay;
     either the RA or XML driver will call editor->set_target_revision
     later on. */
  SVN_ERR (svn_wc_get_update_editor (anchor,
                                     target,
                                     revision,
                                     &update_editor,
                                     &update_edit_baton,
                                     pool));

  /* Wrap it up with outside editors. */
  svn_delta_wrap_editor (&update_editor, &update_edit_baton,
                         before_editor, before_edit_baton,
                         update_editor, update_edit_baton,
                         after_editor, after_edit_baton, pool);

  /* Using an RA layer */
  if (! xml_src)
    {
      void *ra_baton, *session, *cb_baton;
      svn_ra_plugin_t *ra_lib;
      svn_ra_callbacks_t *ra_callbacks;

      /* Get the RA vtable that matches URL. */
      SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
      SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, URL->data, pool));

      /* Get the client callbacks for auth stuffs. */
      SVN_ERR (svn_client__get_ra_callbacks (&ra_callbacks, &cb_baton,
                                             auth_baton, anchor, TRUE,
                                             TRUE, pool));
      SVN_ERR (ra_lib->open (&session, URL, ra_callbacks, cb_baton, pool));

      /* If TM is given, convert the time into a revision number. */
      if (tm)
        SVN_ERR (ra_lib->get_dated_revision (session, &revision, tm));
      
      /* Tell RA to do a update of URL+TARGET to REVISION; if we pass an
         invalid revnum, that means RA will use the latest revision.  */
      SVN_ERR (ra_lib->do_update (session,
                                  &reporter, &report_baton,
                                  revision,
                                  target,
                                  update_editor, update_edit_baton));

      /* Drive the reporter structure, describing the revisions within
         PATH.  When we call reporter->finish_report, the
         update_editor will be driven by svn_repos_dir_delta. */
      SVN_ERR (svn_wc_crawl_revisions (path, reporter, report_baton,
                                       TRUE, TRUE, pool));

      /* Close the RA session. */
      SVN_ERR (ra_lib->close (session));
    }      
  
  /* Else we're checking out from xml */
  else
    {
      apr_status_t apr_err;
      apr_file_t *in = NULL;

      /* Open xml file. */
      apr_err = apr_file_open (&in, xml_src->data, (APR_READ | APR_CREATE),
                               APR_OS_DEFAULT, pool);
      if (apr_err)
        return svn_error_createf (apr_err, 0, NULL, pool,
                                  "unable to open %s", xml_src->data);

      /* Do an update by xml-parsing the stream.  An invalid revnum
         means that there will be a revision number in the <delta-pkg>
         tag.  Otherwise, a valid revnum will be stored in the wc,
         assuming there's no <delta-pkg> tag to override it. */
      SVN_ERR (svn_delta_xml_auto_parse (svn_stream_from_aprfile (in, pool),
                                         update_editor,
                                         update_edit_baton,
                                         URL,
                                         revision,
                                         pool));
      /* Close XML file. */
      apr_file_close (in);
    }

  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: */
