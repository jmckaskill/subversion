/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2003-2007 CollabNet.  All rights reserved.
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
 */
// SVNClient.cpp: implementation of the SVNClient class.
//
//////////////////////////////////////////////////////////////////////

#include "SVNClient.h"
#include "JNIUtil.h"
#include "Notify.h"
#include "Notify2.h"
#include "CopySources.h"
#include "DiffSummaryReceiver.h"
#include "ProgressListener.h"
#include "Prompter.h"
#include "Pool.h"
#include "Targets.h"
#include "Revision.h"
#include "BlameCallback.h"
#include "ProplistCallback.h"
#include "LogMessageCallback.h"
#include "JNIByteArray.h"
#include "CommitMessage.h"
#include "EnumMapper.h"
#include "svn_types.h"
#include "svn_client.h"
#include "svn_sorts.h"
#include "svn_time.h"
#include "svn_diff.h"
#include "svn_config.h"
#include "svn_io.h"
#include "svn_path.h"
#include "svn_utf.h"
#include "svn_private_config.h"
#include "../include/org_tigris_subversion_javahl_Revision.h"
#include "../include/org_tigris_subversion_javahl_NodeKind.h"
#include "../include/org_tigris_subversion_javahl_StatusKind.h"
#include "JNIStringHolder.h"
#include <vector>
#include <iostream>
#include <sstream>
//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
struct log_msg_baton
{
    const char *message;
    CommitMessage *messageHandler;
};

SVNClient::SVNClient()
{
    m_notify = NULL;
    m_notify2 = NULL;
    m_progressListener = NULL;
    m_prompter = NULL;
    m_commitMessage = NULL;
}

SVNClient::~SVNClient()
{
    delete m_notify;
    delete m_notify2;
    delete m_progressListener;
    delete m_prompter;
}

SVNClient *SVNClient::getCppObject(jobject jthis)
{
    static jfieldID fid = 0;
    jlong cppAddr = SVNBase::findCppAddrForJObject(jthis, &fid,
                                                   JAVA_PACKAGE"/SVNClient");
    return (cppAddr == 0 ? NULL : reinterpret_cast<SVNClient *>(cppAddr));
}

void SVNClient::dispose(jobject jthis)
{
    static jfieldID fid = 0;
    SVNBase::dispose(jthis, &fid, JAVA_PACKAGE"/SVNClient");
}

jstring SVNClient::getAdminDirectoryName()
{
    Pool requestPool;
    jstring name = JNIUtil::makeJString(svn_wc_get_adm_dir(requestPool.pool()));
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    return name;
}

jboolean SVNClient::isAdminDirectory(const char *name)
{
    Pool requestPool;
    return svn_wc_is_adm_dir(name, requestPool.pool()) ? JNI_TRUE : JNI_FALSE;
}

const char *SVNClient::getLastPath()
{
    return m_lastPath.c_str();
}

/**
 * List directory entries of a URL
 */
jobjectArray SVNClient::list(const char *url, Revision &revision,
                             Revision &pegRevision, bool recurse)
{
    Pool requestPool;
    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return NULL;
    }

    SVN_JNI_NULL_PTR_EX(url, "path or url", NULL);

    Path urlPath(url);
    SVN_JNI_ERR(urlPath.error_occured(), NULL);

    apr_hash_t *dirents;
    SVN_JNI_ERR(svn_client_ls2(&dirents, urlPath.c_str(),
                               pegRevision.revision(),
                               revision.revision (),
                               recurse, ctx, requestPool.pool()),
                NULL);

    apr_array_header_t *array =
           svn_sort__hash(dirents, svn_sort_compare_items_as_paths,
                          requestPool.pool());

    // create the array of DirEntry
    JNIEnv *env = JNIUtil::getEnv();
    jclass clazz = env->FindClass(JAVA_PACKAGE"/DirEntry");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jobjectArray ret = env->NewObjectArray(array->nelts, clazz, NULL);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(clazz);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    for (int i = 0; i < array->nelts; i++)
    {
        const svn_sort__item_t *item;
        svn_dirent_t *dirent = NULL;

        item = &APR_ARRAY_IDX (array, i, const svn_sort__item_t);
        dirent = (svn_dirent_t *) item->value;

        jobject obj = createJavaDirEntry((const char *)item->key, dirent);
        env->SetObjectArrayElement(ret, i, obj);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        env->DeleteLocalRef(obj);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }
    return ret;
}


struct status_entry
{
    const char *path;
    svn_wc_status2_t *status;
};

struct status_baton
{
    std::vector<status_entry> statusVect;
    apr_pool_t *pool;
};


/**
 * callback for svn_client_status (used by status and singleStatus)
 */
void SVNClient::statusReceiver(void *baton, const char *path,
                               svn_wc_status2_t *status)
{
    if (JNIUtil::isJavaExceptionThrown())
        return;

    // Avoid creating Java Status objects here, as there could be
    // many, and we don't want too many local JNI references.
    status_baton *statusBaton = (status_baton*)baton;
    status_entry statusEntry;
    statusEntry.path = apr_pstrdup(statusBaton->pool, path);
    statusEntry.status = svn_wc_dup_status2(status, statusBaton->pool);
    statusBaton->statusVect.push_back(statusEntry);
}


jobjectArray SVNClient::status(const char *path, svn_depth_t depth,
                               bool onServer, bool getAll, bool noIgnore,
                               bool ignoreExternals)
{
    status_baton statusBaton;
    Pool requestPool;
    svn_revnum_t youngest = SVN_INVALID_REVNUM;
    svn_opt_revision_t rev;

    SVN_JNI_NULL_PTR_EX(path, "path", NULL);

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return NULL;
    }
    Path checkedPath(path);
    SVN_JNI_ERR(checkedPath.error_occured(), NULL);

    rev.kind = svn_opt_revision_unspecified;
    statusBaton.pool = requestPool.pool();

    SVN_JNI_ERR(svn_client_status3(&youngest, checkedPath.c_str(),
                                   &rev, statusReceiver,
                                   &statusBaton,
                                   depth,
                                   getAll, onServer, noIgnore,
                                   ignoreExternals,
                                   ctx, requestPool.pool()),
                NULL);

    JNIEnv *env = JNIUtil::getEnv();
    int size = statusBaton.statusVect.size();
    jclass clazz = env->FindClass(JAVA_PACKAGE"/Status");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jobjectArray ret = env->NewObjectArray(size, clazz, NULL);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(clazz);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    for(int i = 0; i < size; i++)
    {
        status_entry statusEntry = statusBaton.statusVect[i];

        jobject jStatus = createJavaStatus(statusEntry.path,
                                           statusEntry.status);
        env->SetObjectArrayElement(ret, i, jStatus);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        env->DeleteLocalRef(jStatus);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }
    return ret;
}

void SVNClient::username(const char *pi_username)
{
    m_userName = (pi_username == NULL ? "" : pi_username);
}

void SVNClient::password(const char *pi_password)
{
    m_passWord = (pi_password == NULL ? "" : pi_password);
}

void SVNClient::setPrompt(Prompter *prompter)
{
    delete m_prompter;
    m_prompter = prompter;
}

void SVNClient::logMessages(const char *path, Revision &pegRevision,
                            Revision &revisionStart,
                            Revision &revisionEnd, bool stopOnCopy,
                            bool discoverPaths, long limit,
                            LogMessageCallback *callback)
{
    Pool requestPool;

    SVN_JNI_NULL_PTR_EX(path, "path", );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return;
    }
    Targets target (path);
    const apr_array_header_t *targets = target.array(requestPool);
    SVN_JNI_ERR(target.error_occured(), );
    SVN_JNI_ERR(svn_client_log3(targets,
                                pegRevision.revision(),
                                revisionStart.revision (),
                                revisionEnd.revision (),
                                limit,
                                discoverPaths,
                                stopOnCopy,
                                LogMessageCallback::callback, callback, ctx,
                                requestPool.pool()), );
}

jlong SVNClient::checkout(const char *moduleName, const char *destPath,
                          Revision &revision, Revision &pegRevision,
                          svn_depth_t depth, bool ignoreExternals,
                          bool allowUnverObstructions)
{
    Pool requestPool;

    SVN_JNI_NULL_PTR_EX(moduleName, "moduleName", -1);
    SVN_JNI_NULL_PTR_EX(destPath, "destPath", -1);

    Path url(moduleName);
    Path path(destPath);
    SVN_JNI_ERR(url.error_occured(), -1);
    SVN_JNI_ERR(path.error_occured(), -1);
    svn_revnum_t retval;

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return -1;
    }

    SVN_JNI_ERR(svn_client_checkout3(&retval, url.c_str(),
                                     path.c_str (),
                                     pegRevision.revision (),
                                     revision.revision (),
                                     depth,
                                     ignoreExternals,
                                     allowUnverObstructions,
                                     ctx,
                                     requestPool.pool()),
                -1);

    return retval;

}

