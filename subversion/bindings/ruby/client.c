/* ====================================================================
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
#include <ruby.h>

#include <svn_pools.h>
#include <svn_client.h>

#include "svn_ruby.h"
#include "delta_editor.h"
#include "wc.h"
#include "util.h"
#include "error.h"

static svn_error_t *
cl_prompt (char **info,
           const char *prompt,
           svn_boolean_t hide,
           void *baton,
           apr_pool_t *pool)
{
  VALUE self = (VALUE) baton;
  VALUE obj;
  int error;
  VALUE args[4];

  args[0] = self;
  args[1] = (VALUE) "call";
  args[2] = rb_str_new2 (prompt);
  args[3] = hide ? Qtrue : Qfalse;

  if (self == Qnil)
    svn_error_createf
      (APR_EGENERAL, 0, 0, pool,
       "Authentication is required but no block is given to get user data");
  obj = rb_protect (svn_ruby_protect_call2, (VALUE) args, &error);

  if (error)
    svn_ruby_error ("authenticator", pool);

  if (BUILTIN_TYPE (obj) != T_STRING)
    return svn_error_create (APR_EGENERAL, 0, 0, pool,
                             "auth block must return string object");

  *info = apr_pstrdup (pool, StringValuePtr (obj));
  return SVN_NO_ERROR;
}

static void
free_cl (void *p)
{
  svn_client_auth_baton_t *auth_baton = p;

  apr_pool_destroy (auth_baton->pool);
  free (auth_baton);
}

static VALUE
cl_new (int argc, VALUE *argv, VALUE class)
{
  VALUE obj, auth;
  svn_client_auth_baton_t *auth_baton;

  rb_scan_args (argc, argv, "00&", &auth);
  obj = Data_Make_Struct (class, svn_client_auth_baton_t, 0, free_cl,
                          auth_baton);
  auth_baton->pool = svn_pool_create (NULL);
  auth_baton->prompt_callback = cl_prompt;
  auth_baton->prompt_baton = (void *) auth;
  rb_iv_set (obj, "@auth", auth);

  return obj;
}

/* Parse args of type [DeltaEditor, DeltaEditor, xmlSrc] */
static void
cl_get_parse_arg (VALUE args,
                  const svn_delta_edit_fns_t **before_editor,
                  void **before_edit_baton,
                  const svn_delta_edit_fns_t **after_editor,
                  void *after_edit_baton,
                  char **xml_src)
{
  long len = RARRAY (args)->len;
  int i = 0;

  if (len > 3)
    rb_raise (rb_eArgError, "wrong # of arguments (%d)",
              3 + RARRAY (args)->len);
  else if (len == 0)
    return;

  if (BUILTIN_TYPE (RARRAY (args)->ptr[len - 1]) == T_STRING)
    {
      *xml_src = StringValuePtr (RARRAY (args)->ptr[len - 1]);
      if (len-- == 1)
        return;
    }
  else if (len == 3)
    rb_raise (rb_eTypeError, "last argument must be string");

  if (i < len)
    svn_ruby_delta_editor (before_editor, before_edit_baton,
                           RARRAY (args)->ptr[i++]);
  if (i < len)
    svn_ruby_delta_editor (after_editor, after_edit_baton,
                           RARRAY (args)->ptr[i++]);
}

