/*
 * status.c: construct a status structure from an entry structure
 *
 * ====================================================================
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



#include <string.h>
#include <apr_pools.h>
#include <apr_file_io.h>
#include <apr_hash.h>
#include <apr_time.h>
#include <apr_fnmatch.h>
#include "svn_pools.h"
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_wc.h"
#include "wc.h"   


static void add_default_ignores (apr_array_header_t *patterns)
{
  static const char * const ignores[] = 
  {
    "*.o", "*.lo", "*.la", "#*#", "*.rej", "*~", ".#*",
    /* what else? */
    NULL
  };
  int i;
  
  for (i = 0; ignores[i] != NULL; i++)
    {
      const char **ent = apr_array_push(patterns);
      *ent = ignores[i];
    }

}


/* Helper routine: add to a *PATTERNS list patterns from the value of
   the SVN_PROP_IGNORE property set on DIRPATH.  If there is no such
   property, or the property contains no patterns, do nothing.
   Otherwise, add to *PATTERNS a list of (const char *) patterns to
   match. */
static svn_error_t *
add_ignore_patterns (const char *dirpath,
                     apr_array_header_t *patterns,
                     apr_pool_t *pool)
{
  svn_stringbuf_t *value = NULL;
  svn_stringbuf_t *name = svn_stringbuf_create (SVN_PROP_IGNORE, pool);

  /* Try to load the SVN_PROP_IGNORE property. */
  SVN_ERR (svn_wc_prop_get (&value, name, 
                            svn_stringbuf_create (dirpath, pool), pool));
  if (value != NULL)
    {
      char sep[3] = "\n\r";
      char *last;
      char *p = apr_strtok (value->data, sep, &last);
      while (p)
        {
          if (p[0] != '\0')
            {
              (*((const char **) apr_array_push (patterns))) = 
                apr_pstrdup (pool, p);
            }
          p = apr_strtok (NULL, sep, &last);
        } 
    }    
  return SVN_NO_ERROR;
}                  


                        
/* Fill in *STATUS for PATH, whose entry data is in ENTRY.  Allocate
   *STATUS in POOL. 

   ENTRY may be null, for non-versioned entities.  In this case, we
   will assemble a special status structure item which implies a
   non-versioned thing.

   Else, ENTRY's pool must not be shorter-lived than STATUS's, since
   ENTRY will be stored directly, not copied.

   If GET_ALL is zero, and ENTRY is not locally modified, then *STATUS
   will be set to NULL.  If GET_ALL is non-zero, then *STATUS will be
   allocated and returned no matter what.

*/
static svn_error_t *
assemble_status (svn_wc_status_t **status,
                 svn_stringbuf_t *path,
                 svn_wc_entry_t *entry,
                 svn_boolean_t get_all,
                 apr_pool_t *pool)
{
  svn_wc_status_t *stat;
  enum svn_node_kind path_kind;
  svn_boolean_t has_props;
  svn_boolean_t text_modified_p = FALSE;
  svn_boolean_t prop_modified_p = FALSE;

  /* Defaults for two main variables. */
  enum svn_wc_status_kind final_text_status = svn_wc_status_normal;
  enum svn_wc_status_kind final_prop_status = svn_wc_status_none;

  /* Check the path kind for PATH. */
  SVN_ERR( svn_io_check_path (path, &path_kind, pool));

  if (! entry)
    {
      /* return a blank structure. */
      stat = apr_pcalloc (pool, sizeof(*stat));
      stat->entry = NULL;
      stat->repos_rev = SVN_INVALID_REVNUM;
      stat->text_status = svn_wc_status_none;
      stat->prop_status = svn_wc_status_none;
      stat->repos_text_status = svn_wc_status_none;
      stat->repos_prop_status = svn_wc_status_none;
      stat->locked = FALSE;
      stat->copied = FALSE;

      /* If this path has no entry, but IS present on disk, it's
         unversioned. */
      if (path_kind != svn_node_none)
        stat->text_status = svn_wc_status_unversioned;

      *status = stat;
      return SVN_NO_ERROR;
    }

  /* Implement predecence rules: */

  /* 1. Set the two main variables to "discovered" values first (M, C). 
        Together, these two stati are of lowest precedence, and C has
        precedence over M. */

  /* Does the entry have props? */
  SVN_ERR (svn_wc__has_props (&has_props, path, pool));
  if (has_props)
    final_prop_status = svn_wc_status_normal;

  /* If the entry has a property file, see if it has local changes. */
  SVN_ERR (svn_wc_props_modified_p (&prop_modified_p, path, pool));
  
  /* If the entry is a file, check for textual modifications */
  if (entry->kind == svn_node_file)
    SVN_ERR (svn_wc_text_modified_p (&text_modified_p, path, pool));
  
  if (text_modified_p)
    final_text_status = svn_wc_status_modified;
  
  if (prop_modified_p)
    final_prop_status = svn_wc_status_modified;      
  
  if (entry->conflicted)
    {
      /* We must decide if either component is still "conflicted",
         based on whether reject files continue to exist. */
      svn_boolean_t text_conflict_p, prop_conflict_p;
      svn_stringbuf_t *parent_dir;
      
      if (entry->kind == svn_node_dir)
        parent_dir = path;
      else  /* non-directory, that's all we need to know */
        {
          parent_dir = svn_stringbuf_dup (path, pool);
          svn_path_remove_component (parent_dir, svn_path_local_style);
        }
      
      SVN_ERR (svn_wc_conflicted_p (&text_conflict_p, &prop_conflict_p,
                                    parent_dir, entry, pool));
      
      if (text_conflict_p)
        final_text_status = svn_wc_status_conflicted;
      if (prop_conflict_p)
        final_prop_status = svn_wc_status_conflicted;
    }

   
  /* 2. Possibly overwrite the the text_status variable with
        "scheduled" states from the entry (A, D, R).  As a group,
        these states are of medium precedence.  They also override any
        C or M that may be in the prop_status field at this point.*/

  if (entry->schedule == svn_wc_schedule_add)
    {
      final_text_status = svn_wc_status_added;
      final_prop_status = svn_wc_status_none;
    }
      
  else if (entry->schedule == svn_wc_schedule_replace)
    {
      final_text_status = svn_wc_status_replaced;
      final_prop_status = svn_wc_status_none;
    }
    
  else if (entry->schedule == svn_wc_schedule_delete)
    {
      final_text_status = svn_wc_status_deleted;
      final_prop_status = svn_wc_status_none;
    }


  /* 3. Highest precedence: check to see if file or dir is just
        missing.  This overrides every possible state *except*
        deletion.  (If something is deleted or scheduled for it, we
        don't care if the working file exists.)  */
  if ((path_kind == svn_node_none)
      && (final_text_status != svn_wc_status_deleted))
    final_text_status = svn_wc_status_absent;


  /* 4. Easy out:  unless we're fetching -every- entry, don't bother
     to allocate a struct for an uninteresting entry. */

  if (! get_all)
    if (((final_text_status == svn_wc_status_none)
         || (final_text_status == svn_wc_status_normal))
        && ((final_prop_status == svn_wc_status_none)
            || (final_prop_status == svn_wc_status_normal)))
      {
        *status = NULL;
        return SVN_NO_ERROR;
      }


  /* 5. Build and return a status structure. */

  stat = apr_pcalloc (pool, sizeof(**status));
  stat->entry = svn_wc__entry_dup (entry, pool);
  stat->repos_rev = SVN_INVALID_REVNUM;           /* caller fills in */
  stat->text_status = final_text_status;       
  stat->prop_status = final_prop_status;    
  stat->repos_text_status = svn_wc_status_none;   /* default */
  stat->repos_prop_status = svn_wc_status_none;   /* default */
  stat->locked = FALSE;
  stat->copied = FALSE;
  
  /* 6. Check for locked directory, or if the item is 'copied'. */

  if (entry->kind == svn_node_dir)
    SVN_ERR (svn_wc__locked (&(stat->locked), path, pool));
  if (entry->copied)
    stat->copied = TRUE;

  *status = stat;

  return SVN_NO_ERROR;
}


