/*
 * session.c :  routines for maintaining sessions state (to the DAV server)
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

#include <apr_pools.h>
#define APR_WANT_STRFUNC
#include <apr_want.h>
#include <apr_general.h>

#include <ne_socket.h>
#include <ne_request.h>
#include <ne_uri.h>
#include <ne_auth.h>

#include "svn_error.h"
#include "svn_ra.h"
#include "svn_config.h"
#include "svn_delta.h"
#include "svn_version.h"
#include "svn_path.h"
#include "svn_private_config.h"

#include "ra_dav.h"

#define DEFAULT_HTTP_TIMEOUT 3600


/* a cleanup routine attached to the pool that contains the RA session
   baton. */
static apr_status_t cleanup_session(void *sess)
{
  ne_session_destroy(sess);
  return APR_SUCCESS;
}

/* a cleanup routine attached to the pool that contains the RA session
   root URI. */
static apr_status_t cleanup_uri(void *uri)
{
  ne_uri_free(uri);
  return APR_SUCCESS;
}

/* A neon-session callback to 'pull' authentication data when
   challenged.  In turn, this routine 'pulls' the data from the client
   callbacks if needed.  */
static int request_auth(void *userdata, const char *realm, int attempt,
                        char *username, char *password)
{
  svn_error_t *err;
  svn_ra_session_t *ras = userdata;
  void *creds;
  svn_auth_cred_simple_t *simple_creds;  

  /* No auth_baton?  Give up. */
  if (! ras->callbacks->auth_baton)
    return -1;

  if (attempt == 0)
    {
      const char *realmstring;

      /* <https://svn.collab.net:80> Subversion repository */
      realmstring = apr_psprintf (ras->pool, "<%s://%s:%d> %s",
                                  ras->root.scheme, ras->root.host,
                                  ras->root.port, realm);

      err = svn_auth_first_credentials (&creds,
                                        &(ras->auth_iterstate), 
                                        SVN_AUTH_CRED_SIMPLE,
                                        realmstring,
                                        ras->callbacks->auth_baton,
                                        ras->pool);
    }

  else /* attempt > 0 */
    /* ### TODO:  if the http realm changed this time around, we
       should be calling first_creds(), not next_creds(). */
    err = svn_auth_next_credentials (&creds,
                                     ras->auth_iterstate,
                                     ras->pool);
  if (err || ! creds)
    {
      svn_error_clear (err);
      return -1;
    }
  simple_creds = creds;
  
  /* ### silently truncates username/password to 256 chars. */
  apr_cpystrn(username, simple_creds->username, NE_ABUFSIZ);
  apr_cpystrn(password, simple_creds->password, NE_ABUFSIZ);

  return 0;
}


static const apr_uint32_t neon_failure_map[][2] =
{
  { NE_SSL_NOTYETVALID,        SVN_AUTH_SSL_NOTYETVALID },
  { NE_SSL_EXPIRED,            SVN_AUTH_SSL_EXPIRED },
  { NE_SSL_IDMISMATCH,         SVN_AUTH_SSL_CNMISMATCH },
  { NE_SSL_UNTRUSTED,          SVN_AUTH_SSL_UNKNOWNCA }
};

/* Convert neon's SSL failure mask to our own failure mask. */
static apr_uint32_t
convert_neon_failures(int neon_failures)
{
  apr_uint32_t svn_failures = 0;
  apr_size_t i;

  for (i = 0; i < sizeof(neon_failure_map) / (2 * sizeof(int)); ++i)
    {
      if (neon_failures & neon_failure_map[i][0])
        {
          svn_failures |= neon_failure_map[i][1];
          neon_failures &= ~neon_failure_map[i][0];
        }
    }

  /* Map any remaining neon failure bits to our OTHER bit. */
  if (neon_failures)
    {
      svn_failures |= SVN_AUTH_SSL_OTHER;
    }

  return svn_failures;
}

/* A neon-session callback to validate the SSL certificate when the CA
   is unknown or there are other SSL certificate problems. */
