/*
 * swigutil_java.c: utility functions for the SWIG Java bindings
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


#include <jni.h>

#include <apr_pools.h>
#include <apr_hash.h>


#include "svn_client.h"

#include "svn_string.h"
#include "svn_delta.h"

#include "swigutil_java.h"

/* FIXME: Need java.swg for the JCALL macros.  The following was taken
   from javahead.swg (which is included by java.swg). */
#ifndef JCALL0
#ifdef __cplusplus
#   define JCALL0(func, jenv) jenv->func()
#   define JCALL1(func, jenv, ar1) jenv->func(ar1)
#   define JCALL2(func, jenv, ar1, ar2) jenv->func(ar1, ar2)
#   define JCALL3(func, jenv, ar1, ar2, ar3) jenv->func(ar1, ar2, ar3)
#   define JCALL4(func, jenv, ar1, ar2, ar3, ar4) jenv->func(ar1, ar2, ar3, ar4)
#else
#   define JCALL0(func, jenv) (*jenv)->func(jenv)
#   define JCALL1(func, jenv, ar1) (*jenv)->func(jenv, ar1)
#   define JCALL2(func, jenv, ar1, ar2) (*jenv)->func(jenv, ar1, ar2)
#   define JCALL3(func, jenv, ar1, ar2, ar3) (*jenv)->func(jenv, ar1, ar2, ar3)
#   define JCALL4(func, jenv, ar1, ar2, ar3, ar4) (*jenv)->func(jenv, ar1, ar2, ar3, ar4)
#endif
#endif

/* this baton is used for the editor, directory, and file batons. */
typedef struct {
  jobject editor;       /* the editor handling the callbacks */
  jobject baton;        /* the dir/file baton (or NULL for edit baton) */
  apr_pool_t *pool;     /* pool to use for errors */
  JNIEnv *jenv;         /* Java native interface structure */
} item_baton;

typedef struct {
  jobject handler;      /* the window handler (a callable) */
  apr_pool_t *pool;     /* a pool for constructing errors */
  JNIEnv *jenv;         /* Java native interface structure */
} handler_baton;

static jobject make_pointer(JNIEnv* env, void *ptr)
{
  /* Return a Long object contining the C pointer to the object
     (SWIG/Java knows nothing of SWIG_NewPointerObj) */
  jclass cls = JCALL1(FindClass, env, "java/lang/Long");
  return JCALL3(NewObject, env, cls,
                JCALL3(GetMethodID, env, cls, "<init>", "(J)V"), (jlong) ptr);
}

static jobject convert_hash(JNIEnv* jenv, apr_hash_t *hash,
                            jobject (*converter_func)(JNIEnv* env,
                                                      void *value,
                                                      void *ctx),
                            void *ctx)
{
  apr_hash_index_t *hi;
  jclass cls = JCALL1(FindClass, jenv, "java/util/HashMap");
  jobject dict = JCALL3(NewObject, jenv, cls,
                        JCALL3(GetMethodID, jenv, cls, "<init>", "(I)V"),
                        (jint) apr_hash_count(hash));
  jmethodID put = JCALL3(GetMethodID, jenv, cls, "put",
                         "(Ljava/lang/Object;Ljava/lang/Object;)"
                         "Ljava/lang/Object;");

  if (dict == NULL)
    return NULL;

  for (hi = apr_hash_first(NULL, hash); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      void *val;
      jobject value;

      apr_hash_this(hi, &key, NULL, &val);
      value = (*converter_func)(jenv, val, ctx);
      JCALL4(CallObjectMethod, jenv, dict, put,
              JCALL1(NewStringUTF, jenv, key), value);
      JCALL1(DeleteLocalRef, jenv, value);
    }

  return dict;
}

void svn_swig_java_add_to_list(JNIEnv* jenv, apr_array_header_t *array,
                               jobject list)
{
  /* TODO: This impl will be much like svn_swig_java_add_to_map */
}

