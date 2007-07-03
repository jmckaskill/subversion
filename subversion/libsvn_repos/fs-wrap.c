/* fs-wrap.c --- filesystem interface wrappers.
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
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

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "svn_pools.h"
#include "svn_error.h"
#include "svn_fs.h"
#include "svn_path.h"
#include "svn_props.h"
#include "svn_repos.h"
#include "repos.h"
#include "svn_private_config.h"


/*** Commit wrappers ***/

svn_error_t *
svn_repos_fs_commit_txn(const char **conflict_p,
                        svn_repos_t *repos,
                        svn_revnum_t *new_rev,
                        svn_fs_txn_t *txn,
                        apr_pool_t *pool)
{
  svn_error_t *err;
  const char *txn_name;

  /* Run pre-commit hooks. */
  SVN_ERR(svn_fs_txn_name(&txn_name, txn, pool));
  SVN_ERR(svn_repos__hooks_pre_commit(repos, txn_name, pool));

  /* Commit. */
  SVN_ERR(svn_fs_commit_txn(conflict_p, new_rev, txn, pool));

  /* Run post-commit hooks.   Notice that we're wrapping the error
     with a -specific- errorcode, so that our caller knows not to try
     and abort the transaction. */
  if ((err = svn_repos__hooks_post_commit(repos, *new_rev, pool)))
    return svn_error_create
      (SVN_ERR_REPOS_POST_COMMIT_HOOK_FAILED, err,
       _("Commit succeeded, but post-commit hook failed"));

  return SVN_NO_ERROR;
}



/*** Transaction creation wrappers. ***/

svn_error_t *
svn_repos__change_txn_props(svn_fs_txn_t *txn,
                            apr_hash_t *txnprop_table,
                            apr_pool_t *pool)
{
  apr_pool_t *iterpool;
  apr_hash_index_t *hi;
  const void *key;
  void *value;
  const char *propname;
  const svn_string_t *propval;

  iterpool = svn_pool_create(pool);
  for (hi = apr_hash_first(pool, txnprop_table); hi; hi = apr_hash_next(hi))
    {
      svn_pool_clear(iterpool);
      apr_hash_this(hi, &key, NULL, &value);
      propname = key;
      propval = value;
      SVN_ERR(svn_repos_fs_change_txn_prop(txn, propname, propval, iterpool));
    }
  svn_pool_destroy(iterpool);
    
  return SVN_NO_ERROR;
}


