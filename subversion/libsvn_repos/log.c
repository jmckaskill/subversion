/* log.c --- retrieving log messages
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


#define APR_WANT_STRFUNC
#include <apr_want.h>

#include "svn_private_config.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_fs.h"
#include "svn_repos.h"
#include "svn_string.h"
#include "svn_time.h"
#include "svn_sorts.h"
#include "repos.h"



/* Store as keys in CHANGED the paths of all node in ROOT that show a
 * significant change.  "Significant" means that the text or
 * properties of the node were changed, or that the node was added or
 * deleted.
 *
 * The CHANGED hash set and the key are allocated in POOL;
 * the value is (void *) 'U', 'A', 'D', or 'R', for modified, added,
 * deleted, or replaced, respectively.
 * 
 * If optional AUTHZ_READ_FUNC is non-NULL, then use it (with
 * AUTHZ_READ_BATON and FS) to check whether each changed-path (and
 * copyfrom_path) is readable:
 *
 *     - If some paths are readable and some are not, then silently
 *     omit the unreadable paths from the CHANGED hash, and return
 *     SVN_ERR_AUTHZ_PARTIALLY_READABLE.
 *
 *     - If absolutely every changed-path (and copyfrom_path) is
 *     unreadable, then return an empty CHANGED hash and
 *     SVN_ERR_AUTHZ_UNREADABLE.  (This is to distinguish a revision
 *     which truly has no changed paths from a revision in which all
 *     paths are unreadable.)
 */
static svn_error_t *
detect_changed (apr_hash_t **changed,
                svn_fs_root_t *root,
                svn_fs_t *fs,
                svn_repos_authz_func_t authz_read_func,
                void *authz_read_baton,
                apr_pool_t *pool)
{
  apr_hash_t *changes;
  apr_hash_index_t *hi;
  apr_pool_t *subpool = svn_pool_create (pool);
  svn_boolean_t found_readable = FALSE;
  svn_boolean_t found_unreadable = FALSE;

  *changed = apr_hash_make (pool);
  SVN_ERR (svn_fs_paths_changed (&changes, root, pool));

  if (apr_hash_count (changes) == 0)
    /* No paths changed in this revision?  Uh, sure, I guess the
       revision is readable, then.  */
    return SVN_NO_ERROR;

  for (hi = apr_hash_first (pool, changes); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;
      svn_fs_path_change_t *change;
      const char *path;
      char action;
      svn_log_changed_path_t *item;

      svn_pool_clear (subpool);

      /* KEY will be the path, VAL the change. */
      apr_hash_this (hi, &key, NULL, &val);
      path = (const char *) key;
      change = val;

      /* Skip path if unreadable. */
      if (authz_read_func)
        {
          svn_boolean_t readable;
          SVN_ERR (authz_read_func (&readable,
                                    root, path,
                                    authz_read_baton, subpool));
          if (! readable)
            {
              found_unreadable = TRUE;
              continue;
            }
        }

      /* At least one changed-path was readable. */
      found_readable = TRUE;

      switch (change->change_kind)
        {
        case svn_fs_path_change_reset:
          continue;

        case svn_fs_path_change_add:
          action = 'A';
          break;

        case svn_fs_path_change_replace:
          action = 'R';
          break;

        case svn_fs_path_change_delete:
          action = 'D';
          break;

        case svn_fs_path_change_modify:
        default:
          action = 'M';
          break;
        }

      item = apr_pcalloc (pool, sizeof (*item));
      item->action = action;
      item->copyfrom_rev = SVN_INVALID_REVNUM;
      if ((action == 'A') || (action == 'R'))
        {
          const char *copyfrom_path;
          svn_revnum_t copyfrom_rev;

          SVN_ERR (svn_fs_copied_from (&copyfrom_rev, &copyfrom_path,
                                       root, path, subpool));

          if (copyfrom_path && SVN_IS_VALID_REVNUM (copyfrom_rev))
            {
              svn_boolean_t readable = TRUE;

              if (authz_read_func)
                {
                  svn_fs_root_t *copyfrom_root;
                  
                  SVN_ERR (svn_fs_revision_root (&copyfrom_root, fs,
                                                 copyfrom_rev, subpool));
                  SVN_ERR (authz_read_func (&readable,
                                            copyfrom_root, copyfrom_path,
                                            authz_read_baton, subpool));
                  if (! readable)
                    found_unreadable = TRUE;
                }

              if (readable)
                {
                  item->copyfrom_path = apr_pstrdup (pool, copyfrom_path);
                  item->copyfrom_rev = copyfrom_rev;
                }
            }
        }
      apr_hash_set (*changed, apr_pstrdup (pool, path), 
                    APR_HASH_KEY_STRING, item);
    }

  svn_pool_destroy (subpool);

  if (! found_readable)
    /* Every changed-path was unreadable. */
    return svn_error_create (SVN_ERR_AUTHZ_UNREADABLE,
                             NULL, NULL);

  if (found_unreadable)
    /* At least one changed-path was unreadable. */
    return svn_error_create (SVN_ERR_AUTHZ_PARTIALLY_READABLE,
                             NULL, NULL);

  /* Every changed-path was readable. */
  return SVN_NO_ERROR;
}



