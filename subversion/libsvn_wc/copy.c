/*
 * copy.c:  wc 'copy' functionality.
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



/*** Includes. ***/

#include <string.h>
#include "svn_wc.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"

#include "wc.h"
#include "adm_files.h"
#include "entries.h"
#include "props.h"
#include "translate.h"

#include "svn_private_config.h"


/*** Code. ***/

svn_error_t *
svn_wc__remove_wcprops (svn_wc_adm_access_t *adm_access,
                        svn_boolean_t recurse,
                        apr_pool_t *pool)
{
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  const char *wcprop_path;
  apr_pool_t *subpool = svn_pool_create (pool);
  svn_error_t *err;

  /* Read PATH's entries. */
  SVN_ERR (svn_wc_entries_read (&entries, adm_access, FALSE, subpool));

  /* Remove this_dir's wcprops */
  SVN_ERR (svn_wc__wcprop_path (&wcprop_path,
                                svn_wc_adm_access_path (adm_access),
                                adm_access, FALSE, subpool));
  err = svn_io_remove_file (wcprop_path, subpool);
  if (err)
    svn_error_clear (err);

  /* Recursively loop over all children. */
  for (hi = apr_hash_first (subpool, entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;
      const char *name;
      const svn_wc_entry_t *current_entry;
      const char *child_path;

      apr_hash_this (hi, &key, NULL, &val);
      name = key;
      current_entry = val;

      /* Ignore the "this dir" entry. */
      if (! strcmp (name, SVN_WC_ENTRY_THIS_DIR))
        continue;

      child_path = svn_path_join (svn_wc_adm_access_path (adm_access), name,
                                  subpool);

      /* If a file, remove it from wcprops. */
      if (current_entry->kind == svn_node_file)
        {
          SVN_ERR (svn_wc__wcprop_path (&wcprop_path, child_path, adm_access,
                                        FALSE, subpool));
          err = svn_io_remove_file (wcprop_path, subpool);
          if (err)
            svn_error_clear (err);
          /* ignoring any error value from the removal; most likely,
             apr_file_remove will complain about trying to a remove a
             file that's not there.  But this more efficient than
             doing an independant stat for each file's existence
             before trying to remove it, no? */
        }        

      /* If a dir, recurse. */
      else if (recurse && current_entry->kind == svn_node_dir)
        {
          svn_wc_adm_access_t *child_access;
          SVN_ERR (svn_wc_adm_retrieve (&child_access, adm_access, child_path,
                                        subpool));
          SVN_ERR (svn_wc__remove_wcprops (child_access, recurse, subpool));
        }
    }

  /* Cleanup */
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}


/* This function effectively creates and schedules a file for
   addition, but does extra administrative things to allow it to
   function as a 'copy'.

   ASSUMPTIONS:

     - src_path points to a file under version control
     - dst_parent points to a dir under version control, in the same
                  working copy.
     - dst_basename will be the 'new' name of the copied file in dst_parent
 */
static svn_error_t *
copy_file_administratively (const char *src_path, 
                            svn_wc_adm_access_t *src_access,
                            svn_wc_adm_access_t *dst_parent,
                            const char *dst_basename,
                            svn_wc_notify_func2_t notify_copied,
                            void *notify_baton,
                            apr_pool_t *pool)
{
  svn_node_kind_t dst_kind;
  const svn_wc_entry_t *src_entry, *dst_entry;
  svn_boolean_t special;

  /* The 'dst_path' is simply dst_parent/dst_basename */
  const char *dst_path
    = svn_path_join (svn_wc_adm_access_path (dst_parent), dst_basename, pool);

  /* Sanity check:  if dst file exists already, don't allow overwrite. */
  SVN_ERR (svn_io_check_path (dst_path, &dst_kind, pool));
  if (dst_kind != svn_node_none)
    return svn_error_createf (SVN_ERR_ENTRY_EXISTS, NULL,
                              _("'%s' already exists and is in the way"),
                              svn_path_local_style (dst_path, pool));

  /* Even if DST_PATH doesn't exist it may still be a versioned file; it
     may be scheduled for deletion, or the user may simply have removed the
     working copy.  Since we are going to write to DST_PATH text-base and
     prop-base we need to detect such cases and abort. */
  SVN_ERR (svn_wc_entry (&dst_entry, dst_path, dst_parent, FALSE, pool));
  if (dst_entry && dst_entry->kind == svn_node_file)
    {
      if (dst_entry->schedule == svn_wc_schedule_delete)
        return svn_error_createf (SVN_ERR_ENTRY_EXISTS, NULL,
                                  _("'%s' is scheduled for deletion; it must"
                                    " be committed before it can be"
                                    " overwritten"),
                                  svn_path_local_style (dst_path, pool));
      else
        return svn_error_createf (SVN_ERR_ENTRY_EXISTS, NULL,
                                  _("There is already a versioned item '%s'"),
                                  svn_path_local_style (dst_path, pool));
    }

  /* Sanity check:  you cannot make a copy of something that's not
     in the repository.  See comment at the bottom of this file for an
     explanation. */
  SVN_ERR (svn_wc_entry (&src_entry, src_path, src_access, FALSE, pool));
  if (! src_entry)
    return svn_error_createf 
      (SVN_ERR_UNVERSIONED_RESOURCE, NULL,
       _("Cannot copy or move '%s': it's not under version control"),
       svn_path_local_style (src_path, pool));
  if ((src_entry->schedule == svn_wc_schedule_add)
      || (! src_entry->url)
      || (src_entry->copied))
    return svn_error_createf 
      (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
       _("Cannot copy or move '%s': it's not in the repository yet; "
         "try committing first"),
       svn_path_local_style (src_path, pool));

  /* Now, make an actual copy of the working file.  If this is a
     special file, we can't copy it directly, but should instead
     use the translation routines to create the new file. */
  SVN_ERR (svn_wc__get_special (&special, src_path, src_access, pool));
  if (! special)
    SVN_ERR (svn_io_copy_file (src_path, dst_path, TRUE, pool));
  else
    SVN_ERR (svn_subst_copy_and_translate2 (src_path,
                                            dst_path, NULL, FALSE,
                                            NULL,
                                            TRUE, /* expand */
                                            TRUE, /* special */
                                            pool));
  
  /* Copy the pristine text-base over.  Why?  Because it's the *only*
     way we can detect any upcoming local mods on the copy.

     In other words, we're talking about the scenario where somebody
     makes local mods to 'foo.c', then does an 'svn cp foo.c bar.c'.
     In this case, bar.c should still be locally modified too.
     
     Why do we want the copy to have local mods?  Even though the user
     will only see an 'A' instead of an 'M', local mods means that the
     client doesn't have to send anything but a small delta during
     commit; the server can make efficient use of the copyfrom args. 

     As long as we're copying the text-base over, we should copy the
     working and pristine propfiles over too. */
  {
    svn_node_kind_t kind;
    const char *src_wprop, *src_bprop, *dst_wprop, *dst_bprop;

    /* Discover the paths to the two text-base files */
    const char *src_txtb = svn_wc__text_base_path (src_path, FALSE, pool);
    const char *dst_txtb = svn_wc__text_base_path (dst_path, FALSE, pool);

    /* Discover the paths to the four prop files */
    SVN_ERR (svn_wc__prop_path (&src_wprop, src_path, 
                                src_access, FALSE, pool));
    SVN_ERR (svn_wc__prop_base_path (&src_bprop, src_path, 
                                     src_access, FALSE, pool));
    SVN_ERR (svn_wc__prop_path (&dst_wprop, dst_path, 
                                dst_parent, FALSE, pool));
    SVN_ERR (svn_wc__prop_base_path (&dst_bprop, dst_path, 
                                     dst_parent, FALSE, pool));

    /* Copy the text-base over unconditionally. */
    SVN_ERR (svn_io_copy_file (src_txtb, dst_txtb, TRUE, pool));

    /* Copy the props over if they exist. */
    SVN_ERR (svn_io_check_path (src_wprop, &kind, pool));
    if (kind == svn_node_file)
      SVN_ERR (svn_io_copy_file (src_wprop, dst_wprop, TRUE, pool));
      
    /* Copy the base-props over if they exist */
    SVN_ERR (svn_io_check_path (src_bprop, &kind, pool));
    if (kind == svn_node_file)
      SVN_ERR (svn_io_copy_file (src_bprop, dst_bprop, TRUE, pool));
  }

  /* Schedule the new file for addition in its parent, WITH HISTORY. */
  {
    char *copyfrom_url;
    svn_revnum_t copyfrom_rev;

    SVN_ERR (svn_wc_get_ancestry (&copyfrom_url, &copyfrom_rev,
                                  src_path, src_access, pool));
    
    /* Pass NULL, NULL for cancellation func and baton, as this is
       only one file, not N files. */
    SVN_ERR (svn_wc_add2 (dst_path, dst_parent,
                          copyfrom_url, copyfrom_rev,
                          NULL, NULL,
                          notify_copied, notify_baton, pool));
  }

  return SVN_NO_ERROR;
}


/* Recursively crawl over a directory PATH and do a number of things:
     - Remove lock tokens
     - Remove WC props
     - Convert deleted items to schedule-delete items
     - Set .svn directories to be hidden
*/
static svn_error_t *
post_copy_cleanup (svn_wc_adm_access_t *adm_access,
                   apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  svn_wc_entry_t *entry;
  const char *path = svn_wc_adm_access_path (adm_access);
  
  /* Remove wcprops. */
  SVN_ERR (svn_wc__remove_wcprops (adm_access, FALSE, pool));

  /* Read this directory's entries file. */
  SVN_ERR (svn_wc_entries_read (&entries, adm_access, FALSE, pool));

  /* Because svn_io_copy_dir_recursively() doesn't copy directory
     permissions, we'll patch up our tree's .svn subdirs to be
     hidden. */
#ifdef APR_FILE_ATTR_HIDDEN
  {
    const char *adm_dir = svn_path_join (path, SVN_WC_ADM_DIR_NAME, pool);
    const char *path_apr;
    apr_status_t status;
    SVN_ERR (svn_path_cstring_from_utf8 (&path_apr, adm_dir, pool));
    status = apr_file_attrs_set (path_apr,
                                 APR_FILE_ATTR_HIDDEN,
                                 APR_FILE_ATTR_HIDDEN,
                                 pool);
    if (status)
      return svn_error_wrap_apr (status, _("Can't hide directory '%s'"),
                                 svn_path_local_style (adm_dir, pool));
  }
#endif

  /* Loop over all children, removing lock tokens and recursing into
     directories. */
  SVN_ERR (svn_wc_entries_read (&entries, adm_access, TRUE, pool));
  for (hi = apr_hash_first (pool, entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;
      svn_node_kind_t kind;
      svn_boolean_t deleted = FALSE;
      apr_uint32_t flags = SVN_WC__ENTRY_MODIFY_FORCE;

      svn_pool_clear (subpool);

      apr_hash_this (hi, &key, NULL, &val);
      entry = val;
      kind = entry->kind;
      deleted = entry->deleted;

      /* Convert deleted="true" into schedule="delete" for all
         children (and grandchildren, if RECURSE is set) of the path
         represented by ADM_ACCESS.  The result of this is that when
         the copy is committed the items in question get deleted and
         the result is a directory in the repository that matches the
         original source directory for copy.  If this were not done
         the deleted="true" items would simply vanish from the entries
         file as the copy is added to the working copy.  The new
         schedule="delete" files do not have a text-base and so their
         scheduled deletion cannot be reverted.  For directories a
         placeholder with an svn_node_kind_t of svn_node_file and
         schedule="delete" is used to avoid the problems associated
         with creating a directory.  See Issue #2101 for details. */
      if (entry->deleted)
        {
          entry->schedule = svn_wc_schedule_delete;
          flags |= SVN_WC__ENTRY_MODIFY_SCHEDULE;
          
          entry->deleted = FALSE;
          flags |= SVN_WC__ENTRY_MODIFY_DELETED;

          if (entry->kind == svn_node_dir)
            {
              /* ### WARNING: Very dodgy stuff here! ###

              Directories are a problem since a schedule delete directory
              needs an admin directory to be present.  It's possible to
              create a dummy admin directory and that sort of works, it's
              good enough if the user commits the copy.  Where it falls
              down is if the user *reverts* the dummy directory since the
              now schedule normal, copied, directory doesn't have the
              correct contents.

              The dodgy solution is to cheat and use a schedule delete file
              as a placeholder!  This is sufficient to provide a delete
              when the copy is committed.  Attempts to revert any such
              "fake" files will fail due to a missing text-base. This
              effectively means that the schedule deletes have to remain
              schedule delete until the copy is committed, when they become
              state deleted and everything works! */
              entry->kind = svn_node_file;
              flags |= SVN_WC__ENTRY_MODIFY_KIND;
            }
        }

      /* Remove lock stuffs. */
      if (entry->lock_token)
        {
          entry->lock_token = NULL;
          entry->lock_owner = NULL;
          entry->lock_comment = NULL;
          entry->lock_creation_date = 0;
          flags |= (SVN_WC__ENTRY_MODIFY_LOCK_TOKEN
                    | SVN_WC__ENTRY_MODIFY_LOCK_OWNER
                    | SVN_WC__ENTRY_MODIFY_LOCK_COMMENT
                    | SVN_WC__ENTRY_MODIFY_LOCK_CREATION_DATE);
        }
      
      /* If we meaningfully modified the flags, we must be wanting to
         change the entry. */
      if (flags != SVN_WC__ENTRY_MODIFY_FORCE)
        SVN_ERR (svn_wc__entry_modify (adm_access, key, entry,
                                       flags, TRUE, subpool));
      
      /* If a dir, not deleted, and not "this dir", recurse. */
      if ((! deleted)
          && (kind == svn_node_dir)
          && (strcmp (key, SVN_WC_ENTRY_THIS_DIR) != 0))
        {
          svn_wc_adm_access_t *child_access;
          const char *child_path;
          child_path = svn_path_join 
            (svn_wc_adm_access_path (adm_access), key, subpool);
          SVN_ERR (svn_wc_adm_retrieve (&child_access, adm_access, 
                                        child_path, subpool));
          SVN_ERR (post_copy_cleanup (child_access, subpool));
        }
    }

  /* Cleanup */
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}


/* This function effectively creates and schedules a dir for
   addition, but does extra administrative things to allow it to
   function as a 'copy'.

   ASSUMPTIONS:

     - src_path points to a dir under version control
     - dst_parent points to a dir under version control, in the same
                  working copy.
     - dst_basename will be the 'new' name of the copied dir in dst_parent
 */
static svn_error_t *
copy_dir_administratively (const char *src_path, 
                           svn_wc_adm_access_t *src_access,
                           svn_wc_adm_access_t *dst_parent,
                           const char *dst_basename,
                           svn_cancel_func_t cancel_func,
                           void *cancel_baton,
                           svn_wc_notify_func2_t notify_copied,
                           void *notify_baton,
                           apr_pool_t *pool)
{
  const svn_wc_entry_t *src_entry;
  svn_wc_adm_access_t *adm_access;

  /* The 'dst_path' is simply dst_parent/dst_basename */
  const char *dst_path = svn_path_join (svn_wc_adm_access_path (dst_parent),
                                        dst_basename, pool);

  /* Sanity check:  you cannot make a copy of something that's not
     in the repository.  See comment at the bottom of this file for an
     explanation. */
  SVN_ERR (svn_wc_entry (&src_entry, src_path, src_access, FALSE, pool));
  if (! src_entry)
    return svn_error_createf
      (SVN_ERR_ENTRY_NOT_FOUND, NULL, 
       _("'%s' is not under version control"),
       svn_path_local_style (src_path, pool));
  if ((src_entry->schedule == svn_wc_schedule_add)
      || (! src_entry->url)
      || (src_entry->copied))
    return svn_error_createf 
      (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
       _("Cannot copy or move '%s': it is not in the repository yet; "
         "try committing first"),
       svn_path_local_style (src_path, pool));

  /* Recursively copy the whole directory over.  This gets us all
     text-base, props, base-props, as well as entries, local mods,
     schedulings, existences, etc.

      ### Should we be copying unversioned items within the directory? */
  SVN_ERR (svn_io_copy_dir_recursively (src_path,
                                        svn_wc_adm_access_path (dst_parent),
                                        dst_basename,
                                        TRUE,
                                        cancel_func, cancel_baton,
                                        pool));

  /* If this is part of a move, the copied directory will be locked,
     because the source directory was locked.  Running cleanup will remove
     the locks, even though this directory has not yet been added to the
     parent. */
  SVN_ERR (svn_wc_cleanup2 (dst_path, NULL, cancel_func, cancel_baton, pool));

  /* We've got some post-copy cleanup to do now. */
  SVN_ERR (svn_wc_adm_open3 (&adm_access, NULL, dst_path, TRUE, -1,
                             cancel_func, cancel_baton, pool));
  SVN_ERR (post_copy_cleanup (adm_access, pool));
  SVN_ERR (svn_wc_adm_close (adm_access));

  /* Schedule the directory for addition in both its parent and itself
     (this_dir) -- WITH HISTORY.  This function should leave the
     existing administrative dir untouched.  */
  {
    char *copyfrom_url;
    svn_revnum_t copyfrom_rev;
    
    SVN_ERR (svn_wc_get_ancestry (&copyfrom_url, &copyfrom_rev,
                                  src_path, src_access, pool));
    
    SVN_ERR (svn_wc_add2 (dst_path, dst_parent,
                          copyfrom_url, copyfrom_rev,
                          cancel_func, cancel_baton,
                          notify_copied, notify_baton, pool));
  }
 
  return SVN_NO_ERROR;
}



/* Public Interface */

svn_error_t *
svn_wc_copy2 (const char *src_path,
              svn_wc_adm_access_t *dst_parent,
              const char *dst_basename,
              svn_cancel_func_t cancel_func,
              void *cancel_baton,
              svn_wc_notify_func2_t notify_func,
              void *notify_baton,
              apr_pool_t *pool)
{
  svn_wc_adm_access_t *adm_access;
  svn_node_kind_t src_kind;
  const svn_wc_entry_t *entry;

  SVN_ERR (svn_wc_entry (&entry, svn_wc_adm_access_path (dst_parent),
                         dst_parent, FALSE, pool));
  if (entry->schedule == svn_wc_schedule_delete)
    return svn_error_createf
      (SVN_ERR_WC_INVALID_SCHEDULE, NULL,
       _("Cannot copy to '%s' as it is scheduled for deletion"),
       svn_path_local_style (svn_wc_adm_access_path (dst_parent), pool));

  SVN_ERR (svn_wc_adm_probe_open3 (&adm_access, NULL, src_path, FALSE, -1,
                                   cancel_func, cancel_baton, pool));

  SVN_ERR (svn_io_check_path (src_path, &src_kind, pool));
  
  if (src_kind == svn_node_file)
    SVN_ERR (copy_file_administratively (src_path, adm_access,
                                         dst_parent, dst_basename,
                                         notify_func, notify_baton, pool));

  else if (src_kind == svn_node_dir)
    SVN_ERR (copy_dir_administratively (src_path, adm_access,
                                        dst_parent, dst_basename,
                                        cancel_func, cancel_baton,
                                        notify_func, notify_baton, pool));

  SVN_ERR (svn_wc_adm_close (adm_access));


  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_copy (const char *src_path,
             svn_wc_adm_access_t *dst_parent,
             const char *dst_basename,
             svn_cancel_func_t cancel_func,
             void *cancel_baton,
             svn_wc_notify_func_t notify_func,
             void *notify_baton,
             apr_pool_t *pool)
{
  svn_wc__compat_notify_baton_t nb;
  
  nb.func = notify_func;
  nb.baton = notify_baton;

  return svn_wc_copy2 (src_path, dst_parent, dst_basename, cancel_func,
                       cancel_baton, svn_wc__compat_call_notify_func,
                       &nb, pool);
}


/*
  Rabbinic Commentary


  Q:  Why can't we 'svn cp' something that we just copied?
      i.e.  'svn cp foo foo2;  svn cp foo2 foo3"

  A:  It leads to inconsistencies.

      In the example above, foo2 has no associated repository URL,
      because it hasn't been committed yet.  But suppose foo3 simply
      inherited foo's URL (i.e. foo3 'pointed' to foo as a copy
      ancestor by virtue of transitivity.)
 
      For one, this is not what the user would expect.  That's
      certainly not what the user typed!  Second, suppose that the
      user did a commit between the two 'svn cp' commands.  Now foo3
      really *would* point to foo2, but without that commit, it
      pointed to foo.  Ugly inconsistency, and the user has no idea
      that foo3's ancestor would be different in each case.

      And even if somehow we *could* make foo3 point to foo2 before
      foo2 existed in the repository... what's to prevent a user from
      committing foo3 first?  That would break.

*/
