/*
 * svn_client.i :  SWIG interface file for svn_client.h
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

#if defined(SWIGPERL)
%module "SVN::_Client"
#elif defined(SWIGRUBY)
%module "svn::ext::client"
#else
%module client
#endif

%include typemaps.i

%include svn_global.swg
%import core.i
%import apr.swg
%import svn_types.swg
%import svn_string.swg
%import svn_delta.i
%import svn_wc.i

/* -----------------------------------------------------------------------
   %apply-ing of typemaps defined elsewhere
*/

%apply SWIGTYPE **OUTPARAM {
  svn_client_commit_info_t **,
  svn_auth_provider_object_t **,
  svn_client_ctx_t **
};

%apply const apr_array_header_t *STRINGLIST {
    const apr_array_header_t *targets,
    const apr_array_header_t *diff_options
};

%apply const char *MAY_BE_NULL {
    const char *native_eol,
    const char *comment
};

/* svn_client_propget(), svn_client_proplist(), svn_client_revprop_list() */
%apply apr_hash_t **PROPHASH { apr_hash_t **props };

/* svn_client_url_from_path(), svn_client_uuid_from_url()
 * svn_client_uuid_from_path */
%apply const char **OUTPUT {
    const char **url,
    const char **uuid
};

#ifdef SWIGPYTHON
%apply svn_stream_t *WRAPPED_STREAM { svn_stream_t * };
#endif

/* -----------------------------------------------------------------------
   svn_client_proplist()
   returns apr_array_header_t * <svn_client_proplist_item_t *>
*/

%typemap(in, numinputs=0) apr_array_header_t **props (apr_array_header_t *temp) {
    $1 = &temp;
}

/* svn_client_proplist_item_t is used exclusively for svn_client_proplist().
   The python bindings convert it to a native python tuple. */
#ifdef SWIGPYTHON
    %ignore svn_client_proplist_item_t;
#endif
%typemap(python, argout, fragment="t_output_helper") apr_array_header_t **props
{
    svn_client_proplist_item_t **ppitem;
    int i;
    int nelts = (*$1)->nelts;
    PyObject *list = PyList_New(nelts);
    if (list == NULL)
        SWIG_fail;
    ppitem = (svn_client_proplist_item_t **)(*$1)->elts;
    for (i = 0; i < nelts; ++i, ++ppitem) {
        PyObject *item = PyTuple_New(2);
        PyObject *name = PyString_FromStringAndSize((*ppitem)->node_name->data,
                                                    (*ppitem)->node_name->len);
        PyObject *hash = svn_swig_py_prophash_to_dict((*ppitem)->prop_hash);

        if (item == NULL || name == NULL || hash == NULL) {
            Py_XDECREF(item);
            Py_XDECREF(name);
            Py_XDECREF(hash);
            Py_DECREF(list);
            SWIG_fail;
        }
        PyTuple_SET_ITEM(item, 0, name);
        PyTuple_SET_ITEM(item, 1, hash);

        PyList_SET_ITEM(list, i, item);
    }
    $result = t_output_helper($result, list);
}

%typemap(ruby, argout) apr_array_header_t **props
{
  $result = svn_swig_rb_apr_array_to_array_proplist_item(*$1);
}

%typemap(ruby, out) apr_hash_t *prop_hash
{
  $result = svn_swig_rb_prop_hash_to_hash($1);
}

%typemap(perl5,argout) apr_array_header_t **props {
    $result = svn_swig_pl_convert_array(*$1,
      $descriptor(svn_client_proplist_item_t *));
    argvi++;
}

%typemap(perl5,out) apr_hash_t *prop_hash {
    $result = svn_swig_pl_prophash_to_hash($1);
    argvi++;
}

/* -----------------------------------------------------------------------
   Callback: svn_client_get_commit_log_t
   svn_client_ctx_t
*/

%typemap(python,in) (svn_client_get_commit_log_t log_msg_func, 
                     void *log_msg_baton) {

  $1 = svn_swig_py_get_commit_log_func;
  $2 = $input; /* our function is the baton. */
}