void SVNClient::notification(Notify *notify)
{
    delete m_notify;
    m_notify = notify;
}

void SVNClient::notification2(Notify2 *notify2)
{
    delete m_notify2;
    m_notify2 = notify2;
}

void SVNClient::setProgressListener(ProgressListener *listener)
{
    delete m_progressListener;
    m_progressListener = listener;
}

void SVNClient::remove(Targets &targets, const char *message, bool force,
                       bool keep_local)
{
    svn_commit_info_t *commit_info = NULL;
    Pool requestPool;
    svn_client_ctx_t *ctx = getContext(message);
    if (ctx == NULL)
    {
        return;
    }
    const apr_array_header_t *targets2 = targets.array(requestPool);
    SVN_JNI_ERR(targets.error_occured(), );

    SVN_JNI_ERR(svn_client_delete3(&commit_info, targets2, force, keep_local,
                                   ctx, requestPool.pool()), );
}

void SVNClient::revert(const char *path, bool recurse)
{
    Pool requestPool;

    SVN_JNI_NULL_PTR_EX(path, "path", );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return;
    }
    Targets target (path);
    const apr_array_header_t *targets = target.array(requestPool);
    SVN_JNI_ERR(target.error_occured(), );
    SVN_JNI_ERR(svn_client_revert(targets, recurse, ctx, requestPool.pool()), );
}

void SVNClient::add(const char *path, bool recurse, bool force)
{
    Pool requestPool;

    SVN_JNI_NULL_PTR_EX(path, "path", );

    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), );
    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return;
    }
    SVN_JNI_ERR(svn_client_add3(intPath.c_str (), recurse, force, FALSE,
                                ctx, requestPool.pool()), );
}

jlongArray SVNClient::update(Targets &targets, Revision &revision,
                             svn_depth_t depth, bool ignoreExternals,
                             bool allowUnverObstructions)
{
    Pool requestPool;

    svn_client_ctx_t *ctx = getContext(NULL);
    apr_array_header_t *retval;
    if (ctx == NULL)
    {
        return NULL;
    }
    const apr_array_header_t *array = targets.array(requestPool);
    SVN_JNI_ERR(targets.error_occured(), NULL);
    SVN_JNI_ERR(svn_client_update3(&retval, array,
                                   revision.revision(),
                                   depth,
                                   ignoreExternals,
                                   allowUnverObstructions,
                                   ctx, requestPool.pool()),
                NULL);

    JNIEnv *env = JNIUtil::getEnv();
    jlongArray ret = env->NewLongArray(retval->nelts);
    if (JNIUtil::isJavaExceptionThrown())
        return NULL;
    jlong *retArray = env->GetLongArrayElements(ret, NULL);
    if (JNIUtil::isJavaExceptionThrown())
        return NULL;
    for(int i = 0; i < retval->nelts; i++)
    {
        jlong rev = APR_ARRAY_IDX (retval, i, svn_revnum_t);
        retArray[i] = rev;
    }
    env->ReleaseLongArrayElements(ret, retArray, 0);
    return ret;

}

jlong SVNClient::commit(Targets &targets, const char *message, bool recurse,
                        bool noUnlock, bool keepChangelist,
                        const char *changelistName)
{
    Pool requestPool;
    svn_commit_info_t *commit_info = NULL;
    const apr_array_header_t *targets2 = targets.array(requestPool);
    SVN_JNI_ERR(targets.error_occured(), -1);
    svn_client_ctx_t *ctx = getContext(message);
    if (ctx == NULL)
    {
        return -1;
    }
    SVN_JNI_ERR(svn_client_commit4(&commit_info, targets2, recurse,
                                   noUnlock, keepChangelist, changelistName,
                                   ctx, requestPool.pool()),
                -1);

    if (commit_info && SVN_IS_VALID_REVNUM (commit_info->revision))
      return commit_info->revision;

    return -1;
}

void SVNClient::copy(CopySources &copySources, const char *destPath,
                     const char *message, bool copyAsChild)
{
    Pool requestPool;

    apr_array_header_t *srcs = copySources.array(requestPool);
    if (srcs == NULL)
    {
        JNIUtil::throwNativeException(JAVA_PACKAGE "/ClientException",
                                      "Invalid copy sources");
        return;
    }
    SVN_JNI_NULL_PTR_EX(destPath, "destPath", );
    Path destinationPath(destPath);
    SVN_JNI_ERR(destinationPath.error_occured(), );

    svn_client_ctx_t *ctx = getContext(message);
    if (ctx == NULL)
        return;
    svn_commit_info_t *commit_info;
    SVN_JNI_ERR(svn_client_copy4(&commit_info, srcs, destinationPath.c_str(),
                                 copyAsChild, ctx, requestPool.pool()), );
}

void SVNClient::move(Targets &srcPaths, const char *destPath,
                     const char *message, bool force, bool moveAsChild)
{
    Pool requestPool;

    const apr_array_header_t *srcs = srcPaths.array(requestPool);
    SVN_JNI_ERR(srcPaths.error_occured(), );
    SVN_JNI_NULL_PTR_EX(destPath, "destPath", );
    Path destinationPath(destPath);
    SVN_JNI_ERR(destinationPath.error_occured(), );

    svn_client_ctx_t *ctx = getContext(message);
    if (ctx == NULL)
        return;
    svn_commit_info_t *commit_info;
    SVN_JNI_ERR(svn_client_move5(&commit_info, (apr_array_header_t *) srcs,
                                 destinationPath.c_str(), force, moveAsChild,
                                 ctx, requestPool.pool()), );
}

void SVNClient::mkdir(Targets &targets, const char *message)
{
    Pool requestPool;
    svn_client_commit_info_t *commit_info = NULL;
    svn_client_ctx_t *ctx = getContext(message);
    if (ctx == NULL)
    {
        return;
    }
    const apr_array_header_t *targets2 = targets.array(requestPool);
    SVN_JNI_ERR(targets.error_occured(), );

    SVN_JNI_ERR(svn_client_mkdir(&commit_info, targets2, ctx,
                                 requestPool.pool()), );
}

void SVNClient::cleanup(const char *path)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return;
    }
    SVN_JNI_ERR(svn_client_cleanup (intPath.c_str (), ctx, requestPool.pool()),
                );
}

void SVNClient::resolved(const char *path, bool recurse)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), );
    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return;
    }
    SVN_JNI_ERR(svn_client_resolved(intPath.c_str(), recurse,
                                    ctx, requestPool.pool()), );
}

jlong SVNClient::doExport(const char *srcPath, const char *destPath,
                          Revision &revision, Revision &pegRevision, bool force,
                          bool ignoreExternals, svn_depth_t depth,
                          const char *nativeEOL)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(srcPath, "srcPath", -1);
    SVN_JNI_NULL_PTR_EX(destPath, "destPath", -1);
    Path sourcePath(srcPath);
    SVN_JNI_ERR(sourcePath.error_occured(), -1);
    Path destinationPath(destPath);
    SVN_JNI_ERR(destinationPath.error_occured(), -1);
    svn_revnum_t retval;
    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return -1;
    }
    SVN_JNI_ERR(svn_client_export4(&retval, sourcePath.c_str(),
                                   destinationPath.c_str(),
                                   pegRevision.revision(),
                                   revision.revision(), force,
                                   ignoreExternals,
                                   depth,
                                   nativeEOL, ctx,
                                   requestPool.pool()),
                -1);

    return retval;

}

jlong SVNClient::doSwitch(const char *path, const char *url,
                          Revision &revision, svn_depth_t depth,
                          bool allowUnverObstructions)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", -1);
    SVN_JNI_NULL_PTR_EX(url, "url", -1);
    Path intUrl(url);
    SVN_JNI_ERR(intUrl.error_occured(), -1);
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), -1);

    svn_revnum_t retval;
    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return -1;
    }
    SVN_JNI_ERR(svn_client_switch2(&retval, intPath.c_str (),
                                   intUrl.c_str(),
                                   revision.revision (),
                                   depth,
                                   allowUnverObstructions,
                                   ctx,
                                   requestPool.pool()),
                -1);

    return retval;
}

