/*
 * log.c :  routines for requesting and parsing log reports
 *
 * ====================================================================
 * Copyright (c) 2000-2007 CollabNet.  All rights reserved.
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



#define APR_WANT_STRFUNC
#include <apr_want.h> /* for strcmp() */

#include <apr_pools.h>
#include <apr_tables.h>
#include <apr_strings.h>
#include <apr_xml.h>

#include "svn_error.h"
#include "svn_pools.h"
#include "svn_path.h"
#include "svn_xml.h"
#include "../libsvn_ra/ra_loader.h"

#include "ra_dav.h"



/*** Code ***/

/* Userdata for the Neon XML element callbacks. */
struct log_baton
{
  /*WARNING: WANT_CDATA should stay the first element in the baton:
    svn_ra_dav__xml_collect_cdata() assumes the baton starts with a stringbuf.
  */
  svn_stringbuf_t *want_cdata;
  svn_stringbuf_t *cdata;
  /* Allocate log message information.
   * NOTE: this pool may be cleared multiple times as log messages are
   * received.
   */
  apr_pool_t *subpool;

  /* Information about each log item in turn. */
  svn_log_entry_t *log_entry;

  /* The current changed path item. */
  svn_log_changed_path_t *this_path_item;

  /* Client's callback, invoked on the above fields when the end of an
     item is seen. */
  svn_log_message_receiver2_t receiver;
  void *receiver_baton;

  int limit;
  int count;

  /* If we're in backwards compatibility mode for the svn log --limit
     stuff, we need to be able to bail out while parsing log messages.
     The way we do that is returning an error to neon, but we need to
     be able to tell that the error we returned wasn't actually a
     problem, so if this is TRUE it means we can safely ignore that
     error and return success. */
  svn_boolean_t limit_compat_bailout;
};


/* Prepare LB to start accumulating the next log item, by wiping all
 * information related to the previous item and clearing the pool in
 * which they were allocated.  Do not touch any stored error, however.
 */
static void
reset_log_item(struct log_baton *lb)
{
  lb->log_entry->revision      = SVN_INVALID_REVNUM;
  lb->log_entry->author        = NULL;
  lb->log_entry->date          = NULL;
  lb->log_entry->message       = NULL;
  lb->log_entry->changed_paths = NULL;

  svn_pool_clear(lb->subpool);
}


/*
 * This implements the `svn_ra_dav__xml_startelm_cb' prototype.
 */
