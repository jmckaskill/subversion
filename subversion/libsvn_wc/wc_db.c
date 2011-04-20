/*
 * wc_db.c :  manipulating the administrative database
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
#include <apr_pools.h>
#include <apr_hash.h>

#include "svn_types.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_hash.h"
#include "svn_wc.h"
#include "svn_checksum.h"
#include "svn_pools.h"

#include "wc.h"
#include "wc_db.h"
#include "adm_files.h"
#include "wc-queries.h"
#include "entries.h"
#include "lock.h"
#include "tree_conflicts.h"
#include "wc_db_private.h"
#include "workqueue.h"

#include "svn_private_config.h"
#include "private/svn_sqlite.h"
#include "private/svn_skel.h"
#include "private/svn_wc_private.h"
#include "private/svn_token.h"


#define NOT_IMPLEMENTED() SVN__NOT_IMPLEMENTED()


/*
 * Some filename constants.
 */
#define SDB_FILE  "wc.db"

#define WCROOT_TEMPDIR_RELPATH   "tmp"


/*
 * PARAMETER ASSERTIONS
 *
 * Every (semi-)public entrypoint in this file has a set of assertions on
 * the parameters passed into the function. Since this is a brand new API,
 * we want to make sure that everybody calls it properly. The original WC
 * code had years to catch stray bugs, but we do not have that luxury in
 * the wc-nb rewrite. Any extra assurances that we can find will be
 * welcome. The asserts will ensure we have no doubt about the values
 * passed into the function.
 *
 * Some parameters are *not* specifically asserted. Typically, these are
 * params that will be used immediately, so something like a NULL value
 * will be obvious.
 *
 * ### near 1.7 release, it would be a Good Thing to review the assertions
 * ### and decide if any can be removed or switched to assert() in order
 * ### to remove their runtime cost in the production release.
 *
 *
 * DATABASE OPERATIONS
 *
 * Each function should leave the database in a consistent state. If it
 * does *not*, then the implication is some other function needs to be
 * called to restore consistency. Subtle requirements like that are hard
 * to maintain over a long period of time, so this API will not allow it.
 *
 *
 * STANDARD VARIABLE NAMES
 *
 * db     working copy database (this module)
 * sdb    SQLite database (not to be confused with 'db')
 * wc_id  a WCROOT id associated with a node
 */

#define INVALID_REPOS_ID ((apr_int64_t) -1)
#define UNKNOWN_WC_ID ((apr_int64_t) -1)
#define FORMAT_FROM_SDB (-1)

/* Check if the column contains actual properties. The empty set of properties
   is stored as "()", so we have properties if the size of the column is
   larger then 2. */
#define SQLITE_PROPERTIES_AVAILABLE(stmt, i) \
                 (svn_sqlite__column_bytes(stmt, i) > 2)

/* This is a character used to escape itself and the globbing character in
   globbing sql expressions below.  See escape_sqlite_like().

   NOTE: this should match the character used within wc-metadata.sql  */
#define LIKE_ESCAPE_CHAR     "#"

/* Calculates the depth of the relpath below "" */
APR_INLINE static int relpath_depth(const char *relpath)
{
  int n = 1;
  if (*relpath == '\0')
    return 0;

  do
  {
    if (*relpath == '/')
      n++;
  }
  while (*(++relpath));

  return n;
}


int svn_wc__db_op_depth_for_upgrade(const char *local_relpath)
{
  return relpath_depth(local_relpath);
}


typedef struct insert_base_baton_t {
  /* common to all insertions into BASE */
  svn_wc__db_status_t status;
  svn_wc__db_kind_t kind;
  apr_int64_t repos_id;
  const char *repos_relpath;
  svn_revnum_t revision;

  /* Only used when repos_id == -1 */
  const char *repos_root_url;
  const char *repos_uuid;

  /* common to all "normal" presence insertions */
  const apr_hash_t *props;
  svn_revnum_t changed_rev;
  apr_time_t changed_date;
  const char *changed_author;
  apr_hash_t *dav_cache;

  /* for inserting directories */
  const apr_array_header_t *children;
  svn_depth_t depth;

  /* for inserting files */
  const svn_checksum_t *checksum;
  svn_filesize_t translated_size;

  /* for inserting symlinks */
  const char *target;

  /* may need to insert/update ACTUAL to record a conflict  */
  const svn_skel_t *conflict;

  /* may need to insert/update ACTUAL to record new properties */
  svn_boolean_t update_actual_props;
  const apr_hash_t *new_actual_props;

  /* may have work items to queue in this transaction  */
  const svn_skel_t *work_items;

} insert_base_baton_t;


typedef struct insert_working_baton_t {
  /* common to all insertions into WORKING (including NODE_DATA) */
  svn_wc__db_status_t presence;
  svn_wc__db_kind_t kind;
  apr_int64_t op_depth;

  /* common to all "normal" presence insertions */
  const apr_hash_t *props;
  svn_revnum_t changed_rev;
  apr_time_t changed_date;
  const char *changed_author;
  apr_int64_t original_repos_id;
  const char *original_repos_relpath;
  svn_revnum_t original_revnum;
  svn_boolean_t moved_here;

  /* for inserting directories */
  const apr_array_header_t *children;
  svn_depth_t depth;

  /* for inserting (copied/moved-here) files */
  const svn_checksum_t *checksum;

  /* for inserting symlinks */
  const char *target;

  /* may have work items to queue in this transaction  */
  const svn_skel_t *work_items;

  /* If the value is > 0 and < op_depth, also insert a not-present
     at op-depth NOT_PRESENT_OP_DEPTH, based on this same information */
  apr_int64_t not_present_op_depth;

} insert_working_baton_t;


static const svn_token_map_t kind_map[] = {
  { "file", svn_wc__db_kind_file },
  { "dir", svn_wc__db_kind_dir },
  { "symlink", svn_wc__db_kind_symlink },
  { "unknown", svn_wc__db_kind_unknown },
  { NULL }
};

/* Note: we only decode presence values from the database. These are a subset
   of all the status values. */
static const svn_token_map_t presence_map[] = {
  { "normal", svn_wc__db_status_normal },
  { "absent", svn_wc__db_status_absent },
  { "excluded", svn_wc__db_status_excluded },
  { "not-present", svn_wc__db_status_not_present },
  { "incomplete", svn_wc__db_status_incomplete },
  { "base-deleted", svn_wc__db_status_base_deleted },
  { NULL }
};


/* Forward declarations  */
static svn_error_t *
add_work_items(svn_sqlite__db_t *sdb,
               const svn_skel_t *skel,
               apr_pool_t *scratch_pool);

static svn_error_t *
set_actual_props(apr_int64_t wc_id,
                 const char *local_relpath,
                 apr_hash_t *props,
                 svn_sqlite__db_t *db,
                 apr_pool_t *scratch_pool);

static svn_error_t *
insert_incomplete_children(svn_sqlite__db_t *sdb,
                           apr_int64_t wc_id,
                           const char *local_relpath,
                           apr_int64_t repos_id,
                           const char *repos_relpath,
                           svn_revnum_t revision,
                           const apr_array_header_t *children,
                           apr_int64_t op_depth,
                           apr_pool_t *scratch_pool);

static svn_error_t *
db_read_pristine_props(apr_hash_t **props,
                       svn_wc__db_wcroot_t *wcroot,
                       const char *local_relpath,
                       apr_pool_t *result_pool,
                       apr_pool_t *scratch_pool);

static svn_error_t *
read_info(svn_wc__db_status_t *status,
          svn_wc__db_kind_t *kind,
          svn_revnum_t *revision,
          const char **repos_relpath,
          apr_int64_t *repos_id,
          svn_revnum_t *changed_rev,
          apr_time_t *changed_date,
          const char **changed_author,
          svn_depth_t *depth,
          const svn_checksum_t **checksum,
          const char **target,
          const char **original_repos_relpath,
          apr_int64_t *original_repos_id,
          svn_revnum_t *original_revision,
          svn_wc__db_lock_t **lock,
          svn_filesize_t *recorded_size,
          apr_time_t *recorded_mod_time,
          const char **changelist,
          svn_boolean_t *conflicted,
          svn_boolean_t *op_root,
          svn_boolean_t *had_props,
          svn_boolean_t *props_mod,
          svn_boolean_t *have_base,
          svn_boolean_t *have_more_work,
          svn_boolean_t *have_work,
          svn_wc__db_wcroot_t *wcroot,
          const char *local_relpath,
          apr_pool_t *result_pool,
          apr_pool_t *scratch_pool);

static svn_error_t *
scan_addition(svn_wc__db_status_t *status,
              const char **op_root_relpath,
              const char **repos_relpath,
              apr_int64_t *repos_id,
              const char **original_repos_relpath,
              apr_int64_t *original_repos_id,
              svn_revnum_t *original_revision,
              svn_wc__db_wcroot_t *wcroot,
              const char *local_relpath,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool);

static svn_error_t *
scan_deletion(const char **base_del_relpath,
              const char **moved_to_relpath,
              const char **work_del_relpath,
              svn_wc__db_wcroot_t *wcroot,
              const char *local_relpath,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool);

static svn_error_t *
convert_to_working_status(svn_wc__db_status_t *working_status,
                          svn_wc__db_status_t status);

static svn_error_t *
wclock_owns_lock(svn_boolean_t *own_lock,
                 svn_wc__db_wcroot_t *wcroot,
                 const char *local_relpath,
                 svn_boolean_t exact,
                 apr_pool_t *scratch_pool);



/* Return the absolute path, in local path style, of LOCAL_RELPATH
   in WCROOT.  */
static const char *
path_for_error_message(const svn_wc__db_wcroot_t *wcroot,
                       const char *local_relpath,
                       apr_pool_t *result_pool)
{
  const char *local_abspath
    = svn_dirent_join(wcroot->abspath, local_relpath, result_pool);

  return svn_dirent_local_style(local_abspath, result_pool);
}


/* Return a file size from column SLOT of the SQLITE statement STMT, or
   SVN_INVALID_FILESIZE if the column value is NULL.  */
static svn_filesize_t
get_translated_size(svn_sqlite__stmt_t *stmt, int slot)
{
  if (svn_sqlite__column_is_null(stmt, slot))
    return SVN_INVALID_FILESIZE;
  return svn_sqlite__column_int64(stmt, slot);
}


/* Return a lock info structure constructed from the given columns of the
   SQLITE statement STMT, or return NULL if the token column value is null.  */
static svn_wc__db_lock_t *
lock_from_columns(svn_sqlite__stmt_t *stmt,
                  int col_token,
                  int col_owner,
                  int col_comment,
                  int col_date,
                  apr_pool_t *result_pool)
{
  svn_wc__db_lock_t *lock;

  if (svn_sqlite__column_is_null(stmt, col_token))
    {
      lock = NULL;
    }
  else
    {
      lock = apr_pcalloc(result_pool, sizeof(svn_wc__db_lock_t));
      lock->token = svn_sqlite__column_text(stmt, col_token, result_pool);
      lock->owner = svn_sqlite__column_text(stmt, col_owner, result_pool);
      lock->comment = svn_sqlite__column_text(stmt, col_comment, result_pool);
      lock->date = svn_sqlite__column_int64(stmt, col_date);
    }
  return lock;
}


/* */
static const char *
escape_sqlite_like(const char * const str, apr_pool_t *result_pool)
{
  char *result;
  const char *old_ptr;
  char *new_ptr;
  int len = 0;

  /* Count the number of extra characters we'll need in the escaped string.
     We could just use the worst case (double) value, but we'd still need to
     iterate over the string to get it's length.  So why not do something
     useful why iterating over it, and save some memory at the same time? */
  for (old_ptr = str; *old_ptr; ++old_ptr)
    {
      len++;
      if (*old_ptr == '%'
            || *old_ptr == '_'
            || *old_ptr == LIKE_ESCAPE_CHAR[0])
        len++;
    }

  result = apr_palloc(result_pool, len + 1);

  /* Now do the escaping. */
  for (old_ptr = str, new_ptr = result; *old_ptr; ++old_ptr, ++new_ptr)
    {
      if (*old_ptr == '%'
            || *old_ptr == '_'
            || *old_ptr == LIKE_ESCAPE_CHAR[0])
        *(new_ptr++) = LIKE_ESCAPE_CHAR[0];
      *new_ptr = *old_ptr;
    }
  *new_ptr = '\0';

  return result;
}


/* Return a string that can be used as the argument to a SQLite 'LIKE'
   operator, in order to match any path that is a child of LOCAL_RELPATH
   (at any depth below LOCAL_RELPATH), *excluding* LOCAL_RELPATH itself.
   LOCAL_RELPATH may be the empty string, in which case the result will
   match any path except the empty path.

   Allocate the result either statically or in RESULT_POOL.  */
static const char *construct_like_arg(const char *local_relpath,
                                      apr_pool_t *result_pool)
{
  if (local_relpath[0] == '\0')
    return "_%";

  return apr_pstrcat(result_pool,
                     escape_sqlite_like(local_relpath, result_pool),
                     "/%", (char *)NULL);
}


/* Look up REPOS_ID in SDB and set *REPOS_ROOT_URL and/or *REPOS_UUID to
   its root URL and UUID respectively.  If REPOS_ID is INVALID_REPOS_ID,
   use NULL for both URL and UUID.  Either or both output parameters may be
   NULL if not wanted.  */
static svn_error_t *
fetch_repos_info(const char **repos_root_url,
                 const char **repos_uuid,
                 svn_sqlite__db_t *sdb,
                 apr_int64_t repos_id,
                 apr_pool_t *result_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  if (!repos_root_url && !repos_uuid)
    return SVN_NO_ERROR;

  if (repos_id == INVALID_REPOS_ID)
    {
      if (repos_root_url)
        *repos_root_url = NULL;
      if (repos_uuid)
        *repos_uuid = NULL;
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                    STMT_SELECT_REPOSITORY_BY_ID));
  SVN_ERR(svn_sqlite__bindf(stmt, "i", repos_id));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    return svn_error_createf(SVN_ERR_WC_CORRUPT, svn_sqlite__reset(stmt),
                             _("No REPOSITORY table entry for id '%ld'"),
                             (long int)repos_id);

  if (repos_root_url)
    *repos_root_url = svn_sqlite__column_text(stmt, 0, result_pool);
  if (repos_uuid)
    *repos_uuid = svn_sqlite__column_text(stmt, 1, result_pool);

  return svn_error_return(svn_sqlite__reset(stmt));
}


/* Set *REPOS_ID, *REVISION and *REPOS_RELPATH from the
   given columns of the SQLITE statement STMT, or to NULL if the respective
   column value is null.  Any of the output parameters may be NULL if not
   required.  */
static svn_error_t *
repos_location_from_columns(apr_int64_t *repos_id,
                            svn_revnum_t *revision,
                            const char **repos_relpath,
                            svn_sqlite__stmt_t *stmt,
                            int col_repos_id,
                            int col_revision,
                            int col_repos_relpath,
                            apr_pool_t *result_pool)
{
  svn_error_t *err = SVN_NO_ERROR;

  if (repos_id)
    {
      /* Fetch repository information via REPOS_ID. */
      if (svn_sqlite__column_is_null(stmt, col_repos_id))
        *repos_id = INVALID_REPOS_ID;
      else
        *repos_id = svn_sqlite__column_int64(stmt, col_repos_id);
    }
  if (revision)
    {
      *revision = svn_sqlite__column_revnum(stmt, col_revision);
    }
  if (repos_relpath)
    {
      *repos_relpath = svn_sqlite__column_text(stmt, col_repos_relpath,
                                               result_pool);
    }

  return err;
}


/* Set *REPOS_ID and *REPOS_RELPATH to the BASE node of LOCAL_RELPATH.
   Either of REPOS_ID and REPOS_RELPATH may be NULL if not wanted.  */
static svn_error_t *
scan_upwards_for_repos(apr_int64_t *repos_id,
                       const char **repos_relpath,
                       const svn_wc__db_wcroot_t *wcroot,
                       const char *local_relpath,
                       apr_pool_t *result_pool,
                       apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(wcroot->sdb != NULL && wcroot->wc_id != UNKNOWN_WC_ID);
  SVN_ERR_ASSERT(repos_id != NULL || repos_relpath != NULL);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_SELECT_BASE_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (!have_row)
    {
      svn_error_t *err = svn_error_createf(
            SVN_ERR_WC_PATH_NOT_FOUND, NULL,
            _("The node '%s' was not found."),
            path_for_error_message(wcroot, local_relpath, scratch_pool));

      return svn_error_compose_create(err, svn_sqlite__reset(stmt));
    }

  SVN_ERR_ASSERT(!svn_sqlite__column_is_null(stmt, 0));
  SVN_ERR_ASSERT(!svn_sqlite__column_is_null(stmt, 1));

  if (repos_id)
    *repos_id = svn_sqlite__column_int64(stmt, 0);
  if (repos_relpath)
    *repos_relpath = svn_sqlite__column_text(stmt, 1, result_pool);
  return svn_sqlite__reset(stmt);
}


/* Get the statement given by STMT_IDX, and bind the appropriate wc_id and
   local_relpath based upon LOCAL_ABSPATH.  Store it in *STMT, and use
   SCRATCH_POOL for temporary allocations.

   Note: WC_ID and LOCAL_RELPATH must be arguments 1 and 2 in the statement. */
static svn_error_t *
get_statement_for_path(svn_sqlite__stmt_t **stmt,
                       svn_wc__db_t *db,
                       const char *local_abspath,
                       int stmt_idx,
                       apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_sqlite__get_statement(stmt, wcroot->sdb, stmt_idx));
  SVN_ERR(svn_sqlite__bindf(*stmt, "is", wcroot->wc_id, local_relpath));

  return SVN_NO_ERROR;
}


/* For a given REPOS_ROOT_URL/REPOS_UUID pair, return the existing REPOS_ID
   value. If one does not exist, then create a new one. */
static svn_error_t *
create_repos_id(apr_int64_t *repos_id,
                const char *repos_root_url,
                const char *repos_uuid,
                svn_sqlite__db_t *sdb,
                apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *get_stmt;
  svn_sqlite__stmt_t *insert_stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&get_stmt, sdb, STMT_SELECT_REPOSITORY));
  SVN_ERR(svn_sqlite__bindf(get_stmt, "s", repos_root_url));
  SVN_ERR(svn_sqlite__step(&have_row, get_stmt));

  if (have_row)
    {
      *repos_id = svn_sqlite__column_int64(get_stmt, 0);
      return svn_error_return(svn_sqlite__reset(get_stmt));
    }
  SVN_ERR(svn_sqlite__reset(get_stmt));

  /* NOTE: strictly speaking, there is a race condition between the
     above query and the insertion below. We're simply going to ignore
     that, as it means two processes are *modifying* the working copy
     at the same time, *and* new repositores are becoming visible.
     This is rare enough, let alone the miniscule chance of hitting
     this race condition. Further, simply failing out will leave the
     database in a consistent state, and the user can just re-run the
     failed operation. */

  SVN_ERR(svn_sqlite__get_statement(&insert_stmt, sdb,
                                    STMT_INSERT_REPOSITORY));
  SVN_ERR(svn_sqlite__bindf(insert_stmt, "ss", repos_root_url, repos_uuid));
  return svn_error_return(svn_sqlite__insert(repos_id, insert_stmt));
}


/* Initialize the baton with appropriate "blank" values. This allows the
   insertion function to leave certain columns null.  */
static void
blank_ibb(insert_base_baton_t *pibb)
{
  memset(pibb, 0, sizeof(*pibb));
  pibb->revision = SVN_INVALID_REVNUM;
  pibb->changed_rev = SVN_INVALID_REVNUM;
  pibb->depth = svn_depth_infinity;
  pibb->translated_size = SVN_INVALID_FILESIZE;
  pibb->repos_id = -1;
}


/* Extend any delete of the parent of LOCAL_RELPATH to LOCAL_RELPATH.

   Given a wc:

              0         1         2         3         4
              normal
   A          normal
   A/B        normal              normal
   A/B/C                          not-pres  normal
   A/B/C/D                                            normal

   That is checkout, delete A/B, copy a replacement A/B, delete copied
   child A/B/C, add replacement A/B/C, add A/B/C/D.

   Now an update that adds base nodes for A/B/C, A/B/C/D and A/B/C/D/E
   must extend the A/B deletion:

              0         1         2         3         4
              normal
   A          normal
   A/B        normal              normal
   A/B/C      normal              not-pres  normal
   A/B/C/D    normal              base-del            normal
   A/B/C/D/E  normal              base-del

   When adding a base node if the parent has a working node then the
   parent base is deleted and this must be extended to cover new base
   node.

   In the example above A/B/C/D and A/B/C/D/E are the nodes that get
   the extended delete, A/B/C is already deleted.
 */
static svn_error_t *
extend_parent_delete(svn_sqlite__db_t *sdb,
                     apr_int64_t wc_id,
                     const char *local_relpath,
                     apr_pool_t *scratch_pool)
{
  svn_boolean_t have_row;
  svn_sqlite__stmt_t *stmt;
  apr_int64_t parent_op_depth;
  const char *parent_relpath = svn_relpath_dirname(local_relpath, scratch_pool);

  SVN_ERR_ASSERT(local_relpath[0]);

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                    STMT_SELECT_LOWEST_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, parent_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (have_row)
    parent_op_depth = svn_sqlite__column_int64(stmt, 0);
  SVN_ERR(svn_sqlite__reset(stmt));
  if (have_row)
    {
      apr_int64_t op_depth;

      SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
      if (have_row)
        op_depth = svn_sqlite__column_int64(stmt, 0);
      SVN_ERR(svn_sqlite__reset(stmt));
      if (!have_row || parent_op_depth < op_depth)
        {
          SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                          STMT_INSERT_WORKING_NODE_FROM_BASE));
          SVN_ERR(svn_sqlite__bindf(stmt, "isit", wc_id,
                                    local_relpath, parent_op_depth,
                                    presence_map,
                                    svn_wc__db_status_base_deleted));
          SVN_ERR(svn_sqlite__update(NULL, stmt));
        }
    }

  return SVN_NO_ERROR;
}


/* This is the reverse of extend_parent_delete.

   When removing a base node if the parent has a working node then the
   parent base and this node are both deleted and so the delete of
   this node must be removed.
 */
static svn_error_t *
retract_parent_delete(svn_sqlite__db_t *sdb,
                      apr_int64_t wc_id,
                      const char *local_relpath,
                      apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                    STMT_DELETE_LOWEST_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}



/* */
static svn_error_t *
insert_base_node(void *baton,
                 svn_wc__db_wcroot_t *wcroot,
                 const char *local_relpath,
                 apr_pool_t *scratch_pool)
{
  const insert_base_baton_t *pibb = baton;
  apr_int64_t repos_id = pibb->repos_id;
  svn_sqlite__stmt_t *stmt;
  /* The directory at the WCROOT has a NULL parent_relpath. Otherwise,
     bind the appropriate parent_relpath. */
  const char *parent_relpath =
    (*local_relpath == '\0') ? NULL
    : svn_relpath_dirname(local_relpath, scratch_pool);

  if (pibb->repos_id == -1)
    SVN_ERR(create_repos_id(&repos_id, pibb->repos_root_url, pibb->repos_uuid,
                            wcroot->sdb, scratch_pool));

  SVN_ERR_ASSERT(repos_id != INVALID_REPOS_ID);
  SVN_ERR_ASSERT(pibb->repos_relpath != NULL);

  /* ### we can't handle this right now  */
  SVN_ERR_ASSERT(pibb->conflict == NULL);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_INSERT_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "isisisr"
                            "tstr"               /* 8 - 11 */
                            "isnnnnns",          /* 12 - 19 */
                            wcroot->wc_id,       /* 1 */
                            local_relpath,       /* 2 */
                            (apr_int64_t)0, /* op_depth is 0 for base */
                            parent_relpath,      /* 4 */
                            repos_id,
                            pibb->repos_relpath,
                            pibb->revision,
                            presence_map, pibb->status, /* 8 */
                            (pibb->kind == svn_wc__db_kind_dir) ? /* 9 */
                               svn_depth_to_word(pibb->depth) : NULL,
                            kind_map, pibb->kind, /* 10 */
                            pibb->changed_rev,    /* 11 */
                            pibb->changed_date,   /* 12 */
                            pibb->changed_author, /* 13 */
                            (pibb->kind == svn_wc__db_kind_symlink) ?
                                pibb->target : NULL)); /* 19 */

  if (pibb->kind == svn_wc__db_kind_file)
    {
      SVN_ERR(svn_sqlite__bind_checksum(stmt, 14, pibb->checksum,
                                        scratch_pool));
      if (pibb->translated_size != SVN_INVALID_FILESIZE)
        SVN_ERR(svn_sqlite__bind_int64(stmt, 16, pibb->translated_size));
    }

  SVN_ERR(svn_sqlite__bind_properties(stmt, 15, pibb->props,
                                      scratch_pool));
  if (pibb->dav_cache)
    SVN_ERR(svn_sqlite__bind_properties(stmt, 18, pibb->dav_cache,
                                        scratch_pool));

  SVN_ERR(svn_sqlite__insert(NULL, stmt));

  if (pibb->update_actual_props)
    {
      /* Cast away const, to allow calling property helpers */
      apr_hash_t *base_props = (apr_hash_t *)pibb->props;
      apr_hash_t *new_actual_props = (apr_hash_t *)pibb->new_actual_props;

      if (base_props != NULL
          && new_actual_props != NULL
          && (apr_hash_count(base_props) == apr_hash_count(new_actual_props)))
        {
          apr_array_header_t *diffs;

          SVN_ERR(svn_prop_diffs(&diffs, (apr_hash_t *)new_actual_props,
                                 (apr_hash_t *)pibb->props, scratch_pool));

          if (diffs->nelts == 0)
            new_actual_props = NULL;
        }

      SVN_ERR(set_actual_props(wcroot->wc_id, local_relpath, new_actual_props,
                               wcroot->sdb, scratch_pool));
    }

  if (pibb->kind == svn_wc__db_kind_dir && pibb->children)
    SVN_ERR(insert_incomplete_children(wcroot->sdb, wcroot->wc_id,
                                       local_relpath,
                                       repos_id,
                                       pibb->repos_relpath,
                                       pibb->revision,
                                       pibb->children,
                                       0 /* BASE */,
                                       scratch_pool));

  if (parent_relpath)
    SVN_ERR(extend_parent_delete(wcroot->sdb, wcroot->wc_id, local_relpath,
                                 scratch_pool));

  SVN_ERR(add_work_items(wcroot->sdb, pibb->work_items, scratch_pool));

  return SVN_NO_ERROR;
}


static void
blank_iwb(insert_working_baton_t *piwb)
{
  memset(piwb, 0, sizeof(*piwb));
  piwb->changed_rev = SVN_INVALID_REVNUM;
  piwb->depth = svn_depth_infinity;

  /* ORIGINAL_REPOS_ID and ORIGINAL_REVNUM could use some kind of "nil"
     value, but... meh. We'll avoid them if ORIGINAL_REPOS_RELPATH==NULL.  */
}


/* Insert a row in NODES for each (const char *) child name in CHILDREN,
   whose parent directory is LOCAL_RELPATH, at op_depth=OP_DEPTH.  Set each
   child's presence to 'incomplete', kind to 'unknown', repos_id to REPOS_ID,
   repos_path by appending the child name to REPOS_PATH, and revision to
   REVISION (which should match the parent's revision).

   If REPOS_ID is INVALID_REPOS_ID, set each child's repos_id to null. */
static svn_error_t *
insert_incomplete_children(svn_sqlite__db_t *sdb,
                           apr_int64_t wc_id,
                           const char *local_relpath,
                           apr_int64_t repos_id,
                           const char *repos_path,
                           svn_revnum_t revision,
                           const apr_array_header_t *children,
                           apr_int64_t op_depth,
                           apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  int i;

  SVN_ERR_ASSERT(repos_path != NULL || op_depth > 0);
  SVN_ERR_ASSERT((repos_id != INVALID_REPOS_ID)
                 == (repos_path != NULL));

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_INSERT_NODE));

  for (i = children->nelts; i--; )
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);

      SVN_ERR(svn_sqlite__bindf(stmt, "isisnnrsns",
                                wc_id,
                                svn_relpath_join(local_relpath, name,
                                                 scratch_pool),
                                op_depth,
                                local_relpath,
                                revision,
                                "incomplete", /* 8, presence */
                                "unknown"));  /* 10, kind */

      if (repos_id != INVALID_REPOS_ID)
        {
          SVN_ERR(svn_sqlite__bind_int64(stmt, 5, repos_id));
          SVN_ERR(svn_sqlite__bind_text(stmt, 6,
                                        svn_relpath_join(repos_path, name,
                                                         scratch_pool)));
        }

      SVN_ERR(svn_sqlite__insert(NULL, stmt));
    }

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
insert_working_node(void *baton,
                    svn_wc__db_wcroot_t *wcroot,
                    const char *local_relpath,
                    apr_pool_t *scratch_pool)
{
  const insert_working_baton_t *piwb = baton;
  const char *parent_relpath;
  svn_sqlite__stmt_t *stmt;

  SVN_ERR_ASSERT(piwb->op_depth > 0);

  /* We cannot insert a WORKING_NODE row at the wcroot.  */
  SVN_ERR_ASSERT(*local_relpath != '\0');
  parent_relpath = svn_relpath_dirname(local_relpath, scratch_pool);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_INSERT_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "isisnnntstrisn"
                "nnnn" /* properties translated_size last_mod_time dav_cache */
                "s",
                wcroot->wc_id, local_relpath,
                piwb->op_depth,
                parent_relpath,
                presence_map, piwb->presence,
                (piwb->kind == svn_wc__db_kind_dir)
                            ? svn_depth_to_word(piwb->depth) : NULL,
                kind_map, piwb->kind,
                piwb->changed_rev,
                piwb->changed_date,
                piwb->changed_author,
                (piwb->kind == svn_wc__db_kind_symlink)
                            ? piwb->target : NULL));


  if (piwb->kind == svn_wc__db_kind_file)
    {
      SVN_ERR(svn_sqlite__bind_checksum(stmt, 14, piwb->checksum,
                                        scratch_pool));
    }
  else if (piwb->kind == svn_wc__db_kind_symlink)
    {
      /* Note: incomplete nodes may have a NULL target.  */
      if (piwb->target)
        SVN_ERR(svn_sqlite__bind_text(stmt, 19, piwb->target));
    }

  if (piwb->original_repos_relpath != NULL)
    {
      SVN_ERR(svn_sqlite__bind_int64(stmt, 5, piwb->original_repos_id));
      SVN_ERR(svn_sqlite__bind_text(stmt, 6, piwb->original_repos_relpath));
      SVN_ERR(svn_sqlite__bind_int64(stmt, 7, piwb->original_revnum));
    }


  SVN_ERR(svn_sqlite__bind_properties(stmt, 15, piwb->props, scratch_pool));

  SVN_ERR(svn_sqlite__insert(NULL, stmt));

  /* Insert incomplete children, if specified.
     The children are part of the same op and so have the same op_depth.
     (The only time we'd want a different depth is during a recursive
     simple add, but we never insert children here during a simple add.) */
  if (piwb->kind == svn_wc__db_kind_dir && piwb->children)
    SVN_ERR(insert_incomplete_children(wcroot->sdb, wcroot->wc_id,
                                       local_relpath,
                                       INVALID_REPOS_ID /* inherit repos_id */,
                                       NULL /* inherit repos_path */,
                                       piwb->original_revnum,
                                       piwb->children,
                                       piwb->op_depth,
                                       scratch_pool));

  SVN_ERR(add_work_items(wcroot->sdb, piwb->work_items, scratch_pool));

  if (piwb->not_present_op_depth > 0
      && piwb->not_present_op_depth < piwb->op_depth)
    {
      /* And also insert a not-present node to tell the commit processing that
         a child of the parent node was not copied. */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_INSERT_NODE));
          
      SVN_ERR(svn_sqlite__bindf(stmt, "isisisrtnt", 
                                wcroot->wc_id, local_relpath,
                                piwb->not_present_op_depth, parent_relpath,
                                piwb->original_repos_id,
                                piwb->original_repos_relpath,
                                piwb->original_revnum,
                                presence_map, svn_wc__db_status_not_present,
                                /* NULL */
                                kind_map, piwb->kind));

      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  return SVN_NO_ERROR;
}


/* Each name is allocated in RESULT_POOL and stored into CHILDREN as a key
   pointed to the same name.  */
static svn_error_t *
add_children_to_hash(apr_hash_t *children,
                     int stmt_idx,
                     svn_sqlite__db_t *sdb,
                     apr_int64_t wc_id,
                     const char *parent_relpath,
                     apr_pool_t *result_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, stmt_idx));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, parent_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  while (have_row)
    {
      const char *child_relpath = svn_sqlite__column_text(stmt, 0, NULL);
      const char *name = svn_relpath_basename(child_relpath, result_pool);

      apr_hash_set(children, name, APR_HASH_KEY_STRING, name);

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }

  return svn_sqlite__reset(stmt);
}


/* Set *CHILDREN to a new array of the (const char *) basenames of the
   immediate children, whatever their status, of the working node at
   LOCAL_RELPATH. */
static svn_error_t *
gather_children2(const apr_array_header_t **children,
                 svn_wc__db_wcroot_t *wcroot,
                 const char *local_relpath,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool)
{
  apr_hash_t *names_hash = apr_hash_make(scratch_pool);
  apr_array_header_t *names_array;

  /* All of the names get allocated in RESULT_POOL.  It
     appears to be faster to use the hash to remove duplicates than to
     use DISTINCT in the SQL query. */
  SVN_ERR(add_children_to_hash(names_hash, STMT_SELECT_WORKING_CHILDREN,
                               wcroot->sdb, wcroot->wc_id,
                               local_relpath, result_pool));

  SVN_ERR(svn_hash_keys(&names_array, names_hash, result_pool));
  *children = names_array;
  return SVN_NO_ERROR;
}

/* Return in *CHILDREN all of the children of the directory LOCAL_RELPATH,
   of any status, in all op-depths in the NODES table. */
static svn_error_t *
gather_children(const apr_array_header_t **children,
                svn_wc__db_wcroot_t *wcroot,
                const char *local_relpath,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  apr_hash_t *names_hash = apr_hash_make(scratch_pool);
  apr_array_header_t *names_array;

  /* All of the names get allocated in RESULT_POOL.  It
     appears to be faster to use the hash to remove duplicates than to
     use DISTINCT in the SQL query. */
  SVN_ERR(add_children_to_hash(names_hash, STMT_SELECT_NODE_CHILDREN,
                               wcroot->sdb, wcroot->wc_id,
                               local_relpath, result_pool));

  SVN_ERR(svn_hash_keys(&names_array, names_hash, result_pool));
  *children = names_array;
  return SVN_NO_ERROR;
}


/* Set *CHILDREN to a new array of (const char *) names of the children of
   the repository directory corresponding to the (working) path
   WCROOT:LOCAL_RELPATH - that is, only the children that are at the same
   op-depth as their parent. */
static svn_error_t *
gather_repo_children(const apr_array_header_t **children,
                     svn_wc__db_wcroot_t *wcroot,
                     const char *local_relpath,
                     apr_int64_t op_depth,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  apr_array_header_t *result
    = apr_array_make(result_pool, 0, sizeof(const char *));
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_OP_DEPTH_CHILDREN));
  SVN_ERR(svn_sqlite__bindf(stmt, "isi", wcroot->wc_id, local_relpath,
                            op_depth));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  while (have_row)
    {
      const char *child_relpath = svn_sqlite__column_text(stmt, 0, NULL);

      /* Allocate the name in RESULT_POOL so we won't have to copy it. */
      APR_ARRAY_PUSH(result, const char *)
        = svn_relpath_basename(child_relpath, result_pool);

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  *children = result;
  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
flush_entries(svn_wc__db_wcroot_t *wcroot,
              const char *local_abspath,
              apr_pool_t *scratch_pool)
{
  svn_wc_adm_access_t *adm_access;
  const char *parent_abspath;

  if (apr_hash_count(wcroot->access_cache) == 0)
    return SVN_NO_ERROR;

  adm_access = apr_hash_get(wcroot->access_cache, local_abspath,
                            APR_HASH_KEY_STRING);

  if (adm_access)
    svn_wc__adm_access_set_entries(adm_access, NULL);

  /* We're going to be overly aggressive here and just flush the parent
     without doing much checking.  This may hurt performance for
     legacy API consumers, but that's not our problem. :) */
  parent_abspath = svn_dirent_dirname(local_abspath, scratch_pool);
  adm_access = apr_hash_get(wcroot->access_cache, parent_abspath,
                            APR_HASH_KEY_STRING);

  if (adm_access)
    svn_wc__adm_access_set_entries(adm_access, NULL);

  return SVN_NO_ERROR;
}


/* Add a single WORK_ITEM into the given SDB's WORK_QUEUE table. This does
   not perform its work within a transaction, assuming the caller will
   manage that.  */
static svn_error_t *
add_single_work_item(svn_sqlite__db_t *sdb,
                     const svn_skel_t *work_item,
                     apr_pool_t *scratch_pool)
{
  svn_stringbuf_t *serialized;
  svn_sqlite__stmt_t *stmt;

  serialized = svn_skel__unparse(work_item, scratch_pool);
  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_INSERT_WORK_ITEM));
  SVN_ERR(svn_sqlite__bind_blob(stmt, 1, serialized->data, serialized->len));
  return svn_error_return(svn_sqlite__insert(NULL, stmt));
}


