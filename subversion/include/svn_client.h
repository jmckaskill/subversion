/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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
 * @endcopyright
 *
 * @file svn_client.h
 * @brief Public interface for libsvn_client.
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



/*** Authentication stuff ***/

/*  The new authentication system allows the RA layer to "pull"
    information as needed from libsvn_client.  See svn_ra.h */

/** A callback function type defined by a top-level svn application.
 *
 * If libsvn_client is unable to retrieve certain authorization
 * information, it can use this callback; the application will then
 * directly query the user with @a prompt and return the answer in 
 * @c info, allocated in @a pool.  @a baton is provided at the same 
 * time as the callback, and @a hide indicates that the user's answer 
 * should not be displayed on the screen.
 */
typedef svn_error_t *
(*svn_client_prompt_t) (const char **info,
                        const char *prompt,
                        svn_boolean_t hide,
                        void *baton,
                        apr_pool_t *pool);


/** Fetch an authentication provider which prompts the user for name
 * and password.
 *
 * Set @a *provider and @a *provider_baton to an authentication
 * provider of type @c svn_auth_cred_simple_t that gets information by
 * prompting the user with @a prompt_func and @a prompt_baton.  If
 * either @c SVN_AUTH_PARAM_DEFAULT_USERNAME or @c
 * SVN_AUTH_PARAM_DEFAULT_PASSWORD is defined as a runtime parameter
 * in the @c auth_baton, then return the default argument(s) when @c
 * svn_auth_first_credentials is called.  If @c
 * svn_auth_first_credentials fails, then re-prompt @a retry_limit
 * number of times (via @c svn_auth_next_credentials). */
void 
svn_client_get_simple_prompt_provider (const svn_auth_provider_t **provider,
                                       void **provider_baton,
                                       svn_client_prompt_t prompt_func,
                                       void *prompt_baton,
                                       int retry_limit,
                                       apr_pool_t *pool);


/** Fetch an authentication provider which prompts the user for a
 * username.
 *
 * Set @a *provider and @a *provider_baton to an authentication
 * provider of type @c svn_auth_cred_username_t that gets information by
 * prompting the user with @a prompt_func and @a prompt_baton.  If
 * @c SVN_AUTH_PARAM_DEFAULT_USERNAME is defined as a runtime parameter
 * in the @c auth_baton, then return the default argument when @c
 * svn_auth_first_credentials is called.  If @c
 * svn_auth_first_credentials fails, then re-prompt @a retry_limit
 * number of times (via @c svn_auth_next_credentials). */
void 
svn_client_get_username_prompt_provider (const svn_auth_provider_t **provider,
                                         void **provider_baton,
                                         svn_client_prompt_t prompt_func,
                                         void *prompt_baton,
                                         int retry_limit,
                                         apr_pool_t *pool);


/** Set @a *provider and @ *provider_baton to an authentication
 *  provider of type @c svn_auth_cred_simple_t that gets/sets
 *  information from the user's ~/.subversion configuration directory. 
 *  
 *  If a default username or password is available, this provider will
 *  honor them as well, and return them when @c
 *  svn_auth_first_credentials is called.  (see @c
 *  SVN_AUTH_PARAM_DEFAULT_USERNAME and @c
 *  SVN_AUTH_PARAM_DEFAULT_PASSWORD).
 */
void 
svn_client_get_simple_provider (const svn_auth_provider_t **provider,
                                void **provider_baton,
                                apr_pool_t *pool);


/** Set @a *provider and @ *provider_baton to an authentication
 *  provider of type @c svn_auth_cred_username_t that gets/sets
 *  information from a user's ~/.subversion configuration directory.
 *
 *  If a default username is available, this provider will honor it,
 *  and return it when @c svn_auth_first_credentials is called.  (see
 *  @c SVN_AUTH_PARAM_DEFAULT_USERNAME).
 */
void 
svn_client_get_username_provider (const svn_auth_provider_t **provider,
                                  void **provider_baton,
                                  apr_pool_t *pool);


/** Set @a *provider and @ *provider_baton to an authentication
 *  provider for type @c svn_auth_cred_server_ssl_t. This provider retrieves
 *  its credentials from the configuration mechanism. The returned credential
 *  is used to override SSL security on an error.
 *  
 *  This provider requires certain run-time parameters be present in
 *  the auth_baton:
 *
 *     - a loaded @c svn_config_t object
 *        (@c SVN_AUTH_PARAM_CONFIG)
 *
 *     - the name of the server-specific settings group if available
 *        (@c SVN_AUTH_PARAM_SERVER_GROUP)
 *
 *     - the failure bitmask reported by the ssl certificate validator
 *        (@c SVN_AUTH_PARAM_SSL_SERVER_FAILURES_IN)
 */
void 
svn_client_get_ssl_server_file_provider (const svn_auth_provider_t **provider,
                                         void **provider_baton,
                                         apr_pool_t *pool);

/** Set @a *provider and @ *provider_baton to an authentication
 *  provider for type @c svn_auth_cred_client_ssl_t. This provider retrieves
 *  its credentials from the configuration mechanism. The returned credential
 *  is used to load the appropriate client certificate for authentication when
 *  requested by a server. 
 *  
 *  This provider requires certain run-time parameters be present in
 *  the auth_baton:
 *
 *     - a loaded @c svn_config_t object
 *        (@c SVN_AUTH_PARAM_CONFIG)
 *
 *     - the name of the server-specific settings group if available
 *        (@c SVN_AUTH_PARAM_SERVER_GROUP)
 */
void 
svn_client_get_ssl_client_file_provider (const svn_auth_provider_t **provider,
                                         void **provider_baton,
                                         apr_pool_t *pool);

