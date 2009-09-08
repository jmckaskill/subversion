/*
 * gnome_keyring.c: GNOME Keyring provider for SVN_AUTH_CRED_*
 *
 * ====================================================================
 *    Licensed to the Subversion Corporation (SVN Corp.) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The SVN Corp. licenses this file
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

/* ==================================================================== */



/*** Includes. ***/

#include <apr_pools.h>
#include "svn_auth.h"
#include "svn_config.h"
#include "svn_error.h"
#include "svn_pools.h"

#include "private/svn_auth_private.h"

#include "svn_private_config.h"

#include <glib.h>
#include <dbus/dbus.h>
#include <gnome-keyring.h>


/*-----------------------------------------------------------------------*/
/* GNOME Keyring simple provider, puts passwords in GNOME Keyring        */
/*-----------------------------------------------------------------------*/


struct gnome_keyring_baton
{
  const char *keyring_name;
  GnomeKeyringInfo *info;
  GMainLoop *loop;
};


/* Callback function to destroy gnome_keyring_baton. */
static void
callback_destroy_data_keyring(void *data)
{
  struct gnome_keyring_baton *key_info =
                                  (struct gnome_keyring_baton*) data;

  if (data == NULL)
    return;

  if (key_info->keyring_name)
    {
      free((void*)key_info->keyring_name);
      key_info->keyring_name = NULL;
    }

  if (key_info->info)
    {
      gnome_keyring_info_free(key_info->info);
      key_info->info = NULL;
    }

  return;
}


/* Callback function to complete the keyring operation. */
static void
callback_done(GnomeKeyringResult result,
              gpointer data)
{
  struct gnome_keyring_baton *key_info =
                                (struct gnome_keyring_baton*) data;

  g_main_loop_quit(key_info->loop);
  return;
}


/* Callback function to get the keyring info. */
static void
callback_get_info_keyring(GnomeKeyringResult result,
                          GnomeKeyringInfo *info,
                          void *data)
{
  struct gnome_keyring_baton *key_info =
                                  (struct gnome_keyring_baton*) data;

  if (result == GNOME_KEYRING_RESULT_OK && info != NULL)
    {
      key_info->info = gnome_keyring_info_copy(info);
    }
  else
    {
      if (key_info->info != NULL)
        gnome_keyring_info_free(key_info->info);

      key_info->info = NULL;
    }

  g_main_loop_quit(key_info->loop);

  return;
}


/* Callback function to get the default keyring string name. */
static void
callback_default_keyring(GnomeKeyringResult result,
                         const char *string,
                         void *data)
{
  struct gnome_keyring_baton *key_info =
                                  (struct gnome_keyring_baton*) data;

  if (result == GNOME_KEYRING_RESULT_OK && string != NULL)
    {
      key_info->keyring_name = strdup(string);
    }
  else
    {
      if (key_info->keyring_name != NULL)
        free((void*)key_info->keyring_name);
      key_info->keyring_name = NULL;
    }

  g_main_loop_quit(key_info->loop);

  return;
}

/* Returns the default keyring name. */
static char*
get_default_keyring_name(apr_pool_t *pool)
{
  char *def = NULL;
  struct gnome_keyring_baton key_info;

  key_info.info = NULL;
  key_info.keyring_name = NULL;

  /* Finds default keyring. */
  key_info.loop = g_main_loop_new(NULL, FALSE);
  gnome_keyring_get_default_keyring(
   (GnomeKeyringOperationGetStringCallback)callback_default_keyring,
   (void*)&key_info, NULL);
  g_main_loop_run(key_info.loop);

  if (key_info.keyring_name == NULL)
    {
      callback_destroy_data_keyring((void*)&key_info);
      return NULL;
    }

  def = strdup(key_info.keyring_name);
  callback_destroy_data_keyring((void*)&key_info);

  return def;
}