%typemap(ruby, in) svn_client_get_commit_log_t log_msg_func
{
  $1 = svn_swig_rb_get_commit_log_func;
}

/* -----------------------------------------------------------------------
   Callback: svn_cancel_func_t
   svn_client_ctx_t
*/

%typemap(ruby, in) svn_cancel_func_t cancel_func
{
  $1 = svn_swig_rb_cancel_func;
}

/* -----------------------------------------------------------------------
   Callback: svn_wc_notify_func2_t
   svn_client_ctx_t
*/

%typemap(ruby, in) svn_wc_notify_func2_t notify_func2
{
  $1 = svn_swig_rb_notify_func2;
}


/* -----------------------------------------------------------------------
   Callback: svn_client_blame_receiver_t
   svn_client_blame()
*/

%typemap(python, in) (svn_client_blame_receiver_t receiver, 
                      void *receiver_baton) {
    $1 = svn_swig_py_client_blame_receiver_func;
    $2 = (void *)$input;
}

%typemap(perl5, in) (svn_client_blame_receiver_t receiver,
                     void *receiver_baton) {
  $1 = svn_swig_pl_blame_func;
  $2 = $input; /* our function is the baton. */
}

%typemap(ruby, in) (svn_client_blame_receiver_t receiver,
                    void *receiver_baton)
{
  $1 = svn_swig_rb_client_blame_receiver_func;
  $2 = (void *)$input;
}

/* -----------------------------------------------------------------------
   We use 'svn_wc_status_t *' in some custom code, but it isn't in the
   API anywhere. Thus, SWIG doesn't generate a typemap entry for it. by
   adding a simple declaration here, SWIG will insert a name for it.
   FIXME: This may be untrue. See svn_wc_status, etc.
*/
%types(svn_wc_status_t *);

/* We also need SWIG to wrap svn_dirent_t and svn_lock_t for us.  They
   don't appear in any API, but svn_client_ls returns a hash of pointers
   to dirents and locks. */
%types(svn_dirent_t *);
%types(svn_lock_t *);

/* -----------------------------------------------------------------------
  thunk the various authentication prompt functions.
  PERL NOTE: store the inputed SV in _global_callback for use in the
             later argout typemap
*/
%typemap(perl5, in) (svn_auth_simple_prompt_func_t prompt_func,
                     void *prompt_baton) {
    $1 = svn_swig_pl_thunk_simple_prompt;
    _global_callback = $input;
    $2 = (void *) _global_callback;
}
%typemap(python, in) (svn_auth_simple_prompt_func_t prompt_func,
                      void *prompt_baton) {
    $1 = svn_swig_py_auth_simple_prompt_func;
    $2 = $input;
}
%typemap(ruby, in) (svn_auth_simple_prompt_func_t prompt_func,
                    void *prompt_baton) {
    $1 = svn_swig_rb_auth_simple_prompt_func;
    $2 = (void *) $input;
}

%typemap(perl5, in) (svn_auth_username_prompt_func_t prompt_func,
                     void *prompt_baton) {
    $1 = svn_swig_pl_thunk_username_prompt;
    _global_callback = $input;
    $2 = (void *) _global_callback;
}
%typemap(python, in) (svn_auth_username_prompt_func_t prompt_func,
                      void *prompt_baton) {
    $1 = svn_swig_py_auth_username_prompt_func;
    $2 = $input;
}
%typemap(ruby, in) (svn_auth_username_prompt_func_t prompt_func,
                      void *prompt_baton) {
    $1 = svn_swig_rb_auth_username_prompt_func;
    $2 = (void *) $input;
}

