/*
 * ra_dav.h :  private declarations for the RA/DAV module
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
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



#ifndef RA_DAV_H
#define RA_DAV_H

#include <apr_pools.h>
#include <apr_tables.h>

#include <ne_request.h>
#include <ne_uri.h>
#include <ne_207.h>            /* for NE_ELM_207_UNUSED */
#include <ne_props.h>          /* for ne_propname */

#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_delta.h"
#include "svn_ra.h"


typedef struct {
  apr_pool_t *pool;

  struct uri root;              /* repository root */

  ne_session *sess;           /* HTTP session to server */
  ne_session *sess2;
  
  svn_ra_callbacks_t *callbacks;  /* callbacks to get auth data */
  void *callback_baton;

  int number_of_tries;    /* how many times neon has attempted to
                             fetch authentication info */

} svn_ra_session_t;


#ifdef SVN_DEBUG
#define DEBUG_CR "\n"
#else
#define DEBUG_CR ""
#endif


/** plugin function prototypes */

svn_error_t *svn_ra_dav__get_latest_revnum(void *session_baton,
                                           svn_revnum_t *latest_revnum);

svn_error_t *svn_ra_dav__get_dated_revision (void *session_baton,
                                             svn_revnum_t *revision,
                                             apr_time_t timestamp);

svn_error_t * svn_ra_dav__get_commit_editor(
  void *session_baton,
  const svn_delta_edit_fns_t **editor,
  void **edit_baton,
  svn_stringbuf_t *log_msg,
  svn_ra_get_wc_prop_func_t get_func,
  svn_ra_set_wc_prop_func_t set_func,
  svn_ra_close_commit_func_t close_func,
  void *close_baton);

svn_error_t * svn_ra_dav__abort_commit(
 void *session_baton,
 void *edit_baton);

svn_error_t * svn_ra_dav__do_checkout (
  void *session_baton,
  svn_revnum_t revision,
  svn_boolean_t recurse,
  const svn_delta_edit_fns_t *editor,
  void *edit_baton);

svn_error_t * svn_ra_dav__do_update(
  void *session_baton,
  const svn_ra_reporter_t **reporter,
  void **report_baton,
  svn_revnum_t revision_to_update_to,
  svn_stringbuf_t *update_target,
  svn_boolean_t recurse,
  const svn_delta_edit_fns_t *wc_update,
  void *wc_update_baton);

svn_error_t * svn_ra_dav__do_status(
  void *session_baton,
  const svn_ra_reporter_t **reporter,
  void **report_baton,
  svn_stringbuf_t *status_target,
  svn_boolean_t recurse,
  const svn_delta_edit_fns_t *wc_status,
  void *wc_status_baton);

svn_error_t * svn_ra_dav__get_log(
  void *session_baton,
  apr_array_header_t *paths,
  svn_revnum_t start,
  svn_revnum_t end,
  svn_boolean_t discover_changed_paths,
  svn_log_message_receiver_t receiver,
  void *receiver_baton);

/*
** SVN_RA_DAV__LP_*: local properties for RA/DAV
**
** ra_dav stores properties on the client containing information needed
** to operate against the SVN server. Some of this informations is strictly
** necessary to store, and some is simply stored as a cached value.
*/

#define SVN_RA_DAV__LP_NAMESPACE SVN_PROP_WC_PREFIX "ra_dav:"

/* store the URL where Activities can be created */
#define SVN_RA_DAV__LP_ACTIVITY_URL     SVN_RA_DAV__LP_NAMESPACE "activity-url"

/* store the URL of the version resource (from the DAV:checked-in property) */
#define SVN_RA_DAV__LP_VSN_URL          SVN_RA_DAV__LP_NAMESPACE "version-url"


/*
** SVN_RA_DAV__PROP_*: properties that we fetch from the server
**
** These are simply symbolic names for some standard properties that we fetch.
*/
#define SVN_RA_DAV__PROP_BASELINE_COLLECTION    "DAV:baseline-collection"
#define SVN_RA_DAV__PROP_CHECKED_IN     "DAV:checked-in"
#define SVN_RA_DAV__PROP_VCC            "DAV:version-controlled-configuration"
#define SVN_RA_DAV__PROP_VERSION_NAME   "DAV:version-name"

#define SVN_RA_DAV__PROP_BASELINE_RELPATH \
    SVN_PROP_PREFIX "baseline-relative-path"

typedef struct {
  /* what is the URL for this resource */
  const char *url;

  /* is this resource a collection? (from the DAV:resourcetype element) */
  int is_collection;

  /* PROPSET: NAME -> VALUE (const char * -> const char *) */
  apr_hash_t *propset;

  /* --- only used during response processing --- */
  /* when we see a DAV:href element, what element is the parent? */
  int href_parent;

  apr_pool_t *pool;

} svn_ra_dav_resource_t;

/* ### WARNING: which_props can only identify properties which props.c
   ### knows about. see the elem_definitions[] array. */

/* fetch a bunch of properties from the server. */
svn_error_t * svn_ra_dav__get_props(apr_hash_t **results,
                                    ne_session *sess,
                                    const char *url,
                                    int depth,
                                    const char *label,
                                    const ne_propname *which_props,
                                    apr_pool_t *pool);

/* fetch a single resource's props from the server. */
svn_error_t * svn_ra_dav__get_props_resource(svn_ra_dav_resource_t **rsrc,
                                             ne_session *sess,
                                             const char *url,
                                             const char *label,
                                             const ne_propname *which_props,
                                             apr_pool_t *pool);