static VALUE
cl_checkout (int argc, VALUE *argv, VALUE self)
{
  VALUE aURL, aPath, aRevOrTime, rest;
  const svn_delta_edit_fns_t *before_editor = NULL;
  void *before_edit_baton = NULL;
  const svn_delta_edit_fns_t *after_editor = NULL;
  void *after_edit_baton = NULL;
  svn_client_auth_baton_t *auth_baton;
  svn_stringbuf_t *URL, *path;
  svn_revnum_t revision = SVN_INVALID_REVNUM;
  apr_time_t tm = 0;
  svn_stringbuf_t *xml_src;
  apr_pool_t *pool;
  svn_error_t *err;
  char *xml = NULL;

  rb_scan_args (argc, argv, "3*", &aURL, &aPath, &aRevOrTime, &rest);
  Check_Type (aURL, T_STRING);
  Check_Type (aPath, T_STRING);
  if (rb_obj_is_kind_of (aRevOrTime, rb_cTime) == Qtrue)
    {
      time_t sec, usec;
      sec = NUM2LONG (rb_funcall (aRevOrTime, rb_intern ("tv_sec"), 0));
      usec = NUM2LONG (rb_funcall (aRevOrTime, rb_intern ("tv_usec"), 0));
      tm = sec * APR_USEC_PER_SEC + usec;
    }
  else
    revision = NUM2LONG (aRevOrTime);

  cl_get_parse_arg (rest, &before_editor, &before_edit_baton,
                    &after_editor, &after_edit_baton, &xml);
  if (xml && revision == SVN_INVALID_REVNUM)
    rb_raise (rb_eArgError, "xmlSrc requires explicit revision");
  
  pool = svn_pool_create (NULL);
  Data_Get_Struct (self, svn_client_auth_baton_t, auth_baton);
  URL = svn_stringbuf_create (StringValuePtr (aURL), pool);
  path = svn_stringbuf_create (StringValuePtr (aPath), pool);
  if (xml)
    xml_src = svn_stringbuf_create (xml, pool);
  else
    xml_src = NULL;

  err = svn_client_checkout (before_editor, before_edit_baton,
                             after_editor, after_edit_baton,
                             auth_baton, URL, path, revision,
			     TRUE, tm, xml_src, pool);
  if (err)
    {
      apr_pool_destroy (pool);
      svn_ruby_raise (err);
    }

  return Qnil;
}

static VALUE
cl_update (int argc, VALUE *argv, VALUE self)
{
  VALUE aPath, aRevOrTime, recurse, rest;
  const svn_delta_edit_fns_t *before_editor = NULL;
  void *before_edit_baton = NULL;
  const svn_delta_edit_fns_t *after_editor = NULL;
  void *after_edit_baton = NULL;
  svn_client_auth_baton_t *auth_baton;
  svn_stringbuf_t *path;
  svn_revnum_t revision = SVN_INVALID_REVNUM;
  apr_time_t tm = 0;
  svn_stringbuf_t *xml_src;
  apr_pool_t *pool;
  svn_error_t *err;
  char *xml = NULL;

  rb_scan_args (argc, argv, "3*", &aPath, &aRevOrTime, &recurse, &rest);
  Check_Type (aPath, T_STRING);
  if (rb_obj_is_kind_of (aRevOrTime, rb_cTime) == Qtrue)
    {
      time_t sec, usec;
      sec = NUM2LONG (rb_funcall (aRevOrTime, rb_intern ("tv_sec"), 0));
      usec = NUM2LONG (rb_funcall (aRevOrTime, rb_intern ("tv_usec"), 0));
      tm = sec * APR_USEC_PER_SEC + usec;
    }
  else
    revision = NUM2LONG (aRevOrTime);

  cl_get_parse_arg (rest, &before_editor, &before_edit_baton,
                    &after_editor, &after_edit_baton, &xml);
  if (xml && revision == SVN_INVALID_REVNUM)
    rb_raise (rb_eArgError, "xmlSrc requires explicit revision");

  pool = svn_pool_create (NULL);
  Data_Get_Struct (self, svn_client_auth_baton_t, auth_baton);
  path = svn_stringbuf_create (StringValuePtr (aPath), pool);
  if (xml)
    xml_src = svn_stringbuf_create (xml, pool);
  else
    xml_src = NULL;

  err = svn_client_update (before_editor, before_edit_baton,
                           after_editor, after_edit_baton,
                           auth_baton, path, xml_src,
                           revision, tm, RTEST (recurse),
                           pool);
  if (err)
    {
      apr_pool_destroy (pool);
      svn_ruby_raise (err);
    }

  return Qnil;
}

