/*
 * svn_client.h :  public interface for libsvn_client
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



/*** Includes ***/

/* 
 * Requires:  The working copy library and repository access library.
 * Provides:  Broad wrappers around working copy library functionality.
 * Used By:   Client programs.
 */

#ifndef SVN_CLIENT_H
#define SVN_CLIENT_H

#include <apr_tables.h>
#include "svn_types.h"
#include "svn_wc.h"
#include "svn_ra.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_opt.h"


#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* ### TODO:  Multiple Targets

    - Up for debate:  an update on multiple targets is *not* atomic.
    Right now, svn_client_update only takes one path.  What's
    debatable is whether this should ever change.  On the one hand,
    it's kind of losing to have the client application loop over
    targets and call svn_client_update() on each one;  each call to
    update initializes a whole new repository session (network
    overhead, etc.)  On the other hand, it's this is a very simple
    implementation, and allows for the possibility that different
    targets may come from different repositories.  */



/*** Authentication stuff -- new M4 Edition  ***/

/*  The new authentication system allows the RA layer to "pull"
    information as needed from libsvn_client.  See svn_ra.h */

/*  A callback function type defined by the top-level client
    application (the user of libsvn_client.)

    If libsvn_client is unable to retrieve certain authorization
    information, it can use this callback; the application will then
    directly query the user with PROMPT and return the answer in INFO,
    allocated in POOL.  BATON is provided at the same time as the
    callback, and HIDE indicates that the user's answer should not be
    displayed on the screen. */
typedef svn_error_t *(*svn_client_prompt_t)
       (char **info,
        const char *prompt,
        svn_boolean_t hide,
        void *baton,
        apr_pool_t *pool);


/* This is a baton that contains information from the calling
   application, passed to libsvn_client to aid in authentication. 

   Applications must build and pass one of these to any routine that
   may require authentication.  */
typedef struct svn_client_auth_baton_t
{
  /* auth info that the app -may- already have, e.g. from argv[] */
  const char *username;    
  const char *password; 
  
  /* a callback provided by the app layer, for prompting the user */
  svn_client_prompt_t prompt_callback;
  void *prompt_baton;

  /* ### Right now, we only cache username and password.  Since
     there's only a single --no-auth-cache option, and it applies to
     both the username and password, we don't offer any framework for
     storing just the username but not the password.  If we wanted to
     do that, each of the two variables below should probably be split
     into two, one pair for username, one pair for password. 

     But note that we already check the `store_password' config
     option, so the important case is already covered. */

  /* true means ok to overwrite wc auth info, i.e., not --no-auth-cache */
  svn_boolean_t store_auth_info;

  /* true means there's new auth info to store */
  svn_boolean_t got_new_auth_info;

} svn_client_auth_baton_t;


/* This is a structure which stores a filename and a hash of property
   names and values. */

typedef struct svn_client_proplist_item_s
{
  /* The name of the node on which these properties are set. */
  svn_stringbuf_t *node_name;  

  /* A hash of (const char *) property names, and (svn_string_t *) property
     values. */
  apr_hash_t *prop_hash;

} svn_client_proplist_item_t;


/* Information about commits passed back to client from this module. */
typedef struct svn_client_commit_info_t
{
  svn_revnum_t revision; /* just-committed revision. */
  const char *date;      /* server-side date of the commit. */
  const char *author;    /* author of the commit. */
} svn_client_commit_info_t;


/* State flags for use with the svn_client_commit_item_t structure
   (see the note about the namespace for that structure, which also
   applies to these flags). */
#define SVN_CLIENT_COMMIT_ITEM_ADD         0x01
#define SVN_CLIENT_COMMIT_ITEM_DELETE      0x02
#define SVN_CLIENT_COMMIT_ITEM_TEXT_MODS   0x04
#define SVN_CLIENT_COMMIT_ITEM_PROP_MODS   0x08
#define SVN_CLIENT_COMMIT_ITEM_IS_COPY     0x10


