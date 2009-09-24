/*
 * questions.c:  routines for asking questions about working copies
 *
 * ====================================================================
 *    Licensed to the Subversion Corporation (SVN Corp.) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The SVN Corp. licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */



#include <string.h>

#include <apr_pools.h>
#include <apr_file_io.h>
#include <apr_file_info.h>
#include <apr_time.h>

#include "svn_pools.h"
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_time.h"
#include "svn_io.h"
#include "svn_props.h"

#include "wc.h"
#include "adm_files.h"
#include "entries.h"
#include "props.h"
#include "translate.h"
#include "wc_db.h"
#include "lock.h"
#include "tree_conflicts.h"

#include "svn_private_config.h"
#include "private/svn_wc_private.h"



/*** svn_wc_text_modified_p ***/

/* svn_wc_text_modified_p answers the question:

   "Are the contents of F different than the contents of
   .svn/text-base/F.svn-base or .svn/tmp/text-base/F.svn-base?"

   In the first case, we're looking to see if a user has made local
   modifications to a file since the last update or commit.  In the
   second, the file may not be versioned yet (it doesn't exist in
   entries).  Support for the latter case came about to facilitate
   forced checkouts, updates, and switches, where an unversioned file
   may obstruct a file about to be added.

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


/* Set *MODIFIED_P to TRUE if (after translation) VERSIONED_FILE_ABSPATH
 * differs from BASE_FILE_ABSPATH, else to FALSE if not.  Also verify that
 * BASE_FILE_ABSPATH matches the stored checksum for VERSIONED_FILE_ABSPATH,
 * if verify_checksum is TRUE. If checksum does not match, return the error
 * SVN_ERR_WC_CORRUPT_TEXT_BASE.
 *
 * DB is a wc_db; use SCRATCH_POOL for temporary allocation.
 */
static svn_error_t *
compare_and_verify(svn_boolean_t *modified_p,
                   svn_wc__db_t *db,
                   const char *versioned_file_abspath,
                   const char *base_file_abspath,
                   svn_boolean_t compare_textbases,
                   svn_boolean_t verify_checksum,
                   apr_pool_t *scratch_pool)
{
  svn_boolean_t same;
  svn_subst_eol_style_t eol_style;
  const char *eol_str;
  apr_hash_t *keywords;
  svn_boolean_t special;
  svn_boolean_t need_translation;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(base_file_abspath));
  SVN_ERR_ASSERT(svn_dirent_is_absolute(versioned_file_abspath));

  SVN_ERR(svn_wc__get_eol_style(&eol_style, &eol_str, db,
                                versioned_file_abspath, scratch_pool,
                                scratch_pool));
  SVN_ERR(svn_wc__get_keywords(&keywords, db, versioned_file_abspath, NULL,
                               scratch_pool, scratch_pool));
  SVN_ERR(svn_wc__get_special(&special, db, versioned_file_abspath,
                              scratch_pool));

  need_translation = svn_subst_translation_required(eol_style, eol_str,
                                                    keywords, special, TRUE);

  if (verify_checksum || need_translation)
    {
      /* Reading files is necessary. */
      svn_checksum_t *checksum;
      svn_stream_t *v_stream;  /* versioned_file */
      svn_stream_t *b_stream;  /* base_file */
      svn_checksum_t *node_checksum;

      SVN_ERR(svn_stream_open_readonly(&b_stream, base_file_abspath,
                                       scratch_pool, scratch_pool));

      if (verify_checksum)
        {
          /* Need checksum verification, so read checksum from entries file
           * and setup checksummed stream for base file. */
          SVN_ERR(svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL,
                                       NULL, &node_checksum, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL,
                                       NULL, NULL, NULL,
                                       NULL, NULL, NULL, NULL, NULL,
                                       db, versioned_file_abspath,
                                       scratch_pool, scratch_pool));

          if (node_checksum)
            b_stream = svn_stream_checksummed2(b_stream, &checksum, NULL,
                                               svn_checksum_md5, TRUE,
                                               scratch_pool);
        }

      if (special)
        {
          SVN_ERR(svn_subst_read_specialfile(&v_stream, versioned_file_abspath,
                                             scratch_pool, scratch_pool));
        }
      else
        {
          SVN_ERR(svn_stream_open_readonly(&v_stream, versioned_file_abspath,
                                           scratch_pool, scratch_pool));

          if (compare_textbases && need_translation)
            {
              if (eol_style == svn_subst_eol_style_native)
                eol_str = SVN_SUBST_NATIVE_EOL_STR;
              else if (eol_style != svn_subst_eol_style_fixed
                       && eol_style != svn_subst_eol_style_none)
                return svn_error_create(SVN_ERR_IO_UNKNOWN_EOL, NULL, NULL);

              /* Wrap file stream to detranslate into normal form. */
              v_stream = svn_subst_stream_translated(v_stream,
                                                     eol_str,
                                                     TRUE,
                                                     keywords,
                                                     FALSE /* expand */,
                                                     scratch_pool);
            }
          else if (need_translation)
            {
              /* Wrap base stream to translate into working copy form. */
              b_stream = svn_subst_stream_translated(b_stream, eol_str,
                                                     FALSE, keywords, TRUE,
                                                     scratch_pool);
            }
        }

      SVN_ERR(svn_stream_contents_same(&same, b_stream, v_stream,
                                       scratch_pool));

      SVN_ERR(svn_stream_close(v_stream));
      SVN_ERR(svn_stream_close(b_stream));

      if (verify_checksum && node_checksum)
        {
          if (!svn_checksum_match(checksum, node_checksum))
            {
              return svn_error_createf(SVN_ERR_WC_CORRUPT_TEXT_BASE, NULL,
                   _("Checksum mismatch indicates corrupt text base: '%s':\n"
                     "   expected:  %s\n"
                     "     actual:  %s\n"),
                  svn_dirent_local_style(base_file_abspath, scratch_pool),
                  svn_checksum_to_cstring_display(node_checksum, scratch_pool),
                  svn_checksum_to_cstring_display(checksum, scratch_pool));
            }
        }
    }
  else
    {
      /* Translation would be a no-op, so compare the original file. */
      SVN_ERR(svn_io_files_contents_same_p(&same, base_file_abspath,
                                           versioned_file_abspath,
                                           scratch_pool));
    }

  *modified_p = (! same);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__internal_versioned_file_modcheck(svn_boolean_t *modified_p,
                                         svn_wc__db_t *db,
                                         const char *versioned_file_abspath,
                                         const char *base_file_abspath,
                                         svn_boolean_t compare_textbases,
                                         apr_pool_t *scratch_pool)
{
  return svn_error_return(compare_and_verify(modified_p, db,
                                             versioned_file_abspath,
                                             base_file_abspath,
                                             compare_textbases, FALSE,
                                             scratch_pool));
}