void SVNClient::doImport(const char *path, const char *url,
                         const char *message, bool recurse)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    SVN_JNI_NULL_PTR_EX(url, "url", );
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), );
    Path intUrl(url);
    SVN_JNI_ERR(intUrl.error_occured(), );

    svn_client_commit_info_t *commit_info = NULL;
    svn_client_ctx_t *ctx = getContext(message);
    if (ctx == NULL)
    {
        return;
    }

    SVN_JNI_ERR(svn_client_import(&commit_info, intPath.c_str(), intUrl.c_str(),
                                  !recurse, ctx, requestPool.pool()), );
}

void SVNClient::merge(const char *path1, Revision &revision1,
                      const char *path2, Revision &revision2,
                      const char *localPath, bool force, svn_depth_t depth,
                      bool ignoreAncestry, bool dryRun)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path1, "path1", );
    SVN_JNI_NULL_PTR_EX(path2, "path2", );
    SVN_JNI_NULL_PTR_EX(localPath, "localPath", );
    Path intLocalPath(localPath);
    SVN_JNI_ERR(intLocalPath.error_occured(), );

    Path srcPath1(path1);
    SVN_JNI_ERR(srcPath1.error_occured(), );

    Path srcPath2 = path2;
    SVN_JNI_ERR(srcPath2.error_occured(), );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return;
    }

    SVN_JNI_ERR(svn_client_merge3(srcPath1.c_str(), revision1.revision(),
                                  srcPath2.c_str(), revision2.revision(),
                                  intLocalPath.c_str(),
                                  depth,
                                  ignoreAncestry, force, FALSE, dryRun, NULL,
                                  ctx, requestPool.pool()), );
}

void SVNClient::merge(const char *path, Revision &pegRevision,
                      Revision &revision1, Revision &revision2,
                      const char *localPath, bool force, svn_depth_t depth,
                      bool ignoreAncestry, bool dryRun)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    SVN_JNI_NULL_PTR_EX(localPath, "localPath", );
    Path intLocalPath(localPath);
    SVN_JNI_ERR(intLocalPath.error_occured(), );

    Path srcPath(path);
    SVN_JNI_ERR(srcPath.error_occured(), );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return;
    }

    SVN_JNI_ERR(svn_client_merge_peg3(srcPath.c_str(), revision1.revision(),
                                      revision2.revision(),
                                      pegRevision.revision(),
                                      intLocalPath.c_str(),
                                      depth,
                                      ignoreAncestry, force, FALSE, dryRun,
                                      NULL, ctx, requestPool.pool()), );
}


/**
 * Get a property
 */
jobject SVNClient::propertyGet(jobject jthis, const char *path,
                               const char *name, Revision &revision,
                               Revision &pegRevision)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", NULL);
    SVN_JNI_NULL_PTR_EX(name, "name", NULL);
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), NULL);

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return NULL;
    }

    apr_hash_t *props;
    SVN_JNI_ERR(svn_client_propget2(&props, name,
                                    intPath.c_str(), pegRevision.revision(),
                                    revision.revision(), FALSE,
                                    ctx, requestPool.pool()),
                NULL);

    apr_hash_index_t *hi;
    // only one element since we disabled recurse
    hi = apr_hash_first (requestPool.pool(), props);
    if (hi == NULL)
        return NULL; // no property with this name

    svn_string_t *propval;
    apr_hash_this(hi, NULL, NULL, (void**)&propval);

    if (propval == NULL)
        return NULL;

    return createJavaProperty(jthis, path, name, propval);
}

void SVNClient::properties(const char *path, Revision & revision,
                           Revision &pegRevision, bool recurse,
                           ProplistCallback *callback)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return;
    }

    SVN_JNI_ERR(svn_client_proplist3(intPath.c_str(), pegRevision.revision(),
                                     revision.revision(), recurse,
                                     ProplistCallback::callback, callback,
                                     ctx, requestPool.pool()), );

    return;
}

void SVNClient::propertySet(const char *path, const char *name,
                            const char *value, bool recurse, bool force)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    SVN_JNI_NULL_PTR_EX(name, "name", );
    SVN_JNI_NULL_PTR_EX(value, "value", );
    svn_string_t *val = svn_string_create(value, requestPool.pool());
    propertySet(path, name, val, recurse, force, SVN_INVALID_REVNUM);
}

void SVNClient::propertyRemove(const char *path, const char *name,
                               bool recurse)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    SVN_JNI_NULL_PTR_EX(name, "name", );
    propertySet(path, name, (svn_string_t*)NULL, recurse, false,
                SVN_INVALID_REVNUM);
}

void SVNClient::propertyCreate(const char *path, const char *name,
                               const char *value, bool recurse, bool force)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    SVN_JNI_NULL_PTR_EX(name, "name", );
    SVN_JNI_NULL_PTR_EX(value, "value", );
    svn_string_t *val = svn_string_create(value, requestPool.pool());
    propertySet(path, name, val, recurse, force, SVN_INVALID_REVNUM);
}

void SVNClient::diff(const char *target1, Revision &revision1,
                     const char *target2, Revision &revision2,
                     Revision *pegRevision, const char *outfileName,
                     svn_depth_t depth, bool ignoreAncestry,
                     bool noDiffDelete, bool force)
{
    svn_error_t *err;
    Pool requestPool;

    SVN_JNI_NULL_PTR_EX(target1, "target", );
    // target2 is ignored when pegRevision is provided.
    if (pegRevision == NULL)
        SVN_JNI_NULL_PTR_EX(target2, "target2", );
    SVN_JNI_NULL_PTR_EX(outfileName, "outfileName", );
    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
        return;

    Path path1(target1);
    SVN_JNI_ERR(path1.error_occured(), );

    apr_file_t *outfile = NULL;
    apr_status_t rv =
        apr_file_open(&outfile,
                      svn_path_internal_style(outfileName, requestPool.pool()),
                      APR_CREATE|APR_WRITE|APR_TRUNCATE , APR_OS_DEFAULT,
                      requestPool.pool());
    if (rv != APR_SUCCESS)
    {
        SVN_JNI_ERR(svn_error_createf(rv, NULL, _("Cannot open file '%s'"),
                                      outfileName), );
    }

    // We don't use any options to diff.
    apr_array_header_t *diffOptions = apr_array_make(requestPool.pool(),
                                                     0, sizeof(char *));

    if (pegRevision)
    {
        err = svn_client_diff_peg4(diffOptions,
                                   path1.c_str(),
                                   pegRevision->revision(),
                                   revision1.revision(),
                                   revision2.revision(),
                                   depth,
                                   ignoreAncestry,
                                   noDiffDelete,
                                   force,
                                   SVN_APR_LOCALE_CHARSET,
                                   outfile,
                                   NULL /* error file */,
                                   ctx,
                                   requestPool.pool());
    }
    else
    {
        // "Regular" diff (without a peg revision).
        Path path2(target2);
        err = path2.error_occured();
        if (err)
        {
            if (outfile)
                goto cleanup;

            SVN_JNI_ERR(err, );
        }

        err = svn_client_diff4(diffOptions,
                               path1.c_str(),
                               revision1.revision(),
                               path2.c_str(),
                               revision2.revision(),
                               depth,
                               ignoreAncestry,
                               noDiffDelete,
                               force,
                               SVN_APR_LOCALE_CHARSET,
                               outfile,
                               NULL /* error file */,
                               ctx,
                               requestPool.pool());
    }

 cleanup:
    rv = apr_file_close(outfile);
    if (rv != APR_SUCCESS)
    {
        svn_error_clear(err);

        SVN_JNI_ERR(svn_error_createf(rv, NULL, _("Cannot close file '%s'"),
                                      outfileName), );
    }

    SVN_JNI_ERR(err, );
}

void SVNClient::diff(const char *target1, Revision &revision1,
                     const char *target2, Revision &revision2,
                     const char *outfileName, svn_depth_t depth,
                     bool ignoreAncestry, bool noDiffDelete, bool force)
{
    diff(target1, revision1, target2, revision2, NULL, outfileName, depth,
         ignoreAncestry, noDiffDelete, force);
}

void SVNClient::diff(const char *target, Revision &pegRevision,
                     Revision &startRevision, Revision &endRevision,
                     const char *outfileName, svn_depth_t depth,
                     bool ignoreAncestry, bool noDiffDelete, bool force)
{
    diff(target, startRevision, NULL, endRevision, &pegRevision, outfileName,
         depth, ignoreAncestry, noDiffDelete, force);
}

