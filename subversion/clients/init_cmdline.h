/*
 * init_cmdline.h : Initialzation for command-line programs.
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



#ifndef SVN_INIT_CMDLINE_H
#define SVN_INIT_CMDLINE_H

#include <locale.h>             /* for setlocale() */
#include <stdlib.h>             /* for atexit() */

#define APR_WANT_STDIO
#include <apr_want.h>           /* for stdio stuff */
#include <apr_errno.h>          /* for apr_strerror */
#include <apr_general.h>        /* for apr_initialize/apr_terminate */
#include "svn_private_config.h" /* for SVN_WIN32 */


/* Set up the locale for character conversion, and initialize APR. If
   ERROR_STREAM is non-null, print error messages to the stream, using
   PROGNAME as the program name. Return EXIT_SUCCESS if successful,
   otherwise EXIT_FAILURE. */

static int
init_cmdline (const char *progname, FILE *error_stream)
{
  apr_status_t status;

#ifdef SVN_WIN32
  /* Force the Windows console to use the same multibyte character set
     that the app uses internally. */
  {
    UINT codepage = GetACP();
    if (!SetConsoleCP(codepage))
      {
        if (error_stream)
          fprintf(error_stream,
                  "%s: error: cannot set console input codepage (code %lu)\n",
                  progname, (unsigned long) GetLastError());
        return EXIT_FAILURE;
      }

    if (!SetConsoleOutputCP(codepage))
      {
        if (error_stream)
          fprintf(error_stream,
                  "%s: error: cannot set console output codepage (code %lu)\n",
                  progname, (unsigned long) GetLastError());
        return EXIT_FAILURE;
      }
  }
#endif /* SVN_WIN32 */

  /* C programs default to the "C" locale. But because svn is supposed
     to be i18n-aware, it should inherit the default locale of its
     environment.  */
  if (!setlocale(LC_ALL, ""))
    {
      if (error_stream)
        fprintf(error_stream,
                "%s: error: cannot set the locale\n",
                progname);
      return EXIT_FAILURE;
    }

  /* Initialize the APR subsystem, and register an atexit() function
     to Uninitialize that subsystem at program exit. */
  status = apr_initialize();
  if (status)
    {
      if (error_stream)
        {
          char buf[1024];
          apr_strerror(status, buf, sizeof(buf) - 1);
          fprintf(error_stream,
                  "%s: error: cannot initialize APR: %s\n",
                  progname, buf);
        }
      return EXIT_FAILURE;
    }

  if (0 > atexit(apr_terminate))
    {
      if (error_stream)
        fprintf(error_stream,
                "%s: error: atexit registration failed\n",
                progname);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}

#endif /* SVN_INIT_CMDLINE_H */
