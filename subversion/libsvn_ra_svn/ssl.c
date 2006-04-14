/*
 * ssl.c :  SSL routines for Subversion protocol
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


#include <assert.h>
#include <stdlib.h>

#define APR_WANT_STRFUNC
#include <apr_want.h>
#include <apr_general.h>
#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_network_io.h>
#include <apr_poll.h>

#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "svn_io.h"
#include "svn_base64.h"
#include "svn_private_config.h"

#include "ra_svn.h"

#ifndef CIPHER_LIST
#define CIPHER_LIST "ALL:!LOW"
#endif

#ifdef SVN_HAVE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/x509v3.h>

#ifndef BUFFER_SIZE
#define BUFFER_SIZE 8192
#endif

#ifndef CERT_BUFFER_SIZE
#define CERT_BUFFER_SIZE 256
#endif

/* Baton for a SSL stream connection. */
typedef struct ssl_baton {
  svn_ra_svn_stream_t *in;       /* Inherited input stream. */
  svn_ra_svn_stream_t *out;      /* Inherited output stream. */
  SSL *ssl;                      /* The SSL to use for this connection. */
  BIO *internal_bio;             /* The Subversion/SSL side of a BIO pair. */
  BIO *network_bio;              /* The network side of a BIO pair. */
} ssl_baton_t;


/* The interface layer between network and BIO-pair. The BIO-pair buffers
 * the data to/from the TLS layer. Hence, at any time, there may be data
 * in the buffer that must be written to the network. This writing has
 * highest priority because the handshake might fail otherwise.
 * Only then a read_request can be satisfied.
 * Adapted from network_biopair_interop() in postfixtls patch by Lutz Jaenicke
 * at http://www.aet.tu-cottbus.de/personen/jaenicke/postfix_tls/ */
static svn_error_t *
network_biopair_interop(ssl_baton_t *conn)
{
  char buffer[BUFFER_SIZE];
  int want_write;
  int write_pos;
  int from_bio;
  int want_read;
  int to_bio;

  while ((want_write = BIO_ctrl_pending(conn->network_bio)) > 0)
    {
      if (want_write > BUFFER_SIZE)
          want_write = BUFFER_SIZE;
        from_bio = BIO_read(conn->network_bio, buffer, want_write);

      /* Write the complete contents of the buffer. Since TLS performs
       * underlying handshaking, we cannot afford to leave the buffer
       * unflushed, as we could run into a deadlock trap (the peer
       * waiting for a final byte and we already waiting for his reply
       * in read position). */
      write_pos = 0;
      do {
          apr_size_t num_write = from_bio - write_pos;
          SVN_ERR(svn_ra_svn__stream_write(conn->out, buffer + write_pos,
                                           &num_write));
          write_pos += num_write;
      } while (write_pos < from_bio);
    }

  while ((want_read = BIO_ctrl_get_read_request(conn->network_bio)) > 0)
    {
      if (want_read > BUFFER_SIZE)
          want_read = BUFFER_SIZE;

      apr_size_t num_read = want_read;

      SVN_ERR(svn_ra_svn__stream_read(conn->in, buffer, &num_read));

      if (num_read == 0)
        return svn_error_create(SVN_ERR_RA_SVN_CONNECTION_CLOSED, NULL,
                                _("Connection closed unexpectedly"));

      to_bio = BIO_write(conn->network_bio, buffer, num_read);
      if (to_bio != num_read)
        return svn_error_create(SVN_ERR_RA_SVN_SSL_ERROR, NULL,
                        _("Failed to read from SSL socket layer"));
    }

  return SVN_NO_ERROR;
}

/* Function to perform the handshake for SSL_accept(), SSL_connect(),
 * and SSL_shutdown() and perform the SSL_read(), SSL_write() operations.
 * Call the underlying network_biopair_interop-layer to make sure the
 * write buffer is flushed after every operation (that did not fail with
 * a fatal error).
 * The value of *count will be the result of the underlying SSL_operation(),
 * and for SSL_read/SSL_write this will be number of bytes read/written
 * if the operation was successfull.
 *
 * Adapted from do_tls_operation() in postfixtls patch by Lutz Jaenicke
 * at http://www.aet.tu-cottbus.de/personen/jaenicke/postfix_tls/ */