void
SVNClient::diffSummarize(const char *target1, Revision &revision1,
                         const char *target2, Revision &revision2,
                         svn_depth_t depth, bool ignoreAncestry,
                         DiffSummaryReceiver &receiver)
{
    Pool requestPool;

    SVN_JNI_NULL_PTR_EX(target1, "target1", );
    SVN_JNI_NULL_PTR_EX(target2, "target2", );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
        return;

    Path path1(target1);
    SVN_JNI_ERR(path1.error_occured(), );
    Path path2(target2);
    SVN_JNI_ERR(path2.error_occured(), );

    SVN_JNI_ERR(svn_client_diff_summarize2(path1.c_str(), revision1.revision(),
                                           path2.c_str(), revision2.revision(),
                                           depth,
                                           ignoreAncestry,
                                           DiffSummaryReceiver::summarize,
                                           &receiver,
                                           ctx, requestPool.pool()), );
}

void
SVNClient::diffSummarize(const char *target, Revision &pegRevision,
                         Revision &startRevision, Revision &endRevision,
                         svn_depth_t depth, bool ignoreAncestry,
                         DiffSummaryReceiver &receiver)
{
    Pool requestPool;

    SVN_JNI_NULL_PTR_EX(target, "target", );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
        return;

    Path path(target);
    SVN_JNI_ERR(path.error_occured(), );

    SVN_JNI_ERR(svn_client_diff_summarize_peg2(path.c_str(),
                                               pegRevision.revision(),
                                               startRevision.revision(),
                                               endRevision.revision(),
                                               depth,
                                               ignoreAncestry,
                                               DiffSummaryReceiver::summarize,
                                               &receiver, ctx,
                                               requestPool.pool()), );
}

svn_client_ctx_t *SVNClient::getContext(const char *message)
{
    apr_pool_t *pool = JNIUtil::getRequestPool()->pool();
    svn_auth_baton_t *ab;
    svn_client_ctx_t *ctx;
    SVN_JNI_ERR(svn_client_create_context(&ctx, pool), NULL);

    apr_array_header_t *providers
      = apr_array_make (pool, 10, sizeof (svn_auth_provider_object_t *));

    /* The main disk-caching auth providers, for both
       'username/password' creds and 'username' creds.  */
    svn_auth_provider_object_t *provider;
#ifdef WIN32
    svn_client_get_windows_simple_provider (&provider, pool);
    APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
#endif
    svn_client_get_simple_provider (&provider, pool);
    APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
    svn_client_get_username_provider (&provider, pool);
    APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

    /* The server-cert, client-cert, and client-cert-password providers. */
    svn_client_get_ssl_server_trust_file_provider (&provider, pool);
    APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
    svn_client_get_ssl_client_cert_file_provider (&provider, pool);
    APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
    svn_client_get_ssl_client_cert_pw_file_provider (&provider, pool);
    APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

    if (m_prompter != NULL)
    {
         /* Two basic prompt providers: username/password, and just username.*/
        provider = m_prompter->getProviderSimple();

        APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

        provider = m_prompter->getProviderUsername();
        APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

        /* Three ssl prompt providers, for server-certs, client-certs,
           and client-cert-passphrases.  */
        provider = m_prompter->getProviderServerSSLTrust();
        APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

        provider = m_prompter->getProviderClientSSL();
        APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

        provider = m_prompter->getProviderClientSSLPassword();
        APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
      }



    /* Build an authentication baton to give to libsvn_client. */
    svn_auth_open(&ab, providers, pool);

    /* Place any default --username or --password credentials into the
       auth_baton's run-time parameter hash.  ### Same with --no-auth-cache? */
    if (!m_userName.empty())
      svn_auth_set_parameter(ab, SVN_AUTH_PARAM_DEFAULT_USERNAME,
                             m_userName.c_str());
    if (!m_passWord.empty())
      svn_auth_set_parameter(ab, SVN_AUTH_PARAM_DEFAULT_PASSWORD,
                             m_passWord.c_str());


    ctx->auth_baton = ab;
    ctx->notify_func = Notify::notify;
    ctx->notify_baton = m_notify;
    ctx->log_msg_func3 = getCommitMessage;
    ctx->log_msg_baton3 = getCommitMessageBaton(message);
    ctx->cancel_func = checkCancel;
    m_cancelOperation = false;
    ctx->cancel_baton = this;
    const char *configDir = m_configDir.c_str();
    if (m_configDir.length() == 0)
        configDir = NULL;
    SVN_JNI_ERR(svn_config_get_config (&(ctx->config), configDir, pool), NULL);
    ctx->notify_func2= Notify2::notify;
    ctx->notify_baton2 = m_notify2;

    ctx->progress_func = ProgressListener::progress;
    ctx->progress_baton = m_progressListener;

    return ctx;
}

svn_error_t *
SVNClient::getCommitMessage(const char **log_msg,
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
void *SVNClient::getCommitMessageBaton(const char *message)
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

jobject SVNClient::createJavaStatus(const char *path, svn_wc_status2_t *status)
{
    JNIEnv *env = JNIUtil::getEnv();
    jclass clazz = env->FindClass(JAVA_PACKAGE"/Status");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    static jmethodID mid = 0;
    if (mid == 0)
    {
        mid = env->GetMethodID(clazz, "<init>",
            "(Ljava/lang/String;Ljava/lang/String;IJJJLjava/lang/String;IIIIZZ"
             "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
             "Ljava/lang/String;JZLjava/lang/String;Ljava/lang/String;"
             "Ljava/lang/String;JLorg/tigris/subversion/javahl/Lock;"
             "JJILjava/lang/String;)V");
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }
    jstring jPath = JNIUtil::makeJString(path);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    jstring jUrl = NULL;
    jint jNodeKind = org_tigris_subversion_javahl_NodeKind_unknown;
    jlong jRevision = org_tigris_subversion_javahl_Revision_SVN_INVALID_REVNUM;
    jlong jLastChangedRevision =
                    org_tigris_subversion_javahl_Revision_SVN_INVALID_REVNUM;
    jlong jLastChangedDate = 0;
    jstring jLastCommitAuthor = NULL;
    jint jTextType = org_tigris_subversion_javahl_StatusKind_none;
    jint jPropType = org_tigris_subversion_javahl_StatusKind_none;
    jint jRepositoryTextType = org_tigris_subversion_javahl_StatusKind_none;
    jint jRepositoryPropType = org_tigris_subversion_javahl_StatusKind_none;
    jboolean jIsLocked = JNI_FALSE;
    jboolean jIsCopied = JNI_FALSE;
    jboolean jIsSwitched = JNI_FALSE;
    jstring jConflictOld = NULL;
    jstring jConflictNew = NULL;
    jstring jConflictWorking = NULL;
    jstring jURLCopiedFrom = NULL;
    jlong jRevisionCopiedFrom =
                    org_tigris_subversion_javahl_Revision_SVN_INVALID_REVNUM;
    jstring jLockToken = NULL;
    jstring jLockComment = NULL;
    jstring jLockOwner = NULL;
    jlong jLockCreationDate = 0;
    jobject jLock = NULL;
    jlong jOODLastCmtRevision =
                    org_tigris_subversion_javahl_Revision_SVN_INVALID_REVNUM;
    jlong jOODLastCmtDate = 0;
    jint jOODKind = org_tigris_subversion_javahl_NodeKind_none;
    jstring jOODLastCmtAuthor = NULL;
    if (status != NULL)
    {
        jTextType = EnumMapper::mapStatusKind(status->text_status);
        jPropType = EnumMapper::mapStatusKind(status->prop_status);
        jRepositoryTextType = EnumMapper::mapStatusKind(status->repos_text_status);
        jRepositoryPropType = EnumMapper::mapStatusKind(status->repos_prop_status);
        jIsCopied = (status->copied == 1) ? JNI_TRUE: JNI_FALSE;
        jIsLocked = (status->locked == 1) ? JNI_TRUE: JNI_FALSE;
        jIsSwitched = (status->switched == 1) ? JNI_TRUE: JNI_FALSE;
        jLock = createJavaLock(status->repos_lock);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        jUrl = JNIUtil::makeJString(status->url);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        jOODLastCmtRevision = status->ood_last_cmt_rev;
        jOODLastCmtDate = status->ood_last_cmt_date;
        jOODKind = EnumMapper::mapNodeKind(status->ood_kind);
        jOODLastCmtAuthor = JNIUtil::makeJString(status->ood_last_cmt_author);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }

        svn_wc_entry_t *entry = status->entry;
        if (entry != NULL)
        {
            jNodeKind = EnumMapper::mapNodeKind(entry->kind);
            jRevision = entry->revision;
            jLastChangedRevision = entry->cmt_rev;
            jLastChangedDate = entry->cmt_date;
            jLastCommitAuthor = JNIUtil::makeJString(entry->cmt_author);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }

            jConflictNew = JNIUtil::makeJString(entry->conflict_new);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
            jConflictOld = JNIUtil::makeJString(entry->conflict_old);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
            jConflictWorking= JNIUtil::makeJString(entry->conflict_wrk);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
            jURLCopiedFrom = JNIUtil::makeJString(entry->copyfrom_url);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
            jRevisionCopiedFrom = entry->copyfrom_rev;
            jLockToken = JNIUtil::makeJString(entry->lock_token);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
            jLockComment = JNIUtil::makeJString(entry->lock_comment);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
            jLockOwner = JNIUtil::makeJString(entry->lock_owner);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
            jLockCreationDate = entry->lock_creation_date;
        }
    }

    jobject ret = env->NewObject(clazz, mid, jPath, jUrl, jNodeKind, jRevision,
        jLastChangedRevision, jLastChangedDate, jLastCommitAuthor,
        jTextType, jPropType, jRepositoryTextType, jRepositoryPropType,
        jIsLocked, jIsCopied, jConflictOld, jConflictNew, jConflictWorking,
        jURLCopiedFrom, jRevisionCopiedFrom, jIsSwitched, jLockToken,
        jLockOwner, jLockComment, jLockCreationDate, jLock,
        jOODLastCmtRevision, jOODLastCmtDate, jOODKind, jOODLastCmtAuthor);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(clazz);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jPath);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jUrl);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jLastCommitAuthor);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jConflictNew);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jConflictOld);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jConflictWorking);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jURLCopiedFrom);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jLockComment);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jLockOwner);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jLockToken);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jLock);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jOODLastCmtAuthor);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    return ret;
}

