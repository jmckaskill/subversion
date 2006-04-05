/*
 * tests-main.c:  shared main() & friends for SVN test-suite programs
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



#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <apr_pools.h>
#include <apr_general.h>
#include <apr_lib.h>

#include "svn_pools.h"
#include "svn_error.h"
#include "svn_test.h"
#include "svn_io.h"
#include "svn_path.h"
#include "svn_private_config.h"


/* Some Subversion test programs may want to parse options in the
   argument list, so we remember it here. */
int test_argc;
char **test_argv;


/* Test option: Print more output */
static int verbose_mode = 0;

/* Test option: Remove test directories after success */
static int cleanup_mode = 0;


/* ================================================================= */
/* Stuff for cleanup processing */

/* When non-zero, don't remove test directories */
static int skip_cleanup = 0;

/* All cleanup actions are registered as cleanups on this pool. */
static apr_pool_t *cleanup_pool = 0;

static apr_status_t
cleanup_rmtree (void *data)
{
  if (!skip_cleanup)
    {
      apr_pool_t *pool = svn_pool_create (NULL);
      const char *path = data;

      /* Ignore errors here. */
      svn_error_t *err = svn_io_remove_dir (path, pool);
      if (verbose_mode)
        {
          if (err)
            printf ("FAILED CLEANUP: %s\n", path);
          else
            printf ("CLEANUP: %s\n", path);
        }
      svn_pool_destroy (pool);
    }
  return APR_SUCCESS;
}


void
svn_test_add_dir_cleanup (const char *path)
{
  if (cleanup_mode)
    {
      const char *abspath;
      svn_error_t *err = svn_path_get_absolute (&abspath, path, cleanup_pool);
      if (!err)
        apr_pool_cleanup_register (cleanup_pool, abspath, cleanup_rmtree,
                                   apr_pool_cleanup_null);
      else if (verbose_mode)
        printf ("FAILED ABSPATH: %s\n", path);
    }
}


/* ================================================================= */
/* Quite a few tests use random numbers. */

apr_uint32_t
svn_test_rand (apr_uint32_t *seed)
{
  *seed = (*seed * 1103515245UL + 12345UL) & 0xffffffffUL;
  return *seed;
}


/* ================================================================= */


/* Determine the array size of test_funcs[], the inelegant way.  :)  */
static int
get_array_size (void)
{
  int i;

  for (i = 1; test_funcs[i].func; i++)
    {
    }

  return (i - 1);
}



/* Execute a test number TEST_NUM.  Pretty-print test name and dots
   according to our test-suite spec, and return the result code. */
static int
do_test_num (const char *progname, 
             int test_num, 
             svn_boolean_t msg_only,
             svn_test_opts_t *opts,
             apr_pool_t *pool)
{
  svn_test_driver_t func;
  svn_boolean_t skip, xfail;
  svn_error_t *err;
  int array_size = get_array_size();
  const char *msg = 0;  /* the message this individual test prints out */

  /* Check our array bounds! */
  if ((test_num > array_size) || (test_num <= 0))
    {
      printf ("FAIL: %s: THERE IS NO TEST NUMBER %2d\n", progname, test_num);
      return (skip_cleanup = 1);  /* BAIL, this test number doesn't exist. */
    }
  else
    {
      func = test_funcs[test_num].func;
      skip = (test_funcs[test_num].mode == svn_test_skip);
      xfail = (test_funcs[test_num].mode == svn_test_xfail);
    }

  /* Do test */
  err = func(&msg, msg_only || skip, opts, pool);

  /* If we got an error, print it out.  */
  if (err)
    {
      svn_handle_error2 (err, stdout, FALSE, "svn_tests: ");
      svn_error_clear (err);
    }

  if (msg_only)
    {
      printf (" %2d     %-5s  %s\n",
              test_num,
              (xfail ? "XFAIL" : (skip ? "SKIP" : "")),
              msg ? msg : "(test did not provide name)");
    }
  else
    {
      printf ("%s %s %d: %s\n", 
              (err
               ? (xfail ? "XFAIL:" : "FAIL: ")
               : (xfail ? "XPASS:" : (skip ? "SKIP: " : "PASS: "))),
              progname,
              test_num, 
              msg ? msg : "(test did not provide name)");
    }

  if (msg)
    {
      int len = strlen (msg);
      if (len > 50)
        printf ("WARNING: Test docstring exceeds 50 characters\n");
      if (msg[len - 1] == '.')
        printf ("WARNING: Test docstring ends in a period (.)\n");
      if (apr_isupper(msg[0]))
        printf ("WARNING: Test docstring is capitalized\n");
    }
    
  /* Fail on unexpected result -- FAIL or XPASS. */
  skip_cleanup = ((err != SVN_NO_ERROR) != (xfail != 0));
  return skip_cleanup;
}


