/*
 * client.h :  shared stuff internal to the client library.
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
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


#ifndef SVN_LIBSVN_CLIENT_H
#define SVN_LIBSVN_CLIENT_H


#include <apr_pools.h>

#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_ra.h"
#include "svn_client.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* Set *REVNUM to the revision number identified by REVISION.
 *
 * If REVISION->kind is svn_client_revision_number, just use
 * REVISION->value.number, ignoring PATH, RA_LIB, and SESSION.
 *
 * Else if REVISION->kind is svn_client_revision_committed,
 * svn_client_revision_previous, or svn_client_revision_base, or
 * svn_client_revision_working, then the revision can be identified
 * purely based on the working copy's administrative information for
 * PATH, so RA_LIB and SESSION are ignored.  If PATH is not under
 * revision control, return SVN_ERR_UNVERSIONED_RESOURCE, or if PATH
 * is null, return SVN_ERR_CLIENT_VERSIONED_PATH_REQUIRED.
 * 
 * Else if REVISION->kind is svn_client_revision_date or
 * svn_client_revision_head, then RA_LIB and SESSION are used to
 * retrieve the revision from the repository (using
 * REVISION->value.date in the former case), and PATH is ignored.  If
 * RA_LIB or SESSION is null, return SVN_ERR_CLIENT_RA_ACCESS_REQUIRED. 
 *
 * Else if REVISION->kind is svn_client_revision_unspecified, set
 * *REVNUM to SVN_INVALID_REVNUM.  
 *
 * Else return SVN_ERR_CLIENT_BAD_REVISION.
 * 
 * Use POOL for any temporary allocation.
 */
svn_error_t *
svn_client__get_revision_number (svn_revnum_t *revnum,
                                 svn_ra_plugin_t *ra_lib,
                                 void *session,
                                 const svn_client_revision_t *revision,
                                 const char *path,
                                 apr_pool_t *pool);


/* ---------------------------------------------------------------- */

/*** RA callbacks ***/


/* This is the baton that we pass to RA->open(), and is associated with
   the callback table we provide to RA. */
typedef struct
{
  /* This is provided by the calling application for handling authentication
     information for this session. */
  svn_client_auth_baton_t *auth_baton;

  /* Holds the directory that corresponds to the REPOS_URL at RA->open()
     time. When callbacks specify a relative path, they are joined with
     this base directory. */
  const char *base_dir;

  /* Record whether we should store the user/pass into the WC */
  svn_boolean_t do_store;

  /* An array of svn_client_commit_item_t * structures, present only
     during working copy commits. */
  apr_array_header_t *commit_items;

  /* The pool to use for session-related items. */
  apr_pool_t *pool;

} svn_client__callback_baton_t;


/* Open an RA session, returning the session baton in SESSION_BATON. The
   RA library to use is specified by RA_LIB.

   The root of the session is specified by BASE_URL and BASE_DIR.

   Additional control parameters:

      - COMMIT_ITEMS is an array of svn_client_commit_item_t *
        structures, present only for working copy commits, NULL otherwise.

      - DO_STORE indicates whether the RA layer should attempt to
        store authentication info.

      - USE_ADMIN indicates that the RA layer should create tempfiles
        in the administrative area instead of in the working copy itself.

      - READ_ONLY_WC indicates that the RA layer should not attempt
        modify the working copy directly.

   BASE_DIR may be NULL if the RA operation does not correspond to a
   working copy (in which case, DO_STORE and USE_ADMIN should both
   be FALSE).

   The calling application's authentication baton is provided in AUTH_BATON,
   and allocations related to this session are performed in POOL.  */
svn_error_t * svn_client__open_ra_session (void **session_baton,
                                           const svn_ra_plugin_t *ra_lib,
                                           const char *base_url,
                                           const char *base_dir,
                                           apr_array_header_t *commit_items,
                                           svn_boolean_t do_store,
                                           svn_boolean_t use_admin,
                                           svn_boolean_t read_only_wc,
                                           svn_client_auth_baton_t *auth_baton,
                                           apr_pool_t *pool);


