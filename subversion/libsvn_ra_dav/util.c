/*
 * util.c :  utility functions for the RA/DAV library
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

#define APR_WANT_STRFUNC
#include <apr_want.h>

#include <ne_uri.h>
#include <ne_compress.h>

#include "svn_string.h"
#include "svn_xml.h"
#include "svn_path.h"
#include "svn_config.h"

#include "ra_dav.h"





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

svn_error_t *svn_ra_dav__convert_error(ne_session *sess,
                                       const char *context,
                                       int retcode)
{
  int errcode = SVN_ERR_RA_DAV_REQUEST_FAILED;
  const char *msg;

  /* Convert the return codes. */
  switch (retcode) 
    {
    case NE_AUTH:
      errcode = SVN_ERR_RA_NOT_AUTHORIZED;
      msg = "authorization failed";
      break;
      
    case NE_CONNECT:
      msg = "could not connect to server";
      break;

    case NE_TIMEOUT:
      msg = "timed out waiting for server";
      break;

    default:
      /* Get the error string from neon. */
      msg = ne_get_error (sess);
      break;
    }

  return svn_error_createf (errcode, NULL, "%s: %s", context, msg);
  
}


/** Error parsing **/


/* Custom function of type ne_accept_response. */
static int ra_dav_error_accepter(void *userdata,
                                 ne_request *req,
                                 const ne_status *st)
{
  /* Only accept the body-response if the HTTP status code is *not* 2XX. */
  return (st->klass != 2);
}


static const struct ne_xml_elm error_elements[] =
{
  { "DAV:", "error", ELEM_error, 0 },
  { "svn:", "error", ELEM_svn_error, 0 },
  { "http://apache.org/dav/xmlns", "human-readable", 
    ELEM_human_readable, NE_XML_CDATA },

  /* ### our validator doesn't yet recognize the rich, specific
         <D:some-condition-failed/> objects as defined by DeltaV.*/

  { NULL }
};


static int validate_error_elements(void *userdata,
                                   ne_xml_elmid parent,
                                   ne_xml_elmid child)
{
  switch (parent)
    {
    case NE_ELM_root:
      if (child == ELEM_error)
        return NE_XML_VALID;
      else
        return NE_XML_INVALID;

    case ELEM_error:
      if (child == ELEM_svn_error
          || child == ELEM_human_readable)
        return NE_XML_VALID;
      else
        return NE_XML_DECLINE;  /* ignore if something else was in there */

    default:
      return NE_XML_DECLINE;
    }

  /* NOTREACHED */
}


static int start_err_element(void *userdata, const struct ne_xml_elm *elm,
                             const char **atts)
{
  svn_error_t **err = userdata;

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

  return 0;
}

static int end_err_element(void *userdata, const struct ne_xml_elm *elm,
                           const char *cdata)
{
  svn_error_t **err = userdata;

  switch (elm->id)
    {
    case ELEM_human_readable:
      {
        if (cdata && *err)
          (*err)->message = apr_pstrdup((*err)->pool, cdata);
        break;
      }

    default:
      break;
    }

  return 0;
}


/* A body provider for ne_set_request_body_provider that pulls data
 * from an APR file. See ne_request.h for a description of the
 * interface.
 */
static ssize_t ra_dav_body_provider (void *userdata,
                                     char *buffer,
                                     size_t buflen)
{
  apr_file_t *body_file = userdata;
  apr_status_t status;

  if (buflen == 0)
    {
      /* This is the beginning of a new body pull. Rewind the file. */
      apr_off_t offset = 0;
      status = apr_file_seek(body_file, APR_SET, &offset);
      return (status ? -1 : 0);
    }
  else
    {
      apr_size_t nbytes = buflen;
      status = apr_file_read(body_file, buffer, &nbytes);
      if (status)
        return (APR_STATUS_IS_EOF(status) ? 0 : -1);
      else
        return nbytes;
    }
}


svn_error_t *svn_ra_dav__set_neon_body_provider(ne_request *req,
                                                apr_file_t *body_file)
{
  apr_status_t status;
  apr_finfo_t finfo;

  /* ### APR bug? apr_file_info_get won't always return the correct
         size for buffered files. */
  status = apr_file_info_get(&finfo, APR_FINFO_SIZE, body_file);
  if (status)
    return svn_error_create(status, NULL,
                            "Could not calculate the request body size");

  ne_set_request_body_provider(req, (size_t) finfo.size,
                               ra_dav_body_provider, body_file);
  return SVN_NO_ERROR;
}



