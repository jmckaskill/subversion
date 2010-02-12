/*
 * notify.c:  feedback handlers for cmdline client.
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
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

/* ==================================================================== */



/*** Includes. ***/

#define APR_WANT_STDIO
#define APR_WANT_STRFUNC
#include <apr_want.h>

#include "svn_cmdline.h"
#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "cl.h"

#include "svn_private_config.h"


/* Baton for notify and friends. */
struct notify_baton
{
  svn_boolean_t received_some_change;
  svn_boolean_t is_checkout;
  svn_boolean_t is_export;
  svn_boolean_t suppress_final_line;
  svn_boolean_t sent_first_txdelta;
  svn_boolean_t in_external;
  svn_boolean_t had_print_error; /* Used to not keep printing error messages
                                    when we've already had one print error. */

  /* Conflict stats for update and merge. */
  unsigned int text_conflicts;
  unsigned int prop_conflicts;
  unsigned int tree_conflicts;
  unsigned int skipped_paths;

  /* Conflict stats for update and merge (for externals). */
  unsigned int ext_text_conflicts;
  unsigned int ext_prop_conflicts;
  unsigned int ext_tree_conflicts;
  unsigned int ext_skipped_paths;

  /* The cwd, for use in decomposing absolute paths. */
  const char *path_prefix;
};


svn_error_t *
svn_cl__print_conflict_stats(void *notify_baton, apr_pool_t *pool)
{
  struct notify_baton *nb = notify_baton;
  const char *header;
  unsigned int text_conflicts;
  unsigned int prop_conflicts;
  unsigned int tree_conflicts;
  unsigned int skipped_paths;

  if (nb->in_external)
    {
      header = _("Summary of conflicts in external item:\n");
      text_conflicts = nb->ext_text_conflicts;
      prop_conflicts = nb->ext_prop_conflicts;
      tree_conflicts = nb->ext_tree_conflicts;
      skipped_paths = nb->ext_skipped_paths;
    }
  else
    {
      header = _("Summary of conflicts:\n");
      text_conflicts = nb->text_conflicts;
      prop_conflicts = nb->prop_conflicts;
      tree_conflicts = nb->tree_conflicts;
      skipped_paths = nb->skipped_paths;
    }

  if (text_conflicts > 0 || prop_conflicts > 0
    || tree_conflicts > 0 || skipped_paths > 0)
      SVN_ERR(svn_cmdline_printf(pool, "%s", header));

  if (text_conflicts > 0)
    SVN_ERR(svn_cmdline_printf
      (pool, _("  Text conflicts: %u\n"), text_conflicts));

  if (prop_conflicts > 0)
    SVN_ERR(svn_cmdline_printf
      (pool, _("  Property conflicts: %u\n"), prop_conflicts));

  if (tree_conflicts > 0)
    SVN_ERR(svn_cmdline_printf
      (pool, _("  Tree conflicts: %u\n"), tree_conflicts));

  if (skipped_paths > 0)
    SVN_ERR(svn_cmdline_printf
      (pool, _("  Skipped paths: %u\n"), skipped_paths));

  return SVN_NO_ERROR;
}

/* This implements `svn_wc_notify_func2_t'.
 * NOTE: This function can't fail, so we just ignore any print errors. */