/* Add work item(s) to the given SDB. Also see add_one_work_item(). This
   SKEL is usually passed to the various wc_db operation functions. It may
   be NULL, indicating no additional work items are needed, it may be a
   single work item, or it may be a list of work items.  */
static svn_error_t *
add_work_items(svn_sqlite__db_t *sdb,
               const svn_skel_t *skel,
               apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool;

  /* Maybe there are no work items to insert.  */
  if (skel == NULL)
    return SVN_NO_ERROR;

  /* Should have a list.  */
  SVN_ERR_ASSERT(!skel->is_atom);

  /* Is the list a single work item? Or a list of work items?  */
  if (SVN_WC__SINGLE_WORK_ITEM(skel))
    return svn_error_return(add_single_work_item(sdb, skel, scratch_pool));

  /* SKEL is a list-of-lists, aka list of work items.  */

  iterpool = svn_pool_create(scratch_pool);
  for (skel = skel->children; skel; skel = skel->next)
    {
      svn_pool_clear(iterpool);

      SVN_ERR(add_single_work_item(sdb, skel, iterpool));
    }
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


/* Determine which trees' nodes exist for a given WC_ID and LOCAL_RELPATH
   in the specified SDB.  */
static svn_error_t *
which_trees_exist(svn_boolean_t *any_exists,
                  svn_boolean_t *base_exists,
                  svn_boolean_t *working_exists,
                  svn_sqlite__db_t *sdb,
                  apr_int64_t wc_id,
                  const char *local_relpath)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  if (base_exists)
    *base_exists = FALSE;
  if (working_exists)
    *working_exists = FALSE;

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                    STMT_DETERMINE_WHICH_TREES_EXIST));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (any_exists)
    *any_exists = have_row;

  while (have_row)
    {
      apr_int64_t op_depth = svn_sqlite__column_int64(stmt, 0);

      if (op_depth == 0)
        {
          if (base_exists)
            *base_exists = TRUE;
          if (!working_exists)
            break;
        }
      else if (op_depth > 0)
        {
          if (working_exists)
            *working_exists = TRUE;
          break;
        }

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }

  return svn_error_return(svn_sqlite__reset(stmt));
}


/* */
static svn_error_t *
create_db(svn_sqlite__db_t **sdb,
          apr_int64_t *repos_id,
          apr_int64_t *wc_id,
          const char *dir_abspath,
          const char *repos_root_url,
          const char *repos_uuid,
          const char *sdb_fname,
          apr_pool_t *result_pool,
          apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_wc__db_util_open_db(sdb, dir_abspath, sdb_fname,
                                  svn_sqlite__mode_rwcreate, result_pool,
                                  scratch_pool));

  /* Create the database's schema.  */
  SVN_ERR(svn_sqlite__exec_statements(*sdb, STMT_CREATE_SCHEMA));
  SVN_ERR(svn_sqlite__exec_statements(*sdb, STMT_CREATE_NODES));
  SVN_ERR(svn_sqlite__exec_statements(*sdb, STMT_CREATE_NODES_TRIGGERS));

  /* Insert the repository. */
  SVN_ERR(create_repos_id(repos_id, repos_root_url, repos_uuid, *sdb,
                          scratch_pool));

  /* Insert the wcroot. */
  /* ### Right now, this just assumes wc metadata is being stored locally. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, *sdb, STMT_INSERT_WCROOT));
  SVN_ERR(svn_sqlite__insert(wc_id, stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_init(svn_wc__db_t *db,
                const char *local_abspath,
                const char *repos_relpath,
                const char *repos_root_url,
                const char *repos_uuid,
                svn_revnum_t initial_rev,
                svn_depth_t depth,
                apr_pool_t *scratch_pool)
{
  svn_sqlite__db_t *sdb;
  apr_int64_t repos_id;
  apr_int64_t wc_id;
  svn_wc__db_wcroot_t *wcroot;
  insert_base_baton_t ibb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(repos_relpath != NULL);
  SVN_ERR_ASSERT(depth == svn_depth_empty
                 || depth == svn_depth_files
                 || depth == svn_depth_immediates
                 || depth == svn_depth_infinity);

  /* ### REPOS_ROOT_URL and REPOS_UUID may be NULL. ... more doc: tbd  */

  /* Create the SDB and insert the basic rows.  */
  SVN_ERR(create_db(&sdb, &repos_id, &wc_id, local_abspath, repos_root_url,
                    repos_uuid, SDB_FILE, db->state_pool, scratch_pool));

  /* Create the WCROOT for this directory.  */
  SVN_ERR(svn_wc__db_pdh_create_wcroot(&wcroot,
                        apr_pstrdup(db->state_pool, local_abspath),
                        sdb, wc_id, FORMAT_FROM_SDB,
                        FALSE /* auto-upgrade */,
                        FALSE /* enforce_empty_wq */,
                        db->state_pool, scratch_pool));

  /* The WCROOT is complete. Stash it into DB.  */
  apr_hash_set(db->dir_data, wcroot->abspath, APR_HASH_KEY_STRING, wcroot);

  blank_ibb(&ibb);

  if (initial_rev > 0)
    ibb.status = svn_wc__db_status_incomplete;
  else
    ibb.status = svn_wc__db_status_normal;
  ibb.kind = svn_wc__db_kind_dir;
  ibb.repos_id = repos_id;
  ibb.repos_relpath = repos_relpath;
  ibb.revision = initial_rev;

  /* ### what about the children?  */
  ibb.children = NULL;
  ibb.depth = depth;

  /* ### no children, conflicts, or work items to install in a txn... */

  return svn_error_return(insert_base_node(&ibb, wcroot, "", scratch_pool));
}


svn_error_t *
svn_wc__db_to_relpath(const char **local_relpath,
                      svn_wc__db_t *db,
                      const char *wri_abspath,
                      const char *local_abspath,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &relpath, db,
                              wri_abspath, result_pool, scratch_pool));

  /* This function is indirectly called from the upgrade code, so we
     can't verify the wcroot here. Just check that it is not NULL */
  SVN_ERR_ASSERT(wcroot != NULL);

  if (svn_dirent_is_ancestor(wcroot->abspath, local_abspath))
    {
      *local_relpath = apr_pstrdup(result_pool,
                                   svn_dirent_skip_ancestor(wcroot->abspath,
                                                            local_abspath));
    }
  else
    /* Probably moving from $TMP. Should we allow this? */
    *local_relpath = apr_pstrdup(result_pool, local_abspath);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_from_relpath(const char **local_abspath,
                        svn_wc__db_t *db,
                        const char *wri_abspath,
                        const char *local_relpath,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *unused_relpath;
#if 0
  SVN_ERR_ASSERT(svn_relpath_is_canonical(local_relpath, scratch_pool));
#endif

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &unused_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  *local_abspath = svn_dirent_join(wcroot->abspath,
                                   local_relpath,
                                   result_pool);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_get_wcroot(const char **wcroot_abspath,
                      svn_wc__db_t *db,
                      const char *wri_abspath,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *unused_relpath;

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &unused_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));

  /* Can't use VERIFY_USABLE_WCROOT, as this should be usable to detect
     where call upgrade */

  if (wcroot == NULL)
    return svn_error_createf(SVN_ERR_WC_NOT_WORKING_COPY, NULL,
                             _("The node '%s' is not in a workingcopy."),
                             svn_dirent_local_style(wri_abspath,
                                                    scratch_pool));

  *wcroot_abspath = apr_pstrdup(result_pool, wcroot->abspath);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_base_add_directory(svn_wc__db_t *db,
                              const char *local_abspath,
                              const char *repos_relpath,
                              const char *repos_root_url,
                              const char *repos_uuid,
                              svn_revnum_t revision,
                              const apr_hash_t *props,
                              svn_revnum_t changed_rev,
                              apr_time_t changed_date,
                              const char *changed_author,
                              const apr_array_header_t *children,
                              svn_depth_t depth,
                              apr_hash_t *dav_cache,
                              const svn_skel_t *conflict,
                              svn_boolean_t update_actual_props,
                              apr_hash_t *new_actual_props,
                              const svn_skel_t *work_items,
                              apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  insert_base_baton_t ibb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(repos_relpath != NULL);
  SVN_ERR_ASSERT(svn_uri_is_canonical(repos_root_url, scratch_pool));
  SVN_ERR_ASSERT(repos_uuid != NULL);
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(revision));
  SVN_ERR_ASSERT(props != NULL);
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(changed_rev));
#if 0
  SVN_ERR_ASSERT(children != NULL);
#endif

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  blank_ibb(&ibb);

  /* Calculate repos_id in insert_base_node() to avoid extra transaction */
  ibb.repos_root_url = repos_root_url;
  ibb.repos_uuid = repos_uuid;

  ibb.status = svn_wc__db_status_normal;
  ibb.kind = svn_wc__db_kind_dir;
  ibb.repos_relpath = repos_relpath;
  ibb.revision = revision;

  ibb.props = props;
  ibb.changed_rev = changed_rev;
  ibb.changed_date = changed_date;
  ibb.changed_author = changed_author;

  ibb.children = children;
  ibb.depth = depth;

  ibb.dav_cache = dav_cache;
  ibb.conflict = conflict;
  ibb.work_items = work_items;

  if (update_actual_props)
    {
      ibb.update_actual_props = TRUE;
      ibb.new_actual_props = new_actual_props;
    }

  /* Insert the directory and all its children transactionally.

     Note: old children can stick around, even if they are no longer present
     in this directory's revision.  */
  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, insert_base_node, &ibb,
                              scratch_pool));

  /* ### worry about flushing child subdirs?  */
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_base_add_file(svn_wc__db_t *db,
                         const char *local_abspath,
                         const char *repos_relpath,
                         const char *repos_root_url,
                         const char *repos_uuid,
                         svn_revnum_t revision,
                         const apr_hash_t *props,
                         svn_revnum_t changed_rev,
                         apr_time_t changed_date,
                         const char *changed_author,
                         const svn_checksum_t *checksum,
                         svn_filesize_t translated_size,
                         apr_hash_t *dav_cache,
                         const svn_skel_t *conflict,
                         svn_boolean_t update_actual_props,
                         apr_hash_t *new_actual_props,
                         const svn_skel_t *work_items,
                         apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  insert_base_baton_t ibb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(repos_relpath != NULL);
  SVN_ERR_ASSERT(svn_uri_is_canonical(repos_root_url, scratch_pool));
  SVN_ERR_ASSERT(repos_uuid != NULL);
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(revision));
  SVN_ERR_ASSERT(props != NULL);
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(changed_rev));
  SVN_ERR_ASSERT(checksum != NULL);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  blank_ibb(&ibb);

  /* Calculate repos_id in insert_base_node() to avoid extra transaction */
  ibb.repos_root_url = repos_root_url;
  ibb.repos_uuid = repos_uuid;

  ibb.status = svn_wc__db_status_normal;
  ibb.kind = svn_wc__db_kind_file;
  ibb.repos_relpath = repos_relpath;
  ibb.revision = revision;

  ibb.props = props;
  ibb.changed_rev = changed_rev;
  ibb.changed_date = changed_date;
  ibb.changed_author = changed_author;

  ibb.checksum = checksum;
  ibb.translated_size = translated_size;

  ibb.dav_cache = dav_cache;
  ibb.conflict = conflict;
  ibb.work_items = work_items;

  if (update_actual_props)
    {
      ibb.update_actual_props = TRUE;
      ibb.new_actual_props = new_actual_props;
    }


  /* ### hmm. if this used to be a directory, we should remove children.
     ### or maybe let caller deal with that, if there is a possibility
     ### of a node kind change (rather than eat an extra lookup here).  */

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, insert_base_node, &ibb,
                              scratch_pool));

  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_base_add_symlink(svn_wc__db_t *db,
                            const char *local_abspath,
                            const char *repos_relpath,
                            const char *repos_root_url,
                            const char *repos_uuid,
                            svn_revnum_t revision,
                            const apr_hash_t *props,
                            svn_revnum_t changed_rev,
                            apr_time_t changed_date,
                            const char *changed_author,
                            const char *target,
                            apr_hash_t *dav_cache,
                            const svn_skel_t *conflict,
                            svn_boolean_t update_actual_props,
                            apr_hash_t *new_actual_props,
                            const svn_skel_t *work_items,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  insert_base_baton_t ibb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(repos_relpath != NULL);
  SVN_ERR_ASSERT(svn_uri_is_canonical(repos_root_url, scratch_pool));
  SVN_ERR_ASSERT(repos_uuid != NULL);
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(revision));
  SVN_ERR_ASSERT(props != NULL);
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(changed_rev));
  SVN_ERR_ASSERT(target != NULL);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  blank_ibb(&ibb);

  /* Calculate repos_id in insert_base_node() to avoid extra transaction */
  ibb.repos_root_url = repos_root_url;
  ibb.repos_uuid = repos_uuid;

  ibb.status = svn_wc__db_status_normal;
  ibb.kind = svn_wc__db_kind_symlink;
  ibb.repos_relpath = repos_relpath;
  ibb.revision = revision;

  ibb.props = props;
  ibb.changed_rev = changed_rev;
  ibb.changed_date = changed_date;
  ibb.changed_author = changed_author;

  ibb.target = target;

  ibb.dav_cache = dav_cache;
  ibb.conflict = conflict;
  ibb.work_items = work_items;

  if (update_actual_props)
    {
      ibb.update_actual_props = TRUE;
      ibb.new_actual_props = new_actual_props;
    }

  /* ### hmm. if this used to be a directory, we should remove children.
     ### or maybe let caller deal with that, if there is a possibility
     ### of a node kind change (rather than eat an extra lookup here).  */

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, insert_base_node, &ibb,
                              scratch_pool));

  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));
  return SVN_NO_ERROR;
}


static svn_error_t *
add_absent_excluded_not_present_node(svn_wc__db_t *db,
                                     const char *local_abspath,
                                     const char *repos_relpath,
                                     const char *repos_root_url,
                                     const char *repos_uuid,
                                     svn_revnum_t revision,
                                     svn_wc__db_kind_t kind,
                                     svn_wc__db_status_t status,
                                     const svn_skel_t *conflict,
                                     const svn_skel_t *work_items,
                                     apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  insert_base_baton_t ibb;
  const char *dir_abspath, *name;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(repos_relpath != NULL);
  SVN_ERR_ASSERT(svn_uri_is_canonical(repos_root_url, scratch_pool));
  SVN_ERR_ASSERT(repos_uuid != NULL);
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(revision));
  SVN_ERR_ASSERT(status == svn_wc__db_status_absent
                 || status == svn_wc__db_status_excluded
                 || status == svn_wc__db_status_not_present);

  /* These absent presence nodes are only useful below a parent node that is
     present. To avoid problems with working copies obstructing the child
     we calculate the wcroot and local_relpath of the parent and then add
     our own relpath. */

  svn_dirent_split(&dir_abspath, &name, local_abspath, scratch_pool);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              dir_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  local_relpath = svn_relpath_join(local_relpath, name, scratch_pool);

  blank_ibb(&ibb);

  /* Calculate repos_id in insert_base_node() to avoid extra transaction */
  ibb.repos_root_url = repos_root_url;
  ibb.repos_uuid = repos_uuid;

  ibb.status = status;
  ibb.kind = kind;
  ibb.repos_relpath = repos_relpath;
  ibb.revision = revision;

  /* Depending upon KIND, any of these might get used. */
  ibb.children = NULL;
  ibb.depth = svn_depth_unknown;
  ibb.checksum = NULL;
  ibb.translated_size = SVN_INVALID_FILESIZE;
  ibb.target = NULL;

  ibb.conflict = conflict;
  ibb.work_items = work_items;

  /* ### hmm. if this used to be a directory, we should remove children.
     ### or maybe let caller deal with that, if there is a possibility
     ### of a node kind change (rather than eat an extra lookup here).  */

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, insert_base_node, &ibb,
                              scratch_pool));

  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_base_add_absent_node(svn_wc__db_t *db,
                                const char *local_abspath,
                                const char *repos_relpath,
                                const char *repos_root_url,
                                const char *repos_uuid,
                                svn_revnum_t revision,
                                svn_wc__db_kind_t kind,
                                svn_wc__db_status_t status,
                                const svn_skel_t *conflict,
                                const svn_skel_t *work_items,
                                apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(status == svn_wc__db_status_absent
                 || status == svn_wc__db_status_excluded);

  return add_absent_excluded_not_present_node(
    db, local_abspath, repos_relpath, repos_root_url, repos_uuid, revision,
    kind, status, conflict, work_items, scratch_pool);
}


svn_error_t *
svn_wc__db_base_add_not_present_node(svn_wc__db_t *db,
                                     const char *local_abspath,
                                     const char *repos_relpath,
                                     const char *repos_root_url,
                                     const char *repos_uuid,
                                     svn_revnum_t revision,
                                     svn_wc__db_kind_t kind,
                                     const svn_skel_t *conflict,
                                     const svn_skel_t *work_items,
                                     apr_pool_t *scratch_pool)
{
  return add_absent_excluded_not_present_node(
    db, local_abspath, repos_relpath, repos_root_url, repos_uuid, revision,
    kind, svn_wc__db_status_not_present, conflict, work_items, scratch_pool);
}


/* This implements svn_wc__db_txn_callback_t */
static svn_error_t *
db_base_remove(void *baton,
               svn_wc__db_wcroot_t *wcroot,
               const char *local_relpath,
               apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_BASE_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step_done(stmt));

  SVN_ERR(retract_parent_delete(wcroot->sdb, wcroot->wc_id, local_relpath,
                                scratch_pool));

  /* If there is no working node then any actual node must be deleted,
     unless it marks a conflict */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));
  if (!have_row)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_ACTUAL_NODE_WITHOUT_CONFLICT));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_base_remove(svn_wc__db_t *db,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, db_base_remove, NULL,
                              scratch_pool));

  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


/* Like svn_wc__db_base_get_info(), but taking WCROOT+LOCAL_RELPATH instead of
   DB+LOCAL_ABSPATH and outputting REPOS_ID instead of URL+UUID. */
static svn_error_t *
base_get_info(svn_wc__db_status_t *status,
              svn_wc__db_kind_t *kind,
              svn_revnum_t *revision,
              const char **repos_relpath,
              apr_int64_t *repos_id,
              svn_revnum_t *changed_rev,
              apr_time_t *changed_date,
              const char **changed_author,
              svn_depth_t *depth,
              const svn_checksum_t **checksum,
              const char **target,
              svn_wc__db_lock_t **lock,
              svn_filesize_t *recorded_size,
              apr_time_t *recorded_mod_time,
              svn_boolean_t *had_props,
              svn_boolean_t *update_root,
              svn_boolean_t *needs_full_update,
              svn_wc__db_wcroot_t *wcroot,
              const char *local_relpath,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  svn_error_t *err = SVN_NO_ERROR;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    lock ? STMT_SELECT_BASE_NODE_WITH_LOCK
                                         : STMT_SELECT_BASE_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (have_row)
    {
      svn_wc__db_kind_t node_kind = svn_sqlite__column_token(stmt, 3,
                                                             kind_map);

      if (kind)
        {
          *kind = node_kind;
        }
      if (status)
        {
          *status = svn_sqlite__column_token(stmt, 2, presence_map);
        }
      err = repos_location_from_columns(repos_id, revision, repos_relpath,
                                        stmt, 0, 4, 1, result_pool);
      SVN_ERR_ASSERT(!repos_id || *repos_id != INVALID_REPOS_ID);
      SVN_ERR_ASSERT(!repos_relpath || *repos_relpath);
      if (lock)
        {
          *lock = lock_from_columns(stmt, 15, 16, 17, 18, result_pool);
        }
      if (changed_rev)
        {
          *changed_rev = svn_sqlite__column_revnum(stmt, 7);
        }
      if (changed_date)
        {
          *changed_date = svn_sqlite__column_int64(stmt, 8);
        }
      if (changed_author)
        {
          /* Result may be NULL. */
          *changed_author = svn_sqlite__column_text(stmt, 9, result_pool);
        }
      if (recorded_mod_time)
        {
          *recorded_mod_time = svn_sqlite__column_int64(stmt, 12);
        }
      if (depth)
        {
          if (node_kind != svn_wc__db_kind_dir)
            {
              *depth = svn_depth_unknown;
            }
          else
            {
              const char *depth_str = svn_sqlite__column_text(stmt, 10, NULL);

              if (depth_str == NULL)
                *depth = svn_depth_unknown;
              else
                *depth = svn_depth_from_word(depth_str);
            }
        }
      if (checksum)
        {
          if (node_kind != svn_wc__db_kind_file)
            {
              *checksum = NULL;
            }
          else
            {
              err = svn_sqlite__column_checksum(checksum, stmt, 5,
                                                result_pool);
              if (err != NULL)
                err = svn_error_createf(
                        err->apr_err, err,
                        _("The node '%s' has a corrupt checksum value."),
                        path_for_error_message(wcroot, local_relpath,
                                               scratch_pool));
            }
        }
      if (recorded_size)
        {
          *recorded_size = get_translated_size(stmt, 6);
        }
      if (target)
        {
          if (node_kind != svn_wc__db_kind_symlink)
            *target = NULL;
          else
            *target = svn_sqlite__column_text(stmt, 11, result_pool);
        }
      if (had_props)
        {
          *had_props = SQLITE_PROPERTIES_AVAILABLE(stmt, 13);
        }
      if (update_root)
        {
          *update_root = svn_sqlite__column_boolean(stmt, 14);
        }
      if (needs_full_update)
        {
          /* Before we add a new column it is equivalent to the wc-ng
             incomplete presence */
          *status = (svn_sqlite__column_token(stmt, 2, presence_map)
                            == svn_wc__db_status_incomplete);
        }
    }
  else
    {
      err = svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                              _("The node '%s' was not found."),
                              path_for_error_message(wcroot, local_relpath,
                                                     scratch_pool));
    }

  /* Note: given the composition, no need to wrap for tracing.  */
  return svn_error_compose_create(err, svn_sqlite__reset(stmt));
}


svn_error_t *
svn_wc__db_base_get_info(svn_wc__db_status_t *status,
                         svn_wc__db_kind_t *kind,
                         svn_revnum_t *revision,
                         const char **repos_relpath,
                         const char **repos_root_url,
                         const char **repos_uuid,
                         svn_revnum_t *changed_rev,
                         apr_time_t *changed_date,
                         const char **changed_author,
                         svn_depth_t *depth,
                         const svn_checksum_t **checksum,
                         const char **target,
                         svn_wc__db_lock_t **lock,
                         svn_filesize_t *recorded_size,
                         apr_time_t *recorded_mod_time,
                         svn_boolean_t *had_props,
                         svn_boolean_t *update_root,
                         svn_boolean_t *needs_full_update,
                         svn_wc__db_t *db,
                         const char *local_abspath,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  apr_int64_t repos_id;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(base_get_info(status, kind, revision, repos_relpath, &repos_id,
                        changed_rev, changed_date, changed_author, depth,
                        checksum, target, lock, recorded_size,
                        recorded_mod_time, had_props,
                        update_root, needs_full_update,
                        wcroot, local_relpath, result_pool, scratch_pool));
  SVN_ERR_ASSERT(repos_id != INVALID_REPOS_ID);
  SVN_ERR(fetch_repos_info(repos_root_url, repos_uuid,
                           wcroot->sdb, repos_id, result_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_base_get_children_info(apr_hash_t **nodes,
                                  svn_wc__db_t *db,
                                  const char *dir_abspath,
                                  apr_pool_t *result_pool,
                                  apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(dir_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              dir_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  *nodes = apr_hash_make(result_pool);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, 
                                    STMT_SELECT_BASE_CHILDREN_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));

  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  while (have_row)
    {
      struct svn_wc__db_base_info_t *info;
      svn_error_t *err;
      apr_int64_t repos_id;
      const char *depth_str;
      const char *child_relpath = svn_sqlite__column_text(stmt, 0, NULL);
      const char *name = svn_relpath_basename(child_relpath, result_pool);

      info = apr_pcalloc(result_pool, sizeof(*info));

      repos_id = svn_sqlite__column_int64(stmt, 1);
      info->repos_relpath = svn_sqlite__column_text(stmt, 2, result_pool);
      info->status = svn_sqlite__column_token(stmt, 3, presence_map);
      info->kind = svn_sqlite__column_token(stmt, 4, kind_map);
      info->revnum = svn_sqlite__column_revnum(stmt, 5);

      depth_str = svn_sqlite__column_text(stmt, 6, NULL);

      info->depth = (depth_str != NULL) ? svn_depth_from_word(depth_str)
                                        : svn_depth_unknown;

      info->had_props = SQLITE_PROPERTIES_AVAILABLE(stmt, 7);
      info->update_root = svn_sqlite__column_boolean(stmt, 8);

      info->lock = lock_from_columns(stmt, 9, 10, 11, 12, result_pool);

      err = fetch_repos_info(&info->repos_root_url, NULL, wcroot->sdb,
                             repos_id, result_pool);

      if (err)
        return svn_error_return(
                 svn_error_compose_create(err,
                                          svn_sqlite__reset(stmt)));
                           

      apr_hash_set(*nodes, name, APR_HASH_KEY_STRING, info);

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_base_get_props(apr_hash_t **props,
                          svn_wc__db_t *db,
                          const char *local_abspath,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  svn_error_t *err;

  SVN_ERR(get_statement_for_path(&stmt, db, local_abspath,
                                 STMT_SELECT_BASE_PROPS, scratch_pool));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    {
      err = svn_sqlite__reset(stmt);
      return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, err,
                               _("The node '%s' was not found."),
                               svn_dirent_local_style(local_abspath,
                                                      scratch_pool));
    }

  err = svn_sqlite__column_properties(props, stmt, 0, result_pool,
                                      scratch_pool);
  if (err == NULL && *props == NULL)
    {
      /* ### is this a DB constraint violation? the column "probably" should
         ### never be null.  */
      *props = apr_hash_make(result_pool);
    }

  return svn_error_compose_create(err, svn_sqlite__reset(stmt));
}


svn_error_t *
svn_wc__db_base_get_children(const apr_array_header_t **children,
                             svn_wc__db_t *db,
                             const char *local_abspath,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                                             local_abspath,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  return gather_repo_children(children, wcroot, local_relpath, 0,
                              result_pool, scratch_pool);
}


svn_error_t *
svn_wc__db_base_set_dav_cache(svn_wc__db_t *db,
                              const char *local_abspath,
                              const apr_hash_t *props,
                              apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  int affected_rows;

  SVN_ERR(get_statement_for_path(&stmt, db, local_abspath,
                                 STMT_UPDATE_BASE_NODE_DAV_CACHE,
                                 scratch_pool));
  SVN_ERR(svn_sqlite__bind_properties(stmt, 3, props, scratch_pool));

  SVN_ERR(svn_sqlite__update(&affected_rows, stmt));

  if (affected_rows != 1)
    return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                             _("The node '%s' was not found."),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_base_get_dav_cache(apr_hash_t **props,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(get_statement_for_path(&stmt, db, local_abspath,
                                 STMT_SELECT_BASE_DAV_CACHE, scratch_pool));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND,
                             svn_sqlite__reset(stmt),
                             _("The node '%s' was not found."),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  SVN_ERR(svn_sqlite__column_properties(props, stmt, 0, result_pool,
                                        scratch_pool));
  return svn_error_return(svn_sqlite__reset(stmt));
}


svn_error_t *
svn_wc__db_base_clear_dav_cache_recursive(svn_wc__db_t *db,
                                          const char *local_abspath,
                                          apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  const char *like_arg;
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                             db, local_abspath,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  like_arg = construct_like_arg(local_relpath, scratch_pool);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_CLEAR_BASE_NODE_RECURSIVE_DAV_CACHE));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                            like_arg));

  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}


/* Helper for svn_wc__db_op_copy to handle copying from one db to
   another */