static svn_error_t *
log_start_element(int *elem, void *baton, int parent,
                  const char *nspace, const char *name, const char **atts)
{
  const char *copyfrom_path, *copyfrom_revstr;
  svn_revnum_t copyfrom_rev;
  struct log_baton *lb = baton;
  static const svn_ra_dav__xml_elm_t log_report_elements[] =
    {
      { SVN_XML_NAMESPACE, "log-report", ELEM_log_report, 0 },
      { SVN_XML_NAMESPACE, "log-item", ELEM_log_item, 0 },
      { SVN_XML_NAMESPACE, "date", ELEM_log_date, SVN_RA_DAV__XML_CDATA },
      { SVN_XML_NAMESPACE, "added-path", ELEM_added_path,
        SVN_RA_DAV__XML_CDATA },
      { SVN_XML_NAMESPACE, "deleted-path", ELEM_deleted_path,
        SVN_RA_DAV__XML_CDATA },
      { SVN_XML_NAMESPACE, "modified-path", ELEM_modified_path,
        SVN_RA_DAV__XML_CDATA },
      { SVN_XML_NAMESPACE, "replaced-path", ELEM_replaced_path,
        SVN_RA_DAV__XML_CDATA },
      { "DAV:", "version-name", ELEM_version_name, SVN_RA_DAV__XML_CDATA },
      { "DAV:", "creator-displayname", ELEM_creator_displayname,
        SVN_RA_DAV__XML_CDATA },
      { "DAV:", "comment", ELEM_comment, SVN_RA_DAV__XML_CDATA },
      { SVN_XML_NAMESPACE, "nbr-children", ELEM_nbr_children,
        SVN_RA_DAV__XML_CDATA },
      { NULL }
    };
  const svn_ra_dav__xml_elm_t *elm
    = svn_ra_dav__lookup_xml_elem(log_report_elements, nspace, name);

  *elem = elm ? elm->id : SVN_RA_DAV__XML_DECLINE;
  if (!elm)
    return SVN_NO_ERROR;

  switch (elm->id)
    {
    case ELEM_creator_displayname:
    case ELEM_log_date:
    case ELEM_version_name:
    case ELEM_added_path:
    case ELEM_replaced_path:
    case ELEM_deleted_path:
    case ELEM_modified_path:
    case ELEM_comment:
    case ELEM_nbr_children:
      lb->want_cdata = lb->cdata;
      svn_stringbuf_setempty(lb->cdata);
      break;

    default:
      lb->want_cdata = NULL;
      break;
    }

  switch (elm->id)
    {
    case ELEM_added_path:
    case ELEM_replaced_path:
    case ELEM_deleted_path:
    case ELEM_modified_path:
      lb->this_path_item = apr_pcalloc(lb->subpool, 
                                       sizeof(*(lb->this_path_item)));
      lb->this_path_item->copyfrom_rev = SVN_INVALID_REVNUM;

      /* See documentation for `svn_repos_node_t' in svn_repos.h,
         and `svn_log_message_receiver_t' in svn_types.h, for more
         about these action codes. */
      if ((elm->id == ELEM_added_path) || (elm->id == ELEM_replaced_path))
        {
          lb->this_path_item->action 
            = (elm->id == ELEM_added_path) ? 'A' : 'R';
          copyfrom_path = svn_xml_get_attr_value("copyfrom-path", atts);
          copyfrom_revstr = svn_xml_get_attr_value("copyfrom-rev", atts);
          if (copyfrom_path && copyfrom_revstr
              && (SVN_IS_VALID_REVNUM
                  (copyfrom_rev = SVN_STR_TO_REV(copyfrom_revstr))))
            {
              lb->this_path_item->copyfrom_path = apr_pstrdup(lb->subpool,
                                                              copyfrom_path);
              lb->this_path_item->copyfrom_rev = copyfrom_rev;
            }
        }
      else if (elm->id == ELEM_deleted_path)
        {
          lb->this_path_item->action = 'D';
        }
      else
        {
          lb->this_path_item->action = 'M';
        }
      break;

    default:
      lb->this_path_item = NULL;
      break;
    }
  return SVN_NO_ERROR;
}


/*
 * This implements the `svn_ra_dav__xml_endelm_cb' prototype.
 */
