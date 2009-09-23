/**
 * @copyright
 * ====================================================================
 *    Licensed to the Subversion Corporation (SVN Corp.) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The SVN Corp. licenses this file
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
 * @endcopyright
 *
 * @file svn_wc_db.h
 * @brief The Subversion Working Copy Library - Metadata/Base-Text Support
 *
 * Requires:
 *            - A working copy
 *
 * Provides:
 *            - Ability to manipulate working copy's administrative files.
 *
 * Used By:
 *            - The main working copy library
 */

#ifndef SVN_WC_DB_H
#define SVN_WC_DB_H

#include "svn_wc.h"

#include "svn_types.h"
#include "svn_error.h"
#include "svn_config.h"
#include "svn_io.h"

#include "private/svn_skel.h"
#include "private/svn_sqlite.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* INTERFACE CONVENTIONS

   "OUT" PARAMETERS

   There are numerous functions within this API which take a (large) number
   of "out" parameters. These are listed individually, rather than combined
   into a struct, so that a caller can be fine-grained about the which
   pieces of information are being requested. In many cases, only a subset
   is required, so the implementation can perform various optimizations
   to fulfill the limited request for information.


   POOLS

   wc_db uses the dual-pool paradigm for all of its functions. Any OUT
   parameter will be allocated within the result pool, and all temporary
   allocations will be performed within the scratch pool.

   The pool that DB is allocated within (the "state" pool) is only used
   for a few, limited allocations to track each of the working copy roots
   that the DB is asked to operate upon. The memory usage on this pool
   os O(# wcroots), which should normally be one or a few. Custom clients
   which hold open structures over a significant period of time should
   pay particular attention to the number of roots touched, and the
   resulting impact on memory consumption (which should still be minimal).


   PARAMETER NAMING

   * DB
     This parameter is the primary context for all operations on the
     metadata for working copies. This parameter is passed to almost every
     function, and maintains information and state about every working
     copy "touched" by any of the APIs in this interface.

   * LOCAL_ABSPATH
     This parameter specifies a particular *versioned* node in the local
     filesystem. From this node, a working copy root is implied, and will
     be used for the given API operation.

   * LOCAL_DIR_ABSPATH
     This parameter is similar to LOCAL_ABSPATH, but the semantics of the
     parameter and operation require the node to be a directory within
     the working copy.

   * WRI_ABSPATH
     This is a "Working copy Root Indicator" path. It does not refer to a
     specific versioned node, but rather to an implied location in the
     local filesystem, which further implies a working copy root. The
     given operation will be performed within the context of that root.
*/

/** Context data structure for interacting with the administrative data. */
typedef struct svn_wc__db_t svn_wc__db_t;

/** Pristine Directory Handle
 *
 * Handle for working with pristine files associated with a specific
 * directory on the local filesystem.
 */
typedef struct svn_wc__db_pdh_t svn_wc__db_pdh_t;


/**
 * Enumerated constants for how to open a WC datastore.
 */
typedef enum {
  svn_wc__db_openmode_default,    /* Open in the default mode (r/w now). */
  svn_wc__db_openmode_readonly,   /* Changes will definitely NOT be made. */
  svn_wc__db_openmode_readwrite   /* Changes will definitely be made. */

} svn_wc__db_openmode_t;


/* Enum indicating what kind of versioned object we're talking about.
 *
 * ### KFF: That is, my understanding is that this is *not* an enum
 * ### indicating what kind of storage the DB is using, even though
 * ### one might think that from its name.  Rather, the "svn_wc__db_"
 * ### is a generic prefix, and this "_kind_t" type indicates the kind
 * ### of something that's being stored in the DB.
 *
 * ### KFF: Does this overlap too much with what svn_node_kind_t does?
 *
 * ### gjs: possibly. but that doesn't have a symlink kind. and that
 * ###   cannot simply be added. it would surprise too much code.
 * ###   (we could probably create svn_node_kind2_t though)
 */
typedef enum {
    /* The node is a directory. */
    svn_wc__db_kind_dir,

    /* The node is a file. */
    svn_wc__db_kind_file,

    /* The node is a symbolic link. */
    svn_wc__db_kind_symlink,

    /* The type of the node is not known, due to its absence, exclusion,
       deletion, or incomplete status. */
    svn_wc__db_kind_unknown,

    /* This directory node is a placeholder; the actual information is
       held within the subdirectory.

       Note: users of this API shouldn't see this kind. It will be
       handled internally to wc_db.

       ### only used with per-dir .svn subdirectories.  */
    svn_wc__db_kind_subdir

} svn_wc__db_kind_t;


/** Enumerated values describing the state of a node. */
typedef enum {
    /* The node is present and has no known modifications applied to it. */
    svn_wc__db_status_normal,

    /* The node has been added (potentially obscuring a delete or move of
       the BASE node; see BASE_SHADOWED param). The text will be marked as
       modified, and if properties exist, they will be marked as modified. */
    svn_wc__db_status_added,

    /* This node is no longer present because it was the source of a move. */
    svn_wc__db_status_moved_away,

    /* This node has been added with history, based on the move source.
       Text and property modifications are based on whether changes have
       been made against their pristine versions. */
    svn_wc__db_status_moved_here,

    /* This node has been added with history, based on the copy source.
       Text and property modifications are based on whether changes have
       been made against their pristine versions. */
    svn_wc__db_status_copied,

    /* This node has been deleted. No text or property modifications
       will be present. */
    svn_wc__db_status_deleted,

    /* The information for this directory node is obstructed by something
       in the local filesystem. Full details are not available.

       This is only returned by an unshadowed BASE node. If a WORKING node
       is present, then obstructed_delete or obstructed_add is returned as
       appropriate.

       ### only used with per-dir .svn subdirectories.  */
    svn_wc__db_status_obstructed,

    /* The information for this directory node is obstructed by something
       in the local filesystem. Full details are not available.

       The directory has been marked for deletion.

       ### only used with per-dir .svn subdirectories.  */
    svn_wc__db_status_obstructed_delete,

    /* The information for this directory node is obstructed by something
       in the local filesystem. Full details are not available.

       The directory has been marked for addition.

       ### only used with per-dir .svn subdirectories.  */
    svn_wc__db_status_obstructed_add,

    /* This node was named by the server, but no information was provided. */
    svn_wc__db_status_absent,

    /* This node has been administratively excluded. */
    svn_wc__db_status_excluded,

    /* This node is not present in this revision. This typically happens
       when a node is deleted and committed without updating its parent.
       The parent revision indicates it should be present, but this node's
       revision states otherwise. */
    svn_wc__db_status_not_present,

    /* This node is known, but its information is incomplete. Generally,
       it should be treated similar to the other missing status values
       until some (later) process updates the node with its data. */
    svn_wc__db_status_incomplete,

    /* The BASE node has been marked as deleted.
       ### is this internal or external to wc_db? we may be able to hide
       ### behind status_deleted, and this value is only used within the
       ### scan_deletion function.  */
    svn_wc__db_status_base_deleted

} svn_wc__db_status_t;

/** Lock information.  We write/read it all as one, so let's use a struct
    for convenience.  */
typedef struct {
  /* The lock token */
  const char *token;

  /* The owner of the lock, possibly NULL */
  const char *owner;

  /* A comment about the lock, possibly NULL */
  const char *comment;

  /* The date the lock was created */
  apr_time_t date;
} svn_wc__db_lock_t;


/* ### note conventions of "result_pool" for the pool where return results
   ### are allocated, and "scratch_pool" for the pool that is used for
   ### intermediate allocations (and which can be safely cleared upon
   ### return from the function).
*/

/* ### NOTE: I have not provided docstrings for most of this file at this
   ### point in time. The shape and extent of this API is still in massive
   ### flux. I'm iterating in public, but do not want to doc until it feels
   ### like it is "Right".
*/

/* ### where/how to handle: text_time, locks, working_size */

/* ### update docstrings: all paths should be internal/canonical */

/**
 * ### KFF: Would be good to state, here or in an introductory comment
 * ### at the top of this file, whether subsequent 'path' parameters
 * ### are absolute, or relative to the root at which @a *db was
 * ### opened, or perhaps that both are acceptable.
 */
/* ### all paths in this API are absolute, and internal/canonical form.  */


/**
 * @defgroup svn_wc__db_admin  General administractive functions
 * @{
 */

/** Open a working copy administrative database context.
 *
 * This context is (initially) not associated with any particular working
 * copy directory or working copy root (wcroot). As operations are performed,
 * this context will load the appropriate wcroot information.
 *
 * The context is returned in DB. The MODE parameter indicates whether the
 * caller knows all interactions will be read-only, whether writing will
 * definitely happen, or whether a default should be chosen.
 *
 * CONFIG should hold the various configuration options that may apply to
 * the administrative operation. It should live at least as long as the
 * RESULT_POOL parameter.
 *
 * When AUTO_UPGRADE is TRUE, then the working copy databases will be
 * upgraded when possible (when an old database is found/detected during
 * the operation of a wc_db API). If it is detected that a manual upgrade is
 * required, then SVN_ERR_WC_UPGRADE_REQUIRED will be returned from that API.
 * Passing FALSE will allow a bare minimum of APIs to function (most notably,
 * the temp_get_format() function will always return a value) since most of
 * these APIs expect a current-format database to be present.
 *
 * If ENFORCE_EMPTY_WQ is TRUE, then any databases with stale work items in
 * their work queue will raise an error when they are opened. The operation
 * will raise SVN_ERR_WC_CLEANUP_REQUIRED. Passing FALSE for this routine
 * means that the work queue is being processed (via 'svn cleanup') and all
 * operations should be allowed.
 *
 * The DB will be closed when RESULT_POOL is cleared. It may also be closed
 * manually using svn_wc__db_close(). In particular, this will close any
 * SQLite databases that have been opened and cached.
 *
 * The context is allocated in RESULT_POOL. This pool is *retained* and used
 * for future allocations within the DB. Be forewarned about unbounded
 * memory growth if this DB is used across an unbounded number of wcroots
 * and versioned directories.
 *
 * Temporary allocations will be made in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__db_open(svn_wc__db_t **db,
                svn_wc__db_openmode_t mode,
                svn_config_t *config,
                svn_boolean_t auto_upgrade,
                svn_boolean_t enforce_empty_wq,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool);


/**
 * Close DB.
 */
svn_error_t *
svn_wc__db_close(svn_wc__db_t *db);


/**
 * Initialize the DB for LOCAL_ABSPATH, which should be a working copy path.
 * Use SCRATCH_POOL for temporary allocations.
 */
svn_error_t *
svn_wc__db_init(const char *local_abspath,
                const char *repos_relpath,
                const char *repos_root_url,
                const char *repos_uuid,
                svn_revnum_t initial_rev,
                svn_depth_t depth,
                apr_pool_t *scratch_pool);


/** @} */

/**
 * Different kinds of trees
 *
 * The design doc mentions three different kinds of trees, BASE, WORKING and
 * ACTUAL: http://svn.collab.net/repos/svn/trunk/notes/wc-ng-design
 * We have different APIs to handle each tree, enumerated below, along with
 * a blurb to explain what that tree represents.
 */

/**
 * @defgroup svn_wc__db_base  BASE tree management
 *
 * BASE should be what we get from the server. The *absolute* pristine copy.
 * Nothing can change it -- it is always a reflection of the repository.
 * You need to use checkout, update, switch, or commit to alter your view of
 * the repository.
 *
 * @{
 */

/** Add or replace a directory in the BASE tree.
 *
 * The directory is located at LOCAL_ABSPATH on the local filesystem, and
 * corresponds to <REPOS_RELPATH, REPOS_ROOT_URL, REPOS_UUID> in the
 * repository, at revision REVISION.
 *
 * The directory properties are given by the PROPS hash (which is
 * const char *name => const svn_string_t *).
 *
 * The last-change information is given by <CHANGED_REV, CHANGED_DATE,
 * CHANGED_AUTHOR>.
 *
 * The directory's children are listed in CHILDREN, as an array of
 * const char *. The child nodes do NOT have to exist when this API
 * is called. For each child node which does not exists, an "incomplete"
 * node will be added. These child nodes will be added regardless of
 * the DEPTH value. The caller must sort out which must be recorded,
 * and which must be omitted.
 *
 * This subsystem does not use DEPTH, but it can be recorded here in
 * the BASE tree for higher-level code to use.
 *
 * All temporary allocations will be made in SCRATCH_POOL.
 */
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
                              apr_pool_t *scratch_pool);