static svn_error_t *
cross_db_copy(svn_wc__db_wcroot_t *src_wcroot,
              const char *src_relpath,
              svn_wc__db_wcroot_t *dst_wcroot,
              const char *dst_relpath,
              svn_wc__db_status_t dst_status,
              apr_int64_t dst_op_depth,
              apr_int64_t dst_np_op_depth,
              svn_wc__db_kind_t kind,
              const apr_array_header_t *children,
              apr_int64_t copyfrom_id,
              const char *copyfrom_relpath,
              svn_revnum_t copyfrom_rev,
              apr_pool_t *scratch_pool)
{
  insert_working_baton_t iwb;
  svn_revnum_t changed_rev;
  apr_time_t changed_date;
  const char *changed_author;
  const svn_checksum_t *checksum;
  apr_hash_t *props;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  svn_depth_t depth;

  SVN_ERR_ASSERT(kind == svn_wc__db_kind_file
                 || kind == svn_wc__db_kind_dir
                 );

  SVN_ERR(read_info(NULL, NULL, NULL, NULL, NULL,
                    &changed_rev, &changed_date, &changed_author, &depth,
                    &checksum, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    src_wcroot, src_relpath, scratch_pool, scratch_pool));

  SVN_ERR(db_read_pristine_props(&props, src_wcroot, src_relpath,
                                 scratch_pool, scratch_pool));

  blank_iwb(&iwb);
  iwb.presence = dst_status;
  iwb.kind = kind;

  iwb.props = props;
  iwb.changed_rev = changed_rev;
  iwb.changed_date = changed_date;
  iwb.changed_author = changed_author;
  iwb.original_repos_id = copyfrom_id;
  iwb.original_repos_relpath = copyfrom_relpath;
  iwb.original_revnum = copyfrom_rev;
  iwb.moved_here = FALSE;

  iwb.op_depth = dst_op_depth;

  iwb.checksum = checksum;
  iwb.children = children;
  iwb.depth = depth;

  iwb.not_present_op_depth = dst_np_op_depth;

  SVN_ERR(insert_working_node(&iwb, dst_wcroot, dst_relpath, scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, src_wcroot->sdb,
                                    STMT_SELECT_ACTUAL_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", src_wcroot->wc_id, src_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (have_row)
    {
      /* ### STMT_INSERT_ACTUAL_NODE doesn't cover every column, it's
         ### enough for some cases but will probably need to extended. */
      const char *prop_reject = svn_sqlite__column_text(stmt, 0, scratch_pool);
      const char *changelist = svn_sqlite__column_text(stmt, 1, scratch_pool);
      const char *conflict_old = svn_sqlite__column_text(stmt, 2, scratch_pool);
      const char *conflict_new = svn_sqlite__column_text(stmt, 3, scratch_pool);
      const char *conflict_wrk = svn_sqlite__column_text(stmt, 4, scratch_pool);
      const char *tree_conflict_data = svn_sqlite__column_text(stmt, 5,
                                                               scratch_pool);
      apr_size_t props_size;
      /* No need to parse the properties when simply copying. */
      const char *properties = svn_sqlite__column_blob(stmt, 6, &props_size,
                                                       scratch_pool);

      SVN_ERR(svn_sqlite__reset(stmt));

      if (prop_reject)
        prop_reject = svn_relpath_join(dst_relpath,
                                       svn_relpath_skip_ancestor(src_relpath,
                                                                 prop_reject),
                                       scratch_pool);
      if (conflict_old)
        conflict_old = svn_relpath_join(dst_relpath,
                                        svn_relpath_skip_ancestor(src_relpath,
                                                                  conflict_old),
                                        scratch_pool);
      if (conflict_new)
        conflict_new = svn_relpath_join(dst_relpath,
                                        svn_relpath_skip_ancestor(src_relpath,
                                                                  conflict_new),
                                        scratch_pool);
      if (conflict_wrk)
        conflict_wrk = svn_relpath_join(dst_relpath,
                                        svn_relpath_skip_ancestor(src_relpath,
                                                                  conflict_wrk),
                                        scratch_pool);

      /* ### Do we need to adjust relpaths in tree conflict data? */

      SVN_ERR(svn_sqlite__get_statement(&stmt, dst_wcroot->sdb,
                                        STMT_INSERT_ACTUAL_NODE));
      SVN_ERR(svn_sqlite__bindf(stmt, "issbssssss",
                                dst_wcroot->wc_id, dst_relpath,
                                svn_relpath_dirname(dst_relpath, scratch_pool),
                                properties, props_size,
                                conflict_old, conflict_new, conflict_wrk,
                                prop_reject, changelist, tree_conflict_data));
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  return SVN_NO_ERROR;
}


/* Set *COPYFROM_ID, *COPYFROM_RELPATH, *COPYFROM_REV to the values
   appropriate for the copy. Also return *STATUS, *KIND and *HAVE_WORK
   since they are available.  This is a helper for
   svn_wc__db_op_copy. */
static svn_error_t *
get_info_for_copy(apr_int64_t *copyfrom_id,
                  const char **copyfrom_relpath,
                  svn_revnum_t *copyfrom_rev,
                  svn_wc__db_status_t *status,
                  svn_wc__db_kind_t *kind,
                  svn_boolean_t *have_work,
                  svn_wc__db_wcroot_t *wcroot,
                  const char *local_relpath,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  const char *repos_relpath;
  svn_revnum_t revision;

  SVN_ERR(read_info(status, kind, &revision, &repos_relpath, copyfrom_id,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL /* have_base */,
                    NULL /* have_more_work */,
                    have_work,
                    wcroot, local_relpath, result_pool, scratch_pool));

  if (*status == svn_wc__db_status_excluded)
    {
      /* The parent cannot be excluded, so look at the parent and then
         adjust the relpath */
      const char *parent_relpath, *base_name;
      svn_wc__db_status_t parent_status;
      svn_wc__db_kind_t parent_kind;
      svn_boolean_t parent_have_work;

      svn_dirent_split(&parent_relpath, &base_name, local_relpath,
                       scratch_pool);
      SVN_ERR(get_info_for_copy(copyfrom_id, copyfrom_relpath, copyfrom_rev,
                                &parent_status,
                                &parent_kind,
                                &parent_have_work,
                                wcroot, parent_relpath,
                                scratch_pool, scratch_pool));
      if (*copyfrom_relpath)
        *copyfrom_relpath = svn_relpath_join(*copyfrom_relpath, base_name,
                                             result_pool);
    }
  else if (*status == svn_wc__db_status_added)
    {
      const char *op_root_relpath;

      SVN_ERR(scan_addition(NULL, &op_root_relpath,
                            NULL, NULL, /* repos_* */
                            copyfrom_relpath, copyfrom_id, copyfrom_rev,
                            wcroot, local_relpath,
                            scratch_pool, scratch_pool));
      if (*copyfrom_relpath)
        {
          *copyfrom_relpath
            = svn_relpath_join(*copyfrom_relpath,
                               svn_dirent_skip_ancestor(op_root_relpath,
                                                        local_relpath),
                               result_pool);
        }
    }
  else if (*status == svn_wc__db_status_deleted)
    {
      const char *base_del_relpath, *work_del_relpath;

      SVN_ERR(scan_deletion(&base_del_relpath, NULL, &work_del_relpath,
                            wcroot, local_relpath, scratch_pool,
                            scratch_pool));
      if (work_del_relpath)
        {
          const char *op_root_relpath;
          const char *parent_del_relpath = svn_dirent_dirname(work_del_relpath,
                                                              scratch_pool);

          /* Similar to, but not the same as, the _scan_addition and
             _join above.  Can we use get_copyfrom here? */
          SVN_ERR(scan_addition(NULL, &op_root_relpath,
                                NULL, NULL, /* repos_* */
                                copyfrom_relpath, copyfrom_id, copyfrom_rev,
                                wcroot, parent_del_relpath,
                                scratch_pool, scratch_pool));
          *copyfrom_relpath
            = svn_relpath_join(*copyfrom_relpath,
                               svn_dirent_skip_ancestor(op_root_relpath,
                                                        local_relpath),
                               result_pool);
        }
      else if (base_del_relpath)
        {
          SVN_ERR(base_get_info(NULL, NULL, copyfrom_rev, copyfrom_relpath,
                                copyfrom_id,
                                NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                NULL, NULL, NULL, NULL, NULL,
                                wcroot, base_del_relpath,
                                result_pool, scratch_pool));
        }
      else
        SVN_ERR_MALFUNCTION();
    }
  else
    {
      *copyfrom_relpath = repos_relpath;
      *copyfrom_rev = revision;
    }

  return SVN_NO_ERROR;
}


/* Forward declarations for db_op_copy() to use.

   ### these are just to avoid churn. a future commit should shuffle the
   ### functions around.  */
static svn_error_t *
op_depth_of(apr_int64_t *op_depth,
            svn_wc__db_wcroot_t *wcroot,
            const char *local_relpath);

static svn_error_t *
op_depth_for_copy(apr_int64_t *op_depth,
                  apr_int64_t *np_op_depth,
                  apr_int64_t copyfrom_repos_id,
                  const char *copyfrom_relpath,
                  svn_revnum_t copyfrom_revision,
                  svn_wc__db_wcroot_t *wcroot,
                  const char *local_relpath,
                  apr_pool_t *scratch_pool);


/* Like svn_wc__db_op_copy(), but with WCROOT+LOCAL_RELPATH instead of
   DB+LOCAL_ABSPATH.  */
static svn_error_t *
db_op_copy(svn_wc__db_wcroot_t *src_wcroot,
           const char *src_relpath,
           svn_wc__db_wcroot_t *dst_wcroot,
           const char *dst_relpath,
           const svn_skel_t *work_items,
           apr_pool_t *scratch_pool)
{
  const char *copyfrom_relpath;
  svn_revnum_t copyfrom_rev;
  svn_wc__db_status_t status;
  svn_wc__db_status_t dst_status;
  svn_boolean_t have_work;
  apr_int64_t copyfrom_id;
  apr_int64_t dst_op_depth;
  apr_int64_t dst_np_op_depth;
  svn_wc__db_kind_t kind;
  const apr_array_header_t *children;

  SVN_ERR(get_info_for_copy(&copyfrom_id, &copyfrom_relpath, &copyfrom_rev,
                            &status, &kind, &have_work, src_wcroot,
                            src_relpath, scratch_pool, scratch_pool));

  SVN_ERR(op_depth_for_copy(&dst_op_depth, &dst_np_op_depth, copyfrom_id,
                            copyfrom_relpath, copyfrom_rev,
                            dst_wcroot, dst_relpath, scratch_pool));

  SVN_ERR_ASSERT(kind == svn_wc__db_kind_file || kind == svn_wc__db_kind_dir);

  /* ### New status, not finished, see notes/wc-ng/copying */
  switch (status)
    {
    case svn_wc__db_status_normal:
    case svn_wc__db_status_added:
    case svn_wc__db_status_moved_here:
    case svn_wc__db_status_copied:
      dst_status = svn_wc__db_status_normal;
      break;
    case svn_wc__db_status_deleted:
    case svn_wc__db_status_not_present:
    case svn_wc__db_status_excluded:
      /* These presence values should not create a new op depth */
      if (dst_np_op_depth > 0)
        {
          dst_op_depth = dst_np_op_depth;
          dst_np_op_depth = -1;
        }
      if (status == svn_wc__db_status_excluded)
        dst_status = svn_wc__db_status_excluded;
      else
        dst_status = svn_wc__db_status_not_present;
      break;
    case svn_wc__db_status_absent:
      return svn_error_createf(SVN_ERR_AUTHZ_UNREADABLE, NULL,
                               _("Cannot copy '%s' excluded by server"),
                               path_for_error_message(src_wcroot,
                                                      src_relpath,
                                                      scratch_pool));
    default:
      return svn_error_createf(SVN_ERR_WC_PATH_UNEXPECTED_STATUS, NULL,
                               _("Cannot handle status of '%s'"),
                               path_for_error_message(src_wcroot,
                                                      src_relpath,
                                                      scratch_pool));
    }

  if (kind == svn_wc__db_kind_dir)
    {
      apr_int64_t src_op_depth;

      SVN_ERR(op_depth_of(&src_op_depth, src_wcroot, src_relpath));
      SVN_ERR(gather_repo_children(&children, src_wcroot, src_relpath,
                                   src_op_depth, scratch_pool, scratch_pool));
    }
  else
    children = NULL;

  if (src_wcroot == dst_wcroot)
    {
      svn_sqlite__stmt_t *stmt;
      const char *dst_parent_relpath = svn_relpath_dirname(dst_relpath,
                                                           scratch_pool);

      if (have_work)
        SVN_ERR(svn_sqlite__get_statement(&stmt, src_wcroot->sdb,
                         STMT_INSERT_WORKING_NODE_COPY_FROM_WORKING));
      else
        SVN_ERR(svn_sqlite__get_statement(&stmt, src_wcroot->sdb,
                          STMT_INSERT_WORKING_NODE_COPY_FROM_BASE));

      SVN_ERR(svn_sqlite__bindf(stmt, "issisnnnt",
                    src_wcroot->wc_id, src_relpath,
                    dst_relpath,
                    dst_op_depth,
                    dst_parent_relpath,
                    presence_map, dst_status));

      if (copyfrom_relpath)
        {
          SVN_ERR(svn_sqlite__bind_int64(stmt, 6, copyfrom_id));
          SVN_ERR(svn_sqlite__bind_text(stmt, 7, copyfrom_relpath));
          SVN_ERR(svn_sqlite__bind_int64(stmt, 8, copyfrom_rev));
        }
      SVN_ERR(svn_sqlite__step_done(stmt));

      /* ### Copying changelist is OK for a move but what about a copy? */
      SVN_ERR(svn_sqlite__get_statement(&stmt, src_wcroot->sdb,
                                  STMT_INSERT_ACTUAL_NODE_FROM_ACTUAL_NODE));
      SVN_ERR(svn_sqlite__bindf(stmt, "isss", src_wcroot->wc_id, src_relpath,
                                dst_relpath, dst_parent_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));

      if (dst_np_op_depth > 0)
        {
          /* We introduce a not-present node at the parent's op_depth to
             properly start a new op-depth at our own op_depth. This marks
             us as an op_root for commit and allows reverting just this
             operation */

          SVN_ERR(svn_sqlite__get_statement(&stmt, dst_wcroot->sdb,
                                            STMT_INSERT_NODE));
          SVN_ERR(svn_sqlite__bindf(stmt, "isisisrtnt", 
                                    src_wcroot->wc_id, dst_relpath,
                                    dst_np_op_depth, dst_parent_relpath,
                                    copyfrom_id, copyfrom_relpath,
                                    copyfrom_rev,
                                    presence_map,
                                       svn_wc__db_status_not_present,
                                    /* NULL */
                                    kind_map, kind));

          SVN_ERR(svn_sqlite__step_done(stmt));
        }
      /* Insert incomplete children, if relevant.
         The children are part of the same op and so have the same op_depth.
         (The only time we'd want a different depth is during a recursive
         simple add, but we never insert children here during a simple add.) */
      if (kind == svn_wc__db_kind_dir)
        SVN_ERR(insert_incomplete_children(
                  dst_wcroot->sdb,
                  dst_wcroot->wc_id,
                  dst_relpath,
                  INVALID_REPOS_ID /* inherit repos_id */,
                  NULL /* inherit repos_path */,
                  copyfrom_rev,
                  children,
                  dst_op_depth,
                  scratch_pool));
    }
  else
    {
      SVN_ERR(cross_db_copy(src_wcroot, src_relpath, dst_wcroot,
                            dst_relpath, dst_status, dst_op_depth,
                            dst_np_op_depth, kind,
                            children, copyfrom_id, copyfrom_relpath,
                            copyfrom_rev, scratch_pool));
    }

  SVN_ERR(add_work_items(dst_wcroot->sdb, work_items, scratch_pool));

  return SVN_NO_ERROR;
}

/* Baton for op_copy_txn */
struct op_copy_baton
{
  svn_wc__db_wcroot_t *src_wcroot;
  const char *src_relpath;

  svn_wc__db_t *db;
  const char *dst_abspath;
  svn_wc__db_wcroot_t *dst_wcroot;
  const char *dst_relpath;

  const svn_skel_t *work_items;
};

/* Helper for svn_wc__db_op_copy.
   Implements  svn_sqlite__transaction_callback_t */
static svn_error_t *
op_copy_txn(void * baton, svn_sqlite__db_t *sdb, apr_pool_t *scratch_pool)
{
  struct op_copy_baton *ocb = baton;

  if (ocb->dst_wcroot == NULL)
    {
       SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&ocb->dst_wcroot,
                                                     &ocb->dst_relpath,
                                                     ocb->db,
                                                     ocb->dst_abspath,
                                                     scratch_pool,
                                                     scratch_pool));

       VERIFY_USABLE_WCROOT(ocb->dst_wcroot);

       if (ocb->dst_wcroot->sdb != sdb)
         {
            /* Source and destination databases differ; so also start a lock
               in the destination database, by calling ourself in a lock. */

            return svn_error_return(
                        svn_sqlite__with_lock(ocb->dst_wcroot->sdb,
                                              op_copy_txn, ocb, scratch_pool));
         }
    }

  /* From this point we can assume a lock in the src and dst databases */

  SVN_ERR(db_op_copy(ocb->src_wcroot, ocb->src_relpath,
                     ocb->dst_wcroot, ocb->dst_relpath,
                     ocb->work_items, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_op_copy(svn_wc__db_t *db,
                   const char *src_abspath,
                   const char *dst_abspath,
                   const char *dst_op_root_abspath,
                   const svn_skel_t *work_items,
                   apr_pool_t *scratch_pool)
{
   struct op_copy_baton ocb = {0};

  SVN_ERR_ASSERT(svn_dirent_is_absolute(src_abspath));
  SVN_ERR_ASSERT(svn_dirent_is_absolute(dst_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&ocb.src_wcroot,
                                                &ocb.src_relpath, db,
                                                src_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(ocb.src_wcroot);

  ocb.db = db;
  ocb.dst_abspath = dst_abspath;
  ocb.work_items = work_items;

  SVN_ERR(svn_sqlite__with_lock(ocb.src_wcroot->sdb, op_copy_txn, &ocb,
                                scratch_pool));

  return SVN_NO_ERROR;
}


/* Set *OP_DEPTH to the highest op depth of WCROOT:LOCAL_RELPATH. */
static svn_error_t *
op_depth_of(apr_int64_t *op_depth,
            svn_wc__db_wcroot_t *wcroot,
            const char *local_relpath)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_NODE_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  SVN_ERR_ASSERT(have_row);
  *op_depth = svn_sqlite__column_int64(stmt, 0);
  SVN_ERR(svn_sqlite__reset(stmt));

  return SVN_NO_ERROR;
}


/* If there are any absent (excluded by authz) base nodes then the
   copy must fail as it's not possible to commit such a copy.  Return
   an error if there are any absent nodes. */
static svn_error_t *
catch_copy_of_absent(svn_wc__db_wcroot_t *wcroot,
                     const char *local_relpath,
                     apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  const char *absent_relpath;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_ABSENT_NODES));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss",
                            wcroot->wc_id,
                            local_relpath,
                            construct_like_arg(local_relpath,
                                               scratch_pool)));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (have_row)
    absent_relpath = svn_sqlite__column_text(stmt, 0, scratch_pool);
  SVN_ERR(svn_sqlite__reset(stmt));
  if (have_row)
    return svn_error_createf(SVN_ERR_AUTHZ_UNREADABLE, NULL,
                             _("Cannot copy '%s' excluded by server"),
                             path_for_error_message(wcroot, absent_relpath,
                                                    scratch_pool));

  return SVN_NO_ERROR;
}


/* Determine at which OP_DEPTH a copy of COPYFROM_REPOS_ID, COPYFROM_RELPATH at
   revision COPYFROM_REVISION should be inserted as LOCAL_RELPATH. Do this
   by checking if this would be a direct child of a copy of its parent
   directory. If it is then set *OP_DEPTH to the op_depth of its parent.

   If the node is not a direct copy at the same revision of the parent
   *NP_OP_DEPTH will be set to the op_depth of the parent when a not-present
   node should be inserted at this op_depth. This will be the case when the
   parent already defined an incomplete child with the same name. Otherwise
   *NP_OP_DEPTH will be set to -1.

   If the parent node is not the parent of the to be copied node, then
   *OP_DEPTH will be set to the proper op_depth for a new oew operation root.
 */
static svn_error_t *
op_depth_for_copy(apr_int64_t *op_depth,
                  apr_int64_t *np_op_depth,
                  apr_int64_t copyfrom_repos_id,
                  const char *copyfrom_relpath,
                  svn_revnum_t copyfrom_revision,
                  svn_wc__db_wcroot_t *wcroot,
                  const char *local_relpath,
                  apr_pool_t *scratch_pool)
{
  const char *parent_relpath, *name;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_int64_t incomplete_op_depth = -1;

  *op_depth = relpath_depth(local_relpath);
  *np_op_depth = -1;

  if (!copyfrom_relpath)
    return SVN_NO_ERROR;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (have_row)
    {
      svn_wc__db_status_t status = svn_sqlite__column_token(stmt, 1,
                                                            presence_map);
      if (status == svn_wc__db_status_incomplete)
        incomplete_op_depth = svn_sqlite__column_int64(stmt, 0);
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  svn_relpath_split(&parent_relpath, &name, local_relpath, scratch_pool);
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, parent_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (have_row)
    {
      apr_int64_t parent_op_depth = svn_sqlite__column_int64(stmt, 0);
      svn_wc__db_status_t status = svn_sqlite__column_token(stmt, 1,
                                                            presence_map);

      svn_error_t *err = convert_to_working_status(&status, status);
      if (err)
        SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));

      if (status == svn_wc__db_status_added
          && ((incomplete_op_depth < 0)
              || (incomplete_op_depth == parent_op_depth)))
        {
          apr_int64_t parent_copyfrom_repos_id
            = svn_sqlite__column_int64(stmt, 10);
          const char *parent_copyfrom_relpath
            = svn_sqlite__column_text(stmt, 11, NULL);
          svn_revnum_t parent_copyfrom_revision
            = svn_sqlite__column_revnum(stmt, 12);

          if (parent_copyfrom_repos_id == copyfrom_repos_id)
            {
              if (copyfrom_revision == parent_copyfrom_revision
                  && !strcmp(copyfrom_relpath,
                             svn_relpath_join(parent_copyfrom_relpath, name,
                                              scratch_pool)))
                *op_depth = parent_op_depth;
              else if (incomplete_op_depth > 0)
                *np_op_depth = incomplete_op_depth;
            }
        }
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_op_copy_dir(svn_wc__db_t *db,
                       const char *local_abspath,
                       const apr_hash_t *props,
                       svn_revnum_t changed_rev,
                       apr_time_t changed_date,
                       const char *changed_author,
                       const char *original_repos_relpath,
                       const char *original_root_url,
                       const char *original_uuid,
                       svn_revnum_t original_revision,
                       const apr_array_header_t *children,
                       svn_depth_t depth,
                       const svn_skel_t *conflict,
                       const svn_skel_t *work_items,
                       apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  insert_working_baton_t iwb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(props != NULL);
  /* ### any assertions for CHANGED_* ?  */
  /* ### any assertions for ORIGINAL_* ?  */
#if 0
  SVN_ERR_ASSERT(children != NULL);
#endif
  SVN_ERR_ASSERT(conflict == NULL);  /* ### can't handle yet  */

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  blank_iwb(&iwb);

  iwb.presence = svn_wc__db_status_normal;
  iwb.kind = svn_wc__db_kind_dir;

  iwb.props = props;
  iwb.changed_rev = changed_rev;
  iwb.changed_date = changed_date;
  iwb.changed_author = changed_author;
  iwb.moved_here = FALSE;

  if (original_root_url != NULL)
    {
      SVN_ERR(create_repos_id(&iwb.original_repos_id,
                              original_root_url, original_uuid,
                              wcroot->sdb, scratch_pool));
      iwb.original_repos_relpath = original_repos_relpath;
      iwb.original_revnum = original_revision;
    }

  /* ### Should we do this inside the transaction? */
  SVN_ERR(op_depth_for_copy(&iwb.op_depth, &iwb.not_present_op_depth,
                            iwb.original_repos_id,
                            original_repos_relpath, original_revision,
                            wcroot, local_relpath, scratch_pool));

  iwb.children = children;
  iwb.depth = depth;

  iwb.work_items = work_items;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, insert_working_node, &iwb,
                              scratch_pool));
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_op_copy_file(svn_wc__db_t *db,
                        const char *local_abspath,
                        const apr_hash_t *props,
                        svn_revnum_t changed_rev,
                        apr_time_t changed_date,
                        const char *changed_author,
                        const char *original_repos_relpath,
                        const char *original_root_url,
                        const char *original_uuid,
                        svn_revnum_t original_revision,
                        const svn_checksum_t *checksum,
                        const svn_skel_t *conflict,
                        const svn_skel_t *work_items,
                        apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  insert_working_baton_t iwb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(props != NULL);
  /* ### any assertions for CHANGED_* ?  */
  SVN_ERR_ASSERT((! original_repos_relpath && ! original_root_url
                  && ! original_uuid && ! checksum
                  && original_revision == SVN_INVALID_REVNUM)
                 || (original_repos_relpath && original_root_url
                     && original_uuid && checksum
                     && original_revision != SVN_INVALID_REVNUM));
  SVN_ERR_ASSERT(conflict == NULL);  /* ### can't handle yet  */

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  blank_iwb(&iwb);

  iwb.presence = svn_wc__db_status_normal;
  iwb.kind = svn_wc__db_kind_file;

  iwb.props = props;
  iwb.changed_rev = changed_rev;
  iwb.changed_date = changed_date;
  iwb.changed_author = changed_author;
  iwb.moved_here = FALSE;

  if (original_root_url != NULL)
    {
      SVN_ERR(create_repos_id(&iwb.original_repos_id,
                              original_root_url, original_uuid,
                              wcroot->sdb, scratch_pool));
      iwb.original_repos_relpath = original_repos_relpath;
      iwb.original_revnum = original_revision;
    }

  /* ### Should we do this inside the transaction? */
  SVN_ERR(op_depth_for_copy(&iwb.op_depth, &iwb.not_present_op_depth,
                            iwb.original_repos_id,
                            original_repos_relpath, original_revision,
                            wcroot, local_relpath, scratch_pool));

  iwb.checksum = checksum;

  iwb.work_items = work_items;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, insert_working_node, &iwb,
                              scratch_pool));
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_op_copy_symlink(svn_wc__db_t *db,
                           const char *local_abspath,
                           const apr_hash_t *props,
                           svn_revnum_t changed_rev,
                           apr_time_t changed_date,
                           const char *changed_author,
                           const char *original_repos_relpath,
                           const char *original_root_url,
                           const char *original_uuid,
                           svn_revnum_t original_revision,
                           const char *target,
                           const svn_skel_t *conflict,
                           const svn_skel_t *work_items,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  insert_working_baton_t iwb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(props != NULL);
  /* ### any assertions for CHANGED_* ?  */
  /* ### any assertions for ORIGINAL_* ?  */
  SVN_ERR_ASSERT(target != NULL);
  SVN_ERR_ASSERT(conflict == NULL);  /* ### can't handle yet  */

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  blank_iwb(&iwb);

  iwb.presence = svn_wc__db_status_normal;
  iwb.kind = svn_wc__db_kind_symlink;

  iwb.props = props;
  iwb.changed_rev = changed_rev;
  iwb.changed_date = changed_date;
  iwb.changed_author = changed_author;
  iwb.moved_here = FALSE;

  if (original_root_url != NULL)
    {
      SVN_ERR(create_repos_id(&iwb.original_repos_id,
                              original_root_url, original_uuid,
                              wcroot->sdb, scratch_pool));
      iwb.original_repos_relpath = original_repos_relpath;
      iwb.original_revnum = original_revision;
    }

  /* ### Should we do this inside the transaction? */
  SVN_ERR(op_depth_for_copy(&iwb.op_depth, &iwb.not_present_op_depth,
                            iwb.original_repos_id,
                            original_repos_relpath, original_revision,
                            wcroot, local_relpath, scratch_pool));

  iwb.target = target;

  iwb.work_items = work_items;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, insert_working_node, &iwb,
                              scratch_pool));
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_op_add_directory(svn_wc__db_t *db,
                            const char *local_abspath,
                            const svn_skel_t *work_items,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  insert_working_baton_t iwb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  blank_iwb(&iwb);

  iwb.presence = svn_wc__db_status_normal;
  iwb.kind = svn_wc__db_kind_dir;
  iwb.op_depth = relpath_depth(local_relpath);

  iwb.work_items = work_items;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, insert_working_node, &iwb,
                              scratch_pool));
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_op_add_file(svn_wc__db_t *db,
                       const char *local_abspath,
                       const svn_skel_t *work_items,
                       apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  insert_working_baton_t iwb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  blank_iwb(&iwb);

  iwb.presence = svn_wc__db_status_normal;
  iwb.kind = svn_wc__db_kind_file;
  iwb.op_depth = relpath_depth(local_relpath);

  iwb.work_items = work_items;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, insert_working_node, &iwb,
                              scratch_pool));
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_op_add_symlink(svn_wc__db_t *db,
                          const char *local_abspath,
                          const char *target,
                          const svn_skel_t *work_items,
                          apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  insert_working_baton_t iwb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(target != NULL);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  blank_iwb(&iwb);

  iwb.presence = svn_wc__db_status_normal;
  iwb.kind = svn_wc__db_kind_symlink;
  iwb.op_depth = relpath_depth(local_relpath);

  iwb.target = target;

  iwb.work_items = work_items;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, insert_working_node, &iwb,
                              scratch_pool));
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


struct set_props_baton_t
{
  apr_hash_t *props;

  const svn_skel_t *conflict;
  const svn_skel_t *work_items;
};


/* Set the ACTUAL_NODE properties column for (WC_ID, LOCAL_RELPATH) to
 * PROPS. */
static svn_error_t *
set_actual_props(apr_int64_t wc_id,
                 const char *local_relpath,
                 apr_hash_t *props,
                 svn_sqlite__db_t *db,
                 apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  int affected_rows;

  SVN_ERR(svn_sqlite__get_statement(&stmt, db, STMT_UPDATE_ACTUAL_PROPS));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, local_relpath));
  SVN_ERR(svn_sqlite__bind_properties(stmt, 3, props, scratch_pool));
  SVN_ERR(svn_sqlite__update(&affected_rows, stmt));

  if (affected_rows == 1 || !props)
    return SVN_NO_ERROR; /* We are done */

  /* We have to insert a row in ACTUAL */

  SVN_ERR(svn_sqlite__get_statement(&stmt, db, STMT_INSERT_ACTUAL_PROPS));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, local_relpath));
  if (*local_relpath != '\0')
    SVN_ERR(svn_sqlite__bind_text(stmt, 3,
                                  svn_relpath_dirname(local_relpath,
                                                      scratch_pool)));
  SVN_ERR(svn_sqlite__bind_properties(stmt, 4, props, scratch_pool));
  return svn_error_return(svn_sqlite__step_done(stmt));
}


/* Set the 'properties' column in the 'ACTUAL_NODE' table to BATON->props.
   Create an entry in the ACTUAL table for the node if it does not yet
   have one.
   To specify no properties, BATON->props must be an empty hash, not NULL.
   BATON is of type 'struct set_props_baton_t'. */
static svn_error_t *
set_props_txn(void *baton,
              svn_wc__db_wcroot_t *wcroot,
              const char *local_relpath,
              apr_pool_t *scratch_pool)
{
  struct set_props_baton_t *spb = baton;
  apr_hash_t *pristine_props;

  /* ### we dunno what to do with CONFLICT yet.  */
  SVN_ERR_ASSERT(spb->conflict == NULL);

  /* First order of business: insert all the work items.  */
  SVN_ERR(add_work_items(wcroot->sdb, spb->work_items, scratch_pool));

  /* Check if the props are modified. If no changes, then wipe out the
     ACTUAL props.  PRISTINE_PROPS==NULL means that any
     ACTUAL props are okay as provided, so go ahead and set them.  */
  SVN_ERR(db_read_pristine_props(&pristine_props, wcroot, local_relpath,
                                 scratch_pool, scratch_pool));
  if (spb->props && pristine_props)
    {
      apr_array_header_t *prop_diffs;

      SVN_ERR(svn_prop_diffs(&prop_diffs, spb->props, pristine_props,
                             scratch_pool));
      if (prop_diffs->nelts == 0)
        spb->props = NULL;
    }

  SVN_ERR(set_actual_props(wcroot->wc_id, local_relpath,
                           spb->props, wcroot->sdb, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_op_set_props(svn_wc__db_t *db,
                        const char *local_abspath,
                        apr_hash_t *props,
                        const svn_skel_t *conflict,
                        const svn_skel_t *work_items,
                        apr_pool_t *scratch_pool)
{
  struct set_props_baton_t spb;
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                              db, local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  spb.props = props;
  spb.conflict = conflict;
  spb.work_items = work_items;

  return svn_error_return(svn_wc__db_with_txn(wcroot, local_relpath,
                                              set_props_txn, &spb,
                                              scratch_pool));
}


#ifdef SVN__SUPPORT_BASE_MERGE

/* Set properties in a given table. The row must exist.  */
static svn_error_t *
set_properties(svn_wc__db_t *db,
               const char *local_abspath,
               const apr_hash_t *props,
               int stmt_idx,
               const char *table_name,
               apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  int affected_rows;

  SVN_ERR_ASSERT(props != NULL);

  SVN_ERR(get_statement_for_path(&stmt, db, local_abspath, stmt_idx,
                                 scratch_pool));

  SVN_ERR(svn_sqlite__bind_properties(stmt, 3, props, scratch_pool));
  SVN_ERR(svn_sqlite__update(&affected_rows, stmt));

  if (affected_rows != 1)
    return svn_error_createf(SVN_ERR_WC_DB_ERROR, NULL,
                             _("Can't store properties for '%s' in '%s'."),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool),
                             table_name);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_base_set_props(svn_wc__db_t *db,
                               const char *local_abspath,
                               const apr_hash_t *props,
                               apr_pool_t *scratch_pool)
{
  SVN_ERR(set_properties(db, local_abspath, props,
                         STMT_UPDATE_NODE_BASE_PROPS,
                         "base node", scratch_pool));
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_working_set_props(svn_wc__db_t *db,
                                  const char *local_abspath,
                                  const apr_hash_t *props,
                                  apr_pool_t *scratch_pool)
{
  SVN_ERR(set_properties(db, local_abspath, props,
                         STMT_UPDATE_NODE_WORKING_PROPS,
                         "working node", scratch_pool));
  return SVN_NO_ERROR;
}

#endif /* SVN__SUPPORT_BASE_MERGE  */


svn_error_t *
svn_wc__db_op_move(svn_wc__db_t *db,
                   const char *src_abspath,
                   const char *dst_abspath,
                   apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(svn_dirent_is_absolute(src_abspath));
  SVN_ERR_ASSERT(svn_dirent_is_absolute(dst_abspath));

  NOT_IMPLEMENTED();
}


svn_error_t *
svn_wc__db_op_modified(svn_wc__db_t *db,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  NOT_IMPLEMENTED();
}


struct set_changelist_baton_t
{
  const char *new_changelist;
  const apr_array_header_t *changelists;
};


/* */
static svn_error_t *
set_changelist_txn(void *baton,
                   svn_wc__db_wcroot_t *wcroot,
                   const char *local_relpath,
                   apr_pool_t *scratch_pool)
{
  struct set_changelist_baton_t *scb = baton;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  /* If we are filtering based on changelists, we *must* already have nodes,
   * so we can skip this check. */
  if (scb->changelists && scb->changelists->nelts > 0)
    have_row = TRUE;
  else
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_SELECT_ACTUAL_NODE));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
      SVN_ERR(svn_sqlite__reset(stmt));
    }

  if (!have_row)
    {
      /* We need to insert an ACTUAL node, but only if we're not attempting
         to remove a (non-existent) changelist. */
      if (scb->new_changelist == NULL)
        return SVN_NO_ERROR;

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_INSERT_ACTUAL_CHANGELIST));

      /* The parent of relpath=="" is null, so we simply skip binding the
         column. Otherwise, bind the proper value to 'parent_relpath'.  */
      if (*local_relpath != '\0')
        SVN_ERR(svn_sqlite__bind_text(stmt, 4,
                                      svn_relpath_dirname(local_relpath,
                                                          scratch_pool)));
    }
  else if (!scb->changelists || scb->changelists->nelts == 0)
    {
      /* No filtering going on: we can just use the simple statement. */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_UPDATE_ACTUAL_CHANGELIST));
    }
  else
    {
      /* We need to execute (potentially) multiple changelist-filtered
         queries, one for each changelist.  */
      int i;

      /* Start with the second changelist in the list of changelist filters.
         In the case where we only have one changelist filter, this loop is
         skipped, and we get simple single-query execution. */
      for (i = 1; i < scb->changelists->nelts; i++)
        {
          const char *cl = APR_ARRAY_IDX(scb->changelists, i, const char *);

          SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                           STMT_UPDATE_ACTUAL_CHANGELIST_FILTER_CHANGELIST));
          SVN_ERR(svn_sqlite__bindf(stmt, "isss", wcroot->wc_id,
                                    local_relpath, scb->new_changelist, cl));
          SVN_ERR(svn_sqlite__step_done(stmt));
        }

      /* Finally, we do the first changelist, and let the actual execution
         fall through below. */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                           STMT_UPDATE_ACTUAL_CHANGELIST_FILTER_CHANGELIST));
      SVN_ERR(svn_sqlite__bind_text(stmt, 4,
                                   APR_ARRAY_IDX(scb->changelists, 0,
                                                 const char *)));
    }

  /* Run the update or insert query */
  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                            scb->new_changelist));
  SVN_ERR(svn_sqlite__step_done(stmt));

  if (scb->new_changelist == NULL)
    {
     /* When removing a changelist, we may have left an empty ACTUAL node, so
        remove it.  */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_DELETE_ACTUAL_EMPTY));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_op_set_changelist(svn_wc__db_t *db,
                             const char *local_abspath,
                             const char *changelist,
                             const apr_array_header_t *changelists,
                             svn_depth_t depth,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_txn_callback_t txn_func;
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct set_changelist_baton_t scb = { changelist, changelists };
  svn_error_t *err;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                              db, local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  switch (depth)
    {
      case svn_depth_empty:
        txn_func = set_changelist_txn;
        break;

      default:
        /* ### This is only implemented for depth = empty right now. */
        NOT_IMPLEMENTED();
    }

  /* We MUST remove the triggers and not leave them to affect subsequent
     operations. */
  err = svn_sqlite__exec_statements(wcroot->sdb, STMT_CREATE_CHANGELIST_LIST);
  if (err)
    return svn_error_compose_create(err,
                                    svn_sqlite__exec_statements(wcroot->sdb,
                                        STMT_DROP_CHANGELIST_LIST_TRIGGERS));

  err = svn_wc__db_with_txn(wcroot, local_relpath, set_changelist_txn,
                            &scb, scratch_pool);

  err = svn_error_compose_create(err,
                                 svn_sqlite__exec_statements(wcroot->sdb,
                                      STMT_DROP_CHANGELIST_LIST_TRIGGERS));

  err = svn_error_compose_create(err,
                                 flush_entries(wcroot, local_abspath,
                                               scratch_pool));

  return err;
}


svn_error_t *
svn_wc__db_changelist_list_notify(svn_wc_notify_func2_t notify_func,
                                  void *notify_baton,
                                  svn_wc__db_t *db,
                                  const char *local_abspath,
                                  apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  const char *like_arg;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                              db, local_abspath, scratch_pool, iterpool));
  VERIFY_USABLE_WCROOT(wcroot);

  like_arg = construct_like_arg(local_relpath, scratch_pool);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_CHANGELIST_LIST_RECURSIVE));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                            like_arg));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    return svn_error_return(svn_sqlite__reset(stmt)); /* optimise for no row */

  while (have_row)
    {
      const char *notify_relpath = svn_sqlite__column_text(stmt, 0, NULL);
      svn_wc_notify_action_t action = svn_sqlite__column_int64(stmt, 1);
      svn_wc_notify_t *notify;
      const char *notify_abspath;

      svn_pool_clear(iterpool);

      notify_abspath = svn_dirent_join(wcroot->abspath, notify_relpath,
                                       iterpool);
      notify = svn_wc_create_notify(notify_abspath, action, iterpool);
      notify->changelist_name = svn_sqlite__column_text(stmt, 2, NULL);
      notify_func(notify_baton, notify, iterpool);

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_CHANGELIST_LIST_RECURSIVE));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                            like_arg));
  SVN_ERR(svn_sqlite__step_done(stmt));

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;

}


svn_error_t *
svn_wc__db_op_mark_conflict(svn_wc__db_t *db,
                            const char *local_abspath,
                            apr_pool_t *scratch_pool)
{
  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  NOT_IMPLEMENTED();
}


svn_error_t *
svn_wc__db_op_mark_resolved(svn_wc__db_t *db,
                            const char *local_abspath,
                            svn_boolean_t resolved_text,
                            svn_boolean_t resolved_props,
                            svn_boolean_t resolved_tree,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  /* ### we're not ready to handy RESOLVED_TREE just yet.  */
  SVN_ERR_ASSERT(!resolved_tree);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* ### these two statements are not transacted together. is this a
     ### problem? I suspect a failure simply leaves the other in a
     ### continued, unresolved state. However, that still retains
     ### "integrity", so another re-run by the user will fix it.  */

  if (resolved_text)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_CLEAR_TEXT_CONFLICT));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }
  if (resolved_props)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_CLEAR_PROPS_CONFLICT));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  /* Some entries have cached the above values. Kapow!!  */
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
set_tc_txn(void *baton,
           svn_wc__db_wcroot_t *wcroot,
           const char *local_relpath,
           apr_pool_t *scratch_pool)
{
  const svn_wc_conflict_description2_t *tree_conflict = baton;
  const char *parent_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  const char *tree_conflict_data;

  /* ### does this work correctly? */
  parent_relpath = svn_relpath_dirname(local_relpath, scratch_pool);

  /* Get existing conflict information for LOCAL_RELPATH. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_ACTUAL_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));

  if (tree_conflict)
    {
      svn_skel_t *skel;

      SVN_ERR(svn_wc__serialize_conflict(&skel, tree_conflict,
                                         scratch_pool, scratch_pool));
      tree_conflict_data = svn_skel__unparse(skel, scratch_pool)->data;
    }
  else
    tree_conflict_data = NULL;

  if (have_row)
    {
      /* There is an existing ACTUAL row, so just update it. */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_UPDATE_ACTUAL_TREE_CONFLICTS));
    }
  else
    {
      /* We need to insert an ACTUAL row with the tree conflict data. */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_INSERT_ACTUAL_TREE_CONFLICTS));
    }

  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                            tree_conflict_data));
  if (!have_row)
    SVN_ERR(svn_sqlite__bind_text(stmt, 4, parent_relpath));

  SVN_ERR(svn_sqlite__step_done(stmt));

  /* Now, remove the actual node if it doesn't have any more useful
     information.  We only need to do this if we've remove data ourselves. */
  if (!tree_conflict_data)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_DELETE_ACTUAL_EMPTY));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_op_set_tree_conflict(svn_wc__db_t *db,
                                const char *local_abspath,
                                const svn_wc_conflict_description2_t *tree_conflict,
                                apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                              db, local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, set_tc_txn,
                              (void *) tree_conflict, scratch_pool));

  /* There may be some entries, and the lock info is now out of date.  */
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


/* This implements svn_wc__db_txn_callback_t */
static svn_error_t *
op_revert_txn(void *baton,
              svn_wc__db_wcroot_t *wcroot,
              const char *local_relpath,
              apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_int64_t op_depth;
  int affected_rows;

  /* ### Similar structure to op_revert_recursive_txn, should they be
         combined? */

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_NODE_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    {
      SVN_ERR(svn_sqlite__reset(stmt));

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_DELETE_ACTUAL_NODE));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__update(&affected_rows, stmt));
      if (affected_rows)
        {
          /* Can't do non-recursive actual-only revert if actual-only
             children exist */
          SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                            STMT_SELECT_ACTUAL_CHILDREN));
          SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
          SVN_ERR(svn_sqlite__step(&have_row, stmt));
          SVN_ERR(svn_sqlite__reset(stmt));
          if (have_row)
            return svn_error_createf(SVN_ERR_WC_INVALID_OPERATION_DEPTH, NULL,
                                     _("Can't revert '%s' without"
                                       " reverting children"),
                                     path_for_error_message(wcroot,
                                                            local_relpath,
                                                            scratch_pool));
          return SVN_NO_ERROR;
        }

      return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                               _("The node '%s' was not found."),
                               path_for_error_message(wcroot,
                                                      local_relpath,
                                                      scratch_pool));
    }

  op_depth = svn_sqlite__column_int64(stmt, 0);
  SVN_ERR(svn_sqlite__reset(stmt));

  if (op_depth > 0 && op_depth == relpath_depth(local_relpath))
    {
      /* Can't do non-recursive revert if children exist */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_SELECT_GE_OP_DEPTH_CHILDREN));
      SVN_ERR(svn_sqlite__bindf(stmt, "isi", wcroot->wc_id,
                                local_relpath, op_depth));
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
      SVN_ERR(svn_sqlite__reset(stmt));
      if (have_row)
        return svn_error_createf(SVN_ERR_WC_INVALID_OPERATION_DEPTH, NULL,
                                 _("Can't revert '%s' without"
                                   " reverting children"),
                                 path_for_error_message(wcroot,
                                                        local_relpath,
                                                        scratch_pool));

      /* Rewrite the op-depth of all deleted children making the
         direct children into roots of deletes. */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                     STMT_UPDATE_OP_DEPTH_INCREASE_RECURSIVE));
      SVN_ERR(svn_sqlite__bindf(stmt, "isi", wcroot->wc_id,
                                construct_like_arg(local_relpath,
                                                   scratch_pool),
                                op_depth));
      SVN_ERR(svn_sqlite__step_done(stmt));

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_DELETE_WORKING_NODE));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));

      /* ### This removes the lock, but what about the access baton? */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_DELETE_WC_LOCK_ORPHAN));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                  STMT_DELETE_ACTUAL_NODE_LEAVING_CHANGELIST));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__update(&affected_rows, stmt));
  if (!affected_rows)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_CLEAR_ACTUAL_NODE_LEAVING_CHANGELIST));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__update(&affected_rows, stmt));
    }

  return SVN_NO_ERROR;
}


