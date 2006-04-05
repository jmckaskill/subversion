/*
 * cram.c :  Minimal standalone CRAM-MD5 implementation
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
#define APR_WANT_STDIO
#include <apr_want.h>
#include <apr_general.h>
#include <apr_strings.h>
#include <apr_network_io.h>
#include <apr_time.h>
#include <apr_md5.h>

#include <svn_types.h>
#include <svn_string.h>
#include <svn_error.h>
#include <svn_ra_svn.h>
#include <svn_config.h>
#include <svn_utf.h>
#include "svn_private_config.h"

#include "ra_svn.h"

#define FAILURE_STR \
        "\x66\x61\x69\x6c\x75\x72\x65"
        /* "failure" */

#define INERNAL_SERVER_ERR_STR \
        "\x49\x6e\x74\x65\x72\x6e\x61\x6c\x20\x73\x65\x72\x76\x65\x72\x20" \
        "\x65\x72\x72\x6f\x72\x20\x69\x6e\x20\x61\x75\x74\x68\x65\x6e\x74" \
        "\x69\x63\x61\x74\x69\x6f\x6e"
        /* "Internal server error in authentication" */

#define MALFORMED_CLIENT_RESP_STR \
        "\x4d\x61\x6c\x66\x6f\x72\x6d\x65\x64\x20\x63\x6c\x69\x65\x6e\x74" \
        "\x20\x72\x65\x73\x70\x6f\x6e\x73\x65\x20\x69\x6e\x20\x61\x75\x74" \
        "\x68\x65\x6e\x74\x69\x63\x61\x74\x69\x6f\x6e"
        /* "Malformed client response in authentication" */

#define PASSWORD_INCORRECT_STR \
        "\x50\x61\x73\x73\x77\x6f\x72\x64\x20\x69\x6e\x63\x6f\x72\x72\x65" \
        "\x63\x74"
        /* "Password incorrect" */
        
#define STEP_STR \
        "\x73\x74\x65\x70"
        /* "step" */

#define SUCCESS_STR \
        "\x73\x75\x63\x63\x65\x73\x73"
        /* "success" */
        
#define USER_NOT_FOUND_STR \
        "\x55\x73\x65\x72\x6e\x61\x6d\x65\x20\x6e\x6f\x74\x20\x66\x6f\x75" \
        "\x6e\x64"
        /* "Username not found" */
        
                
static int hex_to_int(char c)
{
  return (c >= SVN_UTF8_0 && c <= SVN_UTF8_9) ? c - SVN_UTF8_0
    : (c >= SVN_UTF8_a && c <= SVN_UTF8_f) ? c - SVN_UTF8_a + 10
    : -1;
}

static char int_to_hex(int v)
{
  return (v < 10) ? SVN_UTF8_0 + v : SVN_UTF8_a + (v - 10);
}

static svn_boolean_t hex_decode(unsigned char *hashval, const char *hexval)
{
  int i, h1, h2;

  for (i = 0; i < APR_MD5_DIGESTSIZE; i++)
    {
      h1 = hex_to_int(hexval[2 * i]);
      h2 = hex_to_int(hexval[2 * i + 1]);
      if (h1 == -1 || h2 == -1)
        return FALSE;
      hashval[i] = (h1 << 4) | h2;
    }
  return TRUE;
}

static void hex_encode(char *hexval, const unsigned char *hashval)
{
  int i;

  for (i = 0; i < APR_MD5_DIGESTSIZE; i++)
    {
      hexval[2 * i] = int_to_hex((hashval[i] >> 4) & 0xf);
      hexval[2 * i + 1] = int_to_hex(hashval[i] & 0xf);
    }
}

static void compute_digest(unsigned char *digest, const char *challenge,
                           const char *password)
{
  unsigned char secret[64];
  apr_size_t len = strlen(password), i;
  apr_md5_ctx_t ctx;

  /* Munge the password into a 64-byte secret. */
  memset(secret, 0, sizeof(secret));
  if (len <= sizeof(secret))
    memcpy(secret, password, len);
  else
    apr_md5(secret, password, len);

  /* Compute MD5(secret XOR opad, MD5(secret XOR ipad, challenge)),
   * where ipad is a string of 0x36 and opad is a string of 0x5c. */
  for (i = 0; i < sizeof(secret); i++)
    secret[i] ^= 0x36;
  apr_md5_init(&ctx);
  apr_md5_update(&ctx, secret, sizeof(secret));
  apr_md5_update(&ctx, challenge, strlen(challenge));
  apr_md5_final(digest, &ctx);
  for (i = 0; i < sizeof(secret); i++)
    secret[i] ^= (0x36 ^ 0x5c);
  apr_md5_init(&ctx);
  apr_md5_update(&ctx, secret, sizeof(secret));
  apr_md5_update(&ctx, digest, APR_MD5_DIGESTSIZE);
  apr_md5_final(digest, &ctx);
}

