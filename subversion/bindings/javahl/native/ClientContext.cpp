/**
 * @copyright
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
 * @endcopyright
 *
 * @file ClientContext.cpp
 * @brief Implementation of the class ClientContext
 */

#include "svn_client.h"
#include "svn_private_config.h"

#include "ClientContext.h"
#include "JNIUtil.h"
#include "JNICriticalSection.h"

#include "Prompter.h"
#include "CreateJ.h"
#include "CommitMessage.h"
#include "ConflictResolverCallback.h"


struct log_msg_baton
{
    const char *message;
    CommitMessage *messageHandler;
};

ClientContext::ClientContext(jobject jsvnclient)
    : m_prompter(NULL),
      m_commitMessage(NULL),
      m_conflictResolver(NULL)
{
    JNIEnv *env = JNIUtil::getEnv();
    JNICriticalSection criticalSection(*JNIUtil::getGlobalPoolMutex());

    /* Grab a global reference to the Java object embedded in the parent Java
       object. */
    static jfieldID ctxFieldID = 0;
    if (ctxFieldID == 0)
    {
        jclass clazz = env->GetObjectClass(jsvnclient);
        if (JNIUtil::isJavaExceptionThrown())
            return;

        ctxFieldID = env->GetFieldID(clazz, "clientContext",
                                "L"JAVA_PACKAGE"/SVNClient$ClientContext;");
        if (JNIUtil::isJavaExceptionThrown() || ctxFieldID == 0)
            return;

        env->DeleteLocalRef(clazz);
    }

    jobject jctx = env->GetObjectField(jsvnclient, ctxFieldID);
    if (JNIUtil::isJavaExceptionThrown())
        return;

    m_jctx = env->NewGlobalRef(jctx);
    if (JNIUtil::isJavaExceptionThrown())
        return;

    env->DeleteLocalRef(jctx);

    /* Create a long-lived client context object in the global pool. */
    SVN_JNI_ERR(svn_client_create_context(&persistentCtx, JNIUtil::getPool()),
                );

    /* None of the following members change during the lifetime of
       this object. */
    persistentCtx->notify_func = NULL;
    persistentCtx->notify_baton = NULL;
    persistentCtx->log_msg_func3 = getCommitMessage;
    persistentCtx->cancel_func = checkCancel;
    persistentCtx->cancel_baton = this;
    persistentCtx->notify_func2= notify;
    persistentCtx->notify_baton2 = m_jctx;
    persistentCtx->progress_func = progress;
    persistentCtx->progress_baton = m_jctx;
}

ClientContext::~ClientContext()
{
    delete m_prompter;
    delete m_commitMessage;
    delete m_conflictResolver;

    JNIEnv *env = JNIUtil::getEnv();
    env->DeleteGlobalRef(m_jctx);
}

