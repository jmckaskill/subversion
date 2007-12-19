/*
 * mergeinfo.h : Client library-internal mergeinfo APIs.
 *
 * ====================================================================
 * Copyright (c) 2007 CollabNet.  All rights reserved.
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

#ifndef SVN_LIBSVN_CLIENT_MERGEINFO_H
#define SVN_LIBSVN_CLIENT_MERGEINFO_H


/*** Data Structures ***/


typedef struct svn_client__remaining_range_info_t {
  /* Subset of requested merge range. */
  svn_merge_range_t *range;
  /* If reflected_ranges is not NULL then above 'range' is a
     reflective range of it. */
  apr_array_header_t *reflected_ranges;
} svn_client__remaining_range_info_t;

/* Structure used by discover_and_merge_children() and consumers of the
   children_with_mergeinfo array it populates.  The struct describes
   working copy paths that meet one or more of the following criteria:

     1) Path has explicit mergeinfo
     2) Path is switched
     3) Path has an immediate child which is switched or otherwise
        missing from the WC.
     4) Path has a sibling which is switched or otherwise missing
        from the WC.
*/
typedef struct svn_client__merge_path_t
{
  const char *path;
  svn_boolean_t missing_child;       /* PATH has an immediate child which is
                                        missing. */
  svn_boolean_t switched;            /* PATH is switched. */
  svn_boolean_t has_noninheritable;  /* PATH has svn:mergeinfo set on it which
                                        includes non-inheritable revision
                                        ranges. */
  svn_boolean_t absent;              /* PATH is absent from the WC, probably
                                        due to authz restrictions. */
  const svn_string_t *propval;       /* Working mergeinfo for PATH at start
                                        of merge.  May be NULL. */
  apr_array_header_t *remaining_ranges; /* Per path remaining 
                                           svn_client__remaining_range_info_t*
                                           list. */
  apr_hash_t *pre_merge_mergeinfo;      /* mergeinfo on a path prior to a
                                           merge.*/
  svn_boolean_t indirect_mergeinfo;
  svn_boolean_t scheduled_for_deletion; /* PATH is scheduled for deletion. */
} svn_client__merge_path_t;



/*** Functions ***/

/* Find explicit or inherited WC mergeinfo for WCPATH, and return it
   in *MERGEINFO (NULL if no mergeinfo is set).  Set *INHERITED to
   whether the mergeinfo was inherited (TRUE or FALSE).

   INHERIT indicates whether explicit, explicit or inherited, or only
   inherited mergeinfo for WCPATH is retrieved.

   Don't look for inherited mergeinfo any higher than LIMIT_PATH
   (ignored if NULL).

   Set *WALKED_PATH to the path climbed from WCPATH to find inherited
   mergeinfo, or "" if none was found. (ignored if NULL). */
svn_error_t *
svn_client__get_wc_mergeinfo(apr_hash_t **mergeinfo,
                             svn_boolean_t *inherited,
                             svn_boolean_t pristine,
                             svn_mergeinfo_inheritance_t inherit,
                             const svn_wc_entry_t *entry,
                             const char *wcpath,
                             const char *limit_path,
                             const char **walked_path,
                             svn_wc_adm_access_t *adm_access,
                             svn_client_ctx_t *ctx,
                             apr_pool_t *pool);

/* Obtain any mergeinfo for the root-relative repository filesystem path
   REL_PATH from the repository, and set it in *TARGET_MERGEINFO.

   INHERIT indicates whether explicit, explicit or inherited, or only
   inherited mergeinfo for REL_PATH is obtained.

   If there is no mergeinfo available for REL_PATH, or if the server
   doesn't support a mergeinfo capability and SQUELCH_INCAPABLE is
   TRUE, set *TARGET_MERGEINFO to NULL. */
svn_error_t *
svn_client__get_repos_mergeinfo(svn_ra_session_t *ra_session,
                                apr_hash_t **target_mergeinfo,
                                const char *rel_path,
                                svn_revnum_t rev,
                                svn_mergeinfo_inheritance_t inherit,
                                svn_boolean_t squelch_incapable,
                                apr_pool_t *pool);

/* Retrieve the direct mergeinfo for the TARGET_WCPATH from the WC's
   mergeinfo prop, or that inherited from its nearest ancestor if the
   target has no info of its own.

   If no mergeinfo can be obtained from the WC or REPOS_ONLY is TRUE,
   get it from the repository (opening a new RA session if RA_SESSION
   is NULL).  Store any mergeinfo obtained for TARGET_WCPATH -- which
   is reflected by ENTRY -- in *TARGET_MERGEINFO, if no mergeinfo is
   found *TARGET_MERGEINFO is NULL.

   INHERIT indicates whether explicit, explicit or inherited, or only
   inherited mergeinfo for TARGET_WCPATH is retrieved.

   If TARGET_WCPATH inherited its mergeinfo from a working copy ancestor
   or if it was obtained from the repository, set *INDIRECT to TRUE, set it
   to FALSE *otherwise. */
svn_error_t *
svn_client__get_wc_or_repos_mergeinfo(apr_hash_t **target_mergeinfo,
                                      const svn_wc_entry_t *entry,
                                      svn_boolean_t *indirect,
                                      svn_boolean_t repos_only,
                                      svn_mergeinfo_inheritance_t inherit,
                                      svn_ra_session_t *ra_session,
                                      const char *target_wcpath,
                                      svn_wc_adm_access_t *adm_access,
                                      svn_client_ctx_t *ctx,
                                      apr_pool_t *pool);

