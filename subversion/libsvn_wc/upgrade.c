/*
 * upgrade.c:  routines for upgrading a working copy
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

#include <apr_pools.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_hash.h"

#include "wc.h"
#include "adm_files.h"
#include "lock.h"
#include "log.h"
#include "entries.h"
#include "wc_db.h"
#include "tree_conflicts.h"
#include "wc-queries.h"  /* for STMT_*  */

#include "svn_private_config.h"
#include "private/svn_wc_private.h"
#include "private/svn_sqlite.h"
#include "private/svn_token.h"


/* Old locations for storing "wcprops" (aka "dav cache").  */
#define WCPROPS_SUBDIR_FOR_FILES "wcprops"
#define WCPROPS_FNAME_FOR_DIR "dir-wcprops"
#define WCPROPS_ALL_DATA "all-wcprops"

/* Old property locations. */
#define PROPS_SUBDIR "props"
#define PROP_BASE_SUBDIR "prop-base"
#define PROP_BASE_FOR_DIR "dir-prop-base"
#define PROP_REVERT_FOR_DIR "dir-prop-revert"
#define PROP_WORKING_FOR_DIR "dir-props"

/* Old textbase location. */
#define TEXT_BASE_SUBDIR "text-base"

#define TEMP_DIR "tmp"

/* Old data files that we no longer need/use.  */
#define ADM_README "README.txt"
#define ADM_EMPTY_FILE "empty-file"
#define ADM_LOG "log"
#define ADM_LOCK "lock"

/* New pristine location */
#define PRISTINE_STORAGE_RELPATH "pristine"


/* Forward declare until we decide to shift/reorder functions.  */
static svn_error_t *
migrate_node_props(const char *wcroot_abspath,
                   const char *name,
                   svn_sqlite__db_t *sdb,
                   int original_format,
                   apr_pool_t *scratch_pool);


/* Read the properties from the file at PROPFILE_ABSPATH, returning them
   as a hash in *PROPS. If the propfile is NOT present, then NULL will
   be returned in *PROPS.  */
static svn_error_t *
read_propfile(apr_hash_t **props,
              const char *propfile_abspath,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  svn_stream_t *stream;

  err = svn_stream_open_readonly(&stream, propfile_abspath,
                                 scratch_pool, scratch_pool);
  if (err)
    {
      if (!APR_STATUS_IS_ENOENT(err->apr_err))
        return svn_error_return(err);

      svn_error_clear(err);

      /* The propfile was not there. Signal with a NULL.  */
      *props = NULL;
      return SVN_NO_ERROR;
    }

  /* ### does this function need to be smarter? will we see zero-length
     ### files? see props.c::load_props(). there may be more work here.
     ### need a historic analysis of 1.x property storage. what will we
     ### actually run into?  */

  /* ### loggy_write_properties() and immediate_install_props() write
     ### zero-length files for "no props", so we should be a bit smarter
     ### in here.  */

  /* ### should we be forgiving in here? I say "no". if we can't be sure,
     ### then we could effectively corrupt the local working copy.  */

  *props = apr_hash_make(result_pool);
  SVN_ERR(svn_hash_read2(*props, stream, SVN_HASH_TERMINATOR, result_pool));

  return svn_error_return(svn_stream_close(stream));
}


/* Read one proplist (allocated from RESULT_POOL) from STREAM, and place it
   into ALL_WCPROPS at NAME.  */
static svn_error_t *
read_one_proplist(apr_hash_t *all_wcprops,
                  const char *name,
                  svn_stream_t *stream,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  apr_hash_t *proplist;

  proplist = apr_hash_make(result_pool);
  SVN_ERR(svn_hash_read2(proplist, stream, SVN_HASH_TERMINATOR, result_pool));
  apr_hash_set(all_wcprops, name, APR_HASH_KEY_STRING, proplist);

  return SVN_NO_ERROR;
}


/* Read the wcprops from all the files in the admin area of DIR_ABSPATH,
   returning them in *ALL_WCPROPS. Results are allocated in RESULT_POOL,
   and temporary allocations are performed in SCRATCH_POOL.  */
static svn_error_t *
read_many_wcprops(apr_hash_t **all_wcprops,
                  const char *dir_abspath,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  const char *propfile_abspath;
  apr_hash_t *wcprops;
  apr_hash_t *dirents;
  const char *props_dir_abspath;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_hash_index_t *hi;

  *all_wcprops = apr_hash_make(result_pool);

  /* First, look at dir-wcprops. */
  propfile_abspath = svn_wc__adm_child(dir_abspath, WCPROPS_FNAME_FOR_DIR,
                                       scratch_pool);
  SVN_ERR(read_propfile(&wcprops, propfile_abspath, result_pool, iterpool));
  if (wcprops != NULL)
    apr_hash_set(*all_wcprops, SVN_WC_ENTRY_THIS_DIR, APR_HASH_KEY_STRING,
                 wcprops);

  props_dir_abspath = svn_wc__adm_child(dir_abspath, WCPROPS_SUBDIR_FOR_FILES,
                                        scratch_pool);

  /* Now walk the wcprops directory. */
  SVN_ERR(svn_io_get_dirents2(&dirents, props_dir_abspath, scratch_pool));

  for (hi = apr_hash_first(scratch_pool, dirents);
       hi;
       hi = apr_hash_next(hi))
    {
      const char *name = svn__apr_hash_index_key(hi);

      svn_pool_clear(iterpool);

      propfile_abspath = svn_dirent_join(props_dir_abspath, name, iterpool);

      SVN_ERR(read_propfile(&wcprops, propfile_abspath,
                            result_pool, iterpool));
      SVN_ERR_ASSERT(wcprops != NULL);
      apr_hash_set(*all_wcprops,
                   apr_pstrdup(result_pool, name), APR_HASH_KEY_STRING,
                   wcprops);
    }

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}


/* For wcprops stored in a single file in this working copy, read that
   file and return it in *ALL_WCPROPS, allocated in RESULT_POOL.   Use
   SCRATCH_POOL for temporary allocations. */
