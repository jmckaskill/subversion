/*
 * import-cmd.c -- Import a file or tree into the repository.
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

#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_error.h"
#include "cl.h"


/*** Code. ***/

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__import (apr_getopt_t *os,
                void *baton,
                apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_array_header_t *targets;
  const char *path;
  const char *url;
  const char *new_entry;
  svn_client_commit_info_t *commit_info = NULL;
  svn_wc_notify_func_t notify_func = NULL;
  void *notify_baton = NULL;
  void *log_msg_baton;

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

  SVN_ERR (svn_opt_args_to_target_array (&targets, os, 
                                         opt_state->targets,
                                         &(opt_state->start_revision),
                                         &(opt_state->end_revision),
                                         FALSE, pool));

  /* Get a repository url. */
  if (targets->nelts < 1)
    return svn_error_create
      (SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
       "repository url required when importing");
  else
    url = ((const char **) (targets->elts))[0];

  /* Get a local path. */
  if (targets->nelts < 2)
    path = "";
  else
    path = ((const char **) (targets->elts))[1];

  /* Optionally get the dest entry name. */
  if (targets->nelts < 3)
    new_entry = NULL;  /* tells import() to create many entries at top
                          level. */
  else if (targets->nelts == 3)
    new_entry = ((const char **) (targets->elts))[2];
  else
    return svn_error_create
      (SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
       "too many arguments to import command");
  
  if (! opt_state->quiet)
    svn_cl__get_notifier (&notify_func, &notify_baton,
                          FALSE, FALSE, FALSE, pool);

  log_msg_baton = svn_cl__make_log_msg_baton (opt_state, NULL, pool);
  SVN_ERR (svn_cl__cleanup_log_msg 
           (log_msg_baton, svn_client_import (&commit_info,
                                              notify_func, notify_baton,
                                              path,
                                              url,
                                              new_entry,
                                              &svn_cl__get_log_message,
                                              log_msg_baton,
                                              opt_state->nonrecursive,
                                              ctx,
                                              pool)));

  if (commit_info && ! opt_state->quiet)
    svn_cl__print_commit_info (commit_info);

  return SVN_NO_ERROR;
}
