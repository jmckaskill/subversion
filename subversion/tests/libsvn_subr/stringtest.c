/*
 * stringtest.c:  a collection of libsvn_string tests
 *
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

/* ====================================================================
   To add tests, look toward the bottom of this file.

*/



#include <stdio.h>
#include <string.h>
#include "apr_pools.h"
#include "svn_error.h"
#include "svn_string.h"   /* This includes <apr_*.h> */


/* Required global, initialized by included main() */
apr_pool_t *pool;

/* A quick way to create error messages.  */
static svn_error_t *
fail (const char *fmt, ...)
{
  va_list ap;
  char *msg;

  va_start (ap, fmt);
  msg = apr_pvsprintf (pool, fmt, ap);
  va_end (ap);

  return svn_error_create (SVN_ERR_TEST_FAILED, 0, 0, pool, msg);
}


/* Some of our own global variables, for simplicity.  Yes,
   simplicity. */
svn_string_t *a = NULL, *b = NULL, *c = NULL;
const char *phrase_1 = "hello, ";
const char *phrase_2 = "a longish phrase of sorts, longer than 16 anyway";




static svn_error_t *
test1 (const char **msg)
{
  *msg = "make svn_string_t from cstring";
  a = svn_string_create (phrase_1, pool);
  
  /* Test that length, data, and null-termination are correct. */
  if ((a->len == strlen (phrase_1)) && ((strcmp (a->data, phrase_1)) == 0))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test2 (const char **msg)
{
  *msg = "make svn_string_t from substring of cstring";
  b = svn_string_ncreate (phrase_2, 16, pool);
  
  /* Test that length, data, and null-termination are correct. */
  if ((b->len == 16) && ((strncmp (b->data, phrase_2, 16)) == 0))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test3 (const char **msg)
{
  char *tmp;
  size_t old_len;
  
  *msg = "append svn_string_t to svn_string_t";

  a = svn_string_create (phrase_1, pool);
  b = svn_string_ncreate (phrase_2, 16, pool);

  tmp = apr_palloc (pool, (a->len + b->len + 1));
  strcpy (tmp, a->data);
  strcat (tmp, b->data);
  old_len = a->len;
  svn_string_appendstr (a, b);
  
  /* Test that length, data, and null-termination are correct. */
  if ((a->len == (old_len + b->len)) && ((strcmp (a->data, tmp)) == 0))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test4 (const char **msg)
{
  a = svn_string_create (phrase_1, pool);
  svn_string_appendcstr (a, "new bytes to append");
  
  *msg = "append C string to svn_string_t";

  /* Test that length, data, and null-termination are correct. */
  if (svn_string_compare 
      (a, svn_string_create ("hello, new bytes to append", pool)))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test5 (const char **msg)
{
  a = svn_string_create (phrase_1, pool);
  svn_string_appendbytes (a, "new bytes to append", 9);
  
  *msg = "append bytes, then compare two strings";

  /* Test that length, data, and null-termination are correct. */
  if (svn_string_compare 
      (a, svn_string_create ("hello, new bytes", pool)))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test6 (const char **msg)
{
  a = svn_string_create (phrase_1, pool);
  b = svn_string_create (phrase_2, pool);
  c = svn_string_dup (a, pool);

  *msg = "dup two strings, then compare";

  /* Test that length, data, and null-termination are correct. */
  if ((svn_string_compare (a, c)) && (! svn_string_compare (b, c)))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test7 (const char **msg)
{
  char *tmp;
  size_t tmp_len;

  *msg = "chopping a string";

  c = svn_string_create (phrase_2, pool);

  tmp_len = c->len;
  tmp = apr_palloc (pool, c->len + 1);
  strcpy (tmp, c->data);

  svn_string_chop (c, 11);
  
  if ((c->len == (tmp_len - 11))
      && (strncmp (tmp, c->data, c->len) == 0)
      && (c->data[c->len] == '\0'))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test8 (const char **msg)
{
  c = svn_string_create (phrase_2, pool);  
  
  *msg = "emptying a string";

  svn_string_setempty (c);
  
  if ((c->len == 0) && (c->data[0] == '\0'))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test9 (const char **msg)
{
  a = svn_string_create (phrase_1, pool);

  *msg = "fill string with hashmarks";

  svn_string_fillchar (a, '#');

  if ((strcmp (a->data, "#######") == 0)
      && ((strncmp (a->data, "############", a->len - 1)) == 0)
      && (a->data[(a->len - 1)] == '#')
      && (a->data[(a->len)] == '\0'))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test10 (const char **msg)
{
  svn_string_t *s;
  
  apr_size_t num_chopped_1 = 0;
  apr_size_t num_chopped_2 = 0;
  apr_size_t num_chopped_3 = 0;
  
  int chopped_okay_1 = 0;
  int chopped_okay_2 = 0;
  int chopped_okay_3 = 0;
  
  *msg = "chop_back_to_char";

  s = svn_string_create ("chop from slash/you'll never see this", pool);

  num_chopped_1 = svn_string_chop_back_to_char (s, '/');
  chopped_okay_1 = (! strcmp (s->data, "chop from slash"));
  
  num_chopped_2 = svn_string_chop_back_to_char (s, 'X');
  chopped_okay_2 = (! strcmp (s->data, "chop from slash"));
  
  num_chopped_3 = svn_string_chop_back_to_char (s, 'c');
  chopped_okay_3 = (strlen (s->data) == 0);

  if (chopped_okay_1 
      && chopped_okay_2
      && chopped_okay_3
      && (num_chopped_1 == strlen ("/you'll never see this"))
      && (num_chopped_2 == 0)
      && (num_chopped_3 == strlen ("chop from slash")))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test11 (const char **msg)
{
  svn_string_t *s, *t;
  size_t len_1 = 0;
  size_t len_2 = 0;
  size_t block_len_1 = 0;
  size_t block_len_2 = 0;
  
  *msg = "block initialization and growth";

  s = svn_string_create ("a small string", pool);
  len_1       = (s->len);
  block_len_1 = (s->blocksize);
  
  t = svn_string_create (", plus a string more than twice as long", pool);
  svn_string_appendstr (s, t);
  len_2       = (s->len);
  block_len_2 = (s->blocksize);
  
  /* Test that:
   *   - The initial block was just the right fit.
   *   - The block more than doubled (because second string so long).
   *   - The block grew by a power of 2.
   */
  if ((len_1 == (block_len_1 - 1))
      && ((block_len_2 / block_len_1) > 2)
        && (((block_len_2 / block_len_1) % 2) == 0))
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


static svn_error_t *
test12 (const char **msg)
{
  svn_string_t *s;
  
  *msg = "formatting strings from varargs";

  s = svn_string_createf (pool, 
                          "This %s is used in test %d.",
                          "string",
                          12);
  
  if (strcmp (s->data, "This string is used in test 12.") == 0)
    return SVN_NO_ERROR;
  else
    return fail ("test failed");
}


/*
   ====================================================================
   If you add a new test to this file, update this array.

   (These globals are required by our included main())
*/

/* An array of all test functions */
svn_error_t *(*test_funcs[])(const char **msg) =
{
  NULL,
  test1,
  test2,
  test3,
  test4,
  test5,
  test6,
  test7,
  test8,
  test9,
  test10,
  test11,
  test12,
  NULL
};



/* --------------------------------------------------------------
 * local variables:
 * eval: (load-file "../../svn-dev.el")
 * end:
 */