/* The commit candidate structure. */
typedef struct svn_client_commit_item_t
{
  const char *path;              /* absolute working-copy path of item */
  svn_node_kind_t kind;          /* node kind (dir, file) */
  const char *url;               /* commit url for this item */
  svn_revnum_t revision;         /* revision (copyfrom-rev if _IS_COPY) */
  const char *copyfrom_url;      /* copyfrom-url */
  apr_byte_t state_flags;        /* state flags */

  /* An array of `svn_prop_t *' changes to wc properties.  If adding
     to this array, allocate the svn_prop_t and its contents in
     wcprop_changes->pool, so that it has the same lifetime as this
     svn_client_commit_item_t.

     See http://subversion.tigris.org/issues/show_bug.cgi?id=806 for 
     what would happen if the post-commit process didn't group these
     changes together with all other changes to the item :-). */
  apr_array_header_t *wcprop_changes;

} svn_client_commit_item_t;


/* Callback type used by commit-y operations to get a commit log message
   from the caller.
   
   Set *LOG_MSG to the log message for the commit, allocated in POOL,
   or NULL if wish to abort the commit process.  COMMIT_ITEMS is an
   array of svn_client_commit_item_t structures, which may be fully or
   only partially filled-in, depending on the type of commit
   operation.

   BATON is provided along with the callback for use by the handler.

   All allocations should be performed in POOL.  */
typedef svn_error_t *
(*svn_client_get_commit_log_t) (const char **log_msg,
                                apr_array_header_t *commit_items,
                                void *baton,
                                apr_pool_t *pool);



/* Names of files that contain authentication information.

   These filenames are decided by libsvn_client, since this library
   implements all the auth-protocols;  libsvn_wc does nothing but
   blindly store and retrieve these files from protected areas. */
#define SVN_CLIENT_AUTH_USERNAME            "username"
#define SVN_CLIENT_AUTH_PASSWORD            "password"




/*** Milestone 4 Interfaces ***/

/* Checkout a working copy of URL at REVISION, using PATH as the root
   directory of the newly checked out working copy, and authenticating
   with AUTH_BATON.

   REVISION must be of kind svn_client_revision_number,
   svn_client_revision_head, or svn_client_revision_date.  If REVISION 
   does not meet these requirements, return the error
   SVN_ERR_CLIENT_BAD_REVISION.

   If NOTIFY_FUNC is non-null, invoke NOTIFY_FUNC with NOTIFY_BATON as
   the checkout progresses.

   Use POOL for any temporary allocation. */
svn_error_t *
svn_client_checkout (svn_wc_notify_func_t notify_func,
                     void *notify_baton,
                     svn_client_auth_baton_t *auth_baton,
                     const char *URL,
                     const char *path,
                     const svn_opt_revision_t *revision,
                     svn_boolean_t recurse,
                     apr_pool_t *pool);


/* Update working tree PATH to REVISION, authenticating with
   AUTH_BATON.

   REVISION must be of kind svn_client_revision_number,
   svn_client_revision_head, or svn_client_revision_date.  If REVISION 
   does not meet these requirements, return the error
   SVN_ERR_CLIENT_BAD_REVISION.

   If NOTIFY_FUNC is non-null, invoke NOTIFY_FUNC with NOTIFY_BATON
   for each item handled by the update, and also for files restored
   from text-base.

   Use POOL for any temporary allocation. */
svn_error_t *
svn_client_update (svn_client_auth_baton_t *auth_baton,
                   const char *path,
                   const svn_opt_revision_t *revision,
                   svn_boolean_t recurse,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   apr_pool_t *pool);


/* Switch working tree PATH to URL at REVISION, authenticating with
   AUTH_BATON.

   Summary of purpose: this is normally used to switch a working
   directory over to another line of development, such as a branch or
   a tag.  Switching an existing working directory is more efficient
   than checking out URL from scratch.

   REVISION must be of kind svn_client_revision_number,
   svn_client_revision_head, or svn_client_revision_date; otherwise,
   return SVN_ERR_CLIENT_BAD_REVISION.

   If NOTIFY_FUNC is non-null, invoke it with NOTIFY_BATON on paths
   affected by the switch.  Also invoke it for files may be restored
   from the text-base because they were removed from the working
   copy.

   Use POOL for any temporary allocation. */
svn_error_t *
svn_client_switch (svn_client_auth_baton_t *auth_baton,
                   const char *path,
                   const char *url,
                   const svn_opt_revision_t *revision,
                   svn_boolean_t recurse,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   apr_pool_t *pool);


