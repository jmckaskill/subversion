/*
 * Regression tests for the diff/diff3 library -- parsing unidiffs
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


#include "../svn_test.h"

#include "svn_diff.h"
#include "svn_pools.h"
#include "svn_utf.h"

/* Used to terminate lines in large multi-line string literals. */
#define NL APR_EOL_STR

static const char *unidiff =
  "Index: A/mu (deleted)"                                               NL
  "===================================================================" NL
  "Index: A/C/gamma"                                                    NL
  "===================================================================" NL
  "--- A/C/gamma\t(revision 2)"                                         NL
  "+++ A/C/gamma\t(working copy)"                                       NL
  "@@ -1 +1,2 @@"                                                       NL
  " This is the file 'gamma'."                                          NL
  "+some more bytes to 'gamma'"                                         NL
  "Index: A/D/gamma"                                                    NL
  "===================================================================" NL
  "--- A/D/gamma.orig"                                                  NL
  "+++ A/D/gamma"                                                       NL
  "@@ -1,2 +1 @@"                                                       NL
  " This is the file 'gamma'."                                          NL
  "-some less bytes to 'gamma'"                                         NL
  ""                                                                    NL
  "Property changes on: mu-ng"                                          NL
  "___________________________________________________________________" NL
  "Name: newprop"                                                       NL
  "   + newpropval"                                                     NL
  "Name: svn:mergeinfo"                                                 NL
  ""                                                                    NL;

static const char *git_unidiff =
  "Index: A/mu (deleted)"                                               NL
  "===================================================================" NL
  "git --diff a/A/mu b/A/mu"                                            NL
  "deleted file mode 100644"                                            NL
  "Index: A/C/gamma"                                                    NL
  "===================================================================" NL
  "git --diff a/A/C/gamma b/A/C/gamma"                                  NL
  "--- a/A/C/gamma\t(revision 2)"                                       NL
  "+++ b/A/C/gamma\t(working copy)"                                     NL
  "@@ -1 +1,2 @@"                                                       NL
  " This is the file 'gamma'."                                          NL
  "+some more bytes to 'gamma'"                                         NL
  "Index: iota"                                                         NL
  "===================================================================" NL
  "git --diff a/iota b/iota.copied"                                     NL
  "copy from iota"                                                      NL
  "copy to iota.copied"                                                 NL
  "Index: new"                                                          NL
  "===================================================================" NL
  "git --diff a/new b/new"                                              NL
  "new file mode 100644"                                                NL
  ""                                                                    NL;

static const char *git_tree_and_text_unidiff =
  "Index: iota.copied"                                                  NL
  "===================================================================" NL
  "git --diff a/iota b/iota.copied"                                     NL
  "copy from iota"                                                      NL
  "copy to iota.copied"                                                 NL
  "@@ -1 +1,2 @@"                                                       NL
  " This is the file 'iota'."                                           NL
  "+some more bytes to 'iota'"                                          NL
  "Index: A/mu.moved"                                                   NL
  "===================================================================" NL
  "git --diff a/A/mu b/A/mu.moved"                                      NL
  "move from A/mu"                                                      NL
  "move to A/mu.moved"                                                  NL
  "@@ -1 +1,2 @@"                                                       NL
  " This is the file 'mu'."                                             NL
  "+some more bytes to 'mu'"                                            NL
  ""                                                                    NL;

  /* Only the last git diff header is valid. The other ones either misses a
   * path element or has noise between lines that must be continous. */
