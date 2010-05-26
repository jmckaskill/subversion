/*
 * status.c: construct a status structure from an entry structure
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



#include <assert.h>
#include <string.h>

#include <apr_pools.h>
#include <apr_file_io.h>
#include <apr_hash.h>

#include "svn_pools.h"
#include "svn_types.h"
#include "svn_delta.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_io.h"
#include "svn_config.h"
#include "svn_time.h"
#include "svn_hash.h"

#include "svn_private_config.h"

#include "wc.h"
#include "lock.h"
#include "props.h"
#include "entries.h"
#include "translate.h"
#include "tree_conflicts.h"

#include "private/svn_wc_private.h"



/*** Baton used for walking the local status */
struct walk_status_baton
{
  /* The DB handle for managing the working copy state. */
  svn_wc__db_t *db;

  /*** External handling ***/
  /* Target of the status */
  const char *target_abspath;

  /* Externals info harvested during the status run. */
  apr_hash_t *externals;

  /* Externals function/baton */
  svn_wc_external_update_t external_func;
  void *external_baton;

  /*** Repository lock handling ***/
  /* The repository root URL, if set. */
  const char *repos_root;

  /* Repository locks, if set. */
  apr_hash_t *repos_locks;
};

/*** Editor batons ***/

struct edit_baton
{
  /* For status, the "destination" of the edit.  */
  const char *anchor_abspath;
  const char *target_abspath;
  const char *target_basename;

  /* The DB handle for managing the working copy state.  */
  svn_wc__db_t *db;
  svn_wc_context_t *wc_ctx;

  /* The overall depth of this edit (a dir baton may override this).
   *
   * If this is svn_depth_unknown, the depths found in the working
   * copy will govern the edit; or if the edit depth indicates a
   * descent deeper than the found depths are capable of, the found
   * depths also govern, of course (there's no point descending into
   * something that's not there).
   */
  svn_depth_t default_depth;

  /* Do we want all statuses (instead of just the interesting ones) ? */
  svn_boolean_t get_all;

  /* Ignore the svn:ignores. */
  svn_boolean_t no_ignore;

  /* The comparison revision in the repository.  This is a reference
     because this editor returns this rev to the driver directly, as
     well as in each statushash entry. */
  svn_revnum_t *target_revision;

  /* Status function/baton. */
  svn_wc_status_func4_t status_func;
  void *status_baton;

  /* Cancellation function/baton. */
  svn_cancel_func_t cancel_func;
  void *cancel_baton;

  /* The configured set of default ignores. */
  const apr_array_header_t *ignores;

  /* Status item for the path represented by the anchor of the edit. */
  svn_wc_status3_t *anchor_status;

  /* Was open_root() called for this edit drive? */
  svn_boolean_t root_opened;

  /* The local status baton */
  struct walk_status_baton wb;
};


struct dir_baton
{
  /* The path to this directory. */
  const char *local_abspath;

  /* Basename of this directory. */
  const char *name;

  /* The global edit baton. */
  struct edit_baton *edit_baton;

  /* Baton for this directory's parent, or NULL if this is the root
     directory. */
  struct dir_baton *parent_baton;

  /* The ambient requested depth below this point in the edit.  This
     can differ from the parent baton's depth (with the edit baton
     considered the ultimate parent baton).  For example, if the
     parent baton has svn_depth_immediates, then here we should have
     svn_depth_empty, because there would be no further recursion, not
     even to file children. */
  svn_depth_t depth;

  /* Is this directory filtered out due to depth?  (Note that if this
     is TRUE, the depth field is undefined.) */
  svn_boolean_t excluded;

  /* 'svn status' shouldn't print status lines for things that are
     added;  we're only interest in asking if objects that the user
     *already* has are up-to-date or not.  Thus if this flag is set,
     the next two will be ignored.  :-)  */
  svn_boolean_t added;

  /* Gets set iff there's a change to this directory's properties, to
     guide us when syncing adm files later. */
  svn_boolean_t prop_changed;

  /* This means (in terms of 'svn status') that some child was deleted
     or added to the directory */
  svn_boolean_t text_changed;

  /* Working copy status structures for children of this directory.
     This hash maps const char * abspaths  to svn_wc_status3_t *
     status items. */
  apr_hash_t *statii;

  /* The pool in which this baton itself is allocated. */
  apr_pool_t *pool;

  /* The URI to this item in the repository. */
  const char *url;

  /* out-of-date info corresponding to ood_* fields in svn_wc_status3_t. */
  svn_revnum_t ood_last_cmt_rev;
  apr_time_t ood_last_cmt_date;
  svn_node_kind_t ood_kind;
  const char *ood_last_cmt_author;
};


struct file_baton
{
/* Absolute local path to this file */
  const char *local_abspath;

  /* The global edit baton. */
  struct edit_baton *edit_baton;

  /* Baton for this file's parent directory. */
  struct dir_baton *dir_baton;

  /* Pool specific to this file_baton. */
  apr_pool_t *pool;

  /* Basename of this file */
  const char *name;

  /* 'svn status' shouldn't print status lines for things that are
     added;  we're only interest in asking if objects that the user
     *already* has are up-to-date or not.  Thus if this flag is set,
     the next two will be ignored.  :-)  */
  svn_boolean_t added;

  /* This gets set if the file underwent a text change, which guides
     the code that syncs up the adm dir and working copy. */
  svn_boolean_t text_changed;

  /* This gets set if the file underwent a prop change, which guides
     the code that syncs up the adm dir and working copy. */
  svn_boolean_t prop_changed;

  /* The URI to this item in the repository. */
  const char *url;

  /* out-of-date info corresponding to ood_* fields in svn_wc_status3_t. */
  svn_revnum_t ood_last_cmt_rev;
  apr_time_t ood_last_cmt_date;
  svn_node_kind_t ood_kind;
  const char *ood_last_cmt_author;
};


/** Code **/
static svn_error_t *
internal_status(svn_wc_status3_t **status,
                svn_wc__db_t *db,
                const char *local_abspath,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool);

