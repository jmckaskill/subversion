/* strings-reps-test.c --- test `strings' and `representations' interfaces
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

#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "svn_error.h"
#include "apr.h"
#include "../fs-helpers.h"
#include "../../libsvn_fs/skel.h"
#include "../../libsvn_fs/strings-table.h"
#include "../../libsvn_fs/reps-table.h"



/*-----------------------------------------------------------------*/
/* Helper functions and batons for reps-table testing. */
struct rep_args
{
  const char *key;
  svn_fs_t *fs;
  skel_t *skel;
};


static svn_error_t *
txn_body_write_new_rep (void *baton, trail_t *trail)
{
  struct rep_args *b = (struct rep_args *) baton;
  return svn_fs__write_new_rep (&(b->key), b->fs, b->skel, trail);
}


static svn_error_t *
txn_body_write_rep (void *baton, trail_t *trail)
{
  struct rep_args *b = (struct rep_args *) baton;
  return svn_fs__write_rep (b->fs, b->key, b->skel, trail);
}


static svn_error_t *
txn_body_read_rep (void *baton, trail_t *trail)
{
  struct rep_args *b = (struct rep_args *) baton;
  return svn_fs__read_rep (&(b->skel), b->fs, b->key, trail);
}


static svn_error_t *
txn_body_delete_rep (void *baton, trail_t *trail)
{
  struct rep_args *b = (struct rep_args *) baton;
  return svn_fs__delete_rep (b->fs, b->key, trail);
}



/* Representation Table Test functions. */

static svn_error_t *
write_new_rep (const char **msg, 
               svn_boolean_t msg_only,
               apr_pool_t *pool)
{
  struct rep_args args;
  const char *rep = "(fulltext a83t2Z0q)";
  svn_fs_t *fs;

  *msg = "Write a new rep, get a new key back.";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Create a new fs and repos */
  SVN_ERR (svn_test__create_fs_and_repos
           (&fs, "test-repo-write-new-rep", pool));

  /* Set up transaction baton */
  args.fs = fs;
  args.skel = svn_fs__parse_skel ((char *)rep, strlen (rep), pool);
  args.key = NULL;

  /* Write new rep to reps table. */
  SVN_ERR (svn_fs__retry_txn (args.fs, txn_body_write_new_rep, &args, pool));

  /* Close the filesystem. */
  SVN_ERR (svn_fs_close_fs (fs));

  if (args.key == NULL)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "error writing new representation");

  return SVN_NO_ERROR;
}


static svn_error_t *
write_rep (const char **msg, 
           svn_boolean_t msg_only,
           apr_pool_t *pool)
{
  struct rep_args new_args;
  struct rep_args args;
  const char *new_rep = "(fulltext a83t2Z0q)";
  const char *rep = "(fulltext kfogel31337)";
  svn_fs_t *fs;

  *msg = "Write a new rep, then overwrite it.";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Create a new fs and repos */
  SVN_ERR (svn_test__create_fs_and_repos
           (&fs, "test-repo-write-rep", pool));

  /* Set up transaction baton */
  new_args.fs = fs;
  new_args.skel = svn_fs__parse_skel ((char *)new_rep, strlen (new_rep), pool);
  new_args.key = NULL;

  /* Write new rep to reps table. */
  SVN_ERR (svn_fs__retry_txn (new_args.fs, 
                              txn_body_write_new_rep, &new_args, pool));

  /* Make sure we got a valid key. */
  if (new_args.key == NULL)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "error writing new representation");

  /* Set up transaction baton for re-writing reps. */
  args.fs = new_args.fs;
  args.skel = svn_fs__parse_skel ((char *)rep, strlen (rep), pool);
  args.key = new_args.key;

  /* Overwrite first rep in reps table. */
  SVN_ERR (svn_fs__retry_txn (new_args.fs, 
                              txn_body_write_rep, &args, pool));

  /* Close the filesystem. */
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


