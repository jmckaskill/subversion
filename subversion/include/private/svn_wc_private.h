/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_wc_private.h
 * @brief The Subversion Working Copy Library - Internal routines
 *
 * Requires:
 *            - A working copy
 *
 * Provides:
 *            - Ability to manipulate working copy's versioned data.
 *            - Ability to manipulate working copy's administrative files.
 *
 * Used By:
 *            - Clients.
 */

#ifndef SVN_WC_PRIVATE_H
#define SVN_WC_PRIVATE_H

#include "svn_types.h"
#include "svn_wc.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* Return TRUE iff CLHASH (a hash whose keys are const char *
   changelist names) is NULL or if LOCAL_ABSPATH is part of a changelist in
   CLHASH. */
svn_boolean_t
svn_wc__changelist_match(svn_wc_context_t *wc_ctx,
                         const char *local_abspath,
                         const apr_hash_t *clhash,
                         apr_pool_t *scratch_pool);

/* Like svn_wc_get_update_editorX and svn_wc_get_status_editorX, but only
   allows updating a file external LOCAL_ABSPATH */
svn_error_t *
svn_wc__get_file_external_editor(const svn_delta_editor_t **editor,
                                 void **edit_baton,
                                 svn_revnum_t *target_revision,
                                 svn_wc_context_t *wc_ctx,
                                 const char *local_abspath,
                                 const char *wri_abspath,
                                 const char *url,
                                 const char *repos_root_url,
                                 const char *repos_uuid,
                                 svn_boolean_t use_commit_times,
                                 const char *diff3_cmd,
                                 const apr_array_header_t *preserved_exts,
                                 const char *record_ancestor_abspath,
                                 const char *recorded_url,
                                 const svn_opt_revision_t *recorded_peg_rev,
                                 const svn_opt_revision_t *recorded_rev,
                                 svn_wc_conflict_resolver_func2_t conflict_func,
                                 void *conflict_baton,
                                 svn_cancel_func_t cancel_func,
                                 void *cancel_baton,
                                 svn_wc_notify_func2_t notify_func,
                                 void *notify_baton,
                                 apr_pool_t *result_pool,
                                 apr_pool_t *scratch_pool);

/* Like svn_wc_crawl_revisionsX, but only supports updating a file external
   LOCAL_ABSPATH which may or may not exist yet. */
svn_error_t *
svn_wc__crawl_file_external(svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            const svn_ra_reporter3_t *reporter,
                            void *report_baton,
                            svn_boolean_t restore_files,
                            svn_boolean_t use_commit_times,
                            svn_cancel_func_t cancel_func,
                            void *cancel_baton,
                            svn_wc_notify_func2_t notify_func,
                            void *notify_baton,
                            apr_pool_t *scratch_pool);

/* Check if LOCAL_ABSPATH is an external in the working copy identified
   by WRI_ABSPATH. If not return SVN_ERR_WC_PATH_NOT_FOUND.

   If it is an external return more information on this external.

   If IGNORE_ENOENT, then set *external_kind to svn_node_none, when
   LOCAL_ABSPATH is not an external instead of returning an error.

   Here is an overview of how DEFINING_REVISION and
   DEFINING_OPERATIONAL_REVISION would be set for which kinds of externals
   definitions:

     svn:externals line   DEFINING_REV.       DEFINING_OP._REV.

         ^/foo@2 bar       2                   2
     -r1 ^/foo@2 bar       1                   2
     -r1 ^/foo   bar       1                  SVN_INVALID_REVNUM
         ^/foo   bar      SVN_INVALID_REVNUM  SVN_INVALID_REVNUM
         ^/foo@HEAD bar   SVN_INVALID_REVNUM  SVN_INVALID_REVNUM
     -rHEAD ^/foo bar     -- not a valid externals definition --
*/
svn_error_t *
svn_wc__read_external_info(svn_node_kind_t *external_kind,
                           const char **defining_abspath,
                           const char **defining_url,
                           svn_revnum_t *defining_operational_revision,
                           svn_revnum_t *defining_revision,
                           svn_wc_context_t *wc_ctx,
                           const char *wri_abspath,
                           const char *local_abspath,
                           svn_boolean_t ignore_enoent,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool);

/** See svn_wc__committable_externals_below(). */
typedef struct svn_wc__committable_external_info_t {

  /* The local absolute path where the external should be checked out. */
  const char *local_abspath;

  /* The relpath part of the source URL the external should be checked out
   * from. */
  const char *repos_relpath;

  /* The root URL part of the source URL the external should be checked out
   * from. */
  const char *repos_root_url;

  /* Set to either svn_kind_file or svn_kind_dir. */
  svn_kind_t kind;

} svn_wc__committable_external_info_t;

/* Add svn_wc__committable_external_info_t* items to *EXTERNALS, describing
 * 'committable' externals checked out below LOCAL_ABSPATH. Recursively find
 * all nested externals (externals defined inside externals).
 *
 * In this context, a 'committable' external belongs to the same repository as
 * LOCAL_ABSPATH, is not revision-pegged and is currently checked out in the
 * WC. (Local modifications are not tested for.)
 *
 * *EXTERNALS must be initialized either to NULL or to a pointer created with
 * apr_array_make(..., sizeof(svn_wc__committable_external_info_t *)). If
 * *EXTERNALS is initialized to NULL, an array will be allocated from
 * RESULT_POOL as necessary. If no committable externals are found,
 * *EXTERNALS is left unchanged.
 *
 * DEPTH limits the recursion below LOCAL_ABSPATH.
 *
 * This function will not find externals defined in some parent WC above
 * LOCAL_ABSPATH's WC-root.
 *
 * ###TODO: Add a WRI_ABSPATH (wc root indicator) separate from LOCAL_ABSPATH,
 * to allow searching any wc-root for externals under LOCAL_ABSPATH, not only
 * LOCAL_ABSPATH's most immediate wc-root. */
svn_error_t *
svn_wc__committable_externals_below(apr_array_header_t **externals,
                                    svn_wc_context_t *wc_ctx,
                                    const char *local_abspath,
                                    svn_depth_t depth,
                                    apr_pool_t *result_pool,
                                    apr_pool_t *scratch_pool);

/* Gets a mapping from const char * local abspaths of externals to the const
   char * local abspath of where they are defined for all externals defined
   at or below LOCAL_ABSPATH.

   ### Returns NULL in *EXTERNALS until we bumped to format 29.

   Allocate the result in RESULT_POOL and perform temporary allocations in
   SCRATCH_POOL. */
svn_error_t *
svn_wc__externals_defined_below(apr_hash_t **externals,
                                svn_wc_context_t *wc_ctx,
                                const char *local_abspath,
                                apr_pool_t *result_pool,
                                apr_pool_t *scratch_pool);


/* Registers a new external at LOCAL_ABSPATH in the working copy containing
   DEFINING_ABSPATH.

   The node is registered as defined on DEFINING_ABSPATH (must be an ancestor
   of LOCAL_ABSPATH) of kind KIND.

   The external is registered as from repository REPOS_ROOT_URL with uuid
   REPOS_UUID and the defining relative path REPOS_RELPATH.

   If the revision of the node is locked OPERATIONAL_REVISION and REVISION
   are the peg and normal revision; otherwise their value is
   SVN_INVALID_REVNUM.

   ### Only KIND svn_node_dir is supported.

   Perform temporary allocations in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__external_register(svn_wc_context_t *wc_ctx,
                          const char *defining_abspath,
                          const char *local_abspath,
                          svn_node_kind_t kind,
                          const char *repos_root_url,
                          const char *repos_uuid,
                          const char *repos_relpath,
                          svn_revnum_t operational_revision,
                          svn_revnum_t revision,
                          apr_pool_t *scratch_pool);

/* Remove the external at LOCAL_ABSPATH from the working copy identified by
   WRI_ABSPATH using WC_CTX.

   If not NULL, call CANCEL_FUNC with CANCEL_BATON to allow canceling while
   removing the working copy files.

   ### This function wraps svn_wc_remove_from_revision_control2().
 */
svn_error_t *
svn_wc__external_remove(svn_wc_context_t *wc_ctx,
                        const char *wri_abspath,
                        const char *local_abspath,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        apr_pool_t *scratch_pool);