svn_client_ctx_t *
ClientContext::getContext(const char *message)
{
    SVN::Pool *requestPool = JNIUtil::getRequestPool();
    apr_pool_t *pool = requestPool->pool();
    svn_auth_baton_t *ab;
    svn_client_ctx_t *ctx = persistentCtx;
    //SVN_JNI_ERR(svn_client_create_context(&ctx, pool), NULL);

    const char *configDir = m_configDir.c_str();
    if (m_configDir.length() == 0)
        configDir = NULL;
    SVN_JNI_ERR(svn_config_get_config(&(ctx->config), configDir, pool), NULL);
    svn_config_t *config = (svn_config_t *) apr_hash_get(ctx->config,
                                                         SVN_CONFIG_CATEGORY_CONFIG,
                                                         APR_HASH_KEY_STRING);

    /* The whole list of registered providers */
    apr_array_header_t *providers;

    /* Populate the registered providers with the platform-specific providers */
    SVN_JNI_ERR(svn_auth_get_platform_specific_client_providers(&providers,
                                                                config,
                                                                pool),
                NULL);

    /* Use the prompter (if available) to prompt for password and cert
     * caching. */
    svn_auth_plaintext_prompt_func_t plaintext_prompt_func = NULL;
    void *plaintext_prompt_baton = NULL;
    svn_auth_plaintext_passphrase_prompt_func_t plaintext_passphrase_prompt_func;
    void *plaintext_passphrase_prompt_baton = NULL;

    if (m_prompter != NULL)
    {
        plaintext_prompt_func = Prompter::plaintext_prompt;
        plaintext_prompt_baton = m_prompter;
        plaintext_passphrase_prompt_func = Prompter::plaintext_passphrase_prompt;
        plaintext_passphrase_prompt_baton = m_prompter;
    }

    /* The main disk-caching auth providers, for both
     * 'username/password' creds and 'username' creds.  */
    svn_auth_provider_object_t *provider;

    svn_auth_get_simple_provider2(&provider, plaintext_prompt_func,
                                  plaintext_prompt_baton, pool);
    APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;

    svn_auth_get_username_provider(&provider, pool);
    APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;

    /* The server-cert, client-cert, and client-cert-password providers. */
    SVN_JNI_ERR(svn_auth_get_platform_specific_provider(&provider,
                                                        "windows",
                                                        "ssl_server_trust",
                                                        pool),
                NULL);

    if (provider)
        APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;

    svn_auth_get_ssl_server_trust_file_provider(&provider, pool);
    APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;
    svn_auth_get_ssl_client_cert_file_provider(&provider, pool);
    APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;
    svn_auth_get_ssl_client_cert_pw_file_provider2(&provider,
                        plaintext_passphrase_prompt_func,
                        plaintext_passphrase_prompt_baton, pool);
    APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;

    if (m_prompter != NULL)
    {
        /* Two basic prompt providers: username/password, and just username.*/
        provider = m_prompter->getProviderSimple();

        APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;

        provider = m_prompter->getProviderUsername();
        APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;

        /* Three ssl prompt providers, for server-certs, client-certs,
         * and client-cert-passphrases.  */
        provider = m_prompter->getProviderServerSSLTrust();
        APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;

        provider = m_prompter->getProviderClientSSL();
        APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;

        provider = m_prompter->getProviderClientSSLPassword();
        APR_ARRAY_PUSH(providers, svn_auth_provider_object_t *) = provider;
    }

    /* Build an authentication baton to give to libsvn_client. */
    svn_auth_open(&ab, providers, pool);

    /* Place any default --username or --password credentials into the
     * auth_baton's run-time parameter hash.  ### Same with --no-auth-cache? */
    if (!m_userName.empty())
        svn_auth_set_parameter(ab, SVN_AUTH_PARAM_DEFAULT_USERNAME,
                               m_userName.c_str());
    if (!m_passWord.empty())
        svn_auth_set_parameter(ab, SVN_AUTH_PARAM_DEFAULT_PASSWORD,
                               m_passWord.c_str());

    ctx->auth_baton = ab;
    ctx->log_msg_baton3 = getCommitMessageBaton(message);
    m_cancelOperation = false;

    if (m_conflictResolver)
    {
        ctx->conflict_func = ConflictResolverCallback::resolveConflict;
        ctx->conflict_baton = m_conflictResolver;
    }

    return ctx;
}

svn_error_t *
ClientContext::getCommitMessage(const char **log_msg,
                                const char **tmp_file,
                                const apr_array_header_t *commit_items,
                                void *baton,
                                apr_pool_t *pool)
{
    *log_msg = NULL;
    *tmp_file = NULL;
    log_msg_baton *lmb = (log_msg_baton *) baton;

    if (lmb && lmb->messageHandler)
    {
        jstring jmsg = lmb->messageHandler->getCommitMessage(commit_items);
        if (jmsg != NULL)
        {
            JNIStringHolder msg(jmsg);
            *log_msg = apr_pstrdup(pool, msg);
        }
        return SVN_NO_ERROR;
    }
    else if (lmb && lmb->message)
    {
        *log_msg = apr_pstrdup(pool, lmb->message);
        return SVN_NO_ERROR;
    }

    return SVN_NO_ERROR;
}

void *
ClientContext::getCommitMessageBaton(const char *message)
{
    if (message != NULL || m_commitMessage)
    {
        log_msg_baton *baton = (log_msg_baton *)
            apr_palloc(JNIUtil::getRequestPool()->pool(), sizeof(*baton));

        baton->message = message;
        baton->messageHandler = m_commitMessage;

        return baton;
    }
    return NULL;
}

void
ClientContext::username(const char *pi_username)
{
    m_userName = (pi_username == NULL ? "" : pi_username);
}