/* Fill in *STATUS for LOCAL_ABSPATH, whose entry data is in ENTRY.  Allocate
   *STATUS in POOL.  LOCAL_ABSPATH must be absolute.  Use SCRATCH_POOL for
   temporary allocations.

   PARENT_ENTRY is the entry for the parent directory of LOCAL_ABSPATH, it
   may be NULL if LOCAL_ABSPATH is a working copy root.
   The lifetime of PARENT_ENTRY's pool is not important.

   PATH_KIND is the node kind of LOCAL_ABSPATH as determined by the caller.
   PATH_SPECIAL indicates whether the entry is a special file.

   If GET_ALL is zero, and ENTRY is not locally modified, then *STATUS
   will be set to NULL.  If GET_ALL is non-zero, then *STATUS will be
   allocated and returned no matter what.

   If IS_IGNORED is non-zero and this is a non-versioned entity, set
   the text_status to svn_wc_status_none.  Otherwise set the
   text_status to svn_wc_status_unversioned.

   The status struct's repos_lock field will be set to REPOS_LOCK.
*/
static svn_error_t *
assemble_status(svn_wc_status3_t **status,
                svn_wc__db_t *db,
                const char *local_abspath,
                const svn_wc_entry_t *entry,
                const char *parent_repos_root_url,
                const char *parent_repos_relpath,
                svn_node_kind_t path_kind,
                svn_boolean_t path_special,
                svn_boolean_t get_all,
                svn_boolean_t is_ignored,
                const svn_lock_t *repos_lock,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  svn_wc_status3_t *stat;
  svn_wc__db_status_t db_status;
  svn_wc__db_kind_t db_kind;
  const char *repos_relpath;
  const char *repos_root_url;
  const char *url;
  svn_boolean_t locked_p = FALSE;
  svn_boolean_t switched_p = FALSE;
  const svn_wc_conflict_description2_t *tree_conflict;
  svn_boolean_t file_external_p = FALSE;
  svn_wc__db_lock_t *lock;
  svn_revnum_t revision;
  svn_revnum_t changed_rev;
  const char *changed_author;
  apr_time_t changed_date;
  const char *changelist;
  svn_boolean_t base_shadowed;
  svn_boolean_t conflicted;

  /* Defaults for two main variables. */
  enum svn_wc_status_kind final_text_status = svn_wc_status_normal;
  enum svn_wc_status_kind final_prop_status = svn_wc_status_none;
  /* And some intermediate results */
  enum svn_wc_status_kind pristine_text_status = svn_wc_status_none;
  enum svn_wc_status_kind pristine_prop_status = svn_wc_status_none;

  SVN_ERR_ASSERT(entry != NULL);

  /* Find out whether the path is a tree conflict victim.
   * This function will set tree_conflict to NULL if the path
   * is not a victim. */
  SVN_ERR(svn_wc__db_op_read_tree_conflict(&tree_conflict, db, local_abspath,
                                           scratch_pool, scratch_pool));

  SVN_ERR(svn_wc__db_read_info(&db_status, &db_kind, &revision,
                               &repos_relpath, &repos_root_url, NULL,
                               &changed_rev, &changed_date,
                               &changed_author, NULL, NULL, NULL, NULL,
                               NULL, &changelist, NULL, NULL, NULL, NULL,
                               NULL, NULL, &base_shadowed, &conflicted,
                               &lock, db, local_abspath, result_pool,
                               scratch_pool));

  /* ### Temporary until we've revved svn_wc_status3_t to only use
   * ### repos_{root_url,relpath} */
  SVN_ERR(svn_wc__internal_node_get_url(&url, db, local_abspath,
                                        result_pool, scratch_pool));

  SVN_ERR(svn_wc__internal_is_file_external(&file_external_p, db,
                                            local_abspath, scratch_pool));

  /** File externals are switched files, but they are not shown as
      such.  To be switched it must have both an URL and a parent with
      an URL, at the very least. */
  if (! file_external_p)
    {
      if (parent_repos_root_url && repos_root_url &&
          (strcmp(parent_repos_root_url, repos_root_url) == 0))
        {
          const char *base = svn_dirent_basename(local_abspath, scratch_pool);

          if (! repos_relpath)
            {
              repos_relpath = svn_relpath_join( parent_repos_relpath, base,
                                                result_pool);
              /* If _read_info() doesn't give us a repos_relpath, it means
               * that it is implied by the parent, thus the path can not be
               * switched. */
              switched_p = FALSE;
            }
          else
            {
              switched_p = (strcmp(svn_relpath_join(parent_repos_relpath, base,
                                                    scratch_pool),
                                   repos_relpath) != 0);
            }
        }
    }

  /* Examine whether our directory metadata is present, and compensate
     if it is missing.

     There are a several kinds of obstruction that we detect here:

     - versioned subdir is missing
     - the versioned subdir's admin area is missing
     - the versioned subdir has been replaced with a file/symlink

     Net result: the target is obstructed and the metadata is unavailable.

     Note: wc_db can also detect a versioned file that has been replaced
     with a versioned subdir (moved from somewhere). We don't look for
     that right away because the file's metadata is still present, so we
     can examine properties and conflicts and whatnot.

     ### note that most obstruction concepts disappear in single-db mode
  */
  if (db_kind == svn_wc__db_kind_dir)
    {
      if (db_status == svn_wc__db_status_incomplete)
        {
          /* Highest precedence.  */
          final_text_status = svn_wc_status_incomplete;
        }
      else if (db_status == svn_wc__db_status_obstructed_delete)
        {
          /* Deleted directories are never reported as missing.  */
          if (path_kind == svn_node_none)
            final_text_status = svn_wc_status_deleted;
          else
            final_text_status = svn_wc_status_obstructed;
        }
      else if (db_status == svn_wc__db_status_obstructed
               || db_status == svn_wc__db_status_obstructed_add)
        {
          /* A present or added directory should be on disk, so it is
             reported missing or obstructed.  */
          if (path_kind == svn_node_none)
            final_text_status = svn_wc_status_missing;
          else
            final_text_status = svn_wc_status_obstructed;
        }
    }

  /* If FINAL_TEXT_STATUS is still normal, after the above checks, then
     we should proceed to refine the status.

     If it was changed, then the subdir is incomplete or missing/obstructed.
     It means that no further information is available, and we should skip
     all this work.  */
  if (final_text_status == svn_wc_status_normal)
    {
      svn_boolean_t has_props;
      svn_boolean_t prop_modified_p = FALSE;
      svn_boolean_t text_modified_p = FALSE;
#ifdef HAVE_SYMLINK
      svn_boolean_t wc_special;
#endif /* HAVE_SYMLINK */

      /* Implement predecence rules: */

      /* 1. Set the two main variables to "discovered" values first (M, C).
            Together, these two stati are of lowest precedence, and C has
            precedence over M. */

      /* Does the entry have props? */
      {
        apr_hash_t *pristine;
        apr_hash_t *actual;

        SVN_ERR(svn_wc__get_pristine_props(&pristine, db, local_abspath,
                                           scratch_pool, scratch_pool));
        SVN_ERR(svn_wc__get_actual_props(&actual, db, local_abspath,
                                         scratch_pool, scratch_pool));
        has_props = ((pristine != NULL && apr_hash_count(pristine) > 0)
                     || (actual != NULL && apr_hash_count(actual) > 0));
      }
      if (has_props)
        final_prop_status = svn_wc_status_normal;

      /* If the entry has a property file, see if it has local changes. */
      /* ### we could compute this ourself, based on the prop hashes
         ### fetched above. but for now, there is some trickery we may
         ### need to rely upon in ths function. keep it for now.  */
      /* ### see r944980 as an example of the brittleness of this stuff.  */
      SVN_ERR(svn_wc__props_modified(&prop_modified_p, db, local_abspath,
                                     scratch_pool));

      /* Record actual property status */
      pristine_prop_status = prop_modified_p ? svn_wc_status_modified
                                             : svn_wc_status_normal;

#ifdef HAVE_SYMLINK
      if (has_props)
        SVN_ERR(svn_wc__get_special(&wc_special, db, local_abspath,
                                    scratch_pool));
      else
        wc_special = FALSE;
#endif /* HAVE_SYMLINK */

      /* If the entry is a file, check for textual modifications */
      if ((db_kind == svn_wc__db_kind_file)
#ifdef HAVE_SYMLINK
          && (wc_special == path_special)
#endif /* HAVE_SYMLINK */
          )
        {
          svn_error_t *err = svn_wc__internal_text_modified_p(&text_modified_p,
                                                              db,
                                                              local_abspath,
                                                              FALSE, TRUE,
                                                              scratch_pool);

          if (err)
            {
              if (!APR_STATUS_IS_EACCES(err->apr_err))
                return svn_error_return(err);

              /* An access denied is very common on Windows when another
                 application has the file open.  Previously we ignored
                 this error in svn_wc__text_modified_internal_p, where it
                 should have really errored. */
              svn_error_clear(err);
            }
          else
            {
              /* Record actual text status */
              pristine_text_status = text_modified_p ? svn_wc_status_modified
                                                     : svn_wc_status_normal;
            }
        }

      if (text_modified_p)
        final_text_status = svn_wc_status_modified;

      if (prop_modified_p)
        final_prop_status = svn_wc_status_modified;

      if (entry->prejfile || entry->conflict_old ||
          entry->conflict_new || entry->conflict_wrk)
        {
          svn_boolean_t text_conflict_p, prop_conflict_p;

          /* The entry says there was a conflict, but the user might have
             marked it as resolved by deleting the artifact files, so check
             for that. */
            SVN_ERR(svn_wc__internal_conflicted_p(&text_conflict_p,
                                                  &prop_conflict_p, NULL, db,
                                                  local_abspath,
                                                  scratch_pool));

          if (text_conflict_p)
            final_text_status = svn_wc_status_conflicted;
          if (prop_conflict_p)
            final_prop_status = svn_wc_status_conflicted;
        }

      /* 2. Possibly overwrite the text_status variable with "scheduled"
            states from the entry (A, D, R).  As a group, these states are
            of medium precedence.  They also override any C or M that may
            be in the prop_status field at this point, although they do not
            override a C text status.*/

      /* ### db_status, base_shadowed, and fetching base_status can
         ### fully replace entry->schedule here.  */

      if (entry->schedule == svn_wc_schedule_add
          && final_text_status != svn_wc_status_conflicted)
        {
          final_text_status = svn_wc_status_added;
          final_prop_status = svn_wc_status_none;
        }

      else if (entry->schedule == svn_wc_schedule_replace
               && final_text_status != svn_wc_status_conflicted)
        {
          final_text_status = svn_wc_status_replaced;
          final_prop_status = svn_wc_status_none;
        }

      else if (entry->schedule == svn_wc_schedule_delete
               && final_text_status != svn_wc_status_conflicted)
        {
          final_text_status = svn_wc_status_deleted;
          final_prop_status = svn_wc_status_none;
        }


      /* 3. Highest precedence:

            a. check to see if file or dir is just missing, or
               incomplete.  This overrides every possible state
               *except* deletion.  (If something is deleted or
               scheduled for it, we don't care if the working file
               exists.)

            b. check to see if the file or dir is present in the
               file system as the same kind it was versioned as.

         4. Check for locked directory (only for directories). */

      if (entry->incomplete
          && (final_text_status != svn_wc_status_deleted)
          && (final_text_status != svn_wc_status_added))
        {
          final_text_status = svn_wc_status_incomplete;
        }
      else if (path_kind == svn_node_none)
        {
          if (final_text_status != svn_wc_status_deleted)
            final_text_status = svn_wc_status_missing;
        }
      /* ### We can do this db_kind to node_kind translation since the cases
       * where db_kind would have been unknown are treated as unversioned
       * paths and thus have already returned. */
      else if (path_kind != (db_kind == svn_wc__db_kind_dir ?  
                                        svn_node_dir : svn_node_file))
        final_text_status = svn_wc_status_obstructed;
#ifdef HAVE_SYMLINK
      else if ( wc_special != path_special)
        final_text_status = svn_wc_status_obstructed;
#endif /* HAVE_SYMLINK */

      if (path_kind == svn_node_dir && db_kind == svn_wc__db_kind_dir)
        SVN_ERR(svn_wc__db_wclocked(&locked_p, db, local_abspath,
                                    scratch_pool));
    }

  /* 5. Easy out:  unless we're fetching -every- entry, don't bother
     to allocate a struct for an uninteresting entry. */

  if (! get_all)
    if (((final_text_status == svn_wc_status_none)
         || (final_text_status == svn_wc_status_normal))
        && ((final_prop_status == svn_wc_status_none)
            || (final_prop_status == svn_wc_status_normal))
        && (! locked_p)
        && (! switched_p)
        && (! file_external_p)
        && (! lock) 
        && (! repos_lock)
        && (! changelist)
        && (! tree_conflict))
      {
        *status = NULL;
        return SVN_NO_ERROR;
      }


  /* 6. Build and return a status structure. */

  stat = apr_pcalloc(result_pool, sizeof(**status));
  stat->entry = svn_wc_entry_dup(entry, result_pool);
  stat->text_status = final_text_status;
  stat->prop_status = final_prop_status;
  stat->repos_text_status = svn_wc_status_none;   /* default */
  stat->repos_prop_status = svn_wc_status_none;   /* default */
  stat->locked = locked_p;
  stat->switched = switched_p;
  stat->file_external = file_external_p;
  stat->copied = entry->copied;
  stat->repos_lock = repos_lock;
  stat->url = url;
  stat->revision = revision;
  stat->changed_rev = changed_rev;
  stat->changed_author = changed_author;
  stat->changed_date = changed_date;
  stat->ood_last_cmt_rev = SVN_INVALID_REVNUM;
  stat->ood_last_cmt_date = 0;
  stat->ood_kind = svn_node_none;
  stat->ood_last_cmt_author = NULL;
  stat->pristine_text_status = pristine_text_status;
  stat->pristine_prop_status = pristine_prop_status;
  stat->lock_token = lock ? lock->token : NULL;
  stat->lock_owner = lock ? lock->owner : NULL;
  stat->lock_comment = lock ? lock->comment : NULL;
  stat->lock_creation_date = lock ? lock->date : 0;
  stat->conflicted = conflicted;
  stat->versioned = TRUE;
  stat->changelist = changelist;
  stat->repos_root_url = repos_root_url;
  stat->repos_relpath = repos_relpath;

  *status = stat;

  return SVN_NO_ERROR;
}


static svn_error_t *
assemble_unversioned(svn_wc_status3_t **status,
                     svn_wc__db_t *db,
                     const char *local_abspath,
                     svn_node_kind_t path_kind,
                     svn_boolean_t is_ignored,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool)
{
  svn_wc_status3_t *stat;
  const svn_wc_conflict_description2_t *tree_conflict;

  /* Find out whether the path is a tree conflict victim.
     This function will set tree_conflict to NULL if the path
     is not a victim. */
  SVN_ERR(svn_wc__db_op_read_tree_conflict(&tree_conflict,
                                           db, local_abspath,
                                           scratch_pool, scratch_pool));

  /* return a fairly blank structure. */
  stat = apr_pcalloc(result_pool, sizeof(**status));
  stat->text_status = svn_wc_status_none;
  stat->prop_status = svn_wc_status_none;
  stat->repos_text_status = svn_wc_status_none;
  stat->repos_prop_status = svn_wc_status_none;

  /* If this path has no entry, but IS present on disk, it's
     unversioned.  If this file is being explicitly ignored (due
     to matching an ignore-pattern), the text_status is set to
     svn_wc_status_ignored.  Otherwise the text_status is set to
     svn_wc_status_unversioned. */
  if (path_kind != svn_node_none)
    {
      if (is_ignored)
        stat->text_status = svn_wc_status_ignored;
      else
        stat->text_status = svn_wc_status_unversioned;
    }
  else if (tree_conflict != NULL)
    {
      /* If this path has no entry, is NOT present on disk, and IS a
         tree conflict victim, count it as missing. */
      stat->text_status = svn_wc_status_missing;
    }

  stat->revision = SVN_INVALID_REVNUM;
  stat->changed_rev = SVN_INVALID_REVNUM;
  stat->ood_last_cmt_rev = SVN_INVALID_REVNUM;
  stat->ood_kind = svn_node_none;

  /* For the case of an incoming delete to a locally deleted path during
     an update, we get a tree conflict. */
  stat->conflicted = (tree_conflict != NULL);
  stat->changelist = NULL;

  *status = stat;
  return SVN_NO_ERROR;
}


