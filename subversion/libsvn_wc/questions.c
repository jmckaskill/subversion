/*
 * questions.c:  routines for asking questions about working copies
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



#include <apr_pools.h>
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



svn_error_t *
svn_wc__check_wc (const svn_string_t *path,
                  svn_boolean_t *is_wc,
                  apr_pool_t *pool)
{
  /* Nothing fancy, just check for an administrative subdir and a
     `README' file. */ 

  apr_file_t *f = NULL;
  svn_error_t *err = NULL;
  enum svn_node_kind kind;

  err = svn_io_check_path (path, &kind, pool);
  if (err)
    return err;
  
  if (kind != svn_node_dir)
    *is_wc = FALSE;
  else
    {
      err = svn_wc__open_adm_file (&f, path, SVN_WC__ADM_README,
                                   APR_READ, pool);
      
      /* It really doesn't matter what kind of error it is; if there
         was an error at all, then for our purposes this is not a
         working copy. */
      if (err)
        *is_wc = FALSE;
      else
        {
          *is_wc = TRUE;
          err = svn_wc__close_adm_file (f, path, SVN_WC__ADM_README, 0, pool);
          if (err)
            return err;
        }
    }

  return SVN_NO_ERROR;
}




/*** svn_wc_text_modified_p ***/

/* svn_wc_text_modified_p answers the question:

   "Are the contents of F different than the contents of SVN/text-base/F?"

   or

   "Are the contents of SVN/props/xxx different than SVN/prop-base/xxx?"

   In other words, we're looking to see if a user has made local
   modifications to a file since the last update or commit.

   Note: Assuming that F lives in a directory D at revision V, please
   notice that we are *NOT* answering the question, "are the contents
   of F different than revision V of F?"  While F may be at a different
   revision number than its parent directory, but we're only looking
   for local edits on F, not for consistent directory revisions.  

   TODO:  the logic of the routines on this page might change in the
   future, as they bear some relation to the user interface.  For
   example, if a file is removed -- without telling subversion about
   it -- how should subversion react?  Should it copy the file back
   out of text-base?  Should it ask whether one meant to officially
   mark it for removal?
*/

enum svn_wc__timestamp_kind
{
  svn_wc__text_time = 1,
  svn_wc__prop_time
};


/* Is PATH's timestamp the same as the one recorded in our
   `entries' file?  Return the answer in EQUAL_P.  TIMESTAMP_KIND
   should be one of the enumerated type above. */
static svn_error_t *
timestamps_equal_p (svn_boolean_t *equal_p,
                    svn_string_t *path,
                    const enum svn_wc__timestamp_kind timestamp_kind,
                    apr_pool_t *pool)
{
  svn_error_t *err;
  apr_time_t wfile_time, entrytime;
  svn_string_t *dirpath, *entryname;
  apr_hash_t *entries = NULL;
  struct svn_wc_entry_t *entry;
  enum svn_node_kind kind;

  svn_io_check_path (path, &kind, pool);
  if (kind == svn_node_dir)
    {
      dirpath = path;
      entryname = svn_string_create (SVN_WC_ENTRY_THIS_DIR, pool);
    }
  else
    svn_path_split (path, &dirpath, &entryname, svn_path_local_style, pool);

  /* Get the timestamp from the entries file */
  err = svn_wc__entries_read (&entries, dirpath, pool);
  if (err)
    return err;
  entry = apr_hash_get (entries, entryname->data, entryname->len);

  /* Get the timestamp from the working file and the entry */
  if (timestamp_kind == svn_wc__text_time)
    {
      err = svn_io_file_affected_time (&wfile_time, path, pool);
      if (err) return err;

      entrytime = entry->text_time;
    }
  
  else if (timestamp_kind == svn_wc__prop_time)
    {
      svn_string_t *prop_path;

      err = svn_wc__prop_path (&prop_path, path, 0, pool);
      if (err) return err;

      err = svn_io_file_affected_time (&wfile_time, prop_path, pool);
      if (err) return err;      

      entrytime = entry->prop_time;
    }

  if (entry == NULL || (! entrytime))
    {
      /* TODO: If either timestamp is inaccessible, the test cannot
         return an answer.  Assume that the timestamps are
         different. */
      *equal_p = FALSE;
      return SVN_NO_ERROR;
    }

  {
    /* Put the disk timestamp through a string conversion, so it's
       at the same resolution as entry timestamps. */
    svn_string_t *tstr = svn_wc__time_to_string (wfile_time, pool);
    wfile_time = svn_wc__string_to_time (tstr);
  }
  
  if (wfile_time == entrytime)
    *equal_p = TRUE;
  else
    *equal_p = FALSE;

  return SVN_NO_ERROR;
}




