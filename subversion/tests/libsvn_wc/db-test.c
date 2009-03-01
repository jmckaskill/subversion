/*
 * db-test.c :  test the wc_db subsystem
 *
 * ====================================================================
 * Copyright (c) 2009 CollabNet.  All rights reserved.
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

#include <apr_pools.h>
#include <apr_general.h>

#include "svn_types.h"
#include "svn_io.h"
#include "svn_dirent_uri.h"

#include "private/svn_sqlite.h"

#include "../../libsvn_wc/wc.h"
#include "../../libsvn_wc/wc_db.h"
#include "../../libsvn_wc/wc-metadata.h"

#include "../svn_test.h"


#define ROOT_ONE "http://example.com/one/"
#define ROOT_TWO "http://example.com/two/"

#define UUID_ONE "uuid1"
#define UUID_TWO "uuid2"

#define TIME_1 1235142208
#define TIME_2 1235142268
#define TIME_3 1235142328

#define TIME_1s APR_STRINGIFY(TIME_1) "000000"
#define TIME_2s APR_STRINGIFY(TIME_2) "000000"
#define TIME_3s APR_STRINGIFY(TIME_3) "000000"

#define TIME_1a apr_time_from_sec(TIME_1)
#define TIME_2a apr_time_from_sec(TIME_2)
#define TIME_3a apr_time_from_sec(TIME_3)

#define AUTHOR_1 "johndoe"
#define AUTHOR_2 "janedoe"

#define MD5_1 "2d18c5e57e84c5b8a5e9a6e13fa394dc"
#define MD5_2 "5d41402abc4b2a76b9719d911017c592"
#define SHA1_1 "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d"

static const char * const data_loading_sql[] = {
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  (
   /* Load the table and index definitions. */
   WC_METADATA_SQL
   " "

   /* Load our test data.

      Note: do not use named-column insertions. This allows us to test
      the column count in the schema matches our expectation here. */

   "insert into repository values (1, '" ROOT_ONE "', '" UUID_ONE "'); "
   "insert into repository values (2, '" ROOT_TWO "', '" UUID_TWO "'); "
   "insert into wcroot values (1, null); "

   "insert into base_node values ("
   "  1, '', 1, '', null, 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', 'infinity', null, null, '()', 0); "
   "insert into base_node values ("
   "  1, 'A', null, null, '', 'normal', 'file', "
   "  1, '$md5 $" MD5_1 "', 10, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null); "
   "insert into base_node values ("
   "  1, 'B', null, null, '', 'excluded', 'symlink', "
   "  null, null, null, "
   "  null, null, null, null, null, null, null, null); "
   "insert into base_node values ("
   "  1, 'C', null, null, '', 'absent', 'unknown', "
   "  null, null, null, "
   "  null, null, null, null, null, null, null, null); "
   "insert into base_node values ("
   "  1, 'D', null, null, '', 'not-present', 'unknown', "
   "  null, null, null, "
   "  null, null, null, null, null, null, null, null); "
   "insert into base_node values ("
   "  1, 'E', null, null, '', 'incomplete', 'unknown', "
   "  null, null, null, "
   "  null, null, null, null, null, null, null, null); "
   "insert into base_node values ("
   "  1, 'F', null, null, '', 'normal', 'file', "
   "  1, '$sha1$" SHA1_1 "', 15, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null); "
   "insert into base_node values ("
   "  1, 'G', 2, 'G-alt', '', 'normal', 'file', "
   "  1, '$sha1$" SHA1_1 "', 15, "
   "  2, " TIME_2s ", '" AUTHOR_2 "', null, null, null, '()', null); "
   "insert into base_node values ("
   "  1, 'H', null, null, '', 'normal', 'symlink', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, 'H-target', null, '()', null); "
   "insert into base_node values ("
   "  1, 'I', null, null, '', 'normal', 'dir', "
   "  1, null, null, "
   "  1, " TIME_1s ", '" AUTHOR_1 "', null, null, null, '()', null); "
   " "
   "insert into working_node values ("
   "  1, 'I', '', 'normal', 'dir', "
   "  null, null, "
   "  2, " TIME_2s ", '" AUTHOR_2 "', 'immediates', null, "
   "  2, 'some/dir', 2, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J', '', 'normal', 'dir', "
   "  null, null, "
   "  null, null, null, 'immediates', null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-a', 'J', 'normal', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-b', 'J', 'normal', 'dir', "
   "  null, null, "
   "  2, " TIME_2s ", '" AUTHOR_2 "', 'infinity', null, "
   "  2, 'some/dir', 2, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-b/J-b-a', 'J/J-b', 'normal', 'dir', "
   "  null, null, "
   "  2, " TIME_2s ", '" AUTHOR_2 "', 'infinity', null, "
   "  2, 'another/dir', 2, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-b/J-b-b', 'J/J-b', 'normal', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-c', 'J', 'not-present', 'dir', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-c/J-c-a', 'J/J-c', 'not-present', 'dir', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-d', 'J', 'normal', 'file', "
   "  '$md5 $" MD5_1 "', 10, "
   "  2, " TIME_2s ", '" AUTHOR_2 "', null, null, "
   "  2, 'moved/file', 2, 1, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-e', 'J', 'not-present', 'dir', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, 'other/place', null, '()', 0); "
   "insert into working_node values ("
   "  1, 'J/J-e/J-e-a', 'J/J-e', 'not-present', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   "insert into working_node values ("
   "  1, 'K', '', 'not-present', 'file', "
   "  null, null, "
   "  null, null, null, null, null, "
   "  null, null, null, 0, null, null, '()', 0); "
   " "
   "insert into actual_node values ("
   "  1, 'I', '', null, null, null, null, null, 'changelist', null, null); "
   " "
   )
};