/** Add or replace a file in the BASE tree.
 *
 * The file is located at LOCAL_ABSPATH on the local filesystem, and
 * corresponds to <REPOS_RELPATH, REPOS_ROOT_URL, REPOS_UUID> in the
 * repository, at revision REVISION.
 *
 * The file properties are given by the PROPS hash (which is
 * const char *name => const svn_string_t *).
 *
 * The last-change information is given by <CHANGED_REV, CHANGED_DATE,
 * CHANGED_AUTHOR>.
 *
 * The checksum of the file contents is given in CHECKSUM. An entry in
 * the pristine text base is NOT required when this API is called.
 *
 * If the translated size of the file (its contents, translated as defined
 * by its properties) is known, then pass it as TRANSLATED_SIZE. Otherwise,
 * pass SVN_INVALID_FILESIZE.
 *
 * All temporary allocations will be made in SCRATCH_POOL.
 */
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
                         apr_pool_t *scratch_pool);


/** Add or replace a symlink in the BASE tree.
 *
 * The symlink is located at LOCAL_ABSPATH on the local filesystem, and
 * corresponds to <REPOS_RELPATH, REPOS_ROOT_URL, REPOS_UUID> in the
 * repository, at revision REVISION.
 *
 * The symlink's properties are given by the PROPS hash (which is
 * const char *name => const svn_string_t *).
 *
 * The last-change information is given by <CHANGED_REV, CHANGED_DATE,
 * CHANGED_AUTHOR>.
 *
 * The target of the symlink is specified by TARGET.
 *
 * All temporary allocations will be made in SCRATCH_POOL.
 */
/* ### KFF: This is an interesting question, because currently
 * ### symlinks are versioned as regular files with the svn:special
 * ### property; then the file's text contents indicate that it is a
 * ### symlink and where that symlink points.  That's for portability:
 * ### you can check 'em out onto a platform that doesn't support
 * ### symlinks, and even modify the link and check it back in.  It's
 * ### a great solution; but then the question for wc-ng is:
 * ###
 * ### Suppose you check out a symlink on platform X and platform Y.
 * ### X supports symlinks; Y does not.  Should the wc-ng storage for
 * ### those two be the same?  I mean, on platform Y, the file is just
 * ### going to look and behave like a regular file.  It would be sort
 * ### of odd for the wc-ng storage for that file to be of a different
 * ### type from all the other files.  (On the other hand, maybe it's
 * ### weird today that the wc-1 storage for a working symlink is to
 * ### be like a regular file (i.e., regular text-base and whatnot).
 * ###
 * ### I'm still feeling my way around this problem; just pointing out
 * ### the issues.
 *
 * ### gjs: symlinks are stored in the database as first-class objects,
 * ###   rather than in the filesystem as "special" regular files. thus,
 * ###   all portability concerns are moot. higher-levels can figure out
 * ###   how to represent the link in ACTUAL. higher-levels can also
 * ###   deal with translating to/from the svn:special property and
 * ###   the plain-text file contents.
 * ### dlr: What about hard links? At minimum, mention in doc string.
 */
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
                            apr_pool_t *scratch_pool);