static const char *bad_git_diff_header =
  "Index: iota.copied"                                                  NL
  "===================================================================" NL
  "git --diff a/foo1 b/"                                                NL
  "git --diff a/foo2 b"                                                 NL
  "git --diff a/foo3 "                                                  NL
  "git --diff a/foo3 "                                                  NL
  "git --diff foo4 b/foo4"                                              NL
  "git --diff a/foo5 b/foo5"                                            NL
  "random noise"                                                        NL
  "copy from foo5"                                                      NL
  "copy to foo5"                                                        NL
  "git --diff a/foo6 b/foo6"                                            NL
  "copy from foo6"                                                      NL
  "random noise"                                                        NL
  "copy to foo6"                                                        NL
  "git --diff a/foo6 b/foo6"                                            NL
  "copy from foo6"                                                      NL
  "git --diff a/iota b/iota.copied"                                     NL
  "copy from iota"                                                      NL
  "copy to iota.copied"                                                 NL
  "@@ -1 +1,2 @@"                                                       NL
  " This is the file 'iota'."                                           NL
  "+some more bytes to 'iota'"                                          NL
  ""                                                                    NL;

  static const char *property_unidiff =
  "Index: iota"                                                         NL
  "===================================================================" NL
  "--- iota"                                                            NL
  "+++ iota"                                                            NL
  ""                                                                    NL
  "Property changes on: iota"                                           NL
  "___________________________________________________________________" NL
  "Deleted: prop_del"                                                   NL
  "## -1 +0,0 ##"                                                       NL
  "-value"                                                              NL
  ""                                                                    NL
  "Property changes on: iota"                                           NL
  "___________________________________________________________________" NL
  "Added: prop_add"                                                     NL
  "## -0,0 +1 ##"                                                       NL
  "+value"                                                              NL
  ""                                                                    NL
  "Property changes on: iota"                                           NL
  "___________________________________________________________________" NL
  "Modified: prop_mod"                                                  NL
  "## -1,4 +1,4 ##"                                                     NL
  "-value"                                                              NL
  "+new value"                                                          NL
  " context"                                                            NL
  " context"                                                            NL
  " context"                                                            NL
  "## -10,4 +10,4 ##"                                                   NL
  " context"                                                            NL
  " context"                                                            NL
  " context"                                                            NL
  "-value"                                                              NL
  "+new value"                                                          NL
  ""                                                                    NL;

  /* ### Add edge cases like context lines stripped from leading whitespaces
   * ### that starts with 'Added: ', 'Deleted: ' or 'Modified: '. */
  static const char *property_and_text_unidiff =
  "Index: iota"                                                         NL
  "===================================================================" NL
  "--- iota"                                                            NL
  "+++ iota"                                                            NL
  "@@ -1 +1,2 @@"                                                       NL
  " This is the file 'iota'."                                           NL
  "+some more bytes to 'iota'"                                          NL
  ""                                                                    NL
  "Property changes on: iota"                                           NL
  "___________________________________________________________________" NL
  "Added: prop_add"                                                     NL
  "## -0,0 +1 ##"                                                       NL
  "+value"                                                              NL;

  /* A unidiff containing diff symbols in the body of the hunks. */
  static const char *diff_symbols_in_prop_unidiff =
  "Index: iota"                                                         NL
  "===================================================================" NL
  "--- iota"                                                            NL
  "+++ iota"                                                            NL
  ""                                                                    NL
  "Property changes on: iota"                                           NL
  "___________________________________________________________________" NL
  "Added: prop_add"                                                     NL
  "## -0,0 +1,3 ##"                                                     NL
  "+Added: bogus_prop"                                                  NL
  "+## -0,0 +20 ##"                                                     NL
  "+@@ -1,2 +0,0 @@"                                                    NL
  "Deleted: prop_del"                                                   NL
  "## -1,2 +0,0 ##"                                                      NL
  "---- iota"                                                           NL
  "-+++ iota"                                                           NL
  "Modified: non-existent"                                              NL
  "blah, just noise - no valid hunk header"                             NL
  "Modified: prop_mod"                                                  NL
  "## -1,4 +1,4 ##"                                                     NL
  "-## -1,2 +1,2 ##"                                                    NL
  "+## -1,3 +1,3 ##"                                                    NL
  " ## -1,5 -0,0 ##"                                                    NL
  " @@ -1,5 -0,0 @@"                                                    NL
  " Modified: prop_mod"                                                 NL
  "## -10,4 +10,4 ##"                                                   NL
  " context"                                                            NL
  " context"                                                            NL
  " context"                                                            NL
  "-## -0,0 +1 ##"                                                      NL
  "+## -1,2 +1,4 ##"                                                    NL
  ""                                                                    NL;


