/* svn_error.h:  common exception handling for Subversion
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




#ifndef SVN_ERROR_H
#define SVN_ERROR_H

#include <apr.h>
#include <apr_errno.h>     /* APR's error system */
#include <apr_pools.h>

#define APR_WANT_STDIO
#include <apr_want.h>

#include <svn_types.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define SVN_NO_ERROR   0   /* the best kind of (svn_error_t *) ! */

/* The actual error codes are kept in a separate file; see comments
   there for the reasons why. */
#include "svn_error_codes.h"

/* Put a English description of STATCODE into BUF and return BUF,
   null-terminated.  STATCODE is either an svn error or apr error.  */
char *svn_strerror (apr_status_t statcode, char *buf, apr_size_t bufsize);



/*** SVN error creation and destruction. ***/

/*
  svn_error_create() : for creating nested exception structures.

  Input:  an APR or SVN custom error code,
          the original errno,
          a "child" error to wrap,
          a pool
          a descriptive message,

  Returns:  a new error structure (containing the old one).

  Notes: Errors are always allocated in a special top-level error
         pool, obtained from POOL's attributes.  If POOL is null, then
         the error pool is obtained from CHILD's pool's attributes.
         A pool has this attribute if it was allocated using
         svn_pool_create().

         If creating the "bottommost" error in a chain, pass NULL for
         the child argument.
 */
svn_error_t *svn_error_create (apr_status_t apr_err,
                               int src_err,
                               svn_error_t *child,
                               apr_pool_t *pool,
                               const char *message);

/* Create an error structure with the given APR_ERR, SRC_ERR, CHILD,
   and POOL, with a printf-style error message produced by passing
   FMT, ... through apr_psprintf.  */
svn_error_t *svn_error_createf (apr_status_t apr_err,
                                int src_err,
                                svn_error_t *child,
                                apr_pool_t *pool,
                                const char *fmt, 
                                ...)
       __attribute__ ((format (printf, 5, 6)));


/* A quick n' easy way to create a wrappered exception with your own
   message, before throwing it up the stack.  (It uses all of the
   child's fields.)  */
svn_error_t *svn_error_quick_wrap (svn_error_t *child, const char *new_msg);


/* Add NEW to the end of CHAIN's chain of errors; i.e., NEW will be
   the last child error in CHAIN.  */
void svn_error_compose (svn_error_t *chain, svn_error_t *new_err);


/* Free ERROR by destroying its pool; note that the pool may be shared
   with wrapped child errors inside this error. */
void svn_error_free (svn_error_t *error);


/* Very basic default error handler: print out error stack, and quit
   iff the FATAL flag is set. */
void svn_handle_error (svn_error_t *error,
                       FILE *stream,
                       svn_boolean_t fatal);

/* Basic, default warning handler, just prints to stderr. */
void svn_handle_warning (void *data, const char *fmt, ...);


/* A statement macro for checking error return values.
   Evaluate EXPR.  If it yields an error, return that error from the
   current function.  Otherwise, continue.

   The `do { ... } while (0)' wrapper has no semantic effect, but it
   makes this macro syntactically equivalent to the expression
   statement it resembles.  Without it, statements like

     if (a)
       SVN_ERR (some operation);
     else
       foo;

   would not mean what they appear to.  */

#define SVN_ERR(expr)                           \
  do {                                          \
    svn_error_t *svn_err__temp = (expr);        \
    if (svn_err__temp)                          \
      return svn_err__temp;                     \
  } while (0)


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_ERROR_H */


/* ----------------------------------------------------------------
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