jobject SVNClient::createJavaProperty(jobject jthis, const char *path,
                                      const char *name, svn_string_t *value)
{
    JNIEnv *env = JNIUtil::getEnv();
    jclass clazz = env->FindClass(JAVA_PACKAGE"/PropertyData");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    static jmethodID mid = 0;
    if (mid == 0)
    {
        mid = env->GetMethodID(clazz, "<init>",
                  "(L"JAVA_PACKAGE"/SVNClient;Ljava/lang/String;"
                   "Ljava/lang/String;Ljava/lang/String;[B)V");
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }
    jstring jPath = JNIUtil::makeJString(path);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jName = JNIUtil::makeJString(name);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jValue = JNIUtil::makeJString(value->data);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jbyteArray jData = JNIUtil::makeJByteArray((const signed char *)value->data,
                                               value->len);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jobject ret = env->NewObject(clazz, mid, jthis, jPath, jName, jValue,
                                 jData);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(clazz);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jPath);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jName);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jValue);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jData);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    return ret;
}

void SVNClient::propertySet(const char *path, const char *name,
                            svn_string_t *value, bool recurse, bool force,
                            svn_revnum_t baseRevisionForURL)
{
    svn_commit_info_t *commit_info = NULL;
    Pool requestPool;
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
        return;
    SVN_JNI_ERR(svn_client_propset3(&commit_info, name, value, intPath.c_str(),
                                    recurse, force, baseRevisionForURL,
                                    ctx, requestPool.pool()), );
}

jbyteArray SVNClient::fileContent(const char *path, Revision &revision,
                                  Revision &pegRevision)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", NULL);
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), NULL);

    size_t size = 0;
    svn_stream_t *read_stream = createReadStream(requestPool.pool(),
                                                 intPath.c_str(), revision,
                                                 pegRevision, size);
    if (read_stream == NULL)
    {
        return NULL;
    }

    JNIEnv *env = JNIUtil::getEnv();
    // size will be set to the number of bytes available.
    jbyteArray ret = env->NewByteArray(size);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jbyte *retdata = env->GetByteArrayElements(ret, NULL);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    svn_error_t *err = svn_stream_read(read_stream, (char *)retdata, &size);
    env->ReleaseByteArrayElements(ret, retdata, 0);
    SVN_JNI_ERR(err, NULL);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    return ret;
}

void SVNClient::streamFileContent(const char *path, Revision &revision,
                                  Revision &pegRevision, jobject outputStream,
                                  size_t bufSize)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), );

    JNIEnv *env = JNIUtil::getEnv();
    jclass outputStreamClass = env->FindClass("java/io/OutputStream");
    if (outputStreamClass == NULL)
    {
        return;
    }
    jmethodID writeMethod = env->GetMethodID(outputStreamClass, "write",
                                             "([BII)V");
    if (writeMethod == NULL)
    {
        return;
    }

    // Create the buffer.
    jbyteArray buffer = env->NewByteArray(bufSize);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return;
    }
    jbyte *bufData = env->GetByteArrayElements(buffer, NULL);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return;
    }

    size_t contentSize = 0;
    svn_stream_t* read_stream = createReadStream(requestPool.pool(), path,
                                                 revision, pegRevision,
                                                 contentSize);
    if (read_stream == NULL)
    {
        return;
    }

    while (contentSize > 0)
    {
        size_t readSize = bufSize > contentSize ? contentSize : bufSize;
        svn_error_t *err;

        err = svn_stream_read(read_stream, (char *)bufData, &readSize);
        if (err != NULL)
        {
            env->ReleaseByteArrayElements(buffer, bufData, 0);
            svn_stream_close(read_stream);
            SVN_JNI_ERR(err, );
        }

        env->ReleaseByteArrayElements(buffer, bufData, JNI_COMMIT);
        env->CallVoidMethod(outputStream, writeMethod, buffer, 0, readSize);
        if (JNIUtil::isJavaExceptionThrown())
        {
            env->ReleaseByteArrayElements(buffer, bufData, 0);
            svn_stream_close(read_stream);
            return;
        }
        contentSize -= readSize;
    }

    env->ReleaseByteArrayElements(buffer, bufData, 0);
    return;
}

svn_stream_t* SVNClient::createReadStream(apr_pool_t* pool, const char *path,
                                          Revision& revision,
                                          Revision &pegRevision, size_t& size)
{
    svn_stream_t *read_stream = NULL;

    if (revision.revision()->kind == svn_opt_revision_working)
    {
        // We want the working copy. Going back to the server returns
        // base instead (which is not what we want).
        apr_file_t *file = NULL;
        apr_finfo_t finfo;
        apr_status_t apr_err = apr_stat(&finfo, path, APR_FINFO_MIN, pool);
        if (apr_err)
        {
            JNIUtil::handleAPRError(apr_err, _("open file"));
            return NULL;
        }
        apr_err = apr_file_open(&file, path, APR_READ, 0, pool);
        if (apr_err)
        {
            JNIUtil::handleAPRError(apr_err, _("open file"));
            return NULL;
        }
        read_stream = svn_stream_from_aprfile(file, pool);
        size = finfo.size;
    }
    else
    {
        svn_client_ctx_t *ctx = getContext(NULL);
        if (ctx == NULL)
        {
            return NULL;
        }
        svn_stringbuf_t *buf = svn_stringbuf_create("", pool);
        read_stream = svn_stream_from_stringbuf(buf, pool);
        SVN_JNI_ERR(svn_client_cat2(read_stream, path, pegRevision.revision(),
                                    revision.revision(), ctx, pool),
                    NULL);
        size = buf->len;
    }
    return read_stream;
}


/**
 * create a DirEntry java object from svn_dirent_t structure
 */
jobject SVNClient::createJavaDirEntry(const char *path, svn_dirent_t *dirent)
{
    JNIEnv *env = JNIUtil::getEnv();
    jclass clazz = env->FindClass(JAVA_PACKAGE"/DirEntry");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    static jmethodID mid = 0;
    if (mid == 0)
    {
        mid = env->GetMethodID(clazz, "<init>",
                               "(Ljava/lang/String;IJZJJLjava/lang/String;)V");
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }
    jstring jPath = JNIUtil::makeJString(path);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jint jNodeKind = EnumMapper::mapNodeKind(dirent->kind);
    jlong jSize = dirent->size;
    jboolean jHasProps = (dirent->has_props? JNI_TRUE : JNI_FALSE);
    jlong jLastChangedRevision = dirent->created_rev;
    jlong jLastChanged = dirent->time;
    jstring jLastAuthor = JNIUtil::makeJString(dirent->last_author);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jobject ret = env->NewObject(clazz, mid, jPath, jNodeKind, jSize,
                                 jHasProps, jLastChangedRevision,
                                 jLastChanged, jLastAuthor);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(clazz);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jPath);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    if (jLastAuthor != NULL)
    {
        env->DeleteLocalRef(jLastAuthor);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }
    return ret;
}