/** Create a node in the BASE tree that is present in name only.
 *
 * The new node will be located at LOCAL_ABSPATH, and correspond to the
 * repository node described by <REPOS_RELPATH, REPOS_ROOT_URL, REPOS_UUID>
 * at revision REVISION.
 *
 * The node's kind is described by KIND, and the reason for its absence
 * is specified by STATUS. Only three values are allowed for STATUS:
 *
 *   svn_wc__db_status_absent
 *   svn_wc__db_status_excluded
 *   svn_wc__db_status_not_present
 *
 * All temporary allocations will be made in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__db_base_add_absent_node(svn_wc__db_t *db,
                                const char *local_abspath,
                                const char *repos_relpath,
                                const char *repos_root_url,
                                const char *repos_uuid,
                                svn_revnum_t revision,
                                svn_wc__db_kind_t kind,
                                svn_wc__db_status_t status,
                                apr_pool_t *scratch_pool);


/** Remove a node from the BASE tree.
 *
 * The node to remove is indicated by LOCAL_ABSPATH from the local
 * filesystem.
 *
 * Note that no changes are made to the local filesystem; LOCAL_ABSPATH
 * is merely the key to figure out which BASE node to remove.
 *
 * If the node is a directory, then ALL child nodes will be removed
 * from the BASE tree, too.
 *
 * All temporary allocations will be made in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__db_base_remove(svn_wc__db_t *db,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool);


/** Retrieve information about a node in the BASE tree.
 *
 * For the BASE node implied by LOCAL_ABSPATH from the local filesystem,
 * return information in the provided OUT parameters. Each OUT parameter
 * may be NULL, indicating that specific item is not requested.
 *
 * If there is no information about this node, then SVN_ERR_WC_PATH_NOT_FOUND
 * will be returned.
 *
 * The OUT parameters, and their "not available" values are:
 *   STATUS           n/a (always available)
 *   KIND             n/a (always available)
 *   REVISION         SVN_INVALID_REVNUM
 *   REPOS_RELPATH    NULL (caller should scan up)
 *   REPOS_ROOT_URL   NULL (caller should scan up)
 *   REPOS_UUID       NULL (caller should scan up)
 *   CHANGED_REV      SVN_INVALID_REVNUM
 *   CHANGED_DATE     0
 *   CHANGED_AUTHOR   NULL
 *   LAST_MOD_TIME    0
 *   DEPTH            svn_depth_unknown
 *   CHECKSUM         NULL
 *   TRANSLATED_SIZE  SVN_INVALID_FILESIZE
 *   TARGET           NULL
 *   LOCK             NULL
 *
 * If the STATUS is normal, and the REPOS_* values are NULL, then the
 * caller should use svn_wc__db_scan_base_repos() to scan up the BASE
 * tree for the repository information.
 *
 * If DEPTH is requested, and the node is NOT a directory, then the
 * value will be set to svn_depth_unknown. If LOCAL_ABSPATH is a link,
 * it's up to the caller to resolve depth for the link's target.
 *
 * If CHECKSUM is requested, and the node is NOT a file, then it will
 * be set to NULL.
 *
 * If TRANSLATED_SIZE is requested, and the node is NOT a file, then
 * it will be set to SVN_INVALID_FILESIZE.
 *
 * If TARGET is requested, and the node is NOT a symlink, then it will
 * be set to NULL.
 *
 * All returned data will be allocated in RESULT_POOL. All temporary
 * allocations will be made in SCRATCH_POOL.
 */
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
                         apr_time_t *last_mod_time,
                         svn_depth_t *depth,
                         svn_checksum_t **checksum,
                         svn_filesize_t *translated_size,
                         const char **target,
                         svn_wc__db_lock_t **lock,
                         svn_wc__db_t *db,
                         const char *local_abspath,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool);


/** Return a property's value from a node in the BASE tree.
 *
 * This is a convenience function to return a single property from the
 * BASE tree node indicated by LOCAL_ABSPATH. The property's name is
 * given in PROPNAME, and the value returned in PROPVAL.
 *
 * All returned data will be allocated in RESULT_POOL. All temporary
 * allocations will be made in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__db_base_get_prop(const svn_string_t **propval,
                         svn_wc__db_t *db,
                         const char *local_abspath,
                         const char *propname,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool);


/** Return all properties of the given BASE tree node.
 *
 * All of the properties for the node indicated by LOCAL_ABSPATH will be
 * returned in PROPS as a mapping of const char * names to
 * const svn_string_t * values.
 *
 * All returned data will be allocated in RESULT_POOL. All temporary
 * allocations will be made in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__db_base_get_props(apr_hash_t **props,
                          svn_wc__db_t *db,
                          const char *local_abspath,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool);


/** Return a list of the BASE tree node's children's names.
 *
 * For the node indicated by LOCAL_ABSPATH, this function will return
 * the names of all of its children in the array CHILDREN. The array
 * elements are const char * values.
 *
 * If the node is not a directory, then SVN_ERR_WC_NOT_DIRECTORY will
 * be returned.
 *
 * All returned data will be allocated in RESULT_POOL. All temporary
 * allocations will be made in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__db_base_get_children(const apr_array_header_t **children,
                             svn_wc__db_t *db,
                             const char *local_abspath,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool);


/** Set the dav cache for LOCAL_ABSPATH to PROPS.  Use SCRATCH_POOL for
    temporary allocations. */
svn_error_t *
svn_wc__db_base_set_dav_cache(svn_wc__db_t *db,
                              const char *local_abspath,
                              const apr_hash_t *props,
                              apr_pool_t *scratch_pool);


/** Retrieve the dav cache for LOCAL_ABSPATH into *PROPS, allocated in
    RESULT_POOL.  Use SCRATCH_POOL for temporary allocations.  Return
    SVN_ERR_WC_PATH_NOT_FOUND if no dav cache can be located for
    LOCAL_ABSPATH in DB.  */
svn_error_t *
svn_wc__db_base_get_dav_cache(apr_hash_t **props,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool);


/* ### how to handle depth? empty != absent. thus, record depth on each
   ### directory? empty, files, immediates, infinity. recording depth
   ### doesn't seem to be part of BASE, but instructions on how to maintain
   ### the BASE/WORKING/ACTUAL trees. are there other instructional items?
*/

/* ### anything else needed for maintaining the BASE tree? */


/** @} */

/**
 * @defgroup svn_wc__db_pristine  Pristine ("text base") management
 * @{
 */

/**
 * Enumerated constants for how hard svn_wc__db_pristine_check() should
 * work on checking for the pristine file.
 */
typedef enum {

  /* ### bah. this is bogus. we open the sqlite database "all the time",
     ### and don't worry about optimizing that. so: given the db is always
     ### open, then the following modes are overengineered, premature
     ### optimizations. ... will clean up in a future rev.  */

  /* The caller wants to be sure the pristine file is present and usable.
     This is the typical mode to use.

     Implementation note: the SQLite database is opened (if not already)
       and its state is verified against the file in the filesystem. */
  svn_wc__db_checkmode_usable,

  /* The caller is performing just this one check. The implementation will
     optimize around the assumption no further calls to _check() will occur
     (but of course has no problem if they do).

     Note: this test is best used for detecting a *missing* file
     rather than for detecting a usable file.

     Implementation note: this will examine the presence of the pristine file
       in the filesystem. The SQLite database is untouched, though if it is
       (already) open, then it will be used instead. */
  svn_wc__db_checkmode_single,

  /* The caller is going to perform multiple calls, so the implementation
     should optimize its operation around that.

     Note: this test is best used for detecting a *missing* file
     rather than for detecting a usable file.

     Implementation note: the SQLite database will be opened (if not already),
     and all checks will simply look in the TEXT_BASE table to see if the
     given key is present. Note that the file may not be present. */
  svn_wc__db_checkmode_multi,

  /* Similar to _usable, but the file is checksum'd to ensure that it has
     not been corrupted in some way. */
  svn_wc__db_checkmode_validate

} svn_wc__db_checkmode_t;