static int
server_ssl_callback(void *userdata,
                    int failures,
                    const ne_ssl_certificate *cert)
{
  svn_ra_session_t *ras = userdata;
  svn_auth_cred_ssl_server_trust_t *server_creds = NULL;
  void *creds;
  svn_auth_iterstate_t *state;
  apr_pool_t *pool;
  svn_error_t *error;
  char *ascii_cert = ne_ssl_cert_export(cert);
  char *issuer_dname = ne_ssl_readable_dname(ne_ssl_cert_issuer(cert));
  svn_auth_ssl_server_cert_info_t cert_info;
  char fingerprint[NE_SSL_DIGESTLEN];
  char valid_from[NE_SSL_VDATELEN], valid_until[NE_SSL_VDATELEN];
  const char *realmstring;
  apr_uint32_t *svn_failures = apr_palloc (ras->pool, sizeof(*svn_failures));

  /* Construct the realmstring, e.g. https://svn.collab.net:80 */
  realmstring = apr_psprintf (ras->pool, "%s://%s:%d", ras->root.scheme,
                              ras->root.host, ras->root.port);

  *svn_failures = convert_neon_failures(failures);
  svn_auth_set_parameter(ras->callbacks->auth_baton,
                         SVN_AUTH_PARAM_SSL_SERVER_FAILURES,
                         svn_failures);

  /* Extract the info from the certificate */
  cert_info.hostname = ne_ssl_cert_identity(cert);
  if (ne_ssl_cert_digest(cert, fingerprint) != 0)
    {
      strcpy(fingerprint, "<unknown>");
    }
  cert_info.fingerprint = fingerprint;
  ne_ssl_cert_validity(cert, valid_from, valid_until);
  cert_info.valid_from = valid_from;
  cert_info.valid_until = valid_until;
  cert_info.issuer_dname = issuer_dname;
  cert_info.ascii_cert = ascii_cert;

  svn_auth_set_parameter(ras->callbacks->auth_baton,
                         SVN_AUTH_PARAM_SSL_SERVER_CERT_INFO,
                         &cert_info);

  apr_pool_create(&pool, ras->pool);
  error = svn_auth_first_credentials(&creds, &state,
                                     SVN_AUTH_CRED_SSL_SERVER_TRUST,
                                     realmstring,
                                     ras->callbacks->auth_baton,
                                     pool);
  if (error || ! creds)
    {
      svn_error_clear(error);
    }
  else
    {
      server_creds = creds;
      error = svn_auth_save_credentials(state, pool);
      if (error)
        {
          /* It would be nice to show the error to the user somehow... */
          svn_error_clear(error);
        }
    }

  free(issuer_dname);
  free(ascii_cert);
  svn_auth_set_parameter(ras->callbacks->auth_baton,
                         SVN_AUTH_PARAM_SSL_SERVER_CERT_INFO, NULL);

  apr_pool_destroy(pool);
  return ! server_creds;
}

static svn_boolean_t
client_ssl_decrypt_cert(svn_ra_session_t *ras,
                        const char *cert_file,
                        ne_ssl_client_cert *clicert)
{
  svn_auth_iterstate_t *state;
  svn_error_t *error;
  apr_pool_t *pool;
  svn_boolean_t ok = FALSE;
  void *creds;
  int try;

  apr_pool_create(&pool, ras->pool);
  for (try = 0; TRUE; ++try)
    {
      if (try == 0)
        {
          error = svn_auth_first_credentials(&creds, &state,
                                             SVN_AUTH_CRED_SSL_CLIENT_CERT_PW,
                                             cert_file,
                                             ras->callbacks->auth_baton,
                                             pool);
        }
      else
        {
          error = svn_auth_next_credentials(&creds, state, pool);
        }

      if (error || ! creds)
        {
          /* Failure or too many attempts */
          svn_error_clear(error);
          break;
        }
      else
        {
          svn_auth_cred_ssl_client_cert_pw_t *pw_creds = creds;

          if (ne_ssl_clicert_decrypt(clicert, pw_creds->password) == 0)
            {
              /* Success */
              ok = TRUE;
              break;
            }
        }
    }
  apr_pool_destroy(pool);

  return ok;
}


