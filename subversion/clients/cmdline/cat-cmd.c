/*
 * cat-cmd.c -- Print the content of a file or URL.
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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

#include "svn_client.h"
#include "svn_string.h"
#include "svn_error.h"
#include "cl.h"


/*** Code. ***/

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__cat (apr_getopt_t *os,
             void *baton,
             apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = baton;
  apr_array_header_t *targets;
  int i;
  svn_client_auth_baton_t *auth_baton;
  apr_file_t *std_out;
  apr_status_t status;

  if (opt_state->end_revision.kind != svn_opt_revision_unspecified)
    return svn_error_createf (SVN_ERR_CLIENT_REVISION_RANGE, NULL,
                              "cat only accepts a single revision");

  SVN_ERR (svn_opt_args_to_target_array (&targets, os,
                                         opt_state->targets,
                                         &(opt_state->start_revision),
                                         &(opt_state->end_revision),
                                         FALSE, pool));

  /* Cat cannot operate on an implicit '.' so a filename is required */
  if (! targets->nelts)
    return svn_error_create (SVN_ERR_CL_ARG_PARSING_ERROR, 0, "");

  /* Build an authentication baton to give to libsvn_client. */
  auth_baton = svn_cl__make_auth_baton (opt_state, pool);

  /* Use APR's stdout, not the C library's.  Why?  Ryan Bloom says:
   *
   *    I can shed some light on this.  :-) The only reason that the
   *    apr_file_open_std* functions exist is that you may not always
   *    have a stderr/out/in on Windows.  This is generally a problem
   *    with newer versions of Windows and services.
   * 
   *    The other problem is that the C library functions generally
   *    work differently on Windows and Unix.  So, by using
   *    apr_file_open_std* functions, you can get a handle to an APR
   *    struct that works with the APR functions which are supposed to
   *    work identically on all platforms.
   */
  status = apr_file_open_stdout (&std_out, pool);
  if (!APR_STATUS_IS_SUCCESS (status))
    {
      return svn_error_create (status, NULL, "");
    }

  for (i = 0; i < targets->nelts; i++)
    {
      const char *target = ((const char **) (targets->elts))[i];
      svn_stream_t *out = svn_stream_from_aprfile (std_out, pool);
      const char *URL;

      SVN_ERR (svn_cl__get_url_from_target (&URL, target, pool));
      if (! URL)
        return svn_error_createf (SVN_ERR_ENTRY_MISSING_URL, NULL,
                                  "'%s' has no URL", target);

      SVN_ERR (svn_client_cat (out, URL, &(opt_state->start_revision),
                               auth_baton, pool));
    }

  return SVN_NO_ERROR;
}
