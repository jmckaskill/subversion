/*
 * adm_files.c: helper routines for handling files & dirs in the
 *              working copy administrative area (creating,
 *              deleting, opening, and closing).  This is the only
 *              code that actually knows where administrative
 *              information is kept.  
 *
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */



#include <stdarg.h>
#include <assert.h>
#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_file_io.h>
#include <apr_time.h>
#include <apr_strings.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_hash.h"
#include "svn_path.h"
#include "svn_wc.h"
#include "wc.h"



/*** File names in the adm area. ***/

/* No one outside this file should ever need to know this.  In fact,
   no one outside adm_subdir() should ever need to know this. */
#define SVN_WC__ADM_DIR_DEFAULT   "SVN"
static const char *
adm_subdir (void)
{
  return SVN_WC__ADM_DIR_DEFAULT;
}


/* Extend PATH to the name of something in PATH's administrative area.
 * Returns the number of path components added to PATH.
 * 
 * First, the adm subdir is appended to PATH as a component, then each
 * of the varargs in AP (char *'s) is appended as a path component.
 * The list must be terminated with a NULL argument.
 *
 * Adding an empty component results in no effect (i.e., the separator
 * char is not doubled).
 *
 * Important: chances are you will want to call chop_admin_name() to
 * restore PATH to its original value before exiting anything that
 * calls this.  If you exit, say by returning an error, before calling
 * chop_admin_name(), then PATH will still be in its extended state.
 *
 * So, the safest recipe is for callers of extend_with_adm_name() to
 * always have exactly one return statement, occurring *after* an
 * unconditional call to chop_admin_name().
 */
static int
v_extend_with_adm_name (svn_string_t *path,
                        svn_boolean_t use_tmp,
                        apr_pool_t *pool,
                        va_list ap)
{
  const char *this;
  int components_added = 0;

  /* Tack on the administrative subdirectory. */
  svn_path_add_component_nts (path, adm_subdir (), svn_path_local_style);
  components_added++;

  /* If this is a tmp file, name it into the tmp area. */
  if (use_tmp)
    {
      svn_path_add_component_nts
        (path, SVN_WC__ADM_TMP, svn_path_local_style);
      components_added++;
    }

  /* Tack on everything else. */
  while ((this = va_arg (ap, const char *)) != NULL)
    {
      if (this[0] == '\0')
        continue;

      svn_path_add_component_nts (path, this, svn_path_local_style);
      components_added++;
    }

  return components_added;
}


/* See v_extend_with_adm_name() for details. */
static int
extend_with_adm_name (svn_string_t *path,
                      svn_boolean_t use_tmp,
                      apr_pool_t *pool,
                      ...)
{
  va_list ap;
  int components_added;

  va_start (ap, pool);
  components_added = v_extend_with_adm_name (path,
                                             use_tmp,
                                             pool,
                                             ap);
  va_end (ap);

  return components_added;
}


svn_string_t *
svn_wc__adm_path (svn_string_t *path,
                  svn_boolean_t tmp,
                  apr_pool_t *pool, 
                  ...)
{
  svn_string_t *newpath = svn_string_dup (path, pool);
  va_list ap;

  va_start (ap, pool);
  v_extend_with_adm_name (newpath, tmp, pool, ap);
  va_end (ap);

  return newpath;
}


/* Restore PATH to what it was before a call to
 * extend_with_adm_name(), by lopping off NUM_COMPONENTS
 * components.
 */
static void
chop_admin_name (svn_string_t *path, int num_components)
{
  while (num_components-- > 0)
    svn_path_remove_component (path, svn_path_local_style);
}



/*** Making and using files in the adm area. ***/


/* Create an empty THING in the adm area. 
 * If TMP is non-zero, then create THING in the tmp dir.
 *
 * Does not check if THING already exists, so be careful -- THING will
 * be empty after this no matter what.
 */