static void
client_ssl_callback(void *userdata, ne_session *sess,
                    const ne_ssl_dname *const *dnames,
                    int dncount)
{
  svn_ra_session_t *ras = userdata;
  ne_ssl_client_cert *clicert = NULL;
  void *creds;
  svn_auth_iterstate_t *state;
  const char *realmstring;
  apr_pool_t *pool;
  svn_error_t *error;
  int try;

  apr_pool_create(&pool, ras->pool);

  realmstring = apr_psprintf (pool, "%s://%s:%d", ras->root.scheme,
                              ras->root.host, ras->root.port);

  for (try = 0; TRUE; ++try)
    {
      if (try == 0)
        {
          error = svn_auth_first_credentials(&creds, &state,
                                             SVN_AUTH_CRED_SSL_CLIENT_CERT,
                                             realmstring,
                                             ras->callbacks->auth_baton,
                                             pool);
        }
      else
        {
          error = svn_auth_next_credentials(&creds, state, pool);
        }

      if (error || ! creds)
        {
          /* Failure or too many attempts */
          svn_error_clear(error);
          break;
        }
      else
        {
          svn_auth_cred_ssl_client_cert_t *client_creds = creds;

          clicert = ne_ssl_clicert_read(client_creds->cert_file);
          if (clicert)
            {
              if (! ne_ssl_clicert_encrypted(clicert) ||
                  client_ssl_decrypt_cert(ras, client_creds->cert_file,
                                          clicert))
                {
                  ne_ssl_set_clicert(sess, clicert);
                }
              break;
            }
        }
    }

  apr_pool_destroy(pool);
}

/* Set *PROXY_HOST, *PROXY_PORT, *PROXY_USERNAME, *PROXY_PASSWORD,
 * *TIMEOUT_SECONDS and *NEON_DEBUG to the information for REQUESTED_HOST,
 * allocated in POOL, if there is any applicable information.  If there is
 * no applicable information or if there is an error, then set *PROXY_PORT
 * to (unsigned int) -1, *TIMEOUT_SECONDS and *NEON_DEBUG to zero, and the
 * rest to NULL.  This function can return an error, so before checking any
 * values, check the error return value.
 */
