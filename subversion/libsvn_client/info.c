/*
 * info.c:  return system-generated metadata about paths or URLs.
 *
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
 */

/* ==================================================================== */



#include "client.h"
#include "svn_client.h"
#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_hash.h"
#include "svn_wc.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"


/* Helper: build an svn_info_t *INFO struct from svn_dirent_t DIRENT
   and (possibly NULL) svn_lock_t LOCK, all allocated in POOL.
   Pointer fields are copied by reference, not dup'd. */
static svn_error_t *
build_info_from_dirent(svn_info_t **info,
                       const svn_dirent_t *dirent,
                       svn_lock_t *lock,
                       const char *URL,
                       svn_revnum_t revision,
                       const char *repos_UUID,
                       const char *repos_root,
                       apr_pool_t *pool)
{
  svn_info_t *tmpinfo = apr_pcalloc(pool, sizeof(*tmpinfo));

  tmpinfo->URL                  = URL;
  tmpinfo->rev                  = revision;
  tmpinfo->kind                 = dirent->kind;
  tmpinfo->repos_UUID           = repos_UUID;
  tmpinfo->repos_root_URL       = repos_root;
  tmpinfo->last_changed_rev     = dirent->created_rev;
  tmpinfo->last_changed_date    = dirent->time;
  tmpinfo->last_changed_author  = dirent->last_author;
  tmpinfo->lock                 = lock;
  tmpinfo->depth                = svn_depth_unknown;
  tmpinfo->working_size         = SVN_INFO_SIZE_UNKNOWN;

  if (((apr_size_t)dirent->size) == dirent->size)
    tmpinfo->size               = (apr_size_t)dirent->size;
  else /* >= 4GB */
    tmpinfo->size               = SVN_INFO_SIZE_UNKNOWN;

  tmpinfo->size64               = dirent->size;
  tmpinfo->working_size64       = SVN_INVALID_FILESIZE;
  tmpinfo->tree_conflict        = NULL;

  *info = tmpinfo;
  return SVN_NO_ERROR;
}


/* Helper: build an svn_info_t *INFO struct from svn_wc_entry_t ENTRY,
   allocated in POOL.  Pointer fields are copied by reference, not dup'd.
   PATH is the path of the WC node that ENTRY represents. */
static svn_error_t *
build_info_from_entry(svn_info_t **info,
                      svn_wc_context_t *wc_ctx,
                      const svn_wc_entry_t *entry,
                      const char *path,
                      apr_pool_t *pool)
{
  const char *local_abspath;
  svn_info_t *tmpinfo = apr_pcalloc(pool, sizeof(*tmpinfo));

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));

  SVN_ERR(svn_wc__node_get_kind(&tmpinfo->kind, wc_ctx, local_abspath, TRUE,
                                pool));

  tmpinfo->URL                  = entry->url;
  tmpinfo->rev                  = entry->revision;
  tmpinfo->kind                 = entry->kind;
  tmpinfo->repos_UUID           = entry->uuid;
  tmpinfo->repos_root_URL       = entry->repos;
  SVN_ERR(svn_wc__node_get_changed_info(&tmpinfo->last_changed_rev,
                                        &tmpinfo->last_changed_date,
                                        &tmpinfo->last_changed_author,
                                        wc_ctx, local_abspath, pool, pool));

  /* entry-specific stuff */
  tmpinfo->has_wc_info          = TRUE;
  tmpinfo->schedule             = entry->schedule;
  tmpinfo->depth                = entry->depth;
  tmpinfo->copyfrom_url         = entry->copyfrom_url;
  tmpinfo->copyfrom_rev         = entry->copyfrom_rev;
  tmpinfo->text_time            = entry->text_time;
  tmpinfo->checksum             = entry->checksum;
  tmpinfo->conflict_old         = entry->conflict_old;
  tmpinfo->conflict_new         = entry->conflict_new;
  tmpinfo->conflict_wrk         = entry->conflict_wrk;
  tmpinfo->prejfile             = entry->prejfile;
  tmpinfo->changelist           = entry->changelist;

  if (((apr_size_t)entry->working_size) == entry->working_size)
    tmpinfo->working_size       = (apr_size_t)entry->working_size;
  else /* >= 4GB */
    tmpinfo->working_size       = SVN_INFO_SIZE_UNKNOWN;

  tmpinfo->size                 = SVN_INFO_SIZE_UNKNOWN;
  tmpinfo->size64               = SVN_INVALID_FILESIZE;

  tmpinfo->working_size64       = entry->working_size;

  /* lock stuff */
  if (entry->lock_token)  /* the token is the critical bit. */
    {
      tmpinfo->lock = apr_pcalloc(pool, sizeof(*(tmpinfo->lock)));

      tmpinfo->lock->token      = entry->lock_token;
      tmpinfo->lock->owner      = entry->lock_owner;
      tmpinfo->lock->comment    = entry->lock_comment;
      tmpinfo->lock->creation_date = entry->lock_creation_date;
    }

  *info = tmpinfo;
  return SVN_NO_ERROR;
}