/* Retrieve an AUTHENTICATOR/AUTH_BATON pair from the client,
   which represents the protocol METHOD.  */
svn_error_t * svn_client__get_authenticator (void **authenticator,
                                             void **auth_baton,
                                             enum svn_ra_auth_method method,
                                             void *callback_baton,
                                             apr_pool_t *pool);

/* ---------------------------------------------------------------- */

/*** Commit ***/

/* If REVISION or AUTHOR or DATE has a valid value, then allocate (in
   POOL) an svn_client_commit_info_t structure and populate it with
   those values (that is, copies of them allocated in POOL).  */
svn_client_commit_info_t *svn_client__make_commit_info (svn_revnum_t revision,
                                                        const char *author,
                                                        const char *date,
                                                        apr_pool_t *pool);

/* ---------------------------------------------------------------- */

/*** Status ***/

/* Verify that the path can be deleted without losing stuff, i.e. ensure
   that there are no modified or unversioned resources under PATH.  This is
   similar to checking the output of the status command. */
svn_error_t * svn_client__can_delete (const char *path, apr_pool_t *pool);

/* ---------------------------------------------------------------- */

/*** Checkout and update ***/

svn_error_t *
svn_client__checkout_internal (const svn_delta_editor_t *before_editor,
                               void *before_edit_baton,
                               const svn_delta_editor_t *after_editor,
                               void *after_edit_baton,
                               const char *path,
                               const char *xml_src,
                               const char *ancestor_path,
                               svn_revnum_t ancestor_revision,
                               svn_boolean_t recurse,
                               apr_pool_t *pool);


svn_error_t *
svn_client__update_internal (const svn_delta_editor_t *before_editor,
                             void *before_edit_baton,
                             const svn_delta_editor_t *after_editor,
                             void *after_edit_baton,
                             const char *path,
                             const char *xml_src,
                             svn_revnum_t ancestor_revision,
                             svn_boolean_t recurse,
                             apr_pool_t *pool);


/* ---------------------------------------------------------------- */

/*** Editor for repository diff ***/

/* Create an editor for a pure repository comparison, i.e. comparing one
 * repository version against the other. 
 *
 * TARGET is a working-copy path, the base of the hierarchy to be
 * compared.  It corresponds to the URL opened in RA_SESSION below.
 *
 * DIFF_CMD/DIFF_CMD_BATON represent the callback and calback argument that
 * implement the file comparison function
 *
 * RECURSE is set if the diff is to be recursive.
 *
 * RA_LIB/RA_SESSION define the additional ra session for requesting file
 * contents.
 *
 * REVISION is the start revision in the comparison.
 *
 * EDITOR/EDIT_BATON return the newly created editor and baton/
 */
svn_error_t *
svn_client__get_diff_editor (const char *target,
                             const svn_diff_callbacks_t *diff_cmd,
                             void *diff_cmd_baton,
                             svn_boolean_t recurse,
                             svn_ra_plugin_t *ra_lib,
                             void *ra_session, 
                             svn_revnum_t revision,
                             const svn_delta_editor_t **editor,
                             void **edit_baton,
                             apr_pool_t *pool);


/* ---------------------------------------------------------------- */

/*** Commit Stuff ***/

