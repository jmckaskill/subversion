/*
 * checkout-cmd.c -- Subversion checkout command
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
svn_cl__checkout (apr_getopt_t *os,
                  svn_cl__opt_state_t *opt_state,
                  apr_pool_t *pool)
{
  const svn_delta_edit_fns_t *trace_editor;
  void *trace_edit_baton;
  svn_error_t *err;
  int i;

  err = svn_cl__parse_all_args (os, opt_state, "checkout", pool);

  if (err)
    return err;

  /* TODO Fixme: This only works for one repo checkout at a shot.  In
     CVS, when we checkout one project and give it a destination
     directory, it dumps it in the directory. If you check out more
     than one, it dumps each project into its own directory *inside*
     the one that you specified with the -d flag. So, for example, we
     have project A:

         A/one_mississippi.txt
         A/two_mississippi.txt
         A/three_mississippi.txt
     
     And project B:

         B/cat
         B/dog
         B/pig

     If I do 'cvs -d :pserver:fitz@subversion.tigris.org:/cvs co -d foo A', I get the following:

         foo/one_mississippi.txt
         foo/two_mississippi.txt
         foo/three_mississippi.txt

     But if I do this 'cvs -d :pserver:fitz@subversion.tigris.org:/cvs
     co -d foo A B', I get the following:

         foo/A/one_mississippi.txt
         foo/A/two_mississippi.txt
         foo/A/three_mississippi.txt
         foo/B/cat
         foo/B/dog
         foo/B/pig
      
    Makes sense, right? Right. Note that we have no provision for this
    right now and we need to support it. My vote is that we stop
    iterating over opt_state->args here and just pass the args into
    svn_client_checkout and let it decide what to do based on
    (args->nelts == 1) or (args->nelts > 1). -Fitz

   */
  for (i = 0; i < opt_state->args->nelts; i++)
    {
      svn_stringbuf_t *local_dir;
      svn_stringbuf_t *repos_url
        = ((svn_stringbuf_t **) (opt_state->args->elts))[0];

      /* Ensure that we have a default dir to checkout into. */
      if (! opt_state->target)
        local_dir = svn_path_last_component (repos_url,
                                             svn_path_local_style,
                                             pool);
      else
        local_dir = opt_state->target;
      
      err = svn_cl__get_trace_update_editor (&trace_editor,
                                             &trace_edit_baton,
                                             local_dir,
                                             pool);
      if (err)
        return err;
      
      err = svn_client_checkout (NULL, NULL,
                                 trace_editor, trace_edit_baton,
                                 repos_url,
                                 local_dir,
                                 opt_state->revision,
                                 opt_state->date,
                                 opt_state->xml_file,
                                 pool);
      if (err)
        return err;
      
    }
  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: 
 */
