/*
 * file_revs.c :  routines for requesting and parsing file-revs reports
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



#define APR_WANT_STRFUNC
#include <apr_want.h> /* for strcmp() */

#include <apr_tables.h>
#include <apr_strings.h>
#include <apr_xml.h>

#include <ne_socket.h>
#include <ne_utils.h>
#include <ne_xml.h>

#include "svn_error.h"
#include "svn_pools.h"
#include "svn_delta.h"
#include "svn_io.h"
#include "svn_path.h"
#include "svn_xml.h"
#include "svn_base64.h"
#include "svn_props.h"
#include "../libsvn_ra/ra_loader.h"
#include "svn_private_config.h"

#include "ra_dav.h"



/*** Code ***/

struct report_baton {
  /* From the caller. */
  svn_ra_file_rev_handler_t handler;
  void *handler_baton;

  /* Arguments for the callback. */
  const char *path;
  svn_revnum_t revnum;
  apr_hash_t *rev_props;
  apr_array_header_t *prop_diffs;

  /* The current property name. */
  const char *prop_name;

  /* Is the current property encoded in base64? */
  svn_boolean_t base64_prop;

  /* Buffer for accumulating CDATA for prop values. */
  svn_stringbuf_t *cdata_accum;

  /* Stream for writing text delta to. */
  svn_stream_t *stream;

  svn_boolean_t had_txdelta;  /* Did we have a txdelta in this file-rev elem? */

  /* Error from parse callback. */
  svn_error_t *err;
  apr_pool_t *subpool;
};

/* Prepare RB for a new revision. */
static void
reset_file_rev (struct report_baton *rb)
{
  svn_pool_clear (rb->subpool);
  rb->path = NULL;
  rb->revnum = SVN_INVALID_REVNUM;
  rb->rev_props = apr_hash_make (rb->subpool);
  rb->prop_diffs = apr_array_make (rb->subpool, 0, sizeof (svn_prop_t));
  rb->had_txdelta = FALSE;
  /* Just in case... */
  rb->stream = NULL;
}


/* Our beloved elements. */
static const svn_ra_dav__xml_elm_t report_elements[] =
  {
    { SVN_XML_NAMESPACE, "file-revs-report", ELEM_file_revs_report, 0 },
    { SVN_XML_NAMESPACE, "file-rev", ELEM_file_rev, 0 },
    { SVN_XML_NAMESPACE, "rev-prop", ELEM_rev_prop, 0 },
    { SVN_XML_NAMESPACE, "set-prop", ELEM_set_prop, 0 },
    { SVN_XML_NAMESPACE, "remove-prop", ELEM_remove_prop, 0 },
    { SVN_XML_NAMESPACE, "txdelta", ELEM_txdelta, 0 },
    { NULL }
  };

/* If an error occurs, store it and abort the XML parse. */
#define CHKERR(e)               \
do {                            \
  if ((rb->err = (e)) != NULL)  \
    return NE_XML_ABORT;        \
} while(0)


