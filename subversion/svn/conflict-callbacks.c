/*
 * conflict-callbacks.c: conflict resolution callbacks specific to the
 * commandline client.
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

#include <apr_xlate.h>  /* for APR_LOCALE_CHARSET */

#define APR_WANT_STDIO
#define APR_WANT_STRFUNC
#include <apr_want.h>

#include "svn_cmdline.h"
#include "svn_client.h"
#include "svn_dirent_uri.h"
#include "svn_types.h"
#include "svn_pools.h"

#include "cl.h"
#include "tree-conflicts.h"

#include "svn_private_config.h"




svn_error_t *
svn_cl__conflict_baton_make(svn_cl__conflict_baton_t **b,
                            svn_cl__accept_t accept_which,
                            apr_hash_t *config,
                            const char *editor_cmd,
                            svn_cmdline_prompt_baton_t *pb,
                            apr_pool_t *pool)
{
  *b = apr_palloc(pool, sizeof(**b));
  (*b)->accept_which = accept_which;
  (*b)->config = config;
  (*b)->editor_cmd = editor_cmd;
  (*b)->external_failed = FALSE;
  (*b)->pb = pb;
  SVN_ERR(svn_dirent_get_absolute(&(*b)->path_prefix, "", pool));
  (*b)->show_moves = FALSE;
  return SVN_NO_ERROR;
}

svn_cl__accept_t
svn_cl__accept_from_word(const char *word)
{
  /* Shorthand options are consistent with  svn_cl__conflict_handler(). */
  if (strcmp(word, SVN_CL__ACCEPT_POSTPONE) == 0
      || strcmp(word, "p") == 0 || strcmp(word, ":-P") == 0)
    return svn_cl__accept_postpone;
  if (strcmp(word, SVN_CL__ACCEPT_BASE) == 0)
    /* ### shorthand? */
    return svn_cl__accept_base;
  if (strcmp(word, SVN_CL__ACCEPT_WORKING) == 0)
    /* ### shorthand? */
    return svn_cl__accept_working;
  if (strcmp(word, SVN_CL__ACCEPT_MINE_CONFLICT) == 0
      || strcmp(word, "mc") == 0 || strcmp(word, "X-)") == 0)
    return svn_cl__accept_mine_conflict;
  if (strcmp(word, SVN_CL__ACCEPT_THEIRS_CONFLICT) == 0
      || strcmp(word, "tc") == 0 || strcmp(word, "X-(") == 0)
    return svn_cl__accept_theirs_conflict;
  if (strcmp(word, SVN_CL__ACCEPT_MINE_FULL) == 0
      || strcmp(word, "mf") == 0 || strcmp(word, ":-)") == 0)
    return svn_cl__accept_mine_full;
  if (strcmp(word, SVN_CL__ACCEPT_THEIRS_FULL) == 0
      || strcmp(word, "tf") == 0 || strcmp(word, ":-(") == 0)
    return svn_cl__accept_theirs_full;
  if (strcmp(word, SVN_CL__ACCEPT_EDIT) == 0
      || strcmp(word, "e") == 0 || strcmp(word, ":-E") == 0)
    return svn_cl__accept_edit;
  if (strcmp(word, SVN_CL__ACCEPT_LAUNCH) == 0
      || strcmp(word, "l") == 0 || strcmp(word, ":-l") == 0)
    return svn_cl__accept_launch;
  /* word is an invalid action. */
  return svn_cl__accept_invalid;
}


/* Print on stdout a diff between the 'base' and 'merged' files, if both of
 * those are available, else between 'their' and 'my' files, of DESC. */
static svn_error_t *
show_diff(const svn_wc_conflict_description2_t *desc,
          apr_pool_t *pool)
{
  const char *path1, *path2;
  svn_diff_t *diff;
  svn_stream_t *output;
  svn_diff_file_options_t *options;

  if (desc->merged_file && desc->base_abspath)
    {
      /* Show the conflict markers to the user */
      path1 = desc->base_abspath;
      path2 = desc->merged_file;
    }
  else
    {
      /* There's no base file, but we can show the
         difference between mine and theirs. */
      path1 = desc->their_abspath;
      path2 = desc->my_abspath;
    }

  options = svn_diff_file_options_create(pool);
  options->ignore_eol_style = TRUE;
  SVN_ERR(svn_stream_for_stdout(&output, pool));
  SVN_ERR(svn_diff_file_diff_2(&diff, path1, path2,
                               options, pool));
  return svn_diff_file_output_unified3(output, diff,
                                       path1, path2,
                                       NULL, NULL,
                                       APR_LOCALE_CHARSET,
                                       NULL, FALSE,
                                       pool);
}