/* Schedule a working copy PATH for addition to the repository.
   PATH's parent must be under revision control already, but PATH is
   not.  If RECURSIVE is set, then assuming PATH is a directory, all
   of its contents will be scheduled for addition as well.

   If NOTIFY_FUNC is non-null, then for each added item, call
   NOTIFY_FUNC with NOTIFY_BATON and the path of the added
   item.

   Important:  this is a *scheduling* operation.  No changes will
   happen to the repository until a commit occurs.  This scheduling
   can be removed with svn_client_revert. */
svn_error_t *
svn_client_add (const char *path,
                svn_boolean_t recursive,
                svn_wc_notify_func_t notify_func,
                void *notify_baton,
                apr_pool_t *pool);

/* If PATH is a URL, use the AUTH_BATON and MESSAGE to immediately
   attempt to commit the creation of the directory URL in the
   repository.  If the commit succeeds, allocate (in POOL) and
   populate *COMMIT_INFO.

   Else, create the directory on disk, and attempt to schedule it for
   addition (using svn_client_add, whose docstring you should
   read).

   LOG_MSG_FUNC/LOG_MSG_BATON are a callback/baton combo that this
   function can use to query for a commit log message when one is
   needed.

   If NOTIFY_FUNC is non-null, when the directory has been created
   (successfully) in the working copy, call NOTIFY_FUNC with
   NOTIFY_BATON and the path of the new directory.  Note that this is
   only called for items added to the working copy.
*/
svn_error_t *
svn_client_mkdir (svn_client_commit_info_t **commit_info,
                  const char *path,
                  svn_client_auth_baton_t *auth_baton,
                  svn_client_get_commit_log_t log_msg_func,
                  void *log_msg_baton,
                  svn_wc_notify_func_t notify_func,
                  void *notify_baton,
                  apr_pool_t *pool);
                  

/* If PATH is a URL, use the AUTH_BATON and MESSAGE to immediately
   attempt to commit a deletion of the URL from the repository.  If
   the commit succeeds, allocate (in POOL) and populate *COMMIT_INFO.
  
   Else, schedule a working copy PATH for removal from the repository.
   PATH's parent must be under revision control. This is just a
   *scheduling* operation.  No changes will happen to the repository until
   a commit occurs.  This scheduling can be removed with
   svn_client_revert. If PATH is a file it is immediately removed from the
   working copy. If PATH is a directory it will remain in the working copy
   but all the files, and all unversioned items, it contains will be
   removed. If FORCE is not set then this operation will fail if PATH
   contains locally modified and/or unversioned items. If FORCE is set such
   items will be deleted.

   If deleting from a working copy, OPTIONAL_ADM_ACCESS can either be a
   baton that holds a write lock for the parent of PATH, or it can be
   NULL. If it is NULL the lock for the parent will be acquired and
   released by the function.  If deleting from a repository (PATH is an
   URL) then OPTIONAL_ADM_ACCESS is irrelevant.

   LOG_MSG_FUNC/LOG_MSG_BATON are a callback/baton combo that this
   function can use to query for a commit log message when one is
   needed.

   If NOTIFY_FUNC is non-null, then for each item deleted, call
   NOTIFY_FUNC with NOTIFY_BATON and the path of the deleted
   item. */
svn_error_t *
svn_client_delete (svn_client_commit_info_t **commit_info,
                   const char *path,
                   svn_wc_adm_access_t *optional_adm_access,
                   svn_boolean_t force,
                   svn_client_auth_baton_t *auth_baton,
                   svn_client_get_commit_log_t log_msg_func,
                   void *log_msg_baton,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   apr_pool_t *pool);