/* ### @a contents may NOT be NULL. */
svn_error_t *
svn_wc__db_pristine_read(svn_stream_t **contents,
                         svn_wc__db_t *db,
                         const char *wri_abspath,
                         const svn_checksum_t *checksum,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool);


/* ### caller pushes contents into storage, keyed by @a checksum.
   ### note: if caller has a source stream, then it should use
   ###   svn_stream_copy3 to pull/push the content into storage. */
/* ### @a contents may NOT be NULL. */
svn_error_t *
svn_wc__db_pristine_write(svn_stream_t **contents,
                          svn_wc__db_t *db,
                          const char *wri_abspath,
                          const svn_checksum_t *checksum,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool);


/* ### get a tempdir to drop files for later installation. */
/* ### dlr: Why is a less specific temp dir insufficient? 
   ###  bh: See svn_wc__db_pristine_install() */
svn_error_t *
svn_wc__db_pristine_get_tempdir(const char **temp_dir_abspath,
                                svn_wc__db_t *db,
                                const char *wri_abspath,
                                apr_pool_t *result_pool,
                                apr_pool_t *scratch_pool);


/* ### Given file TEMPFILE_ABSPATH sitting in a tempdir (specified by
   ### _get_tempdir),
   ### install the sucker into the pristine datastore for the given checksum.
   ### This is used for files where we don't know the checksum ahead of
   ### time, so we drop it into a temp area first, computing the checksum
   ### as we write it there. */
svn_error_t *
svn_wc__db_pristine_install(svn_wc__db_t *db,
                            const char *tempfile_abspath,
                            const svn_checksum_t *checksum,
                            apr_pool_t *scratch_pool);


/* ### check for presence, according to the given mode (on how hard we
   ### should examine things)
*/
svn_error_t *
svn_wc__db_pristine_check(svn_boolean_t *present,
                          svn_wc__db_t *db,
                          const char *wri_abspath,
                          const svn_checksum_t *checksum,
                          svn_wc__db_checkmode_t mode,
                          apr_pool_t *scratch_pool);


/* ### if _check() returns "corrupted pristine file", then this function
   ### can be used to repair it. It will attempt to restore integrity
   ### between the SQLite database and the filesystem. Failing that, then
   ### it will attempt to clean out the record and/or file. Failing that,
   ### then it will return SOME_ERROR. */
/* ### dlr: What is this the checksum of? */
svn_error_t *
svn_wc__db_pristine_repair(svn_wc__db_t *db,
                           const char *wri_abspath,
                           const svn_checksum_t *checksum,
                           apr_pool_t *scratch_pool);


/** @} */


/**
 * @defgroup svn_wc__db_repos  Repository information management
 * @{
 */

/**
 * Ensure an entry for the repository at REPOS_ROOT_URL with UUID exists
 * in DB for LOCAL_ABSPATH, either by finding the correct row, or inserting
 * a new row.  In either case return the id in *REPOS_ID.
 *
 * Use SCRATCH_POOL for temporary allocations.
 */
svn_error_t *
svn_wc__db_repos_ensure(apr_int64_t *repos_id,
                        svn_wc__db_t *db,
                        const char *local_abspath,
                        const char *repos_root_url,
                        const char *repos_uuid,
                        apr_pool_t *scratch_pool);
                        

/** @} */


/**
 * @defgroup svn_wc__db_op  Operations on WORKING tree
 * @{
 */

/* ### svn cp WCPATH WCPATH ... can copy mixed base/working around */
svn_error_t *
svn_wc__db_op_copy(svn_wc__db_t *db,
                   const char *src_abspath,
                   const char *dst_abspath,
                   apr_pool_t *scratch_pool);


/* ### svn cp URL WCPATH ... copies pure repos into wc. only this "root"
   ### metadata is present. caller needs to "set" all information recursively.
   ### and caller definitely has to populate ACTUAL. */
/* ### mark node as absent? adding children or props: auto-convert away
   ### from absent? ... or not "absent" but an "incomplete" status? */
/* ### this needs to be split out into four functions to add nodes of each
   ### kind. records copyfrom_* history, changed_* values, incomplete
   ### children, properties, checksum values, etc. clients should construct
   ### the full copied tree (maybe DEPTH can be used to avoid creating
   ### nodes for all children?). for the child nodes, their copyfrom_*
   ### information should be NULL.  */
svn_error_t *
svn_wc__db_op_copy_url(svn_wc__db_t *db,
                       const char *local_abspath,
                       const char *copyfrom_repos_relpath,
                       const char *copyfrom_root_url,
                       const char *copyfrom_uuid,
                       svn_revnum_t copyfrom_revision,
                       apr_pool_t *scratch_pool);


/* ### add a new versioned directory. a list of children is NOT passed
   ### since they are added in future, distinct calls to db_op_add_*.
   ### this is freshly added, so it has no properties.  */
svn_error_t *
svn_wc__db_op_add_directory(svn_wc__db_t *db,
                            const char *local_abspath,
                            apr_pool_t *scratch_pool);


/* ### as a new file, there are no properties. this file has no "pristine"
   ### contents, so a checksum [reference] is not required.  */
svn_error_t *
svn_wc__db_op_add_file(svn_wc__db_t *db,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool);


/* ### newly added symlinks have no properties.  */
svn_error_t *
svn_wc__db_op_add_symlink(svn_wc__db_t *db,
                          const char *local_abspath,
                          const char *target,
                          apr_pool_t *scratch_pool);


/* ### note: there is no db_op_set_prop() function. callers must read
   ### all the properties, change one, and write all the properties.  */

/* Set the props on the WORKING node for LOCAL_ABSPATH to PROPS.  This will
   overwrite whatever working props the node currently has.  PROPS maps
   property names of type "const char *" to values of type
   "const svn_string_t *".  Use SCRATCH_POOL for temporary allocations. */
svn_error_t *
svn_wc__db_op_set_props(svn_wc__db_t *db,
                        const char *local_abspath,
                        const apr_hash_t *props,
                        apr_pool_t *scratch_pool);


/* ### KFF: This handles files, dirs, symlinks, anything else? */
/* ### dlr: Does this support recursive dir deletes (e.g. _revert)? Document. */
svn_error_t *
svn_wc__db_op_delete(svn_wc__db_t *db,
                     const char *local_abspath,
                     apr_pool_t *scratch_pool);


/* ### KFF: Would like to know behavior when dst_path already exists
 * ### and is a) a dir or b) a non-dir. */
svn_error_t *
svn_wc__db_op_move(svn_wc__db_t *db,
                   const char *src_abspath,
                   const char *dst_abspath,
                   apr_pool_t *scratch_pool);


/* ### mark PATH as (possibly) modified. "svn edit" ... right API here? */
svn_error_t *
svn_wc__db_op_modified(svn_wc__db_t *db,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool);


/* ### use NULL to remove from a changelist.  */
svn_error_t *
svn_wc__db_op_set_changelist(svn_wc__db_t *db,
                             const char *local_abspath,
                             const char *changelist,
                             apr_pool_t *scratch_pool);


/* ### caller maintains ACTUAL. we're just recording state. */
/* ### we probably need to record details of the conflict. how? */
svn_error_t *
svn_wc__db_op_mark_conflict(svn_wc__db_t *db,
                            const char *local_abspath,
                            apr_pool_t *scratch_pool);


/* ### caller maintains ACTUAL, and how the resolution occurred. we're just
   ### recording state. */
