/*
 * export.c:  export a tree.
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

/* ==================================================================== */



/*** Includes. ***/

#include <apr_file_io.h>
#include <apr_md5.h>
#include "svn_types.h"
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_subst.h"
#include "svn_time.h"
#include "svn_md5.h"
#include "client.h"


/*** Code. ***/



static svn_error_t *
copy_versioned_files (const char *from,
                      const char *to,
                      svn_opt_revision_t *revision,
                      svn_boolean_t force,
                      svn_client_ctx_t *ctx,
                      apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  apr_hash_t *dirents;
  svn_wc_adm_access_t *adm_access;
  const svn_wc_entry_t *entry;
  svn_error_t *err;

  SVN_ERR (svn_wc_adm_probe_open (&adm_access, NULL, from, FALSE, 
                                  FALSE, pool));
  err = svn_wc_entry (&entry, from, adm_access, FALSE, subpool);
  SVN_ERR (svn_wc_adm_close (adm_access));
  if (err)
    {
      if (err->apr_err != SVN_ERR_WC_NOT_DIRECTORY)
        return err;
      else
        svn_error_clear (err);
    }

  /* We don't want to copy some random non-versioned directory. */
  if (entry)
    {
      apr_hash_index_t *hi;
      apr_finfo_t finfo;

      SVN_ERR (svn_io_stat (&finfo, from, APR_FINFO_PROT, subpool));

      /* Try to make the new directory.  If this fails because the
         directory already exists, check our FORCE flag to see if we
         care. */
      err = svn_io_dir_make (to, finfo.protection, subpool);
      if (err)
        {
          if (! APR_STATUS_IS_EEXIST (err->apr_err))
            return err;
          if (! force)
            SVN_ERR_W (err,
                       "Destination directory exists.  Please remove the "
                       "directory, or use --force to override this error.");
          else
            svn_error_clear (err);
        }

      SVN_ERR (svn_io_get_dirents (&dirents, from, pool));

      for (hi = apr_hash_first (pool, dirents); hi; hi = apr_hash_next (hi))
        {
          const svn_node_kind_t *type;
          const char *item;
          const void *key;
          void *val;

          apr_hash_this (hi, &key, NULL, &val);

          item = key;
          type = val;

          if (ctx->cancel_func)
            SVN_ERR (ctx->cancel_func (ctx->cancel_baton));

          /* ### We could also invoke ctx->notify_func somewhere in
             ### here... Is it called for, though?  Not sure. */ 

          if (*type == svn_node_dir)
            {
              if (strcmp (item, SVN_WC_ADM_DIR_NAME) == 0)
                {
                  ; /* skip this, it's an administrative directory. */
                }
              else
                {
                  const char *new_from = svn_path_join (from, key, subpool);
                  const char *new_to = svn_path_join (to, key, subpool);

                  SVN_ERR (copy_versioned_files (new_from, new_to, revision,
                                                 force, ctx, subpool));
                }
            }
          else if (*type == svn_node_file)
            {
              const char *copy_from = svn_path_join (from, item, subpool);
              const char *copy_to = svn_path_join (to, item, subpool);

              err = svn_wc_entry (&entry, copy_from, adm_access, FALSE,
                                  subpool);

              if (err)
                {
                  if (err->apr_err != SVN_ERR_WC_NOT_FILE)
                    return err;
                  svn_error_clear (err);
                }

              /* don't copy it if it isn't versioned. */
              if (entry)
                {
                  svn_subst_keywords_t kw = { 0 };
                  svn_subst_eol_style_t style;
                  apr_hash_t *props;
                  const char *base;
                  svn_string_t *eol_style;
                  svn_string_t *keywords;
                  svn_string_t *executable;
                  const char *eol = NULL;
                  svn_boolean_t local_mod = FALSE;
                  apr_time_t time;
    
                  if (revision->kind != svn_opt_revision_working)
                    {
                      SVN_ERR (svn_wc_get_pristine_copy_path 
                               (copy_from, &base, subpool));
                      SVN_ERR (svn_wc_get_prop_diffs
                               (NULL, &props, copy_from, adm_access, subpool));
                    }
                  else
                    {
                      svn_wc_status_t *status;

                      base = copy_from;
                      SVN_ERR (svn_wc_prop_list (&props, copy_from,
                                                 adm_access, subpool));
                      SVN_ERR (svn_wc_status (&status, copy_from, adm_access, subpool));
                      if (status->text_status != svn_wc_status_normal)
                        local_mod = TRUE;
                    }

                  eol_style = apr_hash_get (props, SVN_PROP_EOL_STYLE,
                                            APR_HASH_KEY_STRING);

                  keywords = apr_hash_get (props, SVN_PROP_KEYWORDS,
                                           APR_HASH_KEY_STRING);

                  executable = apr_hash_get (props, SVN_PROP_EXECUTABLE,
                                             APR_HASH_KEY_STRING);

                  if (eol_style)
                    svn_subst_eol_style_from_value (&style, &eol,
                                                    eol_style->data);

                  if (local_mod)
                    /* Use the modified time from the working copy if the file */
                    SVN_ERR (svn_io_file_affected_time (&time, copy_from, subpool));
                  else
                    time = entry->cmt_date;

                  if (keywords)
                    {
                      const char *fmt;
                      const char *author;

                      if (local_mod)
                        {
                          /* For locally modified files, we'll append an 'M'
                             to the revision number, and set the author to
                             "(local)" since we can't always determine the
                             current user's username */
                          fmt = "%" SVN_REVNUM_T_FMT "M";
                          author = "(local)";
                        }
                      else
                        {
                          fmt = "%" SVN_REVNUM_T_FMT;
                          author = entry->cmt_author;
                        }

                      SVN_ERR (svn_subst_build_keywords 
                               (&kw, keywords->data,
                                apr_psprintf (pool, 
                                              fmt,
                                              entry->cmt_rev),
                                entry->url,
                                time,
                                author,
                                subpool));
                    }

                  SVN_ERR (svn_subst_copy_and_translate (base, copy_to,
                                                         eol, FALSE,
                                                         &kw, TRUE,
                                                         subpool));
                  if (executable)
                    SVN_ERR (svn_io_set_file_executable (copy_to, TRUE, FALSE, subpool));
                
                  SVN_ERR (svn_io_set_file_affected_time (time, copy_to, subpool));
                }
            }

          svn_pool_clear (subpool);
        }
    }

  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}