static svn_error_t *get_server_settings(const char **proxy_host,
                                        unsigned int *proxy_port,
                                        const char **proxy_username,
                                        const char **proxy_password,
                                        int *timeout_seconds,
                                        int *neon_debug,
                                        svn_boolean_t *compression,
                                        svn_config_t *cfg,
                                        const char *requested_host,
                                        apr_pool_t *pool)
{
  const char *exceptions, *port_str, *timeout_str, *server_group;
  const char *debug_str;
  svn_boolean_t is_exception = FALSE;
  /* If we find nothing, default to nulls. */
  *proxy_host     = NULL;
  *proxy_port     = (unsigned int) -1;
  *proxy_username = NULL;
  *proxy_password = NULL;
  port_str        = NULL;
  timeout_str     = NULL;
  debug_str       = NULL;

  /* If there are defaults, use them, but only if the requested host
     is not one of the exceptions to the defaults. */
  svn_config_get(cfg, &exceptions, SVN_CONFIG_SECTION_GLOBAL, 
                 SVN_CONFIG_OPTION_HTTP_PROXY_EXCEPTIONS, NULL);
  if (exceptions)
    {
      apr_array_header_t *l = svn_cstring_split (exceptions, ",", TRUE, pool);
      is_exception = svn_cstring_match_glob_list (requested_host, l);
    }
  if (! is_exception)
    {
      svn_config_get(cfg, proxy_host, SVN_CONFIG_SECTION_GLOBAL, 
                     SVN_CONFIG_OPTION_HTTP_PROXY_HOST, NULL);
      svn_config_get(cfg, &port_str, SVN_CONFIG_SECTION_GLOBAL, 
                     SVN_CONFIG_OPTION_HTTP_PROXY_PORT, NULL);
      svn_config_get(cfg, proxy_username, SVN_CONFIG_SECTION_GLOBAL, 
                     SVN_CONFIG_OPTION_HTTP_PROXY_USERNAME, NULL);
      svn_config_get(cfg, proxy_password, SVN_CONFIG_SECTION_GLOBAL, 
                     SVN_CONFIG_OPTION_HTTP_PROXY_PASSWORD, NULL);
      svn_config_get(cfg, &timeout_str, SVN_CONFIG_SECTION_GLOBAL, 
                     SVN_CONFIG_OPTION_HTTP_TIMEOUT, NULL);
      SVN_ERR(svn_config_get_bool(cfg, compression, SVN_CONFIG_SECTION_GLOBAL,
                                  SVN_CONFIG_OPTION_HTTP_COMPRESSION, TRUE));
      svn_config_get(cfg, &debug_str, SVN_CONFIG_SECTION_GLOBAL, 
                     SVN_CONFIG_OPTION_NEON_DEBUG_MASK, NULL);
    }

  if (cfg)
    server_group = svn_config_find_group(cfg, requested_host, 
                                         SVN_CONFIG_SECTION_GROUPS, pool);
  else
    server_group = NULL;

  if (server_group)
    {
      svn_config_get(cfg, proxy_host, server_group, 
                     SVN_CONFIG_OPTION_HTTP_PROXY_HOST, *proxy_host);
      svn_config_get(cfg, &port_str, server_group, 
                     SVN_CONFIG_OPTION_HTTP_PROXY_PORT, port_str);
      svn_config_get(cfg, proxy_username, server_group, 
                     SVN_CONFIG_OPTION_HTTP_PROXY_USERNAME, *proxy_username);
      svn_config_get(cfg, proxy_password, server_group, 
                     SVN_CONFIG_OPTION_HTTP_PROXY_PASSWORD, *proxy_password);
      svn_config_get(cfg, &timeout_str, server_group, 
                     SVN_CONFIG_OPTION_HTTP_TIMEOUT, timeout_str);
      SVN_ERR(svn_config_get_bool(cfg, compression, server_group,
                                  SVN_CONFIG_OPTION_HTTP_COMPRESSION,
                                  *compression));
      svn_config_get(cfg, &debug_str, server_group, 
                     SVN_CONFIG_OPTION_NEON_DEBUG_MASK, debug_str);
    }

  /* Special case: convert the port value, if any. */
  if (port_str)
    {
      char *endstr;
      const long int port = strtol(port_str, &endstr, 10);

      if (*endstr)
        return svn_error_create(SVN_ERR_RA_ILLEGAL_URL, NULL,
                                _("Invalid URL: illegal character in proxy "
                                  "port number"));
      if (port < 0)
        return svn_error_create(SVN_ERR_RA_ILLEGAL_URL, NULL,
                                _("Invalid URL: negative proxy port number"));
      if (port > 65535)
        return svn_error_create(SVN_ERR_RA_ILLEGAL_URL, NULL,
                                _("Invalid URL: proxy port number greater "
                                  "than maximum TCP port number 65535"));
      *proxy_port = port;
    }
  else
    *proxy_port = 80;

  if (timeout_str)
    {
      char *endstr;
      const long int timeout = strtol(timeout_str, &endstr, 10);

      if (*endstr)
        return svn_error_create(SVN_ERR_RA_DAV_INVALID_CONFIG_VALUE, NULL,
                                _("Invalid config: illegal character in "
                                  "timeout value"));
      if (timeout < 0)
        return svn_error_create(SVN_ERR_RA_DAV_INVALID_CONFIG_VALUE, NULL,
                                _("Invalid config: negative timeout value"));
      *timeout_seconds = timeout;
    }
  else
    *timeout_seconds = 0;

  if (debug_str)
    {
      char *endstr;
      const long int debug = strtol(debug_str, &endstr, 10);

      if (*endstr)
        return svn_error_create(SVN_ERR_RA_DAV_INVALID_CONFIG_VALUE, NULL,
                                _("Invalid config: illegal character in "
                                  "debug mask value"));

      *neon_debug = debug;
    }
  else
    *neon_debug = 0;

  return SVN_NO_ERROR;
}


/* Userdata for the `proxy_auth' function. */
struct proxy_auth_baton
{
  const char *username;  /* Cannot be NULL, but "" is okay. */
  const char *password;  /* Cannot be NULL, but "" is okay. */
};


/* An `ne_request_auth' callback, see ne_auth.h.  USERDATA is a
 * `struct proxy_auth_baton *'.
 *
 * If ATTEMPT < 10, copy USERDATA->username and USERDATA->password
 * into USERNAME and PASSWORD respectively (but do not copy more than
 * NE_ABUFSIZ bytes of either), and return zero to indicate to Neon
 * that authentication should be attempted.
 *
 * If ATTEMPT >= 10, copy nothing into USERNAME and PASSWORD and
 * return 1, to cancel further authentication attempts.
 *
 * Ignore REALM.
 *
 * ### Note: There is no particularly good reason for the 10-attempt
 * limit.  Perhaps there should only be one attempt, and if it fails,
 * we just cancel any further attempts.  I used 10 just in case the
 * proxy tries various times with various realms, since we ignore
 * REALM.  And why do we ignore REALM?  Because we currently don't
 * have any way to specify different auth information for different
 * realms.  (I'm assuming that REALM would be a realm on the proxy
 * server, not on the Subversion repository server that is the real
 * destination.)  Do we have any need to support proxy realms?
 */
