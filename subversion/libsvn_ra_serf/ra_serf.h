/*
 * ra_serf.h :  headers file for ra_serf
 *
 * ====================================================================
 * Copyright (c) 2006 CollabNet.  All rights reserved.
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



#include <serf.h>
#include <expat.h>
#include <apr_uri.h>

#include "svn_types.h"
#include "svn_string.h"
#include "svn_pools.h"
#include "svn_ra.h"
#include "svn_delta.h"
#include "svn_version.h"

#include "svn_dav.h"


/* A serf connection and optionally associated SSL context.  */
typedef struct {
  /* Our connection to a server. */
  serf_connection_t *conn;

  /* Bucket allocator for this connection. */
  serf_bucket_alloc_t *bkt_alloc;

  /* Host name */
  const char *hostinfo;

  /* The address where the connections are made to */
  apr_sockaddr_t *address;

  /* Are we using ssl */
  svn_boolean_t using_ssl;

  /* What was the last HTTP status code we got on this connection? */
  int last_status_code;

  /* Current authorization header used for this connection; may be NULL */
  const char *auth_header;

  /* Current authorization value used for this connection; may be NULL */
  char *auth_value;

  /* Optional SSL context for this connection. */
  serf_ssl_context_t *ssl_context;
} svn_ra_serf__connection_t;

/*
 * The master serf RA session.
 *
 * This is stored in the ra session ->priv field.
 */
typedef struct {
  /* Pool for allocations during this session */
  apr_pool_t *pool;

  /* The current context */
  serf_context_t *context;

  /* Bucket allocator for this context. */
  serf_bucket_alloc_t *bkt_alloc;

  /* Are we using ssl */
  svn_boolean_t using_ssl;

  /* The current connection */
  svn_ra_serf__connection_t **conns;
  int num_conns;
  int cur_conn;

  /* The URL that was passed into _open() */
  apr_uri_t repos_url;
  const char *repos_url_str;

  /* The actual discovered root; may be NULL until we know it. */
  apr_uri_t repos_root;
  const char *repos_root_str;

  /* Our Version-Controlled-Configuration; may be NULL until we know it. */
  const char *vcc_url;

  /* Cached properties */
  apr_hash_t *cached_props;

  /* Authentication related properties. */
  const char *realm;
  const char *auth_header;
  char *auth_value;
  svn_auth_iterstate_t *auth_state;
  int auth_attempts;

  /* Callback functions to get info from WC */
  const svn_ra_callbacks2_t *wc_callbacks;
  void *wc_callback_baton;
} svn_ra_serf__session_t;

/*
 * Structure which represents a DAV element with a NAMESPACE and NAME.
 */
typedef struct {
  /* Element namespace */
  const char *namespace;
  /* Element name */
  const char *name;
} svn_ra_serf__dav_props_t;

/*
 * Structure which represents an XML namespace.
 */
typedef struct ns_t {
  /* The assigned name. */
  const char *namespace;
  /* The full URL for this namespace. */
  const char *url;
  /* The next namespace in our list. */
  struct ns_t *next;
} svn_ra_serf__ns_t;

/*
 * An incredibly simple list.
 */
typedef struct ra_serf_list_t {
  void *data;
  struct ra_serf_list_t *next;
} svn_ra_serf__list_t;

/** DAV property sets **/

static const svn_ra_serf__dav_props_t base_props[] =
{
  { "DAV:", "version-controlled-configuration" },
  { "DAV:", "resourcetype" },
  { SVN_DAV_PROP_NS_DAV, "baseline-relative-path" },
  { SVN_DAV_PROP_NS_DAV, "repository-uuid" },
  NULL
};

static const svn_ra_serf__dav_props_t checked_in_props[] =
{
  { "DAV:", "checked-in" },
  NULL
};

static const svn_ra_serf__dav_props_t baseline_props[] =
{
  { "DAV:", "baseline-collection" },
  { "DAV:", "version-name" },
  NULL
};

static const svn_ra_serf__dav_props_t all_props[] =
{
  { "DAV:", "allprop" },
  NULL
};