/* Gather all svn:externals property values from the actual properties on
   directories below LOCAL_ABSPATH as a mapping of const char *local_abspath
   to const char * values.

   Use DEPTH as how it would be used to limit the externals property results
   on update. (So any depth < infinity will only read svn:externals on
   LOCAL_ABSPATH itself)

   If DEPTHS is not NULL, set *depths to an apr_hash_t* mapping the same
   local_abspaths to the const char * ambient depth of the node.

   Allocate the result in RESULT_POOL and perform temporary allocations in
   SCRATCH_POOL. */
svn_error_t *
svn_wc__externals_gather_definitions(apr_hash_t **externals,
                                     apr_hash_t **ambient_depths,
                                     svn_wc_context_t *wc_ctx,
                                     const char *local_abspath,
                                     svn_depth_t depth,
                                     apr_pool_t *result_pool,
                                     apr_pool_t *scratch_pool);

/* Close the DB for LOCAL_ABSPATH.  Perform temporary allocations in
   SCRATCH_POOL.

   Wraps svn_wc__db_drop_root(). */
svn_error_t *
svn_wc__close_db(const char *external_abspath,
                 svn_wc_context_t *wc_ctx,
                 apr_pool_t *scratch_pool);

/** Set @a *tree_conflict to a newly allocated @c
 * svn_wc_conflict_description_t structure describing the tree
 * conflict state of @a victim_abspath, or to @c NULL if @a victim_abspath
 * is not in a state of tree conflict. @a wc_ctx is a working copy context
 * used to access @a victim_path.  Allocate @a *tree_conflict in @a result_pool,
 * use @a scratch_pool for temporary allocations.
 */
svn_error_t *
svn_wc__get_tree_conflict(const svn_wc_conflict_description2_t **tree_conflict,
                          svn_wc_context_t *wc_ctx,
                          const char *victim_abspath,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool);

/** Record the tree conflict described by @a conflict in the WC for
 * @a conflict->local_abspath.  Use @a scratch_pool for all temporary
 * allocations.
 */
svn_error_t *
svn_wc__add_tree_conflict(svn_wc_context_t *wc_ctx,
                          const svn_wc_conflict_description2_t *conflict,
                          apr_pool_t *scratch_pool);

/* Remove any tree conflict on victim @a victim_abspath using @a wc_ctx.
 * (If there is no such conflict recorded, do nothing and return success.)
 *
 * Do all temporary allocations in @a scratch_pool.
 */
svn_error_t *
svn_wc__del_tree_conflict(svn_wc_context_t *wc_ctx,
                          const char *victim_abspath,
                          apr_pool_t *scratch_pool);

/** Like svn_wc_is_wc_root(), but it doesn't consider switched subdirs or
 * deleted entries as working copy roots.
 */
svn_error_t *
svn_wc__strictly_is_wc_root(svn_boolean_t *wc_root,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            apr_pool_t *scratch_pool);


/** Set @a *wcroot_abspath to the local abspath of the root of the
 * working copy in which @a local_abspath resides.
 */
svn_error_t *
svn_wc__get_wc_root(const char **wcroot_abspath,
                    svn_wc_context_t *wc_ctx,
                    const char *local_abspath,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool);

/**
 * The following are temporary APIs to aid in the transition from wc-1 to
 * wc-ng.  Use them for new development now, but they may be disappearing
 * before the 1.7 release.
 */


/*
 * Convert from svn_wc_conflict_description2_t to
 * svn_wc_conflict_description_t. This is needed by some backwards-compat
 * code in libsvn_client/ctx.c
 *
 * Allocate the result in RESULT_POOL.
 */
svn_wc_conflict_description_t *
svn_wc__cd2_to_cd(const svn_wc_conflict_description2_t *conflict,
                  apr_pool_t *result_pool);


/*
 * Convert from svn_wc_status3_t to svn_wc_status2_t.
 * Allocate the result in RESULT_POOL.
 */
svn_error_t *
svn_wc__status2_from_3(svn_wc_status2_t **status,
                       const svn_wc_status3_t *old_status,
                       svn_wc_context_t *wc_ctx,
                       const char *local_abspath,
                       apr_pool_t *result_pool,
                       apr_pool_t *scratch_pool);


/**
 * Set @a *children to a new array of the immediate children of the working
 * node at @a dir_abspath.  The elements of @a *children are (const char *)
 * absolute paths.
 *
 * Include children that are scheduled for deletion.  Iff @a show_hidden
 * is true, also include children that are 'excluded' or 'server-excluded' or
 * 'not-present'.
 *
 * Return every path that refers to a child of the working node at
 * @a dir_abspath.  Do not include a path just because it was a child of a
 * deleted directory that existed at @a dir_abspath if that directory is now
 * sheduled to be replaced by the working node at @a dir_abspath.
 *
 * Allocate @a *children in @a result_pool.  Use @a wc_ctx to access the
 * working copy, and @a scratch_pool for all temporary allocations.
 */
svn_error_t *
svn_wc__node_get_children_of_working_node(const apr_array_header_t **children,
                                          svn_wc_context_t *wc_ctx,
                                          const char *dir_abspath,
                                          svn_boolean_t show_hidden,
                                          apr_pool_t *result_pool,
                                          apr_pool_t *scratch_pool);

/**
 * Like svn_wc__node_get_children_of_working_node(), except also include any
 * path that was a child of a deleted directory that existed at
 * @a dir_abspath, even if that directory is now scheduled to be replaced by
 * the working node at @a dir_abspath.
 */
svn_error_t *
svn_wc__node_get_children(const apr_array_header_t **children,
                          svn_wc_context_t *wc_ctx,
                          const char *dir_abspath,
                          svn_boolean_t show_hidden,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool);


/**
 * Fetch the repository root information for the working version
 * of the node at @a local_abspath into @a *repos_root_url
 * and @a *repos_uuid. Use @a wc_ctx to access the working copy
 * for @a local_abspath, @a scratch_pool for all temporary allocations,
 * @a result_pool for result allocations. Note: the results will be NULL if
 * the node does not exist or is not under version control. If the node is
 * locally added, return the repository root it will have if committed.
 *
 * Either output argument may be NULL, indicating no interest.
 */
svn_error_t *
svn_wc__node_get_repos_info(const char **repos_root_url,
                            const char **repos_uuid,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool);



/**
 * Get the depth of @a local_abspath using @a wc_ctx.  If @a local_abspath is
 * not in the working copy, return @c SVN_ERR_WC_PATH_NOT_FOUND.
 */
svn_error_t *
svn_wc__node_get_depth(svn_depth_t *depth,
                       svn_wc_context_t *wc_ctx,
                       const char *local_abspath,
                       apr_pool_t *scratch_pool);

/**
 * Get the changed revision, date and author for @a local_abspath using @a
 * wc_ctx.  Allocate the return values in @a result_pool; use @a scratch_pool
 * for temporary allocations.  Any of the return pointers may be @c NULL, in
 * which case they are not set.
 *
 * If @a local_abspath is not in the working copy, return
 * @c SVN_ERR_WC_PATH_NOT_FOUND.
 */
svn_error_t *
svn_wc__node_get_changed_info(svn_revnum_t *changed_rev,
                              apr_time_t *changed_date,
                              const char **changed_author,
                              svn_wc_context_t *wc_ctx,
                              const char *local_abspath,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool);


/**
 * Set @a *url to the corresponding url for @a local_abspath, using @a wc_ctx.
 * If the node is added, return the url it will have in the repository.
 *
 * If @a local_abspath is not in the working copy, return
 * @c SVN_ERR_WC_PATH_NOT_FOUND.
 */
svn_error_t *
svn_wc__node_get_url(const char **url,
                     svn_wc_context_t *wc_ctx,
                     const char *local_abspath,
                     apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool);