static void
notify(void *baton, const svn_wc_notify_t *n, apr_pool_t *pool)
{
  struct notify_baton *nb = baton;
  char statchar_buf[5] = "    ";
  const char *path_local;
  svn_error_t *err;

  if (n->url)
    path_local = n->url;
  else
    {
      if (n->path_prefix)
        path_local = svn_dirent_skip_ancestor(n->path_prefix, n->path);
      else if (nb->path_prefix)
        path_local = svn_dirent_skip_ancestor(nb->path_prefix, n->path);
      else
        path_local = n->path;

      path_local = svn_dirent_local_style(path_local, pool);
    }

  switch (n->action)
    {
    case svn_wc_notify_skip:
      nb->in_external ? nb->ext_skipped_paths++
                      : nb->skipped_paths++;
      if (n->content_state == svn_wc_notify_state_missing)
        {
          if ((err = svn_cmdline_printf
               (pool, _("Skipped missing target: '%s'\n"),
                path_local)))
            goto print_error;
        }
      else if (n->content_state == svn_wc_notify_state_source_missing)
        {
          if ((err = svn_cmdline_printf
               (pool, _("Skipped target: '%s' -- copy-source is missing\n"),
                path_local)))
            goto print_error;
        }
      else
        {
          if ((err = svn_cmdline_printf
               (pool, _("Skipped '%s'\n"), path_local)))
            goto print_error;
        }
      break;

    case svn_wc_notify_update_add_deleted:
    case svn_wc_notify_update_update_deleted:
      /* ### Before 1.7.0 these notifications where suppressed in the wc
         ### library.. how should we notify these?

         ### Fall through in deleted notification. */

    case svn_wc_notify_update_delete:
    case svn_wc_notify_update_external_removed:
      nb->received_some_change = TRUE;
      if ((err = svn_cmdline_printf(pool, "D    %s\n", path_local)))
        goto print_error;
      break;

    case svn_wc_notify_update_replace:
      nb->received_some_change = TRUE;
      if ((err = svn_cmdline_printf(pool, "R    %s\n", path_local)))
        goto print_error;
      break;

    case svn_wc_notify_update_add:
      nb->received_some_change = TRUE;
      if (n->content_state == svn_wc_notify_state_conflicted)
        {
          nb->in_external ? nb->ext_text_conflicts++
                          : nb->text_conflicts++;
          if ((err = svn_cmdline_printf(pool, "C    %s\n", path_local)))
            goto print_error;
        }
      else
        {
          if ((err = svn_cmdline_printf(pool, "A    %s\n", path_local)))
            goto print_error;
        }
      break;

    case svn_wc_notify_exists:
      nb->received_some_change = TRUE;
      if (n->content_state == svn_wc_notify_state_conflicted)
        {
          nb->in_external ? nb->ext_text_conflicts++
                          : nb->text_conflicts++;
          statchar_buf[0] = 'C';
        }
      else
        statchar_buf[0] = 'E';

      if (n->prop_state == svn_wc_notify_state_conflicted)
        {
          nb->in_external ? nb->ext_prop_conflicts++
                          : nb->prop_conflicts++;
          statchar_buf[1] = 'C';
        }
      else if (n->prop_state == svn_wc_notify_state_merged)
        statchar_buf[1] = 'G';

      if ((err = svn_cmdline_printf(pool, "%s %s\n", statchar_buf, path_local)))
        goto print_error;
      break;

    case svn_wc_notify_restore:
      if ((err = svn_cmdline_printf(pool, _("Restored '%s'\n"),
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_revert:
      if ((err = svn_cmdline_printf(pool, _("Reverted '%s'\n"),
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_failed_revert:
      if (( err = svn_cmdline_printf(pool, _("Failed to revert '%s' -- "
                                             "try updating instead.\n"),
                                     path_local)))
        goto print_error;
      break;

    case svn_wc_notify_resolved:
      if ((err = svn_cmdline_printf(pool,
                                    _("Resolved conflicted state of '%s'\n"),
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_add:
      /* We *should* only get the MIME_TYPE if PATH is a file.  If we
         do get it, and the mime-type is not textual, note that this
         is a binary addition. */
      if (n->mime_type && (svn_mime_type_is_binary(n->mime_type)))
        {
          if ((err = svn_cmdline_printf(pool, "A  (bin)  %s\n",
                                        path_local)))
            goto print_error;
        }
      else
        {
          if ((err = svn_cmdline_printf(pool, "A         %s\n",
                                        path_local)))
            goto print_error;
        }
      break;

    case svn_wc_notify_delete:
      nb->received_some_change = TRUE;
      if ((err = svn_cmdline_printf(pool, "D         %s\n",
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_patch:
      {
        nb->received_some_change = TRUE;
        if (n->content_state == svn_wc_notify_state_conflicted)
          {
            nb->in_external ? nb->ext_text_conflicts++
                            : nb->text_conflicts++;
            statchar_buf[0] = 'C';
          }
        else if (n->kind == svn_node_file)
          {
            if (n->content_state == svn_wc_notify_state_merged)
              statchar_buf[0] = 'G';
            else if (n->content_state == svn_wc_notify_state_changed)
              statchar_buf[0] = 'U';
          }

        if (statchar_buf[0] != ' ')
          {
            if ((err = svn_cmdline_printf(pool, "%s      %s\n",
                                          statchar_buf, path_local)))
              goto print_error;
          }
      }
      break;

    case svn_wc_notify_patch_applied_hunk:
      nb->received_some_change = TRUE;
      if (n->hunk_original_start != n->hunk_matched_line)
        {
          apr_uint64_t off;
          const char *s;
          const char *minus;

          if (n->hunk_matched_line > n->hunk_original_start)
            {
              off = n->hunk_matched_line - n->hunk_original_start;
              minus = "";
            }
          else
            {
              off = n->hunk_original_start - n->hunk_matched_line;
              minus = "-";
            }

          /* ### APR_INT64_T_FMT isn't translator-friendly */
          if (n->hunk_fuzz)
            {
              s = _(">         applied hunk @@ -%lu,%lu +%lu,%lu @@ "
                    "with offset %s");
              if ((err = svn_cmdline_printf(pool,
                                            apr_pstrcat(pool, s,
                                                        "%"APR_UINT64_T_FMT
                                                        " and fuzz %d\n",
                                                        NULL),
                                            n->hunk_original_start,
                                            n->hunk_original_length,
                                            n->hunk_modified_start,
                                            n->hunk_modified_length,
                                            minus, off, n->hunk_fuzz)))
                goto print_error;
            }
          else 
            {
              s = _(">         applied hunk @@ -%lu,%lu +%lu,%lu @@ "
                    "with offset %s");
              if ((err = svn_cmdline_printf(pool,
                                            apr_pstrcat(pool, s,
                                                        "%"APR_UINT64_T_FMT"\n",
                                                        NULL),
                                            n->hunk_original_start,
                                            n->hunk_original_length,
                                            n->hunk_modified_start,
                                            n->hunk_modified_length,
                                            minus, off)))
                goto print_error;
            }
        }
      else if (n->hunk_fuzz)
        {
          if ((err = svn_cmdline_printf(pool, 
                          _(">         applied hunk @@ -%lu,%lu +%lu,%lu @@ "
                                        "with fuzz %d\n"),
                                        n->hunk_original_start,
                                        n->hunk_original_length,
                                        n->hunk_modified_start,
                                        n->hunk_modified_length,
                                        n->hunk_fuzz)))
            goto print_error;

        }
      break;

    case svn_wc_notify_patch_rejected_hunk:
      nb->received_some_change = TRUE;
      if ((err = svn_cmdline_printf(pool,
                                    _(">         rejected hunk "
                                      "@@ -%lu,%lu +%lu,%lu @@\n"),
                                    n->hunk_original_start,
                                    n->hunk_original_length,
                                    n->hunk_modified_start,
                                    n->hunk_modified_length)))
        goto print_error;
      break;

    case svn_wc_notify_update_update:
    case svn_wc_notify_merge_record_info:
      {
        if (n->content_state == svn_wc_notify_state_conflicted)
          {
            nb->in_external ? nb->ext_text_conflicts++
                            : nb->text_conflicts++;
            statchar_buf[0] = 'C';
          }
        else if (n->kind == svn_node_file)
          {
            if (n->content_state == svn_wc_notify_state_merged)
              statchar_buf[0] = 'G';
            else if (n->content_state == svn_wc_notify_state_changed)
              statchar_buf[0] = 'U';
          }

        if (n->prop_state == svn_wc_notify_state_conflicted)
          {
            nb->in_external ? nb->ext_prop_conflicts++
                            : nb->prop_conflicts++;
            statchar_buf[1] = 'C';
          }
        else if (n->prop_state == svn_wc_notify_state_merged)
          statchar_buf[1] = 'G';
        else if (n->prop_state == svn_wc_notify_state_changed)
          statchar_buf[1] = 'U';

        if (n->lock_state == svn_wc_notify_lock_state_unlocked)
          statchar_buf[2] = 'B';

        if (statchar_buf[0] != ' ' || statchar_buf[1] != ' ')
          nb->received_some_change = TRUE;

        if (statchar_buf[0] != ' ' || statchar_buf[1] != ' '
            || statchar_buf[2] != ' ')
          {
            if ((err = svn_cmdline_printf(pool, "%s %s\n",
                                          statchar_buf, path_local)))
              goto print_error;
          }
      }
      break;

    case svn_wc_notify_update_external:
      /* Remember that we're now "inside" an externals definition. */
      nb->in_external = TRUE;

      /* Currently this is used for checkouts and switches too.  If we
         want different output, we'll have to add new actions. */
      if ((err = svn_cmdline_printf(pool,
                                    _("\nFetching external item into '%s'\n"),
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_failed_external:
      /* If we are currently inside the handling of an externals
         definition, then we can simply present n->err as a warning
         and feel confident that after this, we aren't handling that
         externals definition any longer. */
      if (nb->in_external)
        {
          svn_handle_warning2(stderr, n->err, "svn: ");
          nb->in_external = FALSE;
          nb->ext_text_conflicts = nb->ext_prop_conflicts
            = nb->ext_tree_conflicts = nb->ext_skipped_paths = 0;
          if ((err = svn_cmdline_printf(pool, "\n")))
            goto print_error;
        }
      /* Otherwise, we'll just print two warnings.  Why?  Because
         svn_handle_warning2() only shows the single "best message",
         but we have two pretty important ones: that the external at
         '/some/path' didn't pan out, and then the more specific
         reason why (from n->err). */
      else
        {
          svn_error_t *warn_err =
            svn_error_createf(SVN_ERR_BASE, NULL,
                              _("Error handling externals definition for '%s':"),
                              path_local);
          svn_handle_warning2(stderr, warn_err, "svn: ");
          svn_error_clear(warn_err);
          svn_handle_warning2(stderr, n->err, "svn: ");
        }
      break;

    case svn_wc_notify_update_completed:
      {
        if (! nb->suppress_final_line)
          {
            if (SVN_IS_VALID_REVNUM(n->revision))
              {
                if (nb->is_export)
                  {
                    if ((err = svn_cmdline_printf
                         (pool, nb->in_external
                          ? _("Exported external at revision %ld.\n")
                          : _("Exported revision %ld.\n"),
                          n->revision)))
                      goto print_error;
                  }
                else if (nb->is_checkout)
                  {
                    if ((err = svn_cmdline_printf
                         (pool, nb->in_external
                          ? _("Checked out external at revision %ld.\n")
                          : _("Checked out revision %ld.\n"),
                          n->revision)))
                      goto print_error;
                  }
                else
                  {
                    if (nb->received_some_change)
                      {
                        if ((err = svn_cmdline_printf
                             (pool, nb->in_external
                              ? _("Updated external to revision %ld.\n")
                              : _("Updated to revision %ld.\n"),
                              n->revision)))
                          goto print_error;
                      }
                    else
                      {
                        if ((err = svn_cmdline_printf
                             (pool, nb->in_external
                              ? _("External at revision %ld.\n")
                              : _("At revision %ld.\n"),
                              n->revision)))
                          goto print_error;
                      }
                  }
              }
            else  /* no revision */
              {
                if (nb->is_export)
                  {
                    if ((err = svn_cmdline_printf
                         (pool, nb->in_external
                          ? _("External export complete.\n")
                          : _("Export complete.\n"))))
                      goto print_error;
                  }
                else if (nb->is_checkout)
                  {
                    if ((err = svn_cmdline_printf
                         (pool, nb->in_external
                          ? _("External checkout complete.\n")
                          : _("Checkout complete.\n"))))
                      goto print_error;
                  }
                else
                  {
                    if ((err = svn_cmdline_printf
                         (pool, nb->in_external
                          ? _("External update complete.\n")
                          : _("Update complete.\n"))))
                      goto print_error;
                  }
              }
          }
      }

      if (nb->in_external)
        {
          nb->in_external = FALSE;
          if ((err = svn_cmdline_printf(pool, "\n")))
            goto print_error;
        }
      break;

    case svn_wc_notify_status_external:
      if ((err = svn_cmdline_printf
           (pool, _("\nPerforming status on external item at '%s'\n"),
            path_local)))
        goto print_error;
      break;

    case svn_wc_notify_status_completed:
      if (SVN_IS_VALID_REVNUM(n->revision))
        if ((err = svn_cmdline_printf(pool,
                                      _("Status against revision: %6ld\n"),
                                      n->revision)))
          goto print_error;
      break;

    case svn_wc_notify_commit_modified:
      /* xgettext: Align the %s's on this and the following 4 messages */
      if ((err = svn_cmdline_printf(pool,
                                    _("Sending        %s\n"),
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_commit_added:
      if (n->mime_type && svn_mime_type_is_binary(n->mime_type))
        {
          if ((err = svn_cmdline_printf(pool,
                                        _("Adding  (bin)  %s\n"),
                                        path_local)))
          goto print_error;
        }
      else
        {
          if ((err = svn_cmdline_printf(pool,
                                        _("Adding         %s\n"),
                                        path_local)))
            goto print_error;
        }
      break;

    case svn_wc_notify_commit_deleted:
      if ((err = svn_cmdline_printf(pool, _("Deleting       %s\n"),
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_commit_replaced:
      if ((err = svn_cmdline_printf(pool,
                                    _("Replacing      %s\n"),
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_commit_postfix_txdelta:
      if (! nb->sent_first_txdelta)
        {
          nb->sent_first_txdelta = TRUE;
          if ((err = svn_cmdline_printf(pool,
                                        _("Transmitting file data "))))
            goto print_error;
        }

      if ((err = svn_cmdline_printf(pool, ".")))
        goto print_error;
      break;

    case svn_wc_notify_locked:
      if ((err = svn_cmdline_printf(pool, _("'%s' locked by user '%s'.\n"),
                                    path_local, n->lock->owner)))
        goto print_error;
      break;

    case svn_wc_notify_unlocked:
      if ((err = svn_cmdline_printf(pool, _("'%s' unlocked.\n"),
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_failed_lock:
    case svn_wc_notify_failed_unlock:
      svn_handle_warning2(stderr, n->err, "svn: ");
      break;

    case svn_wc_notify_changelist_set:
      if ((err = svn_cmdline_printf(pool, _("Path '%s' is now a member of "
                                            "changelist '%s'.\n"),
                                    path_local, n->changelist_name)))
        goto print_error;
      break;

    case svn_wc_notify_changelist_clear:
      if ((err = svn_cmdline_printf(pool,
                                    _("Path '%s' is no longer a member of "
                                      "a changelist.\n"),
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_changelist_moved:
      svn_handle_warning2(stderr, n->err, "svn: ");
      break;

    case svn_wc_notify_merge_begin:
      if (n->merge_range == NULL)
        err = svn_cmdline_printf(pool,
                                 _("--- Merging differences between "
                                   "repository URLs into '%s':\n"),
                                 path_local);
      else if (n->merge_range->start == n->merge_range->end - 1
          || n->merge_range->start == n->merge_range->end)
        err = svn_cmdline_printf(pool, _("--- Merging r%ld into '%s':\n"),
                                 n->merge_range->end, path_local);
      else if (n->merge_range->start - 1 == n->merge_range->end)
        err = svn_cmdline_printf(pool,
                                 _("--- Reverse-merging r%ld into '%s':\n"),
                                 n->merge_range->start, path_local);
      else if (n->merge_range->start < n->merge_range->end)
        err = svn_cmdline_printf(pool,
                                 _("--- Merging r%ld through r%ld into "
                                   "'%s':\n"),
                                 n->merge_range->start + 1,
                                 n->merge_range->end, path_local);
      else /* n->merge_range->start > n->merge_range->end - 1 */
        err = svn_cmdline_printf(pool,
                                 _("--- Reverse-merging r%ld through r%ld "
                                   "into '%s':\n"),
                                 n->merge_range->start,
                                 n->merge_range->end + 1, path_local);
      if (err)
        goto print_error;
      break;

    case svn_wc_notify_merge_record_info_begin:
      if (!n->merge_range)
        {
          err = svn_cmdline_printf(pool,
                                   _("--- Recording mergeinfo for merge "
                                     "between repository URLs into '%s':\n"),
                                   path_local);
        }
      else
        {
          if (n->merge_range->start == n->merge_range->end - 1
              || n->merge_range->start == n->merge_range->end)
            err = svn_cmdline_printf(
              pool,
              _("--- Recording mergeinfo for merge of r%ld into '%s':\n"),
              n->merge_range->end, path_local);
          else if (n->merge_range->start - 1 == n->merge_range->end)
            err = svn_cmdline_printf(
              pool,
              _("--- Recording mergeinfo for reverse merge of r%ld into '%s':\n"),
              n->merge_range->start, path_local);
           else if (n->merge_range->start < n->merge_range->end)
             err = svn_cmdline_printf(
               pool,
               _("--- Recording mergeinfo for merge of r%ld through r%ld into '%s':\n"),
               n->merge_range->start + 1, n->merge_range->end, path_local);
           else /* n->merge_range->start > n->merge_range->end - 1 */
             err = svn_cmdline_printf(
               pool,
               _("--- Recording mergeinfo for reverse merge of r%ld through r%ld into '%s':\n"),
               n->merge_range->start, n->merge_range->end + 1, path_local);
 
      if (err)
        goto print_error;
        }
      break;

    case svn_wc_notify_merge_elide_info:
      if ((err = svn_cmdline_printf(pool,
                                    _("--- Eliding mergeinfo from '%s':\n"),
                                    path_local)))
        goto print_error;
      break;

    case svn_wc_notify_foreign_merge_begin:
      if (n->merge_range == NULL)
        err = svn_cmdline_printf(pool,
                                 _("--- Merging differences between "
                                   "foreign repository URLs into '%s':\n"),
                                 path_local);
      else if (n->merge_range->start == n->merge_range->end - 1
          || n->merge_range->start == n->merge_range->end)
        err = svn_cmdline_printf(pool,
                                 _("--- Merging (from foreign repository) "
                                   "r%ld into '%s':\n"),
                                 n->merge_range->end, path_local);
      else if (n->merge_range->start - 1 == n->merge_range->end)
        err = svn_cmdline_printf(pool,
                                 _("--- Reverse-merging (from foreign "
                                   "repository) r%ld into '%s':\n"),
                                 n->merge_range->start, path_local);
      else if (n->merge_range->start < n->merge_range->end)
        err = svn_cmdline_printf(pool,
                                 _("--- Merging (from foreign repository) "
                                   "r%ld through r%ld into '%s':\n"),
                                 n->merge_range->start + 1,
                                 n->merge_range->end, path_local);
      else /* n->merge_range->start > n->merge_range->end - 1 */
        err = svn_cmdline_printf(pool,
                                 _("--- Reverse-merging (from foreign "
                                   "repository) r%ld through r%ld into "
                                   "'%s':\n"),
                                 n->merge_range->start,
                                 n->merge_range->end + 1, path_local);
      if (err)
        goto print_error;
      break;

    case svn_wc_notify_tree_conflict:
      nb->in_external ? nb->ext_tree_conflicts++
                      : nb->tree_conflicts++;
      if ((err = svn_cmdline_printf(pool, "   C %s\n", path_local)))
        goto print_error;
      break;

    case svn_wc_notify_property_modified:
    case svn_wc_notify_property_added:
        err = svn_cmdline_printf(pool,
                                 _("property '%s' set on '%s'\n"),
                                 n->prop_name, path_local);
        if (err)
          goto print_error;
      break;

    case svn_wc_notify_property_deleted:
        err = svn_cmdline_printf(pool,
                                 _("property '%s' deleted from '%s'.\n"),
                                 n->prop_name, path_local);
        if (err)
          goto print_error;
      break;

    case svn_wc_notify_revprop_set:
        err = svn_cmdline_printf(pool,
                          _("property '%s' set on repository revision %ld\n"),
                          n->prop_name, n->revision);
        if (err)
          goto print_error;
      break;

    case svn_wc_notify_revprop_deleted:
        err = svn_cmdline_printf(pool,
                     _("property '%s' deleted from repository revision %ld\n"),
                     n->prop_name, n->revision);
        if (err)
          goto print_error;
      break;

    case svn_wc_notify_upgraded_path:
        err = svn_cmdline_printf(pool, _("Upgraded '%s'.\n"), path_local);
        if (err)
          goto print_error;
      break;

    default:
      break;
    }

  if ((err = svn_cmdline_fflush(stdout)))
    goto print_error;

  return;

 print_error:
  /* If we had no errors before, print this error to stderr. Else, don't print
     anything.  The user already knows there were some output errors,
     so there is no point in flooding her with an error per notification. */
  if (!nb->had_print_error)
    {
      nb->had_print_error = TRUE;
      svn_handle_error2(err, stderr, FALSE, "svn: ");
    }
  svn_error_clear(err);
}


svn_error_t *
svn_cl__get_notifier(svn_wc_notify_func2_t *notify_func_p,
                     void **notify_baton_p,
                     svn_boolean_t is_checkout,
                     svn_boolean_t is_export,
                     svn_boolean_t suppress_final_line,
                     apr_pool_t *pool)
{
  struct notify_baton *nb = apr_palloc(pool, sizeof(*nb));

  nb->received_some_change = FALSE;
  nb->sent_first_txdelta = FALSE;
  nb->is_checkout = is_checkout;
  nb->is_export = is_export;
  nb->suppress_final_line = suppress_final_line;
  nb->in_external = FALSE;
  nb->had_print_error = FALSE;
  nb->text_conflicts = 0;
  nb->prop_conflicts = 0;
  nb->tree_conflicts = 0;
  nb->skipped_paths = 0;
  nb->ext_text_conflicts = 0;
  nb->ext_prop_conflicts = 0;
  nb->ext_tree_conflicts = 0;
  nb->ext_skipped_paths = 0;
  SVN_ERR(svn_dirent_get_absolute(&nb->path_prefix, "", pool));

  *notify_func_p = notify;
  *notify_baton_p = nb;
  return SVN_NO_ERROR;
}