/* Fail the authentication, from the server's perspective. */
static svn_error_t *fail(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                         const char *msg)
{
  SVN_ERR(svn_ra_svn_write_tuple(conn, pool, "w(c)", FAILURE_STR, msg));
  return svn_ra_svn_flush(conn, pool);
}

/* If we can, make the nonce with random bytes.  If we can't... well,
 * it just has to be different each time.  The current time isn't
 * absolutely guaranteed to be different for each connection, but it
 * should prevent replay attacks in practice. */
static apr_status_t make_nonce(apr_uint64_t *nonce)
{
#if APR_HAS_RANDOM
  return apr_generate_random_bytes((unsigned char *) nonce, sizeof(*nonce));
#else
  *nonce = apr_time_now();
  return APR_SUCCESS;
#endif
}

svn_error_t *svn_ra_svn_cram_server(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                    svn_config_t *pwdb, const char **user,
                                    svn_boolean_t *success)
{
  apr_status_t status;
  apr_uint64_t nonce;
  char hostbuf[APRMAXHOSTLEN + 1];
  unsigned char cdigest[APR_MD5_DIGESTSIZE], sdigest[APR_MD5_DIGESTSIZE];
  const char *challenge, *sep, *password;
  svn_ra_svn_item_t *item;
  svn_string_t *resp;

  *success = FALSE;

  /* Send a challenge. */
  status = make_nonce(&nonce);
  if (!status)
    status = apr_gethostname(hostbuf, sizeof(hostbuf), pool);
  if (status)
    return fail(conn, pool, INERNAL_SERVER_ERR_STR);

  challenge = apr_psprintf(pool,
                           "<%" APR_UINT64_T_FMT ".%" APR_TIME_T_FMT
                           "@%s>", nonce, apr_time_now(), hostbuf);                           
#if APR_CHARSET_EBCDIC
  SVN_ERR (svn_utf_cstring_to_utf8(&challenge, challenge, pool));
#endif
                           
  SVN_ERR(svn_ra_svn_write_tuple(conn, pool, "w(c)", STEP_STR, challenge));

  /* Read the client's response and decode it into *user and cdigest. */
  SVN_ERR(svn_ra_svn_read_item(conn, pool, &item));
  if (item->kind != SVN_RA_SVN_STRING)  /* Very wrong; don't report failure */
    return SVN_NO_ERROR;
  resp = item->u.string;
  sep = strrchr(resp->data, SVN_UTF8_SPACE);
  if (!sep || resp->len - (sep + 1 - resp->data) != APR_MD5_DIGESTSIZE * 2
      || !hex_decode(cdigest, sep + 1))
    return fail(conn, pool, MALFORMED_CLIENT_RESP_STR);
  *user = apr_pstrmemdup(pool, resp->data, sep - resp->data);
  
  /* Verify the digest against the password in pwfile. */
  svn_config_get(pwdb, &password, SVN_CONFIG_SECTION_USERS, *user, NULL);
  if (!password)
    return fail(conn, pool, USER_NOT_FOUND_STR);
  compute_digest(sdigest, challenge, password);
  if (memcmp(cdigest, sdigest, sizeof(sdigest)) != 0)
    return fail(conn, pool, PASSWORD_INCORRECT_STR);
  *success = TRUE;
  return svn_ra_svn_write_tuple(conn, pool, "w()", SUCCESS_STR);
}

svn_error_t *svn_ra_svn__cram_client(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                     const char *user, const char *password,
                                     const char **message)
{
  const char *status, *str, *reply;
  unsigned char digest[APR_MD5_DIGESTSIZE];
  char hex[2 * APR_MD5_DIGESTSIZE + 1];

  /* Read the server challenge. */
  SVN_ERR(svn_ra_svn_read_tuple(conn, pool,
                                "w(?c)", &status, &str));
  if (strcmp(status, FAILURE_STR) == 0 && str)
    {
      *message = str;
      return SVN_NO_ERROR;
    }
  else if (strcmp(status, STEP_STR) != 0 || !str)
    return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, NULL,
                            _("Unexpected server response to authentication"));

  /* Write our response. */
  compute_digest(digest, str, password);
  hex_encode(hex, digest);
  hex[sizeof(hex) - 1] = '\0';
  reply = apr_psprintf(pool, "%s\x20%s", user, hex);
  SVN_ERR(svn_ra_svn_write_cstring(conn, pool, reply));

  /* Read the success or failure response from the server. */
  SVN_ERR(svn_ra_svn_read_tuple(conn, pool,
                                "w(?c)", &status, &str));
  if (strcmp(status, FAILURE_STR) == 0 && str)
    {
      *message = str;
      return SVN_NO_ERROR;
    }
  else if (strcmp(status, SUCCESS_STR) != 0 ||
           str)
    return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, NULL,
                            _("Unexpected server response to authentication"));

  *message = NULL;
  return SVN_NO_ERROR;
}
