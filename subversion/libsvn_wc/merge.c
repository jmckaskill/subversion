/*
 * merge.c:  merging changes into a working file
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

#include "svn_wc.h"
#include "svn_diff.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_pools.h"

#include "wc.h"
#include "entries.h"
#include "adm_files.h"
#include "translate.h"
#include "log.h"
#include "lock.h"

#include "svn_private_config.h"


/* Return a pointer to the svn_prop_t structure from PROP_DIFF
   belonging to PROP_NAME, if any.  NULL otherwise.*/
static const svn_prop_t *
get_prop(const apr_array_header_t *prop_diff,
         const char *prop_name)
{
  if (prop_diff)
    {
      int i;
      for (i = 0; i < prop_diff->nelts; i++)
        {
          const svn_prop_t *elt = &APR_ARRAY_IDX(prop_diff, i, svn_prop_t);
          if (strcmp(elt->name,prop_name) == 0)
            return elt;
        }
    }

  return NULL;
}


/* Detranslate a working copy file MERGE_TARGET to achieve the effect of:

   1. Detranslate
   2. Install new props
   3. Retranslate
   4. Detranslate

   in 1 pass to get a file which can be compared with the left and right
   files which were created with the 'new props' above.

   Property changes make this a little complex though. Changes in

   - svn:mime-type
   - svn:eol-style
   - svn:keywords
   - svn:special

   may change the way a file is translated.

   Effect for svn:mime-type:

     The value for svn:mime-type affects the translation wrt keywords
     and eol-style settings.

   I) both old and new mime-types are texty
      -> just do the translation dance (as lined out below)

   II) the old one is texty, the new one is binary
      -> detranslate with the old eol-style and keywords
         (the new re+detranslation is a no-op)

   III) the old one is binary, the new one texty
      -> detranslate with the new eol-style
         (the old detranslation is a no-op)

   IV) the old and new ones are binary
      -> don't detranslate, just make a straight copy


   Effect for svn:eol-style

   I) On add or change use the new value

   II) otherwise: use the old value (absent means 'no translation')


   Effect for svn:keywords

     Always use old settings (re+detranslation are no-op)


   Effect for svn:special

     Always use the old settings (same reasons as for svn:keywords)

*/
static svn_error_t *
detranslate_wc_file(const char **detranslated_abspath,
                    svn_wc__db_t *db,
                    const char *merge_abspath,
                    svn_boolean_t force_copy,
                    const apr_array_header_t *prop_diff,
                    const char *source_abspath,
                    apr_pool_t *result_pool,
                    apr_pool_t *scratch_pool)
{
  svn_boolean_t is_binary;
  const svn_prop_t *prop;
  svn_subst_eol_style_t style;
  const char *eol;
  apr_hash_t *keywords;
  svn_boolean_t special;

  /* Decide if the merge target currently is a text or binary file. */
  SVN_ERR(svn_wc__marked_as_binary(&is_binary, merge_abspath, db,
                                   scratch_pool));

  /* See if we need to do a straight copy:
     - old and new mime-types are binary, or
     - old mime-type is binary and no new mime-type specified */
  if (is_binary
      && (((prop = get_prop(prop_diff, SVN_PROP_MIME_TYPE))
           && prop->value && svn_mime_type_is_binary(prop->value->data))
          || prop == NULL))
    {
      /* this is case IV above */
      keywords = NULL;
      special = FALSE;
      eol = NULL;
      style = svn_subst_eol_style_none;
    }
  else if ((!is_binary)
           && (prop = get_prop(prop_diff, SVN_PROP_MIME_TYPE))
           && prop->value && svn_mime_type_is_binary(prop->value->data))
    {
      /* Old props indicate texty, new props indicate binary:
         detranslate keywords and old eol-style */
      SVN_ERR(svn_wc__get_keywords(&keywords, db, merge_abspath, NULL,
                                   scratch_pool, scratch_pool));
      SVN_ERR(svn_wc__get_special(&special, db, merge_abspath, scratch_pool));
    }
  else
    {
      /* New props indicate texty, regardless of old props */

      /* In case the file used to be special, detranslate specially */
      SVN_ERR(svn_wc__get_special(&special, db, merge_abspath, scratch_pool));

      if (special)
        {
          keywords = NULL;
          eol = NULL;
          style = svn_subst_eol_style_none;
        }
      else
        {
          /* In case a new eol style was set, use that for detranslation */
          if ((prop = get_prop(prop_diff, SVN_PROP_EOL_STYLE)) && prop->value)
            {
              /* Value added or changed */
              svn_subst_eol_style_from_value(&style, &eol, prop->value->data);
            }
          else if (!is_binary)
            SVN_ERR(svn_wc__get_eol_style(&style, &eol, db, merge_abspath,
                                          scratch_pool, scratch_pool));
          else
            {
              eol = NULL;
              style = svn_subst_eol_style_none;
            }

          /* In case there were keywords, detranslate with keywords
             (iff we were texty) */
          if (!is_binary)
            SVN_ERR(svn_wc__get_keywords(&keywords, db, merge_abspath, NULL,
                                         scratch_pool, scratch_pool));
          else
            keywords = NULL;
        }
    }

  /* Now, detranslate with the settings we created above */

  if (force_copy || keywords || eol || special)
    {
      const char *detranslated;

      /* Force a copy into the temporary wc area to avoid having
         temporary files created below to appear in the actual wc. */

      /* ### svn_subst_copy_and_translate3() also creates a tempfile
         ### internally.  Anyway to piggyback on that? */
      SVN_ERR(svn_io_mktemp(NULL, &detranslated, NULL, NULL,
                            svn_io_file_del_none, scratch_pool, scratch_pool));

      /* Always 'repair' EOLs here, so that we can apply a diff that
         changes from inconsistent newlines and no 'svn:eol-style' to
         consistent newlines and 'svn:eol-style' set.  */

      if (style == svn_subst_eol_style_native)
        eol = SVN_SUBST_NATIVE_EOL_STR;
      else if (style != svn_subst_eol_style_fixed
               && style != svn_subst_eol_style_none)
        return svn_error_create(SVN_ERR_IO_UNKNOWN_EOL, NULL, NULL);

      SVN_ERR(svn_subst_copy_and_translate3(source_abspath,
                                            detranslated,
                                            eol,
                                            TRUE /* repair */,
                                            keywords,
                                            FALSE /* contract keywords */,
                                            special,
                                            scratch_pool));

      SVN_ERR(svn_dirent_get_absolute(detranslated_abspath, detranslated,
                                      result_pool));
    }
  else
    *detranslated_abspath = apr_pstrdup(result_pool, source_abspath);

  return SVN_NO_ERROR;
}