jobject SVNClient::revProperty(jobject jthis, const char *path,
                               const char *name, Revision &rev)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", NULL);
    SVN_JNI_NULL_PTR_EX(name, "name", NULL);
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), NULL);

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return NULL;
    }
    const char *URL;
    svn_string_t *propval;
    svn_revnum_t set_rev;
    SVN_JNI_ERR(svn_client_url_from_path(&URL, intPath.c_str(),
                                         requestPool.pool()),
                NULL);

    if (URL == NULL)
    {
        SVN_JNI_ERR(svn_error_create(SVN_ERR_UNVERSIONED_RESOURCE, NULL,
                            _("Either a URL or versioned item is required.")),
                    NULL);
    }

    SVN_JNI_ERR(svn_client_revprop_get(name, &propval, URL,
                                       rev.revision(), &set_rev, ctx,
                                       requestPool.pool()),
                NULL);
    if (propval == NULL)
        return NULL;

    return createJavaProperty(jthis, path, name, propval);
}
void SVNClient::relocate(const char *from, const char *to, const char *path,
                         bool recurse)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    SVN_JNI_NULL_PTR_EX(from, "from", );
    SVN_JNI_NULL_PTR_EX(to, "to", );
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), );

    Path intFrom(from);
    SVN_JNI_ERR(intFrom.error_occured(), );

    Path intTo(to);
    SVN_JNI_ERR(intTo.error_occured(), );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return;
    }

    SVN_JNI_ERR(svn_client_relocate(intPath.c_str(), intFrom.c_str(),
                                    intTo.c_str(), recurse, ctx,
                                    requestPool.pool()), );
}

void SVNClient::blame(const char *path, Revision &pegRevision,
                      Revision &revisionStart, Revision &revisionEnd,
                      bool ignoreMimeType, BlameCallback *callback)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    apr_pool_t *pool = requestPool.pool ();
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return;
    }
    SVN_JNI_ERR(svn_client_blame3(intPath.c_str(), pegRevision.revision(),
                                  revisionStart.revision(),
                                  revisionEnd.revision(),
                                  svn_diff_file_options_create(pool), false,
                                  BlameCallback::callback, callback, ctx, pool), );
}

void SVNClient::setConfigDirectory(const char *configDir)
{
    // A change to the config directory may necessitate creation of
    // the config templates.
    Pool requestPool;
    SVN_JNI_ERR(svn_config_ensure(configDir, requestPool.pool()), );

    m_configDir = (configDir == NULL ? "" : configDir);
}

const char *SVNClient::getConfigDirectory()
{
    return m_configDir.c_str();
}

void SVNClient::commitMessageHandler(CommitMessage *commitMessage)
{
    delete m_commitMessage;
    m_commitMessage = commitMessage;
}

void SVNClient::cancelOperation()
{
    m_cancelOperation = true;
}

svn_error_t *SVNClient::checkCancel(void *cancelBaton)
{
    SVNClient *that = (SVNClient*)cancelBaton;
    if (that->m_cancelOperation)
        return svn_error_create (SVN_ERR_CANCELLED, NULL,
            _("Operation canceled"));
    else
        return SVN_NO_ERROR;
}

void SVNClient::addToChangelist(Targets &srcPaths, const char *changelist)
{
    Pool requestPool;
    svn_client_ctx_t *ctx = getContext(NULL);

    const apr_array_header_t *srcs = srcPaths.array(requestPool);
    SVN_JNI_ERR(srcPaths.error_occured(), );

    SVN_JNI_ERR(svn_client_add_to_changelist(srcs, changelist, ctx,
                                             requestPool.pool()), );
}

void SVNClient::removeFromChangelist(Targets &srcPaths, const char *changelist)
{
    Pool requestPool;
    svn_client_ctx_t *ctx = getContext(NULL);

    const apr_array_header_t *srcs = srcPaths.array(requestPool);
    SVN_JNI_ERR(srcPaths.error_occured(), );

    SVN_JNI_ERR(svn_client_remove_from_changelist(srcs, changelist, ctx,
                                                  requestPool.pool()), );
}

jobjectArray SVNClient::getChangelist(const char *changelist,
                                      const char *rootPath)
{
    Pool requestPool;
    svn_client_ctx_t *ctx = getContext(NULL);
    apr_array_header_t *paths;

    SVN_JNI_ERR(svn_client_get_changelist(&paths, changelist, rootPath,
                                          ctx, requestPool.pool()),
                NULL);

    JNIEnv *env = JNIUtil::getEnv();
    jclass clazz = env->FindClass("java/lang/String");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    jobjectArray ret = env->NewObjectArray(paths->nelts, clazz, NULL);

    for (int i = 0; i < paths->nelts; i++)
    {
        const char *path = APR_ARRAY_IDX(paths, i, const char *);
        jstring jpath = JNIUtil::makeJString(path);

        env->SetObjectArrayElement(ret, i, jpath);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }

    return ret;
}

jobject SVNClient::createJavaLock(const svn_lock_t *lock)
{
    if (lock == NULL)
        return NULL;
    JNIEnv *env = JNIUtil::getEnv();

    jclass clazz = env->FindClass(JAVA_PACKAGE"/Lock");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    static jmethodID mid = 0;
    if (mid == 0)
    {
        mid = env->GetMethodID(clazz, "<init>",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
            "Ljava/lang/String;JJ)V");
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }

    jstring jOwner = JNIUtil::makeJString(lock->owner);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jPath = JNIUtil::makeJString(lock->path);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jToken = JNIUtil::makeJString(lock->token);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jComment = JNIUtil::makeJString(lock->comment);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jlong jCreationDate = lock->creation_date;
    jlong jExpirationDate = lock->expiration_date;
    jobject ret = env->NewObject(clazz, mid, jOwner, jPath, jToken, jComment,
        jCreationDate, jExpirationDate);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    env->DeleteLocalRef(clazz);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    env->DeleteLocalRef(jOwner);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    env->DeleteLocalRef(jPath);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    env->DeleteLocalRef(jToken);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    env->DeleteLocalRef(jComment);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    return ret;
}

void SVNClient::lock(Targets &targets, const char *comment, bool force)
{
    Pool requestPool;
    const apr_array_header_t *targetsApr = targets.array(requestPool);
    SVN_JNI_ERR(targets.error_occured(), );
    svn_client_ctx_t *ctx = getContext(NULL);

    SVN_JNI_ERR(svn_client_lock(targetsApr, comment, force, ctx,
                                requestPool.pool()), );
}

void SVNClient::unlock(Targets &targets, bool force)
{
    Pool requestPool;

    const apr_array_header_t *targetsApr = targets.array(requestPool);
    SVN_JNI_ERR(targets.error_occured(), );
    svn_client_ctx_t *ctx = getContext(NULL);
    SVN_JNI_ERR(svn_client_unlock((apr_array_header_t*)targetsApr, force,
                                  ctx, requestPool.pool()), );
}
void SVNClient::setRevProperty(jobject jthis, const char *path,
                               const char *name, Revision &rev,
                               const char *value, bool force)
{
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", );
    SVN_JNI_NULL_PTR_EX(name, "name", );
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), );

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return ;
    }
    const char *URL;
    SVN_JNI_ERR(svn_client_url_from_path(&URL, intPath.c_str(),
                                         requestPool.pool()), );

    if (URL == NULL)
    {
        SVN_JNI_ERR(svn_error_create(SVN_ERR_UNVERSIONED_RESOURCE, NULL,
                            _("Either a URL or versioned item is required.")),
                    );
    }

    svn_string_t *val = svn_string_create(value, requestPool.pool());

    svn_revnum_t set_revision;
    SVN_JNI_ERR(svn_client_revprop_set(name, val, URL, rev.revision(),
                                       &set_revision, force, ctx,
                                       requestPool.pool()), );
}

