/*
 * auth_digest.c : digest authn implementation
 *
 * ====================================================================
 * Copyright (c) 2009 CollabNet.  All rights reserved.
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

#include <string.h>

#include <apr.h>
#include <apr_base64.h>
#include <apr_md5.h>
#include <apr_uuid.h>

#include "svn_error.h"
#include "svn_private_config.h"
#include "ra_serf.h"
#include "auth_digest.h"

/** Digest authentication, implements RFC 2671. **/
static
char int_to_hex(int v)
{
  return (v < 10) ? '0' + v : 'a' + (v - 10);
}

static
char* hex_encode(const unsigned char *hashval,
		 apr_pool_t *pool)
{
  int i;
  char *hexval = apr_palloc(pool, (APR_MD5_DIGESTSIZE * 2) + 1);
  for (i = 0; i < APR_MD5_DIGESTSIZE; i++)
    {
      hexval[2 * i] = int_to_hex((hashval[i] >> 4) & 0xf);
      hexval[2 * i + 1] = int_to_hex(hashval[i] & 0xf);
    }
  hexval[APR_MD5_DIGESTSIZE * 2] = '\0';
  return hexval;
}

static char *
random_cnonce(apr_pool_t *pool)
{
  apr_uuid_t uuid;
  char *buf = apr_palloc(pool, APR_UUID_FORMATTED_LENGTH + 1);

  apr_uuid_get(&uuid);
  apr_uuid_format(buf, &uuid);

  return hex_encode((unsigned char*)buf, pool);
}

static char *
build_digest_ha1(svn_auth_cred_simple_t *simple_creds,
		 const char *realm_name,
		 apr_pool_t *pool)
{
  char *tmp;
  unsigned char ha1[APR_MD5_DIGESTSIZE];
  apr_status_t status;

  /* calculate ha1:
     MD5 hash of the combined user name, authentication realm and password */
  tmp = apr_psprintf(pool, "%s:%s:%s",
		     simple_creds->username,
		     realm_name,
		     simple_creds->password);
  status = apr_md5(ha1, tmp, strlen(tmp));

  return hex_encode(ha1, pool);
}

static char *
build_digest_ha2(const char *uri,
		 const char *method,
		 const char *qop,
		 apr_pool_t *pool)
{
  char *tmp;
  unsigned char ha2[APR_MD5_DIGESTSIZE];
  apr_status_t status;

  if (!qop || strcmp(qop, "auth") == 0)
    {
      /* calculate ha2:
	 MD5 hash of the combined method and URI */
      tmp = apr_psprintf(pool, "%s:%s",
			 method,
			 uri);
      status = apr_md5(ha2, tmp, strlen(tmp));

      return hex_encode(ha2, pool);
    }
  else
    {
      /* TODO: auth-int isn't supported! */
    }

  return NULL;
}

static char *
build_auth_header(serf_digest_context_t *context,
		  const char *uri,
		  const char *method,
		  apr_pool_t *pool)
{
  unsigned char response_hdr[APR_MD5_DIGESTSIZE]; 
  apr_status_t status;
  char *hdr, *tmp, *response_hdr_hex, *ha2, *nc_str;

  ha2 = build_digest_ha2(uri, method, context->qop, pool);

  hdr = apr_psprintf(pool,
		     "Digest realm=\"%s\","
                     " username=\"%s\","
 		     " nonce=\"%s\","
		     " uri=\"%s\",",
		     context->realm, context->username, context->nonce,
		     uri);

  if (context->qop)
    {
      /* calculate the response header:
	 MD5 hash of the combined HA1 result, server nonce (nonce),
         request counter (nc), client nonce (cnonce),
	 quality of protection code (qop) and HA2 result. */
      if (! context->cnonce)
	context->cnonce = random_cnonce(context->pool);
      nc_str = apr_psprintf(pool, "%08x", context->digest_nc);

      tmp = apr_psprintf(pool, "%s:%s:%s:%s:%s:%s",
			 context->ha1, context->nonce, nc_str,
			 context->cnonce, context->qop, ha2);
      status = apr_md5(response_hdr, tmp, strlen(tmp));
      response_hdr_hex =  hex_encode(response_hdr, pool);

      hdr = apr_pstrcat(pool,
			hdr,
			", nc=", nc_str,
			", cnonce=\"",context->cnonce, "\"",
			", qop=\"", context->qop, "\"",
			", response=\"", response_hdr_hex, "\"",
			NULL);
    }
  else
    {
      /* calculate the response header:
	 MD5 hash of the combined HA1 result, server nonce (nonce)
	 and HA2 result. */
      tmp = apr_psprintf(pool, "%s:%s:%s",
			 context->ha1, context->nonce, ha2);
      status = apr_md5(response_hdr, tmp, strlen(tmp));
      response_hdr_hex =  hex_encode(response_hdr, pool);

      hdr = apr_pstrcat(pool,
			hdr,
			", response=\"", response_hdr_hex, "\"",
			NULL);
    }
  if (context->opaque) {
    hdr = apr_pstrcat(pool,
                      hdr,
                      ", opaque=\"", context->opaque, "\"",
                      NULL);
  }
  if (context->algorithm) {
    hdr = apr_pstrcat(pool,
                      hdr,
                      ", algorithm=", context->algorithm, "",
                      NULL);
  }
  
  return hdr;
}

