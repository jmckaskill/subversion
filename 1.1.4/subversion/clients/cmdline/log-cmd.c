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
#define APR_WANT_STDIO
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
#include "svn_ebcdic.h"
#include "cl.h"

#include "svn_private_config.h"


#define ACTION_STR \
        "\x61\x63\x74\x69\x6f\x6e"
        /* "action" */

#define AUTHOR_STR \
        "\x61\x75\x74\x68\x6f\x72"
        /* "author" */

#define COPYFROM_PATH_STR \
        "\x63\x6f\x70\x79\x66\x72\x6f\x6d\x2d\x70\x61\x74\x68"
        /* "copyfrom-path" */

#define COPYFROM_REV_STR \
        "\x63\x6f\x70\x79\x66\x72\x6f\x6d\x2d\x72\x65\x76"
        /* "copyfrom-rev" */

#define DATE_STR \
        "\x64\x61\x74\x65"
        /* "date" */

#define LOG_STR \
        "\x6c\x6f\x67"
        /* "logentry" */
        
#define LOGENTRY_STR \
        "\x6c\x6f\x67\x65\x6e\x74\x72\x79"
        /* "logentry" */

#define MSG_STR \
        "\x6d\x73\x67"
        /* "msg" */

#define PATH_STR \
        "\x70\x61\x74\x68"
        /* "path" */

#define PATHS_STR \
        "\x70\x61\x74\x68\x73"
        /* "paths" */

#define REVISION_STR \
        "\x72\x65\x76\x69\x73\x69\x6f\x6e"
        /* "revision" */ 

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
      if (*p == SVN_UTF8_NEWLINE)
        {
          count++;
          if (*(p + 1) == SVN_UTF8_CR)
            p++;
        }
      else if (*p == SVN_UTF8_CR)
        {
          count++;
          if (*(p + 1) == SVN_UTF8_NEWLINE)
            p++;
        }
    }

  return count;
}

static svn_error_t *
error_checked_fputs(const char *string, FILE* stream)
{
  /* This function is equal to svn_cmdline_fputs() minus
     the utf8->local encoding translation */

  /* On POSIX systems, errno will be set on an error in fputs, but this might
     not be the case on other platforms.  We reset errno and only
     use it if it was set by the below fputs call.  Else, we just return
     a generic error. */
  errno = 0;

  if (fputs (string, stream) == EOF)
    {
      if (errno)
        return svn_error_wrap_apr (errno, _("Write error"));
      else
        return svn_error_create (SVN_ERR_IO_WRITE_ERROR, NULL, NULL);
    }

  return SVN_NO_ERROR;
}

/* Baton for log_message_receiver() and log_message_receiver_xml(). */
struct log_receiver_baton
{
  /* Check for cancellation on each invocation of a log receiver. */
  svn_cancel_func_t cancel_func;
  void *cancel_baton;

  /* Don't print log message body nor its line count. */
  svn_boolean_t omit_log_message;
};


/* The separator between log messages. */
#define SEP_STRING \
  "------------------------------------------------------------------------\n"


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

  /* Number of lines in the msg. */
  int lines;

  if (lb->cancel_func)
    SVN_ERR (lb->cancel_func (lb->cancel_baton));

  if (rev == 0)
    return svn_cmdline_printf (pool, _("No commit for revision 0.\n"));

  /* ### See http://subversion.tigris.org/issues/show_bug.cgi?id=807
     for more on the fallback fuzzy conversions below. */

  if (author == NULL)
    author = _("(no author)");
#if APR_CHARSET_EBCDIC
  else
    SVN_ERR (svn_utf_cstring_from_utf8 (&author, author, pool));