/* This implements svn_wc__db_txn_callback_t */
static svn_error_t *
op_revert_recursive_txn(void *baton,
                        svn_wc__db_wcroot_t *wcroot,
                        const char *local_relpath,
                        apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_int64_t op_depth;
  int affected_rows;
  const char *like_arg = construct_like_arg(local_relpath, scratch_pool);

  /* ### Similar structure to op_revert_txn, should they be
         combined? */

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_NODE_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    {
      SVN_ERR(svn_sqlite__reset(stmt));

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_DELETE_ACTUAL_NODE_RECURSIVE));
      SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id,
                                local_relpath, like_arg));
      SVN_ERR(svn_sqlite__step(&affected_rows, stmt));

      if (affected_rows)
        return SVN_NO_ERROR;  /* actual-only revert */

      return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                               _("The node '%s' was not found."),
                               path_for_error_message(wcroot,
                                                      local_relpath,
                                                      scratch_pool));
    }

  op_depth = svn_sqlite__column_int64(stmt, 0);
  SVN_ERR(svn_sqlite__reset(stmt));

  if (op_depth > 0 && op_depth != relpath_depth(local_relpath))
    return svn_error_createf(SVN_ERR_WC_INVALID_OPERATION_DEPTH, NULL,
                             _("Can't revert '%s' without"
                               " reverting parent"),
                             path_for_error_message(wcroot,
                                                    local_relpath,
                                                    scratch_pool));

  if (!op_depth)
    op_depth = 1; /* Don't delete BASE nodes */

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_NODES_RECURSIVE));
  SVN_ERR(svn_sqlite__bindf(stmt, "issi", wcroot->wc_id,
                            local_relpath, like_arg, op_depth));
  SVN_ERR(svn_sqlite__step_done(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                        STMT_DELETE_ACTUAL_NODE_LEAVING_CHANGELIST_RECURSIVE));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id,
                            local_relpath, like_arg));
  SVN_ERR(svn_sqlite__step_done(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                        STMT_CLEAR_ACTUAL_NODE_LEAVING_CHANGELIST_RECURSIVE));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id,
                            local_relpath, like_arg));
  SVN_ERR(svn_sqlite__step_done(stmt));

  /* ### This removes the locks, but what about the access batons? */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_WC_LOCK_ORPHAN_RECURSIVE));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id,
                            local_relpath, like_arg));
  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_op_revert(svn_wc__db_t *db,
                     const char *local_abspath,
                     svn_depth_t depth,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  svn_wc__db_txn_callback_t txn_func;
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_error_t *err;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  switch (depth)
    {
    case svn_depth_empty:
      txn_func = op_revert_txn;
      break;
    case svn_depth_infinity:
      txn_func = op_revert_recursive_txn;
      break;
    default:
      return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                               _("Unsupported depth for revert of '%s'"),
                               svn_dirent_local_style(local_abspath,
                                                      scratch_pool));
    }

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                              db, local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* We MUST remove the triggers and not leave them to affect subsequent
     operations. */
  err = svn_sqlite__exec_statements(wcroot->sdb, STMT_CREATE_REVERT_LIST);
  if (err)
    return svn_error_compose_create(err,
                                    svn_sqlite__exec_statements(wcroot->sdb,
                                               STMT_DROP_REVERT_LIST_TRIGGERS));

  err = svn_wc__db_with_txn(wcroot, local_relpath, txn_func, NULL,
                            scratch_pool);

  err = svn_error_compose_create(err,
                                 svn_sqlite__exec_statements(wcroot->sdb,
                                               STMT_DROP_REVERT_LIST_TRIGGERS));

  err = svn_error_compose_create(err,
                                 flush_entries(wcroot, local_abspath,
                                               scratch_pool));

  return err;
}

struct revert_list_read_baton {
  svn_boolean_t *reverted;
  const char **conflict_old;
  const char **conflict_new;
  const char **conflict_working;
  const char **prop_reject;
  apr_pool_t *result_pool;
};

static svn_error_t *
revert_list_read(void *baton,
                 svn_wc__db_wcroot_t *wcroot,
                 const char *local_relpath,
                 apr_pool_t *scratch_pool)
{
  struct revert_list_read_baton *b = baton;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_REVERT_LIST));
  SVN_ERR(svn_sqlite__bindf(stmt, "s", local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (have_row)
    {
      *(b->reverted) = !svn_sqlite__column_is_null(stmt, 4);
      if (svn_sqlite__column_is_null(stmt, 0))
        *(b->conflict_new) = NULL;
      else
        *(b->conflict_new) = svn_dirent_join(wcroot->abspath,
                                             svn_sqlite__column_text(stmt, 0,
                                                                     NULL),
                                             b->result_pool);
      if (svn_sqlite__column_is_null(stmt, 1))
        *(b->conflict_old) = NULL;
      else
        *(b->conflict_old) = svn_dirent_join(wcroot->abspath,
                                             svn_sqlite__column_text(stmt, 1,
                                                                     NULL),
                                             b->result_pool);
      if (svn_sqlite__column_is_null(stmt, 2))
        *(b->conflict_working) = NULL;
      else
        *(b->conflict_working) = svn_dirent_join(wcroot->abspath,
                                                 svn_sqlite__column_text(stmt,
                                                                         2,
                                                                         NULL),
                                                 b->result_pool);
      if (svn_sqlite__column_is_null(stmt, 3))
        *(b->prop_reject) = NULL;
      else
        *(b->prop_reject) = svn_dirent_join(wcroot->abspath,
                                            svn_sqlite__column_text(stmt, 3,
                                                                    NULL),
                                            b->result_pool);
    }
  else
    {
      *(b->reverted) = FALSE;
      *(b->conflict_new) = *(b->conflict_old) = *(b->conflict_working) = NULL;
      *(b->prop_reject) = NULL;
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  if (have_row)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_DELETE_REVERT_LIST));
      SVN_ERR(svn_sqlite__bindf(stmt, "s", local_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_revert_list_read(svn_boolean_t *reverted,
                            const char **conflict_old,
                            const char **conflict_new,
                            const char **conflict_working,
                            const char **prop_reject,
                            svn_wc__db_t *db,
                            const char *local_abspath,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct revert_list_read_baton b = {reverted, conflict_old, conflict_new,
                                     conflict_working, prop_reject,
                                     result_pool};

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                              db, local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, revert_list_read, &b,
                              scratch_pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_revert_list_notify(svn_wc_notify_func2_t notify_func,
                              void *notify_baton,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath, *like_arg;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                              db, local_abspath, scratch_pool, iterpool));
  VERIFY_USABLE_WCROOT(wcroot);

  like_arg = construct_like_arg(local_relpath, scratch_pool);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_REVERT_LIST_RECURSIVE));
  SVN_ERR(svn_sqlite__bindf(stmt, "ss", local_relpath, like_arg));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (!have_row)
    return svn_error_return(svn_sqlite__reset(stmt)); /* optimise for no row */
  while (have_row)
    {
      const char *notify_relpath = svn_sqlite__column_text(stmt, 0, NULL);

      svn_pool_clear(iterpool);

      if (svn_sqlite__column_int64(stmt, 1))
        {
          const char *notify_abspath = svn_dirent_join(wcroot->abspath,
                                                       notify_relpath,
                                                       iterpool);
          notify_func(notify_baton,
                      svn_wc_create_notify(notify_abspath,
                                           svn_wc_notify_revert,
                                           iterpool),
                      iterpool);

          /* ### Need cancel_func? */
        }
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_REVERT_LIST_RECURSIVE));
  SVN_ERR(svn_sqlite__bindf(stmt, "ss", local_relpath, like_arg));
  SVN_ERR(svn_sqlite__step_done(stmt));

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


/* Set *TREE_CONFLICT_DATA to a string describing any tree conflicts on
   immediate children of WCROOT+LOCAL_RELPATH. The format of the string is as
   produced by svn_wc__write_tree_conflicts().  */
static svn_error_t *
read_all_tree_conflicts(apr_hash_t **tree_conflicts,
                        svn_wc__db_wcroot_t *wcroot,
                        const char *local_relpath,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  *tree_conflicts = apr_hash_make(result_pool);

  /* Get the conflict information for children of LOCAL_ABSPATH. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                               STMT_SELECT_ACTUAL_CHILDREN_TREE_CONFLICT));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  while (have_row)
    {
      const char *child_basename;
      const char *child_relpath;
      const char *conflict_data;
      const svn_skel_t *skel;
      const svn_wc_conflict_description2_t *conflict;

      svn_pool_clear(iterpool);

      child_relpath = svn_sqlite__column_text(stmt, 0, NULL);
      child_basename = svn_relpath_basename(child_relpath, result_pool);

      conflict_data = svn_sqlite__column_text(stmt, 1, NULL);
      skel = svn_skel__parse(conflict_data, strlen(conflict_data), iterpool);
      SVN_ERR(svn_wc__deserialize_conflict(&conflict, skel, wcroot->abspath,
                                           result_pool, iterpool));

      apr_hash_set(*tree_conflicts, child_basename, APR_HASH_KEY_STRING,
                   conflict);

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_op_read_all_tree_conflicts(apr_hash_t **tree_conflicts,
                                      svn_wc__db_t *db,
                                      const char *local_abspath,
                                      apr_pool_t *result_pool,
                                      apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(read_all_tree_conflicts(tree_conflicts, wcroot, local_relpath,
                                  result_pool, scratch_pool));

  return SVN_NO_ERROR;
}


/* Like svn_wc__db_op_read_tree_conflict(), but with WCROOT+LOCAL_RELPATH
   instead of DB+LOCAL_ABSPATH.  */
static svn_error_t *
read_tree_conflict(const svn_wc_conflict_description2_t **tree_conflict,
                   svn_wc__db_wcroot_t *wcroot,
                   const char *local_relpath,
                   apr_pool_t *result_pool,
                   apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  const char *conflict_data;
  const svn_skel_t *skel;
  svn_error_t *err;

  *tree_conflict = NULL;

  if (!local_relpath[0])
    return SVN_NO_ERROR;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_ACTUAL_TREE_CONFLICT));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (!have_row)
    return svn_error_return(svn_sqlite__reset(stmt));

  conflict_data = svn_sqlite__column_text(stmt, 0, NULL);
  skel = svn_skel__parse(conflict_data, strlen(conflict_data), scratch_pool);
  err = svn_wc__deserialize_conflict(tree_conflict, skel,
                                     wcroot->abspath, result_pool,
                                     scratch_pool);

  return svn_error_compose_create(err,
                                  svn_sqlite__reset(stmt));
}


svn_error_t *
svn_wc__db_op_read_tree_conflict(
                     const svn_wc_conflict_description2_t **tree_conflict,
                     svn_wc__db_t *db,
                     const char *local_abspath,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));

  SVN_ERR(read_tree_conflict(tree_conflict, wcroot, local_relpath,
                             result_pool, scratch_pool));

  return SVN_NO_ERROR;
}

/* Baton for remove_post_commit_txn */
struct remove_node_baton
{
  svn_revnum_t not_present_rev;
  svn_wc__db_kind_t not_present_kind;
};

/* Implements svn_wc__db_txn_callback_t for svn_wc__db_op_remove_node */
static svn_error_t *
remove_node_txn(void *baton,
                svn_wc__db_wcroot_t *wcroot,
                const char *local_relpath,
                apr_pool_t *scratch_pool)
{
  struct remove_node_baton *rnb = baton;
  svn_sqlite__stmt_t *stmt;

  apr_int64_t repos_id;
  const char *repos_relpath;

  const char *like_arg = construct_like_arg(local_relpath, scratch_pool);

  SVN_ERR_ASSERT(*local_relpath != '\0'); /* Never on a wcroot */

  /* Need info for not_present node? */
  if (SVN_IS_VALID_REVNUM(rnb->not_present_rev))
    SVN_ERR(scan_upwards_for_repos(&repos_id, &repos_relpath, wcroot,
                                   local_relpath, scratch_pool, scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_NODES_RECURSIVE));

  /* Remove all nodes at or below local_relpath where op_depth >= 0 */
  SVN_ERR(svn_sqlite__bindf(stmt, "issi", wcroot->wc_id,
                                          local_relpath,
                                          like_arg,
                                          (apr_int64_t)0));
  SVN_ERR(svn_sqlite__step_done(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_ACTUAL_NODE_RECURSIVE));

  /* Delete all actual nodes at or below local_relpath */
  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id,
                                         local_relpath,
                                         like_arg));
  SVN_ERR(svn_sqlite__step_done(stmt));

  /* Should we leave a not-present node? */
  if (SVN_IS_VALID_REVNUM(rnb->not_present_rev))
    {
      insert_base_baton_t ibb;
      blank_ibb(&ibb);

      ibb.repos_id = repos_id;
      ibb.status = svn_wc__db_status_not_present;
      ibb.kind = rnb->not_present_kind;

      ibb.repos_relpath = repos_relpath;
      ibb.revision = rnb->not_present_rev;

      SVN_ERR(insert_base_node(&ibb, wcroot, local_relpath, scratch_pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_op_remove_node(svn_wc__db_t *db,
                          const char *local_abspath,
                          svn_revnum_t not_present_revision,
                          svn_wc__db_kind_t not_present_kind,
                          apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  struct remove_node_baton rnb;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  rnb.not_present_rev = not_present_revision;
  rnb.not_present_kind = not_present_kind;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, remove_node_txn,
                              &rnb, scratch_pool));

  /* ### Flush everything below this node in all ways */
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));
  SVN_ERR(svn_wc__db_temp_forget_directory(db, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_op_remove_working(svn_wc__db_t *db,
                                  const char *local_abspath,
                                  apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  svn_sqlite__stmt_t *stmt;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}


static svn_error_t *
update_depth_values(svn_wc__db_t *db,
                    const char *local_abspath,
                    svn_wc__db_wcroot_t *wcroot,
                    const char *local_relpath,
                    svn_depth_t depth,
                    apr_pool_t *scratch_pool)
{
  svn_boolean_t excluded = (depth == svn_depth_exclude);
  svn_sqlite__stmt_t *stmt;

  /* Flush any entries before we start monkeying the database.  */
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    excluded
                                      ? STMT_UPDATE_NODE_BASE_EXCLUDED
                                      : STMT_UPDATE_NODE_BASE_DEPTH));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  if (!excluded)
    SVN_ERR(svn_sqlite__bind_text(stmt, 3, svn_depth_to_word(depth)));
  SVN_ERR(svn_sqlite__step_done(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    excluded
                                      ? STMT_UPDATE_NODE_WORKING_EXCLUDED
                                      : STMT_UPDATE_NODE_WORKING_DEPTH));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  if (!excluded)
    SVN_ERR(svn_sqlite__bind_text(stmt, 3, svn_depth_to_word(depth)));
  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_op_set_dir_depth(svn_wc__db_t *db,
                                 const char *local_abspath,
                                 svn_depth_t depth,
                                  apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(depth >= svn_depth_empty && depth <= svn_depth_infinity);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* ### We set depth on working and base to match entry behavior.
         Maybe these should be separated later? */

  SVN_ERR(update_depth_values(db, local_abspath, wcroot, local_relpath, depth,
                              scratch_pool));

  return SVN_NO_ERROR;
}


/* Delete child sub-trees of LOCAL_RELPATH that are presence=not-present
   and at the same op_depth.

   ### Do we need to handle incomplete here? */
static svn_error_t *
remove_children(svn_wc__db_wcroot_t *wcroot,
                const char *local_relpath,
                svn_wc__db_status_t status,
                apr_int64_t op_depth,
                apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_CHILD_NODES_RECURSIVE));
  SVN_ERR(svn_sqlite__bindf(stmt, "isit", wcroot->wc_id,
                            construct_like_arg(local_relpath,
                                               scratch_pool),
                            op_depth, presence_map, status));
  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}


/* ### Forward declarations to avoid function movement churn.  */
static svn_error_t *
db_working_actual_remove(svn_wc__db_wcroot_t *wcroot,
                         const char *local_relpath,
                         apr_pool_t *scratch_pool);

static svn_error_t *
info_below_working(svn_boolean_t *have_base,
                   svn_boolean_t *have_work,
                   svn_wc__db_status_t *status,
                   svn_wc__db_wcroot_t *wcroot,
                   const char *local_relpath,
                   apr_pool_t *scratch_pool);


/* Update the working node for LOCAL_ABSPATH setting presence=STATUS */
static svn_error_t *
db_working_update_presence(apr_int64_t op_depth,
                           svn_wc__db_status_t status,
                           svn_wc__db_wcroot_t *wcroot,
                           const char *local_relpath,
                           apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_UPDATE_NODE_WORKING_PRESENCE));
  SVN_ERR(svn_sqlite__bindf(stmt, "ist", wcroot->wc_id, local_relpath,
                            presence_map, status));
  SVN_ERR(svn_sqlite__step_done(stmt));

  if (status == svn_wc__db_status_base_deleted)
    {
      /* Switching to base-deleted is undoing an add/copy.  By this
         stage an add will have no children. */
      const apr_array_header_t *children;
      apr_pool_t *iterpool;
      int i;

      /* Children of the copy will be marked deleted in the layer
         above. */
      SVN_ERR(remove_children(wcroot, local_relpath,
                              svn_wc__db_status_base_deleted, op_depth + 1,
                              scratch_pool));

      /* Children of the copy that overlay a lower level become
         base_deleted, otherwise they get removed. */
      SVN_ERR(gather_repo_children(&children, wcroot, local_relpath,
                                   op_depth, scratch_pool, scratch_pool));
      iterpool = svn_pool_create(scratch_pool);
      for (i = 0; i < children->nelts; ++i)
        {
          const char *name = APR_ARRAY_IDX(children, i, const char *);
          const char *child_relpath;
          svn_boolean_t below_base;
          svn_boolean_t below_work;
          svn_wc__db_status_t below_status;

          svn_pool_clear(iterpool);

          child_relpath = svn_relpath_join(local_relpath, name, iterpool);
          SVN_ERR(info_below_working(&below_base, &below_work, &below_status,
                                     wcroot, child_relpath, iterpool));
          if ((below_base || below_work)
              && (below_status == svn_wc__db_status_normal
                  || below_status == svn_wc__db_status_added
                  || below_status == svn_wc__db_status_incomplete))
            SVN_ERR(db_working_update_presence(op_depth,
                                               svn_wc__db_status_base_deleted,
                                               wcroot, child_relpath,
                                               iterpool));
          else
            SVN_ERR(db_working_actual_remove(wcroot, child_relpath, iterpool));
        }
      svn_pool_destroy(iterpool);

      /* Reset the copyfrom in case this was a copy.
         ### What else should be reset? Properties? Or copy the node again? */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_UPDATE_COPYFROM_TO_INHERIT));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  /* ### Should the switch to not-present remove an ACTUAL row? */

  return SVN_NO_ERROR;
}


/* Delete working and actual nodes for LOCAL_ABSPATH.  When called any
   remaining working child sub-trees should be presence=not-present
   and will be deleted. */
static svn_error_t *
db_working_actual_remove(svn_wc__db_wcroot_t *wcroot,
                         const char *local_relpath,
                         apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  apr_int64_t op_depth;

  /* Precondition: There is a working row in NODES.

     Record its op_depth, which is needed for postcondition checking. */
  {
    svn_boolean_t have_row;

    SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                      STMT_SELECT_WORKING_NODE));
    SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
    SVN_ERR(svn_sqlite__step(&have_row, stmt));
    SVN_ERR_ASSERT(have_row);
    op_depth = svn_sqlite__column_int64(stmt, 0);
    SVN_ERR(svn_sqlite__reset(stmt));
  }

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step_done(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_CLEAR_ACTUAL_NODE_LEAVING_CONFLICT));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step_done(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_ACTUAL_NODE_WITHOUT_CONFLICT));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step_done(stmt));

  SVN_ERR(remove_children(wcroot, local_relpath,
                          svn_wc__db_status_base_deleted,
                          op_depth + 1, scratch_pool));
  SVN_ERR(remove_children(wcroot, local_relpath, svn_wc__db_status_normal,
                          op_depth, scratch_pool));
  SVN_ERR(remove_children(wcroot, local_relpath, svn_wc__db_status_not_present,
                          op_depth, scratch_pool));
  SVN_ERR(remove_children(wcroot, local_relpath, svn_wc__db_status_incomplete,
                          op_depth, scratch_pool));

#ifdef SVN_DEBUG
  /* Postcondition: There are no NODES rows in this subtree, at same or
     greater op_depth. */
  {
    svn_boolean_t have_row;

    SVN_ERR(svn_sqlite__get_statement(
              &stmt, wcroot->sdb,
              STMT_SELECT_NODES_GE_OP_DEPTH_RECURSIVE));
    SVN_ERR(svn_sqlite__bindf(stmt, "issi", wcroot->wc_id, local_relpath,
                              construct_like_arg(local_relpath, scratch_pool),
                              op_depth));
    SVN_ERR(svn_sqlite__step(&have_row, stmt));
    SVN_ERR_ASSERT(! have_row);
    SVN_ERR(svn_sqlite__reset(stmt));
  }

  /* Postcondition: There are no ACTUAL_NODE rows in this subtree, save
     those with conflict information. */
  {
    svn_boolean_t have_row;

    SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                      STMT_SELECT_ACTUAL_NODE_RECURSIVE));
    SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                              construct_like_arg(local_relpath,
                                                 scratch_pool)));
    SVN_ERR(svn_sqlite__step(&have_row, stmt));
    SVN_ERR_ASSERT(! have_row);
    SVN_ERR(svn_sqlite__reset(stmt));
  }
#endif

  return SVN_NO_ERROR;
}


/* Insert a working node for LOCAL_ABSPATH with presence=STATUS. */
static svn_error_t *
db_working_insert(svn_wc__db_status_t status,
                  svn_wc__db_wcroot_t *wcroot,
                  const char *local_relpath,
                  apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  const char *like_arg = construct_like_arg(local_relpath, scratch_pool);
  apr_int64_t op_depth = relpath_depth(local_relpath);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_INSERT_WORKING_NODE_FROM_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "isit", wcroot->wc_id,
                            local_relpath, op_depth, presence_map, status));
  SVN_ERR(svn_sqlite__insert(NULL, stmt));

  /* Need to update the op_depth of all deleted child trees -- this
     relies on the recursion having already deleted the trees so
     that they are all at op_depth+1.

     ### Rewriting the op_depth means that the number of queries is
     ### O(depth^2).  Fix it by implementing svn_wc__db_op_delete so
     ### that the recursion gets moved from adm_ops.c to wc_db.c and
     ### one transaction does the whole tree and thus each op_depth
     ### only gets written once. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_UPDATE_OP_DEPTH_REDUCE_RECURSIVE));
  SVN_ERR(svn_sqlite__bindf(stmt, "isi",
                            wcroot->wc_id, like_arg, op_depth + 1));
  SVN_ERR(svn_sqlite__update(NULL, stmt));

  return SVN_NO_ERROR;
}


/* Set *ROOT_OF_COPY to TRUE if LOCAL_ABSPATH is an add or the root of
   a copy, to FALSE otherwise. */
static svn_error_t*
is_add_or_root_of_copy(svn_boolean_t *add_or_root_of_copy,
                       svn_wc__db_wcroot_t *wcroot,
                       const char *local_relpath,
                       apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  apr_int64_t op_depth;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_WORKING_NODE));

  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step_row(stmt));

  op_depth = svn_sqlite__column_int64(stmt, 0);

  *add_or_root_of_copy = (op_depth == relpath_depth(local_relpath));

  return svn_error_return(svn_sqlite__reset(stmt));
}


/* Convert STATUS, the raw status obtained from the presence map, to
   the status appropriate for a working (op_depth > 0) node and return
   it in *WORKING_STATUS. */
static svn_error_t *
convert_to_working_status(svn_wc__db_status_t *working_status,
                          svn_wc__db_status_t status)
{
  svn_wc__db_status_t work_status = status;

  SVN_ERR_ASSERT(work_status == svn_wc__db_status_normal
                 || work_status == svn_wc__db_status_not_present
                 || work_status == svn_wc__db_status_base_deleted
                 || work_status == svn_wc__db_status_incomplete
                 || work_status == svn_wc__db_status_excluded);

  if (work_status == svn_wc__db_status_incomplete)
    {
      *working_status = svn_wc__db_status_incomplete;
    }
  else if (work_status == svn_wc__db_status_excluded)
    {
      *working_status = svn_wc__db_status_excluded;
    }
  else if (work_status == svn_wc__db_status_not_present
           || work_status == svn_wc__db_status_base_deleted)
    {
      /* The caller should scan upwards to detect whether this
         deletion has occurred because this node has been moved
         away, or it is a regular deletion. Also note that the
         deletion could be of the BASE tree, or a child of
         something that has been copied/moved here. */

      *working_status = svn_wc__db_status_deleted;
    }
  else /* normal */
    {
      /* The caller should scan upwards to detect whether this
         addition has occurred because of a simple addition,
         a copy, or is the destination of a move. */
      *working_status = svn_wc__db_status_added;
    }

  return SVN_NO_ERROR;
}


/* Return the status of the node, if any, below the "working" node.
   Set *HAVE_BASE or *HAVE_WORK to indicate if a base node or lower
   working node is present, and *STATUS to the status of the node. */
static svn_error_t *
info_below_working(svn_boolean_t *have_base,
                   svn_boolean_t *have_work,
                   svn_wc__db_status_t *status,
                   svn_wc__db_wcroot_t *wcroot,
                   const char *local_relpath,
                   apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  *have_base = *have_work =  FALSE;
  *status = svn_wc__db_status_normal;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_NODE_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (have_row)
    {
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
      if (have_row)
        *status = svn_sqlite__column_token(stmt, 3, presence_map);

      while (have_row)
        {
          apr_int64_t op_depth = svn_sqlite__column_int64(stmt, 0);

          if (op_depth > 0)
            *have_work = TRUE;
          else
            *have_base = TRUE;

          SVN_ERR(svn_sqlite__step(&have_row, stmt));
        }
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  if (*have_work)
    SVN_ERR(convert_to_working_status(status, *status));

  return SVN_NO_ERROR;
}


struct temp_op_delete_baton_t {
  /* The following two are only needed for svn_wc__db_temp_forget_directory */
  svn_wc__db_t *db;
  const char *local_abspath;
};


/* Deletes LOCAL_RELPATH using WCROOT. */
static svn_error_t *
temp_op_delete_txn(void *baton,
                   svn_wc__db_wcroot_t *wcroot,
                   const char *local_relpath,
                   apr_pool_t *scratch_pool)
{
  struct temp_op_delete_baton_t *b = baton;
  svn_wc__db_status_t status;
  svn_wc__db_status_t new_working_status;
  svn_boolean_t have_work;
  svn_boolean_t add_work = FALSE;
  svn_boolean_t del_work = FALSE;
  svn_boolean_t mod_work = FALSE;

  SVN_ERR(read_info(&status,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL,
                    NULL, NULL, &have_work,
                    wcroot, local_relpath,
                    scratch_pool, scratch_pool));

  if (!have_work)
    {
      /* No structural changes  */
      if (status == svn_wc__db_status_normal
          || status == svn_wc__db_status_incomplete)
        {
          add_work = TRUE;
        }
    }
  else if (status == svn_wc__db_status_added)
    {
      /* ADD/COPY-HERE/MOVE-HERE that could be a replace */
      svn_boolean_t add_or_root_of_copy;

      SVN_ERR(is_add_or_root_of_copy(&add_or_root_of_copy, wcroot,
                                     local_relpath, scratch_pool));
      if (add_or_root_of_copy)
        {
          svn_boolean_t below_base;
          svn_boolean_t below_work;
          svn_wc__db_status_t below_status;

          SVN_ERR(info_below_working(&below_base, &below_work, &below_status,
                                     wcroot, local_relpath, scratch_pool));

          if ((below_base || below_work)
              && below_status != svn_wc__db_status_not_present
              && below_status != svn_wc__db_status_deleted)
            mod_work = TRUE;
          else
            del_work = TRUE;
        }
      else
        {
          add_work = TRUE;
        }
    }
  else if (status == svn_wc__db_status_incomplete)
    {
      svn_boolean_t add_or_root_of_copy;
      SVN_ERR(is_add_or_root_of_copy(&add_or_root_of_copy, wcroot,
                                     local_relpath, scratch_pool));
      if (add_or_root_of_copy)
        del_work = TRUE;
      else
        add_work = TRUE;
    }

  if (del_work)
    {
      SVN_ERR(db_working_actual_remove(wcroot, local_relpath,
                                       scratch_pool));

      /* This is needed for access batons? */
      SVN_ERR(svn_wc__db_temp_forget_directory(b->db, b->local_abspath,
                                               scratch_pool));
    }
  else if (add_work)
    {
      new_working_status = svn_wc__db_status_base_deleted;
      SVN_ERR(db_working_insert(new_working_status, wcroot,
                                local_relpath, scratch_pool));
    }
  else if (mod_work)
    {
      new_working_status = svn_wc__db_status_base_deleted;
      SVN_ERR(db_working_update_presence(relpath_depth(local_relpath),
                                         new_working_status, wcroot,
                                         local_relpath, scratch_pool));
    }
  else
    {
      /* Already deleted, or absent or excluded. */
      /* ### Nothing to do, return an error?  Which one? */
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_op_delete(svn_wc__db_t *db,
                          const char *local_abspath,
                          apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct temp_op_delete_baton_t b;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                              db, local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* These two for svn_wc__db_temp_forget_directory */
  b.db = db;
  b.local_abspath = local_abspath;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, temp_op_delete_txn, &b,
                              scratch_pool));

  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}

struct op_delete_baton_t {
  apr_int64_t delete_depth;  /* op-depth for root of delete */
};

static svn_error_t *
op_delete_txn(void *baton,
              svn_wc__db_wcroot_t *wcroot,
              const char *local_relpath,
              apr_pool_t *scratch_pool)
{
  struct op_delete_baton_t *b = baton;
  svn_wc__db_status_t status;
  svn_boolean_t have_base, have_work;

  SVN_ERR(read_info(&status,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL,
                    &have_base, NULL, &have_work,
                    wcroot, local_relpath,
                    scratch_pool, scratch_pool));

  if (have_base && !have_work)
    {
      svn_sqlite__stmt_t *stmt;
      const char *like_arg = construct_like_arg(local_relpath, scratch_pool);

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_DELETE_WORKING_NODE_NOT_DELETED));
      SVN_ERR(svn_sqlite__bindf(stmt, "isi",
                                wcroot->wc_id, like_arg, b->delete_depth));
      SVN_ERR(svn_sqlite__update(NULL, stmt));

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_UPDATE_OP_DEPTH_RECURSIVE));
      SVN_ERR(svn_sqlite__bindf(stmt, "isi",
                                wcroot->wc_id, like_arg, b->delete_depth));
      SVN_ERR(svn_sqlite__update(NULL, stmt));

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                 STMT_INSERT_WORKING_NODE_FROM_NODE_RECURSIVE));
      SVN_ERR(svn_sqlite__bindf(stmt, "issi",
                                wcroot->wc_id, local_relpath, like_arg,
                                b->delete_depth));
      SVN_ERR(svn_sqlite__insert(NULL, stmt));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_op_delete(svn_wc__db_t *db,
                     const char *local_abspath,
                     apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct op_delete_baton_t b;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                              db, local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  b.delete_depth = relpath_depth(local_relpath);

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, op_delete_txn, &b,
                              scratch_pool));

  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


/* Like svn_wc__db_read_info(), but taking WCROOT+LOCAL_RELPATH instead of
   DB+LOCAL_ABSPATH, and outputting repos ids instead of URL+UUID. */
static svn_error_t *
read_info(svn_wc__db_status_t *status,
          svn_wc__db_kind_t *kind,
          svn_revnum_t *revision,
          const char **repos_relpath,
          apr_int64_t *repos_id,
          svn_revnum_t *changed_rev,
          apr_time_t *changed_date,
          const char **changed_author,
          svn_depth_t *depth,
          const svn_checksum_t **checksum,
          const char **target,
          const char **original_repos_relpath,
          apr_int64_t *original_repos_id,
          svn_revnum_t *original_revision,
          svn_wc__db_lock_t **lock,
          svn_filesize_t *recorded_size,
          apr_time_t *recorded_mod_time,
          const char **changelist,
          svn_boolean_t *conflicted,
          svn_boolean_t *op_root,
          svn_boolean_t *had_props,
          svn_boolean_t *props_mod,
          svn_boolean_t *have_base,
          svn_boolean_t *have_more_work,
          svn_boolean_t *have_work,
          svn_wc__db_wcroot_t *wcroot,
          const char *local_relpath,
          apr_pool_t *result_pool,
          apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt_info;
  svn_sqlite__stmt_t *stmt_act;
  svn_boolean_t have_info;
  svn_boolean_t have_act;
  svn_error_t *err = NULL;

  /* Obtain the most likely to exist record first, to make sure we don't
     have to obtain the SQLite read-lock multiple times */
  SVN_ERR(svn_sqlite__get_statement(&stmt_info, wcroot->sdb,
                                    lock ? STMT_SELECT_NODE_INFO_WITH_LOCK
                                         : STMT_SELECT_NODE_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt_info, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_info, stmt_info));

  if (changelist || conflicted || props_mod)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt_act, wcroot->sdb,
                                        STMT_SELECT_ACTUAL_NODE));
      SVN_ERR(svn_sqlite__bindf(stmt_act, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step(&have_act, stmt_act));
    }
  else
    {
      have_act = FALSE;
      stmt_act = NULL;
    }

  if (have_info)
    {
      apr_int64_t op_depth;
      svn_wc__db_kind_t node_kind;

      op_depth = svn_sqlite__column_int64(stmt_info, 0);
      node_kind = svn_sqlite__column_token(stmt_info, 4, kind_map);

      if (status)
        {
          *status = svn_sqlite__column_token(stmt_info, 3, presence_map);

          if (op_depth != 0) /* WORKING */
            err = svn_error_compose_create(err,
                                           convert_to_working_status(status,
                                                                     *status));
        }
      if (kind)
        {
          *kind = node_kind;
        }
      if (op_depth != 0)
        {
          if (repos_id)
            *repos_id = INVALID_REPOS_ID;
          if (revision)
            *revision = SVN_INVALID_REVNUM;
          if (repos_relpath)
            /* Our path is implied by our parent somewhere up the tree.
               With the NULL value and status, the caller will know to
               search up the tree for the base of our path.  */
            *repos_relpath = NULL;
        }
      else
        {
          /* Fetch repository information. If we have a
             WORKING_NODE (and have been added), then the repository
             we're being added to will be dependent upon a parent. The
             caller can scan upwards to locate the repository.  */
          err = svn_error_compose_create(
            err, repos_location_from_columns(repos_id, revision, repos_relpath,
                                             stmt_info, 1, 5, 2, result_pool));
        }
      if (changed_rev)
        {
          *changed_rev = svn_sqlite__column_revnum(stmt_info, 8);
        }
      if (changed_date)
        {
          *changed_date = svn_sqlite__column_int64(stmt_info, 9);
        }
      if (changed_author)
        {
          *changed_author = svn_sqlite__column_text(stmt_info, 10,
                                                    result_pool);
        }
      if (recorded_mod_time)
        {
          *recorded_mod_time = svn_sqlite__column_int64(stmt_info, 13);
        }
      if (depth)
        {
          if (node_kind != svn_wc__db_kind_dir)
            {
              *depth = svn_depth_unknown;
            }
          else
            {
              const char *depth_str;

              depth_str = svn_sqlite__column_text(stmt_info, 11, NULL);

              if (depth_str == NULL)
                *depth = svn_depth_unknown;
              else
                *depth = svn_depth_from_word(depth_str);
            }
        }
      if (checksum)
        {
          if (node_kind != svn_wc__db_kind_file)
            {
              *checksum = NULL;
            }
          else
            {
              svn_error_t *err2;
              err2 = svn_sqlite__column_checksum(checksum, stmt_info, 6,
                                                 result_pool);

              if (err2 != NULL)
                err = svn_error_compose_create(
                         err,
                         svn_error_createf(
                               err->apr_err, err2,
                              _("The node '%s' has a corrupt checksum value."),
                              path_for_error_message(wcroot, local_relpath,
                                                     scratch_pool)));
            }
        }
      if (recorded_size)
        {
          *recorded_size = get_translated_size(stmt_info, 7);
        }
      if (target)
        {
          if (node_kind != svn_wc__db_kind_symlink)
            *target = NULL;
          else
            *target = svn_sqlite__column_text(stmt_info, 12, result_pool);
        }
      if (changelist)
        {
          if (have_act)
            *changelist = svn_sqlite__column_text(stmt_act, 1, result_pool);
          else
            *changelist = NULL;
        }
      if (op_depth == 0)
        {
          if (original_repos_id)
            *original_repos_id = INVALID_REPOS_ID;
          if (original_revision)
            *original_revision = SVN_INVALID_REVNUM;
          if (original_repos_relpath)
            *original_repos_relpath = NULL;
        }
      else
        {
          err = svn_error_compose_create(
            err, repos_location_from_columns(original_repos_id,
                                             original_revision,
                                             original_repos_relpath,
                                             stmt_info, 1, 5, 2, result_pool));
        }
      if (props_mod)
        {
          *props_mod = have_act && !svn_sqlite__column_is_null(stmt_act, 6);
        }
      if (had_props)
        {
          *had_props = SQLITE_PROPERTIES_AVAILABLE(stmt_info, 14);
        }
      if (conflicted)
        {
          if (have_act)
            {
              *conflicted =
                 !svn_sqlite__column_is_null(stmt_act, 2) || /* old */
                 !svn_sqlite__column_is_null(stmt_act, 3) || /* new */
                 !svn_sqlite__column_is_null(stmt_act, 4) || /* working */
                 !svn_sqlite__column_is_null(stmt_act, 0) || /* prop_reject */
                 !svn_sqlite__column_is_null(stmt_act, 5); /* tree_conflict_data */
            }
          else
            *conflicted = FALSE;
        }

      if (lock)
        {
          if (op_depth != 0)
            *lock = NULL;
          else
            *lock = lock_from_columns(stmt_info, 15, 16, 17, 18, result_pool);
        }

      if (have_work)
        *have_work = (op_depth != 0);

      if (op_root)
        {
          *op_root = ((op_depth > 0)
                      && (op_depth == relpath_depth(local_relpath)));
        }

      if (have_base || have_more_work)
        {
          if (have_more_work)
            *have_more_work = FALSE;

          while (!err && op_depth != 0)
            {
              err = svn_sqlite__step(&have_info, stmt_info);

              if (err || !have_info)
                break;

              op_depth = svn_sqlite__column_int64(stmt_info, 0);

              if (have_more_work)
                {
                  if (op_depth > 0)
                    *have_more_work = TRUE;

                  if (!have_base)
                   break;
                }
            }

          if (have_base)
            *have_base = (op_depth == 0);
        }
    }
  else if (have_act)
    {
      /* A row in ACTUAL_NODE should never exist without a corresponding
         node in BASE_NODE and/or WORKING_NODE unless it flags a conflict. */
      if (svn_sqlite__column_is_null(stmt_act, 5)) /* conflict_data */
          err = svn_error_createf(SVN_ERR_WC_CORRUPT, NULL,
                                  _("Corrupt data for '%s'"),
                                  path_for_error_message(wcroot, local_relpath,
                                                         scratch_pool));
      /* ### What should we return?  Should we have a separate
             function for reading actual-only nodes? */

      /* As a safety measure, until we decide if we want to use
         read_info for actual-only nodes, make sure the caller asked
         for the conflict status. */
      SVN_ERR_ASSERT(conflicted);

      if (status)
        *status = svn_wc__db_status_normal;  /* What! No it's not! */
      if (kind)
        *kind = svn_wc__db_kind_unknown;
      if (revision)
        *revision = SVN_INVALID_REVNUM;
      if (repos_relpath)
        *repos_relpath = NULL;
      if (repos_id)
        *repos_id = INVALID_REPOS_ID;
      if (changed_rev)
        *changed_rev = SVN_INVALID_REVNUM;
      if (changed_date)
        *changed_date = 0;
      if (depth)
        *depth = svn_depth_unknown;
      if (checksum)
        *checksum = NULL;
      if (target)
        *target = NULL;
      if (original_repos_relpath)
        *original_repos_relpath = NULL;
      if (original_repos_id)
        *original_repos_id = INVALID_REPOS_ID;
      if (original_revision)
        *original_revision = SVN_INVALID_REVNUM;
      if (lock)
        *lock = NULL;
      if (recorded_size)
        *recorded_size = 0;
      if (recorded_mod_time)
        *recorded_mod_time = 0;
      if (changelist)
        *changelist = svn_sqlite__column_text(stmt_act, 1, result_pool);
      if (op_root)
        *op_root = FALSE;
      if (had_props)
        *had_props = FALSE;
      if (props_mod)
        *props_mod = FALSE;
      if (conflicted)
        *conflicted = TRUE;
      if (have_base)
        *have_base = FALSE;
      if (have_more_work)
        *have_more_work = FALSE;
      if (have_work)
        *have_work = FALSE;
    }
  else
    {
      err = svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                              _("The node '%s' was not found."),
                              path_for_error_message(wcroot, local_relpath,
                                                     scratch_pool));
    }

  if (stmt_act != NULL)
    err = svn_error_compose_create(err, svn_sqlite__reset(stmt_act));

  SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt_info)));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_read_info(svn_wc__db_status_t *status,
                     svn_wc__db_kind_t *kind,
                     svn_revnum_t *revision,
                     const char **repos_relpath,
                     const char **repos_root_url,
                     const char **repos_uuid,
                     svn_revnum_t *changed_rev,
                     apr_time_t *changed_date,
                     const char **changed_author,
                     svn_depth_t *depth,
                     const svn_checksum_t **checksum,
                     const char **target,
                     const char **original_repos_relpath,
                     const char **original_root_url,
                     const char **original_uuid,
                     svn_revnum_t *original_revision,
                     svn_wc__db_lock_t **lock,
                     svn_filesize_t *recorded_size,
                     apr_time_t *recorded_mod_time,
                     const char **changelist,
                     svn_boolean_t *conflicted,
                     svn_boolean_t *op_root,
                     svn_boolean_t *have_props,
                     svn_boolean_t *props_mod,
                     svn_boolean_t *have_base,
                     svn_boolean_t *have_more_work,
                     svn_boolean_t *have_work,
                     svn_wc__db_t *db,
                     const char *local_abspath,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  apr_int64_t repos_id, original_repos_id;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(read_info(status, kind, revision, repos_relpath, &repos_id,
                    changed_rev, changed_date, changed_author,
                    depth, checksum, target, original_repos_relpath,
                    &original_repos_id, original_revision, lock,
                    recorded_size, recorded_mod_time, changelist, conflicted,
                    op_root, have_props, props_mod,
                    have_base, have_more_work, have_work,
                    wcroot, local_relpath, result_pool, scratch_pool));
  SVN_ERR(fetch_repos_info(repos_root_url, repos_uuid,
                           wcroot->sdb, repos_id, result_pool));
  SVN_ERR(fetch_repos_info(original_root_url, original_uuid,
                           wcroot->sdb, original_repos_id, result_pool));

  return SVN_NO_ERROR;
}