void svn_swig_java_add_to_map(JNIEnv* jenv, apr_hash_t *hash, jobject map)
{
  apr_hash_index_t *hi;
  jclass cls = JCALL1(FindClass, jenv, "java/util/Map");
  jmethodID put = JCALL3(GetMethodID, jenv, cls, "put",
                         "(Ljava/lang/Object;Ljava/lang/Object;)"
                         "Ljava/lang/Object;");

  for (hi = apr_hash_first(NULL, hash); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      void *val;
      jobject keyname, value, oldvalue;

      apr_hash_this(hi, &key, NULL, &val);
      keyname = JCALL1(NewStringUTF, jenv, key);
      value = make_pointer(jenv, val);
	  
      oldvalue = JCALL4(CallObjectMethod, jenv, map, put, keyname, value);
  
      JCALL1(DeleteLocalRef, jenv, value);
      JCALL1(DeleteLocalRef, jenv, oldvalue);
      JCALL1(DeleteLocalRef, jenv, keyname);

	  if (JCALL0(ExceptionOccurred, jenv))
          return;
    }
}

static jobject convert_to_swigtype(JNIEnv* jenv, void *value, void *ctx)
{
  return make_pointer(jenv, value);
}

static jobject convert_svn_string_t(JNIEnv* jenv, void *value, void *ctx)
{
  const svn_string_t *s = value;

  /* This will copy the data */
  return JCALL1(NewStringUTF, jenv, s->data);
}

jobject svn_swig_java_prophash_to_dict(JNIEnv *jenv, apr_hash_t *hash)
{
  return convert_hash(jenv, hash, convert_svn_string_t, jenv);
}

jobject svn_swig_java_convert_hash(JNIEnv *jenv, apr_hash_t *hash)
{
  return convert_hash(jenv, hash, convert_to_swigtype, NULL);
}

jobject svn_swig_java_c_strings_to_list(JNIEnv *jenv, char **strings)
{
  jclass cls = JCALL1(FindClass, jenv, "java/util/ArrayList");
  jobject list = JCALL2(NewObject, jenv, cls,
                        JCALL3(GetMethodID, jenv, cls, "<init>", "()V"));
  jmethodID add = JCALL3(GetMethodID, jenv, cls, "add", "(Ljava/lang/Object;)Z");
  char *s;
  jobject obj;
  while ((s = *strings++) != NULL)
    {
      obj = JCALL1(NewStringUTF, jenv, s);

      if (obj == NULL)
          goto error;

      JCALL3(CallBooleanMethod, jenv, list, add, obj);

      JCALL1(DeleteLocalRef, jenv, obj);
    }

  return list;

 error:
  JCALL1(DeleteLocalRef, jenv, list);
  return NULL;
}

jobject svn_swig_java_array_to_list(JNIEnv *jenv,
                                    const apr_array_header_t *strings)
{
  jclass cls = JCALL1(FindClass, jenv, "java/util/ArrayList");
  jobject list = JCALL3(NewObject, jenv, cls,
                        JCALL3(GetMethodID, jenv, cls, "<init>", "(I)V"),
                        strings->nelts);
  int i;
  jobject obj;

  jmethodID add;
  if (strings->nelts > 0)
    add = JCALL3(GetMethodID, jenv, cls, "add", "(i, Ljava/lang/Object;)Z");

  for (i = 0; i < strings->nelts; ++i)
    {
      const char *s;

      s = APR_ARRAY_IDX(strings, i, const char *);
      obj = JCALL1(NewStringUTF, jenv, s);
      if (obj == NULL)
        goto error;
      /* ### HELP: The format specifier might be 'I' instead of 'i' */
      JCALL4(CallObjectMethod, jenv, list, add, i, obj);
      JCALL1(DeleteLocalRef, jenv, obj);
    }

  return list;

 error:
  JCALL1(DeleteLocalRef, jenv, list);
  return NULL;
}