/* Updates (by copying and translating) the eol style in
   OLD_TARGET returning the filename containing the
   correct eol style in NEW_TARGET, if an eol style
   change is contained in PROP_DIFF */
static svn_error_t *
maybe_update_target_eols(const char **new_target_abspath,
                         svn_wc__db_t *db,
                         const char *old_target_abspath,
                         const apr_array_header_t *prop_diff,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  const svn_prop_t *prop = get_prop(prop_diff, SVN_PROP_EOL_STYLE);

  if (prop && prop->value)
    {
      const char *eol;
      const char *tmp_new;

      svn_subst_eol_style_from_value(NULL, &eol, prop->value->data);
      SVN_ERR(svn_io_mktemp(NULL, &tmp_new, NULL, NULL,
                            svn_io_file_del_none, scratch_pool, scratch_pool));

      /* Always 'repair' EOLs here, so that we can apply a diff that
         changes from inconsistent newlines and no 'svn:eol-style' to
         consistent newlines and 'svn:eol-style' set.  */
      SVN_ERR(svn_subst_copy_and_translate3(old_target_abspath,
                                            tmp_new,
                                            eol, TRUE /* repair EOLs */,
                                            NULL, FALSE,
                                            FALSE, scratch_pool));
      *new_target_abspath = apr_pstrdup(result_pool, tmp_new);
    }
  else
    *new_target_abspath = apr_pstrdup(result_pool, old_target_abspath);

  return SVN_NO_ERROR;
}


/* Helper for do_text_merge_internal() below. */
static void
init_conflict_markers(const char **target_marker,
                      const char **left_marker,
                      const char **right_marker,
                      const char *target_label,
                      const char *left_label,
                      const char *right_label,
                      apr_pool_t *pool)
{
  /* Labels fall back to sensible defaults if not specified. */
  if (target_label)
    *target_marker = apr_psprintf(pool, "<<<<<<< %s", target_label);
  else
    *target_marker = "<<<<<<< .working";

  if (left_label)
    *left_marker = apr_psprintf(pool, "||||||| %s", left_label);
  else
    *left_marker = "||||||| .old";

  if (right_label)
    *right_marker = apr_psprintf(pool, ">>>>>>> %s", right_label);
  else
    *right_marker = ">>>>>>> .new";
}

/* Do a 3-way merge of the files at paths LEFT, DETRANSLATED_TARGET,
 * and RIGHT, using diff options provided in OPTIONS.  Store the merge
 * result in the file RESULT_F.
 * If there are conflicts, set *CONTAINS_CONFLICTS to true, and use
 * TARGET_LABEL, LEFT_LABEL, and RIGHT_LABEL as labels for conflict
 * markers.  Else, set *CONTAINS_CONFLICTS to false.
 * Do all allocations in POOL. */
static svn_error_t*
do_text_merge(svn_boolean_t *contains_conflicts,
              apr_file_t *result_f,
              const char *detranslated_target,
              const char *left,
              const char *right,
              const char *target_label,
              const char *left_label,
              const char *right_label,
              svn_diff_file_options_t *options,
              apr_pool_t *pool)
{
  svn_diff_t *diff;
  svn_stream_t *ostream;
  const char *target_marker;
  const char *left_marker;
  const char *right_marker;

  init_conflict_markers(&target_marker, &left_marker, &right_marker,
                        target_label, left_label, right_label, pool);

  SVN_ERR(svn_diff_file_diff3_2(&diff, left, detranslated_target, right,
                                options, pool));

  ostream = svn_stream_from_aprfile2(result_f, TRUE, pool);

  SVN_ERR(svn_diff_file_output_merge2(ostream, diff,
                                      left, detranslated_target, right,
                                      left_marker,
                                      target_marker,
                                      right_marker,
                                      "=======", /* separator */
                                      svn_diff_conflict_display_modified_latest,
                                      pool));
  SVN_ERR(svn_stream_close(ostream));

  *contains_conflicts = svn_diff_contains_conflicts(diff);

  return SVN_NO_ERROR;
}

/* Same as do_text_merge() above, but use the external diff3
 * command DIFF3_CMD to perform the merge.  Pass MERGE_OPTIONS
 * to the diff3 command.  Do all allocations in POOL. */
static svn_error_t*
do_text_merge_external(svn_boolean_t *contains_conflicts,
                            apr_file_t *result_f,
                            const char *detranslated_target,
                            const char *left,
                            const char *right,
                            const char *target_label,
                            const char *left_label,
                            const char *right_label,
                            const char *diff3_cmd,
                            const apr_array_header_t *merge_options,
                            apr_pool_t *pool)
{
  int exit_code;

  SVN_ERR(svn_io_run_diff3_2(&exit_code, ".",
                             detranslated_target, left, right,
                             target_label, left_label, right_label,
                             result_f, diff3_cmd,
                             merge_options, pool));

  *contains_conflicts = exit_code == 1;

  return SVN_NO_ERROR;
}