/* baton for read_children_info() */
struct read_children_info_baton_t
{
  apr_hash_t *nodes;
  apr_hash_t *conflicts;
  apr_pool_t *result_pool;
};

/* What we really want to store about a node */
struct read_children_info_item_t
{
  struct svn_wc__db_info_t info;
  apr_int64_t op_depth;
};

static svn_error_t *
read_children_info(void *baton,
                   svn_wc__db_wcroot_t *wcroot,
                   const char *dir_relpath,
                   apr_pool_t *scratch_pool)
{
  struct read_children_info_baton_t *rci = baton;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  const char *repos_root_url = NULL;
  apr_int64_t last_repos_id;
  apr_hash_t *nodes = rci->nodes;
  apr_hash_t *conflicts = rci->conflicts;
  apr_pool_t *result_pool = rci->result_pool;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_NODE_CHILDREN_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, dir_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  
  while (have_row)
    {
      /* CHILD item points to what we have about the node. We only provide
         CHILD->item to our caller. */
      struct read_children_info_item_t *child_item;
      const char *child_relpath = svn_sqlite__column_text(stmt, 19, NULL);
      const char *name = svn_relpath_basename(child_relpath, NULL);
      svn_error_t *err;
      apr_int64_t op_depth;
      svn_boolean_t new_child;

      child_item = apr_hash_get(nodes, name, APR_HASH_KEY_STRING);
      if (child_item)
        new_child = FALSE;
      else
        {
          child_item = apr_pcalloc(result_pool, sizeof(*child_item));
          new_child = TRUE;
        }

      op_depth = svn_sqlite__column_int(stmt, 0);

      /* Do we have new or better information? */
      if (new_child || op_depth > child_item->op_depth)
        {
          struct svn_wc__db_info_t *child = &child_item->info;
          child_item->op_depth = op_depth;

          child->kind = svn_sqlite__column_token(stmt, 4, kind_map);

          child->status = svn_sqlite__column_token(stmt, 3, presence_map);
          if (op_depth != 0)
            {
              err = convert_to_working_status(&child->status, child->status);
              if (err)
                SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));
            }

          if (op_depth != 0)
            child->revnum = SVN_INVALID_REVNUM;
          else
            child->revnum = svn_sqlite__column_revnum(stmt, 5);

          if (op_depth != 0)
            child->repos_relpath = NULL;
          else
            child->repos_relpath = svn_sqlite__column_text(stmt, 2,
                                                           result_pool);

          if (op_depth != 0 || svn_sqlite__column_is_null(stmt, 1))
            {
              child->repos_root_url = NULL;
            }
          else
            {
              apr_int64_t repos_id = svn_sqlite__column_int64(stmt, 1);
              if (!repos_root_url)
                {
                  err = fetch_repos_info(&repos_root_url, NULL,
                                         wcroot->sdb, repos_id, result_pool);
                  if (err)
                    SVN_ERR(svn_error_compose_create(err,
                                                     svn_sqlite__reset(stmt)));
                  last_repos_id = repos_id;
                }

              /* Assume working copy is all one repos_id so that a
                 single cached value is sufficient. */
              SVN_ERR_ASSERT(repos_id == last_repos_id);
              child->repos_root_url = repos_root_url;
            }

          child->changed_rev = svn_sqlite__column_revnum(stmt, 8);

          child->changed_date = svn_sqlite__column_int64(stmt, 9);

          child->changed_author = svn_sqlite__column_text(stmt, 10,
                                                          result_pool);

          if (child->kind != svn_wc__db_kind_dir)
            child->depth = svn_depth_unknown;
          else
            {
              const char *depth = svn_sqlite__column_text(stmt, 11,
                                                          scratch_pool);
              if (depth)
                child->depth = svn_depth_from_word(depth);
              else
                child->depth = svn_depth_unknown;
            }

          child->recorded_mod_time = svn_sqlite__column_int64(stmt, 13);
          child->recorded_size = get_translated_size(stmt, 7);
          child->had_props = SQLITE_PROPERTIES_AVAILABLE(stmt, 14);
#ifdef HAVE_SYMLINK
          if (child->had_props)
            {
              apr_hash_t *properties;
              err = svn_sqlite__column_properties(&properties, stmt, 14,
                                                  scratch_pool, scratch_pool);
              if (err)
                SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));

              child->special = (child->had_props
                                && apr_hash_get(properties, SVN_PROP_SPECIAL,
                                              APR_HASH_KEY_STRING));
            }
#endif
          if (op_depth == 0)
            child->op_root = FALSE;
          else
            child->op_root = (op_depth == relpath_depth(child_relpath));

          apr_hash_set(nodes, apr_pstrdup(result_pool, name),
                       APR_HASH_KEY_STRING, child);
        }

      if (op_depth == 0)
        {
          child_item->info.have_base = TRUE;

          /* Get the lock info. The query only reports lock info in the row at
            * op_depth 0. */
          if (op_depth == 0)
            child_item->info.lock = lock_from_columns(stmt, 15, 16, 17, 18,
                                                      result_pool);
        }

      err = svn_sqlite__step(&have_row, stmt);
      if (err)
        SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_ACTUAL_CHILDREN_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, dir_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  while (have_row)
    {
      struct read_children_info_item_t *child_item;
      struct svn_wc__db_info_t *child;
      const char *child_relpath = svn_sqlite__column_text(stmt, 7, NULL);
      const char *name = svn_relpath_basename(child_relpath, NULL);
      svn_error_t *err;

      child_item = apr_hash_get(nodes, name, APR_HASH_KEY_STRING);
      if (!child_item)
        {
          child_item = apr_pcalloc(result_pool, sizeof(*child_item));
          child_item->info.status = svn_wc__db_status_not_present;
        }

      child = &child_item->info;

      child->changelist = svn_sqlite__column_text(stmt, 1, result_pool);

      child->props_mod = !svn_sqlite__column_is_null(stmt, 6);
#ifdef HAVE_SYMLINK
      if (child->props_mod)
        {
          apr_hash_t *properties;

          err = svn_sqlite__column_properties(&properties, stmt, 6,
                                              scratch_pool, scratch_pool);
          if (err)
            SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));
          child->special = (NULL != apr_hash_get(properties, SVN_PROP_SPECIAL,
                                                 APR_HASH_KEY_STRING));
        }
#endif

      child->conflicted = !svn_sqlite__column_is_null(stmt, 2) ||  /* old */
                          !svn_sqlite__column_is_null(stmt, 3) ||  /* new */
                          !svn_sqlite__column_is_null(stmt, 4) ||  /* work */
                          !svn_sqlite__column_is_null(stmt, 0) ||  /* prop */
                          !svn_sqlite__column_is_null(stmt, 5);  /* tree */

      if (child->conflicted)
        apr_hash_set(conflicts, apr_pstrdup(result_pool, name),
                     APR_HASH_KEY_STRING, "");

      err = svn_sqlite__step(&have_row, stmt);
      if (err)
        SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_read_children_info(apr_hash_t **nodes,
                              apr_hash_t **conflicts,
                              svn_wc__db_t *db,
                              const char *dir_abspath,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  struct read_children_info_baton_t rci;
  svn_wc__db_wcroot_t *wcroot;
  const char *dir_relpath;

  *conflicts = apr_hash_make(result_pool);
  *nodes = apr_hash_make(result_pool);
  SVN_ERR_ASSERT(svn_dirent_is_absolute(dir_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &dir_relpath, db,
                                                dir_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  rci.result_pool = result_pool;
  rci.conflicts = *conflicts;
  rci.nodes = *nodes;

  SVN_ERR(svn_wc__db_with_txn(wcroot, dir_relpath, read_children_info, &rci,
                              scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_read_children_walker_info(apr_hash_t **nodes,
                                     svn_wc__db_t *db,
                                     const char *dir_abspath,
                                     apr_pool_t *result_pool,
                                     apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *dir_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_int64_t op_depth;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(dir_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &dir_relpath, db,
                                             dir_abspath,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_NODE_CHILDREN_WALKER_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, dir_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  *nodes = apr_hash_make(result_pool);
  while (have_row)
    {
      struct svn_wc__db_walker_info_t *child;
      const char *child_relpath = svn_sqlite__column_text(stmt, 0, NULL);
      const char *name = svn_relpath_basename(child_relpath, NULL);
      svn_error_t *err;

      child = apr_hash_get(*nodes, name, APR_HASH_KEY_STRING);
      if (child == NULL)
        child = apr_palloc(result_pool, sizeof(*child));

      op_depth = svn_sqlite__column_int(stmt, 1);
      child->status = svn_sqlite__column_token(stmt, 2, presence_map);
      if (op_depth > 0)
        {
          err = convert_to_working_status(&child->status, child->status);
          if (err)
            SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));
        }
      child->kind = svn_sqlite__column_token(stmt, 3, kind_map);
      apr_hash_set(*nodes, apr_pstrdup(result_pool, name),
                   APR_HASH_KEY_STRING, child);

      err = svn_sqlite__step(&have_row, stmt);
      if (err)
        SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_read_node_install_info(const char **wcroot_abspath,
                                  svn_wc__db_status_t *status,
                                  svn_wc__db_kind_t *kind,
                                  const svn_checksum_t **sha1_checksum,
                                  const char **target,
                                  apr_hash_t **pristine_props,
                                  svn_wc__db_t *db,
                                  const char *local_abspath,
                                  apr_pool_t *result_pool,
                                  apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_error_t *err = NULL;

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  if (wcroot_abspath != NULL)
    *wcroot_abspath = apr_pstrdup(result_pool, wcroot->abspath);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_NODE_INFO));

  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));

  SVN_ERR(svn_sqlite__step_row(stmt)); /* Row must exist */

  if (status)
    {
      apr_int64_t op_depth = svn_sqlite__column_int64(stmt, 0);

      *status = svn_sqlite__column_token(stmt, 3, presence_map);

      if (op_depth > 0)
        err = convert_to_working_status(status, *status);
    }

  if (kind)
    *kind = svn_sqlite__column_token(stmt, 4, kind_map);

  if (!err && sha1_checksum)
    err = svn_sqlite__column_checksum(sha1_checksum, stmt, 6, result_pool);

  if (target)
    *target = svn_sqlite__column_text(stmt, 12, result_pool);

  if (!err && pristine_props)
    err = svn_sqlite__column_properties(pristine_props, stmt, 14, result_pool,
                                        scratch_pool);

  return svn_error_compose_create(err,
                                  svn_sqlite__reset(stmt));
}



struct read_url_baton_t {
  const char **url;
  apr_pool_t *result_pool;
};


static svn_error_t *
read_url_txn(void *baton,
             svn_wc__db_wcroot_t *wcroot,
             const char *local_relpath,
             apr_pool_t *scratch_pool)
{
  struct read_url_baton_t *rub = baton;
  svn_wc__db_status_t status;
  const char *repos_relpath;
  const char *repos_root_url;
  apr_int64_t repos_id;
  svn_boolean_t have_base;

  SVN_ERR(read_info(&status, NULL, NULL, &repos_relpath, &repos_id, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    &have_base, NULL, NULL,
                    wcroot, local_relpath, scratch_pool, scratch_pool));

  if (repos_relpath == NULL)
    {
      if (status == svn_wc__db_status_added)
        {
          SVN_ERR(scan_addition(NULL, NULL, &repos_relpath, &repos_id, NULL,
                                NULL, NULL, wcroot, local_relpath,
                                scratch_pool, scratch_pool));
        }
      else if (have_base)
        {
          SVN_ERR(scan_upwards_for_repos(&repos_id, &repos_relpath,
                                         wcroot, local_relpath,
                                         scratch_pool, scratch_pool));
        }
      else if (status == svn_wc__db_status_absent
               || status == svn_wc__db_status_excluded
               || status == svn_wc__db_status_not_present
               || (!have_base && (status == svn_wc__db_status_deleted)))
        {
          const char *parent_relpath;
          struct read_url_baton_t new_rub;
          const char *url;

          /* Set 'repos_root_url' to the *full URL* of the parent WC dir,
           * and 'repos_relpath' to the *single path component* that is the
           * basename of this WC directory, so that joining them will result
           * in the correct full URL. */
          svn_relpath_split(&parent_relpath, &repos_relpath, local_relpath,
                            scratch_pool);
          new_rub.result_pool = scratch_pool;
          new_rub.url = &url;
          SVN_ERR(read_url_txn(&new_rub, wcroot, parent_relpath,
                               scratch_pool));
        }
      else
        {
          /* Status: obstructed, obstructed_add */
          *rub->url = NULL;
          return SVN_NO_ERROR;
        }
    }

  SVN_ERR(fetch_repos_info(&repos_root_url, NULL, wcroot->sdb, repos_id,
                           scratch_pool));

  SVN_ERR_ASSERT(repos_root_url != NULL && repos_relpath != NULL);
  *rub->url = svn_path_url_add_component2(repos_root_url, repos_relpath,
                                          rub->result_pool);

  return SVN_NO_ERROR;
}


static svn_error_t *
read_url(const char **url,
         svn_wc__db_wcroot_t *wcroot,
         const char *local_relpath,
         apr_pool_t *result_pool,
         apr_pool_t *scratch_pool)
{
  struct read_url_baton_t rub = { url, result_pool };
  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, read_url_txn, &rub,
                              scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_read_url(const char **url,
                    svn_wc__db_t *db,
                    const char *local_abspath,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                                                local_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  return svn_error_return(read_url(url, wcroot, local_relpath, result_pool,
                                   scratch_pool));
}

/* Baton for db_read_props */
struct db_read_props_baton_t
{
  apr_hash_t *props;
  apr_pool_t *result_pool;
};


/* Helper for svn_wc__db_read_props(). Implements svn_wc__db_txn_callback_t */
static svn_error_t *
db_read_props(void *baton,
              svn_wc__db_wcroot_t *wcroot,
              const char *local_relpath,
              apr_pool_t *scratch_pool)
{
  struct db_read_props_baton_t *rpb = baton;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  svn_error_t *err = NULL;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_ACTUAL_PROPS));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (have_row && !svn_sqlite__column_is_null(stmt, 0))
    {
      err = svn_sqlite__column_properties(&rpb->props, stmt, 0,
                                          rpb->result_pool, scratch_pool);
    }
  else
    have_row = FALSE;

  SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));

  if (have_row)
    return SVN_NO_ERROR;

  /* No local changes. Return the pristine props for this node.  */
  SVN_ERR(db_read_pristine_props(&rpb->props, wcroot, local_relpath,
                                 rpb->result_pool, scratch_pool));
  if (rpb->props == NULL)
    {
      /* Pristine properties are not defined for this node.
         ### we need to determine whether this node is in a state that
         ### allows for ACTUAL properties (ie. not deleted). for now,
         ### just say all nodes, no matter the state, have at least an
         ### empty set of props.  */
      rpb->props = apr_hash_make(rpb->result_pool);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_read_props(apr_hash_t **props,
                      svn_wc__db_t *db,
                      const char *local_abspath,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct db_read_props_baton_t rpb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  rpb.result_pool = result_pool;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, db_read_props, &rpb,
                              scratch_pool));

  *props = rpb.props;

  return SVN_NO_ERROR;
}



/* Call RECEIVER_FUNC, passing RECEIVER_BATON, an absolute path, and
   a hash table mapping <tt>char *</tt> names onto svn_string_t *
   values for any properties of immediate or recursive child nodes of
   LOCAL_ABSPATH, the actual query being determined by STMT_IDX.
   If FILES_ONLY is true, only report properties for file child nodes.
   Check for cancellation between calls of RECEIVER_FUNC.
*/
typedef struct cache_props_baton_t
{
  svn_boolean_t immediates_only;
  svn_boolean_t pristine;
  svn_cancel_func_t cancel_func;
  void *cancel_baton;
} cache_props_baton_t;


static svn_error_t *
cache_props_recursive(void *cb_baton,
                      svn_wc__db_wcroot_t *wcroot,
                      const char *local_relpath,
                      apr_pool_t *scratch_pool)
{
  cache_props_baton_t *baton = cb_baton;
  svn_sqlite__stmt_t *stmt;

  if (baton->immediates_only)
    {
      if (baton->pristine)
        SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_CACHE_NODE_BASE_PROPS_OF_CHILDREN));
      else
        SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                          STMT_CACHE_NODE_PROPS_OF_CHILDREN));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
    }
  else
    {
      if (baton->pristine)
        SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_CACHE_NODE_BASE_PROPS_RECURSIVE));
      else
        SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                          STMT_CACHE_NODE_PROPS_RECURSIVE));
      SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                                construct_like_arg(local_relpath,
                                                   scratch_pool)));
    }
  SVN_ERR(svn_sqlite__step_done(stmt));

  /* ACTUAL props aren't relevant in the pristine case. */
  if (baton->pristine)
    return SVN_NO_ERROR;

  if (baton->cancel_func)
    SVN_ERR(baton->cancel_func(baton->cancel_baton));
 
  if (baton->immediates_only)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_CACHE_ACTUAL_PROPS_OF_CHILDREN));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
    }
  else
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_CACHE_ACTUAL_PROPS_RECURSIVE));
      SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                                construct_like_arg(local_relpath,
                                                   scratch_pool)));
    }
  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_read_props_streamily(svn_wc__db_t *db,
                                const char *local_abspath,
                                const char *propname,
                                svn_depth_t depth,
                                svn_boolean_t pristine,
                                svn_wc__proplist_receiver_t receiver_func,
                                void *receiver_baton,
                                svn_cancel_func_t cancel_func,
                                void *cancel_baton,
                                apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;
  cache_props_baton_t baton;
  svn_boolean_t have_row;
  int row_number;
  apr_pool_t *iterpool;
  svn_boolean_t files_only = (depth == svn_depth_files);
  svn_boolean_t immediates_only = ((depth == svn_depth_immediates) ||
                                   (depth == svn_depth_files));

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(receiver_func);
  SVN_ERR_ASSERT((depth == svn_depth_files) ||
                 (depth == svn_depth_immediates) ||
                 (depth == svn_depth_infinity));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                                db, local_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_sqlite__exec_statements(wcroot->sdb,
                                      STMT_CLEAR_NODE_PROPS_CACHE));

  baton.immediates_only = immediates_only;
  baton.pristine = pristine;
  baton.cancel_func = cancel_func;
  baton.cancel_baton = cancel_baton;
  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, cache_props_recursive,
                              &baton, scratch_pool));

  iterpool = svn_pool_create(scratch_pool);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_RELEVANT_PROPS_FROM_CACHE));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  for (row_number = 0; have_row; ++row_number)
    {
      const char *prop_data;
      apr_size_t len;

      if (files_only && row_number > 0)
        {
          svn_wc__db_kind_t child_kind;

          child_kind = svn_sqlite__column_token(stmt, 1, kind_map);
          if (child_kind != svn_wc__db_kind_file &&
              child_kind != svn_wc__db_kind_symlink)
            {
              SVN_ERR(svn_sqlite__step(&have_row, stmt));
              continue;
            }
        }

      svn_pool_clear(iterpool);

      /* see if someone wants to cancel this operation. */
      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));

      prop_data = svn_sqlite__column_blob(stmt, 2, &len, NULL);
      if (prop_data)
        {
          svn_skel_t *prop_skel;

          prop_skel = svn_skel__parse(prop_data, len, iterpool);
          if (svn_skel__list_length(prop_skel) != 0)
            {
              const char *child_relpath;
              const char *child_abspath;
              apr_hash_t *props = NULL;

              child_relpath = svn_sqlite__column_text(stmt, 0, NULL);
              child_abspath = svn_dirent_join(wcroot->abspath,
                                              child_relpath, iterpool);
              if (propname)
                {
                  svn_string_t *propval;

                  SVN_ERR(svn_skel__parse_prop(&propval, prop_skel, propname,
                                               iterpool));
                  if (propval)
                    {
                      props = apr_hash_make(iterpool);
                      apr_hash_set(props, propname, APR_HASH_KEY_STRING,
                                   propval);
                    }

                }
              else
                {
                  SVN_ERR(svn_skel__parse_proplist(&props, prop_skel,
                                                   iterpool));
                }

              if (receiver_func && props && apr_hash_count(props) != 0)
                {
                  SVN_ERR((*receiver_func)(receiver_baton,
                                           child_abspath, props,
                                           iterpool));
                }
            }
        }

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  svn_pool_destroy(iterpool);

  SVN_ERR(svn_sqlite__exec_statements(wcroot->sdb,
                                      STMT_CLEAR_NODE_PROPS_CACHE));
  return SVN_NO_ERROR;
}


static svn_error_t *
db_read_pristine_props(apr_hash_t **props,
                       svn_wc__db_wcroot_t *wcroot,
                       const char *local_relpath,
                       apr_pool_t *result_pool,
                       apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  svn_wc__db_status_t presence;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_SELECT_NODE_PROPS));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));

  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (!have_row)
    {
      return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND,
                               svn_sqlite__reset(stmt),
                               _("The node '%s' was not found."),
                               path_for_error_message(wcroot,
                                                      local_relpath,
                                                      scratch_pool));
    }


  /* Examine the presence: */
  presence = svn_sqlite__column_token(stmt, 1, presence_map);

  /* For "base-deleted", it is obvious the pristine props are located
     in the BASE table. Fall through to fetch them.
     ### BH: Is this really the behavior we want here? */
  if (presence == svn_wc__db_status_base_deleted)
    {
      SVN_ERR(svn_sqlite__step(&have_row, stmt));

      SVN_ERR_ASSERT(have_row);

      presence = svn_sqlite__column_token(stmt, 1, presence_map);
    }

  /* normal or copied: Fetch properties (during update we want
     properties for incomplete as well) */
  if (presence == svn_wc__db_status_normal
      || presence == svn_wc__db_status_incomplete)
    {
      svn_error_t *err;

      err = svn_sqlite__column_properties(props, stmt, 0, result_pool,
                                          scratch_pool);
      SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));

      if (!*props)
        *props = apr_hash_make(result_pool);

      return SVN_NO_ERROR;
    }

  *props = NULL;
  return svn_error_return(svn_sqlite__reset(stmt));
}


svn_error_t *
svn_wc__db_read_pristine_props(apr_hash_t **props,
                               svn_wc__db_t *db,
                               const char *local_abspath,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(db_read_pristine_props(props, wcroot, local_relpath,
                                 result_pool, scratch_pool));
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_read_children_of_working_node(const apr_array_header_t **children,
                                         svn_wc__db_t *db,
                                         const char *local_abspath,
                                         apr_pool_t *result_pool,
                                         apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                                             local_abspath,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  return gather_children2(children, wcroot, local_relpath,
                          result_pool, scratch_pool);
}

/* Baton for check_replace_txn */
struct check_replace_baton
{
  svn_boolean_t *is_replace_root;
  svn_boolean_t *base_replace;
  svn_boolean_t is_replace;
};

/* Helper for svn_wc__db_node_check_replace. Implements
   svn_wc__db_txn_callback_t */
static svn_error_t *
check_replace_txn(void *baton,
                  svn_wc__db_wcroot_t *wcroot,
                  const char *local_relpath,
                  apr_pool_t *scratch_pool)
{
  struct check_replace_baton *crb = baton;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_int64_t replaced_op_depth;
  svn_wc__db_status_t replaced_status;

  /* Our caller initialized the output values in crb to FALSE */

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_NODE_INFO));

  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));

  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (!have_row)
    return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND,
                             svn_sqlite__reset(stmt),
                             _("The node '%s' was not found."),
                             path_for_error_message(wcroot, local_relpath,
                                                    scratch_pool));

  {
    svn_wc__db_status_t status;

    status = svn_sqlite__column_token(stmt, 3, presence_map);

    if (status != svn_wc__db_status_normal)
      return svn_error_return(svn_sqlite__reset(stmt));
  }

  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (!have_row)
    return svn_error_return(svn_sqlite__reset(stmt));

  replaced_status = svn_sqlite__column_token(stmt, 3, presence_map);

  /* If the layer below the add describes a not present or a deleted node,
     this is not a replacement. Deleted can only occur if an ancestor is
     the delete root. */
  if (replaced_status != svn_wc__db_status_not_present
      && replaced_status != svn_wc__db_status_excluded
      && replaced_status != svn_wc__db_status_absent
      && replaced_status != svn_wc__db_status_base_deleted)
    crb->is_replace = TRUE;

  replaced_op_depth = svn_sqlite__column_int64(stmt, 0);

  if (crb->base_replace)
    {
      apr_int64_t op_depth = svn_sqlite__column_int64(stmt, 0);

      while (op_depth != 0 && have_row)
        {
          SVN_ERR(svn_sqlite__step(&have_row, stmt));

          if (have_row)
            op_depth = svn_sqlite__column_int64(stmt, 0);
        }

      if (have_row && op_depth == 0)
        {
          svn_wc__db_status_t base_status;

          base_status = svn_sqlite__column_token(stmt, 3, presence_map);
                                           
          *crb->base_replace = (base_status != svn_wc__db_status_not_present);
        }
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  if (!crb->is_replace_root || !crb->is_replace)
    return SVN_NO_ERROR;

  if (replaced_status != svn_wc__db_status_base_deleted)
    {
      apr_int64_t parent_op_depth;
    
      /* Check the current op-depth of the parent to see if we are a replacement
         root */
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id,
                                svn_relpath_dirname(local_relpath,
                                                    scratch_pool)));
    
      SVN_ERR(svn_sqlite__step_row(stmt)); /* Parent must exist as 'normal' */
    
      parent_op_depth = svn_sqlite__column_int64(stmt, 0);
    
      if (parent_op_depth >= replaced_op_depth)
        {
          /* Did we replace inside our directory? */

          *crb->is_replace_root = (parent_op_depth == replaced_op_depth);
          SVN_ERR(svn_sqlite__reset(stmt));
          return SVN_NO_ERROR;
        }
    
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    
      if (have_row)
        parent_op_depth = svn_sqlite__column_int64(stmt, 0);
    
      SVN_ERR(svn_sqlite__reset(stmt));
    
      if (!have_row)
        *crb->is_replace_root = TRUE; /* Parent is no replacement */
      else if (parent_op_depth < replaced_op_depth)
        *crb->is_replace_root = TRUE; /* Parent replaces a lower layer */
      /*else // No replacement root */
  }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_node_check_replace(svn_boolean_t *is_replace_root,
                              svn_boolean_t *base_replace,
                              svn_boolean_t *is_replace,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct check_replace_baton crb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                                             local_abspath,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  if (is_replace_root)
    *is_replace_root = FALSE;
  if (is_replace)
    *is_replace = FALSE;
  if (base_replace)
    *base_replace = FALSE;

  if (local_relpath[0] == '\0')
    return SVN_NO_ERROR; /* Working copy root can't be replaced */

  crb.is_replace_root = is_replace_root;
  crb.base_replace = base_replace;
  crb.is_replace = FALSE;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, check_replace_txn, &crb,
                              scratch_pool));

  if (is_replace)
    *is_replace = crb.is_replace;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_read_children(const apr_array_header_t **children,
                         svn_wc__db_t *db,
                         const char *local_abspath,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                                             local_abspath,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  return gather_children(children, wcroot, local_relpath,
                         result_pool, scratch_pool);
}


struct relocate_baton_t
{
  const char *repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_boolean_t have_base_node;
  apr_int64_t old_repos_id;
};


/* */
static svn_error_t *
relocate_txn(void *baton,
             svn_wc__db_wcroot_t *wcroot,
             const char *local_relpath,
             apr_pool_t *scratch_pool)
{
  struct relocate_baton_t *rb = baton;
  const char *like_arg;
  svn_sqlite__stmt_t *stmt;
  apr_int64_t new_repos_id;

  /* This function affects all the children of the given local_relpath,
     but the way that it does this is through the repos inheritance mechanism.
     So, we only need to rewrite the repos_id of the given local_relpath,
     as well as any children with a non-null repos_id, as well as various
     repos_id fields in the locks and working_node tables.
   */

  /* Get the repos_id for the new repository. */
  SVN_ERR(create_repos_id(&new_repos_id, rb->repos_root_url, rb->repos_uuid,
                          wcroot->sdb, scratch_pool));

  like_arg = construct_like_arg(local_relpath, scratch_pool);

  /* Set the (base and working) repos_ids and clear the dav_caches */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_RECURSIVE_UPDATE_NODE_REPO));
  SVN_ERR(svn_sqlite__bindf(stmt, "issii", wcroot->wc_id, local_relpath,
                            like_arg, rb->old_repos_id, new_repos_id));
  SVN_ERR(svn_sqlite__step_done(stmt));

  if (rb->have_base_node)
    {
      /* Update any locks for the root or its children. */
      like_arg = construct_like_arg(rb->repos_relpath, scratch_pool);

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_UPDATE_LOCK_REPOS_ID));
      SVN_ERR(svn_sqlite__bindf(stmt, "issi", rb->old_repos_id,
                                rb->repos_relpath, like_arg, new_repos_id));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_global_relocate(svn_wc__db_t *db,
                           const char *local_dir_abspath,
                           const char *repos_root_url,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  const char *local_dir_relpath;
  svn_wc__db_status_t status;
  struct relocate_baton_t rb;
  const char *stored_local_dir_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_dir_abspath));
  /* ### assert that we were passed a directory?  */

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_dir_relpath,
                           db, local_dir_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);
  local_relpath = local_dir_relpath;

  SVN_ERR(read_info(&status,
                    NULL, NULL,
                    &rb.repos_relpath, &rb.old_repos_id,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL,
                    &rb.have_base_node, NULL, NULL,
                    wcroot, local_relpath,
                    scratch_pool, scratch_pool));

  if (status == svn_wc__db_status_excluded)
    {
      /* The parent cannot be excluded, so look at the parent and then
         adjust the relpath */
      const char *parent_relpath = svn_relpath_dirname(local_dir_relpath,
                                                       scratch_pool);
      SVN_ERR(read_info(&status,
                        NULL, NULL,
                        &rb.repos_relpath, &rb.old_repos_id,
                        NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                        NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                        NULL, NULL, NULL,
                        NULL, NULL, NULL,
                        wcroot, parent_relpath,
                        scratch_pool, scratch_pool));
      stored_local_dir_relpath = local_relpath;
      local_dir_relpath = parent_relpath;
    }
  else
    stored_local_dir_relpath = NULL;

  if (!rb.repos_relpath || rb.old_repos_id == INVALID_REPOS_ID)
    {
      /* Do we need to support relocating something that is
         added/deleted/excluded without relocating the parent?  If not
         then perhaps relpath, root_url and uuid should be passed down
         to the children so that they don't have to scan? */

      if (status == svn_wc__db_status_deleted)
        {
          const char *work_del_relpath;
          SVN_ERR(scan_deletion(NULL, NULL, &work_del_relpath,
                                wcroot, local_dir_relpath,
                                scratch_pool, scratch_pool));
          if (work_del_relpath)
            {
              /* Deleted within a copy/move */
              SVN_ERR_ASSERT(!stored_local_dir_relpath);
              stored_local_dir_relpath = local_relpath;

              /* The parent of the delete is added. */
              status = svn_wc__db_status_added;
              local_dir_relpath = svn_relpath_dirname(work_del_relpath,
                                                      scratch_pool);
            }
        }

      if (status == svn_wc__db_status_added)
        {
          SVN_ERR(scan_addition(NULL, NULL,
                                &rb.repos_relpath, &rb.old_repos_id,
                                NULL, NULL, NULL,
                                wcroot, local_dir_relpath,
                                scratch_pool, scratch_pool));
        }
      else
        SVN_ERR(scan_upwards_for_repos(&rb.old_repos_id, &rb.repos_relpath,
                                       wcroot, local_dir_relpath,
                                       scratch_pool, scratch_pool));
    }

  SVN_ERR(fetch_repos_info(NULL, &rb.repos_uuid,
                           wcroot->sdb, rb.old_repos_id, scratch_pool));
  SVN_ERR_ASSERT(rb.repos_relpath && rb.repos_uuid);

  if (stored_local_dir_relpath)
    {
      const char *part = svn_relpath_is_child(local_dir_relpath,
                                              stored_local_dir_relpath,
                                              scratch_pool);
      rb.repos_relpath = svn_relpath_join(rb.repos_relpath, part,
                                          scratch_pool);
    }

  rb.repos_root_url = repos_root_url;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, relocate_txn, &rb,
                              scratch_pool));

  return SVN_NO_ERROR;
}


