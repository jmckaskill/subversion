/*
 * log-cmd.c -- Display log messages
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

#define APR_WANT_STRFUNC
#include <apr_want.h>

#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "svn_sorts.h"
#include "svn_xml.h"
#include "svn_time.h"
#include "svn_cmdline.h"
#include "svn_subst.h"
#include "cl.h"


/*** Code. ***/

/* Helper for log_message_receiver(). 
 *
 * Return the number of lines in MSG, allowing any kind of newline
 * termination (CR, CRLF, or LFCR), even inconsistent.  The minimum
 * number of lines in MSG is 1 -- even the empty string is considered
 * to have one line, due to the way we print log messages.
 */
static int
num_lines (const char *msg)
{
  int count = 1;
  const char *p;

  for (p = msg; *p; p++)
    {
      if (*p == '\n')
        {
          count++;
          if (*(p + 1) == '\r')
            p++;
        }
      else if (*p == '\r')
        {
          count++;
          if (*(p + 1) == '\n')
            p++;
        }
    }

  return count;
}


/* Baton for log_message_receiver() and log_message_receiver_xml(). */
struct log_receiver_baton
{
  /* Check for cancellation on each invocation of a log receiver. */
  svn_cancel_func_t cancel_func;
  void *cancel_baton;

  /* Don't print log message body nor its line count. */
  svn_boolean_t omit_log_message;

  /* Output stream */
  svn_stream_t *out;
};


/* The separator between log messages. */
#define SEP_STRING \
  "------------------------------------------------------------------------" \
  APR_EOL_STR


/* Implement `svn_log_message_receiver_t', printing the logs in
 * a human-readable and machine-parseable format.  
 *
 * BATON is of type `struct log_receiver_baton'.
 *
 * First, print a header line.  Then if CHANGED_PATHS is non-null,
 * print all affected paths in a list headed "Changed paths:\n",
 * immediately following the header line.  Then print a newline
 * followed by the message body, unless BATON->omit_log_message is true.
 *
 * Here are some examples of the output:
 *
 * $ svn log -r1847:1846
 * ------------------------------------------------------------------------
 * rev 1847:  cmpilato | Wed 1 May 2002 15:44:26 | 7 lines
 * 
 * Fix for Issue #694.
 * 
 * * subversion/libsvn_repos/delta.c
 *   (delta_files): Rework the logic in this function to only call
 * send_text_deltas if there are deltas to send, and within that case,
 * only use a real delta stream if the caller wants real text deltas.
 * 
 * ------------------------------------------------------------------------
 * rev 1846:  whoever | Wed 1 May 2002 15:23:41 | 1 line
 *   
 * imagine an example log message here
 * ------------------------------------------------------------------------
 * 
 * Or:
 *
 * $ svn log -r1847:1846 -v
 * ------------------------------------------------------------------------
 * rev 1847:  cmpilato | Wed 1 May 2002 15:44:26 | 7 lines
 * Changed paths:
 *    M /trunk/subversion/libsvn_repos/delta.c
 * 
 * Fix for Issue #694.
 * 
 * * subversion/libsvn_repos/delta.c
 *   (delta_files): Rework the logic in this function to only call
 * send_text_deltas if there are deltas to send, and within that case,
 * only use a real delta stream if the caller wants real text deltas.
 * 
 * ------------------------------------------------------------------------
 * rev 1846:  whoever | Wed 1 May 2002 15:23:41 | 1 line
 * Changed paths:
 *    M /trunk/notes/fs_dumprestore.txt
 *    M /trunk/subversion/libsvn_repos/dump.c
 *   
 * imagine an example log message here
 * ------------------------------------------------------------------------
 * 
 * Or:
 *
 * $ svn log -r1847:1846 -q
 * ------------------------------------------------------------------------
 * rev 1847:  cmpilato | Wed 1 May 2002 15:44:26
 * ------------------------------------------------------------------------
 * rev 1846:  whoever | Wed 1 May 2002 15:23:41
 * ------------------------------------------------------------------------
 *
 * Or:
 *
 * $ svn log -r1847:1846 -qv
 * ------------------------------------------------------------------------
 * rev 1847:  cmpilato | Wed 1 May 2002 15:44:26
 * Changed paths:
 *    M /trunk/subversion/libsvn_repos/delta.c
 * ------------------------------------------------------------------------
 * rev 1846:  whoever | Wed 1 May 2002 15:23:41
 * Changed paths:
 *    M /trunk/notes/fs_dumprestore.txt
 *    M /trunk/subversion/libsvn_repos/dump.c
 * ------------------------------------------------------------------------
 *
 */