/** Set @a *provider and @ *provider_baton to an authentication
 *  provider for type @c svn_auth_cred_client_ssl_pass_t. This provider
 *  retrieves its credentials from the configuration mechanism. The returned
 *  credential is used when a loaded client certificate is protected by a
 *  passphrase.
 *  
 *  This provider requires certain run-time parameters be present in
 *  the auth_baton:
 *
 *     - a loaded @c svn_config_t object
 *        (@c SVN_AUTH_PARAM_CONFIG)
 *
 *     - the name of the server-specific settings group if available
 *        (@c SVN_AUTH_PARAM_SERVER_GROUP)
 */
void
svn_client_get_ssl_pw_file_provider (const svn_auth_provider_t **provider,
                                     void **provider_baton,
                                     apr_pool_t *pool);

/** Set @a *provider and @ *provider_baton to an authentication
 *  provider for type @c svn_auth_cred_server_ssl_t. This provider retrieves
 *  its credentials by using the @a prompt_func and @a prompt_baton. The
 *  returned credential is used to override SSL security on an error.
 *  
 *  This provider requires certain run-time parameters be present in
 *  the auth_baton:
 *
 *     - the failure bitmask reported by the ssl certificate validator
 *        (@c SVN_AUTH_PARAM_SSL_SERVER_FAILURES_IN)
 */
void
svn_client_get_ssl_server_prompt_provider(const svn_auth_provider_t **provider,
                                          void **provider_baton,
                                          svn_client_prompt_t prompt_func,
                                          void *prompt_baton,
                                          apr_pool_t *pool);

/** Set @a *provider and @ *provider_baton to an authentication
 *  provider for type @c svn_auth_cred_client_ssl_t. This provider retrieves
 *  its credentials by using the @a prompt_func and @a prompt_baton. The
 *  returned credential is used to load the appropriate client certificate for
 *  authentication when requested by a server. 
 *  
 *  There are no run-time parameters required for this provider.
 */
void
svn_client_get_ssl_client_prompt_provider(const svn_auth_provider_t **provider,
                                          void **provider_baton,
                                          svn_client_prompt_t prompt_func,
                                          void *prompt_baton,
                                          apr_pool_t *pool);

/** Set @a *provider and @ *provider_baton to an authentication
 *  provider for type @c svn_auth_cred_client_ssl_pass_t. This provider
 *  retrieves its credentials by using the @a prompt_func and @a prompt_baton.
 *  The returned credential is used when a loaded client certificate is
 *  protected by a passphrase.
 *
 *  There are no run-time parameters required for this provider.
 */
void
svn_client_get_ssl_pw_prompt_provider(const svn_auth_provider_t **provider,
                                      void **provider_baton,
                                      svn_client_prompt_t prompt_func,
                                      void *prompt_baton,
                                      apr_pool_t *pool);


/** This is a structure which stores a filename and a hash of property
 * names and values.
 */
typedef struct svn_client_proplist_item_t
{
  /** The name of the node on which these properties are set. */
  svn_stringbuf_t *node_name;  

  /** A hash of (const char *) property names, and (svn_string_t *) property
   * values. */
  apr_hash_t *prop_hash;

} svn_client_proplist_item_t;


/** Information about commits passed back to client from this module. */
typedef struct svn_client_commit_info_t
{
  /** just-committed revision. */
  svn_revnum_t revision;

  /** server-side date of the commit. */
  const char *date;

  /** author of the commit. */
  const char *author;

} svn_client_commit_info_t;


/** State flags for use with the @c svn_client_commit_item_t structure
 *
 * (see the note about the namespace for that structure, which also
 * applies to these flags).
 * @defgroup svn_client_commit_item_flags state flags
 * @{
 */
#define SVN_CLIENT_COMMIT_ITEM_ADD         0x01
#define SVN_CLIENT_COMMIT_ITEM_DELETE      0x02
#define SVN_CLIENT_COMMIT_ITEM_TEXT_MODS   0x04
#define SVN_CLIENT_COMMIT_ITEM_PROP_MODS   0x08
#define SVN_CLIENT_COMMIT_ITEM_IS_COPY     0x10
/** @} */

/** The commit candidate structure. */
typedef struct svn_client_commit_item_t
{
  /** absolute working-copy path of item */
  const char *path;

  /** node kind (dir, file) */
  svn_node_kind_t kind;

  /** commit url for this item */
  const char *url;

  /** revision (copyfrom-rev if _IS_COPY) */
  svn_revnum_t revision;

  /** copyfrom-url */
  const char *copyfrom_url;

  /** state flags */
  apr_byte_t state_flags;

  /** An array of `svn_prop_t *' changes to wc properties.  If adding
   * to this array, allocate the svn_prop_t and its contents in
   * wcprop_changes->pool, so that it has the same lifetime as this
   * svn_client_commit_item_t.
   *
   * See http://subversion.tigris.org/issues/show_bug.cgi?id=806 for 
   * what would happen if the post-commit process didn't group these
   * changes together with all other changes to the item :-).
   */
  apr_array_header_t *wcprop_changes;

} svn_client_commit_item_t;


/** Callback type used by commit-y operations to get a commit log message
 * from the caller.
 *  
 * Set @a *log_msg to the log message for the commit, allocated in @a 
 * pool, or @c NULL if wish to abort the commit process.  Set @a *tmpfile 
 * to the path of any temporary file which might be holding that log 
 * message, or @c NULL if no such file exists (though, if @a *log_msg is 
 * @c NULL, this value is undefined).  The log message MUST be a UTF8 
 * string with LF line separators.
 *
 * @a commit_items is an array of @c svn_client_commit_item_t structures,
 * which may be fully or only partially filled-in, depending on the
 * type of commit operation.
 *
 * @a baton is provided along with the callback for use by the handler.
 *
 * All allocations should be performed in @a pool.
 */
typedef svn_error_t *
(*svn_client_get_commit_log_t) (const char **log_msg,
                                const char **tmp_file,
                                apr_array_header_t *commit_items,
                                void *baton,
                                apr_pool_t *pool);


