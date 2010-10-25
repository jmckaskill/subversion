/*
 * dirent_uri-test.c -- test the directory entry and URI functions
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <stdio.h>
#include <string.h>

#if defined(WIN32) || defined(__OS2__)
#include <direct.h>
#define getcwd _getcwd
#define getdcwd _getdcwd
#else
#include <unistd.h> /* for getcwd() */
#endif

#include <apr_general.h>

#include "svn_pools.h"
#include "svn_dirent_uri.h"

#include "../svn_test.h"

#define SVN_EMPTY_PATH ""

/* This check must match the check on top of dirent_uri.c */
#if defined(WIN32) || defined(__CYGWIN__) || defined(__OS2__)
#define SVN_USE_DOS_PATHS
#endif

#define COUNT_OF(x) (sizeof(x) / sizeof(x[0]))

static svn_error_t *
test_dirent_is_root(apr_pool_t *pool)
{
  apr_size_t i;

  /* Paths to test and their expected results. */
  struct {
    const char *path;
    svn_boolean_t result;
  } tests[] = {
    { "/",             TRUE  },
    { "/foo/bar",      FALSE },
    { "/foo",          FALSE },
    { "",              FALSE },
#ifdef SVN_USE_DOS_PATHS
    { "X:/foo",        FALSE },
    { "X:/",           TRUE },
    { "X:foo",         FALSE }, /* Based on non absolute root */
    { "X:",            TRUE },
    { "//srv/shr",     TRUE },
    { "//srv/shr/fld", FALSE },
    { "//srv/s r",     TRUE },
    { "//srv/s r/fld", FALSE },
#else /* !SVN_USE_DOS_PATHS */
    { "/",             TRUE },
    { "/X:foo",        FALSE },
    { "/X:",           FALSE },
#endif /* SVN_USE_DOS_PATHS */
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t retval;

      retval = svn_dirent_is_root(tests[i].path, strlen(tests[i].path));
      if (tests[i].result != retval)
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_dirent_is_root (%s) returned %s instead of %s",
           tests[i].path, retval ? "TRUE" : "FALSE",
           tests[i].result ? "TRUE" : "FALSE");
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_is_root(apr_pool_t *pool)
{
  apr_size_t i;

  /* Paths to test and their expected results. */
  struct {
    const char *path;
    svn_boolean_t result;
  } tests[] = {
    { "/foo/bar",      FALSE },
    { "/foo",          FALSE },
    { "/",             TRUE },
    { "",              FALSE },
    { "X:/foo",        FALSE },
    { "X:/",           FALSE },
    { "X:foo",         FALSE },
    { "X:",            FALSE },
    { "file://",       TRUE },
    { "file://a",      FALSE },
    { "file:///a",     FALSE },
    { "file:///A:/",   FALSE },
    { "http://server", TRUE },
    { "http://server/file", FALSE },
    { "http://",       TRUE },
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t retval;

      retval = svn_uri_is_root(tests[i].path, strlen(tests[i].path));
      if (tests[i].result != retval)
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_uri_is_root (%s) returned %s instead of %s",
           tests[i].path, retval ? "TRUE" : "FALSE",
           tests[i].result ? "TRUE" : "FALSE");
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_is_absolute(apr_pool_t *pool)
{
  apr_size_t i;

  /* Paths to test and their expected results. */
  struct {
    const char *path;
    svn_boolean_t result;
  } tests[] = {
    { "foo/bar",       FALSE },
    { "foo",           FALSE },
    { "",              FALSE },
#ifdef SVN_USE_DOS_PATHS
    { "/foo/bar",      FALSE },
    { "/foo",          FALSE },
    { "/",             FALSE },
    { "C:/foo",        TRUE },
    { "C:/",           TRUE },
    { "c:/",           FALSE },
    { "c:/foo",        FALSE },
    { "//srv/shr",     TRUE },
    { "//srv/shr/fld", TRUE },
    { "//srv/s r",     TRUE },
    { "//srv/s r/fld", TRUE },
#else /* !SVN_USE_DOS_PATHS */
    { "/foo/bar",      TRUE },
    { "/foo",          TRUE },
    { "/",             TRUE },
    { "X:/foo",        FALSE },
    { "X:/",           FALSE },
#endif /* SVN_USE_DOS_PATHS */
    { "X:foo",         FALSE }, /* Not special on Posix, relative on Windows */
    { "X:foo/bar",     FALSE },
    { "X:",            FALSE },
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t retval;

      retval = svn_dirent_is_absolute(tests[i].path);
      if (tests[i].result != retval)
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_dirent_is_absolute (%s) returned %s instead of %s",
           tests[i].path, retval ? "TRUE" : "FALSE",
           tests[i].result ? "TRUE" : "FALSE");

      /* Don't get absolute paths for the UNC paths, because this will
         always fail */
      if (tests[i].result &&
          strncmp(tests[i].path, "//", 2) != 0)
        {
          const char *abspath;

          SVN_ERR(svn_dirent_get_absolute(&abspath, tests[i].path, pool));

          if (tests[i].result != (strcmp(tests[i].path, abspath) == 0))
            return svn_error_createf(
                          SVN_ERR_TEST_FAILED,
                          NULL,
                          "svn_dirent_is_absolute(%s) returned TRUE, but "
                          "svn_dirent_get_absolute() returned \"%s\"",
                          tests[i].path,
                          abspath);
        }
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_is_absolute(apr_pool_t *pool)
{
  apr_size_t i;

  /* Paths to test and their expected results. */
  struct {
    const char *path;
    svn_boolean_t result;
  } tests[] = {
    { "/foo/bar",      TRUE },
    { "/foo",          TRUE },
    { "/",             TRUE },
    { "foo/bar",       FALSE },
    { "foo",           FALSE },
    { "",              FALSE },
    { "X:/foo",        FALSE },
    { "X:foo",         FALSE },
    { "X:foo/bar",     FALSE },
    { "X:",            FALSE },
    { "http://",       TRUE },
    { "http://test",   TRUE },
    { "http://foo/bar",TRUE },
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t retval;

      retval = svn_uri_is_absolute(tests[i].path);
      if (tests[i].result != retval)
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_uri_is_absolute (%s) returned %s instead of %s",
           tests[i].path, retval ? "TRUE" : "FALSE",
           tests[i].result ? "TRUE" : "FALSE");
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_join(apr_pool_t *pool)
{
  int i;
  char *result;

  static const char * const joins[][3] = {
    { "abc", "def", "abc/def" },
    { "a", "def", "a/def" },
    { "a", "d", "a/d" },
    { "/", "d", "/d" },
    { "/abc", "d", "/abc/d" },
    { "/abc", "def", "/abc/def" },
    { "/abc", "/def", "/def" },
    { "/abc", "/d", "/d" },
    { "/abc", "/", "/" },
    { "abc", "/def", "/def" },
    { SVN_EMPTY_PATH, "/", "/" },
    { "/", SVN_EMPTY_PATH, "/" },
    { SVN_EMPTY_PATH, "abc", "abc" },
    { "abc", SVN_EMPTY_PATH, "abc" },
    { SVN_EMPTY_PATH, "/abc", "/abc" },
    { SVN_EMPTY_PATH, SVN_EMPTY_PATH, SVN_EMPTY_PATH },
    { "/", "/", "/" },
#ifdef SVN_USE_DOS_PATHS
    { "X:/", SVN_EMPTY_PATH, "X:/" },
    { "X:/", "abc", "X:/abc" },
    { "X:/", "/def", "X:/def" },
    { "X:/abc", "/d", "X:/d" },
    { "X:/abc", "/", "X:/" },
    { "X:/abc", "X:/", "X:/" },
    { "X:/abc", "X:/def", "X:/def" },
    { "X:", SVN_EMPTY_PATH, "X:" },
    { "X:", "abc", "X:abc" },
    { "X:", "/def", "X:/def" },
    { "X:abc", "/d", "X:/d" },
    { "X:abc", "/", "X:/" },
    { "X:abc", "X:/", "X:/" },
    { "X:abc", "X:/def", "X:/def" },
    { "//srv/shr",     "fld",     "//srv/shr/fld" },
    { "//srv/shr/fld", "subfld",  "//srv/shr/fld/subfld" },
    { "//srv/shr/fld", "//srv/shr", "//srv/shr" },
    { "//srv/s r",     "fld",     "//srv/s r/fld" },
    { "aa", "/dir", "/dir"} ,
    { "aa", "A:", "A:" },
    { "aa", "A:file", "A:file"},
    { "A:", "/", "A:/" },
#else /* !SVN_USE_DOS_PATHS */
    { "X:abc", "X:/def", "X:abc/X:/def" },
    { "X:","abc", "X:/abc" },
    { "X:/abc", "X:/def", "X:/abc/X:/def" },
#endif /* SVN_USE_DOS_PATHS */
  };

  for (i = 0; i < COUNT_OF(joins); i++ )
    {
      const char *base = joins[i][0];
      const char *comp = joins[i][1];
      const char *expect = joins[i][2];

      result = svn_dirent_join(base, comp, pool);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_join(\"%s\", \"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 base, comp, result, expect);

      result = svn_dirent_join_many(pool, base, comp, NULL);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_join_many(\"%s\", \"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 base, comp, result, expect);
    }

#define TEST_MANY(args, expect) \
  result = svn_dirent_join_many args ; \
  if (strcmp(result, expect) != 0) \
    return svn_error_createf(SVN_ERR_TEST_FAILED, NULL, \
                             "svn_dirent_join_many" #args " returns \"%s\". " \
                             "expected \"%s\"", \
                             result, expect);

  TEST_MANY((pool, "abc", NULL), "abc");
  TEST_MANY((pool, "/abc", NULL), "/abc");
  TEST_MANY((pool, "/", NULL), "/");

  TEST_MANY((pool, "abc", "def", "ghi", NULL), "abc/def/ghi");
  TEST_MANY((pool, "abc", "/def", "ghi", NULL), "/def/ghi");
  TEST_MANY((pool, "/abc", "def", "ghi", NULL), "/abc/def/ghi");
  TEST_MANY((pool, "abc", "def", "/ghi", NULL), "/ghi");
  TEST_MANY((pool, "/", "def", "/ghi", NULL), "/ghi");
  TEST_MANY((pool, "/", "/def", "/ghi", NULL), "/ghi");

  TEST_MANY((pool, SVN_EMPTY_PATH, "def", "ghi", NULL), "def/ghi");
  TEST_MANY((pool, "abc", SVN_EMPTY_PATH, "ghi", NULL), "abc/ghi");
  TEST_MANY((pool, "abc", "def", SVN_EMPTY_PATH, NULL), "abc/def");
  TEST_MANY((pool, SVN_EMPTY_PATH, "def", SVN_EMPTY_PATH, NULL), "def");
  TEST_MANY((pool, SVN_EMPTY_PATH, SVN_EMPTY_PATH, "ghi", NULL), "ghi");
  TEST_MANY((pool, "abc", SVN_EMPTY_PATH, SVN_EMPTY_PATH, NULL), "abc");
  TEST_MANY((pool, SVN_EMPTY_PATH, "def", "/ghi", NULL), "/ghi");
  TEST_MANY((pool, SVN_EMPTY_PATH, SVN_EMPTY_PATH, "/ghi", NULL), "/ghi");

  TEST_MANY((pool, "/", "def", "ghi", NULL), "/def/ghi");
  TEST_MANY((pool, "abc", "/", "ghi", NULL), "/ghi");
  TEST_MANY((pool, "abc", "def", "/", NULL), "/");
  TEST_MANY((pool, "/", "/", "ghi", NULL), "/ghi");
  TEST_MANY((pool, "/", "/", "/", NULL), "/");
  TEST_MANY((pool, "/", SVN_EMPTY_PATH, "ghi", NULL), "/ghi");
  TEST_MANY((pool, "/", "def", SVN_EMPTY_PATH, NULL), "/def");
  TEST_MANY((pool, SVN_EMPTY_PATH, "/", "ghi", NULL), "/ghi");
  TEST_MANY((pool, "/", SVN_EMPTY_PATH, SVN_EMPTY_PATH, NULL), "/");
  TEST_MANY((pool, SVN_EMPTY_PATH, "/", SVN_EMPTY_PATH, NULL), "/");
  TEST_MANY((pool, SVN_EMPTY_PATH, SVN_EMPTY_PATH, "/", NULL), "/");

#ifdef SVN_USE_DOS_PATHS
  TEST_MANY((pool, "X:/", "def", "ghi", NULL), "X:/def/ghi");
  TEST_MANY((pool, "abc", "X:/", "ghi", NULL), "X:/ghi");
  TEST_MANY((pool, "abc", "def", "X:/", NULL), "X:/");
  TEST_MANY((pool, "X:/", "X:/", "ghi", NULL), "X:/ghi");
  TEST_MANY((pool, "X:/", "X:/", "/", NULL), "/");
  TEST_MANY((pool, "X:/", SVN_EMPTY_PATH, "ghi", NULL), "X:/ghi");
  TEST_MANY((pool, "X:/", "def", SVN_EMPTY_PATH, NULL), "X:/def");
  TEST_MANY((pool, SVN_EMPTY_PATH, "X:/", "ghi", NULL), "X:/ghi");
  TEST_MANY((pool, "X:/", SVN_EMPTY_PATH, SVN_EMPTY_PATH, NULL), "X:/");
  TEST_MANY((pool, SVN_EMPTY_PATH, "X:/", SVN_EMPTY_PATH, NULL), "X:/");
  TEST_MANY((pool, SVN_EMPTY_PATH, SVN_EMPTY_PATH, "X:/", NULL), "X:/");

  TEST_MANY((pool, "X:", "def", "ghi", NULL), "X:def/ghi");
  TEST_MANY((pool, "X:", "X:/", "ghi", NULL), "X:/ghi");
  TEST_MANY((pool, "X:", "X:/", "/", NULL), "/");
  TEST_MANY((pool, "X:", SVN_EMPTY_PATH, "ghi", NULL), "X:ghi");
  TEST_MANY((pool, "X:", "def", SVN_EMPTY_PATH, NULL), "X:def");
  TEST_MANY((pool, SVN_EMPTY_PATH, "X:", "ghi", NULL), "X:ghi");
  TEST_MANY((pool, "//srv/shr", "def", "ghi", NULL), "//srv/shr/def/ghi");
  TEST_MANY((pool, "//srv/shr/fld", "def", "ghi", NULL), "//srv/shr/fld/def/ghi");
  TEST_MANY((pool, "//srv/shr/fld", "def", "//srv/shr", NULL), "//srv/shr");
  TEST_MANY((pool, "//srv/s r/fld", "def", "//srv/s r", NULL), "//srv/s r");
  TEST_MANY((pool, SVN_EMPTY_PATH, "//srv/shr/fld", "def", "ghi", NULL), "//srv/shr/fld/def/ghi");
  TEST_MANY((pool, SVN_EMPTY_PATH, "//srv/shr/fld", "def", "//srv/shr", NULL), "//srv/shr");

  TEST_MANY((pool, "abcd", "/dir", "A:", "file", NULL), "A:file");
  TEST_MANY((pool, "abcd", "A:", "/dir", "file", NULL), "A:/dir/file");

#else /* !SVN_USE_DOS_PATHS */
  TEST_MANY((pool, "X:", "def", "ghi", NULL), "X:/def/ghi");
  TEST_MANY((pool, "X:", SVN_EMPTY_PATH, "ghi", NULL), "X:/ghi");
  TEST_MANY((pool, "X:", "def", SVN_EMPTY_PATH, NULL), "X:/def");
  TEST_MANY((pool, SVN_EMPTY_PATH, "X:", "ghi", NULL), "X:/ghi");
#endif /* SVN_USE_DOS_PATHS */

  /* ### probably need quite a few more tests... */

  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_join(apr_pool_t *pool)
{
  int i;
  char *result;

  static const char * const joins[][3] = {
    { "abc", "def", "abc/def" },
    { "a", "def", "a/def" },
    { "a", "d", "a/d" },
    { SVN_EMPTY_PATH, "abc", "abc" },
    { "abc", SVN_EMPTY_PATH, "abc" },
    { "", "", "" },
  };

  for (i = 0; i < COUNT_OF(joins); i++)
    {
      const char *base = joins[i][0];
      const char *comp = joins[i][1];
      const char *expect = joins[i][2];

      result = svn_relpath_join(base, comp, pool);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_relpath_join(\"%s\", \"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 base, comp, result, expect);

      /*result = svn_relpath_join_many(pool, base, comp, NULL);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_relpath_join_many(\"%s\", \"%s\") "
                                 "returned \"%s\". expected \"%s\"",
                                 base, comp, result, expect);*/
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_join(apr_pool_t *pool)
{
  int i;
  char *result;

  static const char * const joins[][3] = {
    { "abc", "def", "abc/def" },
    { "a", "def", "a/def" },
    { "a", "d", "a/d" },
    { "/", "d", "/d" },
    { "/abc", "d", "/abc/d" },
    { "/abc", "def", "/abc/def" },
    { "/abc", "/def", "/def" },
    { "/abc", "/d", "/d" },
    { "/abc", "/", "/" },
    { SVN_EMPTY_PATH, "/", "/" },
    { "/", SVN_EMPTY_PATH, "/" },
    { SVN_EMPTY_PATH, "abc", "abc" },
    { "abc", SVN_EMPTY_PATH, "abc" },
    { SVN_EMPTY_PATH, "/abc", "/abc" },
    { SVN_EMPTY_PATH, SVN_EMPTY_PATH, SVN_EMPTY_PATH },
    { "http://server/dir", "file", "http://server/dir/file" },
    { "svn+ssh://user@host", "abc", "svn+ssh://user@host/abc" },
    { "http://server/dir", "/file", "http://server/file" },
    { "http://server/dir", "svn://server2", "svn://server2" },
    { "file:///etc/rc.d", "/shr", "file:///shr" },
  };

  for (i = 0; i < COUNT_OF(joins); i++)
    {
      const char *base = joins[i][0];
      const char *comp = joins[i][1];
      const char *expect = joins[i][2];

      result = svn_uri_join(base, comp, pool);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_uri_join(\"%s\", \"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 base, comp, result, expect);

      /*result = svn_uri_join_many(pool, base, comp, NULL);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_join_many(\"%s\", \"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 base, comp, result, expect);*/
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_basename(apr_pool_t *pool)
{
  int i;
  const char *result;

  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "abc", "abc" },
    { "/abc", "abc" },
    { "/abc", "abc" },
    { "/x/abc", "abc" },
    { "/xx/abc", "abc" },
    { "/xx/abc", "abc" },
    { "/xx/abc", "abc" },
    { "a", "a" },
    { "/a", "a" },
    { "/b/a", "a" },
    { "/b/a", "a" },
    { "/", "" },
    { SVN_EMPTY_PATH, SVN_EMPTY_PATH },
    { "X:/abc", "abc" },
#ifdef SVN_USE_DOS_PATHS
    { "X:", "" },
    { "X:/", "" },
    { "X:abc", "abc" },
    { "//srv/shr", "" },
    { "//srv/shr/fld", "fld" },
    { "//srv/shr/fld/subfld", "subfld" },
    { "//srv/s r/fld", "fld" },
#else /* !SVN_USE_DOS_PATHS */
    { "X:", "X:" },
    { "X:abc", "X:abc" },
#endif /* SVN_USE_DOS_PATHS */
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *path = tests[i].path;
      const char *expect = tests[i].result;

      result = svn_dirent_basename(path, pool);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_basename(\"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 path, result, expect);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_basename(apr_pool_t *pool)
{
  int i;
  const char *result;

  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "", "" },
    { " ", " " },
    { "foo/bar", "bar" },
    { "foo/bar/bad", "bad" },
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *path = tests[i].path;
      const char *expect = tests[i].result;

      result = svn_relpath_basename(path, pool);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_relpath_basename(\"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 path, result, expect);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_basename(apr_pool_t *pool)
{
  int i;
  const char *result;

  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "/", "" },
    { SVN_EMPTY_PATH, SVN_EMPTY_PATH },
    { "http://s/file", "file" },
    { "http://s/dir/file", "file" },
    { "http://s", "" },
    { "file://", "" },
    { "file:///a", "a" },
    { "file:///a/b", "b" },
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *path = tests[i].path;
      const char *expect = tests[i].result;

      result = svn_uri_basename(path, pool);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_uri_basename(\"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 path, result, expect);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_dirname(apr_pool_t *pool)
{
  int i;
  char *result;

  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "abc", "" },
    { "/abc", "/" },
    { "/x/abc", "/x" },
    { "/xx/abc", "/xx" },
    { "a", "" },
    { "/a", "/" },
    { "/b/a", "/b" },
    { "/", "/" },
    { SVN_EMPTY_PATH, SVN_EMPTY_PATH },
    { "X:abc/def", "X:abc" },
#ifdef SVN_USE_DOS_PATHS
    { "X:/", "X:/" },
    { "X:/abc", "X:/" },
    { "X:abc", "X:" },
    { "X:", "X:" },
    { "//srv/shr",      "//srv/shr" },
    { "//srv/shr/fld",  "//srv/shr" },
    { "//srv/shr/fld/subfld", "//srv/shr/fld" },
    { "//srv/s r/fld",  "//srv/s r" },
#else /* !SVN_USE_DOS_PATHS */
    /* on non-Windows platforms, ':' is allowed in pathnames */
    { "X:", "" },
    { "X:abc", "" },
#endif /* SVN_USE_DOS_PATHS */
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *path = tests[i].path;
      const char *expect = tests[i].result;

      result = svn_dirent_dirname(path, pool);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_dirname(\"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 path, result, expect);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_dirname(apr_pool_t *pool)
{
  int i;
  char *result;

  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "", "" },
    { " ", "" },
    { "foo", "" },
    { "foo/bar", "foo" },
    { "foo/bar/bad", "foo/bar" },
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *path = tests[i].path;
      const char *expect = tests[i].result;

      result = svn_relpath_dirname(path, pool);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_relpath_dirname(\"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 path, result, expect);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_dirname(apr_pool_t *pool)
{
  int i;
  char *result;

  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "/", "/" },
    { "/a", "/" },
    { "/a/b", "/a" },
    { SVN_EMPTY_PATH, SVN_EMPTY_PATH },
    { "http://server/dir", "http://server" },
    { "http://server/dir/file", "http://server/dir" },
    { "http://server", "http://server" },
    { "file:///a/b", "file:///a" },
    { "file:///a", "file://" },
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *path = tests[i].path;
      const char *expect = tests[i].result;

      result = svn_uri_dirname(path, pool);
      if (strcmp(result, expect))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_uri_dirname(\"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 path, result, expect);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_canonicalize(apr_pool_t *pool)
{
  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "",                     "" },
    { ".",                    "" },
    { "/",                    "/" },
    { "/.",                   "/" },
    { "./",                   "" },
    { "./.",                  "" },
    { "//",                   "/" },
    { "/////",                "/" },
    { "./././.",              "" },
    { "////././.",            "/" },
    { "foo",                  "foo" },
    { ".foo",                 ".foo" },
    { "foo.",                 "foo." },
    { "/foo",                 "/foo" },
    { "foo/",                 "foo" },
    { "foo./",                "foo." },
    { "foo./.",               "foo." },
    { "foo././/.",            "foo." },
    { "/foo/bar",             "/foo/bar" },
    { "foo/..",               "foo/.." },
    { "foo/../",              "foo/.." },
    { "foo/../.",             "foo/.." },
    { "foo//.//bar",          "foo/bar" },
    { "///foo",               "/foo" },
    { "/.//./.foo",           "/.foo" },
    { ".///.foo",             ".foo" },
    { "../foo",               "../foo" },
    { "../../foo/",           "../../foo" },
    { "../../foo/..",         "../../foo/.." },
    { "/../../",              "/../.." },
    { "X:/foo",               "X:/foo" },
    { "X:",                   "X:" },
    { "X:foo",                "X:foo" },
    { "C:/folder/subfolder/file", "C:/folder/subfolder/file" },
#ifdef SVN_USE_DOS_PATHS
    { "X:/",                  "X:/" },
    { "X:/./",                "X:/" },
    { "x:/",                  "X:/" },
    { "x:",                   "X:" },
    { "x:AAAAA",              "X:AAAAA" },
    /* We permit UNC dirents on Windows.  By definition UNC
     * dirents must have two components so we should remove the
     * double slash if there is only one component. */
    { "//hst/foo",            "//hst/foo" },
    { "//hst",                "/hst" },
    { "//hst/./",             "/hst" },
    { "//server/share/",      "//server/share" },
    { "//server/SHare/",      "//server/SHare" },
    { "//SERVER/SHare/",      "//server/SHare" },
    { "//srv/s r",            "//srv/s r" },
    { "//srv/s r/qq",         "//srv/s r/qq" },
#endif /* SVN_USE_DOS_PATHS */
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *canonical = svn_dirent_canonicalize(tests[i].path, pool);

      if (strcmp(canonical, tests[i].result))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_canonicalize(\"%s\") returned "
                                 "\"%s\" expected \"%s\"",
                                 tests[i].path, canonical, tests[i].result);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_canonicalize(apr_pool_t *pool)
{
  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "",                     "" },
    { ".",                    "" },
    { "/",                    "" },
    { "/.",                   "" },
    { "./",                   "" },
    { "./.",                  "" },
    { "//",                   "" },
    { "/////",                "" },
    { "./././.",              "" },
    { "////././.",            "" },
    { "foo",                  "foo" },
    { ".foo",                 ".foo" },
    { "foo.",                 "foo." },
    { "/foo",                 "foo" },
    { "foo/",                 "foo" },
    { "foo./",                "foo." },
    { "foo./.",               "foo." },
    { "foo././/.",            "foo." },
    { "/foo/bar",             "foo/bar" },
    { "foo/..",               "foo/.." },
    { "foo/../",              "foo/.." },
    { "foo/../.",             "foo/.." },
    { "foo//.//bar",          "foo/bar" },
    { "///foo",               "foo" },
    { "/.//./.foo",           ".foo" },
    { ".///.foo",             ".foo" },
    { "../foo",               "../foo" },
    { "../../foo/",           "../../foo" },
    { "../../foo/..",         "../../foo/.." },
    { "/../../",              "../.." },
    { "X:/foo",               "X:/foo" },
    { "X:",                   "X:" },
    { "X:foo",                "X:foo" },
    { "C:/folder/subfolder/file", "C:/folder/subfolder/file" },
    { "http://hst",           "http:/hst" },
    { "http://hst/foo/../bar","http:/hst/foo/../bar" },
    { "http://hst/",          "http:/hst" },
    { "http:///",             "http:" },
    { "https://",             "https:" },
    { "file:///",             "file:" },
    { "file://",              "file:" },
    { "svn:///",              "svn:" },
    { "svn+ssh:///",          "svn+ssh:" },
    { "http://HST/",          "http:/HST" },
    { "http://HST/FOO/BaR",   "http:/HST/FOO/BaR" },
    { "svn+ssh://j.raNDom@HST/BaR", "svn+ssh:/j.raNDom@HST/BaR" },
    { "svn+SSH://j.random:jRaY@HST/BaR", "svn+SSH:/j.random:jRaY@HST/BaR" },
    { "SVN+ssh://j.raNDom:jray@HST/BaR", "SVN+ssh:/j.raNDom:jray@HST/BaR" },
    { "fILe:///Users/jrandom/wc", "fILe:/Users/jrandom/wc" },
    { "fiLE:///",             "fiLE:" },
    { "fiLE://",              "fiLE:" },
    { "file://SRV/shr/repos",  "file:/SRV/shr/repos" },
    { "file://SRV/SHR/REPOS",  "file:/SRV/SHR/REPOS" },
    { "http://server////",     "http:/server" },
    { "http://server/file//",  "http:/server/file" },
    { "http://server//.//f//", "http:/server/f" },
    { "file:///c:/temp/repos", "file:/c:/temp/repos" },
    { "file:///c:/temp/REPOS", "file:/c:/temp/REPOS" },
    { "file:///C:/temp/REPOS", "file:/C:/temp/REPOS" },
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)

    {
      const char *canonical = svn_relpath_canonicalize(tests[i].path, pool);

      if (strcmp(canonical, tests[i].result))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_relpath_canonicalize(\"%s\") returned "
                                 "\"%s\" expected \"%s\"",
                                 tests[i].path, canonical, tests[i].result);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_canonicalize(apr_pool_t *pool)
{
  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "",                     "" },
    { ".",                    "" },
    { "/",                    "/" },
    { "/.",                   "/" },
    { "./",                   "" },
    { "./.",                  "" },
    { "//",                   "/" },
    { "/////",                "/" },
    { "./././.",              "" },
    { "////././.",            "/" },
    { "foo",                  "foo" },
    { ".foo",                 ".foo" },
    { "foo.",                 "foo." },
    { "/foo",                 "/foo" },
    { "foo/",                 "foo" },
    { "foo./",                "foo." },
    { "foo./.",               "foo." },
    { "foo././/.",            "foo." },
    { "/foo/bar",             "/foo/bar" },
    { "foo/..",               "foo/.." },
    { "foo/../",              "foo/.." },
    { "foo/../.",             "foo/.." },
    { "foo//.//bar",          "foo/bar" },
    { "///foo",               "/foo" },
    { "/.//./.foo",           "/.foo" },
    { ".///.foo",             ".foo" },
    { "../foo",               "../foo" },
    { "../../foo/",           "../../foo" },
    { "../../foo/..",         "../../foo/.." },
    { "/../../",              "/../.." },
    { "X:/foo",               "X:/foo" },
    { "X:",                   "X:" },
    { "X:foo",                "X:foo" },
    { "C:/folder/subfolder/file", "C:/folder/subfolder/file" },
    { "http://hst",           "http://hst" },
    { "http://hst/foo/../bar","http://hst/foo/../bar" },
    { "http://hst/",          "http://hst" },
    { "http:///",             "http://" },
    { "https://",             "https://" },
    { "file:///",             "file://" },
    { "file://",              "file://" },
    { "svn:///",              "svn://" },
    { "svn+ssh:///",          "svn+ssh://" },
    { "http://HST/",          "http://hst" },
    { "http://HST/FOO/BaR",   "http://hst/FOO/BaR" },
    { "svn+ssh://j.raNDom@HST/BaR", "svn+ssh://j.raNDom@hst/BaR" },
    { "svn+SSH://j.random:jRaY@HST/BaR", "svn+ssh://j.random:jRaY@hst/BaR" },
    { "SVN+ssh://j.raNDom:jray@HST/BaR", "svn+ssh://j.raNDom:jray@hst/BaR" },
    { "fILe:///Users/jrandom/wc", "file:///Users/jrandom/wc" },
    { "fiLE:///",             "file://" },
    { "fiLE://",              "file://" },
    { "file://SRV/shr/repos",  "file://srv/shr/repos" },
    { "file://SRV/SHR/REPOS",  "file://srv/SHR/REPOS" },
    { "http://server////",     "http://server" },
    { "http://server/file//",  "http://server/file" },
    { "http://server//.//f//", "http://server/f" },
    { "s://d/%KK",             "s://d/%25KK" }, /* Make bad escapings safe */
    { "s://d/c%3A",            "s://d/c:" },
    { "s://d/c#",              "s://d/c%23" }, /* Escape schema separator */
    { "s://d/c($) .+?",        "s://d/c($)%20.+%3F" }, /* Test special chars */
    { "file:///C%3a/temp",     "file:///C:/temp" },
#ifdef SVN_USE_DOS_PATHS
    { "file:///c:/temp/repos", "file:///C:/temp/repos" },
    { "file:///c:/temp/REPOS", "file:///C:/temp/REPOS" },
    { "file:///C:/temp/REPOS", "file:///C:/temp/REPOS" },
#else /* !SVN_USE_DOS_PATHS */
    { "file:///c:/temp/repos", "file:///c:/temp/repos" },
    { "file:///c:/temp/REPOS", "file:///c:/temp/REPOS" },
    { "file:///C:/temp/REPOS", "file:///C:/temp/REPOS" },
#endif /* SVN_USE_DOS_PATHS */
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)

    {
      const char *canonical = svn_uri_canonicalize(tests[i].path, pool);

      if (strcmp(canonical, tests[i].result))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_uri_canonicalize(\"%s\") returned "
                                 "\"%s\" expected \"%s\"",
                                 tests[i].path, canonical, tests[i].result);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_is_canonical(apr_pool_t *pool)
{
  struct {
    const char *path;
    svn_boolean_t canonical;
  } tests[] = {
    { "",                      TRUE },
    { ".",                     FALSE },
    { "/",                     TRUE },
    { "/.",                    FALSE },
    { "./",                    FALSE },
    { "./.",                   FALSE },
    { "//",                    FALSE },
    { "/////",                 FALSE },
    { "./././.",               FALSE },
    { "////././.",             FALSE },
    { "foo",                   TRUE },
    { ".foo",                  TRUE },
    { "foo.",                  TRUE },
    { "/foo",                  TRUE },
    { "foo/",                  FALSE },
    { "foo./",                 FALSE },
    { "foo./.",                FALSE },
    { "foo././/.",             FALSE },
    { "/foo/bar",              TRUE },
    { "foo/..",                TRUE },
    { "foo/../",               FALSE },
    { "foo/../.",              FALSE },
    { "foo//.//bar",           FALSE },
    { "///foo",                FALSE },
    { "/.//./.foo",            FALSE },
    { ".///.foo",              FALSE },
    { "../foo",                TRUE },
    { "../../foo/",            FALSE },
    { "../../foo/..",          TRUE },
    { "/../../",               FALSE },
    { "dirA",                  TRUE },
    { "foo/dirA",              TRUE },
    { "foo/./bar",             FALSE },
    { "C:/folder/subfolder/file", TRUE },
    { "X:/foo",                TRUE },
    { "X:",                    TRUE },
    { "X:foo",                 TRUE },
    { "X:foo/",                FALSE },
    { "file with spaces",      TRUE },
#ifdef SVN_USE_DOS_PATHS
    { "X:/",                   TRUE },
    { "X:/foo",                TRUE },
    { "X:",                    TRUE },
    { "X:foo",                 TRUE },
    { "x:/",                   FALSE },
    { "x:/foo",                FALSE },
    { "x:",                    FALSE },
    { "x:foo",                 FALSE },
    /* We permit UNC dirents on Windows.  By definition UNC
     * dirents must have two components so we should remove the
     * double slash if there is only one component. */
    { "//hst",                 FALSE },
    { "//hst/./",              FALSE },
    { "//server/share/",       FALSE },
    { "//server/share",        TRUE },
    { "//server/SHare",        TRUE },
    { "//SERVER/SHare",        FALSE },
    { "//srv/SH RE",           TRUE },
#else /* !SVN_USE_DOS_PATHS */
    { "X:/",                   FALSE },
    /* Some people use colons in their filenames. */
    { ":", TRUE },
    { ".:", TRUE },
    { "foo/.:", TRUE },
#endif /* SVN_USE_DOS_PATHS */
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t canonical;
      const char* canonicalized;

      canonical = svn_dirent_is_canonical(tests[i].path, pool);
      if (tests[i].canonical != canonical)
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_is_canonical(\"%s\") returned "
                                 "\"%s\" expected \"%s\"",
                                 tests[i].path,
                                 canonical ? "TRUE" : "FALSE",
                                 tests[i].canonical ? "TRUE" : "FALSE");

      canonicalized = svn_dirent_canonicalize(tests[i].path, pool);

      if (canonical && (strcmp(tests[i].path, canonicalized) != 0))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_canonicalize(\"%s\") returned \"%s\" "
                                 "while svn_dirent_is_canonical returned TRUE",
                                 tests[i].path,
                                 canonicalized);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_is_canonical(apr_pool_t *pool)
{
  struct {
    const char *path;
    svn_boolean_t canonical;
  } tests[] = {
    { "",                      TRUE },
    { ".",                     FALSE },
    { "/",                     FALSE },
    { "/.",                    FALSE },
    { "./",                    FALSE },
    { "./.",                   FALSE },
    { "//",                    FALSE },
    { "/////",                 FALSE },
    { "./././.",               FALSE },
    { "////././.",             FALSE },
    { "foo",                   TRUE },
    { ".foo",                  TRUE },
    { "foo.",                  TRUE },
    { "/foo",                  FALSE },
    { "foo/",                  FALSE },
    { "foo./",                 FALSE },
    { "foo./.",                FALSE },
    { "foo././/.",             FALSE },
    { "/foo/bar",              FALSE },
    { "foo/..",                TRUE },
    { "foo/../",               FALSE },
    { "foo/../.",              FALSE },
    { "foo//.//bar",           FALSE },
    { "///foo",                FALSE },
    { "/.//./.foo",            FALSE },
    { ".///.foo",              FALSE },
    { "../foo",                TRUE },
    { "../../foo/",            FALSE },
    { "../../foo/..",          TRUE },
    { "/../../",               FALSE },
    { "dirA",                  TRUE },
    { "foo/dirA",              TRUE },
    { "foo/./bar",             FALSE },
    { "http://hst",            FALSE },
    { "http://hst/foo/../bar", FALSE },
    { "http://HST/",           FALSE },
    { "http://HST/FOO/BaR",    FALSE },
    { "svn+ssh://jens@10.0.1.1", FALSE },
    { "svn+ssh:/jens@10.0.1.1", TRUE },
    { "fILe:///Users/jrandom/wc", FALSE },
    { "fILe:/Users/jrandom/wc", TRUE },
    { "X:/foo",                TRUE },
    { "X:",                    TRUE },
    { "X:foo",                 TRUE },
    { "X:foo/",                FALSE },
    /* Some people use colons in their filenames. */
    { ":", TRUE },
    { ".:", TRUE },
    { "foo/.:", TRUE },
    { "//server/share",         FALSE }, /* Only valid as dirent */
    { "//server",               FALSE },
    { "//",                     FALSE },
    { "file:///c:/temp/repos", FALSE },
    { "file:///c:/temp/REPOS", FALSE },
    { "file:///C:/temp/REPOS", FALSE },
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t canonical;
      const char* canonicalized;

      canonical = svn_relpath_is_canonical(tests[i].path, pool);
      if (tests[i].canonical != canonical)
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_relpath_is_canonical(\"%s\") returned "
                                 "\"%s\" expected \"%s\"",
                                 tests[i].path,
                                 canonical ? "TRUE" : "FALSE",
                                 tests[i].canonical ? "TRUE" : "FALSE");

      canonicalized = svn_relpath_canonicalize(tests[i].path, pool);

      if (canonical && (strcmp(tests[i].path, canonicalized) != 0))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_relpath_canonicalize(\"%s\") returned "
                                 "\"%s\"  while svn_relpath_is_canonical "
                                 "returned %s",
                                 tests[i].path,
                                 canonicalized,
                                 canonical ? "TRUE" : "FALSE");
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_is_canonical(apr_pool_t *pool)
{
  struct {
    const char *path;
    svn_boolean_t canonical;
  } tests[] = {
    { "",                      TRUE },
    { ".",                     FALSE },
    { "/",                     TRUE },
    { "/.",                    FALSE },
    { "./",                    FALSE },
    { "./.",                   FALSE },
    { "//",                    FALSE },
    { "/////",                 FALSE },
    { "./././.",               FALSE },
    { "////././.",             FALSE },
    { "foo",                   TRUE },
    { ".foo",                  TRUE },
    { "foo.",                  TRUE },
    { "/foo",                  TRUE },
    { "foo/",                  FALSE },
    { "foo./",                 FALSE },
    { "foo./.",                FALSE },
    { "foo././/.",             FALSE },
    { "/foo/bar",              TRUE },
    { "foo/..",                TRUE },
    { "foo/../",               FALSE },
    { "foo/../.",              FALSE },
    { "foo//.//bar",           FALSE },
    { "///foo",                FALSE },
    { "/.//./.foo",            FALSE },
    { ".///.foo",              FALSE },
    { "../foo",                TRUE },
    { "../../foo/",            FALSE },
    { "../../foo/..",          TRUE },
    { "/../../",               FALSE },
    { "dirA",                  TRUE },
    { "foo/dirA",              TRUE },
    { "foo/./bar",             FALSE },
    { "http://hst",            TRUE },
    { "http://hst/foo/../bar", TRUE },
    { "http://hst/foo/bar/",   FALSE },
    { "http://hst/",           FALSE },
    { "http://HST/",           FALSE },
    { "http://HST/FOO/BaR",    FALSE },
    { "http://hst/foo/./bar",  FALSE },
    { "hTTp://hst/foo/bar",   FALSE },
    { "http://hst/foo/bar/",   FALSE },
    { "svn+ssh://jens@10.0.1.1", TRUE },
    { "svn+ssh://j.raNDom@HST/BaR", FALSE },
    { "svn+SSH://j.random:jRaY@HST/BaR", FALSE },
    { "SVN+ssh://j.raNDom:jray@HST/BaR", FALSE },
    { "svn+ssh://j.raNDom:jray@hst/BaR", TRUE },
    { "fILe:///Users/jrandom/wc", FALSE },
    { "fiLE:///",              FALSE },
    { "fiLE://",               FALSE },
    { "C:/folder/subfolder/file", TRUE },
    { "X:/foo",                TRUE },
    { "X:",                    TRUE },
    { "X:foo",                 TRUE },
    { "X:foo/",                FALSE },
    /* Some people use colons in their filenames. */
    { ":", TRUE },
    { ".:", TRUE },
    { "foo/.:", TRUE },
    { "file://SRV/share/repos", FALSE },
    { "file://srv/SHARE/repos", TRUE },
    { "file://srv/share/repos", TRUE },
    { "file://srv/share/repos/", FALSE },
    { "//server/share",         FALSE }, /* Only valid as dirent */
    { "//server",               FALSE },
    { "//",                     FALSE },
    { "file:///folder/c#",      FALSE }, /* # needs escaping */
    { "file:///fld/with space", FALSE }, /* # needs escaping */
    { "file:///fld/c%23",       TRUE }, /* Properly escaped C# */
#ifdef SVN_USE_DOS_PATHS
    { "file:///c:/temp/repos", FALSE },
    { "file:///c:/temp/REPOS", FALSE },
    { "file:///C:/temp/REPOS", TRUE },
#else /* !SVN_USE_DOS_PATHS */
    { "file:///c:/temp/repos", TRUE },
    { "file:///c:/temp/REPOS", TRUE },
    { "file:///C:/temp/REPOS", TRUE },
#endif /* SVN_USE_DOS_PATHS */
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t canonical;
      const char* canonicalized;

      canonical = svn_uri_is_canonical(tests[i].path, pool);
      if (tests[i].canonical != canonical)
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_uri_is_canonical(\"%s\") returned "
                                 "\"%s\" expected \"%s\"",
                                 tests[i].path,
                                 canonical ? "TRUE" : "FALSE",
                                 tests[i].canonical ? "TRUE" : "FALSE");

      canonicalized = svn_uri_canonicalize(tests[i].path, pool);

      if (canonical && (strcmp(tests[i].path, canonicalized) != 0))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_uri_canonicalize(\"%s\") returned \"%s\" "
                                 "while svn_uri_is_canonical returned %s",
                                 tests[i].path,
                                 canonicalized,
                                 canonical ? "TRUE" : "FALSE");
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_split(apr_pool_t *pool)
{
  apr_size_t i;

  static const char * const paths[][3] = {
    { "/foo/bar",        "/foo",          "bar" },
    { "/foo/bar/ ",       "/foo/bar",      " " },
    { "/foo",            "/",             "foo" },
    { "foo",             SVN_EMPTY_PATH,  "foo" },
    { ".bar",            SVN_EMPTY_PATH,  ".bar" },
    { "/.bar",           "/",             ".bar" },
    { "foo/bar",         "foo",           "bar" },
    { "/foo/bar",        "/foo",          "bar" },
    { "foo/bar",         "foo",           "bar" },
    { "foo./.bar",       "foo.",          ".bar" },
    { "../foo",          "..",            "foo" },
    { SVN_EMPTY_PATH,   SVN_EMPTY_PATH,   SVN_EMPTY_PATH },
    { "/flu\\b/\\blarg", "/flu\\b",       "\\blarg" },
    { "/",               "/",             "" },
    { "X:/foo/bar",      "X:/foo",        "bar" },
    { "X:foo/bar",       "X:foo",         "bar" },
#ifdef SVN_USE_DOS_PATHS
    { "X:/",             "X:/",           "" },
    { "X:/foo",          "X:/",           "foo" },
    { "X:foo",           "X:",            "foo" },
    { "//srv/shr",       "//srv/shr",     "" },
    { "//srv/shr/fld",   "//srv/shr",     "fld" },
    { "//srv/s r",       "//srv/s r",     "" },
#else /* !SVN_USE_DOS_PATHS */
    { "X:foo",           SVN_EMPTY_PATH,  "X:foo" },
#endif /* SVN_USE_DOS_PATHS */
  };

  for (i = 0; i < COUNT_OF(paths); i++)
    {
      const char *dir, *base_name;

      svn_dirent_split(&dir, &base_name, paths[i][0], pool);
      if (strcmp(dir, paths[i][1]))
        {
          return svn_error_createf
            (SVN_ERR_TEST_FAILED, NULL,
             "svn_dirent_split (%s) returned dirname '%s' instead of '%s'",
             paths[i][0], dir, paths[i][1]);
        }
      if (strcmp(base_name, paths[i][2]))
        {
          return svn_error_createf
            (SVN_ERR_TEST_FAILED, NULL,
             "svn_dirent_split (%s) returned basename '%s' instead of '%s'",
             paths[i][0], base_name, paths[i][2]);
        }
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_split(apr_pool_t *pool)
{
  apr_size_t i;

  static const char * const paths[][3] = {
    { "", "", "" },
    { "bar", "", "bar" },
    { "foo/bar", "foo", "bar" },
    { "a/b/c", "a/b", "c" },
  };

  for (i = 0; i < COUNT_OF(paths); i++)
    {
      const char *dir, *base_name;

      svn_relpath_split( &dir, &base_name, paths[i][0], pool);
      if (strcmp(dir, paths[i][1]))
        {
          return svn_error_createf
            (SVN_ERR_TEST_FAILED, NULL,
             "svn_relpath_split (%s) returned dirname '%s' instead of '%s'",
             paths[i][0], dir, paths[i][1]);
        }
      if (strcmp(base_name, paths[i][2]))
        {
          return svn_error_createf
            (SVN_ERR_TEST_FAILED, NULL,
             "svn_relpath_split (%s) returned basename '%s' instead of '%s'",
             paths[i][0], base_name, paths[i][2]);
        }
    }
  return SVN_NO_ERROR;
}


static svn_error_t *
test_uri_split(apr_pool_t *pool)
{
  apr_size_t i;

  static const char * const paths[][3] = {
    { "http://server/foo/bar", "http://server/foo", "bar" },
    { "http://server/dir/foo/bar", "http://server/dir/foo", "bar" },
    { "http://server/foo", "http://server", "foo" },
    { "http://server", "http://server", "" },
    { SVN_EMPTY_PATH,   SVN_EMPTY_PATH,   SVN_EMPTY_PATH },
    { "file://", "file://", "" },
    { "file:///a", "file://", "a" }
  };

  for (i = 0; i < COUNT_OF(paths); i++)
    {
      const char *dir, *base_name;

      svn_uri_split(&dir, &base_name, paths[i][0], pool);
      if (strcmp(dir, paths[i][1]))
        {
          return svn_error_createf
            (SVN_ERR_TEST_FAILED, NULL,
             "svn_uri_split (%s) returned dirname '%s' instead of '%s'",
             paths[i][0], dir, paths[i][1]);
        }
      if (strcmp(base_name, paths[i][2]))
        {
          return svn_error_createf
            (SVN_ERR_TEST_FAILED, NULL,
             "svn_uri_split (%s) returned basename '%s' instead of '%s'",
             paths[i][0], base_name, paths[i][2]);
        }
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_is_ancestor(apr_pool_t *pool)
{
  apr_size_t i;

  /* Dirents to test and their expected results. */
  struct {
    const char *path1;
    const char *path2;
    svn_boolean_t result;
  } tests[] = {
    { "/foo",            "/foo/bar",      TRUE},
    { "/foo/bar",        "/foo/bar/",     TRUE},
    { "/",               "/foo",          TRUE},
    { SVN_EMPTY_PATH,    "foo",           TRUE},
    { SVN_EMPTY_PATH,    ".bar",          TRUE},
    { SVN_EMPTY_PATH,    "/",             FALSE},
    { SVN_EMPTY_PATH,    "/foo",          FALSE},
    { "/.bar",           "/",             FALSE},
    { "foo/bar",         "foo",           FALSE},
    { "/foo/bar",        "/foo",          FALSE},
    { "foo",             "foo/bar",       TRUE},
    { "foo.",            "foo./.bar",     TRUE},

    { "../foo",          "..",            FALSE},
    { SVN_EMPTY_PATH,    SVN_EMPTY_PATH,  TRUE},
    { "/",               "/",             TRUE},
    { "X:foo",           "X:bar",         FALSE},
#ifdef SVN_USE_DOS_PATHS
    { "//srv/shr",       "//srv",         FALSE},
    { "//srv/shr",       "//srv/shr/fld", TRUE },
    { "//srv/s r",       "//srv/s r/fld", TRUE },
    { "//srv",           "//srv/shr/fld", TRUE },
    { "//srv/shr/fld",   "//srv/shr",     FALSE },
    { "//srv/shr/fld",   "//srv2/shr/fld", FALSE },
    { "X:/",             "X:/",           TRUE},
    { "X:/foo",          "X:/",           FALSE},
    { "X:/",             "X:/foo",        TRUE},
    { "X:",              "X:foo",         TRUE},
    { SVN_EMPTY_PATH,    "C:/",           FALSE},
#else /* !SVN_USE_DOS_PATHS */
    { "X:",              "X:foo",         FALSE},
    { SVN_EMPTY_PATH,    "C:/",           TRUE},
#endif /* SVN_USE_DOS_PATHS */
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t retval;

      retval = svn_dirent_is_ancestor(tests[i].path1, tests[i].path2);
      if (tests[i].result != retval)
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_dirent_is_ancestor (%s, %s) returned %s instead of %s",
           tests[i].path1, tests[i].path2, retval ? "TRUE" : "FALSE",
           tests[i].result ? "TRUE" : "FALSE");
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_is_ancestor(apr_pool_t *pool)
{
  apr_size_t i;

  /* Dirents to test and their expected results. */
  struct {
    const char *path1;
    const char *path2;
    svn_boolean_t result;
  } tests[] = {
    { "foo",            "foo/bar",        TRUE},
    { "food/bar",       "foo/bar",        FALSE},
    { "/",               "/foo",          TRUE},
    { "",                "foo",           TRUE},
    { "",                ".bar",          TRUE},
    { "foo/bar",         "foo",           FALSE},
    { "foo",             "foo/bar",       TRUE},
    { "foo.",            "foo./.bar",     TRUE},
    { "",                "",              TRUE},
    { "X:foo",           "X:bar",         FALSE},
    { "X:",              "X:foo",         FALSE},
    { "",                "C:",            TRUE},
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t retval;

      retval = svn_relpath_is_ancestor(tests[i].path1, tests[i].path2);
      if (tests[i].result != retval)
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_relpath_is_ancestor (%s, %s) returned %s instead of %s",
           tests[i].path1, tests[i].path2, retval ? "TRUE" : "FALSE",
           tests[i].result ? "TRUE" : "FALSE");
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_is_ancestor(apr_pool_t *pool)
{
  apr_size_t i;

  /* URIs to test and their expected results. */
  struct {
    const char *path1;
    const char *path2;
    svn_boolean_t result;
  } tests[] = {
    { "/foo",            "/foo/bar",      TRUE},
    { "/foo/bar",        "/foo/bar/",     TRUE},
    { "/",               "/foo",          TRUE},
    { SVN_EMPTY_PATH,    "foo",           TRUE},
    { SVN_EMPTY_PATH,    ".bar",          TRUE},
    { SVN_EMPTY_PATH,    "/",             FALSE},
    { SVN_EMPTY_PATH,    "/foo",          FALSE},
    { "/.bar",           "/",             FALSE},
    { "foo/bar",         "foo",           FALSE},
    { "/foo/bar",        "/foo",          FALSE},
    { "foo",             "foo/bar",       TRUE},
    { "foo.",            "foo./.bar",     TRUE},

    { "../foo",          "..",            FALSE},
    { SVN_EMPTY_PATH,    SVN_EMPTY_PATH,  TRUE},
    { "/",               "/",             TRUE},

    { "http://test",    "http://test",     TRUE},
    { "http://test",    "http://taste",    FALSE},
    { "http://test",    "http://test/foo", TRUE},
    { "http://test",    "file://test/foo", FALSE},
    { "http://test",    "http://testF",    FALSE},
    { "http://",        "http://test",     TRUE},
    { SVN_EMPTY_PATH,   "http://test",     FALSE},
    { "X:foo",          "X:bar",           FALSE},
    { "X:",             "X:foo",           FALSE},
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t retval;

      retval = svn_uri_is_ancestor(tests[i].path1, tests[i].path2);
      if (tests[i].result != retval)
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_uri_is_ancestor (%s, %s) returned %s instead of %s",
           tests[i].path1, tests[i].path2, retval ? "TRUE" : "FALSE",
           tests[i].result ? "TRUE" : "FALSE");
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_skip_ancestor(apr_pool_t *pool)
{
  apr_size_t i;

  /* Dirents to test and their expected results. */
  struct {
    const char *path1;
    const char *path2;
    const char *result;
  } tests[] = {
    { "/foo",            "/foo/bar",        "bar"},
    { "/foo/bar",        "/foot/bar",       "/foot/bar"},
    { "/foo",            "/foo",            ""},
    { "/foo",            "/foot",           "/foot"},
    { "/foot",           "/foo",            "/foo"},
    { "",                "foo",             "foo"},
    { "",                "/foo",            "/foo"},
    { "/",               "/foo",            "foo"},
    { "/foo/bar/bla",    "/foo/bar",        "/foo/bar"},
    { "/foo/bar",        "/foo/bar/bla",    "bla"},
    { "foo/bar",         "foo",             "foo"},
    { "/foo/bar",        "foo",             "foo"},
    { "/",               "bar/bla",         "bar/bla"},
#ifdef SVN_USE_DOS_PATHS
    { "A:/foo",          "A:/foo/bar",      "bar"},
    { "A:/foo",          "A:/foot",         "A:/foot"},
    { "A:/",             "A:/foo",          "foo"},
    { "A:",              "A:foo",           "foo"},
    { "A:",              "A:/",             "A:/"},
    { "//srv/share",     "//vrs/share",     "//vrs/share"},
    { "//srv",           "//srv/share",     "//srv/share"},
    { "//srv/share",     "//srv/share/foo", "foo"},
    { "/",               "//srv/share",     "//srv/share"},
#endif
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char* retval;

      retval = svn_dirent_skip_ancestor(tests[i].path1, tests[i].path2);
      if (strcmp(tests[i].result, retval))
        return svn_error_createf(
             SVN_ERR_TEST_FAILED, NULL,
             "test_dirent_skip_ancestor (%s, %s) returned %s instead of %s",
             tests[i].path1, tests[i].path2, retval, tests[i].result);
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_skip_ancestor(apr_pool_t *pool)
{
  apr_size_t i;

  /* Dirents to test and their expected results. */
  struct {
    const char *path1;
    const char *path2;
    const char *result;
  } tests[] = {
    { "foo",             "foo/bar",        "bar"},
    { "foo/bar",         "foot/bar",       "foot/bar"},
    { "foo",             "foo",            ""},
    { "foo",             "foot",           "foot"},
    { "foot",            "foo",            "foo"},
    { "",                "foo",            "foo"},
    { "",                "foo",            "foo"},
    { "",                "foo",            "foo"},
    { "foo/bar/bla",     "foo/bar",        "foo/bar"},
    { "foo/bar",         "foo/bar/bla",    "bla"},
    { "foo/bar",         "foo",            "foo"},
    { "foo/bar",         "foo",            "foo"},
    { "",                "bar/bla",        "bar/bla"},
    { "http:/server",    "http:/server/q", "q" },
    { "svn:/server",     "http:/server/q", "http:/server/q" },
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char* retval;

      retval = svn_relpath_skip_ancestor(tests[i].path1, tests[i].path2);
      if (strcmp(tests[i].result, retval))
        return svn_error_createf(
             SVN_ERR_TEST_FAILED, NULL,
             "svn_relpath_skip_ancestor (%s, %s) returned %s instead of %s",
             tests[i].path1, tests[i].path2, retval, tests[i].result);
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_skip_ancestor(apr_pool_t *pool)
{
  apr_size_t i;

  /* Dirents to test and their expected results. */
  struct {
    const char *path1;
    const char *path2;
    const char *result;
  } tests[] = {
    { "/foo",            "/foo/bar",        "bar"},
    { "/foo/bar",        "/foot/bar",       "/foot/bar"},
    { "/foo",            "/foo",            ""},
    { "/foo",            "/foot",           "/foot"},
    { "/foot",           "/foo",            "/foo"},
    { "",                "foo",             "foo"},
    { "",                "/foo",            "/foo"},
    { "/",               "/foo",            "foo"},
    { "/foo/bar/bla",    "/foo/bar",        "/foo/bar"},
    { "/foo/bar",        "/foo/bar/bla",    "bla"},
    { "foo/bar",         "foo",             "foo"},
    { "/foo/bar",        "foo",             "foo"},
    { "/",               "bar/bla",         "bar/bla"},
    { "http://server",   "http://server/q", "q" },
    { "svn://server",   "http://server/q",  "http://server/q" },
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char* retval;

      retval = svn_uri_skip_ancestor(tests[i].path1, tests[i].path2);
      if (strcmp(tests[i].result, retval))
        return svn_error_createf(
             SVN_ERR_TEST_FAILED, NULL,
             "svn_uri_skip_ancestor (%s, %s) returned %s instead of %s",
             tests[i].path1, tests[i].path2, retval, tests[i].result);
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_get_longest_ancestor(apr_pool_t *pool)
{
  apr_size_t i;

  /* Paths to test and their expected results. */
  struct {
    const char *path1;
    const char *path2;
    const char *result;
  } tests[] = {
    { "/foo",           "/foo/bar",        "/foo"},
    { "/foo/bar",       "foo/bar",         SVN_EMPTY_PATH},
    { "/",              "/foo",            "/"},
    { SVN_EMPTY_PATH,   "foo",             SVN_EMPTY_PATH},
    { SVN_EMPTY_PATH,   ".bar",            SVN_EMPTY_PATH},
    { "/.bar",          "/",               "/"},
    { "foo/bar",        "foo",             "foo"},
    { "/foo/bar",       "/foo",            "/foo"},
    { "/rif",           "/raf",            "/"},
    { "foo",            "bar",             SVN_EMPTY_PATH},
    { "foo",            "foo/bar",         "foo"},
    { "foo.",           "foo./.bar",       "foo."},
    { SVN_EMPTY_PATH,   SVN_EMPTY_PATH,    SVN_EMPTY_PATH},
    { "/",              "/",               "/"},
    { "X:foo",          "Y:foo",           SVN_EMPTY_PATH},
    { "X:/folder1",     "Y:/folder2",      SVN_EMPTY_PATH},
#ifdef SVN_USE_DOS_PATHS
    { "X:/",            "X:/",             "X:/"},
    { "X:/foo/bar/A/D/H/psi", "X:/foo/bar/A/B", "X:/foo/bar/A" },
    { "X:/foo/bar/boo", "X:/foo/bar/baz/boz", "X:/foo/bar"},
    { "X:foo/bar",      "X:foo/bar/boo",   "X:foo/bar"},
    { "//srv/shr",      "//srv/shr/fld",   "//srv/shr" },
    { "//srv/shr/fld",  "//srv/shr",       "//srv/shr" },
    { "//srv/shr/fld",  "//srv2/shr/fld",  SVN_EMPTY_PATH },
    { "X:/foo",         "X:/",             "X:/"},
    { "X:/folder1",     "X:/folder2",      "X:/"},
    { "X:/",            "X:/foo",          "X:/"},
    { "X:",             "X:foo",           "X:"},
    { "X:",             "X:/",             SVN_EMPTY_PATH},
    { "X:foo",          "X:bar",           "X:"},
#else /* !SVN_USE_DOS_PATHS */
    { "X:/foo",         "X:",              "X:"},
    { "X:/folder1",     "X:/folder2",      "X:"},
    { "X:",             "X:foo",           SVN_EMPTY_PATH},
    { "X:foo",          "X:bar",           SVN_EMPTY_PATH},
#endif /* SVN_USE_DOS_PATHS */
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *retval;

      retval = svn_dirent_get_longest_ancestor(tests[i].path1, tests[i].path2,
                                               pool);

      if (strcmp(tests[i].result, retval))
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_dirent_get_longest_ancestor (%s, %s) returned %s instead of %s",
           tests[i].path1, tests[i].path2, retval, tests[i].result);

      /* changing the order of the paths should return the same results */
      retval = svn_dirent_get_longest_ancestor(tests[i].path2, tests[i].path1,
                                               pool);

      if (strcmp(tests[i].result, retval))
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_dirent_get_longest_ancestor (%s, %s) returned %s instead of %s",
           tests[i].path2, tests[i].path1, retval, tests[i].result);
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_get_longest_ancestor(apr_pool_t *pool)
{
  apr_size_t i;

  /* Paths to test and their expected results. */
  struct {
    const char *path1;
    const char *path2;
    const char *result;
  } tests[] = {
    { "foo",            "foo/bar",         "foo"},
    { "foo/bar",        "foo/bar",         "foo/bar"},
    { "",               "foo",             ""},
    { "",               "foo",             ""},
    { "",               ".bar",            ""},
    { ".bar",           "",                ""},
    { "foo/bar",        "foo",             "foo"},
    { "foo/bar",        "foo",             "foo"},
    { "rif",            "raf",             ""},
    { "foo",            "bar",             ""},
    { "foo",            "foo/bar",         "foo"},
    { "foo.",           "foo./.bar",       "foo."},
    { "",               "",                ""},
    { "http:/test",     "http:/test",      "http:/test"},
    { "http:/test",     "http:/taste",     "http:"},
    { "http:/test",     "http:/test/foo",  "http:/test"},
    { "http:/test",     "file:/test/foo",  ""},
    { "http:/test",     "http:/testF",     "http:"},
    { "file:/A/C",      "file:/B/D",       "file:"},
    { "file:/A/C",      "file:/A/D",       "file:/A"},
    { "X:/foo",         "X:",              "X:"},
    { "X:/folder1",     "X:/folder2",      "X:"},
    { "X:",             "X:foo",           ""},
    { "X:foo",          "X:bar",           ""},
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *retval;

      retval = svn_uri_get_longest_ancestor(tests[i].path1, tests[i].path2,
                                             pool);

      if (strcmp(tests[i].result, retval))
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_relpath_get_longest_ancestor (%s, %s) returned "
           "%s instead of %s",
           tests[i].path1, tests[i].path2, retval, tests[i].result);

      /* changing the order of the paths should return the same results */
      retval = svn_relpath_get_longest_ancestor(tests[i].path2, tests[i].path1,
                                                pool);

      if (strcmp(tests[i].result, retval))
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_relpath_get_longest_ancestor (%s, %s) returned "
           "%s instead of %s",
           tests[i].path2, tests[i].path1, retval, tests[i].result);
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_get_longest_ancestor(apr_pool_t *pool)
{
  apr_size_t i;

  /* Paths to test and their expected results. */
  struct {
    const char *path1;
    const char *path2;
    const char *result;
  } tests[] = {
    { "/foo",           "/foo/bar",        "/foo"},
    { "/foo/bar",       "foo/bar",         SVN_EMPTY_PATH},
    { "/",              "/foo",            "/"},
    { SVN_EMPTY_PATH,   "foo",             SVN_EMPTY_PATH},
    { SVN_EMPTY_PATH,   ".bar",            SVN_EMPTY_PATH},
    { "/.bar",          "/",               "/"},
    { "foo/bar",        "foo",             "foo"},
    { "/foo/bar",       "/foo",            "/foo"},
    { "/rif",           "/raf",            "/"},
    { "foo",            "bar",             SVN_EMPTY_PATH},
    { "foo",            "foo/bar",         "foo"},
    { "foo.",           "foo./.bar",       "foo."},
    { SVN_EMPTY_PATH,   SVN_EMPTY_PATH,    SVN_EMPTY_PATH},
    { "/",              "/",               "/"},
    { "http://test",    "http://test",     "http://test"},
    { "http://test",    "http://taste",    SVN_EMPTY_PATH},
    { "http://test",    "http://test/foo", "http://test"},
    { "http://test",    "file://test/foo", SVN_EMPTY_PATH},
    { "http://test",    "http://testF",    SVN_EMPTY_PATH},
    { "http://",        "http://test",     SVN_EMPTY_PATH},
    { "file:///A/C",    "file:///B/D",     SVN_EMPTY_PATH},
    { "file:///A/C",    "file:///A/D",     "file:///A"},
    { "X:/foo",         "X:",              "X:"},
    { "X:/folder1",     "X:/folder2",      "X:"},
    { "X:",             "X:foo",           SVN_EMPTY_PATH},
    { "X:foo",          "X:bar",           SVN_EMPTY_PATH},
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *retval;

      retval = svn_uri_get_longest_ancestor(tests[i].path1, tests[i].path2,
                                             pool);

      if (strcmp(tests[i].result, retval))
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_uri_get_longest_ancestor (%s, %s) returned %s instead of %s",
           tests[i].path1, tests[i].path2, retval, tests[i].result);

      /* changing the order of the paths should return the same results */
      retval = svn_uri_get_longest_ancestor(tests[i].path2, tests[i].path1,
                                             pool);

      if (strcmp(tests[i].result, retval))
        return svn_error_createf
          (SVN_ERR_TEST_FAILED, NULL,
           "svn_uri_get_longest_ancestor (%s, %s) returned %s instead of %s",
           tests[i].path2, tests[i].path1, retval, tests[i].result);
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_is_child(apr_pool_t *pool)
{
  int i, j;

  static const char * const paths[] = {
    "/foo/bar",
    "/foo/bars",
    "/foo/baz",
    "/foo/bar/baz",
    "/flu/blar/blaz",
    "/foo/bar/baz/bing/boom",
    SVN_EMPTY_PATH,
    "foo",
    ".foo",
    "/",
    "foo2",
#ifdef SVN_USE_DOS_PATHS
    "//srv",
    "//srv2",
    "//srv/shr",
    "//srv/shr/fld",
    "H:/foo/bar",
    "H:/foo/baz",
    "H:/foo/bar/baz",
    "H:/flu/blar/blaz",
    "H:/foo/bar/baz/bing/boom",
    "H:/",
    "H:/iota",
    "H:",
    "H:foo",
    "H:foo/baz",
#endif /* SVN_USE_DOS_PATHS */
    };

  /* Maximum number of path[] items for all platforms */
#define MAX_PATHS 32

  static const char * const
    remainders[COUNT_OF(paths)][MAX_PATHS] = {
    { 0, 0, 0, "baz", 0, "baz/bing/boom", 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, "bing/boom", 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, "foo", ".foo", 0, "foo2",
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { "foo/bar", "foo/bars", "foo/baz", "foo/bar/baz", "flu/blar/blaz",
      "foo/bar/baz/bing/boom", 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
#ifdef SVN_USE_DOS_PATHS
    /* //srv paths */
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, "shr", "shr/fld", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, "fld", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    /* H:/ paths */
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, "baz", 0, "baz/bing/boom", 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, "bing/boom", 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, "foo/bar", "foo/baz", "foo/bar/baz", "flu/blar/blaz",
      "foo/bar/baz/bing/boom", 0, "iota", 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    /* H: paths */
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "foo", "foo/baz" },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "baz" },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
#endif /* SVN_USE_DOS_PATHS */
  };

  for (i = 0; i < COUNT_OF(paths); i++)
    {
      for (j = 0; j < COUNT_OF(paths); j++)
        {
          const char *remainder;

          remainder = svn_dirent_is_child(paths[i], paths[j], pool);

          if (((remainder) && (! remainders[i][j]))
              || ((! remainder) && (remainders[i][j]))
              || (remainder && strcmp(remainder, remainders[i][j])))
            return svn_error_createf
              (SVN_ERR_TEST_FAILED, NULL,
               "svn_dirent_is_child (%s, %s) returned '%s' instead of '%s'",
               paths[i], paths[j],
               remainder ? remainder : "(null)",
               remainders[i][j] ? remainders[i][j] : "(null)" );
        }
    }

#undef NUM_TEST_PATHS
  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_is_child(apr_pool_t *pool)
{
  int i, j;

  static const char * const paths[] = {
    "",
    "foo",
    "foo/bar",
    "foo/bars",
    "foo/baz",
    "foo/bar/baz",
    "flu/blar/blaz",
    "foo/bar/baz/bing/boom",
    ".foo",
    ":",
    "foo2",
    "food",
    "bar",
    "baz",
    "ba",
    "bad"
    };

  /* Maximum number of path[] items for all platforms */
#define MAX_PATHS 32

  static const char * const
    remainders[COUNT_OF(paths)][MAX_PATHS] = {
    { 0, "foo", "foo/bar", "foo/bars", "foo/baz", "foo/bar/baz",
      "flu/blar/blaz", "foo/bar/baz/bing/boom", ".foo", ":", "foo2", "food",
      "bar", "baz", "ba", "bad" },
    { 0, 0, "bar", "bars", "baz", "bar/baz", 0, "bar/baz/bing/boom", 0, 0, 0,
      0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, "baz", 0, "baz/bing/boom", 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, "bing/boom", 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
  };

  for (i = 0; i < COUNT_OF(paths); i++)
    {
      for (j = 0; j < COUNT_OF(paths); j++)
        {
          const char *remainder;

          remainder = svn_dirent_is_child(paths[i], paths[j], pool);

          if (strcmp(paths[j], "foodbar") == 0)
            SVN_ERR_MALFUNCTION();

          if (((remainder) && (! remainders[i][j]))
              || ((! remainder) && (remainders[i][j]))
              || (remainder && strcmp(remainder, remainders[i][j])))
            return svn_error_createf
              (SVN_ERR_TEST_FAILED, NULL,
               "svn_relpath_is_child(%s, %s) returned '%s' instead of '%s'",
               paths[i], paths[j],
               remainder ? remainder : "(null)",
               remainders[i][j] ? remainders[i][j] : "(null)" );
        }
    }

#undef NUM_TEST_PATHS
  return SVN_NO_ERROR;
}


static svn_error_t *
test_uri_is_child(apr_pool_t *pool)
{
  int i, j;

#define NUM_TEST_PATHS 20

  static const char * const paths[] = {
    "/foo/bar",
    "/foo/bars",
    "/foo/baz",
    "/foo/bar/baz",
    "/flu/blar/blaz",
    "/foo/bar/baz/bing/boom",
    SVN_EMPTY_PATH,
    "foo",
    ".foo",
    "/",
    "foo2",
    "http://foo/bar",
    "http://foo/baz",
    "H:",
    "http://foo",
    "http://f",
    "H:/foo/bar",
    "H:/foo/baz",
    "H:foo",
    "H:foo/baz",
    };

  static const char * const
    remainders[COUNT_OF(paths)][COUNT_OF(paths)] = {
    { 0, 0, 0, "baz", 0, "baz/bing/boom", 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, "bing/boom", 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, "foo", ".foo", 0, "foo2",
      0, 0, "H:", 0, 0, "H:/foo/bar", "H:/foo/baz", "H:foo", "H:foo/baz" },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { "foo/bar", "foo/bars", "foo/baz", "foo/bar/baz", "flu/blar/blaz",
      "foo/bar/baz/bing/boom", 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, "foo/bar", "foo/baz", 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      "bar", "baz", 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, "baz" },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0 }
  };

  for (i = 0; i < COUNT_OF(paths); i++)
    {
      for (j = 0; j < COUNT_OF(paths); j++)
        {
          const char *remainder;

          remainder = svn_uri_is_child(paths[i], paths[j], pool);

          if (((remainder) && (! remainders[i][j]))
              || ((! remainder) && (remainders[i][j]))
              || (remainder && strcmp(remainder, remainders[i][j])))
            return svn_error_createf
              (SVN_ERR_TEST_FAILED, NULL,
               "svn_uri_is_child (%s, %s) [%d,%d] returned '%s' instead of '%s'",
               paths[i], paths[j], i, j,
               remainder ? remainder : "(null)",
               remainders[i][j] ? remainders[i][j] : "(null)" );
        }
    }

#undef NUM_TEST_PATHS
  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_get_absolute(apr_pool_t *pool)
{
  int i;
  const char *curdir;
  char buf[8192];
#ifdef SVN_USE_DOS_PATHS
  const char *curdironc;
  char curdrive[3] = "C:";
#endif /* SVN_USE_DOS_PATHS */

  struct {
    const char *path;
    const char *result;
  } tests[] = {
    /* '%' will be replaced by the current working dir. */
    { "abc", "%/abc" },
    { SVN_EMPTY_PATH, "%" },
#ifdef SVN_USE_DOS_PATHS
    /* '@' will be replaced by the current working dir on C:\. */
    /* '$' will be replaced by the current drive */
    { "C:/", "C:/" },
    { "C:/abc", "C:/abc" },
    { "C:abc", "@/abc" },
    { "C:", "@" },
    { "/", "$/" },
    { "/x/abc", "$/x/abc" },
    { "c:/", "C:/" },
    { "c:/AbC", "C:/AbC" },
    { "c:abc", "@/abc" },
    /* svn_dirent_get_absolute will check existence of this UNC shares on the
       test machine, so we can't really test this.
    { "//srv/shr",      "//srv/shr" },
    { "//srv/shr/fld",  "//srv/shr" },
    { "//srv/shr/fld/subfld", "//srv/shr/fld" }, */
#else /* !SVN_USE_DOS_PATHS */
    { "/abc", "/abc" },
    { "/x/abc", "/x/abc" },
    { "X:", "%/X:" },
    { "X:abc", "%/X:abc" },
#endif /* SVN_USE_DOS_PATHS */
  };

  if (! getcwd(buf, sizeof(buf)))
    return svn_error_create(SVN_ERR_BASE, NULL, "getcwd() failed");

  curdir = svn_dirent_internal_style(buf, pool);

#ifdef SVN_USE_DOS_PATHS
  if (! getdcwd(3, buf, sizeof(buf))) /* 3 stands for drive C: */
    return svn_error_create(SVN_ERR_BASE, NULL, "getdcwd() failed");

  curdironc = svn_dirent_internal_style(buf, pool);
  curdrive[0] = curdir[0];
#endif /* SVN_USE_DOS_PATHS */

  for (i = 0 ; i < COUNT_OF(tests) ; i++ )
    {
      const char *path = tests[i].path;
      const char *expect = tests[i].result;
      const char *expect_abs, *result;

      expect_abs = expect;
      if (*expect == '%')
        expect_abs = apr_pstrcat(pool, curdir, expect + 1, (char *)NULL);
#ifdef SVN_USE_DOS_PATHS
      if (*expect == '@')
        expect_abs = apr_pstrcat(pool, curdironc, expect + 1, NULL);

      if (*expect == '$')
        expect_abs = apr_pstrcat(pool, curdrive, expect + 1, NULL);

      /* Remove double '/' when CWD was the root dir (E.g. C:/) */
      expect_abs = svn_dirent_canonicalize(expect_abs, pool);
#endif /* SVN_USE_DOS_PATHS */

      SVN_ERR(svn_dirent_get_absolute(&result, path, pool));
      if (strcmp(result, expect_abs))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_get_absolute(\"%s\") returned "
                                 "\"%s\". expected \"%s\"",
                                 path, result, expect_abs);
    }

  return SVN_NO_ERROR;
}

#ifdef WIN32
static svn_error_t *
test_dirent_get_absolute_from_lc_drive(apr_pool_t *pool)
{
  char current_dir[1024];
  char current_dir_on_C[1024];
  char *dir_on_c;
  svn_error_t *err;
  apr_hash_t *dirents;
  apr_hash_index_t *hi;
  const char *some_dir_on_C = NULL;

  if (! getcwd(current_dir, sizeof(current_dir)))
    return svn_error_create(SVN_ERR_BASE, NULL, "getcwd() failed");

   /* 3 stands for drive C: */
  if (! getdcwd(3, current_dir_on_C, sizeof(current_dir_on_C)))
    return svn_error_create(SVN_ERR_BASE, NULL, "getdcwd() failed");

  SVN_ERR(svn_io_get_dirents2(&dirents, "C:\\", pool));

  /* We need a directory on 'C:\' to switch to lower case and back.
     We use the first directory we can find that is not the CWD and
     where we can chdir to */

  for (hi = apr_hash_first(pool, dirents); hi; hi = apr_hash_next(hi))
    {
      const char *dir = svn__apr_hash_index_key(hi);
      svn_io_dirent_t *de = svn__apr_hash_index_val(hi);

      if (de->kind == svn_node_dir &&
          strcmp(dir, current_dir_on_C))
        {
          dir = svn_dirent_join("C:/", dir, pool);
          if (!chdir(dir))
            {
              chdir(current_dir_on_C); /* Switch back to old CWD */
              some_dir_on_C = dir;
              break;
            }
        }
    }

  if (!some_dir_on_C)
    return svn_error_create(SVN_ERR_BASE, NULL, 
                            "No usable test directory found in C:\\");

  /* Use the test path, but now with a lower case driveletter */
  dir_on_c = apr_pstrdup(pool, some_dir_on_C);
  dir_on_c[0] = (char)tolower(dir_on_c[0]);

  chdir(dir_on_c);

  err = test_dirent_get_absolute(pool);

  /* Change back to original directory for next tests */
  chdir("C:\\"); /* Switch to upper case */
  chdir(current_dir_on_C); /* Switch cwd on C: */
  chdir(current_dir); /* Switch back to original cwd */
  return err;
}
#endif

static svn_error_t *
test_dirent_condense_targets(apr_pool_t *pool)
{
  int i;
  struct {
    const char *paths[8];
    const char *common;
    const char *results[8]; /* must be same size as paths */
  } tests[] = {
    { { "/dir", "/dir/file", NULL },         NULL,     { "", "file" } },
    { { "/dir1", "/dir2", NULL },            NULL,     { "dir1", "dir2" } },
    { { "dir1", "dir2", NULL },              NULL,     { "dir1", "dir2" } },
#ifdef SVN_USE_DOS_PATHS
    { {"C:/", "C:/zeta", NULL},              "C:/",    {"", "zeta"} },
    { {"C:/dir", "C:/dir/zeta", NULL},       "C:/dir", {"", "zeta"} },
    { {"C:/dir/omega", "C:/dir/zeta", NULL}, "C:/dir", {"omega", "zeta" } },
    { {"C:/dir", "D:/dir", NULL},            "",       {"C:/dir", "D:/dir"} },
    { {"C:A", "C:dir/b", NULL},              NULL,     {"A", "dir/b"} },
#else
    { { "/dir", "/dir/file", NULL },        "/dir",    { "", "file" } },
    { { "/dir1", "/dir2", NULL },           "/",       { "dir1", "dir2" } },
#endif
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      int j;
      const char* common;
      apr_array_header_t *hdr = apr_array_make(pool, 8, sizeof(const char*));
      apr_array_header_t *condensed;

      for (j = 0; j < COUNT_OF(tests[i].paths); j++)
        {
          if (tests[i].paths[j] != NULL)
            APR_ARRAY_PUSH(hdr, const char*) = tests[i].paths[j];
          else
            break;
        }

      SVN_ERR(svn_dirent_condense_targets(&common, &condensed, hdr,
                                          FALSE, pool, pool));

      if (tests[i].common != NULL && strcmp(common, tests[i].common))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_condense_targets returned common "
                                 "\"%s\". expected \"%s\"",
                                 common, tests[i].common);

      for (j = 0; j < COUNT_OF(tests[i].paths); j++)
        {
          if (tests[i].paths[j] == NULL || tests[i].results[j] == NULL)
            break;

          if (strcmp(APR_ARRAY_IDX(condensed, j, const char*),
                     tests[i].results[j]))
            return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                           "svn_dirent_condense_targets returned first"
                           "\"%s\". expected \"%s\"",
                           APR_ARRAY_IDX(condensed, j, const char*),
                           tests[i].results[j]);
        }
    }


  return SVN_NO_ERROR;
}

static svn_error_t *
test_uri_condense_targets(apr_pool_t *pool)
{
  int i;
  struct {
    const char *paths[8];
    const char *common;
    const char *results[8]; /* must be same size as paths */
  } tests[] = {
    { { "/dir", "/dir/file", NULL },         "/dir",     { "", "file" } },
    { { "dir", "dir/file", NULL },           "dir",      { "", "file" } },
    { { "/dir1", "/dir2", NULL },            "/",        { "dir1", "dir2" } },
    { { "dir1", "dir2", NULL },              "",         { "dir1", "dir2" } },
    { { "/dir", "/dir/file", NULL },         "/dir",     { "", "file" } },
    { { "/dir1", "/dir2", NULL },            "/",        { "dir1", "dir2" } },
    { { "/dir1", "dir2", NULL },             "",         { "/dir1", "dir2" } },
    { { "sc://s/A", "sc://s/B", "sc://s" },  "sc://s",   { "A", "B", "" } },
    { { "sc://S/A", "sc://S/B", "sc://S" },  "sc://s",   { "A", "B", "" } },
    { { "sc://A/A", "sc://B/B", "sc://s" },  "",         { "sc://a/A", "sc://b/B", "sc://s"} },
    { { "sc://A/A", "sc://A/a/B", "sc://a/Q" }, "sc://a",{ "A", "a/B", "Q"} },
  };

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      int j;
      const char* common;
      apr_array_header_t *hdr = apr_array_make(pool, 8, sizeof(const char*));
      apr_array_header_t *condensed;

      for (j = 0; j < COUNT_OF(tests[i].paths); j++)
        {
          if (tests[i].paths[j] != NULL)
            APR_ARRAY_PUSH(hdr, const char*) = tests[i].paths[j];
          else
            break;
        }

      SVN_ERR(svn_uri_condense_targets(&common, &condensed, hdr,
                                       FALSE, pool, pool));

      if (tests[i].common != NULL && strcmp(common, tests[i].common))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_uri_condense_targets returned common "
                                 "\"%s\". expected \"%s\"",
                                 common, tests[i].common);

      for (j = 0; j < COUNT_OF(tests[i].paths); j++)
        {
          if (tests[i].paths[j] == NULL || tests[i].results[j] == NULL)
            break;

          if (strcmp(APR_ARRAY_IDX(condensed, j, const char*),
                     tests[i].results[j]))
            return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                           "svn_uri_condense_targets returned first"
                           "\"%s\". expected \"%s\"",
                           APR_ARRAY_IDX(condensed, j, const char*),
                           tests[i].results[j]);
        }
    }


  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_local_style(apr_pool_t *pool)
{
  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "",                     "." },
    { ".",                    "." },
#ifdef SVN_USE_DOS_PATHS
    { "A:/",                 "A:\\" },
    { "A:/file",             "A:\\file" },
    { "a:/",                 "A:\\" },
    { "a:/file",             "A:\\file" },
    { "dir/file",            "dir\\file" },
    { "/",                   "\\" },
    { "//server/share/dir",  "\\\\server\\share\\dir" },
    { "//server/sh re/dir",  "\\\\server\\sh re\\dir" },
#else
    { "a:/",                 "a:" }, /* Wrong but expected for svn_path_*() */
    { "a:/file",             "a:/file" },
    { "dir/file",            "dir/file" },
    { "/",                   "/" },
    { "//server/share/dir",  "/server/share/dir" },
#endif
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *local = svn_dirent_local_style(tests[i].path, pool);

      if (strcmp(local, tests[i].result))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_local_style(\"%s\") returned "
                                 "\"%s\" expected \"%s\"",
                                 tests[i].path, local, tests[i].result);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_local_style(apr_pool_t *pool)
{
  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "",                     "." },
    { ".",                    "." },
    { "c:hi",                 "c:hi" },
#ifdef SVN_USE_DOS_PATHS
    { "dir/file",             "dir\\file" },
    { "a:/file",              "a:\\file" },
#else
    { "dir/file",             "dir/file" },
    { "a:/file",              "a:/file" },
#endif
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *local = svn_relpath_local_style(tests[i].path, pool);

      if (strcmp(local, tests[i].result))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_relpath_local_style(\"%s\") returned "
                                 "\"%s\" expected \"%s\"",
                                 tests[i].path, local, tests[i].result);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_internal_style(apr_pool_t *pool)
{
  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "",                    "" },
    { ".",                   "" },
    { "/",                   "/" },
    { "file",                "file" },
    { "dir/file",            "dir/file" },
    { "dir/file/./.",        "dir/file" },
#ifdef SVN_USE_DOS_PATHS
    { "A:\\",                "A:/" },
    { "A:\\file",            "A:/file" },
    { "A:file",              "A:file" },
    { "a:\\",                "A:/" },
    { "a:\\file",            "A:/file" },
    { "a:file",              "A:file" },
    { "dir\\file",           "dir/file" },
    { "\\\\srv\\shr\\dir",   "//srv/shr/dir" },
    { "\\\\srv\\shr\\",      "//srv/shr" },
    { "\\\\srv\\s r\\",      "//srv/s r" },
    { "//srv/shr",           "//srv/shr" },
    { "//srv/s r",           "//srv/s r" },
    { "//srv/s r",           "//srv/s r" },
#else
    { "a:/",                 "a:" }, /* Wrong but expected for svn_path_*() */
    { "a:/file",             "a:/file" },
    { "dir/file",            "dir/file" },
    { "/",                   "/" },
    { "//server/share/dir",  "/server/share/dir" },
#endif
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *internal = svn_dirent_internal_style(tests[i].path, pool);

      if (strcmp(internal, tests[i].result))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_internal_style(\"%s\") returned "
                                 "\"%s\" expected \"%s\"",
                                 tests[i].path, internal, tests[i].result);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_relpath_internal_style(apr_pool_t *pool)
{
  struct {
    const char *path;
    const char *result;
  } tests[] = {
    { "",                    "" },
    { ".",                   "" },
    { "/",                   "" },
    { "file",                "file" },
    { "dir/file",            "dir/file" },
    { "a:/",                 "a:" },
    { "a:/file",             "a:/file" },
    { "dir/file",            "dir/file" },
    { "//server/share/dir",  "server/share/dir" },
    { "a/./.",               "a" },
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *internal = svn_relpath_internal_style(tests[i].path, pool);

      if (strcmp(internal, tests[i].result))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_relpath_internal_style(\"%s\") returned "
                                 "\"%s\" expected \"%s\"",
                                 tests[i].path, internal, tests[i].result);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_from_file_url(apr_pool_t *pool)
{
  struct {
    const char *url;
    const char *result;
  } tests[] = {
    { "file://",                   "/" },
    { "file:///dir",               "/dir" },
    { "file:///dir/path",          "/dir/path" },
    { "file://localhost",          "/" },
    { "file://localhost/dir",      "/dir" },
    { "file://localhost/dir/path", "/dir/path" },
#ifdef SVN_USE_DOS_PATHS
    { "file://server/share",       "//server/share" },
    { "file://server/share/dir",   "//server/share/dir" },
    { "file:///A:",                "A:/" },
    { "file:///A:/dir",            "A:/dir" },
    { "file:///A:dir",             "A:dir" },
    { "file:///A%7C",              "A:/" },
    { "file:///A%7C/dir",          "A:/dir" },
    { "file:///A%7Cdir",           "A:dir" },
#endif
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *result;
      
      SVN_ERR(svn_uri_get_dirent_from_file_url(&result, tests[i].url, pool));

      if (strcmp(result, tests[i].result))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_uri_get_dirent_from_file_url(\"%s\") "
                                 "returned \"%s\" expected \"%s\"",
                                 tests[i].url, result, tests[i].result);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_from_file_url_errors(apr_pool_t *pool)
{
  const char *bad_file_urls[] = {
    /* error if scheme is not "file" */
    "http://localhost/dir",
    "file+ssh://localhost/dir",
#ifndef SVN_USE_DOS_PATHS
    "file://localhostwrongname/dir",  /* error if host name not "localhost" */
#endif
  };
  int i;

  for (i = 0; i < COUNT_OF(bad_file_urls); i++)
    {
      const char *result;
      svn_error_t *err;

      err = svn_uri_get_dirent_from_file_url(&result, bad_file_urls[i],
                                             pool);

      if (err == NULL)
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_uri_get_dirent_from_file_url(\"%s\") "
                                 "didn't return an error.",
                                 bad_file_urls[i]);
      svn_error_clear(err);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_file_url_from_dirent(apr_pool_t *pool)
{
  struct {
    const char *dirent;
    const char *result;
  } tests[] = {
#ifdef SVN_USE_DOS_PATHS
    { "C:/file",                   "file:///C:/file" },
    { "C:/",                       "file:///C:/" },
    { "C:/File#$",                 "file:///C:/File%23$" },
    /* We can't check these as svn_dirent_get_absolute() won't work
       on shares that don't exist */
    /*{ "//server/share",            "file://server/share" },
    { "//server/share/file",       "file://server/share/file" },*/
#else
    { "/a/b",                      "file:///a/b" },
    { "/a",                        "file:///a" },
    { "/",                         "file:///" },
    { "/File#$",                   "file:///File%23$" },
#endif
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      const char *result;
      
      SVN_ERR(svn_uri_get_file_url_from_dirent(&result, tests[i].dirent,
                                               pool));

      if (strcmp(result, tests[i].result))
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_uri_get_file_url_from_dirent(\"%s\") "
                                 "returned \"%s\" expected \"%s\"",
                                 tests[i].dirent, result, tests[i].result);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
test_dirent_is_under_root(apr_pool_t *pool)
{
  struct {
    const char *base_path;
    const char *path;
    svn_boolean_t under_root;
    const char *result;
  } tests[] = {
    { "/",        "/base",          FALSE},
    { "/aa",      "/aa/bb",         FALSE},
    { "/base",    "/base2",         FALSE},
    { "/b",       "bb",             TRUE, "/b/bb"},
    { "/b",       "../bb",          FALSE},
    { "/b",       "r/./bb",         TRUE, "/b/r/bb"},
    { "/b",       "r/../bb",        TRUE, "/b/bb"},
    { "/b",       "r/../../bb",     FALSE},
    { "/b",       "./bb",           TRUE, "/b/bb"},
    { "/b",       ".",              TRUE, "/b"},
    { "/b",       "",               TRUE, "/b"},
    { "b",        "b",              TRUE, "b/b"},
#ifdef SVN_USE_DOS_PATHS
    { "C:/file",  "a\\d",           TRUE, "C:/file/a/d"},
    { "C:/file",  "aa\\..\\d",      TRUE, "C:/file/d"},
    { "C:/file",  "aa\\..\\..\\d",  FALSE},
#else
    { "C:/file",  "a\\d",           TRUE, "C:/file/a\\d"},
    { "C:/file",  "aa\\..\\d",      TRUE, "C:/file/aa\\..\\d"},
    { "C:/file",  "aa\\..\\..\\d",  TRUE, "C:/file/aa\\..\\..\\d"},
#endif /* SVN_USE_DOS_PATHS */
  };
  int i;

  for (i = 0; i < COUNT_OF(tests); i++)
    {
      svn_boolean_t under_root;
      const char *result;
      
      SVN_ERR(svn_dirent_is_under_root(&under_root,
                                       &result,
                                       tests[i].base_path,
                                       tests[i].path,
                                       pool));

      if (under_root != tests[i].under_root)
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_is_under_root(..\"%s\", \"%s\"..)"
                                 " returned %s expected %s.",
                                 tests[i].base_path,
                                 tests[i].path,
                                 under_root ? "TRUE" : "FALSE",
                                 tests[i].under_root ? "TRUE" : "FALSE");

      if (under_root
          && strcmp(result, tests[i].result) != 0)
        return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                                 "svn_dirent_is_under_root(..\"%s\", \"%s\"..)"
                                 " found \"%s\" expected \"%s\".",
                                 tests[i].base_path,
                                 tests[i].path,
                                 result,
                                 tests[i].result);
    }

  return SVN_NO_ERROR;
}


/* The test table.  */

struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS2(test_dirent_is_root,
                   "test svn_dirent_is_root"),
    SVN_TEST_PASS2(test_uri_is_root,
                   "test svn_uri_is_root"),
    SVN_TEST_PASS2(test_dirent_is_absolute,
                   "test svn_dirent_is_absolute"),
    SVN_TEST_PASS2(test_uri_is_absolute,
                   "test svn_uri_is_absolute"),
    SVN_TEST_PASS2(test_dirent_join,
                   "test svn_dirent_join(_many)"),
    SVN_TEST_PASS2(test_relpath_join,
                   "test svn_relpath_join"),
    SVN_TEST_PASS2(test_uri_join,
                   "test svn_uri_join"),
    SVN_TEST_PASS2(test_dirent_basename,
                   "test svn_dirent_basename"),
    SVN_TEST_PASS2(test_relpath_basename,
                   "test svn_relpath_basename"),
    SVN_TEST_PASS2(test_uri_basename,
                   "test svn_uri_basename"),
    SVN_TEST_PASS2(test_relpath_dirname,
                   "test svn_relpath_dirname"),
    SVN_TEST_PASS2(test_dirent_dirname,
                   "test svn_dirent_dirname"),
    SVN_TEST_PASS2(test_uri_dirname,
                   "test svn_dirent_dirname"),
    SVN_TEST_PASS2(test_dirent_canonicalize,
                   "test svn_dirent_canonicalize"),
    SVN_TEST_PASS2(test_relpath_canonicalize,
                   "test svn_relpath_canonicalize"),
    SVN_TEST_PASS2(test_uri_canonicalize,
                   "test svn_uri_canonicalize"),
    SVN_TEST_PASS2(test_dirent_is_canonical,
                   "test svn_dirent_is_canonical"),
    SVN_TEST_PASS2(test_relpath_is_canonical,
                   "test svn_relpath_is_canonical"),
    SVN_TEST_PASS2(test_uri_is_canonical,
                   "test svn_uri_is_canonical"),
    SVN_TEST_PASS2(test_dirent_split,
                   "test svn_dirent_split"),
    SVN_TEST_PASS2(test_relpath_split,
                   "test test_relpath_split"),
    SVN_TEST_PASS2(test_uri_split,
                   "test test_uri_split"),
    SVN_TEST_PASS2(test_dirent_get_longest_ancestor,
                   "test svn_dirent_get_longest_ancestor"),
    SVN_TEST_PASS2(test_relpath_get_longest_ancestor,
                   "test svn_relpath_get_longest_ancestor"),
    SVN_TEST_PASS2(test_uri_get_longest_ancestor,
                   "test svn_uri_get_longest_ancestor"),
    SVN_TEST_PASS2(test_dirent_is_child,
                   "test svn_dirent_is_child"),
    SVN_TEST_PASS2(test_relpath_is_child,
                   "test svn_relpath_is_child"),
    SVN_TEST_PASS2(test_uri_is_child,
                   "test svn_uri_is_child"),
    SVN_TEST_PASS2(test_dirent_is_ancestor,
                   "test svn_dirent_is_ancestor"),
    SVN_TEST_PASS2(test_relpath_is_ancestor,
                   "test svn_relpath_is_ancestor"),
    SVN_TEST_PASS2(test_uri_is_ancestor,
                   "test svn_uri_is_ancestor"),
    SVN_TEST_PASS2(test_dirent_skip_ancestor,
                   "test test_dirent_skip_ancestor"),
    SVN_TEST_PASS2(test_relpath_skip_ancestor,
                   "test test_relpath_skip_ancestor"),
    SVN_TEST_PASS2(test_uri_skip_ancestor,
                   "test test_uri_skip_ancestor"),
    SVN_TEST_PASS2(test_dirent_get_absolute,
                   "test svn_dirent_get_absolute"),
#ifdef WIN32
    SVN_TEST_XFAIL2(test_dirent_get_absolute_from_lc_drive,
                   "test svn_dirent_get_absolute with lc drive"),
#endif
    SVN_TEST_PASS2(test_dirent_condense_targets,
                   "test svn_dirent_condense_targets"),
    SVN_TEST_PASS2(test_uri_condense_targets,
                   "test svn_uri_condense_targets"),
    SVN_TEST_PASS2(test_dirent_local_style,
                   "test svn_dirent_local_style"),
    SVN_TEST_PASS2(test_relpath_local_style,
                   "test svn_relpath_local_style"),
    SVN_TEST_PASS2(test_dirent_internal_style,
                   "test svn_dirent_internal_style"),
    SVN_TEST_PASS2(test_relpath_internal_style,
                   "test svn_relpath_internal_style"),
    SVN_TEST_PASS2(test_dirent_from_file_url,
                   "test svn_uri_get_dirent_from_file_url"),
    SVN_TEST_PASS2(test_dirent_from_file_url_errors,
                   "test svn_uri_get_dirent_from_file_url errors"),
    SVN_TEST_PASS2(test_file_url_from_dirent,
                   "test svn_uri_get_file_url_from_dirent"),
    SVN_TEST_PASS2(test_dirent_is_under_root,
                   "test svn_dirent_is_under_root"),
    SVN_TEST_NULL
  };