static svn_error_t *
log_message_receiver (void *baton,
                      apr_hash_t *changed_paths,
                      svn_revnum_t rev,
                      const char *author,
                      const char *date,
                      const char *msg,
                      apr_pool_t *pool)
{
  struct log_receiver_baton *lb = baton;
  const char *author_stdout, *date_stdout;
  const char *msg_stdout = NULL;     /* Silence a gcc uninitialized warning */
  svn_error_t *err;

  /* Number of lines in the msg. */
  int lines;

  if (lb->cancel_func)
    SVN_ERR (lb->cancel_func (lb->cancel_baton));

  if (rev == 0)
    {
      return svn_stream_printf (lb->out, pool,
                                "No commit for revision 0." APR_EOL_STR);
    }

  /* ### See http://subversion.tigris.org/issues/show_bug.cgi?id=807
     for more on the fallback fuzzy conversions below. */

  if (author == NULL)
    author = "(no author)";

  err = svn_cmdline_cstring_from_utf8 (&author_stdout, author, pool);
  if (err && (APR_STATUS_IS_EINVAL (err->apr_err)))
    {
      author_stdout = svn_cmdline_cstring_from_utf8_fuzzy (author, pool);
      svn_error_clear (err);
    }
  else if (err)
    return err;

  if (date && date[0])
    {
      /* Convert date to a format for humans. */
      apr_time_t time_temp;
      const char *date_utf8;
      
      SVN_ERR (svn_time_from_cstring (&time_temp, date, pool));
      date_utf8 = svn_time_to_human_cstring(time_temp, pool);
      SVN_ERR (svn_cmdline_cstring_from_utf8 (&date_stdout, date_utf8, pool));
    }
  else
    date_stdout = "(no date)";
  
  if (! lb->omit_log_message)
    {
      if (msg == NULL)
        msg = "";

      {
        /* Convert log message from UTF8/LF to native locale and eol-style. */
        svn_string_t *logmsg = svn_string_create (msg, pool);
        SVN_ERR (svn_subst_detranslate_string (&logmsg, logmsg, TRUE, pool));
        msg_stdout = logmsg->data;
      }
    }

  SVN_ERR (svn_stream_printf (lb->out, pool, SEP_STRING));

  SVN_ERR (svn_stream_printf (lb->out, pool,
                              "r%" SVN_REVNUM_T_FMT " | %s | %s",
                              rev, author_stdout, date_stdout));

  if (! lb->omit_log_message)
    {
      lines = num_lines (msg_stdout);
      SVN_ERR (svn_stream_printf (lb->out, pool,
                                  " | %d line%s", lines,
                                  (lines > 1) ? "s" : ""));
    }

  SVN_ERR (svn_stream_printf (lb->out, pool, APR_EOL_STR));

  if (changed_paths)
    {
      apr_array_header_t *sorted_paths;
      int i;

      /* Get an array of sorted hash keys. */
      sorted_paths = apr_hash_sorted_keys (changed_paths,
                                           svn_sort_compare_items_as_paths, 
                                           pool);

      SVN_ERR (svn_stream_printf (lb->out, pool,
                                  "Changed paths:" APR_EOL_STR));
      for (i = 0; i < sorted_paths->nelts; i++)
        {
          svn_item_t *item = &(APR_ARRAY_IDX (sorted_paths, i, svn_item_t));
          const char *path_stdout, *path = item->key;
          svn_log_changed_path_t *log_item 
            = apr_hash_get (changed_paths, item->key, item->klen);
          const char *copy_data = "";
          
          if (log_item->copyfrom_path 
              && SVN_IS_VALID_REVNUM (log_item->copyfrom_rev))
            {
              SVN_ERR (svn_cmdline_path_local_style_from_utf8
                       (&path_stdout, log_item->copyfrom_path, pool));
              copy_data 
                = apr_psprintf (pool, 
                                " (from %s:%" SVN_REVNUM_T_FMT ")",
                                path_stdout,
                                log_item->copyfrom_rev);
            }
          SVN_ERR (svn_cmdline_path_local_style_from_utf8
                   (&path_stdout, path, pool));
          SVN_ERR (svn_stream_printf (lb->out, pool, "   %c %s%s" APR_EOL_STR,
                                      log_item->action, path_stdout,
                                      copy_data));
        }
    }

  if (! lb->omit_log_message)
    {
      /* A blank line always precedes the log message. */
      SVN_ERR (svn_stream_printf (lb->out, pool, APR_EOL_STR "%s" APR_EOL_STR,
                                  msg_stdout));
    }

  return SVN_NO_ERROR;
}