/* This implements the `ne_xml_startelm_cb' prototype. */
static int
start_element (void *userdata, int parent_state, const char *ns,
               const char *ln, const char **atts)
{
  struct report_baton *rb = userdata;
  const svn_ra_dav__xml_elm_t *elm;
  const char *att;

  elm = svn_ra_dav__lookup_xml_elem (report_elements, ns, ln);

  /* Skip unknown elements. */
  if (!elm)
    return NE_XML_DECLINE;

  switch (parent_state)
    {
    case ELEM_root:
      if (elm->id != ELEM_file_revs_report)
        return NE_XML_ABORT;
      break;

    case ELEM_file_revs_report:
      if (elm->id == ELEM_file_rev)
        {
          reset_file_rev (rb);
          att = svn_xml_get_attr_value ("rev", atts);
          if (!att)
            return NE_XML_ABORT;
          rb->revnum = SVN_STR_TO_REV (att);
          att = svn_xml_get_attr_value ("path", atts);
          if (!att)
            return NE_XML_ABORT;
          rb->path = apr_pstrdup (rb->subpool, att);
        }
      else
        return NE_XML_ABORT;
      break;

    case ELEM_file_rev:
      /* txdelta must be the last elem in file-rev. */
      if (rb->had_txdelta)
        return NE_XML_ABORT;
      switch (elm->id)
        {
        case ELEM_rev_prop:
        case ELEM_set_prop:
          att = svn_xml_get_attr_value ("name", atts);
          if (!att)
            return NE_XML_ABORT;
          rb->prop_name = apr_pstrdup (rb->subpool, att);
          att = svn_xml_get_attr_value ("encoding", atts);
          if (att && strcmp (att, "base64") == 0)
            rb->base64_prop = TRUE;
          else
            rb->base64_prop = FALSE;
          break;
        case ELEM_remove_prop:
          {
            svn_prop_t *prop = apr_array_push (rb->prop_diffs);
            att = svn_xml_get_attr_value ("name", atts);
            if (!att || *att == '\0')
              return NE_XML_ABORT;
            prop->name = apr_pstrdup (rb->subpool, att);
            prop->value = NULL;
          }
          break;
        case ELEM_txdelta:
          {
            svn_txdelta_window_handler_t whandler = NULL;
            void *wbaton;
            /* It's time to call our hanlder. */
            CHKERR (rb->handler (rb->handler_baton, rb->path, rb->revnum,
                                 rb->rev_props, &whandler, &wbaton,
                                 rb->prop_diffs, rb->subpool));
            if (whandler)
              rb->stream = svn_base64_decode
                (svn_txdelta_parse_svndiff (whandler, wbaton, TRUE,
                                            rb->subpool), rb->subpool);
          }
          break;
        default:
          return NE_XML_ABORT;
        }
      break;
    default:
      return NE_XML_ABORT;
    }

  return elm->id;
}

/* Extract the property value from RB, possibly base64-decoding it.
   Resets RB->cdata_accum. */
static const svn_string_t *
extract_propval (struct report_baton *rb)
{
  const svn_string_t *v = svn_string_create_from_buf (rb->cdata_accum,
                                                      rb->subpool);
  svn_stringbuf_setempty (rb->cdata_accum);
  if (rb->base64_prop)
    return svn_base64_decode_string (v, rb->subpool);
  else
    return v;
}

/* This implements the `ne_xml_endelm_cb' prototype. */
static int
end_element (void *userdata, int state,
             const char *nspace, const char *elt_name)
{
  struct report_baton *rb = userdata;

  switch (state)
    {
    case ELEM_file_rev:
      /* If we had no txdelta, we call the handler here, informing it that
         there were no content changes. */
      if (!rb->had_txdelta)
        CHKERR (rb->handler (rb->handler_baton, rb->path, rb->revnum,
                             rb->rev_props, NULL, NULL, rb->prop_diffs,
                             rb->subpool));
      break;

    case ELEM_rev_prop:
      apr_hash_set (rb->rev_props, rb->prop_name, APR_HASH_KEY_STRING,
                    extract_propval (rb));
      break;

    case ELEM_set_prop:
      {
        svn_prop_t *prop = apr_array_push (rb->prop_diffs);
        prop->name = rb->prop_name;
        prop->value = extract_propval (rb);
        break;
      }

    case ELEM_txdelta:
      if (rb->stream)
        {
          CHKERR (svn_stream_close (rb->stream));
          rb->stream = NULL;
        }
      rb->had_txdelta = TRUE;
      break;
    }
  return 0;
}

