/*
 * swigutil_py.c: utility functions for the SWIG Python bindings
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


#include <Python.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_portable.h>
#include <apr_thread_proc.h>

#include "svn_string.h"
#include "svn_opt.h"
#include "svn_delta.h"

#include "swigutil_py.h"



/*** Manage the Global Interpreter Lock ***/

/* If both Python and APR have threads available, we can optimize ourselves
 * by releasing the global interpreter lock when we drop into our SVN calls.
 *
 * In svn_types.i, svn_swig_py_release_py_lock is called before every
 * function, then svn_swig_py_acquire_py_lock is called after every
 * function.  So, if these functions become no-ops, then Python will
 * start to block...
 *
 * The Subversion libraries can be assumed to be thread-safe *only* when
 * APR_HAS_THREAD is 1.  The APR pool allocations aren't thread-safe unless
 * APR_HAS_THREAD is 1.
 */

#if defined(WITH_THREAD) && APR_HAS_THREADS
#define ACQUIRE_PYTHON_LOCK
#endif

#ifdef ACQUIRE_PYTHON_LOCK
static apr_threadkey_t *_saved_thread_key = NULL;
static apr_pool_t *_saved_thread_pool = NULL;
#endif

void svn_swig_py_release_py_lock(void)
{
#ifdef ACQUIRE_PYTHON_LOCK
  PyThreadState *thread_state;

  if (_saved_thread_key == NULL) {

    /* This is ugly.  We call svn_swig_py_release_py_lock before executing any
       subversion function.  Thus it gets called before any call to
       apr_initialize in our script.  This means we have to call
       apr_initialize ourselves, or otherwise we won't be able to
       create our pool. */
    apr_initialize();
    
    /* Obviously, creating a top-level pool for this is pretty stupid. */
    apr_pool_create(&_saved_thread_pool, NULL);
    apr_threadkey_private_create(&_saved_thread_key, NULL, _saved_thread_pool);
  }

  thread_state = PyEval_SaveThread();
  apr_threadkey_private_set(thread_state, _saved_thread_key);
#endif
}

void svn_swig_py_acquire_py_lock(void)
{
#ifdef ACQUIRE_PYTHON_LOCK
  void *val;
  PyThreadState *thread_state;
  apr_threadkey_private_get(&val, _saved_thread_key);
  thread_state = val;
  PyEval_RestoreThread(thread_state);
#endif
}



/*** Custom SubversionException stuffs. ***/

/* Global SubversionException class object. */
static PyObject *SubversionException = NULL;


PyObject *svn_swig_py_exception_type(void)
{
  Py_INCREF(SubversionException);
  return SubversionException;
}

PyObject *svn_swig_py_register_exception(void)
{
  /* If we haven't created our exception class, do so. */
  if (SubversionException == NULL)
    {
      SubversionException = PyErr_NewException
        ((char *)"libsvn._core.SubversionException", NULL, NULL);
    }

  /* Regardless, return the exception class. */
  return svn_swig_py_exception_type();
}

void svn_swig_py_svn_exception(svn_error_t *err)
{ 
  PyObject *exc_ob, *apr_err_ob;

  if (err == NULL)
    return;

  /* Make an integer for the error code. */
  apr_err_ob = PyInt_FromLong(err->apr_err);
  if (apr_err_ob == NULL)
    return;

  /* Instantiate a SubversionException object. */
  exc_ob = PyObject_CallFunction(SubversionException, (char *)"sO", 
                                 err->message, apr_err_ob);
  if (exc_ob == NULL)
    {
      Py_DECREF(apr_err_ob);
      return;
    }

  /* Set the "apr_err" attribute of the exception to our error code. */
  if (PyObject_SetAttrString(exc_ob, (char *)"apr_err", apr_err_ob) == -1)
    {
      Py_DECREF(apr_err_ob);
      Py_DECREF(exc_ob);
      return;
    }

  /* Finished with the apr_err object. */
  Py_DECREF(apr_err_ob);

  /* Set the error state to our exception object. */
  PyErr_SetObject(SubversionException, exc_ob);

  /* Finished with the exc_ob object. */
  Py_DECREF(exc_ob);
}



