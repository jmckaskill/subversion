/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2003 CollabNet.  All rights reserved.
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
 * @endcopyright
 *
 * @file Prompter.cpp
 * @brief Implementation of the class Prompter
 */

#include "Prompter.h"
#include "Pool.h"
#include "JNIUtil.h"
#include "JNIStringHolder.h"
#include "../include/org_tigris_subversion_javahl_PromptUserPassword2.h"
#include "svn_client.h"
#include "svn_private_config.h"

/**
 * Constructor
 * @param jprompter     a global reference to the java callback object
 * @param v2            the callback objects implements PromptUserPassword2
 * @param v3            the callback objects implements PromptUserPassword3
 */
Prompter::Prompter(jobject jprompter, bool v2, bool v3)
{
    m_prompter = jprompter;
    m_version2 = v2;
    m_version3 = v3;
}

/**
 * Destructor
 */
Prompter::~Prompter()
{
    if (m_prompter!= NULL)
    {
        // since the reference to the java object is a global one, it has to
        // be deleted
        JNIEnv *env = JNIUtil::getEnv();
        env->DeleteGlobalRef(m_prompter);
    }
}
/**
 * Create a C++ peer object for the java callback object
 *
 * @param jprompter     java callback object
 * @return              C++ peer object
 */
Prompter *Prompter::makeCPrompter(jobject jprompter)
{
    if (jprompter == NULL) // if we have no java object -> we need no C++ object
    {
        return NULL;
    }
    JNIEnv *env = JNIUtil::getEnv();

    // sanity check that the java object implements PromptUserPassword
    jclass clazz = env->FindClass(JAVA_PACKAGE"/PromptUserPassword");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    if (!env->IsInstanceOf(jprompter, clazz))
    {
        env->DeleteLocalRef(clazz);
        return NULL;
    }
    env->DeleteLocalRef(clazz);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    // check if PromptUserPassword2 is implemented by the java object
    jclass clazz2 = env->FindClass(JAVA_PACKAGE"/PromptUserPassword2");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    bool v2 = env->IsInstanceOf(jprompter, clazz2) ? true: false;
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(clazz2);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    bool v3 = false;
    if (v2)
    {
        // check if PromptUserPassword3 is implemented by the java object
        jclass clazz3 = env->FindClass(JAVA_PACKAGE"/PromptUserPassword3");
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        v3 = env->IsInstanceOf(jprompter, clazz3) ? true: false;
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        env->DeleteLocalRef(clazz3);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }

    // create a new global ref for the java object, because it is longer used
    // that this call
    jobject myPrompt = env->NewGlobalRef(jprompter);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    // create the C++ peer
    return new Prompter(myPrompt, v2, v3);
}

/**
 * Retrieve the username from the java object
 * @return java string for the username or NULL
 */
jstring Prompter::username()
{
    JNIEnv *env = JNIUtil::getEnv();
    // the method id will not change during
    // the time this library is loaded, so
    // it can be cached.
    static jmethodID mid = 0;
    if (mid == 0)
    {
        jclass clazz = env->FindClass(JAVA_PACKAGE"/PromptUserPassword");
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        mid = env->GetMethodID(clazz, "getUsername", "()Ljava/lang/String;");
        if (JNIUtil::isJavaExceptionThrown() || mid == 0)
        {
            return NULL;
        }
        env->DeleteLocalRef(clazz);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }
    jstring ret = static_cast<jstring>(env->CallObjectMethod(m_prompter, mid));
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    return ret;
}

/**
 * Retrieve the username from the java object
 * @return java string for the username or NULL
 */
jstring Prompter::password()
{
    JNIEnv *env = JNIUtil::getEnv();
    // the method id will not change during
    // the time this library is loaded, so
    // it can be cached.
    static jmethodID mid = 0;
    if (mid == 0)
    {
        jclass clazz = env->FindClass(JAVA_PACKAGE"/PromptUserPassword");
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        mid = env->GetMethodID(clazz, "getPassword", "()Ljava/lang/String;");
        if (JNIUtil::isJavaExceptionThrown() || mid == 0)
        {
            return NULL;
        }
        env->DeleteLocalRef(clazz);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
    }

    jstring ret = static_cast<jstring>(env->CallObjectMethod(m_prompter, mid));
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    return ret;
}
/**
 * Ask the user a question, which can be answered by yes/no.
 * @param realm         the server realm, for which this question is asked
 * @param question      the question to ask the user
 * @param yesIsDefault  flag if the yes-button should be the default button
 * @return flag who the user answered the question
 */