/**
 * Retrieves the origin of the node as it is known in the repository. For
 * a copied node this retrieves where the node is copied from, for an added
 * node this returns NULL/INVALID outputs, and for any other node this
 * retrieves the repository location.
 *
 * All output arguments may be NULL.
 *
 * If @a is_copy is not NULL, sets @a *is_copy to TRUE if the origin is a copy
 * of the original node.
 *
 * If not NULL, sets @a revision, @a repos_relpath, @a repos_root_url and
 * @a repos_uuid to the original (if a copy) or their current values.
 *
 * If @a copy_root_abspath is not NULL, and @a *is_copy indicates that the
 * node was copied, set @a *copy_root_abspath to the local absolute path of
 * the root of the copied subtree containing the node. If the copied node is
 * a root by itself, @a *copy_root_abspath will match @a local_abspath (but
 * won't necessarily point to the same string in memory).
 *
 * If @a scan_deleted is TRUE, determine the origin of the deleted node. If
 * @a scan_deleted is FALSE, return NULL, SVN_INVALID_REVNUM or FALSE for
 * deleted nodes.
 *
 * Allocate the result in @a result_pool. Perform temporary allocations in
 * @a scratch_pool */
svn_error_t *
svn_wc__node_get_origin(svn_boolean_t *is_copy,
                        svn_revnum_t *revision,
                        const char **repos_relpath,
                        const char **repos_root_url,
                        const char **repos_uuid,
                        const char **copy_root_abspath,
                        svn_wc_context_t *wc_ctx,
                        const char *local_abspath,
                        svn_boolean_t scan_deleted,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool);


/**
 * Set @a *repos_relpath to the corresponding repos_relpath for @a
 * local_abspath, using @a wc_ctx. If the node is added, return the
 * repos_relpath it will have in the repository.
 *
 * If @a local_abspath is not in the working copy, return @c
 * SVN_ERR_WC_PATH_NOT_FOUND.
 * */
svn_error_t *
svn_wc__node_get_repos_relpath(const char **repos_relpath,
                               svn_wc_context_t *wc_ctx,
                               const char *local_abspath,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool);

/**
 * Set @a *is_deleted to TRUE if @a local_abspath is deleted, using
 * @a wc_ctx.  If @a local_abspath is not in the working copy, return
 * @c SVN_ERR_WC_PATH_NOT_FOUND.  Use @a scratch_pool for all temporary
 * allocations.
 */
svn_error_t *
svn_wc__node_is_status_deleted(svn_boolean_t *is_deleted,
                               svn_wc_context_t *wc_ctx,
                               const char *local_abspath,
                               apr_pool_t *scratch_pool);

/**
 * Set @a *deleted_ancestor_abspath to the root of the delete operation
 * that deleted @a local_abspath. If @a local_abspath itself was deleted
 * and has no deleted ancestor, @a *deleted_ancestor_abspath will equal
 * @a local_abspath. If @a local_abspath was not deleted,
 * set @a *deleted_ancestor_abspath to @c NULL.
 * @a *deleted_ancestor_abspath is allocated in @a result_pool.
 * Use @a scratch_pool for all temporary allocations.
 */
svn_error_t *
svn_wc__node_get_deleted_ancestor(const char **deleted_ancestor_abspath,
                                  svn_wc_context_t *wc_ctx,
                                  const char *local_abspath,
                                  apr_pool_t *result_pool,
                                  apr_pool_t *scratch_pool);

/**
 * Set @a *is_server_excluded to whether @a local_abspath has been
 * excluded by the server, using @a wc_ctx.  If @a local_abspath is not
 * in the working copy, return @c SVN_ERR_WC_PATH_NOT_FOUND.
 * Use @a scratch_pool for all temporary allocations.
 */
svn_error_t *
svn_wc__node_is_status_server_excluded(svn_boolean_t *is_server_excluded,
                                       svn_wc_context_t *wc_ctx,
                                       const char *local_abspath,
                                       apr_pool_t *scratch_pool);

/**
 * Set @a *is_not_present to whether the status of @a local_abspath is
 * #svn_wc__db_status_not_present, using @a wc_ctx.
 * If @a local_abspath is not in the working copy, return
 * @c SVN_ERR_WC_PATH_NOT_FOUND.  Use @a scratch_pool for all temporary
 * allocations.
 */
svn_error_t *
svn_wc__node_is_status_not_present(svn_boolean_t *is_not_present,
                                   svn_wc_context_t *wc_ctx,
                                   const char *local_abspath,
                                   apr_pool_t *scratch_pool);

/**
 * Set @a *is_excluded to whether the status of @a local_abspath is
 * #svn_wc__db_status_excluded, using @a wc_ctx.
 * If @a local_abspath is not in the working copy, return
 * @c SVN_ERR_WC_PATH_NOT_FOUND.  Use @a scratch_pool for all temporary
 * allocations.
 */
svn_error_t *
svn_wc__node_is_status_excluded(svn_boolean_t *is_excluded,
                                   svn_wc_context_t *wc_ctx,
                                   const char *local_abspath,
                                   apr_pool_t *scratch_pool);

/**
 * Set @a *is_added to whether @a local_abspath is added, using
 * @a wc_ctx.  If @a local_abspath is not in the working copy, return
 * @c SVN_ERR_WC_PATH_NOT_FOUND.  Use @a scratch_pool for all temporary
 * allocations.
 *
 * NOTE: "added" in this sense, means it was added, copied-here, or
 *   moved-here. This function provides NO information on whether this
 *   addition has replaced another node.
 *
 *   To be clear, this does NOT correspond to svn_wc_schedule_add.
 */
svn_error_t *
svn_wc__node_is_added(svn_boolean_t *is_added,
                      svn_wc_context_t *wc_ctx,
                      const char *local_abspath,
                      apr_pool_t *scratch_pool);

/**
 * Set @a *has_working to whether @a local_abspath has a working node (which
 * might shadow BASE nodes)
 *
 * This is a check similar to status = added or status = deleted.
 */
svn_error_t *
svn_wc__node_has_working(svn_boolean_t *has_working,
                         svn_wc_context_t *wc_ctx,
                         const char *local_abspath,
                         apr_pool_t *scratch_pool);


/**
 * Get the repository location of the base node at @a local_abspath.
 *
 * Set *REVISION, *REPOS_RELPATH, *REPOS_ROOT_URL and *REPOS_UUID to the
 * location that this node was checked out at or last updated/switched to,
 * regardless of any uncommitted changes (delete, replace and/or
 * copy-here/move-here).
 *
 * If there is no base node at @a local_abspath (such as when there is a
 * locally added/copied/moved-here node that is not part of a replace),
 * return @c SVN_INVALID_REVNUM/NULL/NULL/NULL.
 *
 * All output arguments may be NULL.
 *
 * Allocate the results in @a result_pool. Perform temporary allocations in
 * @a scratch_pool.
 */
svn_error_t *
svn_wc__node_get_base(svn_revnum_t *revision,
                      const char **repos_relpath,
                      const char **repos_root_url,
                      const char **repos_uuid,
                      svn_wc_context_t *wc_ctx,
                      const char *local_abspath,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool);


/* Get the working revision of @a local_abspath using @a wc_ctx. If @a
 * local_abspath is not in the working copy, return @c
 * SVN_ERR_WC_PATH_NOT_FOUND.
 *
 * This function is meant as a temporary solution for using the old-style
 * semantics of entries. It will handle any uncommitted changes (delete,
 * replace and/or copy-here/move-here).
 *
 * For a delete the @a revision is the BASE node of the operation root, e.g
 * the path that was deleted. But if the delete is  below an add, the
 * revision is set to SVN_INVALID_REVNUM. For an add, copy or move we return
 * SVN_INVALID_REVNUM. In case of a replacement, we return the BASE
 * revision.
 *
 * The @a changed_rev is set to the latest committed change to @a
 * local_abspath before or equal to @a revision, unless the node is
 * copied-here or moved-here. Then it is the revision of the latest committed
 * change before or equal to the copyfrom_rev.  NOTE, that we use
 * SVN_INVALID_REVNUM for a scheduled copy or move.
 *
 * The @a changed_date and @a changed_author are the ones associated with @a
 * changed_rev.
 */
svn_error_t *
svn_wc__node_get_pre_ng_status_data(svn_revnum_t *revision,
                                    svn_revnum_t *changed_rev,
                                    apr_time_t *changed_date,
                                    const char **changed_author,
                                    svn_wc_context_t *wc_ctx,
                                    const char *local_abspath,
                                    apr_pool_t *result_pool,
                                    apr_pool_t *scratch_pool);