/* Helper: build an svn_info_t *INFO struct with minimal content, to be
   used in reporting info for unversioned tree conflict victims. */
/* ### Some fields we could fill out based on the parent dir's entry
       or by looking at an obstructing item. */
static svn_error_t *
build_info_for_unversioned(svn_info_t **info,
                           apr_pool_t *pool)
{
  svn_info_t *tmpinfo = apr_pcalloc(pool, sizeof(*tmpinfo));

  tmpinfo->URL                  = NULL;
  tmpinfo->rev                  = SVN_INVALID_REVNUM;
  tmpinfo->kind                 = svn_node_none;
  tmpinfo->repos_UUID           = NULL;
  tmpinfo->repos_root_URL       = NULL;
  tmpinfo->last_changed_rev     = SVN_INVALID_REVNUM;
  tmpinfo->last_changed_date    = 0;
  tmpinfo->last_changed_author  = NULL;
  tmpinfo->lock                 = NULL;
  tmpinfo->working_size         = SVN_INFO_SIZE_UNKNOWN;
  tmpinfo->size                 = SVN_INFO_SIZE_UNKNOWN;
  tmpinfo->size64               = SVN_INVALID_FILESIZE;
  tmpinfo->working_size64       = SVN_INVALID_FILESIZE;
  tmpinfo->tree_conflict        = NULL;

  *info = tmpinfo;
  return SVN_NO_ERROR;
}


/* The dirent fields we care about for our calls to svn_ra_get_dir2. */
#define DIRENT_FIELDS (SVN_DIRENT_KIND        | \
                       SVN_DIRENT_CREATED_REV | \
                       SVN_DIRENT_TIME        | \
                       SVN_DIRENT_LAST_AUTHOR)