static VALUE
cl_add (VALUE class, VALUE aPath, VALUE recursive)
{
  svn_stringbuf_t *path;
  apr_pool_t *pool;
  svn_error_t *err;

  Check_Type (aPath, T_STRING);
  pool = svn_pool_create (NULL);
  path = svn_stringbuf_create (StringValuePtr (aPath), pool);

  err = svn_client_add (path, RTEST (recursive), pool);

  apr_pool_destroy (pool);

  if (err)
    svn_ruby_raise (err);

  return Qnil;
}

static VALUE
cl_delete (VALUE class, VALUE aPath, VALUE force)
{
  svn_stringbuf_t *path;
  apr_pool_t *pool;
  svn_error_t *err;

  Check_Type (aPath, T_STRING);
  pool = svn_pool_create (NULL);
  path = svn_stringbuf_create (StringValuePtr (aPath), pool);

  err = svn_client_delete (path, RTEST (force), pool);

  apr_pool_destroy (pool);

  if (err)
    svn_ruby_raise (err);

  return Qnil;
}

/* Parse arg [logMsg, beforeEditor, afterEditor, [xmlFile, revision]] */
static void
cl_put_parse_arg (VALUE args,
                  const svn_delta_edit_fns_t **before_editor,
                  void **before_edit_baton,
                  const svn_delta_edit_fns_t **after_editor,
                  void *after_edit_baton,
                  char **log_msg,
                  char **xml_src,
                  svn_revnum_t *revision)
{
  long len = RARRAY (args)->len;
  int i = 0;

  if (len > 5)
    rb_raise (rb_eArgError, "wrong # of optional arguments (%d)",
              RARRAY (args)->len);

  if (len == 0)
    return;

  /* Parse [xmlFile, revision] part. */
  if (len >= 2)
    {
      if (BUILTIN_TYPE (RARRAY (args)->ptr[len - 2]) == T_STRING)
        {
          *xml_src = StringValuePtr (RARRAY (args)->ptr[len - 2]);
          *revision = NUM2LONG (RARRAY (args)->ptr[len - 1]);
          len = len - 2;
        }
      if (len == 0)
        return;
    }

  /* Parse [logMsg, beforeEditor, afterEditor part. */
  if (BUILTIN_TYPE (RARRAY (args)->ptr[0]) == T_STRING)
    {
      *log_msg = StringValuePtr (RARRAY (args)->ptr[0]);
      i++;
    }
  if (i < len)
    {
      svn_ruby_delta_editor (before_editor, before_edit_baton,
                             RARRAY (args)->ptr[i]);
      i++;
    }
  if (i < len)
    {
      svn_ruby_delta_editor (after_editor, after_edit_baton,
                             RARRAY (args)->ptr[i]);
    }
}

