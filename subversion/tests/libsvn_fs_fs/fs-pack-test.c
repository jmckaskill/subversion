/* fs-pack-test.c --- tests for the filesystem
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

#include <stdlib.h>
#include <string.h>
#include <apr_pools.h>

#include "../svn_test.h"
#include "../../libsvn_fs_fs/fs.h"

#include "svn_pools.h"
#include "svn_props.h"
#include "svn_fs.h"

#include "../svn_test_fs.h"


/*-----------------------------------------------------------------*/

/** The actual fs-tests called by `make check` **/

/* Write the format number and maximum number of files per directory
   to a new format file in PATH, overwriting a previously existing file.

   Use POOL for temporary allocation.

   This implementation is largely stolen from libsvn_fs_fs/fs_fs.c. */
static svn_error_t *
write_format(const char *path,
             int format,
             int max_files_per_dir,
             apr_pool_t *pool)
{
  const char *contents;

  path = svn_path_join(path, "format", pool);

  if (format >= SVN_FS_FS__MIN_LAYOUT_FORMAT_OPTION_FORMAT)
    {
      if (max_files_per_dir)
        contents = apr_psprintf(pool,
                                "%d\n"
                                "layout sharded %d\n",
                                format, max_files_per_dir);
      else
        contents = apr_psprintf(pool,
                                "%d\n"
                                "layout linear",
                                format);
    }
  else
    {
      contents = apr_psprintf(pool, "%d\n", format);
    }

    {
      const char *path_tmp;

      SVN_ERR(svn_io_write_unique(&path_tmp,
                                  svn_path_dirname(path, pool),
                                  contents, strlen(contents),
                                  svn_io_file_del_none, pool));

#ifdef WIN32
      /* make the destination writable, but only on Windows, because
         Windows does not let us replace read-only files. */
      SVN_ERR(svn_io_set_file_read_write(path, TRUE, pool));
#endif /* WIN32 */

      /* rename the temp file as the real destination */
      SVN_ERR(svn_io_file_rename(path_tmp, path, pool));
    }

  /* And set the perms to make it read only */
  return svn_io_set_file_read_only(path, FALSE, pool);
}

/* Return the expected contents of "iota" in revision REV. */
static const char *
get_rev_contents(svn_revnum_t rev, apr_pool_t *pool)
{
  /* Toss in a bunch of magic numbers for spice. */
  apr_int64_t num = ((rev * 1234353 + 4358) * 4583 + ((rev % 4) << 1)) / 42;
  return apr_psprintf(pool, "%" APR_INT64_T_FMT "\n", num);
}

/* Create a packed filesystem in DIR.  Set the shard size to SHARD_SIZE
   and create MAX_REV number of revisions.  Use POOL for allocations. */
static svn_error_t *
create_packed_filesystem(const char *dir,
                         const svn_test_opts_t *opts,
                         int max_rev,
                         int shard_size,
                         apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t after_rev;
  apr_pool_t *subpool = svn_pool_create(pool);

  /* Create a filesystem, then close it */
  SVN_ERR(svn_test__create_fs(&fs, dir, opts, subpool));
  svn_pool_destroy(subpool);

  subpool = svn_pool_create(pool);

  /* Rewrite the format file */
  SVN_ERR(write_format(dir, SVN_FS_FS__FORMAT_NUMBER,
                       shard_size, subpool));

  /* Reopen the filesystem */
  SVN_ERR(svn_fs_open(&fs, dir, NULL, subpool));

  /* Revision 1: the Greek tree */
  SVN_ERR(svn_fs_begin_txn(&txn, fs, 0, subpool));
  SVN_ERR(svn_fs_txn_root(&txn_root, txn, subpool));
  SVN_ERR(svn_test__create_greek_tree(txn_root, subpool));
  SVN_ERR(svn_fs_commit_txn(&conflict, &after_rev, txn, subpool));

  /* Revisions 2-11: A bunch of random changes. */
  while (after_rev < max_rev + 1)
    {
      SVN_ERR(svn_fs_begin_txn(&txn, fs, after_rev, subpool));
      SVN_ERR(svn_fs_txn_root(&txn_root, txn, subpool));
      SVN_ERR(svn_test__set_file_contents(txn_root, "iota",
                                          get_rev_contents(after_rev + 1,
                                                           subpool),
                                          subpool));
      SVN_ERR(svn_fs_commit_txn(&conflict, &after_rev, txn, subpool));
    }
  svn_pool_destroy(subpool);

  /* Now pack the FS */
  return svn_fs_pack(dir, NULL, NULL, NULL, NULL, pool);
}