/* fetch a single property from a single resource */
svn_error_t * svn_ra_dav__get_one_prop(const svn_string_t **propval,
                                       ne_session *sess,
                                       const char *url,
                                       const char *label,
                                       const ne_propname *propname,
                                       apr_pool_t *pool);

/* Get various Baseline-related information for a given "public" URL.

   Given a Neon session SESS and a URL, return whether the URL is a
   directory in IS_DIR. IS_DIR may be NULL if this flag is unneeded.

   REVISION may be SVN_INVALID_REVNUM to indicate that the operation
   should work against the latest (HEAD) revision, or whether it should
   return information about that specific revision.

   If BC_URL is not NULL, then it will be filled in with the URL for
   the Baseline Collection for the specified revision, or the HEAD.

   If BC_RELATIVE is not NULL, then it will be filled in with a
   relative pathname for the baselined resource corresponding to the
   revision of the resource specified by URL.

   If LATEST_REV is not NULL, then it will be filled in with the revision
   that this information corresponds to. Generally, this will be the same
   as the REVISION parameter, unless we are working against the HEAD. In
   that case, the HEAD revision number is returned.

   Allocation for BC_URL->data, BC_RELATIVE->data, and temporary data,
   will occur in POOL.

   Note: a Baseline Collection is a complete tree for a specified Baseline.
   DeltaV baselines correspond one-to-one to Subversion revisions. Thus,
   the entire state of a revision can be found in a Baseline Collection.
*/
svn_error_t *svn_ra_dav__get_baseline_info(svn_boolean_t *is_dir,
                                           svn_string_t *bc_url,
                                           svn_string_t *bc_relative,
                                           svn_revnum_t *latest_rev,
                                           ne_session *sess,
                                           const char *url,
                                           svn_revnum_t revision,
                                           apr_pool_t *pool);

extern const ne_propname svn_ra_dav__vcc_prop;
extern const ne_propname svn_ra_dav__checked_in_prop;




/* send an OPTIONS request to fetch the activity-collection-set */
svn_error_t * svn_ra_dav__get_activity_url(svn_stringbuf_t **activity_url,
                                           svn_ra_session_t *ras,
                                           const char *url,
                                           apr_pool_t *pool);


/* Send a METHOD request (e.g., "MERGE", "REPORT", "PROPFIND") to URL
 * in session RAS, and parse the response.  If BODY is non-null, it is
 * the body of the request, else use the contents of file FD as the body.
 *
 * ELEMENTS is the set of xml elements to recognize in the response.
 *
 * VALIDATE_CB, STARTELM_CB, and ENDELM_CB are Neon validation, start
 * element, and end element handlers, respectively.  BATON is passed
 * to each as userdata.
 *
 * Use POOL for any temporary allocation.
 */
svn_error_t *svn_ra_dav__parsed_request(svn_ra_session_t *ras,
                                        const char *method,
                                        const char *url,
                                        const char *body,
                                        int fd,
                                        const struct ne_xml_elm *elements, 
                                        ne_xml_validate_cb validate_cb,
                                        ne_xml_startelm_cb startelm_cb, 
                                        ne_xml_endelm_cb endelm_cb,
                                        void *baton,
                                        apr_pool_t *pool);


/* ### add SVN_RA_DAV_ to these to prefix conflicts with (sys) headers? */
enum {
  /* DAV elements */
  ELEM_activity_coll_set = NE_ELM_207_UNUSED,
  ELEM_baseline,
  ELEM_baseline_coll,
  ELEM_checked_in,
  ELEM_collection,
  ELEM_comment,
  ELEM_creator_displayname,
  ELEM_ignored_set,
  ELEM_merge_response,
  ELEM_merged_set,
  ELEM_options_response,
  ELEM_remove_prop,
  ELEM_resourcetype,
  ELEM_updated_set,
  ELEM_vcc,
  ELEM_version_name,

  /* SVN elements */
  ELEM_add_directory,
  ELEM_add_file,
  ELEM_baseline_relpath,
  ELEM_deleted_path,  /* used in log reports */
  ELEM_added_path,    /* used in log reports */
  ELEM_changed_path,  /* used in log reports */
  ELEM_delete_entry,
  ELEM_fetch_file,
  ELEM_fetch_props,
  ELEM_log_date,
  ELEM_log_item,
  ELEM_log_report,
  ELEM_replace_directory,
  ELEM_replace_file,
  ELEM_target_revision,
  ELEM_update_report
};

/* ### docco */
svn_error_t * svn_ra_dav__merge_activity(
    svn_ra_session_t *ras,
    const char *repos_url,
    const char *activity_url,
    apr_hash_t *valid_targets,
    svn_ra_set_wc_prop_func_t set_prop,
    svn_ra_close_commit_func_t close_commit,
    void *close_baton,
    apr_array_header_t *deleted_entries,
    apr_pool_t *pool);


/* Make a buffer for repeated use with svn_stringbuf_set().
   ### it would be nice to start this buffer with N bytes, but there isn't
   ### really a way to do that in the string interface (yet), short of
   ### initializing it with a fake string (and copying it) */
#define MAKE_BUFFER(p) svn_stringbuf_ncreate("", 0, (p))

void svn_ra_dav__copy_href(svn_stringbuf_t *dst, const char *src);



/* If RAS contains authentication info, attempt to store it via client
   callbacks.  */
svn_error_t *
svn_ra_dav__maybe_store_auth_info (svn_ra_session_t *ras);


#endif  /* RA_DAV_H */


/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