static svn_error_t *
read_wcprops(apr_hash_t **all_wcprops,
             const char *dir_abspath,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  svn_stream_t *stream;
  svn_error_t *err;

  *all_wcprops = apr_hash_make(result_pool);

  err = svn_wc__open_adm_stream(&stream, dir_abspath,
                                WCPROPS_ALL_DATA,
                                scratch_pool, scratch_pool);

  /* A non-existent file means there are no props. */
  if (err && APR_STATUS_IS_ENOENT(err->apr_err))
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  SVN_ERR(err);

  /* Read the proplist for THIS_DIR. */
  SVN_ERR(read_one_proplist(*all_wcprops, SVN_WC_ENTRY_THIS_DIR, stream,
                            result_pool, scratch_pool));

  /* And now, the children. */
  while (1729)
    {
      svn_stringbuf_t *line;
      svn_boolean_t eof;

      SVN_ERR(svn_stream_readline(stream, &line, "\n", &eof, result_pool));
      if (eof)
        {
          if (line->len > 0)
            return svn_error_createf
              (SVN_ERR_WC_CORRUPT, NULL,
               _("Missing end of line in wcprops file for '%s'"),
               svn_dirent_local_style(dir_abspath, scratch_pool));
          break;
        }
      SVN_ERR(read_one_proplist(*all_wcprops, line->data, stream,
                                result_pool, scratch_pool));
    }

  return svn_error_return(svn_stream_close(stream));
}


/* If the versioned child (which should be a directory) exists on disk as
   an actual directory, then add it to the array of subdirs.  */
static svn_error_t *
maybe_add_subdir(apr_array_header_t *subdirs,
                 const char *dir_abspath,
                 const char *child_name,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool)
{
  const char *child_abspath = svn_dirent_join(dir_abspath, child_name,
                                              scratch_pool);
  svn_node_kind_t kind;

  SVN_ERR(svn_io_check_path(child_abspath, &kind, scratch_pool));
  if (kind == svn_node_dir)
    {
      APR_ARRAY_PUSH(subdirs, const char *) = apr_pstrdup(result_pool,
                                                          child_abspath);
    }

  return SVN_NO_ERROR;
}


/* Return in CHILDREN, the list of all versioned subdirectories which also
   exist on disk as directories.  */
