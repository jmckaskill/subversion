/* svn_error:  common exception handling for Subversion
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



#include <stdarg.h>
#include <assert.h>

#include "apr_lib.h"
#include "apr_general.h"
#include "apr_pools.h"
#include "apr_strings.h"
#include "apr_hash.h"

#include "svn_pools.h"
#include "svn_error.h"
#include "svn_io.h"

/* Key for the error pool itself. */
static const char SVN_ERROR_POOL[] = "svn-error-pool";

/* Key for a boolean signifying whether the error pool is a subpool of
   the pool whose prog_data we got it from. */
static const char SVN_ERROR_POOL_ROOTED_HERE[] = "svn-error-pool-rooted-here";

/* file_line for the non-debug case. */
static const char SVN_FILE_LINE_UNDEFINED[] = "svn:<undefined>";





/*** helpers for creating errors ***/
#ifdef SVN_DEBUG
#undef svn_error_create
#undef svn_error_createf
#undef svn_error_quick_wrap
#endif


/* XXX FIXME: These should be protected by a thread mutex.
   svn_error__locate and make_error_internal should cooperate
   in locking and unlocking it. */

#ifdef SVN_DEBUG
/* XXX TODO: Define mutex here #if APR_HAS_THREADS */
static const char *error_file = NULL;
static long error_line = -1;
#endif

void
svn_error__locate (const char *file, long line)
{
#ifdef SVN_DEBUG
  /* XXX TODO: Lock mutex here */
  error_file = file;
  error_line = line;
#endif
}


static svn_error_t *
make_error_internal (apr_status_t apr_err,
                     int src_err,
                     svn_error_t *child,
                     apr_pool_t *pool)
{
  svn_error_t *new_error;
  apr_pool_t *newpool;

  /* Make a new subpool of the active error pool, or else use child's pool. */
  if (pool)
    {
      apr_pool_t *error_pool;
      apr_pool_userdata_get ((void **) &error_pool, SVN_ERROR_POOL, pool);
      if (error_pool)
        newpool = svn_pool_create (error_pool);
      else
        newpool = pool;
    }
  else if (child)
    newpool = child->pool;
  else            /* can't happen */
    return NULL;  /* kff todo: is this a good reaction? */

  assert (newpool != NULL);

  /* Create the new error structure */
  new_error = (svn_error_t *) apr_pcalloc (newpool, sizeof (*new_error));

  /* Fill 'er up. */
  new_error->apr_err = apr_err;
  new_error->src_err = src_err;
  new_error->child   = child;
  new_error->pool    = newpool;  
#ifdef SVN_DEBUG
  new_error->file    = error_file;
  new_error->line    = error_line;
  /* XXX TODO: Unlock mutex here */
#else
  new_error->file    = NULL;
  new_error->line    = -1;
#endif

  return new_error;
}



/*** Setting a semi-global error pool. ***/

static int
abort_on_pool_failure (int retcode)
{
  abort ();
  return -1; /* prevent compiler warnings */
}


static apr_status_t
svn_error__make_error_pool (apr_pool_t *parent, apr_pool_t **error_pool)
{
  apr_status_t apr_err;

  /* Create a subpool to hold all error allocations. We use a subpool rather
     than the parent itself, so that we can clear the error pool. */
  apr_pool_create_ex (error_pool, parent, abort_on_pool_failure,
                      APR_POOL_FDEFAULT);
  
  /* Set the error pool on itself. */
  apr_err = apr_pool_userdata_set (*error_pool, SVN_ERROR_POOL, 
                                   apr_pool_cleanup_null, *error_pool);

  return apr_err;
}


/* Get POOL's error pool into *ERROR_POOL.
 * 
 * If ROOTED_HERE is not null, then
 *   - If the error pool is a direct subpool of POOL, set *ROOTED_HERE to 1
 *   - Else set *ROOTED_HERE to 0
 * Else don't touch *ROOTED_HERE.
 *
 * Abort if POOL does not have an error pool.
 */
static void
svn_error__get_error_pool (apr_pool_t *pool,
                           apr_pool_t **error_pool,
                           svn_boolean_t *rooted_here)
{
  apr_pool_userdata_get ((void **) error_pool, SVN_ERROR_POOL, pool);
  if (*error_pool == NULL)
    abort_on_pool_failure (SVN_ERR_BAD_CONTAINING_POOL);

  if (rooted_here)
    {
      void *value;

      apr_pool_userdata_get (&value, SVN_ERROR_POOL_ROOTED_HERE, pool);
      *rooted_here = (svn_boolean_t)value;
    }
}


