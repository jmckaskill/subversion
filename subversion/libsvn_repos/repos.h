/* repos.h : interface to Subversion repository, private to libsvn_repos
 *
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
 */

#ifndef SVN_LIBSVN_REPOS_H
#define SVN_LIBSVN_REPOS_H

#include <apr_pools.h>
#include <apr_hash.h>

#include "svn_fs.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/*** Repository version number. */
#define SVN_REPOS__VERSION     2


/*** Repository layout. ***/

/* The top-level repository dir contains a README and various
   subdirectories.  */
#define SVN_REPOS__README        "README.txt"    /* Explanation. */
#define SVN_REPOS__SVNSERVE_CONF "svnserve.conf" /* svnserve configuration. */
#define SVN_REPOS__FORMAT        "format"        /* Stores the current version
                                                    of the repository. */
#define SVN_REPOS__DB_DIR        "db"            /* Where Berkeley lives. */
#define SVN_REPOS__DAV_DIR       "dav"           /* DAV sandbox. */
#define SVN_REPOS__LOCK_DIR      "locks"         /* Lock files live here. */
#define SVN_REPOS__HOOK_DIR      "hooks"         /* Hook programs. */

/* Things for which we keep lockfiles. */
#define SVN_REPOS__DB_LOCKFILE "db.lock" /* Our Berkeley lockfile. */
#define SVN_REPOS__DB_LOGS_LOCKFILE "db-logs.lock" /* BDB logs lockfile. */

/* In the repository hooks directory, look for these files. */
#define SVN_REPOS__HOOK_START_COMMIT    "start-commit"
#define SVN_REPOS__HOOK_PRE_COMMIT      "pre-commit"
#define SVN_REPOS__HOOK_POST_COMMIT     "post-commit"
#define SVN_REPOS__HOOK_READ_SENTINEL   "read-sentinels"
#define SVN_REPOS__HOOK_WRITE_SENTINEL  "write-sentinels"
#define SVN_REPOS__HOOK_PRE_REVPROP_CHANGE  "pre-revprop-change"
#define SVN_REPOS__HOOK_POST_REVPROP_CHANGE "post-revprop-change"


/* The extension added to the names of example hook scripts. */
#define SVN_REPOS__HOOK_DESC_EXT        ".tmpl"


/* The Repository object, created by svn_repos_open() and
   svn_repos_create(), allocated in POOL. */
struct svn_repos_t
{
  /* A Subversion filesystem object. */
  svn_fs_t *fs;

  /* The path to the repository's top-level directory. */
  char *path;

  /* The path to the repository's dav directory. */
  char *dav_path;

  /* The path to the repository's hooks directory. */
  char *hook_path;

  /* The path to the repository's locks directory. */
  char *lock_path;

  /* The path to the Berkeley DB filesystem environment. */
  char *db_path;
};


/*** Hook-running Functions ***/

/* Run the start-commit hook for REPOS.  Use POOL for any temporary
   allocations.  If the hook fails, return SVN_ERR_REPOS_HOOK_FAILURE.

   USER is the authenticated name of the user starting the commit.  */
svn_error_t *
svn_repos__hooks_start_commit (svn_repos_t *repos,
                               const char *user,
                               apr_pool_t *pool);

/* Run the pre-commit hook for REPOS.  Use POOL for any temporary
   allocations.  If the hook fails, return SVN_ERR_REPOS_HOOK_FAILURE.  

   TXN_NAME is the name of the transaction that is being committed.  */
svn_error_t *
svn_repos__hooks_pre_commit (svn_repos_t *repos,
                             const char *txn_name,
                             apr_pool_t *pool);

/* Run the post-commit hook for REPOS.  Use POOL for any temporary
   allocations.  If the hook fails, run SVN_ERR_REPOS_HOOK_FAILURE.

   REV is the revision that was created as a result of the commit.  */
svn_error_t *
svn_repos__hooks_post_commit (svn_repos_t *repos,
                              svn_revnum_t rev,
                              apr_pool_t *pool);

/* Run the pre-revprop-change hook for REPOS.  Use POOL for any
   temporary allocations.  If the hook fails, return
   SVN_ERR_REPOS_HOOK_FAILURE.  

   REV is the revision whose property is being changed.
   AUTHOR is the authenticated name of the user changing the prop.
   NAME is the name of the property being changed.  
   VALUE is the current (unchanged) value of the property.  */
svn_error_t *
svn_repos__hooks_pre_revprop_change (svn_repos_t *repos,
                                     svn_revnum_t rev,
                                     const char *author,
                                     const char *name,
                                     const svn_string_t *value,
                                     apr_pool_t *pool);

/* Run the pre-revprop-change hook for REPOS.  Use POOL for any
   temporary allocations.  If the hook fails, return
   SVN_ERR_REPOS_HOOK_FAILURE. 

   REV is the revision whose property was changed.
   AUTHOR is the authenticated name of the user who changed the prop.
   NAME is the name of the property that was changed, and OLD_VALUE is
   that property's value immediately before the change, or null if
   none.

   ### The old value will be passed to the post-revprop hook either
   ### on stdin, or in a tempfile.  We're still figuring out which
   ### way to do it, stay tuned... */
svn_error_t *
svn_repos__hooks_post_revprop_change (svn_repos_t *repos,
                                      svn_revnum_t rev,
                                      const char *author,
                                      const char *name,
                                      svn_string_t *old_value,
                                      apr_pool_t *pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_REPOS_H */