/* Abstraction of open_root.
 *
 * Create PATH if it does not exist and is not obstructed, and invoke
 * NOTIFY_FUNC with NOTIFY_BATON on PATH.
 *
 * If PATH exists but is a file, then error with SVN_ERR_WC_NOT_DIRECTORY.
 *
 * If PATH is a already a directory, then error with
 * SVN_ERR_WC_OBSTRUCTED_UPDATE, unless FORCE, in which case just
 * export into PATH with no error.
 */
static svn_error_t *
open_root_internal (const char *path,
                    svn_boolean_t force,
                    svn_wc_notify_func_t notify_func,
                    void *notify_baton,
                    apr_pool_t *pool)
{
  svn_node_kind_t kind;
  
  SVN_ERR (svn_io_check_path (path, &kind, pool));
  if (kind == svn_node_none)
    SVN_ERR (svn_io_dir_make (path, APR_OS_DEFAULT, pool));
  else if (kind == svn_node_file)
    return svn_error_create (SVN_ERR_WC_NOT_DIRECTORY, NULL, path);
  else if ((kind != svn_node_dir) || (! force))
    return svn_error_create (SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL, path);

  if (notify_func)
    (*notify_func) (notify_baton,
                    path,
                    svn_wc_notify_update_add,
                    svn_node_dir,
                    NULL,
                    svn_wc_notify_state_unknown,
                    svn_wc_notify_state_unknown,
                    SVN_INVALID_REVNUM);

  return SVN_NO_ERROR;
}


/* ---------------------------------------------------------------------- */

/*** A dedicated 'export' editor, which does no .svn/ accounting.  ***/


struct edit_baton
{
  const char *root_path;
  const char *root_url;
  svn_boolean_t force;
  svn_revnum_t *target_revision;

  svn_wc_notify_func_t notify_func;
  void *notify_baton;
};


struct file_baton
{
  struct edit_baton *edit_baton;

  const char *path;
  const char *tmppath;

  /* We need to keep this around so we can explicitly close it in close_file, 
     thus flushing it's output to disk so we can copy and translate it. */
  apr_file_t *tmp_file;

  /* The MD5 digest of the file's fulltext.  This is all zeros until
     the last textdelta window handler call returns. */
  unsigned char text_digest[APR_MD5_DIGESTSIZE];

  /* The three svn: properties we might actually care about. */
  const svn_string_t *eol_style_val;
  const svn_string_t *keywords_val;
  const svn_string_t *executable_val;