svn_error_t *
handle_digest_auth(svn_ra_serf__handler_t *ctx,
                   serf_request_t *request,
                   serf_bucket_t *response,
                   char *auth_hdr,
                   char *auth_attr,
                   apr_pool_t *pool)
{
  void *creds;
  char *nextkv, *realm_name = NULL, *nonce = NULL, *algorithm = NULL;
  char *qop = NULL, *opaque = NULL;
  svn_auth_cred_simple_t *simple_creds;
  apr_port_t port;
  svn_ra_serf__session_t *session = ctx->session;
  svn_ra_serf__connection_t *conn = ctx->conn;
  serf_digest_context_t *context = (serf_digest_context_t *)conn->auth_context;
  char *kv, *key, *val;

  /* We're expecting a list of key=value pairs, separated by a comma. 
     Ex. realm="SVN Digest",
         nonce="f+zTl/leBAA=e371bd3070adfb47b21f5fc64ad8cc21adc371a5", 
         algorithm=MD5, qop="auth" */
  while ((kv = apr_strtok(auth_attr, ",", &nextkv)))
    {
      key = apr_strtok(kv, "=", &val);
      /* skip leading spaces */
      while (*key && *key == ' ') key++;
      
      if (strcasecmp(key, "realm") == 0)
	{
	  realm_name = apr_strtok(NULL, "=", &val);
	  if (realm_name[0] == '\"')
	    {
	      apr_size_t realm_len;
              
	      realm_len = strlen(realm_name);
	      if (realm_name[realm_len - 1] == '\"')
		{
		  realm_name[realm_len - 1] = '\0';
		  realm_name++;
		}
	    }
	}
      if (strcmp(key, "nonce") == 0)
	{
	  nonce = apr_pstrdup(session->pool, val);
	  if (nonce[0] == '\"')
	    {
	      apr_size_t nonce_len;
              
	      nonce_len = strlen(nonce);
	      if (nonce[nonce_len - 1] == '\"')
		{
		  nonce[nonce_len - 1] = '\0';
		  nonce++;
		}
	    }
	}
      if (strcmp(key, "algorithm") == 0)
	{
	  algorithm = apr_pstrdup(session->pool, val);
	}
      if (strcmp(key, "qop") == 0)
	{
	  qop = apr_pstrdup(session->pool, val);
	  if (qop[0] == '\"')
	    {
	      apr_size_t qop_len;
              
	      qop_len = strlen(qop);
	      if (qop[qop_len - 1] == '\"')
		{
		  qop[qop_len - 1] = '\0';
		  qop++;
		}
	    }
	  
	}
      if (strcmp(key, "opaque") == 0)
	{
	  opaque = apr_pstrdup(session->pool, val);
	}
      
      /* Ignore all unsupported attributes. */
      
      auth_attr = NULL;
    }
  if (!realm_name)
    {
      return svn_error_create
	(SVN_ERR_RA_DAV_MALFORMED_DATA, NULL,
	 _("Missing 'realm' attribute in Authorization header"));
    }
  
  if (session->repos_url.port_str)
    {
      port = session->repos_url.port;
    }
  else
    {
      port = apr_uri_port_of_scheme(session->repos_url.scheme);
    }
  
  session->realm = apr_psprintf(session->pool, "<%s://%s:%d> %s",
				session->repos_url.scheme,
				session->repos_url.hostname,
				port,
				realm_name);

  /* Use svn_auth_first_credentials if this is the first time we ask for
     credentials during this session OR if the last time we asked
     session->auth_state wasn't set (eg. if the credentials provider was
     cancelled by the user). */
  if (!session->auth_state)
    {
      SVN_ERR(svn_auth_first_credentials(&creds,
                                         &session->auth_state,
                                         SVN_AUTH_CRED_SIMPLE,
                                         session->realm,
                                         session->wc_callbacks->auth_baton,
                                         session->pool));
    }
  else
    {
      SVN_ERR(svn_auth_next_credentials(&creds,
                                        session->auth_state,
                                        session->pool));
    }

  session->auth_attempts++;

  if (!creds || session->auth_attempts > 4)
    {
      /* No more credentials. */
      return svn_error_create(SVN_ERR_AUTHN_FAILED, NULL,
                "No more credentials or we tried too many times.\n"
                "Authentication failed");
    }

  simple_creds = creds;

  /* Store the digest authentication parameters in the context relative
     to this connection, so we can use it to create the Authorization header 
     when setting up requests. */
  if (conn->auth_context)
    context = conn->auth_context;
  else
    context = 
      (serf_digest_context_t *)apr_pcalloc(session->pool, 
					   sizeof(serf_digest_context_t));

  context->pool = session->pool;
  context->qop = qop;
  context->nonce = nonce;
  context->cnonce = NULL;
  context->opaque = opaque;
  context->algorithm = algorithm;
  context->realm = apr_pstrdup(context->pool, realm_name);
  context->username = apr_pstrdup(context->pool, simple_creds->username);
  context->digest_nc++;

  conn->auth_context = context;

  context->ha1 = build_digest_ha1(simple_creds, context->realm,
				  context->pool);

  /* If the handshake is finished tell serf it can send as much requests as it
     likes. */
  serf_connection_set_max_outstanding_requests(conn->conn, 0);

  return SVN_NO_ERROR;
}