/** A client context structure, which holds client specific callbacks, 
 * batons, serves as a cache for configuration options, and other various 
 * and sundry things.
 */
typedef struct svn_client_ctx_t
{
  /** main authentication baton. */
  svn_auth_baton_t *auth_baton;

  /** prompt callback function */
  svn_client_prompt_t prompt_func;

  /** prompt callback baton */
  void *prompt_baton;

  /** notification callback function */
  svn_wc_notify_func_t notify_func;

  /** notification callback baton */
  void *notify_baton;

  /** log message callback function */
  svn_client_get_commit_log_t log_msg_func;

  /** log message callback baton */
  void *log_msg_baton;

  /** a hash mapping of <tt>const char *</tt> configuration file names to
   * @c svn_config_t *'s, for example, the '~/.subversion/config' file's 
   * contents should have the key "config".
   */
  apr_hash_t *config;

  /** a callback to be used to see if the client wishes to cancel the running 
   * operation. */
  svn_cancel_func_t cancel_func;

  /** a baton to pass to the cancellation callback. */
  void *cancel_baton;

} svn_client_ctx_t;


/** Names of files that contain authentication information.
 *
 * These filenames are decided by libsvn_client, since this library
 * implements all the auth-protocols;  libsvn_wc does nothing but
 * blindly store and retrieve these files from protected areas.
 *
 * @defgroup svn_client_auth_files authentication files
 * @{
 */
#define SVN_CLIENT_AUTH_USERNAME            "username"
#define SVN_CLIENT_AUTH_PASSWORD            "password"
/** @} */


/** Checkout a working copy of @a url at @a revision, using @a path as 
 * the root directory of the newly checked out working copy, and 
 * authenticating with the authentication baton cached in @a ctx.
 *
 * @a revision must be of kind @c svn_client_revision_number,
 * @c svn_client_revision_head, or @c svn_client_revision_date.  If
 * @c revision does not meet these requirements, return the error
 * @c SVN_ERR_CLIENT_BAD_REVISION.
 *
 * If @a ctx->notify_func is non-null, invoke @a ctx->notify_func with 
 * @a ctx->notify_baton as the checkout progresses.
 *
 * Use @a pool for any temporary allocation.
 */
svn_error_t *
svn_client_checkout (const char *URL,
                     const char *path,
                     const svn_opt_revision_t *revision,
                     svn_boolean_t recurse,
                     svn_client_ctx_t *ctx,
                     apr_pool_t *pool);


/** Update working tree @a path to @a revision, authenticating with
 * the authentication baton cached in @a ctx.
 *
 * @a revision must be of kind @c svn_client_revision_number,
 * @c svn_client_revision_head, or @c svn_client_revision_date.  If @a 
 * revision does not meet these requirements, return the error
 * @c SVN_ERR_CLIENT_BAD_REVISION.
 *
 * If @a ctx->notify_func is non-null, invoke @a ctx->notify_func with 
 * @a ctx->notify_baton for each item handled by the update, and also for 
 * files restored from text-base.
 *
 * Use @a pool for any temporary allocation.
 */
svn_error_t *
svn_client_update (const char *path,
                   const svn_opt_revision_t *revision,
                   svn_boolean_t recurse,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool);


/** Switch working tree @a path to @a url at @a revision, authenticating 
 * with the authentication baton cached in @a ctx.
 *
 * Summary of purpose: this is normally used to switch a working
 * directory over to another line of development, such as a branch or
 * a tag.  Switching an existing working directory is more efficient
 * than checking out @a url from scratch.
 *
 * @a revision must be of kind @c svn_client_revision_number,
 * @c svn_client_revision_head, or @c svn_client_revision_date; otherwise,
 * return @c SVN_ERR_CLIENT_BAD_REVISION.
 *
 * If @a ctx->notify_func is non-null, invoke it with @a ctx->notify_baton 
 * on paths affected by the switch.  Also invoke it for files may be restored
 * from the text-base because they were removed from the working copy.
 *
 * Use @a pool for any temporary allocation.
 */
svn_error_t *
svn_client_switch (const char *path,
                   const char *url,
                   const svn_opt_revision_t *revision,
                   svn_boolean_t recurse,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool);


/** Schedule a working copy @a path for addition to the repository.
 *
 * @a path's parent must be under revision control already, but @a 
 * path is not.  If @a recursive is set, then assuming @a path is a 
 * directory, all of its contents will be scheduled for addition as 
 * well.
 *
 * If @a ctx->notify_func is non-null, then for each added item, call
 * @a ctx->notify_func with @a ctx->notify_baton and the path of the 
 * added item.
 *
 * Important:  this is a *scheduling* operation.  No changes will
 * happen to the repository until a commit occurs.  This scheduling
 * can be removed with svn_client_revert.
 */
svn_error_t *
svn_client_add (const char *path,
                svn_boolean_t recursive,
                svn_client_ctx_t *ctx,
                apr_pool_t *pool);

/** Create a directory, either in a repository or a working copy.
 *
 * If @a path is a URL, use the authentication baton in @a ctx and 
 * @a message to immediately attempt to commit the creation of the directory 
 * @a path in the repository.  If the commit succeeds, allocate (in @a pool) 
 * and populate @a *commit_info.
 *
 * Else, create the directory on disk, and attempt to schedule it for
 * addition (using @c svn_client_add, whose docstring you should
 * read).
 *
 * @a ctx->log_msg_func/@a ctx->log_msg_baton are a callback/baton combo that 
 * this function can use to query for a commit log message when one is
 * needed.
 *
 * If @a ctx->notify_func is non-null, when the directory has been created
 * (successfully) in the working copy, call @a ctx->notify_func with
 * @a ctx->notify_baton and the path of the new directory.  Note that this is
 * only called for items added to the working copy.
 */