static int proxy_auth(void *userdata,
                      const char *realm,
                      int attempt,
                      char *username,
                      char *password)
{
  struct proxy_auth_baton *pab = userdata;

  if (attempt >= 10)
    return 1;

  /* Else. */

  apr_cpystrn(username, pab->username, NE_ABUFSIZ);
  apr_cpystrn(password, pab->password, NE_ABUFSIZ);

  return 0;
}


/* ### need an ne_session_dup to avoid the second gethostbyname
 * call and make this halfway sane. */


static svn_error_t *
svn_ra_dav__open (void **session_baton,
                  const char *repos_URL,
                  const svn_ra_callbacks_t *callbacks,
                  void *callback_baton,
                  apr_hash_t *config,
                  apr_pool_t *pool)
{
  apr_size_t len;
  ne_session *sess, *sess2;
  ne_uri uri = { 0 };
  svn_ra_session_t *ras;
  int is_ssl_session;
  svn_boolean_t compression;
  svn_config_t *cfg;
  const char *server_group;

  /* Sanity check the URI */
  if (ne_uri_parse(repos_URL, &uri) 
      || uri.host == NULL || uri.path == NULL || uri.scheme == NULL)
    {
      ne_uri_free(&uri);
      return svn_error_create(SVN_ERR_RA_ILLEGAL_URL, NULL,
                              _("Malformed URL for repository"));
    }

  /* Can we initialize network? */
  if (ne_sock_init() != 0)
    {
      ne_uri_free(&uri);
      return svn_error_create(SVN_ERR_RA_DAV_SOCK_INIT, NULL,
                              _("Network socket initialization failed"));
    }

  /* we want to know if the repository is actually somewhere else */
  /* ### not yet: http_redirect_register(sess, ... ); */

  is_ssl_session = (strcasecmp(uri.scheme, "https") == 0);
  if (is_ssl_session)
    {
      if (ne_supports_ssl() == 0)
        {
          ne_uri_free(&uri);
          return svn_error_create(SVN_ERR_RA_DAV_SOCK_INIT, NULL,
                                  _("SSL is not supported"));
        }
    }
  if (uri.port == 0)
    {
      uri.port = ne_uri_defaultport(uri.scheme);
    }

  /* Create two neon session objects, and set their properties... */
  sess = ne_session_create(uri.scheme, uri.host, uri.port);
  sess2 = ne_session_create(uri.scheme, uri.host, uri.port);

  cfg = config ? apr_hash_get (config, 
                               SVN_CONFIG_CATEGORY_SERVERS,
                               APR_HASH_KEY_STRING) : NULL;
  if (cfg)
    server_group = svn_config_find_group(cfg, uri.host,
                                         SVN_CONFIG_SECTION_GROUPS, pool);
  else
    server_group = NULL;
  
  /* If there's a timeout or proxy for this URL, use it. */
  {
    const char *proxy_host;
    unsigned int proxy_port;
    const char *proxy_username;
    const char *proxy_password;
    int timeout;
    int debug;
    svn_error_t *err;
    
    err = get_server_settings(&proxy_host,
                              &proxy_port,
                              &proxy_username,
                              &proxy_password,
                              &timeout,
                              &debug,
                              &compression,
                              cfg,
                              uri.host,
                              pool);
    if (err)
      {
        ne_uri_free(&uri);
        return err;
      }

    if (debug)
      ne_debug_init(stderr, debug);

    if (proxy_host)
      {
        ne_session_proxy(sess, proxy_host, proxy_port);
        ne_session_proxy(sess2, proxy_host, proxy_port);

        if (proxy_username)
          {
            /* Allocate the baton in pool, not on stack, so it will
               last till whenever Neon needs it. */
            struct proxy_auth_baton *pab = apr_palloc(pool, sizeof (*pab));

            pab->username = proxy_username;
            pab->password = proxy_password ? proxy_password : "";
        
            ne_set_proxy_auth(sess, proxy_auth, pab);
            ne_set_proxy_auth(sess2, proxy_auth, pab);
          }
      }

    if (!timeout)
      timeout = DEFAULT_HTTP_TIMEOUT;
    ne_set_read_timeout(sess, timeout);
    ne_set_read_timeout(sess2, timeout);
  }

  /* make sure we will eventually destroy the session */
  apr_pool_cleanup_register(pool, sess, cleanup_session,
                            apr_pool_cleanup_null);
  apr_pool_cleanup_register(pool, sess2, cleanup_session,
                            apr_pool_cleanup_null);

  ne_set_useragent(sess, "SVN/" SVN_VERSION);
  ne_set_useragent(sess2, "SVN/" SVN_VERSION);

  /* clean up trailing slashes from the URL */
  len = strlen(uri.path);
  if (len > 1 && uri.path[len - 1] == '/')
    uri.path[len - 1] = '\0';

  /* Create and fill a session_baton. */
  ras = apr_pcalloc(pool, sizeof(*ras));
  ras->pool = pool;
  ras->url = apr_pstrdup (pool, repos_URL);
  /* copies uri pointer members, they get free'd in __close. */
  ras->root = uri; 
  ras->sess = sess;
  ras->sess2 = sess2;  
  ras->callbacks = callbacks;
  ras->callback_baton = callback_baton;
  ras->compression = compression;
  /* save config and server group in the auth parameter hash */
  svn_auth_set_parameter(ras->callbacks->auth_baton,
                         SVN_AUTH_PARAM_CONFIG, cfg);
  svn_auth_set_parameter(ras->callbacks->auth_baton,
                         SVN_AUTH_PARAM_SERVER_GROUP, server_group);

  /* make sure we eventually destroy the uri */
  apr_pool_cleanup_register(pool, &ras->root, cleanup_uri,
                            apr_pool_cleanup_null);

  /* note that ras->username and ras->password are still NULL at this
     point. */


  /* Register an authentication 'pull' callback with the neon sessions */
  ne_set_server_auth(sess, request_auth, ras);
  ne_set_server_auth(sess2, request_auth, ras);

  /* Store our RA session baton in Neon's private data slot so we can
     get at it in functions that take only ne_session_t *sess
     (instead of the full svn_ra_session_t *ras). */
  ne_set_session_private(sess, SVN_RA_NE_SESSION_ID, ras);
  ne_set_session_private(sess2, SVN_RA_NE_SESSION_ID, ras);

  if (is_ssl_session)
    {
      const char *authorities, *trust_default_ca;
      authorities = svn_config_get_server_setting(
            cfg, server_group,
            SVN_CONFIG_OPTION_SSL_AUTHORITY_FILES,
            NULL);
      
      if (authorities != NULL)
        {
          char *files, *file, *last;
          files = apr_pstrdup(pool, authorities);

          while ((file = apr_strtok(files, ";", &last)) != NULL)
            {
              ne_ssl_certificate *ca_cert;
              files = NULL;
              ca_cert = ne_ssl_cert_read(file);
              if (ca_cert == NULL)
                {
                  return svn_error_createf
                    (SVN_ERR_RA_DAV_INVALID_CONFIG_VALUE, NULL,
                     _("Invalid config: unable to load certificate file '%s'"),
                     svn_path_local_style(file, pool));
                }
              ne_ssl_trust_cert(sess, ca_cert);
              ne_ssl_trust_cert(sess2, ca_cert);
            }
        }

      /* When the CA certificate or server certificate has
         verification problems, neon will call our verify function before
         outright rejection of the connection.*/
      ne_ssl_set_verify(sess, server_ssl_callback, ras);
      ne_ssl_set_verify(sess2, server_ssl_callback, ras);
      /* For client connections, we register a callback for if the server
         wants to authenticate the client via client certificate. */

      ne_ssl_provide_clicert(sess, client_ssl_callback, ras);
      ne_ssl_provide_clicert(sess2, client_ssl_callback, ras);

      /* See if the user wants us to trust "default" openssl CAs. */
      trust_default_ca = svn_config_get_server_setting(
               cfg, server_group,
               SVN_CONFIG_OPTION_SSL_TRUST_DEFAULT_CA,
               "true");

      if (strcasecmp(trust_default_ca, "true") == 0)
        {
          ne_ssl_trust_default_ca(sess);
          ne_ssl_trust_default_ca(sess2);
        }
    }

  *session_baton = ras;

  return SVN_NO_ERROR;
}