  /* Any keyword vals to be substituted */
  const char *revision;
  const char *url;
  const char *author;
  apr_time_t date;

  /* Pool associated with this baton. */
  apr_pool_t *pool;
};


struct handler_baton
{
  svn_txdelta_window_handler_t apply_handler;
  void *apply_baton;
  apr_pool_t *pool;
  const char *tmppath;
};


static svn_error_t *
set_target_revision (void *edit_baton, 
                     svn_revnum_t target_revision,
                     apr_pool_t *pool)
{
  struct edit_baton *eb = edit_baton;

  /* Stashing a target_revision in the baton */
  *(eb->target_revision) = target_revision;
  return SVN_NO_ERROR;
}



/* Just ensure that the main export directory exists. */
static svn_error_t *
open_root (void *edit_baton,
           svn_revnum_t base_revision,
           apr_pool_t *pool,
           void **root_baton)
{
  struct edit_baton *eb = edit_baton;  

  SVN_ERR (open_root_internal (eb->root_path, eb->force,
                               eb->notify_func, eb->notify_baton, pool));

  *root_baton = eb;
  return SVN_NO_ERROR;
}


/* Ensure the directory exists, and send feedback. */
static svn_error_t *
add_directory (const char *path,
               void *parent_baton,
               const char *copyfrom_path,
               svn_revnum_t copyfrom_revision,
               apr_pool_t *pool,
               void **baton)
{
  struct edit_baton *eb = parent_baton;
  const char *full_path = svn_path_join (eb->root_path,
                                         path, pool);
  svn_node_kind_t kind;

  SVN_ERR (svn_io_check_path (full_path, &kind, pool));
  if ( kind == svn_node_none )
      SVN_ERR (svn_io_dir_make (full_path, APR_OS_DEFAULT, pool));
  else if (kind == svn_node_file)
      return svn_error_create (SVN_ERR_WC_NOT_DIRECTORY,
                               NULL, full_path);
  else if (! (kind == svn_node_dir && eb->force))
      return svn_error_create (SVN_ERR_WC_OBSTRUCTED_UPDATE,
                               NULL, full_path);

  if (eb->notify_func)
    (*eb->notify_func) (eb->notify_baton,
                        full_path,
                        svn_wc_notify_update_add,
                        svn_node_dir,
                        NULL,
                        svn_wc_notify_state_unknown,
                        svn_wc_notify_state_unknown,
                        SVN_INVALID_REVNUM);

  *baton = eb;
  return SVN_NO_ERROR;
}


/* Build a file baton. */
static svn_error_t *
add_file (const char *path,
          void *parent_baton,
          const char *copyfrom_path,
          svn_revnum_t copyfrom_revision,
          apr_pool_t *pool,
          void **baton)
{
  struct edit_baton *eb = parent_baton;
  struct file_baton *fb = apr_pcalloc (pool, sizeof(*fb));
  const char *full_path = svn_path_join (eb->root_path, path, pool);
  const char *full_url = svn_path_join (eb->root_url, path, pool);

  fb->edit_baton = eb;
  fb->path = full_path;
  fb->url = full_url;
  fb->pool = pool;

  *baton = fb;
  return SVN_NO_ERROR;
}


static svn_error_t *
window_handler (svn_txdelta_window_t *window, void *baton)
{
  struct handler_baton *hb = baton;
  svn_error_t *err;

  err = hb->apply_handler (window, hb->apply_baton);
  if (err)
    {
      /* We failed to apply the patch; clean up the temporary file.  */
      apr_file_remove (hb->tmppath, hb->pool);
    }

  return err;
}



/* Write incoming data into the tmpfile stream */
static svn_error_t *
apply_textdelta (void *file_baton,
                 const char *base_checksum,
                 apr_pool_t *pool,
                 svn_txdelta_window_handler_t *handler,
                 void **handler_baton)
{
  struct file_baton *fb = file_baton;
  struct handler_baton *hb = apr_palloc (pool, sizeof (*hb));

  SVN_ERR (svn_io_open_unique_file (&fb->tmp_file, &(fb->tmppath),
                                    fb->path, ".tmp", FALSE, fb->pool));

  hb->pool = pool;
  hb->tmppath = fb->tmppath;

  svn_txdelta_apply (svn_stream_empty (pool),
                     svn_stream_from_aprfile (fb->tmp_file, pool),
                     fb->text_digest, NULL, pool,
                     &hb->apply_handler, &hb->apply_baton);

  *handler_baton = hb;
  *handler = window_handler;
  return SVN_NO_ERROR;
}