svn_error_t *
svn_client_mkdir (svn_client_commit_info_t **commit_info,
                  const char *path,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool);
                  

/** Delete an item from a repository or working copy.
 *
 * If @a path is a @a url, use the authentication baton in @a ctx and 
 * @a ctx->log_msg_func/@a ctx->log_msg_baton to immediately attempt to 
 * commit a deletion of the @a url from the repository.  If the commit 
 * succeeds, allocate (in @a pool) and populate @a *commit_info.
 *
 * Else, schedule a working copy @a path for removal from the repository.
 * @a path's parent must be under revision control. This is just a
 * *scheduling* operation.  No changes will happen to the repository until
 * a commit occurs.  This scheduling can be removed with
 * @c svn_client_revert. If @a path is a file it is immediately removed from 
 * the working copy. If @a path is a directory it will remain in the working 
 * copy but all the files, and all unversioned items, it contains will be
 * removed. If @a force is not set then this operation will fail if @a path
 * contains locally modified and/or unversioned items. If @a force is set 
 * such items will be deleted.
 *
 * @a ctx->log_msg_func/@a ctx->log_msg_baton are a callback/baton combo that 
 * this function can use to query for a commit log message when one is
 * needed.
 *
 * If @a ctx->notify_func is non-null, then for each item deleted, call
 * @a ctx->notify_func with @a ctx->notify_baton and the path of the deleted
 * item.
 */
svn_error_t *
svn_client_delete (svn_client_commit_info_t **commit_info,
                   const char *path,
                   svn_boolean_t force,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool);


/** Import file or directory @a path into repository directory @a url at
 * head, authenticating with the authentication baton cached in @a ctx, 
 * and using @a ctx->log_msg_func/@ctx->log_msg_baton to get a log message 
 * for the (implied) commit.  Set @a *commit_info to the results of the 
 * commit, allocated in @a pool.
 *
 * @a new_entry is the new entry created in the repository directory
 * identified by @a url.  @a new_entry may be null (see below), but may 
 * not be the empty string.
 *
 * If @a path is a directory, the contents of that directory are
 * imported, under a new directory named @a new_entry under @a url; or 
 * if @a new_entry is null, then the contents of @a path are imported 
 * directly into the directory identified by @a url.  Note that the 
 * directory @a path itself is not imported -- that is, the basename of 
 * @a path is not part of the import.
 *
 * If @a path is a file, that file is imported as @a new_entry (which may
 * not be @c NULL).
 *
 * In all cases, if @a new_entry already exists in @a url, return error.
 * 
 * If @a ctx->notify_func is non-null, then call @a ctx->notify_func with 
 * @a ctx->notify_baton as the import progresses, with any of the following 
 * actions: @c svn_wc_notify_commit_added,
 * @c svn_wc_notify_commit_postfix_txdelta.
 *
 * Use @a pool for any temporary allocation.  
 * 
 * @a ctx->log_msg_func/@a ctx->log_msg_baton are a callback/baton combo that 
 * this function can use to query for a commit log message when one is needed.
 *
 * Use @a nonrecursive to indicate that imported directories should not
 * recurse into any subdirectories they may have.
 *
 * ### kff todo: This import is similar to cvs import, in that it does
 * not change the source tree into a working copy.  However, this
 * behavior confuses most people, and I think eventually svn _should_
 * turn the tree into a working copy, or at least should offer the
 * option. However, doing so is a bit involved, and we don't need it
 * right now.  
 */
svn_error_t *svn_client_import (svn_client_commit_info_t **commit_info,
                                const char *path,
                                const char *url,
                                const char *new_entry,
                                svn_boolean_t nonrecursive,
                                svn_client_ctx_t *ctx,
                                apr_pool_t *pool);


/** Commit file or directory @a path into repository, authenticating with
 * the authentication baton cached in @a ctx, and using 
 * @a ctx->log_msg_func/@a ctx->log_msg_baton to obtain the log message. 
 * Set @a *commit_info to the results of the commit, allocated in @a pool.
 *
 * @a targets is an array of <tt>const char *</tt> paths to commit.  They 
 * need not be canonicalized nor condensed; this function will take care of
 * that.  If @a targets has zero elements, then do nothing and return
 * immediately without error.
 *
 * If @a notify_func is non-null, then call @a ctx->notify_func with 
 * @a ctx->notify_baton as the commit progresses, with any of the following 
 * actions: @c svn_wc_notify_commit_modified, @c svn_wc_notify_commit_added,
 * @c svn_wc_notify_commit_deleted, @c svn_wc_notify_commit_replaced,
 * @c svn_wc_notify_commit_postfix_txdelta.
 *
 * Use @a nonrecursive to indicate that subdirectories of directory
 * @a targets should be ignored.
 *
 * Use @a pool for any temporary allocation.
 *
 * If no error is returned and @a (*commit_info)->revision is set to
 * @c SVN_INVALID_REVNUM, then the commit was a no-op; nothing needed to
 * be committed.
 */
svn_error_t *
svn_client_commit (svn_client_commit_info_t **commit_info,
                   const apr_array_header_t *targets,
                   svn_boolean_t nonrecursive,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool);


