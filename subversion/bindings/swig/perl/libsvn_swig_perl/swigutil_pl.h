/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2000-2007 CollabNet.  All rights reserved.
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
 * @endcopyright
 *
 * @file swigutil_pl.h
 * @brief Utility functions and related code for the SWIG Perl bindings
 */


#ifndef SVN_SWIG_SWIGUTIL_PL_H
#define SVN_SWIG_SWIGUTIL_PL_H

#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

#include <apr.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_hash.h>
#include <apr_tables.h>

#include "svn_types.h"
#include "svn_string.h"
#include "svn_delta.h"
#include "svn_client.h"
#include "svn_repos.h"
#include "svn_private_config.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#if defined(_MSC_VER)
#  if _MSC_VER >= 1300 && _INTEGRAL_MAX_BITS >= 64
#    define strtoll _strtoi64
#  else
#    define strtoll(str, endptr, base) _atoi64(str)
#  endif
#endif



#if defined(SVN_AVOID_CIRCULAR_LINKAGE_AT_ALL_COSTS_HACK)
typedef apr_pool_t *(*svn_swig_pl_get_current_pool_t)(void);
typedef void (*svn_swig_pl_set_current_pool_t)(apr_pool_t *pool);

void svn_swig_pl_bind_current_pool_fns(svn_swig_pl_get_current_pool_t get,
                                       svn_swig_pl_set_current_pool_t set);
#endif

apr_pool_t *svn_swig_pl_make_pool(SV *obj);

/** Used by callers of svn_swig_pl_callback_func() to specify whether the
 * function should be called as a method or as a function.
 */
typedef enum perl_func_invoker {
    CALL_METHOD,
    CALL_SV
} perl_func_invoker_t;

/** Call @a func, placing the result in @a **result (unless @a
 * **result is NULL, in which case it is ignored).  @a caller_func is
 * either CALL_SV, in which case @a func is called as a function (with
 * call_sv()) or CALL_METHOD, in which case @a func is called as a
 * method (with call_method()).
 *
 * The variadic arguments following @a fmt are passed, in order, to @a func.
 *
 * @a fmt is a printf()-like format string that specifies the types of
 * the arguments that follow, and therefore how they should be
 * converted to Perl data types when calling @a func.  Each character
 * in the string represents one argument.  The recognised
 * characters, and their meanings, are:
 *
 *  - O: perl object
 *  - i: apr_int32_t
 *  - u: apr_uint32_t
 *  - L: apr_int64_t
 *  - U: apr_uint64_t
 *  - s: string
 *  - S: swigtype
 *  - r: svn_revnum_t
 *  - b: svn_boolean_t
 *  - t: svn_string_t
 *  - z: apr_size_t
 *  
 *  Please do not add C types here.  Add a new format code if needed.
 *  Using the underlying C types and not the APR or SVN types can
 *  break things if these data types change in the future or on
 *  platforms which use different types.
 */

svn_error_t *svn_swig_pl_callback_thunk(perl_func_invoker_t caller_func,
                                        void *func,
                                        SV **result,
                                        const char *fmt, ...);

SV *svn_swig_pl_prophash_to_hash(apr_hash_t *hash);
SV *svn_swig_pl_convert_hash(apr_hash_t *hash, swig_type_info *tinfo);

const apr_array_header_t *svn_swig_pl_strings_to_array(SV *source,
                                                       apr_pool_t *pool);

apr_hash_t *svn_swig_pl_strings_to_hash(SV *source,
                                        apr_pool_t *pool);
apr_hash_t *svn_swig_pl_objs_to_hash(SV *source, swig_type_info *tinfo,
                                     apr_pool_t *pool);
apr_hash_t *svn_swig_pl_objs_to_hash_by_name(SV *source,
                                             const char *typename,
                                             apr_pool_t *pool);
const apr_array_header_t *svn_swig_pl_objs_to_array(SV *source,
                                                    swig_type_info *tinfo,
                                                    apr_pool_t *pool);

SV *svn_swig_pl_array_to_list(const apr_array_header_t *array);
/* Formerly used by pre-1.0 APIs. Now unused
SV *svn_swig_pl_ints_to_list(const apr_array_header_t *array);
*/
SV *svn_swig_pl_convert_array(const apr_array_header_t *array,
                              swig_type_info *tinfo);


/** Call a Perl callback invoked by the SWIG wrapper for svn_client_list().
 * @a baton points to the Perl subroutine to call, @a path, @a dirent,
 * @a lock, @a abs_path, and @a pool are converted to their Perl equivalents
 * and passed to the callback.
 */
svn_error_t *svn_swig_pl_thunk_list_receiver(void *baton,
					     const char *path,
					     const svn_dirent_t *dirent,
					     const svn_lock_t *lock,
					     const char *abs_path,
					     apr_pool_t *pool);

/* thunked log receiver function.  */
svn_error_t * svn_swig_pl_thunk_log_receiver(void *py_receiver,
                                             apr_hash_t *changed_paths,
                                             svn_revnum_t rev,
                                             const char *author,
                                             const char *date,
                                             const char *msg,
                                             apr_pool_t *pool);