static svn_error_t *svn_ra_dav__get_repos_root(void *session_baton,
                                               const char **url,
                                               apr_pool_t *pool)
{
  svn_ra_session_t *ras = session_baton;

  if (! ras->repos_root)
    {
      svn_string_t bc_relative;
      svn_stringbuf_t *url_buf;

      SVN_ERR(svn_ra_dav__get_baseline_info(NULL, NULL, &bc_relative,
                                            NULL, ras->sess, ras->url,
                                            SVN_INVALID_REVNUM, pool));

      /* Remove as many path components from the URL as there are components
         in bc_relative. */
      url_buf = svn_stringbuf_create(ras->url, pool);
      svn_path_remove_components
        (url_buf, svn_path_component_count(bc_relative.data));
      ras->repos_root = url_buf->data;
    }

  *url = ras->repos_root;
  return SVN_NO_ERROR; 
}


static svn_error_t *svn_ra_dav__do_get_uuid(void *session_baton,
                                            const char **uuid,
                                            apr_pool_t *pool)
{
  svn_ra_session_t *ras = session_baton;

  if (! ras->uuid)
    {
      svn_ra_dav_resource_t *rsrc;
      const char *lopped_path;
      const svn_string_t *uuid_propval;

      SVN_ERR (svn_ra_dav__search_for_starting_props(&rsrc, &lopped_path,
                                                     ras->sess, ras->url,
                                                     pool) );

      uuid_propval = apr_hash_get(rsrc->propset,
                                  SVN_RA_DAV__PROP_REPOSITORY_UUID,
                                  APR_HASH_KEY_STRING);
      if (uuid_propval == NULL)
        /* ### better error reporting... */
        return svn_error_create(APR_EGENERAL, NULL,
                                _("The UUID property was not found on the "
                                  "resource or any of its parents"));

      if (uuid_propval && (uuid_propval->len > 0))
        ras->uuid = apr_pstrdup(ras->pool, uuid_propval->data); /* cache */
      else
        return svn_error_create
          (SVN_ERR_RA_NO_REPOS_UUID, NULL,
           _("Please upgrade the server to 0.19 or later"));
    }

  *uuid = ras->uuid;
  return SVN_NO_ERROR; 
}


