/*
 * adm_crawler.c:  report local WC mods to an Editor.
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

/* ==================================================================== */


#include <string.h>

#include <apr_pools.h>
#include <apr_file_io.h>
#include <apr_hash.h>
#include <apr_md5.h>

#include <assert.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_wc.h"
#include "svn_io.h"
#include "svn_md5.h"
#include "svn_base64.h"
#include "svn_delta.h"
#include "svn_path.h"

#include "wc.h"
#include "adm_files.h"
#include "props.h"
#include "translate.h"
#include "entries.h"

#include "svn_private_config.h"


/* Helper for report_revisions().
   
   Perform an atomic restoration of the file FILE_PATH; that is, copy
   the file's text-base to the administrative tmp area, and then move
   that file to FILE_PATH with possible translations/expansions.  If
   USE_COMMIT_TIMES is set, then set working file's timestamp to
   last-commit-time.  Either way, set entry-timestamp to match that of
   the working file when all is finished. */
static svn_error_t *
restore_file (const char *file_path,
              svn_wc_adm_access_t *adm_access,
              svn_boolean_t use_commit_times,
              apr_pool_t *pool)
{
  const char *text_base_path, *tmp_text_base_path;
  svn_subst_keywords_t *keywords;
  const char *eol;
  const svn_wc_entry_t *entry;
  svn_wc_entry_t newentry;
  apr_time_t tstamp;
  const char *bname;
  apr_uint32_t modify_flags = 0;
  svn_boolean_t special;

  text_base_path = svn_wc__text_base_path (file_path, FALSE, pool);
  tmp_text_base_path = svn_wc__text_base_path (file_path, TRUE, pool);
  bname = svn_path_basename (file_path, pool);

  SVN_ERR (svn_io_copy_file (text_base_path, tmp_text_base_path,
                             FALSE, pool));

  SVN_ERR (svn_wc__get_eol_style (NULL, &eol, file_path, adm_access, pool));
  SVN_ERR (svn_wc__get_keywords (&keywords,
                                 file_path, adm_access, NULL, pool));
  SVN_ERR (svn_wc__get_special (&special, file_path, adm_access, pool));
                                
  
  /* When copying the tmp-text-base out to the working copy, make
     sure to do any eol translations or keyword substitutions,
     as dictated by the property values.  If these properties
     are turned off, then this is just a normal copy. */
  SVN_ERR (svn_subst_copy_and_translate2 (tmp_text_base_path,
                                          file_path,
                                          eol, FALSE, /* don't repair */
                                          keywords,
                                          TRUE, /* expand keywords */
                                          special,
                                          pool));
  
  SVN_ERR (svn_io_remove_file (tmp_text_base_path, pool));

  SVN_ERR (svn_wc__maybe_set_read_only (NULL, file_path, adm_access, pool));

  /* If necessary, tweak the new working file's executable bit. */
  SVN_ERR (svn_wc__maybe_set_executable (NULL, file_path, adm_access, pool));

  /* Remove any text conflict */
  SVN_ERR (svn_wc_resolved_conflict2 (file_path, adm_access, TRUE, FALSE,
                                      FALSE, NULL, NULL, NULL, NULL, pool));

  SVN_ERR (svn_wc_entry (&entry, file_path, adm_access, FALSE, pool));
  assert(entry != NULL);

  /* Possibly set timestamp to last-commit-time. */
  if (use_commit_times && (! special))
    {
      SVN_ERR (svn_io_set_file_affected_time (entry->cmt_date,
                                              file_path, pool));
      tstamp = entry->cmt_date;
    }
  else
    {
      SVN_ERR (svn_io_file_affected_time (&tstamp, file_path, pool));
    }
  
  /* Modify our entry's text-timestamp to match the working file. */
  modify_flags |= SVN_WC__ENTRY_MODIFY_TEXT_TIME;
  newentry.text_time = tstamp;
  SVN_ERR (svn_wc__entry_modify (adm_access, bname,
                                 &newentry, modify_flags,
                                 TRUE /* do_sync now */, pool));

  return SVN_NO_ERROR;
}


