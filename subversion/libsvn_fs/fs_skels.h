/* fs_skels.h : headers for conversion between fs native types and
 *              skeletons
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
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

#ifndef SVN_LIBSVN_FS_FS_SKELS_H
#define SVN_LIBSVN_FS_FS_SKELS_H

#include "db.h"                 /* Berkeley DB interface */
#include "apr_pools.h"
#include "apr_hash.h"
#include "svn_fs.h"
#include "skel.h"
#include "fs.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/*** Parsing (conversion from skeleton to native FS type) ***/


/* Parse a `PROPLIST' SKEL into a regular hash of properties,
   *PROPLIST_P, which has const char * property names, and svn_string_t * 
   values.  Use POOL for all allocations.  */
svn_error_t *
svn_fs__parse_proplist_skel (apr_hash_t **proplist_p,
                             skel_t *skel,
                             apr_pool_t *pool);

/* Parse a `REVISION' SKEL into *REVISION_P.  Use POOL for all
   allocations.  */
svn_error_t *
svn_fs__parse_revision_skel (svn_fs__revision_t **revision_p, 
                             skel_t *skel,
                             apr_pool_t *pool);

/* Parse a `TRANSACTION' SKEL into *TRANSACTION_P.  Use POOL for all
   allocations.  */
svn_error_t *
svn_fs__parse_transaction_skel (svn_fs__transaction_t **transaction_p, 
                                skel_t *skel,
                                apr_pool_t *pool);

/* Parse a `REPRESENTATION' SKEL into *REP_P.  Use POOL for all
   allocations.  */
svn_error_t *
svn_fs__parse_representation_skel (svn_fs__representation_t **rep_p,
                                   skel_t *skel,
                                   apr_pool_t *pool);


/*** Unparsing (conversion from native FS type to skeleton) ***/


/* Unparse a PROPLIST hash (which has const char * property names and
   svn_stringbuf_t * values) into a `PROPLIST' *SKEL_P.  Use POOL for
   all allocations.  */
svn_error_t *
svn_fs__unparse_proplist_skel (skel_t **skel_p,
                               apr_hash_t *proplist,
                               apr_pool_t *pool);

/* Unparse *REVISION into a `REVISION' *SKEL_P.  Use POOL for all
   allocations.  */
svn_error_t *
svn_fs__unparse_revision_skel (skel_t **skel_p,
                               svn_fs__revision_t *revision,
                               apr_pool_t *pool);

/* Unparse *TRANSACTION into a `TRANSACTION' *SKEL_P.  Use POOL for all
   allocations.  */
svn_error_t *
svn_fs__unparse_transaction_skel (skel_t **skel_p,
                                  svn_fs__transaction_t *transaction,
                                  apr_pool_t *pool);

/* Unparse *REP into a `REPRESENTATION' *SKEL_P.  Use POOL for all
   allocations.  */
svn_error_t *
svn_fs__unparse_representation_skel (skel_t **skel_p,
                                     svn_fs__representation_t *rep,
                                     apr_pool_t *pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_FS_SKELS_H */


/* 
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end:
 */