static const svn_ra_serf__dav_props_t vcc_props[] =
{
  { "DAV:", "version-controlled-configuration" },
  NULL
};

static const svn_ra_serf__dav_props_t check_path_props[] =
{
  { "DAV:", "resourcetype" },
  NULL
};

static const svn_ra_serf__dav_props_t uuid_props[] =
{
  { SVN_DAV_PROP_NS_DAV, "repository-uuid" },
  NULL
};

static const svn_ra_serf__dav_props_t repos_root_props[] =
{
  { SVN_DAV_PROP_NS_DAV, "baseline-relative-path" },
  NULL
};

/* WC props compatibility with ra_dav. */
#define SVN_RA_SERF__WC_NAMESPACE SVN_PROP_WC_PREFIX "ra_dav:"
#define SVN_RA_SERF__WC_ACTIVITY_URL SVN_RA_SERF__WC_NAMESPACE "activity-url"
#define SVN_RA_SERF__WC_CHECKED_IN_URL SVN_RA_SERF__WC_NAMESPACE "version-url"

/** Serf utility functions **/

serf_bucket_t *
svn_ra_serf__conn_setup(apr_socket_t *sock,
                        void *baton,
                        apr_pool_t *pool);

serf_bucket_t*
svn_ra_serf__accept_response(serf_request_t *request,
                             serf_bucket_t *stream,
                             void *acceptor_baton,
                             apr_pool_t *pool);

void
svn_ra_serf__conn_closed(serf_connection_t *conn,
                         void *closed_baton,
                         apr_status_t why,
                         apr_pool_t *pool);

apr_status_t
svn_ra_serf__is_conn_closing(serf_bucket_t *response);

apr_status_t
svn_ra_serf__cleanup_serf_session(void *data);

/*
 * Create a REQUEST with an associated REQ_BKT in the SESSION.
 *
 * If HDRS_BKT is not-NULL, it will be set to a headers_bucket that
 * corresponds to the new request.
 *
 * The request will be METHOD at URL.
 *
 * If BODY_BKT is not-NULL, it will be sent as the request body.
 *
 * If CONTENT_TYPE is not-NULL, it will be sent as the Content-Type header.
 */
void
svn_ra_serf__setup_serf_req(serf_request_t *request,
                            serf_bucket_t **req_bkt, serf_bucket_t **hdrs_bkt,
                            svn_ra_serf__connection_t *conn,
                            const char *method, const char *url,
                            serf_bucket_t *body_bkt, const char *content_type);

/*
 * This function will run the serf context in SESS until *DONE is TRUE.
 */
svn_error_t *
svn_ra_serf__context_run_wait(svn_boolean_t *done,
                              svn_ra_serf__session_t *sess,
                              apr_pool_t *pool);

/* Callback for when a request body is needed. */
typedef serf_bucket_t*
(*svn_ra_serf__request_body_delegate_t)(void *baton,
                                        serf_bucket_alloc_t *alloc,
                                        apr_pool_t *pool);

/* Callback for when a request headers are needed. */
typedef apr_status_t
(*svn_ra_serf__request_header_delegate_t)(serf_bucket_t *headers,
                                          void *baton,
                                          apr_pool_t *pool);

/* Callback for when a response has an error. */
typedef apr_status_t
(*svn_ra_serf__response_error_t)(serf_request_t *request,
                                 serf_bucket_t *response,
                                 int status_code,
                                 void *baton);

/*
 * Structure that can be passed to our default handler to guide the
 * execution of the request through its lifecycle.
 */
typedef struct {
  const char *method;
  const char *path;

  serf_bucket_t *body_buckets;
  const char *body_type;

  serf_response_handler_t response_handler;
  void *response_baton;

  svn_ra_serf__response_error_t response_error;
  void *response_error_baton;

  serf_request_setup_t delegate;
  void *delegate_baton;

  svn_ra_serf__request_header_delegate_t header_delegate;
  void *header_delegate_baton;

  svn_ra_serf__request_body_delegate_t body_delegate;
  void *body_delegate_baton;

  svn_ra_serf__connection_t *conn;
  svn_ra_serf__session_t *session;
} svn_ra_serf__handler_t;