/* Print on stdout just the conflict hunks of a diff among the 'base', 'their'
 * and 'my' files of DESC. */
static svn_error_t *
show_conflicts(const svn_wc_conflict_description2_t *desc,
               apr_pool_t *pool)
{
  svn_diff_t *diff;
  svn_stream_t *output;
  svn_diff_file_options_t *options;

  options = svn_diff_file_options_create(pool);
  options->ignore_eol_style = TRUE;
  SVN_ERR(svn_stream_for_stdout(&output, pool));
  SVN_ERR(svn_diff_file_diff3_2(&diff,
                                desc->base_abspath,
                                desc->my_abspath,
                                desc->their_abspath,
                                options, pool));
  /* ### Consider putting the markers/labels from
     ### svn_wc__merge_internal in the conflict description. */
  return svn_diff_file_output_merge2(output, diff,
                                     desc->base_abspath,
                                     desc->my_abspath,
                                     desc->their_abspath,
                                     _("||||||| ORIGINAL"),
                                     _("<<<<<<< MINE (select with 'mc')"),
                                     _(">>>>>>> THEIRS (select with 'tc')"),
                                     "=======",
                                     svn_diff_conflict_display_only_conflicts,
                                     pool);
}


/* Run an external editor, passing it the 'merged' file in DESC, or, if the
 * 'merged' file is null, return an error. The tool to use is determined by
 * B->editor_cmd, B->config and environment variables; see
 * svn_cl__edit_file_externally() for details.
 *
 * If the tool runs, set *PERFORMED_EDIT to true; if a tool is not
 * configured or cannot run, do not touch *PERFORMED_EDIT, report the error
 * on stderr, and return SVN_NO_ERROR; if any other error is encountered,
 * return that error. */
static svn_error_t *
open_editor(svn_boolean_t *performed_edit,
            const svn_wc_conflict_description2_t *desc,
            svn_cl__conflict_baton_t *b,
            apr_pool_t *pool)
{
  svn_error_t *err;

  if (desc->merged_file)
    {
      err = svn_cl__edit_file_externally(desc->merged_file, b->editor_cmd,
                                         b->config, pool);
      if (err && (err->apr_err == SVN_ERR_CL_NO_EXTERNAL_EDITOR))
        {
          SVN_ERR(svn_cmdline_fprintf(stderr, pool, "%s\n",
                                      err->message ? err->message :
                                      _("No editor found.")));
          svn_error_clear(err);
        }
      else if (err && (err->apr_err == SVN_ERR_EXTERNAL_PROGRAM))
        {
          SVN_ERR(svn_cmdline_fprintf(stderr, pool, "%s\n",
                                      err->message ? err->message :
                                      _("Error running editor.")));
          svn_error_clear(err);
        }
      else if (err)
        return svn_error_trace(err);
      else
        *performed_edit = TRUE;
    }
  else
    SVN_ERR(svn_cmdline_fprintf(stderr, pool,
                                _("Invalid option; there's no "
                                  "merged version to edit.\n\n")));

  return SVN_NO_ERROR;
}


/* Run an external merge tool, passing it the 'base', 'their', 'my' and
 * 'merged' files in DESC. The tool to use is determined by B->config and
 * environment variables; see svn_cl__merge_file_externally() for details.
 *
 * If the tool runs, set *PERFORMED_EDIT to true; if a tool is not
 * configured or cannot run, do not touch *PERFORMED_EDIT, report the error
 * on stderr, and return SVN_NO_ERROR; if any other error is encountered,
 * return that error.  */
static svn_error_t *
launch_resolver(svn_boolean_t *performed_edit,
                const svn_wc_conflict_description2_t *desc,
                svn_cl__conflict_baton_t *b,
                apr_pool_t *pool)
{
  svn_error_t *err;

  err = svn_cl__merge_file_externally(desc->base_abspath, desc->their_abspath,
                                      desc->my_abspath, desc->merged_file,
                                      desc->local_abspath, b->config, NULL, pool);
  if (err && err->apr_err == SVN_ERR_CL_NO_EXTERNAL_MERGE_TOOL)
    {
      SVN_ERR(svn_cmdline_fprintf(stderr, pool, "%s\n",
                                  err->message ? err->message :
                                  _("No merge tool found.\n")));
      svn_error_clear(err);
    }
  else if (err && err->apr_err == SVN_ERR_EXTERNAL_PROGRAM)
    {
      SVN_ERR(svn_cmdline_fprintf(stderr, pool, "%s\n",
                                  err->message ? err->message :
                             _("Error running merge tool.")));
      svn_error_clear(err);
    }
  else if (err)
    return svn_error_trace(err);
  else if (performed_edit)
    *performed_edit = TRUE;

  return SVN_NO_ERROR;
}