static svn_error_t *
read_rep (const char **msg, 
          svn_boolean_t msg_only,
          apr_pool_t *pool)
{
  struct rep_args new_args;
  struct rep_args args;
  struct rep_args read_args;
  const char *new_rep = "(fulltext a83t2Z0q)";
  const char *rep = "(fulltext kfogel31337)";
  svn_stringbuf_t *skel_data;
  svn_fs_t *fs;

  *msg = "Write and overwrite a new rep; confirm with reads.";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Create a new fs and repos */
  SVN_ERR (svn_test__create_fs_and_repos
           (&fs, "test-repo-read-rep", pool));

  /* Set up transaction baton */
  new_args.fs = fs;
  new_args.skel = svn_fs__parse_skel ((char *)new_rep, strlen (new_rep), pool);
  new_args.key = NULL;

  /* Write new rep to reps table. */
  SVN_ERR (svn_fs__retry_txn (new_args.fs, 
                              txn_body_write_new_rep, &new_args, pool));

  /* Make sure we got a valid key. */
  if (new_args.key == NULL)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "error writing new representation");

  /* Read the new rep back from the reps table. */
  read_args.fs = new_args.fs;
  read_args.skel = NULL;
  read_args.key = new_args.key;
  SVN_ERR (svn_fs__retry_txn (new_args.fs, 
                              txn_body_read_rep, &read_args, pool));

  /* Make sure the skel matches. */
  if (! read_args.skel)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "error reading new representation");
  
  skel_data = svn_fs__unparse_skel (read_args.skel, pool);
  if (strcmp (skel_data->data, new_rep))
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "representation corrupted");
  
  /* Set up transaction baton for re-writing reps. */
  args.fs = new_args.fs;
  args.skel = svn_fs__parse_skel ((char *)rep, strlen (rep), pool);
  args.key = new_args.key;

  /* Overwrite first rep in reps table. */
  SVN_ERR (svn_fs__retry_txn (new_args.fs, 
                              txn_body_write_rep, &args, pool));

  /* Read the new rep back from the reps table (using the same FS and
     key as the first read...let's make sure this thing didn't get
     written to the wrong place). */
  read_args.skel = NULL;
  SVN_ERR (svn_fs__retry_txn (new_args.fs, 
                              txn_body_read_rep, &read_args, pool));

  /* Make sure the skel matches. */
  if (! read_args.skel)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "error reading new representation");
  
  skel_data = svn_fs__unparse_skel (read_args.skel, pool);
  if (strcmp (skel_data->data, rep))
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "representation corrupted");
  
  /* Close the filesystem. */
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


static svn_error_t *
delete_rep (const char **msg, 
            svn_boolean_t msg_only,
            apr_pool_t *pool)
{
  struct rep_args new_args;
  struct rep_args delete_args;
  struct rep_args read_args;
  const char *new_rep = "(fulltext a83t2Z0q)";
  svn_fs_t *fs;
  svn_error_t *err;

  *msg = "Write, then delete, a new rep; confirm deletion.";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Create a new fs and repos */
  SVN_ERR (svn_test__create_fs_and_repos
           (&fs, "test-repo-delete-rep", pool));

  /* Set up transaction baton */
  new_args.fs = fs;
  new_args.skel = svn_fs__parse_skel ((char *)new_rep, strlen (new_rep), pool);
  new_args.key = NULL;

  /* Write new rep to reps table. */
  SVN_ERR (svn_fs__retry_txn (new_args.fs, 
                              txn_body_write_new_rep, &new_args, pool));

  /* Make sure we got a valid key. */
  if (new_args.key == NULL)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "error writing new representation");

  /* Delete the rep we just wrote. */
  delete_args.fs = new_args.fs;
  delete_args.key = new_args.key;
  SVN_ERR (svn_fs__retry_txn (new_args.fs, 
                              txn_body_delete_rep, &delete_args, pool));

  /* Try to read the new rep back from the reps table. */
  read_args.fs = new_args.fs;
  read_args.skel = NULL;
  read_args.key = new_args.key;
  err = svn_fs__retry_txn (new_args.fs, 
                           txn_body_read_rep, &read_args, pool);

  /* We better have an error... */
  if ((! err) && (read_args.skel))
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "error deleting representation");
  
  /* Close the filesystem. */
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