bool Prompter::askYesNo(const char *realm, const char *question, 
                        bool yesIsDefault)
{
    JNIEnv *env = JNIUtil::getEnv();
    // the method id will not change during
    // the time this library is loaded, so
    // it can be cached.
    static jmethodID mid = 0;
    if (mid == 0)
    {
        jclass clazz = env->FindClass(JAVA_PACKAGE"/PromptUserPassword");
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        mid = env->GetMethodID(clazz, "askYesNo", 
            "(Ljava/lang/String;Ljava/lang/String;Z)Z");
        if (JNIUtil::isJavaExceptionThrown() || mid == 0)
        {
            return false;
        }
        env->DeleteLocalRef(clazz);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
    }

    // convert the texts to java strings
    jstring jrealm = JNIUtil::makeJString(realm);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return false;
    }
    jstring jquestion = JNIUtil::makeJString(question);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return false;
    }

    // execute the callback
    jboolean ret = env->CallBooleanMethod(m_prompter, mid, jrealm, jquestion, 
                                          yesIsDefault ? JNI_TRUE : JNI_FALSE);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return false;
    }

    // delete the java strings
    env->DeleteLocalRef(jquestion);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return false;
    }
    env->DeleteLocalRef(jrealm);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return false;
    }
    return ret ? true:false;
}

/**
 *
 */
const char *Prompter::askQuestion(const char *realm, const char *question, 
                                  bool showAnswer, bool maySave)
{
    JNIEnv *env = JNIUtil::getEnv();
    if (m_version3)
    {
        static jmethodID mid = 0;
        static jmethodID mid2 = 0;
        if (mid == 0)
        {
            jclass clazz = env->FindClass(JAVA_PACKAGE"/PromptUserPassword3");
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
            mid = env->GetMethodID(clazz, "askQuestion", 
                "(Ljava/lang/String;Ljava/lang/String;ZZ)Ljava/lang/String;");
            if (JNIUtil::isJavaExceptionThrown() || mid == 0)
            {
                return NULL;
            }
            mid2 = env->GetMethodID(clazz, "userAllowedSave", "()Z");
            if (JNIUtil::isJavaExceptionThrown() || mid == 0)
            {
                return NULL;
            }
            env->DeleteLocalRef(clazz);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
        }

        jstring jrealm = JNIUtil::makeJString(realm);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        jstring jquestion = JNIUtil::makeJString(question);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        jstring janswer = static_cast<jstring>(
            env->CallObjectMethod(m_prompter, mid, jrealm, jquestion, 
                                  showAnswer ? JNI_TRUE : JNI_FALSE,
                                  maySave ? JNI_TRUE : JNI_FALSE));
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        env->DeleteLocalRef(jquestion);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        env->DeleteLocalRef(jrealm);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        JNIStringHolder answer(janswer);
        if (answer != NULL)
        {
            m_answer = answer;
            m_maySave = env->CallBooleanMethod(m_prompter, mid2) ? true: false;
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
        }
        else
        {
            m_answer = "";
            m_maySave = false;
        }
        return m_answer.c_str();
    }
    else
    {
        static jmethodID mid = 0;
        if (mid == 0)
        {
            jclass clazz = env->FindClass(JAVA_PACKAGE"/PromptUserPassword");
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
            mid = env->GetMethodID(clazz, "askQuestion", 
                "(Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/String;");
            if (JNIUtil::isJavaExceptionThrown() || mid == 0)
            {
                return NULL;
            }
            env->DeleteLocalRef(clazz);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
        }

        jstring jrealm = JNIUtil::makeJString(realm);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        jstring jquestion = JNIUtil::makeJString(question);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        jstring janswer = static_cast<jstring>(
            env->CallObjectMethod(m_prompter, mid, jrealm, jquestion, 
                                  showAnswer ? JNI_TRUE : JNI_FALSE));
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        env->DeleteLocalRef(jquestion);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        env->DeleteLocalRef(jrealm);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        JNIStringHolder answer(janswer);
        if (answer != NULL)
        {
            m_answer = answer;
            if (maySave)
                m_maySave = askYesNo(realm, _("May save the answer ?"), true);
            else
                m_maySave = false;
        }
        else
        {
            m_answer = "";
            m_maySave = false;
        }
        return m_answer.c_str();
    }
}
int Prompter::askTrust(const char *question, bool maySave)
{
    if (m_version2)
    {
        static jmethodID mid = 0;
        JNIEnv *env = JNIUtil::getEnv();
        if (mid == 0)
        {
            jclass clazz = env->FindClass(JAVA_PACKAGE"/PromptUserPassword2");
            if (JNIUtil::isJavaExceptionThrown())
            {
                return -1;
            }
            mid = env->GetMethodID(clazz, "askTrustSSLServer", 
                                   "(Ljava/lang/String;Z)I");
            if (JNIUtil::isJavaExceptionThrown() || mid == 0)
            {
                return -1;
            }
            env->DeleteLocalRef(clazz);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return -1;
            }
        }
        jstring jquestion = JNIUtil::makeJString(question);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return -1;
        }
        jint ret = env->CallIntMethod(m_prompter, mid, jquestion, 
                                      maySave ? JNI_TRUE : JNI_FALSE);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return -1;
        }
        env->DeleteLocalRef(jquestion);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return -1;
        }
        return ret;
    }
    else
    {
        std::string q = question;
        if (maySave)
        {
            q += _("(R)eject, accept (t)emporarily or accept (p)ermanently?");
        }
        else
        {
            q += _("(R)eject or accept (t)emporarily?");
        }
        const char *answer = askQuestion(NULL, q.c_str(), true, false);
        if (*answer == 't' || *answer == 'T')
        {
            return 
              org_tigris_subversion_javahl_PromptUserPassword2_AcceptTemporary;
        }
        else if (maySave && (*answer == 'p' || *answer == 'P'))
        {
            return 
             org_tigris_subversion_javahl_PromptUserPassword2_AcceptPermanently;
        }
        else
            return org_tigris_subversion_javahl_PromptUserPassword2_Reject;
    }
    return -1;
}