/**
 * Return the location of the base for this node's next commit,
 * reflecting any local tree modifications affecting this node.
 *
 * Get the base location of @a local_abspath using @a wc_ctx.  If @a
 * local_abspath is not in the working copy, return @c
 * SVN_ERR_WC_PATH_NOT_FOUND.
 *
 * If this node has no uncommitted changes, return the same location as
 * svn_wc__node_get_base().
 *
 * If this node is moved-here or copied-here (possibly as part of a replace),
 * return the location of the copy/move source. Do the same even when the node
 * has been removed from a recursive copy (subpath excluded from the copy).
 *
 * Else, if this node is locally added, return SVN_INVALID_REVNUM/NULL, or
 * if locally deleted or replaced, return the revert-base location.
 */
svn_error_t *
svn_wc__node_get_commit_base(svn_revnum_t *revision,
                             const char **repos_relpath,
                             const char **repos_root_url,
                             const char **repos_uuid,
                             svn_wc_context_t *wc_ctx,
                             const char *local_abspath,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool);

/**
 * Fetch lock information (if any) for @a local_abspath using @a wc_ctx:
 *
 *   Set @a *lock_token to the lock token (or NULL)
 *   Set @a *lock_owner to the owner of the lock (or NULL)
 *   Set @a *lock_comment to the comment associated with the lock (or NULL)
 *   Set @a *lock_date to the timestamp of the lock (or 0)
 *
 * Any of the aforementioned return values may be NULL to indicate
 * that the caller doesn't care about those values.
 *
 * If @a local_abspath is not in the working copy, return @c
 * SVN_ERR_WC_PATH_NOT_FOUND.
 */
svn_error_t *
svn_wc__node_get_lock_info(const char **lock_token,
                           const char **lock_owner,
                           const char **lock_comment,
                           apr_time_t *lock_date,
                           svn_wc_context_t *wc_ctx,
                           const char *local_abspath,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool);

/**
 * Acquire a recursive write lock for @a local_abspath.  If @a lock_anchor
 * is true, determine if @a local_abspath has an anchor that should be locked
 * instead; otherwise, @a local_abspath must be a versioned directory.
 * Store the obtained lock in @a wc_ctx.
 *
 * If @a lock_root_abspath is not NULL, store the root of the lock in
 * @a *lock_root_abspath. If @a lock_root_abspath is NULL, then @a
 * lock_anchor must be FALSE.
 *
 * Returns @c SVN_ERR_WC_LOCKED if an existing lock is encountered, in
 * which case any locks acquired will have been released.
 *
 * If @a lock_anchor is TRUE and @a lock_root_abspath is not NULL, @a
 * lock_root_abspath will be set even when SVN_ERR_WC_LOCKED is returned.
 */
svn_error_t *
svn_wc__acquire_write_lock(const char **lock_root_abspath,
                           svn_wc_context_t *wc_ctx,
                           const char *local_abspath,
                           svn_boolean_t lock_anchor,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool);


/**
 * Recursively release write locks for @a local_abspath, using @a wc_ctx
 * for working copy access.  Only locks held by @a wc_ctx are released.
 * Locks are not removed if work queue items are present.
 *
 * If @a local_abspath is not the root of an owned SVN_ERR_WC_NOT_LOCKED
 * is returned.
 */
svn_error_t *
svn_wc__release_write_lock(svn_wc_context_t *wc_ctx,
                           const char *local_abspath,
                           apr_pool_t *scratch_pool);

/** A callback invoked by the svn_wc__call_with_write_lock() function.  */
typedef svn_error_t *(*svn_wc__with_write_lock_func_t)(void *baton,
                                                       apr_pool_t *result_pool,
                                                       apr_pool_t *scratch_pool);


/** Call function @a func while holding a write lock on
 * @a local_abspath. The @a baton, and @a result_pool and
 * @a scratch_pool, is passed @a func.
 *
 * If @a lock_anchor is TRUE, determine if @a local_abspath has an anchor
 * that should be locked instead.
 *
 * Use @a wc_ctx for working copy access.
 * The lock is guaranteed to be released after @a func returns.
 */
svn_error_t *
svn_wc__call_with_write_lock(svn_wc__with_write_lock_func_t func,
                             void *baton,
                             svn_wc_context_t *wc_ctx,
                             const char *local_abspath,
                             svn_boolean_t lock_anchor,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool);

/** Evaluate the expression @a expr while holding a write lock on
 * @a local_abspath.
 *
 * @a expr must yield an (svn_error_t *) error code.  If the error code
 * is not #SVN_NO_ERROR, cause the function using this macro to return
 * the error to its caller.
 *
 * If @a lock_anchor is TRUE, determine if @a local_abspath has an anchor
 * that should be locked instead.
 *
 * Use @a wc_ctx for working copy access.
 *
 * The lock is guaranteed to be released after evaluating @a expr.
 */
#define SVN_WC__CALL_WITH_WRITE_LOCK(expr, wc_ctx, local_abspath,             \
                                     lock_anchor, scratch_pool)               \
  do {                                                                        \
    svn_error_t *svn_wc__err1, *svn_wc__err2;                                 \
    const char *svn_wc__lock_root_abspath;                                    \
    SVN_ERR(svn_wc__acquire_write_lock(&svn_wc__lock_root_abspath, wc_ctx,    \
                                       local_abspath, lock_anchor,            \
                                       scratch_pool, scratch_pool));          \
    svn_wc__err1 = (expr);                                                    \
    svn_wc__err2 = svn_wc__release_write_lock(                                \
                     wc_ctx, svn_wc__lock_root_abspath, scratch_pool);        \
    SVN_ERR(svn_error_compose_create(svn_wc__err1, svn_wc__err2));            \
  } while (0)


/**
 * Calculates the schedule and copied status of a node as that would
 * have been stored in an svn_wc_entry_t instance.
 *
 * If not @c NULL, @a schedule and @a copied are set to their calculated
 * values.
 */
svn_error_t *
svn_wc__node_get_schedule(svn_wc_schedule_t *schedule,
                          svn_boolean_t *copied,
                          svn_wc_context_t *wc_ctx,
                          const char *local_abspath,
                          apr_pool_t *scratch_pool);

/** A callback invoked by svn_wc__prop_list_recursive().
 * It is equivalent to svn_proplist_receiver_t declared in svn_client.h,
 * but kept private within the svn_wc__ namespace because it is used within
 * the bowels of libsvn_wc which don't include svn_client.h.
 *
 * @since New in 1.7. */
typedef svn_error_t *(*svn_wc__proplist_receiver_t)(void *baton,
                                                    const char *local_abspath,
                                                    apr_hash_t *props,
                                                    apr_pool_t *scratch_pool);

/** Call @a receiver_func, passing @a receiver_baton, an absolute path, and
 * a hash table mapping <tt>const char *</tt> names onto <tt>const
 * svn_string_t *</tt> values for all the regular properties of the node
 * at @a local_abspath and any node beneath @a local_abspath within the
 * specified @a depth. @a receiver_fun must not be NULL.
 *
 * If @a propname is not NULL, the passed hash table will only contain
 * the property @a propname.
 *
 * If @a pristine is not @c TRUE, and @a base_props is FALSE show local
 * modifications to the properties.
 *
 * If a node has no properties, @a receiver_func is not called for the node.
 *
 * If @a changelists are non-NULL and non-empty, filter by them.
 *
 * Use @a wc_ctx to access the working copy, and @a scratch_pool for
 * temporary allocations.
 *
 * If the node at @a local_abspath does not exist,
 * #SVN_ERR_WC_PATH_NOT_FOUND is returned.
 *
 * @since New in 1.7.
 */
svn_error_t *
svn_wc__prop_list_recursive(svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            const char *propname,
                            svn_depth_t depth,
                            svn_boolean_t pristine,
                            const apr_array_header_t *changelists,
                            svn_wc__proplist_receiver_t receiver_func,
                            void *receiver_baton,
                            svn_cancel_func_t cancel_func,
                            void *cancel_baton,
                            apr_pool_t *scratch_pool);

/** Obtain a mapping of const char * local_abspaths to const svn_string_t*
 * property values in *VALUES, of all PROPNAME properties on LOCAL_ABSPATH
 * and its descendants.
 *
 * Allocate the result in RESULT_POOL, and perform temporary allocations in
 * SCRATCH_POOL.
 */