/* thunked commit editor callback. */
svn_error_t *svn_swig_pl_thunk_commit_callback(svn_revnum_t new_revision,
					       const char *date,
					       const char *author,
					       void *baton);

/* thunked repos_history callback. */
svn_error_t *svn_swig_pl_thunk_history_func(void *baton,
                                            const char *path,
                                            svn_revnum_t revision,
                                            apr_pool_t *pool);

/* thunked dir_delta authz read function. */
svn_error_t *svn_swig_pl_thunk_authz_func(svn_boolean_t *allowed,
                                          svn_fs_root_t *root,
                                          const char *path,
                                          void *baton,
                                          apr_pool_t *pool);

/* ra callbacks. */
svn_error_t *svn_ra_make_callbacks(svn_ra_callbacks_t **cb,
				   void **c_baton,
				   SV *perl_callbacks,
				   apr_pool_t *pool);

/* thunked simple_prompt callback function */
svn_error_t *svn_swig_pl_thunk_simple_prompt(svn_auth_cred_simple_t **cred,
                                             void *baton,
                                             const char *realm,
                                             const char *username,
                                             svn_boolean_t may_save,
                                             apr_pool_t *pool);

/* thunked username_prompt callback function */
svn_error_t *svn_swig_pl_thunk_username_prompt(svn_auth_cred_username_t **cred,
                                               void *baton,
                                               const char *realm,
                                               svn_boolean_t may_save,
                                               apr_pool_t *pool);

/* thunked ssl_server_trust_prompt callback function */
svn_error_t *svn_swig_pl_thunk_ssl_server_trust_prompt
  (svn_auth_cred_ssl_server_trust_t **cred,
   void *baton,
   const char *realm,
   apr_uint32_t failures,
   const svn_auth_ssl_server_cert_info_t *cert_info,
   svn_boolean_t may_save,
   apr_pool_t *pool);

/* thunked ssl_client_cert callback function */
svn_error_t *svn_swig_pl_thunk_ssl_client_cert_prompt
  (svn_auth_cred_ssl_client_cert_t **cred,
   void *baton,
   const char *realm,
   svn_boolean_t may_save,
   apr_pool_t *pool);

/* thunked ssl_client_cert_pw callback function */
svn_error_t *svn_swig_pl_thunk_ssl_client_cert_pw_prompt
  (svn_auth_cred_ssl_client_cert_pw_t **cred,
   void *baton,
   const char *realm,
   svn_boolean_t may_save,
   apr_pool_t *pool);

/* thunked callback for svn_ra_get_wc_prop_func_t */
svn_error_t *thunk_get_wc_prop(void *baton,
                               const char *relpath,
                               const char *name,
                               const svn_string_t **value,
                               apr_pool_t *pool);

/* Thunked version of svn_wc_notify_func_t callback type */
void svn_swig_pl_notify_func(void * baton,
                             const char *path,
		             svn_wc_notify_action_t action,
			     svn_node_kind_t kind,
			     const char *mime_type,
			     svn_wc_notify_state_t content_state,
			     svn_wc_notify_state_t prop_state,
			     svn_revnum_t revision);


/* Thunked version of svn_client_get_commit_log3_t callback type. */
svn_error_t *svn_swig_pl_get_commit_log_func(const char **log_msg,
                                             const char **tmp_file,
                                             const apr_array_header_t *
                                             commit_items,
                                             void *baton,
                                             apr_pool_t *pool);

/* Thunked version of svn_client_info_t callback type. */
svn_error_t *svn_swig_pl_info_receiver(void *baton,
                                       const char *path,
                                       const svn_info_t *info,
                                       apr_pool_t *pool);

/* Thunked version of svn_wc_cancel_func_t callback type. */
svn_error_t *svn_swig_pl_cancel_func(void *cancel_baton);

/* Thunked version of svn_wc_status_func_t callback type. */
void svn_swig_pl_status_func(void *baton,
                             const char *path,
                             svn_wc_status_t *status);
/* Thunked version of svn_client_blame_receiver_t callback type. */
svn_error_t *svn_swig_pl_blame_func(void *baton,
                                    apr_int64_t line_no,
                                    svn_revnum_t revision,
                                    const char *author,
                                    const char *date,
                                    const char *line,
                                    apr_pool_t *pool);

/* Thunked config enumerator */
svn_boolean_t svn_swig_pl_thunk_config_enumerator(const char *name, const char *value, void *baton);

/* helper for making the editor */
void svn_delta_make_editor(svn_delta_editor_t **editor,
                           void **edit_baton,
                           SV *perl_editor,
                           apr_pool_t *pool);

/* svn_stream_t helpers */
svn_error_t *svn_swig_pl_make_stream(svn_stream_t **stream, SV *obj);
SV *svn_swig_pl_from_stream(svn_stream_t *stream);

/* apr_file_t * */
apr_file_t *svn_swig_pl_make_file(SV *file, apr_pool_t *pool);

void svn_swig_pl_hold_ref_in_pool(apr_pool_t *pool, SV *sv);

/* md5 access class */
SV *svn_swig_pl_from_md5(unsigned char *digest);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif  /* SVN_SWIG_SWIGUTIL_PL_H */