bool Prompter::prompt(const char *realm, const char *pi_username, bool maySave)
{
    JNIEnv *env = JNIUtil::getEnv();
    if (m_version3)
    {
        static jmethodID mid = 0;
        static jmethodID mid2 = 0;
        if (mid == 0)
        {
            jclass clazz = env->FindClass(JAVA_PACKAGE"/PromptUserPassword3");
            if (JNIUtil::isJavaExceptionThrown())
            {
                return false;
            }
            mid = env->GetMethodID(clazz, "prompt", 
                                   "(Ljava/lang/String;Ljava/lang/String;Z)Z");
            if (JNIUtil::isJavaExceptionThrown() || mid == 0)
            {
                return false;
            }
            mid2 = env->GetMethodID(clazz, "userAllowedSave", "()Z");
            if (JNIUtil::isJavaExceptionThrown() || mid == 0)
            {
                return false;
            }
            env->DeleteLocalRef(clazz);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return false;
            }
        }

        jstring jrealm = JNIUtil::makeJString(realm);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        jstring jusername = JNIUtil::makeJString(pi_username);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        jboolean ret = env->CallBooleanMethod(m_prompter, mid, jrealm,
                                    jusername, maySave ? JNI_TRUE: JNI_FALSE);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        env->DeleteLocalRef(jusername);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        env->DeleteLocalRef(jrealm);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        m_maySave = env->CallBooleanMethod(m_prompter, mid2) ? true : false;
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        return ret ? true:false;
    }
    else
    {
        static jmethodID mid = 0;
        if (mid == 0)
        {
            jclass clazz = env->FindClass(JAVA_PACKAGE"/PromptUserPassword");
            if (JNIUtil::isJavaExceptionThrown())
            {
                return false;
            }
            mid = env->GetMethodID(clazz, "prompt", 
                                   "(Ljava/lang/String;Ljava/lang/String;)Z");
            if (JNIUtil::isJavaExceptionThrown() || mid == 0)
            {
                return false;
            }
            env->DeleteLocalRef(clazz);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return false;
            }
        }

        jstring jrealm = JNIUtil::makeJString(realm);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        jstring jusername = JNIUtil::makeJString(pi_username);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        jboolean ret = env->CallBooleanMethod(m_prompter, mid, jrealm, 
            jusername);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        env->DeleteLocalRef(jusername);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        env->DeleteLocalRef(jrealm);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return false;
        }
        if (maySave)
            m_maySave = askYesNo(realm, _("May save the answer ?"), true);
        else
            m_maySave = false;
        return ret ? true:false;
    }
}
svn_auth_provider_object_t *Prompter::getProviderSimple()
{
    apr_pool_t *pool = JNIUtil::getRequestPool()->pool();
    svn_auth_provider_object_t *provider;
    svn_client_get_simple_prompt_provider(&provider,
                                          simple_prompt,
                                          this,
                                          2, /* retry limit */
                                          pool);

    return provider;
}
svn_auth_provider_object_t *Prompter::getProviderUsername()
{
    apr_pool_t *pool = JNIUtil::getRequestPool()->pool();
    svn_auth_provider_object_t *provider;
    svn_client_get_username_prompt_provider(&provider,
                                            username_prompt,
                                            this,
                                            2, /* retry limit */
                                            pool);

    return provider;
}
svn_auth_provider_object_t *Prompter::getProviderServerSSLTrust()
{
    apr_pool_t *pool = JNIUtil::getRequestPool()->pool();
    svn_auth_provider_object_t *provider;
    svn_client_get_ssl_server_trust_prompt_provider
          (&provider, ssl_server_trust_prompt, this, pool);

    return provider;
}
svn_auth_provider_object_t *Prompter::getProviderClientSSL()
{
    apr_pool_t *pool = JNIUtil::getRequestPool()->pool();
    svn_auth_provider_object_t *provider;
    svn_client_get_ssl_client_cert_prompt_provider
          (&provider, ssl_client_cert_prompt, this, 2, /* retry limit */pool);

    return provider;
}
svn_auth_provider_object_t *Prompter::getProviderClientSSLPassword()
{
    apr_pool_t *pool = JNIUtil::getRequestPool()->pool();
    svn_auth_provider_object_t *provider;
    svn_client_get_ssl_client_cert_pw_prompt_provider
          (&provider, ssl_client_cert_pw_prompt, this, 2 /* retry limit */,
                                                         pool);

    return provider;
}
svn_error_t *Prompter::simple_prompt(svn_auth_cred_simple_t **cred_p, 
                                     void *baton,
                                     const char *realm, const char *username, 
                                     svn_boolean_t may_save,
                                     apr_pool_t *pool)
{
    Prompter *that = (Prompter*)baton;
    svn_auth_cred_simple_t *ret = (svn_auth_cred_simple_t*)apr_pcalloc(pool, 
                                                                sizeof(*ret));
    if (!that->prompt(realm, username, may_save ? true : false))
        return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, NULL,
                        _("User canceled dialog"));
    jstring juser = that->username();
    JNIStringHolder user(juser);
    if (user == NULL)
        return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, NULL,
                        _("User canceled dialog"));
    ret->username = apr_pstrdup(pool,user);
    jstring jpass = that->password();
    JNIStringHolder pass(jpass);
    if (pass == NULL)
        return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, NULL,
                            _("User canceled dialog"));
    else
    {
        ret->password  = apr_pstrdup(pool, pass);
        ret->may_save = that->m_maySave;
    }
    *cred_p = ret;
    return SVN_NO_ERROR;
}
svn_error_t *Prompter::username_prompt(svn_auth_cred_username_t **cred_p, 
                                       void *baton,
                                       const char *realm, 
                                       svn_boolean_t may_save, 
                                       apr_pool_t *pool)
{
    Prompter *that = (Prompter*)baton;
    svn_auth_cred_username_t *ret = 
        (svn_auth_cred_username_t*)apr_pcalloc(pool, sizeof(*ret));
    const char *user = that->askQuestion(realm, _("Username: "), true, 
                                         may_save ? true : false);
    if (user == NULL)
        return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, NULL,
                        _("User canceled dialog"));
    ret->username = apr_pstrdup(pool,user);
    ret->may_save = that->m_maySave;
    *cred_p = ret;
    return SVN_NO_ERROR;
}
svn_error_t *Prompter::ssl_server_trust_prompt(
                              svn_auth_cred_ssl_server_trust_t **cred_p,
                              void *baton,
                              const char *realm,
                              apr_uint32_t failures,
                              const svn_auth_ssl_server_cert_info_t *cert_info,
                              svn_boolean_t may_save,
                              apr_pool_t *pool)
{
    Prompter *that = (Prompter*)baton;
    svn_auth_cred_ssl_server_trust_t *ret = 
            (svn_auth_cred_ssl_server_trust_t*)apr_pcalloc(pool, sizeof(*ret));

    std::string question = _("Error validating server certificate for ");
    question += realm;
    question += ":\n";

    if (failures & SVN_AUTH_SSL_UNKNOWNCA)
    {
        question += _(" - Unknown certificate issuer\n");
        question += _("   Fingerprint: ");
        question += cert_info->fingerprint;
        question += "\n";
        question += _("   Distinguished name: ");
        question += cert_info->issuer_dname;
        question += "\n";
    }

    if (failures & SVN_AUTH_SSL_CNMISMATCH)
    {
        question += _(" - Hostname mismatch (");
        question += cert_info->hostname;
        question += _(")\n");
    }

    if (failures & SVN_AUTH_SSL_NOTYETVALID)
    {
        question += _(" - Certificate is not yet valid\n");
        question += _("   Valid from ");
        question += cert_info->valid_from;
        question += "\n";
    }

    if (failures & SVN_AUTH_SSL_EXPIRED)
    {
        question += _(" - Certificate is expired\n");
        question += _("   Valid until ");
        question += cert_info->valid_until;
        question += "\n";
    }

    switch(that->askTrust(question.c_str(), may_save ? true : false))
    {
    case org_tigris_subversion_javahl_PromptUserPassword2_AcceptTemporary:
        *cred_p = ret;
        ret->may_save = FALSE;
        break;
    case org_tigris_subversion_javahl_PromptUserPassword2_AcceptPermanently:
        *cred_p = ret;
        ret->may_save = TRUE;
        ret->accepted_failures = failures;
        break;
    default:
        *cred_p = NULL;
    }
    return SVN_NO_ERROR;
}
svn_error_t *Prompter::ssl_client_cert_prompt(
                                     svn_auth_cred_ssl_client_cert_t **cred_p,
                                     void *baton, 
                                     const char *realm, 
                                     svn_boolean_t may_save,
                                     apr_pool_t *pool)
{
    Prompter *that = (Prompter*)baton;
    svn_auth_cred_ssl_client_cert_t *ret = 
        (svn_auth_cred_ssl_client_cert_t*)apr_pcalloc(pool, sizeof(*ret));
    const char *cert_file = that->askQuestion(realm, 
                                _("client certificate filename: "), true, 
                                may_save ? true : false);
    if (cert_file == NULL)
        return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, NULL,
                        _("User canceled dialog"));
    ret->cert_file = apr_pstrdup(pool, cert_file);
    ret->may_save = that->m_maySave;
    *cred_p = ret;
    return SVN_NO_ERROR;
}
svn_error_t *Prompter::ssl_client_cert_pw_prompt(
                                  svn_auth_cred_ssl_client_cert_pw_t **cred_p,
                                  void *baton, 
                                  const char *realm, 
                                  svn_boolean_t may_save,
                                  apr_pool_t *pool)
{
    Prompter *that = (Prompter*)baton;
    svn_auth_cred_ssl_client_cert_pw_t *ret = 
        (svn_auth_cred_ssl_client_cert_pw_t*)apr_pcalloc(pool, sizeof(*ret));
    const char *info = that->askQuestion(realm, 
                                         _("client certificate passphrase: "), 
                                         false, may_save ? true : false);
    if (info == NULL)
        return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, NULL,
                        _("User canceled dialog"));
    ret->password = apr_pstrdup(pool, info);
    ret->may_save = that->m_maySave;
    *cred_p = ret;
    return SVN_NO_ERROR;
}

