/*
 * swigutil_py.h :  utility functions and stuff for the SWIG Python bindings
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
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


#ifndef SVN_SWIG_SWIGUTIL_PY_H
#define SVN_SWIG_SWIGUTIL_PY_H

#include <Python.h>

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

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */



#if SVN_SWIG_VERSION < 103024
/* If this file is being included outside of a wrapper file, then need to
   create stubs for some of the SWIG types. */

/* if SWIGEXPORT is defined, then we're in a wrapper. otherwise, we need
   the prototypes and type definitions. */
#ifndef SWIGEXPORT
#define SVN_NEED_SWIG_TYPES
#endif

#ifdef SVN_NEED_SWIG_TYPES

#if SVN_SWIG_VERSION >= 103020
#include "python/precommon.swg"
#ifndef SWIG_ConvertPtr
#define SWIG_ConvertPtr SWIG_Python_ConvertPtr
#endif
#ifndef SWIG_NewPointerObj
#define SWIG_NewPointerObj SWIG_Python_NewPointerObj
#endif
#endif

typedef struct _unnamed swig_type_info;

PyObject *SWIG_NewPointerObj(void *, swig_type_info *, int own);
swig_type_info *SWIG_TypeQuery(const char *name);
int SWIG_ConvertPtr(PyObject *, void **, swig_type_info *, int flags);

#endif /* SVN_NEED_SWIG_TYPES */
#endif /* SVN_SWIG_VERSION < 103024 */


/* Functions to manage python's global interpreter lock */
void svn_swig_py_release_py_lock(void);
void svn_swig_py_acquire_py_lock(void);


/*** Functions to expose a custom SubversionException ***/

/* register a new subversion exception class */
PyObject *svn_swig_py_register_exception(void);

/* get the object which represents the subversion exception class */
PyObject *svn_swig_py_exception_type(void);

/* raise a subversion exception, created from a normal subversion error */
void svn_swig_py_svn_exception(svn_error_t *err);



/* helper function to convert an apr_hash_t* (char* -> svnstring_t*) to
   a Python dict */
PyObject *svn_swig_py_prophash_to_dict(apr_hash_t *hash);

/* helper function to convert an apr_hash_t* (svn_revnum_t* -> const
   char *) to a Python dict */
PyObject *svn_swig_py_locationhash_to_dict(apr_hash_t *hash);

/* convert a hash of 'const char *' -> TYPE into a Python dict */
PyObject *svn_swig_py_convert_hash(apr_hash_t *hash, swig_type_info *type);

/* helper function to convert a 'char **' into a Python list of string
   objects */
PyObject *svn_swig_py_c_strings_to_list(char **strings);

/* helper function to convert an array of 'const char *' to a Python list
   of string objects */
PyObject *svn_swig_py_array_to_list(const apr_array_header_t *strings);

/* helper function to convert an array of 'svn_revnum_t' to a Python list
   of int objects */
/* Formerly used by pre-1.0 APIs. Now unused
PyObject *svn_swig_py_revarray_to_list(const apr_array_header_t *revs);
*/

/* helper function to convert a Python dictionary mapping strings to
   strings into an apr_hash_t mapping const char *'s to const char *'s,
   allocated in POOL. */
apr_hash_t *svn_swig_py_stringhash_from_dict(PyObject *dict,
                                             apr_pool_t *pool);

/* helper function to convert a Python dictionary mapping strings to
   strings into an apr_hash_t mapping const char *'s to svn_string_t's,
   allocated in POOL. */
apr_hash_t *svn_swig_py_prophash_from_dict(PyObject *dict,
                                           apr_pool_t *pool);

/* helper function to convert a Python sequence of strings into an
   'apr_array_header_t *' of 'const char *' objects.  Note that the
   objects must remain alive -- the values are not copied. This is
   appropriate for incoming arguments which are defined to last the
   duration of the function's execution.  */
const apr_array_header_t *svn_swig_py_strings_to_array(PyObject *source,
                                                       apr_pool_t *pool);

/* like svn_swig_py_strings_to_array(), but for array's of 'svn_revnum_t's. */
const apr_array_header_t *svn_swig_py_revnums_to_array(PyObject *source,
                                                       apr_pool_t *pool);