/* Loggy-copy the merge result obtained during interactive conflict
 * resolution to the file RESULT_TARGET. The merge result is expected
 * in the same directory as TARGET_ABSPATH the same basename as
 * TARGET_ABSPATH, but followed by ".edited".
 * Use LOG_ACCUM as log accumulator.  DB contains an access baton with
 * a write lock for the directory containing RESULT_TARGET.
 * Do all allocations in POOL.
 */
static svn_error_t*
save_merge_result(svn_stringbuf_t **log_accum,
                  svn_wc__db_t *db,
                  const char *target_abspath,
                  const char *result_target,
                  apr_pool_t *pool)
{
  const char *edited_copy;
  const char *merge_dirpath, *merge_filename;

  svn_dirent_split(target_abspath, &merge_dirpath, &merge_filename, pool);

  /* ### Should use preserved-conflict-file-exts. */
  SVN_ERR(svn_io_open_uniquely_named(NULL,
                                     &edited_copy,
                                     merge_dirpath,
                                     merge_filename,
                                     ".edited",
                                     svn_io_file_del_none,
                                     pool, pool));
  SVN_ERR(svn_wc__loggy_copy(log_accum, merge_dirpath,
                             result_target, edited_copy, pool, pool));
  return SVN_NO_ERROR;
}

/* Deal with the RESULT of the conflict resolution callback.
 * LEFT, RIGHT, and MERGE_TARGET are the files involved in
 * the 3-way merge.  Store the result of the 3-way merge in
 * MERGE_OUTCOME.  If the callback did not provide the name to
 * a merged file, use RESULT_TARGET is a fallback.
 * DETRANSLATED_TARGET is the detranslated version of MERGE_TARGET
 * (see detranslate_wc_file() above).  OPTIONS are passed to the
 * diff3 implementation in case a 3-way  merge has to be carried out.
 * Do all allocations in POOL. */
static svn_error_t*
eval_conflict_func_result(enum svn_wc_merge_outcome_t *merge_outcome,
                          svn_wc_conflict_result_t *result,
                          svn_stringbuf_t **log_accum,
                          svn_wc__db_t *db,
                          const char *left,
                          const char *right,
                          const char *merge_target,
                          const char *copyfrom_text,
                          const char *adm_abspath,
                          const char *result_target,
                          const char *detranslated_target,
                          svn_diff_file_options_t *options,
                          apr_pool_t *pool)
{
  switch (result->choice)
    {
      /* If the callback wants to use one of the fulltexts
         to resolve the conflict, so be it.*/
      case svn_wc_conflict_choose_base:
        {
          SVN_ERR(svn_wc__loggy_copy(log_accum, adm_abspath,
                                     left, merge_target, pool, pool));
          *merge_outcome = svn_wc_merge_merged;
          return SVN_NO_ERROR;
        }
      case svn_wc_conflict_choose_theirs_full:
        {
          SVN_ERR(svn_wc__loggy_copy(log_accum, adm_abspath,
                                     right, merge_target, pool, pool));
          *merge_outcome = svn_wc_merge_merged;
          return SVN_NO_ERROR;
        }
      case svn_wc_conflict_choose_mine_full:
        {
          /* Do nothing to merge_target, let it live untouched! */
          *merge_outcome = svn_wc_merge_merged;
          return SVN_NO_ERROR;
        }
      case svn_wc_conflict_choose_theirs_conflict:
      case svn_wc_conflict_choose_mine_conflict:
        {
          apr_file_t *chosen_f;
          const char *chosen_path;
          svn_stream_t *chosen_stream;
          svn_diff_t *diff;
          svn_diff_conflict_display_style_t style;

          style = result->choice == svn_wc_conflict_choose_theirs_conflict
                    ? svn_diff_conflict_display_latest
                    : svn_diff_conflict_display_modified;

          SVN_ERR(svn_wc_create_tmp_file2(&chosen_f,
                                          &chosen_path, adm_abspath,
                                          svn_io_file_del_none,
                                          pool));
          chosen_stream = svn_stream_from_aprfile2(chosen_f, FALSE,
                                                   pool);
          SVN_ERR(svn_diff_file_diff3_2(&diff,
                                        left, detranslated_target, right,
                                        options, pool));
          SVN_ERR(svn_diff_file_output_merge2(chosen_stream, diff,
                                              left,
                                              detranslated_target,
                                              right,
                                              /* markers ignored */
                                              NULL, NULL,
                                              NULL, NULL,
                                              style,
                                              pool));
          SVN_ERR(svn_stream_close(chosen_stream));
          SVN_ERR(svn_wc__loggy_copy(log_accum, adm_abspath,
                                     chosen_path, merge_target, pool, pool));
          *merge_outcome = svn_wc_merge_merged;
          return SVN_NO_ERROR;
        }

        /* For the case of 3-way file merging, we don't
           really distinguish between these return values;
           if the callback claims to have "generally
           resolved" the situation, we still interpret
           that as "OK, we'll assume the merged version is
           good to use". */
      case svn_wc_conflict_choose_merged:
        {
          SVN_ERR(svn_wc__loggy_copy(log_accum, adm_abspath,
                                     /* Look for callback's own
                                        merged-file first: */
                                     result->merged_file
                                       ? result->merged_file
                                       : result_target,
                                     merge_target,
                                     pool, pool));
          *merge_outcome = svn_wc_merge_merged;
          return SVN_NO_ERROR;
        }
      case svn_wc_conflict_choose_postpone:
      default:
        {
          /* Issue #3354: We need to install the copyfrom_text,
           * which now carries conflicts, into ACTUAL, by copying
           * it to the merge target. */
          if (copyfrom_text)
            {
              SVN_ERR(svn_wc__loggy_copy(log_accum, adm_abspath,
                                     copyfrom_text, merge_target,
                                     pool, pool));
            }

          /* Assume conflict remains. */
          return SVN_NO_ERROR;
        }
    }
}