/* Given an ENTRY object representing PATH, build a status structure
   and pass it off to the STATUS_FUNC/STATUS_BATON.  All other
   arguments are the same as those passed to assemble_status().  */
static svn_error_t *
send_status_structure(const struct walk_status_baton *wb,
                      const char *local_abspath,
                      const svn_wc_entry_t *entry,
                      const char *parent_repos_root_url,
                      const char *parent_repos_relpath,
                      svn_node_kind_t path_kind,
                      svn_boolean_t path_special,
                      svn_boolean_t get_all,
                      svn_boolean_t is_ignored,
                      svn_wc_status_func4_t status_func,
                      void *status_baton,
                      apr_pool_t *scratch_pool)
{
  svn_wc_status3_t *statstruct;
  const svn_lock_t *repos_lock = NULL;

  SVN_ERR_ASSERT(entry != NULL);

  /* Check for a repository lock. */
  if (wb->repos_locks)
    {
      const char *repos_relpath;
      svn_wc__db_status_t status;
      svn_boolean_t base_shadowed;

      SVN_ERR(svn_wc__db_read_info(&status, NULL, NULL, &repos_relpath, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, &base_shadowed, NULL, NULL, 
                                   wb->db, local_abspath,
                                   scratch_pool, scratch_pool));

      /* A switched path can be deleted: check the right relpath */
      if (status == svn_wc__db_status_deleted && base_shadowed)
        SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath, NULL,
                                           NULL, wb->db, local_abspath,
                                           scratch_pool, scratch_pool));

      if (!repos_relpath && parent_repos_relpath)
        repos_relpath = svn_relpath_join(parent_repos_relpath,
                                         svn_dirent_basename(local_abspath,
                                                             NULL),
                                         scratch_pool);


      if (repos_relpath)
        {
          /* repos_lock still uses the deprecated filesystem absolute path
             format */
          repos_lock = apr_hash_get(wb->repos_locks,
                                    svn_uri_join("/", repos_relpath,
                                                 scratch_pool),
                                    APR_HASH_KEY_STRING);
        }
    }

  SVN_ERR(assemble_status(&statstruct, wb->db, local_abspath, entry,
                          parent_repos_root_url, parent_repos_relpath, 
                          path_kind, path_special, get_all, is_ignored,
                          repos_lock, scratch_pool, scratch_pool));

  if (statstruct && status_func)
    return svn_error_return((*status_func)(status_baton, local_abspath,
                                           statstruct, scratch_pool));

  return SVN_NO_ERROR;
}


/* Store in PATTERNS a list of all svn:ignore properties from
   the working copy directory, including the default ignores
   passed in as IGNORES.

   Upon return, *PATTERNS will contain zero or more (const char *)
   patterns from the value of the SVN_PROP_IGNORE property set on
   the working directory path.

   IGNORES is a list of patterns to include; typically this will
   be the default ignores as, for example, specified in a config file.

   LOCAL_ABSPATH and DB control how to access the ignore information.

   Allocate results in RESULT_POOL, temporary stuffs in SCRATCH_POOL.

   None of the arguments may be NULL.
*/
static svn_error_t *
collect_ignore_patterns(apr_array_header_t **patterns,
                        svn_wc__db_t *db,
                        const char *local_abspath,
                        const apr_array_header_t *ignores,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool)
{
  int i;
  const svn_string_t *value;

  /* ### assert we are passed a directory? */

  *patterns = apr_array_make(result_pool, 1, sizeof(const char *));

  /* Copy default ignores into the local PATTERNS array. */
  for (i = 0; i < ignores->nelts; i++)
    {
      const char *ignore = APR_ARRAY_IDX(ignores, i, const char *);
      APR_ARRAY_PUSH(*patterns, const char *) = apr_pstrdup(result_pool,
                                                            ignore);
    }

  /* Then add any svn:ignore globs to the PATTERNS array. */
  SVN_ERR(svn_wc__internal_propget(&value, db, local_abspath, SVN_PROP_IGNORE,
                                   scratch_pool, scratch_pool));
  if (value != NULL)
    svn_cstring_split_append(*patterns, value->data, "\n\r", FALSE,
                             result_pool);

  return SVN_NO_ERROR;
}


/* Compare LOCAL_ABSPATH with items in the EXTERNALS hash to see if
   LOCAL_ABSPATH is the drop location for, or an intermediate directory
   of the drop location for, an externals definition.  Use SCRATCH_POOL
   for scratchwork.  */
static svn_boolean_t
is_external_path(apr_hash_t *externals,
                 const char *local_abspath,
                 apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;

  /* First try: does the path exist as a key in the hash? */
  if (apr_hash_get(externals, local_abspath, APR_HASH_KEY_STRING))
    return TRUE;

  /* Failing that, we need to check if any external is a child of
     LOCAL_ABSPATH.  */
  for (hi = apr_hash_first(scratch_pool, externals);
       hi;
       hi = apr_hash_next(hi))
    {
      const char *external_abspath = svn__apr_hash_index_key(hi);

      if (svn_dirent_is_child(local_abspath, external_abspath, NULL))
        return TRUE;
    }

  return FALSE;
}


/* Assuming that LOCAL_ABSPATH is unversioned, send a status structure
   for it through STATUS_FUNC/STATUS_BATON unless this path is being
   ignored.  This function should never be called on a versioned entry.

   LOCAL_ABSPATH is the path to the unversioned file whose status is being
   requested.  PATH_KIND is the node kind of NAME as determined by the
   caller.  PATH_SPECIAL is the special status of the path, also determined
   by the caller.
   PATTERNS points to a list of filename patterns which are marked as
   ignored.  None of these parameter may be NULL.  EXTERNALS is a hash
   of known externals definitions for this status run.

   If NO_IGNORE is non-zero, the item will be added regardless of
   whether it is ignored; otherwise we will only add the item if it
   does not match any of the patterns in PATTERNS.

   Allocate everything in POOL.
*/
static svn_error_t *
send_unversioned_item(const struct walk_status_baton *wb,
                      const char *local_abspath,
                      svn_node_kind_t path_kind,
                      const apr_array_header_t *patterns,
                      svn_boolean_t no_ignore,
                      svn_wc_status_func4_t status_func,
                      void *status_baton,
                      apr_pool_t *scratch_pool)
{
  svn_boolean_t is_ignored;
  svn_boolean_t is_external;
  svn_wc_status3_t *status;

  is_ignored = svn_wc_match_ignore_list(
                 svn_dirent_basename(local_abspath, NULL),
                 patterns, scratch_pool);

  SVN_ERR(assemble_unversioned(&status,
                               wb->db, local_abspath,
                               path_kind, is_ignored,
                               scratch_pool, scratch_pool));

  is_external = is_external_path(wb->externals, local_abspath, scratch_pool);
  if (is_external)
    status->text_status = svn_wc_status_external;

  /* We can have a tree conflict on an unversioned path, i.e. an incoming
   * delete on a locally deleted path during an update. Don't ever ignore
   * those! */
  if (status->conflicted)
    is_ignored = FALSE;

  /* If we aren't ignoring it, or if it's an externals path, pass this
     entry to the status func. */
  if (no_ignore || (! is_ignored) || is_external)
    return svn_error_return((*status_func)(status_baton, local_abspath,
                                           status, scratch_pool));

  return SVN_NO_ERROR;
}


/* Prototype for untangling a tango-ing two-some. */
static svn_error_t *
get_dir_status(const struct walk_status_baton *wb,
               const char *local_abspath,
               const char *parent_repos_root_url,
               const char *parent_repos_relpath,
               const char *selected,
               const apr_array_header_t *ignores,
               svn_depth_t depth,
               svn_boolean_t get_all,
               svn_boolean_t no_ignore,
               svn_boolean_t skip_this_dir,
               svn_boolean_t get_excluded,
               svn_wc_status_func4_t status_func,
               void *status_baton,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               apr_pool_t *scratch_pool);

/* Handle LOCAL_ABSPATH (whose entry is ENTRY) as a directory entry
   of the directory whose entry is DIR_ENTRY.  All other arguments
   are the same as those passed to get_dir_status(), the function
   for which this one is a helper.  */
static svn_error_t *
handle_dir_entry(const struct walk_status_baton *wb,
                 const char *local_abspath,
                 const svn_wc_entry_t *dir_entry,
                 const svn_wc_entry_t *entry,
                 const char *dir_repos_root_url,
                 const char *dir_repos_relpath,
                 svn_node_kind_t path_kind,
                 svn_boolean_t path_special,
                 const apr_array_header_t *ignores,
                 svn_depth_t depth,
                 svn_boolean_t get_all,
                 svn_boolean_t no_ignore,
                 svn_boolean_t get_excluded,
                 svn_wc_status_func4_t status_func,
                 void *status_baton,
                 svn_cancel_func_t cancel_func,
                 void *cancel_baton,
                 apr_pool_t *pool)
{
  SVN_ERR_ASSERT(dir_entry != NULL);
  SVN_ERR_ASSERT(entry != NULL);

  /* We are looking at a directory on-disk.  */
  if (path_kind == svn_node_dir)
    {
      /* Descend only if the subdirectory is a working copy directory (which
         we've discovered because we got a THIS_DIR entry. And only descend
         if DEPTH permits it, of course.  */
      if (*entry->name == '\0'
          && (depth == svn_depth_unknown
              || depth == svn_depth_immediates
              || depth == svn_depth_infinity))
        {
          SVN_ERR(get_dir_status(wb, local_abspath, dir_repos_root_url,
                                 dir_repos_relpath, NULL, ignores, depth,
                                 get_all, no_ignore, FALSE, get_excluded,
                                 status_func, status_baton, cancel_func,
                                 cancel_baton,
                                 pool));
        }
      else
        {
          /* ENTRY is a child entry (file or parent stub). Or we have a
             directory entry but DEPTH is limiting our recursion.  */
          SVN_ERR(send_status_structure(wb, local_abspath, entry,
                                        dir_repos_root_url,
                                        dir_repos_relpath, svn_node_dir,
                                        FALSE /* path_special */,
                                        get_all, FALSE /* is_ignored */,
                                        status_func, status_baton, pool));
        }
    }
  else
    {
      /* This is a file/symlink on-disk.  */
      SVN_ERR(send_status_structure(wb, local_abspath, entry,
                                    dir_repos_root_url,
                                    dir_repos_relpath, path_kind,
                                    path_special, get_all, 
                                    FALSE /* is_ignored */,
                                    status_func, status_baton, pool));
    }

  return SVN_NO_ERROR;
}


/* Helper for get_dir_status. If LOCAL_ABSPATH has "svn:externals" property
   set on it, send the name and value to WB->external_func, along with
   this directory's depth, but skip this step if LOCAL_ABSPATH is the anchor
   of a specific target.  (Also, we want to track the externals internally
   so we can report status more accurately.) */
