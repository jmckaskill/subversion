/* error.c:  common exception handling for Subversion
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
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



#include <stdarg.h>
#include <assert.h>

#include <apr_lib.h>
#include <apr_general.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_hash.h>

#include "svn_cmdline.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_io.h"

#ifdef SVN_DEBUG
/* file_line for the non-debug case. */
static const char SVN_FILE_LINE_UNDEFINED[] = "svn:<undefined>";
#endif /* SVN_DEBUG */

#include "svn_private_config.h"


/*** Helpers for creating errors ***/
#undef svn_error_create
#undef svn_error_createf
#undef svn_error_quick_wrap
#undef svn_error_wrap_apr


/* XXX FIXME: These should be protected by a thread mutex.
   svn_error__locate and make_error_internal should cooperate
   in locking and unlocking it. */

/* XXX TODO: Define mutex here #if APR_HAS_THREADS */
static const char *error_file = NULL;
static long error_line = -1;

void
svn_error__locate (const char *file, long line)
{
  /* XXX TODO: Lock mutex here */
  error_file = file;
  error_line = line;
}


/* Cleanup function for errors.  svn_error_clear () removes this so
   errors that are properly handled *don't* hit this code. */
#if defined(SVN_DEBUG_ERROR)
static apr_status_t err_abort (void *data)
{
  svn_error_t *err = data;  /* For easy viewing in a debugger */
  abort();
  err = err; /* Fake a use for the variable */
}
#endif


static svn_error_t *
make_error_internal (apr_status_t apr_err,
                     svn_error_t *child)
{
  apr_pool_t *pool;
  svn_error_t *new_error;

  /* Reuse the child's pool, or create our own. */
  if (child)
    pool = child->pool;
  else
    {
      if (apr_pool_create (&pool, NULL))
        abort ();
    }

  /* Create the new error structure */
  new_error = (svn_error_t *) apr_pcalloc (pool, sizeof (*new_error));

  /* Fill 'er up. */
  new_error->apr_err = apr_err;
  new_error->child   = child;
  new_error->pool    = pool;
  new_error->file    = error_file;
  new_error->line    = error_line;
  /* XXX TODO: Unlock mutex here */

#if defined(SVN_DEBUG_ERROR)
  if (! child)
      apr_pool_cleanup_register(pool, new_error, err_abort, NULL);
#endif

  return new_error;
}



/*** Creating and destroying errors. ***/

svn_error_t *
svn_error_create (apr_status_t apr_err,
                  svn_error_t *child,
                  const char *message)
{
  svn_error_t *err;

  err = make_error_internal (apr_err, child);

  if (message)
    err->message = (const char *) apr_pstrdup (err->pool, message);

  return err;
}


svn_error_t *
svn_error_createf (apr_status_t apr_err,
                   svn_error_t *child,
                   const char *fmt,
                   ...)
{
  svn_error_t *err;
  va_list ap;

  err = make_error_internal (apr_err, child);

  va_start (ap, fmt);
  err->message = apr_pvsprintf (err->pool, fmt, ap);
  va_end (ap);

  return err;
}


svn_error_t *
svn_error_wrap_apr (apr_status_t status,
                    const char *fmt,
                    ...)
{
  svn_error_t *err, *utf8_err;
  va_list ap;
  char errbuf[255];
  const char *msg_apr, *msg;

  err = make_error_internal (status, NULL);

  if (fmt)
    {
      /* Grab the APR error message. */
      apr_strerror (status, errbuf, sizeof (errbuf));
      utf8_err = svn_utf_cstring_to_utf8 (&msg_apr, errbuf, err->pool);
      if (utf8_err)
        msg_apr = NULL;
      svn_error_clear (utf8_err);

      /* Append it to the formatted message. */
      va_start (ap, fmt);
      msg = apr_pvsprintf (err->pool, fmt, ap);
      va_end (ap);
      err->message = apr_psprintf (err->pool, "%s%s%s", msg,
                                   (msg_apr) ? ": " : "",
                                   (msg_apr) ? msg_apr : "");
    }

  return err;
}


svn_error_t *
svn_error_quick_wrap (svn_error_t *child, const char *new_msg)
{
  return svn_error_create (child->apr_err,
                           child,
                           new_msg);
}