/* Preserve the three pre-merge files, and modify the
   entry (mark as conflicted, track the preserved files). */
static svn_error_t*
preserve_pre_merge_files(svn_stringbuf_t **log_accum,
                         svn_wc__db_t *db,
                         const char *left_abspath,
                         const char *right_abspath,
                         const char *target_abspath,
                         const char *left_label,
                         const char *right_label,
                         const char *target_label,
                         apr_pool_t *pool)
{
  const char *left_copy, *right_copy, *target_copy;
  const char *tmp_left, *tmp_right, *detranslated_target_copy;
  const char *dir_abspath, *target_name;
  svn_wc_entry_t tmp_entry;

  svn_dirent_split(target_abspath, &dir_abspath, &target_name, pool);

  /* I miss Lisp. */

  SVN_ERR(svn_io_open_uniquely_named(NULL,
                                     &left_copy,
                                     dir_abspath,
                                     target_name,
                                     left_label,
                                     svn_io_file_del_none,
                                     pool, pool));

  /* Have I mentioned how much I miss Lisp? */

  SVN_ERR(svn_io_open_uniquely_named(NULL,
                                     &right_copy,
                                     dir_abspath,
                                     target_name,
                                     right_label,
                                     svn_io_file_del_none,
                                     pool, pool));

  /* Why, how much more pleasant to be forced to unroll my loops.
     If I'd been writing in Lisp, I might have mapped an inline
     lambda form over a list, or something equally disgusting.
     Thank goodness C was here to protect me! */

  SVN_ERR(svn_io_open_uniquely_named(NULL,
                                     &target_copy,
                                     dir_abspath,
                                     target_name,
                                     target_label,
                                     svn_io_file_del_none,
                                     pool, pool));

  /* We preserve all the files with keywords expanded and line
     endings in local (working) form. */

  /* Log files require their paths to be in the subtree
     relative to the adm_access path they are executed in.

     Make our LEFT and RIGHT files 'local' if they aren't... */
  if (! svn_dirent_is_ancestor(dir_abspath, left_abspath))
    {
      SVN_ERR(svn_wc_create_tmp_file2(NULL, &tmp_left, dir_abspath,
                                      svn_io_file_del_none, pool));
      SVN_ERR(svn_io_copy_file(left_abspath, tmp_left, TRUE, pool));
    }
  else
    tmp_left = left_abspath;

  if (! svn_dirent_is_ancestor(dir_abspath, right_abspath))
    {
      SVN_ERR(svn_wc_create_tmp_file2(NULL, &tmp_right, dir_abspath,
                                      svn_io_file_del_none, pool));
      SVN_ERR(svn_io_copy_file(right_abspath, tmp_right, TRUE, pool));
    }
  else
    tmp_right = right_abspath;

  /* NOTE: Callers must ensure that the svn:eol-style and
     svn:keywords property values are correct in the currently
     installed props.  With 'svn merge', it's no big deal.  But
     when 'svn up' calls this routine, it needs to make sure that
     this routine is using the newest property values that may
     have been received *during* the update.  Since this routine
     will be run from within a log-command, merge_file()
     needs to make sure that a previous log-command to 'install
     latest props' has already executed first.  Ben and I just
     checked, and that is indeed the order in which the log items
     are written, so everything should be fine.  Really.  */

  /* Create LEFT and RIGHT backup files, in expanded form.
     We use merge_target's current properties to do the translation. */
  /* Derive the basenames of the 3 backup files. */
  SVN_ERR(svn_wc__loggy_translated_file(log_accum,
                                        dir_abspath,
                                        left_copy, tmp_left,
                                        target_abspath, pool, pool));
  SVN_ERR(svn_wc__loggy_translated_file(log_accum,
                                        dir_abspath,
                                        right_copy, tmp_right,
                                        target_abspath, pool, pool));

  /* Back up MERGE_TARGET through detranslation/retranslation:
     the new translation properties may not match the current ones */
  SVN_ERR(svn_wc__internal_translated_file(
           &detranslated_target_copy, target_abspath, db, target_abspath,
           SVN_WC_TRANSLATE_TO_NF | SVN_WC_TRANSLATE_NO_OUTPUT_CLEANUP,
           pool, pool));
  SVN_ERR(svn_wc__loggy_translated_file(log_accum,
                                        dir_abspath,
                                        target_copy, detranslated_target_copy,
                                        target_abspath, pool, pool));

  tmp_entry.conflict_old = svn_dirent_is_child(dir_abspath, left_copy, pool);
  tmp_entry.conflict_new = svn_dirent_is_child(dir_abspath, right_copy, pool);
  tmp_entry.conflict_wrk = svn_dirent_basename(target_copy, pool);

  /* Mark merge_target's entry as "Conflicted", and start tracking
     the backup files in the entry as well. */
  SVN_ERR(svn_wc__loggy_entry_modify(log_accum, dir_abspath,
                                     target_abspath, &tmp_entry,
                                     SVN_WC__ENTRY_MODIFY_CONFLICT_OLD
                                       | SVN_WC__ENTRY_MODIFY_CONFLICT_NEW
                                       | SVN_WC__ENTRY_MODIFY_CONFLICT_WRK,
                                     pool, pool));

  return SVN_NO_ERROR;
}