/* Helper func for recursively fetching svn_dirent_t's from a remote
   directory and pushing them at an info-receiver callback.

   DEPTH is the depth starting at DIR, even though RECEIVER is never
   invoked on DIR: if DEPTH is svn_depth_immediates, then invoke
   RECEIVER on all children of DIR, but none of their children; if
   svn_depth_files, then invoke RECEIVER on file children of DIR but
   not on subdirectories; if svn_depth_infinity, recurse fully.
*/
static svn_error_t *
push_dir_info(svn_ra_session_t *ra_session,
              const char *session_URL,
              const char *dir,
              svn_revnum_t rev,
              const char *repos_UUID,
              const char *repos_root,
              svn_info_receiver_t receiver,
              void *receiver_baton,
              svn_depth_t depth,
              svn_client_ctx_t *ctx,
              apr_hash_t *locks,
              apr_pool_t *pool)
{
  apr_hash_t *tmpdirents;
  svn_info_t *info;
  apr_hash_index_t *hi;
  apr_pool_t *subpool = svn_pool_create(pool);

  SVN_ERR(svn_ra_get_dir2(ra_session, &tmpdirents, NULL, NULL,
                          dir, rev, DIRENT_FIELDS, pool));

  for (hi = apr_hash_first(pool, tmpdirents); hi; hi = apr_hash_next(hi))
    {
      const char *path, *URL, *fs_path;
      svn_lock_t *lock;
      const char *name = svn_apr_hash_index_key(hi);
      svn_dirent_t *the_ent = svn_apr_hash_index_val(hi);

      svn_pool_clear(subpool);

      if (ctx->cancel_func)
        SVN_ERR(ctx->cancel_func(ctx->cancel_baton));

      path = svn_path_join(dir, name, subpool);
      URL  = svn_path_url_add_component2(session_URL, name, subpool);

      fs_path = svn_path_is_child(repos_root, URL, subpool);
      fs_path = apr_pstrcat(subpool, "/", fs_path, NULL);
      fs_path = svn_path_uri_decode(fs_path, subpool);

      lock = apr_hash_get(locks, fs_path, APR_HASH_KEY_STRING);

      SVN_ERR(build_info_from_dirent(&info, the_ent, lock, URL, rev,
                                     repos_UUID, repos_root, subpool));

      if (depth >= svn_depth_immediates
          || (depth == svn_depth_files && the_ent->kind == svn_node_file))
        {
          SVN_ERR(receiver(receiver_baton, path, info, subpool));
        }

      if (depth == svn_depth_infinity && the_ent->kind == svn_node_dir)
        {
          SVN_ERR(push_dir_info(ra_session, URL, path,
                                rev, repos_UUID, repos_root,
                                receiver, receiver_baton,
                                depth, ctx, locks, subpool));
        }
    }

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}



/* Callback and baton for crawl_entries() walk over entries files. */
struct found_entry_baton
{
  apr_hash_t *changelist_hash;
  svn_info_receiver_t receiver;
  void *receiver_baton;
  svn_wc_context_t *wc_ctx;
};

/* An svn_wc_entry_callbacks2_t callback function. */
static svn_error_t *
info_found_entry_callback(const char *path,
                          const svn_wc_entry_t *entry,
                          void *walk_baton,
                          apr_pool_t *pool)
{
  struct found_entry_baton *fe_baton = walk_baton;
  const char *local_abspath;

  /* We're going to receive dirents twice;  we want to ignore the
     first one (where it's a child of a parent dir), and only print
     the second one (where we're looking at THIS_DIR.)  */
  if ((entry->kind == svn_node_dir)
      && (strcmp(entry->name, SVN_WC_ENTRY_THIS_DIR)))
    return SVN_NO_ERROR;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
  if (svn_wc__changelist_match(fe_baton->wc_ctx, local_abspath,
                               fe_baton->changelist_hash, pool))
    {
      svn_info_t *info;
      svn_wc_conflict_description2_t *tmp_conflict;

      SVN_ERR(build_info_from_entry(&info, fe_baton->wc_ctx, entry, path,
                                    pool));
      SVN_ERR(svn_wc__get_tree_conflict(&tmp_conflict, fe_baton->wc_ctx,
                                        local_abspath, pool, pool));
      if (tmp_conflict)
        info->tree_conflict = svn_wc__cd2_to_cd(tmp_conflict, pool);
      SVN_ERR(fe_baton->receiver(fe_baton->receiver_baton, path, info, pool));
    }
  return SVN_NO_ERROR;
}

/* An svn_wc_entry_callbacks2_t callback function.
   Handle an error encountered by the walker.
   If the error is "unversioned resource" and, upon checking the
   parent dir's tree conflict data, we find that PATH is a tree
   conflict victim, cancel the error and send a minimal info struct.
   Otherwise re-raise the error.
*/
static svn_error_t *
info_error_handler(const char *path,
                   svn_error_t *err,
                   void *walk_baton,
                   apr_pool_t *pool)
{
  if (err && (err->apr_err == SVN_ERR_UNVERSIONED_RESOURCE))
    {
      struct found_entry_baton *fe_baton = walk_baton;
      svn_wc_conflict_description2_t *tree_conflict;
      const char *local_abspath;

      SVN_ERR(svn_dirent_get_absolute(&local_abspath, path, pool));
      SVN_ERR(svn_wc__get_tree_conflict(&tree_conflict, fe_baton->wc_ctx,
                                        local_abspath, pool, pool));

      if (tree_conflict)
        {
          svn_info_t *info;

          svn_error_clear(err);

          SVN_ERR(build_info_for_unversioned(&info, pool));
          info->tree_conflict = svn_wc__cd2_to_cd(tree_conflict, pool);

          SVN_ERR(svn_wc__node_get_repos_info(&(info->repos_root_URL),
                                              NULL,
                                              fe_baton->wc_ctx,
                                              local_abspath,
                                              pool, pool));

          SVN_ERR(fe_baton->receiver(fe_baton->receiver_baton, path, info,
                                     pool));
          return SVN_NO_ERROR;
        }
    }

  return svn_error_return(err);
}