/*** Helper/Conversion Routines ***/

static PyObject *make_pointer(const char *typename, void *ptr)
{
  /* ### cache the swig_type_info at some point? */
  return SWIG_NewPointerObj(ptr, SWIG_TypeQuery(typename), 0);
}

/* for use by the "O&" format specifier */
static PyObject *make_ob_pool(void *ptr)
{
  return make_pointer("apr_pool_t *", ptr);
}

/* for use by the "O&" format specifier */
static PyObject *make_ob_window(void *ptr)
{
  return make_pointer("svn_txdelta_window_t *", ptr);
}

/* for use by the "O&" format specifier */
static PyObject *make_ob_status(void *ptr)
{
  return make_pointer("svn_wc_status_t *", ptr);
} 

/* for use by the "O&" format specifier */
static PyObject *make_ob_fs_root(void *ptr)
{
  return make_pointer("svn_fs_root_t *", ptr);
} 


static PyObject *convert_hash(apr_hash_t *hash,
                              PyObject * (*converter_func)(void *value,
                                                           void *ctx),
                              void *ctx)
{
    apr_hash_index_t *hi;
    PyObject *dict = PyDict_New();

    if (dict == NULL)
        return NULL;

    for (hi = apr_hash_first(NULL, hash); hi; hi = apr_hash_next(hi)) {
        const void *key;
        void *val;
        PyObject *value;

        apr_hash_this(hi, &key, NULL, &val);
        value = (*converter_func)(val, ctx);
        if (value == NULL) {
            Py_DECREF(dict);
            return NULL;
        }
        /* ### gotta cast this thing cuz Python doesn't use "const" */
        if (PyDict_SetItemString(dict, (char *)key, value) == -1) {
            Py_DECREF(value);
            Py_DECREF(dict);
            return NULL;
        }
        Py_DECREF(value);
    }

    return dict;
}

static PyObject *convert_to_swigtype(void *value, void *ctx)
{
  /* ctx is a 'swig_type_info *' */
  return SWIG_NewPointerObj(value, ctx, 0);
}

static PyObject *convert_svn_string_t(void *value, void *ctx)
{
  /* ctx is unused */

  const svn_string_t *s = value;

  /* ### borrowing from value in the pool. or should we copy? note
     ### that copying is "safest" */

  /* ### gotta cast this thing cuz Python doesn't use "const" */
  return PyBuffer_FromMemory((void *)s->data, s->len);
}

static PyObject *convert_svn_client_commit_item_t(void *value, void *ctx)
{
  PyObject *list;
  PyObject *path, *kind, *url, *rev, *cf_url, *state;
  svn_client_commit_item_t *item = value;

  /* ctx is unused */

  list = PyList_New(6);

  if (item->path)
    path = PyString_FromString(item->path);
  else
    {
      path = Py_None;
      Py_INCREF(Py_None);
    }

  if (item->url)
    url = PyString_FromString(item->url);
  else
    {
      url = Py_None;
      Py_INCREF(Py_None);
    }
        
  if (item->copyfrom_url)
    cf_url = PyString_FromString(item->copyfrom_url);
  else
    {
      cf_url = Py_None;
      Py_INCREF(Py_None);
    }
        
  kind = PyInt_FromLong(item->kind);
  rev = PyInt_FromLong(item->revision);
  state = PyInt_FromLong(item->state_flags);

  if (! (list && path && kind && url && rev && cf_url && state))
    {
      Py_XDECREF(list);
      Py_XDECREF(path);
      Py_XDECREF(kind);
      Py_XDECREF(url);
      Py_XDECREF(rev);
      Py_XDECREF(cf_url);
      Py_XDECREF(state);
      return NULL;
    }

  PyList_SET_ITEM(list, 0, path);
  PyList_SET_ITEM(list, 1, kind);
  PyList_SET_ITEM(list, 2, url);
  PyList_SET_ITEM(list, 3, rev);
  PyList_SET_ITEM(list, 4, cf_url);
  PyList_SET_ITEM(list, 5, state);
  return list;
}

