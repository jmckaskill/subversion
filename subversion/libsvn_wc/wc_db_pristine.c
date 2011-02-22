/*
 * wc_db_pristine.c :  Pristine ("text base") management
 *
 * See the spec in 'notes/wc-ng/pristine-store'.
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

#define SVN_WC__I_AM_WC_DB

#include "svn_dirent_uri.h"

#include "wc.h"
#include "wc_db.h"
#include "wc-queries.h"
#include "wc_db_private.h"

#define PRISTINE_STORAGE_RELPATH "pristine"
#define PRISTINE_TEMPDIR_RELPATH ""



/* Returns in PRISTINE_ABSPATH a new string allocated from RESULT_POOL,
   holding the local absolute path to the file location that is dedicated
   to hold CHECKSUM's pristine file, relating to the pristine store
   configured for the working copy indicated by PDH. The returned path
   does not necessarily currently exist.

   Iff CREATE_SUBDIR is TRUE, then this function will make sure that the
   parent directory of PRISTINE_ABSPATH exists. This is only useful when
   about to create a new pristine.

   Any other allocations are made in SCRATCH_POOL. */
static svn_error_t *
get_pristine_fname(const char **pristine_abspath,
                   const char *wcroot_abspath,
                   const svn_checksum_t *sha1_checksum,
                   svn_boolean_t create_subdir,
                   apr_pool_t *result_pool,
                   apr_pool_t *scratch_pool)
{
  const char *base_dir_abspath;
  const char *hexdigest = svn_checksum_to_cstring(sha1_checksum, scratch_pool);
  char subdir[3];

  /* ### code is in transition. make sure we have the proper data.  */
  SVN_ERR_ASSERT(pristine_abspath != NULL);
  SVN_ERR_ASSERT(svn_dirent_is_absolute(wcroot_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  /* ### need to fix this to use a symbol for ".svn". we don't need
     ### to use join_many since we know "/" is the separator for
     ### internal canonical paths */
  base_dir_abspath = svn_dirent_join_many(scratch_pool,
                                          wcroot_abspath,
                                          svn_wc_get_adm_dir(scratch_pool),
                                          PRISTINE_STORAGE_RELPATH,
                                          NULL);

  /* We should have a valid checksum and (thus) a valid digest. */
  SVN_ERR_ASSERT(hexdigest != NULL);

  /* Get the first two characters of the digest, for the subdir. */
  subdir[0] = hexdigest[0];
  subdir[1] = hexdigest[1];
  subdir[2] = '\0';

  if (create_subdir)
    {
      const char *subdir_abspath = svn_dirent_join(base_dir_abspath, subdir,
                                                   scratch_pool);
      svn_error_t *err;

      err = svn_io_dir_make(subdir_abspath, APR_OS_DEFAULT, scratch_pool);

      /* Whatever error may have occurred... ignore it. Typically, this
         will be "directory already exists", but if it is something
         *different*, then presumably another error will follow when we
         try to access the file within this (missing?) pristine subdir. */
      svn_error_clear(err);
    }

  /* The file is located at DIR/.svn/pristine/XX/XXYYZZ... */
  *pristine_abspath = svn_dirent_join_many(result_pool,
                                           base_dir_abspath,
                                           subdir,
                                           hexdigest,
                                           NULL);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_get_path(const char **pristine_abspath,
                             svn_wc__db_t *db,
                             const char *wri_abspath,
                             const svn_checksum_t *sha1_checksum,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_pdh_t *pdh;
  const char *local_relpath;
  svn_boolean_t present;

  SVN_ERR_ASSERT(pristine_abspath != NULL);
  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  /* ### Transitional: accept MD-5 and look up the SHA-1.  Return an error
   * if the pristine text is not in the store. */
  if (sha1_checksum->kind != svn_checksum_sha1)
    SVN_ERR(svn_wc__db_pristine_get_sha1(&sha1_checksum, db, wri_abspath,
                                         sha1_checksum,
                                         scratch_pool, scratch_pool));
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  SVN_ERR(svn_wc__db_pdh_parse_local_abspath(&pdh, &local_relpath,
                                             db, wri_abspath,
                                             svn_sqlite__mode_readonly,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(pdh->wcroot);

  SVN_ERR(svn_wc__db_pristine_check(&present, db, wri_abspath, sha1_checksum,
                                    scratch_pool));
  if (! present)
    return svn_error_createf(SVN_ERR_WC_DB_ERROR, NULL,
                             _("Pristine text not found"));

  SVN_ERR(get_pristine_fname(pristine_abspath, pdh->wcroot->abspath,
                             sha1_checksum,
                             FALSE /* create_subdir */,
                             result_pool, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_pristine_get_future_path(const char **pristine_abspath,
                                    const char *wcroot_abspath,
                                    svn_checksum_t *sha1_checksum,
                                    apr_pool_t *result_pool,
                                    apr_pool_t *scratch_pool)
{
  SVN_ERR(get_pristine_fname(pristine_abspath, wcroot_abspath,
                             sha1_checksum,
                             FALSE /* create_subdir */,
                             result_pool, scratch_pool));
  return SVN_NO_ERROR;
}

/* Data for pristine_read_txn(). */
typedef struct pristine_read_baton_t
{
  /* Where to put the result stream. */
  svn_stream_t **contents;
  /* The pristine text's SHA-1 checksum. */
  const svn_checksum_t *sha1_checksum;
  /* The path to the pristine file (within the pristine store). */
  const char *pristine_abspath;
  /* The pool from which to allocate the result stream. */
  apr_pool_t *result_pool;
} pristine_read_baton_t;

/* Set (*BATON->contents) to a readable stream from which the pristine text
 * identified by BATON->sha1_checksum can be read from the pristine store of
 * SDB.  If that text is not in the pristine store, return an error.
 *
 * Allocate the stream in BATON->result_pool.
 *
 * This function expects to be executed inside a SQLite txn.
 *
 * Implements 'notes/wc-ng/pristine-store' section A-3(d).
 * Implements svn_sqlite__transaction_callback_t. */
static svn_error_t *
pristine_read_txn(void *baton,
                  svn_sqlite__db_t *sdb,
                  apr_pool_t *scratch_pool)
{
  pristine_read_baton_t *b = baton;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  /* Check that this pristine text is present in the store.  (The presence
   * of the file is not sufficient.) */
  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_SELECT_PRISTINE));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, b->sha1_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));
  if (! have_row)
    {
      return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                               _("Pristine text '%s' not present"),
                               svn_checksum_to_cstring_display(
                                 b->sha1_checksum, scratch_pool));
    }

  SVN_ERR(svn_stream_open_readonly(b->contents, b->pristine_abspath,
                                   b->result_pool, scratch_pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_pristine_read(svn_stream_t **contents,
                         svn_wc__db_t *db,
                         const char *wri_abspath,
                         const svn_checksum_t *sha1_checksum,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  svn_wc__db_pdh_t *pdh;
  const char *local_relpath;
  pristine_read_baton_t b;

  SVN_ERR_ASSERT(contents != NULL);
  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  /* ### Transitional: accept MD-5 and look up the SHA-1.  Return an error
   * if the pristine text is not in the store. */
  if (sha1_checksum->kind != svn_checksum_sha1)
    SVN_ERR(svn_wc__db_pristine_get_sha1(&sha1_checksum, db, wri_abspath,
                                         sha1_checksum,
                                         scratch_pool, scratch_pool));
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  SVN_ERR(svn_wc__db_pdh_parse_local_abspath(&pdh, &local_relpath, db,
                              wri_abspath, svn_sqlite__mode_readonly,
                              scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(pdh->wcroot);

  b.contents = contents;
  b.sha1_checksum = sha1_checksum;
  b.result_pool = result_pool;
  SVN_ERR(get_pristine_fname(&b.pristine_abspath, pdh->wcroot->abspath,
                             sha1_checksum,
                             FALSE /* create_subdir */,
                             scratch_pool, scratch_pool));
  SVN_ERR(svn_sqlite__with_transaction(pdh->wcroot->sdb,
                                       pristine_read_txn, &b,
                                       scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_get_tempdir(const char **temp_dir_abspath,
                                svn_wc__db_t *db,
                                const char *wri_abspath,
                                apr_pool_t *result_pool,
                                apr_pool_t *scratch_pool)
{
  svn_wc__db_pdh_t *pdh;
  const char *local_relpath;

  SVN_ERR_ASSERT(temp_dir_abspath != NULL);
  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));

  SVN_ERR(svn_wc__db_pdh_parse_local_abspath(&pdh, &local_relpath, db,
                              wri_abspath, svn_sqlite__mode_readonly,
                              scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(pdh->wcroot);

  *temp_dir_abspath = svn_dirent_join_many(result_pool,
                                           pdh->wcroot->abspath,
                                           svn_wc_get_adm_dir(scratch_pool),
                                           PRISTINE_TEMPDIR_RELPATH,
                                           NULL);
  return SVN_NO_ERROR;
}


/* Data for pristine_install_txn(). */
typedef struct pristine_install_baton_t
{
  /* The path to the source file that is to be moved into place. */
  const char *tempfile_abspath;
  /* The target path for the file (within the pristine store). */
  const char *pristine_abspath;
  /* The pristine text's SHA-1 checksum. */
  const svn_checksum_t *sha1_checksum;
  /* The pristine text's MD-5 checksum. */
  const svn_checksum_t *md5_checksum;
} pristine_install_baton_t;


/* Install the pristine text described by BATON into the pristine store of
 * SDB.  If it is already stored then just delete the new file
 * BATON->tempfile_abspath.
 *
 * This function expects to be executed inside a SQLite txn that has already
 * acquired a 'RESERVED' lock.
 * 
 * Implements 'notes/wc-ng/pristine-store' section A-3(a).
 * Implements svn_sqlite__transaction_callback_t. */
static svn_error_t *
pristine_install_txn(void *baton,
                     svn_sqlite__db_t *sdb,
                     apr_pool_t *scratch_pool)
{
  pristine_install_baton_t *b = baton;
  apr_finfo_t finfo;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  svn_node_kind_t kind;

  /* If this pristine text is already present in the store, just keep it:
   * delete the new one and return. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_SELECT_PRISTINE));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, b->sha1_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));
  if (have_row)
    {
      /* Remove the temp file: it's already there */
      /* ### TODO: Maybe verify the new file matches the existing one. */
      SVN_ERR(svn_io_remove_file2(b->tempfile_abspath, FALSE, scratch_pool));
      return SVN_NO_ERROR;
    }

  /* Move the file to its target location, or discard it if already there. */
  SVN_ERR(svn_io_check_path(b->pristine_abspath, &kind, scratch_pool));
  if (kind == svn_node_file)
    {
      /* Remove the temp file: it's already there */
      /* ### TODO: Maybe verify the new file matches the existing one. */
      SVN_ERR(svn_io_remove_file2(b->tempfile_abspath, FALSE, scratch_pool));
    }
  else
    {
      SVN_ERR(svn_io_file_rename(b->tempfile_abspath, b->pristine_abspath,
                                 scratch_pool));
    }

  SVN_ERR(svn_io_stat(&finfo, b->pristine_abspath, APR_FINFO_SIZE,
                      scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                    STMT_INSERT_PRISTINE));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, b->sha1_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 2, b->md5_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__bind_int64(stmt, 3, finfo.size));
  SVN_ERR(svn_sqlite__insert(NULL, stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_install(svn_wc__db_t *db,
                            const char *tempfile_abspath,
                            const svn_checksum_t *sha1_checksum,
                            const svn_checksum_t *md5_checksum,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_pdh_t *pdh;
  const char *local_relpath;
  const char *wri_abspath;
  struct pristine_install_baton_t b;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(tempfile_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);
  SVN_ERR_ASSERT(md5_checksum != NULL);
  SVN_ERR_ASSERT(md5_checksum->kind == svn_checksum_md5);

  /* ### this logic assumes that TEMPFILE_ABSPATH follows this pattern:
     ###   WCROOT_ABSPATH/COMPONENT/TEMPFNAME
     ### if we change this (see PRISTINE_TEMPDIR_RELPATH), then this
     ### logic should change.  */
  wri_abspath = svn_dirent_dirname(svn_dirent_dirname(tempfile_abspath,
                                                      scratch_pool),
                                   scratch_pool);

  SVN_ERR(svn_wc__db_pdh_parse_local_abspath(&pdh, &local_relpath, db,
                              wri_abspath, svn_sqlite__mode_readonly,
                              scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(pdh->wcroot);

  b.tempfile_abspath = tempfile_abspath;
  b.sha1_checksum = sha1_checksum;
  b.md5_checksum = md5_checksum;

  SVN_ERR(get_pristine_fname(&b.pristine_abspath, pdh->wcroot->abspath,
                             sha1_checksum,
                             TRUE /* create_subdir */,
                             scratch_pool, scratch_pool));

  /* Ensure the SQL txn has at least a 'RESERVED' lock before we start looking
   * at the disk, to ensure no concurrent pristine install/delete txn. */
  SVN_ERR(svn_sqlite__with_immediate_transaction(pdh->wcroot->sdb,
                                                 pristine_install_txn, &b,
                                                 scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_get_md5(const svn_checksum_t **md5_checksum,
                            svn_wc__db_t *db,
                            const char *wri_abspath,
                            const svn_checksum_t *sha1_checksum,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_pdh_t *pdh;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  SVN_ERR(svn_wc__db_pdh_parse_local_abspath(&pdh, &local_relpath, db,
                              wri_abspath, svn_sqlite__mode_readonly,
                              scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(pdh->wcroot);

  SVN_ERR(svn_sqlite__get_statement(&stmt, pdh->wcroot->sdb,
                                    STMT_SELECT_PRISTINE));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, sha1_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    return svn_error_createf(SVN_ERR_WC_DB_ERROR, svn_sqlite__reset(stmt),
                             _("The pristine text with checksum '%s' was "
                               "not found"),
                             svn_checksum_to_cstring_display(sha1_checksum,
                                                             scratch_pool));

  SVN_ERR(svn_sqlite__column_checksum(md5_checksum, stmt, 0, result_pool));
  SVN_ERR_ASSERT((*md5_checksum)->kind == svn_checksum_md5);

  return svn_error_return(svn_sqlite__reset(stmt));
}


svn_error_t *
svn_wc__db_pristine_get_sha1(const svn_checksum_t **sha1_checksum,
                             svn_wc__db_t *db,
                             const char *wri_abspath,
                             const svn_checksum_t *md5_checksum,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_pdh_t *pdh;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  SVN_ERR_ASSERT(md5_checksum->kind == svn_checksum_md5);

  SVN_ERR(svn_wc__db_pdh_parse_local_abspath(&pdh, &local_relpath, db,
                              wri_abspath, svn_sqlite__mode_readonly,
                              scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(pdh->wcroot);

  SVN_ERR(svn_sqlite__get_statement(&stmt, pdh->wcroot->sdb,
                                    STMT_SELECT_PRISTINE_BY_MD5));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, md5_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    return svn_error_createf(SVN_ERR_WC_DB_ERROR, svn_sqlite__reset(stmt),
                             _("The pristine text with MD5 checksum '%s' was "
                               "not found"),
                             svn_checksum_to_cstring_display(md5_checksum,
                                                             scratch_pool));

  SVN_ERR(svn_sqlite__column_checksum(sha1_checksum, stmt, 0, result_pool));
  SVN_ERR_ASSERT((*sha1_checksum)->kind == svn_checksum_sha1);

  return svn_error_return(svn_sqlite__reset(stmt));
}


/* Data for pristine_remove_if_unreferenced_txn(). */
typedef struct pristine_remove_baton_t
{
  /* The pristine text's SHA-1 checksum. */
  const svn_checksum_t *sha1_checksum;
  /* The path to the pristine file (within the pristine store). */
  const char *pristine_abspath;
} pristine_remove_baton_t;

/* If the pristine text referenced by BATON in SDB has a reference count of
 * zero, delete it (both the database row and the disk file).
 *
 * This function expects to be executed inside a SQLite txn that has already
 * acquired a 'RESERVED' lock.
 *
 * Implements svn_sqlite__transaction_callback_t. */
static svn_error_t *
pristine_remove_if_unreferenced_txn(void *baton,
                                    svn_sqlite__db_t *sdb,
                                    apr_pool_t *scratch_pool)
{
  pristine_remove_baton_t *b = baton;
  svn_sqlite__stmt_t *stmt;
  int affected_rows;

  /* Remove the DB row, if refcount is 0. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                    STMT_DELETE_PRISTINE_IF_UNREFERENCED));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, b->sha1_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__update(&affected_rows, stmt));

  /* If we removed the DB row, then remove the file. */
  if (affected_rows > 0)
    {
      /* ### TODO: If file not present, log a consistency error but return
       * success. */
      SVN_ERR(svn_io_remove_file2(b->pristine_abspath, FALSE /* ignore_enoent */,
                                  scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* If the pristine text referenced by SHA1_CHECKSUM in WCROOT has a
 * reference count of zero, delete it (both the database row and the disk
 * file).
 *
 * Implements 'notes/wc-ng/pristine-store' section A-3(b). */
static svn_error_t *
pristine_remove_if_unreferenced(svn_wc__db_wcroot_t *wcroot,
                                const svn_checksum_t *sha1_checksum,
                                apr_pool_t *scratch_pool)
{
  pristine_remove_baton_t b;

  b.sha1_checksum = sha1_checksum;
  SVN_ERR(get_pristine_fname(&b.pristine_abspath, wcroot->abspath,
                             sha1_checksum, FALSE /* create_subdir */,
                             scratch_pool, scratch_pool));

  /* Ensure the SQL txn has at least a 'RESERVED' lock before we start looking
   * at the disk, to ensure no concurrent pristine install/delete txn. */
  SVN_ERR(svn_sqlite__with_immediate_transaction(
    wcroot->sdb, pristine_remove_if_unreferenced_txn, &b, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_pristine_remove(svn_wc__db_t *db,
                           const char *wri_abspath,
                           const svn_checksum_t *sha1_checksum,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_pdh_t *pdh;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  /* ### Transitional: accept MD-5 and look up the SHA-1.  Return an error
   * if the pristine text is not in the store. */
  if (sha1_checksum->kind != svn_checksum_sha1)
    SVN_ERR(svn_wc__db_pristine_get_sha1(&sha1_checksum, db, wri_abspath,
                                         sha1_checksum,
                                         scratch_pool, scratch_pool));
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  SVN_ERR(svn_wc__db_pdh_parse_local_abspath(&pdh, &local_relpath, db,
                              wri_abspath, svn_sqlite__mode_readwrite,
                              scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(pdh->wcroot);

  /* If the work queue is not empty, don't delete any pristine text because
   * the work queue may contain a reference to it. */
  {
    svn_sqlite__stmt_t *stmt;
    svn_boolean_t have_row;

    SVN_ERR(svn_sqlite__get_statement(&stmt, pdh->wcroot->sdb,
                                      STMT_LOOK_FOR_WORK));
    SVN_ERR(svn_sqlite__step(&have_row, stmt));
    SVN_ERR(svn_sqlite__reset(stmt));

    if (have_row)
      return SVN_NO_ERROR;
  }

  /* If not referenced, remove the PRISTINE table row and the file. */
  SVN_ERR(pristine_remove_if_unreferenced(pdh->wcroot, sha1_checksum,
                                          scratch_pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
pristine_cleanup_wcroot(svn_wc__db_wcroot_t *wcroot,
                        apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  /* Find each unreferenced pristine in the DB and remove it. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_UNREFERENCED_PRISTINES));
  while (1)
    {
      svn_boolean_t have_row;
      const svn_checksum_t *sha1_checksum;

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
      if (! have_row)
        break;

      SVN_ERR(svn_sqlite__column_checksum(&sha1_checksum, stmt, 0,
                                          scratch_pool));
      SVN_ERR(pristine_remove_if_unreferenced(wcroot, sha1_checksum,
                                              scratch_pool));
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_cleanup(svn_wc__db_t *db,
                            const char *wri_abspath,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_pdh_t *pdh;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));

  SVN_ERR(svn_wc__db_pdh_parse_local_abspath(&pdh, &local_relpath, db,
                              wri_abspath, svn_sqlite__mode_readonly,
                              scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(pdh->wcroot);

  SVN_ERR(pristine_cleanup_wcroot(pdh->wcroot, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_pristine_check(svn_boolean_t *present,
                          svn_wc__db_t *db,
                          const char *wri_abspath,
                          const svn_checksum_t *sha1_checksum,
                          apr_pool_t *scratch_pool)
{
  svn_wc__db_pdh_t *pdh;
  const char *local_relpath;
  const char *pristine_abspath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(present != NULL);
  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(sha1_checksum != NULL);
  /* ### Transitional: accept MD-5 and look up the SHA-1.  Return an error
   * if the pristine text is not in the store. */
  if (sha1_checksum->kind != svn_checksum_sha1)
    SVN_ERR(svn_wc__db_pristine_get_sha1(&sha1_checksum, db, wri_abspath,
                                         sha1_checksum,
                                         scratch_pool, scratch_pool));
  SVN_ERR_ASSERT(sha1_checksum->kind == svn_checksum_sha1);

  SVN_ERR(svn_wc__db_pdh_parse_local_abspath(&pdh, &local_relpath, db,
                              wri_abspath, svn_sqlite__mode_readonly,
                              scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(pdh->wcroot);

  /* Check that there is an entry in the PRISTINE table. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, pdh->wcroot->sdb,
                                    STMT_SELECT_PRISTINE));
  SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, sha1_checksum, scratch_pool));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));

#ifdef SVN_DEBUG
  /* Check that the pristine text file exists iff the DB says it does. */
  if (have_row)
    {
      svn_node_kind_t kind_on_disk;
      SVN_ERR(get_pristine_fname(&pristine_abspath, pdh->wcroot->abspath,
                                 sha1_checksum, FALSE /* create_subdir */,
                                 scratch_pool, scratch_pool));
      SVN_ERR(svn_io_check_path(pristine_abspath, &kind_on_disk, scratch_pool));

      if (kind_on_disk != svn_node_file)
        return svn_error_createf(SVN_ERR_WC_DB_ERROR, svn_sqlite__reset(stmt),
                                 _("The pristine text with checksum '%s' was "
                                   "found in the DB but not on disk"),
                                 svn_checksum_to_cstring_display(sha1_checksum,
                                                                 scratch_pool));
    }
#endif

  *present = have_row;
  return SVN_NO_ERROR;
}