/* Standard svn test program */
int
main (int argc, char *argv[])
{
  char *prog_name;
  int test_num;
  int i;
  int got_error = 0;
  apr_pool_t *pool, *test_pool;
  int ran_a_test = 0;
  char **arg;
  /* How many tests are there? */
  int array_size = get_array_size();
  
  svn_test_opts_t opts = { NULL };

  opts.fs_type = DEFAULT_FS_TYPE;

  /* Initialize APR (Apache pools) */
  if (apr_initialize () != APR_SUCCESS)
    {
      printf ("apr_initialize() failed.\n");
      exit (1);
    }

  /* set up the global pool */
  pool = svn_pool_create (NULL);

  /* Strip off any leading path components from the program name.  */
  prog_name = strrchr (argv[0], '/');
  if (prog_name)
    prog_name++;
  else
    {
      /* Just check if this is that weird platform that uses \ instead
         of / for the path separator. */
      prog_name = strrchr (argv[0], '\\');
      if (prog_name)
        prog_name++;
      else
        prog_name = argv[0];
    }

  /* Remember the command line */
  test_argc = argc;
  test_argv = argv;

  /* Scan the command line for the --verbose and --cleanup flags */
  for (arg = &argv[1]; *arg; ++arg)
    {
      if (strcmp(*arg, "--cleanup") == 0)
        cleanup_mode = 1;
      else if (strcmp(*arg, "--verbose") == 0)
        verbose_mode = 1;
      else if (strncmp(*arg, "--fs-type=", 10) == 0)
#if !AS400
        opts.fs_type = apr_pstrdup (pool, (*arg) + 10);
#else
        /* Only fs type support on the iSeries */
        opts.fs_type = DEFAULT_FS_TYPE;
#endif
    }

  /* Create an iteration pool for the tests */
  cleanup_pool = svn_pool_create (pool);
  test_pool = svn_pool_create (pool);

  if (argc >= 2)  /* notice command-line arguments */
    {
      if (! strcmp (argv[1], "list"))
        {
          ran_a_test = 1;

          /* run all tests with MSG_ONLY set to TRUE */

          printf("Test #  Mode   Test Description\n"
                 "------  -----  ----------------\n");
          for (i = 1; i <= array_size; i++)
            {
              if (do_test_num (prog_name, i, TRUE, &opts, test_pool))
                got_error = 1;

              /* Clear the per-function pool */
              svn_pool_clear (test_pool);
              svn_pool_clear (cleanup_pool);
            }
        }
      else
        {
          for (i = 1; i < argc; i++)
            {
              if (apr_isdigit (argv[i][0]))
                {
                  ran_a_test = 1;
                  test_num = atoi (argv[i]);
                  if (do_test_num (prog_name, test_num, FALSE, &opts, test_pool))
                    got_error = 1;

                  /* Clear the per-function pool */
                  svn_pool_clear (test_pool);
                  svn_pool_clear (cleanup_pool);
                }
              else if (argv[i][0] != '-')
                {
                  /* (probably) a source directory pathname */
                  printf ("notice: ignoring argument %d: '%s'\n", i, argv[i]);
                }
            }
        }
    }

  if (! ran_a_test)
    {
      /* just run all tests */
      for (i = 1; i <= array_size; i++)
        {
          if (do_test_num (prog_name, i, FALSE, &opts, test_pool))
            got_error = 1;

          /* Clear the per-function pool */
          svn_pool_clear (test_pool);
          svn_pool_clear (cleanup_pool);
        }
    }

  /* Clean up APR */
  svn_pool_destroy (pool);      /* takes test_pool with it */
  apr_terminate();

  exit (got_error);
  return got_error;
}