/** Obtain the statuses of all the items in a working copy path.
 *
 * Given @a path to a working copy directory (or single file), allocate
 * and return a hash @a statushash which maps (<tt>char *</tt>) paths to
 * (@c svn_wc_status_t *) structures.
 *
 * This is a purely local operation; only information found in the
 * administrative `entries' files is used to initially build the
 * structures.
 *
 *    - If @a descend is non-zero, recurse fully, else do only immediate
 *      children.  This (inversely) corresponds to the "-n"
 *      (--nonrecursive) flag in the commandline client app.
 *
 *    - If @a get_all is set, then all entries are retrieved; otherwise
 *      only "interesting" entries (local mods and/or out-of-date)
 *      will be fetched.  This directly corresponds to the "-v"
 *      (--verbose) flag in the commandline client app.
 *
 *    - If @a update is set, then the repository will be contacted, so
 *      that the structures in @a statushash are augmented with
 *      information about out-of-dateness, and @a *youngest is set to the
 *      youngest repository revision (@a *youngest is not touched unless
 *      @a update is set).  This directly corresponds to the "-u"
 *      (--show-updates) flag in the commandline client app.
 *
 * If @a ctx->notify_func is non-null, then call @a ctx->notify_func with 
 * @a ctx->notify_baton as the status progresses.  Specifically, every time 
 * a status structure is added (or tweaked) in the hash, this routine will 
 * pass the pathname with action @c svn_wc_notify_status.  (Note: callers
 * should *not* attempt to look up the pathname in the hash for the
 * purposes of parsing the status structure; a status structure is
 * created in multiple passes, and is not guaranteed to be completely
 * correct until @c svn_client_status completely finishes.)
 */
svn_error_t *
svn_client_status (apr_hash_t **statushash,
                   svn_revnum_t *youngest,  /* only touched if `update' set */
                   const char *path,
                   svn_boolean_t descend,
                   svn_boolean_t get_all,
                   svn_boolean_t update,
                   svn_boolean_t no_ignore,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool);


/** Obtain log information from the repository.
 *
 * Invoke @a receiver with @a receiver_baton on each log message from @a 
 * start to @a end in turn, inclusive (but never invoke @a receiver on a 
 * given log message more than once).
 *
 * @a targets contains all the working copy paths (as <tt>const char 
 * *</tt>'s) for which log messages are desired.  The repository info is
 * determined by taking the common prefix of the target entries' URLs.
 * The common prefix of @a targets, if it is a valid working copy, 
 * determines the auth info.  @a receiver is invoked only on messages 
 * whose revisions involved a change to some path in @a targets.
 *
 * ### todo: the above paragraph is not fully implemented yet.
 *
 * If @a discover_changed_paths is set, then the `@a changed_paths' argument
 * to @a receiver will be passed on each invocation.
 *
 * If @a strict_node_history is set, copy history (if any exists) will
 * not be traversed while harvest revision logs for each target.
 *
 * If @a start->kind or @a end->kind is @c svn_opt_revision_unspecified,
 * return the error @c SVN_ERR_CLIENT_BAD_REVISION.
 *
 * Use @a pool for any temporary allocation.
 *
 * Special case for repositories at revision 0:
 *
 * If @a start->kind is @c svn_opt_revision_head, and @a end->kind is
 * @c svn_opt_revision_number && @a end->number is @c 1, then handle an
 * empty (no revisions) repository specially: instead of erroring
 * because requested revision 1 when the highest revision is 0, just
 * invoke @a receiver on revision 0, passing @c NULL for changed paths and
 * empty strings for the author and date.  This is because that
 * particular combination of @a start and @a end usually indicates the
 * common case of log invocation -- the user wants to see all log
 * messages from youngest to oldest, where the oldest commit is
 * revision 1.  That works fine, except when there are no commits in
 * the repository, hence this special case.
 */
svn_error_t *
svn_client_log (const apr_array_header_t *targets,
                const svn_opt_revision_t *start,
                const svn_opt_revision_t *end,
                svn_boolean_t discover_changed_paths,
                svn_boolean_t strict_node_history,
                svn_log_message_receiver_t receiver,
                void *receiver_baton,
                svn_client_ctx_t *ctx,
                apr_pool_t *pool);


/** Produce diff output which describes the delta between
 * @a path1/@a revision1 and @a path2/@a revision2.  Print the output 
 * of the diff to @a outfile, and any errors to @a errfile.  @a path1 
 * and @a path2 can be either working-copy paths or URLs.
 *
 * If either @a revision1 or @a revision2 has an `unspecified' or
 * unrecognized `kind', return @c SVN_ERR_CLIENT_BAD_REVISION.
 *
 * @a path1 and @a path2 must both represent the same node kind -- that 
 * is, if @a path1 is a directory, @a path2 must also be, and if @a path1 
 * is a file, @a path2 must also be.  (Currently, @a path1 and @a path2 
 * must be the exact same path)
 *
 * If @a recurse is true (and the @a paths are directories) this will be a
 * recursive operation.
 *
 * If @a no_diff_deleted is true, then no diff output will be
 * generated on deleted files.
 * 
 * @a diff_options (an array of <tt>const char *</tt>) is used to pass 
 * additional command line options to the diff processes invoked to compare
 * files.
 *
 * the authentication baton cached in @a ctx is used to communicate with 
 * the repository.
 */
svn_error_t *svn_client_diff (const apr_array_header_t *diff_options,
                              const char *path1,
                              const svn_opt_revision_t *revision1,
                              const char *path2,
                              const svn_opt_revision_t *revision2,
                              svn_boolean_t recurse,
                              svn_boolean_t no_diff_deleted,
                              apr_file_t *outfile,
                              apr_file_t *errfile,
                              svn_client_ctx_t *ctx,
                              apr_pool_t *pool);


/** Merge changes from @a url1/@a revision1 to @a url2/@a revision2 into 
 * the working-copy path @a target_wcpath.
 *
 * By "merging", we mean:  apply file differences using
 * @c svn_wc_merge, and schedule additions & deletions when appropriate.
 *
 * @a url1 and @a url2 must both represent the same node kind -- that is,
 * if @a url1 is a directory, @a url2 must also be, and if @a url1 is a
 * file, @a url2 must also be.
 *
 * If either @a revision1 or @a revision2 has an `unspecified' or
 * unrecognized `kind', return @c SVN_ERR_CLIENT_BAD_REVISION.
 *
 * If @a recurse is true (and the @a urls are directories), apply changes
 * recursively; otherwise, only apply changes in the current
 * directory.
 *
 * If @a force is not set and the merge involves deleting locally modified or
 * unversioned items the operation will fail.  If @a force is set such items
 * will be deleted.
 *
 * If @a ctx->notify_func is non-null, then call @a ctx->notify_func with @a 
 * ctx->notify_baton once for each merged target, passing the target's local 
 * path.
 *
 * If @a dry_run is @a true the merge is carried out, and full notification
 * feedback is provided, but the working copy is not modified.
 *
 * the authentication baton cached in @a ctx is used to communicate with the 
 * repository.
 */