PyObject *svn_swig_py_prophash_to_dict(apr_hash_t *hash)
{
  return convert_hash(hash, convert_svn_string_t, NULL);
}

PyObject *svn_swig_py_convert_hash(apr_hash_t *hash, swig_type_info *type)
{
  return convert_hash(hash, convert_to_swigtype, type);
}

PyObject *svn_swig_py_c_strings_to_list(char **strings)
{
    PyObject *list = PyList_New(0);
    char *s;

    while ((s = *strings++) != NULL) {
        PyObject *ob = PyString_FromString(s);

        if (ob == NULL)
            goto error;
        if (PyList_Append(list, ob) == -1)
            goto error;
    }

    return list;

  error:
    Py_DECREF(list);
    return NULL;
}

const apr_array_header_t *svn_swig_py_strings_to_array(PyObject *source,
                                                       apr_pool_t *pool)
{
    int targlen;
    apr_array_header_t *temp;

    if (!PySequence_Check(source)) {
        PyErr_SetString(PyExc_TypeError, "not a sequence");
        return NULL;
    }
    targlen = PySequence_Length(source);
    temp = apr_array_make(pool, targlen, sizeof(const char *));
    /* APR_ARRAY_IDX doesn't actually increment the array item count
       (like, say, apr_array_push would). */
    temp->nelts = targlen;
    while (targlen--) {
        PyObject *o = PySequence_GetItem(source, targlen);
        if (o == NULL)
            return NULL;
        if (!PyString_Check(o)) {
            Py_DECREF(o);
            PyErr_SetString(PyExc_TypeError, "not a string");
            return NULL;
        }
        APR_ARRAY_IDX(temp, targlen, const char *) = PyString_AS_STRING(o);
        Py_DECREF(o);
    }
    return temp;
}


/*** apr_array_header_t conversions.  To create a new type of
     converter, simply copy-n-paste one of these function and tweak
     the creation of the PyObject *ob.  ***/

PyObject *svn_swig_py_array_to_list(const apr_array_header_t *array)
{
    PyObject *list = PyList_New(array->nelts);
    int i;

    for (i = 0; i < array->nelts; ++i) {
        PyObject *ob = 
          PyString_FromString(APR_ARRAY_IDX(array, i, const char *));
        if (ob == NULL)
          goto error;
        PyList_SET_ITEM(list, i, ob);
    }
    return list;

  error:
    Py_DECREF(list);
    return NULL;
}

PyObject *svn_swig_py_revarray_to_list(const apr_array_header_t *array)
{
    PyObject *list = PyList_New(array->nelts);
    int i;

    for (i = 0; i < array->nelts; ++i) {
        PyObject *ob 
          = PyInt_FromLong(APR_ARRAY_IDX(array, i, svn_revnum_t));
        if (ob == NULL)
          goto error;
        PyList_SET_ITEM(list, i, ob);
    }
    return list;

  error:
    Py_DECREF(list);
    return NULL;
}

static PyObject *
commit_item_array_to_list(const apr_array_header_t *array)
{
    PyObject *list = PyList_New(array->nelts);
    int i;

    for (i = 0; i < array->nelts; ++i) {
        PyObject *ob = convert_svn_client_commit_item_t
          (APR_ARRAY_IDX(array, i, svn_client_commit_item_t *), NULL);
        if (ob == NULL)
          goto error;
        PyList_SET_ITEM(list, i, ob);
    }
    return list;

  error:
    Py_DECREF(list);
    return NULL;
}



/*** Callback Errors ***/

