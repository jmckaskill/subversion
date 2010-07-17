/*
 * wc_db_pdh.c :  supporting datastructures for the administrative database
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

#include <assert.h>

#include "svn_dirent_uri.h"

#include "wc.h"
#include "adm_files.h"
#include "wc_db_private.h"
#include "wc-queries.h"

#include "svn_private_config.h"

/* ### Same values as wc_db.c */
#define SDB_FILE  "wc.db"
#define UNKNOWN_WC_ID ((apr_int64_t) -1)
#define FORMAT_FROM_SDB (-1)




/* Get the format version from a wc-1 directory. If it is not a working copy
   directory, then it sets VERSION to zero and returns no error.  */
static svn_error_t *
get_old_version(int *version,
                const char *abspath,
                apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  const char *format_file_path;

  /* Try reading the format number from the entries file.  */
  format_file_path = svn_wc__adm_child(abspath, SVN_WC__ADM_ENTRIES,
                                       scratch_pool);
  err = svn_io_read_version_file(version, format_file_path, scratch_pool);
  if (err == NULL)
    return SVN_NO_ERROR;
  if (err->apr_err != SVN_ERR_BAD_VERSION_FILE_FORMAT
      && !APR_STATUS_IS_ENOENT(err->apr_err)
      && !APR_STATUS_IS_ENOTDIR(err->apr_err))
    return svn_error_createf(SVN_ERR_WC_MISSING, err, _("'%s' does not exist"),
                             svn_dirent_local_style(abspath, scratch_pool));
  svn_error_clear(err);

  /* This must be a really old working copy!  Fall back to reading the
     format file.

     Note that the format file might not exist in newer working copies
     (format 7 and higher), but in that case, the entries file should
     have contained the format number. */
  format_file_path = svn_wc__adm_child(abspath, SVN_WC__ADM_FORMAT,
                                       scratch_pool);
  err = svn_io_read_version_file(version, format_file_path, scratch_pool);
  if (err == NULL)
    return SVN_NO_ERROR;

  /* Whatever error may have occurred... we can just ignore. This is not
     a working copy directory. Signal the caller.  */
  svn_error_clear(err);

  *version = 0;
  return SVN_NO_ERROR;
}

#ifndef SVN_WC__SINGLE_DB
/* The filesystem has a directory at LOCAL_RELPATH. Examine the metadata
   to determine if a *file* was supposed to be there.

   ### this function is only required for per-dir .svn support. once all
   ### metadata is collected in a single wcroot, then we won't need to
   ### look in subdirs for other metadata.  */
static svn_error_t *
determine_obstructed_file(svn_boolean_t *obstructed_file,
                          const svn_wc__db_wcroot_t *wcroot,
                          const char *local_relpath,
                          apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(wcroot->sdb != NULL && wcroot->wc_id != UNKNOWN_WC_ID);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_WORKING_IS_FILE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is",
                            wcroot->wc_id,
                            local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (have_row)
    {
      *obstructed_file = svn_sqlite__column_boolean(stmt, 0);
    }
  else
    {
      SVN_ERR(svn_sqlite__reset(stmt));

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_SELECT_BASE_IS_FILE));
      SVN_ERR(svn_sqlite__bindf(stmt, "is",
                                wcroot->wc_id,
                                local_relpath));
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
      if (have_row)
        *obstructed_file = svn_sqlite__column_boolean(stmt, 0);
    }

  return svn_sqlite__reset(stmt);
}
#endif


/* */
static svn_error_t *
verify_no_work(svn_sqlite__db_t *sdb)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_LOOK_FOR_WORK));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));

  if (have_row)
    return svn_error_create(SVN_ERR_WC_CLEANUP_REQUIRED, NULL,
                            NULL /* nothing to add.  */);

  return SVN_NO_ERROR;
}