/* Create a PATCH_FILE with name FNAME containing the contents of DIFF. */
static svn_error_t *
create_patch_file(apr_file_t **patch_file, const char *fname,
                  const char *diff, apr_pool_t *pool)
{
  apr_off_t pos = 0;
  apr_size_t len;
  apr_status_t status;

  /* Create a patch file. */
  status = apr_file_open(patch_file, fname,
                        (APR_READ | APR_WRITE | APR_CREATE | APR_TRUNCATE |
                         APR_DELONCLOSE), APR_OS_DEFAULT, pool);
  if (status != APR_SUCCESS)
    return svn_error_createf(SVN_ERR_TEST_FAILED, NULL, "Cannot open '%s'",
                             fname);
  len = strlen(diff);
  status = apr_file_write_full(*patch_file, diff, len, &len);
  if (status || len != strlen(diff))
    return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                             "Cannot write to '%s'", fname);
  SVN_ERR(svn_io_file_seek(*patch_file, APR_SET, &pos, pool));

  return SVN_NO_ERROR;
}

/* Check that reading a line from HUNK equals what's inside EXPECTED.
 * If ORIGINAL is TRUE, read the original hunk text; else, read the
 * modified hunk text. */
static svn_error_t *
check_content(svn_hunk_t *hunk, svn_boolean_t original,
              const char *expected, apr_pool_t *pool)
{
  svn_stream_t *exp;
  svn_stringbuf_t *exp_buf;
  svn_stringbuf_t *hunk_buf;
  svn_boolean_t exp_eof;
  svn_boolean_t hunk_eof;

  exp = svn_stream_from_string(svn_string_create(expected, pool), 
                               pool);

  while (TRUE)
  {
    SVN_ERR(svn_stream_readline(exp, &exp_buf, NL, &exp_eof, pool));
    if (original)
      SVN_ERR(svn_diff_hunk_readline_original_text(hunk, &hunk_buf, NULL,
                                                   &hunk_eof, pool, pool));
    else
      SVN_ERR(svn_diff_hunk_readline_modified_text(hunk, &hunk_buf, NULL,
                                                   &hunk_eof, pool, pool));
       
    SVN_TEST_ASSERT(exp_eof == hunk_eof);
    if (exp_eof)
      break;
    if (strcmp(exp_buf->data, hunk_buf->data))
      return svn_error_createf(SVN_ERR_TEST_FAILED, NULL,
                               "Expected '%s' but was '%s'", exp_buf->data,
                               hunk_buf->data);
  }

  SVN_TEST_ASSERT(hunk_buf->len == 0);

  return SVN_NO_ERROR;
}

static svn_error_t *
test_parse_unidiff(apr_pool_t *pool)
{
  apr_file_t *patch_file;
  const char *fname = "test_parse_unidiff.patch";
  svn_boolean_t reverse;
  svn_boolean_t ignore_whitespace;
  int i;
  apr_pool_t *iterpool;

  SVN_ERR(create_patch_file(&patch_file, fname, unidiff, pool));

  reverse = FALSE;
  ignore_whitespace = FALSE;
  iterpool = svn_pool_create(pool);
  for (i = 0; i < 2; i++)
    {
      svn_patch_t *patch;
      svn_hunk_t *hunk;
      apr_off_t pos;

      svn_pool_clear(iterpool);

      /* Reset file pointer. */
      pos = 0;
      SVN_ERR(svn_io_file_seek(patch_file, APR_SET, &pos, iterpool));

      /* We have two patches with one hunk each.
       * Parse the first patch. */
      SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, reverse,
                                        ignore_whitespace, iterpool, 
                                        iterpool));
      SVN_TEST_ASSERT(patch);
      SVN_TEST_ASSERT(! strcmp(patch->old_filename, "A/C/gamma"));
      SVN_TEST_ASSERT(! strcmp(patch->new_filename, "A/C/gamma"));
      SVN_TEST_ASSERT(patch->hunks->nelts == 1);

      hunk = APR_ARRAY_IDX(patch->hunks, 0, svn_hunk_t *);
      SVN_ERR(check_content(hunk, ! reverse,
                            "This is the file 'gamma'." NL,
                            pool));

      SVN_ERR(check_content(hunk, reverse,
                            "This is the file 'gamma'." NL
                            "some more bytes to 'gamma'" NL,
                            pool));

      /* Parse the second patch. */
      SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, reverse, 
                                        ignore_whitespace, pool, pool));
      SVN_TEST_ASSERT(patch);
      if (reverse)
        {
          SVN_TEST_ASSERT(! strcmp(patch->new_filename, "A/D/gamma.orig"));
          SVN_TEST_ASSERT(! strcmp(patch->old_filename, "A/D/gamma"));
        }
      else
        {
          SVN_TEST_ASSERT(! strcmp(patch->old_filename, "A/D/gamma.orig"));
          SVN_TEST_ASSERT(! strcmp(patch->new_filename, "A/D/gamma"));
        }
      SVN_TEST_ASSERT(patch->hunks->nelts == 1);

      hunk = APR_ARRAY_IDX(patch->hunks, 0, svn_hunk_t *);
      SVN_ERR(check_content(hunk, ! reverse,
                            "This is the file 'gamma'." NL
                            "some less bytes to 'gamma'" NL,
                            pool));

      SVN_ERR(check_content(hunk, reverse,
                            "This is the file 'gamma'." NL,
                            pool));

      reverse = !reverse;
    }
  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