/* WARNING: This is all new, untested, un-peer-reviewed conceptual
   stuff.

   The day that `svn switch' came into existence, our old commit
   crawler (svn_wc_crawl_local_mods) became obsolete.  It relied far
   too heavily on the on-disk heirarchy of files and directories, and
   simply had no way to support disjoint working copy trees or nest
   working copies.  The primary reason for this is that commit
   process, in order to guarantee atomicity, is a single drive of a
   commit editor which is based not on working copy paths, but on
   URLs.  With the completion of `svn switch', it became all too
   likely that the on-disk working copy heirarchy would no longer be
   guaranteed to map to a similar in-repository heirarchy.

   Aside from this new brokenness of the old system, an unrelated
   feature request had cropped up -- the ability to know in advance of
   your commit, exactly what would be committed (so that log messages
   could be initially populated with this information).  Since the old
   crawler discovered commit candidates while in the process of
   committing, it was impossible to harvest this information upfront.
   As a workaround, svn_wc_statuses() was used to stat the whole
   working copy for changes before the commit started...and then the
   commit would again stat the whole tree for changes.

   Enter the new system.

   The primary goal of this system is very straightforward: harvest
   all commit candidate information up front, and cache enough info in
   the process to use this to drive a URL-sorted commit.

   *** END-OF-KNOWLEDGE ***

   The prototypes below are still in development.  In general, the
   idea is that commit-y processes (`svn mkdir URL`, `svn delete URL`,
   `svn commit`, `svn copy WC_PATH URL`, `svn copy URL1 URL2`, `svn
   move URL1 URL2`, others??) generate the cached commit candidate
   information, and hand this information off to a consumer which is
   responsible for driving the RA layer's commit editor in a
   URL-depth-first fashion and reporting back the post-commit
   information.

*/



/* ### This is TEMPORARY! Until we can find out the canonical
   repository URL of a given entry, we'll just use this bogus value in
   for our single committables hash key.  By the time we support
   multiple repositories we will have to be storing the canonical
   repository URLs anyway, so this will go away and the real URLs will
   be the keys of the committables hash. */
#define SVN_CLIENT__SINGLE_REPOS_NAME "svn:single-repos"


/* Recursively crawl a set of working copy paths (PARENT_DIR + each
   item in the TARGETS array) looking for commit candidates, locking
   working copy directories as the crawl progresses.  For each
   candidate found:

     - create svn_client_commit_item_t for the candidate.

     - add the structure to an apr_array_header_t array of commit
       items that are in the same repository, creating a new array if
       necessary.

     - add (or update) a reference to this array to the COMMITTABLES
       hash, keyed on the canonical repository name.  ### todo, until
       multi-repository support actually exists, the single key here
       will actually be some arbitrary thing to be ignored.  

   At the successful return of this function, COMMITTABLES will be an
   apr_hash_t * hash of apr_array_header_t * arrays (of
   svn_client_commit_item_t * structures), keyed on const char *
   canonical repository URLs.  Also, LOCKED_DIRS will be an apr_hash_t
   * hash of meaningless data keyed on const char * working copy path
   directory names which were locked in the process of this crawl.
   These will need to be unlocked again post-commit.

   If NONRECURSIVE is specified, subdirectories of directory targets
   found in TARGETS will not be crawled for modifications.  */
svn_error_t *
svn_client__harvest_committables (apr_hash_t **committables,
                                  apr_hash_t **locked_dirs,
                                  const char *parent_dir,
                                  apr_array_header_t *targets,
                                  svn_boolean_t nonrecursive,
                                  apr_pool_t *pool);


/* Recursively crawl the working copy path TARGET, harvesting
   commit_items into a COMMITABLES hash (see the docstring for
   svn_client__harvest_committables for what that really means, and
   for the relevance of LOCKED_DIRS) as if every entry at or below
   TARGET was to be committed as a set of adds (mostly with history)
   to a new repository URL (NEW_URL). */
svn_error_t *
svn_client__get_copy_committables (apr_hash_t **committables,
                                   apr_hash_t **locked_dirs,
                                   const char *new_url,
                                   const char *target,
                                   apr_pool_t *pool);
               

/* A qsort()-compatible sort routine for sorting an array of
   svn_client_commit_item_t's by their URL member. */
int svn_client__sort_commit_item_urls (const void *a, const void *b);


/* Rewrite the COMMIT_ITEMS array to be sorted by URL.  Also, discover
   a common *BASE_URL for the items in the array, and rewrite those
   items' URLs to be relative to that *BASE_URL.  

   Afterwards, some of the items in COMMIT_ITEMS may contain data
   allocated in POOL. */