/* */
static apr_status_t
close_wcroot(void *data)
{
  svn_wc__db_wcroot_t *wcroot = data;
  svn_error_t *err;

  SVN_ERR_ASSERT_NO_RETURN(wcroot->sdb != NULL);

  err = svn_sqlite__close(wcroot->sdb);
  wcroot->sdb = NULL;
  if (err)
    {
      apr_status_t result = err->apr_err;
      svn_error_clear(err);
      return result;
    }

  return APR_SUCCESS;
}


svn_wc__db_pdh_t *
svn_wc__db_pdh_get_or_create(svn_wc__db_t *db,
                             const char *local_dir_abspath,
                             svn_boolean_t create_allowed,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_pdh_t *pdh = apr_hash_get(db->dir_data,
                                       local_dir_abspath, APR_HASH_KEY_STRING);

  if (pdh == NULL && create_allowed)
    {
      pdh = apr_pcalloc(db->state_pool, sizeof(*pdh));

      /* Copy the path for the proper lifetime.  */
      pdh->local_abspath = apr_pstrdup(db->state_pool, local_dir_abspath);

      /* We don't know anything about this directory, so we cannot construct
         a svn_wc__db_wcroot_t for it (yet).  */

      /* ### parent */

      apr_hash_set(db->dir_data, pdh->local_abspath, APR_HASH_KEY_STRING, pdh);
    }

  return pdh;
}