static svn_error_t *
handle_externals(const struct walk_status_baton *wb,
                 const char *local_abspath,
                 svn_depth_t depth,
                 apr_pool_t *scratch_pool)
{
  const svn_string_t *prop_val;

  SVN_ERR(svn_wc__internal_propget(&prop_val, wb->db, local_abspath,
                                   SVN_PROP_EXTERNALS, scratch_pool,
                                   scratch_pool));
  if (prop_val)
    {
      apr_pool_t *hash_pool = apr_hash_pool_get(wb->externals);
      apr_array_header_t *ext_items;
      int i;

      if (wb->external_func &&
          svn_dirent_is_ancestor(wb->target_abspath, local_abspath))
        {
          SVN_ERR((wb->external_func)(wb->external_baton, local_abspath,
                                      prop_val, prop_val, depth,
                                      scratch_pool));
        }

      /* Now, parse the thing, and copy the parsed results into
         our "global" externals hash. */
      SVN_ERR(svn_wc_parse_externals_description3(&ext_items, local_abspath,
                                                  prop_val->data, FALSE,
                                                  scratch_pool));
      for (i = 0; ext_items && i < ext_items->nelts; i++)
        {
          const svn_wc_external_item2_t *item;

          item = APR_ARRAY_IDX(ext_items, i, const svn_wc_external_item2_t *);
          apr_hash_set(wb->externals, svn_dirent_join(local_abspath,
                                                      item->target_dir,
                                                      hash_pool),
                       APR_HASH_KEY_STRING, "");
        }
    }

  return SVN_NO_ERROR;
}


/* Send svn_wc_status3_t * structures for the directory LOCAL_ABSPATH and
   for all its entries through STATUS_FUNC/STATUS_BATON, or, if SELECTED
   is non-NULL, only for that directory entry.

   PARENT_ENTRY is the entry for the parent of the directory or NULL
   if LOCAL_ABSPATH is a working copy root.

   If SKIP_THIS_DIR is TRUE (and SELECTED is NULL), the directory's own
   status will not be reported.  However, upon recursing, all subdirs
   *will* be reported, regardless of this parameter's value.

   If GET_EXCLUDED is TRUE, then statuses for the roots of excluded
   subtrees are reported, otherwise they are ignored.

   Other arguments are the same as those passed to
   svn_wc_get_status_editor5().  */
static svn_error_t *
get_dir_status(const struct walk_status_baton *wb,
               const char *local_abspath,
               const char *parent_repos_root_url,
               const char *parent_repos_relpath,
               const char *selected,
               const apr_array_header_t *ignore_patterns,
               svn_depth_t depth,
               svn_boolean_t get_all,
               svn_boolean_t no_ignore,
               svn_boolean_t skip_this_dir,
               svn_boolean_t get_excluded,
               svn_wc_status_func4_t status_func,
               void *status_baton,
               svn_cancel_func_t cancel_func,
               void *cancel_baton,
               apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;
  const svn_wc_entry_t *dir_entry;
  const char *dir_repos_root_url;
  const char *dir_repos_relpath;
  apr_hash_t *dirents, *nodes, *conflicts, *all_children;
  apr_array_header_t *patterns = NULL;
  svn_wc__db_status_t dir_status;
  apr_pool_t *iterpool, *subpool = svn_pool_create(scratch_pool);

  /* See if someone wants to cancel this operation. */
  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  if (depth == svn_depth_unknown)
    depth = svn_depth_infinity;

  /* Make our iteration pool. */
  iterpool = svn_pool_create(subpool);

  /* Load list of childnodes. */
  {
    const apr_array_header_t *child_nodes;

    SVN_ERR(svn_wc__db_read_children(&child_nodes, wb->db, local_abspath,
                                     iterpool, iterpool));
    SVN_ERR(svn_hash_from_cstring_keys(&nodes, child_nodes, subpool));
  }

  SVN_ERR(svn_io_get_dirents2(&dirents, local_abspath, subpool));
  /* Get this directory's entry. */
  SVN_ERR(svn_wc__get_entry(&dir_entry, wb->db, local_abspath, FALSE,
                            svn_node_dir, FALSE, subpool, iterpool));

  SVN_ERR(svn_wc__db_read_info(&dir_status, NULL, NULL, &dir_repos_relpath,
                               &dir_repos_root_url, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, wb->db, local_abspath, scratch_pool,
                               scratch_pool));

  if (dir_repos_relpath == NULL)
    {
      if (parent_repos_root_url != NULL)
        {
          dir_repos_root_url = parent_repos_root_url;
          dir_repos_relpath = svn_relpath_join(
                                    parent_repos_relpath,
                                    svn_dirent_basename(local_abspath, NULL),
                                    scratch_pool);
        }
      else if (dir_status != svn_wc__db_status_deleted
               && dir_status != svn_wc__db_status_added)
        SVN_ERR(svn_wc__db_scan_base_repos(&dir_repos_relpath,
                                           &dir_repos_root_url,
                                           NULL, wb->db, local_abspath,
                                           scratch_pool, scratch_pool));
      else
        {
          dir_repos_relpath = NULL;
          dir_repos_root_url = NULL;
        }
    }

  if (selected == NULL)
    {
      const apr_array_header_t *victims;
      /* Create a hash containing all children */
      all_children = apr_hash_overlay(subpool, nodes, dirents);

      SVN_ERR(svn_wc__db_read_conflict_victims(&victims,
                                               wb->db, local_abspath,
                                               iterpool, iterpool));

      SVN_ERR(svn_hash_from_cstring_keys(&conflicts, victims, subpool));

      /* Optimize for the no-tree-conflict case */
      if (apr_hash_count(conflicts) > 0)
        all_children = apr_hash_overlay(subpool, conflicts, all_children);
    }
  else
    {
      const svn_wc_conflict_description2_t *tc;
      const char *selected_abspath;

      conflicts = apr_hash_make(subpool);
      all_children = apr_hash_make(subpool);

      apr_hash_set(all_children, selected, APR_HASH_KEY_STRING, selected);

      selected_abspath = svn_dirent_join(local_abspath, selected, iterpool);

      SVN_ERR(svn_wc__db_op_read_tree_conflict(&tc, wb->db, selected_abspath,
                                               iterpool, iterpool));

      /* Note this path if a tree conflict is present.  */
      if (tc != NULL)
        apr_hash_set(conflicts, selected, APR_HASH_KEY_STRING, "");
    }

  /* If "this dir" has "svn:externals" property set on it, send the name and
     value to wc->external_func along with this directory's depth. (Also,
     we want to track the externals internally so we can report status more
     accurately.) */
  SVN_ERR(handle_externals(wb, local_abspath, dir_entry->depth, iterpool));

  if (!selected)
    {
      /* Handle "this-dir" first. */
      if (! skip_this_dir)
        SVN_ERR(send_status_structure(wb, local_abspath,
                                      dir_entry, parent_repos_root_url,
                                      parent_repos_relpath, svn_node_dir,
                                      FALSE /* path_special */,
                                      get_all, FALSE /* is_ignored */,
                                      status_func, status_baton,
                                      iterpool));

      /* If the requested depth is empty, we only need status on this-dir. */
      if (depth == svn_depth_empty)
        return SVN_NO_ERROR;
    }

  /* Add empty status structures for each of the unversioned things.
     This also catches externals; not sure whether that's good or bad,
     but it's what's happening right now. */
  for (hi = apr_hash_first(subpool, all_children); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      apr_ssize_t klen;
      const char *node_abspath;
      svn_io_dirent_t *dirent_p;

      svn_pool_clear(iterpool);

      apr_hash_this(hi, &key, &klen, NULL);

      node_abspath = svn_dirent_join(local_abspath, key, iterpool);

      dirent_p = apr_hash_get(dirents, key, klen);

      if (apr_hash_get(nodes, key, klen))
        {
          /* Versioned node */
          svn_error_t *err;
          const svn_wc_entry_t *entry;
          svn_boolean_t hidden;

          SVN_ERR(svn_wc__db_node_hidden(&hidden, wb->db, node_abspath,
                                         iterpool));

          if (!hidden || get_excluded)
            {
              err = svn_wc__get_entry(&entry, wb->db, node_abspath, FALSE,
                                      svn_node_unknown, FALSE,
                                      iterpool, iterpool);
              if (err)
                {
                  if (err->apr_err == SVN_ERR_NODE_UNEXPECTED_KIND)
                    {
                      /* We asked for the contents, but got the stub.  */
                      svn_error_clear(err);
                    }
                  else if (err && err->apr_err == SVN_ERR_WC_MISSING)
                    {
                      svn_error_clear(err);

                      /* Most likely the parent refers to a missing child;
                       * retrieve the stub stored in the parent */

                      err = svn_wc__get_entry(&entry, wb->db, node_abspath,
                                              FALSE, svn_node_dir, TRUE,
                                              iterpool, iterpool);

                      if (err && err->apr_err == SVN_ERR_NODE_UNEXPECTED_KIND)
                        svn_error_clear(err);
                      else
                        SVN_ERR(err);
                    }
                  else
                    return svn_error_return(err);
                }

              if (depth == svn_depth_files && entry->kind == svn_node_dir)
                continue;

              /* Handle this entry (possibly recursing). */
              SVN_ERR(handle_dir_entry(wb,
                                       node_abspath,
                                       dir_entry,
                                       entry,
                                       dir_repos_root_url,
                                       dir_repos_relpath,
                                       dirent_p ? dirent_p->kind
                                                : svn_node_none,
                                       dirent_p ? dirent_p->special : FALSE,
                                       ignore_patterns,
                                       depth == svn_depth_infinity
                                                           ? depth
                                                           : svn_depth_empty,
                                       get_all,
                                       no_ignore, get_excluded,
                                       status_func, status_baton,
                                       cancel_func, cancel_baton, iterpool));
              continue;
            }
        }

      if (apr_hash_get(conflicts, key, klen))
        {
          /* Tree conflict */

          if (ignore_patterns && ! patterns)
            SVN_ERR(collect_ignore_patterns(&patterns, wb->db, local_abspath,
                                            ignore_patterns, subpool,
                                            iterpool));

          SVN_ERR(send_unversioned_item(wb,
                                        node_abspath,
                                        dirent_p ? dirent_p->kind
                                                 : svn_node_none,
                                        patterns,
                                        no_ignore,
                                        status_func,
                                        status_baton,
                                        iterpool));

          continue;
        }

      /* Unversioned node */
      if (dirent_p == NULL)
        continue; /* Selected node, but not found */

      if (depth == svn_depth_files && dirent_p->kind == svn_node_dir)
        continue;

      if (svn_wc_is_adm_dir(key, iterpool))
        continue;

      if (ignore_patterns && ! patterns)
        SVN_ERR(collect_ignore_patterns(&patterns, wb->db, local_abspath,
                                        ignore_patterns, subpool,
                                        iterpool));

      SVN_ERR(send_unversioned_item(wb,
                                    node_abspath,
                                    dirent_p->kind,
                                    patterns,
                                    no_ignore || selected,
                                    status_func, status_baton,
                                    iterpool));
    }

  /* Destroy our subpools. */
  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}



/*** Helpers ***/

/* A faux status callback function for stashing STATUS item in an hash
   (which is the BATON), keyed on PATH.  This implements the
   svn_wc_status_func4_t interface. */