static VALUE
cl_import (int argc, VALUE *argv, VALUE self)
{
  VALUE aURL, aPath, aEntry, rest;
  const svn_delta_edit_fns_t *before_editor = NULL;
  void *before_edit_baton = NULL;
  const svn_delta_edit_fns_t *after_editor = NULL;
  void *after_edit_baton = NULL;
  svn_client_auth_baton_t *auth_baton;
  svn_stringbuf_t *URL, *path, *new_entry;
  svn_stringbuf_t *log_msg;
  svn_stringbuf_t *xml_dst;
  svn_revnum_t revision = SVN_INVALID_REVNUM;
  apr_pool_t *pool;
  svn_error_t *err;
  char *log = NULL;
  char *xml = NULL;

  rb_scan_args (argc, argv, "3*", &aURL, &aPath, &aEntry, &rest);
  Check_Type (aURL, T_STRING);
  Check_Type (aPath, T_STRING);
  if (aEntry != Qnil)
    Check_Type (aEntry, T_STRING);
  cl_put_parse_arg (rest, &before_editor, &before_edit_baton,
                    &after_editor, &after_edit_baton, &log, &xml, &revision);
  
  pool = svn_pool_create (NULL);
  Data_Get_Struct (self, svn_client_auth_baton_t, auth_baton);
  URL = svn_stringbuf_create (StringValuePtr (aURL), pool);
  path = svn_stringbuf_create (StringValuePtr (aPath), pool);
  if (aEntry == Qnil)
    new_entry = NULL;
  else
    new_entry = svn_stringbuf_create (StringValuePtr (aEntry), pool);
  if (xml)
    xml_dst = svn_stringbuf_create (xml, pool);
  else
    xml_dst = NULL;
  if (log)
    log_msg = svn_stringbuf_create (log, pool);
  else
    log_msg = NULL;

  err = svn_client_import (before_editor, before_edit_baton,
                           after_editor, after_edit_baton,
                           auth_baton, path, URL, new_entry, "",
                           log_msg, xml_dst, revision, pool);
  if (err)
    {
      apr_pool_destroy (pool);
      svn_ruby_raise (err);
    }

  return Qnil;
}

static VALUE
cl_commit (int argc, VALUE *argv, VALUE self)
{
  VALUE aTargets, rest;
  const svn_delta_edit_fns_t *before_editor = NULL;
  void *before_edit_baton = NULL;
  const svn_delta_edit_fns_t *after_editor = NULL;
  void *after_edit_baton = NULL;
  svn_client_auth_baton_t *auth_baton;
  apr_array_header_t *targets;
  svn_stringbuf_t *log_msg;
  svn_stringbuf_t *xml_dst;
  svn_revnum_t revision = SVN_INVALID_REVNUM;
  apr_pool_t *pool;
  svn_error_t *err;
  char *log = NULL;
  char *xml = NULL;
  int i;

  rb_scan_args (argc, argv, "1*", &aTargets, &rest);
  Check_Type (aTargets, T_ARRAY);
  for (i = 0; i < RARRAY (aTargets)->len; i++)
    Check_Type (RARRAY (aTargets)->ptr[i], T_STRING);
  cl_put_parse_arg (rest, &before_editor, &before_edit_baton,
                    &after_editor, &after_edit_baton, &log, &xml, &revision);
  
  pool = svn_pool_create (NULL);
  Data_Get_Struct (self, svn_client_auth_baton_t, auth_baton);
  targets = apr_array_make (pool, RARRAY (aTargets)->len,
                            sizeof (svn_stringbuf_t *));
  for (i = 0; i < RARRAY (aTargets)->len; i++)
    (*((svn_stringbuf_t **) apr_array_push (targets))) =
      svn_stringbuf_create (StringValuePtr (RARRAY (aTargets)->ptr[i]), pool);

  if (xml)
    xml_dst = svn_stringbuf_create (xml, pool);
  else
    xml_dst = NULL;
  if (log)
    log_msg = svn_stringbuf_create (log, pool);
  else
    log_msg = NULL;

  err = svn_client_commit (before_editor, before_edit_baton,
                           after_editor, after_edit_baton,
                           auth_baton, targets,
                           log_msg, xml_dst, revision, pool);
  if (err)
    {
      apr_pool_destroy (pool);
      svn_ruby_raise (err);
    }

  return Qnil;
}

static VALUE
cl_status (VALUE self, VALUE aPath,
           VALUE descend, VALUE get_all, VALUE update)
{
  apr_hash_t *statushash;
  svn_stringbuf_t *path;
  svn_client_auth_baton_t *auth_baton;
  apr_pool_t *pool;
  svn_error_t *err;
  VALUE obj;

  Check_Type (aPath, T_STRING);
  Data_Get_Struct (self, svn_client_auth_baton_t, auth_baton);
  pool = svn_pool_create (NULL);
  path = svn_stringbuf_create (StringValuePtr (aPath), pool);

  err = svn_client_status (&statushash, path, auth_baton,
                           RTEST (descend), RTEST (get_all),
                           RTEST (update), pool);

  if (err)
    {
      apr_pool_destroy (pool);
      svn_ruby_raise (err);
    }

  obj = svn_ruby_wc_to_statuses (statushash, pool);
  return obj;
}