/* Returns TRUE if the KEYRING_NAME is locked. */
static svn_boolean_t
check_keyring_is_locked(const char *keyring_name)
{
  struct gnome_keyring_baton key_info;

  key_info.info = NULL;
  key_info.keyring_name = NULL;

  /* Get details about the default keyring. */
  key_info.loop = g_main_loop_new(NULL, FALSE);
  gnome_keyring_get_info(keyring_name,
        (GnomeKeyringOperationGetKeyringInfoCallback)callback_get_info_keyring,
        (void*)&key_info, NULL);
  g_main_loop_run(key_info.loop);

  if (key_info.info == NULL)
    {
      callback_destroy_data_keyring((void*)&key_info);
      return FALSE;
    }

  /* Check if keyring is locked. */
  if (gnome_keyring_info_get_is_locked(key_info.info))
    return TRUE;
  else
    return FALSE;
}

/* Unlock the KEYRING_NAME with the KEYRING_PASSWORD. If KEYRING was
   successfully unlocked return TRUE. */
static svn_boolean_t
unlock_gnome_keyring(const char *keyring_name,
                     const char *keyring_password,
                     apr_pool_t *pool)
{
  struct gnome_keyring_baton key_info;

  key_info.info = NULL;
  key_info.keyring_name = NULL;

  /* Get details about the default keyring. */
  key_info.loop = g_main_loop_new(NULL, FALSE);
  gnome_keyring_get_info(keyring_name,
        (GnomeKeyringOperationGetKeyringInfoCallback)callback_get_info_keyring,
        (void*)&key_info, NULL);
  g_main_loop_run(key_info.loop);

  if (key_info.info == NULL)
    {
      callback_destroy_data_keyring((void*)&key_info);
      return FALSE;
    }
  else
    {
      key_info.loop = g_main_loop_new(NULL, FALSE);
      gnome_keyring_unlock(keyring_name, keyring_password,
                 (GnomeKeyringOperationDoneCallback)callback_done,
                 (void*)&key_info, NULL);
      g_main_loop_run(key_info.loop);
    }
  callback_destroy_data_keyring((void*)&key_info);
  if (check_keyring_is_locked(keyring_name))
    return FALSE;

  return TRUE;
}

/* Implementation of password_get_t that retrieves the password
   from GNOME Keyring. */
static svn_boolean_t
password_get_gnome_keyring(const char **password,
                           apr_hash_t *creds,
                           const char *realmstring,
                           const char *username,
                           apr_hash_t *parameters,
                           svn_boolean_t non_interactive,
                           apr_pool_t *pool)
{
  char *default_keyring = NULL;

  if (! dbus_bus_get(DBUS_BUS_SESSION, NULL))
    return FALSE;

  if (! gnome_keyring_is_available())
    return FALSE;

  default_keyring = get_default_keyring_name(pool);

  GnomeKeyringResult result;
  GList *items;
  svn_boolean_t ret = FALSE;

  if (! apr_hash_get(parameters,
                     "gnome-keyring-opening-failed",
                     APR_HASH_KEY_STRING))
    {
      result = gnome_keyring_find_network_password_sync(username, realmstring,
                                                        NULL, NULL, NULL, NULL,
                                                        0, &items);
    }
  else
    {
      result = GNOME_KEYRING_RESULT_DENIED;
    }

  if (result == GNOME_KEYRING_RESULT_OK)
    {
      if (items && items->data)
        {
          GnomeKeyringNetworkPasswordData *item;
          item = (GnomeKeyringNetworkPasswordData *)items->data;
          if (item->password)
            {
              size_t len = strlen(item->password);
              if (len > 0)
                {
                  *password = apr_pstrmemdup(pool, item->password, len);
                  ret = TRUE;
                }
            }
          gnome_keyring_network_password_list_free(items);
        }
    }
  else
    {
      apr_hash_set(parameters,
                   "gnome-keyring-opening-failed",
                   APR_HASH_KEY_STRING,
                   "");
    }

  if (default_keyring)
    free(default_keyring);

  return ret;
}

/* Implementation of password_set_t that stores the password in
   GNOME Keyring. */