/* Given an ENTRY object representing PATH, build a status structure
   and store it in STATUSHASH.  */
static svn_error_t *
add_status_structure (apr_hash_t *statushash,
                      svn_stringbuf_t *path,
                      svn_wc_entry_t *entry,
                      svn_boolean_t get_all,
                      apr_pool_t *pool)
{
  svn_wc_status_t *statstruct;
  
  SVN_ERR (assemble_status (&statstruct, path, entry, get_all, pool));
  if (statstruct)
    apr_hash_set (statushash, path->data, path->len, statstruct);
  
  return SVN_NO_ERROR;
}


/* Add all items that are NOT in ENTRIES (which is a list of PATH's
   versioned things) to the STATUSHASH as unversioned items,
   allocating everything in POOL. */
static svn_error_t *
add_unversioned_items (svn_stringbuf_t *path, 
                       apr_hash_t *entries,
                       apr_hash_t *statushash,
                       apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  apr_hash_t *dirents;
  apr_hash_index_t *hi;
  apr_array_header_t *patterns;

  /* Read PATH's dirents. */
  SVN_ERR (svn_io_get_dirents (&dirents, path, subpool));

  /* Try to load any '.svnignore' file that may be present. */
  patterns = apr_array_make (subpool, 1, sizeof(const char *));
  add_default_ignores (patterns);
  SVN_ERR (add_ignore_patterns (path->data, patterns, subpool));

  /* Add empty status structures for each of the unversioned things. */
  for (hi = apr_hash_first (subpool, dirents); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      apr_ssize_t klen;
      void *val;
      const char *keystring;
      int i;
      int ignore_me;
      svn_stringbuf_t *printable_path;

      apr_hash_this (hi, &key, &klen, &val);
      keystring = (const char *) key;
        
      /* If the dirent isn't in `.svn/entries'... */
      if (apr_hash_get (entries, key, klen))        
        continue;

      /* and we're not looking at .svn... */
      if (! strcmp (keystring, SVN_WC_ADM_DIR_NAME))
        continue;

      ignore_me = 0;

      /* See if any of the ignore patterns we have matches our
         keystring. */
      for (i = 0; i < patterns->nelts; i++)
        {
          const char *pat = (((const char **) (patterns)->elts))[i];
                
          /* Try to match current_entry_name to pat. */
          if (APR_SUCCESS == apr_fnmatch (pat, keystring, FNM_PERIOD))
            {
              ignore_me = 1;
              break;
            }
        }
      
      /* If we aren't ignoring it, add a status structure for this
         dirent. */
      if (! ignore_me)
        {
          /* Reset our base string... */
          printable_path = svn_stringbuf_dup (path, pool);
          /* ...and append the current entry. */
          svn_path_add_component_nts (printable_path, keystring,
                                      svn_path_local_style);
          
          /* Add this item to the status hash. */
          SVN_ERR (add_status_structure (statushash,
                                         printable_path,
                                         NULL, /* no entry */
                                         FALSE,
                                         pool));
        }
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_status (svn_wc_status_t **status,
               svn_stringbuf_t *path,
               apr_pool_t *pool)
{
  svn_wc_status_t *s;
  svn_wc_entry_t *entry = NULL;

  SVN_ERR (svn_wc_entry (&entry, path, pool));
  SVN_ERR (assemble_status (&s, path, entry, TRUE, pool));
  *status = s;
  return SVN_NO_ERROR;
}



svn_error_t *
svn_wc_statuses (apr_hash_t *statushash,
                 svn_stringbuf_t *path,
                 svn_boolean_t descend,
                 svn_boolean_t get_all,
                 apr_pool_t *pool)
{
  enum svn_node_kind kind;
  apr_hash_t *entries;
  svn_wc_entry_t *entry;
  void *value;
  apr_pool_t *subpool = svn_pool_create (pool);

  /* Is PATH a directory or file? */
  SVN_ERR (svn_io_check_path (path, &kind, subpool));
  
  /* kff todo: this has to deal with the case of a type-changing edit,
     i.e., someone removed a file under vc and replaced it with a dir,
     or vice versa.  In such a case, when you ask for the status, you
     should get mostly information about the now-vanished entity, plus
     some information about what happened to it.  The same situation
     is handled in entries.c:svn_wc_entry. */

  /* Read the appropriate entries file */
  
  /* If path points to just one file, or at least to just one
     non-directory, store just one status structure in the
     STATUSHASH and return. */
  if ((kind == svn_node_file) || (kind == svn_node_none))
    {
      svn_stringbuf_t *dirpath, *basename;

      /* Figure out file's parent dir */
      svn_path_split (path, &dirpath, &basename,
                      svn_path_local_style, subpool);      

      /* Load entries file for file's parent */
      SVN_ERR (svn_wc_entries_read (&entries, dirpath, subpool));

      /* Get the entry by looking up file's basename */
      value = apr_hash_get (entries, basename->data, basename->len);

      /* Convert the entry into a status structure, store in the hash.
         
         ### Notice that because we're getting one specific file,
         we're ignoring the GET_ALL flag and unconditionally fetching
         the status structure. */
      SVN_ERR (add_status_structure (statushash, path, 
                                     (svn_wc_entry_t *)value, 
                                     TRUE, pool));
    }


  /* Fill the hash with a status structure for *each* entry in PATH */
  else if (kind == svn_node_dir)
    {
      apr_hash_index_t *hi;

      /* Load entries file for the directory */
      SVN_ERR (svn_wc_entries_read (&entries, path, subpool));

      /* Add the unversioned items to the status output. */
      SVN_ERR (add_unversioned_items (path, entries, statushash, pool));

      /* Loop over entries hash */
      for (hi = apr_hash_first (subpool, entries); hi; hi = apr_hash_next (hi))
        {
          const void *key;
          void *val;
          const char *basename;
          apr_ssize_t keylen;
          svn_stringbuf_t *fullpath = svn_stringbuf_dup (path, pool);

          /* Get the next dirent */
          apr_hash_this (hi, &key, &keylen, &val);
          basename = (const char *) key;
          if (strcmp (basename, SVN_WC_ENTRY_THIS_DIR) != 0)
            {
              svn_path_add_component_nts (fullpath, basename,
                                          svn_path_local_style);
            }

          entry = (svn_wc_entry_t *) val;

          SVN_ERR (svn_io_check_path (fullpath, &kind, subpool));

          /* In deciding whether or not to descend, we use the actual
             kind of the entity, not the kind claimed by the entries
             file.  The two are usually the same, but where they are
             not, its usually because some directory got moved, and
             one would still want a status report on its contents.
             kff todo: However, must handle mixed working copies.
             What if the subdir is not under revision control, or is
             from another repository? */
          
          /* Do *not* store THIS_DIR in the statushash, unless this
             path has never been seen before.  We don't want to add
             the path key twice. */
          if (! strcmp (basename, SVN_WC_ENTRY_THIS_DIR))
            {
              svn_wc_status_t *s = apr_hash_get (statushash,
                                                 fullpath->data,
                                                 fullpath->len);
              if (! s)
                SVN_ERR (add_status_structure (statushash, fullpath,
                                               entry, get_all, pool));
            }
          else
            {
              if (kind == svn_node_dir && descend)
                {
                  /* Directory entries are incomplete.  We must get
                     their full entry from their own THIS_DIR entry.
                     svn_wc_entry does this for us if it can.  */
                  svn_wc_entry_t *subdir;

                  SVN_ERR (svn_wc_entry (&subdir, fullpath, subpool));
                  SVN_ERR (add_status_structure (statushash, fullpath,
                                                 subdir, get_all, pool));
                  SVN_ERR (svn_wc_statuses (statushash, fullpath,
                                            descend, get_all, pool)); 
                }
              else if ((kind == svn_node_file) || (kind == svn_node_none))
                {
                  /* File entries are ... just fine! */
                  SVN_ERR (add_status_structure (statushash, fullpath,
                                                 entry, get_all, pool));
                }
            }
        }
    }
  
  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;
}





/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