static const char *
format_move_chain_for_display(svn_wc_repos_move_info_t *first_move,
                              const char *indent,
                              apr_pool_t *result_pool)
{
  const char *s;
  svn_wc_repos_move_info_t *last_move;

  last_move = first_move;
  while (last_move->next)
    last_move = last_move->next;

  if (last_move != first_move)
    {
      svn_wc_repos_move_info_t *this_move;

      s = apr_psprintf(result_pool,
                        _("Combined move:\n%s  %s@%ld -> %s\n"
                       "%sIndividual moves:\n"),
                       indent, first_move->moved_from_repos_relpath,
                       first_move->copyfrom_rev,
                       last_move->moved_to_repos_relpath, indent);
      
      this_move = first_move;
      do
        {
          s = apr_pstrcat(result_pool, s,
                          apr_psprintf(
                            result_pool, _("%s  [r%ld] %s@%ld -> %s\n"),
                            indent,
                            this_move->revision,
                            this_move->moved_from_repos_relpath,
                            this_move->copyfrom_rev,
                            this_move->moved_to_repos_relpath),
                          (char *)NULL);
          this_move = this_move->next;
        }
      while (this_move);
    }
  else
    s = apr_psprintf(result_pool, _("  [r%ld] %s@%ld -> %s\n"),
                     first_move->revision,
                     first_move->moved_from_repos_relpath,
                     first_move->copyfrom_rev,
                     first_move->moved_to_repos_relpath);

  return s;
}