%typemap(perl5, in) (svn_auth_ssl_server_trust_prompt_func_t prompt_func,
                     void *prompt_baton) {
    $1 = svn_swig_pl_thunk_ssl_server_trust_prompt;
    _global_callback = $input;
    $2 = (void *) _global_callback;
}
%typemap(python, in) (svn_auth_ssl_server_trust_prompt_func_t prompt_func,
                     void *prompt_baton) {
    $1 = svn_swig_py_auth_ssl_server_trust_prompt_func;
    $2 = $input;
}
%typemap(ruby, in) (svn_auth_ssl_server_trust_prompt_func_t prompt_func,
                     void *prompt_baton) {
    $1 = svn_swig_rb_auth_ssl_server_trust_prompt_func;
    $2 = (void *) $input;
}

%typemap(perl5, in) (svn_auth_ssl_client_cert_prompt_func_t prompt_func,
                      void *prompt_baton) {
    $1 = svn_swig_pl_thunk_ssl_client_cert_prompt;
    _global_callback = $input;
    $2 = (void *) _global_callback;
}
%typemap(python, in) (svn_auth_ssl_client_cert_prompt_func_t prompt_func,
                      void *prompt_baton) {
    $1 = svn_swig_py_auth_ssl_client_cert_prompt_func;
    $2 = $input;
}
%typemap(ruby, in) (svn_auth_ssl_client_cert_prompt_func_t prompt_func,
                      void *prompt_baton) {
    $1 = svn_swig_rb_auth_ssl_client_cert_prompt_func;
    $2 = (void *) $input;
}

%typemap(perl5, in) (svn_auth_ssl_client_cert_pw_prompt_func_t prompt_func,
                      void *prompt_baton) {
    $1 = svn_swig_pl_thunk_ssl_client_cert_pw_prompt;
    _global_callback = $input;
    $2 = (void *) _global_callback;
}
%typemap(python, in) (svn_auth_ssl_client_cert_pw_prompt_func_t prompt_func,
                      void *prompt_baton) {
    $1 = svn_swig_py_auth_ssl_client_cert_pw_prompt_func;
    $2 = $input;
}
%typemap(ruby, in) (svn_auth_ssl_client_cert_pw_prompt_func_t prompt_func,
                      void *prompt_baton) {
    $1 = svn_swig_rb_auth_ssl_client_cert_pw_prompt_func;
    $2 = (void *) $input;
}

/* -----------------------------------------------------------------------
 * For all the various functions that set a callback baton create a reference
 * for the baton (which in this case is an SV pointing to the callback)
 * and make that a return from the function.  The perl side should
 * then store the return in the object the baton is attached to.
 * If the function already returns a value then this value is follows that
 * function.  In the case of the prompt functions auth_open_helper in Core.pm
 * is used to split up these values.
*/
%typemap(perl5, argout) void *CALLBACK_BATON (SV * _global_callback) {
  /* callback baton */
  $result = sv_2mortal (newRV_inc (_global_callback)); argvi++; }

%typemap(perl5, in) void *CALLBACK_BATON (SV * _global_callback) {
  _global_callback = $input;
  $1 = (void *) _global_callback;
}

#ifdef SWIGPERL
%apply void *CALLBACK_BATON {
  void *prompt_baton,
  void *notify_baton,
  void *log_msg_baton,
  void *cancel_baton
}
#endif

#ifdef SWIGRUBY
%apply void *CALLBACK_BATON
{
  void *notify_baton2,
  void *log_msg_baton,
  void *cancel_baton
}
#endif

/* ----------------------------------------------------------------------- 
 * Convert perl hashes back into apr_hash_t * for setting the config
 * member of the svn_client_ctx_t.   This is an ugly hack, it will
 * always allocate the new apr_hash_t out of the global current_pool
 * It would be better to make apr_hash_t's into magic variables in
 * perl that are tied to the apr_hash_t interface.  This would
 * remove the need to convert to and from perl hashs all the time.
 */
%typemap(perl5, in) apr_hash_t *config {
  $1 = svn_swig_pl_objs_to_hash_by_name ($input, "svn_config_t *",
                                         svn_swig_pl_make_pool ((SV *)NULL));
}

%typemap(perl5, out) apr_hash_t *config {
  $result = svn_swig_pl_convert_hash($1, 
    $descriptor(svn_config_t *));
  argvi++;
}