static svn_error_t *
hash_stash(void *baton,
           const char *path,
           const svn_wc_status3_t *status,
           apr_pool_t *scratch_pool)
{
  apr_hash_t *stat_hash = baton;
  apr_pool_t *hash_pool = apr_hash_pool_get(stat_hash);
  assert(! apr_hash_get(stat_hash, path, APR_HASH_KEY_STRING));
  apr_hash_set(stat_hash, apr_pstrdup(hash_pool, path),
               APR_HASH_KEY_STRING, svn_wc_dup_status3(status, hash_pool));

  return SVN_NO_ERROR;
}


/* Look up the key PATH in BATON->STATII.  IS_DIR_BATON indicates whether
   baton is a struct *dir_baton or struct *file_baton.  If the value doesn't
   yet exist, and the REPOS_TEXT_STATUS indicates that this is an
   addition, create a new status struct using the hash's pool.

   If IS_DIR_BATON is true, THIS_DIR_BATON is a *dir_baton cotaining the out
   of date (ood) information we want to set in BATON.  This is necessary
   because this function tweaks the status of out-of-date directories
   (BATON == THIS_DIR_BATON) and out-of-date directories' parents
   (BATON == THIS_DIR_BATON->parent_baton).  In the latter case THIS_DIR_BATON
   contains the ood info we want to bubble up to ancestor directories so these
   accurately reflect the fact they have an ood descendant.

   Merge REPOS_TEXT_STATUS and REPOS_PROP_STATUS into the status structure's
   "network" fields.

   Iff IS_DIR_BATON is true, DELETED_REV is used as follows, otherwise it
   is ignored:

       If REPOS_TEXT_STATUS is svn_wc_status_deleted then DELETED_REV is
       optionally the revision path was deleted, in all other cases it must
       be set to SVN_INVALID_REVNUM.  If DELETED_REV is not
       SVN_INVALID_REVNUM and REPOS_TEXT_STATUS is svn_wc_status_deleted,
       then use DELETED_REV to set PATH's ood_last_cmt_rev field in BATON.
       If DELETED_REV is SVN_INVALID_REVNUM and REPOS_TEXT_STATUS is
       svn_wc_status_deleted, set PATH's ood_last_cmt_rev to its parent's
       ood_last_cmt_rev value - see comment below.

   If a new struct was added, set the repos_lock to REPOS_LOCK. */
static svn_error_t *
tweak_statushash(void *baton,
                 void *this_dir_baton,
                 svn_boolean_t is_dir_baton,
                 svn_wc__db_t *db,
                 const char *local_abspath,
                 svn_boolean_t is_dir,
                 enum svn_wc_status_kind repos_text_status,
                 enum svn_wc_status_kind repos_prop_status,
                 svn_revnum_t deleted_rev,
                 const svn_lock_t *repos_lock,
                 apr_pool_t *scratch_pool)
{
  svn_wc_status3_t *statstruct;
  apr_pool_t *pool;
  apr_hash_t *statushash;

  if (is_dir_baton)
    statushash = ((struct dir_baton *) baton)->statii;
  else
    statushash = ((struct file_baton *) baton)->dir_baton->statii;
  pool = apr_hash_pool_get(statushash);

  /* Is PATH already a hash-key? */
  statstruct = apr_hash_get(statushash, local_abspath, APR_HASH_KEY_STRING);

  /* If not, make it so. */
  if (! statstruct)
    {
      /* If this item isn't being added, then we're most likely
         dealing with a non-recursive (or at least partially
         non-recursive) working copy.  Due to bugs in how the client
         reports the state of non-recursive working copies, the
         repository can send back responses about paths that don't
         even exist locally.  Our best course here is just to ignore
         those responses.  After all, if the client had reported
         correctly in the first, that path would either be mentioned
         as an 'add' or not mentioned at all, depending on how we
         eventually fix the bugs in non-recursivity.  See issue
         #2122 for details. */
      if (repos_text_status != svn_wc_status_added)
        return SVN_NO_ERROR;

      /* Use the public API to get a statstruct, and put it into the hash. */
      SVN_ERR(internal_status(&statstruct, db, local_abspath, pool,
                              scratch_pool));
      statstruct->repos_lock = repos_lock;
      apr_hash_set(statushash, apr_pstrdup(pool, local_abspath),
                   APR_HASH_KEY_STRING, statstruct);
    }

  /* Merge a repos "delete" + "add" into a single "replace". */
  if ((repos_text_status == svn_wc_status_added)
      && (statstruct->repos_text_status == svn_wc_status_deleted))
    repos_text_status = svn_wc_status_replaced;

  /* Tweak the structure's repos fields. */
  if (repos_text_status)
    statstruct->repos_text_status = repos_text_status;
  if (repos_prop_status)
    statstruct->repos_prop_status = repos_prop_status;

  /* Copy out-of-date info. */
  if (is_dir_baton)
    {
      struct dir_baton *b = this_dir_baton;

      if (b->url)
        {
          if (statstruct->repos_text_status == svn_wc_status_deleted)
            {
              /* When deleting PATH, BATON is for PATH's parent,
                 so we must construct PATH's real statstruct->url. */
              statstruct->url =
                svn_path_url_add_component2(b->url,
                                            svn_dirent_basename(local_abspath,
                                                                NULL),
                                            pool);
            }
          else
            statstruct->url = apr_pstrdup(pool, b->url);
        }

      /* The last committed date, and author for deleted items
         isn't available. */
      if (statstruct->repos_text_status == svn_wc_status_deleted)
        {
          statstruct->ood_kind = is_dir ? svn_node_dir : svn_node_file;

          /* Pre 1.5 servers don't provide the revision a path was deleted.
             So we punt and use the last committed revision of the path's
             parent, which has some chance of being correct.  At worse it
             is a higher revision than the path was deleted, but this is
             better than nothing... */
          if (deleted_rev == SVN_INVALID_REVNUM)
            statstruct->ood_last_cmt_rev =
              ((struct dir_baton *) baton)->ood_last_cmt_rev;
          else
            statstruct->ood_last_cmt_rev = deleted_rev;
        }
      else
        {
          statstruct->ood_kind = b->ood_kind;
          statstruct->ood_last_cmt_rev = b->ood_last_cmt_rev;
          statstruct->ood_last_cmt_date = b->ood_last_cmt_date;
          if (b->ood_last_cmt_author)
            statstruct->ood_last_cmt_author =
              apr_pstrdup(pool, b->ood_last_cmt_author);
        }

    }
  else
    {
      struct file_baton *b = baton;
      if (b->url)
        statstruct->url = apr_pstrdup(pool, b->url);
      statstruct->ood_last_cmt_rev = b->ood_last_cmt_rev;
      statstruct->ood_last_cmt_date = b->ood_last_cmt_date;
      statstruct->ood_kind = b->ood_kind;
      if (b->ood_last_cmt_author)
        statstruct->ood_last_cmt_author =
          apr_pstrdup(pool, b->ood_last_cmt_author);
    }
  return SVN_NO_ERROR;
}

/* Returns the URL for DB, or NULL: */
static const char *
find_dir_url(const struct dir_baton *db, apr_pool_t *pool)
{
  /* If we have no name, we're the root, return the anchor URL. */
  if (! db->name)
    return db->edit_baton->anchor_status->url;
  else
    {
      const char *url;
      struct dir_baton *pb = db->parent_baton;
      const svn_wc_status3_t *status = apr_hash_get(pb->statii,
                                                    db->local_abspath,
                                                    APR_HASH_KEY_STRING);
      /* Note that status->url is NULL in the case of a missing
       * directory, which means we need to recurse up another level to
       * get a useful URL. */
      if (status)
        return status->url;

      url = find_dir_url(pb, pool);
      if (url)
        return svn_path_url_add_component2(url, db->name, pool);
      else
        return NULL;
    }
}



/* Create a new dir_baton for subdir PATH. */
static svn_error_t *
make_dir_baton(void **dir_baton,
               const char *path,
               struct edit_baton *edit_baton,
               struct dir_baton *parent_baton,
               apr_pool_t *pool)
{
  struct dir_baton *pb = parent_baton;
  struct edit_baton *eb = edit_baton;
  struct dir_baton *d = apr_pcalloc(pool, sizeof(*d));
  const char *local_abspath;
  const svn_wc_status3_t *status_in_parent;

  SVN_ERR_ASSERT(path || (! pb));

  /* Construct the absolute path of this directory. */
  if (pb)
    local_abspath = svn_dirent_join(eb->anchor_abspath, path, pool);
  else
    local_abspath = eb->anchor_abspath;

  /* Finish populating the baton members. */
  d->local_abspath = local_abspath;
  d->name = path ? svn_dirent_basename(path, pool) : NULL;
  d->edit_baton = edit_baton;
  d->parent_baton = parent_baton;
  d->pool = pool;
  d->statii = apr_hash_make(pool);
  d->url = apr_pstrdup(pool, find_dir_url(d, pool));
  d->ood_last_cmt_rev = SVN_INVALID_REVNUM;
  d->ood_last_cmt_date = 0;
  d->ood_kind = svn_node_dir;
  d->ood_last_cmt_author = NULL;

  if (pb)
    {
      if (pb->excluded)
        d->excluded = TRUE;
      else if (pb->depth == svn_depth_immediates)
        d->depth = svn_depth_empty;
      else if (pb->depth == svn_depth_files || pb->depth == svn_depth_empty)
        d->excluded = TRUE;
      else if (pb->depth == svn_depth_unknown)
        /* This is only tentative, it can be overridden from d's entry
           later. */
        d->depth = svn_depth_unknown;
      else
        d->depth = svn_depth_infinity;
    }
  else
    {
      d->depth = eb->default_depth;
    }

  /* Get the status for this path's children.  Of course, we only want
     to do this if the path is versioned as a directory. */
  if (pb)
    status_in_parent = apr_hash_get(pb->statii, d->local_abspath,
                                    APR_HASH_KEY_STRING);
  else
    status_in_parent = eb->anchor_status;

  /* Order is important here.  We can't depend on status_in_parent->entry
     being non-NULL until after we've checked all the conditions that
     might indicate that the parent is unversioned ("unversioned" for
     our purposes includes being an external or ignored item). */
  if (status_in_parent
      && (status_in_parent->text_status != svn_wc_status_unversioned)
      && (status_in_parent->text_status != svn_wc_status_missing)
      && (status_in_parent->text_status != svn_wc_status_obstructed)
      && (status_in_parent->text_status != svn_wc_status_external)
      && (status_in_parent->text_status != svn_wc_status_ignored)
      && (status_in_parent->entry->kind == svn_node_dir)
      && (! d->excluded)
      && (d->depth == svn_depth_unknown
          || d->depth == svn_depth_infinity
          || d->depth == svn_depth_files
          || d->depth == svn_depth_immediates)
          )
    {
      const svn_wc_status3_t *this_dir_status;
      const apr_array_header_t *ignores = eb->ignores;

      SVN_ERR(get_dir_status(&eb->wb, local_abspath,
                             status_in_parent->repos_root_url,
                             status_in_parent->repos_relpath,
                             NULL, ignores, d->depth == svn_depth_files ?
                             svn_depth_files : svn_depth_immediates,
                             TRUE, TRUE, TRUE, FALSE, hash_stash, d->statii,
                             NULL, NULL, pool));

      /* If we found a depth here, it should govern. */
      this_dir_status = apr_hash_get(d->statii, d->local_abspath,
                                     APR_HASH_KEY_STRING);
      if (this_dir_status && this_dir_status->entry
          && (d->depth == svn_depth_unknown
              || d->depth > status_in_parent->entry->depth))
        {
          d->depth = this_dir_status->entry->depth;
        }
    }

  *dir_baton = d;
  return SVN_NO_ERROR;
}