static svn_error_t *
pick_move(svn_wc_repos_move_info_t **move,
          apr_array_header_t *suggested_moves,
          svn_cmdline_prompt_baton_t *pb,
          apr_pool_t *scratch_pool)
{
  const char *prompt;
  svn_wc_repos_move_info_t *this_move;
  int i = 0;
  int m;
  apr_pool_t *iterpool;

  if (suggested_moves->nelts == 1)
    {
      this_move = APR_ARRAY_IDX(suggested_moves, 0,
                                svn_wc_repos_move_info_t *);
      SVN_ERR(svn_cmdline_fprintf(stderr, scratch_pool, "%s\n",
                                  format_move_chain_for_display(this_move, "",
                                                                scratch_pool)));
      *move = this_move;
      return SVN_NO_ERROR;
    }

  prompt = _("Multiple incoming move candidates found:\n");
  for (i = 0; i < suggested_moves->nelts; i++)
    {
      this_move = APR_ARRAY_IDX(suggested_moves, i,
                                 svn_wc_repos_move_info_t *);
      prompt = apr_pstrcat(scratch_pool, prompt,
                  apr_psprintf(scratch_pool, _("  (%i) %s"), i,
                               format_move_chain_for_display(
                                 this_move, "      ", scratch_pool)),
                  (char *)NULL);
    }

  prompt = apr_pstrcat(scratch_pool, prompt,
                       _("Enter number to choose incoming move or hit enter "
                         "for previous menu: "),
                       (char *)NULL);

  iterpool = svn_pool_create(scratch_pool);
  *move = NULL;
  while (*move == NULL)
    {
      const char *answer;
      svn_error_t *err;

      svn_pool_clear(iterpool);

      SVN_ERR(svn_cmdline_prompt_user2(&answer, prompt, pb, iterpool));

      if (answer[0] == '\0')
        break;

      err = svn_cstring_atoi(&m, answer);
      if (err)
        {
          if (err->apr_err == SVN_ERR_INCORRECT_PARAMS)
            {
              svn_error_clear(err);
              SVN_ERR(svn_cmdline_fprintf(stderr, iterpool,
                                          "'%s' is not a number\n",
                                          answer));
              continue;
            }
          else
            return svn_error_trace(err);
        }

      if (m < 0 || m >= suggested_moves->nelts)
        {
          SVN_ERR(svn_cmdline_fprintf(stderr, iterpool,
                                      "Invalid choice (%i)\n", m));
          continue;
        }

      *move = APR_ARRAY_IDX(suggested_moves, m, svn_wc_repos_move_info_t *);
    }
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

/* Implement svn_wc_conflict_resolver_func2_t; resolves based on
   --accept option if given, else by prompting. */
svn_error_t *
svn_cl__conflict_handler(svn_wc_conflict_result_t **result,
                         const svn_wc_conflict_description2_t *desc,
                         void *baton,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  svn_cl__conflict_baton_t *b = baton;
  svn_error_t *err;
  apr_pool_t *subpool;

  /* Start out assuming we're going to postpone the conflict. */
  *result = svn_wc_create_conflict_result(svn_wc_conflict_choose_postpone,
                                          NULL, scratch_pool);

  switch (b->accept_which)
    {
    case svn_cl__accept_invalid:
    case svn_cl__accept_unspecified:
      /* No (or no valid) --accept option, fall through to prompting. */
      break;
    case svn_cl__accept_postpone:
      (*result)->choice = svn_wc_conflict_choose_postpone;
      return SVN_NO_ERROR;
    case svn_cl__accept_base:
      (*result)->choice = svn_wc_conflict_choose_base;
      return SVN_NO_ERROR;
    case svn_cl__accept_working:
      (*result)->choice = svn_wc_conflict_choose_merged;
      return SVN_NO_ERROR;
    case svn_cl__accept_mine_conflict:
      (*result)->choice = svn_wc_conflict_choose_mine_conflict;
      return SVN_NO_ERROR;
    case svn_cl__accept_theirs_conflict:
      (*result)->choice = svn_wc_conflict_choose_theirs_conflict;
      return SVN_NO_ERROR;
    case svn_cl__accept_mine_full:
      (*result)->choice = svn_wc_conflict_choose_mine_full;
      return SVN_NO_ERROR;
    case svn_cl__accept_theirs_full:
      (*result)->choice = svn_wc_conflict_choose_theirs_full;
      return SVN_NO_ERROR;
    case svn_cl__accept_edit:
      if (desc->merged_file)
        {
          if (b->external_failed)
            {
              (*result)->choice = svn_wc_conflict_choose_postpone;
              return SVN_NO_ERROR;
            }

          err = svn_cl__edit_file_externally(desc->merged_file,
                                             b->editor_cmd, b->config, scratch_pool);
          if (err && (err->apr_err == SVN_ERR_CL_NO_EXTERNAL_EDITOR))
            {
              SVN_ERR(svn_cmdline_fprintf(stderr, scratch_pool, "%s\n",
                                          err->message ? err->message :
                                          _("No editor found;"
                                            " leaving all conflicts.")));
              svn_error_clear(err);
              b->external_failed = TRUE;
            }
          else if (err && (err->apr_err == SVN_ERR_EXTERNAL_PROGRAM))
            {
              SVN_ERR(svn_cmdline_fprintf(stderr, scratch_pool, "%s\n",
                                          err->message ? err->message :
                                          _("Error running editor;"
                                            " leaving all conflicts.")));
              svn_error_clear(err);
              b->external_failed = TRUE;
            }
          else if (err)
            return svn_error_trace(err);
          (*result)->choice = svn_wc_conflict_choose_merged;
          return SVN_NO_ERROR;
        }
      /* else, fall through to prompting. */
      break;
    case svn_cl__accept_launch:
      if (desc->base_abspath && desc->their_abspath
          && desc->my_abspath && desc->merged_file)
        {
          svn_boolean_t remains_in_conflict;

          if (b->external_failed)
            {
              (*result)->choice = svn_wc_conflict_choose_postpone;
              return SVN_NO_ERROR;
            }

          err = svn_cl__merge_file_externally(desc->base_abspath,
                                              desc->their_abspath,
                                              desc->my_abspath,
                                              desc->merged_file,
                                              desc->local_abspath,
                                              b->config,
                                              &remains_in_conflict,
                                              scratch_pool);
          if (err && err->apr_err == SVN_ERR_CL_NO_EXTERNAL_MERGE_TOOL)
            {
              SVN_ERR(svn_cmdline_fprintf(stderr, scratch_pool, "%s\n",
                                          err->message ? err->message :
                                          _("No merge tool found;"
                                            " leaving all conflicts.")));
              b->external_failed = TRUE;
              return svn_error_trace(err);
            }
          else if (err && err->apr_err == SVN_ERR_EXTERNAL_PROGRAM)
            {
              SVN_ERR(svn_cmdline_fprintf(stderr, scratch_pool, "%s\n",
                                          err->message ? err->message :
                                          _("Error running merge tool;"
                                            " leaving all conflicts.")));
              b->external_failed = TRUE;
              return svn_error_trace(err);
            }
          else if (err)
            return svn_error_trace(err);

          if (remains_in_conflict)
            (*result)->choice = svn_wc_conflict_choose_postpone;
          else
            (*result)->choice = svn_wc_conflict_choose_merged;
          return SVN_NO_ERROR;
        }
      /* else, fall through to prompting. */
      break;
    }

  /* We're in interactive mode and either the user gave no --accept
     option or the option did not apply; let's prompt. */
  subpool = svn_pool_create(scratch_pool);

  /* Handle the most common cases, which is either:

     Conflicting edits on a file's text, or
     Conflicting edits on a property.
  */
  if (((desc->node_kind == svn_node_file)
       && (desc->action == svn_wc_conflict_action_edit)
       && (desc->reason == svn_wc_conflict_reason_edited))
      || (desc->kind == svn_wc_conflict_kind_property))
  {
      const char *answer;
      char *prompt;
      svn_boolean_t diff_allowed = FALSE;
      /* Have they done something that might have affected the merged
         file (so that we need to save a .edited copy)? */
      svn_boolean_t performed_edit = FALSE;
      /* Have they done *something* (edit, look at diff, etc) to
         give them a rational basis for choosing (r)esolved? */
      svn_boolean_t knows_something = FALSE;

      if (desc->kind == svn_wc_conflict_kind_text)
        SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                    _("Conflict discovered in '%s'.\n"),
                                    svn_dirent_local_style(
                                      desc->local_abspath, subpool)));
      else if (desc->kind == svn_wc_conflict_kind_property)
        {
          SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                      _("Conflict for property '%s' discovered"
                                        " on '%s'.\n"),
                                      desc->property_name,
                                      svn_dirent_local_style(
                                        desc->local_abspath, subpool)));

          if ((!desc->my_abspath && desc->their_abspath)
              || (desc->my_abspath && !desc->their_abspath))
            {
              /* One agent wants to change the property, one wants to
                 delete it.  This is not something we can diff, so we
                 just tell the user. */
              svn_stringbuf_t *myval = NULL, *theirval = NULL;

              if (desc->my_abspath)
                {
                  SVN_ERR(svn_stringbuf_from_file2(&myval, desc->my_abspath,
                                                   subpool));
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                        _("They want to delete the property, "
                          "you want to change the value to '%s'.\n"),
                          myval->data));
                }
              else
                {
                  SVN_ERR(svn_stringbuf_from_file2(&theirval, desc->their_abspath,
                                                   subpool));
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                        _("They want to change the property value to '%s', "
                          "you want to delete the property.\n"),
                           theirval->data));
                }
            }
        }
      else
        /* We don't recognize any other sort of conflict yet */
        return SVN_NO_ERROR;

      /* Diffing can happen between base and merged, to show conflict
         markers to the user (this is the typical 3-way merge
         scenario), or if no base is available, we can show a diff
         between mine and theirs. */
      if ((desc->merged_file && desc->base_abspath)
          || (!desc->base_abspath && desc->my_abspath && desc->their_abspath))
        diff_allowed = TRUE;

      while (TRUE)
        {
          svn_pool_clear(subpool);

          prompt = apr_pstrdup(subpool, _("Select: (p) postpone"));

          if (diff_allowed)
            {
              prompt = apr_pstrcat(subpool, prompt,
                                   _(", (df) diff-full, (e) edit"),
                                   (char *)NULL);

              if (knows_something)
                prompt = apr_pstrcat(subpool, prompt, _(", (r) resolved"),
                                     (char *)NULL);

              if (! desc->is_binary &&
                  desc->kind != svn_wc_conflict_kind_property)
                prompt = apr_pstrcat(subpool, prompt,
                                     _(",\n        (mc) mine-conflict, "
                                       "(tc) theirs-conflict"),
                                     (char *)NULL);
            }
          else
            {
              if (knows_something)
                prompt = apr_pstrcat(subpool, prompt, _(", (r) resolved"),
                                     (char *)NULL);
              prompt = apr_pstrcat(subpool, prompt,
                                   _(",\n        "
                                     "(mf) mine-full, (tf) theirs-full"),
                                   (char *)NULL);
            }

          prompt = apr_pstrcat(subpool, prompt, ",\n        ", (char *)NULL);
          prompt = apr_pstrcat(subpool, prompt,
                               _("(s) show all options: "),
                               (char *)NULL);

          SVN_ERR(svn_cmdline_prompt_user2(&answer, prompt, b->pb, subpool));

          if (strcmp(answer, "s") == 0)
            {
              /* These are used in svn_cl__accept_from_word(). */
              SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
              _("\n"
                "  (e)  edit             - change merged file in an editor\n"
                "  (df) diff-full        - show all changes made to merged "
                                          "file\n"
                "  (r)  resolved         - accept merged version of file\n"
                "\n"
                "  (dc) display-conflict - show all conflicts "
                                          "(ignoring merged version)\n"
                "  (mc) mine-conflict    - accept my version for all "
                                          "conflicts (same)\n"
                "  (tc) theirs-conflict  - accept their version for all "
                                          "conflicts (same)\n"
                "\n"
                "  (mf) mine-full        - accept my version of entire file "
                                          "(even non-conflicts)\n"
                "  (tf) theirs-full      - accept their version of entire "
                                          "file (same)\n"
                "\n"
                "  (p)  postpone         - mark the conflict to be "
                                          "resolved later\n"
                "  (l)  launch           - launch external tool to "
                                          "resolve conflict\n"
                "  (s)  show all         - show this list\n\n")));
            }
          else if (strcmp(answer, "p") == 0 || strcmp(answer, ":-P") == 0)
            {
              /* Do nothing, let file be marked conflicted. */
              (*result)->choice = svn_wc_conflict_choose_postpone;
              break;
            }
          else if (strcmp(answer, "mc") == 0 || strcmp(answer, "X-)") == 0)
            {
              if (desc->is_binary)
                {
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                              _("Invalid option; cannot choose "
                                                "based on conflicts in a "
                                                "binary file.\n\n")));
                  continue;
                }
              else if (desc->kind == svn_wc_conflict_kind_property)
                {
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                              _("Invalid option; cannot choose "
                                                "based on conflicts for "
                                                "properties.\n\n")));
                  continue;
                }

              (*result)->choice = svn_wc_conflict_choose_mine_conflict;
              if (performed_edit)
                (*result)->save_merged = TRUE;
              break;
            }
          else if (strcmp(answer, "tc") == 0 || strcmp(answer, "X-(") == 0)
            {
              if (desc->is_binary)
                {
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                              _("Invalid option; cannot choose "
                                                "based on conflicts in a "
                                                "binary file.\n\n")));
                  continue;
                }
              else if (desc->kind == svn_wc_conflict_kind_property)
                {
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                              _("Invalid option; cannot choose "
                                                "based on conflicts for "
                                                "properties.\n\n")));
                  continue;
                }
              (*result)->choice = svn_wc_conflict_choose_theirs_conflict;
              if (performed_edit)
                (*result)->save_merged = TRUE;
              break;
            }
          else if (strcmp(answer, "mf") == 0 || strcmp(answer, ":-)") == 0)
            {
              (*result)->choice = svn_wc_conflict_choose_mine_full;
              if (performed_edit)
                (*result)->save_merged = TRUE;
              break;
            }
          else if (strcmp(answer, "tf") == 0 || strcmp(answer, ":-(") == 0)
            {
              (*result)->choice = svn_wc_conflict_choose_theirs_full;
              if (performed_edit)
                (*result)->save_merged = TRUE;
              break;
            }
          else if (strcmp(answer, "dc") == 0)
            {
              if (desc->is_binary)
                {
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                              _("Invalid option; cannot "
                                                "display conflicts for a "
                                                "binary file.\n\n")));
                  continue;
                }
              else if (desc->kind == svn_wc_conflict_kind_property)
                {
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                              _("Invalid option; cannot "
                                                "display conflicts for "
                                                "properties.\n\n")));
                  continue;
                }
              else if (! (desc->my_abspath && desc->base_abspath && desc->their_abspath))
                {
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                              _("Invalid option; original "
                                                "files not available.\n\n")));
                  continue;
                }
              SVN_ERR(show_conflicts(desc, subpool));
              knows_something = TRUE;
            }
          else if (strcmp(answer, "df") == 0)
            {
              if (! diff_allowed)
                {
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                 _("Invalid option; there's no "
                                    "merged version to diff.\n\n")));
                  continue;
                }

              SVN_ERR(show_diff(desc, subpool));
              knows_something = TRUE;
            }
          else if (strcmp(answer, "e") == 0 || strcmp(answer, ":-E") == 0)
            {
              SVN_ERR(open_editor(&performed_edit, desc, b, subpool));
              if (performed_edit)
                knows_something = TRUE;
            }
          else if (strcmp(answer, "l") == 0 || strcmp(answer, ":-l") == 0)
            {
              if (desc->kind == svn_wc_conflict_kind_property)
                {
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                              _("Invalid option; cannot "
                                                "resolve property conflicts "
                                                "with an external merge tool."
                                                "\n\n")));
                  continue;
                }
              if (desc->base_abspath && desc->their_abspath && desc->my_abspath
                    && desc->merged_file)
                {
                  SVN_ERR(launch_resolver(&performed_edit, desc, b, subpool));
                  if (performed_edit)
                    knows_something = TRUE;
                }
              else
                SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                            _("Invalid option.\n\n")));
            }
          else if (strcmp(answer, "r") == 0)
            {
              /* We only allow the user accept the merged version of
                 the file if they've edited it, or at least looked at
                 the diff. */
              if (knows_something)
                {
                  (*result)->choice = svn_wc_conflict_choose_merged;
                  break;
                }
              else
                SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                                            _("Invalid option.\n\n")));
            }
        }
    }
  /*
    Dealing with obstruction of additions can be tricky.  The
    obstructing item could be unversioned, versioned, or even
    schedule-add.  Here's a matrix of how the caller should behave,
    based on results we return.

                         Unversioned       Versioned       Schedule-Add

      choose_mine       skip addition,    skip addition     skip addition
                        add existing item

      choose_theirs     destroy file,    schedule-delete,   revert add,
                        add new item.    add new item.      rm file,
                                                            add new item

      postpone               [              bail out                 ]

   */
  else if ((desc->action == svn_wc_conflict_action_add)
           && (desc->reason == svn_wc_conflict_reason_obstructed))
    {
      const char *answer;
      const char *prompt;

      SVN_ERR(svn_cmdline_fprintf(
                   stderr, subpool,
                   _("Conflict discovered when trying to add '%s'.\n"
                     "An object of the same name already exists.\n"),
                   svn_dirent_local_style(desc->local_abspath, subpool)));
      prompt = _("Select: (p) postpone, (mf) mine-full, "
                 "(tf) theirs-full, (h) help:");

      while (1)
        {
          svn_pool_clear(subpool);

          SVN_ERR(svn_cmdline_prompt_user2(&answer, prompt, b->pb, subpool));

          if (strcmp(answer, "h") == 0 || strcmp(answer, "?") == 0)
            {
              SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
              _("  (p)  postpone    - resolve the conflict later\n"
                "  (mf) mine-full   - accept pre-existing item "
                "(ignore upstream addition)\n"
                "  (tf) theirs-full - accept incoming item "
                "(overwrite pre-existing item)\n"
                "  (h)  help        - show this help\n\n")));
            }
          if (strcmp(answer, "p") == 0 || strcmp(answer, ":-P") == 0)
            {
              (*result)->choice = svn_wc_conflict_choose_postpone;
              break;
            }
          if (strcmp(answer, "mf") == 0 || strcmp(answer, ":-)") == 0)
            {
              (*result)->choice = svn_wc_conflict_choose_mine_full;
              break;
            }
          if (strcmp(answer, "tf") == 0 || strcmp(answer, ":-(") == 0)
            {
              (*result)->choice = svn_wc_conflict_choose_theirs_full;
              break;
            }
        }
    }

  else if (desc->kind == svn_wc_conflict_kind_tree &&
           desc->action == svn_wc_conflict_action_delete &&
           desc->server_sends_moves_as_copy_plus_delete &&
           (desc->reason == svn_wc_conflict_reason_moved_away ||
            desc->reason == svn_wc_conflict_reason_moved_here ||
            desc->reason == svn_wc_conflict_reason_deleted))
    {
      const char *answer;
      const char *prompt;

      if (!desc->suggested_moves)
        {
          const char *readable_desc;

          SVN_ERR(svn_cl__get_human_readable_tree_conflict_description(
                   &readable_desc, desc, scratch_pool));
          SVN_ERR(svn_cmdline_fprintf(
                       stderr, subpool,
                       _("Tree conflict on '%s'\n   > %s\n"
                         "Server is sending moves as copy+delete.\n"
                         "Maybe this incoming delete is part of a move?\n"),
                       svn_cl__local_style_skip_ancestor(b->path_prefix,
                                                         desc->local_abspath,
                                                         scratch_pool),
                       readable_desc));
        }
      prompt = _("Select: (p) postpone, (f) find-move,\n"
                 "        (a) ask-move, (d) is-delete, (h) help: ");

      while (1)
        {
          svn_pool_clear(subpool);

          if (desc->suggested_moves && b->show_moves)
            {
              b->show_moves = FALSE;
              if (desc->suggested_moves->nelts == 0)
                {
                  SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                          _("No move found in revision log.\n")));
                }
              else
                {
                  svn_wc_repos_move_info_t *move;

                  SVN_ERR(pick_move(&move, desc->suggested_moves, b->pb,
                                    subpool));
                  if (move)
                    {
                      (*result)->choice = svn_wc_conflict_choose_incoming_move;
                      (*result)->incoming_move = move;
                      break;
                    }
                }
            }

          SVN_ERR(svn_cmdline_prompt_user2(&answer, prompt, b->pb, subpool));

          if (strcmp(answer, "h") == 0 || strcmp(answer, "?") == 0)
            {
              SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
              _("  (p) postpone      - resolve the conflict later\n"
                "  (f) find-move     - try to find move in revision log\n"
                "  (a) ask-move      - specify server-side move yourself\n"
                "  (d) is-delete     - treat incoming delete as delete\n"
                "  (h) help          - show this help\n\n")));
            }

          if (strcmp(answer, "p") == 0 || strcmp(answer, ":-P") == 0)
            {
              (*result)->choice = svn_wc_conflict_choose_postpone;
              break;
            }
          if (strcmp(answer, "a") == 0)
            {
              SVN_ERR(svn_cmdline_fprintf(stderr, subpool,
                      _("Sorry, 'ask-move' is not implemented yet.\n")));
              continue;
            }
          if (strcmp(answer, "d") == 0)
            {
              (*result)->choice = svn_wc_conflict_choose_delete_is_delete;
              break;
            }

          if (strcmp(answer, "f") == 0)
            {
              b->show_moves = TRUE;
              if (desc->suggested_moves)
                continue;

              /* No suggested moves information yet, need to ask the library
               * to scan the revision log for moves. */
              (*result)->choice = svn_wc_conflict_choose_scan_log_for_moves;
              break;
            }
        }
    }

  else if (desc->kind == svn_wc_conflict_kind_tree &&
           desc->action == svn_wc_conflict_action_move_away)
    {
      const char *answer;
      const char *prompt;
      const char *readable_desc;
      const char *help_text;
      svn_wc_repos_move_info_t *move;

      SVN_ERR(svn_cl__get_human_readable_tree_conflict_description(
               &readable_desc, desc, scratch_pool));
      SVN_ERR(svn_cmdline_fprintf(
                   stderr, subpool,
                   _("Tree conflict on '%s'\n   > %s\n"),
                   /* ### TODO: show the local move, too */
                   svn_cl__local_style_skip_ancestor(b->path_prefix,
                                                     desc->local_abspath,
                                                     scratch_pool),
                   readable_desc));

      if (desc->server_sends_moves_as_copy_plus_delete)
        SVN_ERR(pick_move(&move, desc->suggested_moves, b->pb,
                          subpool));
      else
        return svn_error_createf(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                 _("This client only supports servers which "
                                   "send moves as copy+delete, but the server "
                                   "does not send moves as copy+delete"));

      prompt = _("Select: (p) postpone, (mc) mine-conflict,\n"
                 "        (tc) theirs-conflict, (ci) change-incoming,\n"
                 "        (cl) change-local, (h) help: ");
      if (desc->reason == svn_wc_conflict_reason_moved_away ||
          desc->reason == svn_wc_conflict_reason_moved_here)
        help_text = 
          _("  (p)  postpone        - resolve the conflict later\n"
            "  (mc) mine-conflict   - use local move\n"
            "  (tc) theirs-conflict - use incoming move\n"
            "  (ci) change-incoming - change incoming move target\n"
            "  (cl) change-local    - change local move target\n"
            "  (h)  help            - show this help\n\n");
      else if (desc->reason == svn_wc_conflict_reason_deleted)
        help_text = 
          _("  (p)  postpone        - resolve the conflict later\n"
            "  (mc) mine-conflict   - delete the item\n"
            "  (tc) theirs-conflict - move the item\n"
            "  (h)  help            - show this help\n\n");

      while (1)
        {
          svn_pool_clear(subpool);

          SVN_ERR(svn_cmdline_prompt_user2(&answer, prompt, b->pb, subpool));

          if (strcmp(answer, "h") == 0 || strcmp(answer, "?") == 0)
            {
              SVN_ERR(svn_cmdline_fprintf(stderr, subpool, "%s", help_text));
              continue;
            }
          if (strcmp(answer, "p") == 0 || strcmp(answer, ":-P") == 0)
            {
              (*result)->choice = svn_wc_conflict_choose_postpone;
              break;
            }
          if (strcmp(answer, "mc") == 0 || strcmp(answer, "X-)") == 0)
            {
              (*result)->choice = svn_wc_conflict_choose_mine_conflict;
              break;
            }
          if (strcmp(answer, "ci") == 0)
            {
              /* ### TODO: Needs a user-friendly UI. Tab-completion? */
              prompt = _("Please type the new incoming move target path: ");
              SVN_ERR(svn_cmdline_prompt_user2(&answer, prompt, b->pb,
                                               subpool));
              (*result)->new_incoming_move_target = answer;
              (*result)->choice =
                svn_wc_conflict_choose_new_incoming_move_target;
              break;
            }
          if (strcmp(answer, "tc") == 0 || strcmp(answer, "X-(") == 0)
            {
              (*result)->choice = svn_wc_conflict_choose_theirs_conflict;
              break;
            }
          if (strcmp(answer, "cl") == 0)
            {
              /* ### TODO: Needs a user-friendly UI. Tab-completion? */
              prompt = _("Please type the new local move target path: ");
              SVN_ERR(svn_cmdline_prompt_user2(&answer, prompt, b->pb,
                                               subpool));
              (*result)->new_local_move_target = answer;
              (*result)->choice = svn_wc_conflict_choose_new_local_move_target;
              break;
            }
        }
    }

  else /* other types of conflicts -- do nothing about them. */
    {
      (*result)->choice = svn_wc_conflict_choose_postpone;
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}