void
svn_error_compose (svn_error_t *chain, svn_error_t *new_err)
{
  apr_pool_t *pool = chain->pool;
  apr_pool_t *oldpool = new_err->pool;

  while (chain->child)
    chain = chain->child;

#if defined(SVN_DEBUG_ERROR)
  /* Kill existing handler since the end of the chain is going to change */
  apr_pool_cleanup_kill (pool, chain, err_abort);
#endif

  /* Copy the new error chain into the old chain's pool. */
  while (new_err)
    {
      chain->child = apr_palloc (pool, sizeof (*chain->child));
      chain = chain->child;
      *chain = *new_err;
      chain->message = apr_pstrdup (pool, new_err->message);
      chain->pool = pool;
#if defined(SVN_DEBUG_ERROR)
      if (! new_err->child)
        apr_pool_cleanup_kill (oldpool, new_err, err_abort);
#endif
      new_err = new_err->child;
    }

#if defined(SVN_DEBUG_ERROR)
  apr_pool_cleanup_register (pool, chain, err_abort, NULL);
#endif

  /* Destroy the new error chain. */
  apr_pool_destroy (oldpool);
}


void
svn_error_clear (svn_error_t *err)
{
  if (err)
    {
#if defined(SVN_DEBUG_ERROR)
      while (err->child)
        err = err->child;
      apr_pool_cleanup_kill (err->pool, err, err_abort);
#endif
      apr_pool_destroy (err->pool);
    }
}

static void
print_error (svn_error_t *err, FILE *stream, svn_boolean_t print_strerror)
{
  char errbuf[256];
  const char *err_string;
  svn_error_t *temp_err = NULL;  /* ensure initialized even if
                                    err->file == NULL */
  const char *file_utf8;

  /* Pretty-print the error */
  /* Note: we can also log errors here someday. */

#ifdef SVN_DEBUG
  /* Note: err->file is _not_ in UTF-8, because it's expanded from
           the __FILE__ preprocessor macro. */
  if (err->file
      && !(temp_err = svn_utf_cstring_to_utf8 (&file_utf8, err->file,
                                               err->pool)))
    svn_error_clear (svn_cmdline_fprintf (stream, err->pool,
                                          "%s:%ld", err->file, err->line));
  else
    {
      svn_error_clear (svn_cmdline_fputs (SVN_FILE_LINE_UNDEFINED,
                                          stream, err->pool));
      svn_error_clear (temp_err);
    }

  svn_error_clear (svn_cmdline_fprintf (stream, err->pool,
                                        ": (apr_err=%d)\n", err->apr_err));
#endif /* SVN_DEBUG */
  
  /* Only print the same APR error string once. */
  if (err->message)
    svn_error_clear (svn_cmdline_fprintf (stream, err->pool, "svn: %s\n",
                                          err->message));
  else if (print_strerror)
    {
      /* Is this a Subversion-specific error code? */
      if ((err->apr_err > APR_OS_START_USEERR)
          && (err->apr_err <= APR_OS_START_CANONERR))
        err_string = svn_strerror (err->apr_err, errbuf, sizeof (errbuf));
      /* Otherwise, this must be an APR error code. */
      else if ((temp_err = svn_utf_cstring_to_utf8
                (&err_string, apr_strerror (err->apr_err, errbuf,
                                            sizeof (errbuf)), err->pool)))
        {
          svn_error_clear (temp_err);
          err_string = _("Can't recode error string from APR");
        }
      
      svn_error_clear (svn_cmdline_fprintf (stream, err->pool,
                                            "svn: %s\n", err_string));
    }
}

void
svn_handle_error (svn_error_t *err, FILE *stream, svn_boolean_t fatal)
{
  apr_status_t parent_apr_err = APR_SUCCESS;

  while (err)
    {
      print_error (err, stream, (err->apr_err != parent_apr_err));
      parent_apr_err = err->apr_err;
      err = err->child;
    }

  fflush (stream);
  if (fatal)
    /* XXX Shouldn't we exit(1) here instead, so that atexit handlers
       get called?  --xbc */
    abort ();
}


void
svn_handle_warning (FILE *stream, svn_error_t *err)
{
  svn_error_clear (svn_cmdline_fprintf (stream, err->pool, "svn: warning: %s\n",
                                       err->message));
  fflush (stream);
}



/* svn_strerror() and helpers */

typedef struct {
  svn_errno_t errcode;
  const char *errdesc;
} err_defn;

/* To understand what is going on here, read svn_error_codes.h. */
#define SVN_ERROR_BUILD_ARRAY
#include "svn_error_codes.h"

char *
svn_strerror (apr_status_t statcode, char *buf, apr_size_t bufsize)
{
  const err_defn *defn;

  for (defn = error_table; defn->errdesc != NULL; ++defn)
    if (defn->errcode == (svn_errno_t)statcode)
      {
        apr_cpystrn (buf, dgettext (PACKAGE_NAME, defn->errdesc), bufsize);
        return buf;
      }

  return apr_strerror (statcode, buf, bufsize);
}
