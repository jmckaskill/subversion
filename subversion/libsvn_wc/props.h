/*
 * props.h :  properties
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


#ifndef SVN_LIBSVN_WC_PROPS_H
#define SVN_LIBSVN_WC_PROPS_H

#include <apr_pools.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"


#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* If the working item at PATH has properties attached, set HAS_PROPS.
   ADM_ACCESS is an access baton set that contains PATH. */
svn_error_t *svn_wc__has_props (svn_boolean_t *has_props,
                                const char *path,
                                svn_wc_adm_access_t *adm_access,
                                apr_pool_t *pool);



/* Given two propchange objects, return TRUE iff they conflict.  If
   there's a conflict, DESCRIPTION will contain an english description
   of the problem. */

/* For note, here's the table being implemented:

              |  update set     |    update delete   |
  ------------|-----------------|--------------------|
  user set    | conflict iff    |      conflict      |
              |  vals differ    |                    |
  ------------|-----------------|--------------------|
  user delete |   conflict      |      merge         |
              |                 |    (no problem)    |
  ----------------------------------------------------

*/
svn_boolean_t
svn_wc__conflicting_propchanges_p (const svn_string_t **description,
                                   const svn_prop_t *local,
                                   const svn_prop_t *update,
                                   apr_pool_t *pool);

/* Look up the entry NAME within ADM_ACCESS and see if it has a `current'
   reject file describing a state of conflict.  If such a file exists,
   return the name of the file in REJECT_FILE.  If no such file exists,
   return (REJECT_FILE = NULL). */
svn_error_t *
svn_wc__get_existing_prop_reject_file (const char **reject_file,
                                       svn_wc_adm_access_t *adm_access,
                                       const char *name,
                                       apr_pool_t *pool);

/* If PROPFILE_PATH exists (and is a file), assume it's full of
   properties and load this file into HASH.  Otherwise, leave HASH
   untouched.  */
svn_error_t *svn_wc__load_prop_file (const char *propfile_path,
                                     apr_hash_t *hash,
                                     apr_pool_t *pool);



/* Given a HASH full of property name/values, write them to a file
   located at PROPFILE_PATH */
svn_error_t *svn_wc__save_prop_file (const char *propfile_path,
                                     apr_hash_t *hash,
                                     apr_pool_t *pool);


/* Given ADM_ACCESS/NAME and an array of PROPCHANGES, merge the changes into
   the working copy.  Necessary log entries will be appended to
   ENTRY_ACCUM.

   If we are attempting to merge changes to a directory, simply pass
   ADM_ACCESS and NULL for NAME.

   If BASE_MERGE is FALSE only the working properties will be changed,
   if it is TRUE both the base and working properties will be changed.

   If conflicts are found when merging, they are placed into a
   temporary .prej file within SVN. Log entries are then written to
   move this file into PATH, or to append the conflicts to the file's
   already-existing .prej file in ADM_ACCESS. Base properties are modifed
   unconditionally, if BASE_MERGE is TRUE, they do not generate conficts.

   If STATE is non-null, set *STATE to the state of the local properties
   after the merge.  */
svn_error_t *svn_wc__merge_prop_diffs (svn_wc_notify_state_t *state,
                                       svn_wc_adm_access_t *adm_access,
                                       const char *name,
                                       const apr_array_header_t *propchanges,
                                       svn_boolean_t base_merge,
                                       svn_boolean_t dry_run,
                                       apr_pool_t *pool,
                                       svn_stringbuf_t **entry_accum);


/* Get a single 'wcprop' NAME for versioned object PATH, return in
   *VALUE.  ADM_ACCESS is an access baton set that contains PATH. */
svn_error_t *svn_wc__wcprop_get (const svn_string_t **value,
                                 const char *name,
                                 const char *path,
                                 svn_wc_adm_access_t *adm_access,
                                 apr_pool_t *pool);

/* Set a single 'wcprop' NAME to VALUE for versioned object PATH. 
   If VALUE is null, remove property NAME.  ADM_ACCESS is an access
   baton set that contains PATH. */
svn_error_t *svn_wc__wcprop_set (const char *name,
                                 const svn_string_t *value,
                                 const char *path,
                                 svn_wc_adm_access_t *adm_access,
                                 apr_pool_t *pool);

/* Remove all wc properties under ADM_ACCESS, recursively.  Do any
   temporary allocation in POOL.  */
svn_error_t *svn_wc__remove_wcprops (svn_wc_adm_access_t *adm_access,
                                     svn_boolean_t recurse,
                                     apr_pool_t *pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_WC_PROPS_H */