/* ------------------------------------------------------------------- */
/* Helper functions and batons for strings-table testing. */

static svn_error_t *
verify_expected_record (svn_fs_t *fs, 
                        const char *key, 
                        const char *expected_text,
                        apr_size_t expected_len,
                        trail_t *trail)
{
  apr_size_t size;
  char buf[100];
  svn_stringbuf_t *text;
  apr_off_t offset = 0;

  /* Check the string size. */
  SVN_ERR (svn_fs__string_size (&size, fs, key, trail));
  if (size != expected_len)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, trail->pool,
                             "record has unexpected size");

  /* Read the string back in 100-byte chunks. */
  text = svn_stringbuf_create ("", trail->pool);
  while (1)
    {
      size = 100;
      SVN_ERR (svn_fs__string_read (fs, key, buf, offset, &size, trail));
      svn_stringbuf_appendbytes (text, buf, size);
      if (size < 100)
        break;
      offset += size;
    }

  /* Check the size and contents of the read data. */
  if (text->len != expected_len)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, trail->pool,
                             "record read returned unexpected size");
  if (memcmp (expected_text, text->data, expected_len))
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, trail->pool,
                             "record read returned unexpected data");

  return SVN_NO_ERROR;
}


struct string_args
{
  svn_fs_t *fs;
  const char *key;
  const char *text;
  apr_size_t len;
};


static svn_error_t *
txn_body_verify_string (void *baton, trail_t *trail)
{
  struct string_args *b = (struct string_args *) baton;
  return verify_expected_record (b->fs, b->key, b->text, b->len, trail);
}


static svn_error_t *
txn_body_string_append (void *baton, trail_t *trail)
{
  struct string_args *b = (struct string_args *) baton;
  return svn_fs__string_append (b->fs, &(b->key), b->len, 
                                b->text, trail);
}


static svn_error_t *
txn_body_string_clear (void *baton, trail_t *trail)
{
  struct string_args *b = (struct string_args *) baton;
  return svn_fs__string_clear (b->fs, b->key, trail);
}


static svn_error_t *
txn_body_string_delete (void *baton, trail_t *trail)
{
  struct string_args *b = (struct string_args *) baton;
  return svn_fs__string_delete (b->fs, b->key, trail);
}


static svn_error_t *
txn_body_string_size (void *baton, trail_t *trail)
{
  struct string_args *b = (struct string_args *) baton;
  return svn_fs__string_size (&(b->len), b->fs, b->key, trail);
}


static svn_error_t *
txn_body_string_append_fail (void *baton, trail_t *trail)
{
  struct string_args *b = (struct string_args *) baton;
  SVN_ERR (svn_fs__string_append (b->fs, &(b->key), b->len, 
                                  b->text, trail));
  return svn_error_create (SVN_ERR_TEST_FAILED, 0, NULL, trail->pool,
                           "la dee dah, la dee day...");
}

static svn_error_t *
txn_body_string_copy (void *baton, trail_t *trail)
{
  struct string_args *b = (struct string_args *) baton;
  return svn_fs__string_copy (b->fs, &(b->key), b->key, trail);
}


static const char *bigstring1 =
"    Alice opened the door and found that it led into a small\n"
"passage, not much larger than a rat-hole:  she knelt down and\n"
"looked along the passage into the loveliest garden you ever saw.\n"
"How she longed to get out of that dark hall, and wander about\n"
"among those beds of bright flowers and those cool fountains, but\n"
"she could not even get her head though the doorway; `and even if\n"
"my head would go through,' thought poor Alice, `it would be of\n"
"very little use without my shoulders.  Oh, how I wish\n"
"I could shut up like a telescope!  I think I could, if I only\n"
"know how to begin.'  For, you see, so many out-of-the-way things\n"
"had happened lately, that Alice had begun to think that very few\n"
"things indeed were really impossible.";