/* 
 * Default handler that does dispatching.
 */
apr_status_t
svn_ra_serf__handler_default(serf_request_t *request,
                             serf_bucket_t *response,
                             void *baton,
                             apr_pool_t *pool);

/*
 * Handler that discards the entire request body.
 */
apr_status_t
svn_ra_serf__handler_discard_body(serf_request_t *request,
                                  serf_bucket_t *response,
                                  void *baton,
                                  apr_pool_t *pool);

/*
 * Helper function to queue a request in the handler's connection.
 */
serf_request_t*
svn_ra_serf__request_create(svn_ra_serf__handler_t *handler);

/*
 * Helper structure associated with handle_xml_parser handler that will
 * specify how an XML response will be processed.
 */
typedef struct {
  void *user_data;

  XML_StartElementHandler start;
  XML_EndElementHandler end;
  XML_CharacterDataHandler cdata;

  XML_Parser xmlp;

  int *status_code;
  svn_boolean_t *done;
  svn_ra_serf__list_t **done_list;

  svn_ra_serf__list_t *done_item;

} svn_ra_serf__xml_parser_t;

/*
 * This function will feed the RESPONSE body into XMLP.  When parsing is
 * completed (i.e. an EOF is received), *DONE is set to TRUE.
 *
 * If an error occurs during processing RESP_ERR is invoked with the
 * RESP_ERR_BATON.
 *
 * Temporary allocations are made in POOL.
 */
apr_status_t
svn_ra_serf__handle_xml_parser(serf_request_t *request,
                               serf_bucket_t *response,
                               void *handler_baton,
                               apr_pool_t *pool);

/** XML helper functions. **/

void
svn_ra_serf__add_tag_buckets(serf_bucket_t *agg_bucket,
                             const char *tag,
                             const char *value,
                             serf_bucket_alloc_t *bkt_alloc);

/*
 * Look up the ATTRS array for namespace definitions and add each one
 * to the NS_LIST of namespaces.
 *
 * Temporary allocations are made in POOL.
 *
 * TODO: handle scoping of namespaces
 */
void
svn_ra_serf__define_ns(svn_ra_serf__ns_t **ns_list,
                       const char **attrs,
                       apr_pool_t *pool);

/*
 * Look up NAME in the NS_LIST list for previously declared namespace
 * definitions and return a DAV_PROPS_T-tuple.
 */
svn_ra_serf__dav_props_t
svn_ra_serf__expand_ns(svn_ra_serf__ns_t *ns_list,
                       const char *name);

/*
 * look for ATTR_NAME in the attrs array and return its value.
 *
 * Returns NULL if no matching name is found.
 */
const char *
svn_ra_serf__find_attr(const char **attrs,
          const char *attr_name);

void
svn_ra_serf__expand_string(const char **cur, apr_size_t *cur_len,
                           const char *new, apr_size_t new_len,
                           apr_pool_t *pool);

/** PROPFIND-related functions **/

typedef struct svn_ra_serf__propfind_context_t svn_ra_serf__propfind_context_t;

svn_boolean_t
svn_ra_serf__propfind_is_done(svn_ra_serf__propfind_context_t *ctx);

int
svn_ra_serf__propfind_status_code(svn_ra_serf__propfind_context_t *ctx);

/* Our PROPFIND bucket */
serf_bucket_t *
svn_ra_serf__bucket_propfind_create(svn_ra_serf__connection_t *conn,
                                    const char *path,
                                    const char *label,
                                    const char *depth,
                                    const svn_ra_serf__dav_props_t *find_props,
                                    serf_bucket_alloc_t *allocator);

/*
 * This function will deliver a PROP_CTX PROPFIND request in the SESS
 * serf context for the properties listed in LOOKUP_PROPS at URL for
 * DEPTH ("0","1","infinity").
 *
 * This function will not block waiting for the response.  Instead, the
 * caller is expected to call context_run and wait for the PROP_CTX->done
 * flag to be set.
 */
