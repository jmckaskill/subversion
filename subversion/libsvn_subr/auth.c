/*
 * auth.c: authentication support functions for Subversion
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */


#include <apr_pools.h>
#include <apr_tables.h>
#include <apr_strings.h>

#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_auth.h"
#include "svn_config.h"
#include "svn_private_config.h"
#include "svn_dso.h"
#include "svn_base64.h"
#include "svn_version.h"

/* AN OVERVIEW
   ===========

   A good way to think of this machinery is as a set of tables.

     - Each type of credentials selects a single table.

     - In a given table, each row is a 'provider' capable of returning
       the same type of credentials.  Each column represents a
       provider's repeated attempts to provide credentials.


   Fetching Credentials from Providers
   -----------------------------------

   When the caller asks for a particular type of credentials, the
   machinery in this file walks over the appropriate table.  It starts
   with the first provider (first row), and calls first_credentials()
   to get the first set of credentials (first column).  If the caller
   is unhappy with the credentials, then each subsequent call to
   next_credentials() traverses the row from left to right.  If the
   provider returns error at any point, then we go to the next provider
   (row).  We continue this way until every provider fails, or
   until the client is happy with the returned credentials.

   Note that the caller cannot see the table traversal, and thus has
   no idea when we switch providers.


   Storing Credentials with Providers
   ----------------------------------

   When the server has validated a set of credentials, and when
   credential caching is enabled, we have the chance to store those
   credentials for later use.  The provider which provided the working
   credentials is the first one given the opportunity to (re)cache
   those credentials.  Its save_credentials() function is invoked with
   the working credentials.  If that provider reports that it
   successfully stored the credentials, we're done.  Otherwise, we
   walk the providers (rows) for that type of credentials in order
   from the top of the table, allowing each in turn the opportunity to
   store the credentials.  When one reports that it has done so
   successfully -- or when we run out of providers (rows) to try --
   the table walk ends.
*/



/* This effectively defines a single table.  Every provider in this
   array returns the same kind of credentials. */
typedef struct provider_set_t
{
  /* ordered array of svn_auth_provider_object_t */
  apr_array_header_t *providers;

} provider_set_t;


/* The main auth baton. */
struct svn_auth_baton_t
{
  /* a collection of tables.  maps cred_kind -> provider_set */
  apr_hash_t *tables;

  /* the pool I'm allocated in. */
  apr_pool_t *pool;

  /* run-time parameters needed by providers. */
  apr_hash_t *parameters;

  /* run-time credentials cache. */
  apr_hash_t *creds_cache;

};

/* Abstracted iteration baton */
struct svn_auth_iterstate_t
{
  provider_set_t *table;        /* the table being searched */
  int provider_idx;             /* the current provider (row) */
  svn_boolean_t got_first;      /* did we get the provider's first creds? */
  void *provider_iter_baton;    /* the provider's own iteration context */
  const char *realmstring;      /* The original realmstring passed in */
  const char *cache_key;        /* key to use in auth_baton's creds_cache */
  svn_auth_baton_t *auth_baton; /* the original auth_baton. */
};



void
svn_auth_open(svn_auth_baton_t **auth_baton,
              const apr_array_header_t *providers,
              apr_pool_t *pool)
{
  svn_auth_baton_t *ab;
  svn_auth_provider_object_t *provider;
  int i;

  /* Build the auth_baton. */
  ab = apr_pcalloc(pool, sizeof(*ab));
  ab->tables = apr_hash_make(pool);
  ab->parameters = apr_hash_make(pool);
  ab->creds_cache = apr_hash_make(pool);
  ab->pool = pool;

  /* Register each provider in order.  Providers of different
     credentials will be automatically sorted into different tables by
     register_provider(). */
  for (i = 0; i < providers->nelts; i++)
    {
      provider_set_t *table;
      provider = APR_ARRAY_IDX(providers, i, svn_auth_provider_object_t *);

      /* Add it to the appropriate table in the auth_baton */
      table = apr_hash_get(ab->tables,
                           provider->vtable->cred_kind, APR_HASH_KEY_STRING);
      if (! table)
        {
          table = apr_pcalloc(pool, sizeof(*table));
          table->providers
            = apr_array_make(pool, 1, sizeof(svn_auth_provider_object_t *));

          apr_hash_set(ab->tables,
                       provider->vtable->cred_kind, APR_HASH_KEY_STRING,
                       table);
        }
      APR_ARRAY_PUSH(table->providers, svn_auth_provider_object_t *)
        = provider;
    }

  *auth_baton = ab;
}