/* Import file or directory PATH into repository directory URL at
   head, authenticating with AUTH_BATON, and using LOG_MSG as the log
   message for the (implied) commit.  Set *COMMIT_INFO to the results
   of the commit, allocated in POOL.
  
   NEW_ENTRY is the new entry created in the repository directory
   identified by URL.  NEW_ENTRY may be null (see below), but may not
   be the empty string.
  
   If PATH is a directory, the contents of that directory are
   imported, under a new directory named NEW_ENTRY under URL; or if
   NEW_ENTRY is null, then the contents of PATH are imported directly
   into the directory identified by URL.  Note that the directory PATH
   itself is not imported -- that is, the basename of PATH is not part
   of the import.

   If PATH is a file, that file is imported as NEW_ENTRY (which may
   not be null).

   In all cases, if NEW_ENTRY already exists in URL, return error.
   
   If NOTIFY_FUNC is non-null, then call NOTIFY_FUNC with NOTIFY_BATON
   as the import progresses, with any of the following actions:
   svn_wc_notify_commit_added, svn_wc_notify_commit_postfix_txdelta.

   Use POOL for any temporary allocation.  
   
   LOG_MSG_FUNC/LOG_MSG_BATON are a callback/baton combo that this
   function can use to query for a commit log message when one is
   needed.

   Use NONRECURSIVE to indicate that imported directories should not
   recurse into any subdirectories they may have.

   ### kff todo: This import is similar to cvs import, in that it does
   not change the source tree into a working copy.  However, this
   behavior confuses most people, and I think eventually svn _should_
   turn the tree into a working copy, or at least should offer the
   option. However, doing so is a bit involved, and we don't need it
   right now.  
*/
svn_error_t *svn_client_import (svn_client_commit_info_t **commit_info,
                                svn_wc_notify_func_t notify_func,
                                void *notify_baton,
                                svn_client_auth_baton_t *auth_baton,   
                                const char *path,
                                const char *url,
                                const char *new_entry,
                                svn_client_get_commit_log_t log_msg_func,
                                void *log_msg_baton,
                                svn_boolean_t nonrecursive,
                                apr_pool_t *pool);


/* Commit file or directory PATH into repository, authenticating with
   AUTH_BATON, using LOG_MSG_FUNC/LOG_MSG_BATON to obtain the log
   message.  Set *COMMIT_INFO to the results of the commit, allocated
   in POOL.

   TARGETS is an array of const char * paths to commit.  They need not
   be canonicalized nor condensed; this function will take care of
   that.

   If NOTIFY_FUNC is non-null, then call NOTIFY_FUNC with NOTIFY_BATON
   as the commit progresses, with any of the following actions:
   svn_wc_notify_commit_modified, svn_wc_notify_commit_added,
   svn_wc_notify_commit_deleted, svn_wc_notify_commit_replaced,
   svn_wc_notify_commit_postfix_txdelta.

   Use NONRECURSIVE to indicate that subdirectories of directory
   TARGETS should be ignored.

   Use POOL for any temporary allocation.

   If no error is returned and (*COMMIT_INFO)->revision is set to
   SVN_INVALID_REVNUM, then the commit was a no-op; nothing needed to
   be committed.
 */
svn_error_t *
svn_client_commit (svn_client_commit_info_t **commit_info,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   svn_client_auth_baton_t *auth_baton,
                   const apr_array_header_t *targets,
                   svn_client_get_commit_log_t log_msg_func,
                   void *log_msg_baton,
                   svn_boolean_t nonrecursive,
                   apr_pool_t *pool);


/* Given PATH to a working copy directory (or single file), allocate
   and return a hash STATUSHASH which maps (char *) paths to
   (svn_wc_status_t *) structures.

   This is a purely local operation; only information found in the
   administrative `entries' files is used to initially build the
   structures.

      - If DESCEND is non-zero, recurse fully, else do only immediate
        children.  This (inversely) corresponds to the "-n"
        (--nonrecursive) flag in the commandline client app.

      - If GET_ALL is set, then all entries are retrieved; otherwise
        only "interesting" entries (local mods and/or out-of-date)
        will be fetched.  This directly corresponds to the "-v"
        (--verbose) flag in the commandline client app.

      - If UPDATE is set, then the repository will be contacted, so
        that the structures in STATUSHASH are augmented with
        information about out-of-dateness, and *YOUNGEST is set to the
        youngest repository revision (*YOUNGEST is not touched unless
        UPDATE is set).  This directly corresponds to the "-u"
        (--show-updates) flag in the commandline client app.

   If NOTIFY_FUNC is non-null, then call NOTIFY_FUNC with NOTIFY_BATON
   as the status progresses.  Specifically, every time a status
   structure is added (or tweaked) in the hash, this routine will pass
   the pathname with action svn_wc_notify_status.  (Note: callers
   should *not* attempt to look up the pathname in the hash for the
   purposes of parsing the status structure; a status structure is
   created in multiple passes, and is not guaranteed to be completely
   correct until svn_client_status completely finishes.)
*/
svn_error_t *
svn_client_status (apr_hash_t **statushash,
                   svn_revnum_t *youngest,  /* only touched if `update' set */
                   const char *path,
                   svn_client_auth_baton_t *auth_baton,
                   svn_boolean_t descend,
                   svn_boolean_t get_all,
                   svn_boolean_t update,
                   svn_boolean_t no_ignore,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   apr_pool_t *pool);