/* Helper for maybe_resolve_conflicts() below. */
static const svn_wc_conflict_description_t *
setup_text_conflict_desc(const char *left,
                         const char *right,
                         const char *merge_abspath,
                         const svn_wc_conflict_version_t *left_version,
                         const svn_wc_conflict_version_t *right_version,
                         const char *result_target,
                         const char *detranslated_target,
                         const svn_prop_t *mimeprop,
                         svn_boolean_t is_binary,
                         apr_pool_t *pool)
{
  svn_wc_conflict_description2_t *cdesc;

  cdesc = svn_wc_conflict_description_create_text2(merge_abspath, pool);
  cdesc->is_binary = FALSE;
  cdesc->mime_type = (mimeprop && mimeprop->value)
                     ? mimeprop->value->data : NULL,
  cdesc->base_file = left;
  cdesc->their_file = right;
  cdesc->my_file = detranslated_target;
  cdesc->merged_file = result_target;

  cdesc->src_left_version = left_version;
  cdesc->src_right_version = right_version;

  return svn_wc__cd2_to_cd(cdesc, pool);
}

/* XXX Insane amount of parameters... */
static svn_error_t*
maybe_resolve_conflicts(svn_stringbuf_t **log_accum,
                        svn_wc__db_t *db,
                        const char *left,
                        const char *right,
                        const char *merge_target,
                        const char *copyfrom_text,
                        const char *left_label,
                        const char *right_label,
                        const char *target_label,
                        enum svn_wc_merge_outcome_t *merge_outcome,
                        const svn_wc_conflict_version_t *left_version,
                        const svn_wc_conflict_version_t *right_version,
                        const char *result_target,
                        const char *detranslated_target,
                        const svn_prop_t *mimeprop,
                        svn_diff_file_options_t *options,
                        svn_wc_conflict_resolver_func_t conflict_func,
                        void *conflict_baton,
                        svn_cancel_func_t cancel_func,
                        void *cancel_baton,
                        apr_pool_t *pool)
{
  svn_wc_conflict_result_t *result = NULL;
  const char *left_abspath, *right_abspath, *target_abspath, *dir_abspath;

  SVN_ERR(svn_dirent_get_absolute(&left_abspath, left, pool));
  SVN_ERR(svn_dirent_get_absolute(&right_abspath, right, pool));
  SVN_ERR(svn_dirent_get_absolute(&target_abspath, merge_target, pool));

  dir_abspath = svn_dirent_dirname(target_abspath, pool);

  /* Give the conflict resolution callback a chance to clean
     up the conflicts before we mark the file 'conflicted' */
  if (!conflict_func)
    {
      /* If there is no interactive conflict resolution then we are effectively
         postponing conflict resolution. */
      result = svn_wc_create_conflict_result(svn_wc_conflict_choose_postpone,
                                             NULL, pool);
    }
  else
    {
      const svn_wc_conflict_description_t *cdesc;

      cdesc = setup_text_conflict_desc(left_abspath,
                                       right_abspath,
                                       target_abspath,
                                       left_version,
                                       right_version,
                                       result_target,
                                       detranslated_target,
                                       mimeprop,
                                       FALSE,
                                       pool);

      SVN_ERR(conflict_func(&result, cdesc, conflict_baton, pool));
      if (result == NULL)
        return svn_error_create(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE,
                                NULL, _("Conflict callback violated API:"
                                        " returned no results"));
      if (result->save_merged)
        SVN_ERR(save_merge_result(log_accum,
                                  db,
                                  target_abspath,
                                  /* Look for callback's own
                                     merged-file first: */
                                  result->merged_file
                                    ? result->merged_file
                                    : result_target,
                                  pool));
    }

  SVN_ERR(eval_conflict_func_result(merge_outcome,
                                    result,
                                    log_accum,
                                    db,
                                    left,
                                    right,
                                    merge_target,
                                    copyfrom_text,
                                    dir_abspath,
                                    result_target,
                                    detranslated_target,
                                    options,
                                    pool));

  if (result->choice != svn_wc_conflict_choose_postpone)
    /* The conflicts have been dealt with, nothing else
     * to do for us here. */
    return SVN_NO_ERROR;

  /* The conflicts have not been dealt with. */
  SVN_ERR(preserve_pre_merge_files(log_accum,
                                   db,
                                   left_abspath,
                                   right_abspath,
                                   target_abspath,
                                   left_label,
                                   right_label,
                                   target_label,
                                   pool));

  *merge_outcome = svn_wc_merge_conflict;

  return SVN_NO_ERROR;
}