void
ClientContext::password(const char *pi_password)
{
    m_passWord = (pi_password == NULL ? "" : pi_password);
}

void
ClientContext::setPrompt(Prompter *prompter)
{
    delete m_prompter;
    m_prompter = prompter;
}

void
ClientContext::setConflictResolver(ConflictResolverCallback *conflictResolver)
{
    delete m_conflictResolver;
    m_conflictResolver = conflictResolver;
}

void
ClientContext::setConfigDirectory(const char *configDir)
{
    // A change to the config directory may necessitate creation of
    // the config templates.
    SVN::Pool requestPool;
    SVN_JNI_ERR(svn_config_ensure(configDir, requestPool.pool()), );

    m_configDir = (configDir == NULL ? "" : configDir);
}

const char *
ClientContext::getConfigDirectory()
{
    return m_configDir.c_str();
}

void
ClientContext::commitMessageHandler(CommitMessage *commitMessage)
{
    delete m_commitMessage;
    m_commitMessage = commitMessage;
}

void
ClientContext::cancelOperation()
{
    m_cancelOperation = true;
}

svn_error_t *
ClientContext::checkCancel(void *cancelBaton)
{
    ClientContext *that = (ClientContext *)cancelBaton;
    if (that->m_cancelOperation)
        return svn_error_create(SVN_ERR_CANCELLED, NULL,
                                _("Operation canceled"));
    else
        return SVN_NO_ERROR;
}

void
ClientContext::notify(void *baton,
                      const svn_wc_notify_t *notify,
                      apr_pool_t *pool)
{
  jobject jctx = (jobject) baton;
  JNIEnv *env = JNIUtil::getEnv();

  static jmethodID mid = 0;
  if (mid == 0)
    {
      jclass clazz = env->GetObjectClass(jctx);
      if (JNIUtil::isJavaExceptionThrown())
        return;

      mid = env->GetMethodID(clazz, "onNotify",
                             "(L"JAVA_PACKAGE"/ClientNotifyInformation;)V");
      if (JNIUtil::isJavaExceptionThrown() || mid == 0)
        return;

      env->DeleteLocalRef(clazz);
    }

  jobject jInfo = CreateJ::ClientNotifyInformation(notify, pool);
  if (JNIUtil::isJavaExceptionThrown())
    return;

  env->CallVoidMethod(jctx, mid, jInfo);
  if (JNIUtil::isJavaExceptionThrown())
    return;

  env->DeleteLocalRef(jInfo);
}

void
ClientContext::progress(apr_off_t progressVal, apr_off_t total,
                        void *baton, apr_pool_t *pool)
{
  jobject jctx = (jobject) baton;
  JNIEnv *env = JNIUtil::getEnv();

  // Create a local frame for our references
  env->PushLocalFrame(LOCAL_FRAME_SIZE);
  if (JNIUtil::isJavaExceptionThrown())
    return;

  static jmethodID mid = 0;
  if (mid == 0)
    {
      jclass clazz = env->GetObjectClass(jctx);
      if (JNIUtil::isJavaExceptionThrown())
        POP_AND_RETURN_NOTHING();

      mid = env->GetMethodID(clazz, "onProgress",
                             "(L"JAVA_PACKAGE"/ProgressEvent;)V");
      if (JNIUtil::isJavaExceptionThrown() || mid == 0)
        POP_AND_RETURN_NOTHING();
    }

  static jmethodID midCT = 0;
  jclass clazz = env->FindClass(JAVA_PACKAGE"/ProgressEvent");
  if (JNIUtil::isJavaExceptionThrown())
    POP_AND_RETURN_NOTHING();

  if (midCT == 0)
    {
      midCT = env->GetMethodID(clazz, "<init>", "(JJ)V");
      if (JNIUtil::isJavaExceptionThrown() || midCT == 0)
        POP_AND_RETURN_NOTHING();
    }

  // Call the Java method.
  jobject jevent = env->NewObject(clazz, midCT,
                                  (jlong) progressVal, (jlong) total);
  if (JNIUtil::isJavaExceptionThrown())
    POP_AND_RETURN_NOTHING();

  env->CallVoidMethod(jctx, mid, jevent);
  
  POP_AND_RETURN_NOTHING();
}
