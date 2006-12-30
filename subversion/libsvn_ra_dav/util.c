/*
 * util.c :  utility functions for the RA/DAV library
 *
 * ====================================================================
 * Copyright (c) 2000-2006 CollabNet.  All rights reserved.
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

#include <apr_pools.h>

#define APR_WANT_STRFUNC
#include <apr_want.h>

#include <ne_alloc.h>
#include <ne_uri.h>
#include <ne_compress.h>
#include <ne_basic.h>

#include "svn_pools.h"
#include "svn_path.h"
#include "svn_string.h"
#include "svn_utf.h"
#include "svn_xml.h"

#include "svn_private_config.h"

#include "ra_dav.h"
#include <assert.h>



static apr_status_t
xml_parser_cleanup(void *baton)
{
  ne_xml_destroy(baton);

  return APR_SUCCESS;
}

static ne_xml_parser *
xml_parser_create(svn_ra_dav__request_t *req)
{
  ne_xml_parser *p = ne_xml_create();

  /* ### HACK: Set the parser's error to the empty string.  Someday we
     hope neon will let us have an easy way to tell the difference
     between XML parsing errors, and errors that occur while handling
     the XML tags that we get.  Until then, trust that whenever neon
     has an error somewhere below the API, it sets its own error to
     something non-empty (the API promises non-NULL, at least). */
  ne_xml_set_error(p, "");

  apr_pool_cleanup_register(req->pool, p,
                            xml_parser_cleanup,
                            apr_pool_cleanup_null);

  return p;
}



/* Simple multi-status parser
 *
 * For the purpose of 'simple' requests which - if it weren't
 * for our custom error parser - could use the ne_basic.h interfaces.
 */

static const svn_ra_dav__xml_elm_t multistatus_elements[] =
  { { "DAV:", "multistatus", ELEM_multistatus, 0 },
    { "DAV:", "response", ELEM_response, 0 },
    { "DAV:", "responsedescription", ELEM_responsedescription,
      SVN_RA_DAV__XML_CDATA },
    { "DAV:", "status", ELEM_status, SVN_RA_DAV__XML_CDATA },
    { "DAV:", "href", ELEM_href, SVN_RA_DAV__XML_CDATA },

    /* We start out basic and are not interested in propstat */
    { "", "", ELEM_unknown, 0 },

  };


static const int multistatus_nesting_table[][4] =
  { { ELEM_root, ELEM_multistatus, SVN_RA_DAV__XML_INVALID },
    { ELEM_multistatus, ELEM_response, ELEM_responsedescription,
      SVN_RA_DAV__XML_DECLINE },
    { ELEM_responsedescription, SVN_RA_DAV__XML_INVALID },
    { ELEM_response, ELEM_href, ELEM_status, SVN_RA_DAV__XML_DECLINE },
    { ELEM_status, SVN_RA_DAV__XML_INVALID },
    { ELEM_href, SVN_RA_DAV__XML_INVALID },
    { SVN_RA_DAV__XML_DECLINE },
  };


static int
validate_element(int parent, int child)
{
  int i = 0;
  int j = 0;

  while (parent != multistatus_nesting_table[i][0]
         && multistatus_nesting_table[i][0] > 0)
    i++;

  if (parent == multistatus_nesting_table[i][0])
    while (multistatus_nesting_table[i][++j] != child
           && multistatus_nesting_table[i][j] > 0)
      ;

  return multistatus_nesting_table[i][j];
}

typedef struct
{
  svn_stringbuf_t *want_cdata;
  svn_stringbuf_t *cdata;

  svn_ra_dav__request_t *req;
  svn_stringbuf_t *description;
  svn_boolean_t contains_error;
} multistatus_baton_t;