static svn_error_t *
change_file_prop (void *file_baton,
                  const char *name,
                  const svn_string_t *value,
                  apr_pool_t *pool)
{
  struct file_baton *fb = file_baton;

  if (! value)
    return SVN_NO_ERROR;

  /* Store only the magic three properties. */
  if (strcmp (name, SVN_PROP_EOL_STYLE) == 0)
    fb->eol_style_val = svn_string_dup (value, fb->pool);

  else if (strcmp (name, SVN_PROP_KEYWORDS) == 0)
    fb->keywords_val = svn_string_dup (value, fb->pool);

  else if (strcmp (name, SVN_PROP_EXECUTABLE) == 0)
    fb->executable_val = svn_string_dup (value, fb->pool);

  /* Try to fill out the baton's keywords-structure too. */
  else if (strcmp (name, SVN_PROP_ENTRY_COMMITTED_REV) == 0)
    fb->revision = apr_pstrdup (fb->pool, value->data);

  else if (strcmp (name, SVN_PROP_ENTRY_COMMITTED_DATE) == 0)
      SVN_ERR (svn_time_from_cstring (&fb->date, value->data, fb->pool));

  else if (strcmp (name, SVN_PROP_ENTRY_LAST_AUTHOR) == 0)
    fb->author = apr_pstrdup (fb->pool, value->data);

  return SVN_NO_ERROR;
}



/* Move the tmpfile to file, and send feedback. */
static svn_error_t *
close_file (void *file_baton,
            const char *text_checksum,
            apr_pool_t *pool)
{
  struct file_baton *fb = file_baton;

  /* Was a txdelta even sent? */
  if (! fb->tmppath)
    return SVN_NO_ERROR;

  SVN_ERR (svn_io_file_close (fb->tmp_file, fb->pool));

  if (text_checksum)
    {
      const char *actual_checksum
        = svn_md5_digest_to_cstring (fb->text_digest, pool);

      if (actual_checksum && (strcmp (text_checksum, actual_checksum) != 0))
        {
          return svn_error_createf
            (SVN_ERR_CHECKSUM_MISMATCH, NULL,
             "Checksum mismatch for '%s'; expected: '%s', actual: '%s'",
             fb->path, text_checksum, actual_checksum);
        }
    }

  if ((! fb->eol_style_val) && (! fb->keywords_val))
    {
      SVN_ERR (svn_io_file_rename (fb->tmppath, fb->path, pool));
    }
  else
    {
      svn_subst_eol_style_t style;
      const char *eol;
      svn_subst_keywords_t final_kw = {0};

      if (fb->eol_style_val)
        svn_subst_eol_style_from_value (&style, &eol, fb->eol_style_val->data);

      if (fb->keywords_val)
        SVN_ERR (svn_subst_build_keywords (&final_kw, fb->keywords_val->data, 
                                           fb->revision, fb->url, fb->date, 
                                           fb->author, pool));

      SVN_ERR (svn_subst_copy_and_translate
               (fb->tmppath, fb->path,
                fb->eol_style_val ? eol : NULL,
                fb->eol_style_val ? TRUE : FALSE, /* repair */
                fb->keywords_val ? &final_kw : NULL,
                fb->keywords_val ? TRUE : FALSE, /* expand */
                pool));

      SVN_ERR (svn_io_remove_file (fb->tmppath, pool));
    }
      
  if (fb->executable_val)
    SVN_ERR (svn_io_set_file_executable (fb->path, TRUE, FALSE, pool));

  if (fb->date)
    SVN_ERR (svn_io_set_file_affected_time (fb->date, fb->path, pool));

  if (fb->edit_baton->notify_func)
    (*fb->edit_baton->notify_func) (fb->edit_baton->notify_baton,
                                    fb->path,
                                    svn_wc_notify_update_add,
                                    svn_node_file,
                                    NULL,
                                    svn_wc_notify_state_unknown,
                                    svn_wc_notify_state_unknown,
                                    SVN_INVALID_REVNUM);

  return SVN_NO_ERROR;
}



/*** Public Interfaces ***/