static svn_error_t *
test_parse_git_diff(apr_pool_t *pool)
{
  /* ### Should we check for reversed diffs? */

  apr_file_t *patch_file;
  svn_patch_t *patch;
  svn_hunk_t *hunk;
  const char *fname = "test_parse_git_diff.patch";

  SVN_ERR(create_patch_file(&patch_file, fname, git_unidiff, pool));

  /* Parse a deleted empty file */
  SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, 
                                    FALSE, /* reverse */
                                    FALSE, /* ignore_whitespace */ 
                                    pool, pool));
  SVN_TEST_ASSERT(patch);
  SVN_TEST_ASSERT(! strcmp(patch->old_filename, "A/mu"));
  SVN_TEST_ASSERT(! strcmp(patch->new_filename, "A/mu"));
  SVN_TEST_ASSERT(patch->operation == svn_diff_op_deleted);
  SVN_TEST_ASSERT(patch->hunks->nelts == 0);

  /* Parse a modified file. */
  SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, 
                                    FALSE, /* reverse */
                                    FALSE, /* ignore_whitespace */ 
                                    pool, pool));
  SVN_TEST_ASSERT(patch);
  SVN_TEST_ASSERT(! strcmp(patch->old_filename, "A/C/gamma"));
  SVN_TEST_ASSERT(! strcmp(patch->new_filename, "A/C/gamma"));
  SVN_TEST_ASSERT(patch->operation == svn_diff_op_modified);
  SVN_TEST_ASSERT(patch->hunks->nelts == 1);
  
  hunk = APR_ARRAY_IDX(patch->hunks, 0, svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "This is the file 'gamma'." NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "This is the file 'gamma'." NL
                        "some more bytes to 'gamma'" NL,
                        pool));

  /* Parse a copied empty file */
  SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, 
                                    FALSE, /* reverse */
                                    FALSE, /* ignore_whitespace */ 
                                    pool, pool));

  SVN_TEST_ASSERT(patch);
  SVN_TEST_ASSERT(! strcmp(patch->old_filename, "iota"));
  SVN_TEST_ASSERT(! strcmp(patch->new_filename, "iota.copied"));
  SVN_TEST_ASSERT(patch->operation == svn_diff_op_copied);
  SVN_TEST_ASSERT(patch->hunks->nelts == 0);

  /* Parse an added empty file */
  SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, 
                                    FALSE, /* reverse */
                                    FALSE, /* ignore_whitespace */ 
                                    pool, pool));

  SVN_TEST_ASSERT(patch);
  SVN_TEST_ASSERT(! strcmp(patch->old_filename, "new"));
  SVN_TEST_ASSERT(! strcmp(patch->new_filename, "new"));
  SVN_TEST_ASSERT(patch->operation == svn_diff_op_added);
  SVN_TEST_ASSERT(patch->hunks->nelts == 0);

  return SVN_NO_ERROR;
}