/* Set *MERGEINFO_P to a hash of mergeinfo constructed solely from the
   natural history of PATH_OR_URL@PEG_REVISION.  RA_SESSION is an RA
   session whose session URL maps to PATH_OR_URL's URL, or NULL.
   ADM_ACCESS is a working copy administrative access baton which can
   be used to fetch information about PATH_OR_URL (if PATH_OR_URL is a
   working copy path), or NULL.  If RANGE_YOUNGEST and RANGE_OLDEST
   are valid, use them to bound the revision ranges of returned
   mergeinfo.  */
svn_error_t *
svn_client__get_history_as_mergeinfo(apr_hash_t **mergeinfo_p,
                                     const char *path_or_url,
                                     const svn_opt_revision_t *peg_revision,
                                     svn_revnum_t range_youngest,
                                     svn_revnum_t range_oldest,
                                     svn_ra_session_t *ra_session,
                                     svn_wc_adm_access_t *adm_access,
                                     svn_client_ctx_t *ctx,
                                     apr_pool_t *pool);

/* Parse any mergeinfo from the WCPATH's ENTRY and store it in
   MERGEINFO.  If PRISTINE is true parse the pristine mergeinfo,
   working otherwise. If no record of any mergeinfo exists, set
   MERGEINFO to NULL.  Does not acount for inherited mergeinfo. */
svn_error_t *
svn_client__parse_mergeinfo(apr_hash_t **mergeinfo,
                            const svn_wc_entry_t *entry,
                            const char *wcpath,
                            svn_boolean_t pristine,
                            svn_wc_adm_access_t *adm_access,
                            svn_client_ctx_t *ctx,
                            apr_pool_t *pool);

/* Write MERGEINFO into the WC for WCPATH.  If MERGEINFO is NULL,
   remove any SVN_PROP_MERGEINFO for WCPATH.  If MERGEINFO is empty,
   record an empty property value (e.g. ""). */
svn_error_t *
svn_client__record_wc_mergeinfo(const char *wcpath,
                                apr_hash_t *mergeinfo,
                                svn_wc_adm_access_t *adm_access,
                                apr_pool_t *pool);

/* Elide any svn:mergeinfo set on TARGET_PATH to its nearest working
   copy (or possibly repository) ancestor with equivalent mergeinfo.

   If WC_ELISION_LIMIT_PATH is NULL check up to the root of the working copy
   for an elision destination, if none is found check the repository,
   otherwise check as far as WC_ELISION_LIMIT_PATH within the working copy.
   TARGET_PATH and WC_ELISION_LIMIT_PATH, if it exists, must both be absolute
   or relative to the working directory.

   If TARGET_WCPATH's mergeinfo and its nearest ancestor's mergeinfo
   differ by paths existing only in TARGET_PATH's mergeinfo that map to
   empty revision ranges, then the mergeinfo between the two is considered
   equivalent and elision occurs.  If the mergeinfo between the two still
   differs then partial elision occurs: only the paths mapped to empty
   revision ranges in TARGET_WCPATH's mergeinfo elide.

   If TARGET_WCPATH's mergeinfo and its nearest ancestor's mergeinfo
   differ by paths existing only in the ancestor's mergeinfo that map to
   empty revision ranges, then the mergeinfo between the two is considered
   equivalent and elision occurs.

   If TARGET_WCPATH's mergeinfo consists only of paths mapped to empty
   revision ranges and none of these paths exist in TARGET_WCPATH's nearest
   ancestor, then elision occurs.

   If TARGET_WCPATH's mergeinfo is empty or consists only of paths mapped to
   empty revision ranges and TARGET_WCPATH has no working copy or repository
   ancestor with mergeinfo (WC_ELISION_LIMIT_PATH must be NULL to ensure the
   repository is checked), then elision occurs.
 */
svn_error_t *
svn_client__elide_mergeinfo(const char *target_wcpath,
                            const char *wc_elision_limit_path,
                            const svn_wc_entry_t *entry,
                            svn_wc_adm_access_t *adm_access,
                            svn_client_ctx_t *ctx,
                            apr_pool_t *pool);

/* For each path in CHILDREN_WITH_MERGEINFO which is an immediate child of
   TARGET_WCPATH, check if that path's mergeinfo elides to TARGET_WCPATH.
   If it does elide, clear all mergeinfo from the path.

   CHILDREN_WITH_MERGEINFO is filled with child paths (struct
   merge_path_t *) of TARGET_WCPATH which have svn:mergeinfo set on
   them, arranged in depth first order (see
   discover_and_merge_children). */
svn_error_t *
svn_client__elide_children(apr_array_header_t *children_with_mergeinfo,
                           const char *target_wcpath,
                           const svn_wc_entry_t *entry,
                           svn_wc_adm_access_t *adm_access,
                           svn_client_ctx_t *ctx,
                           apr_pool_t *pool);

/* A wrapper which calls svn_client__elide_mergeinfo() on each child
   in CHILDREN_WITH_MERGEINFO_HASH in depth-first. */
svn_error_t *
svn_client__elide_mergeinfo_for_tree(apr_hash_t *children_with_mergeinfo,
                                     svn_wc_adm_access_t *adm_access,
                                     svn_client_ctx_t *ctx,
                                     apr_pool_t *pool);


#endif /* SVN_LIBSVN_CLIENT_MERGEINFO_H */