/* XXX Insane amount of parameters... */
static svn_error_t*
merge_text_file(svn_stringbuf_t **log_accum,
                enum svn_wc_merge_outcome_t *merge_outcome,
                svn_wc__db_t *db,
                const char *left,
                const char *right,
                const char *merge_target,
                svn_wc_adm_access_t *adm_access,
                const char *left_label,
                const char *right_label,
                const char *target_label,
                svn_boolean_t dry_run,
                const char *diff3_cmd,
                const apr_array_header_t *merge_options,
                const svn_wc_conflict_version_t *left_version,
                const svn_wc_conflict_version_t *right_version,
                const char *copyfrom_text,
                const char *detranslated_target_abspath,
                const svn_prop_t *mimeprop,
                svn_wc_conflict_resolver_func_t conflict_func,
                void *conflict_baton,
                svn_cancel_func_t cancel_func,
                void *cancel_baton,
                apr_pool_t *pool)
{
  svn_diff_file_options_t *options;
  svn_boolean_t contains_conflicts;
  apr_file_t *result_f;
  const char *result_target;
  const char *base_name = svn_dirent_basename(merge_target, pool);
  const char *temp_dir;

  /* Open a second temporary file for writing; this is where diff3
     will write the merged results.  We want to use a tempfile
     with a name that reflects the original, in case this
     ultimately winds up in a conflict resolution editor.  */
  temp_dir = svn_wc__adm_child(svn_wc_adm_access_path(adm_access),
                               SVN_WC__ADM_TMP, pool);
  SVN_ERR(svn_io_open_uniquely_named(&result_f, &result_target,
                                     temp_dir, base_name, ".tmp",
                                     svn_io_file_del_none, pool, pool));

  options = svn_diff_file_options_create(pool);

  if (merge_options)
    SVN_ERR(svn_diff_file_options_parse(options, merge_options, pool));

  /* Run an external merge if requested. */
  if (diff3_cmd)
      SVN_ERR(do_text_merge_external(&contains_conflicts,
                                     result_f,
                                     detranslated_target_abspath,
                                     left,
                                     right,
                                     target_label,
                                     left_label,
                                     right_label,
                                     diff3_cmd,
                                     merge_options,
                                     pool));
  else /* Use internal merge. */
    SVN_ERR(do_text_merge(&contains_conflicts,
                          result_f,
                          detranslated_target_abspath,
                          left,
                          right,
                          target_label,
                          left_label,
                          right_label,
                          options,
                          pool));

  /* Close the output file */
  SVN_ERR(svn_io_file_close(result_f, pool));

  if (contains_conflicts && ! dry_run)
    {
      SVN_ERR(maybe_resolve_conflicts(log_accum,
                                      db,
                                      left,
                                      right,
                                      merge_target,
                                      copyfrom_text,
                                      left_label,
                                      right_label,
                                      target_label,
                                      merge_outcome,
                                      left_version,
                                      right_version,
                                      result_target,
                                      detranslated_target_abspath,
                                      mimeprop,
                                      options,
                                      conflict_func, conflict_baton,
                                      cancel_func, cancel_baton,
                                      pool));
      if (*merge_outcome == svn_wc_merge_merged)
        return SVN_NO_ERROR;
    }
  else if (contains_conflicts && dry_run)
      *merge_outcome = svn_wc_merge_conflict;
  else if (copyfrom_text)
      *merge_outcome = svn_wc_merge_merged;
  else
    {
      svn_boolean_t same, special;
      const char *merge_abspath;

      SVN_ERR(svn_dirent_get_absolute(&merge_abspath, merge_target, pool));

      /* If 'special', then use the detranslated form of the
         target file.  This is so we don't try to follow symlinks,
         but the same treatment is probably also appropriate for
         whatever special file types we may invent in the future. */
      SVN_ERR(svn_wc__get_special(&special, db, merge_abspath, pool));
      SVN_ERR(svn_io_files_contents_same_p(&same, result_target,
                                           (special ?
                                              detranslated_target_abspath :
                                              merge_target),
                                           pool));

      *merge_outcome = same ? svn_wc_merge_unchanged : svn_wc_merge_merged;
    }

  if (*merge_outcome != svn_wc_merge_unchanged && ! dry_run)
    /* replace MERGE_TARGET with the new merged file, expanding. */
    SVN_ERR(svn_wc__loggy_copy(log_accum,
                               svn_wc__adm_access_abspath(adm_access),
                               result_target, merge_target, pool, pool));
  return SVN_NO_ERROR;
}