static const svn_version_t *
ra_dav_version (void)
{
  SVN_VERSION_BODY;
}

static const svn_ra_plugin_t dav_plugin = {
  "ra_dav",
  N_("Module for accessing a repository via WebDAV (DeltaV) protocol."),
  svn_ra_dav__open,
  svn_ra_dav__get_latest_revnum,
  svn_ra_dav__get_dated_revision,
  svn_ra_dav__change_rev_prop,
  svn_ra_dav__rev_proplist,
  svn_ra_dav__rev_prop,
  svn_ra_dav__get_commit_editor,
  svn_ra_dav__get_file,
  svn_ra_dav__get_dir,
  svn_ra_dav__do_update,
  svn_ra_dav__do_switch,
  svn_ra_dav__do_status,
  svn_ra_dav__do_diff,
  svn_ra_dav__get_log,
  svn_ra_dav__do_check_path,
  svn_ra_dav__do_get_uuid,
  svn_ra_dav__get_repos_root,
  svn_ra_dav__get_locations,
  svn_ra_dav__get_file_revs,
  ra_dav_version,
  svn_ra_dav__get_log2
};


svn_error_t *svn_ra_dav_init(int abi_version,
                             apr_pool_t *pconf,
                             apr_hash_t *hash)
{
  static const svn_version_checklist_t checklist[] =
    {
      { "svn_subr",  svn_subr_version },
      { "svn_delta", svn_delta_version },
      { NULL, NULL }
    };

  if (abi_version < 1
      || abi_version > SVN_RA_ABI_VERSION)
    return svn_error_createf (SVN_ERR_RA_UNSUPPORTED_ABI_VERSION, NULL,
                              _("Unsupported RA plugin ABI version (%d) "
                                "for ra_dav"), abi_version);
  SVN_ERR(svn_ver_check_list(ra_dav_version(), checklist));

  apr_hash_set (hash, "http", APR_HASH_KEY_STRING, &dav_plugin);

  if (ne_supports_ssl())
    {
      /* Only add this if neon is compiled with SSL support. */
      apr_hash_set(hash, "https", APR_HASH_KEY_STRING, &dav_plugin);
    }

  return SVN_NO_ERROR;
}
