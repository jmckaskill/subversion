/*
 * wc.h :  shared stuff internal to the svn_wc library.
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


#ifndef SVN_LIBSVN_WC_H
#define SVN_LIBSVN_WC_H

#include <apr_pools.h>
#include <apr_hash.h>

#include "svn_types.h"
#include "svn_error.h"
#include "svn_wc.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


#define SVN_WC__DIFF_EXT      ".diff"
#define SVN_WC__TMP_EXT       ".tmp"
#define SVN_WC__TEXT_REJ_EXT  ".rej"
#define SVN_WC__PROP_REJ_EXT  ".prej"
#define SVN_WC__BASE_EXT      ".svn-base" /* for text and prop bases */
#define SVN_WC__WORK_EXT      ".svn-work" /* for working propfiles */




/* We can handle this format or anything lower, and we (should) error
 * on anything higher.
 *
 * There is no format version 0; we started with 1.
 *
 * The change from 1 to 2 was the introduction of SVN_WC__WORK_EXT.
 * For example, ".svn/props/foo" became ".svn/props/foo.svn-work".
 *
 * The change from 2 to 3 was the introduction of the entry attribute
 * SVN_WC__ENTRY_ATTR_ABSENT.
 *
 * The change from 3 to 4 was the renaming of the magic "svn:this_dir"
 * entry name to "".
 *
 * Please document any further format changes here.
 */
#define SVN_WC__VERSION       4

/* A version <= to this (but > 0, of course) uses the old-style
   property file names, without the .svn-work extension. */
#define SVN_WC__OLD_PROPNAMES_VERSION 1


/*** Update traversals. ***/

struct svn_wc_traversal_info_t
{
  /* The pool in which this structure and everything inside it is
     allocated. */
  apr_pool_t *pool;

  /* The before and after values of the SVN_PROP_EXTERNALS property,
   * for each directory on which that property changed.  These have
   * the same layout as those returned by svn_wc_edited_externals(). 
   *
   * The hashes, their keys, and their values are allocated in the
   * above pool.
   */
  apr_hash_t *externals_old;
  apr_hash_t *externals_new;
};



/*** Timestamps. ***/

/* A special timestamp value which means "use the timestamp from the
   working copy".  This is sometimes used in a log entry like:
   
   <modify-entry name="foo.c" revision="5" timestamp="working"/>
 */
#define SVN_WC_TIMESTAMP_WC   "working"



/*** Names and file/dir operations in the administrative area. ***/

/* kff todo: namespace-protecting these #defines so we never have to
   worry about them conflicting with future all-caps symbols that may
   be defined in svn_wc.h. */

/** The files within the administrative subdir. **/
#define SVN_WC__ADM_FORMAT              "format"
#define SVN_WC__ADM_README              "README.txt"
#define SVN_WC__ADM_ENTRIES             "entries"
#define SVN_WC__ADM_LOCK                "lock"
#define SVN_WC__ADM_TMP                 "tmp"
#define SVN_WC__ADM_TEXT_BASE           "text-base"
#define SVN_WC__ADM_PROPS               "props"
#define SVN_WC__ADM_PROP_BASE           "prop-base"
#define SVN_WC__ADM_DIR_PROPS           "dir-props"
#define SVN_WC__ADM_DIR_PROP_BASE       "dir-prop-base"
#define SVN_WC__ADM_WCPROPS             "wcprops"
#define SVN_WC__ADM_DIR_WCPROPS         "dir-wcprops"
#define SVN_WC__ADM_LOG                 "log"
#define SVN_WC__ADM_KILLME              "KILLME"
#define SVN_WC__ADM_AUTH_DIR            "auth"
#define SVN_WC__ADM_EMPTY_FILE          "empty-file"


/* The basename of the ".prej" file, if a directory ever has property
   conflicts.  This .prej file will appear *within* the conflicted
   directory.  */
#define SVN_WC__THIS_DIR_PREJ           "dir_conflicts"