static const svn_wc_entry_callbacks2_t
entry_walk_callbacks =
  {
    info_found_entry_callback,
    info_error_handler
  };


/* Helper function:  push the svn_wc_entry_t for WCPATH at
   RECEIVER/BATON, and possibly recurse over more entries. */
static svn_error_t *
crawl_entries(const char *wcpath,
              svn_info_receiver_t receiver,
              void *receiver_baton,
              svn_depth_t depth,
              apr_hash_t *changelist_hash,
              svn_client_ctx_t *ctx,
              apr_pool_t *pool)
{
  svn_wc_adm_access_t *adm_access;
  int adm_lock_level = SVN_WC__LEVELS_TO_LOCK_FROM_DEPTH(depth);
  struct found_entry_baton fe_baton;

  SVN_ERR(svn_wc__adm_probe_in_context(&adm_access, ctx->wc_ctx, wcpath, FALSE,
                                       adm_lock_level, ctx->cancel_func,
                                       ctx->cancel_baton, pool));

  fe_baton.changelist_hash = changelist_hash;
  fe_baton.receiver = receiver;
  fe_baton.receiver_baton = receiver_baton;
  fe_baton.wc_ctx = ctx->wc_ctx;
  return svn_wc_walk_entries3(wcpath, adm_access,
                              &entry_walk_callbacks, &fe_baton,
                              depth, FALSE, ctx->cancel_func,
                              ctx->cancel_baton, pool);
}

/* Set *SAME_P to TRUE if URL exists in the head of the repository and
   refers to the same resource as it does in REV, using POOL for
   temporary allocations.  RA_SESSION is an open RA session for URL.  */