svn_error_t *
svn_wc__prop_retrieve_recursive(apr_hash_t **values,
                                svn_wc_context_t *wc_ctx,
                                const char *local_abspath,
                                const char *propname,
                                apr_pool_t *result_pool,
                                apr_pool_t *scratch_pool);

/**
 * For use by entries.c and entries-dump.c to read old-format working copies.
 */
svn_error_t *
svn_wc__read_entries_old(apr_hash_t **entries,
                         const char *dir_abspath,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool);

/**
 * Recursively clear the dav cache (wcprops) in @a wc_ctx for the tree
 * rooted at @a local_abspath.
 */
svn_error_t *
svn_wc__node_clear_dav_cache_recursive(svn_wc_context_t *wc_ctx,
                                       const char *local_abspath,
                                       apr_pool_t *scratch_pool);

/**
 * Set @a lock_tokens to a hash mapping <tt>const char *</tt> URL
 * to <tt>const char *</tt> lock tokens for every path at or under
 * @a local_abspath in @a wc_ctx which has such a lock token set on it.
 * Allocate the hash and all items therein from @a result_pool.
 */
svn_error_t *
svn_wc__node_get_lock_tokens_recursive(apr_hash_t **lock_tokens,
                                       svn_wc_context_t *wc_ctx,
                                       const char *local_abspath,
                                       apr_pool_t *result_pool,
                                       apr_pool_t *scratch_pool);

/* Set @a *min_revision and @a *max_revision to the lowest and highest revision
 * numbers found within @a local_abspath, using context @a wc_ctx.
 * If @a committed is TRUE, set @a *min_revision and @a *max_revision
 * to the lowest and highest comitted (i.e. "last changed") revision numbers,
 * respectively. Use @a scratch_pool for temporary allocations.
 *
 * Either of MIN_REVISION and MAX_REVISION may be passed as NULL if
 * the caller doesn't care about that return value.
 *
 * This function provides a subset of the functionality of
 * svn_wc_revision_status2() and is more efficient if the caller
 * doesn't need all information returned by svn_wc_revision_status2(). */
svn_error_t *
svn_wc__min_max_revisions(svn_revnum_t *min_revision,
                          svn_revnum_t *max_revision,
                          svn_wc_context_t *wc_ctx,
                          const char *local_abspath,
                          svn_boolean_t committed,
                          apr_pool_t *scratch_pool);

/* Indicate in @a is_switched whether any node beneath @a local_abspath
 * is switched, using context @a wc_ctx.
 * Use @a scratch_pool for temporary allocations.
 *
 * If @a trail_url is non-NULL, use it to determine if @a local_abspath itself
 * is switched.  It should be any trailing portion of @a local_abspath's
 * expected URL, long enough to include any parts that the caller considers
 * might be changed by a switch.  If it does not match the end of
 * @a local_abspath's actual URL, then report a "switched" status.
 *
 * This function provides a subset of the functionality of
 * svn_wc_revision_status2() and is more efficient if the caller
 * doesn't need all information returned by svn_wc_revision_status2(). */
svn_error_t *
svn_wc__has_switched_subtrees(svn_boolean_t *is_switched,
                              svn_wc_context_t *wc_ctx,
                              const char *local_abspath,
                              const char *trail_url,
                              apr_pool_t *scratch_pool);

/* Set @a *excluded_subtrees to a hash mapping <tt>const char *</tt>
 * local * absolute paths to <tt>const char *</tt> local absolute paths for
 * every path under @a local_abspath in @a wc_ctx which are excluded
 * by the server (e.g. because of authz) or the users.
 * If no excluded paths are found then @a *server_excluded_subtrees
 * is set to @c NULL.
 * Allocate the hash and all items therein from @a result_pool.
 */
svn_error_t *
svn_wc__get_excluded_subtrees(apr_hash_t **server_excluded_subtrees,
                              svn_wc_context_t *wc_ctx,
                              const char *local_abspath,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool);

/* Indicate in @a *is_modified whether the working copy has local
 * modifications, using context @a wc_ctx.
 * Use @a scratch_pool for temporary allocations.
 *
 * This function provides a subset of the functionality of
 * svn_wc_revision_status2() and is more efficient if the caller
 * doesn't need all information returned by svn_wc_revision_status2(). */
svn_error_t *
svn_wc__has_local_mods(svn_boolean_t *is_modified,
                       svn_wc_context_t *wc_ctx,
                       const char *local_abspath,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       apr_pool_t *scratch_pool);

/* Renames a working copy from @a from_abspath to @a dst_abspath and makes sure
   open handles are closed to allow this on all platforms.

   Summary: This avoids a file lock problem on wc.db on Windows, that is
            triggered by libsvn_client'ss copy to working copy code. */
svn_error_t *
svn_wc__rename_wc(svn_wc_context_t *wc_ctx,
                  const char *from_abspath,
                  const char *dst_abspath,
                  apr_pool_t *scratch_pool);

/* Set *TMPDIR_ABSPATH to a directory that is suitable for temporary
   files which may need to be moved (atomically and same-device) into
   the working copy indicated by WRI_ABSPATH.  */
svn_error_t *
svn_wc__get_tmpdir(const char **tmpdir_abspath,
                   svn_wc_context_t *wc_ctx,
                   const char *wri_abspath,
                   apr_pool_t *result_pool,
                   apr_pool_t *scratch_pool);

/* Gets information needed by the commit harvester.
 *
 * ### Currently this API is work in progress and is designed for just this
 * ### caller. It is certainly possible (and likely) that this function and
 * ### it's caller will eventually move into a wc and maybe wc_db api.
 */
svn_error_t *
svn_wc__node_get_commit_status(svn_boolean_t *added,
                               svn_boolean_t *deleted,
                               svn_boolean_t *is_replace_root,
                               svn_boolean_t *is_op_root,
                               svn_revnum_t *revision,
                               svn_revnum_t *original_revision,
                               const char **original_repos_relpath,
                               svn_wc_context_t *wc_ctx,
                               const char *local_abspath,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool);

/* Gets the md5 checksum for the pristine file identified by a sha1_checksum in the
   working copy identified by wri_abspath.

   Wraps svn_wc__db_pristine_get_md5().
 */
svn_error_t *
svn_wc__node_get_md5_from_sha1(const svn_checksum_t **md5_checksum,
                               svn_wc_context_t *wc_ctx,
                               const char *wri_abspath,
                               const svn_checksum_t *sha1_checksum,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool);

/* Install the file TEMPFILE_ABSPATH (which is sitting in a directory given by
   svn_wc__db_pristine_get_tempdir()) into the pristine data store, to be
   identified by the SHA-1 checksum of its contents, SHA1_CHECKSUM, and whose
   MD-5 checksum is MD5_CHECKSUM. */
svn_error_t *
svn_wc__node_pristine_install(svn_wc_context_t *wc_ctx,
                              const char *tempfile_abspath,
                              const svn_checksum_t *sha1_checksum,
                              const svn_checksum_t *md5_checksum,
                              apr_pool_t *scratch_pool);

/* Like svn_wc_get_pristine_contents2(), but keyed on the CHECKSUM
   rather than on the local absolute path of the working file.
   WRI_ABSPATH is any versioned path of the working copy in whose
   pristine database we'll be looking for these contents.  */
svn_error_t *
svn_wc__get_pristine_contents_by_checksum(svn_stream_t **contents,
                                          svn_wc_context_t *wc_ctx,
                                          const char *wri_abspath,
                                          const svn_checksum_t *checksum,
                                          apr_pool_t *result_pool,
                                          apr_pool_t *scratch_pool);

/* Set *TEMP_DIR_ABSPATH to a directory in which the caller should create
   a uniquely named file for later installation as a pristine text file.

   The directory is guaranteed to be one that svn_wc__node_pristine_install()
   can use: specifically, one from which it can atomically move the file.

   Allocate *TEMP_DIR_ABSPATH in RESULT_POOL. */
svn_error_t *
svn_wc__node_pristine_get_tempdir(const char **temp_dir_abspath,
                                  svn_wc_context_t *wc_ctx,
                                  const char *wri_abspath,
                                  apr_pool_t *result_pool,
                                  apr_pool_t *scratch_pool);