static const char *bigstring2 =
"    There seemed to be no use in waiting by the little door, so she\n"
"went back to the table, half hoping she might find another key on\n"
"it, or at any rate a book of rules for shutting people up like\n"
"telescopes:  this time she found a little bottle on it, (`which\n"
"certainly was not here before,' said Alice,) and round the neck\n"
"of the bottle was a paper label, with the words `DRINK ME'\n"
"beautifully printed on it in large letters.";

static const char *bigstring3 =
"    It was all very well to say `Drink me,' but the wise little\n"
"Alice was not going to do THAT in a hurry.  `No, I'll look\n"
"first,' she said, `and see whether it's marked \"poison\" or not';\n"
"for she had read several nice little histories about children who\n"
"had got burnt, and eaten up by wild beasts and other unpleasant\n"
"things, all because they WOULD not remember the simple rules\n"
"their friends had taught them:  such as, that a red-hot poker\n"
"will burn you if you hold it too long; and that if you cut your\n"
"finger VERY deeply with a knife, it usually bleeds; and she had\n"
"never forgotten that, if you drink much from a bottle marked\n"
"`poison,' it is almost certain to disagree with you, sooner or\n"
"later.";


static svn_error_t *
test_strings (const char **msg, 
              svn_boolean_t msg_only,
              apr_pool_t *pool)
{
  struct string_args args;
  svn_fs_t *fs;
  svn_stringbuf_t *string;

  *msg = "Test many strings table functions together.";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Create a new fs and repos */
  SVN_ERR (svn_test__create_fs_and_repos
           (&fs, "test-repo-test-strings", pool));

  /* The plan (after each step below, verify the size and contents of
     the string):

     1.  Write a new string (string1).
     2.  Append string2 to string.
     3.  Clear string.
     4.  Append string3 to string.
     5.  Delete string (verify by size requested failure).
     6.  Write a new string (string1), appending string2, string3, and 
         string4.
  */

  /* 1. Write a new string (string1). */
  args.fs = fs;
  args.key = NULL;
  args.text = bigstring1;
  args.len = strlen (bigstring1);
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_string_append, &args, pool));

  /* Make sure a key was returned. */
  if (! args.key)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "write of new string failed to return new key");

  /* Verify record's size and contents. */
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_verify_string, &args, pool));

  /* Append a second string to our first one. */
  args.text = bigstring2;
  args.len = strlen (bigstring2);
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_string_append, &args, pool));
  
  /* Verify record's size and contents. */
  string = svn_stringbuf_create (bigstring1, pool);
  svn_stringbuf_appendcstr (string, bigstring2);
  args.text = string->data;
  args.len = string->len;
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_verify_string, &args, pool));

  /* Clear the record */
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_string_clear, &args, pool));

  /* Verify record's size and contents. */
  args.text = "";
  args.len = 0;
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_verify_string, &args, pool));

  /* Append a third string to our first one. */
  args.text = bigstring3;
  args.len = strlen (bigstring3);
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_string_append, &args, pool));

  /* Verify record's size and contents. */
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_verify_string, &args, pool));

  /* Delete our record...she's served us well. */
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_string_delete, &args, pool));

  /* Now, we expect a size request on this record to fail with
     SVN_ERR_FS_NO_SUCH_STRING. */
  {
    svn_error_t *err = svn_fs__retry_txn (args.fs,
                                          txn_body_string_size, &args, pool);

    if (! err)
      return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                               "query unexpectedly successful");
    if (err->apr_err != SVN_ERR_FS_NO_SUCH_STRING)
      return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                               "query failed with unexpected error");
  }

  /* Close the filesystem. */
  SVN_ERR (svn_fs_close_fs (fs));


  return SVN_NO_ERROR;
}