static svn_error_t *
same_resource_in_head(svn_boolean_t *same_p,
                      const char *url,
                      svn_revnum_t rev,
                      svn_ra_session_t *ra_session,
                      svn_client_ctx_t *ctx,
                      apr_pool_t *pool)
{
  svn_error_t *err;
  svn_opt_revision_t start_rev, end_rev, peg_rev;
  svn_opt_revision_t *ignored_rev;
  const char *head_url, *ignored_url;

  start_rev.kind = svn_opt_revision_head;
  peg_rev.kind = svn_opt_revision_number;
  peg_rev.value.number = rev;
  end_rev.kind = svn_opt_revision_unspecified;

  err = svn_client__repos_locations(&head_url, &ignored_rev,
                                    &ignored_url, &ignored_rev,
                                    ra_session,
                                    url, &peg_rev,
                                    &start_rev, &end_rev,
                                    ctx, pool);
  if (err &&
      ((err->apr_err == SVN_ERR_CLIENT_UNRELATED_RESOURCES) ||
       (err->apr_err == SVN_ERR_FS_NOT_FOUND)))
    {
      svn_error_clear(err);
      *same_p = FALSE;
      return SVN_NO_ERROR;
    }
  else 
    SVN_ERR(err);

  /* ### Currently, the URLs should always be equal, since we can't
     ### walk forwards in history. */
  *same_p = (strcmp(url, head_url) == 0);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_info2(const char *path_or_url,
                 const svn_opt_revision_t *peg_revision,
                 const svn_opt_revision_t *revision,
                 svn_info_receiver_t receiver,
                 void *receiver_baton,
                 svn_depth_t depth,
                 const apr_array_header_t *changelists,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool)
{
  svn_ra_session_t *ra_session, *parent_ra_session;
  svn_revnum_t rev;
  const char *url;
  svn_node_kind_t url_kind;
  const char *repos_root_URL, *repos_UUID;
  svn_lock_t *lock;
  svn_boolean_t related;
  apr_hash_t *parent_ents;
  const char *parent_url, *base_name;
  svn_dirent_t *the_ent;
  svn_info_t *info;
  svn_error_t *err;

  if ((revision == NULL
       || revision->kind == svn_opt_revision_unspecified)
      && (peg_revision == NULL
          || peg_revision->kind == svn_opt_revision_unspecified))
    {
      /* Do all digging in the working copy. */
      apr_hash_t *changelist_hash = NULL;
      if (changelists && changelists->nelts)
        SVN_ERR(svn_hash_from_cstring_keys(&changelist_hash,
                                           changelists, pool));
      return crawl_entries(path_or_url, receiver, receiver_baton,
                           depth, changelist_hash, ctx, pool);
    }

  /* Go repository digging instead. */

  /* Trace rename history (starting at path_or_url@peg_revision) and
     return RA session to the possibly-renamed URL as it exists in REVISION.
     The ra_session returned will be anchored on this "final" URL. */
  SVN_ERR(svn_client__ra_session_from_path(&ra_session, &rev,
                                           &url, path_or_url, NULL,
                                           peg_revision,
                                           revision, ctx, pool));

  SVN_ERR(svn_ra_get_repos_root2(ra_session, &repos_root_URL, pool));
  SVN_ERR(svn_ra_get_uuid2(ra_session, &repos_UUID, pool));

  svn_uri_split(url, &parent_url, &base_name, pool);
  base_name = svn_path_uri_decode(base_name, pool);

  /* Get the dirent for the URL itself. */
  err = svn_ra_stat(ra_session, "", rev, &the_ent, pool);

  /* svn_ra_stat() will work against old versions of mod_dav_svn, but
     not old versions of svnserve.  In the case of a pre-1.2 svnserve,
     catch the specific error it throws:*/
  if (err && err->apr_err == SVN_ERR_RA_NOT_IMPLEMENTED)
    {
      /* Fall back to pre-1.2 strategy for fetching dirent's URL. */
      svn_error_clear(err);

      if (strcmp(url, repos_root_URL) == 0)
        {
          /* In this universe, there's simply no way to fetch
             information about the repository's root directory!
             If we're recursing, degrade gracefully: rather than
             throw an error, return no information about the
             repos root. */
          if (depth > svn_depth_empty)
            goto pre_1_2_recurse;

          /* Otherwise, we really are stuck.  Better tell the user
             what's going on. */
          return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                   _("Server does not support retrieving "
                                     "information about the repository root"));
        }

      SVN_ERR(svn_ra_check_path(ra_session, "", rev, &url_kind, pool));
      if (url_kind == svn_node_none)
        return svn_error_createf(SVN_ERR_RA_ILLEGAL_URL, NULL,
                                 _("URL '%s' non-existent in revision %ld"),
                                 url, rev);

      /* Open a new RA session to the item's parent. */
      SVN_ERR(svn_client__open_ra_session_internal(&parent_ra_session,
                                                   parent_url, NULL,
                                                   NULL, FALSE, TRUE,
                                                   ctx, pool));

      /* Get all parent's entries, and find the item's dirent in the hash. */
      SVN_ERR(svn_ra_get_dir2(parent_ra_session, &parent_ents, NULL, NULL,
                              "", rev, DIRENT_FIELDS, pool));
      the_ent = apr_hash_get(parent_ents, base_name, APR_HASH_KEY_STRING);
      if (the_ent == NULL)
        return svn_error_createf(SVN_ERR_RA_ILLEGAL_URL, NULL,
                                 _("URL '%s' non-existent in revision %ld"),
                                 url, rev);
    }
  else if (err)
    {
      return svn_error_return(err);
    }

  if (! the_ent)
    return svn_error_createf(SVN_ERR_RA_ILLEGAL_URL, NULL,
                             _("URL '%s' non-existent in revision %ld"),
                             url, rev);

  /* Check if the URL exists in HEAD and refers to the same resource.
     In this case, we check the repository for a lock on this URL.

     ### There is a possible race here, since HEAD might have changed since
     ### we checked it.  A solution to this problem could be to do the below
     ### check in a loop which only terminates if the HEAD revision is the same
     ### before and after this check.  That could, however, lead to a
     ### starvation situation instead.  */
  SVN_ERR(same_resource_in_head(&related, url, rev, ra_session, ctx, pool));
  if (related)
    {
      err = svn_ra_get_lock(ra_session, &lock, "", pool);

      /* An old mod_dav_svn will always work; there's nothing wrong with
         doing a PROPFIND for a property named "DAV:supportedlock". But
         an old svnserve will error. */
      if (err && err->apr_err == SVN_ERR_RA_NOT_IMPLEMENTED)
        {
          svn_error_clear(err);
          lock = NULL;
        }
      else if (err)
        return svn_error_return(err);
    }
  else
    lock = NULL;

  /* Push the URL's dirent (and lock) at the callback.*/
  SVN_ERR(build_info_from_dirent(&info, the_ent, lock, url, rev,
                                 repos_UUID, repos_root_URL, pool));
  SVN_ERR(receiver(receiver_baton, base_name, info, pool));

  /* Possibly recurse, using the original RA session. */
  if (depth > svn_depth_empty && (the_ent->kind == svn_node_dir))
    {
      apr_hash_t *locks;

pre_1_2_recurse:
      if (peg_revision->kind == svn_opt_revision_head)
        {
          err = svn_ra_get_locks(ra_session, &locks, "", pool);

          /* Catch specific errors thrown by old mod_dav_svn or svnserve. */
          if (err &&
              (err->apr_err == SVN_ERR_RA_NOT_IMPLEMENTED
               || err->apr_err == SVN_ERR_UNSUPPORTED_FEATURE))
            {
              svn_error_clear(err);
              locks = apr_hash_make(pool); /* use an empty hash */
            }
          else if (err)
            return svn_error_return(err);
        }
      else
        locks = apr_hash_make(pool); /* use an empty hash */

      SVN_ERR(push_dir_info(ra_session, url, "", rev,
                            repos_UUID, repos_root_URL,
                            receiver, receiver_baton,
                            depth, ctx, locks, pool));
    }

  return SVN_NO_ERROR;
}