/*** General utilities that may get moved upstairs at some point. */

/* Ensure that DIR exists. */
svn_error_t *svn_wc__ensure_directory (const char *path, apr_pool_t *pool);

/* Take out a write-lock, stealing an existing lock if one exists.  This
   function avoids the potential race between checking for an existing lock
   and creating a lock. The cleanup code uses this function, but stealing
   locks is not a good idea because the code cannot determine whether a
   lock is still in use. Try not to write any more code that requires this
   feature. 

   PATH is the directory to lock, and the lock is returned in
   *ADM_ACCESS.  ASSOCIATED can be another lock in which case the locks
   will be in the same set, or it can be NULL.
*/
svn_error_t *svn_wc__adm_steal_write_lock (svn_wc_adm_access_t **adm_access,
                                           svn_wc_adm_access_t *associated,
                                           const char *path, apr_pool_t *pool);


/* Set *CLEANUP to TRUE if the directory ADM_ACCESS requires cleanup
   processing, set *CLEANUP to FALSE otherwise. */
svn_error_t *svn_wc__adm_is_cleanup_required (svn_boolean_t *cleanup,
                                              svn_wc_adm_access_t *adm_access,
                                              apr_pool_t *pool);

/* Store ENTRIES in the cache in ADM_ACCESS appropriate for SHOW_DELETED.
   ENTRIES may be NULL. */
void svn_wc__adm_access_set_entries (svn_wc_adm_access_t *adm_access,
                                     svn_boolean_t show_deleted,
                                     apr_hash_t *entries);

/* Return the entries hash appropriate for SHOW_DELETED cached in
   ADM_ACCESS.  The returned hash may be NULL.  POOL is used for local,
   short term, memory allocations. */
apr_hash_t *svn_wc__adm_access_entries (svn_wc_adm_access_t *adm_access,
                                        svn_boolean_t show_deleted,
                                        apr_pool_t *pool);

/* Return an access baton for PATH in *ADM_ACCESS.  This function is used
   to lock the working copy during construction of the admin area, it
   necessarily does less checking than svn_wc_adm_open. */
svn_error_t *svn_wc__adm_pre_open (svn_wc_adm_access_t **adm_access,
                                   const char *path,
                                   apr_pool_t *pool);

/* Returns TRUE if PATH is a working copy directory that is obstructed or
   missing such that an access baton is not available for PATH.  This means
   that ADM_ACCESS is an access baton set that contains an access baton for
   the parent of PATH and when that access baton was opened it must have
   attempted to open PATH, i.e. it must have been opened with the TREE_LOCK
   parameter set TRUE. */
svn_boolean_t svn_wc__adm_missing (svn_wc_adm_access_t *adm_access,
                                   const char *path);

/* Sets *ADM_ACCESS to an access baton for PATH from the set ASSOCIATED.
   This function is similar to svn_wc_adm_retrieve except that if the baton
   for PATH is not found, this function sets *ADM_ACCESS to NULL and does
   not return an error. */
svn_error_t * svn_wc__adm_retrieve_internal (svn_wc_adm_access_t **adm_access,
                                             svn_wc_adm_access_t *associated,
                                             const char *path,
                                             apr_pool_t *pool);

/* Return the working copy format version number for ADM_ACCESS. */
int svn_wc__adm_wc_format (svn_wc_adm_access_t *adm_access);

/* Ensure ADM_ACCESS has a write lock and that it is still valid.  Returns
 * the error SVN_ERR_WC_NOT_LOCKED if this is not the case.  Compared to
 * the function svn_wc_adm_locked, this function is run-time expensive as
 * it does additional checking to verify the physical lock.  It is used
 * when the library expects a write lock, and where it is an error for the
 * lock not to be present.  Applications are not expected to call it.
 */
svn_error_t *svn_wc__adm_write_check (svn_wc_adm_access_t *adm_access);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_WC_H */