/* Set *REPOS_ID and *REPOS_RELPATH to the BASE repository location of
   (WCROOT, LOCAL_RELPATH), directly if its BASE row exists or implied from
   its parent's BASE row if not. In the latter case, error if the parent
   BASE row does not exist.  */
static svn_error_t *
determine_repos_info(apr_int64_t *repos_id,
                     const char **repos_relpath,
                     svn_wc__db_wcroot_t *wcroot,
                     const char *local_relpath,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  const char *repos_parent_relpath;
  const char *local_parent_relpath, *name;

  /* ### is it faster to fetch fewer columns? */

  /* Prefer the current node's repository information.  */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_BASE_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (have_row)
    {
      SVN_ERR_ASSERT(!svn_sqlite__column_is_null(stmt, 0));
      SVN_ERR_ASSERT(!svn_sqlite__column_is_null(stmt, 1));

      *repos_id = svn_sqlite__column_int64(stmt, 0);
      *repos_relpath = svn_sqlite__column_text(stmt, 1, result_pool);

      return svn_error_return(svn_sqlite__reset(stmt));
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  /* This was a child node within this wcroot. We want to look at the
     BASE node of the directory.  */
  svn_relpath_split(&local_parent_relpath, &name, local_relpath, scratch_pool);

  /* The REPOS_ID will be the same (### until we support mixed-repos)  */
  SVN_ERR(scan_upwards_for_repos(repos_id, &repos_parent_relpath,
                                 wcroot, local_parent_relpath,
                                 scratch_pool, scratch_pool));

  *repos_relpath = svn_relpath_join(repos_parent_relpath, name, result_pool);

  return SVN_NO_ERROR;
}


struct commit_baton_t {
  svn_revnum_t new_revision;
  svn_revnum_t changed_rev;
  apr_time_t changed_date;
  const char *changed_author;
  const svn_checksum_t *new_checksum;
  const apr_array_header_t *new_children;
  apr_hash_t *new_dav_cache;
  svn_boolean_t keep_changelist;
  svn_boolean_t no_unlock;

  const svn_skel_t *work_items;
};


/* */
static svn_error_t *
commit_node(void *baton,
            svn_wc__db_wcroot_t *wcroot,
            const char *local_relpath,
            apr_pool_t *scratch_pool)
{
  struct commit_baton_t *cb = baton;
  svn_sqlite__stmt_t *stmt_info;
  svn_sqlite__stmt_t *stmt_act;
  svn_boolean_t have_act;
  svn_string_t prop_blob = { 0 };
  const char *changelist = NULL;
  const char *parent_relpath;
  svn_wc__db_status_t new_presence;
  svn_wc__db_kind_t new_kind;
  const char *new_depth_str = NULL;
  svn_sqlite__stmt_t *stmt;
  apr_int64_t repos_id;
  const char *repos_relpath;
  apr_int64_t op_depth;

    /* If we are adding a file or directory, then we need to get
     repository information from the parent node since "this node" does
     not have a BASE).

     For existing nodes, we should retain the (potentially-switched)
     repository information.  */
  SVN_ERR(determine_repos_info(&repos_id, &repos_relpath,
                               wcroot, local_relpath,
                               scratch_pool, scratch_pool));

  /* ### is it better to select only the data needed?  */
  SVN_ERR(svn_sqlite__get_statement(&stmt_info, wcroot->sdb,
                                    STMT_SELECT_NODE_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt_info, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step_row(stmt_info));

  SVN_ERR(svn_sqlite__get_statement(&stmt_act, wcroot->sdb,
                                    STMT_SELECT_ACTUAL_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt_act, "is",
                            wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_act, stmt_act));

  /* There should be something to commit!  */

  op_depth = svn_sqlite__column_int64(stmt_info, 0);

  /* Figure out the new node's kind. It will be whatever is in WORKING_NODE,
     or there will be a BASE_NODE that has it.  */
  new_kind = svn_sqlite__column_token(stmt_info, 4, kind_map);

  /* What will the new depth be?  */
  if (new_kind == svn_wc__db_kind_dir)
    new_depth_str = svn_sqlite__column_text(stmt_info, 11, scratch_pool);

  /* Check that the repository information is not being changed.  */
  if (op_depth == 0)
    {
      SVN_ERR_ASSERT(!svn_sqlite__column_is_null(stmt_info, 1));
      SVN_ERR_ASSERT(!svn_sqlite__column_is_null(stmt_info, 2));

      /* A commit cannot change these values.  */
      SVN_ERR_ASSERT(repos_id == svn_sqlite__column_int64(stmt_info, 1));
      SVN_ERR_ASSERT(strcmp(repos_relpath,
                            svn_sqlite__column_text(stmt_info, 2, NULL)) == 0);
    }

  /* Find the appropriate new properties -- ACTUAL overrides any properties
     in WORKING that arrived as part of a copy/move.

     Note: we'll keep them as a big blob of data, rather than
     deserialize/serialize them.  */
  if (have_act)
    prop_blob.data = svn_sqlite__column_blob(stmt_act, 6, &prop_blob.len,
                                             scratch_pool);
  if (prop_blob.data == NULL)
    prop_blob.data = svn_sqlite__column_blob(stmt_info, 14, &prop_blob.len,
                                             scratch_pool);

  if (cb->keep_changelist && have_act)
    changelist = svn_sqlite__column_text(stmt_act, 1, scratch_pool);

  /* ### other stuff?  */

  SVN_ERR(svn_sqlite__reset(stmt_info));
  SVN_ERR(svn_sqlite__reset(stmt_act));

  if (op_depth > 0)
    {
      int affected_rows;

      /* This removes all layers of this node and at the same time determines
         if we need to remove shadowed layers below our descendants. */

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_DELETE_ALL_LAYERS));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__update(&affected_rows, stmt));

      if (affected_rows > 1)
        {
          /* We commit a shadowing operation

           1) Remove all shadowed nodes
           2) And remove all nodes that have a base-deleted as lowest layer,
              because 1) removed that layer

           Possible followup:
             3) ### Collapse descendants of the current op_depth in layer 0,
                    to commit a remote copy in one step (but don't touch/use
                    ACTUAL!!)
          */

          SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                            STMT_DELETE_SHADOWED_RECURSIVE));

          SVN_ERR(svn_sqlite__bindf(stmt,
                                    "isi",
                                    wcroot->wc_id,
                                    local_relpath,
                                    op_depth));

          SVN_ERR(svn_sqlite__step_done(stmt));
        }
    }

  /* Update or add the BASE_NODE row with all the new information.  */

  if (*local_relpath == '\0')
    parent_relpath = NULL;
  else
    parent_relpath = svn_relpath_dirname(local_relpath, scratch_pool);

  /* ### other presences? or reserve that for separate functions?  */
  new_presence = svn_wc__db_status_normal;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_APPLY_CHANGES_TO_BASE_NODE));
  /* symlink_target not yet used */
  SVN_ERR(svn_sqlite__bindf(stmt, "issisrtstrisnbn",
                            wcroot->wc_id, local_relpath,
                            parent_relpath,
                            repos_id,
                            repos_relpath,
                            cb->new_revision,
                            presence_map, new_presence,
                            new_depth_str,
                            kind_map, new_kind,
                            cb->changed_rev,
                            cb->changed_date,
                            cb->changed_author,
                            prop_blob.data, prop_blob.len));

  SVN_ERR(svn_sqlite__bind_checksum(stmt, 13, cb->new_checksum,
                                    scratch_pool));
  SVN_ERR(svn_sqlite__bind_properties(stmt, 15, cb->new_dav_cache,
                                      scratch_pool));

  SVN_ERR(svn_sqlite__step_done(stmt));

  if (have_act)
    {
      if (cb->keep_changelist && changelist != NULL)
        {
          /* The user told us to keep the changelist. Replace the row in
             ACTUAL_NODE with the basic keys and the changelist.  */
          SVN_ERR(svn_sqlite__get_statement(
                    &stmt, wcroot->sdb,
                    STMT_RESET_ACTUAL_WITH_CHANGELIST));
          SVN_ERR(svn_sqlite__bindf(stmt, "isss",
                                    wcroot->wc_id, local_relpath,
                                    svn_relpath_dirname(local_relpath,
                                                        scratch_pool),
                                    changelist));
          SVN_ERR(svn_sqlite__step_done(stmt));
        }
      else
        {
          /* Toss the ACTUAL_NODE row.  */
          SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                            STMT_DELETE_ACTUAL_NODE));
          SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
          SVN_ERR(svn_sqlite__step_done(stmt));
        }
    }

  if (new_kind == svn_wc__db_kind_dir)
    {
      /* When committing a directory, we should have its new children.  */
      /* ### one day. just not today.  */
#if 0
      SVN_ERR_ASSERT(cb->new_children != NULL);
#endif

      /* ### process the children  */
    }

  if (!cb->no_unlock)
    {
      svn_sqlite__stmt_t *lock_stmt;

      SVN_ERR(svn_sqlite__get_statement(&lock_stmt, wcroot->sdb,
                                        STMT_DELETE_LOCK));
      SVN_ERR(svn_sqlite__bindf(lock_stmt, "is", repos_id, repos_relpath));
      SVN_ERR(svn_sqlite__step_done(lock_stmt));
    }

  /* Install any work items into the queue, as part of this transaction.  */
  SVN_ERR(add_work_items(wcroot->sdb, cb->work_items, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_global_commit(svn_wc__db_t *db,
                         const char *local_abspath,
                         svn_revnum_t new_revision,
                         svn_revnum_t changed_revision,
                         apr_time_t changed_date,
                         const char *changed_author,
                         const svn_checksum_t *new_checksum,
                         const apr_array_header_t *new_children,
                         apr_hash_t *new_dav_cache,
                         svn_boolean_t keep_changelist,
                         svn_boolean_t no_unlock,
                         const svn_skel_t *work_items,
                         apr_pool_t *scratch_pool)
{
  const char *local_relpath;
  svn_wc__db_wcroot_t *wcroot;
  struct commit_baton_t cb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(new_revision));
  SVN_ERR_ASSERT(new_checksum == NULL || new_children == NULL);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  cb.new_revision = new_revision;

  cb.changed_rev = changed_revision;
  cb.changed_date = changed_date;
  cb.changed_author = changed_author;
  cb.new_checksum = new_checksum;
  cb.new_children = new_children;
  cb.new_dav_cache = new_dav_cache;
  cb.keep_changelist = keep_changelist;
  cb.no_unlock = no_unlock;
  cb.work_items = work_items;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, commit_node, &cb,
                              scratch_pool));

  /* We *totally* monkeyed the entries. Toss 'em.  */
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


struct update_baton_t {
  const char *new_repos_relpath;
  svn_revnum_t new_revision;
  const apr_hash_t *new_props;
  svn_revnum_t new_changed_rev;
  apr_time_t new_changed_date;
  const char *new_changed_author;
  const apr_array_header_t *new_children;
  const svn_checksum_t *new_checksum;
  const char *new_target;
  const svn_skel_t *conflict;
  const svn_skel_t *work_items;
};


svn_error_t *
svn_wc__db_global_update(svn_wc__db_t *db,
                         const char *local_abspath,
                         svn_wc__db_kind_t new_kind,
                         const char *new_repos_relpath,
                         svn_revnum_t new_revision,
                         const apr_hash_t *new_props,
                         svn_revnum_t new_changed_rev,
                         apr_time_t new_changed_date,
                         const char *new_changed_author,
                         const apr_array_header_t *new_children,
                         const svn_checksum_t *new_checksum,
                         const char *new_target,
                         const apr_hash_t *new_dav_cache,
                         const svn_skel_t *conflict,
                         const svn_skel_t *work_items,
                         apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct update_baton_t ub;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  /* ### allow NULL for NEW_REPOS_RELPATH to indicate "no change"?  */
  SVN_ERR_ASSERT(svn_relpath_is_canonical(new_repos_relpath, scratch_pool));
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(new_revision));
  SVN_ERR_ASSERT(new_props != NULL);
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(new_changed_rev));
  SVN_ERR_ASSERT((new_children != NULL
                  && new_checksum == NULL
                  && new_target == NULL)
                 || (new_children == NULL
                     && new_checksum != NULL
                     && new_target == NULL)
                 || (new_children == NULL
                     && new_checksum == NULL
                     && new_target != NULL));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  ub.new_repos_relpath = new_repos_relpath;
  ub.new_revision = new_revision;
  ub.new_props = new_props;
  ub.new_changed_rev = new_changed_rev;
  ub.new_changed_date = new_changed_date;
  ub.new_changed_author = new_changed_author;
  ub.new_children = new_children;
  ub.new_checksum = new_checksum;
  ub.new_target = new_target;

  ub.conflict = conflict;
  ub.work_items = work_items;

  NOT_IMPLEMENTED();

#if 0
  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, update_node, &ub,
                              scratch_pool));

  /* We *totally* monkeyed the entries. Toss 'em.  */
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
#endif
}

/* Sets a base nodes revision and/or repository relative path. If
   LOCAL_ABSPATH's rev (REV) is valid, set is revision and if SET_REPOS_RELPATH
   is TRUE set its repository relative path to REPOS_RELPATH (and make sure its
   REPOS_ID is still valid).
 */
static svn_error_t *
db_op_set_rev_and_repos_relpath(svn_wc__db_wcroot_t *wcroot,
                                const char *local_relpath,
                                svn_revnum_t rev,
                                svn_boolean_t set_repos_relpath,
                                const char *repos_relpath,
                                apr_int64_t repos_id,
                                apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(flush_entries(wcroot,
                        svn_dirent_join(wcroot->abspath, local_relpath,
                                        scratch_pool),
                        scratch_pool));


  if (SVN_IS_VALID_REVNUM(rev))
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_UPDATE_BASE_REVISION));

      SVN_ERR(svn_sqlite__bindf(stmt, "isr", wcroot->wc_id, local_relpath,
                                rev));

      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  if (set_repos_relpath)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_UPDATE_BASE_REPOS));

      SVN_ERR(svn_sqlite__bindf(stmt, "isis", wcroot->wc_id, local_relpath,
                                repos_id, repos_relpath));

      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  return SVN_NO_ERROR;
}

/* The main body of bump_revisions_post_update.
 *
 * Tweak the information for LOCAL_RELPATH in WCROOT.  If NEW_REPOS_RELPATH is
 * non-NULL update the entry to the new url specified by NEW_REPOS_RELPATH,
 * NEW_REPOS_ID..  If NEW_REV is valid, make this the node's working revision.
 *
 * Unless S_ROOT is TRUE the tweaks might cause the node for LOCAL_ABSPATH to
 * be removed from the WC; if IS_ROOT is TRUE this will not happen.
 */
static svn_error_t *
bump_node_revision(svn_wc__db_wcroot_t *wcroot,
                   const char *local_relpath,
                   apr_int64_t new_repos_id,
                   const char *new_repos_relpath,
                   svn_revnum_t new_rev,
                   svn_depth_t depth,
                   apr_hash_t *exclude_relpaths,
                   svn_boolean_t is_root,
                   svn_boolean_t skip_when_dir,
                   apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool;
  const apr_array_header_t *children;
  int i;
  svn_wc__db_status_t status;
  svn_wc__db_kind_t db_kind;
  svn_revnum_t revision;
  const char *repos_relpath;
  apr_int64_t repos_id;
  svn_boolean_t set_repos_relpath = FALSE;
  svn_boolean_t update_root;
  svn_depth_t depth_below_here = depth;

  /* Skip an excluded path and its descendants. */
  if (apr_hash_get(exclude_relpaths, local_relpath, APR_HASH_KEY_STRING))
    return SVN_NO_ERROR;

  SVN_ERR(base_get_info(&status, &db_kind, &revision, &repos_relpath,
                        &repos_id, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                        NULL, NULL, NULL, &update_root, NULL,
                        wcroot, local_relpath,
                        scratch_pool, scratch_pool));

  /* Skip file externals */
  if (update_root
      && db_kind == svn_wc__db_kind_file
      && !is_root)
    return SVN_NO_ERROR;

  if (skip_when_dir && db_kind == svn_wc__db_kind_dir)
    return SVN_NO_ERROR;

  /* If the node is still marked 'not-present', then the server did not
     re-add it.  So it's really gone in this revision, thus we remove the node.

     If the node is still marked 'absent' and yet is not the same
     revision as new_rev, then the server did not re-add it, nor
     re-absent it, so we can remove the node. */
  if (!is_root
      && (status == svn_wc__db_status_not_present
          || (status == svn_wc__db_status_absent && revision != new_rev)))
    {
      return svn_error_return(db_base_remove(NULL, wcroot, local_relpath,
                                             scratch_pool));
    }

  if (new_repos_relpath != NULL)
    {
      if (!repos_relpath)
        SVN_ERR(scan_upwards_for_repos(&repos_id, &repos_relpath,
                                       wcroot, local_relpath,
                                       scratch_pool, scratch_pool));

      if (strcmp(repos_relpath, new_repos_relpath))
          set_repos_relpath = TRUE;
    }

  if (set_repos_relpath
      || (SVN_IS_VALID_REVNUM(new_rev) && new_rev != revision))
    SVN_ERR(db_op_set_rev_and_repos_relpath(wcroot, local_relpath,
                                            new_rev,
                                            set_repos_relpath,
                                            new_repos_relpath,
                                            new_repos_id,
                                            scratch_pool));

  /* Early out */
  if (depth <= svn_depth_empty
      || db_kind != svn_wc__db_kind_dir
      || status == svn_wc__db_status_absent
      || status == svn_wc__db_status_excluded
      || status == svn_wc__db_status_not_present)
    return SVN_NO_ERROR;

  /* And now recurse over the children */

  depth_below_here = depth;

  if (depth == svn_depth_immediates || depth == svn_depth_files)
    depth_below_here = svn_depth_empty;

  iterpool = svn_pool_create(scratch_pool);

  SVN_ERR(gather_repo_children(&children, wcroot, local_relpath, 0,
                               scratch_pool, iterpool));
  for (i = 0; i < children->nelts; i++)
    {
      const char *child_basename = APR_ARRAY_IDX(children, i, const char *);
      const char *child_local_relpath;
      const char *child_repos_relpath = NULL;

      svn_pool_clear(iterpool);

      /* Derive the new URL for the current (child) entry */
      if (new_repos_relpath)
        child_repos_relpath = svn_relpath_join(new_repos_relpath,
                                               child_basename, iterpool);

      child_local_relpath = svn_relpath_join(local_relpath, child_basename,
                                             iterpool);

      SVN_ERR(bump_node_revision(wcroot, child_local_relpath, new_repos_id,
                                 child_repos_relpath, new_rev,
                                 depth_below_here,
                                 exclude_relpaths, FALSE /* is_root */,
                                 (depth < svn_depth_immediates),
                                 iterpool));
    }

  /* Cleanup */
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

struct bump_revisions_baton_t
{
  svn_depth_t depth;
  const char *new_repos_relpath;
  const char *new_repos_root_url;
  const char *new_repos_uuid;
  svn_revnum_t new_revision;
  apr_hash_t *exclude_relpaths;
};

static svn_error_t *
bump_revisions_post_commit(void *baton,
                           svn_wc__db_wcroot_t *wcroot,
                           const char *local_relpath,
                           apr_pool_t *scratch_pool)
{
  struct bump_revisions_baton_t *brb = baton;
  svn_wc__db_status_t status;
  svn_wc__db_kind_t kind;
  svn_error_t *err;
  apr_int64_t new_repos_id = -1;

  err = base_get_info(&status, &kind, NULL, NULL, NULL, NULL, NULL, NULL,
                      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                      wcroot, local_relpath, scratch_pool, scratch_pool);
  if (err && err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  else
    SVN_ERR(err);

  switch (status)
    {
      case svn_wc__db_status_excluded:
      case svn_wc__db_status_absent:
      case svn_wc__db_status_not_present:
        return SVN_NO_ERROR;

      /* Explicitly ignore other statii */
      default:
        break;
    }

  if (brb->new_repos_root_url != NULL)
    SVN_ERR(create_repos_id(&new_repos_id, brb->new_repos_root_url, 
                            brb->new_repos_uuid,
                            wcroot->sdb, scratch_pool));

  SVN_ERR(bump_node_revision(wcroot, local_relpath, new_repos_id,
                             brb->new_repos_relpath, brb->new_revision,
                             brb->depth, brb->exclude_relpaths,
                             TRUE /* is_root */, FALSE, scratch_pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_op_bump_revisions_post_update(svn_wc__db_t *db,
                                         const char *local_abspath,
                                         svn_depth_t depth,
                                         const char *new_repos_relpath,
                                         const char *new_repos_root_url,
                                         const char *new_repos_uuid,
                                         svn_revnum_t new_revision,
                                         apr_hash_t *exclude_relpaths,
                                         apr_pool_t *scratch_pool)
{
  const char *local_relpath;
  svn_wc__db_wcroot_t *wcroot;
  struct bump_revisions_baton_t brb;

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));

  VERIFY_USABLE_WCROOT(wcroot);

  if (apr_hash_get(exclude_relpaths, local_relpath, APR_HASH_KEY_STRING))
    return SVN_NO_ERROR;

  if (depth == svn_depth_unknown)
    depth = svn_depth_infinity;

  brb.depth = depth;
  brb.new_repos_relpath = new_repos_relpath;
  brb.new_repos_root_url = new_repos_root_url;
  brb.new_repos_uuid = new_repos_uuid;
  brb.new_revision = new_revision;
  brb.exclude_relpaths = exclude_relpaths;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath,
                              bump_revisions_post_commit, &brb, scratch_pool));

  return SVN_NO_ERROR;
}


struct record_baton_t {
  svn_filesize_t translated_size;
  apr_time_t last_mod_time;
};


/* Record TRANSLATED_SIZE and LAST_MOD_TIME into top layer in NODES */
static svn_error_t *
record_fileinfo(void *baton,
                svn_wc__db_wcroot_t *wcroot,
                const char *local_relpath,
                apr_pool_t *scratch_pool)
{
  struct record_baton_t *rb = baton;
  svn_sqlite__stmt_t *stmt;
  int affected_rows;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_UPDATE_NODE_FILEINFO));
  SVN_ERR(svn_sqlite__bindf(stmt, "isii", wcroot->wc_id, local_relpath,
                            rb->translated_size, rb->last_mod_time));
  SVN_ERR(svn_sqlite__update(&affected_rows, stmt));

  SVN_ERR_ASSERT(affected_rows == 1);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_global_record_fileinfo(svn_wc__db_t *db,
                                  const char *local_abspath,
                                  svn_filesize_t translated_size,
                                  apr_time_t last_mod_time,
                                  apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct record_baton_t rb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  rb.translated_size = translated_size;
  rb.last_mod_time = last_mod_time;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, record_fileinfo, &rb,
                              scratch_pool));

  /* We *totally* monkeyed the entries. Toss 'em.  */
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
lock_add_txn(void *baton,
             svn_wc__db_wcroot_t *wcroot,
             const char *local_relpath,
             apr_pool_t *scratch_pool)
{
  const svn_wc__db_lock_t *lock = baton;
  svn_sqlite__stmt_t *stmt;
  const char *repos_relpath;
  apr_int64_t repos_id;

  SVN_ERR(scan_upwards_for_repos(&repos_id, &repos_relpath,
                                 wcroot, local_relpath,
                                 scratch_pool, scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_INSERT_LOCK));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss",
                            repos_id, repos_relpath, lock->token));

  if (lock->owner != NULL)
    SVN_ERR(svn_sqlite__bind_text(stmt, 4, lock->owner));

  if (lock->comment != NULL)
    SVN_ERR(svn_sqlite__bind_text(stmt, 5, lock->comment));

  if (lock->date != 0)
    SVN_ERR(svn_sqlite__bind_int64(stmt, 6, lock->date));

  SVN_ERR(svn_sqlite__insert(NULL, stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_lock_add(svn_wc__db_t *db,
                    const char *local_abspath,
                    const svn_wc__db_lock_t *lock,
                    apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(lock != NULL);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, lock_add_txn,
                              (void *) lock, scratch_pool));

  /* There may be some entries, and the lock info is now out of date.  */
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
lock_remove_txn(void *baton,
                svn_wc__db_wcroot_t *wcroot,
                const char *local_relpath,
                apr_pool_t *scratch_pool)
{
  const char *repos_relpath;
  apr_int64_t repos_id;
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(scan_upwards_for_repos(&repos_id, &repos_relpath,
                                 wcroot, local_relpath,
                                 scratch_pool, scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_LOCK));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", repos_id, repos_relpath));

  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_lock_remove(svn_wc__db_t *db,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, lock_remove_txn, NULL,
                              scratch_pool));

  /* There may be some entries, and the lock info is now out of date.  */
  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_scan_base_repos(const char **repos_relpath,
                           const char **repos_root_url,
                           const char **repos_uuid,
                           svn_wc__db_t *db,
                           const char *local_abspath,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  apr_int64_t repos_id;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(scan_upwards_for_repos(&repos_id, repos_relpath, wcroot,
                                 local_relpath, result_pool, scratch_pool));
  SVN_ERR(fetch_repos_info(repos_root_url, repos_uuid, wcroot->sdb,
                           repos_id, result_pool));

  return SVN_NO_ERROR;
}


struct scan_addition_baton_t
{
  svn_wc__db_status_t *status;
  const char **op_root_relpath;
  const char **repos_relpath;
  apr_int64_t *repos_id;
  const char **original_repos_relpath;
  apr_int64_t *original_repos_id;
  svn_revnum_t *original_revision;
  apr_pool_t *result_pool;
};

static svn_error_t *
scan_addition_txn(void *baton,
                  svn_wc__db_wcroot_t *wcroot,
                  const char *local_relpath,
                  apr_pool_t *scratch_pool)
{
  struct scan_addition_baton_t *sab = baton;
  const char *current_relpath = local_relpath;
  const char *build_relpath = "";

  /* Initialize most of the OUT parameters. Generally, we'll only be filling
     in a subset of these, so it is easier to init all up front. Note that
     the STATUS parameter will be initialized once we read the status of
     the specified node.  */
  if (sab->op_root_relpath)
    *sab->op_root_relpath = NULL;
  if (sab->original_repos_relpath)
    *sab->original_repos_relpath = NULL;
  if (sab->original_repos_id)
    *sab->original_repos_id = INVALID_REPOS_ID;
  if (sab->original_revision)
    *sab->original_revision = SVN_INVALID_REVNUM;

  {
    svn_sqlite__stmt_t *stmt;
    svn_boolean_t have_row;
    svn_wc__db_status_t presence;
    apr_int64_t op_depth;
    const char *repos_prefix_path = "";
    int i;

    /* ### is it faster to fetch fewer columns? */
    SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                      STMT_SELECT_WORKING_NODE));
    SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
    SVN_ERR(svn_sqlite__step(&have_row, stmt));

    if (!have_row)
      {
        /* Reset statement before returning */
        SVN_ERR(svn_sqlite__reset(stmt));

        /* ### maybe we should return a usage error instead?  */
        return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                                 _("The node '%s' was not found."),
                                 path_for_error_message(wcroot,
                                                        local_relpath,
                                                        scratch_pool));
      }

    presence = svn_sqlite__column_token(stmt, 1, presence_map);

    /* The starting node should exist normally.  */
    if (presence != svn_wc__db_status_normal)
      /* reset the statement as part of the error generation process */
      return svn_error_createf(SVN_ERR_WC_PATH_UNEXPECTED_STATUS,
                               svn_sqlite__reset(stmt),
                               _("Expected node '%s' to be added."),
                               path_for_error_message(wcroot,
                                                      local_relpath,
                                                      scratch_pool));

    if (sab->original_revision)
      *sab->original_revision = svn_sqlite__column_revnum(stmt, 12);

    /* Provide the default status; we'll override as appropriate. */
    if (sab->status)
      *sab->status = svn_wc__db_status_added;


    /* Calculate the op root local path components */
    op_depth = svn_sqlite__column_int64(stmt, 0);
    current_relpath = local_relpath;

    for (i = relpath_depth(local_relpath); i > op_depth; --i)
      {
        /* Calculate the path of the operation root */
        repos_prefix_path =
          svn_relpath_join(svn_dirent_basename(current_relpath, NULL),
                           repos_prefix_path,
                           scratch_pool);
        current_relpath = svn_relpath_dirname(current_relpath, scratch_pool);
      }

    if (sab->op_root_relpath)
      *sab->op_root_relpath = apr_pstrdup(sab->result_pool, current_relpath);

    if (sab->original_repos_relpath
        || sab->original_repos_id
        || (sab->original_revision
                && *sab->original_revision == SVN_INVALID_REVNUM)
        || sab->status)
      {
        if (local_relpath != current_relpath)
          /* requery to get the add/copy root */
          {
            SVN_ERR(svn_sqlite__reset(stmt));

            SVN_ERR(svn_sqlite__bindf(stmt, "is",
                                      wcroot->wc_id, current_relpath));
            SVN_ERR(svn_sqlite__step(&have_row, stmt));

            if (!have_row)
              {
                /* Reset statement before returning */
                SVN_ERR(svn_sqlite__reset(stmt));

                /* ### maybe we should return a usage error instead?  */
                return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                                         _("The node '%s' was not found."),
                                         path_for_error_message(wcroot,
                                                                current_relpath,
                                                                scratch_pool));
              }

            if (sab->original_revision
                    && *sab->original_revision == SVN_INVALID_REVNUM)
              *sab->original_revision = svn_sqlite__column_revnum(stmt, 12);
          }

        /* current_relpath / current_abspath
           as well as the record in stmt contain the data of the op_root */
        if (sab->original_repos_relpath)
          *sab->original_repos_relpath = svn_sqlite__column_text(stmt, 11,
                                                            sab->result_pool);

        if (!svn_sqlite__column_is_null(stmt, 10)
            && (sab->status
                || sab->original_repos_id))
          /* If column 10 (original_repos_id) is NULL,
             this is a plain add, not a copy or a move */
          {
            if (sab->original_repos_id)
              *sab->original_repos_id = svn_sqlite__column_int64(stmt, 10);

            if (sab->status)
              {
                if (svn_sqlite__column_boolean(stmt, 13 /* moved_here */))
                  *sab->status = svn_wc__db_status_moved_here;
                else
                  *sab->status = svn_wc__db_status_copied;
              }
          }
      }


    /* ### This loop here is to skip up to the first node which is a BASE node,
       because scan_upwards_for_repos() doesn't accomodate the scenario that
       we're looking at here; we found the true op_root, which may be inside
       further changed trees. */
    while (TRUE)
      {

        SVN_ERR(svn_sqlite__reset(stmt));

        /* Pointing at op_depth, look at the parent */
        repos_prefix_path =
          svn_relpath_join(svn_dirent_basename(current_relpath, NULL),
                           repos_prefix_path,
                           scratch_pool);
        current_relpath = svn_relpath_dirname(current_relpath, scratch_pool);


        SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, current_relpath));
        SVN_ERR(svn_sqlite__step(&have_row, stmt));

        if (! have_row)
          break;

        op_depth = svn_sqlite__column_int64(stmt, 0);

        /* Skip to op_depth */
        for (i = relpath_depth(current_relpath); i > op_depth; i--)
          {
            /* Calculate the path of the operation root */
            repos_prefix_path =
              svn_relpath_join(svn_dirent_basename(current_relpath, NULL),
                               repos_prefix_path,
                               scratch_pool);
            current_relpath =
              svn_relpath_dirname(current_relpath, scratch_pool);
          }
      }

    SVN_ERR(svn_sqlite__reset(stmt));

    build_relpath = repos_prefix_path;
  }

  /* If we're here, then we have an added/copied/moved (start) node, and
     CURRENT_ABSPATH now points to a BASE node. Figure out the repository
     information for the current node, and use that to compute the start
     node's repository information.  */
  if (sab->repos_relpath || sab->repos_id)
    {
      const char *base_relpath;

      SVN_ERR(scan_upwards_for_repos(sab->repos_id, &base_relpath,
                                     wcroot, current_relpath,
                                     scratch_pool, scratch_pool));

      if (sab->repos_relpath)
        *sab->repos_relpath = svn_relpath_join(base_relpath, build_relpath,
                                               sab->result_pool);
    }

  /* Postconditions */
#ifdef SVN_DEBUG
  if (sab->status)
    {
      SVN_ERR_ASSERT(*sab->status == svn_wc__db_status_added
                     || *sab->status == svn_wc__db_status_copied
                     || *sab->status == svn_wc__db_status_moved_here);
      if (*sab->status == svn_wc__db_status_added)
        {
          SVN_ERR_ASSERT(!sab->original_repos_relpath
                         || *sab->original_repos_relpath == NULL);
          SVN_ERR_ASSERT(!sab->original_revision
                         || *sab->original_revision == SVN_INVALID_REVNUM);
          SVN_ERR_ASSERT(!sab->original_repos_id
                         || *sab->original_repos_id == INVALID_REPOS_ID);
        }
      else
        {
          SVN_ERR_ASSERT(!sab->original_repos_relpath
                         || *sab->original_repos_relpath != NULL);
          SVN_ERR_ASSERT(!sab->original_revision
                         || *sab->original_revision != SVN_INVALID_REVNUM);
          SVN_ERR_ASSERT(!sab->original_repos_id
                         || *sab->original_repos_id != INVALID_REPOS_ID);
        }
    }
  SVN_ERR_ASSERT(!sab->op_root_relpath || *sab->op_root_relpath != NULL);
#endif

  return SVN_NO_ERROR;
}


/* Like svn_wc__db_scan_addition(), but with WCROOT+LOCAL_RELPATH instead of
   DB+LOCAL_ABSPATH.

   The output value of *ORIGINAL_REPOS_ID will be INVALID_REPOS_ID if there
   is no 'copy-from' repository.  */
static svn_error_t *
scan_addition(svn_wc__db_status_t *status,
              const char **op_root_relpath,
              const char **repos_relpath,
              apr_int64_t *repos_id,
              const char **original_repos_relpath,
              apr_int64_t *original_repos_id,
              svn_revnum_t *original_revision,
              svn_wc__db_wcroot_t *wcroot,
              const char *local_relpath,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool)
{
  struct scan_addition_baton_t sab;

  sab.status = status;
  sab.op_root_relpath = op_root_relpath;
  sab.repos_relpath = repos_relpath;
  sab.repos_id = repos_id;
  sab.original_repos_relpath = original_repos_relpath;
  sab.original_repos_id = original_repos_id;
  sab.original_revision = original_revision;
  sab.result_pool = result_pool;

  return svn_error_return(svn_wc__db_with_txn(wcroot, local_relpath,
                                              scan_addition_txn,
                                              &sab, scratch_pool));
}


svn_error_t *
svn_wc__db_scan_addition(svn_wc__db_status_t *status,
                         const char **op_root_abspath,
                         const char **repos_relpath,
                         const char **repos_root_url,
                         const char **repos_uuid,
                         const char **original_repos_relpath,
                         const char **original_root_url,
                         const char **original_uuid,
                         svn_revnum_t *original_revision,
                         svn_wc__db_t *db,
                         const char *local_abspath,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  const char *op_root_relpath;
  apr_int64_t repos_id = INVALID_REPOS_ID;
  apr_int64_t original_repos_id = INVALID_REPOS_ID;
  apr_int64_t *repos_id_p
    = (repos_root_url || repos_uuid) ? &repos_id : NULL;
  apr_int64_t *original_repos_id_p
    = (original_root_url || original_uuid) ? &original_repos_id : NULL;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(scan_addition(status, &op_root_relpath, repos_relpath, repos_id_p,
                        original_repos_relpath, original_repos_id_p,
                        original_revision, wcroot, local_relpath,
                        result_pool, scratch_pool));

  if (op_root_abspath)
    *op_root_abspath = svn_dirent_join(wcroot->abspath, op_root_relpath,
                                       result_pool);
  /* REPOS_ID must be valid if requested; ORIGINAL_REPOS_ID need not be. */
  SVN_ERR_ASSERT(repos_id_p == NULL || repos_id != INVALID_REPOS_ID);

  SVN_ERR(fetch_repos_info(repos_root_url, repos_uuid, wcroot->sdb,
                           repos_id, result_pool));
  SVN_ERR(fetch_repos_info(original_root_url, original_uuid,
                           wcroot->sdb, original_repos_id,
                           result_pool));

  return SVN_NO_ERROR;
}


struct scan_deletion_baton_t
{
  const char **base_del_relpath;
  const char **moved_to_relpath;
  const char **work_del_relpath;
  apr_pool_t *result_pool;
};