svn_error_t *
svn_wc__make_adm_thing (svn_string_t *path,
                        const char *thing,
                        int type,
                        svn_boolean_t tmp,
                        apr_pool_t *pool)
{
  svn_error_t *err = NULL;
  apr_file_t *f = NULL;
  apr_status_t apr_err = 0;
  int components_added;

  components_added = extend_with_adm_name (path, tmp, pool, thing, NULL);

  if (type == svn_node_file)
    {
      apr_err = apr_open (&f, path->data,
                          (APR_WRITE | APR_CREATE | APR_EXCL),
                          APR_OS_DEFAULT,
                          pool);

      if (apr_err)
        err = svn_error_create (apr_err, 0, NULL, pool, path->data);
      else
        {
          /* Creation succeeded, so close immediately. */
          apr_err = apr_close (f);
          if (apr_err)
            err = svn_error_create (apr_err, 0, NULL, pool, path->data);
        }
    }
  else if (type == svn_node_dir)
    {
      apr_err = apr_make_dir (path->data, APR_OS_DEFAULT, pool);
      if (apr_err)
        err = svn_error_create (apr_err, 0, NULL, pool, path->data);
    }
  else   /* unknown type argument, wrongness */
    {
      /* We're only capturing this here because there wouldn't be a
         segfault or other obvious indicator that something went
         wrong.  Even so, not sure if it's appropriate.  Thoughts? */
      err = svn_error_create 
        (0, 0, NULL, pool, "svn_wc__make_admin_thing: bad type indicator");
    }

  /* Restore path to its original state no matter what. */
  chop_admin_name (path, components_added);

  return err;
}



/* Copy SRC to DST if SRC exists, else create DST empty. */
static svn_error_t *
maybe_copy_file (svn_string_t *src, svn_string_t *dst, apr_pool_t *pool)
{
  enum svn_node_kind kind;
  svn_error_t *err;
  apr_status_t apr_err;

  /* First test if SRC exists. */
  err = svn_io_check_path (src, &kind, pool);
  if (err)
    return err;
  else if (kind == svn_node_none)
    {
      /* SRC doesn't exist, create DST empty. */
      apr_file_t *f = NULL;
      apr_err = apr_open (&f,
                          dst->data,
                          (APR_WRITE | APR_CREATE),
                          APR_OS_DEFAULT,
                          pool);
      if (apr_err)
        return svn_error_create (apr_err, 0, NULL, pool, dst->data);
      else
        {
          apr_err = apr_close (f);
          if (apr_err)
            return svn_error_create (apr_err, 0, NULL, pool, dst->data);
          else
            return SVN_NO_ERROR;
        }
    }
  else /* SRC exists, so copy it to DST. */
    {    
      err = svn_io_copy_file (src, dst, pool);
      if (err)
        return err;
    }

  return SVN_NO_ERROR;
}



/*** Syncing files in the adm area. ***/

static svn_error_t *
sync_adm_file (svn_string_t *path,
               apr_pool_t *pool,
               ...)
{
  /* Some code duplication with close_adm_file() seems unavoidable,
     given how C va_lists work. */

  svn_string_t *tmp_path = svn_string_dup (path, pool);
  apr_status_t apr_err;
  int components_added;
  va_list ap;
  
  /* Extend real name. */
  va_start (ap, pool);
  components_added = v_extend_with_adm_name (path, 0, pool, ap);
  va_end (ap);
  
  /* Extend tmp name. */
  va_start (ap, pool);
  v_extend_with_adm_name (tmp_path, 1, pool, ap);
  va_end (ap);
  
  /* Rename. */
  apr_err = apr_rename_file (tmp_path->data, path->data, pool);

  /* Unconditionally restore path. */
  chop_admin_name (path, components_added);
      
  if (apr_err)
    return svn_error_createf (apr_err, 0, NULL, pool,
                              "error renaming %s to %s",
                              tmp_path->data, path->data);
  else
    return SVN_NO_ERROR;
}


/* Rename a tmp text-base file to its real text-base name.
   The file had better already be closed. */
svn_error_t *
svn_wc__sync_text_base (svn_string_t *path, apr_pool_t *pool)
{
  svn_string_t *newpath, *basename;
  svn_path_split (path, &newpath, &basename, svn_path_local_style, pool);
  return sync_adm_file (newpath,
                        pool,
                        SVN_WC__ADM_TEXT_BASE,
                        basename->data,
                        NULL);
}


static svn_string_t *
thing_path (const svn_string_t *path,
            const char *thing,
            svn_boolean_t tmp,
            apr_pool_t *pool)
{
  svn_string_t *newpath, *basename;
  svn_path_split (path, &newpath, &basename, svn_path_local_style, pool);

  extend_with_adm_name (newpath,
                        0,
                        pool,
                        tmp ? SVN_WC__ADM_TMP : "",
                        thing,
                        basename->data,
                        NULL);
    
  return newpath;
}