/* Set *DIFFERENT_P to non-zero if FILENAME1 and FILENAME2 have
   different sizes, else set to zero.  If the size of one or both of
   the files cannot be determined, then the sizes are not "definitely"
   different, so *DIFFERENT_P will be set to 0. */
static svn_error_t *
filesizes_definitely_different_p (svn_boolean_t *different_p,
                                  svn_string_t *filename1,
                                  svn_string_t *filename2,
                                  apr_pool_t *pool)
{
  apr_finfo_t finfo1;
  apr_finfo_t finfo2;
  apr_status_t status;

  /* Stat both files */
  status = apr_stat (&finfo1, filename1->data, pool);
  if (status)
    {
      /* If we got an error stat'ing a file, it could be because the
         file was removed... or who knows.  Whatever the case, we
         don't know if the filesizes are definitely different, so
         assume that they're not. */
      *different_p = FALSE;
      return SVN_NO_ERROR;
    }

  status = apr_stat (&finfo2, filename2->data, pool);
  if (status)
    {
      /* See previous comment. */
      *different_p = FALSE;
      return SVN_NO_ERROR;
    }


  /* Examine file sizes */
  if (finfo1.size == finfo2.size)
    *different_p = FALSE;
  else
    *different_p = TRUE;

  return SVN_NO_ERROR;
}


/* Do a byte-for-byte comparison of FILE1 and FILE2. */
static svn_error_t *
contents_identical_p (svn_boolean_t *identical_p,
                      svn_string_t *file1,
                      svn_string_t *file2,
                      apr_pool_t *pool)
{
  apr_status_t status;
  apr_size_t bytes_read1, bytes_read2;
  char buf1[BUFSIZ], buf2[BUFSIZ];
  apr_file_t *file1_h = NULL;
  apr_file_t *file2_h = NULL;

  status = apr_open (&file1_h, file1->data, APR_READ, APR_OS_DEFAULT, pool);
  if (status)
    return svn_error_createf
      (status, 0, NULL, pool,
       "contents_identical_p: apr_open failed on `%s'", file1->data);

  status = apr_open (&file2_h, file2->data, APR_READ, APR_OS_DEFAULT, pool);
  if (status)
    return svn_error_createf
      (status, 0, NULL, pool,
       "contents_identical_p: apr_open failed on `%s'", file2->data);

  *identical_p = TRUE;  /* assume TRUE, until disproved below */
  while (!APR_STATUS_IS_EOF(status))
    {
      status = apr_full_read (file1_h, buf1, BUFSIZ, &bytes_read1);
      if (status && !APR_STATUS_IS_EOF(status))
        return svn_error_createf
          (status, 0, NULL, pool,
           "contents_identical_p: apr_full_read() failed on %s.", file1->data);

      status = apr_full_read (file2_h, buf2, BUFSIZ, &bytes_read2);
      if (status && !APR_STATUS_IS_EOF(status))
        return svn_error_createf
          (status, 0, NULL, pool,
           "contents_identical_p: apr_full_read() failed on %s.", file2->data);
      
      if ((bytes_read1 != bytes_read2)
          || (memcmp (buf1, buf2, bytes_read1)))
        {
          *identical_p = FALSE;
          break;
        }
    }

  status = apr_close (file1_h);
  if (status)
    return svn_error_createf (status, 0, NULL, pool,
                             "contents_identical_p: apr_close failed on %s.",
                              file1->data);

  status = apr_close (file2_h);
  if (status)
    return svn_error_createf (status, 0, NULL, pool,
                             "contents_identical_p: apr_close failed on %s.",
                             file2->data);

  return SVN_NO_ERROR;
}