svn_error_t *svn_ra_dav__parsed_request(ne_session *sess,
                                        const char *method,
                                        const char *url,
                                        const char *body,
                                        apr_file_t *body_file,
                                        const struct ne_xml_elm *elements, 
                                        ne_xml_validate_cb validate_cb,
                                        ne_xml_startelm_cb startelm_cb, 
                                        ne_xml_endelm_cb endelm_cb,
                                        void *baton,
                                        apr_hash_t *extra_headers,
                                        apr_pool_t *pool)
{
  ne_request *req;
  ne_decompress *decompress_main;
  ne_decompress *decompress_err;
  ne_xml_parser *success_parser;
  ne_xml_parser *error_parser;
  int rv;
  int decompress_rv;
  int code;
  const char *msg;
  svn_error_t *err = SVN_NO_ERROR;
  svn_ra_ne_session_baton_t *sess_baton =
    ne_get_session_private(sess, SVN_RA_NE_SESSION_ID);

  /* create/prep the request */
  req = ne_request_create(sess, method, url);

  if (body != NULL)
    ne_set_request_body_buffer(req, body, strlen(body));
  else
    SVN_ERR(svn_ra_dav__set_neon_body_provider(req, body_file));

  /* ### use a symbolic name somewhere for this MIME type? */
  ne_add_request_header(req, "Content-Type", "text/xml");

  /* add any extra headers passed in by caller. */
  if (extra_headers != NULL)
    {
      apr_hash_index_t *hi;
      for (hi = apr_hash_first (pool, extra_headers);
           hi; hi = apr_hash_next (hi))
        {
          const void *key;
          void *val;
          apr_hash_this (hi, &key, NULL, &val);
          ne_add_request_header(req, (const char *) key, (const char *) val); 
        }
    }

  /* create a parser to read the normal response body */
  success_parser = ne_xml_create();
  ne_xml_push_handler(success_parser, elements,
                       validate_cb, startelm_cb, endelm_cb, baton);

  /* create a parser to read the <D:error> response body */
  error_parser = ne_xml_create();
  ne_xml_push_handler(error_parser, error_elements, validate_error_elements,
                      start_err_element, end_err_element, &err); 

  /* Register the "main" accepter and body-reader with the request --
     the one to use when the HTTP status is 2XX */
  if (sess_baton->compression)
    {
      decompress_main = ne_decompress_reader(req, ne_accept_2xx,
                                             ne_xml_parse_v, success_parser);
    }
  else
    {
      decompress_main = NULL;
      ne_add_response_body_reader(req, ne_accept_2xx, ne_xml_parse_v,
                                  success_parser);
    }

  /* Register the "error" accepter and body-reader with the request --
     the one to use when HTTP status is *not* 2XX */
  if (sess_baton->compression)
    {
      decompress_err = ne_decompress_reader(req, ra_dav_error_accepter,
                                            ne_xml_parse_v, error_parser);
    }
  else
    {
      decompress_err = NULL;
      ne_add_response_body_reader(req, ra_dav_error_accepter, ne_xml_parse_v,
                                  error_parser);
    }

  /* run the request and get the resulting status code. */
  rv = ne_request_dispatch(req);

  if (decompress_main)
    {
      decompress_rv = ne_decompress_destroy(decompress_main);
      if (decompress_rv != 0)
        {
          rv = decompress_rv;
        }
    }

  if (decompress_err)
    {
      decompress_rv = ne_decompress_destroy(decompress_err);
      if (decompress_rv != 0)
        {
          rv = decompress_rv;
        }
    }
  
  code = ne_get_status(req)->code;
  ne_request_destroy(req);

  if (err) /* If the error parser had a problem */
    goto error;

  if (code != 200
      || rv != NE_OK)
    {
      msg = apr_psprintf(pool, "%s of '%s'", method, url);
      err = svn_ra_dav__convert_error(sess, msg, rv);
      goto error;
    }

  /* was there an XML parse error somewhere? */
  msg = ne_xml_get_error(success_parser);
  if (msg != NULL && *msg != '\0')
    {
      err = svn_error_createf(SVN_ERR_RA_DAV_REQUEST_FAILED, NULL,
                              "The %s request returned invalid XML "
                              "in the response: %s. (%s)",
                              method, msg, url);
      goto error;
    }
  /* ### maybe hook this to a pool? */
  ne_xml_destroy(success_parser);
  ne_xml_destroy(error_parser);

  return NULL;

 error:
  ne_xml_destroy(success_parser);
  ne_xml_destroy(error_parser);
  return svn_error_createf(err->apr_err, err,
                           "%s request failed on '%s'", method, url );
}



svn_error_t *
svn_ra_dav__maybe_store_auth_info(svn_ra_session_t *ras)
{
  /* No auth_baton?  Never mind. */
  if (! ras->callbacks->auth_baton)
    return SVN_NO_ERROR;

  /* If we ever got credentials, ask the iter_baton to save them.  */
  SVN_ERR (svn_auth_save_credentials(ras->auth_iterstate,
                                     ras->pool));
  
  return SVN_NO_ERROR;
}


svn_error_t *
svn_ra_dav__request_dispatch(int *code,
                             ne_request *request,
                             ne_session *session,
                             const char *method,
                             const char *url,
                             int okay_1,
                             int okay_2,
                             apr_pool_t *pool)
{
  ne_xml_parser *error_parser;
  int rv;
  const ne_status *statstruct;
  const char *code_desc;
  const char *msg;
  svn_error_t *err = SVN_NO_ERROR;

  /* attach a standard <D:error> body parser to the request */
  error_parser = ne_xml_create();
  ne_xml_push_handler(error_parser, error_elements, validate_error_elements,
                      start_err_element, end_err_element, &err);
  ne_add_response_body_reader(request, ra_dav_error_accepter,
                              ne_xml_parse_v, error_parser);

  /* run the request, see what comes back. */
  rv = ne_request_dispatch(request);

  statstruct = ne_get_status(request);
  *code = statstruct->code;
  code_desc = apr_pstrdup(pool, statstruct->reason_phrase);

  ne_request_destroy(request);
  ne_xml_destroy(error_parser);

  /* If the status code was one of the two that we expected, then go
     ahead and return now. IGNORE any marshalled error. */
  if (rv == NE_OK && (*code == okay_1 || *code == okay_2))
    return SVN_NO_ERROR;

  /* next, check to see if a <D:error> was discovered */
  if (err)
    return err;

  /* We either have a neon error, or some other error
     that we didn't expect. */
  msg = apr_psprintf(pool, "%s of %s", method, url);
  return svn_ra_dav__convert_error(session, msg, rv);
}