/* XXX Insane amount of parameters... */
static svn_error_t*
merge_binary_file(svn_stringbuf_t **log_accum,
                  enum svn_wc_merge_outcome_t *merge_outcome,
                  svn_wc__db_t *db,
                  const char *left,
                  const char *right,
                  const char *merge_target,
                  const char *left_label,
                  const char *right_label,
                  const char *target_label,
                  const svn_wc_conflict_version_t *left_version,
                  const svn_wc_conflict_version_t *right_version,
                  const char *detranslated_target_abspath,
                  const svn_prop_t *mimeprop,
                  svn_wc_conflict_resolver_func_t conflict_func,
                  void *conflict_baton,
                  svn_cancel_func_t cancel_func,
                  void *cancel_baton,
                  apr_pool_t *pool)
{
  /* ### when making the binary-file backups, should we be honoring
     keywords and eol stuff?   */
  const char *left_copy, *right_copy;
  const char *left_base, *right_base;
  const char *merge_abspath;
  const char *merge_dirpath, *merge_filename;
  svn_wc_entry_t tmp_entry;

  SVN_ERR(svn_dirent_get_absolute(&merge_abspath, merge_target, pool));

  svn_dirent_split(merge_abspath, &merge_dirpath, &merge_filename, pool);

  /* Give the conflict resolution callback a chance to clean
     up the conflict before we mark the file 'conflicted' */
  if (conflict_func)
    {
      svn_wc_conflict_result_t *result = NULL;
      const svn_wc_conflict_description_t *cdesc;

      cdesc = setup_text_conflict_desc(left, right, merge_abspath,
                                       left_version, right_version,
                                       NULL /* result_target */,
                                       detranslated_target_abspath,
                                       mimeprop, TRUE, pool);

      SVN_ERR(conflict_func(&result, cdesc, conflict_baton, pool));
      if (result == NULL)
        return svn_error_create(SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE,
                                NULL, _("Conflict callback violated API:"
                                        " returned no results"));

      switch (result->choice)
        {
          /* For a binary file, there's no merged file to look at,
             unless the conflict-callback did the merging itself. */
          case svn_wc_conflict_choose_base:
            {
              SVN_ERR(svn_wc__loggy_copy(log_accum,
                                     merge_dirpath,
                                     left, merge_target, pool, pool));
              *merge_outcome = svn_wc_merge_merged;
              return SVN_NO_ERROR;
            }
          case svn_wc_conflict_choose_theirs_full:
            {
              SVN_ERR(svn_wc__loggy_copy(log_accum,
                                     merge_dirpath,
                                     right, merge_target, pool, pool));
              *merge_outcome = svn_wc_merge_merged;
              return SVN_NO_ERROR;
            }
            /* For a binary file, if the response is to use the
               user's file, we do nothing.  We also do nothing if
               the response claims to have already resolved the
               problem.*/
          case svn_wc_conflict_choose_mine_full:
            {
              *merge_outcome = svn_wc_merge_merged;
              return SVN_NO_ERROR;
            }
          case svn_wc_conflict_choose_merged:
            {
              if (! result->merged_file)
                {
                  /* Callback asked us to choose its own
                     merged file, but didn't provide one! */
                  return svn_error_create
                      (SVN_ERR_WC_CONFLICT_RESOLVER_FAILURE,
                       NULL, _("Conflict callback violated API:"
                               " returned no merged file"));
                }
              else
                {
                  SVN_ERR(svn_wc__loggy_copy(log_accum,
                                     merge_dirpath,
                                     result->merged_file, merge_target,
                                     pool, pool));
                  *merge_outcome = svn_wc_merge_merged;
                  return SVN_NO_ERROR;
                }
            }
          case svn_wc_conflict_choose_postpone:
          default:
            {
              /* Assume conflict remains, fall through to code below. */
            }
        }
    }

  /* reserve names for backups of left and right fulltexts */
  SVN_ERR(svn_io_open_uniquely_named(NULL,
                                     &left_copy,
                                     merge_dirpath,
                                     merge_filename,
                                     left_label,
                                     svn_io_file_del_none,
                                     pool, pool));

  SVN_ERR(svn_io_open_uniquely_named(NULL,
                                     &right_copy,
                                     merge_dirpath,
                                     merge_filename,
                                     right_label,
                                     svn_io_file_del_none,
                                     pool, pool));

  /* create the backup files */
  SVN_ERR(svn_io_copy_file(left, left_copy, TRUE, pool));
  SVN_ERR(svn_io_copy_file(right, right_copy, TRUE, pool));

  /* Was the merge target detranslated? */
  if (strcmp(merge_abspath, detranslated_target_abspath) != 0)
    {
      /* Create a .mine file too */
      const char *mine_copy;

      SVN_ERR(svn_io_open_uniquely_named(NULL,
                                         &mine_copy,
                                         merge_dirpath,
                                         merge_filename,
                                         target_label,
                                         svn_io_file_del_none,
                                         pool, pool));
      SVN_ERR(svn_wc__loggy_move(log_accum,
                                 merge_dirpath,
                                 detranslated_target_abspath,
                                 mine_copy,
                                 pool, pool));
      mine_copy = svn_dirent_is_child(merge_dirpath,
                                      mine_copy, pool);
      tmp_entry.conflict_wrk = mine_copy;
    }
  else
    tmp_entry.conflict_wrk = NULL;

  /* Derive the basenames of the backup files. */
  left_base = svn_dirent_basename(left_copy, pool);
  right_base = svn_dirent_basename(right_copy, pool);

  /* Mark merge_target's entry as "Conflicted", and start tracking
     the backup files in the entry as well. */
  tmp_entry.conflict_old = left_base;
  tmp_entry.conflict_new = right_base;
  SVN_ERR(svn_wc__loggy_entry_modify(
            log_accum,
            merge_dirpath, merge_target,
            &tmp_entry,
            SVN_WC__ENTRY_MODIFY_CONFLICT_OLD
              | SVN_WC__ENTRY_MODIFY_CONFLICT_NEW
              | SVN_WC__ENTRY_MODIFY_CONFLICT_WRK,
            pool, pool));

  *merge_outcome = svn_wc_merge_conflict; /* a conflict happened */

  return SVN_NO_ERROR;
}