static svn_error_t *
start_207_element(int *elem, void *baton, int parent,
                  const char *nspace, const char *name, const char **atts)
{
  multistatus_baton_t *b = baton;
  const svn_ra_dav__xml_elm_t *elm =
    svn_ra_dav__lookup_xml_elem(multistatus_elements, nspace, name);
  *elem = elm ? validate_element(parent, elm->id) : SVN_RA_DAV__XML_DECLINE;


  if (*elem < 1) /* ! > 0 */
    return SVN_NO_ERROR;

  /* We're guaranteed to have ELM now: SVN_RA_DAV__XML_DECLINE < 1 */
  if (elm->flags & SVN_RA_DAV__XML_CDATA)
    {
      svn_stringbuf_setempty(b->cdata);
      b->want_cdata = b->cdata;
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
end_207_element(void *baton, int state,
                const char *nspace, const char *name)
{
  multistatus_baton_t *b = baton;

  switch (state)
    {
    case ELEM_multistatus:
      if (b->contains_error)
        return svn_error_create(SVN_ERR_RA_DAV_REQUEST_FAILED, NULL,
                                _("The request response contained at least "
                                  "one error."));
      break;

    case ELEM_responsedescription:
      b->description = svn_stringbuf_dup(b->cdata, b->req->pool);
      break;

    case ELEM_status:
      {
        ne_status status;

        if (ne_parse_statusline(b->cdata->data, &status) == 0)
          {
            /*### I wanted ||=, but I guess the end result is the same */
            b->contains_error |= (status.klass != 2);
            free(status.reason_phrase);
          }
        else
          return svn_error_create(SVN_ERR_RA_DAV_REQUEST_FAILED, NULL,
                                  _("The response contains a non-conforming "
                                    "HTTP status line."));
      }

    default:
      /* do nothing */
      break;
    }

  /* When we have an element which wants cdata,
     we'll set it all up in start_207_element() again */
  b->want_cdata = NULL;

  return SVN_NO_ERROR;
}


static ne_xml_parser *
multistatus_parser_create(svn_ra_dav__request_t *req)
{
  multistatus_baton_t *b = apr_pcalloc(req->pool, sizeof(*b));
  ne_xml_parser *multistatus_parser =
    svn_ra_dav__xml_parser_create(req, ne_accept_207,
                                  start_207_element,
                                  svn_ra_dav__xml_collect_cdata,
                                  end_207_element, b);
  return multistatus_parser;
}




/* Neon request management */

static apr_status_t
dav_request_cleanup(void *baton)
{
  svn_ra_dav__request_t *req = baton;

  if (req->ne_req)
    ne_request_destroy(req->ne_req);

  return APR_SUCCESS;
}


svn_ra_dav__request_t *
svn_ra_dav__request_create(svn_ra_dav__session_t *sess,
                           const char *method, const char *url,
                           apr_pool_t *pool)
{
  apr_pool_t *reqpool = svn_pool_create(pool);
  svn_ra_dav__request_t *req = apr_pcalloc(reqpool, sizeof(*req));

  req->ne_sess = sess->main_session_busy ? sess->ne_sess2 : sess->ne_sess;
  req->ne_req = ne_request_create(req->ne_sess, method, url);
  req->sess = sess;
  req->pool = reqpool;
  req->iterpool = svn_pool_create(req->pool);
  req->method = apr_pstrdup(req->pool, method);
  req->url = apr_pstrdup(req->pool, url);
  req->rv = -1;

  /* Neon resources may be NULL on out-of-memory */
  assert(req->ne_req != NULL);
  apr_pool_cleanup_register(reqpool, req,
                            dav_request_cleanup,
                            apr_pool_cleanup_null);

  return req;
}


static apr_status_t
compressed_body_reader_cleanup(void *baton)
{
  if (baton)
    ne_decompress_destroy(baton);

  return APR_SUCCESS;
}

static void
attach_ne_body_reader(svn_ra_dav__request_t *req,
                      ne_accept_response accpt,
                      ne_block_reader reader,
                      void *userdata)
{
  if (req->sess->compression)
    {
      ne_decompress *decompress =
        ne_decompress_reader(req->ne_req, accpt, reader, userdata);

      apr_pool_cleanup_register(req->pool,
                                decompress,
                                compressed_body_reader_cleanup,
                                apr_pool_cleanup_null);
    }
  else
    ne_add_response_body_reader(req->ne_req, accpt, reader, userdata);
}


typedef struct
{
  svn_ra_dav__request_t *req;
  svn_ra_dav__block_reader real_reader;
  void *real_baton;
} body_reader_wrapper_baton_t;

static int
body_reader_wrapper(void *userdata, const char *data, size_t len)
{
  body_reader_wrapper_baton_t *b = userdata;

  if (b->req->err)
    /* We already had an error? Bail out. */
    return 1;

  SVN_RA_DAV__REQ_ERR
    (b->req,
     b->real_reader(b->real_baton, data, len));

  if (b->req->err)
    return 1;

  return 0;
}

void
svn_ra_dav__add_response_body_reader(svn_ra_dav__request_t *req,
                                     ne_accept_response accpt,
                                     svn_ra_dav__block_reader reader,
                                     void *userdata)
{
  body_reader_wrapper_baton_t *b = apr_palloc(req->pool, sizeof(*b));

  b->req = req;
  b->real_baton = userdata;
  b->real_reader = reader;

  attach_ne_body_reader(req, accpt, body_reader_wrapper, b);
}



const svn_ra_dav__xml_elm_t *
svn_ra_dav__lookup_xml_elem(const svn_ra_dav__xml_elm_t *table,
                            const char *nspace,
                            const char *name)
{
  /* placeholder for `unknown' element if it's present */
  const svn_ra_dav__xml_elm_t *elem_unknown = NULL;
  const svn_ra_dav__xml_elm_t *elem;

  for(elem = table; elem->nspace; ++elem)
    {
      if (strcmp(elem->nspace, nspace) == 0
          && strcmp(elem->name, name) == 0)
        return elem;

      /* Use a single loop to save CPU cycles.
       *
       * Maybe this element is defined as `unknown'? */
      if (elem->id == ELEM_unknown)
        elem_unknown = elem;
    }

  /* ELEM_unknown position in the table or NULL */
  return elem_unknown;
}

svn_error_t *
svn_ra_dav__xml_collect_cdata(void *baton, int state,
                              const char *cdata, size_t len)
{
  svn_stringbuf_t **b = baton;

  if (*b)
    svn_stringbuf_appendbytes(*b, cdata, len);

  return SVN_NO_ERROR;
}



void svn_ra_dav__copy_href(svn_stringbuf_t *dst, const char *src)
{
  ne_uri parsed_url;

  /* parse the PATH element out of the URL and store it.

     ### do we want to verify the rest matches the current session?

     Note: mod_dav does not (currently) use an absolute URL, but simply a
     server-relative path (i.e. this uri_parse is effectively a no-op).
  */
  (void) ne_uri_parse(src, &parsed_url);
  svn_stringbuf_set(dst, parsed_url.path);
  ne_uri_free(&parsed_url);
}

static svn_error_t *
generate_error(svn_ra_dav__request_t *req, apr_pool_t *pool)
{
  int errcode = SVN_ERR_RA_DAV_REQUEST_FAILED;
  const char *context =
    apr_psprintf(req->pool, _("%s of '%s'"), req->method, req->url);
  const char *msg;
  const char *hostport;

  /* Convert the return codes. */
  switch (req->rv)
    {
    case NE_OK:
      switch (req->code)
        {
        case 404:
          return svn_error_create(SVN_ERR_RA_DAV_PATH_NOT_FOUND, NULL,
                                  apr_psprintf(pool, _("'%s' path not found"),
                                               req->url));

        case 301:
        case 302:
          return svn_error_create
            (SVN_ERR_RA_DAV_RELOCATED, NULL,
             apr_psprintf(pool,
                          (req->code == 301)
                          ? _("Repository moved permanently to '%s';"
                              " please relocate")
                          : _("Repository moved temporarily to '%s';"
                              " please relocate"),
                          svn_ra_dav__request_get_location(req, pool)));

        default:
          return svn_error_create
            (errcode, NULL,
             apr_psprintf(pool,
                          _("Server sent unexpected return value (%d %s) "
                            "in response to %s request for '%s'"), req->code,
                          req->code_desc, req->method, req->url));
        }
    case NE_AUTH:
      errcode = SVN_ERR_RA_NOT_AUTHORIZED;
      msg = _("authorization failed");
      break;

    case NE_CONNECT:
      msg = _("could not connect to server");
      break;

    case NE_TIMEOUT:
      msg = _("timed out waiting for server");
      break;

    default:
      /* Get the error string from neon and convert to UTF-8. */
      SVN_ERR(svn_utf_cstring_to_utf8(&msg, ne_get_error(req->ne_sess), pool));
      break;
    }

  /* The hostname may contain non-ASCII characters, so convert it to UTF-8. */
  SVN_ERR(svn_utf_cstring_to_utf8(&hostport,
                                  ne_get_server_hostport(req->ne_sess), pool));

  /*### This is a translation nightmare. Make sure to compose full strings
    and mark those for translation. */
  return svn_error_createf(errcode, NULL, "%s: %s (%s://%s)",
                           context, msg, ne_get_scheme(req->ne_sess),
                           hostport);
}


/** Error parsing **/


/* Custom function of type ne_accept_response. */
static int ra_dav_error_accepter(void *userdata,
                                 ne_request *req,
                                 const ne_status *st)
{
  /* Before, this function was being run for *all* responses including
     the 401 auth challenge.  In neon 0.24.x that was harmless.  But
     in neon 0.25.0, trying to parse a 401 response as XML using
     ne_xml_parse_v aborts the response; so the auth hooks never got a
     chance. */
  ne_content_type ctype;

  /* Only accept non-2xx responses with text/xml content-type */
  if (st->klass != 2 && ne_get_content_type(req, &ctype) == 0)
    {
      int is_xml = 
        (strcmp(ctype.type, "text") == 0 && strcmp(ctype.subtype, "xml") == 0);
      ne_free(ctype.value);        
      return is_xml;
    }
  else 
    return 0;
}


static const svn_ra_dav__xml_elm_t error_elements[] =
{
  { "DAV:", "error", ELEM_error, 0 },
  { "svn:", "error", ELEM_svn_error, 0 },
  { "http://apache.org/dav/xmlns", "human-readable", 
    ELEM_human_readable, SVN_RA_DAV__XML_CDATA },

  /* ### our validator doesn't yet recognize the rich, specific
         <D:some-condition-failed/> objects as defined by DeltaV.*/

  { NULL }
};


static int validate_error_elements(svn_ra_dav__xml_elmid parent,
                                   svn_ra_dav__xml_elmid child)
{
  switch (parent)
    {
    case ELEM_root:
      if (child == ELEM_error)
        return child;
      else
        return SVN_RA_DAV__XML_INVALID;

    case ELEM_error:
      if (child == ELEM_svn_error
          || child == ELEM_human_readable)
        return child;
      else
        return SVN_RA_DAV__XML_DECLINE;  /* ignore if something else
                                            was in there */

    default:
      return SVN_RA_DAV__XML_DECLINE;
    }

  /* NOTREACHED */
}


static int
collect_error_cdata(void *baton, int state,
                    const char *cdata, size_t len)
{
  svn_stringbuf_t **b = baton;

  if (*b)
    svn_stringbuf_appendbytes(*b, cdata, len);

  return 0;
}

typedef struct error_parser_baton
{
  svn_stringbuf_t *want_cdata;
  svn_stringbuf_t *cdata;

  svn_error_t **dst_err;
  svn_error_t *tmp_err;
  svn_boolean_t *marshalled_error;
} error_parser_baton_t;


static int
start_err_element(void *baton, int parent,
                  const char *nspace, const char *name, const char **atts)
{
  const svn_ra_dav__xml_elm_t *elm
    = svn_ra_dav__lookup_xml_elem(error_elements, nspace, name);
  int acc = elm
    ? validate_error_elements(parent, elm->id) : SVN_RA_DAV__XML_DECLINE;
  error_parser_baton_t *b = baton;
  svn_error_t **err = &(b->tmp_err);

  if (acc < 1) /* ! > 0 */
    return acc;

  switch (elm->id)
    {
    case ELEM_svn_error:
      {
        /* allocate the svn_error_t.  Hopefully the value will be
           overwritten by the <human-readable> tag, or even someday by
           a <D:failed-precondition/> tag. */
        *err = svn_error_create(APR_EGENERAL, NULL,
                                "General svn error from server");
        break;
      }
    case ELEM_human_readable:
      {
        /* get the errorcode attribute if present */
        const char *errcode_str = 
          svn_xml_get_attr_value("errcode", /* ### make constant in
                                               some mod_dav header? */
                                 atts);

        if (errcode_str && *err) 
          (*err)->apr_err = atoi(errcode_str);

        break;
      }

    default:
      break;
    }

  switch (elm->id)
    {
    case ELEM_human_readable:
      b->want_cdata = b->cdata;
      svn_stringbuf_setempty(b->want_cdata);
      break;

    default:
      b->want_cdata = NULL;
      break;
    }

  return elm->id;
}

static int
end_err_element(void *baton, int state, const char *nspace, const char *name)
{
  error_parser_baton_t *b = baton;
  svn_error_t **err = &(b->tmp_err);

  switch (state)
    {
    case ELEM_human_readable:
      {
        if (b->cdata->data && *err)
          {
            /* On the server dav_error_response_tag() will add a leading
               and trailing newline if DEBUG_CR is defined in mod_dav.h,
               so remove any such characters here. */
            apr_size_t len;
            const char *cd = b->cdata->data;
            if (*cd == '\n')
              ++cd;
            len = strlen(cd);
            if (len > 0 && cd[len-1] == '\n')
              --len;

            (*err)->message = apr_pstrmemdup((*err)->pool, cd, len);
          }
        break;
      }

    case ELEM_error:
      {
        if (*(b->dst_err))
          svn_error_clear(b->tmp_err);
        else if (b->tmp_err)
          {
            *(b->dst_err) = b->tmp_err;
            if (b->marshalled_error)
              *(b->marshalled_error) = TRUE;
          }
        b->tmp_err = NULL;
        break;
      }

    default:
      break;
    }

  return 0;
}

static apr_status_t
error_parser_baton_cleanup(void *baton)
{
  error_parser_baton_t *b = baton;

  if (b->tmp_err)
    svn_error_clear(b->tmp_err);

  return APR_SUCCESS;
}

static ne_xml_parser *
error_parser_create(svn_ra_dav__request_t *req)
{
  error_parser_baton_t *b = apr_palloc(req->pool, sizeof(*b));
  ne_xml_parser *error_parser;

  b->dst_err = &(req->err);
  b->marshalled_error = &(req->marshalled_error);
  b->tmp_err = NULL;

  b->want_cdata = NULL;
  b->cdata = svn_stringbuf_create("", req->pool);

  /* attach a standard <D:error> body parser to the request */
  error_parser = xml_parser_create(req);
  ne_xml_push_handler(error_parser,
                      start_err_element,
                      collect_error_cdata,
                      end_err_element, b);

  apr_pool_cleanup_register(req->pool, b,
                            error_parser_baton_cleanup,
                            apr_pool_cleanup_null);

  /* Register the "error" accepter and body-reader with the request --
     the one to use when HTTP status is *not* 2XX */
  attach_ne_body_reader(req, ra_dav_error_accepter,
                        ne_xml_parse_v, error_parser);

  return error_parser;
}


/* A body provider for ne_set_request_body_provider that pulls data
 * from an APR file. See ne_request.h for a description of the
 * interface.
 */

typedef struct
{
  svn_ra_dav__request_t *req;
  apr_file_t *body_file;
} body_provider_baton_t;

static ssize_t ra_dav_body_provider(void *userdata,
                                    char *buffer,
                                    size_t buflen)
{
  body_provider_baton_t *b = userdata;
  svn_ra_dav__request_t *req = b->req;
  apr_file_t *body_file = b->body_file;

  if (req->sess->callbacks &&
      req->sess->callbacks->cancel_func)
    SVN_RA_DAV__REQ_ERR
      (req, (req->sess->callbacks->cancel_func)(req->sess->callback_baton));

  if (req->err)
    return -1;

  svn_pool_clear(req->iterpool);
  if (buflen == 0)
    {
      /* This is the beginning of a new body pull. Rewind the file. */
      apr_off_t offset = 0;
      SVN_RA_DAV__REQ_ERR
        (b->req,
         svn_io_file_seek(body_file, APR_SET, &offset, req->iterpool));
      return (req->err ? -1 : 0);
    }
  else
    {
      apr_size_t nbytes = buflen;
      svn_error_t *err = svn_io_file_read(body_file, buffer, &nbytes,
                                          req->iterpool);
      if (err)
        {
          if (APR_STATUS_IS_EOF(err->apr_err))
            {
              svn_error_clear(err);
              return 0;
            }

          SVN_RA_DAV__REQ_ERR(req, err);
          return -1;
        }
      else
        return nbytes;
    }
}


svn_error_t *svn_ra_dav__set_neon_body_provider(svn_ra_dav__request_t *req,
                                                apr_file_t *body_file)
{
  apr_status_t status;
  apr_finfo_t finfo;
  body_provider_baton_t *b = apr_palloc(req->pool, sizeof(*b));

  status = apr_file_info_get(&finfo, APR_FINFO_SIZE, body_file);
  if (status)
    return svn_error_wrap_apr(status,
                              _("Can't calculate the request body size"));

  b->body_file = body_file;
  b->req = req;

  ne_set_request_body_provider(req->ne_req, (size_t) finfo.size,
                               ra_dav_body_provider, b);
  return SVN_NO_ERROR;
}


typedef struct spool_reader_baton_t
{
  const char *spool_file_name;
  apr_file_t *spool_file;
  svn_ra_dav__request_t *req;
} spool_reader_baton_t;


/* This implements the svn_ra_dav__block_reader() callback interface. */
static svn_error_t *
spool_reader(void *userdata,
             const char *buf,
             size_t len)
{
  spool_reader_baton_t *baton = userdata;

  SVN_ERR(svn_io_file_write_full(baton->spool_file, buf,
                                 len, NULL, baton->req->iterpool));
  svn_pool_clear(baton->req->iterpool);

  return SVN_NO_ERROR;
}


static svn_error_t *
parse_spool_file(svn_ra_dav__session_t *ras,
                 const char *spool_file_name,
                 ne_xml_parser *success_parser,
                 apr_pool_t *pool)
{
  apr_file_t *spool_file;
  svn_stream_t *spool_stream;
  char *buf = apr_palloc(pool, SVN__STREAM_CHUNK_SIZE);
  apr_size_t len;

  SVN_ERR(svn_io_file_open(&spool_file, spool_file_name,
                           (APR_READ | APR_BUFFERED), APR_OS_DEFAULT, pool));
  spool_stream = svn_stream_from_aprfile(spool_file, pool);
  while (1)
    {
      if (ras->callbacks &&
          ras->callbacks->cancel_func)
        SVN_ERR((ras->callbacks->cancel_func)(ras->callback_baton));

      len = SVN__STREAM_CHUNK_SIZE;
      SVN_ERR(svn_stream_read(spool_stream, buf, &len));
      if (len > 0)
        if (ne_xml_parse(success_parser, buf, len) != 0)
          /* The parse encountered an error or
             was aborted by a user defined callback */
          break;

      if (len != SVN__STREAM_CHUNK_SIZE)
        break;
    }
  return SVN_NO_ERROR;
}


/* A baton that is used along with a set of Neon ne_startelm_cb,
 * ne_cdata_cb, and ne_endelm_cb callbacks to handle conversion
 * from Subversion style errors to Neon style errors.
 *
 * The underlying Subversion callbacks are called, and if errors
 * are returned they are stored in this baton and a Neon level
 * error code is returned to the parser.
 */
typedef struct {
  svn_ra_dav__request_t *req;
  ne_xml_parser *parser;

  void *baton;
  svn_ra_dav__startelm_cb_t startelm_cb;
  svn_ra_dav__cdata_cb_t cdata_cb;
  svn_ra_dav__endelm_cb_t endelm_cb;
} parser_wrapper_baton_t;

static int
wrapper_startelm_cb(void *baton,
                    int parent,
                    const char *nspace,
                    const char *name,
                    const char **atts)
{
  parser_wrapper_baton_t *pwb = baton;
  int elem = SVN_RA_DAV__XML_DECLINE;

  if (pwb->startelm_cb)
    SVN_RA_DAV__REQ_ERR
      (pwb->req,
       pwb->startelm_cb(&elem, pwb->baton, parent, nspace, name, atts));

  if (elem == SVN_RA_DAV__XML_INVALID)
    SVN_RA_DAV__REQ_ERR
      (pwb->req,
       svn_error_create(SVN_ERR_XML_MALFORMED, NULL, NULL));

  if (pwb->req->err)
    return NE_XML_ABORT;

  return elem;
}

static int
wrapper_cdata_cb(void *baton, int state, const char *cdata, size_t len)
{
  parser_wrapper_baton_t *pwb = baton;

  if (pwb->cdata_cb)
    SVN_RA_DAV__REQ_ERR
      (pwb->req,
       pwb->cdata_cb(pwb->baton, state, cdata, len));

  if (pwb->req->err)
    return NE_XML_ABORT;

  return 0;
}

static int
wrapper_endelm_cb(void *baton,
                  int state,
                  const char *nspace,
                  const char *name)
{
  parser_wrapper_baton_t *pwb = baton;

  if (pwb->endelm_cb)
    SVN_RA_DAV__REQ_ERR
      (pwb->req,
       pwb->endelm_cb(pwb->baton, state, nspace, name));

  if (pwb->req->err)
    return NE_XML_ABORT;

  return 0;
}

static int
wrapper_reader_cb(void *baton, const char *data, size_t len)
{
  parser_wrapper_baton_t *pwb = baton;
  svn_ra_dav__session_t *sess = pwb->req->sess;

  if (pwb->req->err)
    return 1;

  if (sess->callbacks->cancel_func)
    SVN_RA_DAV__REQ_ERR
      (pwb->req,
       (sess->callbacks->cancel_func)(sess->callback_baton));

  if (pwb->req->err)
    return 1;

  return ne_xml_parse(pwb->parser, data, len);
}

ne_xml_parser *
svn_ra_dav__xml_parser_create(svn_ra_dav__request_t *req,
                              ne_accept_response accpt,
                              svn_ra_dav__startelm_cb_t startelm_cb,
                              svn_ra_dav__cdata_cb_t cdata_cb,
                              svn_ra_dav__endelm_cb_t endelm_cb,
                              void *baton)
{
  ne_xml_parser *p = xml_parser_create(req);
  parser_wrapper_baton_t *pwb = apr_palloc(req->pool, sizeof(*pwb));

  pwb->req = req;
  pwb->parser = p;
  pwb->baton = baton;
  pwb->startelm_cb = startelm_cb;
  pwb->cdata_cb = cdata_cb;
  pwb->endelm_cb = endelm_cb;

  ne_xml_push_handler(p,
                      wrapper_startelm_cb,
                      wrapper_cdata_cb,
                      wrapper_endelm_cb, pwb);

  if (accpt)
    attach_ne_body_reader(req, accpt, wrapper_reader_cb, pwb);

  return p;
}


typedef struct cancellation_baton_t
{
  ne_block_reader real_cb;
  void *real_userdata;
  svn_ra_dav__request_t *req;
} cancellation_baton_t;

static int
cancellation_callback(void *userdata, const char *block, size_t len)
{
  cancellation_baton_t *b = userdata;
  svn_ra_dav__session_t *ras = b->req->sess;

  if (ras->callbacks->cancel_func)
    SVN_RA_DAV__REQ_ERR
      (b->req,
       (ras->callbacks->cancel_func)(ras->callback_baton));

  if (b->req->err)
    return 1;
  else
    return (b->real_cb)(b->real_userdata, block, len);
}


static cancellation_baton_t *
get_cancellation_baton(svn_ra_dav__request_t *req,
                       ne_block_reader real_cb,
                       void *real_userdata,
                       apr_pool_t *pool)
{
  cancellation_baton_t *b = apr_palloc(pool, sizeof(*b));

  b->real_cb = real_cb;
  b->real_userdata = real_userdata;
  b->req = req;

  return b;
}

/* See doc string for svn_ra_dav__parsed_request. */
static svn_error_t *
parsed_request(svn_ra_dav__session_t *ras,
               const char *method,
               const char *url,
               const char *body,
               apr_file_t *body_file,
               void set_parser(ne_xml_parser *parser,
                               void *baton),
               svn_ra_dav__startelm_cb_t startelm_cb,
               svn_ra_dav__cdata_cb_t cdata_cb,
               svn_ra_dav__endelm_cb_t endelm_cb,
               void *baton,
               apr_hash_t *extra_headers,
               int *status_code,
               svn_boolean_t spool_response,
               apr_pool_t *pool)
{
  svn_ra_dav__request_t *req;
  ne_xml_parser *success_parser = NULL;
  const char *msg;
  spool_reader_baton_t spool_reader_baton;

  /* create/prep the request */
  req = svn_ra_dav__request_create(ras, method, url, pool);

  if (body == NULL)
    SVN_ERR(svn_ra_dav__set_neon_body_provider(req, body_file));

  /* ### use a symbolic name somewhere for this MIME type? */
  ne_add_request_header(req->ne_req, "Content-Type", "text/xml");

  /* create a parser to read the normal response body */
  success_parser = svn_ra_dav__xml_parser_create(req, NULL,
                                                 startelm_cb, cdata_cb,
                                                 endelm_cb, baton);

  /* if our caller is interested in having access to this parser, call
     the SET_PARSER callback with BATON. */
  if (set_parser != NULL)
    set_parser(success_parser, baton);

  /* Register the "main" accepter and body-reader with the request --
     the one to use when the HTTP status is 2XX.  If we are spooling
     the response to disk first, we use our custom spool reader.  */
  if (spool_response)
    {
      const char *tmpfile_path;
      SVN_ERR(svn_io_temp_dir(&tmpfile_path, pool));

      tmpfile_path = svn_path_join(tmpfile_path, "dav-spool", pool);
      /* Blow the temp-file away as soon as we eliminate the entire request */
      SVN_ERR(svn_io_open_unique_file2(&spool_reader_baton.spool_file,
                                       &spool_reader_baton.spool_file_name,
                                       tmpfile_path, "",
                                       svn_io_file_del_on_pool_cleanup,
                                       req->pool));
      spool_reader_baton.req = req;

      svn_ra_dav__add_response_body_reader(req, ne_accept_2xx, spool_reader,
                                           &spool_reader_baton);
    }
  else
    attach_ne_body_reader(req, ne_accept_2xx, cancellation_callback,
                          get_cancellation_baton(req, ne_xml_parse_v,
                                                 success_parser, pool));

  /* run the request and get the resulting status code. */
  SVN_ERR(svn_ra_dav__request_dispatch(status_code,
                                       req, extra_headers, body,
                                       (strcmp(method, "PROPFIND") == 0)
                                       ? 207 : 200,
                                       0, /* not used */
                                       pool));

  if (spool_response)
    {
      /* All done with the temporary file we spooled the response into. */
      (void) apr_file_close(spool_reader_baton.spool_file);

      /* The success parser may set an error value in req->err */
      SVN_RA_DAV__REQ_ERR
        (req, parse_spool_file(ras, spool_reader_baton.spool_file_name,
                               success_parser, req->pool));
      if (req->err)
        {
          svn_error_compose(req->err, svn_error_createf
                            (SVN_ERR_RA_DAV_REQUEST_FAILED, NULL,
                             _("Error reading spooled %s request response"),
                             method));
          return req->err;
        }
    }

  /* was there an XML parse error somewhere? */
  msg = ne_xml_get_error(success_parser);
  if (msg != NULL && *msg != '\0')
    return svn_error_createf(SVN_ERR_RA_DAV_REQUEST_FAILED, NULL,
                             _("The %s request returned invalid XML "
                               "in the response: %s (%s)"),
                             method, msg, url);

  svn_ra_dav__request_destroy(req);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_ra_dav__parsed_request(svn_ra_dav__session_t *sess,
                           const char *method,
                           const char *url,
                           const char *body,
                           apr_file_t *body_file,
                           void set_parser(ne_xml_parser *parser,
                                           void *baton),
                           svn_ra_dav__startelm_cb_t startelm_cb,
                           svn_ra_dav__cdata_cb_t cdata_cb,
                           svn_ra_dav__endelm_cb_t endelm_cb,
                           void *baton,
                           apr_hash_t *extra_headers,
                           int *status_code,
                           svn_boolean_t spool_response,
                           apr_pool_t *pool)
{
  SVN_ERR_W(parsed_request(sess, method, url, body, body_file,
                           set_parser,
                           startelm_cb, cdata_cb, endelm_cb,
                           baton, extra_headers, status_code,
                           spool_response, pool),
            apr_psprintf(pool,_("%s request failed on '%s'"), method, url));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_ra_dav__simple_request(int *code,
                           svn_ra_dav__session_t *ras,
                           const char *method,
                           const char *url,
                           apr_hash_t *extra_headers,
                           const char *body,
                           int okay_1, int okay_2, apr_pool_t *pool)
{
  svn_ra_dav__request_t *req =
    svn_ra_dav__request_create(ras, method, url, pool);

  /* we don't need the status parser: it's attached to the request
     and detected errors will be returned there... */
  (void) multistatus_parser_create(req);

  /* svn_ra_dav__request_dispatch() adds the custom error response reader */
  SVN_ERR(svn_ra_dav__request_dispatch(code, req, extra_headers, body,
                                       okay_1, okay_2, pool));
  svn_ra_dav__request_destroy(req);

  return SVN_NO_ERROR;
}

void
svn_ra_dav__add_depth_header(apr_hash_t *extra_headers, int depth)
{
  /*  assert(extra_headers != NULL);
  assert(depth == SVN_RA_DAV__DEPTH_ZERO
         || depth == SVN_RA_DAV__DEPTH_ONE
         || depth == SVN_RA_DAV__DEPTH_INFINITE); */
  apr_hash_set(extra_headers, "Depth", APR_HASH_KEY_STRING,
               (depth == SVN_RA_DAV__DEPTH_INFINITE)
               ? "infinity" : (depth == SVN_RA_DAV__DEPTH_ZERO) ? "0" : "1");

  return;
}


svn_error_t *
svn_ra_dav__copy(svn_ra_dav__session_t *ras,
                 svn_boolean_t overwrite,
                 int depth,
                 const char *src,
                 const char *dst,
                 apr_pool_t *pool)
{
  const char *abs_dst;
  apr_hash_t *extra_headers = apr_hash_make(pool);

  abs_dst = apr_psprintf(pool, "%s://%s%s", ne_get_scheme(ras->ne_sess),
                         ne_get_server_hostport(ras->ne_sess), dst);
  apr_hash_set(extra_headers, "Destination", APR_HASH_KEY_STRING, abs_dst);
  apr_hash_set(extra_headers, "Overwrite", APR_HASH_KEY_STRING,
               overwrite ? "T" : "F");
  svn_ra_dav__add_depth_header(extra_headers, depth);

  return svn_ra_dav__simple_request(NULL, ras, "COPY", src, extra_headers,
                                    NULL, 201, 204, pool);
}



svn_error_t *
svn_ra_dav__maybe_store_auth_info(svn_ra_dav__session_t *ras,
                                  apr_pool_t *pool)
{
  /* No auth_baton?  Never mind. */
  if (! ras->callbacks->auth_baton)
    return SVN_NO_ERROR;

  /* If we ever got credentials, ask the iter_baton to save them.  */
  SVN_ERR(svn_auth_save_credentials(ras->auth_iterstate,
                                    pool));
  
  return SVN_NO_ERROR;
}


svn_error_t *
svn_ra_dav__maybe_store_auth_info_after_result(svn_error_t *err,
                                               svn_ra_dav__session_t *ras,
                                               apr_pool_t *pool)
{
  if (! err || (err->apr_err != SVN_ERR_RA_NOT_AUTHORIZED))
    {
      svn_error_t *err2 = svn_ra_dav__maybe_store_auth_info(ras, pool);
      if (err2 && ! err)
        return err2;
      else if (err)
        {
          svn_error_clear(err2);
          return err;
        }
    }

  return err;
}


svn_error_t *
svn_ra_dav__request_dispatch(int *code_p,
                             svn_ra_dav__request_t *req,
                             apr_hash_t *extra_headers,
                             const char *body,
                             int okay_1,
                             int okay_2,
                             apr_pool_t *pool)
{
  ne_xml_parser *error_parser;
  const ne_status *statstruct;

  /* add any extra headers passed in by caller. */
  if (extra_headers != NULL)
    {
      apr_hash_index_t *hi;
      for (hi = apr_hash_first(pool, extra_headers);
           hi; hi = apr_hash_next(hi))
        {
          const void *key;
          void *val;
          apr_hash_this(hi, &key, NULL, &val);
          ne_add_request_header(req->ne_req,
                                (const char *) key, (const char *) val);
        }
    }

  if (body)
    ne_set_request_body_buffer(req->ne_req, body, strlen(body));

  /* attach a standard <D:error> body parser to the request */
  error_parser = error_parser_create(req);

  if (req->ne_sess == req->sess->ne_sess) /* We're consuming 'session 1' */
    req->sess->main_session_busy = TRUE;
  /* run the request, see what comes back. */
  req->rv = ne_request_dispatch(req->ne_req);
  if (req->ne_sess == req->sess->ne_sess) /* We're done consuming 'session 1' */
    req->sess->main_session_busy = FALSE;

  /* Save values from the request */
  statstruct = ne_get_status(req->ne_req);
  req->code_desc = apr_pstrdup(pool, statstruct->reason_phrase);
  req->code = statstruct->code;

  if (code_p)
     *code_p = req->code;

  if (!req->marshalled_error)
    SVN_ERR(req->err);

  /* If the status code was one of the two that we expected, then go
     ahead and return now. IGNORE any marshalled error. */
  if (req->rv == NE_OK && (req->code == okay_1 || req->code == okay_2))
    return SVN_NO_ERROR;

  /* Any other errors? Report them */
  SVN_ERR(req->err);

  /* We either have a neon error, or some other error
     that we didn't expect. */
  return generate_error(req, pool);
}


const char *
svn_ra_dav__request_get_location(svn_ra_dav__request_t *request,
                                 apr_pool_t *pool)
{
  const char *val = ne_get_response_header(request->ne_req, "Location");
  return val ? apr_pstrdup(pool, val) : NULL;
}