const apr_array_header_t *svn_swig_java_strings_to_array(JNIEnv *jenv,
                                                         jobject source,
                                                         apr_pool_t *pool)
{
  int targlen;
  apr_array_header_t *temp;

  jclass cls = JCALL1(FindClass, jenv, "java/util/List");
  jmethodID size = JCALL3(GetMethodID, jenv, cls, "size", "()I");
  jmethodID get = JCALL3(GetMethodID, jenv, cls, "get",
                         "(I)Ljava/lang/Object;");

  jclass illegalArgCls = JCALL1(FindClass, jenv,
                                "java/lang/IllegalArgumentException");

  if (!JCALL2(IsInstanceOf, jenv, source, cls))
    {
      if (JCALL2(ThrowNew, jenv, illegalArgCls, "Not a List") != JNI_OK)
          return NULL;
    }

  targlen = JCALL2(CallIntMethod, jenv, source, size);
  temp = apr_array_make(pool, targlen, sizeof(const char *));
  while (targlen--)
    {
      jobject o = JCALL3(CallObjectMethod, jenv, source, get, targlen);
	  const char * c_string;
      if (o == NULL)
          return NULL;
      else if (!JCALL2(IsInstanceOf, jenv, o,
                       JCALL1(FindClass, jenv, "java/lang/String")))
        {
          JCALL1(DeleteLocalRef, jenv, o);
          if (JCALL2(ThrowNew, jenv, illegalArgCls, "Not a String") != JNI_OK)
            {
              return NULL;
            }
        }
      c_string = (*jenv)->GetStringUTFChars(jenv, o, 0);
      APR_ARRAY_IDX(temp, targlen, const char *) = apr_pstrdup(pool, c_string);
      (*jenv)->ReleaseStringUTFChars(jenv, o, c_string);
      JCALL1(DeleteLocalRef, jenv, o);

    }
  return temp;
}

static svn_error_t * convert_java_error(JNIEnv *jenv, apr_pool_t *pool)
{
    /* ### need to fetch the Java error and map it to an svn_error_t
       ### ... use something like the (relatively) new
       ### SVN_ERR_SWIG_PY_EXCEPTION_SET */

  return svn_error_create(APR_EGENERAL, NULL,
                          "the Java callback raised an exception");
}

static item_baton * make_baton(JNIEnv *jenv, apr_pool_t *pool,
                               jobject editor, jobject baton)
{
  item_baton *newb = apr_palloc(pool, sizeof(*newb));

  /* one more reference to the editor. */
  JCALL1(NewGlobalRef, jenv, editor);
  JCALL1(NewGlobalRef, jenv, baton);

  /* note: we take the caller's reference to 'baton' */

  newb->editor = JCALL1(NewGlobalRef, jenv, editor);
  newb->baton = baton;
  newb->pool = pool;
  newb->jenv = jenv;

  return newb;
}