/* XXX Insane amount of parameters... */
svn_error_t *
svn_wc__merge_internal(svn_stringbuf_t **log_accum,
                       enum svn_wc_merge_outcome_t *merge_outcome,
                       svn_wc__db_t *db,
                       const char *left,
                       const svn_wc_conflict_version_t *left_version,
                       const char *right,
                       const svn_wc_conflict_version_t *right_version,
                       const char *merge_target,
                       const char *copyfrom_text,
                       const char *left_label,
                       const char *right_label,
                       const char *target_label,
                       svn_boolean_t dry_run,
                       const char *diff3_cmd,
                       const apr_array_header_t *merge_options,
                       const apr_array_header_t *prop_diff,
                       svn_wc_conflict_resolver_func_t conflict_func,
                       void *conflict_baton,
                       svn_cancel_func_t cancel_func,
                       void *cancel_baton,
                       apr_pool_t *pool)
{
  const char *merge_abspath;
  const char *working_text_abspath;
  const char *detranslated_target_abspath, *working_text;
  svn_boolean_t is_binary = FALSE;
  const svn_wc_entry_t *entry;
  const svn_prop_t *mimeprop;
  const char *left_abspath;
  svn_wc_adm_access_t *adm_access;

  SVN_ERR(svn_dirent_get_absolute(&merge_abspath, merge_target, pool));
  SVN_ERR(svn_dirent_get_absolute(&left_abspath, left, pool));

  adm_access = 
      svn_wc__adm_retrieve_internal2(db,
                                     svn_dirent_dirname(merge_abspath, pool),
                                     pool);

  SVN_ERR_ASSERT(adm_access != NULL);

  /* Sanity check:  the merge target must be under revision control,
   * unless the merge target is a copyfrom text, which lives in a
   * temporary file and does not exist in ACTUAL yet. */
  SVN_ERR(svn_wc__get_entry(&entry, db, merge_abspath, TRUE,
                            svn_node_unknown, FALSE, pool, pool));
  if (! entry && ! copyfrom_text)
    {
      *merge_outcome = svn_wc_merge_no_merge;
      return SVN_NO_ERROR;
    }

  /* Decide if the merge target is a text or binary file. */
  if ((mimeprop = get_prop(prop_diff, SVN_PROP_MIME_TYPE))
      && mimeprop->value)
    is_binary = svn_mime_type_is_binary(mimeprop->value->data);
  else if (! copyfrom_text)
    SVN_ERR(svn_wc__marked_as_binary(&is_binary, merge_abspath, db, pool));

  working_text = copyfrom_text ? copyfrom_text : merge_target;
  SVN_ERR(svn_dirent_get_absolute(&working_text_abspath, working_text, pool));
  SVN_ERR(detranslate_wc_file(&detranslated_target_abspath, db, merge_abspath,
                              (! is_binary) && diff3_cmd != NULL,
                              prop_diff, working_text_abspath, pool, pool));

  /* We cannot depend on the left file to contain the same eols as the
     right file. If the merge target has mods, this will mark the entire
     file as conflicted, so we need to compensate. */
  SVN_ERR(maybe_update_target_eols(&left_abspath, db, left_abspath, prop_diff,
                                   pool, pool));

  if (is_binary)
    {
      if (dry_run)
        /* in dry-run mode, binary files always conflict */
        *merge_outcome = svn_wc_merge_conflict;
      else
        SVN_ERR(merge_binary_file(log_accum,
                                  merge_outcome,
                                  db,
                                  left_abspath,
                                  right,
                                  merge_target,
                                  left_label,
                                  right_label,
                                  target_label,
                                  left_version,
                                  right_version,
                                  detranslated_target_abspath,
                                  mimeprop,
                                  conflict_func,
                                  conflict_baton,
                                  cancel_func,
                                  cancel_baton,
                                  pool));
    }
  else
    SVN_ERR(merge_text_file(log_accum,
                            merge_outcome,
                            db,
                            left_abspath,
                            right,
                            merge_target,
                            adm_access,
                            left_label,
                            right_label,
                            target_label,
                            dry_run,
                            diff3_cmd,
                            merge_options,
                            left_version,
                            right_version,
                            copyfrom_text,
                            detranslated_target_abspath,
                            mimeprop,
                            conflict_func,
                            conflict_baton,
                            cancel_func,
                            cancel_baton,
                            pool));

  /* Merging is complete.  Regardless of text or binariness, we might
     need to tweak the executable bit on the new working file, and
     possibly make it read-only. */
  if (! dry_run)
    {
      SVN_ERR(svn_wc__loggy_maybe_set_executable(log_accum,
                                      svn_wc__adm_access_abspath(adm_access),
                                      merge_target, pool, pool));
      SVN_ERR(svn_wc__loggy_maybe_set_readonly(log_accum,
                                      svn_wc__adm_access_abspath(adm_access),
                                      merge_target, pool, pool));
    }

  return SVN_NO_ERROR;
}



svn_error_t *
svn_wc_merge4(enum svn_wc_merge_outcome_t *merge_outcome,
              svn_wc_context_t *wc_ctx,
              const char *left_abspath,
              const char *right_abspath,
              const char *target_abspath,
              const char *left_label,
              const char *right_label,
              const char *target_label,
              const svn_wc_conflict_version_t *left_version,
              const svn_wc_conflict_version_t *right_version,
              svn_boolean_t dry_run,
              const char *diff3_cmd,
              const apr_array_header_t *merge_options,
              const apr_array_header_t *prop_diff,
              svn_wc_conflict_resolver_func_t conflict_func,
              void *conflict_baton,
              svn_cancel_func_t cancel_func,
              void *cancel_baton,
              apr_pool_t *scratch_pool)
{
  const char *left, *right, *merge_target;
  svn_stringbuf_t *log_accum = svn_stringbuf_create("", scratch_pool);
  svn_wc_adm_access_t *adm_access;
  const char *dirname;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(left_abspath));
  SVN_ERR_ASSERT(svn_dirent_is_absolute(right_abspath));
  SVN_ERR_ASSERT(svn_dirent_is_absolute(target_abspath));

  SVN_ERR(svn_wc__temp_get_relpath(&left, wc_ctx->db, left_abspath,
                                   scratch_pool, scratch_pool));
  SVN_ERR(svn_wc__temp_get_relpath(&right, wc_ctx->db, right_abspath,
                                   scratch_pool, scratch_pool));
  SVN_ERR(svn_wc__temp_get_relpath(&merge_target, wc_ctx->db, target_abspath,
                                   scratch_pool, scratch_pool));

  dirname = svn_dirent_dirname(target_abspath, scratch_pool);
  adm_access = svn_wc__adm_retrieve_internal2(wc_ctx->db, dirname,
                                              scratch_pool);
  SVN_ERR_ASSERT(adm_access != NULL);

  SVN_ERR(svn_wc__merge_internal(&log_accum, merge_outcome,
                                 wc_ctx->db,
                                 left, left_version,
                                 right, right_version,
                                 merge_target,
                                 NULL,
                                 left_label, right_label, target_label,
                                 dry_run,
                                 diff3_cmd,
                                 merge_options,
                                 prop_diff,
                                 conflict_func, conflict_baton,
                                 cancel_func, cancel_baton,
                                 scratch_pool));

  /* Write our accumulation of log entries into a log file */
  SVN_ERR(svn_wc__write_log(adm_access, 0, log_accum, scratch_pool));

  return svn_wc__run_log(adm_access, scratch_pool);
}


/* Constructor for the result-structure returned by conflict callbacks. */
svn_wc_conflict_result_t *
svn_wc_create_conflict_result(svn_wc_conflict_choice_t choice,
                              const char *merged_file,
                              apr_pool_t *pool)
{
  svn_wc_conflict_result_t *result = apr_pcalloc(pool, sizeof(*result));
  result->choice = choice;
  result->merged_file = merged_file;
  result->save_merged = FALSE;

  /* If we add more fields to svn_wc_conflict_result_t, add them here. */

  return result;
}