svn_error_t *
svn_wc__db_op_mark_resolved(svn_wc__db_t *db,
                            const char *local_abspath,
                            apr_pool_t *scratch_pool);


svn_error_t *
svn_wc__db_op_revert(svn_wc__db_t *db,
                     const char *local_abspath,
                     svn_depth_t depth,
                     apr_pool_t *scratch_pool);


/** Invalidate the last mod time cache for the appropriate BASE node
 * for LOCAL_ABSPATH in DB.
 *
 * Use SCRATCH_POOL for any temporary allocations.
 */
svn_error_t *
svn_wc__db_op_invalidate_last_mod_time(svn_wc__db_t *db,
                                       const char *local_abspath,
                                       apr_pool_t *scratch_pool);


/** Get any tree conflict associated with LOCAL_ABSPATH in DB, and put it
 * in *TREE_CONFLICT, allocated in RESULT_POOL.
 *
 * Use SCRATCH_POOL for any temporary allocations.
 */
svn_error_t *
svn_wc__db_op_read_tree_conflict(svn_wc_conflict_description2_t **tree_conflict,
                                 svn_wc__db_t *db,
                                 const char *local_abspath,
                                 apr_pool_t *result_pool,
                                 apr_pool_t *scratch_pool);


/** Set the tree conflict on LOCAL_ABSPATH in DB to TREE_CONFLICT.  Use
 * NULL to remove a tree conflict.
 *
 * Use SCRATCH_POOL for any temporary allocations.
 */
/* ### can this also record text/prop conflicts? drop "tree"? */
/* ### dunno if it can, but it definately should be able to. */
svn_error_t *
svn_wc__db_op_set_tree_conflict(svn_wc__db_t *db,
                                const char *local_abspath,
                                const svn_wc_conflict_description2_t *tree_conflict,
                                apr_pool_t *scratch_pool);


/* ### status */


/** @} */

/**
 * @defgroup svn_wc__db_read  Read operations on the BASE/WORKING tree
 * @{
 *
 * These functions query information about nodes in ACTUAL, and returns
 * the requested information from the appropriate ACTUAL, WORKING, or
 * BASE tree.
 *
 * For example, asking for the checksum of the pristine version will
 * return the one recorded in WORKING, or if no WORKING node exists, then
 * the checksum comes from BASE.
 */

/** Retrieve information about a node.
 *
 * For the node implied by LOCAL_ABSPATH from the local filesystem, return
 * information in the provided OUT parameters. Each OUT parameter may be
 * NULL, indicating that specific item is not requested.
 *
 * The information returned comes from the BASE tree, as possibly modified
 * by the WORKING and ACTUAL trees.
 *
 * If there is no information about the node, then SVN_ERR_WC_PATH_NOT_FOUND
 * will be returned.
 *
 * The OUT parameters, and their "not available" values are:
 *   STATUS                  n/a (always available)
 *   KIND                    n/a (always available)
 *   REVISION                SVN_INVALID_REVNUM
 *   REPOS_RELPATH           NULL
 *   REPOS_ROOT_URL          NULL
 *   REPOS_UUID              NULL
 *   CHANGED_REV             SVN_INVALID_REVNUM
 *   CHANGED_DATE            0
 *   CHANGED_AUTHOR          NULL
 *   LAST_MOD_TIME           0
 *   DEPTH                   svn_depth_unknown
 *   CHECKSUM                NULL
 *   TRANSLATED_SIZE         SVN_INVALID_FILESIZE
 *   TARGET                  NULL
 *   CHANGELIST              NULL
 *   ORIGINAL_REPOS_RELPATH  NULL
 *   ORIGINAL_ROOT_URL       NULL
 *   ORIGINAL_UUID           NULL
 *   ORIGINAL_REVISION       SVN_INVALID_REVNUM
 *   TEXT_MOD                n/a (always available)
 *   PROPS_MOD               n/a (always available)
 *   BASE_SHADOWED           n/a (always available)
 *   CONFLICT_OLD            NULL
 *   CONFLICT_NEW            NULL
 *   CONFLICT_WORKING        NULL
 *   PROP_REJECT_FILE        NULL
 *   LOCK                    NULL
 *   TREE_CONFLICT_DATA      NULL
 *
 * If DEPTH is requested, and the node is NOT a directory, then
 * the value will be set to svn_depth_unknown.
 *
 * If CHECKSUM is requested, and the node is NOT a file, then it will
 * be set to NULL.
 *
 * If TRANSLATED_SIZE is requested, and the node is NOT a file, then
 * it will be set to SVN_INVALID_FILESIZE.
 *
 * If TARGET is requested, and the node is NOT a symlink, then it will
 * be set to NULL.
 *
 * ### add information about the need to scan upwards to get a complete
 * ### picture of the state of this node.
 *
 * ### add some documentation about OUT parameter values based on STATUS ??
 *
 * ### the TEXT_MOD may become an enumerated value at some point to
 * ### indicate different states of knowledge about text modifications.
 * ### for example, an "svn edit" command in the future might set a
 * ### flag indicating adminstratively-defined modification. and/or we
 * ### might have a status indicating that we saw it was modified while
 * ### performing a filesystem traversal.
 *
 * All returned data will be allocated in RESULT_POOL. All temporary
 * allocations will be made in SCRATCH_POOL.
 */
/* ### old docco. needs to be incorporated as appropriate. there is
   ### some pending, potential changes to the definition of this API,
   ### so not worrying about it just yet.

   ### if the node has not been committed (after adding):
   ###   revision will be SVN_INVALID_REVNUM
   ###   repos_* will be NULL
   ###   changed_rev will be SVN_INVALID_REVNUM
   ###   changed_date will be 0
   ###   changed_author will be NULLn
   ###   status will be svn_wc__db_status_added
   ###   text_mod will be TRUE
   ###   prop_mod will be TRUE if any props have been set
   ###   base_shadowed will be FALSE

   ### if the node is not a copy, or a move destination:
   ###   original_repos_path will be NULL
   ###   original_root_url will be NULL
   ###   original_uuid will be NULL
   ###   original_revision will be SVN_INVALID_REVNUM

   ### KFF: The position of 'db' in the parameter list is sort of
   ### floating around (e.g., compare this func with the next one).
   ### Would be nice to keep it consistent.  For example, it always
   ### comes first, or always comes first after any result params, or
   ### whatever.

   ### note that @a base_shadowed can be derived. if the status specifies
   ### an add/copy/move *and* there is a corresponding node in BASE, then
   ### the BASE has been deleted to open the way for this node.
*/
svn_error_t *
svn_wc__db_read_info(svn_wc__db_status_t *status,  /* ### derived */
                     svn_wc__db_kind_t *kind,
                     svn_revnum_t *revision,
                     const char **repos_relpath,
                     const char **repos_root_url,
                     const char **repos_uuid,
                     svn_revnum_t *changed_rev,
                     apr_time_t *changed_date,
                     const char **changed_author,
                     apr_time_t *last_mod_time,
                     svn_depth_t *depth,  /* ### dirs only */
                     svn_checksum_t **checksum,
                     svn_filesize_t *translated_size,
                     const char **target,
                     const char **changelist,

                     /* ### the following fields if copied/moved (history) */
                     const char **original_repos_relpath,
                     const char **original_root_url,
                     const char **original_uuid,
                     svn_revnum_t *original_revision,

                     /* ### the followed are derived fields */
                     svn_boolean_t *text_mod,  /* ### possibly modified */
                     svn_boolean_t *props_mod,
                     svn_boolean_t *base_shadowed,  /* ### WORKING shadows a
                                                       ### deleted BASE? */

                     const char **conflict_old,
                     const char **conflict_new,
                     const char **conflict_working,
                     const char **prop_reject_file,  /* ### is this right? */

                     svn_wc__db_lock_t **lock,

                     /* ### this should eventually turn into a 
                        ### svn_wc_conflict_description2_t, but for the time
                        ### being, we're just going to return the raw text
                        ### and let the caller deal with it. */
                     const char **tree_conflict_data,

                     svn_wc__db_t *db,
                     const char *local_abspath,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool);