static svn_error_t * close_baton(void *baton, const char *method)
{
  item_baton *ib = baton;
  jobject result;
  JNIEnv *jenv = ib->jenv;
  jclass cls = JCALL1(GetObjectClass, jenv, ib->editor);
  jmethodID methodID;

  /* If there is no baton object, then it is an edit_baton, and we should
     not bother to pass an object. Note that we still shove a NULL onto
     the stack, but the format specified just won't reference it.  */

  if (ib->baton)
    {
      methodID = JCALL3(GetMethodID, jenv, cls, method,
                       "(Ljava/lang/Object;)Ljava/lang/Object;");
      result = JCALL3(CallObjectMethod, jenv, ib->editor, methodID, ib->baton);
    }
  else
    {
      methodID = JCALL3(GetMethodID, jenv, cls, method,
                        "()Ljava/lang/Object;");
      result = JCALL2(CallObjectMethod, jenv, ib->editor, methodID);
    }

  if (result == NULL)
      return convert_java_error(ib->jenv, ib->pool);

  /* there is no return value, so just toss this object */
  JCALL1(DeleteGlobalRef, ib->jenv, result);

  /* We're now done with the baton. Since there isn't really a free, all
     we need to do is note that its objects are no longer referenced by
     the baton.  */
  JCALL1(DeleteGlobalRef, ib->jenv, ib->editor);
  JCALL1(DeleteGlobalRef, ib->jenv, ib->baton);

#ifdef SVN_DEBUG
  ib->editor = ib->baton = NULL;
#endif

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_set_target_revision(void *edit_baton,
                                               svn_revnum_t target_revision,
                                               apr_pool_t *pool)
{
  item_baton *ib = edit_baton;
  jobject result;
  jclass cls; /*= JCALL(FindClass, ib->jenv, "FIXME");*/
  /* FIXME: Signature wants svn_revnum type instead of java.lang.Object */
  jmethodID methodID = JCALL3(GetMethodID, ib->jenv, cls,
                              "set_target_revision", "(Ljava/lang/Object;)");

  /* FIXME: Translate to JNI
  if ((result = PyObject_CallMethod(ib->editor, (char *)"set_target_revision",
                                    (char *)"l", target_revision)) == NULL)
    {
      return convert_java_error(ib->jenv, pool);
    }
  */

  /* there is no return value, so just toss this object */
  JCALL1(DeleteGlobalRef, ib->jenv, result);

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_open_root(void *edit_baton,
                                     svn_revnum_t base_revision,
                                     apr_pool_t *dir_pool,
                                     void **root_baton)
{
  item_baton *ib = edit_baton;
  jobject result;

  /* FIXME: Translate to JNI
  if ((result = PyObject_CallMethod(ib->editor, (char *)"open_root",
                                    (char *)"lO&", base_revision,
                                    make_ob_pool, dir_pool)) == NULL)
    {
      return convert_java_error(ib->jenv, dir_pool);
    }
  */

  /* make_baton takes our 'result' reference */
  *root_baton = make_baton(ib->jenv, dir_pool, ib->editor, result);

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_delete_entry(const char *path,
                                        svn_revnum_t revision,
                                        void *parent_baton,
                                        apr_pool_t *pool)
{
  item_baton *ib = parent_baton;
  jobject result;

  /* FIXME: Translate to JNI
  if ((result = PyObject_CallMethod(ib->editor, (char *)"delete_entry",
                                    (char *)"slOO&", path, revision, ib->baton,
                                    make_ob_pool, pool)) == NULL)
    {
      return convert_java_error(ib->jenv, pool);
    }
  */

  /* there is no return value, so just toss this object */
  JCALL1(DeleteGlobalRef, ib->jenv, result);

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_add_directory(const char *path,
                                         void *parent_baton,
                                         const char *copyfrom_path,
                                         svn_revnum_t copyfrom_revision,
                                         apr_pool_t *dir_pool,
                                         void **child_baton)
{
  item_baton *ib = parent_baton;
  jobject result;

  /* FIXME: Translate to JNI
  if ((result = PyObject_CallMethod(ib->editor, (char *)"add_directory",
                                    (char *)"sOslO&", path, ib->baton,
                                    copyfrom_path, copyfrom_revision,
                                    make_ob_pool, dir_pool)) == NULL)
    {
      return convert_java_error(ib->jenv, dir_pool);
    }
  */

  /* make_baton takes our 'result' reference */
  *child_baton = make_baton(ib->jenv, dir_pool, ib->editor, result);

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_open_directory(const char *path,
                                          void *parent_baton,
                                          svn_revnum_t base_revision,
                                          apr_pool_t *dir_pool,
                                          void **child_baton)
{
  item_baton *ib = parent_baton;
  jobject result;

  /* FIXME: Translate to JNI
  if ((result = PyObject_CallMethod(ib->editor, (char *)"open_directory",
                                    (char *)"sOlO&", path, ib->baton,
                                    base_revision,
                                    make_ob_pool, dir_pool)) == NULL)
    {
      return convert_java_error(ib->jenv, dir_pool);
    }
  */

  /* make_baton takes our 'result' reference */
  *child_baton = make_baton(ib->jenv, dir_pool, ib->editor, result);

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_change_dir_prop(void *dir_baton,
                                           const char *name,
                                           const svn_string_t *value,
                                           apr_pool_t *pool)
{
  item_baton *ib = dir_baton;
  jobject result;

  /* FIXME: Translate to JNI
  if ((result = PyObject_CallMethod(ib->editor, (char *)"change_dir_prop",
                                    (char *)"Oss#O&", ib->baton, name,
                                    value->data, value->len,
                                    make_ob_pool, pool)) == NULL)
    {
      return convert_java_error(ib->jenv, pool);
    }
  */

  /* there is no return value, so just toss this object */
  JCALL1(DeleteGlobalRef, ib->jenv, result);

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_close_directory(void *dir_baton, apr_pool_t *pool)
{
  return close_baton(dir_baton, "close_directory");
}

static svn_error_t * thunk_add_file(const char *path,
                                    void *parent_baton,
                                    const char *copyfrom_path,
                                    svn_revnum_t copyfrom_revision,
                                    apr_pool_t *file_pool,
                                    void **file_baton)
{
  item_baton *ib = parent_baton;
  jobject result;

  /* FIXME: Translate to JNI
  if ((result = PyObject_CallMethod(ib->editor, (char *)"add_file",
                                    (char *)"sOslO&", path, ib->baton,
                                    copyfrom_path, copyfrom_revision,
                                    make_ob_pool, file_pool)) == NULL)
    {
      return convert_java_error(ib->jenv, file_pool);
    }
  */

  /* make_baton takes our 'result' reference */
  *file_baton = make_baton(ib->jenv, file_pool, ib->editor, result);

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_open_file(const char *path,
                                     void *parent_baton,
                                     svn_revnum_t base_revision,
                                     apr_pool_t *file_pool,
                                     void **file_baton)
{
  item_baton *ib = parent_baton;
  jobject result;

  /* FIXME: Translate to JNI
  if ((result = PyObject_CallMethod(ib->editor, (char *)"open_file",
                                    (char *)"sOlO&", path, ib->baton,
                                    base_revision,
                                    make_ob_pool, file_pool)) == NULL)
    {
      return convert_java_error(ib->jenv, file_pool);
    }
  */

  /* make_baton takes our 'result' reference */
  *file_baton = make_baton(ib->jenv, file_pool, ib->editor, result);

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_window_handler(svn_txdelta_window_t *window,
                                          void *baton)
{
  handler_baton *hb = baton;
  jobject result;

  if (window == NULL)
    {
      /* the last call; it closes the handler */

      /* invoke the handler with None for the window */
      /* ### python doesn't have 'const' on the format */
      /* FIXME: To JNI
      result = PyObject_CallFunction(hb->handler, (char *)"O", Py_None);
      */

      /* we no longer need to refer to the handler object */
      JCALL1(DeleteGlobalRef, hb->jenv, hb->handler);
    }
  else
    {
      /* invoke the handler with the window */
      /* FIXME: Translate to JNI
      result = PyObject_CallFunction(hb->handler,
                                     (char *)"O&", make_ob_window, window);
      */
    }

  if (result == NULL)
    return convert_java_error(hb->jenv, hb->pool);

  /* there is no return value, so just toss this object */
  JCALL1(DeleteGlobalRef, hb->jenv, result);

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_apply_textdelta(
    void *file_baton,
    const char *base_checksum,
    const char *result_checksum,
    apr_pool_t *pool,
    svn_txdelta_window_handler_t *handler,
    void **h_baton)
{
  item_baton *ib = file_baton;
  jobject result;

  /* FIXME: Translate to JNI
  if ((result = PyObject_CallMethod(ib->editor, (char *)"apply_textdelta",
                                    (char *)"O", ib->baton)) == NULL)
    {
      return convert_java_error(ib->jenv, pool);
    }
  */

  /* FIXME: To JNI
  if (result == Py_None)
    {
      JCALL1(DeleteGlobalRef, ib->jenv, result);
      *handler = NULL;
      *h_baton = NULL;
    }
  else
  */
    {
      handler_baton *hb = apr_palloc(ib->pool, sizeof(*hb));

      /* return the thunk for invoking the handler. the baton takes our
         'result' reference. */
      hb->handler = result;
      hb->pool = ib->pool;
      hb->jenv = ib->jenv;

      *handler = thunk_window_handler;
      *h_baton = hb;
    }

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_change_file_prop(void *file_baton,
                                            const char *name,
                                            const svn_string_t *value,
                                            apr_pool_t *pool)
{
  item_baton *ib = file_baton;
  jobject result;

  /* FIXME: Translate to JNI
  if ((result = PyObject_CallMethod(ib->editor, (char *)"change_file_prop",
                                    (char *)"Oss#O&", ib->baton, name,
                                    value->data, value->len,
                                    make_ob_pool, pool)) == NULL)
    {
      return convert_java_error(ib->jenv, pool);
    }
  */

  /* there is no return value, so just toss this object */
  JCALL1(DeleteGlobalRef, ib->jenv, result);

  return SVN_NO_ERROR;
}

static svn_error_t * thunk_close_file(void *file_baton, apr_pool_t *pool)
{
  return close_baton(file_baton, "close_file");
}

static svn_error_t * thunk_close_edit(void *edit_baton, apr_pool_t *pool)
{
  return close_baton(edit_baton, "close_edit");
}

static svn_error_t * thunk_abort_edit(void *edit_baton, apr_pool_t *pool)
{
  return close_baton(edit_baton, "abort_edit");
}

void svn_swig_java_make_editor(JNIEnv *jenv,
                               const svn_delta_editor_t **editor,
                               void **edit_baton,
                               jobject java_editor,
                               apr_pool_t *pool)
{
  svn_delta_editor_t *thunk_editor = svn_delta_default_editor (pool);

  thunk_editor->set_target_revision = thunk_set_target_revision;
  thunk_editor->open_root = thunk_open_root;
  thunk_editor->delete_entry = thunk_delete_entry;
  thunk_editor->add_directory = thunk_add_directory;
  thunk_editor->open_directory = thunk_open_directory;
  thunk_editor->change_dir_prop = thunk_change_dir_prop;
  thunk_editor->close_directory = thunk_close_directory;
  thunk_editor->add_file = thunk_add_file;
  thunk_editor->open_file = thunk_open_file;
  thunk_editor->apply_textdelta = thunk_apply_textdelta;
  thunk_editor->change_file_prop = thunk_change_file_prop;
  thunk_editor->close_file = thunk_close_file;
  thunk_editor->close_edit = thunk_close_edit;
  thunk_editor->abort_edit = thunk_abort_edit;

  *editor = thunk_editor;
  *edit_baton = make_baton(jenv, pool, java_editor, NULL);
}

/* a notify function that executes a Java method on an object which is
   passed in via the baton argument */
void svn_swig_java_notify_func(void *baton,
                               const char *path,
                               svn_wc_notify_action_t action,
                               svn_node_kind_t kind,
                               const char *mime_type,
                               svn_wc_notify_state_t content_state,
                               svn_wc_notify_state_t prop_state,
                               svn_revnum_t revision)
{
    /* TODO: svn_swig_java_notify_func is not implemented yet */
}

/* thunked commit log fetcher */
svn_error_t *svn_swig_java_get_commit_log_func (const char **log_msg,
                                              const char **tmp_file,
                                              apr_array_header_t *commit_items,
                                              void *baton,
                                              apr_pool_t *pool)
{
    return svn_error_create(APR_EGENERAL, NULL, "TODO: "
                            "svn_swig_java_get_commit_log_func is not "
                            "implemented yet");
}

/* log messages are returned in this */
svn_error_t *svn_swig_java_log_message_receiver(void *baton,
      apr_hash_t *changed_paths,
      svn_revnum_t revision,
      const char *author,
      const char *date,  /* use svn_time_from_string() if need apr_time_t */
      const char *message,
      apr_pool_t *pool)
{
    return svn_error_create(APR_EGENERAL, NULL, "TODO: svn_swig_java_get_commit_log_func is not implemented yet");
}

