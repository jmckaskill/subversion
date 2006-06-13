/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_cmdline.h
 * @brief Support functions for command line programs
 */




#ifndef SVN_CMDLINE_H
#define SVN_CMDLINE_H

#ifndef DOXYGEN_SHOULD_SKIP_THIS
#define APR_WANT_STDIO
#endif
#include <apr_want.h>

#include "svn_utf.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/** Set up the locale for character conversion, and initialize APR.
 * If @a error_stream is non-null, print error messages to the stream,
 * using @a progname as the program name. Return @c EXIT_SUCCESS if
 * successful, otherwise @c EXIT_FAILURE.
 *
 * @note This function should be called exactly once at program startup,
 *       before calling any other APR or Subversion functions.
 */
int svn_cmdline_init (const char *progname, FILE *error_stream);


/** Set @a *dest to an output-encoded C string from UTF-8 C string @a
 * src; allocate @a *dest in @a pool.
 */
svn_error_t *svn_cmdline_cstring_from_utf8 (const char **dest,
                                            const char *src,
                                            apr_pool_t *pool);

/** Like svn_utf_cstring_from_utf8_fuzzy(), but converts to an
 * output-encoded C string. */
const char *svn_cmdline_cstring_from_utf8_fuzzy (const char *src,
                                                 apr_pool_t *pool);

/** Set @a *dest to a UTF-8-encoded C string from input-encoded C
 * string @a src; allocate @a *dest in @a pool.
 */
svn_error_t * svn_cmdline_cstring_to_utf8 (const char **dest,
                                           const char *src,
                                           apr_pool_t *pool);

/** Set @a *dest to an output-encoded natively-formatted path string
 * from canonical path @a src; allocate @a *dest in @a pool.
 */
svn_error_t *svn_cmdline_path_local_style_from_utf8 (const char **dest,
                                                     const char *src,
                                                     apr_pool_t *pool);

/** Write to stdout, using a printf-like format string @a fmt, passed
 * through apr_pvsprintf().  All string arguments are in UTF-8; the output
 * is converted to the output encoding.  Use @a pool for temporary
 * allocation.
 *
 * @since New in 1.1.
 */
svn_error_t *svn_cmdline_printf (apr_pool_t *pool,
                                 const char *fmt,
                                 ...)
       __attribute__((format(printf, 2, 3)));

#if APR_CHARSET_EBCDIC
/** Write to stdout, using a printf-like format string @a fmt, passed
 * through svn_ebcdic_pvsprintf2().  All string arguments are in UTF-8; Unlike
 * svn_cmdline_printf the output is not converted to the output encoding but
 * left in utf-8.  Use @a pool for temporary allocation.
 */
svn_error_t *svn_cmdline_printf_ebcdic (apr_pool_t *pool,
                                        const char *fmt,
                                        ...)
       __attribute__((format(printf, 2, 3)));


/** Write to stdout, using a printf-like format string @a fmt, passed
 * through svn_ebcdic_pvsprintf().  All string arguments are in UTF-8; Like
 * svn_cmdline_printf the output is converted to the output encoding.
 * Use @a pool for temporary allocation.
 */
svn_error_t *svn_cmdline_printf_ebcdic2 (apr_pool_t *pool,
                                         const char *fmt,
                                         ...)
       __attribute__((format(printf, 2, 3)));
#endif

/** Write to the stdio @a stream, using a printf-like format string @a fmt,
 * passed through apr_pvsprintf().  All string arguments are in UTF-8;
 * the output is converted to the output encoding.  Use @a pool for
 * temporary allocation.
 *
 * @since New in 1.1.
 */
svn_error_t *svn_cmdline_fprintf (FILE *stream,
                                  apr_pool_t *pool,
                                  const char *fmt,
                                  ...)
       __attribute__((format(printf, 3, 4)));

#if APR_CHARSET_EBCDIC
/** Write to the stdio @a stream, using a printf-like format string @a fmt,
 * passed through svn_ebcdic_pvsprintf2().  All string arguments are in UTF-8;
 * Unlike svn_cmdline_fprintf the output is not converted to the output
 * encoding but left in utf-8.  Use @a pool for temporary allocation.
 */
svn_error_t *svn_cmdline_fprintf_ebcdic (FILE *stream,
                                         apr_pool_t *pool,
                                         const char *fmt,
                                         ...)
       __attribute__((format(printf, 3, 4)));

/** Write to the stdio @a stream, using a printf-like format string @a fmt,
 * passed through svn_ebcdic_pvsprintf2().  All string arguments are in UTF-8;
 * Like svn_cmdline_fprintf the output is converted to the output encoding.
 * Use @a pool for temporary allocation.
 */
svn_error_t *svn_cmdline_fprintf_ebcdic2 (FILE *stream,
                                          apr_pool_t *pool,
                                          const char *fmt,
                                          ...)
       __attribute__((format(printf, 3, 4)));
#endif

/** Output the @a string to the stdio @a stream, converting from UTF-8
 * to the output encoding.  Use @a pool for temporary allocation.
 *
 * @since New in 1.1.
 */
svn_error_t *svn_cmdline_fputs (const char *string,
                                FILE *stream,
                                apr_pool_t *pool);

/** Flush output buffers of the stdio @a stream, returning an error if that
 * fails.  This is just a wrapper for the standard fflush() function for
 * consistent error handling. 
 *
 * @since New in 1.1.
 */
svn_error_t *svn_cmdline_fflush (FILE *stream);

/** Return the name of the output encoding allocated in @a pool, or @c
 * APR_LOCALE_CHARSET if the output encoding is the same as the locale
 * encoding.
 *
 * @since New in 1.3.
 */
const char *svn_cmdline_output_encoding (apr_pool_t *pool);

/** Handle @a error in preparation for immediate exit from a
 * command-line client.  Specifically:
 *
 * Call svn_handle_error2(@a error, stderr, FALSE, @a prefix), clear
 * @a error, destroy @a pool iff it is non-NULL, and return EXIT_FAILURE.
 *
 * @since New in 1.3.
 */
int svn_cmdline_handle_exit_error (svn_error_t *error,
                                   apr_pool_t *pool,
                                   const char *prefix);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_CMDLINE_H */