svn_error_t *
svn_repos_fs_begin_txn_for_commit2(svn_fs_txn_t **txn_p,
                                   svn_repos_t *repos,
                                   svn_revnum_t rev,
                                   apr_hash_t *revprop_table,
                                   apr_pool_t *pool)
{
  svn_string_t *author = apr_hash_get(revprop_table, SVN_PROP_REVISION_AUTHOR,
                                      APR_HASH_KEY_STRING);
  /* Run start-commit hooks. */
  SVN_ERR(svn_repos__hooks_start_commit(repos, author ? author->data : NULL,
                                        pool));

  /* Begin the transaction, ask for the fs to do on-the-fly lock checks. */
  SVN_ERR(svn_fs_begin_txn2(txn_p, repos->fs, rev,
                            SVN_FS_TXN_CHECK_LOCKS, pool));

  /* We pass the revision properties to the filesystem by adding them
     as properties on the txn.  Later, when we commit the txn, these
     properties will be copied into the newly created revision. */
  SVN_ERR(svn_repos__change_txn_props(*txn_p, revprop_table, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_repos_fs_begin_txn_for_commit(svn_fs_txn_t **txn_p,
                                  svn_repos_t *repos,
                                  svn_revnum_t rev,
                                  const char *author,
                                  const char *log_msg,
                                  apr_pool_t *pool)
{
  apr_hash_t *revprop_table = apr_hash_make(pool);
  if (author)
    apr_hash_set(revprop_table, SVN_PROP_REVISION_AUTHOR,
                 APR_HASH_KEY_STRING,
                 svn_string_create(author, pool));
  if (log_msg)
    apr_hash_set(revprop_table, SVN_PROP_REVISION_LOG,
                 APR_HASH_KEY_STRING,
                 svn_string_create(log_msg, pool));
  return svn_repos_fs_begin_txn_for_commit2(txn_p, repos, rev, revprop_table,
                                            pool);
}


svn_error_t *
svn_repos_fs_begin_txn_for_update(svn_fs_txn_t **txn_p,
                                  svn_repos_t *repos,
                                  svn_revnum_t rev,
                                  const char *author,
                                  apr_pool_t *pool)
{
  /* ### someday, we might run a read-hook here. */

  /* Begin the transaction. */
  SVN_ERR(svn_fs_begin_txn2(txn_p, repos->fs, rev, 0, pool));

  /* We pass the author to the filesystem by adding it as a property
     on the txn. */

  /* User (author). */
  if (author)
    {
      svn_string_t val;
      val.data = author;
      val.len = strlen(author);
      SVN_ERR(svn_fs_change_txn_prop(*txn_p, SVN_PROP_REVISION_AUTHOR,
                                     &val, pool));
    }

  return SVN_NO_ERROR;
}



/*** Property wrappers ***/

/* Validate that property NAME is valid for use in a Subversion
   repository. */
static svn_error_t *
validate_prop(const char *name,
              apr_pool_t *pool)
{
  svn_prop_kind_t kind = svn_property_kind(NULL, name);
  if (kind != svn_prop_regular_kind)
    return svn_error_createf 
      (SVN_ERR_REPOS_BAD_ARGS, NULL,
       _("Storage of non-regular property '%s' is disallowed through the "
         "repository interface, and could indicate a bug in your client"), 
       name);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_repos_fs_change_node_prop(svn_fs_root_t *root,
                              const char *path,
                              const char *name,
                              const svn_string_t *value,
                              apr_pool_t *pool)
{
  /* Validate the property, then call the wrapped function. */
  SVN_ERR(validate_prop(name, pool));
  return svn_fs_change_node_prop(root, path, name, value, pool);
}


svn_error_t *
svn_repos_fs_change_txn_prop(svn_fs_txn_t *txn,
                             const char *name,
                             const svn_string_t *value,
                             apr_pool_t *pool)
{
  /* Validate the property, then call the wrapped function. */
  SVN_ERR(validate_prop(name, pool));
  return svn_fs_change_txn_prop(txn, name, value, pool);
}


svn_error_t *
svn_repos_fs_change_rev_prop3(svn_repos_t *repos,
                              svn_revnum_t rev,
                              const char *author,
                              const char *name,
                              const svn_string_t *new_value,
                              svn_boolean_t use_pre_revprop_change_hook,
                              svn_boolean_t use_post_revprop_change_hook,
                              svn_repos_authz_func_t authz_read_func,
                              void *authz_read_baton,
                              apr_pool_t *pool)
{
  svn_string_t *old_value;
  svn_repos_revision_access_level_t readability;
  char action;

  SVN_ERR(svn_repos_check_revision_access(&readability, repos, rev,
                                          authz_read_func, authz_read_baton,
                                          pool));

  if (readability == svn_repos_revision_access_full)
    {
      SVN_ERR(validate_prop(name, pool));
      SVN_ERR(svn_fs_revision_prop(&old_value, repos->fs, rev, name, pool));
      if (! new_value)
        action = 'D';
      else if (! old_value)
        action = 'A';
      else
        action = 'M';

      if (use_pre_revprop_change_hook)
        SVN_ERR(svn_repos__hooks_pre_revprop_change(repos, rev, author, name, 
                                                    new_value, action, pool));

      SVN_ERR(svn_fs_change_rev_prop(repos->fs, rev, name, new_value, pool));

      if (use_post_revprop_change_hook)
        SVN_ERR(svn_repos__hooks_post_revprop_change(repos, rev, author,  name,
                                                     old_value, action, pool));
    }
  else  /* rev is either unreadable or only partially readable */
    {
      return svn_error_createf 
        (SVN_ERR_AUTHZ_UNREADABLE, NULL,
         _("Write denied:  not authorized to read all of revision %ld"), rev);
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_repos_fs_change_rev_prop2(svn_repos_t *repos,
                              svn_revnum_t rev,
                              const char *author,
                              const char *name,
                              const svn_string_t *new_value,
                              svn_repos_authz_func_t authz_read_func,
                              void *authz_read_baton,
                              apr_pool_t *pool)
{
  return svn_repos_fs_change_rev_prop3(repos, rev, author, name, new_value,
                                       TRUE, TRUE, authz_read_func,
                                       authz_read_baton, pool);
}



svn_error_t *
svn_repos_fs_change_rev_prop(svn_repos_t *repos,
                             svn_revnum_t rev,
                             const char *author,
                             const char *name,
                             const svn_string_t *new_value,
                             apr_pool_t *pool)
{
  return svn_repos_fs_change_rev_prop2(repos, rev, author, name, new_value,
                                       NULL, NULL, pool);  
}     



svn_error_t *
svn_repos_fs_revision_prop(svn_string_t **value_p,
                           svn_repos_t *repos,
                           svn_revnum_t rev,
                           const char *propname,
                           svn_repos_authz_func_t authz_read_func,
                           void *authz_read_baton,
                           apr_pool_t *pool)
{
  svn_repos_revision_access_level_t readability;

  SVN_ERR(svn_repos_check_revision_access(&readability, repos, rev,
                                          authz_read_func, authz_read_baton, 
                                          pool));    

  if (readability == svn_repos_revision_access_none)
    {
      /* Property?  What property? */
      *value_p = NULL;
    }
  else if (readability == svn_repos_revision_access_partial)
    {      
      /* Only svn:author and svn:date are fetchable. */
      if ((strncmp(propname, SVN_PROP_REVISION_AUTHOR,
                   strlen(SVN_PROP_REVISION_AUTHOR)) != 0)
          && (strncmp(propname, SVN_PROP_REVISION_DATE,
                      strlen(SVN_PROP_REVISION_DATE)) != 0))
        *value_p = NULL;

      else
        SVN_ERR(svn_fs_revision_prop(value_p, repos->fs,
                                     rev, propname, pool));
    }
  else /* wholly readable revision */
    {
      SVN_ERR(svn_fs_revision_prop(value_p, repos->fs, rev, propname, pool));
    }

  return SVN_NO_ERROR;
}



svn_error_t *
svn_repos_fs_revision_proplist(apr_hash_t **table_p,
                               svn_repos_t *repos,
                               svn_revnum_t rev,
                               svn_repos_authz_func_t authz_read_func,
                               void *authz_read_baton,
                               apr_pool_t *pool)
{
  svn_repos_revision_access_level_t readability;

  SVN_ERR(svn_repos_check_revision_access(&readability, repos, rev,
                                          authz_read_func, authz_read_baton, 
                                          pool));    

  if (readability == svn_repos_revision_access_none)
    {
      /* Return an empty hash. */
      *table_p = apr_hash_make(pool);
    }
  else if (readability == svn_repos_revision_access_partial)
    {      
      apr_hash_t *tmphash;
      svn_string_t *value;

      /* Produce two property hashtables, both in POOL. */
      SVN_ERR(svn_fs_revision_proplist(&tmphash, repos->fs, rev, pool));
      *table_p = apr_hash_make(pool);

      /* If they exist, we only copy svn:author and svn:date into the
         'real' hashtable being returned. */
      value = apr_hash_get(tmphash, SVN_PROP_REVISION_AUTHOR,
                           APR_HASH_KEY_STRING);
      if (value)
        apr_hash_set(*table_p, SVN_PROP_REVISION_AUTHOR,
                     APR_HASH_KEY_STRING, value);

      value = apr_hash_get(tmphash, SVN_PROP_REVISION_DATE,
                           APR_HASH_KEY_STRING);
      if (value)
        apr_hash_set(*table_p, SVN_PROP_REVISION_DATE,
                     APR_HASH_KEY_STRING, value);
    }
  else /* wholly readable revision */
    {
      SVN_ERR(svn_fs_revision_proplist(table_p, repos->fs, rev, pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_repos_fs_lock(svn_lock_t **lock,
                  svn_repos_t *repos,
                  const char *path,
                  const char *token,
                  const char *comment,
                  svn_boolean_t is_dav_comment,
                  apr_time_t expiration_date,
                  svn_revnum_t current_rev,
                  svn_boolean_t steal_lock,
                  apr_pool_t *pool)
{
  svn_error_t *err;
  svn_fs_access_t *access_ctx = NULL;
  const char *username = NULL;
  apr_array_header_t *paths;

  /* Setup an array of paths in anticipation of the ra layers handling
     multiple locks in one request (1.3 most likely).  This is only
     used by svn_repos__hooks_post_lock. */
  paths = apr_array_make(pool, 1, sizeof(const char *));
  APR_ARRAY_PUSH(paths, const char *) = path;

  SVN_ERR(svn_fs_get_access(&access_ctx, repos->fs));
  if (access_ctx)
    SVN_ERR(svn_fs_access_get_username(&username, access_ctx));

  if (! username)
    return svn_error_createf 
      (SVN_ERR_FS_NO_USER, NULL,
       "Cannot lock path '%s', no authenticated username available.", path);
  
  /* Run pre-lock hook.  This could throw error, preventing
     svn_fs_lock() from happening. */
  SVN_ERR(svn_repos__hooks_pre_lock(repos, path, username, pool));

  /* Lock. */
  SVN_ERR(svn_fs_lock(lock, repos->fs, path, token, comment, is_dav_comment,
                      expiration_date, current_rev, steal_lock, pool));

  /* Run post-lock hook. */
  if ((err = svn_repos__hooks_post_lock(repos, paths, username, pool)))
    return svn_error_create
      (SVN_ERR_REPOS_POST_LOCK_HOOK_FAILED, err,
       "Lock succeeded, but post-lock hook failed");

  return SVN_NO_ERROR;
}


svn_error_t *
svn_repos_fs_unlock(svn_repos_t *repos,
                    const char *path,
                    const char *token,
                    svn_boolean_t break_lock,
                    apr_pool_t *pool)
{
  svn_error_t *err;
  svn_fs_access_t *access_ctx = NULL;
  const char *username = NULL;
  /* Setup an array of paths in anticipation of the ra layers handling
     multiple locks in one request (1.3 most likely).  This is only
     used by svn_repos__hooks_post_lock. */
  apr_array_header_t *paths = apr_array_make(pool, 1, sizeof(const char *));
  APR_ARRAY_PUSH(paths, const char *) = path;

  SVN_ERR(svn_fs_get_access(&access_ctx, repos->fs));
  if (access_ctx)
    SVN_ERR(svn_fs_access_get_username(&username, access_ctx));

  if (! break_lock && ! username)
    return svn_error_createf 
      (SVN_ERR_FS_NO_USER, NULL,
       _("Cannot unlock path '%s', no authenticated username available"),
       path);

  /* Run pre-unlock hook.  This could throw error, preventing
     svn_fs_unlock() from happening. */
  SVN_ERR(svn_repos__hooks_pre_unlock(repos, path, username, pool));

  /* Unlock. */
  SVN_ERR(svn_fs_unlock(repos->fs, path, token, break_lock, pool));

  /* Run post-unlock hook. */
  if ((err = svn_repos__hooks_post_unlock(repos, paths, username, pool)))
    return svn_error_create
      (SVN_ERR_REPOS_POST_UNLOCK_HOOK_FAILED, err,
       _("Unlock succeeded, but post-unlock hook failed"));

  return SVN_NO_ERROR;
}


struct get_locks_baton_t
{
  svn_fs_t *fs;
  svn_fs_root_t *head_root;
  svn_repos_authz_func_t authz_read_func;
  void *authz_read_baton;
  apr_hash_t *locks;
};


/* This implements the svn_fs_get_locks_callback_t interface. */
static svn_error_t *
get_locks_callback(void *baton, 
                   svn_lock_t *lock, 
                   apr_pool_t *pool)
{
  struct get_locks_baton_t *b = baton;
  svn_boolean_t readable = TRUE;
  apr_pool_t *hash_pool = apr_hash_pool_get(b->locks);

  /* If there's auth to deal with, deal with it. */
  if (b->authz_read_func)
    {
      SVN_ERR(b->authz_read_func(&readable, b->head_root, lock->path,
                                 b->authz_read_baton, pool));
    }

  /* If we can read this lock path, add the lock to the return hash. */
  if (readable)
    apr_hash_set(b->locks, apr_pstrdup(hash_pool, lock->path), 
                 APR_HASH_KEY_STRING, svn_lock_dup(lock, hash_pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_repos_fs_get_locks(apr_hash_t **locks,
                       svn_repos_t *repos,
                       const char *path,
                       svn_repos_authz_func_t authz_read_func,
                       void *authz_read_baton,
                       apr_pool_t *pool)
{
  apr_hash_t *all_locks = apr_hash_make(pool);
  svn_revnum_t head_rev;
  struct get_locks_baton_t baton;

  /* Locks are always said to apply to HEAD revision, so we'll check
     to see if locked-paths are readable in HEAD as well. */
  SVN_ERR(svn_fs_youngest_rev(&head_rev, repos->fs, pool));

  /* Populate our callback baton. */
  baton.fs = repos->fs;
  baton.locks = all_locks;
  baton.authz_read_func = authz_read_func;
  baton.authz_read_baton = authz_read_baton;
  SVN_ERR(svn_fs_revision_root(&(baton.head_root), repos->fs, 
                               head_rev, pool));

  /* Get all the locks. */
  SVN_ERR(svn_fs_get_locks(repos->fs, path, get_locks_callback,
                           &baton, pool));

  *locks = baton.locks;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_repos_fs_get_mergeinfo(apr_hash_t **mergeinfo,
                           svn_repos_t *repos,
                           const apr_array_header_t *paths,
                           svn_revnum_t rev,
                           svn_mergeinfo_inheritance_t inherit,
                           svn_repos_authz_func_t authz_read_func,
                           void *authz_read_baton,
                           apr_pool_t *pool)
{
  apr_array_header_t *readable_paths = (apr_array_header_t *) paths;
  svn_fs_root_t *root;
  int i;

  if (!SVN_IS_VALID_REVNUM(rev))
    SVN_ERR(svn_fs_youngest_rev(&rev, repos->fs, pool));
  SVN_ERR(svn_fs_revision_root(&root, repos->fs, rev, pool));

  /* Filter out unreadable paths before divining merge tracking info. */
  if (authz_read_func)
    {
      apr_pool_t *subpool = svn_pool_create(pool);

      for (i = 0; i < paths->nelts; i++)
        {
          svn_boolean_t readable;
          const char *path = APR_ARRAY_IDX(paths, i, char *);
          svn_pool_clear(subpool);
          SVN_ERR(authz_read_func(&readable, root, path, authz_read_baton,
                                  subpool));
          if (readable && readable_paths != paths)
            APR_ARRAY_PUSH(readable_paths, const char *) = path;
          else if (!readable && readable_paths == paths)
            {
              /* Requested paths differ from readable paths.  Fork
                 list of readable paths from requested paths. */
              int j;
              readable_paths = apr_array_make(pool, paths->nelts - 1,
                                              sizeof(char *));
              for (j = 0; j < i; j++)
                {
                  path = APR_ARRAY_IDX(paths, j, char *);
                  APR_ARRAY_PUSH(readable_paths, const char *) = path;
                }
            }
        }

      apr_pool_destroy(subpool);
    }

  /* We consciously do not perform authz checks on the paths returned
     in *MERGEINFO, avoiding massive authz overhead which would allow
     us to protect the name of where a change was merged from, but not
     the change itself. */
  if (readable_paths->nelts > 0)
    SVN_ERR(svn_fs_get_mergeinfo(mergeinfo, root, readable_paths, inherit,
                                 pool));
  else
    *mergeinfo = NULL;

  return SVN_NO_ERROR;
}



/* 
 * vim:ts=4:sw=4:expandtab:tw=80:fo=tcroq 
 * vim:isk=a-z,A-Z,48-57,_,.,-,> 
 * vim:cino=>1s,e0,n0,f0,{.5s,}0,^-.5s,=.5s,t0,+1s,c3,(0,u0,\:0
 */