svn_error_t *
init_digest_connection(svn_ra_serf__session_t *session,
                       svn_ra_serf__connection_t *conn,
                       apr_pool_t *pool)
{
  /* Make serf send the initial requests one by one */
  serf_connection_set_max_outstanding_requests(conn->conn, 1);

  conn->auth_context = NULL;

  return SVN_NO_ERROR;
}

svn_error_t *
setup_request_digest_auth(svn_ra_serf__connection_t *conn,
                          const char *method,
			  const char *uri,
                          serf_bucket_t *hdrs_bkt)
{
  serf_digest_context_t *context = (serf_digest_context_t *)conn->auth_context;

  if (context)
    {
      /* Build a new Authorization header. */
      conn->auth_header = "Authorization";
      conn->auth_value = build_auth_header(context, uri, method, 
					   context->pool);

      serf_bucket_headers_setn(hdrs_bkt, conn->auth_header, conn->auth_value);
      context->digest_nc++;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
validate_response_digest_auth(svn_ra_serf__handler_t *ctx,
			      serf_request_t *request,
			      serf_bucket_t *response,
			      apr_pool_t *pool)
{
  svn_ra_serf__connection_t *conn = ctx->conn;
  serf_digest_context_t *context = (serf_digest_context_t *)conn->auth_context;
  char *kv, *key, *val;
  char *auth_attr, *nextkv, *rspauth = NULL, *qop = NULL, *nc_str = NULL;
  serf_bucket_t *hdrs;

  hdrs = serf_bucket_response_get_headers(response);
  auth_attr = (char*)serf_bucket_headers_get(hdrs, "Authentication-Info");

  /* We're expecting a list of key=value pairs, separated by a comma. 
     Ex. rspauth="8a4b8451084b082be6b105e2b7975087", 
         cnonce="346531653132652d303033392d3435", nc=00000007,
         qop=auth */
  while ((kv = apr_strtok(auth_attr, ",", &nextkv)))
    {
      key = apr_strtok(kv, "=", &val);
      /* skip leading spaces */
      while (*key && *key == ' ') key++;
      
      if (strcmp(key, "rspauth") == 0)
	{
	  rspauth = apr_strtok(NULL, "=", &val);
	  if (rspauth[0] == '\"')
	    {
	      apr_size_t str_len;
              
	      str_len = strlen(rspauth);
	      if (rspauth[str_len - 1] == '\"')
		{
		  rspauth[str_len - 1] = '\0';
		  rspauth++;
		}
	    }
	}
      if (strcmp(key, "qop") == 0)
	{
	  qop = apr_pstrdup(pool, val);
	  if (qop[0] == '\"')
	    {
	      apr_size_t qop_len;
              
	      qop_len = strlen(qop);
	      if (qop[qop_len - 1] == '\"')
		{
		  qop[qop_len - 1] = '\0';
		  qop++;
		}
	    }
	}
      if (strcmp(key, "nc") == 0)
	{
	  nc_str = apr_pstrdup(pool, val);
	  if (nc_str[0] == '\"')
	    {
	      apr_size_t nc_len;
              
	      nc_len = strlen(nc_str);
	      if (nc_str[nc_len - 1] == '\"')
		{
		  nc_str[nc_len - 1] = '\0';
		  nc_str++;
		}
	    }
	}

      auth_attr = NULL;
    }

  if (rspauth) 
    {
      char *ha2, *tmp, *resp_hdr_hex;
      unsigned char resp_hdr[APR_MD5_DIGESTSIZE];

      ha2 = build_digest_ha2(ctx->path, "", qop, pool);
      tmp = apr_psprintf(pool, "%s:%s:%s:%s:%s:%s",
			 context->ha1, context->nonce, nc_str,
			 context->cnonce, context->qop, ha2);
      apr_md5(resp_hdr, tmp, strlen(tmp));
      resp_hdr_hex =  hex_encode(resp_hdr, pool);

      if (strcmp(rspauth, resp_hdr_hex) != 0)
	{
	  return svn_error_create
	    (SVN_ERR_AUTHN_FAILED, NULL,
	     _("Incorrect response-digest in Authentication-Info header."));
	}
    }

  return SVN_NO_ERROR;
}