svn_info_t *
svn_info_dup(const svn_info_t *info, apr_pool_t *pool)
{
  svn_info_t *dupinfo = apr_palloc(pool, sizeof(*dupinfo));

  /* Perform a trivial copy ... */
  *dupinfo = *info;

  /* ...and then re-copy stuff that needs to be duped into our pool. */
  if (info->URL)
    dupinfo->URL = apr_pstrdup(pool, info->URL);
  if (info->repos_root_URL)
    dupinfo->repos_root_URL = apr_pstrdup(pool, info->repos_root_URL);
  if (info->repos_UUID)
    dupinfo->repos_UUID = apr_pstrdup(pool, info->repos_UUID);
  if (info->last_changed_author)
    dupinfo->last_changed_author = apr_pstrdup(pool,
                                               info->last_changed_author);
  if (info->lock)
    dupinfo->lock = svn_lock_dup(info->lock, pool);
  if (info->copyfrom_url)
    dupinfo->copyfrom_url = apr_pstrdup(pool, info->copyfrom_url);
  if (info->checksum)
    dupinfo->checksum = apr_pstrdup(pool, info->checksum);
  if (info->conflict_old)
    dupinfo->conflict_old = apr_pstrdup(pool, info->conflict_old);
  if (info->conflict_new)
    dupinfo->conflict_new = apr_pstrdup(pool, info->conflict_new);
  if (info->conflict_wrk)
    dupinfo->conflict_wrk = apr_pstrdup(pool, info->conflict_wrk);
  if (info->prejfile)
    dupinfo->prejfile = apr_pstrdup(pool, info->prejfile);

  return dupinfo;
}