static svn_boolean_t
password_set_gnome_keyring(apr_hash_t *creds,
                           const char *realmstring,
                           const char *username,
                           const char *password,
                           apr_hash_t *parameters,
                           svn_boolean_t non_interactive,
                           apr_pool_t *pool)
{
  char *default_keyring = NULL;

  if (! dbus_bus_get(DBUS_BUS_SESSION, NULL))
    return FALSE;

  if (! gnome_keyring_is_available())
    return FALSE;

  default_keyring = get_default_keyring_name(pool);

  GnomeKeyringResult result;
  guint32 item_id;

  if (! apr_hash_get(parameters,
                     "gnome-keyring-opening-failed",
                     APR_HASH_KEY_STRING))
    {
      result = gnome_keyring_set_network_password_sync(NULL, /* default keyring */
                                                       username, realmstring,
                                                       NULL, NULL, NULL, NULL,
                                                       0, password,
                                                       &item_id);
    }
  else
    {
      result = GNOME_KEYRING_RESULT_DENIED;
    }
  if (result != GNOME_KEYRING_RESULT_OK)
    {
      apr_hash_set(parameters,
                   "gnome-keyring-opening-failed",
                   APR_HASH_KEY_STRING,
                   "");
    }

  if (default_keyring)
    free(default_keyring);

  return result == GNOME_KEYRING_RESULT_OK;
}

/* Get cached encrypted credentials from the simple provider's cache. */
static svn_error_t *
simple_gnome_keyring_first_creds(void **credentials,
                                 void **iter_baton,
                                 void *provider_baton,
                                 apr_hash_t *parameters,
                                 const char *realmstring,
                                 apr_pool_t *pool)
{
  svn_boolean_t non_interactive = apr_hash_get(parameters,
                                               SVN_AUTH_PARAM_NON_INTERACTIVE,
                                               APR_HASH_KEY_STRING) != NULL;
  const char *default_keyring = get_default_keyring_name(pool);
  if (! non_interactive)
    {
      svn_auth_gnome_keyring_unlock_prompt_func_t unlock_prompt_func =
        apr_hash_get(parameters,
                     SVN_AUTH_PARAM_GNOME_KEYRING_UNLOCK_PROMPT_FUNC,
                     APR_HASH_KEY_STRING);
      void *unlock_prompt_baton =
        apr_hash_get(parameters, SVN_AUTH_PARAM_GNOME_KEYRING_UNLOCK_PROMPT_BATON,
                     APR_HASH_KEY_STRING);

      char *keyring_password;

      if (check_keyring_is_locked(default_keyring))
        {
          if (unlock_prompt_func)
            {
              SVN_ERR((*unlock_prompt_func)(&keyring_password,
                                            default_keyring,
                                            unlock_prompt_baton,
                                            pool));

              /* If keyring is locked give up and try the next provider. */
              if (! unlock_gnome_keyring(default_keyring, keyring_password,
                                         pool))
                return SVN_NO_ERROR;
            }
        }
    }

  if (check_keyring_is_locked(default_keyring))
    {
      return svn_error_create(SVN_ERR_AUTHN_CREDS_UNAVAILABLE, NULL,
                              _("GNOME Keyring is locked and "
                                "we are non-interactive"));
    }
  else
    {
      return svn_auth__simple_first_creds_helper
               (credentials,
                iter_baton, provider_baton,
                parameters, realmstring,
                password_get_gnome_keyring,
                SVN_AUTH__GNOME_KEYRING_PASSWORD_TYPE,
                pool);
    }
}