svn_error_t *
svn_client_export (svn_revnum_t *result_rev,
                   const char *from,
                   const char *to,
                   svn_opt_revision_t *revision,
                   svn_boolean_t force, 
                   svn_client_ctx_t *ctx,
                   apr_pool_t *pool)
{
  svn_revnum_t edit_revision = SVN_INVALID_REVNUM;
  svn_boolean_t use_ra = FALSE;
  const char *URL;

  if (! svn_path_is_url (from) &&
      (revision->kind != svn_opt_revision_base) &&
      (revision->kind != svn_opt_revision_committed) &&
      (revision->kind != svn_opt_revision_working))
    {
      if (revision->kind == svn_opt_revision_unspecified)
        /* Default to WORKING in the case that we have
           been given a working copy path */
        revision->kind = svn_opt_revision_working;
      else
        {
          use_ra = TRUE;
          SVN_ERR (svn_client_url_from_path (&URL, from, pool));
          if (! from)
            return svn_error_createf (SVN_ERR_ENTRY_MISSING_URL, NULL,
                                      "'%s' has no URL", from);
        }
    }
  else
    URL = svn_path_canonicalize (from, pool);

  if (svn_path_is_url (from) || use_ra)
    {
      svn_revnum_t revnum;
      void *ra_baton, *session;
      svn_ra_plugin_t *ra_lib;
      void *edit_baton;
      svn_node_kind_t kind;
      const svn_delta_editor_t *export_editor;
      const svn_ra_reporter_t *reporter;
      void *report_baton;
      struct edit_baton *eb = apr_pcalloc (pool, sizeof (*eb));
      svn_delta_editor_t *editor = svn_delta_default_editor (pool);

      eb->root_path = to;
      eb->root_url = URL;
      eb->force = force;
      eb->target_revision = &edit_revision;
      eb->notify_func = ctx->notify_func;
      eb->notify_baton = ctx->notify_baton;

      editor->set_target_revision = set_target_revision;
      editor->open_root = open_root;
      editor->add_directory = add_directory;
      editor->add_file = add_file;
      editor->apply_textdelta = apply_textdelta;
      editor->close_file = close_file;
      editor->change_file_prop = change_file_prop;
      
      SVN_ERR (svn_delta_get_cancellation_editor (ctx->cancel_func,
                                                  ctx->cancel_baton,
                                                  editor,
                                                  eb,
                                                  &export_editor,
                                                  &edit_baton,
                                                  pool));
  
      SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
      SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, URL, pool));

      SVN_ERR (svn_client__open_ra_session (&session, ra_lib, URL, NULL,
                                            NULL, NULL, FALSE, TRUE,
                                            ctx, pool));

      /* Unfortunately, it's not kosher to pass an invalid revnum into
         set_path(), so we actually need to convert it to HEAD. */
      if (revision->kind == svn_opt_revision_unspecified)
        revision->kind = svn_opt_revision_head;
      SVN_ERR (svn_client__get_revision_number
               (&revnum, ra_lib, session, revision, from, pool));

      /* Manufacture a basic 'report' to the update reporter. */
      SVN_ERR (ra_lib->do_update (session,
                                  &reporter, &report_baton,
                                  revnum,
                                  NULL, /* no sub-target */
                                  TRUE, /* recurse */
                                  export_editor, edit_baton, pool));

      SVN_ERR (reporter->set_path (report_baton, "", revnum,
                                   TRUE, /* "help, my dir is empty!" */
                                   pool));

      SVN_ERR (reporter->finish_report (report_baton));               

      /* Special case: Due to our sly export/checkout method of
       * updating an empty directory, no target will have been created
       * if the exported item is itself an empty directory
       * (export_editor->open_root never gets called, because there
       * are no "changes" to make to the empty dir we reported to the
       * repository).
       *
       * So we just create the empty dir manually; but we do it via
       * open_root_internal(), in order to get proper notification.
       */
      SVN_ERR (svn_io_check_path (to, &kind, pool));
      if (kind == svn_node_none)
        SVN_ERR (open_root_internal
                 (to, force, ctx->notify_func, ctx->notify_baton, pool));
    }
  else
    {
      /* just copy the contents of the working copy into the target path. */
      SVN_ERR (copy_versioned_files (from, to, revision, force, ctx, pool));
    }

  if (ctx->notify_func)
    (*ctx->notify_func) (ctx->notify_baton,
                         to,
                         svn_wc_notify_update_completed,
                         svn_node_unknown,
                         NULL,
                         svn_wc_notify_state_unknown,
                         svn_wc_notify_state_unknown,
                         edit_revision);

  if (result_rev)
    *result_rev = edit_revision;

  return SVN_NO_ERROR;
}