svn_error_t *
svn_client_merge (const char *URL1,
                  const svn_opt_revision_t *revision1,
                  const char *URL2,
                  const svn_opt_revision_t *revision2,
                  const char *target_wcpath,
                  svn_boolean_t recurse,
                  svn_boolean_t force,
                  svn_boolean_t dry_run,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool);


/** Recursively cleanup a working copy directory @a dir, finishing any
 * incomplete operations, removing lockfiles, etc.
 *
 * If @a ctx->cancel_func is non-null, invoke it with @a
 * ctx->cancel_baton at various points during the operation.  If it
 * returns an error (typically SVN_ERR_CANCELLED), return that error
 * immediately.
 */
svn_error_t *
svn_client_cleanup (const char *dir,
                    svn_client_ctx_t *ctx,
                    apr_pool_t *pool);


/** Restore the pristine version of a working copy @a path, effectively
 * undoing any local mods.  If @a path is a directory, and @a recursive 
 * is @a true, this will be a recursive operation.
 *
 * If @a ctx->notify_func is non-null, then for each item reverted, call
 * @a ctx->notify_func with @a ctx->notify_baton and the path of the reverted 
 * item.
 */
svn_error_t *
svn_client_revert (const char *path,
                   svn_boolean_t recursive,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool);


/** Remove the 'conflicted' state on a working copy @a path.  This will
 * not semantically resolve conflicts;  it just allows @a path to be
 * committed in the future.  The implementation details are opaque.
 * If @a recursive is set, recurse below @a path, looking for conflicts 
 * to resolve.
 *
 * If @a path is not in a state of conflict to begin with, do nothing.
 * If @a path's conflict state is removed and @a ctx->notify_func is non-null,
 * call @a ctx->notify_func with @a ctx->notify_baton and @a path.
 */
svn_error_t *
svn_client_resolve (const char *path,
                    svn_boolean_t recursive,
                    svn_client_ctx_t *ctx,
                    apr_pool_t *pool);


/** Copy @a src_path to @a dst_path.
 *
 * @a src_path must be a file or directory under version control, or the
 * @a url of a versioned item in the repository.  If @a src_path is a @a 
 * url, @a src_revision is used to choose the revision from which to copy 
 * the @a src_path.  @a dst_path must be a file or directory under version
 * control, or a repository @a url, existent or not.
 *
 * ### 838 The argument to be removed when 838 stops using @c svn_client_copy.
 * @a optional_adm_access can either be a baton that holds a write lock for
 * the parent of @a path, or it can be @c NULL. If it is @c NULL the lock for 
 * the parent will be acquired and released by the function.
 *
 * If either @a src_path or @a dst_path are URLs, use the authentication baton 
 * in @a ctx and @a ctx->log_msg_func/@a ctx->log_msg_baton to immediately 
 * attempt to commit the copy action in the repository.  If the commit 
 * succeeds, allocate (in @a pool) and populate @a *commit_info.
 *
 * If neither @a src_path nor @a dst_path is a URL, then this is just a
 * variant of @c svn_client_add, where the @a dst_path items are scheduled
 * for addition as copies.  No changes will happen to the repository
 * until a commit occurs.  This scheduling can be removed with
 * @c svn_client_revert.
 *
 * @a ctx->log_msg_func/@a ctx->log_msg_baton are a callback/baton combo that
 * this function can use to query for a commit log message when one is
 * needed.
 *
 * If @a ctx->notify_func is non-null, invoke it with @a ctx->notify_baton 
 * for each item added at the new location, passing the new, relative path of
 * the added item.
 */
svn_error_t *
svn_client_copy (svn_client_commit_info_t **commit_info,
                 const char *src_path,
                 const svn_opt_revision_t *src_revision,
                 const char *dst_path,
                 svn_wc_adm_access_t *optional_adm_access,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool);


/** Move @a src_path to @a dst_path.
 *
 * @a src_path must be a file or directory under version control, or the
 * URL of a versioned item in the repository.  
 *
 * If @a src_path is a repository URL:
 *
 *   - @a dst_path must also be a repository URL (existent or not).
 *
 *   - @a src_revision is used to choose the revision from which to copy 
 *     the @a src_path.
 *
 *   - the authentication baton in @a ctx and @a ctx->log_msg_func/@a 
 *     ctx->log_msg_baton are used to commit the move.
 *
 *   - The move operation will be immediately committed.  If the
 *     commit succeeds, allocate (in @a pool) and populate @a *commit_info.
 *
 * If @a src_path is a working copy path
 *
 *   - @a dst_path must also be a working copy path (existent or not).
 *
 *   - @a src_revision, and @a ctx->log_msg_func/@a ctx->log_msg_baton are 
 *     ignored.
 *
 *   - This is a scheduling operation.  No changes will happen to the
 *     repository until a commit occurs.  This scheduling can be removed
 *     with @c svn_client_revert.  If @a src_path is a file it is removed 
 *     from the working copy immediately.  If @a src_path is a directory it 
 *     will remain n the working copy but all the files, and unversioned 
 *     items, it contains will be removed.
 *
 *   - If @a src_path contains locally modified and/or unversioned items 
 *     and @a force is not set, the copy will fail. If @a force is set such 
 *     items will be removed.
 *
 * @a ctx->log_msg_func/@a ctx->log_msg_baton are a callback/baton combo that
 * this function can use to query for a commit log message when one is needed.
 *
 * If @a ctx->notify_func is non-null, then for each item moved, call
 * @a ctx->notify_func with the @a ctx->notify_baton twice, once to indicate 
 * the deletion of the moved thing, and once to indicate the addition of
 * the new location of the thing.
 *
 * ### Is this really true?  What about @c svn_wc_notify_commit_replaced? ### 
 */ 
