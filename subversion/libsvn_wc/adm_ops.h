/*
 * adm_ops.h :  side-effecting wc adm information
 *              (This code doesn't know where any adm information is
 *              located.  The caller always passes in a path obtained
 *              by using the adm_files.h API.)
 *        
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


#ifndef SVN_LIBSVN_WC_ADM_OPS_H
#define SVN_LIBSVN_WC_ADM_OPS_H

#include <apr_pools.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* Modify the entry of working copy PATH, presumably after an update
   completes.   If PATH doesn't exist, this routine does nothing.
   ADM_ACCESS must be an access baton for PATH (assuming it existed).

   Set the entry's 'url' and 'working revision' fields to BASE_URL and
   NEW_REVISION.  If BASE_URL is null, the url field is untouched; if
   NEW_REVISION in invalid, the working revision field is untouched.
   The modifications are mutually exclusive.

   If PATH is a directory and RECURSIVE is set, then recursively walk
   over all entries files below PATH.  While doing this, if
   NEW_REVISION is valid, then tweak every entry to have this new
   working revision (excluding files that are scheduled for addition
   or replacement.)  Likewise, if BASE_URL is non-null, then rewrite
   all urls to be "telescoping" children of the base_url.

   If REMOVE_MISSING_DIRS is TRUE, then delete the entries for any 
   missing directories.  If NOTIFY_FUNC is non-null, invoke it with 
   NOTIFY_BATON for each missing entry deleted.
*/
svn_error_t *svn_wc__do_update_cleanup (const char *path,
                                        svn_wc_adm_access_t *adm_access,
                                        svn_boolean_t recursive,
                                        const char *base_url,
                                        svn_revnum_t new_revision,
                                        svn_wc_notify_func_t notify_func,
                                        void *notify_baton,
                                        svn_boolean_t remove_missing_dirs,
                                        apr_pool_t *pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_WC_ADM_OPS_H */