/* Gets an array of const char *repos_relpaths of descendants of LOCAL_ABSPATH,
 * which must be the op root of an addition, copy or move. The descendants
 * returned are at the same op_depth, but are to be deleted by the commit
 * processing because they are not present in the local copy.
 */
svn_error_t *
svn_wc__get_not_present_descendants(const apr_array_header_t **descendants,
                                    svn_wc_context_t *wc_ctx,
                                    const char *local_abspath,
                                    apr_pool_t *result_pool,
                                    apr_pool_t *scratch_pool);


/* Checks a node LOCAL_ABSPATH in WC_CTX for several kinds of obstructions
 * for tasks like merge processing.
 *
 * If a node is not obstructed it sets *OBSTRUCTION_STATE to
 * svn_wc_notify_state_inapplicable. If a node is obstructed or when its
 * direct parent does not exist or is deleted return _state_obstructed. When
 * a node doesn't exist but should exist return svn_wc_notify_state_missing.
 *
 * A node is also obstructed if it is marked excluded or server-excluded or when
 * an unversioned file or directory exists. And if NO_WCROOT_CHECK is FALSE,
 * the root of a working copy is also obstructed; this to allow detecting
 * obstructing working copies.
 *
 * If KIND is not NULL, set *KIND to the kind of node registered in the working
 * copy, or SVN_NODE_NONE if the node doesn't
 *
 * If ADDED is not NULL, set *ADDED to TRUE if the node is added. (Addition,
 * copy or moved).
 *
 * If DELETED is not NULL, set *DELETED to TRUE if the node is marked as
 * deleted in the working copy.
 *
 * All output arguments except OBSTRUCTION_STATE can be NULL to ommit the
 * result.
 *
 * This function performs temporary allocations in SCRATCH_POOL.
 */
svn_error_t *
svn_wc__check_for_obstructions(svn_wc_notify_state_t *obstruction_state,
                               svn_node_kind_t *kind,
                               svn_boolean_t *added,
                               svn_boolean_t *deleted,
                               svn_wc_context_t *wc_ctx,
                               const char *local_abspath,
                               svn_boolean_t no_wcroot_check,
                               apr_pool_t *scratch_pool);


/**
 * A structure which describes various system-generated metadata about
 * a working-copy path or URL.
 *
 * @note Fields may be added to the end of this structure in future
 * versions.  Therefore, users shouldn't allocate structures of this
 * type, to preserve binary compatibility.
 *
 * @since New in 1.7.
 */
typedef struct svn_wc__info2_t
{
  /** Where the item lives in the repository. */
  const char *URL;

  /** The root URL of the repository. */
  const char *repos_root_URL;

  /** The repository's UUID. */
  const char *repos_UUID;

  /** The revision of the object.  If the target is a working-copy
   * path, then this is its current working revision number.  If the target
   * is a URL, then this is the repository revision that it lives in. */
  svn_revnum_t rev;

  /** The node's kind. */
  svn_node_kind_t kind;

  /** The size of the file in the repository (untranslated,
   * e.g. without adjustment of line endings and keyword
   * expansion). Only applicable for file -- not directory -- URLs.
   * For working copy paths, @a size will be #SVN_INVALID_FILESIZE. */
  svn_filesize_t size;

  /** The last revision in which this object changed. */
  svn_revnum_t last_changed_rev;

  /** The date of the last_changed_rev. */
  apr_time_t last_changed_date;

  /** The author of the last_changed_rev. */
  const char *last_changed_author;

  /** An exclusive lock, if present.  Could be either local or remote. */
  svn_lock_t *lock;

  /* Possible information about the working copy, NULL if not valid. */
  struct svn_wc_info_t *wc_info;

} svn_wc__info2_t;

/** The callback invoked by info retrievers.  Each invocation
 * describes @a local_abspath with the information present in @a info.
 * Use @a scratch_pool for all temporary allocation.
 *
 * @since New in 1.7.
 */
typedef svn_error_t *(*svn_wc__info_receiver2_t)(void *baton,
                                                 const char *local_abspath,
                                                 const svn_wc__info2_t *info,
                                                 apr_pool_t *scratch_pool);

/* Walk the children of LOCAL_ABSPATH and push svn_wc__info2_t's through
   RECEIVER/RECEIVER_BATON.  Honor DEPTH while crawling children, and
   filter the pushed items against CHANGELISTS.

   If FETCH_EXCLUDED is TRUE, also fetch excluded nodes.
   If FETCH_ACTUAL_ONLY is TRUE, also fetch actual-only nodes. */
svn_error_t *
svn_wc__get_info(svn_wc_context_t *wc_ctx,
                 const char *local_abspath,
                 svn_depth_t depth,
                 svn_boolean_t fetch_excluded,
                 svn_boolean_t fetch_actual_only,
                 const apr_array_header_t *changelists,
                 svn_wc__info_receiver2_t receiver,
                 void *receiver_baton,
                 svn_cancel_func_t cancel_func,
                 void *cancel_baton,
                 apr_pool_t *scratch_pool);

/* Internal version of svn_wc_delete4(). It has one additional parameter,
 * MOVED_TO_ABSPATH. If not NULL, this parameter indicates that the
 * delete operation is the delete-half of a move. */
svn_error_t *
svn_wc__delete_internal(svn_wc_context_t *wc_ctx,
                        const char *local_abspath,
                        svn_boolean_t keep_local,
                        svn_boolean_t delete_unversioned_target,
                        const char *moved_to_abspath,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        svn_wc_notify_func2_t notify_func,
                        void *notify_baton,
                        apr_pool_t *scratch_pool);


/* Alternative version of svn_wc_delete4().
 * It can delete multiple TARGETS more efficiently (within a single sqlite
 * transaction per working copy), but lacks support for moves. */
svn_error_t *
svn_wc__delete_many(svn_wc_context_t *wc_ctx,
                    const apr_array_header_t *targets,
                    svn_boolean_t keep_local,
                    svn_boolean_t delete_unversioned_target,
                    svn_cancel_func_t cancel_func,
                    void *cancel_baton,
                    svn_wc_notify_func2_t notify_func,
                    void *notify_baton,
                    apr_pool_t *scratch_pool);


/* If the node at LOCAL_ABSPATH was moved away set *MOVED_TO_ABSPATH to
 * the absolute path of the copied move-target node, and *COPY_OP_ROOT_ABSPATH
 * to the absolute path of the root node of the copy operation.
 *
 * If the node was not moved, set *MOVED_TO_ABSPATH and *COPY_OP_ROOT_ABSPATH
 * to NULL.
 *
 * Either MOVED_TO_ABSPATH or OP_ROOT_ABSPATH may be NULL to indicate
 * that the caller is not interested in the result.
 */
svn_error_t *
svn_wc__node_was_moved_away(const char **moved_to_abspath,
                            const char **copy_op_root_abspath,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool);

/* If the node at LOCAL_ABSPATH was moved here set *MOVED_FROM_ABSPATH to
 * the absolute path of the deleted move-source node, and set
 * *DELETE_OP_ROOT_ABSPATH to the absolute path of the root node of the
 * delete operation.
 *
 * If the node was not moved, set *MOVED_FROM_ABSPATH and
 * *DELETE_OP_ROOT_ABSPATH to NULL.
 *
 * Either MOVED_FROM_ABSPATH or OP_ROOT_ABSPATH may be NULL to indicate
 * that the caller is not interested in the result.
 */
svn_error_t *
svn_wc__node_was_moved_here(const char **moved_from_abspath,
                            const char **delete_op_root_abspath,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool);

/* During an upgrade to wc-ng, supply known details about an existing
 * external.  The working copy will suck in and store the information supplied
 * about the existing external at @a local_abspath. */
svn_error_t *
svn_wc__upgrade_add_external_info(svn_wc_context_t *wc_ctx,
                                  const char *local_abspath,
                                  svn_node_kind_t kind,
                                  const char *def_local_abspath,
                                  const char *repos_relpath,
                                  const char *repos_root_url,
                                  const char *repos_uuid,
                                  svn_revnum_t def_peg_revision,
                                  svn_revnum_t def_revision,
                                  apr_pool_t *scratch_pool);

