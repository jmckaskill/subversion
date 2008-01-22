/*
 * opt-test.c -- test the option functions
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
#include "svn_opt.h"
#include <apr_general.h>

#include "../svn_test.h"


static svn_error_t *
test_parse_peg_rev(const char **msg,
                   svn_boolean_t msg_only,
                   svn_test_opts_t *opts,
                   apr_pool_t *pool)
{
  apr_size_t i;
  static struct {
      const char *input;
      const char *path; /* NULL means an error is expected. */
      svn_opt_revision_t peg;
  } const tests[] = {
    { "foo/bar",              "foo/bar",      {svn_opt_revision_unspecified} },
    { "foo/bar@13",           "foo/bar",      {svn_opt_revision_number, {13}} },
    { "foo/bar@HEAD",         "foo/bar",      {svn_opt_revision_head} },
    { "foo/bar@{1999-12-31}", "foo/bar",      {svn_opt_revision_date, {0}} },
    { "http://a/b@27",        "http://a/b",   {svn_opt_revision_number, {27}} },
    { "http://a/b@COMMITTED", "http://a/b",   {svn_opt_revision_committed} },
    { "foo/bar@1:2",          NULL,           {svn_opt_revision_unspecified} },
    { "foo/bar@baz",          NULL,           {svn_opt_revision_unspecified} },
    { "foo/bar@",             "foo/bar",      {svn_opt_revision_unspecified} },
    { "foo/bar/@13",          "foo/bar/",     {svn_opt_revision_number, {13}} },
    { "foo/bar@@13",          "foo/bar@",     {svn_opt_revision_number, {13}} },
    { "foo/@bar@HEAD",        "foo/@bar",     {svn_opt_revision_head} },
    { "foo@/bar",             "foo@/bar",     {svn_opt_revision_unspecified} },
    { "foo@HEAD/bar",         "foo@HEAD/bar", {svn_opt_revision_unspecified} },
  };

  *msg = "test svn_opt_parse_path";
  if (msg_only)
    return SVN_NO_ERROR;

  for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
      const char *path;
      svn_opt_revision_t peg;
      svn_error_t *err;

      err = svn_opt_parse_path(&peg, &path, tests[i].input, pool);
      if (err)
        {
          svn_error_clear(err);
          if (tests[i].path)
            {
              return svn_error_createf
                (SVN_ERR_TEST_FAILED, NULL,
                 "svn_opt_parse_path ('%s') returned an error instead of '%s'",
                 tests[i].input, tests[i].path);
            }
        }
      else
        {
          if ((path == NULL)
              || (tests[i].path == NULL)
              || (strcmp(path, tests[i].path) != 0)
              || (peg.kind != tests[i].peg.kind)
              || (peg.kind == svn_opt_revision_number && peg.value.number != tests[i].peg.value.number))
            return svn_error_createf
              (SVN_ERR_TEST_FAILED, NULL,
               "svn_opt_parse_path ('%s') returned '%s' instead of '%s'", tests[i].input,
               path ? path : "NULL", tests[i].path ? tests[i].path : "NULL");
        }
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_svn_opt_args_to_target_array2(const char **msg,
                                   svn_boolean_t msg_only,
                                   svn_test_opts_t *opts,
                                   apr_pool_t *pool)
{
  apr_size_t i;
  static struct {
    const char *input;
    const char *output; /* NULL means an error is expected. */
  } const tests[] = {
    { ".",                      "" },
    { ".@BASE",                 "@BASE" },
    { "foo///bar",              "foo/bar" },
    { "foo///bar@13",           "foo/bar@13" },
    { "foo///bar@HEAD",         "foo/bar@HEAD" },
    { "foo///bar@{1999-12-31}", "foo/bar@{1999-12-31}" },
    { "http://a//b////",        "http://a/b" },
    { "http://a///b@27",        "http://a/b@27" },
    { "http://a/b//@COMMITTED", "http://a/b@COMMITTED" },
    { "foo///bar@1:2",          "foo/bar@1:2" },
    { "foo///bar@baz",          "foo/bar@baz" },
    { "foo///bar@",             "foo/bar@" },
    { "foo///bar///@13",        "foo/bar@13" },
    { "foo///bar@@13",          "foo/bar@@13" },
    { "foo///@bar@HEAD",        "foo/@bar@HEAD" },
    { "foo@///bar",             "foo@/bar" },
    { "foo@HEAD///bar",         "foo@HEAD/bar" },
  };

  *msg = "test svn_opt_args_to_target_array2";
  if (msg_only)
    return SVN_NO_ERROR;

  for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
      const char *input = tests[i].input;
      const char *expected_output = tests[i].output;
      apr_array_header_t *targets;
      apr_getopt_t *os;
      const int argc = 2;
      const char *argv[] = { "opt-test", input, NULL };
      apr_status_t apr_err;
      svn_error_t *err;

      apr_err = apr_getopt_init(&os, pool, argc, argv);
      if (apr_err)
        return svn_error_wrap_apr(apr_err,
                                  "Error initializing command line arguments");

      err = svn_opt_args_to_target_array2(&targets, os, NULL, pool);

      if (expected_output)
        {
          const char *actual_output;

          if (err)
            return err;
          if (argc - 1 != targets->nelts)
            return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                     "Passed %d target(s) to "
                                     "svn_opt_args_to_target_array2() but "
                                     "got %d back.",
                                     argc - 1,
                                     targets->nelts);

          actual_output = APR_ARRAY_IDX(targets, 0, const char *);

          if (! svn_path_is_canonical(actual_output, pool))
            return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                     "Input '%s' to "
                                     "svn_opt_args_to_target_array2() should "
                                     "have returned a canonical path but "
                                     "'%s' is not.",
                                     input,
                                     actual_output);
            
          if (strcmp(expected_output, actual_output) != 0)
            return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                     "Input '%s' to "
                                     "svn_opt_args_to_target_array2() should "
                                     "have returned '%s' but returned '%s'.",
                                     input,
                                     expected_output,
                                     actual_output);
        }
      else
        {
          if (! err)
            return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                     "Unexpected success in passing '%s' "
                                     "to svn_opt_args_to_target_array2().",
                                     input);
        }
    }

  return SVN_NO_ERROR;
}


/* The test table.  */

struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS(test_parse_peg_rev),
    SVN_TEST_PASS(test_svn_opt_args_to_target_array2),
    SVN_TEST_NULL
  };