svn_string_t *
svn_wc__text_base_path (const svn_string_t *path,
                        svn_boolean_t tmp,
                        apr_pool_t *pool)
{
  return thing_path (path, SVN_WC__ADM_TEXT_BASE, tmp, pool);
}


static svn_error_t *
prop_path_internal (svn_string_t **prop_path,
                    const svn_string_t *path,
                    svn_boolean_t base,
                    svn_boolean_t tmp,
                    apr_pool_t *pool)
{
  svn_error_t *err;
  enum svn_node_kind kind;
  svn_boolean_t is_wc;
  svn_string_t *entry_name;

  err = svn_io_check_path (path, &kind, pool);
  if (err)
    return err;

  /* kff todo: some factorization can be done on most callers of
     svn_wc_check_wc()? */

  is_wc = FALSE;
  entry_name = NULL;
  if (kind == svn_node_dir)
    {
      err = svn_wc_check_wc (path, &is_wc, pool);
      if (err)
        return err;
    }

  if (is_wc)  /* It's not only a dir, it's a working copy dir */
    {
      *prop_path = svn_string_dup (path, pool);
      extend_with_adm_name 
        (*prop_path,
         0,
         pool,
         tmp ? SVN_WC__ADM_TMP : "",
         base ? SVN_WC__ADM_DIR_PROP_BASE : SVN_WC__ADM_DIR_PROPS,
         NULL);
    }
  else  /* It's either a file, or a non-wc dir (i.e., maybe an ex-file) */
    {
      svn_path_split (path, prop_path, &entry_name,
                      svn_path_local_style, pool);
 
      err = svn_wc_check_wc (*prop_path, &is_wc, pool);
      if (err)
        return err;
      else if (! is_wc)
        return svn_error_createf
          (SVN_ERR_WC_OBSTRUCTED_UPDATE, 0, NULL, pool,
           "svn_wc__prop_path: %s is not a working copy directory",
           (*prop_path)->data);

      extend_with_adm_name (*prop_path,
                            0,
                            pool,
                            tmp ? SVN_WC__ADM_TMP : "",
                            base ? SVN_WC__ADM_PROP_BASE : SVN_WC__ADM_PROPS,
                            entry_name->data,
                            NULL);
    }

  return SVN_NO_ERROR;
}



svn_error_t *
svn_wc__prop_path (svn_string_t **prop_path,
                   const svn_string_t *path,
                   svn_boolean_t tmp,
                   apr_pool_t *pool)
{
  return prop_path_internal (prop_path, path, FALSE, tmp, pool);
}


svn_error_t *
svn_wc__prop_base_path (svn_string_t **prop_path,
                        const svn_string_t *path,
                        svn_boolean_t tmp,
                        apr_pool_t *pool)
{
  return prop_path_internal (prop_path, path, TRUE, tmp, pool);
}




/*** Opening and closing files in the adm area. ***/

/* Open a file somewhere in the adm area for directory PATH.
 * First, the adm subdir is appended as a path component, then each of
 * the varargs (they are char *'s) is appended as a path component,
 * and the resulting file opened.  
 *
 * If FLAGS indicates writing, then the file is opened in the adm tmp
 * area, whence it must be renamed, either by passing the sync flag to
 * close_adm_file() or with an explicit call to sync_adm_file().
 */
static svn_error_t *
open_adm_file (apr_file_t **handle,
               svn_string_t *path,
               apr_int32_t flags,
               apr_pool_t *pool,
               ...)
{
  svn_error_t *err = NULL;
  apr_status_t apr_err = 0;
  int components_added;
  va_list ap;

  /* If we're writing, always do it to a tmp file. */
  if (flags & APR_WRITE)
    {
      if (flags & APR_APPEND)
        {
          svn_string_t *opath, *tmp_path;  /* just keep it all local */

          opath    = svn_string_dup (path, pool);
          tmp_path = svn_string_dup (path, pool);

          va_start (ap, pool);
          v_extend_with_adm_name (opath, 0, pool, ap);
          va_end (ap);

          va_start (ap, pool);
          v_extend_with_adm_name (tmp_path, 1, pool, ap);
          va_end (ap);

          /* Copy the original thing to the tmp location. */
          err = maybe_copy_file (opath, tmp_path, pool);
          if (err)
            return err;
        }

      /* Extend with tmp name. */
      va_start (ap, pool);
      components_added
        = v_extend_with_adm_name (path, 1, pool, ap);
      va_end (ap);
    }
  else
    {
      /* Extend with regular adm name. */
      va_start (ap, pool);
      components_added
        = v_extend_with_adm_name (path, 0, pool, ap);
      va_end (ap);
    }

  apr_err = apr_open (handle, path->data, flags, APR_OS_DEFAULT, pool);
  if (apr_err)
    {
      /* Oddly enough, APR will set *HANDLE even if the open failed.
         You'll get a filehandle whose descriptor is -1.  There must
         be a reason this is useful... Anyway, we don't want the
         handle. */
      *handle = NULL;
      err = svn_error_create (apr_err, 0, NULL, pool, path->data);
    }

  /* Restore path to its original state no matter what. */
  chop_admin_name (path, components_added);

  return err;
}