static svn_error_t *
write_null_string (const char **msg, 
                   svn_boolean_t msg_only,
                   apr_pool_t *pool)
{
  struct string_args args;
  svn_fs_t *fs;

  *msg = "Write a null string.";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Create a new fs and repos */
  SVN_ERR (svn_test__create_fs_and_repos
           (&fs, "test-repo-test-strings", pool));

  args.fs = fs;
  args.key = NULL;
  args.text = NULL;
  args.len = 0;
  SVN_ERR (svn_fs__retry_txn (args.fs, txn_body_string_append, &args, pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
abort_string (const char **msg, 
              svn_boolean_t msg_only,
              apr_pool_t *pool)
{
  struct string_args args, args2;
  svn_fs_t *fs;

  *msg = "Write a string, then abort during an overwrite.";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Create a new fs and repos */
  SVN_ERR (svn_test__create_fs_and_repos
           (&fs, "test-repo-abort-string", pool));

  /* The plan:

     1.  Write a new string (string1).
     2.  Overwrite string1 with string2, but then ABORT the transaction.
     3.  Read string to make sure it is still string1.
  */

  /* 1. Write a new string (string1). */
  args.fs = fs;
  args.key = NULL;
  args.text = bigstring1;
  args.len = strlen (bigstring1);
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_string_append, &args, pool));

  /* Make sure a key was returned. */
  if (! args.key)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "write of new string failed to return new key");

  /* Verify record's size and contents. */
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_verify_string, &args, pool));

  /* Append a second string to our first one. */
  args2.fs = fs;
  args2.key = args.key;
  args2.text = bigstring2;
  args2.len = strlen (bigstring2);
  {
    svn_error_t *err;

    /* This function is *supposed* to fail with SVN_ERR_TEST_FAILED */
    err = svn_fs__retry_txn (args.fs, txn_body_string_append_fail, 
                             &args2, pool);
    if ((! err) || (err->apr_err != SVN_ERR_TEST_FAILED))
      return svn_error_create (SVN_ERR_TEST_FAILED, 0, NULL, pool,
                               "failed to intentionally abort a trail.");
  }
  
  /* Verify that record's size and contents are still that of string1 */
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_verify_string, &args, pool));

  /* Close the filesystem. */
  SVN_ERR (svn_fs_close_fs (fs));


  return SVN_NO_ERROR;
}

static svn_error_t *
copy_string (const char **msg, 
             svn_boolean_t msg_only,
             apr_pool_t *pool)
{
  struct string_args args;
  svn_fs_t *fs;
  const char *old_key;

  *msg = "Create and copy a string";

  if (msg_only)
    return SVN_NO_ERROR;

  /* Create a new fs and repos */
  SVN_ERR (svn_test__create_fs_and_repos
           (&fs, "test-repo-copy-string", pool));

  /*  Write a new string (string1). */
  args.fs = fs;
  args.key = NULL;
  args.text = bigstring1;
  args.len = strlen (bigstring1);
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_string_append, &args, pool));

  /* Make sure a key was returned. */
  if (! (old_key = args.key))
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "write of new string failed to return new key");

  /* Now copy that string into a new location. */
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_string_copy, &args, pool));

  /* Make sure a different key was returned. */
  if ((! args.key) || (! strcmp (old_key, args.key)))
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "copy of string failed to return new key");

  /* Verify record's size and contents. */
  SVN_ERR (svn_fs__retry_txn (args.fs, 
                              txn_body_verify_string, &args, pool));

  /* Close the filesystem. */
  SVN_ERR (svn_fs_close_fs (fs));


  return SVN_NO_ERROR;
}



/* The test table.  */

svn_error_t * (*test_funcs[]) (const char **msg,
                               svn_boolean_t msg_only,
                               apr_pool_t *pool) = {
  0,
  write_new_rep,
  write_rep,
  read_rep,
  delete_rep,
  test_strings,
  write_null_string,
  abort_string,
  copy_string,
  0
};



/* 
 * local variables:
 * eval: (load-file "../../svn-dev.el")
 * end:
 */