static svn_error_t *
do_ssl_operation(ssl_baton_t *conn,
                 int (*hsfunc)(SSL *),
                 int (*rfunc)(SSL *, void *, int),
                 int (*wfunc)(SSL *, const void *, int),
                 char *buffer, int *count)
{
  int status;
  int ssl_err;
  int count_val;
  int ret_status = -1;
  svn_boolean_t done = FALSE;

  count_val = (count == NULL) ? 0 : *count;

  while (!done)
    {
      if (hsfunc)
         status = hsfunc(conn->ssl);
      else if (rfunc)
        status = rfunc(conn->ssl, buffer, count_val);
      else
        status = wfunc(conn->ssl, (const char *)buffer, count_val);
      ssl_err = SSL_get_error(conn->ssl, status);

      switch (ssl_err)
        {
          case SSL_ERROR_NONE:          /* success */
            ret_status = status;
            done = TRUE;                /* no break, flush buffer before */
                                        /* leaving */
          case SSL_ERROR_WANT_WRITE:
          case SSL_ERROR_WANT_READ:
            SVN_ERR(network_biopair_interop(conn));
            break;
          case SSL_ERROR_ZERO_RETURN:   /* connection was closed cleanly */
          case SSL_ERROR_SYSCALL:
          case SSL_ERROR_SSL:
          default:
            ret_status = status;
            done = TRUE;
        };
    };

  if (count != NULL)
    *count = ret_status;

  if (ret_status > 0)
    return SVN_NO_ERROR;
  else
    return svn_error_create(SVN_ERR_RA_SVN_SSL_ERROR, NULL,
                            _("SSL network problem"));
}

/* Format an ASN1 time to a string. */
static svn_boolean_t
asn1time_to_string(ASN1_TIME *tm, char *buffer, apr_size_t len)
{
  svn_boolean_t retval = FALSE;
  if (len--)
    {
      BIO *mem = BIO_new(BIO_s_mem());
      if (mem)
        {
          if (ASN1_TIME_print(mem, tm))
            {
              long data_len;
              char *data;
              data_len = BIO_get_mem_data(mem, &data);
              if (data_len < len)
                len = data_len;
              memcpy(buffer, data, len);
              buffer[len] = 0;
              retval = TRUE;
            }
          BIO_free(mem);
        }
    }
    return retval;
}

/* Compare peername against hostname. Allow wildcard in leftmost
 * position in peername, and the comparision is case insensitive. */
static svn_boolean_t
match_hostname(const char *peername, const char *hostname, apr_pool_t *pool)
{
  char *str, *last_str;
  char *hostname_copy;

  if (apr_strnatcasecmp(peername, hostname) == 0)
    return TRUE;

  if (strlen(peername) < 3)
    return FALSE;

  if (peername[0] != '*' || peername[1] != '.')
    return FALSE;

  hostname_copy = apr_pstrdup(pool, hostname);
  str = apr_strtok (hostname_copy, ".", &last_str);
  if (str == NULL)
    return FALSE;

  return (apr_strnatcasecmp(&peername[2], str) == 0);
}

/* Verify that the certificates common name matches the hostname.
 * Adapted from verify_callback() in postfixtls patch by Lutz Jaenicke
 * at http://www.aet.tu-cottbus.de/personen/jaenicke/postfix_tls/ */