svn_error_t *
svn_client__condense_commit_items (char **base_url,
                                   apr_array_header_t *commit_items,
                                   apr_pool_t *pool);


/* Commit the items in the COMMIT_ITEMS array using EDITOR/EDIT_BATON
   to describe the committed local mods.  Prior to this call,
   COMMIT_ITEMS should have been run through (and BASE_URL generated
   by) svn_client__condense_commit_items.

   REVNUM_FN/REV_BATON allows this routine to query the repository for
   the latest revision.  It is used (temporarily) for checking that
   directories are "up-to-date" when a dir-propchange is discovered.
   We don't expect it to be here forever.  :-) 

   NOTIFY_FUNC/BATON will be called as the commit progresses, as a way
   of describing actions to the application layer (if non NULL).

   NOTIFY_PATH_OFFSET is used to send shorter, relative paths to the
   notify_func (it's the number of characters subtracted from the front
   of the paths.)

   If the caller wants to keep track of any outstanding temporary
   files left after the transmission of text and property mods,
   *TEMPFILES is the place to look.  */
svn_error_t *
svn_client__do_commit (const char *base_url,
                       apr_array_header_t *commit_items,
                       const svn_delta_editor_t *editor,
                       void *edit_baton,
                       svn_wc_notify_func_t notify_func,
                       void *notify_baton,
                       int notify_path_offset,
                       apr_hash_t **tempfiles,
                       apr_pool_t *pool);



/*** Externals (Modules) ***/

/* One external item.  This usually represents one line from an
   svn:externals description. 

   ### 
*/
typedef struct svn_client__external_item_t
{
  /* The name of the subdirectory into which this external should be
     checked out.  (But note that these structs are often stored in
     hash tables with the target dirs as keys, so this field will
     often be redundant.) */
  const char *target_dir;

  /* Where to check out from. */
  const char *url;

  /* What revision to check out.  Only svn_client_revision_number,
     svn_client_revision_date, and svn_client_revision_head are
     valid.  ### Any reason to make this inline instead of pointer? */
  svn_client_revision_t *revision;

} svn_client__external_item_t;


/* Set *EXTERNALS_P to a hash table whose keys are target subdir
 * names, and values are `svn_client__external_item_t *' objects,
 * based on DESC.
 *
 * The format of EXTERNALS is the same as for values of the directory
 * property SVN_PROP_EXTERNALS, which see.
 *
 * Allocate the table, keys, and values in POOL.
 *
 * If the format of DESC is invalid, don't touch *EXTERNALS_P and
 * return SVN_ERR_CLIENT_INVALID_EXTERNALS_DESCRIPTION.
 */
svn_error_t *svn_client__parse_externals_description
   (apr_hash_t **externals_p,
    const char *desc,
    apr_pool_t *pool);


/* Walk newly checked-out tree PATH looking for directories that have
   the "svn:externals" property set.  For each one, read the external
   items from in the property value, and check them out as subdirs
   of the directory that had the property.

   BEFORE_EDITOR/BEFORE_EDIT_BATON and AFTER_EDITOR/AFTER_EDIT_BATON,
   along with AUTH_BATON, are passed along to svn_client_checkout() to
   check out the external items.   ### This is a lousy notification
   system, soon it will be notification callbacks instead, that will
   be nice! ###

   ### todo: AUTH_BATON may not be so useful.  It's almost like we
       need access to the original auth-obtaining callbacks that
       produced auth baton in the first place.  Hmmm. ###

   Use POOL for temporary allocation.  */
svn_error_t *svn_client__checkout_externals
   (const char *path, 
    const svn_delta_editor_t *before_editor,
    void *before_edit_baton,
    const svn_delta_editor_t *after_editor,
    void *after_edit_baton,
    svn_client_auth_baton_t *auth_baton,
    apr_pool_t *pool);



#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_CLIENT_H */


/* 
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end:
 */