/* Save encrypted credentials to the simple provider's cache. */
static svn_error_t *
simple_gnome_keyring_save_creds(svn_boolean_t *saved,
                                void *credentials,
                                void *provider_baton,
                                apr_hash_t *parameters,
                                const char *realmstring,
                                apr_pool_t *pool)
{
  svn_boolean_t non_interactive = apr_hash_get(parameters,
                                               SVN_AUTH_PARAM_NON_INTERACTIVE,
                                               APR_HASH_KEY_STRING) != NULL;
  const char *default_keyring = get_default_keyring_name(pool);
  if (! non_interactive)
    {
      svn_auth_gnome_keyring_unlock_prompt_func_t unlock_prompt_func =
        apr_hash_get(parameters,
                     SVN_AUTH_PARAM_GNOME_KEYRING_UNLOCK_PROMPT_FUNC,
                     APR_HASH_KEY_STRING);
      void *unlock_prompt_baton =
        apr_hash_get(parameters, SVN_AUTH_PARAM_GNOME_KEYRING_UNLOCK_PROMPT_BATON,
                     APR_HASH_KEY_STRING);

      char *keyring_password;

      if (check_keyring_is_locked(default_keyring))
        {
          if (unlock_prompt_func)
            {
              SVN_ERR((*unlock_prompt_func)(&keyring_password,
                                            default_keyring,
                                            unlock_prompt_baton,
                                            pool));

              /* If keyring is locked give up and try the next provider. */
              if (! unlock_gnome_keyring(default_keyring, keyring_password,
                                         pool))
                return SVN_NO_ERROR;
            }
        }
    }
  if (check_keyring_is_locked(default_keyring))
    {
      return svn_error_create(SVN_ERR_AUTHN_CREDS_NOT_SAVED, NULL,
                              _("GNOME Keyring is locked and "
                                "we are non-interactive"));
    }
  else
    {
      return svn_auth__simple_save_creds_helper
               (saved, credentials,
                provider_baton, parameters,
                realmstring,
                password_set_gnome_keyring,
                SVN_AUTH__GNOME_KEYRING_PASSWORD_TYPE,
                pool);
    }
}

static void
log_noop(const gchar *log_domain, GLogLevelFlags log_level,
         const gchar *message, gpointer user_data)
{
  /* do nothing */
}

static void
init_gnome_keyring(void)
{
  const char *application_name = NULL;
  application_name = g_get_application_name();
  if (!application_name)
    g_set_application_name("Subversion");

  /* Ideally we call g_log_set_handler() with a log_domain specific to
     libgnome-keyring.  Unfortunately, at least as of gnome-keyring
     2.22.3, it doesn't have its own log_domain.  As a result, we
     suppress stderr spam for not only libgnome-keyring, but for
     anything else the app is linked to that uses glib logging and
     doesn't specify a log_domain. */
  g_log_set_default_handler(log_noop, NULL);
}

static const svn_auth_provider_t gnome_keyring_simple_provider = {
  SVN_AUTH_CRED_SIMPLE,
  simple_gnome_keyring_first_creds,
  NULL,
  simple_gnome_keyring_save_creds
};

/* Public API */
void
svn_auth_get_gnome_keyring_simple_provider
    (svn_auth_provider_object_t **provider,
     apr_pool_t *pool)
{
  svn_auth_provider_object_t *po = apr_pcalloc(pool, sizeof(*po));

  po->vtable = &gnome_keyring_simple_provider;
  *provider = po;

  init_gnome_keyring();
}


/*-----------------------------------------------------------------------*/
/* GNOME Keyring SSL client certificate passphrase provider,             */
/* puts passphrases in GNOME Keyring                                     */
/*-----------------------------------------------------------------------*/

/* Get cached encrypted credentials from the ssl client cert password
   provider's cache. */