static svn_boolean_t
verify_hostname(ssl_baton_t *ssl_baton,
                apr_pool_t *pool,
                const char *hostname,
                svn_auth_ssl_server_cert_info_t *cert_info)
{
  int i, r;
  svn_boolean_t matched = FALSE;
  svn_boolean_t dnsname_found = FALSE;
  X509 *peer = SSL_get_peer_certificate(ssl_baton->ssl);
  STACK_OF(GENERAL_NAME) *gens;

  /* Check out the name certified against the hostname expected.
   * Standards are not always clear with respect to the handling of
   * dNSNames. RFC3207 does not specify the handling. We therefore follow
   * the strict rules in RFC2818 (HTTP over TLS), Section 3.1:
   * The Subject Alternative Name/dNSName has precedence over CommonName
   * (CN). If dNSName entries are provided, CN is not checked anymore. */
  gens = X509_get_ext_d2i(peer, NID_subject_alt_name, 0, 0);
  if (gens)
    {
      for (i = 0, r = sk_GENERAL_NAME_num(gens); i < r; i++)
        {
          const GENERAL_NAME *gn = sk_GENERAL_NAME_value(gens, i);
          if (gn->type == GEN_DNS)
            {
              dnsname_found = TRUE;
              matched = match_hostname((const char *)gn->d.ia5->data,
                                       hostname, pool);
              if (matched)
                break;
            }
        }
      sk_GENERAL_NAME_free(gens);
      if (dnsname_found)
        return matched;
    }

  return match_hostname(cert_info->hostname, hostname, pool);
}

static svn_error_t *
fill_cert_info(ssl_baton_t *ssl_baton, apr_pool_t *pool, const char *hostname,
               svn_auth_ssl_server_cert_info_t *cert_info,
               apr_uint32_t *cert_failures)
{
  const char hexcodes[] = "0123456789ABCDEF";
  X509 *peer;
  char buffer[CERT_BUFFER_SIZE];
  unsigned char md[EVP_MAX_MD_SIZE];
  char fingerprint[EVP_MAX_MD_SIZE * 3];
  unsigned int md_size;
  unsigned int i;
  unsigned int cert_buffer_size;
  const svn_string_t *ascii_cert;
  long verify_result;

  cert_info->hostname = NULL;
  cert_info->fingerprint = NULL;
  cert_info->valid_from = NULL;
  cert_info->valid_until = NULL;
  cert_info->issuer_dname = NULL;
  cert_info->ascii_cert = NULL;

  *cert_failures = 0;

  peer = SSL_get_peer_certificate(ssl_baton->ssl);
  if (peer == NULL)
    return svn_error_create(SVN_ERR_RA_SVN_SSL_ERROR, NULL,
                            _("Unable to obtain server certificate"));


  if (!X509_NAME_get_text_by_NID(X509_get_subject_name(peer),
                                 NID_commonName, buffer, CERT_BUFFER_SIZE))
    return svn_error_create(SVN_ERR_RA_SVN_SSL_ERROR, NULL,
                            _("Could not obtain server certificate CN"));
  cert_info->hostname = apr_pstrdup(pool, buffer);

  if (!X509_NAME_get_text_by_NID(X509_get_issuer_name(peer),
                                 NID_commonName, buffer, CERT_BUFFER_SIZE))
    {
      if (!X509_NAME_get_text_by_NID(X509_get_issuer_name(peer),
                                     NID_organizationName, buffer,
                                     CERT_BUFFER_SIZE))
        return svn_error_create(SVN_ERR_RA_SVN_SSL_ERROR, NULL,
                                _("Could not obtain server certificate issuer "
                                  "or organization"));
    }
  cert_info->issuer_dname = apr_pstrdup(pool, buffer);

  /* Neon uses sha1 for calculating fingerprint, and not MD5. */
  if (X509_digest(peer, EVP_sha1(), md, &md_size))
    {
      for (i=0; i<md_size; i++)
        {
          fingerprint[3*i] = hexcodes[(md[i] & 0xf0) >> 4];
          fingerprint[(3*i)+1] = hexcodes[(md[i] & 0x0f)];
          fingerprint[(3*i)+2] = ':';
        }
      if (md_size > 0)
        fingerprint[(3*(md_size-1))+2] = '\0';
      else
        fingerprint[0] = '\0';
      cert_info->fingerprint = apr_pstrdup(pool, fingerprint);
    }
  else
    cert_info->fingerprint = apr_pstrdup(pool, "<unknown>");

  cert_buffer_size = i2d_X509(peer, NULL);
  if (cert_buffer_size > 0)
    {
      svn_string_t certdata;
      unsigned char *derbuffer = apr_pcalloc(pool, cert_buffer_size);
      unsigned char *p = derbuffer;

      i2d_X509(peer, &p);
      certdata.data = (char *)derbuffer;
      certdata.len = cert_buffer_size;
      ascii_cert = svn_base64_encode_string(&certdata, pool);
      cert_info->ascii_cert = ascii_cert->data;
    }

  /* Read the certificate validity dates, but keep the output format
   * same as in Neon. */
  if (asn1time_to_string(X509_get_notBefore(peer), buffer, CERT_BUFFER_SIZE))
    cert_info->valid_from = apr_pstrdup(pool, buffer);
  else
    cert_info->valid_from = apr_pstrdup(pool, "[invalid date]");

  if (asn1time_to_string(X509_get_notAfter(peer), buffer, CERT_BUFFER_SIZE))
    cert_info->valid_until = apr_pstrdup(pool, buffer);
  else
    cert_info->valid_until = apr_pstrdup(pool, "[invalid date]");

  /* Now we start checking the certificate. Similarly as done in Neon.*/
  if (X509_cmp_current_time(X509_get_notBefore(peer)) >= 0)
    *cert_failures |= SVN_AUTH_SSL_NOTYETVALID;
  else if (X509_cmp_current_time(X509_get_notAfter(peer)) <= 0)
    *cert_failures |= SVN_AUTH_SSL_EXPIRED;

  /* Only the last verification failure will be returned from
   * SSL_get_verify_result, even thugh there may be several errors. */
  verify_result = SSL_get_verify_result(ssl_baton->ssl);
  switch (verify_result)
    {
      case X509_V_OK:
      case X509_V_ERR_CERT_NOT_YET_VALID:
      case X509_V_ERR_CERT_HAS_EXPIRED:
        break;
      case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
      case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
      case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        *cert_failures |= SVN_AUTH_SSL_UNKNOWNCA;
        break;
      default:
        *cert_failures |= SVN_AUTH_SSL_OTHER;
    }

  if (!verify_hostname(ssl_baton, pool, hostname, cert_info))
    *cert_failures |= SVN_AUTH_SSL_CNMISMATCH;

  return SVN_NO_ERROR;
}