svn_error_t *
svn_wc__db_open(svn_wc__db_t **db,
                svn_wc__db_openmode_t mode,
                svn_config_t *config,
                svn_boolean_t auto_upgrade,
                svn_boolean_t enforce_empty_wq,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  *db = apr_pcalloc(result_pool, sizeof(**db));
  (*db)->mode = mode;
  (*db)->config = config;
  (*db)->auto_upgrade = auto_upgrade;
  (*db)->enforce_empty_wq = enforce_empty_wq;
  (*db)->dir_data = apr_hash_make(result_pool);
  (*db)->state_pool = result_pool;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_close(svn_wc__db_t *db)
{
  apr_pool_t *scratch_pool = db->state_pool;
  apr_hash_t *roots = apr_hash_make(scratch_pool);
  apr_hash_index_t *hi;

  /* Collect all the unique WCROOT structures, and empty out DIR_DATA.  */
  for (hi = apr_hash_first(scratch_pool, db->dir_data);
       hi;
       hi = apr_hash_next(hi))
    {
      const void *key;
      apr_ssize_t klen;
      void *val;
      svn_wc__db_pdh_t *pdh;

      apr_hash_this(hi, &key, &klen, &val);
      pdh = val;

      if (pdh->wcroot && pdh->wcroot->sdb)
        apr_hash_set(roots, pdh->wcroot->abspath, APR_HASH_KEY_STRING,
                     pdh->wcroot);

      apr_hash_set(db->dir_data, key, klen, NULL);
    }

  /* Run the cleanup for each WCROOT.  */
  return svn_error_return(svn_wc__db_close_many_wcroots(roots, db->state_pool,
                                                        scratch_pool));
}


svn_error_t *
svn_wc__db_pdh_create_wcroot(svn_wc__db_wcroot_t **wcroot,
                             const char *wcroot_abspath,
                             svn_sqlite__db_t *sdb,
                             apr_int64_t wc_id,
                             int format,
                             svn_boolean_t auto_upgrade,
                             svn_boolean_t enforce_empty_wq,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  if (sdb != NULL)
    SVN_ERR(svn_sqlite__read_schema_version(&format, sdb, scratch_pool));

  /* If we construct a wcroot, then we better have a format.  */
  SVN_ERR_ASSERT(format >= 1);

  /* If this working copy is PRE-1.0, then simply bail out.  */
  if (format < 4)
    {
      return svn_error_createf(
        SVN_ERR_WC_UNSUPPORTED_FORMAT, NULL,
        _("Working copy format of '%s' is too old (%d); "
          "please check out your working copy again"),
        svn_dirent_local_style(wcroot_abspath, scratch_pool), format);
    }

  /* If this working copy is from a future version, then bail out.  */
  if (format > SVN_WC__VERSION)
    {
      return svn_error_createf(
        SVN_ERR_WC_UNSUPPORTED_FORMAT, NULL,
        _("This client is too old to work with the working copy at\n"
          "'%s' (format %d).\n"
          "You need to get a newer Subversion client. For more details, see\n"
          "  http://subversion.apache.org/faq.html#working-copy-format-change\n"
          ),
        svn_dirent_local_style(wcroot_abspath, scratch_pool),
        format);
    }

  /* Verify that no work items exists. If they do, then our integrity is
     suspect and, thus, we cannot use this database.  */
  if (format >= SVN_WC__HAS_WORK_QUEUE
      && (enforce_empty_wq || (format < SVN_WC__VERSION && auto_upgrade)))
    {
      svn_error_t *err = verify_no_work(sdb);
      if (err)
        {
          /* Special message for attempts to upgrade a 1.7-dev wc with
             outstanding workqueue items. */
          if (err->apr_err == SVN_ERR_WC_CLEANUP_REQUIRED
              && format < SVN_WC__VERSION && auto_upgrade)
            err = svn_error_quick_wrap(err, _("Cleanup with an older 1.7 "
                                              "client before upgrading with "
                                              "this client"));
          return svn_error_return(err);
        }
    }

  /* Auto-upgrade the SDB if possible.  */
  if (format < SVN_WC__VERSION && auto_upgrade)
    SVN_ERR(svn_wc__upgrade_sdb(&format, wcroot_abspath, sdb, format,
                                scratch_pool));

  *wcroot = apr_palloc(result_pool, sizeof(**wcroot));

  (*wcroot)->abspath = wcroot_abspath;
  (*wcroot)->sdb = sdb;
  (*wcroot)->wc_id = wc_id;
  (*wcroot)->format = format;
#ifdef SVN_WC__SINGLE_DB
  /* 8 concurrent locks is probably more than a typical wc_ng based svn client
     uses. */
  (*wcroot)->owned_locks = apr_array_make(result_pool, 8,
                                          sizeof(svn_wc__db_wclock_t));
#endif

  /* SDB will be NULL for pre-NG working copies. We only need to run a
     cleanup when the SDB is present.  */
  if (sdb != NULL)
    apr_pool_cleanup_register(result_pool, *wcroot, close_wcroot,
                              apr_pool_cleanup_null);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_close_many_wcroots(apr_hash_t *roots,
                              apr_pool_t *state_pool,
                              apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;

  for (hi = apr_hash_first(scratch_pool, roots); hi; hi = apr_hash_next(hi))
    {
      svn_wc__db_wcroot_t *wcroot = svn__apr_hash_index_val(hi);
      apr_status_t result;

      result = apr_pool_cleanup_run(state_pool, wcroot, close_wcroot);
      if (result != APR_SUCCESS)
        return svn_error_wrap_apr(result, NULL);
    }

  return SVN_NO_ERROR;
}


/* POOL may be NULL if the lifetime of LOCAL_ABSPATH is sufficient.  */
const char *
svn_wc__db_pdh_compute_relpath(const svn_wc__db_pdh_t *pdh,
                               apr_pool_t *result_pool)
{
  const char *relpath = svn_dirent_is_child(pdh->wcroot->abspath,
                                            pdh->local_abspath,
                                            result_pool);
  if (relpath == NULL)
    return "";
  return relpath;
}


svn_error_t *
svn_wc__db_pdh_parse_local_abspath(svn_wc__db_pdh_t **pdh,
                                   const char **local_relpath,
                                   svn_wc__db_t *db,
                                   const char *local_abspath,
                                   svn_sqlite__mode_t smode,
                                   apr_pool_t *result_pool,
                                   apr_pool_t *scratch_pool)
{
  const char *original_abspath = local_abspath;
  svn_node_kind_t kind;
  svn_boolean_t special;
  const char *build_relpath;
  svn_wc__db_pdh_t *found_pdh = NULL;
  svn_wc__db_pdh_t *child_pdh;
  svn_boolean_t obstruction_possible = FALSE;
  svn_sqlite__db_t *sdb;
  svn_boolean_t moved_upwards = FALSE;
  svn_boolean_t always_check = FALSE;
  int wc_format = 0;

  /* ### we need more logic for finding the database (if it is located
     ### outside of the wcroot) and then managing all of that within DB.
     ### for now: play quick & dirty. */

  /* ### for now, overwrite the provided mode.  We currently cache the
     ### sdb handles, which is great but for the occasion where we
     ### initially open the sdb in readonly mode and then later want
     ### to write to it.  The solution is to reopen the db in readwrite
     ### mode, but that assumes we can track the fact that it was
     ### originally opened readonly.  So for now, just punt and open
     ### everything in readwrite mode.  */
  smode = svn_sqlite__mode_readwrite;

  *pdh = apr_hash_get(db->dir_data, local_abspath, APR_HASH_KEY_STRING);
  if (*pdh != NULL && (*pdh)->wcroot != NULL)
    {
      /* We got lucky. Just return the thing BEFORE performing any I/O.  */
      /* ### validate SMODE against how we opened wcroot->sdb? and against
         ### DB->mode? (will we record per-dir mode?)  */

      /* ### for most callers, we could pass NULL for result_pool.  */
      *local_relpath = svn_wc__db_pdh_compute_relpath(*pdh, result_pool);

      return SVN_NO_ERROR;
    }

  /* ### at some point in the future, we may need to find a way to get
     ### rid of this stat() call. it is going to happen for EVERY call
     ### into wc_db which references a file. calls for directories could
     ### get an early-exit in the hash lookup just above.  */
  SVN_ERR(svn_io_check_special_path(local_abspath, &kind,
                                    &special /* unused */, scratch_pool));
  if (kind != svn_node_dir)
    {
      /* If the node specified by the path is NOT present, then it cannot
         possibly be a directory containing ".svn/wc.db".

         If it is a file, then it cannot contain ".svn/wc.db".

         For both of these cases, strip the basename off of the path and
         move up one level. Keep record of what we strip, though, since
         we'll need it later to construct local_relpath.  */
      svn_dirent_split(&local_abspath, &build_relpath, local_abspath,
                       scratch_pool);

      /* ### if *pdh != NULL (from further above), then there is (quite
         ### probably) a bogus value in the DIR_DATA hash table. maybe
         ### clear it out? but what if there is an access baton?  */

      /* Is this directory in our hash?  */
      *pdh = apr_hash_get(db->dir_data, local_abspath, APR_HASH_KEY_STRING);
      if (*pdh != NULL && (*pdh)->wcroot != NULL)
        {
          const char *dir_relpath;

          /* Stashed directory's local_relpath + basename. */
          dir_relpath = svn_wc__db_pdh_compute_relpath(*pdh, NULL);
          *local_relpath = svn_relpath_join(dir_relpath,
                                            build_relpath,
                                            result_pool);
          return SVN_NO_ERROR;
        }

      /* If the requested path is not on the disk, then we don't know how
         many ancestors need to be scanned until we start hitting content
         on the disk. Set ALWAYS_CHECK to keep looking for .svn/entries
         rather than bailing out after the first check.  */
      if (kind == svn_node_none)
        always_check = TRUE;
    }
  else
    {
      /* Start the local_relpath empty. If *this* directory contains the
         wc.db, then relpath will be the empty string.  */
      build_relpath = "";

      /* It is possible that LOCAL_ABSPATH was *intended* to be a file,
         but we just found a directory in its place. After we build
         the PDH, then we'll examine the parent to see how it describes
         this particular path.

         ### this is only possible with per-dir wc.db databases.  */
      obstruction_possible = TRUE;
    }

  /* LOCAL_ABSPATH refers to a directory at this point. The PDH corresponding
     to that directory is what we need to return. At this point, we've
     determined that a PDH with a discovered WCROOT is NOT in the DB's hash
     table of wcdirs. Let's fill in an existing one, or create one. Then
     go figure out where the WCROOT is.  */

  if (*pdh == NULL)
    {
      *pdh = apr_pcalloc(db->state_pool, sizeof(**pdh));
      (*pdh)->local_abspath = apr_pstrdup(db->state_pool, local_abspath);
    }
  else
    {
      /* The PDH should have been built correctly (so far).  */
      SVN_ERR_ASSERT(strcmp((*pdh)->local_abspath, local_abspath) == 0);
    }

  /* Assume that LOCAL_ABSPATH is a directory, and look for the SQLite
     database in the right place. If we find it... great! If not, then
     peel off some components, and try again. */

  while (TRUE)
    {
      svn_error_t *err;

      err = svn_wc__db_util_open_db(&sdb, local_abspath, SDB_FILE, smode,
                                    db->state_pool, scratch_pool);
      if (err == NULL)
        break;
      if (err->apr_err != SVN_ERR_SQLITE_ERROR
          && !APR_STATUS_IS_ENOENT(err->apr_err))
        return svn_error_return(err);
      svn_error_clear(err);

      /* If we have not moved upwards, then check for a wc-1 working copy.
         Since wc-1 has a .svn in every directory, and we didn't find one
         in the original directory, then we aren't looking at a wc-1.

         If the original path is not present, then we have to check on every
         iteration. The content may be the immediate parent, or possibly
         five ancetors higher. We don't test for directory presence (just
         for the presence of subdirs/files), so we don't know when we can
         stop checking ... so just check always.  */
      if (!moved_upwards || always_check)
        {
          SVN_ERR(get_old_version(&wc_format, local_abspath, scratch_pool));
          if (wc_format != 0)
            break;
        }

      /* We couldn't open the SDB within the specified directory, so
         move up one more directory. */
      if (svn_dirent_is_root(local_abspath, strlen(local_abspath)))
        {
          /* Hit the root without finding a wcroot. */
          return svn_error_createf(SVN_ERR_WC_NOT_WORKING_COPY, NULL,
                                   _("'%s' is not a working copy"),
                                   svn_dirent_local_style(original_abspath,
                                                          scratch_pool));
        }

      local_abspath = svn_dirent_dirname(local_abspath, scratch_pool);

      moved_upwards = TRUE;

      /* An obstruction is no longer possible.

         Example: we were given "/some/file" and "file" turned out to be
         a directory. We did not find an SDB at "/some/file/.svn/wc.db",
         so we are now going to look at "/some/.svn/wc.db". That SDB will
         contain the correct information for "file".

         ### obstruction is only possible with per-dir wc.db databases.  */
      obstruction_possible = FALSE;

      /* Is the parent directory recorded in our hash?  */
      found_pdh = apr_hash_get(db->dir_data,
                               local_abspath, APR_HASH_KEY_STRING);
      if (found_pdh != NULL)
        {
          if (found_pdh->wcroot != NULL)
            break;
          found_pdh = NULL;
        }
    }

  if (found_pdh != NULL)
    {
      /* We found a PDH with data in it. We can now construct the child
         from this, rather than continuing to scan upwards.  */

      /* The subdirectory uses the same WCROOT as the parent dir.  */
      (*pdh)->wcroot = found_pdh->wcroot;
    }
  else if (wc_format == 0)
    {
      /* We finally found the database. Construct the PDH record.  */

      apr_int64_t wc_id;
      svn_error_t *err;

      err = svn_wc__db_util_fetch_wc_id(&wc_id, sdb, scratch_pool);
      if (err)
        {
          if (err->apr_err == SVN_ERR_WC_CORRUPT)
            return svn_error_quick_wrap(
              err, apr_psprintf(scratch_pool,
                                _("Missing a row in WCROOT for '%s'."),
                                svn_dirent_local_style(original_abspath,
                                                       scratch_pool)));
          return svn_error_return(err);
        }

      /* WCROOT.local_abspath may be NULL when the database is stored
         inside the wcroot, but we know the abspath is this directory
         (ie. where we found it).  */

      SVN_ERR(svn_wc__db_pdh_create_wcroot(&(*pdh)->wcroot,
                            apr_pstrdup(db->state_pool, local_abspath),
                            sdb, wc_id, FORMAT_FROM_SDB,
                            db->auto_upgrade, db->enforce_empty_wq,
                            db->state_pool, scratch_pool));
    }
  else
    {
      /* We found a wc-1 working copy directory.  */
      SVN_ERR(svn_wc__db_pdh_create_wcroot(&(*pdh)->wcroot,
                            apr_pstrdup(db->state_pool, local_abspath),
                            NULL, UNKNOWN_WC_ID, wc_format,
                            db->auto_upgrade, db->enforce_empty_wq,
                            db->state_pool, scratch_pool));

      /* Don't test for a directory obstructing a versioned file. The wc-1
         code can manage that itself.  */
      obstruction_possible = FALSE;
    }

  {
    const char *dir_relpath;

    /* The subdirectory's relpath is easily computed relative to the
       wcroot that we just found.  */
    dir_relpath = svn_wc__db_pdh_compute_relpath(*pdh, NULL);

    /* And the result local_relpath may include a filename.  */
    *local_relpath = svn_relpath_join(dir_relpath, build_relpath, result_pool);
  }

  /* Check to see if this (versioned) directory is obstructing what should
     be a file in the parent directory.

     ### obstruction is only possible with per-dir wc.db databases.  */
  if (obstruction_possible)
    {
      const char *parent_dir;
      svn_wc__db_pdh_t *parent_pdh;

      /* We should NOT have moved up a directory.  */
      assert(!moved_upwards);

      /* Get/make a PDH for the parent.  */
      parent_dir = svn_dirent_dirname(local_abspath, scratch_pool);
      parent_pdh = apr_hash_get(db->dir_data, parent_dir, APR_HASH_KEY_STRING);
      if (parent_pdh == NULL || parent_pdh->wcroot == NULL)
        {
          svn_error_t *err = svn_wc__db_util_open_db(&sdb, parent_dir,
                                                     SDB_FILE, smode,
                                                     db->state_pool,
                                                     scratch_pool);
          if (err)
            {
              if (err->apr_err != SVN_ERR_SQLITE_ERROR
                  && !APR_STATUS_IS_ENOENT(err->apr_err))
                return svn_error_return(err);
              svn_error_clear(err);

              /* No parent, so we're at a wcroot apparently. An obstruction
                 is (therefore) not possible.  */
              parent_pdh = NULL;
            }
          else
            {
              /* ### construct this according to per-dir semantics.  */
              if (parent_pdh == NULL)
                {
                  parent_pdh = apr_pcalloc(db->state_pool,
                                           sizeof(*parent_pdh));
                  parent_pdh->local_abspath = apr_pstrdup(db->state_pool,
                                                          parent_dir);
                }
              else
                {
                  /* The PDH should have been built correctly (so far).  */
                  SVN_ERR_ASSERT(strcmp(parent_pdh->local_abspath,
                                        parent_dir) == 0);
                }

              SVN_ERR(svn_wc__db_pdh_create_wcroot(&parent_pdh->wcroot,
                                    parent_pdh->local_abspath,
                                    sdb,
                                    1 /* ### hack.  */,
                                    FORMAT_FROM_SDB,
                                    db->auto_upgrade, db->enforce_empty_wq,
                                    db->state_pool, scratch_pool));

              apr_hash_set(db->dir_data,
                           parent_pdh->local_abspath, APR_HASH_KEY_STRING,
                           parent_pdh);

              (*pdh)->parent = parent_pdh;
            }
        }

#ifndef SVN_WC__SINGLE_DB
      if (parent_pdh)
        {
          const char *lookfor_relpath = svn_dirent_basename(local_abspath,
                                                            scratch_pool);

          /* Was there supposed to be a file sitting here?  */
          SVN_ERR(determine_obstructed_file(&(*pdh)->obstructed_file,
                                            parent_pdh->wcroot,
                                            lookfor_relpath,
                                            scratch_pool));

          /* If we determined that a file was supposed to be at the
             LOCAL_ABSPATH requested, then return the PDH and LOCAL_RELPATH
             which describes that file.  */
          if ((*pdh)->obstructed_file)
            {
              *pdh = parent_pdh;
              *local_relpath = apr_pstrdup(result_pool, lookfor_relpath);
              return SVN_NO_ERROR;
            }
        }
#endif
    }

  /* The PDH is complete. Stash it into DB.  */
  apr_hash_set(db->dir_data,
               (*pdh)->local_abspath, APR_HASH_KEY_STRING,
               *pdh);

  /* Did we traverse up to parent directories?  */
  if (!moved_upwards)
    {
      /* We did NOT move to a parent of the original requested directory.
         We've constructed and filled in a PDH for the request, so we
         are done.  */
      return SVN_NO_ERROR;
    }

  /* The PDH that we just built was for the LOCAL_ABSPATH originally passed
     into this function. We stepped *at least* one directory above that.
     We should now create PDH records for each parent directory that does
     not (yet) have one.  */

  child_pdh = *pdh;

  do
    {
      const char *parent_dir = svn_dirent_dirname(child_pdh->local_abspath,
                                                  scratch_pool);
      svn_wc__db_pdh_t *parent_pdh;

      parent_pdh = apr_hash_get(db->dir_data, parent_dir, APR_HASH_KEY_STRING);
      if (parent_pdh == NULL)
        {
          parent_pdh = apr_pcalloc(db->state_pool, sizeof(*parent_pdh));
          parent_pdh->local_abspath = apr_pstrdup(db->state_pool, parent_dir);

          /* All the PDHs have the same wcroot.  */
          parent_pdh->wcroot = (*pdh)->wcroot;

          apr_hash_set(db->dir_data,
                       parent_pdh->local_abspath, APR_HASH_KEY_STRING,
                       parent_pdh);
        }
      else if (parent_pdh->wcroot == NULL)
        {
          parent_pdh->wcroot = (*pdh)->wcroot;
        }

      /* Point the child PDH at this (new) parent PDH. This will allow for
         easy traversals without path munging.  */
      child_pdh->parent = parent_pdh;
      child_pdh = parent_pdh;

      /* Loop if we haven't reached the PDH we found, or the abspath
         where we terminated the search (when we found wc.db). Note that
         if we never located a PDH in our ancestry, then FOUND_PDH will
         be NULL and that portion of the test will always be TRUE.  */
    }
  while (child_pdh != found_pdh
         && strcmp(child_pdh->local_abspath, local_abspath) != 0);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_pdh_navigate_to_parent(svn_wc__db_pdh_t **parent_pdh,
                                  svn_wc__db_t *db,
                                  svn_wc__db_pdh_t *child_pdh,
                                  svn_sqlite__mode_t smode,
                                  apr_pool_t *scratch_pool)
{
  const char *parent_abspath;
  const char *local_relpath;

  if ((*parent_pdh = child_pdh->parent) != NULL
      && (*parent_pdh)->wcroot != NULL)
    return SVN_NO_ERROR;

  /* Make sure we don't see the root as its own parent */
  SVN_ERR_ASSERT(!svn_dirent_is_root(child_pdh->local_abspath,
                                     strlen(child_pdh->local_abspath)));

  parent_abspath = svn_dirent_dirname(child_pdh->local_abspath, scratch_pool);
  SVN_ERR(svn_wc__db_pdh_parse_local_abspath(parent_pdh, &local_relpath, db,
                              parent_abspath, smode,
                              scratch_pool, scratch_pool));
  VERIFY_USABLE_PDH(*parent_pdh);

  child_pdh->parent = *parent_pdh;

  return SVN_NO_ERROR;
}