/** Return a property's value for a node, first using ACTUAL, then
 * WORKING, then BASE.  The property's name is given in PROPNAME, and
 * the value returned in PROPVAL.
 *
 * All returned data will be allocated in RESULT_POOL. All temporary
 * allocations will be made in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__db_read_prop(const svn_string_t **propval,
                     svn_wc__db_t *db,
                     const char *local_abspath,
                     const char *propname,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool);


/* Read into PROPS the properties for LOCAL_ABSPATH in DB.  This first check
   the ACTUAL node, then the WORKING node, and finally the BASE node for
   properties.  PROPS maps property names of type "const char *" to values
   of type "const svn_string_t *".
   
   Allocate PROPS in RESULT_POOL and do temporary allocations
   in SCRATCH_POOL. */
svn_error_t *
svn_wc__db_read_props(apr_hash_t **props,
                      svn_wc__db_t *db,
                      const char *local_abspath,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool);


/* Read into PROPS the properties for LOCAL_ABSPATH in DB.  This first check
   the WORKING node, and then the BASE node for properties.  PROPS maps
   property names of type "const char *" to values of type
   "const svn_string_t *".
   
   Allocate PROPS in RESULT_POOL and do temporary allocations
   in SCRATCH_POOL. */
svn_error_t *
svn_wc__db_read_pristine_props(apr_hash_t **props,
                               svn_wc__db_t *db,
                               const char *local_abspath,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool);

/* Read into CHILDREN the basenames of the immediate children of
   LOCAL_ABSPATH in DB.
   
   Allocate *CHILDREN in RESULT_POOL and do temporary allocations in
   SCRATCH_POOL.

   ### return some basic info for each child? e.g. kind.
 * ### maybe the data in _read_get_info should be a structure, and this
 * ### can return a struct for each one.
 * ### however: _read_get_info can say "not interested", which isn't the
 * ###   case with a struct. thus, a struct requires fetching and/or
 * ###   computing all info.
 */
svn_error_t *
svn_wc__db_read_children(const apr_array_header_t **children,
                         svn_wc__db_t *db,
                         const char *local_abspath,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool);


/* Return the kind of the node in DB at LOCAL_ABSPATH. The WORKING tree will
   be examined first, then the BASE tree. If the node is not present in either
   tree and ALLOW_MISSING is TRUE, then svn_wc__db_kind_unknown is returned.
   If the node is missing and ALLOW_MISSING is FALSE, then it will return
   SVN_ERR_WC_PATH_NOT_FOUND.

   Uses SCRATCH_POOL for temporary allocations.  */
svn_error_t *
svn_wc__db_read_kind(svn_wc__db_kind_t *kind,
                     svn_wc__db_t *db,
                     const char *local_abspath,
                     svn_boolean_t allow_missing,
                     apr_pool_t *scratch_pool);


/* An analog to svn_wc__entry_is_hidden().  Set *HIDDEN to TRUE if
   LOCAL_ABSPATH in DB "is not present, and I haven't scheduled something
   over the top of it." */
svn_error_t *
svn_wc__db_node_hidden(svn_boolean_t *hidden,
                       svn_wc__db_t *db,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool);


/* ### changelists. return an array, or an iterator interface? how big
   ### are these things? are we okay with an in-memory array? examine other
   ### changelist usage -- we may already assume the list fits in memory.
  */


/** @} */


/**
 * @defgroup svn_wc__db_global  Operations that alter BASE and WORKING trees
 * @{
 */

/*
 * Associate LOCAL_DIR_ABSPATH, and all its children with the repository at
 * at REPOS_ROOT_URL.  The relative path to the repos root will not change,
 * just the repository root.  The repos uuid will also remain the same.
 * This also updates any locks which may exist for the node, as well as any
 * copyfrom repository information.
 *
 * Use SCRATCH_POOL for any temporary allocations.
 *
 * ### local_dir_abspath "should be" the wcroot or a switch root. all URLs
 * ### under this directory (depth=infinity) will be rewritten.
 *
 * ### This API had a depth parameter, which was removed, should it be
 * ### resurrected?  What's the purpose if we claim relocate is infinitely
 * ### recursive?
 *
 * ### Assuming the future ability to copy across repositories, should we
 * ### refrain from resetting the copyfrom information in this operation?
 *
 * ### SINGLE_DB is a temp argument, and should be TRUE if using compressed
 * ### metadata.  When *all* metadata gets compressed, it should disappear.
 */
svn_error_t *
svn_wc__db_global_relocate(svn_wc__db_t *db,
                           const char *local_dir_abspath,
                           const char *repos_root_url,
                           svn_boolean_t single_db,
                           apr_pool_t *scratch_pool);


/* ### collapse changes (for this node) from the trees into a new BASE node. */
svn_error_t *
svn_wc__db_global_commit(svn_wc__db_t *db,
                         const char *local_abspath,
                         svn_revnum_t new_revision,
                         apr_time_t new_date,
                         const char *new_author,
                         apr_pool_t *scratch_pool);


/* ### post-commit handling.
 * ### maybe multiple phases?
 * ### 1) mark a changelist as being-committed
 * ### 2) collect ACTUAL content, store for future use as TEXTBASE
 * ### 3) caller performs commit
 * ### 4) post-commit, integrate changelist into BASE
 */


/** @} */


/**
 * @defgroup svn_wc__db_lock  Function to manage the LOCKS table.
 * @{
 */

/** Add or replace LOCK for LOCAL_ABSPATH to DB.  */
svn_error_t *
svn_wc__db_lock_add(svn_wc__db_t *db,
                    const char *local_abspath,
                    const svn_wc__db_lock_t *lock,
                    apr_pool_t *scratch_pool);


/** Remove any lock for LOCAL_ABSPATH in DB.  */
svn_error_t *
svn_wc__db_lock_remove(svn_wc__db_t *db,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool);


/** @} */


/**
 * @defgroup svn_wc__db_scan  Functions to scan up a tree for further data.
 * @{
 */