static void
ssl_timeout_cb(void *baton, apr_interval_time_t interval)
{
  ssl_baton_t *ssl_baton = baton;
  svn_ra_svn__stream_timeout(ssl_baton->out, interval);
}

static svn_boolean_t
ssl_pending_cb(void *baton)
{
  ssl_baton_t *ssl_baton = baton;
  int n;

  /* Note that SSL_pending may return number of bytes to read,
   * even if the data is not application data. */
  svn_error_t *err = do_ssl_operation(ssl_baton, SSL_pending,
                                      NULL, NULL, NULL, &n);
  if (!err)
    return n > 0;

  svn_error_clear(err);
  return FALSE;
}


static svn_error_t *
ssl_read_cb(void *baton, char *buffer, apr_size_t *len)
{
  ssl_baton_t *ssl_baton = baton;
  apr_size_t toread = *len;

  int block = toread > BUFFER_SIZE ? BUFFER_SIZE : toread;
  SVN_ERR(do_ssl_operation(ssl_baton, NULL, SSL_read, NULL, buffer,
                           &block));
  *len = block;
  if (*len == 0)
    return svn_error_create(SVN_ERR_RA_SVN_CONNECTION_CLOSED, NULL,
                            _("Connection closed unexpectedly"));
  return SVN_NO_ERROR;
}