svn_error_t *
svn_client_move (svn_client_commit_info_t **commit_info,
                 const char *src_path,
                 const svn_opt_revision_t *src_revision,
                 const char *dst_path,
                 svn_boolean_t force,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool);


/** Properties
 *
 * Note that certain svn-controlled properties must always have their
 * values set and stored in UTF8 with LF line endings.  When
 * retrieving these properties, callers must convert the values back
 * to native locale and native line-endings before displaying them to
 * the user.  For help with this task, see
 * @c svn_prop_needs_translation, @c svn_subst_translate_string,  and @c 
 * svn_subst_detranslate_string.
 *
 * @defgroup svn_client_prop_funcs property functions
 * @{
 */


/** Set @a propname to @a propval on @a target.  If @a recurse is true, 
 * then @a propname will be set on recursively on @a target and all 
 * children.  If @a recurse is false, and @a target is a directory, @a 
 * propname will be set on _only_ @a target.
 * 
 * A @a propval of @c NULL will delete the property.
 *
 * If @a propname is an svn-controlled property (i.e. prefixed with
 * @c SVN_PROP_PREFIX), then the caller is responsible for ensuring that
 * the value UTF8-encoded and uses LF line-endings.
 *
 * Use @a pool for all memory allocation.
 */
svn_error_t *
svn_client_propset (const char *propname,
                    const svn_string_t *propval,
                    const char *target,
                    svn_boolean_t recurse,
                    apr_pool_t *pool);

/** Set @a propname to @a propval on revision @a revision in the repository
 * represented by @a url.  Use the authentication baton in @a ctx for 
 * authentication, and @a pool for all memory allocation.  Return the actual 
 * rev affected in @a *set_rev.  A @a propval of @c NULL will delete the 
 * property.
 *
 * If @a propname is an svn-controlled property (i.e. prefixed with
 * @c SVN_PROP_PREFIX), then the caller is responsible for ensuring that
 * the value UTF8-encoded and uses LF line-endings.
 *
 * Note that unlike its cousin @c svn_client_propset, this routine
 * doesn't affect the working copy at all;  it's a pure network
 * operation that changes an *unversioned* property attached to a
 * revision.  This can be used to tweak log messages, dates, authors,
 * and the like.  Be careful:  it's a lossy operation.
 *
 * Also note that unless the administrator creates a
 * pre-revprop-change hook in the repository, this feature will fail.
 */
svn_error_t *
svn_client_revprop_set (const char *propname,
                        const svn_string_t *propval,
                        const char *URL,
                        const svn_opt_revision_t *revision,
                        svn_revnum_t *set_rev,
                        svn_client_ctx_t *ctx,
                        apr_pool_t *pool);
                        
/** Get properties from an entry in a working copy or repository.
 *
 * Set @a *props to a hash table whose keys are `<tt>char *</tt>' paths,
 * prefixed by @a target (a working copy path or a url), of items on
 * which property @a propname is set, and whose values are `@c svn_string_t
 * *' representing the property value for @a propname at that path.
 *
 * Allocate @a *props, its keys, and its values in @a pool.
 *           
 * Don't store any path, not even @a target, if it does not have a
 * property named @a propname.
 *
 * If @a revision->kind is @c svn_opt_revision_unspecified, then: get
 * properties from the working copy if @a target is a working copy path,
 * or from the repository head if @a target is a url.  Else get the
 * properties as of @a revision.  Use the authentication baton in @a ctx 
 * for authentication if contacting the repository.
 *
 * If @a target is a file or @a recurse is false, @a *props will have
 * at most one element.
 *
 * If error, don't touch @a *props, otherwise @a *props is a hash table 
 * even if empty.
 */
svn_error_t *
svn_client_propget (apr_hash_t **props,
                    const char *propname,
                    const char *target,
                    const svn_opt_revision_t *revision,
                    svn_boolean_t recurse,
                    svn_client_ctx_t *ctx,
                    apr_pool_t *pool);

/** Get a revision property from a repository URL.
 *
 * Set @a *propname to the value of @a propval on revision @a revision 
 * in the repository represented by @a url.  Use the authentication baton 
 * in @a ctx for authentication, and @a pool for all memory allocation.  
 * Return the actual rev queried in @a *set_rev.
 *
 * Note that unlike its cousin @c svn_client_propget, this routine
 * doesn't affect the working copy at all; it's a pure network
 * operation that queries an *unversioned* property attached to a
 * revision.  This can be query log messages, dates, authors, and the
 * like.
 */
svn_error_t *
svn_client_revprop_get (const char *propname,
                        svn_string_t **propval,
                        const char *URL,
                        const svn_opt_revision_t *revision,
                        svn_revnum_t *set_rev,
                        svn_client_ctx_t *ctx,
                        apr_pool_t *pool);

/** List the properties on an entry in a working copy or repository.
 *
 * Set @a *props to the regular properties of @a target, a url or working
 * copy path.
 *
 * Each element of the returned array is (@c svn_client_proplist_item_t *).
 * For each item, item->node_name contains the name relative to the
 * same base as @a target, and @a item->prop_hash maps (<tt>const char *</tt>)
 * property names to (@c svn_string_t *) values.
 * 
 * Allocate @a *props and its contents in @a pool.
 *
 * If @a revision->kind is @c svn_opt_revision_unspecified, then get
 * properties from the working copy, if @a target is a working copy path,
 * or from the repository head if @a target is a url.  Else get the
 * properties as of @a revision.  Use the authentication baton cached in @a ctx 
 * for authentication if contacting the repository.
 *
 * If @a recurse is false, or @a target is a file, @a *props will contain 
 * only a single element.  Otherwise, it will contain one element for each
 * versioned entry below (and including) @a target.
 */