static svn_error_t *
ssl_client_cert_pw_gnome_keyring_first_creds(void **credentials,
                                             void **iter_baton,
                                             void *provider_baton,
                                             apr_hash_t *parameters,
                                             const char *realmstring,
                                             apr_pool_t *pool)
{
  svn_boolean_t non_interactive = apr_hash_get(parameters,
                                               SVN_AUTH_PARAM_NON_INTERACTIVE,
                                               APR_HASH_KEY_STRING) != NULL;
  const char *default_keyring = get_default_keyring_name(pool);
  if (! non_interactive)
    {
      svn_auth_gnome_keyring_unlock_prompt_func_t unlock_prompt_func =
        apr_hash_get(parameters,
                     SVN_AUTH_PARAM_GNOME_KEYRING_UNLOCK_PROMPT_FUNC,
                     APR_HASH_KEY_STRING);
      void *unlock_prompt_baton =
        apr_hash_get(parameters, SVN_AUTH_PARAM_GNOME_KEYRING_UNLOCK_PROMPT_BATON,
                     APR_HASH_KEY_STRING);

      char *keyring_password;

      if (check_keyring_is_locked(default_keyring))
        {
          if (unlock_prompt_func)
            {
              SVN_ERR((*unlock_prompt_func)(&keyring_password,
                                            default_keyring,
                                            unlock_prompt_baton,
                                            pool));
            
              /* If keyring is locked give up and try the next provider. */
              if (! unlock_gnome_keyring(default_keyring, keyring_password,
                                         pool))
                return SVN_NO_ERROR;
            }
        }
    }
  if (check_keyring_is_locked(default_keyring))
    {
      return svn_error_create(SVN_ERR_AUTHN_CREDS_UNAVAILABLE, NULL,
                              _("GNOME Keyring is locked and "
                                "we are non-interactive"));
    }
  else
    {
      return svn_auth__ssl_client_cert_pw_file_first_creds_helper
               (credentials,
                iter_baton, provider_baton,
                parameters, realmstring,
                password_get_gnome_keyring,
                SVN_AUTH__GNOME_KEYRING_PASSWORD_TYPE,
                pool);
    }
}

/* Save encrypted credentials to the ssl client cert password provider's
   cache. */
static svn_error_t *
ssl_client_cert_pw_gnome_keyring_save_creds(svn_boolean_t *saved,
                                            void *credentials,
                                            void *provider_baton,
                                            apr_hash_t *parameters,
                                            const char *realmstring,
                                            apr_pool_t *pool)
{
  svn_boolean_t non_interactive = apr_hash_get(parameters,
                                               SVN_AUTH_PARAM_NON_INTERACTIVE,
                                               APR_HASH_KEY_STRING) != NULL;
  const char *default_keyring = get_default_keyring_name(pool);
  if (! non_interactive)
    {
      svn_auth_gnome_keyring_unlock_prompt_func_t unlock_prompt_func =
        apr_hash_get(parameters,
                     SVN_AUTH_PARAM_GNOME_KEYRING_UNLOCK_PROMPT_FUNC,
                     APR_HASH_KEY_STRING);
      void *unlock_prompt_baton =
        apr_hash_get(parameters, SVN_AUTH_PARAM_GNOME_KEYRING_UNLOCK_PROMPT_BATON,
                     APR_HASH_KEY_STRING);

      char *keyring_password;

      if (check_keyring_is_locked(default_keyring))
        {
          if (unlock_prompt_func)
            {
              SVN_ERR((*unlock_prompt_func)(&keyring_password,
                                            default_keyring,
                                            unlock_prompt_baton,
                                            pool));

              /* If keyring is locked give up and try the next provider. */
              if (! unlock_gnome_keyring(default_keyring, keyring_password,
                                         pool))
                return SVN_NO_ERROR;
            }
        }
    }
  if (check_keyring_is_locked(default_keyring))
    {
      return svn_error_create(SVN_ERR_AUTHN_CREDS_UNAVAILABLE, NULL,
                              _("GNOME Keyring is locked and "
                                "we are non-interactive"));
    }
  else
    {
      return svn_auth__ssl_client_cert_pw_file_save_creds_helper
               (saved, credentials,
                provider_baton, parameters,
                realmstring,
                password_set_gnome_keyring,
                SVN_AUTH__GNOME_KEYRING_PASSWORD_TYPE,
                pool);
    }
}

static const svn_auth_provider_t gnome_keyring_ssl_client_cert_pw_provider = {
  SVN_AUTH_CRED_SSL_CLIENT_CERT_PW,
  ssl_client_cert_pw_gnome_keyring_first_creds,
  NULL,
  ssl_client_cert_pw_gnome_keyring_save_creds
};

/* Public API */
void
svn_auth_get_gnome_keyring_ssl_client_cert_pw_provider
    (svn_auth_provider_object_t **provider,
     apr_pool_t *pool)
{
  svn_auth_provider_object_t *po = apr_pcalloc(pool, sizeof(*po));

  po->vtable = &gnome_keyring_ssl_client_cert_pw_provider;
  *provider = po;

  init_gnome_keyring();
}