static svn_error_t *
ssl_write_cb(void *baton, const char *buffer, apr_size_t *len)
{
  ssl_baton_t *ssl_baton = baton;
  apr_size_t towrite = *len;
  apr_size_t written = 0;
  if (towrite <= 0)
    *len = 0;
  else while (written < towrite)
    {
      apr_size_t remainder = towrite - written;
      int block = remainder > BUFFER_SIZE ? BUFFER_SIZE : remainder;
      SVN_ERR(do_ssl_operation(ssl_baton, NULL, NULL, SSL_write,
                               (char *) buffer, &block));
      written += block;
      buffer += block;
      *len = written;
    }
  return SVN_NO_ERROR;
}

/* Releases the resources allocated by SSL. */
static apr_status_t
cleanup_ssl(void *data)
{
  ssl_baton_t *conn = data;

  /* The connection has been setup between client and server,
   * so we tell the other side that we are finished. */
  if (!do_ssl_operation(conn, SSL_shutdown, NULL, NULL, NULL, NULL))
    do_ssl_operation(conn, SSL_shutdown, NULL, NULL, NULL, NULL);

  /* Implicitely frees conn->internal_bio. */
  SSL_free(conn->ssl);
  BIO_free(conn->network_bio);

  return APR_SUCCESS;
}

static svn_error_t *
wrap_conn(svn_ra_svn_conn_t *conn, SSL_CTX *ctx, ssl_baton_t **result,
          apr_pool_t *pool)
{
  ssl_baton_t *ssl_baton = apr_palloc(pool, sizeof(*ssl_baton));

  ssl_baton->in = conn->in_stream;
  ssl_baton->out = conn->out_stream;
  ssl_baton->ssl = SSL_new(ctx);
  if (ssl_baton->ssl == NULL)
    return svn_error_create(SVN_ERR_RA_SVN_SSL_INIT, NULL,
                            "Could not create a SSL from the SSL context");

  if (!BIO_new_bio_pair(&ssl_baton->internal_bio, BUFFER_SIZE,
                        &ssl_baton->network_bio, BUFFER_SIZE))
    {
      SSL_free(ssl_baton->ssl);
      return svn_error_create(SVN_ERR_RA_SVN_SSL_INIT, NULL,
                              "Could not create a a new BIO pair");
    }

  SSL_set_bio(ssl_baton->ssl, ssl_baton->internal_bio, ssl_baton->internal_bio);

  /* Need to release SSL resources when the connection is destroyed.
   * Assumes that the owning SSL_CTX is destroyed after cleanup
   * of SSL. */
  apr_pool_cleanup_register(pool, ssl_baton, cleanup_ssl,
                            apr_pool_cleanup_null);


  conn->in_stream = svn_ra_svn__stream_create(ssl_baton, ssl_read_cb,
                                              ssl_write_cb, ssl_timeout_cb,
                                              ssl_pending_cb, pool);
  conn->out_stream = conn->in_stream;
  *result = ssl_baton;
  return SVN_NO_ERROR;
}


/* Frees the allocated SSL context.
 * Assumes that this is called after freeing of SSL
 * allocated by svn_ra_svn_create_conn. */
static apr_status_t
destroy_ssl_ctx(void *data)
{
  SSL_CTX *ssl_ctx = data;
  if (ssl_ctx != NULL)
    SSL_CTX_free(ssl_ctx);
  return APR_SUCCESS;
}


/* Authenticate the server certificate. */
static svn_error_t *
server_auth(ssl_baton_t *ssl_baton, svn_auth_baton_t *auth_baton,
            const char *hostname, const char *realm, apr_pool_t *pool)
{
  svn_auth_iterstate_t *state;
  void *creds;
  svn_error_t *error;
  apr_uint32_t *cert_failures = apr_palloc (pool, sizeof(*cert_failures));

  svn_auth_ssl_server_cert_info_t *cert_info =
    apr_palloc(pool, sizeof(*cert_info));

  SVN_ERR(fill_cert_info(ssl_baton, pool, hostname, cert_info,
                         cert_failures));

  svn_auth_set_parameter(auth_baton, SVN_AUTH_PARAM_SSL_SERVER_FAILURES,
                         cert_failures);

  svn_auth_set_parameter(auth_baton, SVN_AUTH_PARAM_SSL_SERVER_CERT_INFO,
                         cert_info);

  SVN_ERR(svn_auth_first_credentials(&creds, &state,
                                     SVN_AUTH_CRED_SSL_SERVER_TRUST,
                                     realm, auth_baton, pool));

  if (!creds)
        return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, NULL,
                                _("Server certificate rejected"));

  error = svn_auth_save_credentials(state, pool);

  svn_auth_set_parameter(auth_baton, SVN_AUTH_PARAM_SSL_SERVER_CERT_INFO, NULL);

  return error;
}


