/*
 * list.c:  list local and remote directory entries.
 *
 * ====================================================================
 * Copyright (c) 2000-2006 CollabNet.  All rights reserved.
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

/* ==================================================================== */



#include "client.h"
#include "svn_client.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_time.h"
#include "svn_sorts.h"

#include "svn_private_config.h"

/* Get the directory entries of DIR at REV (relative to the root of
   RA_SESSION), getting at least the fields specified by DIRENT_FIELDS.
   Use the cancellation function/baton of CTX to check for cancellation.
   If RECURSE is TRUE, recurse into child directories.
   LOCKS, if non-NULL, is a hash mapping const char * paths to svn_lock_t
   objects and FS_PATH is the absolute filesystem path of the RA session.
   Use POOL for temporary allocations.
*/
static svn_error_t *
get_dir_contents(apr_uint32_t dirent_fields,
                 const char *dir,
                 svn_revnum_t rev,
                 svn_ra_session_t *ra_session,
                 apr_hash_t *locks,
                 const char *fs_path,
                 svn_boolean_t recurse,
                 svn_client_ctx_t *ctx,
                 svn_client_list_func_t list_func,
                 void *baton,
                 apr_pool_t *pool)
{
  apr_hash_t *tmpdirents;
  apr_pool_t *subpool = svn_pool_create(pool);
  apr_array_header_t *array;
  int i;

  /* Get the directory's entries, but not its props. */
  SVN_ERR(svn_ra_get_dir2(ra_session, &tmpdirents, NULL, NULL,
                          dir, rev, dirent_fields, pool));

  if (ctx->cancel_func)
    SVN_ERR(ctx->cancel_func(ctx->cancel_baton));

  /* Sort the hash, so we can call the callback in a "deterministic" order. */
  array = svn_sort__hash(tmpdirents, svn_sort_compare_items_lexically, pool);
  for (i = 0; i < array->nelts; ++i)
    {
      svn_sort__item_t *item = &APR_ARRAY_IDX(array, i, svn_sort__item_t);
      const char *path;
      svn_dirent_t *the_ent = apr_hash_get(tmpdirents, item->key, item->klen);
      svn_lock_t *lock;

      svn_pool_clear(subpool);

      path = svn_path_join(dir, item->key, subpool);

      if (locks)
        {
          const char *abs_path = svn_path_join(fs_path, path, subpool);
          lock = apr_hash_get(locks, abs_path, APR_HASH_KEY_STRING);
        }
      else
        lock = NULL;

      SVN_ERR(list_func(baton, path, the_ent, lock, fs_path, subpool));

      if (recurse && the_ent->kind == svn_node_dir)
        SVN_ERR(get_dir_contents(dirent_fields, path, rev,
                                 ra_session, locks, fs_path, recurse, ctx,
                                 list_func, baton, subpool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_list(const char *path_or_url,
                const svn_opt_revision_t *peg_revision,
                const svn_opt_revision_t *revision,
                svn_boolean_t recurse,
                apr_uint32_t dirent_fields,
                svn_boolean_t fetch_locks,
                svn_client_list_func_t list_func,
                void *baton,
                svn_client_ctx_t *ctx,
                apr_pool_t *pool)
{
  svn_ra_session_t *ra_session;
  svn_revnum_t rev;
  svn_dirent_t *dirent;
  const char *url;
  const char *repos_root;
  const char *fs_path;
  svn_error_t *err;
  apr_hash_t *locks;

  /* We use the kind field to determine if we should recurse, so we
     always need it. */
  dirent_fields |= SVN_DIRENT_KIND;

  /* Get an RA plugin for this filesystem object. */
  SVN_ERR(svn_client__ra_session_from_path(&ra_session, &rev,
                                           &url, path_or_url, peg_revision,
                                           revision, ctx, pool));

  SVN_ERR(svn_ra_get_repos_root(ra_session, &repos_root, pool));

  /* Get path relative to repository root. */
  fs_path = svn_path_is_child(repos_root, url, pool);
  /* Make sure fs_path begins with a slash.  fs_path is NULL if the url is
     the repository root. */
  fs_path = svn_path_join("/", fs_path ? fs_path : "", pool);
  fs_path = svn_path_uri_decode(fs_path, pool);

  err = svn_ra_stat(ra_session, "", rev, &dirent, pool);

  /* svnserve before 1.2 doesn't support the above, so fall back on
     a less efficient method. */
  if (err && err->apr_err == SVN_ERR_RA_NOT_IMPLEMENTED)
    {
      svn_node_kind_t kind;

      svn_error_clear(err);

      SVN_ERR(svn_ra_check_path(ra_session, "", rev, &kind, pool));

      if (kind != svn_node_none)
        {
          if (strcmp(url, repos_root) != 0)
            {
              svn_ra_session_t *parent_session;
              apr_hash_t *parent_ents;
              const char *parent_url, *base_name;

              /* Open another session to the path's parent.  This server
                 doesn't support svn_ra_reparent anyway, so don't try it. */
              svn_path_split(url, &parent_url, &base_name, pool);

              /* 'base_name' is now the last component of an URL, but we want
                 to use it as a plain file name. Therefore, we must URI-decode
                 it. */
              base_name = svn_path_uri_decode(base_name, pool);

              SVN_ERR(svn_client__open_ra_session_internal(&parent_session,
                                                           parent_url, NULL,
                                                           NULL, NULL, FALSE,
                                                           TRUE, ctx, pool));

              /* Get all parent's entries, no props. */
              SVN_ERR(svn_ra_get_dir2(parent_session, &parent_ents, NULL,
                                      NULL, "", rev, dirent_fields, pool));

              /* Get the relevant entry. */
              dirent = apr_hash_get(parent_ents, base_name,
                                    APR_HASH_KEY_STRING);
            }
          else
            {
              /* We can't get the directory entry for the repository root,
                 but we can still get the information we want.
                 The created-rev of the repository root must, by definition,
                 be rev. */
              dirent = apr_palloc(pool, sizeof(*dirent));
              dirent->kind = kind;
              dirent->size = 0;
              if (dirent_fields & SVN_DIRENT_HAS_PROPS)
                {
                  apr_hash_t *props;
                  SVN_ERR(svn_ra_get_dir2(ra_session, NULL, NULL, &props,
                                          "", rev, 0 /* no dirent fields */,
                                          pool));
                  dirent->has_props = (apr_hash_count(props) != 0);
                }
              dirent->created_rev = rev;
              if (dirent_fields & (SVN_DIRENT_TIME | SVN_DIRENT_LAST_AUTHOR))
                {
                  apr_hash_t *props;
                  svn_string_t *val;

                  SVN_ERR(svn_ra_rev_proplist(ra_session, rev, &props,
                                              pool));
                  val = apr_hash_get(props, SVN_PROP_REVISION_DATE,
                                     APR_HASH_KEY_STRING);
                  if (val)
                    SVN_ERR(svn_time_from_cstring(&dirent->time, val->data,
                                                  pool));
                  else
                    dirent->time = 0;

                  val = apr_hash_get(props, SVN_PROP_REVISION_AUTHOR,
                                     APR_HASH_KEY_STRING);
                  dirent->last_author = val ? val->data : NULL;
                }
            }
        }
      else
        dirent = NULL;
    }
  else if (err)
    return err;

  if (! dirent)
    return svn_error_createf(SVN_ERR_FS_NOT_FOUND, NULL,
                             _("URL '%s' non-existent in that revision"),
                             url);

  /* Maybe get all locks under url. */
  if (fetch_locks)
    {
      /* IMPORTANT: If locks are stored in a more temporary pool, we need
         to fix store_dirent below to duplicate the locks. */
      err = svn_ra_get_locks(ra_session, &locks, "", pool);

      if (err && err->apr_err == SVN_ERR_RA_NOT_IMPLEMENTED)
        {
          svn_error_clear(err);
          locks = NULL;
        }
      else if (err)
        return err;
    }
  else
    locks = NULL;

  /* Report the dirent for the target. */
  SVN_ERR(list_func(baton, "", dirent, locks
                    ? (apr_hash_get(locks, fs_path,
                                    APR_HASH_KEY_STRING))
                    : NULL, fs_path, pool));

  if (dirent->kind == svn_node_dir)
    SVN_ERR(get_dir_contents(dirent_fields, "", rev, ra_session, locks,
                             fs_path, recurse, ctx, list_func, baton, pool));

  return SVN_NO_ERROR;
}

/* Baton used by compatibility wrapper svn_client_ls3. */
struct ls_baton {
  apr_hash_t *dirents;
  apr_hash_t *locks;
  apr_pool_t *pool;
};

/* This implements svn_client_list_func_t. */
static svn_error_t *
store_dirent(void *baton, const char *path, const svn_dirent_t *dirent,
             const svn_lock_t *lock, const char *abs_path, apr_pool_t *pool)
{
  struct ls_baton *lb = baton;

  /* The dirent is allocated in a temporary pool, so duplicate it into the
     correct pool.  Note, however, that the locks are stored in the correct
     pool already. */
  dirent = svn_dirent_dup(dirent, lb->pool);

  /* An empty path means we are called for the target of the operation.
     For compatibility, we only store the target if it is a file, and we
     store it under the basename of the URL.  Note that this makes it
     impossible to differentiate between the target being a directory with a
     child with the same basename as the target and the target being a file,
     but that's how it was implemented. */
  if (path[0] == '\0')
    {
      if (dirent->kind == svn_node_file)
        {
          const char *base_name = svn_path_basename(abs_path, lb->pool);
          apr_hash_set(lb->dirents, base_name, APR_HASH_KEY_STRING, dirent);
          if (lock)
            apr_hash_set(lb->locks, base_name, APR_HASH_KEY_STRING, lock);
        }
    }
  else
    {
      path = apr_pstrdup(lb->pool, path);
      apr_hash_set(lb->dirents, path, APR_HASH_KEY_STRING, dirent);
      if (lock)
        apr_hash_set(lb->locks, path, APR_HASH_KEY_STRING, lock);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_ls3(apr_hash_t **dirents,
               apr_hash_t **locks,
               const char *path_or_url,
               const svn_opt_revision_t *peg_revision,
               const svn_opt_revision_t *revision,
               svn_boolean_t recurse,
               svn_client_ctx_t *ctx,
               apr_pool_t *pool)
{
  struct ls_baton lb;

  *dirents = lb.dirents = apr_hash_make(pool);
  if (locks)
    *locks = lb.locks = apr_hash_make(pool);
  lb.pool = pool;

  return svn_client_list(path_or_url, peg_revision, revision, recurse,
                         SVN_DIRENT_ALL, locks != NULL,
                         store_dirent, &lb, ctx, pool);
}

svn_error_t *
svn_client_ls2(apr_hash_t **dirents,
               const char *path_or_url,
               const svn_opt_revision_t *peg_revision,
               const svn_opt_revision_t *revision,
               svn_boolean_t recurse,
               svn_client_ctx_t *ctx,
               apr_pool_t *pool)
{

  return svn_client_ls3(dirents, NULL, path_or_url, peg_revision,
                        revision, recurse, ctx, pool);
}


svn_error_t *
svn_client_ls(apr_hash_t **dirents,
              const char *path_or_url,
              svn_opt_revision_t *revision,
              svn_boolean_t recurse,               
              svn_client_ctx_t *ctx,
              apr_pool_t *pool)
{
  return svn_client_ls2(dirents, path_or_url, revision,
                        revision, recurse, ctx, pool);
}