static VALUE
cl_file_diff (VALUE class, VALUE aPath)
{
  svn_stringbuf_t *path;
  svn_stringbuf_t *pristine_copy_path;
  apr_pool_t *pool;

  VALUE obj;
  svn_error_t *err;

  Check_Type (aPath, T_STRING);

  pool = svn_pool_create (NULL);
  path = svn_stringbuf_create (StringValuePtr (aPath), pool);
  err = svn_client_file_diff (path, &pristine_copy_path, pool);

  if (err)
    {
      apr_pool_destroy (pool);
      svn_ruby_raise (err);
    }

  obj = rb_str_new (pristine_copy_path->data, pristine_copy_path->len);
  apr_pool_destroy (pool);
  return obj;
}

static VALUE
cl_cleanup (VALUE class, VALUE aPath)
{
  svn_stringbuf_t *path;
  apr_pool_t *pool;

  svn_error_t *err;

  Check_Type (aPath, T_STRING);

  pool = svn_pool_create (NULL);
  path = svn_stringbuf_create (StringValuePtr (aPath), pool);
  err = svn_client_cleanup (path, pool);

  apr_pool_destroy (pool);
  if (err)
    svn_ruby_raise (err);

  return Qnil;
}

static VALUE
cl_revert (VALUE class, VALUE aPath, VALUE recursive)
{
  svn_stringbuf_t *path;
  apr_pool_t *pool;

  svn_error_t *err;

  Check_Type (aPath, T_STRING);

  pool = svn_pool_create (NULL);
  path = svn_stringbuf_create (StringValuePtr (aPath), pool);
  err = svn_client_revert (path, RTEST (recursive), pool);

  apr_pool_destroy (pool);
  if (err)
    svn_ruby_raise (err);

  return Qnil;
}


static VALUE
cl_copy (VALUE class, VALUE srcPath, VALUE dstPath)
{
  svn_stringbuf_t *src_path, *dst_path;
  apr_pool_t *pool;

  svn_error_t *err;

  Check_Type (srcPath, T_STRING);
  Check_Type (dstPath, T_STRING);

  rb_notimplement ();
  pool = svn_pool_create (NULL);
  src_path = svn_stringbuf_create (StringValuePtr (srcPath), pool);
  dst_path = svn_stringbuf_create (StringValuePtr (dstPath), pool);
  err = svn_client_copy (src_path, dst_path, pool);

  apr_pool_destroy (pool);
  if (err)
    svn_ruby_raise (err);

  return Qnil;
}

void svn_ruby_init_client (void)
{
  VALUE cSvnClient;

  cSvnClient = rb_define_class_under (svn_ruby_mSvn, "Client", rb_cObject);
  rb_define_singleton_method (cSvnClient, "new", cl_new, -1);
  rb_define_method (cSvnClient, "checkout", cl_checkout, -1);
  rb_define_method (cSvnClient, "update", cl_update, -1);
  rb_define_singleton_method (cSvnClient, "add", cl_add, 2);
  rb_define_singleton_method (cSvnClient, "delete", cl_delete, 2);
  rb_define_method (cSvnClient, "import", cl_import, -1);
  rb_define_method (cSvnClient, "commit", cl_commit, -1);
  rb_define_method (cSvnClient, "status", cl_status, 4);
  rb_define_singleton_method (cSvnClient, "fileDiff", cl_file_diff, 1);
  rb_define_singleton_method (cSvnClient, "cleanup", cl_cleanup, 1);
  rb_define_singleton_method (cSvnClient, "revert", cl_revert, 2);
  rb_define_singleton_method (cSvnClient, "copy", cl_copy, 2);
}