struct version_status_baton
{
  svn_revnum_t min_rev;   /* lowest revision found. */
  svn_revnum_t max_rev;   /* highest revision found. */
  svn_boolean_t switched; /* is anything switched? */
  svn_boolean_t modified; /* is anything modified? */
  svn_boolean_t committed; /* examine last committed revisions */
  svn_boolean_t done;     /* note completion of our task. */
  const char *wc_path;    /* path whose URL we're looking for. */
  const char *wc_url;     /* URL for the path whose URL we're looking for. */
  apr_pool_t *pool;       /* pool in which to store alloc-needy things. */
};

/* This implements `svn_cancel_func_t'. */
static svn_error_t *
cancel (void *baton)
{
  struct version_status_baton *sb = (version_status_baton *)baton;
  if (sb->done)
    return svn_error_create (SVN_ERR_CANCELLED, NULL, "Finished");
  else
    return SVN_NO_ERROR;
}

/* An svn_wc_status_func_t callback function for anaylyzing status
   structures. */
static void
analyze_status(void *baton,
               const char *path,
               svn_wc_status_t *status)
{
  struct version_status_baton *sb = (version_status_baton *)baton;

  if (sb->done)
    return;

  if (! status->entry)
    return;

  /* Added files have a revision of no interest */
  if (status->text_status != svn_wc_status_added)
    {
      svn_revnum_t item_rev = (sb->committed
                               ? status->entry->cmt_rev
                               : status->entry->revision);

      if (sb->min_rev == SVN_INVALID_REVNUM || item_rev < sb->min_rev)
        sb->min_rev = item_rev;

      if (sb->max_rev == SVN_INVALID_REVNUM || item_rev > sb->max_rev)
        sb->max_rev = item_rev;
    }

  sb->switched |= status->switched;
  sb->modified |= (status->text_status != svn_wc_status_normal);
  sb->modified |= (status->prop_status != svn_wc_status_normal
                   && status->prop_status != svn_wc_status_none);

  if (sb->wc_path
      && (! sb->wc_url)
      && (strcmp (path, sb->wc_path) == 0)
      && (status->entry))
    sb->wc_url = apr_pstrdup (sb->pool, status->entry->url);
}


/* This implements `svn_wc_notify_func_t'. */
static void
notify(void *baton,
       const char *path,
       svn_wc_notify_action_t action,
       svn_node_kind_t kind,
       const char *mime_type,
       svn_wc_notify_state_t content_state,
       svn_wc_notify_state_t prop_state,
       svn_revnum_t revision)
{
  struct version_status_baton *sb = (version_status_baton *)baton;
  if ((action == svn_wc_notify_status_external)
      || (action == svn_wc_notify_status_completed))
    sb->done = TRUE;
}

jstring SVNClient::getVersionInfo(const char *path, const char *trailUrl,
                                  bool lastChanged)
{
    struct version_status_baton sb;
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", NULL);
    sb.switched = FALSE;
    sb.modified = FALSE;
    sb.committed = FALSE;
    sb.min_rev = SVN_INVALID_REVNUM;
    sb.max_rev = SVN_INVALID_REVNUM;
    sb.wc_path = NULL;
    sb.wc_url = NULL;
    sb.done = FALSE;
    sb.pool = requestPool.pool();

    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), NULL);

    int wc_format;
    svn_client_ctx_t ctx = { 0 };
    SVN_JNI_ERR(svn_wc_check_wc(intPath.c_str(), &wc_format,
                                requestPool.pool()),
                NULL);

    if (! wc_format)
    {
        svn_node_kind_t kind;
        SVN_JNI_ERR(svn_io_check_path(intPath.c_str(), &kind,
                                      requestPool.pool()),
                    NULL);
        if (kind == svn_node_dir)
        {
            return JNIUtil::makeJString("exported");
        }
        else
        {
            char *message = JNIUtil::getFormatBuffer();
            apr_snprintf(message, JNIUtil::formatBufferSize,
                _("'%s' not versioned, and not exported\n"), path);
            return JNIUtil::makeJString(message);
        }
    }

    sb.wc_path = path;
    svn_opt_revision_t rev;
    rev.kind = svn_opt_revision_unspecified;
    ctx.config = apr_hash_make (requestPool.pool());

  /* Setup the notification and cancellation callbacks, and their
     shared baton (which is also shared with the status function). */
    ctx.notify_func = notify;
    ctx.notify_baton = &sb;
    ctx.cancel_func = cancel;
    ctx.cancel_baton = &sb;

    svn_error_t *err;
    err = svn_client_status(NULL, intPath.c_str(), &rev, analyze_status,
                            &sb, TRUE, TRUE, FALSE, FALSE, &ctx,
                            requestPool.pool());
    if (err && (err->apr_err == SVN_ERR_CANCELLED))
        svn_error_clear (err);
    else
        SVN_JNI_ERR(err, NULL);

    if ((! sb.switched ) && (trailUrl))
    {
        /* If the trailing part of the URL of the working copy directory
           does not match the given trailing URL then the whole working
           copy is switched. */
        if (! sb.wc_url)
        {
            sb.switched = TRUE;
        }
        else
        {
            apr_size_t len1 = strlen (trailUrl);
            apr_size_t len2 = strlen (sb.wc_url);
            if ((len1 > len2) || strcmp (sb.wc_url + len2 - len1, trailUrl))
                sb.switched = TRUE;
        }
    }

    std::ostringstream value;
    value << sb.min_rev;
    if (sb.min_rev != sb.max_rev)
    {
        value << ":";
        value << sb.max_rev;
    }
    if (sb.modified)
        value << "M";
    if (sb.switched)
        value << "S";

    return JNIUtil::makeJString(value.str().c_str());
}

jobjectArray SVNClient::revProperties(jobject jthis, const char *path,
                                      Revision &revision)
{
    apr_hash_t *props;
    Pool requestPool;
    SVN_JNI_NULL_PTR_EX(path, "path", NULL);
    Path intPath(path);
    SVN_JNI_ERR(intPath.error_occured(), NULL);

    const char *URL;
    svn_revnum_t set_rev;
    SVN_JNI_ERR(svn_client_url_from_path(&URL, intPath.c_str(),
                                         requestPool.pool()),
                NULL);

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return NULL;
    }

    SVN_JNI_ERR(svn_client_revprop_list(&props, URL, revision.revision(),
                                        &set_rev, ctx, requestPool.pool()),
                NULL);

    apr_hash_index_t *hi;

    int count = apr_hash_count (props);

    JNIEnv *env = JNIUtil::getEnv();
    jclass clazz = env->FindClass(JAVA_PACKAGE"/PropertyData");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jobjectArray ret = env->NewObjectArray(count, clazz, NULL);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(clazz);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    int i = 0;
    for (hi = apr_hash_first (requestPool.pool(), props); hi;
         hi = apr_hash_next (hi), i++)
    {
        const char *key;
        svn_string_t *val;

        apr_hash_this(hi, (const void **)&key, NULL, (void**)&val);

        jobject object = createJavaProperty(jthis, path,
                                            key, val);

        env->SetObjectArrayElement(ret, i, object);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        env->DeleteLocalRef(object);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }
    return ret;
}
struct info_entry
{
    const char *path;
    bool copied;
    bool deleted;
    bool absent;
    bool incomplete;
    svn_info_t *info;
};

struct info_baton
{
    std::vector<info_entry> infoVect;
    const char *wcPath;
    apr_pool_t *pool;
};

jobjectArray
SVNClient::info(const char *path, Revision &revision, Revision &pegRevision,
                bool recurse)
{
    info_baton infoBaton;
    Pool requestPool;

    SVN_JNI_NULL_PTR_EX(path, "path", NULL);

    svn_client_ctx_t *ctx = getContext(NULL);
    if (ctx == NULL)
    {
        return NULL;
    }
    Path checkedPath(path);
    SVN_JNI_ERR(checkedPath.error_occured(), NULL);

    infoBaton.pool = requestPool.pool();

    // If either revision is not unspecified, we'll need to store the our
    // directory, so that we can retreive the absolute path in the receiver
    if (revision.revision()->kind != svn_opt_revision_unspecified
        || pegRevision.revision()->kind != svn_opt_revision_unspecified)
    {
        infoBaton.wcPath = path;
    }
    else
    {
        infoBaton.wcPath = NULL;
    }

    SVN_JNI_ERR(svn_client_info(checkedPath.c_str(),
                                pegRevision.revision(),
                                revision.revision(),
                                infoReceiver,
                                &infoBaton,
                                recurse ? TRUE :FALSE,
                                ctx, requestPool.pool()),
                NULL);

    JNIEnv *env = JNIUtil::getEnv();
    int size = infoBaton.infoVect.size();
    jclass clazz = env->FindClass(JAVA_PACKAGE"/Info2");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jobjectArray ret = env->NewObjectArray(size, clazz, NULL);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(clazz);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }

    for(int i = 0; i < size; i++)
    {
        info_entry infoEntry = infoBaton.infoVect[i];

        // Because we can't store a NULL reference in the infoVect, we signal
        // the lack of an entry by storing a NULL path.  If the path is NULL,
        // we add a NULL to the array of info objects.
        if (infoEntry.path == NULL)
        {
            env->SetObjectArrayElement(ret, i, NULL);
            if (JNIUtil::isJavaExceptionThrown())
            {
                return NULL;
            }
            continue;
        }

        jobject jInfo = createJavaInfo2(&infoEntry);
        env->SetObjectArrayElement(ret, i, jInfo);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
        env->DeleteLocalRef(jInfo);
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }
    return ret;
}

