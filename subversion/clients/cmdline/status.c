/*
 * status.c:  the command-line's portion of the "svn status" command
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
#include <apr_hash.h>
#include <apr_tables.h>
#include "svn_cmdline.h"
#include "svn_sorts.h"
#include "svn_wc.h"
#include "cl.h"


/* Return the single character representation of STATUS */
static char
generate_status_code (enum svn_wc_status_kind status)
{
  switch (status)
    {
    case svn_wc_status_none:        return ' ';
    case svn_wc_status_normal:      return ' ';
    case svn_wc_status_added:       return 'A';
    case svn_wc_status_missing:     return '!';
    case svn_wc_status_incomplete:  return '!';
    case svn_wc_status_deleted:     return 'D';
    case svn_wc_status_replaced:    return 'R';
    case svn_wc_status_modified:    return 'M';
    case svn_wc_status_merged:      return 'G';
    case svn_wc_status_conflicted:  return 'C';
    case svn_wc_status_obstructed:  return '~';
    case svn_wc_status_ignored:     return 'I';
    case svn_wc_status_external:    return 'X';
    case svn_wc_status_unversioned: return '?';
    default:                        return '?';
    }
}

/* Print STATUS and PATH in a format determined by DETAILED and
   SHOW_LAST_COMMITTED */
static void 
print_status (const char *path,
              svn_boolean_t detailed,
              svn_boolean_t show_last_committed,
              svn_wc_status_t *status,
              apr_pool_t *pool)
{
  char ood_status = '@';    /* Silence a gcc uninitialized warning */
  char working_rev_buf[21]; /* Enough for 2^64 in base 10 plus '\0' */
  char commit_rev_buf[21];
  const char *working_rev = working_rev_buf;
  const char *commit_rev = commit_rev_buf;
  const char *commit_author = NULL; /* Silence a gcc uninitialised warning */

  if (detailed)
    {
      if (! status->entry)
        working_rev = "";
      else if (! SVN_IS_VALID_REVNUM (status->entry->revision))
        working_rev = " ? ";
      else if (status->copied)
        working_rev = "-";
      else
        sprintf (working_rev_buf, "%" SVN_REVNUM_T_FMT,
                 status->entry->revision);

      if (status->repos_text_status != svn_wc_status_none
          || status->repos_prop_status != svn_wc_status_none)
        ood_status = '*';
      else
        ood_status = ' ';

      if (show_last_committed)
        {
          if (status->entry && SVN_IS_VALID_REVNUM (status->entry->cmt_rev))
            sprintf(commit_rev_buf, "%" SVN_REVNUM_T_FMT,
                    status->entry->cmt_rev);
          else if (status->entry)
            commit_rev = " ? ";
          else
            commit_rev = "";

          if (status->entry && status->entry->cmt_author)
            {
              const char *const author_utf8 = status->entry->cmt_author;
              svn_error_t *err =
                svn_cmdline_cstring_from_utf8 (&commit_author, author_utf8,
                                               pool);
              if (err)
                {
                  svn_error_clear (err);
                  commit_author =
                    svn_cmdline_cstring_from_utf8_fuzzy (author_utf8, pool);
                }
            }
          else if (status->entry)
            commit_author = " ? ";
          else
            commit_author = "";
        }
    }

  if (detailed && show_last_committed)
    printf ("%c%c%c%c%c  %c   %6s   %6s %-12s %s\n",
            generate_status_code (status->text_status),
            generate_status_code (status->prop_status),
            status->locked ? 'L' : ' ',
            status->copied ? '+' : ' ',
            status->switched ? 'S' : ' ',
            ood_status,
            working_rev,
            commit_rev,
            commit_author,
            path);

  else if (detailed)
    printf ("%c%c%c%c%c  %c   %6s   %s\n",
            generate_status_code (status->text_status),
            generate_status_code (status->prop_status),
            status->locked ? 'L' : ' ',
            status->copied ? '+' : ' ',
            status->switched ? 'S' : ' ',
            ood_status,
            working_rev,
            path);

  else
    printf ("%c%c%c%c%c  %s\n",
            generate_status_code (status->text_status),
            generate_status_code (status->prop_status),
            status->locked ? 'L' : ' ',
            status->copied ? '+' : ' ',
            status->switched ? 'S' : ' ',
            path);
}

/* Called by status-cmd.c */
void
svn_cl__print_status (const char *path,
                      svn_wc_status_t *status,
                      svn_boolean_t detailed,
                      svn_boolean_t show_last_committed,
                      svn_boolean_t skip_unrecognized,
                      apr_pool_t *pool)
{
  svn_error_t *err;
  const char *path_stdout;

  if (! status 
      || (skip_unrecognized && ! status->entry)
      || (status->text_status == svn_wc_status_none
          && status->repos_text_status == svn_wc_status_none))
    return;

  err = svn_cmdline_path_local_style_from_utf8 (&path_stdout, path, pool);
  if (err)
    {
      svn_handle_error (err, stderr, FALSE);
      svn_error_clear (err);
    }

  print_status (path_stdout, detailed, show_last_committed, status, pool);
}