/* If the URL for @a item is relative, then using the repository root
   URL @a repos_root_url and the parent directory URL @parent_dir_url,
   resolve it into an absolute URL and save it in @a *resolved_url.

   Regardless if the URL is absolute or not, if there are no errors,
   the URL returned in @a *resolved_url will be canonicalized.

   The following relative URL formats are supported:

     ../    relative to the parent directory of the external
     ^/     relative to the repository root
     //     relative to the scheme
     /      relative to the server's hostname

   The ../ and ^/ relative URLs may use .. to remove path elements up
   to the server root.

   The external URL should not be canonicalized before calling this function,
   as otherwise the scheme relative URL '//host/some/path' would have been
   canonicalized to '/host/some/path' and we would not be able to match on
   the leading '//'. */
svn_error_t *
svn_wc__resolve_relative_external_url(const char **resolved_url,
                                      const svn_wc_external_item2_t *item,
                                      const char *repos_root_url,
                                      const char *parent_dir_url,
                                      apr_pool_t *result_pool,
                                      apr_pool_t *scratch_pool);


/**
 * Set @a *editor and @a *edit_baton to an editor that generates
 * #svn_wc_status3_t structures and sends them through @a status_func /
 * @a status_baton.  @a anchor_abspath is a working copy directory
 * directory which will be used as the root of our editor.  If @a
 * target_basename is not "", it represents a node in the @a anchor_abspath
 * which is the subject of the editor drive (otherwise, the @a
 * anchor_abspath is the subject).
 *
 * If @a set_locks_baton is non-@c NULL, it will be set to a baton that can
 * be used in a call to the svn_wc_status_set_repos_locks() function.
 *
 * Callers drive this editor to describe working copy out-of-dateness
 * with respect to the repository.  If this information is not
 * available or not desired, callers should simply call the
 * close_edit() function of the @a editor vtable.
 *
 * If the editor driver calls @a editor's set_target_revision() vtable
 * function, then when the edit drive is completed, @a *edit_revision
 * will contain the revision delivered via that interface.
 *
 * Assuming the target is a directory, then:
 *
 *   - If @a get_all is FALSE, then only locally-modified entries will be
 *     returned.  If TRUE, then all entries will be returned.
 *
 *   - If @a depth is #svn_depth_empty, a status structure will
 *     be returned for the target only; if #svn_depth_files, for the
 *     target and its immediate file children; if
 *     #svn_depth_immediates, for the target and its immediate
 *     children; if #svn_depth_infinity, for the target and
 *     everything underneath it, fully recursively.
 *
 *     If @a depth is #svn_depth_unknown, take depths from the
 *     working copy and behave as above in each directory's case.
 *
 *     If the given @a depth is incompatible with the depth found in a
 *     working copy directory, the found depth always governs.
 *
 * If @a no_ignore is set, statuses that would typically be ignored
 * will instead be reported.
 *
 * @a ignore_patterns is an array of file patterns matching
 * unversioned files to ignore for the purposes of status reporting,
 * or @c NULL if the default set of ignorable file patterns should be used.
 *
 * If @a cancel_func is non-NULL, call it with @a cancel_baton while building
 * the @a statushash to determine if the client has canceled the operation.
 *
 * If @a depth_as_sticky is set handle @a depth like when depth_is_sticky is
 * passed for updating. This will show excluded nodes show up as added in the
 * repository.
 *
 * If @a server_performs_filtering is TRUE, assume that the server handles
 * the ambient depth filtering, so this doesn't have to be handled in the
 * editor.
 *
 * Allocate the editor itself in @a result_pool, and use @a scratch_pool
 * for temporary allocations. The editor will do its temporary allocations
 * in a subpool of @a result_pool.
 *
 * @since New in 1.8.
 */
svn_error_t *
svn_wc__get_status_editor(const svn_delta_editor_t **editor,
                          void **edit_baton,
                          void **set_locks_baton,
                          svn_revnum_t *edit_revision,
                          svn_wc_context_t *wc_ctx,
                          const char *anchor_abspath,
                          const char *target_basename,
                          svn_depth_t depth,
                          svn_boolean_t get_all,
                          svn_boolean_t no_ignore,
                          svn_boolean_t depth_as_sticky,
                          svn_boolean_t server_performs_filtering,
                          const apr_array_header_t *ignore_patterns,
                          svn_wc_status_func4_t status_func,
                          void *status_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool);


/**
 * Set @a *editor and @a *edit_baton to an editor and baton for updating a
 * working copy.
 *
 * @a anchor_abspath is a local working copy directory, with a fully recursive
 * write lock in @a wc_ctx, which will be used as the root of our editor.
 *
 * @a target_basename is the entry in @a anchor_abspath that will actually be
 * updated, or the empty string if all of @a anchor_abspath should be updated.
 *
 * The editor invokes @a notify_func with @a notify_baton as the update
 * progresses, if @a notify_func is non-NULL.
 *
 * If @a cancel_func is non-NULL, the editor will invoke @a cancel_func with
 * @a cancel_baton as the update progresses to see if it should continue.
 *
 * If @a conflict_func is non-NULL, then invoke it with @a
 * conflict_baton whenever a conflict is encountered, giving the
 * callback a chance to resolve the conflict before the editor takes
 * more drastic measures (such as marking a file conflicted, or
 * bailing out of the update).
 *
 * If @a external_func is non-NULL, then invoke it with @a external_baton
 * whenever external changes are encountered, giving the callback a chance
 * to store the external information for processing.
 *
 * If @a diff3_cmd is non-NULL, then use it as the diff3 command for
 * any merging; otherwise, use the built-in merge code.
 *
 * @a preserved_exts is an array of filename patterns which, when
 * matched against the extensions of versioned files, determine for
 * which such files any related generated conflict files will preserve
 * the original file's extension as their own.  If a file's extension
 * does not match any of the patterns in @a preserved_exts (which is
 * certainly the case if @a preserved_exts is @c NULL or empty),
 * generated conflict files will carry Subversion's custom extensions.
 *
 * @a target_revision is a pointer to a revision location which, after
 * successful completion of the drive of this editor, will be
 * populated with the revision to which the working copy was updated.
 *
 * If @a use_commit_times is TRUE, then all edited/added files will
 * have their working timestamp set to the last-committed-time.  If
 * FALSE, the working files will be touched with the 'now' time.
 *
 * If @a allow_unver_obstructions is TRUE, then allow unversioned
 * obstructions when adding a path.
 *
 * If @a adds_as_modification is TRUE, a local addition at the same path
 * as an incoming addition of the same node kind results in a normal node
 * with a possible local modification, instead of a tree conflict.
 *
 * If @a depth is #svn_depth_infinity, update fully recursively.
 * Else if it is #svn_depth_immediates, update the uppermost
 * directory, its file entries, and the presence or absence of
 * subdirectories (but do not descend into the subdirectories).
 * Else if it is #svn_depth_files, update the uppermost directory
 * and its immediate file entries, but not subdirectories.
 * Else if it is #svn_depth_empty, update exactly the uppermost
 * target, and don't touch its entries.
 *
 * If @a depth_is_sticky is set and @a depth is not
 * #svn_depth_unknown, then in addition to updating PATHS, also set
 * their sticky ambient depth value to @a depth.
 *
 * If @a server_performs_filtering is TRUE, assume that the server handles
 * the ambient depth filtering, so this doesn't have to be handled in the
 * editor.
 *
 * If @a fetch_dirents_func is not NULL, the update editor may call this
 * callback, when asked to perform a depth restricted update. It will do this
 * before returning the editor to allow using the primary ra session for this.
 *
 * @since New in 1.8.
 */
svn_error_t *
svn_wc__get_update_editor(const svn_delta_editor_t **editor,
                          void **edit_baton,
                          svn_revnum_t *target_revision,
                          svn_wc_context_t *wc_ctx,
                          const char *anchor_abspath,
                          const char *target_basename,
                          svn_boolean_t use_commit_times,
                          svn_depth_t depth,
                          svn_boolean_t depth_is_sticky,
                          svn_boolean_t allow_unver_obstructions,
                          svn_boolean_t adds_as_modification,
                          svn_boolean_t server_performs_filtering,
                          svn_boolean_t clean_checkout,
                          const char *diff3_cmd,
                          const apr_array_header_t *preserved_exts,
                          svn_wc_dirents_func_t fetch_dirents_func,
                          void *fetch_dirents_baton,
                          svn_wc_conflict_resolver_func2_t conflict_func,
                          void *conflict_baton,
                          svn_wc_external_update_t external_func,
                          void *external_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool);


