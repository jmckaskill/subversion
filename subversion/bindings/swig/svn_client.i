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

%module _client
%include typemaps.i

%import apr.i
%import svn_types.i
%import svn_string.i
%import svn_delta.i

/* -----------------------------------------------------------------------
   don't wrap the following items
*/
%ignore svn_client_proplist_item_t;

/* -----------------------------------------------------------------------
   these types (as 'type **') will always be an OUT param
*/
%apply SWIGTYPE **OUTPARAM {
  svn_client_commit_info_t **
};

/* -----------------------------------------------------------------------
   all "targets" and "diff_options" arrays are constant inputs of
   svn_stringbuf_t *
 */
%apply const apr_array_header_t *STRINGLIST {
    const apr_array_header_t *targets,
    const apr_array_header_t *diff_options
};

/* -----------------------------------------------------------------------
   fix up the return hash for svn_client_propget()
*/
%apply apr_hash_t **PROPHASH { apr_hash_t **props };

/* -----------------------------------------------------------------------
   handle the return value for svn_client_proplist()
*/

%typemap(python,in,numinputs=0) apr_array_header_t ** (apr_array_header_t *temp) {
    $1 = &temp;
}
%typemap(python,argout,fragment="t_output_helper") apr_array_header_t ** {
    svn_client_proplist_item_t **ppitem;
    int i;
    int nelts = (*$1)->nelts;
    PyObject *list = PyList_New(nelts);
    if (list == NULL)
        return NULL;
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
            return NULL;
        }
        PyTuple_SET_ITEM(item, 0, name);
        PyTuple_SET_ITEM(item, 1, hash);

        PyList_SET_ITEM(list, i, item);
    }
    $result = t_output_helper($result, list);
}

/* -----------------------------------------------------------------------
   handle svn_wc_notify_func_t/baton pairs
*/

%typemap(python,in) (svn_wc_notify_func_t notify_func, void *notify_baton) {

  $1 = svn_swig_py_notify_func;
  $2 = $input; /* our function is the baton. */
}

%typemap(java,in) (svn_wc_notify_func_t notify_func, void *notify_baton) {

  $1 = svn_swig_java_notify_func;
  $2 = (void*)$input; /* our function is the baton. */
}

%typemap(jni) svn_wc_notify_func_t "jobject"
%typemap(jtype) svn_wc_notify_func_t "org.tigris.subversion.wc.Notifier"
%typemap(jstype) svn_wc_notify_func_t "org.tigris.subversion.wc.Notifier"
%typemap(javain) svn_wc_notify_func_t "$javainput"
%typemap(javaout) svn_wc_notify_func_t {
    return $jnicall;
  }

/* -----------------------------------------------------------------------
   handle svn_client_get_commit_log_t/baton pairs
*/

%typemap(python,in) (svn_client_get_commit_log_t log_msg_func, 
                     void *log_msg_baton) {

  $1 = svn_swig_py_get_commit_log_func;
  $2 = $input; /* our function is the baton. */
}

%typemap(java,in) (svn_client_get_commit_log_t log_msg_func, 
                   void *log_msg_baton) {

  $1 = svn_swig_java_get_commit_log_func;
  $2 = (void*)$input; /* our function is the baton. */
}

%typemap(jni) svn_client_get_commit_log_t "jobject"
%typemap(jtype) svn_client_get_commit_log_t "org.tigris.subversion.client.ClientPrompt"
%typemap(jstype) svn_client_get_commit_log_t "org.tigris.subversion.client.ClientPrompt"
%typemap(javain) svn_client_get_commit_log_t "$javainput"
%typemap(javaout) svn_client_get_commit_log_t {
    return $jnicall;
  }


/* -----------------------------------------------------------------------
   handle svn_client_get_commit_log_t/baton pairs
*/

%typemap(java,in) (svn_log_message_receiver_t receiver,
                void *receiver_baton) {

  $1 = svn_swig_java_log_message_receiver;
  $2 = (void*)$input; /* our function is the baton. */
}

%typemap(jni) svn_log_message_receiver_t "jobject"
%typemap(jtype) svn_log_message_receiver_t "org.tigris.subversion.client.LogMessageReceiver"
%typemap(jstype) svn_log_message_receiver_t "org.tigris.subversion.client.LogMessageReceiver"
%typemap(javain) svn_log_message_receiver_t "$javainput"
%typemap(javaout) svn_log_message_receiver_t {
    return $jnicall;
  }

/* -----------------------------------------------------------------------
   handle the "statushash" OUTPUT param for svn_client_status()
*/
%typemap(python,in,numinputs=0) apr_hash_t **statushash = apr_hash_t **OUTPUT;
%typemap(python,argout,fragment="t_output_helper") apr_hash_t **statushash {
    $result = t_output_helper(
        $result,
        svn_swig_py_convert_hash(*$1, SWIGTYPE_p_svn_wc_status_t));
}

/* -----------------------------------------------------------------------
   fix up the return hash for svn_client_ls() 
*/

%typemap(python,in,numinputs=0) apr_hash_t **dirents = apr_hash_t **OUTPUT;
%typemap(python,argout,fragment="t_output_helper") apr_hash_t **dirents {
	$result = t_output_helper(
		$result,
		svn_swig_py_convert_hash(*$1, SWIGTYPE_p_svn_dirent_t));
}

/* -----------------------------------------------------------------------
   handle the prompt_baton
*/

%typemap(jni) svn_log_message_receiver_t "jobject"
%typemap(jtype) svn_log_message_receiver_t "org.tigris.subversion.client.LogMessageReceiver"
%typemap(jstype) svn_log_message_receiver_t "org.tigris.subversion.client.LogMessageReceiver"
%typemap(javain) svn_log_message_receiver_t "$javainput"
%typemap(javaout) svn_log_message_receiver_t {
    return $jnicall;
  }

/* -----------------------------------------------------------------------
   We use 'svn_wc_status_t *' in some custom code, but it isn't in the
   API anywhere. Thus, SWIG doesn't generate a typemap entry for it. by
   adding a simple declaration here, SWIG will insert a name for it.
*/
%types(svn_wc_status_t *);

/* We also need SWIG to wrap svn_dirent_t for us.  It doesn't appear in
   any API, but svn_client_ls returns a hash of pointers to dirents. */
%types(svn_dirent_t *);

/* ----------------------------------------------------------------------- */

/* Include the headers before we swig-include the svn_client.h header file.
   SWIG will split the nested svn_client_revision_t structure, and we need
   the types declared *before* the split structure is encountered.  */

%header %{
#include "svn_client.h"

#ifdef SWIGPYTHON
#include "swigutil_py.h"
#endif

#ifdef SWIGJAVA
#include "swigutil_java.h"
#endif
%}

%include svn_client.h