/* Invoke RECEIVER with RECEIVER_BATON on each log message from START
   to END in turn, inclusive (but never invoke RECEIVER on a given log
   message more than once).
  
   TARGETS contains all the working copy paths (as const char *'s)
   for which log messages are desired; the common prefix of TARGETS
   determines the repository and auth info.  RECEIVER is invoked only
   on messages whose revisions involved a change to some path in
   TARGETS.
  
   ### todo: the above paragraph is not fully implemented yet.
  
   If DISCOVER_CHANGED_PATHS is set, then the `changed_paths' argument
   to RECEIVER will be passed on each invocation.

   If STRICT_NODE_HISTORY is set, copy history (if any exists) will
   not be traversed while harvest revision logs for each target.

   If START->kind or END->kind is svn_opt_revision_unspecified,
   return the error SVN_ERR_CLIENT_BAD_REVISION.

   Use POOL for any temporary allocation.

   Special case for repositories at revision 0:

   If START->kind is svn_opt_revision_head, and END->kind is
   svn_opt_revision_number && END->number is 1, then handle an
   empty (no revisions) repository specially: instead of erroring
   because requested revision 1 when the highest revision is 0, just
   invoke RECEIVER on revision 0, passing NULL for changed paths and
   empty strings for the author and date.  This is because that
   particular combination of START and END usually indicates the
   common case of log invocation -- the user wants to see all log
   messages from youngest to oldest, where the oldest commit is
   revision 1.  That works fine, except when there are no commits in
   the repository, hence this special case.  */
svn_error_t *
svn_client_log (svn_client_auth_baton_t *auth_baton,
                const apr_array_header_t *targets,
                const svn_opt_revision_t *start,
                const svn_opt_revision_t *end,
                svn_boolean_t discover_changed_paths,
                svn_boolean_t strict_node_history,
                svn_log_message_receiver_t receiver,
                void *receiver_baton,
                apr_pool_t *pool);


/* Produce diff output which describes the delta between
   PATH1/REVISION1 and PATH2/REVISION2.  Print the output of the diff
   to OUTFILE, and any errors to ERRFILE.  PATH1 and PATH can be
   either working-copy paths or URLs.

   If either REVISION1 or REVISION2 has an `unspecified' or
   unrecognized `kind', return SVN_ERR_CLIENT_BAD_REVISION.

   PATH1 and PATH2 must both represent the same node kind -- that is,
   if PATH1 is a directory, PATH2 must also be, and if PATH1 is a
   file, PATH2 must also be.  (Currently, PATH1 and PATH2 must be the
   exact same path)

   If RECURSE is true (and the PATHs are directories) this will be a
   recursive operation.
  
   DIFF_OPTIONS (an array of const char *) is used to pass additional
   command line options to the diff processes invoked to compare
   files.
  
   AUTH_BATON is used to communicate with the repository.  */
svn_error_t *svn_client_diff (const apr_array_header_t *diff_options,
                              svn_client_auth_baton_t *auth_baton,
                              const char *path1,
                              const svn_opt_revision_t *revision1,
                              const char *path2,
                              const svn_opt_revision_t *revision2,
                              svn_boolean_t recurse,
                              apr_file_t *outfile,
                              apr_file_t *errfile,
                              apr_pool_t *pool);