/* Pack a filesystem.  */
#define REPO_NAME "test-repo-fsfs-pack"
#define SHARD_SIZE 7
#define MAX_REV 53
static svn_error_t *
pack_filesystem(const svn_test_opts_t *opts,
                apr_pool_t *pool)
{
  int i;
  svn_node_kind_t kind;
  const char *path;
  char buf[80];
  apr_file_t *file;
  apr_size_t len;

  /* Bail (with success) on known-untestable scenarios */
  if ((strcmp(opts->fs_type, "fsfs") != 0)
      || (opts->server_minor_version && (opts->server_minor_version < 6)))
    return SVN_NO_ERROR;

  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, SHARD_SIZE,
                                   pool));

  /* Check to see that the pack files exist, and that the rev directories
     don't. */
  for (i = 0; i < (MAX_REV + 1) / SHARD_SIZE; i++)
    {
      path = svn_path_join_many(pool, REPO_NAME, "revs",
            apr_psprintf(pool, "%d.pack", i / SHARD_SIZE), "pack", NULL);

      /* These files should exist. */
      SVN_ERR(svn_io_check_path(path, &kind, pool));
      if (kind != svn_node_file)
        return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                                 "Expected pack file '%s' not found", path);

      path = svn_path_join_many(pool, REPO_NAME, "revs",
            apr_psprintf(pool, "%d.pack", i / SHARD_SIZE), "manifest", NULL);
      SVN_ERR(svn_io_check_path(path, &kind, pool));
      if (kind != svn_node_file)
        return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                                 "Expected manifest file '%s' not found",
                                 path);

      /* This directory should not exist. */
      path = svn_path_join_many(pool, REPO_NAME, "revs",
            apr_psprintf(pool, "%d", i / SHARD_SIZE), NULL);
      SVN_ERR(svn_io_check_path(path, &kind, pool));
      if (kind != svn_node_none)
        return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                                 "Unexpected directory '%s' found", path);
    }

  /* Ensure the min-unpacked-rev jives with the above operations. */
  SVN_ERR(svn_io_file_open(&file,
                           svn_path_join(REPO_NAME, PATH_MIN_UNPACKED_REV,
                                         pool),
                           APR_READ | APR_BUFFERED, APR_OS_DEFAULT, pool));
  len = sizeof(buf);
  SVN_ERR(svn_io_read_length_line(file, buf, &len, pool));
  SVN_ERR(svn_io_file_close(file, pool));
  if (SVN_STR_TO_REV(buf) != (MAX_REV / SHARD_SIZE) * SHARD_SIZE)
    return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                             "Bad '%s' contents", PATH_MIN_UNPACKED_REV);

  /* Finally, make sure the final revision directory does exist. */
  path = svn_path_join_many(pool, REPO_NAME, "revs",
        apr_psprintf(pool, "%d", (i / SHARD_SIZE) + 1), NULL);
  SVN_ERR(svn_io_check_path(path, &kind, pool));
  if (kind != svn_node_none)
    return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                             "Expected directory '%s' not found",
                             path);


  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef SHARD_SIZE
#undef MAX_REV

/* Pack a filesystem.  */
#define REPO_NAME "test-repo-fsfs-pack-even"
#define SHARD_SIZE 4
#define MAX_REV 10
static svn_error_t *
pack_even_filesystem(const svn_test_opts_t *opts,
                     apr_pool_t *pool)
{
  svn_node_kind_t kind;
  const char *path;

  /* Bail (with success) on known-untestable scenarios */
  if ((strcmp(opts->fs_type, "fsfs") != 0)
      || (opts->server_minor_version && (opts->server_minor_version < 6)))
    return SVN_NO_ERROR;

  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, SHARD_SIZE,
                                   pool));

  path = svn_path_join_many(pool, REPO_NAME, "revs", "2.pack", NULL);
  SVN_ERR(svn_io_check_path(path, &kind, pool));
  if (kind != svn_node_dir)
    return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                             "Packing did not complete as expected");

  return SVN_NO_ERROR;
}
#undef REPO_NAME
#undef SHARD_SIZE
#undef MAX_REV

/* Check reading from a packed filesystem. */
#define REPO_NAME "test-repo-read-packed-fs"
static svn_error_t *
read_packed_fs(const svn_test_opts_t *opts,
               apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_stream_t *rstream;
  svn_stringbuf_t *rstring;
  svn_revnum_t i;

  /* Bail (with success) on known-untestable scenarios */
  if ((strcmp(opts->fs_type, "fsfs") != 0)
      || (opts->server_minor_version && (opts->server_minor_version < 6)))
    return SVN_NO_ERROR;

  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, 11, 5, pool));
  SVN_ERR(svn_fs_open(&fs, REPO_NAME, NULL, pool));

  for (i = 1; i < 12; i++)
    {
      svn_fs_root_t *rev_root;
      svn_stringbuf_t *sb;

      SVN_ERR(svn_fs_revision_root(&rev_root, fs, i, pool));
      SVN_ERR(svn_fs_file_contents(&rstream, rev_root, "iota", pool));
      SVN_ERR(svn_test__stream_to_string(&rstring, rstream, pool));

      if (i == 1)
        sb = svn_stringbuf_create("This is the file 'iota'.\n", pool);
      else
        sb = svn_stringbuf_create(get_rev_contents(i, pool), pool);

      if (! svn_stringbuf_compare(rstring, sb))
        return svn_error_createf(SVN_ERR_FS_GENERAL, NULL,
                                 "Bad data in revision %ld.", i);
    }

  return SVN_NO_ERROR;
}
#undef REPO_NAME