#endif    

  if (date && date[0])
    {
      /* Convert date to a format for humans. */
      apr_time_t time_temp;
      
      SVN_ERR (svn_time_from_cstring (&time_temp, date, pool));
      date = svn_time_to_human_cstring(time_temp, pool);
#if APR_CHARSET_EBCDIC
      SVN_ERR (svn_utf_cstring_from_utf8 (&date, date, pool));
#endif       
    }
  else
    date = _("(no date)");

  if (! lb->omit_log_message && msg == NULL)
    msg = "";

  SVN_ERR (svn_cmdline_printf (pool,
                               SEP_STRING "r%ld | %s | %s",
                               rev, author, date));

  if (! lb->omit_log_message)
    {
      lines = num_lines (msg);
      /*### FIXME: how do we translate this without ngettext?! */
      SVN_ERR (svn_cmdline_printf (pool,
                                   " | %d line%s", lines,
                                   (lines > 1) ? "s" : ""));
    }

  SVN_ERR (svn_cmdline_printf (pool, "\n"));

  if (changed_paths)
    {
      apr_array_header_t *sorted_paths;
      int i;

      /* Get an array of sorted hash keys. */
      sorted_paths = svn_sort__hash (changed_paths,
                                     svn_sort_compare_items_as_paths, pool);

      SVN_ERR (svn_cmdline_printf (pool,
                                   _("Changed paths:\n")));
      for (i = 0; i < sorted_paths->nelts; i++)
        {
          char *action_utf8;
          char action[2] = {'\0', '\0'};
          svn_sort__item_t *item = &(APR_ARRAY_IDX (sorted_paths, i,
                                                    svn_sort__item_t));
          const char *path = item->key;
          svn_log_changed_path_t *log_item 
            = apr_hash_get (changed_paths, item->key, item->klen);
          const char *copy_data = "";
          
          if (log_item->copyfrom_path 
              && SVN_IS_VALID_REVNUM (log_item->copyfrom_rev))
            {
              copy_data 
                = APR_PSPRINTF2 (pool, 
                                 _(" (from %s:%ld)"),
                                 log_item->copyfrom_path,
                                 log_item->copyfrom_rev);
            }
          SVN_ERR (SVN_CMDLINE_PRINTF (pool, "   %c %s%s\n",
                                       log_item->action, path,
                                       copy_data));
        }
    }

  if (! lb->omit_log_message)
    {
      SVN_ERR (svn_cmdline_printf (pool, "\n%s\n", msg));      
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

  if (lb->cancel_func)
    SVN_ERR (lb->cancel_func (lb->cancel_baton));

  if (rev == 0)
    return SVN_NO_ERROR;

  revstr = APR_PSPRINTF2 (pool, "%ld", rev);

  /* <logentry revision="xxx"> */
  svn_xml_make_open_tag (&sb, pool, svn_xml_normal, LOGENTRY_STR,
                         REVISION_STR, revstr, NULL);

  if (author)
    {
      /* <author>xxx</author> */
      svn_xml_make_open_tag (&sb, pool, svn_xml_protect_pcdata, AUTHOR_STR,
                             NULL);
      svn_xml_escape_cdata_cstring (&sb, author, pool);
      svn_xml_make_close_tag (&sb, pool, AUTHOR_STR);
    }

  if (date)
    {
      /* Print the full, uncut, date.  This is machine output. */
      /* <date>xxx</date> */
      svn_xml_make_open_tag (&sb, pool, svn_xml_protect_pcdata, DATE_STR,
                             NULL);
      svn_xml_escape_cdata_cstring (&sb, date, pool);
      svn_xml_make_close_tag (&sb, pool, DATE_STR);
    }

  if (changed_paths)
    {
      apr_hash_index_t *hi;
      char *path;

      /* <paths> */
      svn_xml_make_open_tag (&sb, pool, svn_xml_normal, PATHS_STR,
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
              revstr = APR_PSPRINTF2 (pool, "%ld", 
                                      log_item->copyfrom_rev);
              svn_xml_make_open_tag (&sb, pool, svn_xml_protect_pcdata,
                                     PATH_STR, ACTION_STR, action,
                                     COPYFROM_PATH_STR, escpath->data,
                                     COPYFROM_REV_STR, revstr, NULL);
            }
          else
            {
              /* <path action="X"> */
              svn_xml_make_open_tag (&sb, pool, svn_xml_protect_pcdata,
                                     PATH_STR, ACTION_STR, action, NULL);
            }
          /* xxx</path> */
          svn_xml_escape_cdata_cstring (&sb, path, pool);
          svn_xml_make_close_tag (&sb, pool, PATH_STR);
        }

      /* </paths> */
      svn_xml_make_close_tag (&sb, pool, PATHS_STR);
    }

  if (! lb->omit_log_message)
    {
      if (msg == NULL)
        msg = "";

      /* <msg>xxx</msg> */
      svn_xml_make_open_tag (&sb, pool, svn_xml_protect_pcdata, MSG_STR, NULL);
      svn_xml_escape_cdata_cstring (&sb, msg, pool);
      svn_xml_make_close_tag (&sb, pool, MSG_STR);
    }

  /* </logentry> */
  svn_xml_make_close_tag (&sb, pool, LOGENTRY_STR);

  SVN_ERR (error_checked_fputs (sb->data, stdout));

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
          svn_xml_make_open_tag (&sb, pool, svn_xml_normal, LOG_STR, NULL);

          SVN_ERR (error_checked_fputs (sb->data, stdout));
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
          svn_xml_make_close_tag (&sb, pool, LOG_STR);

          SVN_ERR (error_checked_fputs (sb->data, stdout));
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
        SVN_ERR (svn_cmdline_printf (pool, SEP_STRING));
    }

  return SVN_NO_ERROR;
}