svn_error_t *
svn_ra_serf__deliver_props(svn_ra_serf__propfind_context_t **prop_ctx,
                           apr_hash_t *prop_vals,
                           svn_ra_serf__session_t *sess,
                           svn_ra_serf__connection_t *conn,
                           const char *url,
                           svn_revnum_t rev,
                           const char *depth,
                           const svn_ra_serf__dav_props_t *lookup_props,
                           svn_boolean_t cache_props,
                           svn_ra_serf__list_t **done_list,
                           apr_pool_t *pool);

/*
 * This helper function will block until the PROP_CTX indicates that is done
 * or another error is returned.
 */
svn_error_t *
svn_ra_serf__wait_for_props(svn_ra_serf__propfind_context_t *prop_ctx,
                            svn_ra_serf__session_t *sess,
                            apr_pool_t *pool);

/*
 * This is a blocking version of deliver_props.
 */
svn_error_t *
svn_ra_serf__retrieve_props(apr_hash_t *prop_vals,
                            svn_ra_serf__session_t *sess,
                            svn_ra_serf__connection_t *conn,
                            const char *url,
                            svn_revnum_t rev,
                            const char *depth,
                            const svn_ra_serf__dav_props_t *props,
                            apr_pool_t *pool);

/** Property walker functions **/

typedef void
(*svn_ra_serf__walker_visitor_t)(void *baton,
                                 const void *ns, apr_ssize_t ns_len,
                                 const void *name, apr_ssize_t name_len,
                                 const void *val,
                                 apr_pool_t *pool);

void
svn_ra_serf__walk_all_props(apr_hash_t *props,
                            const char *name,
                            svn_revnum_t rev,
                            svn_ra_serf__walker_visitor_t walker,
                            void *baton,
                            apr_pool_t *pool);

/* Get PROPS for PATH at REV revision with a NS:NAME. */
const void *
svn_ra_serf__get_ver_prop(apr_hash_t *props,
                          const char *path, svn_revnum_t rev,
                          const char *ns, const char *name);

/* Same as get_prop, but for the unknown revision */
const void *
svn_ra_serf__get_prop(apr_hash_t *props,
                      const char *path,
                      const char *ns,
                      const char *name);

/* Set PROPS for PATH at REV revision with a NS:NAME VAL.
 *
 * The POOL governs allocation.
 */
void
svn_ra_serf__set_rev_prop(apr_hash_t *props,
                          const char *path, svn_revnum_t rev,
                          const char *ns, const char *name,
                          const void *val, apr_pool_t *pool);

/* Same as set_rev_prop, but sets it for the unknown revision. */
void
svn_ra_serf__set_prop(apr_hash_t *props, const char *path,
                      const char *ns, const char *name,
                      const void *val, apr_pool_t *pool);

/** MERGE-related functions **/

typedef struct svn_ra_serf__merge_context_t svn_ra_serf__merge_context_t;

svn_boolean_t*
svn_ra_serf__merge_get_done_ptr(svn_ra_serf__merge_context_t *ctx);

svn_commit_info_t*
svn_ra_serf__merge_get_commit_info(svn_ra_serf__merge_context_t *ctx);

int
svn_ra_serf__merge_get_status(svn_ra_serf__merge_context_t *ctx);

/* Create an MERGE request */
svn_error_t *
svn_ra_serf__merge_create_req(svn_ra_serf__merge_context_t **merge_ctx,
                              svn_ra_serf__session_t *session,
                              svn_ra_serf__connection_t *conn,
                              const char *path,
                              const char *activity_url,
                              apr_size_t activity_url_len,
                              apr_pool_t *pool);

/** OPTIONS-related functions **/

typedef struct svn_ra_serf__options_context_t svn_ra_serf__options_context_t;

/* Is this OPTIONS-request done yet? */
svn_boolean_t*
svn_ra_serf__get_options_done_ptr(svn_ra_serf__options_context_t *ctx);