/* Merge changes from URL1/REVISION1 to URL2/REVISION2 into the
   working-copy path TARGET_WCPATH.

   By "merging", we mean:  apply file differences using
   svn_wc_merge(), and schedule additions & deletions when appopriate.

   URL1 and URL2 must both represent the same node kind -- that is,
   if PATH1 is a directory, PATH2 must also be, and if PATH1 is a
   file, PATH2 must also be.

   If either REVISION1 or REVlISION2 has an `unspecified' or
   unrecognized `kind', return SVN_ERR_CLIENT_BAD_REVISION.
  
   If RECURSE is true (and the URLSs are directories), apply changes
   recursively; otherwise, only apply changes in the current
   directory.

   If FORCE is not set and the merge involves deleting locally modified or
   unversioned items the operation will fail.  If FORCE is set such items
   will be deleted.
  
   If NOTIFY_FUNC is non-null, then call NOTIFY_FUNC with NOTIFY_BATON
   once for each merged target, passing the target's local path.

   If DRY_RUN is TRUE the merge is carried out, and full notofication
   feedback is provided, but the working copy is not modified.

   AUTH_BATON is used to communicate with the repository.  */
svn_error_t *
svn_client_merge (svn_wc_notify_func_t notify_func,
                  void *notify_baton,
                  svn_client_auth_baton_t *auth_baton,
                  const char *URL1,
                  const svn_opt_revision_t *revision1,
                  const char *URL2,
                  const svn_opt_revision_t *revision2,
                  const char *target_wcpath,
                  svn_boolean_t recurse,
                  svn_boolean_t force,
                  svn_boolean_t dry_run,
                  apr_pool_t *pool);


/* Recursively cleanup a working copy directory DIR, finishing any
   incomplete operations, removing lockfiles, etc. */
svn_error_t *
svn_client_cleanup (const char *dir,
                    apr_pool_t *pool);


/* Restore the pristine version of a working copy PATH, effectively
   undoing any local mods.  If PATH is a directory, and RECURSIVE is
   TRUE, this will be a recursive operation.

   If NOTIFY_FUNC is non-null, then for each item reverted, call
   NOTIFY_FUNC with NOTIFY_BATON and the path of the reverted item. */
svn_error_t *
svn_client_revert (const char *path,
                   svn_boolean_t recursive,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   apr_pool_t *pool);


/* Remove the 'conflicted' state on a working copy PATH.  This will
   not semantically resolve conflicts;  it just allows PATH to be
   committed in the future.  The implementation details are opaque.
   If RECURSIVE is set, recurse below PATH, looking for conflicts to
   resolve.

   If PATH is not in a state of conflict to begin with, do nothing.
   If PATH's conflict state is removed and NOTIFY_FUNC is non-null,
   call NOTIFY_FUNC with NOTIFY_BATON and PATH. */
svn_error_t *
svn_client_resolve (const char *path,
                    svn_wc_notify_func_t notify_func,
                    void *notify_baton,
                    svn_boolean_t recursive,
                    apr_pool_t *pool);


/* Copy SRC_PATH to DST_PATH.

   SRC_PATH must be a file or directory under version control, or the
   URL of a versioned item in the repository.  If SRC_PATH is a URL,
   SRC_REVISION is used to choose the revision from which to copy the
   SRC_PATH.  DST_PATH must be a file or directory under version
   control, or a repository URL, existent or not.

   ### 838 The argument to be removed when 838 stops using svn_client_copy.
   OPTIONAL_ADM_ACCESS can either be a baton that holds a write lock for
   the parent of PATH, or it can be NULL. If it is NULL the lock for the
   parent will be acquired and released by the function.

   If either SRC_PATH or DST_PATH are URLs, use the AUTH_BATON and
   MESSAGE to immediately attempt to commit the copy action in the
   repository.  If the commit succeeds, allocate (in POOL) and
   populate *COMMIT_INFO.

   If neither SRC_PATH nor DST_PATH is a URL, then this is just a
   variant of svn_client_add, where the DST_PATH items are scheduled
   for addition as copies.  No changes will happen to the repository
   until a commit occurs.  This scheduling can be removed with
   svn_client_revert.

   LOG_MSG_FUNC/LOG_MSG_BATON are a callback/baton combo that this
   function can use to query for a commit log message when one is
   needed.

   If NOTIFY_FUNC is non-null, invoke it with NOTIFY_BATON for each
   item added at the new location, passing the new, relative path of
   the added item.  */