/* Return a Subversion error about a failed callback. */
static svn_error_t *callback_exception_error(void)
{
  return svn_error_create(SVN_ERR_SWIG_PY_EXCEPTION_SET, NULL,
                          "Python callback raised an exception");
}

/* Raise a TypeError exception with MESSAGE, and return a Subversion
   error about an invalid return from a callback. */
static svn_error_t *callback_bad_return_error(const char *message)
{
  PyErr_SetString(PyExc_TypeError, message);
  return svn_error_create(APR_EGENERAL, NULL,
                          "Python callback returned an invalid object");
}



/*** Editor Wrapping ***/

/* this baton is used for the editor, directory, and file batons. */
typedef struct {
  PyObject *editor;     /* the editor handling the callbacks */
  PyObject *baton;      /* the dir/file baton (or NULL for edit baton) */
} item_baton;

static item_baton *make_baton(apr_pool_t *pool, 
                              PyObject *editor, 
                              PyObject *baton)
{
  item_baton *newb = apr_palloc(pool, sizeof(*newb));

  /* one more reference to the editor. */
  Py_INCREF(editor);

  /* note: we take the caller's reference to 'baton' */

  newb->editor = editor;
  newb->baton = baton;

  return newb;
}

static svn_error_t *close_baton(void *baton, 
                                const char *method)
{
  item_baton *ib = baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* If there is no baton object, then it is an edit_baton, and we should
     not bother to pass an object. Note that we still shove a NULL onto
     the stack, but the format specified just won't reference it.  */
  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)method,
                                    ib->baton ? (char *)"(O)" : NULL,
                                    ib->baton)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* there is no return value, so just toss this object (probably Py_None) */
  Py_DECREF(result);

  /* We're now done with the baton. Since there isn't really a free, all
     we need to do is note that its objects are no longer referenced by
     the baton.  */
  Py_DECREF(ib->editor);
  Py_XDECREF(ib->baton);

#ifdef SVN_DEBUG
  ib->editor = ib->baton = NULL;
#endif

  err = SVN_NO_ERROR;

 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *set_target_revision(void *edit_baton,
                                        svn_revnum_t target_revision,
                                        apr_pool_t *pool)
{
  item_baton *ib = edit_baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"set_target_revision",
                                    (char *)"l", target_revision)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* there is no return value, so just toss this object (probably Py_None) */
  Py_DECREF(result);
  err = SVN_NO_ERROR;
  
 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *open_root(void *edit_baton,
                              svn_revnum_t base_revision,
                              apr_pool_t *dir_pool,
                              void **root_baton)
{
  item_baton *ib = edit_baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"open_root",
                                    (char *)"lO&", base_revision,
                                    make_ob_pool, dir_pool)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* make_baton takes our 'result' reference */
  *root_baton = make_baton(dir_pool, ib->editor, result);
  err = SVN_NO_ERROR;

 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *delete_entry(const char *path,
                                 svn_revnum_t revision,
                                 void *parent_baton,
                                 apr_pool_t *pool)
{
  item_baton *ib = parent_baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"delete_entry",
                                    (char *)"slOO&", path, revision, ib->baton,
                                    make_ob_pool, pool)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* there is no return value, so just toss this object (probably Py_None) */
  Py_DECREF(result);
  err = SVN_NO_ERROR;

 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *add_directory(const char *path,
                                  void *parent_baton,
                                  const char *copyfrom_path,
                                  svn_revnum_t copyfrom_revision,
                                  apr_pool_t *dir_pool,
                                  void **child_baton)
{
  item_baton *ib = parent_baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"add_directory",
                                    (char *)"sOslO&", path, ib->baton,
                                    copyfrom_path, copyfrom_revision,
                                    make_ob_pool, dir_pool)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* make_baton takes our 'result' reference */
  *child_baton = make_baton(dir_pool, ib->editor, result);
  err = SVN_NO_ERROR;

 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *open_directory(const char *path,
                                   void *parent_baton,
                                   svn_revnum_t base_revision,
                                   apr_pool_t *dir_pool,
                                   void **child_baton)
{
  item_baton *ib = parent_baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"open_directory",
                                    (char *)"sOlO&", path, ib->baton,
                                    base_revision,
                                    make_ob_pool, dir_pool)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* make_baton takes our 'result' reference */
  *child_baton = make_baton(dir_pool, ib->editor, result);
  err = SVN_NO_ERROR;
  
 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *change_dir_prop(void *dir_baton,
                                    const char *name,
                                    const svn_string_t *value,
                                    apr_pool_t *pool)
{
  item_baton *ib = dir_baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"change_dir_prop",
                                    (char *)"Oss#O&", ib->baton, name,
                                    value ? value->data : NULL, 
                                    value ? value->len : 0,
                                    make_ob_pool, pool)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* there is no return value, so just toss this object (probably Py_None) */
  Py_DECREF(result);
  err = SVN_NO_ERROR;

 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *close_directory(void *dir_baton,
                                    apr_pool_t *pool)
{
  return close_baton(dir_baton, "close_directory");
}

