/*
 * prompt.c -- ask the user for authentication information.
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

#include "apr_lib.h"



/*** Build an authentication object from commandline args. ***/

svn_client_auth_t *
svn_cl__make_auth_obj (svn_cl__opt_state_t *opt_state,
                       apr_pool_t *pool)
{
  svn_client_auth_t *auth_obj;
  auth_obj = apr_pcalloc (pool, sizeof(*auth_obj));

  auth_obj->prompt_callback = svn_cl__prompt_user;
  auth_obj->prompt_baton = NULL;

  if (opt_state->auth_username)
    auth_obj->username = opt_state->auth_username->data;
  if (opt_state->auth_password)
    auth_obj->password = opt_state->auth_password->data;
  /* Add more authentication args here as necessary... */

  return auth_obj;
}



/*** Our implementation of the 'auth info callback' routine, 
     as defined in svn_client.h.   This callback is passed to any
     libsvn_client routine that needs to authenticate against a
     repository. ***/

svn_error_t *
svn_cl__prompt_user (char **result,
                     const char *prompt,
                     svn_boolean_t hide,
                     void *baton,
                     apr_pool_t *pool)
{
  apr_status_t status;
  apr_file_t *fp;
  char c;

  /* ### baton is NULL... the commandline client app doesn't need one,
   but a GUI app probably would. */

  svn_stringbuf_t *strbuf = svn_stringbuf_create ("", pool);

  status = apr_file_open_stdin (&fp, pool);
  if (status)
    return
      svn_error_create (status, 0, NULL, pool,
                        "svn_cl__prompt_user:  couldn't open stdin.");

  /* ### implement the HIDE flag later using apr_getpassword or
     something. */
  if (! hide)
    {
      printf (prompt);
      fflush (stdout);

      while (1)
        {
          status = apr_file_getc (&c, fp);
          if (status && (status != APR_EOF))
            return svn_error_create (status, 0, NULL, pool,
                                     "error reading stdin.");
          if ((c == '\n') || (c == '\r'))
            break;
          
          svn_stringbuf_appendbytes (strbuf, &c, 1);
        }
    }
  else
    {
      size_t bufsize = 300;
      svn_stringbuf_ensure (strbuf, bufsize);

      /* Hopefully this won't echo to the screen. */
      status = apr_password_get (prompt, strbuf->data, &bufsize);
      if (status)
        return svn_error_create (status, 0, NULL, pool,
                                 "error from apr_password_get().");      
    }

  *result = strbuf->data;

  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../../svn-dev.el")
 * end: 
 */
