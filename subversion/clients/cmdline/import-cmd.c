/*
 * import-cmd.c -- Import a file or tree into the repository.
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

/* ==================================================================== */



/*** Includes. ***/

#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_error.h"
#include "cl.h"


/*** Code. ***/

svn_error_t *
svn_cl__import (apr_getopt_t *os,
                svn_cl__opt_state_t *opt_state,
                apr_pool_t *pool)
{
  apr_array_header_t *targets;
  svn_stringbuf_t *message;

  svn_stringbuf_t *path;
  svn_stringbuf_t *url;
  svn_stringbuf_t *new_entry;
  svn_stringbuf_t *printpath;

  const svn_delta_edit_fns_t *trace_editor;
  void *trace_edit_baton;

  svn_client_auth_t *auth_obj;
  
  /* Take our message from ARGV or a FILE */
  if (opt_state->filedata) 
    message = opt_state->filedata;
  else
    message = opt_state->message;
  
  /* Build an authentication object to give to libsvn_client. */
  auth_obj = svn_cl__make_auth_obj (opt_state, pool);

  /* Import takes up to three arguments, for example
   *
   *   $ svn import  file:///home/jrandom/repos  ./myproj  myproj
   *                 ^^^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^^  ^^^^^^
   *                        (repository)          (source)  (dest)
   *
   * or
   *
   *   $ svn import  file:///home/jrandom/repos/some/subdir  .  myproj
   *
   * What is the nicest behavior for import, from the user's point of
   * view?  This is a subtle question.  Seemingly intuitive answers
   * can lead to weird situations, such never being able to create
   * non-directories in the top-level of the repository.
   *
   * For now, let's keep things simple:
   *
   * If the third arg is present, it is the name of the new entry in
   * the repository target dir (the latter may or may not be the root
   * dir).  If it is absent, then the import happens directly in the
   * repository target dir, creating however many new entries are
   * necessary.
   *
   * If the second arg is also omitted, then "." is implied.
   *
   * The first arg cannot be omitted, of course.
   *
   * ### kff todo: review above behaviors.
   */

  targets = svn_cl__args_to_target_array (os, pool);

  /* Get a repository url. */
  if (targets->nelts < 1)
    return svn_error_create
      (SVN_ERR_CL_ARG_PARSING_ERROR, 0, NULL, pool,
       "repository url required when importing");
  else
    url = ((svn_stringbuf_t **) (targets->elts))[0];

  /* Get a local path. */
  if (targets->nelts < 2)
    path = svn_stringbuf_create (".", pool);
  else
    path = ((svn_stringbuf_t **) (targets->elts))[1];

  /* Because we're working outside the context of a working copy, we
     don't want the trace_editor to print out the 'local' paths like
     it normally does.  This leads to very confusing output.  Instead,
     for consistency, it will print those paths being added in the
     repository, completely ignoring the local source.  */
  printpath = svn_stringbuf_create ("", pool);

  /* Optionally get the dest entry name. */
  if (targets->nelts < 3)
    new_entry = NULL;  /* tells import() to create many entries at top
                          level. */
  else if (targets->nelts == 3)
    new_entry = ((svn_stringbuf_t **) (targets->elts))[2];
  else
    return svn_error_create
      (SVN_ERR_CL_ARG_PARSING_ERROR, 0, NULL, pool,
       "too many arguments to import command");
  
  SVN_ERR (svn_cl__get_trace_commit_editor (&trace_editor,
                                            &trace_edit_baton,
                                            printpath,
                                            pool));

  SVN_ERR (svn_client_import (NULL, NULL,
                              opt_state->quiet ? NULL : trace_editor, 
                              opt_state->quiet ? NULL : trace_edit_baton,
                              auth_obj,
                              path,
                              url,
                              new_entry,
                              "",   /* ### todo: pass author */
                              message,
                              opt_state->xml_file,
                              opt_state->revision,
                              pool));

  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../../svn-dev.el")
 * end: 
 */