/* Make a file baton, using a new subpool of PARENT_DIR_BATON's pool.
   NAME is just one component, not a path. */
static struct file_baton *
make_file_baton(struct dir_baton *parent_dir_baton,
                const char *path,
                apr_pool_t *pool)
{
  struct dir_baton *pb = parent_dir_baton;
  struct edit_baton *eb = pb->edit_baton;
  struct file_baton *f = apr_pcalloc(pool, sizeof(*f));

  /* Finish populating the baton members. */
  f->local_abspath = svn_dirent_join(eb->anchor_abspath, path, pool);
  f->name = svn_dirent_basename(f->local_abspath, NULL);
  f->pool = pool;
  f->dir_baton = pb;
  f->edit_baton = eb;
  f->url = svn_path_url_add_component2(find_dir_url(pb, pool),
                                       f->name,
                                       pool);
  f->ood_last_cmt_rev = SVN_INVALID_REVNUM;
  f->ood_last_cmt_date = 0;
  f->ood_kind = svn_node_file;
  f->ood_last_cmt_author = NULL;
  return f;
}


svn_boolean_t
svn_wc__is_sendable_status(const svn_wc_status3_t *status,
                           svn_boolean_t no_ignore,
                           svn_boolean_t get_all)
{
  /* If the repository status was touched at all, it's interesting. */
  if (status->repos_text_status != svn_wc_status_none)
    return TRUE;
  if (status->repos_prop_status != svn_wc_status_none)
    return TRUE;

  /* If there is a lock in the repository, send it. */
  if (status->repos_lock)
    return TRUE;

  /* If the item is ignored, and we don't want ignores, skip it. */
  if ((status->text_status == svn_wc_status_ignored) && (! no_ignore))
    return FALSE;

  /* If we want everything, we obviously want this single-item subset
     of everything. */
  if (get_all)
    return TRUE;

  /* If the item is unversioned, display it. */
  if (status->text_status == svn_wc_status_unversioned)
    return TRUE;

  /* If the text, property or tree state is interesting, send it. */
  if ((status->text_status != svn_wc_status_none)
      && (status->text_status != svn_wc_status_normal))
    return TRUE;
  if ((status->prop_status != svn_wc_status_none)
      && (status->prop_status != svn_wc_status_normal))
    return TRUE;
  if (status->conflicted)
    return TRUE;

  /* If it's locked or switched, send it. */
  if (status->locked)
    return TRUE;
  if (status->switched)
    return TRUE;
  if (status->file_external)
    return TRUE;

  /* If there is a lock token, send it. */
  if (status->entry && status->entry->lock_token)
    return TRUE;

  /* If the entry is associated with a changelist, send it. */
  if (status->entry && status->entry->changelist)
    return TRUE;

  /* Otherwise, don't send it. */
  return FALSE;
}


/* Baton for mark_status. */
struct status_baton
{
  svn_wc_status_func4_t real_status_func;  /* real status function */
  void *real_status_baton;                 /* real status baton */
};

/* A status callback function which wraps the *real* status
   function/baton.   It simply sets the "repos_text_status" field of the
   STATUS to svn_wc_status_deleted and passes it off to the real
   status func/baton. Implements svn_wc_status_func4_t */
static svn_error_t *
mark_deleted(void *baton,
             const char *local_abspath,
             const svn_wc_status3_t *status,
             apr_pool_t *scratch_pool)
{
  struct status_baton *sb = baton;
  svn_wc_status3_t *new_status = svn_wc_dup_status3(status, scratch_pool);
  new_status->repos_text_status = svn_wc_status_deleted;
  return sb->real_status_func(sb->real_status_baton, local_abspath,
                              new_status, scratch_pool);
}


/* Handle a directory's STATII hash.  EB is the edit baton.  DIR_PATH
   and DIR_ENTRY are the on-disk path and entry, respectively, for the
   directory itself.  Descend into subdirectories according to DEPTH.
   Also, if DIR_WAS_DELETED is set, each status that is reported
   through this function will have its repos_text_status field showing
   a deletion.  Use POOL for all allocations. */
static svn_error_t *
handle_statii(struct edit_baton *eb,
              const svn_wc_entry_t *dir_entry,
              const char *dir_repos_root_url,
              const char *dir_repos_relpath,
              apr_hash_t *statii,
              svn_boolean_t dir_was_deleted,
              svn_depth_t depth,
              apr_pool_t *pool)
{
  const apr_array_header_t *ignores = eb->ignores;
  apr_hash_index_t *hi;
  apr_pool_t *subpool = svn_pool_create(pool);
  svn_wc_status_func4_t status_func = eb->status_func;
  void *status_baton = eb->status_baton;
  struct status_baton sb;

  if (dir_was_deleted)
    {
      sb.real_status_func = eb->status_func;
      sb.real_status_baton = eb->status_baton;
      status_func = mark_deleted;
      status_baton = &sb;
    }

  /* Loop over all the statuses still in our hash, handling each one. */
  for (hi = apr_hash_first(pool, statii); hi; hi = apr_hash_next(hi))
    {
      const char *path = svn__apr_hash_index_key(hi);
      svn_wc_status3_t *status = svn__apr_hash_index_val(hi);

      /* Clear the subpool. */
      svn_pool_clear(subpool);

      /* Now, handle the status.  We don't recurse for svn_depth_immediates
         because we already have the subdirectories' statii. */
      if (status->text_status != svn_wc_status_obstructed
          && status->text_status != svn_wc_status_missing
          && status->entry && status->entry->kind == svn_node_dir
          && (depth == svn_depth_unknown
              || depth == svn_depth_infinity))
        {
          const char *local_abspath;

          SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, subpool));

          SVN_ERR(get_dir_status(&eb->wb,
                                 local_abspath, dir_repos_root_url,
                                 dir_repos_relpath, NULL, ignores, depth,
                                 eb->get_all, eb->no_ignore, TRUE, FALSE,
                                 status_func, status_baton, eb->cancel_func,
                                 eb->cancel_baton, subpool));
        }
      if (dir_was_deleted)
        status->repos_text_status = svn_wc_status_deleted;
      if (svn_wc__is_sendable_status(status, eb->no_ignore, eb->get_all))
        SVN_ERR((eb->status_func)(eb->status_baton, path, status, subpool));
    }

  /* Destroy the subpool. */
  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}


/*----------------------------------------------------------------------*/

/*** The callbacks we'll plug into an svn_delta_editor_t structure. ***/