#ifdef SWIGRUBY
%runtime %{
  #include <apr.h>
  #include <apr_pools.h>

  static apr_pool_t *
  _svn_client_pool(void) 
  {
    static apr_pool_t *__svn_client_pool = NULL;
    if (!__svn_client_pool) {
      apr_pool_create(&__svn_client_pool, NULL);
    }
    return __svn_client_pool;
  }

  static apr_pool_t *
  _svn_client_config_pool(void) 
  {
    static apr_pool_t *__svn_client_config_pool = NULL;
    if (!__svn_client_config_pool) {
      apr_pool_create(&__svn_client_config_pool, _svn_client_pool());
    }
    return __svn_client_config_pool;
  }
%}
#endif

%typemap(ruby, argout) apr_hash_t *config {
  if ($1)
    apr_pool_clear(_svn_client_config_pool());
}

%typemap(ruby, in) apr_hash_t *config {
  $1 = svn_swig_rb_hash_to_apr_hash_swig_type($input, "svn_config_t *",
                                              _svn_client_config_pool());
}

%typemap(ruby, out) apr_hash_t *config {
  $result = svn_swig_rb_apr_hash_to_hash_swig_type($1, "svn_config_t *");
}

/* -----------------------------------------------------------------------
 * override default typemap for svn_client_commit_info_t for perl.  Some calls
 * never allocate and fill the commit_info struct.  This lets us return
 * undef for them.  Otherwise the object we pass back can cause crashes */
%typemap(perl5, in, numinputs=0) svn_client_commit_info_t ** 
                                 ( svn_client_commit_info_t * temp ) {
    temp = NULL;
    $1 = &temp;
}

%typemap(perl5, argout) svn_client_commit_info_t ** {
    if ($1 == NULL) {
        $result = &PL_sv_undef;
        argvi++;
    }  else {
        $result = sv_newmortal();
        SWIG_MakePtr($result, (void *)*$1,
                     $*1_descriptor, 0);
        argvi++;
    }
}

/* -----------------------------------------------------------------------
 * wcprop_changes member of svn_client_commit_info needs to be
 * converted back and forth from an array */

%typemap(perl5, out) apr_array_header_t *wcprop_changes {
    $result = svn_swig_pl_convert_array($1,
      $descriptor(svn_prop_t *));
    argvi++;
}

/* -----------------------------------------------------------------------
 * wrap svn_client_create_context */

%typemap(perl5,in,numinputs=0) svn_client_ctx_t ** (svn_client_ctx_t *temp) {
    $1 = &temp;
}

%typemap(perl5,argout) svn_client_ctx_t ** {
  (*$1)->notify_func = svn_swig_pl_notify_func;
  (*$1)->notify_baton = (void *) &PL_sv_undef;
  (*$1)->log_msg_func = svn_swig_pl_get_commit_log_func;
  (*$1)->log_msg_baton = (void *) &PL_sv_undef;
  (*$1)->cancel_func = svn_swig_pl_cancel_func;
  (*$1)->cancel_baton = (void *) &PL_sv_undef;
  $result = sv_newmortal();
  SWIG_MakePtr($result, (void *)*$1,
               $*1_descriptor, 0);
  argvi++;
}  

/* svn_client_update2 */
%typemap(ruby, in, numinputs=0) apr_array_header_t **result_revs (apr_array_header_t *temp)
{
  $1 = &temp;
}

%typemap(ruby, argout, fragment="output_helper") apr_array_header_t **result_revs
{
  $result = output_helper($result, svn_swig_rb_apr_array_to_array_svn_rev(*$1));
}

/* ----------------------------------------------------------------------- */

%{
#ifdef SWIGPYTHON
#include "swigutil_py.h"
#endif

#ifdef SWIGPERL
#include "swigutil_pl.h"
#endif

#ifdef SWIGRUBY
#include <apu.h>
#include <apr_xlate.h>
#include "swigutil_rb.h"
#endif
%}

%include svn_time_h.swg
%include svn_client_h.swg