static svn_error_t *
ssl_create(svn_boolean_t server, SSL_CTX **result, apr_pool_t *pool)
{
  SSL_CTX *ctx = SSL_CTX_new(server? SSLv23_server_method()
                                   : SSLv23_client_method());
  if (ctx == NULL)
    return svn_error_create(SVN_ERR_RA_SVN_SSL_INIT, NULL,
                            _("Could not obtain an SSL client context"));
  apr_pool_cleanup_register(pool, ctx, destroy_ssl_ctx,
                            apr_pool_cleanup_null);

  if (SSL_CTX_set_cipher_list(ctx, CIPHER_LIST) != 1)
    return svn_error_createf(SVN_ERR_RA_SVN_SSL_INIT, NULL,
                             _("Could not set SSL cipher list to '%s'"),
                             CIPHER_LIST);
  *result = ctx;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_ra_svn__conn_ssl_client(svn_ra_svn_conn_t *conn,
                            svn_auth_baton_t *auth_baton,
                            const char *hostname,
                            const char *realm,
                            apr_pool_t *pool)
{
  SSL_CTX *ctx;
  ssl_baton_t *baton;

  SVN_ERR(ssl_create(FALSE, &ctx, pool));
  SVN_ERR(wrap_conn(conn, ctx, &baton, pool));
  SVN_ERR(do_ssl_operation(baton, SSL_connect, NULL, NULL, NULL, NULL));
  SVN_ERR(server_auth(baton, auth_baton, hostname, realm, pool));
  return SVN_NO_ERROR;
}

/* Helper method for more verbose SSL errors. */
static const char *
ssl_last_error(apr_pool_t *pool)
{
  unsigned long ssl_error = ERR_get_error();
  char *buffer;
  
  if (ssl_error == 0)
    return strerror(errno);

  buffer = apr_pcalloc(pool, 256);  
  ERR_error_string_n(ssl_error, buffer, 256);
  return buffer;
}


svn_error_t *
svn_ra_svn_conn_ssl_server(svn_ra_svn_conn_t *conn, const char *cert,
                           const char *key, apr_pool_t *pool)
{
  SSL_CTX *ctx;
  ssl_baton_t *baton;

  svn_ra_svn_flush(conn, pool);

  SVN_ERR(ssl_create(TRUE, &ctx, pool));

  if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1)
    return svn_error_createf(SVN_ERR_RA_SVN_SSL_INIT, NULL,
                             "Could not load SSL certificate from '%s': %s.",
                             cert, ssl_last_error(pool));

  if (SSL_CTX_use_RSAPrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1)
    return svn_error_createf(SVN_ERR_RA_SVN_SSL_INIT, NULL,
                             "Could not load SSL key from '%s': %s.",
                             key, ssl_last_error(pool));

  if (!SSL_CTX_check_private_key(ctx))
    return svn_error_createf(SVN_ERR_RA_SVN_SSL_INIT, NULL,
                             "Could not verify SSL key: %s.",
                             ssl_last_error(pool));

  SVN_ERR(wrap_conn(conn, ctx, &baton, pool));
  SVN_ERR(do_ssl_operation(baton, SSL_accept, NULL, NULL, NULL, NULL));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_ra_svn__ssl_initialize(apr_pool_t *pool)
{
  SSL_load_error_strings();
  SSL_library_init();
  return SVN_NO_ERROR;
}

#endif /* SVN_HAVE_SSL */
