/*
 * status.c:  return the status of a working copy dirent
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

#include "client.h"

#include "svn_wc.h"
#include "svn_delta.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_test.h"
#include "svn_io.h"



/*** Getting update information ***/


/* Open an RA session to URL, providing PATH/AUTH_BATON for
   authentication callbacks.

   STATUSHASH has presumably been filled with status structures that
   contain only local-mod information.  Ask RA->do_status() to drive a
   custom editor that will add update information to this collection
   of structures.  Also, use the RA session to fill in the "youngest
   revnum" field in each structure.

   Set *YOUNGEST to the youngest revision in the repository.

   If DESCEND is zero, only immediate children of PATH will be edited
   or added to the hash.  Else, the dry-run update will be fully
   recursive. */
static svn_error_t *
add_update_info_to_status_hash (apr_hash_t *statushash,
                                svn_revnum_t *youngest,
                                const char *path,
                                svn_wc_adm_access_t *adm_access,
                                svn_client_auth_baton_t *auth_baton,
                                svn_boolean_t descend,
                                apr_pool_t *pool)
{
  svn_ra_plugin_t *ra_lib;  
  void *ra_baton, *session, *report_baton;
  const svn_delta_editor_t *status_editor;
  void *status_edit_baton;
  const svn_delta_edit_fns_t *wrap_editor;
  void *wrap_edit_baton;
  const svn_ra_reporter_t *reporter;
  const char *anchor, *target, *URL;
  svn_wc_adm_access_t *anchor_access;
  const svn_wc_entry_t *entry;

  /* Use PATH to get the update's anchor and targets. */
  SVN_ERR (svn_wc_get_actual_target (path, &anchor, &target, pool));

  if (strlen (anchor) != strlen (path))
    /* Using pool cleanup to close it */
    SVN_ERR (svn_wc_adm_open (&anchor_access, NULL, anchor, FALSE, FALSE,
                              pool));
  else
    anchor_access = adm_access;

  /* Get full URL from the ANCHOR. */
  SVN_ERR (svn_wc_entry (&entry, anchor, anchor_access, FALSE, pool));
  if (! entry)
    return svn_error_createf
      (SVN_ERR_ENTRY_NOT_FOUND, 0, NULL, pool,
       "add_update_info_to_status_hash: %s is not under revision control",
       anchor);
  if (! entry->url)
    return svn_error_createf
      (SVN_ERR_ENTRY_MISSING_URL, 0, NULL, pool,
       "add_update_info_to_status_hash: entry '%s' has no URL", anchor);
  URL = apr_pstrdup (pool, entry->url);

  /* Get the RA library that handles URL. */
  SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, URL, pool));

  /* Open a repository session to the URL. */
  SVN_ERR (svn_client__open_ra_session (&session, ra_lib, URL, anchor,
                                        anchor_access, NULL, TRUE, TRUE, TRUE, 
                                        auth_baton, pool));

  /* Tell RA to drive a status-editor; this will fill in the
     repos_status_* fields in each status struct. */
  SVN_ERR (svn_wc_get_status_editor (&status_editor, &status_edit_baton,
                                     path, adm_access, descend, statushash,
                                     youngest, pool));

  /* ### todo:  This is a TEMPORARY wrapper around our editor so we
     can use it with an old driver. */
  svn_delta_compat_wrap (&wrap_editor, &wrap_edit_baton, 
                         status_editor, status_edit_baton, pool);

  SVN_ERR (ra_lib->do_status (session,
                              &reporter, &report_baton,
                              target, descend,
                              wrap_editor, wrap_edit_baton));

  /* Drive the reporter structure, describing the revisions within
     PATH.  When we call reporter->finish_report, the
     status_editor will be driven by svn_repos_dir_delta. */
  SVN_ERR (svn_wc_crawl_revisions (path, adm_access, reporter, report_baton, 
                                   FALSE, /* don't restore missing files */
                                   descend,
                                   NULL, NULL, /* notification is N/A */
                                   NULL,
                                   pool));

  /* We're done with the RA session. */
  SVN_ERR (ra_lib->close (session));

  return SVN_NO_ERROR;
}





/*** Public Interface. ***/


svn_error_t *
svn_client_status (apr_hash_t **statushash,
                   svn_revnum_t *youngest,
                   const char *path,
                   svn_client_auth_baton_t *auth_baton,
                   svn_boolean_t descend,
                   svn_boolean_t get_all,
                   svn_boolean_t update,
                   svn_boolean_t no_ignore,
                   apr_pool_t *pool)
{
  apr_hash_t *hash = apr_hash_make (pool);
  svn_wc_adm_access_t *adm_access;

  /* Need to lock the tree as even a non-recursive status requires the
     immediate directories to be locked. */
  SVN_ERR (svn_wc_adm_probe_open (&adm_access, NULL, path, FALSE, TRUE, pool));

  /* Ask the wc to give us a list of svn_wc_status_t structures.
     These structures contain nothing but information found in the
     working copy. */
  SVN_ERR (svn_wc_statuses (hash, path, adm_access,
                            descend, get_all, no_ignore, pool));

  if (update)    
    {
      /* Add "dry-run" update information to our existing structures.
         (Pass the DESCEND flag here, since we may want to ignore update
         info that is below PATH.)  */
      SVN_ERR (add_update_info_to_status_hash (hash, youngest, path, adm_access,
                                               auth_baton, descend, pool));
    }

  SVN_ERR (svn_wc_adm_close (adm_access));

  /* If the caller wants us to contact the repository also... */
  *statushash = hash;

  return SVN_NO_ERROR;
}









/* --------------------------------------------------------------
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end: */