svn_error_t *
svn_client_proplist (apr_array_header_t **props,
                     const char *target, 
                     const svn_opt_revision_t *revision,
                     svn_boolean_t recurse,
                     svn_client_ctx_t *ctx,
                     apr_pool_t *pool);

/** List the revision properties on an entry in a repository.
 *
 * Set @a *props to a hash of the revision props attached to @a revision in
 * the repository represented by @a url.  Use the authentication baton cached 
 * in @a ctx for authentication, and @a pool for all memory allocation.  
 * Return the actual rev queried in @a *set_rev.
 *
 * The allocated hash maps (<tt>const char *</tt>) property names to
 * (@c svn_string_t *) property values.
 *
 * Note that unlike its cousin @c svn_client_proplist, this routine
 * doesn't read a working copy at all; it's a pure network operation
 * that reads *unversioned* properties attached to a revision.
 */
svn_error_t *
svn_client_revprop_list (apr_hash_t **props,
                         const char *URL,
                         const svn_opt_revision_t *revision,
                         svn_revnum_t *set_rev,
                         svn_client_ctx_t *ctx,
                         apr_pool_t *pool);
/** @} */


/** Export the contents of either a subversion repository or a subversion 
 * working copy into a 'clean' directory (meaning a directory with no 
 * administrative directories).
 *
 * @a from is either the path the working copy on disk, or a URL to the
 * repository you wish to export.
 *
 * @a to is the path to the directory where you wish to create the exported
 * tree.
 *
 * @a revision is the revision that should be exported, which is only used 
 * when exporting from a repository.
 *
 * @a ctx->notify_func and @a ctx->notify_baton are the notification functions
 * and baton which are passed to @c svn_client_checkout when exporting from a 
 * repository.
 *
 * @a ctx is a context used for authentication in the repository case.
 *
 * All allocations are done in @a pool.
 */ 
svn_error_t *
svn_client_export (const char *from,
                   const char *to,
                   svn_opt_revision_t *revision,
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool);


/** List the contents of @a path_or_url.
 *
 * Set @a *dirents to a newly allocated hash of entries for @a path_or_url
 * at @a revision.
 *
 * If @a path_or_url is a directory, return all dirents in the hash.  If
 * @a path_or_url is a file, return only the dirent for the file.  If @a
 * path_or_url is non-existent, return @c SVN_ERR_FS_NOT_FOUND.
 *
 * The hash maps entrynames (<tt>const char *</tt>) to @c svn_dirent_t *'s.  
 * Do all allocation in @a pool.
 *
 * Use authentication baton cached in @a ctx to authenticate against the 
 * repository.
 *
 * If @a recurse is true (and @a path_or_url is a directory) this will
 * be a recursive operation.
 */
svn_error_t *
svn_client_ls (apr_hash_t **dirents,
               const char *path_or_url,
               svn_opt_revision_t *revision,
               svn_boolean_t recurse,
               svn_client_ctx_t *ctx,
               apr_pool_t *pool);


/** Output the content of file identified by @a path_or_url and @a
 * revision to the stream @a out.
 *
 * If @a path_or_url is not a local path, then if @a revision is of
 * kind @c svn_opt_revision_previous (or some other kind that requires
 * a local path), an error will be returned, because the desired
 * revision cannot be determined.
 *
 * Use the authentication baton cached in @a ctx to authenticate against the 
 * repository.
 *
 * Perform all allocations from @a pool.
 *
 * ### TODO: Add an expansion/translation flag?
 */
svn_error_t *
svn_client_cat (svn_stream_t* out,
                const char *path_or_url,
                const svn_opt_revision_t *revision,
                svn_client_ctx_t *ctx,
                apr_pool_t *pool);



/* Converting paths to URLs. */

/** Set @a *url to the url for @a path_or_url.
 *
 * If @a path_or_url is already a url, set @a *url to @a path_or_url.
 *
 * If @a path_or_url is a versioned item, set @a *url to @a
 * path_or_url's entry url.  If @a path_or_url is a unversioned (has
 * no entry), set @a *url to null.
 */
svn_error_t *
svn_client_url_from_path (const char **url,
                          const char *path_or_url,
                          apr_pool_t *pool);




/* Fetching repository UUIDs. */

/** Get repository @a uuid for @a url.
 *
 * Use a @a pool to open a temporary RA session to @a url, discover the
 * repository uuid, and free the session.  Return the uuid in @a uuid,
 * allocated in @a pool.  @a ctx is required for possible repository
 * authentication.
 */
svn_error_t *
svn_client_uuid_from_url (const char **uuid,
                          const char *url,
                          svn_client_ctx_t *ctx,
                          apr_pool_t *pool);


/** Return the repository @a uuid for working-copy @a path, allocated
 *  in @a pool, using network if required.
 *
 * Return the repository @a uuid for working-copy @a path, allocated
 * in @a pool.  Use @a adm_access to retrieve the uuid from @a path's
 * entry; if not present in the entry, then call
 * svn_client_uuid_from_url() to retrieve, using the entry's url.  @a
 * ctx is required for possible repository authentication.
 *
 * NOTE:  the only reason this function falls back on
 * @c svn_client_uuid_from_url is for compatibility purposes.  Old
 * working copies may not have uuids in the entries file.
 */
svn_error_t *
svn_client_uuid_from_path (const char **uuid,
                           const char *path,
                           svn_wc_adm_access_t *adm_access,
                           svn_client_ctx_t *ctx,
                           apr_pool_t *pool);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif  /* SVN_CLIENT_H */