/* */
static svn_error_t *
set_target_revision(void *edit_baton,
                    svn_revnum_t target_revision,
                    apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;
  *(eb->target_revision) = target_revision;
  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
open_root(void *edit_baton,
          svn_revnum_t base_revision,
          apr_pool_t *pool,
          void **dir_baton)
{
  struct edit_baton *eb = edit_baton;
  eb->root_opened = TRUE;
  return make_dir_baton(dir_baton, NULL, eb, NULL, pool);
}


/* */
static svn_error_t *
delete_entry(const char *path,
             svn_revnum_t revision,
             void *parent_baton,
             apr_pool_t *pool)
{
  struct dir_baton *db = parent_baton;
  struct edit_baton *eb = db->edit_baton;
  const char *local_abspath = svn_dirent_join(eb->anchor_abspath, path, pool);
  const svn_wc_entry_t *entry;

  /* Note:  when something is deleted, it's okay to tweak the
     statushash immediately.  No need to wait until close_file or
     close_dir, because there's no risk of having to honor the 'added'
     flag.  We already know this item exists in the working copy. */

  /* Read the parent's entries file.  If the deleted thing is not
     versioned in this working copy, it was probably deleted via this
     working copy.  No need to report such a thing. */
  SVN_ERR(svn_wc__get_entry(&entry, eb->db, local_abspath, FALSE,
                            svn_node_unknown, FALSE, pool, pool));

  SVN_ERR(tweak_statushash(db, db, TRUE, eb->db,
                           local_abspath, entry->kind == svn_node_dir,
                           svn_wc_status_deleted, 0, revision, NULL, pool));

  /* Mark the parent dir -- it lost an entry (unless that parent dir
     is the root node and we're not supposed to report on the root
     node).  */
  if (db->parent_baton && (! *eb->target_basename))
    SVN_ERR(tweak_statushash(db->parent_baton, db, TRUE,eb->db,
                             db->local_abspath,
                             entry->kind == svn_node_dir,
                             svn_wc_status_modified, 0, SVN_INVALID_REVNUM,
                             NULL, pool));

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
add_directory(const char *path,
              void *parent_baton,
              const char *copyfrom_path,
              svn_revnum_t copyfrom_revision,
              apr_pool_t *pool,
              void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  struct edit_baton *eb = pb->edit_baton;
  struct dir_baton *new_db;

  SVN_ERR(make_dir_baton(child_baton, path, eb, pb, pool));

  /* Make this dir as added. */
  new_db = *child_baton;
  new_db->added = TRUE;

  /* Mark the parent as changed;  it gained an entry. */
  pb->text_changed = TRUE;

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
open_directory(const char *path,
               void *parent_baton,
               svn_revnum_t base_revision,
               apr_pool_t *pool,
               void **child_baton)
{
  struct dir_baton *pb = parent_baton;
  return make_dir_baton(child_baton, path, pb->edit_baton, pb, pool);
}


/* */
static svn_error_t *
change_dir_prop(void *dir_baton,
                const char *name,
                const svn_string_t *value,
                apr_pool_t *pool)
{
  struct dir_baton *db = dir_baton;
  if (svn_wc_is_normal_prop(name))
    db->prop_changed = TRUE;

  /* Note any changes to the repository. */
  if (value != NULL)
    {
      if (strcmp(name, SVN_PROP_ENTRY_COMMITTED_REV) == 0)
        db->ood_last_cmt_rev = SVN_STR_TO_REV(value->data);
      else if (strcmp(name, SVN_PROP_ENTRY_LAST_AUTHOR) == 0)
        db->ood_last_cmt_author = apr_pstrdup(db->pool, value->data);
      else if (strcmp(name, SVN_PROP_ENTRY_COMMITTED_DATE) == 0)
        {
          apr_time_t tm;
          SVN_ERR(svn_time_from_cstring(&tm, value->data, db->pool));
          db->ood_last_cmt_date = tm;
        }
    }

  return SVN_NO_ERROR;
}



/* */
static svn_error_t *
close_directory(void *dir_baton,
                apr_pool_t *pool)
{
  struct dir_baton *db = dir_baton;
  struct dir_baton *pb = db->parent_baton;
  struct edit_baton *eb = db->edit_baton;

  /* If nothing has changed and directory has no out of
     date descendants, return. */
  if (db->added || db->prop_changed || db->text_changed
      || db->ood_last_cmt_rev != SVN_INVALID_REVNUM)
    {
      enum svn_wc_status_kind repos_text_status;
      enum svn_wc_status_kind repos_prop_status;

      /* If this is a new directory, add it to the statushash. */
      if (db->added)
        {
          repos_text_status = svn_wc_status_added;
          repos_prop_status = db->prop_changed ? svn_wc_status_added
                              : svn_wc_status_none;
        }
      else
        {
          repos_text_status = db->text_changed ? svn_wc_status_modified
                              : svn_wc_status_none;
          repos_prop_status = db->prop_changed ? svn_wc_status_modified
                              : svn_wc_status_none;
        }

      /* Maybe add this directory to its parent's status hash.  Note
         that tweak_statushash won't do anything if repos_text_status
         is not svn_wc_status_added. */
      if (pb)
        {
          /* ### When we add directory locking, we need to find a
             ### directory lock here. */
          SVN_ERR(tweak_statushash(pb, db, TRUE, eb->db, db->local_abspath,
                                   TRUE, repos_text_status, repos_prop_status,
                                   SVN_INVALID_REVNUM, NULL, pool));
        }
      else
        {
          /* We're editing the root dir of the WC.  As its repos
             status info isn't otherwise set, set it directly to
             trigger invocation of the status callback below. */
          eb->anchor_status->repos_prop_status = repos_prop_status;
          eb->anchor_status->repos_text_status = repos_text_status;

          /* If the root dir is out of date set the ood info directly too. */
          if (db->ood_last_cmt_rev != eb->anchor_status->entry->revision)
            {
              eb->anchor_status->ood_last_cmt_rev = db->ood_last_cmt_rev;
              eb->anchor_status->ood_last_cmt_date = db->ood_last_cmt_date;
              eb->anchor_status->ood_kind = db->ood_kind;
              eb->anchor_status->ood_last_cmt_author =
                apr_pstrdup(pool, db->ood_last_cmt_author);
            }
        }
    }

  /* Handle this directory's statuses, and then note in the parent
     that this has been done. */
  if (pb && ! db->excluded)
    {
      svn_boolean_t was_deleted = FALSE;
      const svn_wc_status3_t *dir_status;

      /* See if the directory was deleted or replaced. */
      dir_status = apr_hash_get(pb->statii, db->local_abspath,
                                APR_HASH_KEY_STRING);
      if (dir_status &&
          ((dir_status->repos_text_status == svn_wc_status_deleted)
           || (dir_status->repos_text_status == svn_wc_status_replaced)))
        was_deleted = TRUE;

      /* Now do the status reporting. */
      SVN_ERR(handle_statii(eb, dir_status ? dir_status->entry : NULL,
                            dir_status ? dir_status->repos_root_url : NULL,
                            dir_status ? dir_status->repos_relpath : NULL,
                            db->statii, was_deleted, db->depth, pool));
      if (dir_status && svn_wc__is_sendable_status(dir_status, eb->no_ignore,
                                                  eb->get_all))
        SVN_ERR((eb->status_func)(eb->status_baton, db->local_abspath,
                                  dir_status, pool));
      apr_hash_set(pb->statii, db->local_abspath, APR_HASH_KEY_STRING, NULL);
    }
  else if (! pb)
    {
      /* If this is the top-most directory, and the operation had a
         target, we should only report the target. */
      if (*eb->target_basename)
        {
          const svn_wc_status3_t *tgt_status;

          tgt_status = apr_hash_get(db->statii, eb->target_abspath,
                                    APR_HASH_KEY_STRING);
          if (tgt_status)
            {
              if (tgt_status->entry
                  && tgt_status->entry->kind == svn_node_dir)
                {
                  SVN_ERR(get_dir_status(&eb->wb, eb->target_abspath,
                                         NULL, NULL, NULL, eb->ignores, 
                                         eb->default_depth,
                                         eb->get_all, eb->no_ignore, TRUE,
                                         FALSE,
                                         eb->status_func, eb->status_baton,
                                         eb->cancel_func, eb->cancel_baton,
                                         pool));
                }
              if (svn_wc__is_sendable_status(tgt_status, eb->no_ignore,
                                             eb->get_all))
                SVN_ERR((eb->status_func)(eb->status_baton, eb->target_abspath,
                                          tgt_status, pool));
            }
        }
      else
        {
          /* Otherwise, we report on all our children and ourself.
             Note that our directory couldn't have been deleted,
             because it is the root of the edit drive. */
          SVN_ERR(handle_statii(eb, eb->anchor_status->entry,
                                eb->anchor_status->repos_root_url,
                                eb->anchor_status->repos_relpath,
                                db->statii, FALSE, eb->default_depth, pool));
          if (svn_wc__is_sendable_status(eb->anchor_status, eb->no_ignore,
                                         eb->get_all))
            SVN_ERR((eb->status_func)(eb->status_baton, db->local_abspath,
                                      eb->anchor_status, pool));
          eb->anchor_status = NULL;
        }
    }
  return SVN_NO_ERROR;
}



/* */
static svn_error_t *
add_file(const char *path,
         void *parent_baton,
         const char *copyfrom_path,
         svn_revnum_t copyfrom_revision,
         apr_pool_t *pool,
         void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *new_fb = make_file_baton(pb, path, pool);

  /* Mark parent dir as changed */
  pb->text_changed = TRUE;

  /* Make this file as added. */
  new_fb->added = TRUE;

  *file_baton = new_fb;
  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
open_file(const char *path,
          void *parent_baton,
          svn_revnum_t base_revision,
          apr_pool_t *pool,
          void **file_baton)
{
  struct dir_baton *pb = parent_baton;
  struct file_baton *new_fb = make_file_baton(pb, path, pool);

  *file_baton = new_fb;
  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
apply_textdelta(void *file_baton,
                const char *base_checksum,
                apr_pool_t *pool,
                svn_txdelta_window_handler_t *handler,
                void **handler_baton)
{
  struct file_baton *fb = file_baton;

  /* Mark file as having textual mods. */
  fb->text_changed = TRUE;

  /* Send back a NULL window handler -- we don't need the actual diffs. */
  *handler_baton = NULL;
  *handler = svn_delta_noop_window_handler;

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
change_file_prop(void *file_baton,
                 const char *name,
                 const svn_string_t *value,
                 apr_pool_t *pool)
{
  struct file_baton *fb = file_baton;
  if (svn_wc_is_normal_prop(name))
    fb->prop_changed = TRUE;

  /* Note any changes to the repository. */
  if (value != NULL)
    {
      if (strcmp(name, SVN_PROP_ENTRY_COMMITTED_REV) == 0)
        fb->ood_last_cmt_rev = SVN_STR_TO_REV(value->data);
      else if (strcmp(name, SVN_PROP_ENTRY_LAST_AUTHOR) == 0)
        fb->ood_last_cmt_author = apr_pstrdup(fb->dir_baton->pool,
                                              value->data);
      else if (strcmp(name, SVN_PROP_ENTRY_COMMITTED_DATE) == 0)
        {
          apr_time_t tm;
          SVN_ERR(svn_time_from_cstring(&tm, value->data,
                                        fb->dir_baton->pool));
          fb->ood_last_cmt_date = tm;
        }
    }

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
close_file(void *file_baton,
           const char *text_checksum,  /* ignored, as we receive no data */
           apr_pool_t *pool)
{
  struct file_baton *fb = file_baton;
  enum svn_wc_status_kind repos_text_status;
  enum svn_wc_status_kind repos_prop_status;
  const svn_lock_t *repos_lock = NULL;

  /* If nothing has changed, return. */
  if (! (fb->added || fb->prop_changed || fb->text_changed))
    return SVN_NO_ERROR;

  /* If this is a new file, add it to the statushash. */
  if (fb->added)
    {
      const char *url;
      repos_text_status = svn_wc_status_added;
      repos_prop_status = fb->prop_changed ? svn_wc_status_added : 0;

      if (fb->edit_baton->wb.repos_locks)
        {
          url = find_dir_url(fb->dir_baton, pool);
          if (url)
            {
              url = svn_path_url_add_component2(url, fb->name, pool);
              repos_lock = apr_hash_get
                (fb->edit_baton->wb.repos_locks,
                 svn_path_uri_decode(url +
                                     strlen(fb->edit_baton->wb.repos_root),
                                     pool), APR_HASH_KEY_STRING);
            }
        }
    }
  else
    {
      repos_text_status = fb->text_changed ? svn_wc_status_modified : 0;
      repos_prop_status = fb->prop_changed ? svn_wc_status_modified : 0;
    }

  return tweak_statushash(fb, NULL, FALSE, fb->edit_baton->db,
                          fb->local_abspath, FALSE, repos_text_status,
                          repos_prop_status, SVN_INVALID_REVNUM, repos_lock,
                          pool);
}

/* */
static svn_error_t *
close_edit(void *edit_baton,
           apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;

  /* If we get here and the root was not opened as part of the edit,
     we need to transmit statuses for everything.  Otherwise, we
     should be done. */
  if (eb->root_opened)
    return SVN_NO_ERROR;

  SVN_ERR(svn_wc_walk_status(eb->wc_ctx,
                             eb->target_abspath,
                             eb->default_depth,
                             eb->get_all,
                             eb->no_ignore,
                             FALSE,
                             eb->ignores,
                             eb->status_func,
                             eb->status_baton,
                             eb->wb.external_func,
                             eb->wb.external_baton,
                             eb->cancel_func,
                             eb->cancel_baton,
                             pool));

  return SVN_NO_ERROR;
}



/*** Public API ***/

svn_error_t *
svn_wc_get_status_editor5(const svn_delta_editor_t **editor,
                          void **edit_baton,
                          void **set_locks_baton,
                          svn_revnum_t *edit_revision,
                          svn_wc_context_t *wc_ctx,
                          const char *anchor_abspath,
                          const char *target_basename,
                          svn_depth_t depth,
                          svn_boolean_t get_all,
                          svn_boolean_t no_ignore,
                          const apr_array_header_t *ignore_patterns,
                          svn_wc_status_func4_t status_func,
                          void *status_baton,
                          svn_wc_external_update_t external_func,
                          void *external_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  struct edit_baton *eb;
  svn_delta_editor_t *tree_editor = svn_delta_default_editor(result_pool);

  /* Construct an edit baton. */
  eb = apr_palloc(result_pool, sizeof(*eb));
  eb->default_depth     = depth;
  eb->target_revision   = edit_revision;
  eb->db                = wc_ctx->db;
  eb->wc_ctx            = wc_ctx;
  eb->get_all           = get_all;
  eb->no_ignore         = no_ignore;
  eb->status_func       = status_func;
  eb->status_baton      = status_baton;
  eb->cancel_func       = cancel_func;
  eb->cancel_baton      = cancel_baton;
  eb->anchor_abspath    = apr_pstrdup(result_pool, anchor_abspath);
  eb->target_abspath    = svn_dirent_join(anchor_abspath, target_basename,
                                          result_pool);



  eb->target_basename   = apr_pstrdup(result_pool, target_basename);
  eb->root_opened       = FALSE;

  eb->wb.db             = wc_ctx->db;
  eb->wb.target_abspath = eb->target_abspath;
  eb->wb.external_func  = external_func;
  eb->wb.external_baton = external_baton;
  eb->wb.externals      = apr_hash_make(result_pool);
  eb->wb.repos_locks    = NULL;
  eb->wb.repos_root     = NULL;

  /* Use the caller-provided ignore patterns if provided; the build-time
     configured defaults otherwise. */
  if (ignore_patterns)
    {
      eb->ignores = ignore_patterns;
    }
  else
    {
      apr_array_header_t *ignores;

      svn_wc_get_default_ignores(&ignores, NULL, result_pool);
      eb->ignores = ignores;
    }

  /* The edit baton's status structure maps to PATH, and the editor
     have to be aware of whether that is the anchor or the target. */
  SVN_ERR(internal_status(&(eb->anchor_status), wc_ctx->db, anchor_abspath,
                         result_pool, scratch_pool));

  /* Construct an editor. */
  tree_editor->set_target_revision = set_target_revision;
  tree_editor->open_root = open_root;
  tree_editor->delete_entry = delete_entry;
  tree_editor->add_directory = add_directory;
  tree_editor->open_directory = open_directory;
  tree_editor->change_dir_prop = change_dir_prop;
  tree_editor->close_directory = close_directory;
  tree_editor->add_file = add_file;
  tree_editor->open_file = open_file;
  tree_editor->apply_textdelta = apply_textdelta;
  tree_editor->change_file_prop = change_file_prop;
  tree_editor->close_file = close_file;
  tree_editor->close_edit = close_edit;

  /* Conjoin a cancellation editor with our status editor. */
  SVN_ERR(svn_delta_get_cancellation_editor(cancel_func, cancel_baton,
                                            tree_editor, eb, editor,
                                            edit_baton, result_pool));

  if (set_locks_baton)
    *set_locks_baton = eb;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_walk_status(svn_wc_context_t *wc_ctx,
                   const char *local_abspath,
                   svn_depth_t depth,
                   svn_boolean_t get_all,
                   svn_boolean_t no_ignore,
                   svn_boolean_t get_excluded,
                   const apr_array_header_t *ignore_patterns,
                   svn_wc_status_func4_t status_func,
                   void *status_baton,
                   svn_wc_external_update_t external_func,
                   void *external_baton,
                   svn_cancel_func_t cancel_func,
                   void *cancel_baton,
                   apr_pool_t *scratch_pool)
{
  svn_node_kind_t kind, local_kind;
  struct walk_status_baton wb;

  wb.db = wc_ctx->db;
  wb.target_abspath = local_abspath;
  wb.externals = apr_hash_make(scratch_pool);
  wb.external_func = external_func;
  wb.external_baton = external_baton;
  wb.repos_root = NULL;
  wb.repos_locks = NULL;

  /* Use the caller-provided ignore patterns if provided; the build-time
     configured defaults otherwise. */
  if (!ignore_patterns)
    {
      apr_array_header_t *ignores;

      svn_wc_get_default_ignores(&ignores, NULL, scratch_pool);
      ignore_patterns = ignores;
    }

  SVN_ERR(svn_wc_read_kind(&kind, wc_ctx, local_abspath, FALSE, scratch_pool));
  SVN_ERR(svn_io_check_path(local_abspath, &local_kind, scratch_pool));

  if (kind == svn_node_file && local_kind == svn_node_file)
    {
      SVN_ERR(get_dir_status(&wb,
                             svn_dirent_dirname(local_abspath, scratch_pool),
                             NULL,
                             NULL,
                             svn_dirent_basename(local_abspath, NULL),
                             ignore_patterns,
                             depth,
                             get_all,
                             TRUE,
                             TRUE,
                             get_excluded,
                             status_func,
                             status_baton,
                             cancel_func,
                             cancel_baton,
                             scratch_pool));
    }
  else if (kind == svn_node_dir && local_kind == svn_node_dir)
    {
      SVN_ERR(get_dir_status(&wb,
                             local_abspath,
                             NULL,
                             NULL,
                             NULL,
                             ignore_patterns,
                             depth,
                             get_all,
                             no_ignore,
                             FALSE,
                             get_excluded,
                             status_func,
                             status_baton,
                             cancel_func,
                             cancel_baton,
                             scratch_pool));
    }
  else
    {
      SVN_ERR(get_dir_status(&wb,
                             svn_dirent_dirname(local_abspath, scratch_pool),
                             NULL,
                             NULL,
                             svn_dirent_basename(local_abspath, NULL),
                             ignore_patterns,
                             depth,
                             get_all,
                             no_ignore,
                             TRUE,
                             get_excluded,
                             status_func,
                             status_baton,
                             cancel_func,
                             cancel_baton,
                             scratch_pool));
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_status_set_repos_locks(void *edit_baton,
                              apr_hash_t *locks,
                              const char *repos_root,
                              apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;

  eb->wb.repos_locks = locks;
  eb->wb.repos_root = apr_pstrdup(pool, repos_root);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_get_default_ignores(apr_array_header_t **patterns,
                           apr_hash_t *config,
                           apr_pool_t *pool)
{
  svn_config_t *cfg = config ? apr_hash_get(config,
                                            SVN_CONFIG_CATEGORY_CONFIG,
                                            APR_HASH_KEY_STRING) : NULL;
  const char *val;

  /* Check the Subversion run-time configuration for global ignores.
     If no configuration value exists, we fall back to our defaults. */
  svn_config_get(cfg, &val, SVN_CONFIG_SECTION_MISCELLANY,
                 SVN_CONFIG_OPTION_GLOBAL_IGNORES,
                 SVN_CONFIG_DEFAULT_GLOBAL_IGNORES);
  *patterns = apr_array_make(pool, 16, sizeof(const char *));

  /* Split the patterns on whitespace, and stuff them into *PATTERNS. */
  svn_cstring_split_append(*patterns, val, "\n\r\t\v ", FALSE, pool);
  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
internal_status(svn_wc_status3_t **status,
                svn_wc__db_t *db,
                const char *local_abspath,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  svn_node_kind_t path_kind;
  svn_boolean_t path_special;
  const svn_wc_entry_t *entry;
  const char *parent_repos_relpath;
  const char *parent_repos_root_url;
  svn_error_t *err;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(svn_io_check_special_path(local_abspath, &path_kind, &path_special,
                                    scratch_pool));

  err = svn_wc__get_entry(&entry, db, local_abspath, TRUE,
                          svn_node_unknown, FALSE, scratch_pool, scratch_pool);
  if (err && (err->apr_err == SVN_ERR_WC_MISSING
                || err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY
                || err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND
                || err->apr_err == SVN_ERR_NODE_UNEXPECTED_KIND))
    {
      svn_error_clear(err);
      entry = NULL;
    }
  else if (err)
    return svn_error_return(err);

  if (entry)
    {
      svn_boolean_t hidden;

      SVN_ERR(svn_wc__entry_is_hidden(&hidden, entry));
      if (hidden)
        entry = NULL;
    }
  if (entry == NULL)
    return svn_error_return(assemble_unversioned(status,
                                                 db, local_abspath,
                                                 path_kind,
                                                 FALSE /* is_ignored */,
                                                 result_pool, scratch_pool));

  if (!svn_dirent_is_root(local_abspath, strlen(local_abspath)))
    {
      svn_wc__db_status_t status;
      const char *parent_abspath = svn_dirent_dirname(local_abspath,
                                                      scratch_pool);

      err = svn_wc__db_read_info(&status, NULL, NULL, &parent_repos_relpath,
                                 &parent_repos_root_url, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                 NULL, db, parent_abspath, result_pool,
                                 scratch_pool);

      if (err && (err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY
                  || err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND))
        {
          svn_error_clear(err);
          parent_repos_root_url = NULL;
          parent_repos_relpath = NULL;
        }
      else if (err)
        return svn_error_return(err);

      if (!err 
          && parent_repos_relpath == NULL 
          && status != svn_wc__db_status_added
          && status != svn_wc__db_status_deleted)
        SVN_ERR(svn_wc__db_scan_base_repos(&parent_repos_relpath,
                                           &parent_repos_root_url, NULL,
                                           db, local_abspath,
                                           result_pool, scratch_pool));
    }
  else
    {
      parent_repos_root_url = NULL;
      parent_repos_relpath = NULL;
    }

  return svn_error_return(assemble_status(status, db, local_abspath,
                                          entry, parent_repos_root_url,
                                          parent_repos_relpath, path_kind,
                                          path_special,
                                          TRUE /* get_all */,
                                          FALSE /* is_ignored */,
                                          NULL /* repos_lock */,
                                          result_pool, scratch_pool));
}


svn_error_t *
svn_wc_status3(svn_wc_status3_t **status,
               svn_wc_context_t *wc_ctx,
               const char *local_abspath,
               apr_pool_t *result_pool,
               apr_pool_t *scratch_pool)
{
  return svn_error_return(
    internal_status(status, wc_ctx->db, local_abspath, result_pool,
                    scratch_pool));
}

svn_wc_status3_t *
svn_wc_dup_status3(const svn_wc_status3_t *orig_stat,
                   apr_pool_t *pool)
{
  svn_wc_status3_t *new_stat = apr_palloc(pool, sizeof(*new_stat));

  /* Shallow copy all members. */
  *new_stat = *orig_stat;

  /* Now go back and dup the deep items into this pool. */
  if (orig_stat->entry)
    new_stat->entry = svn_wc_entry_dup(orig_stat->entry, pool);

  if (orig_stat->repos_lock)
    new_stat->repos_lock = svn_lock_dup(orig_stat->repos_lock, pool);

  if (orig_stat->url)
    new_stat->url = apr_pstrdup(pool, orig_stat->url);

  if (orig_stat->changed_author)
    new_stat->changed_author = apr_pstrdup(pool, orig_stat->changed_author);

  if (orig_stat->ood_last_cmt_author)
    new_stat->ood_last_cmt_author
      = apr_pstrdup(pool, orig_stat->ood_last_cmt_author);

  if (orig_stat->lock_token)
    new_stat->lock_token
      = apr_pstrdup(pool, orig_stat->lock_token);

  if (orig_stat->lock_owner)
    new_stat->lock_owner
      = apr_pstrdup(pool, orig_stat->lock_owner);

  if (orig_stat->lock_comment)
    new_stat->lock_comment
      = apr_pstrdup(pool, orig_stat->lock_comment);

  if (orig_stat->changelist)
    new_stat->changelist
      = apr_pstrdup(pool, orig_stat->changelist);

  if (orig_stat->repos_root_url)
    new_stat->repos_root_url
      = apr_pstrdup(pool, orig_stat->repos_root_url);

  if (orig_stat->repos_relpath)
    new_stat->repos_relpath
      = apr_pstrdup(pool, orig_stat->repos_relpath);

  /* Return the new hotness. */
  return new_stat;
}

svn_error_t *
svn_wc_get_ignores2(apr_array_header_t **patterns,
                    svn_wc_context_t *wc_ctx,
                    const char *local_abspath,
                    apr_hash_t *config,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool)
{
  apr_array_header_t *default_ignores;

  SVN_ERR(svn_wc_get_default_ignores(&default_ignores, config, scratch_pool));
  return svn_error_return(collect_ignore_patterns(patterns, wc_ctx->db,
                                                  local_abspath,
                                                  default_ignores,
                                                  result_pool, scratch_pool));
}