static svn_error_t *
scan_deletion_txn(void *baton,
                  svn_wc__db_wcroot_t *wcroot,
                  const char *local_relpath,
                  apr_pool_t *scratch_pool)
{
  struct scan_deletion_baton_t *sd_baton = baton;
  const char *current_relpath = local_relpath;
  const char *child_relpath = NULL;
  svn_wc__db_status_t child_presence;
  svn_boolean_t child_has_base = FALSE;
  svn_boolean_t found_moved_to = FALSE;
  apr_int64_t local_op_depth, op_depth;

  /* Initialize all the OUT parameters.  */
  if (sd_baton->base_del_relpath != NULL)
    *sd_baton->base_del_relpath = NULL;
  if (sd_baton->moved_to_relpath != NULL)
    *sd_baton->moved_to_relpath = NULL;
  if (sd_baton->work_del_relpath != NULL)
    *sd_baton->work_del_relpath = NULL;

  /* Initialize to something that won't denote an important parent/child
     transition.  */
  child_presence = svn_wc__db_status_base_deleted;

  while (TRUE)
    {
      svn_sqlite__stmt_t *stmt;
      svn_boolean_t have_row;
      svn_boolean_t have_base;
      svn_wc__db_status_t work_presence;

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_SELECT_DELETION_INFO));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, current_relpath));
      SVN_ERR(svn_sqlite__step(&have_row, stmt));

      if (!have_row)
        {
          /* There better be a row for the starting node!  */
          if (current_relpath == local_relpath)
            return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND,
                                     svn_sqlite__reset(stmt),
                                     _("The node '%s' was not found."),
                                     path_for_error_message(wcroot,
                                                            local_relpath,
                                                            scratch_pool));

          /* There are no values, so go ahead and reset the stmt now.  */
          SVN_ERR(svn_sqlite__reset(stmt));

          /* No row means no WORKING node at this path, which means we just
             fell off the top of the WORKING tree.

             If the child was not-present this implies the root of the
             (added) WORKING subtree was deleted.  This can occur
             during post-commit processing when the copied parent that
             was in the WORKING tree has been moved to the BASE tree. */
          if (sd_baton->work_del_relpath != NULL
              && child_presence == svn_wc__db_status_not_present
              && *sd_baton->work_del_relpath == NULL)
            *sd_baton->work_del_relpath = apr_pstrdup(sd_baton->result_pool,
                                                      child_relpath);

          /* If the child did not have a BASE node associated with it, then
             we're looking at a deletion that occurred within an added tree.
             There is no root of a deleted/replaced BASE tree.

             If the child was base-deleted, then the whole tree is a
             simple (explicit) deletion of the BASE tree.

             If the child was normal, then it is the root of a replacement,
             which means an (implicit) deletion of the BASE tree.

             In both cases, set the root of the operation (if we have not
             already set it as part of a moved-away).  */
          if (sd_baton->base_del_relpath != NULL
              && child_has_base
              && *sd_baton->base_del_relpath == NULL)
            *sd_baton->base_del_relpath = apr_pstrdup(sd_baton->result_pool,
                                                      child_relpath);

          /* We found whatever roots we needed. This BASE node and its
             ancestors are unchanged, so we're done.  */
          break;
        }

      /* We need the presence of the WORKING node. Note that legal values
         are: normal, not-present, base-deleted, incomplete.  */
      work_presence = svn_sqlite__column_token(stmt, 1, presence_map);

      /* The starting node should be deleted.  */
      if (current_relpath == local_relpath
          && work_presence != svn_wc__db_status_not_present
          && work_presence != svn_wc__db_status_base_deleted)
        return svn_error_createf(SVN_ERR_WC_PATH_UNEXPECTED_STATUS,
                                 svn_sqlite__reset(stmt),
                                 _("Expected node '%s' to be deleted."),
                                 path_for_error_message(wcroot,
                                                        local_relpath,
                                                        scratch_pool));

      /* ### incomplete not handled */
      SVN_ERR_ASSERT(work_presence == svn_wc__db_status_normal
                     || work_presence == svn_wc__db_status_not_present
                     || work_presence == svn_wc__db_status_base_deleted);

      have_base = !svn_sqlite__column_is_null(stmt,
                                              0 /* BASE_NODE.presence */);
      if (have_base)
        {
          svn_wc__db_status_t base_presence
            = svn_sqlite__column_token(stmt, 0, presence_map);

          /* Only "normal" and "not-present" are allowed.  */
          SVN_ERR_ASSERT(base_presence == svn_wc__db_status_normal
                         || base_presence == svn_wc__db_status_not_present

                         /* ### there are cases where the BASE node is
                            ### marked as incomplete. we should treat this
                            ### as a "normal" node for the purposes of
                            ### this function. we really should not allow
                            ### it, but this situation occurs within the
                            ### following tests:
                            ###   switch_tests 31
                            ###   update_tests 46
                            ###   update_tests 53
                         */
                         || base_presence == svn_wc__db_status_incomplete
                         );

#if 1
          /* ### see above comment  */
          if (base_presence == svn_wc__db_status_incomplete)
            base_presence = svn_wc__db_status_normal;
#endif

          /* If a BASE node is marked as not-present, then we'll ignore
             it within this function. That status is simply a bookkeeping
             gimmick, not a real node that may have been deleted.  */
        }

      /* Only grab the nearest ancestor.  */
      if (!found_moved_to &&
          (sd_baton->moved_to_relpath != NULL
                || sd_baton->base_del_relpath != NULL)
          && !svn_sqlite__column_is_null(stmt, 2 /* moved_to */))
        {
          /* There better be a BASE_NODE (that was moved-away).  */
          SVN_ERR_ASSERT(have_base);

          found_moved_to = TRUE;

          /* This makes things easy. It's the BASE_DEL_ABSPATH!  */
          if (sd_baton->base_del_relpath != NULL)
            *sd_baton->base_del_relpath = apr_pstrdup(sd_baton->result_pool,
                                                      current_relpath);

          if (sd_baton->moved_to_relpath != NULL)
            *sd_baton->moved_to_relpath = apr_pstrdup(sd_baton->result_pool,
                                    svn_sqlite__column_text(stmt, 2, NULL));
        }

      op_depth = svn_sqlite__column_int64(stmt, 3);
      if (current_relpath == local_relpath)
        local_op_depth = op_depth;

      if (sd_baton->work_del_relpath && !sd_baton->work_del_relpath[0]
          && ((op_depth < local_op_depth && op_depth > 0)
              || child_presence == svn_wc__db_status_not_present))
        {
          *sd_baton->work_del_relpath = apr_pstrdup(sd_baton->result_pool,
                                                    child_relpath);
        }

      /* We're all done examining the return values.  */
      SVN_ERR(svn_sqlite__reset(stmt));

      /* Move to the parent node. Remember the information about this node
         for our parent to use.  */
      child_relpath = current_relpath;
      child_presence = work_presence;
      child_has_base = have_base;

      /* The wcroot can't be deleted, but make sure we don't loop on invalid
         data */
      SVN_ERR_ASSERT(current_relpath[0] != '\0');

      current_relpath = svn_relpath_dirname(current_relpath, scratch_pool);
    }

  return SVN_NO_ERROR;
}


/* Like svn_wc__db_scan_deletion(), but with WCROOT+LOCAL_RELPATH instead of
   DB+LOCAL_ABSPATH, and outputting relpaths instead of abspaths. */
static svn_error_t *
scan_deletion(const char **base_del_relpath,
              const char **moved_to_relpath,
              const char **work_del_relpath,
              svn_wc__db_wcroot_t *wcroot,
              const char *local_relpath,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool)
{
  struct scan_deletion_baton_t sd_baton;

  sd_baton.base_del_relpath = base_del_relpath;
  sd_baton.moved_to_relpath = moved_to_relpath;
  sd_baton.work_del_relpath = work_del_relpath;
  sd_baton.result_pool = result_pool;

  return svn_error_return(svn_wc__db_with_txn(wcroot, local_relpath,
                                              scan_deletion_txn, &sd_baton,
                                              scratch_pool));
}


svn_error_t *
svn_wc__db_scan_deletion(const char **base_del_abspath,
                         const char **moved_to_abspath,
                         const char **work_del_abspath,
                         svn_wc__db_t *db,
                         const char *local_abspath,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  const char *base_del_relpath, *moved_to_relpath, *work_del_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(scan_deletion(&base_del_relpath, &moved_to_relpath,
                        &work_del_relpath, wcroot,
                        local_relpath, result_pool, scratch_pool));

  if (base_del_abspath)
    {
      *base_del_abspath = (base_del_relpath
                           ? svn_dirent_join(wcroot->abspath,
                                             base_del_relpath, result_pool)
                           : NULL);
    }
  if (moved_to_abspath)
    {
      *moved_to_abspath = (moved_to_relpath
                           ? svn_dirent_join(wcroot->abspath,
                                             moved_to_relpath, result_pool)
                           : NULL);
    }
  if (work_del_abspath)
    {
      *work_del_abspath = (work_del_relpath
                           ? svn_dirent_join(wcroot->abspath,
                                             work_del_relpath, result_pool)
                           : NULL);
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_upgrade_begin(svn_sqlite__db_t **sdb,
                         apr_int64_t *repos_id,
                         apr_int64_t *wc_id,
                         const char *dir_abspath,
                         const char *repos_root_url,
                         const char *repos_uuid,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  return svn_error_return(create_db(sdb, repos_id, wc_id, dir_abspath,
                                    repos_root_url, repos_uuid,
                                    SDB_FILE,
                                    result_pool, scratch_pool));
}


svn_error_t *
svn_wc__db_upgrade_apply_dav_cache(svn_sqlite__db_t *sdb,
                                   const char *dir_relpath,
                                   apr_hash_t *cache_values,
                                   apr_pool_t *scratch_pool)
{
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  apr_int64_t wc_id;
  apr_hash_index_t *hi;
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_wc__db_util_fetch_wc_id(&wc_id, sdb, iterpool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                    STMT_UPDATE_BASE_NODE_DAV_CACHE));

  /* Iterate over all the wcprops, writing each one to the wc_db. */
  for (hi = apr_hash_first(scratch_pool, cache_values);
       hi;
       hi = apr_hash_next(hi))
    {
      const char *name = svn__apr_hash_index_key(hi);
      apr_hash_t *props = svn__apr_hash_index_val(hi);
      const char *local_relpath;

      svn_pool_clear(iterpool);

      local_relpath = svn_relpath_join(dir_relpath, name, iterpool);

      SVN_ERR(svn_sqlite__bindf(stmt, "is", wc_id, local_relpath));
      SVN_ERR(svn_sqlite__bind_properties(stmt, 3, props, iterpool));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_upgrade_apply_props(svn_sqlite__db_t *sdb,
                               const char *dir_abspath,
                               const char *local_relpath,
                               apr_hash_t *base_props,
                               apr_hash_t *revert_props,
                               apr_hash_t *working_props,
                               int original_format,
                               apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_int64_t wc_id;
  apr_int64_t top_op_depth = -1;
  apr_int64_t below_op_depth = -1;
  svn_wc__db_status_t top_presence;
  svn_wc__db_status_t below_presence;
  int affected_rows;

  /* ### working_props: use set_props_txn.
     ### if working_props == NULL, then skip. what if they equal the
     ### pristine props? we should probably do the compare here.
     ###
     ### base props go into WORKING_NODE if avail, otherwise BASE.
     ###
     ### revert only goes into BASE. (and WORKING better be there!)

     Prior to 1.4.0 (ORIGINAL_FORMAT < 8), REVERT_PROPS did not exist. If a
     file was deleted, then a copy (potentially with props) was disallowed
     and could not replace the deletion. An addition *could* be performed,
     but that would never bring its own props.

     1.4.0 through 1.4.5 created the concept of REVERT_PROPS, but had a
     bug in svn_wc_add_repos_file2() whereby a copy-with-props did NOT
     construct a REVERT_PROPS if the target had no props. Thus, reverting
     the delete/copy would see no REVERT_PROPS to restore, leaving the
     props from the copy source intact, and appearing as if they are (now)
     the base props for the previously-deleted file. (wc corruption)

     1.4.6 ensured that an empty REVERT_PROPS would be established at all
     times. See issue 2530, and r861670 as starting points.

     We will use ORIGINAL_FORMAT and SVN_WC__NO_REVERT_FILES to determine
     the handling of our inputs, relative to the state of this node.
  */

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_SELECT_NODE_UPGRADE));
  SVN_ERR(svn_sqlite__bindf(stmt, "s", local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (have_row)
    {
      top_op_depth = svn_sqlite__column_int64(stmt, 0);
      top_presence = svn_sqlite__column_token(stmt, 1, presence_map);
      wc_id = svn_sqlite__column_int64(stmt, 2);
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
      if (have_row)
        {
          below_op_depth = svn_sqlite__column_int64(stmt, 0);
          below_presence = svn_sqlite__column_token(stmt, 1, presence_map);
        }
    }
  SVN_ERR(svn_sqlite__reset(stmt));

  /* Detect the buggy scenario described above. We cannot upgrade this
     working copy if we have no idea where BASE_PROPS should go.  */
  if (original_format > SVN_WC__NO_REVERT_FILES
      && revert_props == NULL
      && top_op_depth != -1
      && top_presence == svn_wc__db_status_normal
      && below_op_depth != -1
      && below_presence != svn_wc__db_status_not_present)
    {
      /* There should be REVERT_PROPS, so it appears that we just ran into
         the described bug. Sigh.  */
      return svn_error_createf(SVN_ERR_WC_CORRUPT, NULL,
                               _("The properties of '%s' are in an "
                                 "indeterminate state and cannot be "
                                 "upgraded. See issue #2530."),
                               svn_dirent_local_style(
                                 svn_dirent_join(dir_abspath, local_relpath,
                                                 scratch_pool), scratch_pool));
    }

  /* Need at least one row, or two rows if there are revert props */
  if (top_op_depth == -1
      || (below_op_depth == -1 && revert_props))
    return svn_error_createf(SVN_ERR_WC_CORRUPT, NULL,
                             _("Insufficient NODES rows for '%s'"),
                             svn_dirent_local_style(
                               svn_dirent_join(dir_abspath, local_relpath,
                                               scratch_pool), scratch_pool));

  /* one row, base props only: upper row gets base props
     two rows, base props only: lower row gets base props
     two rows, revert props only: lower row gets revert props
     two rows, base and revert props: upper row gets base, lower gets revert */


  if (revert_props || below_op_depth == -1)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                        STMT_UPDATE_NODE_PROPS));
      SVN_ERR(svn_sqlite__bindf(stmt, "isi",
                                wc_id, local_relpath, top_op_depth));
      SVN_ERR(svn_sqlite__bind_properties(stmt, 4, base_props, scratch_pool));
      SVN_ERR(svn_sqlite__update(&affected_rows, stmt));

      SVN_ERR_ASSERT(affected_rows == 1);
    }

  if (below_op_depth != -1)
    {
      apr_hash_t *props = revert_props ? revert_props : base_props;

      SVN_ERR(svn_sqlite__get_statement(&stmt, sdb,
                                        STMT_UPDATE_NODE_PROPS));
      SVN_ERR(svn_sqlite__bindf(stmt, "isi",
                                wc_id, local_relpath, below_op_depth));
      SVN_ERR(svn_sqlite__bind_properties(stmt, 4, props, scratch_pool));
      SVN_ERR(svn_sqlite__update(&affected_rows, stmt));

      SVN_ERR_ASSERT(affected_rows == 1);
    }

  /* If there are WORKING_PROPS, then they always go into ACTUAL_NODE.  */
  if (working_props != NULL
      && base_props != NULL)
    {
      apr_array_header_t *diffs;

      SVN_ERR(svn_prop_diffs(&diffs, working_props, base_props, scratch_pool));

      if (diffs->nelts == 0)
        working_props = NULL; /* No differences */
    }

  if (working_props != NULL)
    {
      SVN_ERR(set_actual_props(wc_id, local_relpath, working_props,
                               sdb, scratch_pool));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_upgrade_get_repos_id(apr_int64_t *repos_id,
                                svn_sqlite__db_t *sdb,
                                const char *repos_root_url,
                                apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, sdb, STMT_SELECT_REPOSITORY));
  SVN_ERR(svn_sqlite__bindf(stmt, "s", repos_root_url));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (!have_row)
    return svn_error_createf(SVN_ERR_WC_DB_ERROR, svn_sqlite__reset(stmt),
                             _("Repository '%s' not found in the database"),
                             repos_root_url);

  *repos_id = svn_sqlite__column_int64(stmt, 0);
  return svn_error_return(svn_sqlite__reset(stmt));
}


svn_error_t *
svn_wc__db_wq_add(svn_wc__db_t *db,
                  const char *wri_abspath,
                  const svn_skel_t *work_item,
                  apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));

  /* Quick exit, if there are no work items to queue up.  */
  if (work_item == NULL)
    return SVN_NO_ERROR;

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* Add the work item(s) to the WORK_QUEUE.  */
  return svn_error_return(add_work_items(wcroot->sdb, work_item,
                                         scratch_pool));
}


svn_error_t *
svn_wc__db_wq_fetch(apr_uint64_t *id,
                    svn_skel_t **work_item,
                    svn_wc__db_t *db,
                    const char *wri_abspath,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(id != NULL);
  SVN_ERR_ASSERT(work_item != NULL);
  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_WORK_ITEM));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (!have_row)
    {
      *id = 0;
      *work_item = NULL;
    }
  else
    {
      apr_size_t len;
      const void *val;

      *id = svn_sqlite__column_int64(stmt, 0);

      val = svn_sqlite__column_blob(stmt, 1, &len, result_pool);

      *work_item = svn_skel__parse(val, len, result_pool);
    }

  return svn_error_return(svn_sqlite__reset(stmt));
}


svn_error_t *
svn_wc__db_wq_completed(svn_wc__db_t *db,
                        const char *wri_abspath,
                        apr_uint64_t id,
                        apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));
  SVN_ERR_ASSERT(id != 0);

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_WORK_ITEM));
  SVN_ERR(svn_sqlite__bind_int64(stmt, 1, id));
  return svn_error_return(svn_sqlite__step_done(stmt));
}


/* ### temporary API. remove before release.  */
svn_error_t *
svn_wc__db_temp_get_format(int *format,
                           svn_wc__db_t *db,
                           const char *local_dir_abspath,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_error_t *err;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_dir_abspath));
  /* ### assert that we were passed a directory?  */

  err = svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                                local_dir_abspath, scratch_pool, scratch_pool);

  /* If we hit an error examining this directory, then declare this
     directory to not be a working copy.  */
  if (err)
    {
      if (err && err->apr_err != SVN_ERR_WC_NOT_WORKING_COPY)
        return svn_error_return(err);
      svn_error_clear(err);

      /* Remap the returned error.  */
      *format = 0;
      return svn_error_createf(SVN_ERR_WC_MISSING, NULL,
                               _("'%s' is not a working copy"),
                               svn_dirent_local_style(local_dir_abspath,
                                                      scratch_pool));
    }

  SVN_ERR_ASSERT(wcroot != NULL);
  SVN_ERR_ASSERT(wcroot->format >= 1);

  *format = wcroot->format;

  return SVN_NO_ERROR;
}


/* ### temporary API. remove before release.  */
svn_error_t *
svn_wc__db_temp_forget_directory(svn_wc__db_t *db,
                                 const char *local_dir_abspath,
                                 apr_pool_t *scratch_pool)
{
  apr_hash_t *roots = apr_hash_make(scratch_pool);
  apr_hash_index_t *hi;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  for (hi = apr_hash_first(scratch_pool, db->dir_data);
       hi;
       hi = apr_hash_next(hi))
    {
      svn_wc__db_wcroot_t *wcroot = svn__apr_hash_index_val(hi);
      const char *local_abspath = svn__apr_hash_index_key(hi);
      svn_error_t *err;

      if (!svn_dirent_is_ancestor(local_dir_abspath, local_abspath))
        continue;

      svn_pool_clear(iterpool);

      err = svn_wc__db_wclock_release(db, local_abspath, iterpool);
      if (err
          && (err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY
              || err->apr_err == SVN_ERR_WC_NOT_LOCKED))
        {
          svn_error_clear(err);
        }
      else
        SVN_ERR(err);

      apr_hash_set(db->dir_data, local_abspath, APR_HASH_KEY_STRING, NULL);

      if (wcroot->sdb &&
          svn_dirent_is_ancestor(local_dir_abspath, wcroot->abspath))
        {
          apr_hash_set(roots, wcroot->abspath, APR_HASH_KEY_STRING, wcroot);
        }
    }
  svn_pool_destroy(iterpool);

  return svn_error_return(svn_wc__db_close_many_wcroots(roots, db->state_pool,
                                                        scratch_pool));
}


/* ### temporary API. remove before release.  */
svn_wc_adm_access_t *
svn_wc__db_temp_get_access(svn_wc__db_t *db,
                           const char *local_dir_abspath,
                           apr_pool_t *scratch_pool)
{
  const char *local_relpath;
  svn_wc__db_wcroot_t *wcroot;
  svn_error_t *err;

  SVN_ERR_ASSERT_NO_RETURN(svn_dirent_is_absolute(local_dir_abspath));

  /* ### we really need to assert that we were passed a directory. sometimes
     ### adm_retrieve_internal is asked about a file, and then it asks us
     ### for an access baton for it. we should definitely return NULL, but
     ### ideally: the caller would never ask us about a non-directory.  */

  err = svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                            db, local_dir_abspath, scratch_pool, scratch_pool);
  if (err)
    {
      svn_error_clear(err);
      return NULL;
    }

  if (!wcroot)
    return NULL;

  return apr_hash_get(wcroot->access_cache, local_dir_abspath,
                      APR_HASH_KEY_STRING);
}


/* ### temporary API. remove before release.  */
void
svn_wc__db_temp_set_access(svn_wc__db_t *db,
                           const char *local_dir_abspath,
                           svn_wc_adm_access_t *adm_access,
                           apr_pool_t *scratch_pool)
{
  const char *local_relpath;
  svn_wc__db_wcroot_t *wcroot;
  svn_error_t *err;

  SVN_ERR_ASSERT_NO_RETURN(svn_dirent_is_absolute(local_dir_abspath));
  /* ### assert that we were passed a directory?  */

  err = svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                            db, local_dir_abspath, scratch_pool, scratch_pool);
  if (err)
    {
      /* We don't even have a wcroot, so just bail. */
      svn_error_clear(err);
      return;
    }

  /* Better not override something already there.  */
  SVN_ERR_ASSERT_NO_RETURN(apr_hash_get(wcroot->access_cache,
                                        local_dir_abspath,
                                        APR_HASH_KEY_STRING) == NULL);
  apr_hash_set(wcroot->access_cache, local_dir_abspath,
               APR_HASH_KEY_STRING, adm_access);
}


/* ### temporary API. remove before release.  */
svn_error_t *
svn_wc__db_temp_close_access(svn_wc__db_t *db,
                             const char *local_dir_abspath,
                             svn_wc_adm_access_t *adm_access,
                             apr_pool_t *scratch_pool)
{
  const char *local_relpath;
  svn_wc__db_wcroot_t *wcroot;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_dir_abspath));
  /* ### assert that we were passed a directory?  */

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_dir_abspath, scratch_pool, scratch_pool));
  apr_hash_set(wcroot->access_cache, local_dir_abspath,
               APR_HASH_KEY_STRING, NULL);

  return SVN_NO_ERROR;
}


/* ### temporary API. remove before release.  */
void
svn_wc__db_temp_clear_access(svn_wc__db_t *db,
                             const char *local_dir_abspath,
                             apr_pool_t *scratch_pool)
{
  const char *local_relpath;
  svn_wc__db_wcroot_t *wcroot;
  svn_error_t *err;

  SVN_ERR_ASSERT_NO_RETURN(svn_dirent_is_absolute(local_dir_abspath));
  /* ### assert that we were passed a directory?  */

  err = svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                            db, local_dir_abspath, scratch_pool, scratch_pool);
  if (err)
    {
      svn_error_clear(err);
      return;
    }

  apr_hash_set(wcroot->access_cache, local_dir_abspath,
               APR_HASH_KEY_STRING, NULL);
}


apr_hash_t *
svn_wc__db_temp_get_all_access(svn_wc__db_t *db,
                               apr_pool_t *result_pool)
{
  apr_hash_t *result = apr_hash_make(result_pool);
  apr_hash_index_t *hi;

  for (hi = apr_hash_first(result_pool, db->dir_data);
       hi;
       hi = apr_hash_next(hi))
    {
      const svn_wc__db_wcroot_t *wcroot = svn__apr_hash_index_val(hi);

      /* This is highly redundant, 'cause the same WCROOT will appear many
         times in dir_data. */
      result = apr_hash_overlay(result_pool, result, wcroot->access_cache);
    }

  return result;
}


svn_error_t *
svn_wc__db_temp_borrow_sdb(svn_sqlite__db_t **sdb,
                           svn_wc__db_t *db,
                           const char *local_dir_abspath,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_dir_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                            local_dir_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  *sdb = wcroot->sdb;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_read_conflict_victims(const apr_array_header_t **victims,
                                 svn_wc__db_t *db,
                                 const char *local_abspath,
                                 apr_pool_t *result_pool,
                                 apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_array_header_t *new_victims;

  /* The parent should be a working copy directory. */
  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* ### This will be much easier once we have all conflicts in one
         field of actual*/

  /* Look for text, tree and property conflicts in ACTUAL */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_ACTUAL_CONFLICT_VICTIMS));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));

  new_victims = apr_array_make(result_pool, 0, sizeof(const char *));

  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  while (have_row)
    {
      const char *child_relpath = svn_sqlite__column_text(stmt, 0, NULL);

      APR_ARRAY_PUSH(new_victims, const char *) =
                            svn_dirent_basename(child_relpath, result_pool);

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  *victims = new_victims;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_read_conflicts(const apr_array_header_t **conflicts,
                          svn_wc__db_t *db,
                          const char *local_abspath,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_array_header_t *cflcts;

  /* The parent should be a working copy directory. */
  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* ### This will be much easier once we have all conflicts in one
         field of actual.*/

  /* First look for text and property conflicts in ACTUAL */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_CONFLICT_DETAILS));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));

  cflcts = apr_array_make(result_pool, 4,
                           sizeof(svn_wc_conflict_description2_t*));

  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (have_row)
    {
      const char *prop_reject;
      const char *conflict_old;
      const char *conflict_new;
      const char *conflict_working;
      const char *conflict_data;

      /* ### Store in description! */
      prop_reject = svn_sqlite__column_text(stmt, 0, NULL);
      if (prop_reject)
        {
          svn_wc_conflict_description2_t *desc;

          desc  = svn_wc_conflict_description_create_prop2(local_abspath,
                                                           svn_node_unknown,
                                                           "",
                                                           result_pool);

          desc->their_abspath = svn_dirent_join(wcroot->abspath, prop_reject,
                                                result_pool);

          APR_ARRAY_PUSH(cflcts, svn_wc_conflict_description2_t*) = desc;
        }

      conflict_old = svn_sqlite__column_text(stmt, 1, NULL);
      conflict_new = svn_sqlite__column_text(stmt, 2, NULL);
      conflict_working = svn_sqlite__column_text(stmt, 3, NULL);

      if (conflict_old || conflict_new || conflict_working)
        {
          svn_wc_conflict_description2_t *desc
              = svn_wc_conflict_description_create_text2(local_abspath,
                                                         result_pool);

          if (conflict_old)
            desc->base_abspath = svn_dirent_join(wcroot->abspath, conflict_old,
                                                 result_pool);
          if (conflict_new)
            desc->their_abspath = svn_dirent_join(wcroot->abspath, conflict_new,
                                                  result_pool);
          if (conflict_working)
            desc->my_abspath = svn_dirent_join(wcroot->abspath,
                                               conflict_working, result_pool);
          desc->merged_file = svn_dirent_basename(local_abspath, result_pool);

          APR_ARRAY_PUSH(cflcts, svn_wc_conflict_description2_t*) = desc;
        }

      conflict_data = svn_sqlite__column_text(stmt, 4, scratch_pool);
      if (conflict_data)
        {
          const svn_wc_conflict_description2_t *desc;
          const svn_skel_t *skel;
          svn_error_t *err;

          skel = svn_skel__parse(conflict_data, strlen(conflict_data),
                                 scratch_pool);
          err = svn_wc__deserialize_conflict(&desc, skel,
                          svn_dirent_dirname(local_abspath, scratch_pool),
                          result_pool, scratch_pool);

          if (err)
            SVN_ERR(svn_error_compose_create(err,
                                             svn_sqlite__reset(stmt)));

          APR_ARRAY_PUSH(cflcts, const svn_wc_conflict_description2_t *) = desc;
        }
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  *conflicts = cflcts;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_read_kind(svn_wc__db_kind_t *kind,
                     svn_wc__db_t *db,
                     const char *local_abspath,
                     svn_boolean_t allow_missing,
                     apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt_info;
  svn_boolean_t have_info;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_sqlite__get_statement(&stmt_info, wcroot->sdb,
                                    STMT_SELECT_NODE_INFO));
  SVN_ERR(svn_sqlite__bindf(stmt_info, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_info, stmt_info));

  if (!have_info)
    {
      if (allow_missing)
        {
          *kind = svn_wc__db_kind_unknown;
          SVN_ERR(svn_sqlite__reset(stmt_info));
          return SVN_NO_ERROR;
        }
      else
        {
          SVN_ERR(svn_sqlite__reset(stmt_info));
          return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                                   _("The node '%s' was not found."),
                                   path_for_error_message(wcroot,
                                                          local_relpath,
                                                          scratch_pool));
        }
    }

  *kind = svn_sqlite__column_token(stmt_info, 4, kind_map);

  return svn_error_return(svn_sqlite__reset(stmt_info));
}


svn_error_t *
svn_wc__db_node_hidden(svn_boolean_t *hidden,
                       svn_wc__db_t *db,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_wc__db_status_t status;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(read_info(&status, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL,
                    wcroot, local_relpath,
                    scratch_pool, scratch_pool));

  *hidden = (status == svn_wc__db_status_absent
             || status == svn_wc__db_status_not_present
             || status == svn_wc__db_status_excluded);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_is_wcroot(svn_boolean_t *is_root,
                     svn_wc__db_t *db,
                     const char *local_abspath,
                     apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  if (*local_relpath != '\0')
    {
      *is_root = FALSE; /* Node is a file, or has a parent directory within
                           the same wcroot */
      return SVN_NO_ERROR;
    }

   *is_root = TRUE;

   return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_wcroot_tempdir(const char **temp_dir_abspath,
                               svn_wc__db_t *db,
                               const char *wri_abspath,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(temp_dir_abspath != NULL);
  SVN_ERR_ASSERT(svn_dirent_is_absolute(wri_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              wri_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  *temp_dir_abspath = svn_dirent_join_many(result_pool,
                                           wcroot->abspath,
                                           svn_wc_get_adm_dir(scratch_pool),
                                           WCROOT_TEMPDIR_RELPATH,
                                           NULL);
  return SVN_NO_ERROR;
}


/* Baton for wclock_obtain_cb() */
struct wclock_obtain_baton_t
{
  int levels_to_lock;
  svn_boolean_t steal_lock;
};


/* Helper for wclock_obtain_cb() to steal an existing lock */
static svn_error_t *
wclock_steal(svn_wc__db_wcroot_t *wcroot,
             const char *local_relpath,
             apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_DELETE_WC_LOCK));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));

  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}


/* svn_sqlite__transaction_callback_t for svn_wc__db_wclock_obtain() */
static svn_error_t *
wclock_obtain_cb(void *baton,
                 svn_wc__db_wcroot_t *wcroot,
                 const char *local_relpath,
                 apr_pool_t *scratch_pool)
{
  struct wclock_obtain_baton_t *bt = baton;
  svn_sqlite__stmt_t *stmt;
  svn_error_t *err;
  const char *lock_relpath;
  int max_depth;
  int lock_depth;
  svn_boolean_t got_row;
  const char *filter;

  svn_wc__db_wclock_t lock;

  /* Upgrade locks the root before the node exists.  Apart from that
     the root node always exists so we will just skip the check.

     ### Perhaps the lock for upgrade should be created when the db is
         created?  1.6 used to lock .svn on creation. */
  if (local_relpath[0])
    {
      svn_boolean_t have_any;

      SVN_ERR(which_trees_exist(&have_any, NULL, NULL, wcroot->sdb,
                                wcroot->wc_id, local_relpath));

      if (!have_any)
        return svn_error_createf(
                                 SVN_ERR_WC_PATH_NOT_FOUND, NULL,
                                 _("The node '%s' was not found."),
                                 path_for_error_message(wcroot,
                                                        local_relpath,
                                                        scratch_pool));
    }

  filter = construct_like_arg(local_relpath, scratch_pool);

  /* Check if there are nodes locked below the new lock root */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_FIND_WC_LOCK));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, filter));

  lock_depth = relpath_depth(local_relpath);
  max_depth = lock_depth + bt->levels_to_lock;

  SVN_ERR(svn_sqlite__step(&got_row, stmt));

  while (got_row)
    {
      svn_boolean_t own_lock;

      lock_relpath = svn_sqlite__column_text(stmt, 0, scratch_pool);

      /* If we are not locking with depth infinity, check if this lock
         voids our lock request */
      if (bt->levels_to_lock >= 0
          && relpath_depth(lock_relpath) > max_depth)
        {
          SVN_ERR(svn_sqlite__step(&got_row, stmt));
          continue;
        }

      /* Check if we are the lock owner, because we should be able to
         extend our lock. */
      err = wclock_owns_lock(&own_lock, wcroot, lock_relpath,
                             TRUE, scratch_pool);

      if (err)
        SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));

      if (!own_lock && !bt->steal_lock)
        {
          SVN_ERR(svn_sqlite__reset(stmt));
          err = svn_error_createf(SVN_ERR_WC_LOCKED, NULL,
                                   _("'%s' is already locked."),
                                   path_for_error_message(wcroot,
                                                          lock_relpath,
                                                          scratch_pool));
          return svn_error_createf(SVN_ERR_WC_LOCKED, err,
                                   _("Working copy '%s' locked."),
                                   path_for_error_message(wcroot,
                                                          local_relpath,
                                                          scratch_pool));
        }
      else if (!own_lock)
        {
          err = wclock_steal(wcroot, lock_relpath, scratch_pool);

          if (err)
            SVN_ERR(svn_error_compose_create(err, svn_sqlite__reset(stmt)));
        }

      SVN_ERR(svn_sqlite__step(&got_row, stmt));
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  if (bt->steal_lock)
    SVN_ERR(wclock_steal(wcroot, local_relpath, scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_SELECT_WC_LOCK));
  lock_relpath = local_relpath;

  while (TRUE)
    {
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, lock_relpath));

      SVN_ERR(svn_sqlite__step(&got_row, stmt));

      if (got_row)
        {
          int levels = svn_sqlite__column_int(stmt, 0);
          if (levels >= 0)
            levels += relpath_depth(lock_relpath);

          SVN_ERR(svn_sqlite__reset(stmt));

          if (levels == -1 || levels >= lock_depth)
            {

              err = svn_error_createf(
                              SVN_ERR_WC_LOCKED, NULL,
                              _("'%s' is already locked."),
                              svn_dirent_local_style(
                                       svn_dirent_join(wcroot->abspath,
                                                       lock_relpath,
                                                       scratch_pool),
                              scratch_pool));
              return svn_error_createf(
                              SVN_ERR_WC_LOCKED, err,
                              _("Working copy '%s' locked."),
                              path_for_error_message(wcroot,
                                                     local_relpath,
                                                     scratch_pool));
            }

          break; /* There can't be interesting locks on higher nodes */
        }
      else
        SVN_ERR(svn_sqlite__reset(stmt));

      if (!*lock_relpath)
        break;

      lock_relpath = svn_relpath_dirname(lock_relpath, scratch_pool);
    }

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_INSERT_WC_LOCK));
  SVN_ERR(svn_sqlite__bindf(stmt, "isi", wcroot->wc_id, local_relpath,
                            (apr_int64_t) bt->levels_to_lock));
  err = svn_sqlite__insert(NULL, stmt);
  if (err)
    return svn_error_createf(SVN_ERR_WC_LOCKED, err,
                             _("Working copy '%s' locked"),
                             path_for_error_message(wcroot,
                                                    local_relpath,
                                                    scratch_pool));

  /* And finally store that we obtained the lock */
  lock.local_relpath = apr_pstrdup(wcroot->owned_locks->pool, local_relpath);
  lock.levels = bt->levels_to_lock;
  APR_ARRAY_PUSH(wcroot->owned_locks, svn_wc__db_wclock_t) = lock;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_wclock_obtain(svn_wc__db_t *db,
                         const char *local_abspath,
                         int levels_to_lock,
                         svn_boolean_t steal_lock,
                         apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct wclock_obtain_baton_t baton;

  SVN_ERR_ASSERT(levels_to_lock >= -1);
  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                             db, local_abspath,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  if (!steal_lock)
    {
      int i;
      int depth = relpath_depth(local_relpath);

      for (i = 0; i < wcroot->owned_locks->nelts; i++)
        {
          svn_wc__db_wclock_t* lock = &APR_ARRAY_IDX(wcroot->owned_locks,
                                                     i, svn_wc__db_wclock_t);

          if (svn_relpath_is_ancestor(lock->local_relpath, local_relpath)
              && (lock->levels == -1
                  || (lock->levels + relpath_depth(lock->local_relpath))
                            >= depth))
            {
              return svn_error_createf(
                SVN_ERR_WC_LOCKED, NULL,
                _("'%s' is already locked via '%s'."),
                svn_dirent_local_style(local_abspath, scratch_pool),
                path_for_error_message(wcroot, lock->local_relpath,
                                       scratch_pool));
            }
        }
    }

  baton.steal_lock = steal_lock;
  baton.levels_to_lock = levels_to_lock;

  return svn_error_return(svn_wc__db_with_txn(wcroot, local_relpath,
                                              wclock_obtain_cb, &baton,
                                              scratch_pool));
}