/* Check reading from a packed filesystem. */
#define REPO_NAME "test-repo-commit-packed-fs"
static svn_error_t *
commit_packed_fs(const svn_test_opts_t *opts,
                 apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t after_rev;

  /* Bail (with success) on known-untestable scenarios */
  if ((strcmp(opts->fs_type, "fsfs") != 0)
      || (opts->server_minor_version && (opts->server_minor_version < 6)))
    return SVN_NO_ERROR;

  /* Create the packed FS and open it. */
  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, 11, 5, pool));
  SVN_ERR(svn_fs_open(&fs, REPO_NAME, NULL, pool));

  /* Now do a commit. */
  SVN_ERR(svn_fs_begin_txn(&txn, fs, 12, pool));
  SVN_ERR(svn_fs_txn_root(&txn_root, txn, pool));
  SVN_ERR(svn_test__set_file_contents(txn_root, "iota",
          "How much better is it to get wisdom than gold! and to get "
          "understanding rather to be chosen than silver!", pool));
  SVN_ERR(svn_fs_commit_txn(&conflict, &after_rev, txn, pool));

  return SVN_NO_ERROR;
}
#undef REPO_NAME

/* Get/set revprop while repository is being packed in background. */
#define REPO_NAME "test-get-set-revprop-packed-fs"
#define SHARD_SIZE 4
#define MAX_REV 1
static svn_error_t *
get_set_revprop_packed_fs(const svn_test_opts_t *opts,
                          apr_pool_t *pool)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  const char *conflict;
  svn_revnum_t after_rev;
  svn_string_t *prop_value;
  apr_pool_t *subpool;

  /* Bail (with success) on known-untestable scenarios */
  if ((strcmp(opts->fs_type, "fsfs") != 0)
      || (opts->server_minor_version && (opts->server_minor_version < 7)))
    return SVN_NO_ERROR;

  /* Create the packed FS and open it. */
  SVN_ERR(create_packed_filesystem(REPO_NAME, opts, MAX_REV, SHARD_SIZE, pool));
  SVN_ERR(svn_fs_open(&fs, REPO_NAME, NULL, pool));

  subpool = svn_pool_create(pool);
  /* Do a commit to trigger packing. */
  SVN_ERR(svn_fs_begin_txn(&txn, fs, MAX_REV + 1, subpool));
  SVN_ERR(svn_fs_txn_root(&txn_root, txn, subpool));
  SVN_ERR(svn_test__set_file_contents(txn_root, "iota", "new-iota",  subpool));
  SVN_ERR(svn_fs_commit_txn(&conflict, &after_rev, txn, subpool));
  svn_pool_clear(subpool);

  /* Pack the repository. */
  SVN_ERR(svn_fs_pack(REPO_NAME, NULL, NULL, NULL, NULL, pool));

  /* Try to get revprop for revision 0. */
  SVN_ERR(svn_fs_revision_prop(&prop_value, fs, 0, SVN_PROP_REVISION_AUTHOR, pool));
  
  /* Try to change revprop for revision 0. */
  SVN_ERR(svn_fs_change_rev_prop(fs, 0, SVN_PROP_REVISION_AUTHOR,
                                 svn_string_create("tweaked-author", pool), pool));

  return SVN_NO_ERROR;
}

/* ------------------------------------------------------------------------ */

/* The test table.  */

struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_OPTS_PASS(pack_filesystem,
                       "pack a FSFS filesystem"),
    SVN_TEST_OPTS_PASS(pack_even_filesystem,
                       "pack FSFS where revs % shard = 0"),
    SVN_TEST_OPTS_PASS(read_packed_fs,
                       "read from a packed FSFS filesystem"),
    SVN_TEST_OPTS_PASS(commit_packed_fs,
                       "commit to a packed FSFS filesystem"),
    SVN_TEST_OPTS_XFAIL(get_set_revprop_packed_fs,
                        "get/set revprop while packing FSFS filesystem"),
    SVN_TEST_NULL
  };