svn_error_t *
svn_client_copy (svn_client_commit_info_t **commit_info,
                 const char *src_path,
                 const svn_opt_revision_t *src_revision,
                 const char *dst_path,
                 svn_wc_adm_access_t *optional_adm_access,
                 svn_client_auth_baton_t *auth_baton,
                 svn_client_get_commit_log_t log_msg_func,
                 void *log_msg_baton,
                 svn_wc_notify_func_t notify_func,
                 void *notify_baton,
                 apr_pool_t *pool);


/* Move SRC_PATH to DST_PATH.

   SRC_PATH must be a file or directory under version control, or the
   URL of a versioned item in the repository.  

   If SRC_PATH is a repository URL:

     - DST_PATH must also be a repository URL (existent or not).

     - SRC_REVISION is used to choose the revision from which to copy the
       SRC_PATH.

     - AUTH_BATON and MESSAGE are used to commit the move.

     - The move operation will be immediately committed.  If the
       commit succeeds, allocate (in POOL) and populate *COMMIT_INFO.

   If SRC_PATH is a working copy path

     - DST_PATH must also be a working copy path (existent or not).

     - SRC_REVISION, AUTH and MESSAGE are ignored.

     - This is a scheduling operation.  No changes will happen to the
       repository until a commit occurs.  This scheduling can be removed
       with svn_client_revert.  If SRC_PATH is a file it is removed from
       the working copy immediately.  If SRC_PATH is a directory it will
       remain n the working copy but all the files, and unversioned items,
       it contains will be removed.

     - If SRC_PATH contains locally modified and/or unversioned items and
       FORCE is not set, the copy will fail. If FORCE is set such items
       will be removed.

   LOG_MSG_FUNC/LOG_MSG_BATON are a callback/baton combo that this
   function can use to query for a commit log message when one is
   needed.

   If NOTIFY_FUNC is non-null, then for each item moved, call
   NOTIFY_FUNC with the NOTIFY_BATON twice, once to indicate the
   deletion of the moved thing, and once to indicate the addition of
   the new location of the thing. ### Is this really true?  What about
                                      svn_wc_notify_commit_replaced? ### 
*/ 
svn_error_t *
svn_client_move (svn_client_commit_info_t **commit_info,
                 const char *src_path,
                 const svn_opt_revision_t *src_revision,
                 const char *dst_path,
                 svn_boolean_t force,
                 svn_client_auth_baton_t *auth_baton,
                 svn_client_get_commit_log_t log_msg_func,
                 void *log_msg_baton,
                 svn_wc_notify_func_t notify_func,
                 void *notify_baton,
                 apr_pool_t *pool);



/* Set PROPNAME to PROPVAL on TARGET.  If RECURSE is true, then PROPNAME
   will be set on recursively on TARGET and all children.  If RECURSE is false,
   and TARGET is a directory, PROPNAME will be set on _only_ TARGET.
 
   A PROPVAL of NULL will delete the property.

   Use POOL for all memory allocation. */
svn_error_t *
svn_client_propset (const char *propname,
                    const svn_string_t *propval,
                    const char *target,
                    svn_boolean_t recurse,
                    apr_pool_t *pool);

/* Set PROPNAME to PROPVAL on revision REVISION in the repository
   represented by URL.  Use AUTH_BATON for authentication, and POOL
   for all memory allocation.  Return the actual rev affected in *SET_REV.
   A PROPVAL of NULL will delete the property.

   Note that unlike its cousin svn_client_propset(), this routine
   doesn't affect the working copy at all;  it's a pure network
   operation that changes an *unversioned* property attached to a
   revision.  This can be used to tweak log messages, dates, authors,
   and the like.  Be careful:  it's a lossy operation.

   Also note that unless the administrator creates a
   pre-revprop-change hook in the repository, this feature will fail. */
svn_error_t *
svn_client_revprop_set (const char *propname,
                        const svn_string_t *propval,
                        const char *URL,
                        const svn_opt_revision_t *revision,
                        svn_client_auth_baton_t *auth_baton,
                        svn_revnum_t *set_rev,
                        apr_pool_t *pool);
                        