static svn_error_t *
log_end_element(void *baton, int state,
                const char *nspace, const char *name)
{
  struct log_baton *lb = baton;

  switch (state)
    {
    case ELEM_version_name:
      lb->log_entry->revision = SVN_STR_TO_REV(lb->cdata->data);
      break;
    case ELEM_nbr_children:
      lb->log_entry->nbr_children = atol(lb->cdata->data);
      break;
    case ELEM_creator_displayname:
      lb->log_entry->author = apr_pstrdup(lb->subpool, lb->cdata->data);
      break;
    case ELEM_log_date:
      lb->log_entry->date = apr_pstrdup(lb->subpool, lb->cdata->data);
      break;
    case ELEM_added_path:
    case ELEM_replaced_path:
    case ELEM_deleted_path:
    case ELEM_modified_path:
      {
        char *path = apr_pstrdup(lb->subpool, lb->cdata->data);
        if (! lb->log_entry->changed_paths)
          lb->log_entry->changed_paths = apr_hash_make(lb->subpool);
        apr_hash_set(lb->log_entry->changed_paths, path, APR_HASH_KEY_STRING, 
                     lb->this_path_item);
        break;
      }
    case ELEM_comment:
      lb->log_entry->message = apr_pstrdup(lb->subpool, lb->cdata->data);
      break;
    case ELEM_log_item:
      {
        /* Compatability cruft so that we can provide limit functionality 
           even if the server doesn't support it.

           If we've seen as many log entries as we're going to show just
           error out of the XML parser so we can avoid having to parse the
           remaining XML, but set lb->err to SVN_NO_ERROR so no error will
           end up being shown to the user. */
        if (lb->limit && (++lb->count > lb->limit))
          {
            lb->limit_compat_bailout = TRUE;
            return svn_error_create(APR_EGENERAL, NULL, NULL);
          }
 
        SVN_ERR((*(lb->receiver))(lb->receiver_baton,
                                  lb->log_entry,
                                  lb->subpool));
        reset_log_item(lb);
      }
      break;
    case ELEM_log_report:
      {
        /* Do nothing.  But...
         *
         * ### Possibility:
         *
         * Greg Stein mused that we could treat log_receivers the way
         * we treat delta window consumers -- "no more calls" would be
         * indicated by a special last call that passes
         * SVN_INVALID_REVNUM as the revision number.  That would work
         * fine, but right now most of the code just handles the
         * first-call/last-call thing by having one-time code on
         * either side of the iterator, which works just as well.
         *
         * I don't feel any compelling need to change this right now.
         * If we do change it, the hot spots are:
         *
         *    - libsvn_repos/log.c:
         *         svn_repos_get_logs() would need a new post-loop
         *         call to (*receiver)(), passing SVN_INVALID_REVNUM.
         *         Make sure not to destroy that subpool until
         *         after the new call! :-)
         *
         *    - mod_dav_svn/log.c:
         *        `struct log_receiver_baton' would need a first_call
         *         flag; dav_svn__log_report() would set it up, and
         *         then log_receiver() would be responsible for
         *         emitting "<S:log-report>" and "</S:log-report>"
         *         instead.
         *
         *    - svn/log-cmd.c:
         *         svn_cl__log() would no longer be responsible for
         *         emitting the "<log>" and "</log>" elements.  The
         *         body of this function would get a lot simpler, mmm!
         *         Instead, log_message_receiver_xml() would pay
         *         attention to baton->first_call, and handle
         *         SVN_INVALID_REVNUM, to emit those elements
         *         instead.  The old log_message_receiver() function
         *         wouldn't need to change at all, though, I think.
         *
         *    - Right here:
         *      We'd have a new call to (*(lb->receiver)), passing
         *      SVN_INVALID_REVNUM, of course.
         *
         * There, I think that's the change.  Thoughts? :-)
         */
      }
      break;
    }

  /* Stop collecting cdata */
  lb->want_cdata = NULL;
  return SVN_NO_ERROR;
}


