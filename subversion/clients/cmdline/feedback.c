/*
 * feedback.c:  feedback handlers for cmdline client.
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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

#include <stdio.h>

#define APR_WANT_STDIO
#define APR_WANT_STRFUNC
#include <apr_want.h>

#include "svn_pools.h"
#include "svn_path.h"
#include "svn_utf.h"
#include "cl.h"


/* Baton for notify and friends. */
struct notify_baton
{
  svn_boolean_t received_some_change;
  svn_boolean_t is_checkout;
  svn_boolean_t is_export;
  svn_boolean_t suppress_final_line;
  svn_boolean_t sent_first_txdelta;
  apr_pool_t *pool;
};


/* This implements `svn_wc_notify_func_t'. */
static void
notify (void *baton,
        const char *path,
        svn_wc_notify_action_t action,
        svn_node_kind_t kind,
        const char *mime_type,
        svn_wc_notify_state_t content_state,
        svn_wc_notify_state_t prop_state,
        svn_revnum_t revision)
{
  struct notify_baton *nb = baton;
  char statchar_buf[3] = "  ";

  /* the pool (BATON) is typically the global pool; don't keep filling it */
  apr_pool_t *subpool = svn_pool_create (nb->pool);

  const char *path_native;
  svn_error_t *err;

  err = svn_utf_cstring_from_utf8 (&path_native,
                                   svn_path_local_style (path, subpool),
                                   subpool);
  if (err)
    {
      printf ("WARNING: error decoding UTF-8 for ?\n");
      svn_pool_destroy (subpool);
      return;
    }
  switch (action)
    {
    case svn_wc_notify_skip:
      if (content_state == svn_wc_notify_state_missing)
        printf ("Skipped missing target: %s\n", path_native);
      else
        printf ("Skipped %s\n", path_native);
      break;

    case svn_wc_notify_update_delete:
      nb->received_some_change = TRUE;
      printf ("D  %s\n", path_native);
      break;

    case svn_wc_notify_update_add:
      nb->received_some_change = TRUE;
      printf ("A  %s\n", path_native);
      break;

    case svn_wc_notify_restore:
      printf ("Restored %s\n", path_native);
      break;

    case svn_wc_notify_revert:
      printf ("Reverted %s\n", path_native);
      break;

    case svn_wc_notify_failed_revert:
      printf ("Failed to revert %s -- try updating instead.\n", path_native);
      break;

    case svn_wc_notify_resolved:
      printf ("Resolved conflicted state of %s\n", path_native);
      break;

    case svn_wc_notify_add:
      /* We *should* only get the MIME_TYPE if PATH is a file.  If we
         do get it, and the mime-type is not textual, note that this
         is a binary addition. */
      if (mime_type && (svn_mime_type_is_binary (mime_type)))
        printf ("A  (bin)  %s\n", path_native);
      else
        printf ("A         %s\n", path_native);
      break;

    case svn_wc_notify_delete:
      nb->received_some_change = TRUE;
      printf ("D         %s\n", path_native);
      break;

    case svn_wc_notify_update_update:
      {
        /* If this is an inoperative dir change, do no notification.
           An inoperative dir change is when a directory gets closed
           without any props having been changed. */
        if (! ((kind == svn_node_dir)
               && ((prop_state == svn_wc_notify_state_inapplicable)
                   || (prop_state == svn_wc_notify_state_unknown)
                   || (prop_state == svn_wc_notify_state_unchanged))))
          {
            nb->received_some_change = TRUE;
            
            if (kind == svn_node_file)
              {
                if (content_state == svn_wc_notify_state_conflicted)
                  statchar_buf[0] = 'C';
                else if (content_state == svn_wc_notify_state_merged)
                  statchar_buf[0] = 'G';
                else if (content_state == svn_wc_notify_state_changed)
                  statchar_buf[0] = 'U';
              }
            
            if (prop_state == svn_wc_notify_state_conflicted)
              statchar_buf[1] = 'C';
            else if (prop_state == svn_wc_notify_state_merged)
              statchar_buf[1] = 'G';
            else if (prop_state == svn_wc_notify_state_changed)
              statchar_buf[1] = 'U';

            if (! ((content_state == svn_wc_notify_state_unchanged
                    || content_state == svn_wc_notify_state_unknown)
                   && (prop_state == svn_wc_notify_state_unchanged
                       || prop_state == svn_wc_notify_state_unknown)))
              printf ("%s %s\n", statchar_buf, path_native);
          }
      }
      break;

    case svn_wc_notify_update_external:
      /* Currently this is used for checkouts and switches too.  If we
         want different output, we'll have to add new actions. */
      printf ("\nFetching external item into %s\n", path_native);
      break;

    case svn_wc_notify_update_completed:
      {
        if (! nb->suppress_final_line)
          {
            if (SVN_IS_VALID_REVNUM (revision))
              {
                if (nb->is_export)
                  printf ("Exported revision %" SVN_REVNUM_T_FMT ".\n",
                          revision);
                else if (nb->is_checkout)
                  printf ("Checked out revision %" SVN_REVNUM_T_FMT ".\n",
                          revision);
                else
                  {
                    if (nb->received_some_change)
                      printf ("Updated to revision %" SVN_REVNUM_T_FMT ".\n",
                              revision);
                    else
                      printf ("At revision %" SVN_REVNUM_T_FMT ".\n",
                              revision);
                  }
              }
            else  /* no revision */
              {
                if (nb->is_export)
                  printf ("Export complete.\n");
                else if (nb->is_checkout)
                  printf ("Checkout complete.\n");
                else
                  printf ("Update complete\n");
              }
          }
      }

      break;

    case svn_wc_notify_status_external:
      printf ("\nPerforming status on external item at %s\n", path_native);
      break;

    case svn_wc_notify_status_completed:
      if (SVN_IS_VALID_REVNUM (revision))
        printf ("Status against revision: %6" SVN_REVNUM_T_FMT "\n", revision);
      break;

    case svn_wc_notify_commit_modified:
      printf ("Sending        %s\n", path_native);
      break;

    case svn_wc_notify_commit_added:
      if (mime_type && svn_mime_type_is_binary (mime_type))
        printf ("Adding  (bin)  %s\n", path_native);
      else
        printf ("Adding         %s\n", path_native);
      break;

    case svn_wc_notify_commit_deleted:
      printf ("Deleting       %s\n", path_native);
      break;

    case svn_wc_notify_commit_replaced:
      printf ("Replacing      %s\n", path_native);
      break;

    case svn_wc_notify_commit_postfix_txdelta:
      if (! nb->sent_first_txdelta)
        {
          printf ("Transmitting file data ");
          nb->sent_first_txdelta = TRUE;
        }

      printf (".");
      fflush (stdout);
      break;

    default:
      break;
    }

  svn_pool_destroy (subpool);
}


void
svn_cl__get_notifier (svn_wc_notify_func_t *notify_func_p,
                      void **notify_baton_p,
                      svn_boolean_t is_checkout,
                      svn_boolean_t is_export,
                      svn_boolean_t suppress_final_line,
                      apr_pool_t *pool)
{
  struct notify_baton *nb = apr_palloc (pool, sizeof (*nb));

  nb->received_some_change = FALSE;
  nb->sent_first_txdelta = FALSE;
  nb->is_checkout = is_checkout;
  nb->is_export = is_export;
  nb->suppress_final_line = suppress_final_line;
  nb->pool = pool;

  *notify_func_p = notify;
  *notify_baton_p = nb;
}