static svn_error_t *add_file(const char *path,
                             void *parent_baton,
                             const char *copyfrom_path,
                             svn_revnum_t copyfrom_revision,
                             apr_pool_t *file_pool,
                             void **file_baton)
{
  item_baton *ib = parent_baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"add_file",
                                    (char *)"sOslO&", path, ib->baton,
                                    copyfrom_path, copyfrom_revision,
                                    make_ob_pool, file_pool)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* make_baton takes our 'result' reference */
  *file_baton = make_baton(file_pool, ib->editor, result);

  err = SVN_NO_ERROR;

 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *open_file(const char *path,
                              void *parent_baton,
                              svn_revnum_t base_revision,
                              apr_pool_t *file_pool,
                              void **file_baton)
{
  item_baton *ib = parent_baton;
  PyObject *result;
  svn_error_t *err;
  
  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"open_file",
                                    (char *)"sOlO&", path, ib->baton,
                                    base_revision,
                                    make_ob_pool, file_pool)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* make_baton takes our 'result' reference */
  *file_baton = make_baton(file_pool, ib->editor, result);
  err = SVN_NO_ERROR;

 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *window_handler(svn_txdelta_window_t *window,
                                   void *baton)
{
  PyObject *handler = baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  if (window == NULL)
    {
      /* the last call; it closes the handler */

      /* invoke the handler with None for the window */
      /* ### python doesn't have 'const' on the format */
      result = PyObject_CallFunction(handler, (char *)"O", Py_None);

      /* we no longer need to refer to the handler object */
      Py_DECREF(handler);
    }
  else
    {
      /* invoke the handler with the window */
      /* ### python doesn't have 'const' on the format */
      result = PyObject_CallFunction(handler,
                                     (char *)"O&", make_ob_window, window);
    }

  if (result == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* there is no return value, so just toss this object (probably Py_None) */
  Py_DECREF(result);
  err = SVN_NO_ERROR;

 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *apply_textdelta(void *file_baton, 
                                    const char *base_checksum,
                                    apr_pool_t *pool,
                                    svn_txdelta_window_handler_t *handler,
                                    void **h_baton)
{
  item_baton *ib = file_baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"apply_textdelta",
                                    (char *)"(Os)", ib->baton,
                                    base_checksum)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* Interpret None to mean svn_delta_noop_window_handler. This is much
     easier/faster than making code always have to write a NOOP handler
     in Python.  */
  if (result == Py_None)
    {
      Py_DECREF(result);

      *handler = svn_delta_noop_window_handler;
      *h_baton = NULL;
    }
  else
    {
      /* return the thunk for invoking the handler. the baton takes our
         'result' reference, which is the handler. */
      *handler = window_handler;
      *h_baton = result;
    }

  err = SVN_NO_ERROR;

 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *change_file_prop(void *file_baton,
                                     const char *name,
                                     const svn_string_t *value,
                                     apr_pool_t *pool)
{
  item_baton *ib = file_baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"change_file_prop",
                                    (char *)"Oss#O&", ib->baton, name,
                                    value ? value->data : NULL, 
                                    value ? value->len : 0,
                                    make_ob_pool, pool)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* there is no return value, so just toss this object (probably Py_None) */
  Py_DECREF(result);
  err = SVN_NO_ERROR;
  
 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *close_file(void *file_baton,
                               const char *text_checksum,
                               apr_pool_t *pool)
{
  item_baton *ib = file_baton;
  PyObject *result;
  svn_error_t *err;

  svn_swig_py_acquire_py_lock();

  /* ### python doesn't have 'const' on the method name and format */
  if ((result = PyObject_CallMethod(ib->editor, (char *)"close_file",
                                    (char *)"(Os)", ib->baton,
                                    text_checksum)) == NULL)
    {
      err = callback_exception_error();
      goto finished;
    }

  /* there is no return value, so just toss this object (probably Py_None) */
  Py_DECREF(result);

  /* We're now done with the baton. Since there isn't really a free, all
     we need to do is note that its objects are no longer referenced by
     the baton.  */
  Py_DECREF(ib->editor);
  Py_XDECREF(ib->baton);

#ifdef SVN_DEBUG
  ib->editor = ib->baton = NULL;
#endif

  err = SVN_NO_ERROR;

 finished:
  svn_swig_py_release_py_lock();
  return err;
}

static svn_error_t *close_edit(void *edit_baton,
                               apr_pool_t *pool)
{
  return close_baton(edit_baton, "close_edit");
}

static svn_error_t *abort_edit(void *edit_baton,
                               apr_pool_t *pool)
{
  return close_baton(edit_baton, "abort_edit");
}

void svn_swig_py_make_editor(const svn_delta_editor_t **editor,
                             void **edit_baton,
                             PyObject *py_editor,
                             apr_pool_t *pool)
{
  svn_delta_editor_t *thunk_editor = svn_delta_default_editor (pool);
  
  thunk_editor->set_target_revision = set_target_revision;
  thunk_editor->open_root = open_root;
  thunk_editor->delete_entry = delete_entry;
  thunk_editor->add_directory = add_directory;
  thunk_editor->open_directory = open_directory;
  thunk_editor->change_dir_prop = change_dir_prop;
  thunk_editor->close_directory = close_directory;
  thunk_editor->add_file = add_file;
  thunk_editor->open_file = open_file;
  thunk_editor->apply_textdelta = apply_textdelta;
  thunk_editor->change_file_prop = change_file_prop;
  thunk_editor->close_file = close_file;
  thunk_editor->close_edit = close_edit;
  thunk_editor->abort_edit = abort_edit;

  *editor = thunk_editor;
  *edit_baton = make_baton(pool, py_editor, NULL);
}



/*** Other Wrappers for SVN Functions ***/


/* This is very hacky and gross.  */
apr_file_t *svn_swig_py_make_file (PyObject *py_file,
                                   apr_pool_t *pool)
{
  apr_file_t *apr_file = NULL;

  if (py_file == NULL || py_file == Py_None)
    return NULL;

  if (PyString_Check(py_file))
    {
      /* input is a path -- just open an apr_file_t */
      apr_file_open(&apr_file, PyString_AS_STRING(py_file),
                    APR_CREATE | APR_READ | APR_WRITE,
                    APR_OS_DEFAULT,
                    pool);
    }
  else if (PyFile_Check (py_file))
    {
      apr_status_t status;
      FILE *file;
      apr_os_file_t osfile;

      /* input is a file object -- convert to apr_file_t */
      file = PyFile_AsFile(py_file);
#ifdef WIN32
      osfile = (apr_os_file_t)_get_osfhandle(_fileno(file));
#else
      osfile = (apr_os_file_t)fileno(file);
#endif
      status = apr_os_file_put (&apr_file, &osfile, O_CREAT | O_WRONLY, pool);
      if (status)
        return NULL;
    }
  return apr_file;
}


void svn_swig_py_notify_func(void *baton,
                             const char *path,
                             svn_wc_notify_action_t action,
                             svn_node_kind_t kind,
                             const char *mime_type,
                             svn_wc_notify_state_t content_state,
                             svn_wc_notify_state_t prop_state,
                             svn_revnum_t revision)
{
  PyObject *function = baton;
  PyObject *result;
  svn_error_t *err = SVN_NO_ERROR;

  if (function == NULL || function == Py_None)
    return;

  svn_swig_py_acquire_py_lock();
  if ((result = PyObject_CallFunction(function, 
                                      (char *)"(siisiii)", 
                                      path, action, kind,
                                      mime_type,
                                      content_state, prop_state, 
                                      revision)) == NULL)
    {
      err = callback_exception_error();
    }
  else
    {
      /* The callback shouldn't be returning anything. */
      if (result != Py_None)
        err = callback_bad_return_error("Not None");
      Py_DECREF(result);
    }

  /* Our error has no place to go. :-( */
  if (err)
    svn_error_clear(err);

  svn_swig_py_release_py_lock();
}


void svn_swig_py_status_func(void *baton,
                             const char *path,
                             svn_wc_status_t *status)
{
  PyObject *function = baton;
  PyObject *result;
  svn_error_t *err = SVN_NO_ERROR;

  if (function == NULL || function == Py_None)
    return;

  svn_swig_py_acquire_py_lock();
  if ((result = PyObject_CallFunction(function, (char *)"sO&", path,
                                      make_ob_status, status)) == NULL)
    {
      err = callback_exception_error();
    }
  else
    {
      /* The callback shouldn't be returning anything. */
      if (result != Py_None)
        err = callback_bad_return_error("Not None");
      Py_DECREF(result);
    }

  /* Our error has no place to go. :-( */
  if (err)
    svn_error_clear(err);
    
  svn_swig_py_release_py_lock();
}


svn_error_t *svn_swig_py_cancel_func(void *cancel_baton)
{
  PyObject *function = cancel_baton;
  PyObject *result;
  svn_error_t *err = SVN_NO_ERROR;

  if (function == NULL || function == Py_None)
    return SVN_NO_ERROR;

  svn_swig_py_acquire_py_lock();
  if ((result = PyObject_CallFunction(function, NULL)) == NULL)
    {
      err = callback_exception_error();
    }
  else
    {
      if (PyInt_Check(result))
        {
          if (PyInt_AsLong(result))
            err = svn_error_create(SVN_ERR_CANCELLED, 0, NULL);
        }
      else if (PyLong_Check(result))
        {
          if (PyLong_AsLong(result))
            err = svn_error_create(SVN_ERR_CANCELLED, 0, NULL);
        }
      else if (result != Py_None)
        {
          err = callback_bad_return_error("Not an integer or None");
        }
      Py_DECREF(result);
    }
  svn_swig_py_release_py_lock();
  return err;
}


svn_error_t *svn_swig_py_get_commit_log_func(const char **log_msg,
                                             const char **tmp_file,
                                             apr_array_header_t *commit_items,
                                             void *baton,
                                             apr_pool_t *pool)
{
  PyObject *function = baton;
  PyObject *result;
  PyObject *cmt_items;
  svn_error_t *err;

  *log_msg = NULL;
  *tmp_file = NULL;

  /* ### todo: for now, just ignore the whole tmp_file thing.  */

  if ((function == NULL) || (function == Py_None))
    return SVN_NO_ERROR;

  svn_swig_py_acquire_py_lock();

  if (commit_items)
    {
      cmt_items = commit_item_array_to_list(commit_items);
    }
  else
    {
      cmt_items = Py_None;
      Py_INCREF(Py_None);
    }

  if ((result = PyObject_CallFunction(function, 
                                      (char *)"OO&",
                                      cmt_items,
                                      make_ob_pool, pool)) == NULL)
    {
      Py_DECREF(cmt_items);
      err = callback_exception_error();
      goto finished;
    }

  Py_DECREF(cmt_items);

  if (result == Py_None)
    {
      Py_DECREF(result);
      *log_msg = NULL;
      err = SVN_NO_ERROR;
    }
  else if (PyString_Check(result)) 
    {
      *log_msg = apr_pstrdup(pool, PyString_AS_STRING(result));
      Py_DECREF(result);
      err = SVN_NO_ERROR;
    }
  else
    {
      Py_DECREF(result);
      err = callback_bad_return_error("Not a string");
    }

 finished:
  svn_swig_py_release_py_lock();
  return err;
}


svn_error_t *svn_swig_py_repos_authz_func(svn_boolean_t *allowed,
                                          svn_fs_root_t *root,
                                          const char *path,
                                          void *baton,
                                          apr_pool_t *pool)
{
  PyObject *function = baton;
  PyObject *result;
  svn_error_t *err = SVN_NO_ERROR;

  if (function == NULL || function == Py_None)
    return SVN_NO_ERROR;

  *allowed = TRUE;

  svn_swig_py_acquire_py_lock();
  if ((result = PyObject_CallFunction(function, 
                                      (char *)"O&sO&", 
                                      make_ob_fs_root, root,
                                      path, make_ob_pool, pool)) == NULL)
    {
      err = callback_exception_error();
    }
  else
    {
      if (PyInt_Check(result))
        *allowed = PyInt_AsLong(result);
      else if (PyLong_Check(result))
        *allowed = PyLong_AsLong(result);
      else
        err = callback_bad_return_error("Not an integer");
      Py_DECREF(result);
    }
  svn_swig_py_release_py_lock();
  return err;
}


svn_error_t *svn_swig_py_repos_history_func(void *baton,
                                            const char *path,
                                            svn_revnum_t revision,
                                            apr_pool_t *pool)
{
  PyObject *function = baton;
  PyObject *result;
  svn_error_t *err = SVN_NO_ERROR;

  if (function == NULL || function == Py_None)
    return SVN_NO_ERROR;

  svn_swig_py_acquire_py_lock();
  if ((result = PyObject_CallFunction(function, 
                                      (char *)"slO&", 
                                      path, revision, 
                                      make_ob_pool, pool)) == NULL)
    {
      err = callback_exception_error();
    }
  else
    {
      if (result != Py_None)
        err = callback_bad_return_error("Not None");
      Py_DECREF(result);
    }
  svn_swig_py_release_py_lock();
  return err;
}


svn_error_t *svn_swig_py_log_receiver(void *baton,
                                      apr_hash_t *changed_paths,
                                      svn_revnum_t rev,
                                      const char *author,
                                      const char *date,
                                      const char *msg,
                                      apr_pool_t *pool)
{
  PyObject *receiver = baton;
  PyObject *result;
  swig_type_info *tinfo = SWIG_TypeQuery("svn_log_changed_path_t *");
  PyObject *chpaths;
  svn_error_t *err = SVN_NO_ERROR;
 
  if ((receiver == NULL) || (receiver == Py_None))
    return SVN_NO_ERROR;

  svn_swig_py_acquire_py_lock();

  if (changed_paths)
    {
      chpaths = svn_swig_py_convert_hash (changed_paths, tinfo);
    }
  else
    {
      chpaths = Py_None;
      Py_INCREF(Py_None);
    }

  if ((result = PyObject_CallFunction(receiver, 
                                      (char *)"OlsssO&", 
                                      chpaths, rev, author, date, msg, 
                                      make_ob_pool, pool)) == NULL)
    {
      err = callback_exception_error();
    }
  else
    {
      if (result != Py_None)
        err = callback_bad_return_error("Not None");
      Py_DECREF(result);
    }

  Py_DECREF(chpaths);
  svn_swig_py_release_py_lock();
  return err;
}