const char *
svn_ra_serf__options_get_activity_collection(svn_ra_serf__options_context_t *ctx);

/* Create an OPTIONS request */
svn_error_t *
svn_ra_serf__create_options_req(svn_ra_serf__options_context_t **opt_ctx,
                                svn_ra_serf__session_t *session,
                                svn_ra_serf__connection_t *conn,
                                const char *path,
                                apr_pool_t *pool);

/* Try to discover our current root @a vcc_url and the resultant @a rel_path
 * based on @a orig_path for the @a session on @a conn.
 *
 * @a rel_path may be NULL if the caller is not interested in the relative
 * path.
 *
 * All temporary allocations will be made in @a pool.
 */
svn_error_t *
svn_ra_serf__discover_root(const char **vcc_url,
                           const char **rel_path,
                           svn_ra_serf__session_t *session,
                           svn_ra_serf__connection_t *conn,
                           const char *orig_path,
                           apr_pool_t *pool);

/** RA functions **/

svn_error_t *
svn_ra_serf__get_log(svn_ra_session_t *session,
                     const apr_array_header_t *paths,
                     svn_revnum_t start,
                     svn_revnum_t end,
                     int limit,
                     svn_boolean_t discover_changed_paths,
                     svn_boolean_t strict_node_history,
                     svn_log_message_receiver_t receiver,
                     void *receiver_baton,
                     apr_pool_t *pool);

svn_error_t *
svn_ra_serf__get_locations(svn_ra_session_t *session,
                           apr_hash_t **locations,
                           const char *path,
                           svn_revnum_t peg_revision,
                           apr_array_header_t *location_revisions,
                           apr_pool_t *pool);

svn_error_t *
svn_ra_serf__do_diff(svn_ra_session_t *session,
                     const svn_ra_reporter2_t **reporter,
                     void **report_baton,
                     svn_revnum_t revision,
                     const char *diff_target,
                     svn_boolean_t recurse,
                     svn_boolean_t ignore_ancestry,
                     svn_boolean_t text_deltas,
                     const char *versus_url,
                     const svn_delta_editor_t *diff_editor,
                     void *diff_baton,
                     apr_pool_t *pool);

svn_error_t *
svn_ra_serf__do_status(svn_ra_session_t *ra_session,
                       const svn_ra_reporter2_t **reporter,
                       void **report_baton,
                       const char *status_target,
                       svn_revnum_t revision,
                       svn_boolean_t recurse,
                       const svn_delta_editor_t *status_editor,
                       void *status_baton,
                       apr_pool_t *pool);

svn_error_t *
svn_ra_serf__do_update(svn_ra_session_t *ra_session,
                       const svn_ra_reporter2_t **reporter,
                       void **report_baton,
                       svn_revnum_t revision_to_update_to,
                       const char *update_target,
                       svn_boolean_t recurse,
                       const svn_delta_editor_t *update_editor,
                       void *update_baton,
                       apr_pool_t *pool);

svn_error_t *
svn_ra_serf__get_file_revs(svn_ra_session_t *session,
                           const char *path,
                           svn_revnum_t start,
                           svn_revnum_t end,
                           svn_ra_file_rev_handler_t handler,
                           void *handler_baton,
                           apr_pool_t *pool);

svn_error_t *
svn_ra_serf__get_commit_editor(svn_ra_session_t *session,
                               const svn_delta_editor_t **editor,
                               void **edit_baton,
                               const char *log_msg,
                               svn_commit_callback2_t callback,
                               void *callback_baton,
                               apr_hash_t *lock_tokens,
                               svn_boolean_t keep_locks,
                               apr_pool_t *pool);

svn_error_t *
svn_ra_serf__get_file(svn_ra_session_t *session,
                      const char *path,
                      svn_revnum_t revision,
                      svn_stream_t *stream,
                      svn_revnum_t *fetched_rev,
                      apr_hash_t **props,
                      apr_pool_t *pool);