static svn_error_t *
test_parse_git_tree_and_text_diff(apr_pool_t *pool)
{
  /* ### Should we check for reversed diffs? */

  apr_file_t *patch_file;
  svn_patch_t *patch;
  svn_hunk_t *hunk;
  const char *fname = "test_parse_git_tree_and_text_diff.patch";

  SVN_ERR(create_patch_file(&patch_file, fname, git_tree_and_text_unidiff,
                            pool));

  /* Parse a copied file with text modifications. */
  SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, 
                                    FALSE, /* reverse */
                                    FALSE, /* ignore_whitespace */ 
                                    pool, pool));
  SVN_TEST_ASSERT(patch);
  SVN_TEST_ASSERT(! strcmp(patch->old_filename, "iota"));
  SVN_TEST_ASSERT(! strcmp(patch->new_filename, "iota.copied"));
  SVN_TEST_ASSERT(patch->operation == svn_diff_op_copied);
  SVN_TEST_ASSERT(patch->hunks->nelts == 1);
  
  hunk = APR_ARRAY_IDX(patch->hunks, 0, svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "This is the file 'iota'." NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "This is the file 'iota'." NL
                        "some more bytes to 'iota'" NL,
                        pool));

  /* Parse a moved file with text modifications. */
  SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, 
                                    FALSE, /* reverse */
                                    FALSE, /* ignore_whitespace */ 
                                    pool, pool));
  SVN_TEST_ASSERT(patch);
  SVN_TEST_ASSERT(! strcmp(patch->old_filename, "A/mu"));
  SVN_TEST_ASSERT(! strcmp(patch->new_filename, "A/mu.moved"));
  SVN_TEST_ASSERT(patch->operation == svn_diff_op_moved);
  SVN_TEST_ASSERT(patch->hunks->nelts == 1);
  
  hunk = APR_ARRAY_IDX(patch->hunks, 0, svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "This is the file 'mu'." NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "This is the file 'mu'." NL
                        "some more bytes to 'mu'" NL,
                        pool));

  return SVN_NO_ERROR;
}

/* Tests to parse non-valid git diffs. */
static svn_error_t *
test_bad_git_diff_headers(apr_pool_t *pool)
{
  apr_file_t *patch_file;
  svn_patch_t *patch;
  svn_hunk_t *hunk;
  const char *fname = "test_bad_git_diff_header.patch";

  SVN_ERR(create_patch_file(&patch_file, fname, bad_git_diff_header,
                            pool));

  SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, 
                                    FALSE, /* reverse */
                                    FALSE, /* ignore_whitespace */ 
                                    pool, pool));
  SVN_TEST_ASSERT(patch);
  SVN_TEST_ASSERT(! strcmp(patch->old_filename, "iota"));
  SVN_TEST_ASSERT(! strcmp(patch->new_filename, "iota.copied"));
  SVN_TEST_ASSERT(patch->operation == svn_diff_op_copied);
  SVN_TEST_ASSERT(patch->hunks->nelts == 1);
  
  hunk = APR_ARRAY_IDX(patch->hunks, 0, svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "This is the file 'iota'." NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "This is the file 'iota'." NL
                        "some more bytes to 'iota'" NL,
                        pool));

  return SVN_NO_ERROR;
}

/* Tests to parse a diff with three property changes, one is added, one is
 * modified and one is deleted. */
