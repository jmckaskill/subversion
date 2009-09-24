/*
 * lock.h:  routines for locking working copy subdirectories.
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


#ifndef SVN_LIBSVN_WC_LOCK_H
#define SVN_LIBSVN_WC_LOCK_H

#include <apr_pools.h>
#include <apr_hash.h>

#include "svn_types.h"
#include "svn_error.h"
#include "svn_wc.h"

#include "wc_db.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/*** General utilities that may get moved upstairs at some point. */

/* Take out a write-lock, stealing an existing lock if one exists.  This
   function avoids the potential race between checking for an existing lock
   and creating a lock. The cleanup code uses this function, but stealing
   locks is not a good idea because the code cannot determine whether a
   lock is still in use. Try not to write any more code that requires this
   feature.

   PATH is the directory to lock, and the lock is returned in
   *ADM_ACCESS.
*/
svn_error_t *
svn_wc__adm_steal_write_lock(svn_wc_adm_access_t **adm_access,
                             svn_wc__db_t *db,
                             const char *path,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool);


/* Store ENTRIES in the cache in ADM_ACCESS.  ENTRIES may be NULL. */
void svn_wc__adm_access_set_entries(svn_wc_adm_access_t *adm_access,
                                    apr_hash_t *entries);

/* Return the entries hash cached in ADM_ACCESS.  The returned hash may
   be NULL.  */
apr_hash_t *svn_wc__adm_access_entries(svn_wc_adm_access_t *adm_access);


/* Returns TRUE if LOCAL_ABSPATH is a working copy directory that is obstructed
   or missing such that an access baton is not available for LOCAL_ABSPATH.
   This means DB must also include the parent of LOCAL_ABSPATH. 
   
   This function falls back to using svn_wc__adm_available() if no access batons
   for LOCAL_ABSPATH are stored in DB. */
svn_boolean_t svn_wc__adm_missing(svn_wc__db_t *db,
                                  const char *local_abspath,
                                  apr_pool_t *scratch_pool);

/* Retrieves the KIND of LOCAL_ABSPATH and whether its administrative data is
   available in the working copy.

   *AVAILABLE is set to TRUE when the node and its metadata are available,
   otherwise to FALSE (due to obstruction, missing, absence, exclusion,
   or a "not-present" child).

   *OBSTRUCTED is set to TRUE when the node is not available because
   it is obstructed/missing, otherwise to FALSE.

   KIND and OBSTRUCTED can be NULL.

   ### note: this function should go away when we move to a single
   ### adminstrative area.  */
svn_error_t *
svn_wc__adm_available(svn_boolean_t *available,
                      svn_wc__db_kind_t *kind,
                      svn_boolean_t *obstructed,
                      svn_wc__db_t *db,
                      const char *local_abspath,
                      apr_pool_t *scratch_pool);

/* Sets *ADM_ACCESS to an access baton for PATH from the set ASSOCIATED.
   This function is similar to svn_wc_adm_retrieve except that if the baton
   for PATH is not found, this function sets *ADM_ACCESS to NULL and does
   not return an error. */
svn_error_t *svn_wc__adm_retrieve_internal(svn_wc_adm_access_t **adm_access,
                                           svn_wc_adm_access_t *associated,
                                           const char *path,
                                           apr_pool_t *pool);

/* Same as svn_wc__adm_retrieve_internal, but takes a DB and an absolute
   directory path.  */
svn_wc_adm_access_t *
svn_wc__adm_retrieve_internal2(svn_wc__db_t *db,
                               const char *abspath,
                               apr_pool_t *scratch_pool);

/* ### this is probably bunk. but I dunna want to trace backwards-compat
   ### users of svn_wc_check_wc(). probably gonna be rewritten for wc-ng
   ### in any case.  */
svn_error_t *
svn_wc__internal_check_wc(int *wc_format,
                          svn_wc__db_t *db,
                          const char *local_abspath,
                          apr_pool_t *scratch_pool);


/* Ensure ADM_ACCESS has a write lock and that it is still valid.  Returns
 * the error SVN_ERR_WC_NOT_LOCKED if this is not the case.  Compared to
 * the function svn_wc_adm_locked, this function is run-time expensive as
 * it does additional checking to verify the physical lock.  It is used
 * when the library expects a write lock, and where it is an error for the
 * lock not to be present.  Applications are not expected to call it.
 */
svn_error_t *svn_wc__adm_write_check(const svn_wc_adm_access_t *adm_access,
                                     apr_pool_t *scratch_pool);

/* Ensure ADM_ACCESS has a lock and for an entire WC tree (all the way
   to its leaf nodes).  While locking a tree up front using
   LEVELS_TO_LOCK of -1 is a more appropriate operation, this function
   can be used to extend the depth of a lock via a tree-crawl after a
   lock is taken out.  Use POOL for temporary allocations. */
svn_error_t *svn_wc__adm_extend_lock_to_tree(svn_wc_adm_access_t *adm_access,
                                             apr_pool_t *pool);


/* Return the working copy database associated with this access baton. */
svn_wc__db_t *
svn_wc__adm_get_db(const svn_wc_adm_access_t *adm_access);


/* Get a reference to the baton's internal ABSPATH.  */
const char *
svn_wc__adm_access_abspath(const svn_wc_adm_access_t *adm_access);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_WC_LOCK_H */