svn_error_t *
svn_wc__versioned_file_modcheck(svn_boolean_t *modified_p,
                                svn_wc_context_t *wc_ctx,
                                const char *versioned_file_abspath,
                                const char *base_file_abspath,
                                svn_boolean_t compare_textbases,
                                apr_pool_t *scratch_pool)
{
  return svn_error_return(svn_wc__internal_versioned_file_modcheck(
                            modified_p, wc_ctx->db, versioned_file_abspath,
                            base_file_abspath, compare_textbases,
                            scratch_pool));
}

svn_error_t *
svn_wc__text_modified_internal_p(svn_boolean_t *modified_p,
                                 svn_wc__db_t *db,
                                 const char *local_abspath,
                                 svn_boolean_t force_comparison,
                                 svn_boolean_t compare_textbases,
                                 apr_pool_t *scratch_pool)
{
  const char *textbase_abspath;
  svn_node_kind_t kind;
  svn_error_t *err;
  apr_finfo_t finfo;

  /* No matter which way you look at it, the file needs to exist. */
  err = svn_io_stat(&finfo, local_abspath,
                    APR_FINFO_SIZE | APR_FINFO_MTIME | APR_FINFO_TYPE
                    | APR_FINFO_LINK, scratch_pool);
  if ((err && APR_STATUS_IS_ENOENT(err->apr_err))
      || (!err && !(finfo.filetype == APR_REG ||
                    finfo.filetype == APR_LNK)))
    {
      /* There is no entity, or, the entity is not a regular file or link.
         So, it can't be modified. */
      svn_error_clear(err);
      *modified_p = FALSE;
      return SVN_NO_ERROR;
    }
  else if (err)
    return err;

  if (! force_comparison)
    {
      svn_filesize_t translated_size;
      apr_time_t last_mod_time;

      /* We're allowed to use a heuristic to determine whether files may
         have changed.  The heuristic has these steps:


         1. Compare the working file's size
            with the size cached in the entries file
         2. If they differ, do a full file compare
         3. Compare the working file's timestamp
            with the timestamp cached in the entries file
         4. If they differ, do a full file compare
         5. Otherwise, return indicating an unchanged file.

         There are 2 problematic situations which may occur:

         1. The cached working size is missing
         --> In this case, we forget we ever tried to compare
             and skip to the timestamp comparison.  This is
             because old working copies do not contain cached sizes

         2. The cached timestamp is missing
         --> In this case, we forget we ever tried to compare
             and skip to full file comparison.  This is because
             the timestamp will be removed when the library
             updates a locally changed file.  (ie, this only happens
             when the file was locally modified.)

      */

      /* Read the relevant info */
      err = svn_wc__db_read_info(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                 NULL, NULL, &last_mod_time, NULL, NULL,
                                 &translated_size , NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL,
                                 db, local_abspath,
                                 scratch_pool, scratch_pool);
      if (err)
        {
          svn_error_clear(err);
          goto compare_them;
        }

      /* Compare the sizes, if applicable */
      if (translated_size != SVN_WC_ENTRY_WORKING_SIZE_UNKNOWN
          && finfo.size != translated_size)
        goto compare_them;


      /* Compare the timestamps

         Note: text_time == 0 means absent from entries,
               which also means the timestamps won't be equal,
               so there's no need to explicitly check the 'absent' value. */
      if (last_mod_time != finfo.mtime)
        goto compare_them;


      *modified_p = FALSE;
      return SVN_NO_ERROR;
    }

 compare_them:
 /* If there's no text-base file, we have to assume the working file
     is modified.  For example, a file scheduled for addition but not
     yet committed. */
  /* We used to stat for the working base here, but we just give
     compare_and_verify a try; we'll check for errors afterwards */
  SVN_ERR(svn_wc__text_base_path(&textbase_abspath, db, local_abspath, FALSE,
                                 scratch_pool));

  /* Check all bytes, and verify checksum if requested. */
  err = compare_and_verify(modified_p,
                           db,
                           local_abspath,
                           textbase_abspath,
                           compare_textbases,
                           force_comparison,
                           scratch_pool);
  if (err)
    {
      svn_error_t *err2;

      err2 = svn_io_check_path(textbase_abspath, &kind, scratch_pool);
      if (! err2 && kind != svn_node_file)
        {
          svn_error_clear(err);
          *modified_p = TRUE;
          return SVN_NO_ERROR;
        }

      if (err2)
        {
          svn_error_clear(err);
          return err2;
        }
      
      return err;
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_text_modified_p2(svn_boolean_t *modified_p,
                        svn_wc_context_t *wc_ctx,
                        const char *local_abspath,
                        svn_boolean_t force_comparison,
                        apr_pool_t *scratch_pool)
{
  return svn_wc__text_modified_internal_p(modified_p, wc_ctx->db,
                                          local_abspath, force_comparison,
                                          TRUE, scratch_pool);
}



svn_error_t *
svn_wc__internal_conflicted_p(svn_boolean_t *text_conflicted_p,
                              svn_boolean_t *prop_conflicted_p,
                              svn_boolean_t *tree_conflicted_p,
                              svn_wc__db_t *db,
                              const char *local_abspath,
                              apr_pool_t *scratch_pool)
{
  svn_node_kind_t kind;
  svn_wc__db_kind_t node_kind;
  const char *prop_rej_file;
  const char *conflict_old;
  const char *conflict_new;
  const char *conflict_working;
  const char* dir_path = svn_dirent_dirname(local_abspath, scratch_pool);

  SVN_ERR(svn_wc__db_read_info(NULL, &node_kind, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL, NULL,
                               &conflict_old, &conflict_new,
                               &conflict_working, &prop_rej_file, NULL,
                               db, local_abspath, scratch_pool,
                               scratch_pool));

  if (text_conflicted_p)
    {
      *text_conflicted_p = FALSE;

      /* Look for any text conflict, exercising only as much effort as
         necessary to obtain a definitive answer.  This only applies to
         files, but we don't have to explicitly check that entry is a
         file, since these attributes would never be set on a directory
         anyway.  A conflict file entry notation only counts if the
         conflict file still exists on disk.  */

      /* ### the conflict paths are currently relative.  sure would be nice
         ### if we store them as absolute paths... */

      if (conflict_old)
        {
          const char *path = svn_dirent_join(dir_path, conflict_old,
                                             scratch_pool);
          SVN_ERR(svn_io_check_path(path, &kind, scratch_pool));
          *text_conflicted_p = (kind == svn_node_file);
        }

      if ((! *text_conflicted_p) && (conflict_new))
        {
          const char *path = svn_dirent_join(dir_path, conflict_new,
                                             scratch_pool);
          SVN_ERR(svn_io_check_path(path, &kind, scratch_pool));
          *text_conflicted_p = (kind == svn_node_file);
        }

      if ((! *text_conflicted_p) && (conflict_working))
        {
          const char *path = svn_dirent_join(dir_path, conflict_working,
                                             scratch_pool);
          SVN_ERR(svn_io_check_path(path, &kind, scratch_pool));
          *text_conflicted_p = (kind == svn_node_file);
        }
    }

  /* What about prop conflicts? */
  if (prop_conflicted_p)
    {
      *prop_conflicted_p = FALSE;

      if (prop_rej_file)
        {
          /* A dir's .prej file is _inside_ the dir. */
          const char *path;

          if (node_kind == svn_wc__db_kind_dir)
            path = svn_dirent_join(local_abspath, prop_rej_file, scratch_pool);
          else
            path = svn_dirent_join(dir_path, prop_rej_file, scratch_pool);

          SVN_ERR(svn_io_check_path(path, &kind, scratch_pool));
          *prop_conflicted_p = (kind == svn_node_file);
        }
    }

  /* Find out whether it's a tree conflict victim. */
  if (tree_conflicted_p)
    {
      svn_wc_conflict_description2_t *conflict;

      SVN_ERR(svn_wc__db_op_read_tree_conflict(&conflict, db, local_abspath,
                                               scratch_pool, scratch_pool));
      *tree_conflicted_p = (conflict != NULL);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_conflicted_p3(svn_boolean_t *text_conflicted_p,
                     svn_boolean_t *prop_conflicted_p,
                     svn_boolean_t *tree_conflicted_p,
                     svn_wc_context_t *wc_ctx,
                     const char *local_abspath,
                     apr_pool_t *scratch_pool)
{
  return svn_error_return(svn_wc__internal_conflicted_p(text_conflicted_p,
                                                        prop_conflicted_p,
                                                        tree_conflicted_p,
                                                        wc_ctx->db,
                                                        local_abspath,
                                                        scratch_pool));
}

svn_error_t *
svn_wc__marked_as_binary(svn_boolean_t *marked,
                         const char *local_abspath,
                         svn_wc__db_t *db,
                         apr_pool_t *scratch_pool)
{
  const svn_string_t *value;

  SVN_ERR(svn_wc__internal_propget(&value, db, local_abspath,
                                   SVN_PROP_MIME_TYPE,
                                   scratch_pool, scratch_pool));

  if (value && (svn_mime_type_is_binary(value->data)))
    *marked = TRUE;
  else
    *marked = FALSE;

  return SVN_NO_ERROR;
}


/* Equivalent to the old notion of "entry->schedule == schedule_replace"  */
svn_error_t *
svn_wc__internal_is_replaced(svn_boolean_t *replaced,
                             svn_wc__db_t *db,
                             const char *local_abspath,
                             apr_pool_t *scratch_pool)
{
  svn_wc__db_status_t status;
  svn_boolean_t base_shadowed;
  svn_wc__db_status_t base_status;

  SVN_ERR(svn_wc__db_read_info(
            &status, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL,
            NULL, NULL, &base_shadowed,
            NULL, NULL, NULL, NULL, NULL,
            db, local_abspath,
            scratch_pool, scratch_pool));
  if (base_shadowed)
    SVN_ERR(svn_wc__db_base_get_info(&base_status, NULL, NULL,
                                     NULL, NULL, NULL,
                                     NULL, NULL, NULL,
                                     NULL, NULL, NULL,
                                     NULL, NULL, NULL,
                                     db, local_abspath,
                                     scratch_pool, scratch_pool));

  *replaced = ((status == svn_wc__db_status_added
                || status == svn_wc__db_status_obstructed_add)
               && base_shadowed
               && base_status != svn_wc__db_status_not_present);

  return SVN_NO_ERROR;
}