static svn_error_t *
test_parse_property_diff(apr_pool_t *pool)
{
  apr_file_t *patch_file;
  svn_patch_t *patch;
  svn_prop_patch_t *prop_patch;
  svn_hunk_t *hunk;
  apr_array_header_t *hunks;
  const char *fname = "test_parse_property_diff.patch";

  SVN_ERR(create_patch_file(&patch_file, fname, property_unidiff, pool));

  SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, 
                                    FALSE, /* reverse */
                                    FALSE, /* ignore_whitespace */ 
                                    pool, pool));
  SVN_TEST_ASSERT(patch);
  SVN_TEST_ASSERT(! strcmp(patch->old_filename, "iota"));
  SVN_TEST_ASSERT(! strcmp(patch->new_filename, "iota"));
  SVN_TEST_ASSERT(patch->hunks->nelts == 0);
  SVN_TEST_ASSERT(apr_hash_count(patch->prop_patches) == 3);

  /* Check the deleted property */
  prop_patch = apr_hash_get(patch->prop_patches, "prop_del",
                            APR_HASH_KEY_STRING);

  SVN_TEST_ASSERT(prop_patch->operation == svn_diff_op_deleted);
  hunks = prop_patch->hunks;

  SVN_TEST_ASSERT(hunks->nelts == 1);
  hunk = APR_ARRAY_IDX(hunks, 0 , svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "value" NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "",
                        pool));

  /* Check the added property */
  prop_patch = apr_hash_get(patch->prop_patches, "prop_add",
                            APR_HASH_KEY_STRING);

  SVN_TEST_ASSERT(!strcmp("prop_add", prop_patch->name));
  SVN_TEST_ASSERT(prop_patch->operation == svn_diff_op_added);
  hunks = prop_patch->hunks;

  SVN_TEST_ASSERT(hunks->nelts == 1);
  hunk = APR_ARRAY_IDX(hunks, 0 , svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "",
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "value" NL,
                        pool));

  /* Check the modified property */
  prop_patch = apr_hash_get(patch->prop_patches, "prop_mod",
                            APR_HASH_KEY_STRING);

  SVN_TEST_ASSERT(prop_patch->operation == svn_diff_op_modified);
  hunks = prop_patch->hunks;

  SVN_TEST_ASSERT(hunks->nelts == 2);
  hunk = APR_ARRAY_IDX(hunks, 0 , svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "value" NL
                        "context" NL
                        "context" NL
                        "context" NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "new value" NL
                        "context" NL
                        "context" NL
                        "context" NL,
                        pool));

  hunk = APR_ARRAY_IDX(hunks, 1 , svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "context" NL
                        "context" NL
                        "context" NL
                        "value" NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "context" NL
                        "context" NL
                        "context" NL
                        "new value" NL,
                        pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
test_parse_property_and_text_diff(apr_pool_t *pool)
{
  apr_file_t *patch_file;
  svn_patch_t *patch;
  svn_prop_patch_t *prop_patch;
  svn_hunk_t *hunk;
  apr_array_header_t *hunks;
  const char *fname = "test_parse_property_and_text_diff.patch";

  SVN_ERR(create_patch_file(&patch_file, fname, property_and_text_unidiff,
                            pool));

  SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, 
                                    FALSE, /* reverse */
                                    FALSE, /* ignore_whitespace */ 
                                    pool, pool));
  SVN_TEST_ASSERT(patch);
  SVN_TEST_ASSERT(! strcmp(patch->old_filename, "iota"));
  SVN_TEST_ASSERT(! strcmp(patch->new_filename, "iota"));
  SVN_TEST_ASSERT(patch->hunks->nelts == 1);
  SVN_TEST_ASSERT(apr_hash_count(patch->prop_patches) == 1);

  /* Check contents of text hunk */
  hunk = APR_ARRAY_IDX(patch->hunks, 0, svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "This is the file 'iota'." NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "This is the file 'iota'." NL
                        "some more bytes to 'iota'" NL,
                        pool));

  /* Check the added property */
  prop_patch = apr_hash_get(patch->prop_patches, "prop_add",
                            APR_HASH_KEY_STRING);
  SVN_TEST_ASSERT(prop_patch->operation == svn_diff_op_added);

  hunks = prop_patch->hunks;
  SVN_TEST_ASSERT(hunks->nelts == 1);
  hunk = APR_ARRAY_IDX(hunks, 0 , svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "",
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "value" NL,
                        pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
test_parse_diff_symbols_in_prop_unidiff(apr_pool_t *pool)
{
  svn_patch_t *patch;
  apr_file_t *patch_file;
  svn_prop_patch_t *prop_patch;
  svn_hunk_t *hunk;
  apr_array_header_t *hunks;
  const char *fname = "test_parse_diff_symbols_in_prop_unidiff.patch";

  SVN_ERR(create_patch_file(&patch_file, fname, diff_symbols_in_prop_unidiff,
                            pool));

  SVN_ERR(svn_diff_parse_next_patch(&patch, patch_file, 
                                    FALSE, /* reverse */
                                    FALSE, /* ignore_whitespace */ 
                                    pool, pool));
  SVN_TEST_ASSERT(patch);
  SVN_TEST_ASSERT(! strcmp(patch->old_filename, "iota"));
  SVN_TEST_ASSERT(! strcmp(patch->new_filename, "iota"));
  SVN_TEST_ASSERT(patch->hunks->nelts == 0);
  SVN_TEST_ASSERT(apr_hash_count(patch->prop_patches) == 3);

  /* Check the added property */
  prop_patch = apr_hash_get(patch->prop_patches, "prop_add",
                            APR_HASH_KEY_STRING);
  SVN_TEST_ASSERT(prop_patch->operation == svn_diff_op_added);

  hunks = prop_patch->hunks;
  SVN_TEST_ASSERT(hunks->nelts == 1);
  hunk = APR_ARRAY_IDX(hunks, 0 , svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "",
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "Added: bogus_prop" NL
                        "## -0,0 +20 ##" NL
                        "@@ -1,2 +0,0 @@" NL,
                        pool));

  /* Check the deleted property */
  prop_patch = apr_hash_get(patch->prop_patches, "prop_del",
                            APR_HASH_KEY_STRING);
  SVN_TEST_ASSERT(prop_patch->operation == svn_diff_op_deleted);

  hunks = prop_patch->hunks;
  SVN_TEST_ASSERT(hunks->nelts == 1);
  hunk = APR_ARRAY_IDX(hunks, 0 , svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "--- iota" NL
                        "+++ iota" NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "",
                        pool));

  /* Check the modified property */
  prop_patch = apr_hash_get(patch->prop_patches, "prop_mod",
                            APR_HASH_KEY_STRING);
  SVN_TEST_ASSERT(prop_patch->operation == svn_diff_op_modified);
  hunks = prop_patch->hunks;
  SVN_TEST_ASSERT(hunks->nelts == 2);
  hunk = APR_ARRAY_IDX(hunks, 0 , svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "## -1,2 +1,2 ##" NL
                        "## -1,5 -0,0 ##" NL
                        "@@ -1,5 -0,0 @@" NL
                        "Modified: prop_mod" NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "## -1,3 +1,3 ##" NL
                        "## -1,5 -0,0 ##" NL
                        "@@ -1,5 -0,0 @@" NL
                        "Modified: prop_mod" NL,
                        pool));

  hunk = APR_ARRAY_IDX(hunks, 1 , svn_hunk_t *);

  SVN_ERR(check_content(hunk, TRUE,
                        "context" NL
                        "context" NL
                        "context" NL
                        "## -0,0 +1 ##" NL,
                        pool));

  SVN_ERR(check_content(hunk, FALSE,
                        "context" NL
                        "context" NL
                        "context" NL
                        "## -1,2 +1,4 ##" NL,
                        pool));

  return SVN_NO_ERROR;
}

/* ========================================================================== */

struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS2(test_parse_unidiff,
                   "test unidiff parsing"),
    SVN_TEST_PASS2(test_parse_git_diff,
                    "test git unidiff parsing"),
    SVN_TEST_PASS2(test_parse_git_tree_and_text_diff,
                    "test git unidiff parsing of tree and text changes"),
    SVN_TEST_XFAIL2(test_bad_git_diff_headers,
                    "test badly formatted git diff headers"),
    SVN_TEST_PASS2(test_parse_property_diff,
                   "test property unidiff parsing"),
    SVN_TEST_PASS2(test_parse_property_and_text_diff,
                   "test property and text unidiff parsing"),
    SVN_TEST_PASS2(test_parse_diff_symbols_in_prop_unidiff,
                   "test property diffs with odd symbols"),
    SVN_TEST_NULL
  };