void
svn_auth_set_parameter(svn_auth_baton_t *auth_baton,
                       const char *name,
                       const void *value)
{
  apr_hash_set(auth_baton->parameters, name, APR_HASH_KEY_STRING, value);
}

const void *
svn_auth_get_parameter(svn_auth_baton_t *auth_baton,
                       const char *name)
{
  return apr_hash_get(auth_baton->parameters, name, APR_HASH_KEY_STRING);
}



svn_error_t *
svn_auth_first_credentials(void **credentials,
                           svn_auth_iterstate_t **state,
                           const char *cred_kind,
                           const char *realmstring,
                           svn_auth_baton_t *auth_baton,
                           apr_pool_t *pool)
{
  int i = 0;
  provider_set_t *table;
  svn_auth_provider_object_t *provider = NULL;
  void *creds = NULL;
  void *iter_baton = NULL;
  svn_boolean_t got_first = FALSE;
  svn_auth_iterstate_t *iterstate;
  const char *cache_key;

  /* Get the appropriate table of providers for CRED_KIND. */
  table = apr_hash_get(auth_baton->tables, cred_kind, APR_HASH_KEY_STRING);
  if (! table)
    return svn_error_createf(SVN_ERR_AUTHN_NO_PROVIDER, NULL,
                             _("No provider registered for '%s' credentials"),
                             cred_kind);

  /* First, see if we have cached creds in the auth_baton. */
  cache_key = apr_pstrcat(pool, cred_kind, ":", realmstring, (char *)NULL);
  creds = apr_hash_get(auth_baton->creds_cache,
                       cache_key, APR_HASH_KEY_STRING);
  if (creds)
    {
       got_first = FALSE;
    }
  else
    /* If not, find a provider that can give "first" credentials. */
    {
      /* Find a provider that can give "first" credentials. */
      for (i = 0; i < table->providers->nelts; i++)
        {
          provider = APR_ARRAY_IDX(table->providers, i,
                                   svn_auth_provider_object_t *);
          SVN_ERR(provider->vtable->first_credentials
                  (&creds, &iter_baton, provider->provider_baton,
                   auth_baton->parameters, realmstring, auth_baton->pool));

          if (creds != NULL)
            {
              got_first = TRUE;
              break;
            }
        }
    }

  if (! creds)
    *state = NULL;
  else
    {
      /* Build an abstract iteration state. */
      iterstate = apr_pcalloc(pool, sizeof(*iterstate));
      iterstate->table = table;
      iterstate->provider_idx = i;
      iterstate->got_first = got_first;
      iterstate->provider_iter_baton = iter_baton;
      iterstate->realmstring = apr_pstrdup(pool, realmstring);
      iterstate->cache_key = cache_key;
      iterstate->auth_baton = auth_baton;
      *state = iterstate;

      /* Put the creds in the cache */
      apr_hash_set(auth_baton->creds_cache,
                   apr_pstrdup(auth_baton->pool, cache_key),
                   APR_HASH_KEY_STRING,
                   creds);
    }

  *credentials = creds;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_auth_next_credentials(void **credentials,
                          svn_auth_iterstate_t *state,
                          apr_pool_t *pool)
{
  svn_auth_baton_t *auth_baton = state->auth_baton;
  svn_auth_provider_object_t *provider;
  provider_set_t *table = state->table;
  void *creds = NULL;

  /* Continue traversing the table from where we left off. */
  for (/* no init */;
       state->provider_idx < table->providers->nelts;
       state->provider_idx++)
    {
      provider = APR_ARRAY_IDX(table->providers,
                               state->provider_idx,
                               svn_auth_provider_object_t *);
      if (! state->got_first)
        {
          SVN_ERR(provider->vtable->first_credentials
                  (&creds, &(state->provider_iter_baton),
                   provider->provider_baton, auth_baton->parameters,
                   state->realmstring, auth_baton->pool));
          state->got_first = TRUE;
        }
      else
        {
          if (provider->vtable->next_credentials)
            SVN_ERR(provider->vtable->next_credentials
                    (&creds, state->provider_iter_baton,
                     provider->provider_baton, auth_baton->parameters,
                     state->realmstring, auth_baton->pool));
        }

      if (creds != NULL)
        {
          /* Put the creds in the cache */
          apr_hash_set(auth_baton->creds_cache,
                       state->cache_key, APR_HASH_KEY_STRING,
                       creds);
          break;
        }

      state->got_first = FALSE;
    }

  *credentials = creds;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_auth_save_credentials(svn_auth_iterstate_t *state,
                          apr_pool_t *pool)
{
  int i;
  svn_auth_provider_object_t *provider;
  svn_boolean_t save_succeeded = FALSE;
  const char *no_auth_cache;
  svn_auth_baton_t *auth_baton;
  void *creds;

  if (! state || state->table->providers->nelts <= state->provider_idx)
    return SVN_NO_ERROR;

  auth_baton = state->auth_baton;
  creds = apr_hash_get(state->auth_baton->creds_cache,
                       state->cache_key, APR_HASH_KEY_STRING);
  if (! creds)
    return SVN_NO_ERROR;

  /* Do not save the creds if SVN_AUTH_PARAM_NO_AUTH_CACHE is set */
  no_auth_cache = apr_hash_get(auth_baton->parameters,
                               SVN_AUTH_PARAM_NO_AUTH_CACHE,
                               APR_HASH_KEY_STRING);
  if (no_auth_cache)
    return SVN_NO_ERROR;

  /* First, try to save the creds using the provider that produced them. */
  provider = APR_ARRAY_IDX(state->table->providers,
                           state->provider_idx,
                           svn_auth_provider_object_t *);
  if (provider->vtable->save_credentials)
    SVN_ERR(provider->vtable->save_credentials(&save_succeeded,
                                               creds,
                                               provider->provider_baton,
                                               auth_baton->parameters,
                                               state->realmstring,
                                               pool));
  if (save_succeeded)
    return SVN_NO_ERROR;

  /* Otherwise, loop from the top of the list, asking every provider
     to attempt a save.  ### todo: someday optimize so we don't
     necessarily start from the top of the list. */
  for (i = 0; i < state->table->providers->nelts; i++)
    {
      provider = APR_ARRAY_IDX(state->table->providers, i,
                               svn_auth_provider_object_t *);
      if (provider->vtable->save_credentials)
        SVN_ERR(provider->vtable->save_credentials
                (&save_succeeded, creds,
                 provider->provider_baton,
                 auth_baton->parameters,
                 state->realmstring,
                 pool));

      if (save_succeeded)
        break;
    }

  /* ### notice that at the moment, if no provider can save, there's
     no way the caller will know. */

  return SVN_NO_ERROR;
}

svn_auth_ssl_server_cert_info_t *
svn_auth_ssl_server_cert_info_dup
  (const svn_auth_ssl_server_cert_info_t *info, apr_pool_t *pool)
{
  svn_auth_ssl_server_cert_info_t *new_info
    = apr_palloc(pool, sizeof(*new_info));

  *new_info = *info;

  new_info->hostname = apr_pstrdup(pool, new_info->hostname);
  new_info->fingerprint = apr_pstrdup(pool, new_info->fingerprint);
  new_info->valid_from = apr_pstrdup(pool, new_info->valid_from);
  new_info->valid_until = apr_pstrdup(pool, new_info->valid_until);
  new_info->issuer_dname = apr_pstrdup(pool, new_info->issuer_dname);
  new_info->ascii_cert = apr_pstrdup(pool, new_info->ascii_cert);

  return new_info;
}

svn_error_t *
svn_auth_get_platform_specific_provider(svn_auth_provider_object_t **provider,
                                        const char *provider_name,
                                        const char *provider_type,
                                        apr_pool_t *pool)
{
  *provider = NULL;

  if (apr_strnatcmp(provider_name, "gnome_keyring") == 0 ||
      apr_strnatcmp(provider_name, "kwallet") == 0)
    {
#if defined(SVN_HAVE_GNOME_KEYRING) || defined(SVN_HAVE_KWALLET)
      apr_dso_handle_t *dso;
      apr_dso_handle_sym_t provider_function_symbol, version_function_symbol;
      const char *library_label, *library_name;
      const char *provider_function_name, *version_function_name;
      library_name = apr_psprintf(pool,
                                  "libsvn_auth_%s-%d.so.0",
                                  provider_name,
                                  SVN_VER_MAJOR);
      library_label = apr_psprintf(pool, "svn_%s", provider_name);
      provider_function_name = apr_psprintf(pool,
                                            "svn_auth_get_%s_%s_provider",
                                            provider_name, provider_type);
      version_function_name = apr_psprintf(pool,
                                           "svn_auth_%s_version",
                                           provider_name);
      SVN_ERR(svn_dso_load(&dso, library_name));
      if (dso)
        {
          if (apr_dso_sym(&version_function_symbol,
                          dso,
                          version_function_name) == 0)
            {
              svn_version_func_t version_function
                = version_function_symbol;
              const svn_version_checklist_t check_list[] =
                {
                  { library_label, version_function },
                  { NULL, NULL }
                };
              SVN_ERR(svn_ver_check_list(svn_subr_version(), check_list));
            }
          if (apr_dso_sym(&provider_function_symbol,
                          dso,
                          provider_function_name) == 0)
            {
              if (strcmp(provider_type, "simple") == 0)
                {
                  svn_auth_simple_provider_func_t provider_function
                    = provider_function_symbol;
                  provider_function(provider, pool);
                }
              else if (strcmp(provider_type, "ssl_client_cert_pw") == 0)
                {
                  svn_auth_ssl_client_cert_pw_provider_func_t provider_function
                    = provider_function_symbol;
                  provider_function(provider, pool);
                }
            }
        }
#endif
    }
  else
    {
#if defined(SVN_HAVE_GPG_AGENT)
      if (strcmp(provider_name, "gpg_agent") == 0 &&
          strcmp(provider_type, "simple") == 0)
        {
          svn_auth_get_gpg_agent_simple_provider(provider, pool);
        }
#endif
#ifdef SVN_HAVE_KEYCHAIN_SERVICES
      if (strcmp(provider_name, "keychain") == 0 &&
          strcmp(provider_type, "simple") == 0)
        {
          svn_auth_get_keychain_simple_provider(provider, pool);
        }
      else if (strcmp(provider_name, "keychain") == 0 &&
               strcmp(provider_type, "ssl_client_cert_pw") == 0)
        {
          svn_auth_get_keychain_ssl_client_cert_pw_provider(provider, pool);
        }
#endif

#if defined(WIN32) && !defined(__MINGW32__)
      if (strcmp(provider_name, "windows") == 0 &&
          strcmp(provider_type, "simple") == 0)
        {
          svn_auth_get_windows_simple_provider(provider, pool);
        }
      else if (strcmp(provider_name, "windows") == 0 &&
               strcmp(provider_type, "ssl_client_cert_pw") == 0)
        {
          svn_auth_get_windows_ssl_client_cert_pw_provider(provider, pool);
        }
      else if (strcmp(provider_name, "windows") == 0 &&
               strcmp(provider_type, "ssl_server_trust") == 0)
        {
          svn_auth_get_windows_ssl_server_trust_provider(provider, pool);
        }
#endif
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_auth_get_platform_specific_client_providers(apr_array_header_t **providers,
                                                svn_config_t *config,
                                                apr_pool_t *pool)
{
  svn_auth_provider_object_t *provider;
  const char *password_stores_config_option;
  apr_array_header_t *password_stores;
  int i;

#define SVN__DEFAULT_AUTH_PROVIDER_LIST \
         "gnome-keyring,kwallet,keychain,gpg-agent,windows-cryptoapi"

  if (config)
    {
      svn_config_get
        (config,
         &password_stores_config_option,
         SVN_CONFIG_SECTION_AUTH,
         SVN_CONFIG_OPTION_PASSWORD_STORES,
         SVN__DEFAULT_AUTH_PROVIDER_LIST);
    }
  else
    {
      password_stores_config_option = SVN__DEFAULT_AUTH_PROVIDER_LIST;
    }

  *providers = apr_array_make(pool, 12, sizeof(svn_auth_provider_object_t *));

  password_stores
    = svn_cstring_split(password_stores_config_option, " ,", TRUE, pool);

  for (i = 0; i < password_stores->nelts; i++)
    {
      const char *password_store = APR_ARRAY_IDX(password_stores, i,
                                                 const char *);


      /* GNOME Keyring */
      if (apr_strnatcmp(password_store, "gnome-keyring") == 0)
        {
          SVN_ERR(svn_auth_get_platform_specific_provider(&provider,
                                                          "gnome_keyring",
                                                          "simple",
                                                          pool));

          if (provider)
            APR_ARRAY_PUSH(*providers, svn_auth_provider_object_t *) = provider;

          SVN_ERR(svn_auth_get_platform_specific_provider(&provider,
                                                          "gnome_keyring",
                                                          "ssl_client_cert_pw",
                                                          pool));

          if (provider)
            APR_ARRAY_PUSH(*providers, svn_auth_provider_object_t *) = provider;

          continue;
        }

      /* GPG-AGENT */
      if (apr_strnatcmp(password_store, "gpg-agent") == 0)
        {
          SVN_ERR(svn_auth_get_platform_specific_provider(&provider,
                                                          "gpg_agent",
                                                          "simple",
                                                          pool));

          if (provider)
            APR_ARRAY_PUSH(*providers, svn_auth_provider_object_t *) = provider;

          continue;
        }

      /* KWallet */
      if (apr_strnatcmp(password_store, "kwallet") == 0)
        {
          SVN_ERR(svn_auth_get_platform_specific_provider(&provider,
                                                          "kwallet",
                                                          "simple",
                                                          pool));

          if (provider)
            APR_ARRAY_PUSH(*providers, svn_auth_provider_object_t *) = provider;

          SVN_ERR(svn_auth_get_platform_specific_provider(&provider,
                                                          "kwallet",
                                                          "ssl_client_cert_pw",
                                                          pool));
          if (provider)
            APR_ARRAY_PUSH(*providers, svn_auth_provider_object_t *) = provider;

          continue;
        }

      /* Keychain */
      if (apr_strnatcmp(password_store, "keychain") == 0)
        {
          SVN_ERR(svn_auth_get_platform_specific_provider(&provider,
                                                          "keychain",
                                                          "simple",
                                                          pool));

          if (provider)
            APR_ARRAY_PUSH(*providers, svn_auth_provider_object_t *) = provider;

          SVN_ERR(svn_auth_get_platform_specific_provider(&provider,
                                                          "keychain",
                                                          "ssl_client_cert_pw",
                                                          pool));

          if (provider)
            APR_ARRAY_PUSH(*providers, svn_auth_provider_object_t *) = provider;

          continue;
        }

      /* Windows */
      if (apr_strnatcmp(password_store, "windows-cryptoapi") == 0)
        {
          SVN_ERR(svn_auth_get_platform_specific_provider(&provider,
                                                          "windows",
                                                          "simple",
                                                          pool));

          if (provider)
            APR_ARRAY_PUSH(*providers, svn_auth_provider_object_t *) = provider;

          SVN_ERR(svn_auth_get_platform_specific_provider(&provider,
                                                          "windows",
                                                          "ssl_client_cert_pw",
                                                          pool));

          if (provider)
            APR_ARRAY_PUSH(*providers, svn_auth_provider_object_t *) = provider;

          continue;
        }

      return svn_error_createf(SVN_ERR_BAD_CONFIG_VALUE, NULL,
                               _("Invalid config: unknown password store "
                                 "'%s'"),
                               password_store);
    }

  return SVN_NO_ERROR;
}


/*** Master Passphrase ***/

#define AUTHN_MASTER_PASS_KNOWN_TEXT  "Subversion"
#define AUTHN_FAUX_REALMSTRING        "localhost.localdomain"
#define AUTHN_CHECKTEXT_KEY           "checktext"
#define AUTHN_PASSTYPE_KEY            "passtype"


/* Use SECRET to encrypt TEXT, returning the result (allocated from
   RESULT_POOL) in *CRYPT_TEXT.  Use SCRATCH_POOL for temporary
   allocations. */
static svn_error_t *
encrypt_text(const svn_string_t **crypt_text,
             const svn_string_t *text,
             const char *secret,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  /* ### FIXME!  This a mindless temporary implementation, offering
         all the security and privacy of a glass bathroom!  ***/

  SVN_ERR_ASSERT(text && text->data);
  SVN_ERR_ASSERT(secret);

  *crypt_text = svn_base64_encode_string2(svn_string_createf(scratch_pool,
                                                             "%s+%s",
                                                             text->data,
                                                             secret),
                                          FALSE, result_pool);
  return SVN_NO_ERROR;
}


/* Use SECRET to decrypt CRYPT_TEXT, returning the result (allocated
   from RESULT_POOL) in *TEXT.  Use SCRATCH_POOL for temporary
   allocations. */
static svn_error_t *
decrypt_text(const svn_string_t **text,
             const svn_string_t *crypt_text,
             const char *secret,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  /* ### FIXME!  This a mindless temporary implementation, offering
         all the security and privacy of a glass bathroom!  ***/

  const svn_string_t *work_text;
  int secret_len, text_len;

  SVN_ERR_ASSERT(crypt_text && crypt_text->data);
  SVN_ERR_ASSERT(secret);

  secret_len = strlen(secret);
  work_text = svn_base64_decode_string(crypt_text, scratch_pool);
  if (work_text->len < (secret_len + 1))
    return svn_error_create(SVN_ERR_AUTHN_FAILED, NULL,
                            "Invalid master passphrase.");
  text_len = work_text->len - secret_len - 1;
  if (work_text->data[text_len] != '+')
    return svn_error_create(SVN_ERR_AUTHN_FAILED, NULL,
                            "Invalid master passphrase.");
  if (strcmp(work_text->data + text_len + 1, secret) != 0)
    return svn_error_create(SVN_ERR_AUTHN_FAILED, NULL,
                            "Invalid master passphrase.");
  *text = svn_string_ncreate(work_text->data,
                             work_text->len - secret_len - 1,
                             result_pool);
  return SVN_NO_ERROR;
}
             

svn_error_t *
svn_auth_master_passphrase_get(const char **passphrase,
                               svn_auth_baton_t *auth_baton,
                               apr_pool_t *result_pool,
                               apr_pool_t *scratch_pool)
{
  apr_hash_t *creds_hash;
  const svn_string_t *check_text;
  const char *config_dir = apr_hash_get(auth_baton->parameters,
                                        SVN_AUTH_PARAM_CONFIG_DIR,
                                        APR_HASH_KEY_STRING);
#ifdef SVN_AUTH_TEMP_USE_FAUX_PASSPHRASE
  const char *default_passphrase = SVN_AUTH_TEMP_MASTER_PASSPHRASE;
#else
  const char *default_passphrase =
    apr_hash_get(auth_baton->parameters,
                 SVN_AUTH_PARAM_DEFAULT_MASTER_PASSPHRASE,
                 APR_HASH_KEY_STRING);
#endif

  /* Read the existing passphrase storage record so we can validate
     any master passphrase we have or fetch. If there's no check text,
     we must assume that there's no global master passphrase set, so
     we'll just return that fact. */
  SVN_ERR(svn_config_read_auth_data(&creds_hash,
                                    SVN_AUTH_CRED_MASTER_PASSPHRASE,
                                    AUTHN_FAUX_REALMSTRING, config_dir,
                                    scratch_pool));
  check_text = apr_hash_get(creds_hash, AUTHN_CHECKTEXT_KEY,
                            APR_HASH_KEY_STRING);
  if (! check_text)
    {
      *passphrase = NULL;
      return SVN_NO_ERROR;
    }

  /* If there's a default passphrase, verify that it matches the
     stored known-text.  */
  if (default_passphrase)
    {
      const svn_string_t *crypt_text;
      SVN_ERR(encrypt_text(&crypt_text,
                           svn_string_create(AUTHN_MASTER_PASS_KNOWN_TEXT,
                                             scratch_pool),
                           default_passphrase, scratch_pool, scratch_pool));
      if (svn_string_compare(crypt_text, check_text))
        {
          *passphrase = apr_pstrdup(result_pool, default_passphrase);
          return SVN_NO_ERROR;
        }
      default_passphrase = NULL;
    }

  /* We do not yet know the master passphrase, so we need to consult
     the providers.  */
  /* ### TODO ### */

  default_passphrase = NULL;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_auth_master_passphrase_set(svn_auth_baton_t *auth_baton,
                               const char *new_passphrase,
                               apr_pool_t *scratch_pool)
{
  apr_hash_t *creds_hash;
  const char *config_dir = apr_hash_get(auth_baton->parameters,
                                        SVN_AUTH_PARAM_CONFIG_DIR,
                                        APR_HASH_KEY_STRING);
  const char *old_passphrase;
  const svn_string_t *old_check_text, *new_check_text;

  /* First, fetch the existing passphrase. */
  SVN_ERR(svn_auth_master_passphrase_get(&old_passphrase, auth_baton,
                                         scratch_pool, scratch_pool));

  /* Now, read the existing passphrase storage record and grab the
     current checkidentify. */
  SVN_ERR(svn_config_read_auth_data(&creds_hash,
                                    SVN_AUTH_CRED_MASTER_PASSPHRASE,
                                    AUTHN_FAUX_REALMSTRING, config_dir,
                                    scratch_pool));
  old_check_text = apr_hash_get(creds_hash, AUTHN_CHECKTEXT_KEY,
                                APR_HASH_KEY_STRING);

  SVN_ERR(svn_config_write_auth_data(creds_hash,
                                     SVN_AUTH_CRED_MASTER_PASSPHRASE,
                                     AUTHN_FAUX_REALMSTRING, config_dir,
                                     scratch_pool));

  if (new_passphrase)
    {
      /* Encrypt the known text with NEW_PASSPHRASE to form the crypttext,
         and stuff that into the CREDS_HASH. */
      SVN_ERR(encrypt_text(&new_check_text,
                           svn_string_create(AUTHN_MASTER_PASS_KNOWN_TEXT,
                                             scratch_pool),
                           new_passphrase, scratch_pool, scratch_pool));
      apr_hash_set(creds_hash, AUTHN_CHECKTEXT_KEY,
                   APR_HASH_KEY_STRING, new_check_text);
    }
  else
    {
      apr_hash_set(creds_hash, AUTHN_CHECKTEXT_KEY, APR_HASH_KEY_STRING, NULL);
    }

  /* Re-encrypt all stored credentials in light of NEW_PASSPHRASE. */
  /* ### TODO ### */

  /* Save credentials to disk. */
  return svn_config_write_auth_data(creds_hash,
                                    SVN_AUTH_CRED_MASTER_PASSPHRASE,
                                    AUTHN_FAUX_REALMSTRING, config_dir,
                                    scratch_pool);
}