/** Scan for a BASE node's repository information.
 *
 * In the typical case, a BASE node has unspecified repository information,
 * meaning that it is implied by its parent's information. When the info is
 * needed, this function can be used to scan up the BASE tree to find
 * the data.
 *
 * For the BASE node implied by LOCAL_ABSPATH, its location in the repository
 * returned in *REPOS_ROOT_URL and *REPOS_UUID will be returned in
 * *REPOS_RELPATH. Any of three OUT parameters may be NULL, indicating no
 * interest in that piece of information.
 *
 * All returned data will be allocated in RESULT_POOL. All temporary
 * allocations will be made in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__db_scan_base_repos(const char **repos_relpath,
                           const char **repos_root_url,
                           const char **repos_uuid,
                           svn_wc__db_t *db,
                           const char *local_abspath,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool);


/** Scan upwards for information about an addition to the WORKING tree.
 *
 * A WORKING node's status, as returned from svn_wc__db_read_info() can be
 * one of three states:
 *   svn_wc__db_status_added
 *   svn_wc__db_status_deleted
 *   svn_wc__db_status_incomplete
 *
 * (the "normal" and absent statuses only refer to unshadowed BASE nodes)
 *
 * If the node is in the "added" state, then this function can refine that
 * status into one of three possible states:
 *
 *   svn_wc__db_status_added -- this NODE is a simple add without history.
 *     OP_ROOT_ABSPATH will be set to the topmost node in the added subtree
 *     (implying its parent will be an unshadowed BASE node). The REPOS_*
 *     values will be implied by that ancestor BASE node and this node's
 *     position in the added subtree. ORIGINAL_* will be set to their
 *     NULL values (and SVN_INVALID_REVNUM for ORIGINAL_REVISION).
 *
 *   svn_wc__db_status_copied -- this NODE is the root or child of a copy.
 *     The root of the copy will be stored in OP_ROOT_ABSPATH. Note that
 *     the parent of the operation root could be another WORKING node (from
 *     an add, copy, or move). The REPOS_* values will be implied by the
 *     ancestor unshadowed BASE node. ORIGINAL_* will indicate the source
 *     of the copy.
 *
 *   svn_wc__db_status_moved_here -- this NODE arrived as a result of a move.
 *     The root of the moved nodes will be stored in OP_ROOT_ABSPATH.
 *     Similar to the copied state, its parent may be a WORKING node or a
 *     BASE node. And again, the REPOS_* values are implied by this node's
 *     position in the subtree under the ancestor unshadowed BASE node.
 *     ORIGINAL_* will indicate the source of the move.
 *
 * All OUT parameters may be set to NULL to indicate a lack of interest in
 * that piece of information.
 *
 * ORIGINAL_REPOS_RELPATH will refer to the *root* of the operation. It
 * does *not* correspond to the node given by LOCAL_ABSPATH. The caller
 * can use the suffix on LOCAL_ABSPATH (relative to OP_ROOT_ABSPATH) in
 * order to compute the source node which corresponds to LOCAL_ABSPATH.
 *
 * If the node given by LOCAL_ABSPATH does not have changes recorded in
 * the WORKING tree, then SVN_ERR_WC_PATH_NOT_FOUND is returned. If it
 * doesn't have an "added" status, then SVN_ERR_WC_PATH_UNEXPECTED_STATUS
 * will be returned.
 *
 * All returned data will be allocated in RESULT_POOL. All temporary
 * allocations will be made in SCRATCH_POOL.
 */
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
                         apr_pool_t *scratch_pool);


/** Scan upwards for additional information about a deleted node.
 *
 * When a deleted node is discovered in the WORKING tree, the situation
 * may be quite complex. This function will provide the information to
 * resolve the circumstances of the deletion.
 *
 * For discussion purposes, we will start with the most complex example
 * and then demonstrate simplified examples. Consider node B/W/D/N has been
 * found as deleted. B is an unmodified directory (thus, only in BASE). W is
 * "replacement" content that exists in WORKING, shadowing a similar B/W
 * directory in BASE. D is a deleted subtree in the WORKING tree, and N is
 * the deleted node.
 *
 * In this example, BASE_DEL_ABSPATH will bet set to B/W. That is the root of
 * the BASE tree (implicitly) deleted by the replacement. BASE_REPLACED will
 * be set to TRUE since B/W replaces the BASE node at B/W. WORK_DEL_ABSPATH
 * will be set to the subtree deleted within the replacement; in this case,
 * B/W/D. No move-away took place, so MOVED_TO_ABSPATH is set to NULL.
 *
 * In another scenario, B/W was moved-away before W was put into the WORKING
 * tree through an add/copy/move-here. MOVED_TO_ABSPATH will indicate where
 * B/W was moved to. Note that further operations may have been performed
 * post-move, but that is not known or reported by this function.
 *
 * If BASE does not have a B/W, then the WORKING B/W is not a replacement,
 * but a simple add/copy/move-here. BASE_DEL_ABSPATH will be set to NULL,
 * and BASE_REPLACED will be set to FALSE.
 *
 * If B/W/D does not exist in the WORKING tree (we're only talking about a
 * deletion of nodes of the BASE tree), then deleting B/W/D would have marked
 * the subtree for deletion. BASE_DEL_ABSPATH will refer to B/W/D,
 * BASE_REPLACED will be FALSE, MOVED_TO_ABSPATH will be NULL, and
 * WORK_DEL_ABSPATH will be NULL.
 *
 * If the BASE node B/W/D was moved instead of deleted, then MOVED_TO_ABSPATH
 * would indicate the target location (and other OUT values as above).
 *
 * When the user deletes B/W/D from the WORKING tree, there are a few
 * additional considerations. If B/W is a simple addition (not a copy or
 * a move-here), then the deletion will simply remove the nodes from WORKING
 * and possibly leave behind "base-delete" markers in the WORKING tree.
 * If the source is a copy/moved-here, then the nodes are replaced with
 * deletion markers.
 *
 * If the user moves-away B/W/D from the WORKING tree, then behavior is
 * again dependent upon the origination of B/W. For a plain add, the nodes
 * simply move to the destination. For a copy, a deletion is made at B/W/D,
 * and a new copy (of a subtree of the original source) is made at the
 * destination. For a move-here, a deletion is made, and a copy is made at
 * the destination (we do not track multiple moves; the source is moved to
 * B/W, then B/W/D is deleted; then a copy is made at the destination;
 * however, note the double-move could have been performed by moving the
 * subtree first, then moving the source to B/W).
 *
 * There are three further considerations when resolving a deleted node:
 *
 *   If the BASE B/W/D was moved-away, then BASE_REL_ABSPATH will specify
 *   B/W/D as the root of the BASE deletion (not necessarily B/W as an
 *   implicit delete caused by a replacement; only the closest ancestor is
 *   reported). The other parameters will operate as normal, based on what
 *   is happening in the WORKING tree. Also note that ancestors of B/W/D
 *   may report additional, explicit moved-away status.
 *
 *   If the BASE B/W/D was deleted explicitly *and* B/W is a replacement,
 *   then the explicit deletion is subsumed by the implicit deletion that
 *   occurred with the B/W replacement. Thus, BASE_REL_ABSPATH will point
 *   to B/W as the root of the BASE deletion. IOW, we can detect the
 *   explicit move-away, but not an explicit deletion.
 *
 *   If B/W/D/N refers to a node present in the BASE tree, and B/W was
 *   replaced by a shallow subtree, then it is possible for N to be
 *   reported as deleted (from BASE) yet no deletions occurred in the
 *   WORKING tree above N. Thus, WORK_DEL_ABSPATH will be set to NULL.
 *
 *
 * Summary of OUT parameters:
 *
 * BASE_DEL_ABSPATH will specify the nearest ancestor of the explicit or
 * implicit deletion (if any) that applies to the BASE tree.
 *
 * BASE_REPLACED will specify whether the node at BASE_DEL_ABSPATH has
 * been replaced (shadowed) by nodes in the WORKING tree. If no BASE
 * deletion has occurred (BASE_DEL_ABSPATH is NULL, meaning the deletion
 * is confined to the WORKING TREE), then BASE_REPLACED will be FALSE.
 *
 * MOVED_TO_ABSPATH will specify the nearest ancestor that has moved-away,
 * if any. If no ancestors have been moved-away, then this is set to NULL.
 *
 * WORK_DEL_ABSPATH will specify the root of a deleted subtree within
 * content in the WORKING tree (note there is no concept of layered
 * delete operations in WORKING, so there is only one deletion root in
 * the ancestry).
 *
 *
 * NOTE: contrary to some APIs in wc_db.h, ALL of the OUT parameters must
 *   be received. You may NOT pass NULL for any parameter (otherwise, you
 *   would lose important information about the deletion).
 *
 * If the node given by LOCAL_ABSPATH does not exit, then
 * SVN_ERR_WC_PATH_NOT_FOUND is returned. If it doesn't have a "deleted"
 * status, then SVN_ERR_WC_PATH_UNEXPECTED_STATUS will be returned.
 *
 * All returned data will be allocated in RESULT_POOL. All temporary
 * allocations will be made in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__db_scan_deletion(const char **base_del_abspath,
                         svn_boolean_t *base_replaced,
                         const char **moved_to_abspath,
                         const char **work_del_abspath,
                         svn_wc__db_t *db,
                         const char *local_abspath,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool);
                         

/** @} */