/* Close the file indicated by FP (PATH is passed to make error
 * reporting better).  If SYNC is non-zero, then the file will be
 * sync'd from the adm tmp area to its permanent location, otherwise
 * it will remain in the tmp area.  See open_adm_file().
 */
static svn_error_t *
close_adm_file (apr_file_t *fp,
                svn_string_t *path,
                svn_boolean_t sync,
                apr_pool_t *pool,
                ...)
{
  apr_status_t apr_err = 0;
  int components_added;
  va_list ap;

  /* Get the full name of the thing we want. */
  va_start (ap, pool);
  components_added = v_extend_with_adm_name (path, sync, pool, ap);
  va_end (ap);

  apr_err = apr_close (fp);

  /* Restore path to its original state no matter what. */
  chop_admin_name (path, components_added);

  if (apr_err)
    return svn_error_create (apr_err, 0, NULL, pool, path->data);

  /* If we were writing, then it was to a tmp file, which will have to
     be renamed after closing. */
  if (sync)
    {
      /* Some code duplication with sync_adm_file() seems unavoidable,
         given how C va_lists work. */

      svn_string_t *tmp_path = svn_string_dup (path, pool);
      
      /* Extend real name. */
      va_start (ap, pool);
      components_added = v_extend_with_adm_name (path, 0, pool, ap);
      va_end (ap);
      
      /* Extend tmp name. */
      va_start (ap, pool);
      v_extend_with_adm_name (tmp_path, 1, pool, ap);
      va_end (ap);
      
      /* Rename. */
      apr_err = apr_rename_file (tmp_path->data, path->data, pool);
      
      /* Unconditionally restore path. */
      chop_admin_name (path, components_added);
      
      if (apr_err)
        return svn_error_createf (apr_err, 0, NULL, pool,
                                  "error renaming %s to %s",
                                  tmp_path->data, path->data);
      else
        return SVN_NO_ERROR;
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__open_adm_file (apr_file_t **handle,
                       const svn_string_t *path,
                       const char *fname,
                       apr_int32_t flags,
                       apr_pool_t *pool)
{
  *handle = NULL;  /* satisfy APR's bizarre requirement */
  return open_adm_file (handle, (svn_string_t *) path, flags, pool,
                        fname, NULL);
}


svn_error_t *
svn_wc__close_adm_file (apr_file_t *fp,
                        const svn_string_t *path,
                        const char *fname,
                        int sync,
                        apr_pool_t *pool)
{
  return close_adm_file (fp, (svn_string_t *) path, sync, pool, fname, NULL);
}


svn_error_t *
svn_wc__open_text_base (apr_file_t **handle,
                        svn_string_t *path,
                        apr_int32_t flags,
                        apr_pool_t *pool)
{
  svn_string_t *newpath, *basename;
  svn_path_split (path, &newpath, &basename, svn_path_local_style, pool);
  return open_adm_file (handle, newpath, flags, pool,
                        SVN_WC__ADM_TEXT_BASE, basename->data, NULL);
}


svn_error_t *
svn_wc__close_text_base (apr_file_t *fp,
                         svn_string_t *path,
                         int write,
                         apr_pool_t *pool)
{
  svn_string_t *newpath, *basename;
  svn_path_split (path, &newpath, &basename, svn_path_local_style, pool);
  return close_adm_file (fp, newpath, write, pool,
                         SVN_WC__ADM_TEXT_BASE, basename->data, NULL);
}


svn_error_t *
svn_wc__remove_adm_file (svn_string_t *path, apr_pool_t *pool, ...)
{
  svn_error_t *err = NULL;
  apr_status_t apr_err = 0;
  int components_added;
  va_list ap;

  va_start (ap, pool);
  components_added = v_extend_with_adm_name (path, 0, pool, ap);
  va_end (ap);

  apr_err = apr_remove_file (path->data, pool);
  if (apr_err)
    err = svn_error_create (apr_err, 0, NULL, pool, path->data);

  /* Restore path to its original state no matter what. */
  chop_admin_name (path, components_added);

  return err;
}



/*** Checking for and creating administrative subdirs. ***/

/* Set *EXISTS to non-zero iff there's an adm area for PATH.
   If an error occurs, just return error and don't touch *EXISTS. */
static svn_error_t *
check_adm_exists (int *exists,
                  svn_string_t *path,
                  svn_string_t *ancestor_path,
                  svn_revnum_t ancestor_revision,
                  apr_pool_t *pool)
{
  svn_error_t *err = NULL;
  enum svn_node_kind kind;
  int dir_exists = 0;
  apr_file_t *f = NULL;
  int components_added;

  /** Step 1: check that the directory exists. **/

  components_added = extend_with_adm_name (path, 0, pool, NULL);

  err = svn_io_check_path (path, &kind, pool);
  if (!err)
    {
      if (kind != svn_node_none && kind != svn_node_dir)
        {
          /* If got an error other than dir non-existence, then
             something's weird and we should return a genuine error. */
          err = svn_error_create (APR_ENOTDIR, 0, NULL, pool, path->data);
        }
      else if (kind == svn_node_none)
        {
          dir_exists = 0;
        }
      else                      /* must be a dir. */
        {
          assert (kind == svn_node_dir);
          dir_exists = 1;
        }
    }

  /* Restore path to its original state. */
  chop_admin_name (path, components_added);

  /** Step 1.  If no adm directory, then we're done. */
  if (err)
    return err;
  else if (! dir_exists)
    {
      *exists = 0;
      return SVN_NO_ERROR;
    }

  /** The directory exists, but is it a valid working copy yet?
      Try step 2: checking that SVN_WC__ADM_README exists. **/

  err = svn_wc__open_adm_file (&f, path, SVN_WC__ADM_README, APR_READ, pool);
  if (err && !APR_STATUS_IS_EEXIST(err->apr_err))
    return err;
  else if (err)
    *exists = 0;
  else
    *exists = 1;

  err = svn_wc__close_adm_file (f, path, SVN_WC__ADM_README, 0, pool);
  if (err)
    return err;

  /** kff todo:
      Step 3: now check that repos and ancestry are correct **/

  return SVN_NO_ERROR;
}


static svn_error_t *
make_empty_adm (svn_string_t *path, apr_pool_t *pool)
{
  svn_error_t *err;
  apr_status_t apr_err;
  int components_added;

  components_added = extend_with_adm_name (path, 0, pool, NULL);

  apr_err = apr_make_dir (path->data, APR_OS_DEFAULT, pool);
  if (apr_err)
    err = svn_error_create (apr_err, 0, NULL, pool, path->data);
    
  chop_admin_name (path, components_added);

  return err;
}


/* Init an adm file with some contents. 
   Don't call this until a tmp area exists in adm. */
static svn_error_t *
init_adm_file (svn_string_t *path,
               const char *thing,
               svn_string_t *contents,
               apr_pool_t *pool)
{
  svn_error_t *err;
  apr_status_t apr_err;
  apr_file_t *f = NULL;
  apr_size_t written = 0;

  err = svn_wc__open_adm_file (&f, path, thing, APR_WRITE | APR_CREATE, pool);
  if (err)
    return err;

  apr_err = apr_full_write (f, contents->data, contents->len, &written);

  err = svn_wc__close_adm_file (f, path, thing, 1, pool);
  if (err)
    return err;
  
  if (apr_err)
    err = svn_error_create (apr_err, 0, NULL, pool, path->data);

  return err;
}


/* Set up a new adm area, with appropriate ancestry. 
   The adm area starts out locked; remember to unlock it when done. */
static svn_error_t *
init_adm (svn_string_t *path,
          svn_string_t *repository,
          svn_string_t *ancestor_path,
          apr_pool_t *pool)
{
  svn_error_t *err;

  /* Initial contents for certain adm files. */
  const char *format_contents = "1\n";
  const char *readme_contents =
    "This is a Subversion working copy administrative directory.\n"
    "Visit http://subversion.tigris.org/ for more information.\n";

  /* First, make an empty administrative area. */
  err = make_empty_adm (path, pool);

  /* Lock it immediately.  Theoretically, no compliant wc library
     would ever consider this an adm area until a README file were
     present... but locking it is still appropriately paranoid. */
  err = svn_wc__lock (path, 0, pool);
  if (err)
    return err;


  /** Make subdirectories. ***/

  /* SVN_WC__ADM_TMP */
  err = svn_wc__make_adm_thing (path, SVN_WC__ADM_TMP,
                                svn_node_dir, 0, pool);
  if (err)
    return err;
  
  /* SVN_WC__ADM_TEXT_BASE */
  err = svn_wc__make_adm_thing (path, SVN_WC__ADM_TEXT_BASE,
                                svn_node_dir, 0, pool);
  if (err)
    return err;


  /* SVN_WC__ADM_PROP_BASE */
  err = svn_wc__make_adm_thing (path, SVN_WC__ADM_PROP_BASE,
                                svn_node_dir, 0, pool);
  if (err)
    return err;


  /* SVN_WC__ADM_PROPS */
  err = svn_wc__make_adm_thing (path, SVN_WC__ADM_PROPS,
                                svn_node_dir, 0, pool);
  if (err)
    return err;


  /** Make sub-subdirectories. ***/

  /* SVN_WC__ADM_TMP/SVN_WC__ADM_TEXT_BASE */
  err = svn_wc__make_adm_thing (path, SVN_WC__ADM_TEXT_BASE,
                                svn_node_dir, 1, pool);
  if (err)
    return err;


  /* SVN_WC__ADM_TMP/SVN_WC__ADM_PROP_BASE */
  err = svn_wc__make_adm_thing (path, SVN_WC__ADM_PROP_BASE,
                                svn_node_dir, 1, pool);
  if (err)
    return err;


  /* SVN_WC__ADM_TMP/SVN_WC__ADM_PROPS */
  err = svn_wc__make_adm_thing (path, SVN_WC__ADM_PROPS,
                                svn_node_dir, 1, pool);
  if (err)
    return err;


  /** Initialize each administrative file. */

  /* SVN_WC__ADM_FORMAT */
  err = init_adm_file (path, SVN_WC__ADM_FORMAT,
                       svn_string_create (format_contents, pool), pool);
  if (err)
    return err;


  /* SVN_WC__ADM_REPOSITORY */
  err = init_adm_file (path, SVN_WC__ADM_REPOSITORY, repository, pool);
  if (err)
    return err;


  /* SVN_WC__ADM_ENTRIES */
  err = svn_wc__entries_init (path, ancestor_path, pool);
  if (err)
    return err;


  /* SVN_WC__ADM_DIR_PROPS */
  err = svn_wc__make_adm_thing (path, SVN_WC__ADM_DIR_PROPS,
                                svn_node_file, 0, pool);
  if (err)
    return err;


  /* SVN_WC__ADM_DIR_PROP_BASE */
  err = svn_wc__make_adm_thing (path, SVN_WC__ADM_DIR_PROP_BASE,
                                svn_node_file, 0, pool);
  if (err)
    return err;



  /* THIS FILE MUST BE CREATED LAST: 
     After this exists, the dir is considered complete. */
  err = init_adm_file (path, SVN_WC__ADM_README,
                       svn_string_create (readme_contents, pool),
                       pool);
  if (err)
    return err;


  /* Now unlock it.  It's now a valid working copy directory, that
     just happens to be at revision 0. */
  err = svn_wc__unlock (path, pool);
  if (err)
    return err;

  /* Else no problems, we're outta here. */
  return SVN_NO_ERROR;
}


/* Make sure that PATH (a directory) contains a complete adm area,
 * based at REPOSITORY.
 *
 * Creates the adm area if none, in which case PATH starts out at
 * revision 0.
 */
svn_error_t *
svn_wc__ensure_adm (svn_string_t *path,
                    svn_string_t *repository,
                    svn_string_t *ancestor_path,
                    svn_revnum_t ancestor_revision,
                    apr_pool_t *pool)
{
  svn_error_t *err;
  int exists_already;

  /* kff todo: check repos... and ancestry? */
  err = check_adm_exists (&exists_already,
                          path,
                          ancestor_path,
                          ancestor_revision,
                          pool);
  if (err)
    return err;

  if (! exists_already)
    {
      err = init_adm (path, repository, ancestor_path, pool);
      if (err)
        return err;
    }
        
  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