/* make an editor that "thunks" from C callbacks up to Python */
void svn_swig_py_make_editor(const svn_delta_editor_t **editor,
                             void **edit_baton,
                             PyObject *py_editor,
                             apr_pool_t *pool);

apr_file_t *svn_swig_py_make_file(PyObject *py_file,
                                  apr_pool_t *pool);

svn_stream_t *svn_swig_py_make_stream(PyObject *py_io,
                                      apr_pool_t *pool);

/* a notify function that executes a Python function that is passed in
   via the baton argument */
void svn_swig_py_notify_func(void *baton,
                             const char *path,
                             svn_wc_notify_action_t action,
                             svn_node_kind_t kind,
                             const char *mime_type,
                             svn_wc_notify_state_t content_state,
                             svn_wc_notify_state_t prop_state,
                             svn_revnum_t revision);

/* a status function that executes a Python function that is passed in
   via the baton argument */
void svn_swig_py_status_func(void *baton,
                             const char *path,
                             svn_wc_status_t *status);

/* a cancel function that executes a Python function passed in via the
   cancel_baton argument. */
svn_error_t *svn_swig_py_cancel_func(void *cancel_baton);

/* thunked fs get_locks function */
svn_error_t *svn_swig_py_fs_get_locks_func (void *baton, 
                                            svn_lock_t *lock, 
                                            apr_pool_t *pool);

/* thunked commit log fetcher */
svn_error_t *svn_swig_py_get_commit_log_func(const char **log_msg,
                                             const char **tmp_file,
                                             apr_array_header_t *commit_items,
                                             void *baton,
                                             apr_pool_t *pool);

/* thunked repos authz callback function */
svn_error_t *svn_swig_py_repos_authz_func(svn_boolean_t *allowed,
                                          svn_fs_root_t *root,
                                          const char *path,
                                          void *baton,
                                          apr_pool_t *pool);

/* thunked history callback function */
svn_error_t *svn_swig_py_repos_history_func(void *baton,
                                            const char *path,
                                            svn_revnum_t revision,
                                            apr_pool_t *pool);

/* thunked log receiver function */
svn_error_t *svn_swig_py_log_receiver(void *py_receiver,
                                      apr_hash_t *changed_paths,
                                      svn_revnum_t rev,
                                      const char *author,
                                      const char *date,
                                      const char *msg,
                                      apr_pool_t *pool);

/* thunked blame receiver function */
svn_error_t *svn_swig_py_client_blame_receiver_func(void *baton,
                                                    apr_int64_t line_no,
                                                    svn_revnum_t revision,
                                                    const char *author,
                                                    const char *date,
                                                    const char *line,
                                                    apr_pool_t *pool);

/* auth provider callbacks */
svn_error_t *svn_swig_py_auth_simple_prompt_func(
    svn_auth_cred_simple_t **cred,
    void *baton,
    const char *realm,
    const char *username,
    svn_boolean_t may_save,
    apr_pool_t *pool);

svn_error_t *svn_swig_py_auth_username_prompt_func(
    svn_auth_cred_username_t **cred,
    void *baton,
    const char *realm,
    svn_boolean_t may_save,
    apr_pool_t *pool);

svn_error_t *svn_swig_py_auth_ssl_server_trust_prompt_func(
    svn_auth_cred_ssl_server_trust_t **cred,
    void *baton,
    const char *realm,
    apr_uint32_t failures,
    const svn_auth_ssl_server_cert_info_t *cert_info,
    svn_boolean_t may_save,
    apr_pool_t *pool);

svn_error_t *svn_swig_py_auth_ssl_client_cert_prompt_func(
    svn_auth_cred_ssl_client_cert_t **cred,
    void *baton,
    const char *realm,
    svn_boolean_t may_save,
    apr_pool_t *pool);

svn_error_t *svn_swig_py_auth_ssl_client_cert_pw_prompt_func(
    svn_auth_cred_ssl_client_cert_pw_t **cred,
    void *baton,
    const char *realm,
    svn_boolean_t may_save,
    apr_pool_t *pool);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif  /* SVN_SWIG_SWIGUTIL_PY_H */