/* This implements `svn_log_message_receiver_t', printing the logs in XML.
 *
 * BATON is of type `struct log_receiver_baton'.
 *
 * Here is an example of the output; note that the "<log>" and
 * "</log>" tags are not emitted by this function:
 * 
 * $ svn log --xml -r 1648:1649
 * <log>
 * <logentry
 *    revision="1648">
 * <author>david</author>
 * <date>Sat 6 Apr 2002 16:34:51.428043 (day 096, dst 0, gmt_off -21600)</date>
 * <msg> * packages/rpm/subversion.spec : Now requires apache 2.0.36.
 * </msg>
 * </logentry>
 * <logentry
 *    revision="1649">
 * <author>cmpilato</author>
 * <date>Sat 6 Apr 2002 17:01:28.185136 (day 096, dst 0, gmt_off -21600)</date>
 * <msg>Fix error handling when the $EDITOR is needed but unavailable.  Ah
 * ... now that&apos;s *much* nicer.
 * 
 * * subversion/clients/cmdline/util.c
 *   (svn_cl__edit_externally): Clean up the &quot;no external editor&quot;
 *   error message.
 *   (svn_cl__get_log_message): Wrap &quot;no external editor&quot; 
 *   errors with helpful hints about the -m and -F options.
 * 
 * * subversion/libsvn_client/commit.c
 *   (svn_client_commit): Actually capture and propogate &quot;no external
 *   editor&quot; errors.</msg>
 * </logentry>
 * </log>
 *
 */
static svn_error_t *
log_message_receiver_xml (void *baton,
                          apr_hash_t *changed_paths,
                          svn_revnum_t rev,
                          const char *author,
                          const char *date,
                          const char *msg,
                          apr_pool_t *pool)
{
  struct log_receiver_baton *lb = baton;
  /* Collate whole log message into sb before printing. */
  svn_stringbuf_t *sb = svn_stringbuf_create ("", pool);
  char *revstr;
  const char *msg_native_eol;  

  if (lb->cancel_func)
    SVN_ERR (lb->cancel_func (lb->cancel_baton));

  if (rev == 0)
    return SVN_NO_ERROR;

  revstr = apr_psprintf (pool, "%" SVN_REVNUM_T_FMT, rev);
  /* <logentry revision="xxx"> */
  svn_xml_make_open_tag (&sb, pool, svn_xml_normal, "logentry",
                         "revision", revstr, NULL);

  if (author)
    {
      /* <author>xxx</author> */
      svn_xml_make_open_tag (&sb, pool, svn_xml_protect_pcdata, "author",
                             NULL);
      svn_xml_escape_cdata_cstring (&sb, author, pool);
      svn_xml_make_close_tag (&sb, pool, "author");
    }

  if (date)
    {
      /* Print the full, uncut, date.  This is machine output. */
      /* <date>xxx</date> */
      svn_xml_make_open_tag (&sb, pool, svn_xml_protect_pcdata, "date",
                             NULL);
      svn_xml_escape_cdata_cstring (&sb, date, pool);
      svn_xml_make_close_tag (&sb, pool, "date");
    }

  if (changed_paths)
    {
      apr_hash_index_t *hi;
      char *path;

      /* <paths> */
      svn_xml_make_open_tag (&sb, pool, svn_xml_normal, "paths",
                             NULL);
      
      for (hi = apr_hash_first (pool, changed_paths);
           hi != NULL;
           hi = apr_hash_next (hi))
        {
          void *val;
          char action[2];
          svn_log_changed_path_t *log_item;
          
          apr_hash_this(hi, (void *) &path, NULL, &val);
          log_item = val;

          action[0] = log_item->action;
          action[1] = '\0';
          if (log_item->copyfrom_path
              && SVN_IS_VALID_REVNUM (log_item->copyfrom_rev))
            {
              /* <path action="X" copyfrom-path="aaa" copyfrom-rev="> */
              svn_stringbuf_t *escpath = svn_stringbuf_create ("", pool);
              svn_xml_escape_attr_cstring (&escpath,
                                           log_item->copyfrom_path, pool);
              revstr = apr_psprintf (pool, "%" SVN_REVNUM_T_FMT, 
                                     log_item->copyfrom_rev);
              svn_xml_make_open_tag (&sb, pool, svn_xml_protect_pcdata, "path",
                                     "action", action,
                                     "copyfrom-path", escpath->data,
                                     "copyfrom-rev", revstr, NULL);
            }
          else
            {
              /* <path action="X"> */
              svn_xml_make_open_tag (&sb, pool, svn_xml_protect_pcdata, "path",
                                     "action", action, NULL);
            }
          /* xxx</path> */
          svn_xml_escape_cdata_cstring (&sb, path, pool);
          svn_xml_make_close_tag (&sb, pool, "path");
        }

      /* </paths> */
      svn_xml_make_close_tag (&sb, pool, "paths");
    }

  if (! lb->omit_log_message)
    {
      if (msg == NULL)
        msg = "";

      /* <msg>xxx</msg> */
      svn_xml_make_open_tag (&sb, pool, svn_xml_protect_pcdata, "msg", NULL);
      SVN_ERR (svn_subst_translate_cstring (msg, &msg_native_eol,
                                            APR_EOL_STR, /* the 'native' eol */
                                            FALSE,       /* no need to repair */
                                            NULL,        /* no keywords */
                                            FALSE,       /* no expansion */
                                            pool));
      svn_xml_escape_cdata_cstring (&sb, msg_native_eol, pool);
      svn_xml_make_close_tag (&sb, pool, "msg");
    }

  /* </logentry> */
  svn_xml_make_close_tag (&sb, pool, "logentry");

  SVN_ERR (svn_stream_printf (lb->out, pool, "%s", sb->data));

  return SVN_NO_ERROR;
}