/* Set *PROPS to a hash table whose keys are `char *' paths,
   prefixed by TARGET, of items in the working copy on which 
   property PROPNAME is set, and whose values are `svn_string_t *'
   representing the property value for PROPNAME at that path.
   Allocate *PROPS, its keys, and its values in POOL.
             
   Don't store any path, not even TARGET, if it does not have a
   property named PROPNAME.

   If TARGET is a file or RECURSE is false, *PROPS will have
   at most one element.

   If error, don't touch *PROPS, otherwise *PROPS is a hash table even if
   empty. */
svn_error_t *
svn_client_propget (apr_hash_t **props,
                    const char *propname,
                    const char *target,
                    svn_boolean_t recurse,
                    apr_pool_t *pool);

/* Set *PROPVAL to the value of PROPNAME on revision REVISION in the
   repository represented by URL.  Use AUTH_BATON for authentication,
   and POOL for all memory allocation.  Return the actual rev queried
   in *SET_REV.

   Note that unlike its cousin svn_client_propget(), this routine
   doesn't affect the working copy at all; it's a pure network
   operation that queries an *unversioned* property attached to a
   revision.  This can be query log messages, dates, authors, and the
   like.
*/
svn_error_t *
svn_client_revprop_get (const char *propname,
                        svn_string_t **propval,
                        const char *URL,
                        const svn_opt_revision_t *revision,
                        svn_client_auth_baton_t *auth_baton,
                        svn_revnum_t *set_rev,
                        apr_pool_t *pool);

/* Returns an apr_array_header_t of svn_client_proplist_item_t's in *PROPS,
   allocated from POOL. Each item will contain the node_name relative to the
   same base as target in item->node_name, and a property hash of 
   (const char *) property names, and (svn_string_t *) property values.

   If recurse is false, or TARGET is a file, *PROPS will contain only a single
   element.  Otherwise, it will contain one for each versioned entry below
   (and including) TARGET. */
svn_error_t *
svn_client_proplist (apr_array_header_t **props,
                     const char *target, 
                     svn_boolean_t recurse,
                     apr_pool_t *pool);

/* Set *PROPS to a hash of the revision props attached to REVISION in
   the repository represented by URL.  Use AUTH_BATON for
   authentication, and POOL for all memory allocation.  Return the
   actual rev queried in *SET_REV.

   The allocated hash maps (const char *) property names to
   (svn_string_t *) property values.

   Note that unlike its cousin svn_client_proplist(), this routine
   doesn't read a working copy at all; it's a pure network operation
   that reads *unversioned* properties attached to a revision. */
svn_error_t *
svn_client_revprop_list (apr_hash_t **props,
                         const char *URL,
                         const svn_opt_revision_t *revision,
                         svn_client_auth_baton_t *auth_baton,
                         svn_revnum_t *set_rev,
                         apr_pool_t *pool);


/* Export the contents of either a subversion repository or a subversion 
   working copy into a 'clean' directory (meaning a directory with no 
   administrative directories).

   FROM is either the path the working copy on disk, or a url to the
   repository you wish to export.

   TO is the path to the directory where you wish to create the exported
   tree.

   REVISION is the revision that should be exported, which is only used 
   when exporting from a repository.

   AUTH_BATON is an authentication baton that is only used when exporting 
   from a repository.

   NOTIFY_FUNC and NOTIFY_BATON are the notification functions and baton 
   which are passed to svn_client_checkout when exporting from a repository.

   All allocations are done in POOL.  */
svn_error_t *
svn_client_export (const char *from,
                   const char *to,
                   svn_opt_revision_t *revision,
                   svn_client_auth_baton_t *auth_baton,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   apr_pool_t *pool);


/* Set *DIRENTS to a newly allocated hash of entries for URL at
   REVISION.

   If URL is a directory, return all dirents in the hash.  If URL is a
   file, return only the dirent for the file.  If URL is non-existent,
   return SVN_ERR_FS_NOT_FOUND.

   The hash maps entrynames (const char *) to svn_dirent_t *'s.  Do
   all allocation in POOL.

   Use AUTH_BATON to authenticate against the repository.

   If RECURSE is true (and the URL is a directory) this will be a
   recursive operation. */
svn_error_t *
svn_client_ls (apr_hash_t **dirents,
               const char *url,
               svn_opt_revision_t *revision,
               svn_client_auth_baton_t *auth_baton,
               svn_boolean_t recurse,
               apr_pool_t *pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif  /* SVN_CLIENT_H */