static svn_error_t *
get_versioned_subdirs(apr_array_header_t **children,
                      svn_wc__db_t *db,
                      const char *dir_abspath,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  int wc_format;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  *children = apr_array_make(result_pool, 10, sizeof(const char *));

  SVN_ERR(svn_wc__db_temp_get_format(&wc_format, db, dir_abspath, iterpool));
  if (wc_format >= SVN_WC__WC_NG_VERSION)
    {
      const apr_array_header_t *all_children;
      int i;

      SVN_ERR(svn_wc__db_read_children(&all_children, db, dir_abspath,
                                       scratch_pool, scratch_pool));
      for (i = 0; i < all_children->nelts; ++i)
        {
          const char *name = APR_ARRAY_IDX(all_children, i, const char *);

          svn_pool_clear(iterpool);

          SVN_ERR(maybe_add_subdir(*children, dir_abspath, name,
                                   result_pool, iterpool));
        }
    }
  else
    {
      apr_hash_t *entries;
      apr_hash_index_t *hi;

      SVN_ERR(svn_wc__read_entries_old(&entries, dir_abspath,
                                       scratch_pool, iterpool));
      for (hi = apr_hash_first(scratch_pool, entries);
           hi;
           hi = apr_hash_next(hi))
        {
          const char *name = svn__apr_hash_index_key(hi);

          /* skip "this dir"  */
          if (*name == '\0')
            continue;

          svn_pool_clear(iterpool);

          SVN_ERR(maybe_add_subdir(*children, dir_abspath, name,
                                   result_pool, iterpool));
        }
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


/* */
static const char *
build_lockfile_path(const char *local_dir_abspath,
                    apr_pool_t *result_pool)
{
  return svn_dirent_join_many(result_pool,
                              local_dir_abspath,
                              ".svn", /* ### switch to dynamic?  */
                              ADM_LOCK,
                              NULL);
}


/* Create a physical lock file in the admin directory for ABSPATH.  */
static svn_error_t *
create_physical_lock(const char *abspath, apr_pool_t *scratch_pool)
{
  const char *lock_abspath = build_lockfile_path(abspath, scratch_pool);
  svn_error_t *err;
  apr_file_t *file;

  err = svn_io_file_open(&file, lock_abspath,
                         APR_WRITE | APR_CREATE | APR_EXCL,
                         APR_OS_DEFAULT,
                         scratch_pool);

  if (err && APR_STATUS_IS_EEXIST(err->apr_err))
    {
      /* Congratulations, we just stole a physical lock from somebody */
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }

  return svn_error_return(err);
}


/* Wipe out all the obsolete files/dirs from the administrative area.  */
static void
wipe_obsolete_files(const char *wcroot_abspath, apr_pool_t *scratch_pool)
{
  /* Zap unused files.  */
  svn_error_clear(svn_io_remove_file2(
                    svn_wc__adm_child(wcroot_abspath,
                                      SVN_WC__ADM_FORMAT,
                                      scratch_pool),
                    TRUE, scratch_pool));
  svn_error_clear(svn_io_remove_file2(
                    svn_wc__adm_child(wcroot_abspath,
                                      SVN_WC__ADM_ENTRIES,
                                      scratch_pool),
                    TRUE, scratch_pool));
  svn_error_clear(svn_io_remove_file2(
                    svn_wc__adm_child(wcroot_abspath,
                                      ADM_EMPTY_FILE,
                                      scratch_pool),
                    TRUE, scratch_pool));
  svn_error_clear(svn_io_remove_file2(
                    svn_wc__adm_child(wcroot_abspath,
                                      ADM_README,
                                      scratch_pool),
                    TRUE, scratch_pool));

  /* For formats <= SVN_WC__WCPROPS_MANY_FILES_VERSION, we toss the wcprops
     for the directory itself, and then all the wcprops for the files.  */
  svn_error_clear(svn_io_remove_file2(
                    svn_wc__adm_child(wcroot_abspath,
                                      WCPROPS_FNAME_FOR_DIR,
                                      scratch_pool),
                    TRUE, scratch_pool));
  svn_error_clear(svn_io_remove_dir2(
                    svn_wc__adm_child(wcroot_abspath,
                                      WCPROPS_SUBDIR_FOR_FILES,
                                      scratch_pool),
                    FALSE, NULL, NULL, scratch_pool));

  /* And for later formats, they are aggregated into one file.  */
  svn_error_clear(svn_io_remove_file2(
                    svn_wc__adm_child(wcroot_abspath,
                                      WCPROPS_ALL_DATA,
                                      scratch_pool),
                    TRUE, scratch_pool));

#if 0  /* ### NOT READY TO WIPE THESE FILES!!  */
  /* Remove the old properties files... whole directories at a time.  */
  svn_error_clear(svn_io_remove_dir2(
                    svn_wc__adm_child(wcroot_abspath,
                                      PROPS_SUBDIR,
                                      scratch_pool),
                    FALSE, NULL, NULL, scratch_pool));
  svn_error_clear(svn_io_remove_dir2(
                    svn_wc__adm_child(wcroot_abspath,
                                      PROP_BASE_SUBDIR,
                                      scratch_pool),
                    FALSE, NULL, NULL, scratch_pool));
#endif

#if 0  /* Conditional upon upgrading to format 18 */
  /* Remove the old text-base directory. */
  svn_error_clear(svn_io_remove_dir2(
                    svn_wc__adm_child(wcroot_abspath,
                                      TEXT_BASE_SUBDIR,
                                      scratch_pool),
                    FALSE, NULL, NULL, scratch_pool));
#endif

  /* Remove the old-style lock file LAST.  */
  svn_error_clear(svn_io_remove_file2(
                    build_lockfile_path(wcroot_abspath, scratch_pool),
                    TRUE, scratch_pool));
}


/* Ensure that ENTRY has its REPOS and UUID fields set. These will be
   used to establish the REPOSITORY row in the new database, and then
   used within the upgraded entries as they are written into the database.

   If one or both are not available, then it attempts to retrieve this
   information from REPOS_INFO_FUNC, passing REPOS_INFO_BATON.
   Returns a user understandable error using LOCAL_ABSPATH if the
   information cannot be obtained.  */
static svn_error_t *
ensure_repos_info(svn_wc_entry_t *entry,
                  const char *local_abspath,
                  svn_wc_upgrade_get_repos_info_t repos_info_func,
                  void *repos_info_baton,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  /* Easy exit.  */
  if (entry->repos != NULL && entry->uuid != NULL)
    return SVN_NO_ERROR;

  if (entry->repos == NULL && repos_info_func == NULL)
    return svn_error_createf(
        SVN_ERR_WC_UNSUPPORTED_FORMAT, NULL,
        _("Working copy '%s' can't be upgraded because the repository root is "
          "not available and can't be retrieved"),
        svn_dirent_local_style(local_abspath, scratch_pool));

  if (entry->uuid == NULL && repos_info_func == NULL)
    return svn_error_createf(
        SVN_ERR_WC_UNSUPPORTED_FORMAT, NULL,
        _("Working copy '%s' can't be upgraded because the repository uuid is "
          "not available and can't be retrieved"),
        svn_dirent_local_style(local_abspath, scratch_pool));

   if (entry->url == NULL)
     return svn_error_createf(
        SVN_ERR_WC_UNSUPPORTED_FORMAT, NULL,
        _("Working copy '%s' can't be upgraded because it doesn't have a url"),
        svn_dirent_local_style(local_abspath, scratch_pool));

   return svn_error_return((*repos_info_func)(&entry->repos, &entry->uuid,
                                              repos_info_baton,
                                              entry->url,
                                              result_pool, scratch_pool));
}


/* Upgrade the working copy directory represented by DB/DIR_ABSPATH
   from OLD_FORMAT to the wc-ng format (SVN_WC__WC_NG_VERSION)'.

   Uses SCRATCH_POOL for all temporary allocation.  */
static svn_error_t *
upgrade_to_wcng(svn_wc__db_t *db,
                const char *dir_abspath,
                int old_format,
                svn_wc_upgrade_get_repos_info_t repos_info_func,
                void *repos_info_baton,
                apr_pool_t *scratch_pool)
{
  const char *logfile_path = svn_wc__adm_child(dir_abspath, ADM_LOG,
                                               scratch_pool);
  svn_node_kind_t logfile_on_disk;
  apr_hash_t *entries;
  svn_wc_entry_t *this_dir;
  svn_sqlite__db_t *sdb;
  apr_int64_t repos_id;
  apr_int64_t wc_id;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_hash_index_t *hi;

  /* Don't try to mess with the WC if there are old log files left. */

  /* Is the (first) log file present?  */
  SVN_ERR(svn_io_check_path(logfile_path, &logfile_on_disk, iterpool));
  if (logfile_on_disk == svn_node_file)
    return svn_error_create(SVN_ERR_WC_UNSUPPORTED_FORMAT, NULL,
                            _("Cannot upgrade with existing logs; please "
                              "run 'svn cleanup' with Subversion 1.6"));

  /* Lock this working copy directory, or steal an existing lock. Do this
     BEFORE we read the entries. We don't want another process to modify the
     entries after we've read them into memory.  */
  SVN_ERR(create_physical_lock(dir_abspath, iterpool));

  /* What's going on here?
   *
   * We're attempting to upgrade an older working copy to the new wc-ng format.
   * The semantics and storage mechanisms between the two are vastly different,
   * so it's going to be a bit painful.  Here's a plan for the operation:
   *
   * 1) The 'entries' file needs to be moved to the new format. We read it
   *    using the old-format reader, and then use our compatibility code
   *    for writing entries to fill out the (new) wc_db state.
   *
   * 2) Convert wcprop to the wc-ng format
   *
   * 3) Trash old, unused files and subdirs
   *
   * ### (fill in other bits as they are implemented)
   */

  /***** ENTRIES *****/
  SVN_ERR(svn_wc__read_entries_old(&entries, dir_abspath,
                                   scratch_pool, iterpool));

  this_dir = apr_hash_get(entries, SVN_WC_ENTRY_THIS_DIR, APR_HASH_KEY_STRING);
  SVN_ERR(ensure_repos_info(this_dir, dir_abspath,
                            repos_info_func, repos_info_baton,
                            scratch_pool, iterpool));

  /* Create an empty sqlite database for this directory. */
  SVN_ERR(svn_wc__db_upgrade_begin(&sdb, &repos_id, &wc_id, dir_abspath,
                                   this_dir->repos, this_dir->uuid,
                                   scratch_pool, iterpool));

  /* Migrate the entries over to the new database.
     ### We need to think about atomicity here.

     entries_write_new() writes in current format rather than f12. Thus, this
     function bumps a working copy all the way to current.  */
  SVN_ERR(svn_wc__db_temp_reset_format(SVN_WC__VERSION, db, dir_abspath,
                                       iterpool));
  SVN_ERR(svn_wc__db_wclock_set(db, dir_abspath, 0, iterpool));
  SVN_ERR(svn_wc__write_upgraded_entries(db, sdb, repos_id, wc_id,
                                         dir_abspath, entries,
                                         iterpool));

  /***** WC PROPS *****/

  /* Ugh. We don't know precisely where the wcprops are. Ignore them.  */
  if (old_format != SVN_WC__WCPROPS_LOST)
    {
      apr_hash_t *all_wcprops;

      if (old_format <= SVN_WC__WCPROPS_MANY_FILES_VERSION)
        SVN_ERR(read_many_wcprops(&all_wcprops, dir_abspath,
                                  iterpool, iterpool));
      else
        SVN_ERR(read_wcprops(&all_wcprops, dir_abspath,
                             iterpool, iterpool));

      SVN_ERR(svn_wc__db_upgrade_apply_dav_cache(sdb, all_wcprops, iterpool));
    }

  /* Upgrade all the properties (including "this dir").

     Note: this must come AFTER the entries have been migrated into the
     database. The upgrade process needs to examine the WORKING state.  */
  for (hi = apr_hash_first(scratch_pool, entries);
       hi != NULL;
       hi = apr_hash_next(hi))
    {
      const char *name = svn__apr_hash_index_key(hi);

      svn_pool_clear(iterpool);

#if 0
      /* ### need to skip subdir stub entries.  */
      /* ### not ready for this yet.  */
      SVN_ERR(migrate_node_props(dir_abspath, name, sdb, old_format,
                                 iterpool));
#endif
    }

  SVN_ERR(svn_wc__db_upgrade_finish(dir_abspath, sdb, iterpool));

  /* All subdir access batons (and locks!) will be closed. Of course, they
     should have been closed/unlocked just after their own upgrade process
     has run.  */
  /* ### well, actually.... we don't recursively delete subdir locks here,
     ### we rely upon their own upgrade processes to do it. */
  SVN_ERR(svn_wc__db_wclock_remove(db, dir_abspath, iterpool));

  /* Zap all the obsolete files. This removes the old-style lock file.  */
  wipe_obsolete_files(dir_abspath, iterpool);

  /* ### need to (eventually) delete the .svn subdir.  */

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
bump_to_13(void *baton, svn_sqlite__db_t *sdb, apr_pool_t *scratch_pool)
{
  SVN_ERR(svn_sqlite__exec_statements(sdb, STMT_UPGRADE_TO_13));

  return SVN_NO_ERROR;
}


#if 0 /* ### no tree conflict migration yet */

/* ### duplicated from wc_db.c  */
static const char *
kind_to_word(svn_wc__db_kind_t kind)
{
  switch (kind)
    {
    case svn_wc__db_kind_dir:
      return "dir";
    case svn_wc__db_kind_file:
      return "file";
    case svn_wc__db_kind_symlink:
      return "symlink";
    case svn_wc__db_kind_unknown:
      return "unknown";
    case svn_wc__db_kind_subdir:
      return "subdir";
    default:
      SVN_ERR_MALFUNCTION_NO_RETURN();
    }
}


/* */
static const char *
conflict_kind_to_word(svn_wc_conflict_kind_t conflict_kind)
{
  switch (conflict_kind)
    {
    case svn_wc_conflict_kind_text:
      return "text";
    case svn_wc_conflict_kind_property:
      return "property";
    case svn_wc_conflict_kind_tree:
      return "tree";
    default:
      SVN_ERR_MALFUNCTION_NO_RETURN();
    }
}


/* */
static const char *
conflict_action_to_word(svn_wc_conflict_action_t action)
{
  return svn_token__to_word(svn_wc__conflict_action_map, action);
}


/* */
static const char *
conflict_reason_to_word(svn_wc_conflict_reason_t reason)
{
  return svn_token__to_word(svn_wc__conflict_reason_map, reason);
}


/* */
static const char *
wc_operation_to_word(svn_wc_operation_t operation)
{
  return svn_token__to_word(svn_wc__operation_map, operation);
}


/* */
static svn_wc__db_kind_t
db_kind_from_node_kind(svn_node_kind_t node_kind)
{
  switch (node_kind)
    {
    case svn_node_file:
      return svn_wc__db_kind_file;
    case svn_node_dir:
      return svn_wc__db_kind_dir;
    case svn_node_unknown:
    case svn_node_none:
      return svn_wc__db_kind_unknown;
    default:
      SVN_ERR_MALFUNCTION_NO_RETURN();
    }
}


/* */
static svn_error_t *
migrate_single_tree_conflict_data(svn_sqlite__db_t *sdb,
                                  const char *tree_conflict_data,
                                  apr_uint64_t wc_id,
                                  const char *local_relpath,
                                  apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *insert_stmt;
  apr_hash_t *conflicts;
  apr_hash_index_t *hi;
  apr_pool_t *iterpool;

  SVN_ERR(svn_sqlite__get_statement(&insert_stmt, sdb,
                                    STMT_INSERT_NEW_CONFLICT));

  SVN_ERR(svn_wc__read_tree_conflicts(&conflicts, tree_conflict_data,
                                      local_relpath, scratch_pool));

  iterpool = svn_pool_create(scratch_pool);
  for (hi = apr_hash_first(scratch_pool, conflicts);
       hi;
       hi = apr_hash_next(hi))
    {
      const svn_wc_conflict_description2_t *conflict =
          svn__apr_hash_index_val(hi);
      const char *conflict_relpath;
      apr_int64_t left_repos_id;
      apr_int64_t right_repos_id;

      svn_pool_clear(iterpool);

      conflict_relpath = svn_dirent_join(local_relpath,
                                         svn_dirent_basename(
                                           conflict->local_abspath, iterpool),
                                         iterpool);

      /* Optionally get the right repos ids. */
      if (conflict->src_left_version)
        {
          SVN_ERR(svn_wc__db_upgrade_get_repos_id(
                    &left_repos_id,
                    sdb,
                    conflict->src_left_version->repos_url,
                    iterpool));
        }

      if (conflict->src_right_version)
        {
          SVN_ERR(svn_wc__db_upgrade_get_repos_id(
                    &right_repos_id,
                    sdb,
                    conflict->src_right_version->repos_url,
                    iterpool));
        }

      SVN_ERR(svn_sqlite__bindf(insert_stmt, "is", wc_id, conflict_relpath));

      SVN_ERR(svn_sqlite__bind_text(insert_stmt, 3,
                                    svn_dirent_dirname(conflict_relpath,
                                                       iterpool)));
      SVN_ERR(svn_sqlite__bind_text(insert_stmt, 4,
                                    kind_to_word(db_kind_from_node_kind(
                                                        conflict->node_kind))));
      SVN_ERR(svn_sqlite__bind_text(insert_stmt, 5,
                                    conflict_kind_to_word(conflict->kind)));

      if (conflict->property_name)
        SVN_ERR(svn_sqlite__bind_text(insert_stmt, 6,
                                      conflict->property_name));

      SVN_ERR(svn_sqlite__bind_text(insert_stmt, 7,
                              conflict_action_to_word(conflict->action)));
      SVN_ERR(svn_sqlite__bind_text(insert_stmt, 8,
                              conflict_reason_to_word(conflict->reason)));
      SVN_ERR(svn_sqlite__bind_text(insert_stmt, 9,
                              wc_operation_to_word(conflict->operation)));

      if (conflict->src_left_version)
        {
          SVN_ERR(svn_sqlite__bind_int64(insert_stmt, 10, left_repos_id));
          SVN_ERR(svn_sqlite__bind_text(insert_stmt, 11,
                                   conflict->src_left_version->path_in_repos));
          SVN_ERR(svn_sqlite__bind_int64(insert_stmt, 12,
                                       conflict->src_left_version->peg_rev));
          SVN_ERR(svn_sqlite__bind_text(insert_stmt, 13,
                                        kind_to_word(db_kind_from_node_kind(
                                    conflict->src_left_version->node_kind))));
        }

      if (conflict->src_right_version)
        {
          SVN_ERR(svn_sqlite__bind_int64(insert_stmt, 14, right_repos_id));
          SVN_ERR(svn_sqlite__bind_text(insert_stmt, 15,
                                 conflict->src_right_version->path_in_repos));
          SVN_ERR(svn_sqlite__bind_int64(insert_stmt, 16,
                                       conflict->src_right_version->peg_rev));
          SVN_ERR(svn_sqlite__bind_text(insert_stmt, 17,
                                        kind_to_word(db_kind_from_node_kind(
                                    conflict->src_right_version->node_kind))));
        }

      SVN_ERR(svn_sqlite__insert(NULL, insert_stmt));
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
migrate_tree_conflicts(svn_sqlite__db_t *sdb,
                       apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *select_stmt;
  svn_sqlite__stmt_t *erase_stmt;
  svn_boolean_t have_row;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  /* Iterate over each node which has a set of tree conflicts, then insert
     all of them into the new schema.  */

  SVN_ERR(svn_sqlite__get_statement(&select_stmt, sdb,
                                    STMT_SELECT_OLD_TREE_CONFLICT));

  /* Get all the existing tree conflict data. */
  SVN_ERR(svn_sqlite__step(&have_row, select_stmt));
  while (have_row)
    {
      apr_uint64_t wc_id;
      const char *local_relpath;
      const char *tree_conflict_data;

      svn_pool_clear(iterpool);

      wc_id = svn_sqlite__column_int64(select_stmt, 0);
      local_relpath = svn_sqlite__column_text(select_stmt, 1, iterpool);
      tree_conflict_data = svn_sqlite__column_text(select_stmt, 2,
                                                   iterpool);

      SVN_ERR(migrate_single_tree_conflict_data(sdb,
                                                tree_conflict_data,
                                                wc_id, local_relpath,
                                                iterpool));

      /* We don't need to do anything but step over the previously
         prepared statement. */
      SVN_ERR(svn_sqlite__step(&have_row, select_stmt));
    }
  SVN_ERR(svn_sqlite__reset(select_stmt));

  /* Erase all the old tree conflict data.  */
  SVN_ERR(svn_sqlite__get_statement(&erase_stmt, sdb,
                                    STMT_ERASE_OLD_CONFLICTS));
  SVN_ERR(svn_sqlite__step_done(erase_stmt));

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

#endif /* ### no tree conflict migration yet */


/* */
static svn_error_t *
migrate_locks(const char *wcroot_abspath,
              svn_sqlite__db_t *sdb,
              apr_pool_t *scratch_pool)
{
  const char *lockfile_abspath = build_lockfile_path(wcroot_abspath,
                                                     scratch_pool);
  svn_node_kind_t kind;

  SVN_ERR(svn_io_check_path(lockfile_abspath, &kind, scratch_pool));
  if (kind != svn_node_none)
    {
      svn_sqlite__stmt_t *stmt;
      SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_INSERT_WC_LOCK));
      /* ### These values are magic, and will need to be updated when we
         ### go to a centralized system. */
      SVN_ERR(svn_sqlite__bindf(stmt, "is", (apr_int64_t)1, ""));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
bump_to_14(void *baton, svn_sqlite__db_t *sdb, apr_pool_t *scratch_pool)
{
  const char *wcroot_abspath = baton;

  SVN_ERR(svn_sqlite__exec_statements(sdb, STMT_UPGRADE_TO_14));

  SVN_ERR(migrate_locks(wcroot_abspath, sdb, scratch_pool));

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
bump_to_15(void *baton, svn_sqlite__db_t *sdb, apr_pool_t *scratch_pool)
{
  SVN_ERR(svn_sqlite__exec_statements(sdb, STMT_UPGRADE_TO_15));

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
bump_to_16(void *baton, svn_sqlite__db_t *sdb, apr_pool_t *scratch_pool)
{
  SVN_ERR(svn_sqlite__exec_statements(sdb, STMT_UPGRADE_TO_16));

  return SVN_NO_ERROR;
}


/* Migrate the properties for one node (LOCAL_ABSPATH).  */
static svn_error_t *
migrate_node_props(const char *wcroot_abspath,
                   const char *name,
                   svn_sqlite__db_t *sdb,
                   int original_format,
                   apr_pool_t *scratch_pool)
{
  const char *base_abspath;  /* old name. nowadays: "pristine"  */
  const char *revert_abspath;  /* old name. nowadays: "BASE"  */
  const char *working_abspath;  /* old name. nowadays: "ACTUAL"  */
  apr_hash_t *base_props;
  apr_hash_t *revert_props;
  apr_hash_t *working_props;

  if (*name == '\0')
    {
      base_abspath = svn_wc__adm_child(wcroot_abspath, PROP_BASE_FOR_DIR,
                                       scratch_pool);
      revert_abspath = svn_wc__adm_child(wcroot_abspath, PROP_REVERT_FOR_DIR,
                                         scratch_pool);
      working_abspath = svn_wc__adm_child(wcroot_abspath, PROP_WORKING_FOR_DIR,
                                          scratch_pool);
    }
  else
    {
      const char *basedir_abspath;
      const char *propsdir_abspath;

      propsdir_abspath = svn_wc__adm_child(wcroot_abspath, PROPS_SUBDIR,
                                           scratch_pool);
      basedir_abspath = svn_wc__adm_child(wcroot_abspath, PROP_BASE_SUBDIR,
                                          scratch_pool);

      base_abspath = svn_dirent_join(basedir_abspath,
                                     apr_pstrcat(scratch_pool,
                                                 name,
                                                 SVN_WC__BASE_EXT,
                                                 NULL),
                                     scratch_pool);

      revert_abspath = svn_dirent_join(basedir_abspath,
                                       apr_pstrcat(scratch_pool,
                                                   name,
                                                   SVN_WC__REVERT_EXT,
                                                   NULL),
                                       scratch_pool);

      working_abspath = svn_dirent_join(propsdir_abspath,
                                        apr_pstrcat(scratch_pool,
                                                    name,
                                                    SVN_WC__WORK_EXT,
                                                    NULL),
                                        scratch_pool);
    }

  SVN_ERR(read_propfile(&base_props, base_abspath,
                        scratch_pool, scratch_pool));
  SVN_ERR(read_propfile(&revert_props, revert_abspath,
                        scratch_pool, scratch_pool));
  SVN_ERR(read_propfile(&working_props, working_abspath,
                        scratch_pool, scratch_pool));

  return svn_error_return(svn_wc__db_upgrade_apply_props(
                            sdb, name,
                            base_props, revert_props, working_props,
                            original_format,
                            scratch_pool));
}


/* */
static svn_error_t *
migrate_props(const char *wcroot_abspath,
              svn_sqlite__db_t *sdb,
              int original_format,
              apr_pool_t *scratch_pool)
{
  /* General logic here: iterate over all the immediate children of the root
     (since we aren't yet in a centralized system), and for any properties that
     exist, map them as follows:

     if (revert props exist):
       revert  -> BASE
       base    -> WORKING
       working -> ACTUAL
     else if (prop pristine is working [as defined in props.c] ):
       base    -> WORKING
       working -> ACTUAL
     else:
       base    -> BASE
       working -> ACTUAL

     ### the middle "test" should simply look for a WORKING_NODE row

     Note that it is legal for "working" props to be missing. That implies
     no local changes to the properties.
  */
  const apr_array_header_t *children;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  svn_wc__db_t *db;
  int i;

  /* ### the use of DB within this function must go away.  */

  /* *sigh*  We actually want to use wc_db APIs to read data, but we aren't
     provided a wc_db, so open one. */
  SVN_ERR(svn_wc__db_open(&db, svn_wc__db_openmode_default, NULL, FALSE, TRUE,
                          scratch_pool, scratch_pool));

  /* Migrate the props for "this dir".  */
  SVN_ERR(migrate_node_props(wcroot_abspath, "", sdb, original_format,
                             scratch_pool));

  /* Go find all the children of the wcroot. */
  SVN_ERR(svn_wc__db_read_children(&children, db, wcroot_abspath,
                                   scratch_pool, scratch_pool));

  /* Iterate over the children, as described above */
  for (i = 0; i < children->nelts; i++)
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);

      svn_pool_clear(iterpool);

      SVN_ERR(migrate_node_props(wcroot_abspath, name, sdb, original_format,
                                 iterpool));
    }

#if 0
  /* ### we are not (yet) taking out a write lock  */
  SVN_ERR(svn_wc__adm_cleanup_tmp_area(db, wcroot_abspath, iterpool));
#endif

  SVN_ERR(svn_wc__db_close(db));
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


/* */
struct bump_to_17_baton
{
  const char *wcroot_abspath;
  int original_format;
};


static svn_error_t *
bump_to_17(void *baton, svn_sqlite__db_t *sdb, apr_pool_t *scratch_pool)
{
  struct bump_to_17_baton *b17 = baton;

#if 0
  /* ### no schema changes (yet)... */
  SVN_ERR(svn_sqlite__exec_statements(sdb, STMT_UPGRADE_TO_17));
#endif

  SVN_ERR(migrate_props(b17->wcroot_abspath, sdb, b17->original_format,
                        scratch_pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
migrate_text_bases(const char *wcroot_abspath,
                   svn_sqlite__db_t *sdb,
                   apr_pool_t *scratch_pool)
{
  apr_hash_t *dirents;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_hash_index_t *hi;
  const char *text_base_dir = svn_wc__adm_child(wcroot_abspath,
                                                TEXT_BASE_SUBDIR,
                                                scratch_pool);

  SVN_ERR(svn_io_get_dir_filenames(&dirents, text_base_dir, scratch_pool));
  for (hi = apr_hash_first(scratch_pool, dirents); hi;
            hi = apr_hash_next(hi))
    {
      const char *text_base_basename = svn__apr_hash_index_key(hi);
      const char *pristine_path;
      const char *text_base_path;
      svn_checksum_t *md5_checksum;
      svn_checksum_t *sha1_checksum;
      svn_sqlite__stmt_t *stmt;
      apr_finfo_t finfo;

      svn_pool_clear(iterpool);
      text_base_path = svn_dirent_join(text_base_dir, text_base_basename,
                                       iterpool);

      /* ### This code could be a bit smarter: we could chain checksum
             streams instead of reading the file twice; we could check to
             see if a pristine row exists before attempting to insert one;
             we could check and see if a pristine file exists before
             attempting to copy a new one over it.
             
             However, I think simplicity is the big win here, especially since
             this is code that runs exactly once on a user's machine: when
             doing the upgrade.  If you disagree, feel free to add the
             complexity. :)  */

      /* Gather the two checksums. */
      SVN_ERR(svn_io_file_checksum2(&md5_checksum, text_base_path,
                                    svn_checksum_md5, iterpool));
      SVN_ERR(svn_io_file_checksum2(&sha1_checksum, text_base_path,
                                    svn_checksum_sha1, iterpool));

      SVN_ERR(svn_io_stat(&finfo, text_base_path, APR_FINFO_SIZE, iterpool));

      /* Insert a row into the pristine table. */
      SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_INSERT_PRISTINE));
      SVN_ERR(svn_sqlite__bind_checksum(stmt, 1, sha1_checksum, iterpool));
      SVN_ERR(svn_sqlite__bind_checksum(stmt, 2, md5_checksum, iterpool));
      SVN_ERR(svn_sqlite__bind_int64(stmt, 3, finfo.size));
      SVN_ERR(svn_sqlite__insert(NULL, stmt));

      /* Compute the path of the pristine.
         Note: in format 18, pristines are not yet sharded, so don't include
         that in the path computation. */
      pristine_path = svn_dirent_join_many(iterpool,
                                           wcroot_abspath,
                                           svn_wc_get_adm_dir(iterpool),
                                           PRISTINE_STORAGE_RELPATH,
                                           NULL);

      /* Finally, copy the file over. */
      SVN_ERR(svn_io_copy_file(text_base_path, pristine_path, TRUE,
                               iterpool));
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


static svn_error_t *
bump_to_18(void *baton, svn_sqlite__db_t *sdb, apr_pool_t *scratch_pool)
{
  const char *wcroot_abspath = baton;

  SVN_ERR(migrate_text_bases(wcroot_abspath, sdb, scratch_pool));

  return SVN_NO_ERROR;
}


#if 0 /* ### no tree conflict migration yet */

/* */
static svn_error_t *
bump_to_XXX(void *baton, svn_sqlite__db_t *sdb, apr_pool_t *scratch_pool)
{
  const char *wcroot_abspath = baton;

  SVN_ERR(svn_sqlite__exec_statements(sdb, STMT_UPGRADE_TO_XXX));

  SVN_ERR(migrate_tree_conflicts(sdb, scratch_pool));

  return SVN_NO_ERROR;
}

#endif /* ### no tree conflict migration yet */


svn_error_t *
svn_wc__upgrade_sdb(int *result_format,
                    const char *wcroot_abspath,
                    svn_sqlite__db_t *sdb,
                    int start_format,
                    apr_pool_t *scratch_pool)
{
  if (start_format < SVN_WC__WC_NG_VERSION /* 12 */)
    return svn_error_createf(SVN_ERR_WC_UPGRADE_REQUIRED, NULL,
                             _("Working copy format of '%s' is too old (%d); "
                               "please run 'svn upgrade'"),
                             svn_dirent_local_style(wcroot_abspath,
                                                    scratch_pool),
                             start_format);

  /* ### need lock-out. only one upgrade at a time. note that other code
     ### cannot use this un-upgraded database until we finish the upgrade.  */

  /* Note: none of these have "break" statements; the fall-through is
     intentional. */
  switch (start_format)
    {
      case 12:
        SVN_ERR(svn_sqlite__with_transaction(sdb, bump_to_13,
                                             (void *)wcroot_abspath,
                                             scratch_pool));
        /* If the transaction succeeded, then we don't need the wcprops
           files. We stopped writing them partway through format 12, but
           we may be upgrading from an "early 12" and need to toss those
           files. We aren't going to migrate them because it is *also*
           possible that current/real data is sitting within the database.
           This is why STMT_UPGRADE_TO_13 just clears the 'dav_cache'
           column -- we cannot definitely state that the column values
           are Proper.

           They're removed by wipe_obsolete_files(), below.  */

        *result_format = 13;
        /* FALLTHROUGH  */

      case 13:
        /* Build WCLOCKS and migrate any physical lock.  */
        SVN_ERR(svn_sqlite__with_transaction(sdb, bump_to_14,
                                             (void *)wcroot_abspath,
                                             scratch_pool));
        /* If the transaction succeeded, then any lock has been migrated,
           and we can toss the physical file (below).  */

        *result_format = 14;
        /* FALLTHROUGH  */

      case 14:
        /* Revamp the recording of 'excluded' nodes.  */
        SVN_ERR(svn_sqlite__with_transaction(sdb, bump_to_15,
                                             (void *)wcroot_abspath,
                                             scratch_pool));
        *result_format = 15;
        /* FALLTHROUGH  */

      case 15:
        /* Perform some minor changes to the schema.  */
        SVN_ERR(svn_sqlite__with_transaction(sdb, bump_to_16,
                                             (void *)wcroot_abspath,
                                             scratch_pool));
        *result_format = 16;
        /* FALLTHROUGH  */

#if (SVN_WC__VERSION > 16)
      case 16:
        {
          struct bump_to_17_baton b17;

          b17.wcroot_abspath = wcroot_abspath;
          b17.original_format = start_format;

          /* Move the properties into the database.  */
          SVN_ERR(svn_sqlite__with_transaction(sdb, bump_to_17, &b17,
                                               scratch_pool));
        }

        *result_format = 17;
        /* FALLTHROUGH  */
#endif

#if 0
      case 17:
        {
          const char *pristine_dir;

          /* Create the '.svn/pristine' directory.  */
          pristine_dir = svn_wc__adm_child(wcroot_abspath,
                                           SVN_WC__ADM_PRISTINE,
                                           scratch_pool);
          SVN_ERR(svn_io_dir_make(pristine_dir, APR_OS_DEFAULT, scratch_pool));

          /* Move text bases into the pristine directory, and update the db */
          SVN_ERR(svn_sqlite__with_transaction(sdb, bump_to_18, wcroot_abspath,
                                               scratch_pool));
        }

        *result_format = 18;
        /* FALLTHROUGH  */
#endif

      /* ### future bumps go here.  */
#if 0
      case 98:
        /* Revamp the recording of tree conflicts.  */
        SVN_ERR(svn_sqlite__with_transaction(sdb, bump_to_XXX,
                                             (void *)wcroot_abspath,
                                             scratch_pool));
        *result_format = 99;
#endif
    }

  /* Zap anything that might be remaining or escaped our notice.  */
  wipe_obsolete_files(wcroot_abspath, scratch_pool);

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
upgrade_working_copy(svn_wc__db_t *db,
                     const char *dir_abspath,
                     svn_wc_upgrade_get_repos_info_t repos_info_func,
                     void *repos_info_baton,
                     svn_cancel_func_t cancel_func,
                     void *cancel_baton,
                     svn_wc_notify_func2_t notify_func,
                     void *notify_baton,
                     apr_pool_t *scratch_pool)
{
  int old_format;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_array_header_t *subdirs;
  int i;

  /* Check cancellation; note that this catches recursive calls too. */
  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  SVN_ERR(svn_wc__db_temp_get_format(&old_format, db, dir_abspath,
                                     iterpool));

  SVN_ERR(get_versioned_subdirs(&subdirs, db, dir_abspath,
                                scratch_pool, iterpool));

  /* Upgrade this directory first. */
  if (old_format < SVN_WC__WC_NG_VERSION)
    SVN_ERR(upgrade_to_wcng(db, dir_abspath, old_format,
                            repos_info_func, repos_info_baton,
                            iterpool));

  if (notify_func)
    notify_func(notify_baton,
                svn_wc_create_notify(dir_abspath, svn_wc_notify_upgraded_path,
                                     iterpool),
                iterpool);

  /* Now recurse. */
  for (i = 0; i < subdirs->nelts; ++i)
    {
      const char *child_abspath = APR_ARRAY_IDX(subdirs, i, const char *);

      svn_pool_clear(iterpool);

      SVN_ERR(upgrade_working_copy(db, child_abspath,
                                   repos_info_func, repos_info_baton,
                                   cancel_func, cancel_baton,
                                   notify_func, notify_baton,
                                   iterpool));
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_upgrade(svn_wc_context_t *wc_ctx,
               const char *local_abspath,
               svn_wc_upgrade_get_repos_info_t repos_info_func,
               void *repos_info_baton,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               svn_wc_notify_func2_t notify_func,
               void *notify_baton,
               apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db;
#if 0
  svn_boolean_t is_wcroot;
#endif

  /* We need a DB that does not attempt an auto-upgrade, nor require
     running a stale work queue. We'll handle everything manually.  */
  SVN_ERR(svn_wc__db_open(&db, svn_wc__db_openmode_readwrite,
                          NULL /* ### config */, FALSE, FALSE,
                          scratch_pool, scratch_pool));

  /* ### this expects a wc-ng working copy. sigh. fix up soonish...  */
#if 0
  SVN_ERR(svn_wc__strictly_is_wc_root(&is_wcroot, wc_ctx, local_abspath,
                                      scratch_pool));
  if (!is_wcroot)
    return svn_error_create(
      SVN_ERR_WC_INVALID_OP_ON_CWD, NULL,
      _("'svn upgrade' can only be run from the root of the working copy."));
#endif

  /* Upgrade this directory and/or its subdirectories.  */
  SVN_ERR(upgrade_working_copy(db, local_abspath,
                               repos_info_func, repos_info_baton,
                               cancel_func, cancel_baton,
                               notify_func, notify_baton,
                               scratch_pool));

  SVN_ERR(svn_wc__db_close(db));

  return SVN_NO_ERROR;
}