/* This implements the `ne_xml_cdata_cb' prototype. */
static int
cdata_handler (void *userdata, int state,
               const char *cdata, size_t len)
{
  struct report_baton *rb = userdata;

  switch (state)
    {
    case ELEM_rev_prop:
    case ELEM_set_prop:
      svn_stringbuf_appendbytes (rb->cdata_accum, cdata, len);
      break;
    case ELEM_txdelta:
      if (rb->stream)
        {
          apr_size_t l = len;
          CHKERR (svn_stream_write (rb->stream, cdata, &l));
          if (l != len)
            return NE_XML_ABORT;
        }

      /* In other cases, we just ingore the CDATA. */
    }

  return 0;
}

#undef CHKERR

svn_error_t *
svn_ra_dav__get_file_revs (svn_ra_session_t *session,
                           const char *path,
                           svn_revnum_t start,
                           svn_revnum_t end,
                           svn_ra_file_rev_handler_t handler,
                           void *handler_baton,
                           apr_pool_t *pool)
{
  svn_ra_dav__session_t *ras = session->priv;
  svn_stringbuf_t *request_body = svn_stringbuf_create ("", ras->pool);
  svn_string_t bc_url, bc_relative;
  const char *final_bc_url;
  int http_status = 0;
  struct report_baton rb;
  svn_error_t *err;

  static const char request_head[]
    = "<S:file-revs-report xmlns:S=\"" SVN_XML_NAMESPACE "\">" DEBUG_CR;
  static const char request_tail[]
    = "</S:file-revs-report>";

  /* Construct request body. */
  svn_stringbuf_appendcstr (request_body, request_head);
  svn_stringbuf_appendcstr (request_body,
                            apr_psprintf (ras->pool,
                                          "<S:start-revision>%ld"
                                          "</S:start-revision>", start));
  svn_stringbuf_appendcstr (request_body,
                            apr_psprintf (ras->pool,
                                          "<S:end-revision>%ld"
                                          "</S:end-revision>", end));
  svn_stringbuf_appendcstr (request_body, "<S:path>");
  svn_stringbuf_appendcstr (request_body,
                            apr_xml_quote_string (ras->pool, path, 0));
  svn_stringbuf_appendcstr (request_body, "</S:path>");
  svn_stringbuf_appendcstr (request_body, request_tail);

  /* Initialize the baton. */
  rb.handler = handler;
  rb.handler_baton = handler_baton;
  rb.cdata_accum = svn_stringbuf_create ("", pool);
  rb.err = NULL;
  rb.subpool = svn_pool_create (pool);
  reset_file_rev (&rb);

  /* ras's URL may not exist in HEAD, and thus it's not safe to send
     it as the main argument to the REPORT request; it might cause
     dav_get_resource() to choke on the server.  So instead, we pass a
     baseline-collection URL, which we get from END. */
  SVN_ERR (svn_ra_dav__get_baseline_info (NULL, &bc_url, &bc_relative, NULL,
                                          ras->sess, ras->url, end,
                                          ras->pool));
  final_bc_url = svn_path_url_add_component (bc_url.data, bc_relative.data,
                                             ras->pool);

  /* Dispatch the request. */
  err = svn_ra_dav__parsed_request (ras->sess, "REPORT", final_bc_url,
                                    request_body->data, NULL, NULL,
                                    start_element, cdata_handler, end_element,
                                    &rb, NULL, &http_status, ras->pool);

  /* Map status 501: Method Not Implemented to our not implemented error.
     1.0.x servers and older don't support this report. */
  if (http_status == 501)
    return svn_error_create (SVN_ERR_RA_NOT_IMPLEMENTED, err,
                             _("get-file-revs REPORT not implemented"));
  SVN_ERR (err);
  SVN_ERR (rb.err);

  /* Caller expects at least one revision.  Signal error otherwise. */
  if (!SVN_IS_VALID_REVNUM(rb.revnum))
    return svn_error_create (SVN_ERR_RA_DAV_REQUEST_FAILED, NULL,
                             _("The file-revs report didn't contain any "
                               "revisions"));

  svn_pool_destroy (rb.subpool);

  return SVN_NO_ERROR;
}