static svn_error_t *
create_fake_wc(const char *subdir, apr_pool_t *scratch_pool)
{
  const char *dirpath = svn_dirent_join_many(scratch_pool,
                                             "fake-wc", subdir, ".svn", NULL);
  const char *dbpath = svn_dirent_join(dirpath, "wc.db", scratch_pool);
  svn_sqlite__db_t *sdb;

  SVN_ERR(svn_io_make_dir_recursively(dirpath, scratch_pool));
  svn_error_clear(svn_io_remove_file(dbpath, scratch_pool));
  SVN_ERR(svn_sqlite__open(&sdb, dbpath,
                           svn_sqlite__mode_rwcreate, NULL,
                           SVN_WC__VERSION_EXPERIMENTAL, data_loading_sql,
                           scratch_pool, scratch_pool));

  return SVN_NO_ERROR;
}


static void
set_prop(apr_hash_t *props, const char *name, const char *value,
         apr_pool_t *result_pool)
{
  const svn_string_t *propval = svn_string_create(value, result_pool);

  apr_hash_set(props, name, APR_HASH_KEY_STRING, propval);
}


static svn_error_t *
test_getting_info(const char **msg,
                  svn_boolean_t msg_only,
                  svn_test_opts_t *opts,
                  apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc__db_kind_t kind;
  svn_wc__db_status_t status;
  svn_revnum_t revision;
  const char *repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_revnum_t changed_rev;
  apr_time_t changed_date;
  const char *changed_author;
  svn_depth_t depth;
  svn_checksum_t *checksum;
  svn_filesize_t translated_size;
  const char *target;
  svn_wc__db_t *db;
  svn_error_t *err;

  *msg = "get information from wc.db";
  if (msg_only)
    return SVN_NO_ERROR;

  SVN_ERR(create_fake_wc("test_getting_info", pool));
  SVN_ERR(svn_dirent_get_absolute(&local_abspath,
                                  "fake-wc/test_getting_info",
                                  pool));

  SVN_ERR(svn_wc__db_open(&db, svn_wc__db_openmode_readonly,
                          local_abspath, NULL, pool, pool));

  /* Test: basic fetching of data. */
  SVN_ERR(svn_wc__db_base_get_info(
            &status, &kind, &revision,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &changed_rev, &changed_date, &changed_author,
            &depth, &checksum, &translated_size, &target,
            db, local_abspath,
            pool, pool));
  SVN_ERR_ASSERT(kind == svn_wc__db_kind_dir);
  SVN_ERR_ASSERT(status == svn_wc__db_status_normal);
  SVN_ERR_ASSERT(revision == 1);
  SVN_ERR_ASSERT(strcmp(repos_relpath, "") == 0);
  SVN_ERR_ASSERT(strcmp(repos_root_url, ROOT_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(repos_uuid, UUID_ONE) == 0);
  SVN_ERR_ASSERT(changed_rev == 1);
  SVN_ERR_ASSERT(changed_date == TIME_1a);
  SVN_ERR_ASSERT(strcmp(changed_author, AUTHOR_1) == 0);
  SVN_ERR_ASSERT(depth == svn_depth_infinity);
  SVN_ERR_ASSERT(checksum == NULL);
  SVN_ERR_ASSERT(translated_size == SVN_INVALID_FILESIZE);
  SVN_ERR_ASSERT(target == NULL);

  /* Test: NULL params, file-specific values, inherit repos info. */
  SVN_ERR(svn_wc__db_base_get_info(
            NULL, &kind, NULL,
            &repos_relpath, &repos_root_url, &repos_uuid,
            NULL, NULL, NULL,
            NULL, &checksum, &translated_size, NULL,
            db, svn_dirent_join(local_abspath, "A", pool),
            pool, pool));
  SVN_ERR_ASSERT(kind == svn_wc__db_kind_file);
  SVN_ERR_ASSERT(repos_relpath == NULL);
  SVN_ERR_ASSERT(repos_root_url == NULL);
  SVN_ERR_ASSERT(repos_uuid == NULL);
  SVN_ERR_ASSERT(strcmp(MD5_1, svn_checksum_to_cstring(checksum, pool)) == 0);
  SVN_ERR_ASSERT(translated_size == 10);

  /* Grab the inherited info. */
  SVN_ERR(svn_wc__db_scan_base_repos(
            &repos_relpath, &repos_root_url, &repos_uuid,
            db, svn_dirent_join(local_abspath, "A", pool),
            pool, pool));
  SVN_ERR_ASSERT(strcmp(repos_relpath, "A") == 0);
  SVN_ERR_ASSERT(strcmp(repos_root_url, ROOT_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(repos_uuid, UUID_ONE) == 0);

  /* Test: symlink kind, excluded presence, default values for columns. */
  SVN_ERR(svn_wc__db_base_get_info(
            &status, &kind, &revision,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &changed_rev, &changed_date, &changed_author,
            &depth, &checksum, &translated_size, &target,
            db, svn_dirent_join(local_abspath, "B", pool),
            pool, pool));
  SVN_ERR_ASSERT(kind == svn_wc__db_kind_symlink);
  SVN_ERR_ASSERT(status == svn_wc__db_status_excluded);
  SVN_ERR_ASSERT(!SVN_IS_VALID_REVNUM(revision));
  SVN_ERR_ASSERT(repos_relpath == NULL);
  SVN_ERR_ASSERT(repos_root_url == NULL);
  SVN_ERR_ASSERT(repos_uuid == NULL);
  SVN_ERR_ASSERT(!SVN_IS_VALID_REVNUM(changed_rev));
  SVN_ERR_ASSERT(changed_date == 0);
  SVN_ERR_ASSERT(changed_author == NULL);
  SVN_ERR_ASSERT(depth == svn_depth_unknown);
  SVN_ERR_ASSERT(checksum == NULL);
  SVN_ERR_ASSERT(translated_size == SVN_INVALID_FILESIZE);
  SVN_ERR_ASSERT(target == NULL);

  /* Test: unknown kind, absent presence. */
  SVN_ERR(svn_wc__db_base_get_info(
            &status, &kind, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            db, svn_dirent_join(local_abspath, "C", pool),
            pool, pool));
  SVN_ERR_ASSERT(kind == svn_wc__db_kind_unknown);
  SVN_ERR_ASSERT(status == svn_wc__db_status_absent);

  /* Test: not-present presence. */
  SVN_ERR(svn_wc__db_base_get_info(
            &status, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            db, svn_dirent_join(local_abspath, "D", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_not_present);

  /* Test: incomplete presence. */
  SVN_ERR(svn_wc__db_base_get_info(
            &status, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            db, svn_dirent_join(local_abspath, "E", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_incomplete);

  /* Test: SHA1 checksum. */
  SVN_ERR(svn_wc__db_base_get_info(
            NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, &checksum, &translated_size, NULL,
            db, svn_dirent_join(local_abspath, "F", pool),
            pool, pool));
  SVN_ERR_ASSERT(strcmp(SHA1_1, svn_checksum_to_cstring(checksum, pool)) == 0);
  SVN_ERR_ASSERT(translated_size == 15);

  /* Test: alternate repository (switched file). */
  SVN_ERR(svn_wc__db_base_get_info(
            NULL, NULL, NULL,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &changed_rev, &changed_date, &changed_author,
            NULL, NULL, NULL, NULL,
            db, svn_dirent_join(local_abspath, "G", pool),
            pool, pool));
  SVN_ERR_ASSERT(strcmp(repos_relpath, "G-alt") == 0);
  SVN_ERR_ASSERT(strcmp(repos_root_url, ROOT_TWO) == 0);
  SVN_ERR_ASSERT(strcmp(repos_uuid, UUID_TWO) == 0);
  SVN_ERR_ASSERT(changed_rev == 2);
  SVN_ERR_ASSERT(changed_date == TIME_2a);
  SVN_ERR_ASSERT(strcmp(changed_author, AUTHOR_2) == 0);

  /* Test: symlink target. */
  SVN_ERR(svn_wc__db_base_get_info(
            NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, &checksum, &translated_size, &target,
            db, svn_dirent_join(local_abspath, "H", pool),
            pool, pool));
  SVN_ERR_ASSERT(checksum == NULL);
  SVN_ERR_ASSERT(translated_size == SVN_INVALID_FILESIZE);
  SVN_ERR_ASSERT(strcmp(target, "H-target") == 0);

  /* Test: missing node. */
  err = svn_wc__db_base_get_info(
            NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            db, svn_dirent_join(local_abspath, "missing-file", pool),
            pool, pool);
  SVN_ERR_ASSERT(err != NULL && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND);
  svn_error_clear(err);

  return SVN_NO_ERROR;
}


static svn_error_t *
validate_node(svn_wc__db_t *db,
              const char *local_abspath,
              const char *relpath,
              svn_wc__db_kind_t expected_kind,
              svn_wc__db_status_t expected_status,
              apr_pool_t *scratch_pool)
{
  const char *path = svn_dirent_join(local_abspath, relpath, scratch_pool);
  svn_wc__db_kind_t kind;
  svn_wc__db_status_t status;
#ifdef WHEN_GET_PROPS_WORKS
  apr_hash_t *props;
#endif

  SVN_ERR(svn_wc__db_base_get_info(
            &status, &kind, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            db, path,
            scratch_pool, scratch_pool));
  SVN_ERR_ASSERT(kind == expected_kind);
  SVN_ERR_ASSERT(status == expected_status);

#ifdef WHEN_GET_PROPS_WORKS
  SVN_ERR(svn_wc__db_base_get_props(&props, db, path,
                                    scratch_pool, scratch_pool));

  value = apr_hash_get(props, "p1", APR_HASH_KEY_STRING);
  SVN_ERR_ASSERT(value != NULL && strcmp(value, "v1") == 0);

  value = apr_hash_get(props, "for-file", APR_HASH_KEY_STRING);
  SVN_ERR_ASSERT(value != NULL && strcmp(value, relpath) == 0);
#endif

  return SVN_NO_ERROR;
}


static svn_error_t *
test_inserting_nodes(const char **msg,
                     svn_boolean_t msg_only,
                     svn_test_opts_t *opts,
                     apr_pool_t *pool)
{
  const char *local_abspath;
  svn_checksum_t *checksum;
  svn_wc__db_t *db;
  apr_hash_t *props;
  const apr_array_header_t *children;

  *msg = "insert different nodes into wc.db";
  if (msg_only)
    return SVN_NO_ERROR;

  SVN_ERR(create_fake_wc("test_inserting_nodes", pool));
  SVN_ERR(svn_dirent_get_absolute(&local_abspath,
                                  "fake-wc/test_inserting_nodes",
                                  pool));

  SVN_ERR(svn_wc__db_open(&db, svn_wc__db_openmode_readwrite,
                          local_abspath, NULL, pool, pool));

  props = apr_hash_make(pool);
  set_prop(props, "p1", "v1", pool);

  children = svn_cstring_split("N-a N-b N-c", " ", FALSE, pool);

  SVN_ERR(svn_checksum_parse_hex(&checksum, svn_checksum_md5, MD5_1, pool));

  /* Create a new directory and several child nodes. */
  set_prop(props, "for-file", "N", pool);
  SVN_ERR(svn_wc__db_base_add_directory(
            db, svn_dirent_join(local_abspath, "N", pool),
            "N", ROOT_ONE, UUID_ONE, 3,
            props,
            1, TIME_1a, AUTHOR_1,
            children, svn_depth_infinity,
            pool));

  /* Replace an incomplete node with a file node. */
  set_prop(props, "for-file", "N/N-a", pool);
  SVN_ERR(svn_wc__db_base_add_file(
            db, svn_dirent_join(local_abspath, "N/N-a", pool),
            "N/N-a", ROOT_ONE, UUID_ONE, 3,
            props,
            1, TIME_1a, AUTHOR_1,
            checksum, 10,
            pool));

  /* Create a new symlink node. */
  set_prop(props, "for-file", "O", pool);
  SVN_ERR(svn_wc__db_base_add_symlink(
            db, svn_dirent_join(local_abspath, "O", pool),
            "O", ROOT_ONE, UUID_ONE, 3,
            props,
            1, TIME_1a, AUTHOR_1,
            "O-target",
            pool));

  /* Replace an incomplete node with an absent file node. */
  set_prop(props, "for-file", "N/N-b", pool);
  SVN_ERR(svn_wc__db_base_add_absent_node(
            db, svn_dirent_join(local_abspath, "N/N-b", pool),
            "N/N-b", ROOT_ONE, UUID_ONE, 3,
            svn_wc__db_kind_file, svn_wc__db_status_absent,
            pool));

  /* Create a new excluded directory node. */
  set_prop(props, "for-file", "P", pool);
  SVN_ERR(svn_wc__db_base_add_absent_node(
            db, svn_dirent_join(local_abspath, "P", pool),
            "P", ROOT_ONE, UUID_ONE, 3,
            svn_wc__db_kind_dir, svn_wc__db_status_excluded,
            pool));

  /* Create a new not-present symlink node. */
  set_prop(props, "for-file", "Q", pool);
  SVN_ERR(svn_wc__db_base_add_absent_node(
            db, svn_dirent_join(local_abspath, "Q", pool),
            "Q", ROOT_ONE, UUID_ONE, 3,
            svn_wc__db_kind_symlink, svn_wc__db_status_not_present,
            pool));

  /* Create a new absent unknown-kind node. */
  set_prop(props, "for-file", "R", pool);
  SVN_ERR(svn_wc__db_base_add_absent_node(
            db, svn_dirent_join(local_abspath, "R", pool),
            "R", ROOT_ONE, UUID_ONE, 3,
            svn_wc__db_kind_unknown, svn_wc__db_status_absent,
            pool));


  /* Are all the nodes where we expect them to be? */
  SVN_ERR(validate_node(db, local_abspath, "N",
                        svn_wc__db_kind_dir, svn_wc__db_status_normal,
                        pool));
  SVN_ERR(validate_node(db, local_abspath, "N/N-a",
                        svn_wc__db_kind_file, svn_wc__db_status_normal,
                        pool));
  SVN_ERR(validate_node(db, local_abspath, "N/N-b",
                        svn_wc__db_kind_file, svn_wc__db_status_absent,
                        pool));
  SVN_ERR(validate_node(db, local_abspath, "N/N-c",
                        svn_wc__db_kind_unknown, svn_wc__db_status_incomplete,
                        pool));
  SVN_ERR(validate_node(db, local_abspath, "O",
                        svn_wc__db_kind_symlink, svn_wc__db_status_normal,
                        pool));
  SVN_ERR(validate_node(db, local_abspath, "P",
                        svn_wc__db_kind_dir, svn_wc__db_status_excluded,
                        pool));
  SVN_ERR(validate_node(db, local_abspath, "Q",
                        svn_wc__db_kind_symlink, svn_wc__db_status_not_present,
                        pool));
  SVN_ERR(validate_node(db, local_abspath, "R",
                        svn_wc__db_kind_unknown, svn_wc__db_status_absent,
                        pool));

  /* ### do we need to test any attributes of the node? */

  /* ### yes: test the repos inheritance stuff (at least) */

  return SVN_NO_ERROR;
}


static svn_error_t *
test_base_children(const char **msg,
                   svn_boolean_t msg_only,
                   svn_test_opts_t *opts,
                   apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc__db_t *db;
  const apr_array_header_t *children;
  int i;

  *msg = "getting the list of BASE children";
  if (msg_only)
    return SVN_NO_ERROR;

  SVN_ERR(create_fake_wc("test_base_children", pool));
  SVN_ERR(svn_dirent_get_absolute(&local_abspath,
                                  "fake-wc/test_base_children",
                                  pool));

  SVN_ERR(svn_wc__db_open(&db, svn_wc__db_openmode_readonly,
                          local_abspath, NULL, pool, pool));

  SVN_ERR(svn_wc__db_base_get_children(&children,
                                       db, local_abspath,
                                       pool, pool));
  SVN_ERR_ASSERT(children->nelts == 9);
  for (i = children->nelts; i--; )
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);

      SVN_ERR_ASSERT(strlen(name) == 1);
      /* ### check the actual values */
    }

  SVN_ERR(svn_wc__db_read_children(&children,
                                   db, local_abspath,
                                   pool, pool));
  SVN_ERR_ASSERT(children->nelts == 11);
  for (i = children->nelts; i--; )
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);

      SVN_ERR_ASSERT(strlen(name) == 1);
      /* ### check the actual values */
    }

  /* ### insert some more children. replace some nodes. check values. */

  return SVN_NO_ERROR;
}


static svn_error_t *
test_working_info(const char **msg,
                  svn_boolean_t msg_only,
                  svn_test_opts_t *opts,
                  apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc__db_kind_t kind;
  svn_wc__db_status_t status;
  svn_revnum_t revision;
  const char *repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_revnum_t changed_rev;
  apr_time_t changed_date;
  const char *changed_author;
  svn_depth_t depth;
  svn_checksum_t *checksum;
  svn_filesize_t translated_size;
  const char *target;
  const char *changelist;
  const char *original_repos_relpath;
  const char *original_root_url;
  const char *original_uuid;
  svn_revnum_t original_revnum;
  svn_boolean_t text_mod;
  svn_boolean_t props_mod;
  svn_boolean_t base_shadowed;
  svn_wc__db_t *db;

  *msg = "reading information about the WORKING tree";
  if (msg_only)
    return SVN_NO_ERROR;

  SVN_ERR(create_fake_wc("test_working_info", pool));
  SVN_ERR(svn_dirent_get_absolute(&local_abspath,
                                  "fake-wc/test_working_info",
                                  pool));

  SVN_ERR(svn_wc__db_open(&db, svn_wc__db_openmode_readonly,
                          local_abspath, NULL, pool, pool));

  /* Test: basic fetching of data. */
  SVN_ERR(svn_wc__db_read_info(
            &status, &kind, &revision,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &changed_rev, &changed_date, &changed_author,
            &depth, &checksum, &translated_size, &target,
            &changelist, &original_repos_relpath, &original_root_url,
            &original_uuid, &original_revnum,
            &text_mod, &props_mod, &base_shadowed,
            db, svn_dirent_join(local_abspath, "I", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_added);
  SVN_ERR_ASSERT(kind == svn_wc__db_kind_dir);
  SVN_ERR_ASSERT(revision == SVN_INVALID_REVNUM);
  SVN_ERR_ASSERT(repos_relpath == NULL);
  SVN_ERR_ASSERT(repos_root_url == NULL);
  SVN_ERR_ASSERT(repos_uuid == NULL);
  SVN_ERR_ASSERT(changed_rev == 2);
  SVN_ERR_ASSERT(changed_date == TIME_2a);
  SVN_ERR_ASSERT(strcmp(changed_author, AUTHOR_2) == 0);
  SVN_ERR_ASSERT(depth == svn_depth_immediates);
  SVN_ERR_ASSERT(checksum == NULL);
  SVN_ERR_ASSERT(translated_size == SVN_INVALID_FILESIZE);
  SVN_ERR_ASSERT(target == NULL);
  SVN_ERR_ASSERT(strcmp(changelist, "changelist") == 0);
  SVN_ERR_ASSERT(strcmp(original_repos_relpath, "some/dir") == 0);
  SVN_ERR_ASSERT(strcmp(original_root_url, ROOT_TWO) == 0);
  SVN_ERR_ASSERT(strcmp(original_uuid, UUID_TWO) == 0);
  SVN_ERR_ASSERT(original_revnum == 2);
  SVN_ERR_ASSERT(text_mod == FALSE);
  SVN_ERR_ASSERT(props_mod == FALSE);
  SVN_ERR_ASSERT(base_shadowed == TRUE);


  /* ### we need a hojillion more tests in here. I just want to get this
     ### round checked in, so I'm skipping more tests at this point.  */


  return SVN_NO_ERROR;
}


static svn_error_t *
test_pdh(const char **msg,
         svn_boolean_t msg_only,
         svn_test_opts_t *opts,
         apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc__db_t *db;

  *msg = "creation of per-directory handles";
  if (msg_only)
    return SVN_NO_ERROR;

  SVN_ERR(create_fake_wc("test_pdh", pool));
  SVN_ERR(svn_dirent_get_absolute(&local_abspath,
                                  "fake-wc/test_pdh",
                                  pool));

  SVN_ERR(svn_wc__db_open(&db, svn_wc__db_openmode_readwrite,
                          local_abspath, NULL, pool, pool));

  /* NOTE: this test doesn't do anything apparent -- it simply exercises
     some internal functionality of wc_db.  This is a handy driver for
     debugging wc_db to ensure it manages per-directory handles properly.  */

  SVN_ERR(svn_wc__db_base_add_absent_node(
            db, svn_dirent_join(local_abspath, "sub/A/B", pool),
            "sub/A/B", ROOT_ONE, UUID_ONE, 1,
            svn_wc__db_kind_file, svn_wc__db_status_absent,
            pool));

  SVN_ERR(svn_wc__db_base_add_absent_node(
            db, svn_dirent_join(local_abspath, "sub/A/B/C/D", pool),
            "sub/A/B/C/D", ROOT_ONE, UUID_ONE, 1,
            svn_wc__db_kind_file, svn_wc__db_status_absent,
            pool));
  
  return SVN_NO_ERROR;
}


static svn_error_t *
test_scan_working(const char **msg,
                  svn_boolean_t msg_only,
                  svn_test_opts_t *opts,
                  apr_pool_t *pool)
{
  const char *local_abspath;
  svn_wc__db_t *db;
  svn_wc__db_status_t status;
  const char *op_root_abspath;
  const char *repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  const char *original_repos_relpath;
  const char *original_root_url;
  const char *original_uuid;
  svn_revnum_t original_revision;
  const char *moved_to_abspath;

  *msg = "scanning working nodes";
  if (msg_only)
    return SVN_NO_ERROR;

  SVN_ERR(create_fake_wc("test_scan_working", pool));
  SVN_ERR(svn_dirent_get_absolute(&local_abspath,
                                  "fake-wc/test_scan_working",
                                  pool));
  SVN_ERR(svn_wc__db_open(&db, svn_wc__db_openmode_readonly,
                          local_abspath, NULL, pool, pool));

  /* Simple addition of a directory. */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "J", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_added);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "J", pool)) == 0);
  SVN_ERR_ASSERT(strcmp(repos_relpath, "J") == 0);
  SVN_ERR_ASSERT(strcmp(repos_root_url, ROOT_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(repos_uuid, UUID_ONE) == 0);
  SVN_ERR_ASSERT(original_repos_relpath == NULL);
  SVN_ERR_ASSERT(original_root_url == NULL);
  SVN_ERR_ASSERT(original_uuid == NULL);
  SVN_ERR_ASSERT(original_revision == SVN_INVALID_REVNUM);
  SVN_ERR_ASSERT(moved_to_abspath == NULL);

  /* Simple addition of a file (affects how scan-up is started). */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "J/J-a", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_added);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "J", pool)) == 0);
  SVN_ERR_ASSERT(strcmp(repos_relpath, "J/J-a") == 0);
  SVN_ERR_ASSERT(strcmp(repos_root_url, ROOT_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(repos_uuid, UUID_ONE) == 0);
  SVN_ERR_ASSERT(original_repos_relpath == NULL);
  SVN_ERR_ASSERT(original_root_url == NULL);
  SVN_ERR_ASSERT(original_uuid == NULL);
  SVN_ERR_ASSERT(original_revision == SVN_INVALID_REVNUM);
  SVN_ERR_ASSERT(moved_to_abspath == NULL);

  /* Node was moved elsewhere. */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "J/J-e", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_moved_src);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "J/J-e", pool)) == 0);
  SVN_ERR_ASSERT(repos_relpath == NULL);
  SVN_ERR_ASSERT(repos_root_url == NULL);
  SVN_ERR_ASSERT(repos_uuid == NULL);
  SVN_ERR_ASSERT(original_repos_relpath == NULL);
  SVN_ERR_ASSERT(original_root_url == NULL);
  SVN_ERR_ASSERT(original_uuid == NULL);
  SVN_ERR_ASSERT(original_revision == SVN_INVALID_REVNUM);
  SVN_ERR_ASSERT(strcmp(moved_to_abspath,
                        svn_dirent_join(local_abspath, "other/place", pool)
                        ) == 0);

  /* Node was moved elsewhere (child of operation root). */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "J/J-e/J-e-a", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_moved_src);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "J/J-e", pool)) == 0);
  SVN_ERR_ASSERT(repos_relpath == NULL);
  SVN_ERR_ASSERT(repos_root_url == NULL);
  SVN_ERR_ASSERT(repos_uuid == NULL);
  SVN_ERR_ASSERT(original_repos_relpath == NULL);
  SVN_ERR_ASSERT(original_root_url == NULL);
  SVN_ERR_ASSERT(original_uuid == NULL);
  SVN_ERR_ASSERT(original_revision == SVN_INVALID_REVNUM);
  SVN_ERR_ASSERT(strcmp(moved_to_abspath,
                        svn_dirent_join(local_abspath, "other/place", pool)
                        ) == 0);

  /* Node was moved here. */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "J/J-d", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_moved_dst);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "J/J-d", pool)) == 0);
  SVN_ERR_ASSERT(strcmp(repos_relpath, "J/J-d") == 0);
  SVN_ERR_ASSERT(strcmp(repos_root_url, ROOT_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(repos_uuid, UUID_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(original_repos_relpath, "moved/file") == 0);
  SVN_ERR_ASSERT(strcmp(original_root_url, ROOT_TWO) == 0);
  SVN_ERR_ASSERT(strcmp(original_uuid, UUID_TWO) == 0);
  SVN_ERR_ASSERT(original_revision == 2);
  SVN_ERR_ASSERT(moved_to_abspath == NULL);

  /* Check root of a copy. */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "J/J-b", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_copied);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "J/J-b", pool)) == 0);
  SVN_ERR_ASSERT(strcmp(repos_relpath, "J/J-b") == 0);
  SVN_ERR_ASSERT(strcmp(repos_root_url, ROOT_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(repos_uuid, UUID_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(original_repos_relpath, "some/dir") == 0);
  SVN_ERR_ASSERT(strcmp(original_root_url, ROOT_TWO) == 0);
  SVN_ERR_ASSERT(strcmp(original_uuid, UUID_TWO) == 0);
  SVN_ERR_ASSERT(original_revision == 2);
  SVN_ERR_ASSERT(moved_to_abspath == NULL);

  /* Ignore parent copy. Use copy closest to target.  */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "J/J-b/J-b-a", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_copied);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "J/J-b/J-b-a", pool)
                        ) == 0);
  SVN_ERR_ASSERT(strcmp(repos_relpath, "J/J-b/J-b-a") == 0);
  SVN_ERR_ASSERT(strcmp(repos_root_url, ROOT_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(repos_uuid, UUID_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(original_repos_relpath, "another/dir") == 0);
  SVN_ERR_ASSERT(strcmp(original_root_url, ROOT_TWO) == 0);
  SVN_ERR_ASSERT(strcmp(original_uuid, UUID_TWO) == 0);
  SVN_ERR_ASSERT(original_revision == 2);
  SVN_ERR_ASSERT(moved_to_abspath == NULL);

  /* Inherit parent copy. */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "J/J-b/J-b-b", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_copied);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "J/J-b", pool)) == 0);
  SVN_ERR_ASSERT(strcmp(repos_relpath, "J/J-b/J-b-b") == 0);
  SVN_ERR_ASSERT(strcmp(repos_root_url, ROOT_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(repos_uuid, UUID_ONE) == 0);
  SVN_ERR_ASSERT(strcmp(original_repos_relpath, "some/dir") == 0);
  SVN_ERR_ASSERT(strcmp(original_root_url, ROOT_TWO) == 0);
  SVN_ERR_ASSERT(strcmp(original_uuid, UUID_TWO) == 0);
  SVN_ERR_ASSERT(original_revision == 2);
  SVN_ERR_ASSERT(moved_to_abspath == NULL);

  /* Root of delete. Parent is a WORKING node. */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "J/J-c", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_deleted);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "J/J-c", pool)) == 0);
  SVN_ERR_ASSERT(repos_relpath == NULL);
  SVN_ERR_ASSERT(repos_root_url == NULL);
  SVN_ERR_ASSERT(repos_uuid == NULL);
  SVN_ERR_ASSERT(original_repos_relpath == NULL);
  SVN_ERR_ASSERT(original_root_url == NULL);
  SVN_ERR_ASSERT(original_uuid == NULL);
  SVN_ERR_ASSERT(original_revision == SVN_INVALID_REVNUM);
  SVN_ERR_ASSERT(moved_to_abspath == NULL);

  /* Child of a deleted root. */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "J/J-c/J-c-a", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_deleted);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "J/J-c", pool)) == 0);
  SVN_ERR_ASSERT(repos_relpath == NULL);
  SVN_ERR_ASSERT(repos_root_url == NULL);
  SVN_ERR_ASSERT(repos_uuid == NULL);
  SVN_ERR_ASSERT(original_repos_relpath == NULL);
  SVN_ERR_ASSERT(original_root_url == NULL);
  SVN_ERR_ASSERT(original_uuid == NULL);
  SVN_ERR_ASSERT(original_revision == SVN_INVALID_REVNUM);
  SVN_ERR_ASSERT(moved_to_abspath == NULL);

  /* Root of delete. Parent is a BASE node. */
  SVN_ERR(svn_wc__db_scan_working(
            &status, &op_root_abspath,
            &repos_relpath, &repos_root_url, &repos_uuid,
            &original_repos_relpath, &original_root_url, &original_uuid,
            &original_revision, &moved_to_abspath,
            db, svn_dirent_join(local_abspath, "K", pool),
            pool, pool));
  SVN_ERR_ASSERT(status == svn_wc__db_status_deleted);
  SVN_ERR_ASSERT(strcmp(op_root_abspath,
                        svn_dirent_join(local_abspath, "K", pool)) == 0);
  SVN_ERR_ASSERT(repos_relpath == NULL);
  SVN_ERR_ASSERT(repos_root_url == NULL);
  SVN_ERR_ASSERT(repos_uuid == NULL);
  SVN_ERR_ASSERT(original_repos_relpath == NULL);
  SVN_ERR_ASSERT(original_root_url == NULL);
  SVN_ERR_ASSERT(original_uuid == NULL);
  SVN_ERR_ASSERT(original_revision == SVN_INVALID_REVNUM);
  SVN_ERR_ASSERT(moved_to_abspath == NULL);

  return SVN_NO_ERROR;
}


struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS(test_getting_info),
    SVN_TEST_PASS(test_inserting_nodes),
    SVN_TEST_PASS(test_base_children),
    SVN_TEST_PASS(test_working_info),
    SVN_TEST_PASS(test_pdh),
    SVN_TEST_PASS(test_scan_working),
    SVN_TEST_NULL
  };