/* */
static svn_error_t *
is_wclocked(svn_boolean_t *locked,
            svn_wc__db_t *db,
            const char *local_abspath,
            apr_int64_t recurse_depth,
            apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  svn_error_t *err;

  err = get_statement_for_path(&stmt, db, local_abspath,
                               STMT_SELECT_WC_LOCK, scratch_pool);
  if (err && SVN_WC__ERR_IS_NOT_CURRENT_WC(err))
    {
      svn_error_clear(err);
      *locked = FALSE;
      return SVN_NO_ERROR;
    }
  else
    SVN_ERR(err);

  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (have_row)
    {
      apr_int64_t locked_levels = svn_sqlite__column_int64(stmt, 0);

      /* The directory in question is considered locked if we find a lock
         with depth -1 or the depth of the lock is greater than or equal to
         the depth we've recursed. */
      *locked = (locked_levels == -1 || locked_levels >= recurse_depth);
      return svn_error_return(svn_sqlite__reset(stmt));
    }

  SVN_ERR(svn_sqlite__reset(stmt));

  if (svn_dirent_is_root(local_abspath, strlen(local_abspath)))
    {
      *locked = FALSE;
      return SVN_NO_ERROR;
    }

  return svn_error_return(is_wclocked(locked, db,
                                      svn_dirent_dirname(local_abspath,
                                                         scratch_pool),
                                      recurse_depth + 1, scratch_pool));
}


svn_error_t *
svn_wc__db_wclocked(svn_boolean_t *locked,
                    svn_wc__db_t *db,
                    const char *local_abspath,
                    apr_pool_t *scratch_pool)
{
  return svn_error_return(is_wclocked(locked, db, local_abspath, 0,
                                      scratch_pool));
}


svn_error_t *
svn_wc__db_wclock_release(svn_wc__db_t *db,
                          const char *local_abspath,
                          apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  int i;
  apr_array_header_t *owned_locks;

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));

  VERIFY_USABLE_WCROOT(wcroot);

  /* First check and remove the owns-lock information as failure in
     removing the db record implies that we have to steal the lock later. */
  owned_locks = wcroot->owned_locks;
  for (i = 0; i < owned_locks->nelts; i++)
    {
      svn_wc__db_wclock_t *lock = &APR_ARRAY_IDX(owned_locks, i,
                                                 svn_wc__db_wclock_t);

      if (strcmp(lock->local_relpath, local_relpath) == 0)
        break;
    }

  if (i >= owned_locks->nelts)
    return svn_error_createf(SVN_ERR_WC_NOT_LOCKED, NULL,
                             _("Working copy not locked at '%s'."),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  if (i < owned_locks->nelts)
    {
      owned_locks->nelts--;

      /* Move the last item in the array to the deleted place */
      if (owned_locks->nelts > 0)
        APR_ARRAY_IDX(owned_locks, i, svn_wc__db_wclock_t) =
           APR_ARRAY_IDX(owned_locks, owned_locks->nelts, svn_wc__db_wclock_t);
    }

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_DELETE_WC_LOCK));

  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));

  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}


/* Like svn_wc__db_wclock_owns_lock() but taking WCROOT+LOCAL_RELPATH instead
   of DB+LOCAL_ABSPATH.  */
static svn_error_t *
wclock_owns_lock(svn_boolean_t *own_lock,
                 svn_wc__db_wcroot_t *wcroot,
                 const char *local_relpath,
                 svn_boolean_t exact,
                 apr_pool_t *scratch_pool)
{
  apr_array_header_t *owned_locks;
  int lock_level;
  int i;

  *own_lock = FALSE;
  owned_locks = wcroot->owned_locks;
  lock_level = relpath_depth(local_relpath);

  if (exact)
    {
      for (i = 0; i < owned_locks->nelts; i++)
        {
          svn_wc__db_wclock_t *lock = &APR_ARRAY_IDX(owned_locks, i,
                                                     svn_wc__db_wclock_t);

          if (strcmp(lock->local_relpath, local_relpath) == 0)
            {
              *own_lock = TRUE;
              return SVN_NO_ERROR;
            }
        }
    }
  else
    {
      for (i = 0; i < owned_locks->nelts; i++)
        {
          svn_wc__db_wclock_t *lock = &APR_ARRAY_IDX(owned_locks, i,
                                                     svn_wc__db_wclock_t);

          if (svn_relpath_is_ancestor(lock->local_relpath, local_relpath)
              && (lock->levels == -1
                  || ((relpath_depth(lock->local_relpath) + lock->levels)
                      >= lock_level)))
            {
              *own_lock = TRUE;
              return SVN_NO_ERROR;
            }
        }
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_wclock_owns_lock(svn_boolean_t *own_lock,
                            svn_wc__db_t *db,
                            const char *local_abspath,
                            svn_boolean_t exact,
                            apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));

  if (!wcroot)
    return svn_error_createf(SVN_ERR_WC_NOT_WORKING_COPY, NULL,
                             _("The node '%s' was not found."),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(wclock_owns_lock(own_lock, wcroot, local_relpath, exact,
                           scratch_pool));

  return SVN_NO_ERROR;
}

/* Lock helper for svn_wc__db_temp_op_end_directory_update */
static svn_error_t *
end_directory_update(void *baton,
                     svn_wc__db_wcroot_t *wcroot,
                     const char *local_relpath,
                     apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_wc__db_status_t base_status;

  SVN_ERR(base_get_info(&base_status, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                        wcroot, local_relpath, scratch_pool, scratch_pool));

  SVN_ERR_ASSERT(base_status == svn_wc__db_status_incomplete);

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_UPDATE_NODE_BASE_PRESENCE));
  SVN_ERR(svn_sqlite__bindf(stmt, "ist", wcroot->wc_id, local_relpath,
                            presence_map, svn_wc__db_status_normal));
  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__db_temp_op_end_directory_update(svn_wc__db_t *db,
                                        const char *local_dir_abspath,
                                        apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_dir_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_dir_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, end_directory_update,
                              NULL, scratch_pool));

  SVN_ERR(flush_entries(wcroot, local_dir_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


struct start_directory_update_baton_t
{
  svn_revnum_t new_rev;
  const char *new_repos_relpath;
};


static svn_error_t *
start_directory_update_txn(void *baton,
                           svn_wc__db_wcroot_t *wcroot,
                           const char *local_relpath,
                           apr_pool_t *scratch_pool)
{
  struct start_directory_update_baton_t *du = baton;
  svn_sqlite__stmt_t *stmt;

  /* Note: In the majority of calls, the repos_relpath is unchanged. */
  /* ### TODO: Maybe check if we can make repos_relpath NULL. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                    STMT_UPDATE_BASE_NODE_PRESENCE_REVNUM_AND_REPOS_PATH));

  SVN_ERR(svn_sqlite__bindf(stmt, "istrs",
                            wcroot->wc_id,
                            local_relpath,
                            presence_map, svn_wc__db_status_incomplete,
                            du->new_rev,
                            du->new_repos_relpath));
  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;

}

svn_error_t *
svn_wc__db_temp_op_start_directory_update(svn_wc__db_t *db,
                                          const char *local_abspath,
                                          const char *new_repos_relpath,
                                          svn_revnum_t new_rev,
                                          apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct start_directory_update_baton_t du;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(new_rev));
  SVN_ERR_ASSERT(svn_relpath_is_canonical(new_repos_relpath, scratch_pool));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  du.new_rev = new_rev;
  du.new_repos_relpath = new_repos_relpath;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath,
                              start_directory_update_txn, &du, scratch_pool));

  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


/* Baton for make_copy_txn */
struct make_copy_baton_t
{
  apr_int64_t op_depth;
};


/* Transaction callback for svn_wc__db_temp_op_make_copy.  This is
   used by the update editor when deleting a base node tree would be a
   tree-conflict because there are changes to subtrees.  This function
   inserts a copy of the base node tree below any existing working
   subtrees.  Given a tree:

             0            1           2            3
    /     normal          -
    A     normal          -
    A/B   normal          -         normal
    A/B/C normal          -         base-del       normal
    A/F   normal          -         normal
    A/F/G normal          -         normal
    A/F/H normal          -         base-deleted   normal
    A/F/E normal          -         not-present
    A/X   normal          -
    A/X/Y incomplete      -

    This function adds layers to A and some of its descendants in an attempt
    to make the working copy look like as if it were a copy of the BASE nodes.

             0            1              2            3
    /     normal        -
    A     normal        norm
    A/B   normal        norm        norm
    A/B/C normal        norm        base-del       normal
    A/F   normal        norm        norm
    A/F/G normal        norm        norm
    A/F/H normal        norm        not-pres
    A/F/E normal        norm        base-del
    A/X   normal        norm
    A/X/Y incomplete  incomplete
 */
static svn_error_t *
make_copy_txn(void *baton,
              svn_wc__db_wcroot_t *wcroot,
              const char *local_relpath,
              apr_pool_t *scratch_pool)
{
  struct make_copy_baton_t *mcb = baton;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  svn_boolean_t add_working_base_deleted = FALSE;
  svn_boolean_t remove_working = FALSE;
  const apr_array_header_t *children;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);
  int i;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_LOWEST_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (have_row)
    {
      svn_wc__db_status_t working_status;
      apr_int64_t working_op_depth;

      working_status = svn_sqlite__column_token(stmt, 1, presence_map);
      working_op_depth = svn_sqlite__column_int64(stmt, 0);
      SVN_ERR(svn_sqlite__reset(stmt));

      SVN_ERR_ASSERT(working_status == svn_wc__db_status_normal
                     || working_status == svn_wc__db_status_base_deleted
                     || working_status == svn_wc__db_status_not_present
                     || working_status == svn_wc__db_status_incomplete);

      /* Only change nodes in the layers where we are creating the copy.
         Deletes in higher layers will just apply to the copy */
      if (working_op_depth <= mcb->op_depth)
        {
          add_working_base_deleted = TRUE;

          if (working_status == svn_wc__db_status_base_deleted)
            remove_working = TRUE;
        }
    }
  else
    SVN_ERR(svn_sqlite__reset(stmt));

  /* Get the BASE children, as WORKING children don't need modifications */
  SVN_ERR(gather_repo_children(&children, wcroot, local_relpath,
                               0, scratch_pool, iterpool));

  for (i = 0; i < children->nelts; i++)
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);
      struct make_copy_baton_t cbt;
      const char *copy_relpath;

      svn_pool_clear(iterpool);

      copy_relpath = svn_relpath_join(local_relpath, name, iterpool);

      cbt.op_depth = mcb->op_depth;

      SVN_ERR(make_copy_txn(&cbt, wcroot, copy_relpath, iterpool));
    }

  if (remove_working)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_DELETE_LOWEST_WORKING_NODE));
      SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  if (add_working_base_deleted)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                           STMT_INSERT_WORKING_NODE_FROM_BASE_COPY_PRESENCE));
      SVN_ERR(svn_sqlite__bindf(stmt, "isit", wcroot->wc_id, local_relpath,
                                mcb->op_depth, presence_map,
                                svn_wc__db_status_base_deleted));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }
  else
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                      STMT_INSERT_WORKING_NODE_FROM_BASE_COPY));
      SVN_ERR(svn_sqlite__bindf(stmt, "isi", wcroot->wc_id, local_relpath,
                                mcb->op_depth));
      SVN_ERR(svn_sqlite__step_done(stmt));
    }

  SVN_ERR(flush_entries(wcroot, svn_dirent_join(wcroot->abspath, local_relpath,
                                                iterpool), iterpool));

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_op_make_copy(svn_wc__db_t *db,
                             const char *local_abspath,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct make_copy_baton_t mcb;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* The update editor is supposed to call this function when there is
     no working node for LOCAL_ABSPATH. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_WORKING_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));
  if (have_row)
    return svn_error_createf(SVN_ERR_WC_PATH_UNEXPECTED_STATUS, NULL,
                             _("Modification of '%s' already exists"),
                             path_for_error_message(wcroot,
                                                    local_relpath,
                                                    scratch_pool));

  /* We don't allow copies to contain absent (denied by authz) nodes;
     the update editor is going to have to bail out. */
  SVN_ERR(catch_copy_of_absent(wcroot, local_relpath, scratch_pool));

  mcb.op_depth = relpath_depth(local_relpath);

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, make_copy_txn, &mcb,
                              scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_get_file_external(const char **serialized_file_external,
                                  svn_wc__db_t *db,
                                  const char *local_abspath,
                                  apr_pool_t *result_pool,
                                  apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(get_statement_for_path(&stmt, db, local_abspath,
                                 STMT_SELECT_FILE_EXTERNAL, scratch_pool));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  /* ### file externals are pretty bogus right now. they have just a
     ### WORKING_NODE for a while, eventually settling into just a BASE_NODE.
     ### until we get all that fixed, let's just not worry about raising
     ### an error, and just say it isn't a file external.  */
#if 1
  if (!have_row)
    *serialized_file_external = NULL;
  else
    /* see below: *serialized_file_external = ...  */
#else
  if (!have_row)
    return svn_error_createf(SVN_ERR_WC_PATH_NOT_FOUND,
                             svn_sqlite__reset(stmt),
                             _("'%s' has no BASE_NODE"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));
#endif

  *serialized_file_external = svn_sqlite__column_text(stmt, 0, result_pool);

  return svn_error_return(svn_sqlite__reset(stmt));
}


struct set_file_external_baton_t
{
  const char *repos_relpath;
  const svn_opt_revision_t *peg_rev;
  const svn_opt_revision_t *rev;
};


static svn_error_t *
set_file_external_txn(void *baton,
                      svn_wc__db_wcroot_t *wcroot,
                      const char *local_relpath,
                      apr_pool_t *scratch_pool)
{
  struct set_file_external_baton_t *sfeb = baton;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t got_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_BASE_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  SVN_ERR(svn_sqlite__step(&got_row, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));

  if (!got_row)
    {
      const char *dir_relpath;
      svn_node_kind_t kind;
      apr_int64_t repos_id;

      if (!sfeb->repos_relpath)
        return SVN_NO_ERROR; /* Don't add a BASE node */

      SVN_ERR(svn_io_check_path(svn_dirent_join(wcroot->abspath,
                                                local_relpath, scratch_pool),
                                &kind, scratch_pool));
      if (kind == svn_node_dir)
        dir_relpath = local_relpath;
      else
        dir_relpath = svn_relpath_dirname(local_relpath, scratch_pool);

      SVN_ERR(scan_upwards_for_repos(&repos_id, NULL, wcroot, dir_relpath,
                                     scratch_pool, scratch_pool));

      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_INSERT_NODE));

      SVN_ERR(svn_sqlite__bindf(stmt, "isisisntnt",
                                wcroot->wc_id,
                                local_relpath,
                                (apr_int64_t)0, /* op_depth == BASE */
                                svn_relpath_dirname(local_relpath,
                                                    scratch_pool),
                                repos_id,
                                sfeb->repos_relpath,
                                presence_map, svn_wc__db_status_not_present,
                                kind_map, svn_wc__db_kind_file));

      SVN_ERR(svn_sqlite__insert(NULL, stmt));
    }

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_UPDATE_FILE_EXTERNAL));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));
  if (sfeb->repos_relpath)
    {
      const char *str;

      SVN_ERR(svn_wc__serialize_file_external(&str,
                                              sfeb->repos_relpath,
                                              sfeb->peg_rev,
                                              sfeb->rev,
                                              scratch_pool));

      SVN_ERR(svn_sqlite__bind_text(stmt, 3, str));
    }
  SVN_ERR(svn_sqlite__step_done(stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_op_set_file_external(svn_wc__db_t *db,
                                     const char *local_abspath,
                                     const char *repos_relpath,
                                     const svn_opt_revision_t *peg_rev,
                                     const svn_opt_revision_t *rev,
                                     apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct set_file_external_baton_t sfeb;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(!repos_relpath
                 || svn_relpath_is_canonical(repos_relpath, scratch_pool));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                                             local_abspath,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  sfeb.repos_relpath = repos_relpath;
  sfeb.peg_rev = peg_rev;
  sfeb.rev = rev;

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath, set_file_external_txn,
                              &sfeb, scratch_pool));

  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_op_set_text_conflict_marker_files(svn_wc__db_t *db,
                                                  const char *local_abspath,
                                                  const char *old_abspath,
                                                  const char *new_abspath,
                                                  const char *wrk_abspath,
                                                  apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath, *old_relpath, *new_relpath, *wrk_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t got_row;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(svn_dirent_is_absolute(old_abspath));
  SVN_ERR_ASSERT(svn_dirent_is_absolute(new_abspath));
  /* Binary files usually send NULL */
  SVN_ERR_ASSERT(!wrk_abspath || svn_dirent_is_absolute(wrk_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                                             local_abspath,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* This should be handled in a transaction, but we can assume a db lock
     and this code won't survive until 1.7 */

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_ACTUAL_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));

  SVN_ERR(svn_sqlite__step(&got_row, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));

  if (got_row)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_UPDATE_ACTUAL_TEXT_CONFLICTS));
    }
  else if (old_abspath == NULL
           && new_abspath == NULL
           && wrk_abspath == NULL)
    {
      return SVN_NO_ERROR; /* We don't have to add anything */
    }
  else
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_INSERT_ACTUAL_TEXT_CONFLICTS));

      SVN_ERR(svn_sqlite__bind_text(stmt, 6,
                                    svn_relpath_dirname(local_relpath,
                                                        scratch_pool)));
    }

  old_relpath = svn_dirent_skip_ancestor(wcroot->abspath, old_abspath);
  if (old_relpath == old_abspath)
    return svn_error_createf(SVN_ERR_BAD_FILENAME, svn_sqlite__reset(stmt),
                             _("Invalid conflict file '%s' for '%s'"),
                             svn_dirent_local_style(old_abspath, scratch_pool),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));
  new_relpath = svn_dirent_skip_ancestor(wcroot->abspath, new_abspath);
  if (new_relpath == new_abspath)
    return svn_error_createf(SVN_ERR_BAD_FILENAME, svn_sqlite__reset(stmt),
                             _("Invalid conflict file '%s' for '%s'"),
                             svn_dirent_local_style(new_abspath, scratch_pool),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  if (wrk_abspath)
    {
      wrk_relpath = svn_dirent_skip_ancestor(wcroot->abspath, wrk_abspath);
      if (wrk_relpath == wrk_abspath)
        return svn_error_createf(SVN_ERR_BAD_FILENAME, svn_sqlite__reset(stmt),
                                 _("Invalid conflict file '%s' for '%s'"),
                                 svn_dirent_local_style(wrk_abspath,
                                                        scratch_pool),
                                 svn_dirent_local_style(local_abspath,
                                                        scratch_pool));
    }
  else
    wrk_relpath = NULL;

  SVN_ERR(svn_sqlite__bindf(stmt, "issss", wcroot->wc_id,
                                           local_relpath,
                                           old_relpath,
                                           new_relpath,
                                           wrk_relpath));

  return svn_error_return(svn_sqlite__step_done(stmt));
}


/* Set the conflict marker information on LOCAL_ABSPATH to the specified
   values */
svn_error_t *
svn_wc__db_temp_op_set_property_conflict_marker_file(svn_wc__db_t *db,
                                                     const char *local_abspath,
                                                     const char *prej_abspath,
                                                     apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath, *prej_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t got_row;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                                             local_abspath,
                                             scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  /* This should be handled in a transaction, but we can assume a db locl\
     and this code won't survive until 1.7 */

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_ACTUAL_NODE));
  SVN_ERR(svn_sqlite__bindf(stmt, "is", wcroot->wc_id, local_relpath));

  SVN_ERR(svn_sqlite__step(&got_row, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));

  if (got_row)
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_UPDATE_ACTUAL_PROPERTY_CONFLICTS));
    }
  else if (!prej_abspath)
    return SVN_NO_ERROR;
  else
    {
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_INSERT_ACTUAL_PROPERTY_CONFLICTS));

      if (*local_relpath != '\0')
        SVN_ERR(svn_sqlite__bind_text(stmt, 4,
                                      svn_relpath_dirname(local_relpath,
                                                          scratch_pool)));
    }

  prej_relpath = svn_dirent_skip_ancestor(wcroot->abspath, prej_abspath);
  if (prej_relpath == prej_abspath)
    return svn_error_createf(SVN_ERR_BAD_FILENAME, svn_sqlite__reset(stmt),
                             _("Invalid property reject file '%s' for '%s'"),
                             svn_dirent_local_style(prej_abspath, scratch_pool),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id,
                                         local_relpath,
                                         prej_relpath));

  return svn_error_return(svn_sqlite__step_done(stmt));
}


struct set_new_dir_to_incomplete_baton_t
{
  const char *repos_relpath;
  const char *repos_root_url;
  const char *repos_uuid;
  svn_revnum_t revision;
  svn_depth_t depth;
};


static svn_error_t *
set_new_dir_to_incomplete_txn(void *baton,
                              svn_wc__db_wcroot_t *wcroot,
                              const char *local_relpath,
                              apr_pool_t *scratch_pool)
{
  struct set_new_dir_to_incomplete_baton_t *dtb = baton;
  svn_sqlite__stmt_t *stmt;
  apr_int64_t repos_id;
  const char *parent_relpath = (*local_relpath == '\0')
                                  ? NULL
                                  : svn_relpath_dirname(local_relpath,
                                                        scratch_pool);

  SVN_ERR(create_repos_id(&repos_id, dtb->repos_root_url, dtb->repos_uuid,
                          wcroot->sdb, scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb, STMT_INSERT_NODE));

  SVN_ERR(svn_sqlite__bindf(stmt, "isis" /* 1 - 4 */
                            "isr" "sns", /* 5 - 7, 8, 9(n), 10 */
                            wcroot->wc_id,           /* 1 */
                            local_relpath,           /* 2 */
                            (apr_int64_t)0, /* op_depth == 0; BASE */
                            parent_relpath,          /* 4 */
                            repos_id,
                            dtb->repos_relpath,
                            dtb->revision,
                            "incomplete",            /* 8, presence */
                            "dir"));                 /* 10, kind */

  /* If depth is not unknown: record depth */
  if (dtb->depth >= svn_depth_empty && dtb->depth <= svn_depth_infinity)
    SVN_ERR(svn_sqlite__bind_text(stmt, 9, svn_depth_to_word(dtb->depth)));

  SVN_ERR(svn_sqlite__step_done(stmt));

  if (parent_relpath)
    SVN_ERR(extend_parent_delete(wcroot->sdb, wcroot->wc_id,
                                 local_relpath, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_temp_op_set_new_dir_to_incomplete(svn_wc__db_t *db,
                                             const char *local_abspath,
                                             const char *repos_relpath,
                                             const char *repos_root_url,
                                             const char *repos_uuid,
                                             svn_revnum_t revision,
                                             svn_depth_t depth,
                                             apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct set_new_dir_to_incomplete_baton_t baton;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(revision));
  SVN_ERR_ASSERT(repos_relpath && repos_root_url && repos_uuid);

  baton.repos_relpath = repos_relpath;
  baton.repos_root_url = repos_root_url;
  baton.repos_uuid = repos_uuid;
  baton.revision = revision;
  baton.depth = depth;

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                                db, local_abspath,
                                                scratch_pool, scratch_pool));

  VERIFY_USABLE_WCROOT(wcroot);

  SVN_ERR(flush_entries(wcroot, local_abspath, scratch_pool));

  SVN_ERR(svn_wc__db_with_txn(wcroot, local_relpath,
                              set_new_dir_to_incomplete_txn,
                              &baton, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_info_below_working(svn_boolean_t *have_base,
                              svn_boolean_t *have_work,
                              svn_wc__db_status_t *status,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath, db,
                              local_abspath, scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);
  SVN_ERR(info_below_working(have_base, have_work, status,
                             wcroot, local_relpath, scratch_pool));

  return SVN_NO_ERROR;
}


/* Like svn_wc__db_min_max_revisions(),
 * but accepts a WCROOT/LOCAL_RELPATH pair. */
static svn_error_t *
get_min_max_revisions(svn_revnum_t *min_revision,
                      svn_revnum_t *max_revision,
                      svn_wc__db_wcroot_t *wcroot,
                      const char *local_relpath,
                      svn_boolean_t committed,
                      apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_MIN_MAX_REVISIONS));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                            construct_like_arg(local_relpath, scratch_pool)));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  if (have_row)
    {
        if (committed)
          {
            *min_revision = svn_sqlite__column_revnum(stmt, 2);
            *max_revision = svn_sqlite__column_revnum(stmt, 3);
          }
        else
          {
            *min_revision = svn_sqlite__column_revnum(stmt, 0);
            *max_revision = svn_sqlite__column_revnum(stmt, 1);
          }
    }
  else
    {
        *min_revision = SVN_INVALID_REVNUM;
        *max_revision = SVN_INVALID_REVNUM;
    }

  /* The statement should only return at most one row. */
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  SVN_ERR_ASSERT(! have_row);
  SVN_ERR(svn_sqlite__reset(stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_min_max_revisions(svn_revnum_t *min_revision,
                             svn_revnum_t *max_revision,
                             svn_wc__db_t *db,
                             const char *local_abspath,
                             svn_boolean_t committed,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                                db, local_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  return svn_error_return(get_min_max_revisions(min_revision, max_revision,
                                                wcroot, local_relpath,
                                                committed, scratch_pool));
}


/* Like svn_wc__db_is_sparse_checkout,
 * but accepts a WCROOT/LOCAL_RELPATH pair. */
static svn_error_t *
is_sparse_checkout_internal(svn_boolean_t *is_sparse_checkout,
                            svn_wc__db_wcroot_t *wcroot,
                            const char *local_relpath,
                            apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_SPARSE_NODES));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss",
                            wcroot->wc_id,
                            local_relpath,
                            construct_like_arg(local_relpath,
                                               scratch_pool)));
  /* If this query returns a row, the working copy is sparse. */
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  *is_sparse_checkout = have_row;
  SVN_ERR(svn_sqlite__reset(stmt));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_is_sparse_checkout(svn_boolean_t *is_sparse_checkout,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                                db, local_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  return svn_error_return(is_sparse_checkout_internal(is_sparse_checkout,
                                                      wcroot, local_relpath,
                                                      scratch_pool));
}


/* Like svn_wc__db_has_switched_subtrees(),
 * but accepts a WCROOT/LOCAL_RELPATH pair. */
static svn_error_t *
has_switched_subtrees(svn_boolean_t *is_switched,
                      svn_wc__db_wcroot_t *wcroot,
                      const char *local_relpath,
                      const char *trail_url,
                      apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  const char *wcroot_repos_relpath;
  svn_boolean_t have_row;

  SVN_ERR(read_info(NULL, NULL, NULL, &wcroot_repos_relpath, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL,
                    wcroot, "", scratch_pool, scratch_pool));

  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_SWITCHED_NODES));
  SVN_ERR(svn_sqlite__bindf(stmt, "issss", wcroot->wc_id, local_relpath,
                            construct_like_arg(local_relpath, scratch_pool),
                            construct_like_arg(wcroot_repos_relpath,
                                               scratch_pool),
                            wcroot_repos_relpath[0] == '\0' ?
                              "" : apr_pstrcat(scratch_pool,
                                               wcroot_repos_relpath, "/",
                                               (char *)NULL)));
  /* If this query returns a row, some part of the working copy is switched. */
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  *is_switched = have_row;
  SVN_ERR(svn_sqlite__reset(stmt));

  if (! *is_switched && trail_url != NULL)
    {
      const char *url;

      /* If the trailing part of the URL of the working copy directory
         does not match the given trailing URL then the whole working
         copy is switched. */
      SVN_ERR(read_url(&url, wcroot, local_relpath, scratch_pool,
                       scratch_pool));
      if (! url)
        {
          *is_switched = TRUE;
        }
      else
        {
          apr_size_t len1 = strlen(trail_url);
          apr_size_t len2 = strlen(url);
          if ((len1 > len2) || strcmp(url + len2 - len1, trail_url))
            *is_switched = TRUE;
        }
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_has_switched_subtrees(svn_boolean_t *is_switched,
                                 svn_wc__db_t *db,
                                 const char *local_abspath,
                                 const char *trail_url,
                                 apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                                db, local_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  return svn_error_return(has_switched_subtrees(is_switched, wcroot,
                                                local_relpath, trail_url,
                                                scratch_pool));
}

svn_error_t *
svn_wc__db_get_absent_subtrees(apr_hash_t **absent_subtrees,
                               svn_wc__db_t *db,
                               const char *local_abspath,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));
  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                                db, local_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SELECT_ALL_ABSENT_NODES));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss",
                            wcroot->wc_id,
                            local_relpath,
                            construct_like_arg(local_relpath,
                                               scratch_pool)));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));

  if (have_row)
    *absent_subtrees = apr_hash_make(result_pool);
  else
    *absent_subtrees = NULL;

  while (have_row)
    {
      const char *abs_path =
        svn_dirent_join(wcroot->abspath,
                        svn_sqlite__column_text(stmt, 0, scratch_pool),
                        result_pool);
      apr_hash_set(*absent_subtrees, abs_path, APR_HASH_KEY_STRING, abs_path);
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }

  SVN_ERR(svn_sqlite__reset(stmt));
  return SVN_NO_ERROR;
}

/* Like svn_wc__db_has_local_mods(),
 * but accepts a WCROOT/LOCAL_RELPATH pair.
 * ### This needs a DB as well as a WCROOT/RELPATH pair... */
static svn_error_t *
has_local_mods(svn_boolean_t *is_modified,
               svn_wc__db_wcroot_t *wcroot,
               const char *local_relpath,
               svn_wc__db_t *db,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  /* Check for additions or deletions. */
  SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                    STMT_SUBTREE_HAS_TREE_MODIFICATIONS));
  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                            construct_like_arg(local_relpath, scratch_pool)));
  /* If this query returns a row, the working copy is modified. */
  SVN_ERR(svn_sqlite__step(is_modified, stmt));
  SVN_ERR(svn_sqlite__reset(stmt));

  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  if (! *is_modified)
    {
      /* Check for property modifications. */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_SUBTREE_HAS_PROP_MODIFICATIONS));
      SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                                construct_like_arg(local_relpath,
                                                   scratch_pool)));
      /* If this query returns a row, the working copy is modified. */
      SVN_ERR(svn_sqlite__step(is_modified, stmt));
      SVN_ERR(svn_sqlite__reset(stmt));

      if (cancel_func)
        SVN_ERR(cancel_func(cancel_baton));
    }

  if (! *is_modified)
    {
      apr_pool_t *iterpool = NULL;
      svn_boolean_t have_row;

      /* Check for text modifications. */
      SVN_ERR(svn_sqlite__get_statement(&stmt, wcroot->sdb,
                                        STMT_SELECT_CURRENT_NODES_RECURSIVE));
      SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                                construct_like_arg(local_relpath,
                                                   scratch_pool)));
      SVN_ERR(svn_sqlite__step(&have_row, stmt));
      if (have_row)
        iterpool = svn_pool_create(scratch_pool);
      while (have_row)
        {
          const char *node_abspath;
          svn_wc__db_kind_t node_kind;

          if (cancel_func)
            SVN_ERR(cancel_func(cancel_baton));

          svn_pool_clear(iterpool);

          node_abspath = svn_dirent_join(wcroot->abspath,
                                         svn_sqlite__column_text(stmt, 0,
                                                                 iterpool),
                                         iterpool);
          node_kind = svn_sqlite__column_token(stmt, 1, kind_map);
          if (node_kind == svn_wc__db_kind_file)
            {
              SVN_ERR(svn_wc__internal_file_modified_p(is_modified, NULL,
                                                       NULL, db, node_abspath,
                                                       FALSE, TRUE, iterpool));
              if (*is_modified)
                break;
            }

          SVN_ERR(svn_sqlite__step(&have_row, stmt));
        }
      if (iterpool)
        svn_pool_destroy(iterpool);

      SVN_ERR(svn_sqlite__reset(stmt));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_has_local_mods(svn_boolean_t *is_modified,
                          svn_wc__db_t *db,
                          const char *local_abspath,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                                db, local_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  return svn_error_return(has_local_mods(is_modified, wcroot, local_relpath,
                                         db, cancel_func, cancel_baton,
                                         scratch_pool));
}


struct revision_status_baton_t
{
  svn_revnum_t *min_revision;
  svn_revnum_t *max_revision;
  svn_boolean_t *is_sparse_checkout;
  svn_boolean_t *is_modified;
  svn_boolean_t *is_switched;

  const char *trail_url;
  svn_boolean_t committed;
  svn_cancel_func_t cancel_func;
  void *cancel_baton;

  /* We really shouldn't have to have one of these... */
  svn_wc__db_t *db;
};


static svn_error_t *
revision_status_txn(void *baton,
                    svn_wc__db_wcroot_t *wcroot,
                    const char *local_relpath,
                    apr_pool_t *scratch_pool)
{
  struct revision_status_baton_t *rsb = baton;

  /* Determine mixed-revisionness. */
  SVN_ERR(get_min_max_revisions(rsb->min_revision, rsb->max_revision, wcroot,
                                local_relpath, rsb->committed, scratch_pool));

  if (rsb->cancel_func)
    SVN_ERR(rsb->cancel_func(rsb->cancel_baton));

  /* Determine sparseness. */
  SVN_ERR(is_sparse_checkout_internal(rsb->is_sparse_checkout, wcroot,
                                      local_relpath, scratch_pool));

  if (rsb->cancel_func)
    SVN_ERR(rsb->cancel_func(rsb->cancel_baton));

  /* Check for switched nodes. */
  SVN_ERR(has_switched_subtrees(rsb->is_switched, wcroot, local_relpath,
                                rsb->trail_url, scratch_pool));

  if (rsb->cancel_func)
    SVN_ERR(rsb->cancel_func(rsb->cancel_baton));

  /* Check for local mods. */
  SVN_ERR(has_local_mods(rsb->is_modified, wcroot, local_relpath, rsb->db,
                         rsb->cancel_func, rsb->cancel_baton, scratch_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__db_revision_status(svn_revnum_t *min_revision,
                           svn_revnum_t *max_revision,
                           svn_boolean_t *is_sparse_checkout,
                           svn_boolean_t *is_modified,
                           svn_boolean_t *is_switched,
                           svn_wc__db_t *db,
                           const char *local_abspath,
                           const char *trail_url,
                           svn_boolean_t committed,
                           svn_cancel_func_t cancel_func,
                           void *cancel_baton,
                           apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath;
  struct revision_status_baton_t rsb = { min_revision, max_revision,
        is_sparse_checkout, is_modified, is_switched, trail_url, committed,
        cancel_func, cancel_baton, db };

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                                db, local_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  return svn_error_return(svn_wc__db_with_txn(wcroot, local_relpath,
                                              revision_status_txn, &rsb,
                                              scratch_pool));
}


svn_error_t *
svn_wc__db_base_get_lock_tokens_recursive(apr_hash_t **lock_tokens,
                                          svn_wc__db_t *db,
                                          const char *local_abspath,
                                          apr_pool_t *result_pool,
                                          apr_pool_t *scratch_pool)
{
  svn_wc__db_wcroot_t *wcroot;
  const char *local_relpath; 
  svn_sqlite__stmt_t *stmt;
  svn_boolean_t have_row;
  apr_int64_t last_repos_id = INVALID_REPOS_ID;
  const char *last_repos_root_url;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_wc__db_wcroot_parse_local_abspath(&wcroot, &local_relpath,
                                                db, local_abspath,
                                                scratch_pool, scratch_pool));
  VERIFY_USABLE_WCROOT(wcroot);

  *lock_tokens = apr_hash_make(result_pool);

  /* Fetch all the lock tokens in and under LOCAL_RELPATH. */
  SVN_ERR(svn_sqlite__get_statement(
              &stmt, wcroot->sdb,
              STMT_SELECT_BASE_NODE_LOCK_TOKENS_RECURSIVE));

  SVN_ERR(svn_sqlite__bindf(stmt, "iss", wcroot->wc_id, local_relpath,
                            construct_like_arg(local_relpath, scratch_pool)));
  SVN_ERR(svn_sqlite__step(&have_row, stmt));
  while (have_row)
    {
      apr_int64_t child_repos_id = svn_sqlite__column_int64(stmt, 0);
      const char *child_relpath = svn_sqlite__column_text(stmt, 1, NULL);
      const char *lock_token = svn_sqlite__column_text(stmt, 2, result_pool);

      if (child_repos_id != last_repos_id)
        {
          svn_error_t *err = fetch_repos_info(&last_repos_root_url, NULL,
                                              wcroot->sdb, child_repos_id,
                                              scratch_pool);

          if (err)
            {
              return svn_error_return(
                            svn_error_compose_create(err,
                                                     svn_sqlite__reset(stmt)));
            }

          last_repos_id = child_repos_id;
        }

      apr_hash_set(*lock_tokens,
                   svn_path_url_add_component2(last_repos_root_url,
                                               child_relpath,
                                               result_pool),
                   APR_HASH_KEY_STRING,
                   lock_token);

      SVN_ERR(svn_sqlite__step(&have_row, stmt));
    }
  return svn_sqlite__reset(stmt);
}