svn_error_t *
svn_wc__files_contents_same_p (svn_boolean_t *same,
                               svn_string_t *file1,
                               svn_string_t *file2,
                               apr_pool_t *pool)
{
  svn_error_t *err;
  svn_boolean_t q;

  err = filesizes_definitely_different_p (&q, file1, file2, pool);
  if (err)
    return err;

  if (q)
    {
      *same = 0;
      return SVN_NO_ERROR;
    }
  
  err = contents_identical_p (&q, file1, file2, pool);
  if (err)
    return err;

  if (q)
    *same = 1;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_text_modified_p (svn_boolean_t *modified_p,
                        svn_string_t *filename,
                        apr_pool_t *pool)
{
  svn_boolean_t identical_p;
  svn_error_t *err;
  svn_string_t *textbase_filename;
  svn_boolean_t different_filesizes, equal_timestamps;

  /* Sanity check:  if the path doesn't exist, return FALSE. */
  enum svn_node_kind kind;
  err = svn_io_check_path (filename, &kind, pool);
  if (err) return err;
  if (kind != svn_node_file)
    {
      *modified_p = FALSE;
      return SVN_NO_ERROR;
    }              

  /* Get the full path of the textbase revision of filename */
  textbase_filename = svn_wc__text_base_path (filename, 0, pool);

  /* Simple case:  if there's no text-base revision of the file, all we
     can do is look at timestamps.  */
  if (! textbase_filename)
    {
      err = timestamps_equal_p (&equal_timestamps, filename,
                                svn_wc__text_time, pool);
      if (err) return err;

      if (equal_timestamps)
        *modified_p = FALSE;
      else
        *modified_p = TRUE;

      return SVN_NO_ERROR;
    }
  
  /* Better case:  we have a text-base revision of the file, so there
     are at least three tests we can try in succession. */
  else
    {     
      /* Easy-answer attempt #1:  */
      
      /* Check if the the local and textbase file have *definitely*
         different filesizes. */
      err = filesizes_definitely_different_p (&different_filesizes,
                                              filename, textbase_filename,
                                              pool);
      if (err) return err;
      
      if (different_filesizes) 
        {
          *modified_p = TRUE;
          return SVN_NO_ERROR;
        }
      
      /* Easy-answer attempt #2:  */
      
      /* See if the local file's timestamp is the same as the one recorded
         in the administrative directory.  */
      err = timestamps_equal_p (&equal_timestamps, filename,
                                svn_wc__text_time, pool);
      if (err) return err;
      
      if (equal_timestamps)
        {
          *modified_p = FALSE;
          return SVN_NO_ERROR;
        }
      
      /* Last ditch attempt:  */

      /* If we get here, then we know that the filesizes are the same,
         but the timestamps are different.  That's still not enough
         evidence to make a correct decision.  So we just give up and
         get the answer the hard way -- a brute force, byte-for-byte
         comparison. */
      err = contents_identical_p (&identical_p,
                                  filename,
                                  textbase_filename,
                                  pool);
      if (err)
        return err;
      
      if (identical_p)
        *modified_p = FALSE;
      else
        *modified_p = TRUE;
      
      return SVN_NO_ERROR;
    }
}




svn_error_t *
svn_wc_props_modified_p (svn_boolean_t *modified_p,
                         svn_string_t *path,
                         apr_pool_t *pool)
{
  enum svn_node_kind kind;
  svn_error_t *err;
  svn_string_t *prop_path;
  svn_string_t *prop_base_path;
  svn_boolean_t different_filesizes, equal_timestamps;

  /* First, get the prop_path from the original path */
  err = svn_wc__prop_path (&prop_path, path, 0, pool);
  if (err) return err;
  
  /* Sanity check:  if the prop_path doesn't exist, return FALSE. */
  err = svn_io_check_path (prop_path, &kind, pool);
  if (err) return err;
  if (kind != svn_node_file)
    {
      *modified_p = FALSE;
      return SVN_NO_ERROR;
    }              

  /* Get the full path of the prop-base `pristine' file */
  err = svn_wc__prop_base_path (&prop_base_path, path, 0, pool);
  if (err) return err;

  /* Sanity check:  if the prop_base_path doesn't exist, return FALSE. */
  err = svn_io_check_path (prop_base_path, &kind, pool);
  if (err) return err;
  if (kind != svn_node_file)
    {
      /* If we get here, we know that the property file exists, but
         the base property file doesn't.  Somebody must have started
         adding properties, so that's a local change! */
      *modified_p = TRUE;
      return SVN_NO_ERROR;
    }              
  
  /* There are at least three tests we can try in succession. */
  
  /* Easy-answer attempt #1:  */
  
  /* Check if the the local and prop-base file have *definitely*
     different filesizes. */
  err = filesizes_definitely_different_p (&different_filesizes,
                                          prop_path, prop_base_path,
                                          pool);
  if (err) return err;
  
  if (different_filesizes) 
    {
      *modified_p = TRUE;
      return SVN_NO_ERROR;
    }
  
  /* Easy-answer attempt #2:  */
      
  /* See if the local file's timestamp is the same as the one recorded
     in the administrative directory.  */
  err = timestamps_equal_p (&equal_timestamps, path,
                            svn_wc__prop_time, pool);
  if (err) return err;
  
  if (equal_timestamps)
    {
      *modified_p = FALSE;
      return SVN_NO_ERROR;
    }
  
  /* Last ditch attempt:  */
  
  /* If we get here, then we know that the filesizes are the same,
     but the timestamps are different.  That's still not enough
     evidence to make a correct decision;  we need to look at the
     files' contents directly.

     However, doing a byte-for-byte comparison won't work.  The two
     properties files may have the *exact* same name/value pairs, but
     arranged in a different order.  (Our hashdump format makes no
     guarantees about ordering.)

     Therefore, rather than use contents_identical_p(), we use
     svn_wc__get_local_propchanges(). */
  {
    apr_array_header_t *local_propchanges;
    apr_hash_t *localprops = apr_make_hash (pool);
    apr_hash_t *baseprops = apr_make_hash (pool);

    err = svn_wc__load_prop_file (prop_path, localprops, pool);
    if (err) return err;

    err = svn_wc__load_prop_file (prop_base_path, baseprops, pool);
    if (err) return err;

    err = svn_wc__get_local_propchanges (&local_propchanges,
                                         localprops,
                                         baseprops,
                                         pool);
    if (err) return err;
                                         
    if (local_propchanges->nelts > 0)
      *modified_p = TRUE;
    else
      *modified_p = FALSE;
  }

  
  return SVN_NO_ERROR;
}






svn_error_t *
svn_wc_conflicted_p (svn_boolean_t *text_conflicted_p,
                     svn_boolean_t *prop_conflicted_p,
                     svn_string_t *dir_path,
                     svn_wc_entry_t *entry,
                     apr_pool_t *pool)
{
  svn_error_t *err;
  svn_string_t *rej_file, *prej_file;
  svn_string_t *rej_path, *prej_path;

  /* Note:  it's assumed that ENTRY is a particular entry inside
     DIR_PATH's entries file. */
  
  if (entry->state & SVN_WC_ENTRY_CONFLICTED)
    {
      /* Get up to two reject files */
      rej_file = apr_hash_get (entry->attributes,
                               SVN_WC_ENTRY_ATTR_REJFILE,
                               APR_HASH_KEY_STRING);

      prej_file = apr_hash_get (entry->attributes,
                                SVN_WC_ENTRY_ATTR_PREJFILE,
                                APR_HASH_KEY_STRING);
      
      if ((! rej_file) && (! prej_file))
        {
          /* freaky, why is the entry marked as conflicted, but there
             are no reject files?  assume there's no more conflict.
             but maybe this should be an error someday.  :) */
          *text_conflicted_p = FALSE;
          *prop_conflicted_p = FALSE;
        }

      else
        {
          enum svn_node_kind kind;

          if (rej_file)
            {
              rej_path = svn_string_dup (dir_path, pool);
              svn_path_add_component (rej_path, rej_file,
                                      svn_path_local_style);

              err = svn_io_check_path (rej_path, &kind, pool);
              if (err) return err;

              if (kind == svn_node_file)
                /* The textual conflict file is still there. */
                *text_conflicted_p = TRUE;
              else
                /* The textual conflict file has been removed. */
                *text_conflicted_p = FALSE;  
            }
          else
            /* There's no mention of a .rej file at all */
            *text_conflicted_p = FALSE;

          if (prej_file)
            {
              prej_path = svn_string_dup (dir_path, pool);
              svn_path_add_component (prej_path, prej_file,
                                      svn_path_local_style);

              err = svn_io_check_path (prej_path, &kind, pool);
              if (err) return err;

              if (kind == svn_node_file)
                /* The property conflict file is still there. */
                *prop_conflicted_p = TRUE;
              else
                /* The property conflict file has been removed. */
                *prop_conflicted_p = FALSE;
            }
          else
            /* There's no mention of a .prej file at all. */
            *prop_conflicted_p = FALSE;
        }
    }
  else
    {
      /* The entry isn't marked with `conflict="true"' in the first
         place.  */
      *text_conflicted_p = FALSE;
      *prop_conflicted_p = FALSE;
    }

  return SVN_NO_ERROR;
}




/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: */