/* Implements svn_repos_history_func_t interface.  Accumulate history
   revisions in the apr_array_header_t * which is the BATON. */
static svn_error_t *
history_to_revs_array (void *baton,
                       const char *path,
                       svn_revnum_t revision,
                       apr_pool_t *pool)
{
  apr_array_header_t *revs_array = baton;
  APR_ARRAY_PUSH (revs_array, svn_revnum_t) = revision;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_repos_get_logs2 (svn_repos_t *repos,
                     const apr_array_header_t *paths,
                     svn_revnum_t start,
                     svn_revnum_t end,
                     svn_boolean_t discover_changed_paths,
                     svn_boolean_t strict_node_history,
                     svn_repos_authz_func_t authz_read_func,
                     void *authz_read_baton,
                     svn_log_message_receiver_t receiver,
                     void *receiver_baton,
                     apr_pool_t *pool)
{
  svn_revnum_t this_rev, head = SVN_INVALID_REVNUM;
  apr_pool_t *subpool = svn_pool_create (pool);
  svn_fs_t *fs = repos->fs;
  apr_array_header_t *revs = NULL;

  SVN_ERR (svn_fs_youngest_rev (&head, fs, pool));

  if (! SVN_IS_VALID_REVNUM (start))
    start = head;

  if (! SVN_IS_VALID_REVNUM (end))
    end = head;

  /* Check that revisions are sane before ever invoking receiver. */
  if (start > head)
    return svn_error_createf
      (SVN_ERR_FS_NO_SUCH_REVISION, 0,
       _("No such revision %ld"), start);
  if (end > head)
    return svn_error_createf
      (SVN_ERR_FS_NO_SUCH_REVISION, 0,
       _("No such revision %ld"), end);

  /* If paths were specified, then we only really care about revisions
     in which those paths were changed.  So we ask the filesystem for
     all the revisions in which any of the paths was changed.

     SPECIAL CASE: If we were given only path, and that path is empty,
     then the results are the same as if we were passed no paths at
     all.  Why?  Because the answer to the question "In which
     revisions was the root of the filesystem changed?" is always
     "Every single one of them."  And since this section of code is
     only about answering that question, and we already know the
     answer ... well, you get the picture.
  */
  if (paths 
      && (((paths->nelts == 1) 
           && (! svn_path_is_empty (APR_ARRAY_IDX (paths, 0, const char *))))
          || (paths->nelts > 1)))
    {
      /* If there is only one path, we'll just get its sorted changed
         revisions.  Else, we'll be combining all our findings into a
         hash (to remove duplicates) and then generating a sorted
         array from that hash. */
      if (paths->nelts == 1)
        {
          /* Get the changed revisions for this path. */
          const char *this_path = APR_ARRAY_IDX (paths, 0, const char *);
          revs = apr_array_make (pool, 64, sizeof (svn_revnum_t));
          SVN_ERR (svn_repos_history2 (fs, this_path,
                                       history_to_revs_array, revs,
                                       authz_read_func, authz_read_baton,
                                       start, end, 
                                       strict_node_history ? FALSE : TRUE, 
                                       pool));
        }
      else
        {
          int i;
          apr_hash_t *all_revs = apr_hash_make (pool);
          apr_hash_index_t *hi;

          /* And the search is on... */
          for (i = 0; i < paths->nelts; i++)
            {
              const char *this_path = APR_ARRAY_IDX (paths, i, const char *);
              apr_array_header_t *changed_revs = 
                apr_array_make (pool, 64, sizeof (svn_revnum_t));
              int j;

              /* Get the changed revisions for this path, and add them to
                 the hash (this will eliminate duplicates). */
              SVN_ERR (svn_repos_history2 (fs, this_path,
                                           history_to_revs_array, changed_revs,
                                           authz_read_func, authz_read_baton,
                                           start, end, 
                                           strict_node_history ? FALSE : TRUE, 
                                           pool));
              for (j = 0; j < changed_revs->nelts; j++)
                {
                  /* We're re-using the memory allocated for the array
                     here in order to avoid more allocations.  */
                  svn_revnum_t *chrev = 
                    (((svn_revnum_t *)(changed_revs)->elts) + j);
                  apr_hash_set (all_revs, (void *)chrev, sizeof (chrev), 
                                (void *)1);
                }
            }

          /* Now that we have a hash of all the revisions in which any of
             our paths changed, we can convert that back into a sorted
             array. */
          revs = apr_array_make (pool, apr_hash_count (all_revs), 
                                 sizeof (svn_revnum_t));
          for (hi = apr_hash_first (pool, all_revs); 
               hi; 
               hi = apr_hash_next (hi))
            {
              const void *key;
              svn_revnum_t revision;
              
              apr_hash_this (hi, &key, NULL, NULL);
              revision = *((const svn_revnum_t *)key);
              (*((svn_revnum_t *) apr_array_push (revs))) = revision;
            }

          /* Now sort the array */
          qsort ((revs)->elts, (revs)->nelts, (revs)->elt_size, 
                 svn_sort_compare_revisions);
        }

      /* If no revisions were found for these entries, we have nothing
         to show. Just return now before we break a sweat.  */
      if (! (revs && revs->nelts))
        return SVN_NO_ERROR;
    }

  for (this_rev = start;
       ((start >= end) ? (this_rev >= end) : (this_rev <= end));
       ((start >= end) ? this_rev-- : this_rev++))
    {
      svn_string_t *author, *date, *message;
      apr_hash_t *changed_paths = NULL;

      /* If we have a list of revs for use, check to make sure this is
         one of them.  */
      if (revs)
        {
          int i, matched = 0;
          for (i = 0; ((i < revs->nelts) && (! matched)); i++)
            {
              if (this_rev == ((svn_revnum_t *)(revs->elts))[i])
                matched = 1;
            }

          if (! matched)
            continue;
        }

      SVN_ERR (svn_fs_revision_prop
               (&author, fs, this_rev, SVN_PROP_REVISION_AUTHOR, subpool));
      SVN_ERR (svn_fs_revision_prop
               (&date, fs, this_rev, SVN_PROP_REVISION_DATE, subpool));
      SVN_ERR (svn_fs_revision_prop
               (&message, fs, this_rev, SVN_PROP_REVISION_LOG, subpool));

      /* ### Below, we discover changed paths if the user requested
         them (i.e., "svn log -v" means `discover_changed_paths' will
         be non-zero here).  */


      if ((this_rev > 0)        
          && (authz_read_func || discover_changed_paths))
        {
          svn_fs_root_t *newroot;
          svn_error_t *patherr;

          SVN_ERR (svn_fs_revision_root (&newroot, fs, this_rev, subpool));
          patherr = detect_changed (&changed_paths,
                                    newroot, fs,
                                    authz_read_func, authz_read_baton,
                                    subpool);

          if (patherr
              && patherr->apr_err == SVN_ERR_AUTHZ_UNREADABLE)
            {
              /* All changed-paths are unreadable, so clear all fields. */
              svn_error_clear (patherr);              
              changed_paths = NULL;
              author = NULL;
              date = NULL;
              message = NULL;
            }
          else if (patherr
                   && patherr->apr_err == SVN_ERR_AUTHZ_PARTIALLY_READABLE)
            {
              /* At least one changed-path was unreadable, so omit the
                 log message.  (The unreadable paths are already
                 missing from the hash.) */
              svn_error_clear (patherr);
              message = NULL;
            }
          else if (patherr)
            return patherr;

          /* It may be the case that an authz func was passed in, but
             the user still doesn't want to see any changed-paths. */
          if (! discover_changed_paths)
            changed_paths = NULL;
        }

      SVN_ERR ((*receiver) (receiver_baton,
                            changed_paths,
                            this_rev,
                            author ? author->data : NULL,
                            date ? date->data : NULL,
                            message ? message->data : NULL,
                            subpool));
      
      svn_pool_clear (subpool);
    }

  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}


/* The 1.0 version of the function.  ### Remove in 2.0. */
svn_error_t *
svn_repos_get_logs (svn_repos_t *repos,
                    const apr_array_header_t *paths,
                    svn_revnum_t start,
                    svn_revnum_t end,
                    svn_boolean_t discover_changed_paths,
                    svn_boolean_t strict_node_history,
                    svn_log_message_receiver_t receiver,
                    void *receiver_baton,
                    apr_pool_t *pool)
{
  return svn_repos_get_logs2 (repos, paths, start, end,
                              discover_changed_paths, strict_node_history,
                              NULL, NULL, /* no authz stuff */
                              receiver, receiver_baton, pool);
}