/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__log (apr_getopt_t *os,
             void *baton,
             apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  apr_array_header_t *targets;
  struct log_receiver_baton lb;

  SVN_ERR (svn_opt_args_to_target_array (&targets, os, 
                                         opt_state->targets,
                                         &(opt_state->start_revision),
                                         &(opt_state->end_revision),
                                         FALSE, pool));

  /* Add "." if user passed 0 arguments */
  svn_opt_push_implicit_dot_target(targets, pool);

  if ((opt_state->start_revision.kind != svn_opt_revision_unspecified)
      && (opt_state->end_revision.kind == svn_opt_revision_unspecified))
    {
      /* If the user specified exactly one revision, then start rev is
         set but end is not.  We show the log message for just that
         revision by making end equal to start.

         Note that if the user requested a single dated revision, then
         this will cause the same date to be resolved twice.  The
         extra code complexity to get around this slight inefficiency
         doesn't seem worth it, however.  */

      opt_state->end_revision = opt_state->start_revision;
    }
  else if (opt_state->start_revision.kind == svn_opt_revision_unspecified)
    {
      const char *target = APR_ARRAY_IDX (targets, 0, const char *);

      /* If the first target is a URL, then we default to HEAD:1.
         Otherwise, the default is BASE:1 since WC@HEAD may not exist. */
      if (svn_path_is_url (target))
        opt_state->start_revision.kind = svn_opt_revision_head;
      else
        opt_state->start_revision.kind = svn_opt_revision_base;

      if (opt_state->end_revision.kind == svn_opt_revision_unspecified)
        {
          opt_state->end_revision.kind = svn_opt_revision_number;
          opt_state->end_revision.value.number = 1;  /* oldest commit */
        }
    }

  lb.cancel_func = ctx->cancel_func;
  lb.cancel_baton = ctx->cancel_baton;
  lb.omit_log_message = opt_state->quiet;
  SVN_ERR (svn_stream_for_stdout (&lb.out, pool));

  if (! opt_state->quiet)
    svn_cl__get_notifier (&ctx->notify_func, &ctx->notify_baton, FALSE, FALSE,
                          FALSE, pool);

  if (opt_state->xml)
    {
      /* If output is not incremental, output the XML header and wrap
         everything in a top-level element. This makes the output in
         its entirety a well-formed XML document. */
      if (! opt_state->incremental)
        {
          svn_stringbuf_t *sb = svn_stringbuf_create ("", pool);

          /* <?xml version="1.0" encoding="utf-8"?> */
          svn_xml_make_header (&sb, pool);
          
          /* "<log>" */
          svn_xml_make_open_tag (&sb, pool, svn_xml_normal, "log", NULL);

          SVN_ERR (svn_stream_printf (lb.out, pool, "%s", sb->data));
        }
      
      SVN_ERR (svn_client_log (targets,
                               &(opt_state->start_revision),
                               &(opt_state->end_revision),
                               opt_state->verbose,
                               opt_state->stop_on_copy,
                               log_message_receiver_xml,
                               &lb,
                               ctx,
                               pool));
      
      if (! opt_state->incremental)
        {
          svn_stringbuf_t *sb = svn_stringbuf_create ("", pool);

          /* "</log>" */
          svn_xml_make_close_tag (&sb, pool, "log");

          SVN_ERR (svn_stream_printf (lb.out, pool, "%s", sb->data));
        }
    }
  else  /* default output format */
    {
      /* ### Ideally, we'd also pass the `quiet' flag through to the
       * repository code, so we wouldn't waste bandwith sending the
       * log message bodies back only to have the client ignore them.
       * However, that's an implementation detail; as far as the user
       * is concerned, the result of 'svn log --quiet' is the same
       * either way.
       */
      SVN_ERR (svn_client_log (targets,
                               &(opt_state->start_revision),
                               &(opt_state->end_revision),
                               opt_state->verbose,
                               opt_state->stop_on_copy,
                               log_message_receiver,
                               &lb,
                               ctx,
                               pool));

      if (! opt_state->incremental)
        SVN_ERR (svn_stream_printf (lb.out, pool, SEP_STRING));
    }

  return SVN_NO_ERROR;
}