/* The recursive crawler that describes a mixed-revision working
   copy to an RA layer.  Used to initiate updates.

   This is a depth-first recursive walk of DIR_PATH under ADM_ACCESS.
   Look at each entry and check if its revision is different than
   DIR_REV.  If so, report this fact to REPORTER.  If an entry is
   missing from disk, report its absence to REPORTER.  If an entry has
   a different URL than expected, report that to REPORTER.  Finally,
   if REPORT_EVERYTHING is set, then report all children unconditionally.

   If TRAVERSAL_INFO is non-null, record this directory's
   value of svn:externals in both TRAVERSAL_INFO->externals_old and
   TRAVERSAL_INFO->externals_new, using wc_path + dir_path as the key,
   and the raw (unparsed) value of the property as the value.  NOTE:
   We set the value in both places, because its absence in just one or
   the other place signals that the property was added or deleted;
   thus, storing it in both places signals that it is present and, by
   default, unchanged.

   If RESTORE_FILES is set, then unexpectedly missing working files
   will be restored from text-base and NOTIFY_FUNC/NOTIFY_BATON
   will be called to report the restoration.  USE_COMMIT_TIMES is
   passed to restore_file() helper. */
static svn_error_t *
report_revisions (svn_wc_adm_access_t *adm_access,
                  const char *dir_path,
                  svn_revnum_t dir_rev,
                  const svn_ra_reporter2_t *reporter,
                  void *report_baton,
                  svn_wc_notify_func2_t notify_func,
                  void *notify_baton,
                  svn_boolean_t restore_files,
                  svn_boolean_t recurse,
                  svn_boolean_t report_everything,
                  svn_boolean_t use_commit_times,
                  svn_wc_traversal_info_t *traversal_info,
                  apr_pool_t *pool)
{
  apr_hash_t *entries, *dirents;
  apr_hash_index_t *hi;
  apr_pool_t *subpool = svn_pool_create (pool), *iterpool;
  const svn_wc_entry_t *dot_entry;
  const char *this_url, *this_path, *full_path, *this_full_path;
  svn_wc_adm_access_t *dir_access;
  svn_wc_notify_t *notify;

  /* Get both the SVN Entries and the actual on-disk entries.   Also
     notice that we're picking up hidden entries too. */
  full_path = svn_path_join (svn_wc_adm_access_path (adm_access), 
                             dir_path, subpool);
  SVN_ERR (svn_wc_adm_retrieve (&dir_access, adm_access, full_path, subpool));
  SVN_ERR (svn_wc_entries_read (&entries, dir_access, TRUE, subpool));
  SVN_ERR (svn_io_get_dirents2 (&dirents, full_path, subpool));
  
  /*** Do the real reporting and recursing. ***/
  
  /* First, look at "this dir" to see what its URL is. */
  dot_entry = apr_hash_get (entries, SVN_WC_ENTRY_THIS_DIR, 
                            APR_HASH_KEY_STRING);

  /* If "this dir" has "svn:externals" property set on it, store its name
     in traversal_info. */
  if (traversal_info)
    {
      const svn_string_t *val;
      SVN_ERR (svn_wc_prop_get (&val, SVN_PROP_EXTERNALS, full_path, adm_access,
                                subpool));
      if (val)
        {
          apr_pool_t *dup_pool = traversal_info->pool;
          const char *dup_path = apr_pstrdup (dup_pool, full_path);
          const char *dup_val = apr_pstrmemdup (dup_pool, val->data, val->len);
          apr_hash_set (traversal_info->externals_old,
                        dup_path, APR_HASH_KEY_STRING, dup_val);
          apr_hash_set (traversal_info->externals_new,
                        dup_path, APR_HASH_KEY_STRING, dup_val);
        }
    }

  /* Looping over current directory's SVN entries: */
  iterpool = svn_pool_create (subpool);

  for (hi = apr_hash_first (subpool, entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      apr_ssize_t klen;
      void *val;
      const svn_wc_entry_t *current_entry; 
      svn_io_dirent_t *dirent;
      svn_node_kind_t dirent_kind;
      svn_boolean_t missing = FALSE;

      /* Clear the iteration subpool here because the loop has a bunch
         of 'continue' jump statements. */
      svn_pool_clear (iterpool);

      /* Get the next entry */
      apr_hash_this (hi, &key, &klen, &val);
      current_entry = val;

      /* Compute the name of the entry.  Skip THIS_DIR altogether. */
      if (! strcmp (key, SVN_WC_ENTRY_THIS_DIR))
        continue;

      /* Compute the paths and URLs we need. */
      this_url = svn_path_join (dot_entry->url, 
                                svn_path_uri_encode (key, iterpool), iterpool);
      this_path = svn_path_join (dir_path, key, iterpool);
      this_full_path = svn_path_join (full_path, key, iterpool);

      /*** The Big Tests: ***/

      /* If the entry is 'deleted' or 'absent', make sure the server
         knows it's gone... */
      if (current_entry->deleted || current_entry->absent)
        {
          /* ...unless we're reporting everything, in which case it's already
             missing on the server.  */
          if (! report_everything)
            SVN_ERR (reporter->delete_path (report_baton, this_path, iterpool));
          continue;
        }
      
      /* Is the entry on disk?  Set a flag if not. */
      dirent = apr_hash_get (dirents, key, klen);
      if (! dirent)
        {
          /* It is possible on a case insensitive system that the
             entry is not really missing, so we call our trusty but
             expensive friend svn_io_check_path to be sure. */
          SVN_ERR (svn_io_check_path (this_full_path, &dirent_kind,
                                      iterpool));
          if (dirent_kind == svn_node_none)
            missing = TRUE;
        }
      else
        dirent_kind = dirent->kind;
      
      /* From here on out, ignore any entry scheduled for addition */
      if (current_entry->schedule == svn_wc_schedule_add)
        continue;
      
      /*** Files ***/
      if (current_entry->kind == svn_node_file) 
        {
          /* If the dirent changed kind, report it as missing and
             move on to the next entry.  Later on, the update
             editor will return an 'obstructed update' error.  :) */
          if ((dirent_kind != svn_node_none)
              && (dirent_kind != svn_node_file)
              && (! report_everything))
            {
              SVN_ERR (reporter->delete_path (report_baton, this_path, 
                                              iterpool));
              continue;
            }

          /* If the item is missing from disk, and we're supposed to
             restore missing things, and it isn't missing as a result
             of a scheduling operation, then ... */
          if (missing 
              && restore_files 
              && (current_entry->schedule != svn_wc_schedule_delete)
              && (current_entry->schedule != svn_wc_schedule_replace))
            {
              /* ... recreate file from text-base, and ... */
              SVN_ERR (restore_file (this_full_path, dir_access,
                                     use_commit_times, iterpool));
              
              /* ... report the restoration to the caller.  */
              if (notify_func != NULL)
                {
                  notify = svn_wc_create_notify (this_full_path,
                                                 svn_wc_notify_restore,
                                                 iterpool);
                  notify->kind = svn_node_file;
                  (*notify_func) (notify_baton, notify, iterpool);
                }
            }

          if (report_everything)
            {
              /* Report the file unconditionally, one way or another. */
              if (strcmp (current_entry->url, this_url) != 0)
                SVN_ERR (reporter->link_path (report_baton, this_path,
                                              current_entry->url,
                                              current_entry->revision,
                                              FALSE, current_entry->lock_token,
                                              iterpool));
              else
                SVN_ERR (reporter->set_path (report_baton, this_path,
                                             current_entry->revision,
                                             FALSE, current_entry->lock_token,
                                             iterpool));              
            }

          /* Possibly report a disjoint URL ... */
          else if ((current_entry->schedule != svn_wc_schedule_add)
                   && (current_entry->schedule != svn_wc_schedule_replace)
                   && (strcmp (current_entry->url, this_url) != 0))
            SVN_ERR (reporter->link_path (report_baton,
                                          this_path,
                                          current_entry->url,
                                          current_entry->revision,
                                          FALSE,
                                          current_entry->lock_token,
                                          iterpool));
          /* ... or perhaps just a differing revision or lock token. */
          else if (current_entry->revision !=  dir_rev
                   || current_entry->lock_token)
            SVN_ERR (reporter->set_path (report_baton,
                                         this_path,
                                         current_entry->revision,
                                         FALSE,
                                         current_entry->lock_token,
                                         iterpool));
        } /* end file case */
      
      /*** Directories (in recursive mode) ***/
      else if (current_entry->kind == svn_node_dir && recurse)
        {
          svn_wc_adm_access_t *subdir_access;
          const svn_wc_entry_t *subdir_entry;

          /* If a directory is missing from disk, we have no way to
             recreate it locally, so report as missing and move
             along.  Again, don't bother if we're reporting
             everything, because the dir is already missing on the server. */
          if (missing)
            {
              if (! report_everything)
                SVN_ERR (reporter->delete_path (report_baton, this_path,
                                                iterpool));
              continue;
            }
          
          /* No excuses here.  If the user changed a versioned
             directory into something else, the working copy is hosed.
             It can't receive updates within this dir anymore.  Throw
             a real error. */
          if ((dirent_kind != svn_node_none) && (dirent_kind != svn_node_dir))
            {
              return svn_error_createf
                (SVN_ERR_WC_OBSTRUCTED_UPDATE, NULL,
                 _("The entry '%s' is no longer a directory; "
                   "remove the entry before updating"),
                 svn_path_local_style (this_path, iterpool));
            }

          /* We need to read the full entry of the directory from its
             own "this dir", if available. */
          SVN_ERR (svn_wc_adm_retrieve (&subdir_access, adm_access,
                                        this_full_path, iterpool));
          SVN_ERR (svn_wc_entry (&subdir_entry, this_full_path, subdir_access,
                                 TRUE, iterpool));

          if (report_everything)
            {
              /* Report the dir unconditionally, one way or another. */
              if (strcmp (subdir_entry->url, this_url) != 0)
                SVN_ERR (reporter->link_path (report_baton, this_path,
                                              subdir_entry->url,
                                              subdir_entry->revision,
                                              subdir_entry->incomplete,
                                              subdir_entry->lock_token,
                                              iterpool));
              else
                SVN_ERR (reporter->set_path (report_baton, this_path,
                                             subdir_entry->revision,
                                             subdir_entry->incomplete,
                                             subdir_entry->lock_token,
                                             iterpool));              
            }

          /* Possibly report a disjoint URL ... */
          else if (strcmp (subdir_entry->url, this_url) != 0)
            SVN_ERR (reporter->link_path (report_baton,
                                          this_path,
                                          subdir_entry->url,
                                          subdir_entry->revision,
                                          subdir_entry->incomplete,
                                          subdir_entry->lock_token,
                                          iterpool));
          /* ... or perhaps just a differing revision, lock token or
             incomplete subdir. */
          else if (subdir_entry->revision != dir_rev
                   || subdir_entry->lock_token
                   || subdir_entry->incomplete)
            SVN_ERR (reporter->set_path (report_baton,
                                         this_path,
                                         subdir_entry->revision,
                                         subdir_entry->incomplete,
                                         subdir_entry->lock_token,
                                         iterpool));

          /* Recurse. */
          SVN_ERR (report_revisions (adm_access, this_path,
                                     subdir_entry->revision,
                                     reporter, report_baton,
                                     notify_func, notify_baton,
                                     restore_files, recurse,
                                     subdir_entry->incomplete,
                                     use_commit_times,
                                     traversal_info,
                                     iterpool));
        } /* end directory case */
    } /* end main entries loop */

  /* We're done examining this dir's entries, so free everything. */
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}


/*------------------------------------------------------------------*/
/*** Public Interfaces ***/


/* This is the main driver of the working copy state "reporter", used
   for updates. */
svn_error_t *
svn_wc_crawl_revisions2 (const char *path,
                         svn_wc_adm_access_t *adm_access,
                         const svn_ra_reporter2_t *reporter,
                         void *report_baton,
                         svn_boolean_t restore_files,
                         svn_boolean_t recurse,
                         svn_boolean_t use_commit_times,
                         svn_wc_notify_func2_t notify_func,
                         void *notify_baton,
                         svn_wc_traversal_info_t *traversal_info,
                         apr_pool_t *pool)
{
  svn_error_t *fserr, *err = SVN_NO_ERROR;
  const svn_wc_entry_t *entry;
  svn_revnum_t base_rev = SVN_INVALID_REVNUM;
  svn_boolean_t missing = FALSE;
  const svn_wc_entry_t *parent_entry = NULL;
  svn_wc_notify_t *notify;

  /* The first thing we do is get the base_rev from the working copy's
     ROOT_DIRECTORY.  This is the first revnum that entries will be
     compared to. */
  SVN_ERR (svn_wc_entry (&entry, path, adm_access, FALSE, pool));

  if ((! entry) || ((entry->schedule == svn_wc_schedule_add)
                    && (entry->kind == svn_node_dir)))
    {
      SVN_ERR (svn_wc_entry (&parent_entry,
                             svn_path_dirname (path, pool),
                             adm_access,
                             FALSE, pool));
      base_rev = parent_entry->revision;
      SVN_ERR (reporter->set_path (report_baton, "", base_rev,
                                   entry ? entry->incomplete : TRUE, 
                                   NULL, pool));
      SVN_ERR (reporter->delete_path (report_baton, "", pool)); 

      /* Finish the report, which causes the update editor to be
         driven. */
      SVN_ERR (reporter->finish_report (report_baton, pool));

      return SVN_NO_ERROR;
    }

  base_rev = entry->revision;
  if (base_rev == SVN_INVALID_REVNUM)
    {
      SVN_ERR (svn_wc_entry (&parent_entry, 
                             svn_path_dirname (path, pool),
                             adm_access,
                             FALSE, pool));
      base_rev = parent_entry->revision;
    }

  /* The first call to the reporter merely informs it that the
     top-level directory being updated is at BASE_REV.  Its PATH
     argument is ignored. */
  SVN_ERR (reporter->set_path (report_baton, "", base_rev,
                               entry->incomplete , /* start_empty ? */
                               NULL, pool));

  if (entry->schedule != svn_wc_schedule_delete)
    {
      apr_finfo_t info;
      err = svn_io_stat (&info, path, APR_FINFO_MIN, pool);
      if (err)
        {
          if (APR_STATUS_IS_ENOENT(err->apr_err))
            missing = TRUE;
          svn_error_clear (err);
          err = NULL;
        }
    }

  if (entry->kind == svn_node_dir)
    {
      if (missing)
        {
          /* Always report directories as missing;  we can't recreate
             them locally. */
          err = reporter->delete_path (report_baton, "", pool);
          if (err)
            goto abort_report;
        }
      else 
        {
          /* Recursively crawl ROOT_DIRECTORY and report differing
             revisions. */
          err = report_revisions (adm_access,
                                  "",
                                  base_rev,
                                  reporter, report_baton,
                                  notify_func, notify_baton,
                                  restore_files, recurse,
                                  entry->incomplete,
                                  use_commit_times,
                                  traversal_info,
                                  pool);
          if (err)
            goto abort_report;
        }
    }

  else if (entry->kind == svn_node_file)
    {
      const char *pdir, *bname;

      if (missing && restore_files)
        {
          /* Recreate file from text-base. */
          err = restore_file (path, adm_access, use_commit_times, pool);
          if (err)
            goto abort_report;

          /* Report the restoration to the caller. */
          if (notify_func != NULL)
            {
              notify = svn_wc_create_notify (path, svn_wc_notify_restore,
                                             pool);
              notify->kind = svn_node_file;
              (*notify_func) (notify_baton, notify, pool);
            }
        }
      
      /* Split PATH into parent PDIR and basename BNAME. */
      svn_path_split (path, &pdir, &bname, pool);
      if (! parent_entry)
        {
          err = svn_wc_entry (&parent_entry, pdir, adm_access, FALSE, pool);
          if (err)
            goto abort_report;
        }
      
      if (parent_entry 
          && parent_entry->url 
          && entry->url
          && strcmp (entry->url, 
                     svn_path_url_add_component (parent_entry->url, 
                                                 bname, pool)))
        {
          /* This file is disjoint with respect to its parent
             directory.  Since we are looking at the actual target of
             the report (not some file in a subdirectory of a target
             directory), and that target is a file, we need to pass an
             empty string to link_path. */
          err = reporter->link_path (report_baton,
                                     "",
                                     entry->url,
                                     entry->revision,
                                     FALSE,
                                     entry->lock_token,
                                     pool);
          if (err)
            goto abort_report;
        }
      else if (entry->revision != base_rev || entry->lock_token)
        {
          /* If this entry is a file node, we just want to report that
             node's revision.  Since we are looking at the actual target
             of the report (not some file in a subdirectory of a target
             directory), and that target is a file, we need to pass an
             empty string to set_path. */
          err = reporter->set_path (report_baton, "", base_rev, FALSE,
                                    entry->lock_token, pool);
          if (err)
            goto abort_report;
        }
    }

  /* Finish the report, which causes the update editor to be driven. */
  return reporter->finish_report (report_baton, pool);

 abort_report:
  /* Clean up the fs transaction. */
  if ((fserr = reporter->abort_report (report_baton, pool)))
    {
      fserr = svn_error_quick_wrap (fserr, _("Error aborting report"));
      svn_error_compose (err, fserr);
    }
  return err;
}

/*** Compatibility wrapper: turns an svn_ra_reporter_t into an
     svn_ra_reporter2_t. ***/
struct wrap_report_baton {
  const svn_ra_reporter_t *reporter;
  void *baton;
};

static svn_error_t *wrap_set_path (void *report_baton,
                                   const char *path,
                                   svn_revnum_t revision,
                                   svn_boolean_t start_empty,
                                   const char *lock_token,
                                   apr_pool_t *pool)
{
  struct wrap_report_baton *wrb = report_baton;

  return wrb->reporter->set_path (wrb->baton, path, revision, start_empty,
                                  pool);
}

static svn_error_t *wrap_delete_path (void *report_baton,
                                      const char *path,
                                      apr_pool_t *pool)
{
  struct wrap_report_baton *wrb = report_baton;

  return wrb->reporter->delete_path (wrb->baton, path, pool);
}
    
static svn_error_t *wrap_link_path (void *report_baton,
                                    const char *path,
                                    const char *url,
                                    svn_revnum_t revision,
                                    svn_boolean_t start_empty,
                                    const char *lock_token,
                                    apr_pool_t *pool)
{
  struct wrap_report_baton *wrb = report_baton;

  return wrb->reporter->link_path (wrb->baton, path, url, revision,
                                   start_empty, pool);
}

static svn_error_t *wrap_finish_report (void *report_baton,
                                        apr_pool_t *pool)
{
  struct wrap_report_baton *wrb = report_baton;

  return wrb->reporter->finish_report (wrb->baton, pool);
}

static svn_error_t *wrap_abort_report (void *report_baton,
                                       apr_pool_t *pool)
{
  struct wrap_report_baton *wrb = report_baton;

  return wrb->reporter->abort_report (wrb->baton, pool);
}

static const svn_ra_reporter2_t wrap_reporter = {
  wrap_set_path,
  wrap_delete_path,
  wrap_link_path,
  wrap_finish_report,
  wrap_abort_report
};

svn_error_t *
svn_wc_crawl_revisions (const char *path,
                        svn_wc_adm_access_t *adm_access,
                        const svn_ra_reporter_t *reporter,
                        void *report_baton,
                        svn_boolean_t restore_files,
                        svn_boolean_t recurse,
                        svn_boolean_t use_commit_times,
                        svn_wc_notify_func_t notify_func,
                        void *notify_baton,
                        svn_wc_traversal_info_t *traversal_info,
                        apr_pool_t *pool)
{
  struct wrap_report_baton wrb;
  svn_wc__compat_notify_baton_t nb;
  
  wrb.reporter = reporter;
  wrb.baton = report_baton;

  nb.func = notify_func;
  nb.baton = notify_baton;

  return svn_wc_crawl_revisions2 (path, adm_access, &wrap_reporter, &wrb,
                                  restore_files, recurse, use_commit_times,
                                  svn_wc__compat_call_notify_func, &nb,
                                  traversal_info,
                                  pool);
}

svn_error_t *
svn_wc_transmit_text_deltas (const char *path,
                             svn_wc_adm_access_t *adm_access,
                             svn_boolean_t fulltext,
                             const svn_delta_editor_t *editor,
                             void *file_baton,
                             const char **tempfile,
                             apr_pool_t *pool)
{
  const char *tmpf, *tmp_base;
  svn_txdelta_window_handler_t handler;
  void *wh_baton;
  svn_txdelta_stream_t *txdelta_stream;
  apr_file_t *localfile = NULL;
  apr_file_t *basefile = NULL;
  const char *base_digest_hex = NULL;
  unsigned char digest[APR_MD5_DIGESTSIZE];
  
  /* Make an untranslated copy of the working file in the
     administrative tmp area because a) we want this to work even if
     someone changes the working file while we're generating the
     txdelta, b) we need to detranslate eol and keywords anyway, and
     c) after the commit, we're going to copy the tmp file to become
     the new text base anyway. */
  SVN_ERR (svn_wc_translated_file (&tmpf, path, adm_access, FALSE, pool));

  /* If the translation didn't create a new file then we need an explicit
     copy, if it did create a new file we need to rename it. */
  tmp_base = svn_wc__text_base_path (path, TRUE, pool);
  if (tmpf == path)
    SVN_ERR (svn_io_copy_file (tmpf, tmp_base, FALSE, pool));
  else
    SVN_ERR (svn_io_file_rename (tmpf, tmp_base, pool));

  /* If we're not sending fulltext, we'll be sending diffs against the
     text-base. */
  if (! fulltext)
    {
      /* Before we set up an svndiff stream against the old text base,
         make sure the old text base still matches its checksum.
         Otherwise we could send corrupt data and never know it. */ 
      const svn_wc_entry_t *ent;
      SVN_ERR (svn_wc_entry (&ent, path, adm_access, FALSE, pool));
      
      /* For backwards compatibility, no checksum means assume a match. */
      if (ent->checksum)
        {
          const char *tb = svn_wc__text_base_path (path, FALSE, pool);
          unsigned char tb_digest[APR_MD5_DIGESTSIZE];

          SVN_ERR (svn_io_file_checksum (tb_digest, tb, pool));
          base_digest_hex = svn_md5_digest_to_cstring_display (tb_digest,
                                                               pool);

          if (strcmp (base_digest_hex, ent->checksum) != 0)
            {
              /* Compatibility hack: working copies created before
                 13 Jan 2003 may have entry checksums stored in
                 base64.  See svn_io_file_checksum_base64()'s doc
                 string for details. */ 
              const char *digest_base64
                = (svn_base64_from_md5 (tb_digest, pool))->data;

              if (strcmp (digest_base64, ent->checksum) != 0)
                {
                  /* There is an entry checksum, but it does not match
                     the actual text base checksum.  Extreme badness.
                     Of course, theoretically we could just switch to
                     fulltext transmission here, and everything would
                     work fine; after all, we're going to replace the
                     text base with a new one in a moment anyway, and
                     we'd fix the checksum then.  But it's better to
                     error out.  People should know that their text
                     bases are getting corrupted, so they can
                     investigate.  Other commands could be affected,
                     too, such as `svn diff'.  */
              
                  /* Deliberately ignore error; the error about the
                     checksum mismatch is more important to return.
                     And wrapping the above error into the checksum
                     error would be weird, as they're unrelated. */
                  svn_error_clear (svn_io_remove_file (tmp_base, pool));

                  if (tempfile)
                    *tempfile = NULL;
                  
                  return svn_error_createf
                    (SVN_ERR_WC_CORRUPT_TEXT_BASE, NULL,
                     _("Checksum mismatch for '%s'; "
                       "expected '%s', actual: '%s'"),
                     svn_path_local_style (tb, pool),
                     ent->checksum, base_digest_hex);
                }
            }
        }

      SVN_ERR (svn_wc__open_text_base (&basefile, path, APR_READ, pool));
    }

  /* Tell the editor that we're about to apply a textdelta to the
     file baton; the editor returns to us a window consumer routine
     and baton.  */
  SVN_ERR (editor->apply_textdelta
           (file_baton,
            base_digest_hex, pool, &handler, &wh_baton));

  /* Alert the caller that we have created a temporary file that might
     need to be cleaned up. */
  if (tempfile)
    *tempfile = tmp_base;

  /* Open a filehandle for tmp text-base. */
  SVN_ERR_W (svn_io_file_open (&localfile, tmp_base,
                               APR_READ, APR_OS_DEFAULT, pool),
             _("Error opening local file"));

  /* Create a text-delta stream object that pulls data out of the two
     files. */
  svn_txdelta (&txdelta_stream,
               svn_stream_from_aprfile (basefile, pool),
               svn_stream_from_aprfile (localfile, pool),
               pool);
  
  /* Pull windows from the delta stream and feed to the consumer. */
  SVN_ERR (svn_txdelta_send_txstream (txdelta_stream, handler, 
                                      wh_baton, pool));
    
  /* Close the two files */
  SVN_ERR (svn_io_file_close (localfile, pool));
  
  if (basefile)
    SVN_ERR (svn_wc__close_text_base (basefile, path, 0, pool));

  /* ### This is a pity.  tmp_base was created with svn_io_copy_file()
     above, which uses apr_file_copy(), which probably called
     apr_file_transfer_contents(), which ran over every byte of the
     file and therefore could have computed a checksum effortlessly.
     But we're not about to change the interface of apr_file_copy(),
     so we'll have to run over the bytes again... */
  SVN_ERR (svn_io_file_checksum (digest, tmp_base, pool));

  /* Close the file baton, and get outta here. */
  return editor->close_file
    (file_baton, svn_md5_digest_to_cstring (digest, pool), pool);
}


svn_error_t *
svn_wc_transmit_prop_deltas (const char *path,
                             svn_wc_adm_access_t *adm_access,
                             const svn_wc_entry_t *entry,
                             const svn_delta_editor_t *editor,
                             void *baton,
                             const char **tempfile,
                             apr_pool_t *pool)
{
  int i;
  const char *props, *props_base, *props_tmp;
  apr_array_header_t *propmods;
  apr_hash_t *localprops = apr_hash_make (pool);
  apr_hash_t *baseprops = apr_hash_make (pool);
  
  /* Get the right access baton for the job. */
  SVN_ERR (svn_wc_adm_probe_retrieve (&adm_access, adm_access, path, pool));

  /* First, get the prop_path from the original path */
  SVN_ERR (svn_wc__prop_path (&props, path, adm_access, FALSE, pool));
  
  /* Get the full path of the prop-base `pristine' file */
  if (entry->schedule == svn_wc_schedule_replace)
    {
      /* do nothing: baseprop hash should be -empty- for comparison
         purposes.  if they already exist on disk, they're "leftover"
         from the old file that was replaced. */
      props_base = NULL;
    }
  else
    /* the real prop-base hash */
    SVN_ERR (svn_wc__prop_base_path (&props_base, path, adm_access, FALSE,
                                     pool));

  /* Copy the local prop file to the administrative temp area */
  SVN_ERR (svn_wc__prop_path (&props_tmp, path, adm_access, TRUE, pool));
  SVN_ERR (svn_io_copy_file (props, props_tmp, FALSE, pool));

  /* Alert the caller that we have created a temporary file that might
     need to be cleaned up. */
  if (tempfile)
    *tempfile = props_tmp;

  /* Load all properties into hashes */
  SVN_ERR (svn_wc__load_prop_file (props_tmp, localprops, pool));
  if (props_base)
    SVN_ERR (svn_wc__load_prop_file (props_base, baseprops, pool));
  
  /* Get an array of local changes by comparing the hashes. */
  SVN_ERR (svn_prop_diffs (&propmods, localprops, baseprops, pool));

  /* Apply each local change to the baton */
  for (i = 0; i < propmods->nelts; i++)
    {
      const svn_prop_t *p = &APR_ARRAY_IDX (propmods, i, svn_prop_t);
      if (entry->kind == svn_node_file)
        SVN_ERR (editor->change_file_prop (baton, p->name, p->value, pool));
      else
        SVN_ERR (editor->change_dir_prop (baton, p->name, p->value, pool));
    }

  return SVN_NO_ERROR;
}