/**
 * A variant of svn_wc__get_update_editor().
 *
 * Set @a *editor and @a *edit_baton to an editor and baton for "switching"
 * a working copy to a new @a switch_url.  (Right now, this URL must be
 * within the same repository that the working copy already comes
 * from.)  @a switch_url must not be @c NULL.
 *
 * All other parameters behave as for svn_wc__get_update_editor().
 *
 * @since New in 1.8.
 */
svn_error_t *
svn_wc__get_switch_editor(const svn_delta_editor_t **editor,
                          void **edit_baton,
                          svn_revnum_t *target_revision,
                          svn_wc_context_t *wc_ctx,
                          const char *anchor_abspath,
                          const char *target_basename,
                          const char *switch_url,
                          svn_boolean_t use_commit_times,
                          svn_depth_t depth,
                          svn_boolean_t depth_is_sticky,
                          svn_boolean_t allow_unver_obstructions,
                          svn_boolean_t server_performs_filtering,
                          const char *diff3_cmd,
                          const apr_array_header_t *preserved_exts,
                          svn_wc_dirents_func_t fetch_dirents_func,
                          void *fetch_dirents_baton,
                          svn_wc_conflict_resolver_func2_t conflict_func,
                          void *conflict_baton,
                          svn_wc_external_update_t external_func,
                          void *external_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool);



/**
 * Return an @a editor/@a edit_baton for diffing a working copy against the
 * repository. The editor is allocated in @a result_pool; temporary
 * calculations are performed in @a scratch_pool.
 *
 * This editor supports diffing either the actual files and properties in the
 * working copy (when @a use_text_base is #FALSE), or the current pristine
 * information (when @a use_text_base is #TRUE) against the editor driver.
 *
 * @a anchor_abspath/@a target represent the base of the hierarchy to be
 * compared. The diff callback paths will be relative to this path.
 *
 * Diffs will be reported as valid relpaths, with @a anchor_abspath being
 * the root ("").
 *
 * @a callbacks/@a callback_baton is the callback table to use.
 *
 * If @a depth is #svn_depth_empty, just diff exactly @a target or
 * @a anchor_path if @a target is empty.  If #svn_depth_files then do the same
 * and for top-level file entries as well (if any).  If
 * #svn_depth_immediates, do the same as #svn_depth_files but also diff
 * top-level subdirectories at #svn_depth_empty.  If #svn_depth_infinity,
 * then diff fully recursively.
 *
 * @a ignore_ancestry determines whether paths that have discontinuous node
 * ancestry are treated as delete/add or as simple modifications.  If
 * @a ignore_ancestry is @c FALSE, then any discontinuous node ancestry will
 * result in the diff given as a full delete followed by an add.
 *
 * @a show_copies_as_adds determines whether paths added with history will
 * appear as a diff against their copy source, or whether such paths will
 * appear as if they were newly added in their entirety.
 *
 * If @a use_git_diff_format is TRUE, copied paths will be treated as added
 * if they weren't modified after being copied. This allows the callbacks
 * to generate appropriate --git diff headers for such files.
 *
 * Normally, the difference from repository->working_copy is shown.
 * If @a reverse_order is TRUE, then show working_copy->repository diffs.
 *
 * If @a cancel_func is non-NULL, it will be used along with @a cancel_baton
 * to periodically check if the client has canceled the operation.
 *
 * @a changelist_filter is an array of <tt>const char *</tt> changelist
 * names, used as a restrictive filter on items whose differences are
 * reported; that is, don't generate diffs about any item unless
 * it's a member of one of those changelists.  If @a changelist_filter is
 * empty (or altogether @c NULL), no changelist filtering occurs.
 *
  * If @a server_performs_filtering is TRUE, assume that the server handles
 * the ambient depth filtering, so this doesn't have to be handled in the
 * editor.
 *
 * @since New in 1.8.
 */
svn_error_t *
svn_wc__get_diff_editor(const svn_delta_editor_t **editor,
                        void **edit_baton,
                        svn_wc_context_t *wc_ctx,
                        const char *anchor_abspath,
                        const char *target,
                        svn_depth_t depth,
                        svn_boolean_t ignore_ancestry,
                        svn_boolean_t show_copies_as_adds,
                        svn_boolean_t use_git_diff_format,
                        svn_boolean_t use_text_base,
                        svn_boolean_t reverse_order,
                        svn_boolean_t server_performs_filtering,
                        const apr_array_header_t *changelist_filter,
                        const svn_wc_diff_callbacks4_t *callbacks,
                        void *callback_baton,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        apr_pool_t *result_pool,
                        apr_pool_t *scratch_pool);

/**
 * Assuming @a local_abspath itself or any of its children are under version
 * control or a tree conflict victim and in a state of conflict, take these
 * nodes out of this state. 
 *
 * If @a resolve_text is TRUE then any text conflict is resolved,
 * if @a resolve_tree is TRUE then any tree conflicts are resolved.
 * If @a resolve_prop is set to "" all property conflicts are resolved,
 * if it is set to any other string value, conflicts on that specific
 * property are resolved and when resolve_prop is NULL, no property
 * conflicts are resolved.
 *
 * If @a depth is #svn_depth_empty, act only on @a local_abspath; if
 * #svn_depth_files, resolve @a local_abspath and its conflicted file
 * children (if any); if #svn_depth_immediates, resolve @a local_abspath
 * and all its immediate conflicted children (both files and directories,
 * if any); if #svn_depth_infinity, resolve @a local_abspath and every
 * conflicted file or directory anywhere beneath it.
 *
 * If @a conflict_choice is #svn_wc_conflict_choose_base, resolve the
 * conflict with the old file contents; if
 * #svn_wc_conflict_choose_mine_full, use the original working contents;
 * if #svn_wc_conflict_choose_theirs_full, the new contents; and if
 * #svn_wc_conflict_choose_merged, don't change the contents at all,
 * just remove the conflict status, which is the pre-1.5 behavior.
 *
 * If @a conflict_choice is #svn_wc_conflict_choose_unspecified, invoke the
 * @a conflict_func with the @a conflict_baton argument to obtain a
 * resolution decision for each conflict.
 *
 * #svn_wc_conflict_choose_theirs_conflict and
 * #svn_wc_conflict_choose_mine_conflict are not legal for binary
 * files or properties.
 *
 * @a wc_ctx is a working copy context, with a write lock, for @a
 * local_abspath.
 *
 * The implementation details are opaque, as our "conflicted" criteria
 * might change over time.  (At the moment, this routine removes the
 * three fulltext 'backup' files and any .prej file created in a conflict,
 * and modifies @a local_abspath's entry.)
 *
 * If @a local_abspath is not under version control and not a tree
 * conflict, return #SVN_ERR_ENTRY_NOT_FOUND. If @a path isn't in a
 * state of conflict to begin with, do nothing, and return #SVN_NO_ERROR.
 *
 * If @c local_abspath was successfully taken out of a state of conflict,
 * report this information to @c notify_func (if non-@c NULL.)  If only
 * text, only property, or only tree conflict resolution was requested,
 * and it was successful, then success gets reported.
 *
 * Temporary allocations will be performed in @a scratch_pool.
 *
 * @since New in 1.8.
 */
svn_error_t *
svn_wc__resolve_conflicts(svn_wc_context_t *wc_ctx,
                          const char *local_abspath,
                          svn_depth_t depth,
                          svn_boolean_t resolve_text,
                          const char *resolve_prop,
                          svn_boolean_t resolve_tree,
                          svn_wc_conflict_choice_t conflict_choice,
                          svn_wc_conflict_resolver_func2_t conflict_func,
                          void *conflict_baton,
                          svn_cancel_func_t cancel_func,
                          void *cancel_baton,
                          svn_wc_notify_func2_t notify_func,
                          void *notify_baton,
                          apr_pool_t *scratch_pool);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_WC_PRIVATE_H */
