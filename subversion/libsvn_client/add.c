/*
 * add.c:  wrappers around wc add/mkdir functionality.
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
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

#include <string.h>
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_io.h"
#include "client.h"



/*** Code. ***/

static svn_error_t *
add_dir_recursive (const char *dirname,
                   svn_wc_notify_func_t notify_added,
                   void *notify_baton,
                   apr_pool_t *pool)
{
  apr_dir_t *dir;
  apr_finfo_t this_entry;
  svn_error_t *err;
  apr_pool_t *subpool;
  apr_int32_t flags = APR_FINFO_TYPE | APR_FINFO_NAME;

  /* Add this directory to revision control. */
  SVN_ERR (svn_wc_add (dirname,
                       NULL, SVN_INVALID_REVNUM,
                       notify_added, notify_baton, pool));

  /* Create a subpool for iterative memory control. */
  subpool = svn_pool_create (pool);

  /* Read the directory entries one by one and add those things to
     revision control. */
  SVN_ERR (svn_io_dir_open (&dir, dirname, pool));
  for (err = svn_io_dir_read (&this_entry, flags, dir, subpool);
       err == SVN_NO_ERROR;
       err = svn_io_dir_read (&this_entry, flags, dir, subpool))
    {
      const char *fullpath;

      /* Skip over SVN admin directories. */
      if (strcmp (this_entry.name, SVN_WC_ADM_DIR_NAME) == 0)
        continue;

      /* Skip entries for this dir and its parent.  */
      if (this_entry.name[0] == '.'
          && (this_entry.name[1] == '\0'
              || (this_entry.name[1] == '.' && this_entry.name[2] == '\0')))
        continue;

      /* Construct the full path of the entry. */
      fullpath = svn_path_join (dirname, this_entry.name, subpool);

      if (this_entry.filetype == APR_DIR)
        /* Recurse. */
        SVN_ERR (add_dir_recursive (fullpath,
                                    notify_added, notify_baton,
                                    subpool));

      else if (this_entry.filetype == APR_REG)
        SVN_ERR (svn_wc_add (fullpath, NULL, SVN_INVALID_REVNUM,
                             notify_added, notify_baton, subpool));

      /* Clean out the per-iteration pool. */
      svn_pool_clear (subpool);
    }

  /* Check that the loop exited cleanly. */
  if (! (APR_STATUS_IS_ENOENT (err->apr_err)))
    {
      return svn_error_createf
        (err->apr_err, err->src_err, err, subpool,
         "error during recursive add of `%s'", dirname);
    }
  else  /* Yes, it exited cleanly, so close the dir. */
    {
      apr_status_t apr_err = apr_dir_close (dir);
      if (apr_err)
        return svn_error_createf
          (apr_err, 0, NULL, subpool, "error closing dir `%s'", dirname);
    }

  /* Destroy the per-iteration pool. */
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_client_add (const char *path, 
                svn_boolean_t recursive,
                svn_wc_notify_func_t notify_func,
                void *notify_baton,
                apr_pool_t *pool)
{
  enum svn_node_kind kind;
  svn_error_t *err = NULL;

  SVN_ERR (svn_io_check_path (path, &kind, pool));
  if ((kind == svn_node_dir) && (recursive))
    err = add_dir_recursive (path, notify_func, notify_baton, pool);
  else
    err = svn_wc_add (path, NULL, SVN_INVALID_REVNUM,
                      notify_func, notify_baton, pool);

  if (err && (err->apr_err == SVN_ERR_ENTRY_EXISTS))
    return svn_error_quick_wrap 
      (err, "svn warning: Cannot add because entry already exists.");

  return err;
}

svn_error_t *
svn_client_mkdir (svn_client_commit_info_t **commit_info,
                  const char *path,
                  svn_client_auth_baton_t *auth_baton,
                  svn_client_get_commit_log_t log_msg_func,
                  void *log_msg_baton,
                  svn_wc_notify_func_t notify_func,
                  void *notify_baton,
                  apr_pool_t *pool)
{
  /* If this is a URL, we want to drive a commit editor to create this
     directory. */
  if (svn_path_is_url (path))
    {
      /* This is a remote directory creation.  */
      void *ra_baton, *session;
      svn_ra_plugin_t *ra_lib;
      const char *anchor, *target;
      const svn_delta_editor_t *editor;
      void *edit_baton;
      void *root_baton, *dir_baton;
      svn_revnum_t committed_rev = SVN_INVALID_REVNUM;
      const char *committed_date = NULL;
      const char *committed_author = NULL;
      const char *message;

      *commit_info = NULL;

      /* Create a new commit item and add it to the array. */
      if (log_msg_func)
        {
          svn_client_commit_item_t *item;
          apr_array_header_t *commit_items 
            = apr_array_make (pool, 1, sizeof (item));
          
          item = apr_pcalloc (pool, sizeof (*item));
          item->url = apr_pstrdup (pool, path);
          item->state_flags = SVN_CLIENT_COMMIT_ITEM_ADD;
          (*((svn_client_commit_item_t **) apr_array_push (commit_items))) 
            = item;
          
          SVN_ERR ((*log_msg_func) (&message, commit_items, 
                                    log_msg_baton, pool));
          if (! message)
            return SVN_NO_ERROR;
        }
      else
        message = "";

      /* Split the new directory name from its parent URL. */
      svn_path_split_nts (path, &anchor, &target, pool);
      target = svn_path_uri_decode (target, pool);

      /* Get the RA vtable that matches URL. */
      SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
      SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, anchor, pool));

      /* Open a repository session to the URL. Note that we do not have a
         base directory, do not want to store auth data, and do not
         (necessarily) have an admin area for temp files. */
      SVN_ERR (svn_client__open_ra_session (&session, ra_lib, anchor, NULL,
                                            NULL, FALSE, FALSE, TRUE, 
                                            auth_baton, pool));

      /* Fetch RA commit editor */
      SVN_ERR (ra_lib->get_commit_editor (session, &editor, &edit_baton,
                                          &committed_rev,
                                          &committed_date,
                                          &committed_author,
                                          message));

      /* Drive the editor to create the TARGET. */
      SVN_ERR (editor->open_root (edit_baton, SVN_INVALID_REVNUM, pool,
                                  &root_baton));
      SVN_ERR (editor->add_directory (target, root_baton, NULL, 
                                      SVN_INVALID_REVNUM, pool, &dir_baton));
      SVN_ERR (editor->close_directory (dir_baton));
      SVN_ERR (editor->close_directory (root_baton));
      SVN_ERR (editor->close_edit (edit_baton));

      /* Fill in the commit_info structure. */
      *commit_info = svn_client__make_commit_info (committed_rev,
                                                   committed_author,
                                                   committed_date,
                                                   pool);

      /* Free the RA session. */
      SVN_ERR (ra_lib->close (session));

      return SVN_NO_ERROR;
    }

  /* This is a regular "mkdir" + "svn add" */
  SVN_ERR (svn_io_dir_make (path, APR_OS_DEFAULT, pool));
  
  return svn_wc_add (path, NULL, SVN_INVALID_REVNUM,
                     notify_func, notify_baton, pool);
}



/* 
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end: */