/* Set POOL's error pool to ERROR_POOL.
 *
 * If ROOTED_HERE is non-zero, then record the fact that ERROR_POOL is
 * a direct subpool of POOL.
 *
 * If unable to set either, abort.
 */
static void
svn_error__set_error_pool (apr_pool_t *pool,
                           apr_pool_t *error_pool,
                           svn_boolean_t rooted_here)
{
  apr_status_t apr_err;

  apr_err = apr_pool_userdata_set (error_pool, SVN_ERROR_POOL,
                              apr_pool_cleanup_null, pool);
  if (apr_err)
    abort_on_pool_failure (apr_err);

  if (rooted_here)
    {
      apr_err = apr_pool_userdata_set ((void *) 1, SVN_ERROR_POOL_ROOTED_HERE,
                                       apr_pool_cleanup_null, pool);
      if (apr_err)
        abort_on_pool_failure (apr_err);
    }
}


/* Set P's error pool to that of P's parent.  P's parent must exist
   and have an error pool, else we abort. */
static void
svn_pool__inherit_error_pool (apr_pool_t *p)
{
  apr_pool_t *parent = apr_pool_get_parent (p);
  apr_pool_t *error_pool;

  if (parent == NULL)
    abort_on_pool_failure (SVN_ERR_BAD_CONTAINING_POOL);

  svn_error__get_error_pool (parent, &error_pool, NULL);
  svn_error__set_error_pool (p, error_pool, 0);
}





apr_status_t
svn_error_init_pool (apr_pool_t *top_pool)
{
  void *check;
  apr_pool_t *error_pool;
  apr_status_t apr_err;

  /* just return if an error pool already exists */
  apr_pool_userdata_get (&check, SVN_ERROR_POOL, top_pool);
  if (check == NULL)
    {
      apr_err = svn_error__make_error_pool (top_pool, &error_pool);
      if (! APR_STATUS_IS_SUCCESS (apr_err))
        return apr_err;

      svn_error__set_error_pool (top_pool, error_pool, 1);
    }

  return APR_SUCCESS;
}



/*** Creating and destroying errors. ***/

svn_error_t *
svn_error_create (apr_status_t apr_err,
                  int src_err,
                  svn_error_t *child,
                  apr_pool_t *pool,
                  const char *message)
{
  svn_error_t *err;

  err = make_error_internal (apr_err, src_err, child, pool);
  
  err->message = (const char *) apr_pstrdup (err->pool, message);

  return err;
}


svn_error_t *
svn_error_createf (apr_status_t apr_err,
                   int src_err,
                   svn_error_t *child,
                   apr_pool_t *pool,
                   const char *fmt,
                   ...)
{
  svn_error_t *err;

  va_list ap;

  err = make_error_internal (apr_err, src_err, child, pool);

  va_start (ap, fmt);
  err->message = apr_pvsprintf (err->pool, fmt, ap);
  va_end (ap);

  return err;
}


svn_error_t *
svn_error_quick_wrap (svn_error_t *child, const char *new_msg)
{
  return svn_error_create (child->apr_err,
                           child->src_err,
                           child,
                           NULL,   /* allocate directly in child's pool */
                           new_msg);
}


void
svn_error_compose (svn_error_t *chain, svn_error_t *new_err)
{
  while (chain->child)
    chain = chain->child;

  chain->child = new_err;
}


void
svn_error_free (svn_error_t *err)
{
  svn_pool_destroy (err->pool);
}


void
svn_handle_error (svn_error_t *err, FILE *stream, svn_boolean_t fatal)
{
  char buf[200];

  /* Pretty-print the error */
  /* Note: we can also log errors here someday. */

#ifdef SVN_DEBUG
  if (err->file)
    fprintf (stream, "\n%s:%ld\n", err->file, err->line);
  else
    fprintf (stream, "\n%s\n", SVN_FILE_LINE_UNDEFINED);
#else
  fputc ('\n', stream);
#endif /* SVN_DEBUG */

  /* Is this a Subversion-specific error code? */
  if ((err->apr_err > APR_OS_START_USEERR) 
      && (err->apr_err <= APR_OS_START_CANONERR))
    fprintf (stream, "svn_error: #%d : <%s>\n", err->apr_err,
             svn_strerror (err->apr_err, buf, sizeof (buf)));

  /* Otherwise, this must be an APR error code. */
  else
    fprintf (stream, "apr_error: #%d, src_err %d : <%s>\n",
             err->apr_err,
             err->src_err,
             apr_strerror (err->apr_err, buf, sizeof(buf)));

  if (err->message)
    fprintf (stream, "  %s", err->message);

  fputc ('\n', stream);
  fflush (stream);

  if (err->child)
    svn_handle_error (err->child, stream, 0);

  if (fatal)
    abort ();
}