/**
 * @defgroup svn_wc__db_upgrade  Functions for upgrading a working copy.
 * @{
 */

svn_error_t *
svn_wc__db_upgrade_begin(svn_sqlite__db_t **sdb,
                         const char *local_dir_abspath,
                         const char *repos_root_url,
                         const char *repos_uuid,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool);


svn_error_t *
svn_wc__db_upgrade_apply_dav_cache(svn_sqlite__db_t *sdb,
                                   apr_hash_t *cache_values,
                                   apr_pool_t *scratch_pool);


/* Get the repository identifier corresponding to REPOS_ROOT_URL from the
   database in SDB. The value is returned in *REPOS_ID. All allocations
   are allocated in SCRATCH_POOL.

   NOTE: the row in REPOSITORY must exist. If not, then SVN_ERR_WC_DB_ERROR
   is returned.

   ### unclear on whether/how this interface will stay/evolve.  */
svn_error_t *
svn_wc__db_upgrade_get_repos_id(apr_int64_t *repos_id,
                                svn_sqlite__db_t *sdb,
                                const char *repos_root_url,
                                apr_pool_t *scratch_pool);


svn_error_t *
svn_wc__db_upgrade_finish(const char *local_dir_abspath,
                          svn_sqlite__db_t *sdb,
                          apr_pool_t *scratch_pool);


/** @} */


/**
 * @defgroup svn_wc__db_wq  Work queue manipulation. see workqueue.h
 * @{
 */

/* In the WCROOT associated with DB and LOCAL_ABSPATH, add WORK_ITEM to the
   wcroot's work queue. Use SCRATCH_POOL for all temporary allocations.  */
svn_error_t *
svn_wc__db_wq_add(svn_wc__db_t *db,
                  const char *local_abspath,
                  const svn_skel_t *work_item,
                  apr_pool_t *scratch_pool);


/* In the WCROOT associated with DB and LOCAL_ABSPATH, fetch a work item that
   needs to be completed. Its identifier is returned in ID, and the data in
   WORK_ITEM.

   There is no particular ordering to the work items returned by this function.

   If there are no work items to be completed, then ID will be set to zero,
   and WORK_ITEM to NULL.

   RESULT_POOL will be used to allocate WORK_ITEM, and SCRATCH_POOL
   will be used for all temporary allocations.  */
svn_error_t *
svn_wc__db_wq_fetch(apr_uint64_t *id,
                    svn_skel_t **work_item,
                    svn_wc__db_t *db,
                    const char *local_abspath,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool);


/* In the WCROOT associated with DB and LOCAL_ABSPATH, mark work item ID as
   completed. If an error occurs, then it is unknown whether the work item
   has been marked as completed.

   Uses SCRATCH_POOL for all temporary allocations.  */
svn_error_t *
svn_wc__db_wq_completed(svn_wc__db_t *db,
                        const char *local_abspath,
                        apr_uint64_t id,
                        apr_pool_t *scratch_pool);

/** @} */

/**
 * @defgroup svn_wc__db_temp Various temporary functions during transition
 *
 * ### These functions SHOULD be completely removed before 1.7
 *
 * @{
 */

/** Removes all knowledge about @a local_dir_abspath from @a db. closing
 * file handles and removing cached information from @a db.
 *
 * This function should only called right before blowing away
 * a directory as it removes cached data from the wc_db without releasing
 * memory.
 *
 * After this function is called, a new working copy can be created at
 * @a local_dir_abspath.
 *
 * Perform temporary allocations in @a scratch_pool.
 */
svn_error_t *
svn_wc__db_temp_forget_directory(svn_wc__db_t *db,
                                 const char *local_dir_abspath,
                                 apr_pool_t *scratch_pool);


/**
 * A temporary API similar to svn_wc__db_base_add_directory() and
 * svn_wc__db_base_add_file(), in that it adds a subdirectory to the given
 * DB.  * Arguments are the same as those to svn_wc__db_base_add_directory().
 *
 * Note: Since the subdir node type is a fiction created to satisfy our
 * current backward-compat hacks, this is a temporary API expected to
 * disappear with that node type does.
 */
svn_error_t *
svn_wc__db_temp_base_add_subdir(svn_wc__db_t *db,
                                const char *local_abspath,
                                const char *repos_relpath,
                                const char *repos_root_url,
                                const char *repos_uuid,
                                svn_revnum_t revision,
                                const apr_hash_t *props,
                                svn_revnum_t changed_rev,
                                apr_time_t changed_date,
                                const char *changed_author,
                                svn_depth_t depth,
                                apr_pool_t *scratch_pool);

svn_error_t *
svn_wc__db_temp_is_dir_deleted(svn_boolean_t *not_present,
                               svn_revnum_t *base_revision,
                               svn_wc__db_t *db,
                               const char *local_abspath,
                               apr_pool_t *scratch_pool);

/* Removes all references of LOCAL_ABSPATH from its working copy
   using DB. When FLUSH_ENTRY_CACHE is set to TRUE, flush the related
   entries caches. */
svn_error_t *
svn_wc__db_temp_op_remove_entry(svn_wc__db_t *db,
                                const char *local_abspath,
                                svn_boolean_t flush_entry_cache,
                                apr_pool_t *scratch_pool);

/* Sets the depth of LOCAL_ABSPATH in its working copy to DEPTH
   using DB. When FLUSH_ENTRY_CACHE is set to TRUE, flush the related
   entries caches. */
svn_error_t *
svn_wc__db_temp_op_set_dir_depth(svn_wc__db_t *db,
                                 const char *local_abspath,
                                 svn_depth_t depth,
                                 svn_boolean_t flush_entry_cache,
                                 apr_pool_t *scratch_pool);


/* ### temp function. return the FORMAT for the directory LOCAL_ABSPATH.  */
svn_error_t *
svn_wc__db_temp_get_format(int *format,
                           svn_wc__db_t *db,
                           const char *local_dir_abspath,
                           apr_pool_t *scratch_pool);

/* ### reset any cached format version. it has probably changed.  */
svn_error_t *
svn_wc__db_temp_reset_format(int format,
                             svn_wc__db_t *db,
                             const char *local_dir_abspath,
                             apr_pool_t *scratch_pool);


/* ### temp functions to manage/store access batons within the DB.  */
svn_wc_adm_access_t *
svn_wc__db_temp_get_access(svn_wc__db_t *db,
                           const char *local_dir_abspath,
                           apr_pool_t *scratch_pool);
void
svn_wc__db_temp_set_access(svn_wc__db_t *db,
                           const char *local_dir_abspath,
                           svn_wc_adm_access_t *adm_access,
                           apr_pool_t *scratch_pool);
svn_error_t *
svn_wc__db_temp_close_access(svn_wc__db_t *db,
                             const char *local_dir_abspath,
                             svn_wc_adm_access_t *adm_access,
                             apr_pool_t *scratch_pool);
void
svn_wc__db_temp_clear_access(svn_wc__db_t *db,
                             const char *local_dir_abspath,
                             apr_pool_t *scratch_pool);

/* ### shallow hash: abspath -> svn_wc_adm_access_t *  */
apr_hash_t *
svn_wc__db_temp_get_all_access(svn_wc__db_t *db,
                               apr_pool_t *result_pool);

/* ### temp function to open an sqlite database to the appropriate location.
   ### The *only* reason for this function is because entries.c still
   ### manually hacks the sqlite database.

   ### No matter how tempted you may be DO NOT USE THIS FUNCTION!
   ### (if you do, gstein will hunt you down and burn your knee caps off
   ### in the middle of the night)
   ### "Bet on it." --gstein
*/
svn_error_t *
svn_wc__db_temp_get_sdb(svn_sqlite__db_t **db,
                        const char *local_dir_abspath,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool);

/** @} */


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_WC_DB_H */