svn_error_t * svn_ra_dav__get_log(svn_ra_session_t *session,
                                  const apr_array_header_t *paths,
                                  svn_revnum_t start,
                                  svn_revnum_t end,
                                  int limit,
                                  svn_boolean_t discover_changed_paths,
                                  svn_boolean_t strict_node_history,
                                  svn_boolean_t include_merged_revisions,
                                  svn_log_message_receiver2_t receiver,
                                  void *receiver_baton,
                                  apr_pool_t *pool)
{
  /* The Plan: Send a request to the server for a log report.
   * Somewhere in mod_dav_svn, there will be an implementation, R, of
   * the `svn_log_message_receiver2_t' function type.  Some other
   * function in mod_dav_svn will use svn_repos_get_logs() to loop R
   * over the log messages, and the successive invocations of R will
   * collectively transmit the report back here, where we parse the
   * report and invoke RECEIVER (which is an entirely separate
   * instance of `svn_log_message_receiver2_t') on each individual
   * message in that report.
   */

  int i;
  svn_ra_dav__session_t *ras = session->priv;
  svn_stringbuf_t *request_body = svn_stringbuf_create("", pool);
  struct log_baton lb;
  svn_string_t bc_url, bc_relative;
  const char *final_bc_url;
  svn_revnum_t use_rev;
  svn_error_t *err;

  /* ### todo: I don't understand why the static, file-global
     variables shared by update and status are called `report_head'
     and `report_tail', instead of `request_head' and `request_tail'.
     Maybe Greg can explain?  Meanwhile, I'm tentatively using
     "request_*" for my local vars below. */

  static const char log_request_head[]
    = "<S:log-report xmlns:S=\"" SVN_XML_NAMESPACE "\">" DEBUG_CR;

  static const char log_request_tail[] = "</S:log-report>" DEBUG_CR;

  /* Construct the request body. */
  svn_stringbuf_appendcstr(request_body, log_request_head);
  svn_stringbuf_appendcstr(request_body,
                           apr_psprintf(pool,
                                        "<S:start-revision>%ld"
                                        "</S:start-revision>", start));
  svn_stringbuf_appendcstr(request_body,
                           apr_psprintf(pool,
                                        "<S:end-revision>%ld"
                                        "</S:end-revision>", end));
  if (limit)
    {
      svn_stringbuf_appendcstr(request_body,
                               apr_psprintf(pool,
                                            "<S:limit>%d</S:limit>", limit));
    }

  if (discover_changed_paths)
    {
      svn_stringbuf_appendcstr(request_body,
                               apr_psprintf(pool,
                                            "<S:discover-changed-paths/>"));
    }

  if (strict_node_history)
    {
      svn_stringbuf_appendcstr(request_body,
                               apr_psprintf(pool,
                                            "<S:strict-node-history/>"));
    }

  if (include_merged_revisions)
    {
      svn_stringbuf_appendcstr(request_body,
                               apr_psprintf(pool,
                                            "<S:include-merged-revisions/>"));
    }

  if (paths)
    {
      for (i = 0; i < paths->nelts; i++)
        {
          const char *this_path =
            apr_xml_quote_string(pool,
                                 APR_ARRAY_IDX(paths, i, const char *),
                                 0);
          svn_stringbuf_appendcstr(request_body, "<S:path>");
          svn_stringbuf_appendcstr(request_body, this_path);
          svn_stringbuf_appendcstr(request_body, "</S:path>");
        }
    }

  svn_stringbuf_appendcstr(request_body, log_request_tail);

  lb.receiver = receiver;
  lb.receiver_baton = receiver_baton;
  lb.subpool = svn_pool_create(pool);
  lb.limit = limit;
  lb.count = 0;
  lb.limit_compat_bailout = FALSE;
  lb.cdata = svn_stringbuf_create("", pool);
  lb.log_entry = svn_log_entry_create(pool);
  lb.want_cdata = NULL;
  reset_log_item(&lb);

  /* ras's URL may not exist in HEAD, and thus it's not safe to send
     it as the main argument to the REPORT request; it might cause
     dav_get_resource() to choke on the server.  So instead, we pass a
     baseline-collection URL, which we get from the largest of the
     START and END revisions. */
  use_rev = (start > end) ? start : end;
  SVN_ERR(svn_ra_dav__get_baseline_info(NULL, &bc_url, &bc_relative, NULL,
                                        ras, ras->url->data, use_rev,
                                        pool));
  final_bc_url = svn_path_url_add_component(bc_url.data, bc_relative.data,
                                            pool);


  err = svn_ra_dav__parsed_request(ras,
                                   "REPORT",
                                   final_bc_url,
                                   request_body->data,
                                   0,  /* ignored */
                                   NULL,
                                   log_start_element,
                                   svn_ra_dav__xml_collect_cdata,
                                   log_end_element,
                                   &lb,
                                   NULL,
                                   NULL,
                                   FALSE,
                                   pool);
  svn_pool_destroy(lb.subpool);

  if (err && lb.limit_compat_bailout)
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }

  return err;
}