svn_error_t *SVNClient::infoReceiver(void *baton,
                                     const char *path,
                                     const svn_info_t *info,
                                     apr_pool_t *pool)
{
    if (JNIUtil::isJavaExceptionThrown())
        return SVN_NO_ERROR;

    info_baton *infoBaton = (info_baton*)baton;
    info_entry infoEntry;

    // We still need to fetch the entry and return a few status flags
    // for backward compat.
    svn_wc_adm_access_t *adm_access;
    const svn_wc_entry_t *entry;
    const char *full_path;

    // If we've cached the wcPath, it means that 
    if (infoBaton->wcPath != NULL)
    {
        full_path = svn_path_join(infoBaton->wcPath, path, pool);
    }
    else
    {
        full_path = path;
    }

    SVN_ERR(svn_wc_adm_probe_open2(&adm_access, NULL, full_path, FALSE, 0,
                                   pool));
    SVN_ERR(svn_wc_entry(&entry, path, adm_access, FALSE, pool));
    SVN_ERR(svn_wc_adm_close(adm_access));

    if (!entry)
    {
        // We want to store a NULL in the resulting array, but we can't put a
        // NULL reference into the infoVect vector, so we just set the path to
        // NULL, and use that later.
        infoEntry.path = NULL;
        infoBaton->infoVect.push_back(infoEntry);
        return SVN_NO_ERROR;
    }

    infoEntry.copied = entry->copied;
    infoEntry.deleted = entry->deleted;
    infoEntry.absent = entry->absent;
    infoEntry.incomplete = entry->incomplete;

    // we don't create here java Status object as we don't want too many local
    // references
    infoEntry.path = apr_pstrdup(infoBaton->pool,path);
    infoEntry.info = (svn_info_t*)apr_pcalloc (infoBaton->pool, sizeof(svn_info_t));
    infoEntry.info->URL = apr_pstrdup(infoBaton->pool,info->URL);
    infoEntry.info->rev = info->rev;
    infoEntry.info->kind = info->kind;
    infoEntry.info->repos_root_URL = apr_pstrdup(infoBaton->pool,
        info->repos_root_URL);
    infoEntry.info->repos_UUID = apr_pstrdup(infoBaton->pool, info->repos_UUID);
    infoEntry.info->last_changed_rev = info->last_changed_rev;
    infoEntry.info->last_changed_date = info->last_changed_date;
    infoEntry.info->last_changed_author = apr_pstrdup(infoBaton->pool,
        info->last_changed_author);
    if (info->lock != NULL)
        infoEntry.info->lock = svn_lock_dup(info->lock, infoBaton->pool);
    else
        infoEntry.info->lock = NULL;
    infoEntry.info->has_wc_info = info->has_wc_info;
    infoEntry.info->schedule = info->schedule;
    infoEntry.info->copyfrom_url = apr_pstrdup(infoBaton->pool,
        info->copyfrom_url);
    infoEntry.info->copyfrom_rev = info->copyfrom_rev;
    infoEntry.info->text_time = info->text_time;
    infoEntry.info->prop_time = info->prop_time;
    infoEntry.info->checksum = apr_pstrdup(infoBaton->pool, info->checksum);
    infoEntry.info->conflict_old = apr_pstrdup(infoBaton->pool,
        info->conflict_old);
    infoEntry.info->conflict_new = apr_pstrdup(infoBaton->pool,
        info->conflict_new);
    infoEntry.info->conflict_wrk = apr_pstrdup(infoBaton->pool,
        info->conflict_wrk);
    infoEntry.info->prejfile = apr_pstrdup(infoBaton->pool, info->prejfile);

    infoBaton->infoVect.push_back(infoEntry);
    return SVN_NO_ERROR;
}

jobject SVNClient::createJavaInfo2(void *entry)
{
    info_entry *infoEntry = (info_entry *)entry;
    const char *path = infoEntry->path;
    const svn_info_t *info = infoEntry->info;
    JNIEnv *env = JNIUtil::getEnv();
    jclass clazz = env->FindClass(JAVA_PACKAGE"/Info2");
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    static jmethodID mid = 0;
    if (mid == 0)
    {
        mid = env->GetMethodID(clazz, "<init>",
            "(Ljava/lang/String;Ljava/lang/String;JILjava/lang/String;"
             "Ljava/lang/String;JLjava/util/Date;Ljava/lang/String;"
             "Lorg/tigris/subversion/javahl/Lock;ZILjava/lang/String;J"
             "Ljava/util/Date;Ljava/util/Date;"
             "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
             "Ljava/lang/String;Ljava/lang/String;ZZZZ)V");
        if (JNIUtil::isJavaExceptionThrown())
        {
            return NULL;
        }
    }
    jstring jpath = JNIUtil::makeJString(path);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jurl = JNIUtil::makeJString(info->URL);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jlong jrev = info->rev;
    jint jnodeKind = EnumMapper::mapNodeKind(info->kind);
    jstring jreposRootUrl = JNIUtil::makeJString(info->repos_root_URL);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jreportUUID = JNIUtil::makeJString(info->repos_UUID);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jlong jlastChangedRev = info->last_changed_rev;
    jobject jlastChangedDate = JNIUtil::createDate(info->last_changed_date);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jlastChangedAuthor =
        JNIUtil::makeJString(info->last_changed_author);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jobject jlock = createJavaLock(info->lock);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jboolean jhasWcInfo = info->has_wc_info ? JNI_TRUE:JNI_FALSE;
    jint jschedule = EnumMapper::mapScheduleKind(info->schedule);
    jstring jcopyFromUrl = JNIUtil::makeJString(info->copyfrom_url);
    jlong jcopyFromRev = info->copyfrom_rev;
    jobject jtextTime = JNIUtil::createDate(info->text_time);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jobject jpropTime = JNIUtil::createDate(info->prop_time);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jchecksum = JNIUtil::makeJString(info->checksum);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jconflictOld = JNIUtil::makeJString(info->conflict_old);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jconflictNew = JNIUtil::makeJString(info->conflict_new);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jconflictWrk = JNIUtil::makeJString(info->conflict_wrk);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jstring jprejfile = JNIUtil::makeJString(info->prejfile);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    jboolean jcopied = infoEntry->copied ? JNI_TRUE : JNI_FALSE;
    jboolean jdeleted = infoEntry->deleted ? JNI_TRUE : JNI_FALSE;
    jboolean jabsent = infoEntry->absent ? JNI_TRUE : JNI_FALSE;
    jboolean jincomplete = infoEntry->incomplete ? JNI_TRUE : JNI_FALSE;

    jobject ret = env->NewObject(clazz, mid, jpath, jurl, jrev, jnodeKind,
        jreposRootUrl, jreportUUID, jlastChangedRev, jlastChangedDate,
        jlastChangedAuthor, jlock, jhasWcInfo, jschedule, jcopyFromUrl,
        jcopyFromRev, jtextTime, jpropTime, jchecksum, jconflictOld,
        jconflictNew, jconflictWrk, jprejfile, jcopied, jdeleted, jabsent,
        jincomplete);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(clazz);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jpath);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jurl);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jreposRootUrl);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jlastChangedDate);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jlastChangedAuthor);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jlock);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jcopyFromUrl);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jchecksum);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jtextTime);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jpropTime);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jconflictOld);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jconflictNew);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jconflictWrk);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    env->DeleteLocalRef(jprejfile);
    if (JNIUtil::isJavaExceptionThrown())
    {
        return NULL;
    }
    return ret;
}
