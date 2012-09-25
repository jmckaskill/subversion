/*
 * propdel-cmd.c -- Remove property from files/dirs
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

#include "svn_cmdline.h"
#include "svn_wc.h"
#include "svn_pools.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_error.h"
#include "svn_utf.h"
#include "cl.h"


/*** Code. ***/

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__propdel (apr_getopt_t *os,
                 void *baton,
                 apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  const char *pname, *pname_utf8;
  apr_array_header_t *args, *targets;
  int i;

  /* Get the property's name (and a UTF-8 version of that name). */
  SVN_ERR (svn_opt_parse_num_args (&args, os, 1, pool));
  pname = ((const char **) (args->elts))[0];
  SVN_ERR (svn_utf_cstring_to_utf8 (&pname_utf8, pname, pool));

  /* Suck up all the remaining arguments into a targets array */
  SVN_ERR (svn_opt_args_to_target_array (&targets, os,
                                         opt_state->targets,
                                         &(opt_state->start_revision),
                                         &(opt_state->end_revision),
                                         FALSE, pool));

  /* Add "." if user passed 0 file arguments */
  svn_opt_push_implicit_dot_target (targets, pool);

  if (opt_state->revprop)  /* operate on a revprop */
    {
      svn_revnum_t rev;
      const char *URL, *target;

      /* All property commands insist on a specific revision when
         operating on a revprop. */
      if (opt_state->start_revision.kind == svn_opt_revision_unspecified)
        return svn_cl__revprop_no_rev_error (pool);

      /* Else some revision was specified, so proceed. */

      /* Either we have a URL target, or an implicit wc-path ('.')
         which needs to be converted to a URL. */
      if (targets->nelts <= 0)
        return svn_error_create(SVN_ERR_CL_INSUFFICIENT_ARGS, NULL,
                                "No URL target available.");
      target = ((const char **) (targets->elts))[0];
      SVN_ERR (svn_client_url_from_path (&URL, target, pool));  
      if (URL == NULL)
        return svn_error_create(SVN_ERR_UNVERSIONED_RESOURCE, NULL,
                                "Either a URL or versioned item is required.");

      /* Let libsvn_client do the real work. */
      SVN_ERR (svn_client_revprop_set (pname_utf8, NULL,
                                       URL, &(opt_state->start_revision),
                                       &rev, FALSE, ctx, pool));
      if (! opt_state->quiet) 
        {
          const char *pname_stdout;
          SVN_ERR (svn_cmdline_cstring_from_utf8 (&pname_stdout,
                                                  pname_utf8, pool));
          printf ("property '%s' deleted from repository revision '%"
                  SVN_REVNUM_T_FMT"'\n",
                  pname_stdout, rev);
        }      
    }
  else if (opt_state->start_revision.kind != svn_opt_revision_unspecified)
    {
      return svn_error_createf
        (SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
         "Cannot specify revision for deleting versioned property '%s'.",
         pname);
    }
  else  /* operate on a normal, versioned property (not a revprop) */
    {
      apr_pool_t *subpool = svn_pool_create (pool);

      /* For each target, remove the property PNAME. */
      for (i = 0; i < targets->nelts; i++)
        {
          const char *target = ((const char **) (targets->elts))[i];

          svn_pool_clear (subpool);
          SVN_ERR (svn_client_propset (pname_utf8, NULL, target,
                                       opt_state->recursive, subpool));
          if (! opt_state->quiet) 
            {
              const char *pname_stdout;
              const char *target_stdout;
              SVN_ERR (svn_cmdline_cstring_from_utf8 (&pname_stdout,
                                                      pname_utf8, subpool));
              SVN_ERR (svn_cmdline_cstring_from_utf8 (&target_stdout,
                                                      target, subpool));
              printf ("property '%s' deleted%sfrom '%s'.\n", pname_stdout,
                      opt_state->recursive ? " (recursively) " : " ",
                      target_stdout);
            }
          SVN_ERR (svn_cl__check_cancel (ctx->cancel_baton));
        }
      svn_pool_destroy (subpool);
    }

  return SVN_NO_ERROR;
}