void 
svn_handle_warning (void *data, const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);

  fprintf (stderr, "\n");
  fflush (stderr);
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
    if (defn->errcode == statcode)
      {
        apr_cpystrn (buf, defn->errdesc, bufsize);
        return buf;
      }

  return apr_strerror (statcode, buf, bufsize);
}



/*-----------------------------------------------------------------*/
/*
   Macros to make the preprocessor logic less confusing.
   We need to always have svn_pool_xxx aswell as
   svn_pool_xxx_debug, where one of the two is a simple
   wrapper calling the other.
 */
#if !APR_POOL_DEBUG
#define SVN_POOL_FUNC_DEFINE(rettype, name) \
  rettype name(apr_pool_t *pool)

#else /* APR_POOL_DEBUG */
#undef svn_pool_create
#undef svn_pool_clear

#define SVN_POOL_FUNC_DEFINE(rettype, name) \
  rettype name##_debug(apr_pool_t *pool, const char *file_line)
   
#endif /* APR_POOL_DEBUG */


SVN_POOL_FUNC_DEFINE(apr_pool_t *, svn_pool_create)
{
  apr_pool_t *ret_pool;

#if !APR_POOL_DEBUG
  apr_pool_create_ex (&ret_pool, pool, abort_on_pool_failure,
                      APR_POOL_FDEFAULT);
#else /* APR_POOL_DEBUG */
  apr_pool_create_ex_debug (&ret_pool, pool, abort_on_pool_failure,
                            APR_POOL_FDEFAULT, file_line);
#endif /* APR_POOL_DEBUG */

  /* If there is no parent, then initialize ret_pool as the "top". */
  if (pool == NULL)
    {
      apr_status_t apr_err = svn_error_init_pool (ret_pool);
      if (apr_err)
        abort_on_pool_failure (apr_err);
    }
  else
    {
      /* Inherit the error pool from the parent. */
      svn_pool__inherit_error_pool (ret_pool);
    }

  /* Sanity check:  do we actually have an error pool? */
  {
    svn_boolean_t subpool_of_p_p;
    apr_pool_t *error_pool;
    svn_error__get_error_pool (ret_pool, &error_pool,
                               &subpool_of_p_p);
    if (! error_pool)
      abort_on_pool_failure (SVN_ERR_BAD_CONTAINING_POOL);
  }

  return ret_pool;
}


SVN_POOL_FUNC_DEFINE(void, svn_pool_clear)
{
  apr_pool_t *error_pool;
  svn_boolean_t subpool_of_p_p;  /* That's "predicate" to you, bud. */

  /* Get the error_pool from this pool.  If it's rooted in this pool, we'll
     need to re-create it after we clear the pool. */
  svn_error__get_error_pool (pool, &error_pool, &subpool_of_p_p);

  /* Clear the pool.  All userdata of this pool is now invalid. */
#if !APR_POOL_DEBUG
  apr_pool_clear (pool);
#else /* APR_POOL_DEBUG */
  apr_pool_clear_debug (pool, file_line);
#endif /* APR_POOL_DEBUG */

  if (subpool_of_p_p)
    {
      /* Here we have a problematic situation.  We cleared the pool P,
         which invalidated all its userdata.  The problem is that as
         far as we can tell, the error pool on this pool isn't a copy
         of the original, it *is* the original.  We need to re-create
         it in this pool, since it has been cleared.

         This turns out to be not that big of a deal.  We don't
         actually need to keep *the* original error pool -- we can
         just initialize a new error pool to stuff into P here after
         it's been cleared. */

      /* Make new error pool. */
      svn_error__make_error_pool (pool, &error_pool);
    }

  /* Now, reset the error pool on P. */
  svn_error__set_error_pool (pool, error_pool, subpool_of_p_p);
}


/* Wrappers that ensure binary compatibility */
#if !APR_POOL_DEBUG
apr_pool_t *
svn_pool_create_debug (apr_pool_t *pool, const char *file_line)
{
  return svn_pool_create (pool);
}

void
svn_pool_clear_debug (apr_pool_t *pool, const char *file_line)
{
  svn_pool_clear (pool);
}

#else /* APR_POOL_DEBUG */
apr_pool_t *
svn_pool_create (apr_pool_t *pool)
{
  return svn_pool_create_debug (pool, SVN_FILE_LINE_UNDEFINED);
}

void
svn_pool_clear (apr_pool_t *pool)
{
  svn_pool_clear_debug (pool, SVN_FILE_LINE_UNDEFINED);
}
#endif /* APR_POOL_DEBUG */



/*
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end: 
 */
