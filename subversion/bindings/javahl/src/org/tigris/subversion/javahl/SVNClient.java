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
 */

package org.tigris.subversion.javahl;

import org.apache.subversion.javahl.*;

import java.io.OutputStream;

import java.util.Map;
import java.util.List;
import java.util.HashSet;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.Date;
import java.text.ParseException;
import java.text.DateFormat;
import java.text.SimpleDateFormat;

/**
 * This is the main client class.  All Subversion client APIs are
 * implemented in this class.  This class is not threadsafe; if you
 * need threadsafe access, use SVNClientSynchronized.
 */
public class SVNClient implements SVNClientInterface
{
    private org.apache.subversion.javahl.SVNClient aSVNClient;

    /**
     * Standard empty contructor, builds just the native peer.
     */
    public SVNClient()
    {
        aSVNClient = new org.apache.subversion.javahl.SVNClient();
        cppAddr = aSVNClient.getCppAddr();
    }

     /**
     * release the native peer (should not depend on finalize)
     */
    public void dispose()
    {
        aSVNClient.dispose();
    }

    /**
     * release the native peer (should use dispose instead)
     */
    protected void finalize()
    {
        aSVNClient.finalize();
    }

    /**
     * slot for the adress of the native peer. The JNI code is the only user
     * of this member
     */
    protected long cppAddr;

    /**
     * @since 1.0
     */
    public Version getVersion()
    {
        return new Version(
                        org.apache.subversion.javahl.NativeResources.version);
    }

    /**
     * @since 1.3
     */
    public String getAdminDirectoryName()
    {
        return aSVNClient.getAdminDirectoryName();
    }

    /**
     * @since 1.3
     */
    public boolean isAdminDirectory(String name)
    {
        return aSVNClient.isAdminDirectory(name);
    }

    /**
     * @deprecated
     * @since 1.0
     */
    public String getLastPath()
    {
        return aSVNClient.getLastPath();
    }

    /**
     * @deprecated Use {@link #status(String, int, boolean, boolean,
     *                                boolean, boolean, String[],
     *                                StatusCallback)} instead.
     * @since 1.0
     */
    public Status singleStatus(String path, boolean onServer)
            throws ClientException
    {
        Status[] statusArray = status(path, false, onServer, true, false, false);
        if (statusArray == null || statusArray.length == 0)
            return null;
        return statusArray[0];
    }

    /**
     * @deprecated Use {@link #status(String, int, boolean, boolean,
     *                                boolean, boolean, String[],
     *                                StatusCallback)} instead.
     * @since 1.0
     */
    public Status[] status(String path, boolean descend, boolean onServer,
                           boolean getAll)
            throws ClientException
    {
        return status(path, descend, onServer, getAll, false);
    }

    /**
     * @deprecated Use {@link #status(String, int, boolean, boolean,
     *                                boolean, boolean, String[],
     *                                StatusCallback)} instead.
     * @since 1.0
     */
    public Status[] status(String path, boolean descend,
                           boolean onServer, boolean getAll,
                           boolean noIgnore)
            throws ClientException
    {
        return status(path, descend, onServer, getAll, noIgnore, false);
    }

    /**
     * @deprecated Use {@link #status(String, int, boolean, boolean,
     *                                boolean, boolean, String[],
     *                                StatusCallback)} instead.
     * @since 1.2
     */
    public Status[] status(String path, boolean descend, boolean onServer,
                           boolean getAll, boolean noIgnore,
                           boolean ignoreExternals)
            throws ClientException
    {
        class MyStatusCallback implements StatusCallback
        {
            private List<Status> statuses = new ArrayList<Status>();

            public void doStatus(Status status)
            {
                statuses.add(status);
            }

            public Status[] getStatusArray()
            {
                return statuses.toArray(new Status[statuses.size()]);
            }
        }

        MyStatusCallback callback = new MyStatusCallback();

        status(path, Depth.unknownOrImmediates(descend), onServer, getAll,
               noIgnore, ignoreExternals, null, callback);

        return callback.getStatusArray();
    }

    /**
     * @since 1.5
     */
    public void status(String path, int depth, boolean onServer,
                       boolean getAll, boolean noIgnore,
                       boolean ignoreExternals, String[] changelists,
                       StatusCallback callback)
            throws ClientException
    {
        class aStatusCallback
            implements org.apache.subversion.javahl.callback.StatusCallback
        {
            StatusCallback callback;

            public aStatusCallback(StatusCallback callback)
            {
                this.callback = callback;
            }

            public void doStatus(org.apache.subversion.javahl.Status aStatus)
            {
                callback.doStatus(new Status(aStatus));
            }
        }

        try
        {
            aSVNClient.status(path, depth, onServer, getAll, noIgnore,
                              ignoreExternals,
                              changelists == null ? null
                                : Arrays.asList(changelists),
                              new aStatusCallback(callback));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #list(String, Revision, Revision, int, int,
     *                              boolean, ListCallback)} instead.
     * @since 1.0
     */
    public DirEntry[] list(String url, Revision revision, boolean recurse)
            throws ClientException
    {
        return list(url, revision, revision, recurse);
    }

    /**
     * @deprecated Use {@link #list(String, Revision, Revision, int, int,
     *                              boolean, ListCallback)} instead.
     * @since 1.2
     */
    public DirEntry[] list(String url, Revision revision,
                                  Revision pegRevision, boolean recurse)
            throws ClientException
    {
        class MyListCallback implements ListCallback
        {
            private List<DirEntry> dirents = new ArrayList<DirEntry>();

            public void doEntry(DirEntry dirent, Lock lock)
            {
                // All of this is meant to retain backward compatibility with
                // the old svn_client_ls-style API.  For further information
                // about what is going on here, see the comments in
                // libsvn_client/list.c:store_dirent().

                if (dirent.getPath().length() == 0)
                {
                    if (dirent.getNodeKind() == NodeKind.file)
                    {
                        String absPath = dirent.getAbsPath();
                        int lastSeparator = absPath.lastIndexOf('/');
                        String path = absPath.substring(lastSeparator,
                                                        absPath.length());
                        dirent.setPath(path);
                    }
                    else
                    {
                        // It's the requested directory, which we don't want
                        // to add.
                        return;
                    }
                }

                dirents.add(dirent);
            }

            public DirEntry[] getDirEntryArray()
            {
                return dirents.toArray(new DirEntry[dirents.size()]);
            }
        }

        MyListCallback callback = new MyListCallback();

        list(url, revision, pegRevision, Depth.infinityOrImmediates(recurse),
             DirEntry.Fields.all, false, callback);

        return callback.getDirEntryArray();
    }

    /**
     * @since 1.5
     */
    public void list(String url, Revision revision,
                            Revision pegRevision, int depth, int direntFields,
                            boolean fetchLocks, ListCallback callback)
            throws ClientException
    {
        class aListCallback
            implements org.apache.subversion.javahl.callback.ListCallback
        {
            private ListCallback callback;

            public aListCallback(ListCallback callback)
            {
                this.callback = callback;
            }

            public void doEntry(org.apache.subversion.javahl.DirEntry dirent,
                                org.apache.subversion.javahl.Lock lock)
            {
                callback.doEntry(new DirEntry(dirent),
                                 lock == null ? null : new Lock(lock));
            }
        }

        try
        {
            aSVNClient.list(url,
                         revision == null ? null : revision.toApache(),
                         pegRevision == null ? null : pegRevision.toApache(),
                         depth, direntFields, fetchLocks,
                         new aListCallback(callback));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.0
     */
    public void username(String username)
    {
        aSVNClient.username(username);
    }

    /**
     * @since 1.0
     */
    public void password(String password)
    {
        aSVNClient.password(password);
    }

    /**
     * @since 1.0
     */
    public void setPrompt(PromptUserPassword prompt)
    {
        aSVNClient.setPrompt(prompt);
    }

    /**
     * @deprecated Use {@link #logMessages(String, Revision, Revision, Revision,
     *                                     boolean, boolean, boolean, String[],
     *                                     long, LogMessageCallback)} instead.
     * @since 1.0
     */
    public LogMessage[] logMessages(String path, Revision revisionStart,
                                    Revision revisionEnd)
            throws ClientException
    {
        return logMessages(path, revisionStart, revisionEnd, true, false);
    }

    /**
     * @deprecated Use {@link #logMessages(String, Revision, Revision, Revision,
     *                                     boolean, boolean, boolean, String[],
     *                                     long, LogMessageCallback)} instead.
     * @since 1.0
     */
    public LogMessage[] logMessages(String path, Revision revisionStart,
                                    Revision revisionEnd, boolean stopOnCopy)
            throws ClientException
    {
        return logMessages(path, revisionStart, revisionEnd,
                           stopOnCopy, false);
    }

    /**
     * @deprecated Use {@link #logMessages(String, Revision, Revision, Revision,
     *                                     boolean, boolean, boolean, String[],
     *                                     long, LogMessageCallback)} instead.
     * @since 1.0
     */
    public LogMessage[] logMessages(String path, Revision revisionStart,
                                    Revision revisionEnd,
                                    boolean stopOnCopy,
                                    boolean discoverPath)
            throws ClientException
    {
        return logMessages(path, revisionStart, revisionEnd, stopOnCopy,
                           discoverPath, 0);
    }

    /**
     * @deprecated Use {@link #logMessages(String, Revision, Revision, Revision,
     *                                     boolean, boolean, boolean, String[],
     *                                     long, LogMessageCallback)} instead.
     * @since 1.2
     */
    public LogMessage[] logMessages(String path, Revision revisionStart,
                                    Revision revisionEnd,
                                    boolean stopOnCopy,
                                    boolean discoverPath,
                                    long limit)
            throws ClientException
    {
        class MyLogMessageCallback implements LogMessageCallback
        {
            private List<LogMessage> messages = new ArrayList<LogMessage>();

            public void singleMessage(ChangePath[] changedPaths,
                                      long revision,
                                      Map revprops,
                                      boolean hasChildren)
            {
                String author = (String) revprops.get("svn:author");
                String message = (String) revprops.get("svn:log");
                long timeMicros;

                try {
                    LogDate date = new LogDate((String) revprops.get(
                                                                "svn:date"));
                    timeMicros = date.getTimeMicros();
                } catch (ParseException ex) {
                    timeMicros = 0;
                }

                LogMessage msg = new LogMessage(changedPaths, revision,
                                                author, timeMicros, message);

                /* Filter out the SVN_INVALID_REVNUM message which pre-1.5
                   clients won't expect, nor understand. */
                if (revision != Revision.SVN_INVALID_REVNUM)
                    messages.add(msg);
            }

            public LogMessage[] getMessages()
            {
                return messages.toArray(new LogMessage[messages.size()]);
            }
        }

        MyLogMessageCallback callback = new MyLogMessageCallback();
        String[] revProps = { "svn:log", "svn:date", "svn:author" };

        logMessages(path, revisionEnd, revisionStart, revisionEnd,
                    stopOnCopy, discoverPath, false, revProps, limit,
                    callback);

        return callback.getMessages();
    }

    /**
     * @deprecated Use {@link #logMessages(String, Revision, RevisionRange[],
     *                                     boolean, boolean, boolean, String[],
     *                                     long, LogMessageCallback)} instead.
     * @since 1.5
     */
    public void logMessages(String path,
                            Revision pegRevision,
                            Revision revisionStart,
                            Revision revisionEnd,
                            boolean stopOnCopy,
                            boolean discoverPath,
                            boolean includeMergedRevisions,
                            String[] revProps,
                            long limit,
                            LogMessageCallback callback)
            throws ClientException
    {
        logMessages(path, pegRevision, toRevisionRange(revisionStart,
                                                       revisionEnd), stopOnCopy,
                    discoverPath, includeMergedRevisions, revProps, limit,
                    callback);
    }

    /**
     * @since 1.6
     */
    public void logMessages(String path, Revision pegRevision,
                            RevisionRange[] revisionRanges,
                            boolean stopOnCopy, boolean discoverPath,
                            boolean includeMergedRevisions, String[] revProps,
                            long limit, LogMessageCallback callback)
            throws ClientException
    {
        class aLogMessageCallback
            implements org.apache.subversion.javahl.callback.LogMessageCallback
        {
            private LogMessageCallback callback;

            public aLogMessageCallback(LogMessageCallback callback)
            {
                this.callback = callback;
            }

            public void singleMessage(
                    org.apache.subversion.javahl.ChangePath[] aChangedPaths,
                    long revision, Map revprops, boolean hasChildren)
            {
                ChangePath[] changedPaths;
                
                if (aChangedPaths != null)
                {
                    changedPaths = new ChangePath[aChangedPaths.length];

                    for (int i = 0; i < aChangedPaths.length; i++)
                    {
                        changedPaths[i] = new ChangePath(aChangedPaths[i]);
                    }
                }
                else
                {
                    changedPaths = null;
                }

                callback.singleMessage(changedPaths, revision, revprops,
                                       hasChildren);
            }
        }

        try
        {
            List<org.apache.subversion.javahl.RevisionRange> aRevisions = 
              new ArrayList<org.apache.subversion.javahl.RevisionRange>(revisionRanges.length);

            for (RevisionRange range : revisionRanges)
            {
                aRevisions.add(range.toApache());
            }

            aSVNClient.logMessages(path,
                         pegRevision == null ? null :pegRevision.toApache(),
                         aRevisions, stopOnCopy, discoverPath,
                         includeMergedRevisions,
                         revProps == null ? null
                            : new HashSet<String>(Arrays.asList(revProps)),
                         limit, new aLogMessageCallback(callback));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #checkout(String, String, Revision, Revision,
     *                                  int, boolean, boolean)} instead.
     * @since 1.0
     */
    public long checkout(String moduleName, String destPath,
                         Revision revision, boolean recurse)
            throws ClientException
    {
        return checkout(moduleName, destPath, revision, revision, recurse,
                        false);
    }

    /**
     * @deprecated Use {@link #checkout(String, String, Revision, Revision,
     *                                  int, boolean, boolean)} instead.
     * @since 1.2
     */
    public long checkout(String moduleName, String destPath,
                         Revision revision, Revision pegRevision,
                         boolean recurse, boolean ignoreExternals)
            throws ClientException
    {
        return checkout(moduleName, destPath, revision, revision,
                        Depth.infinityOrFiles(recurse), ignoreExternals,
                        false);
    }

    /**
     * @since 1.5
     */
    public long checkout(String moduleName, String destPath, Revision revision,
                         Revision pegRevision, int depth,
                         boolean ignoreExternals,
                         boolean allowUnverObstructions)
            throws ClientException
    {
        try
        {
            return aSVNClient.checkout(moduleName, destPath,
                          revision == null ? null : revision.toApache(),
                          pegRevision == null ? null : pegRevision.toApache(),
                          depth, ignoreExternals, allowUnverObstructions);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #notification2(Notify2)} instead.
     * @since 1.0
     */
    public void notification(Notify notify)
    {
        aSVNClient.notification(notify);
    }

    /**
     * @since 1.2
     */
    public void notification2(Notify2 notify)
    {
        class MyNotifyCallback
            implements org.apache.subversion.javahl.callback.NotifyCallback
        {
            Notify2 notify;

            public MyNotifyCallback(Notify2 notify)
            {
                this.notify = notify;
            }

            public void onNotify(
                        org.apache.subversion.javahl.NotifyInformation aInfo)
            {
                notify.onNotify(new NotifyInformation(aInfo));
            }
        }

        aSVNClient.notification2(new MyNotifyCallback(notify));
    }

    /**
     * @since 1.5
     */
    public void setConflictResolver(ConflictResolverCallback listener)
    {
        class MyConflictResolverCallback
            implements org.apache.subversion.javahl.callback.ConflictResolverCallback
        {
            private ConflictResolverCallback callback;

            public MyConflictResolverCallback(
                                        ConflictResolverCallback callback)
            {
                this.callback = callback;
            }

            public org.apache.subversion.javahl.ConflictResult resolve(
                    org.apache.subversion.javahl.ConflictDescriptor aDescrip)
                throws org.apache.subversion.javahl.SubversionException
            {
                try
                {
                    return callback.resolve(
                                new ConflictDescriptor(aDescrip)).toApache();
                }
                catch (SubversionException ex)
                {
                    throw org.apache.subversion.javahl.ClientException.fromException(ex);
                }
            }
        }

        aSVNClient.setConflictResolver(
                                    new MyConflictResolverCallback(listener));
    }

    /**
     * @since 1.5
     */
    public void setProgressListener(ProgressListener listener)
    {
        class MyProgressListener
            implements org.apache.subversion.javahl.callback.ProgressCallback
        {
            private ProgressListener listener;

            MyProgressListener(ProgressListener listener)
            {
                this.listener = listener;
            }

            public void onProgress(org.apache.subversion.javahl.ProgressEvent
                                                                        event)
            {
                listener.onProgress(new ProgressEvent(event));
            }
        }

        MyProgressListener aListener = new MyProgressListener(listener);

        aSVNClient.setProgressCallback(aListener);
    }

    /**
     * @since 1.0
     */
    public void commitMessageHandler(CommitMessage messageHandler)
    {
        class MyCommitMessageHandler
            implements org.apache.subversion.javahl.CommitMessage
        {
            private CommitMessage handler;

            MyCommitMessageHandler(CommitMessage handler)
            {
                this.handler = handler;
            }

            public String getLogMessage(
                org.apache.subversion.javahl.CommitItem[] elementsToBeCommited)
            {
                CommitItem[] aElements =
                        new CommitItem[elementsToBeCommited.length];

                for (int i = 0; i < elementsToBeCommited.length; i++)
                {
                    aElements[i] = new CommitItem(elementsToBeCommited[i]);
                }

                if (handler == null)
                  return "";

                return handler.getLogMessage(aElements);
            }
        }

        MyCommitMessageHandler aHandler =
                                new MyCommitMessageHandler(messageHandler);

        aSVNClient.commitMessageHandler(aHandler);
    }

    /**
     * @deprecated Use {@link #remove(String[], String, boolean, boolean, Map)}
     *             instead.
     * @since 1.0
     */
    public void remove(String[] path, String message, boolean force)
            throws ClientException
    {
        remove(path, message, force, false, null);
    }

    /**
     * @since 1.5
     */
    public void remove(String[] path, String message, boolean force,
                       boolean keepLocal, Map revpropTable)
            throws ClientException
    {
        try
        {
            aSVNClient.remove(path, message, force, keepLocal, revpropTable);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #revert(String, int, String[])} instead.
     * @since 1.0
     */
    public void revert(String path, boolean recurse)
            throws ClientException
    {
        revert(path, Depth.infinityOrEmpty(recurse), null);
    }

    /**
     * @since 1.5
     */
    public void revert(String path, int depth, String[] changelists)
            throws ClientException
    {
        try
        {
            aSVNClient.revert(path, depth, changelists == null ? null
                                : Arrays.asList(changelists));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #add(String, int, boolean, boolean, boolean)}
     *             instead.
     * @since 1.0
     */
    public void add(String path, boolean recurse)
            throws ClientException
    {
        add(path, recurse, false);
    }

    /**
     * @deprecated Use {@link #add(String, int, boolean, boolean, boolean)}
     *             instead.
     * @since 1.2
     */
    public void add(String path, boolean recurse, boolean force)
            throws ClientException
    {
        add(path, Depth.infinityOrEmpty(recurse), force, false, false);
    }

    /**
     * @since 1.5
     */
    public void add(String path, int depth, boolean force,
                           boolean noIgnores, boolean addParents)
        throws ClientException
    {
        try
        {
            aSVNClient.add(path, depth, force, noIgnores, addParents);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #update(String[], Revision, int, boolean,
     *                                boolean, boolean)} instead.
     * @since 1.0
     */
    public long update(String path, Revision revision, boolean recurse)
            throws ClientException
    {
        return update(new String[]{path}, revision, recurse, false)[0];
    }

    /**
     * @deprecated Use {@link #update(String[], Revision, int, boolean,
     *                                boolean, boolean)} instead.
     * @since 1.2
     */
    public long[] update(String[] path, Revision revision,
                         boolean recurse, boolean ignoreExternals)
            throws ClientException
    {
        return update(path, revision, Depth.unknownOrFiles(recurse), false,
                      ignoreExternals, false);
    }

    /**
     * @since 1.5
     */
    public long update(String path, Revision revision, int depth,
                       boolean depthIsSticky, boolean ignoreExternals,
                       boolean allowUnverObstructions)
            throws ClientException
    {
        return update(new String[]{path}, revision, depth, depthIsSticky,
                      ignoreExternals, allowUnverObstructions)[0];
    }

    /**
     * @since 1.5
     */
    public long[] update(String[] path, Revision revision, int depth,
                         boolean depthIsSticky, boolean ignoreExternals,
                         boolean allowUnverObstructions)
            throws ClientException
    {
        try
        {
            return aSVNClient.update(path,
                                revision == null ? null : revision.toApache(),
                                depth, depthIsSticky, ignoreExternals,
                                allowUnverObstructions);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #commit(String[], String, int, boolean, boolean,
     *                                String[], Map)} instead.
     * @since 1.0
     */
    public long commit(String[] path, String message, boolean recurse)
            throws ClientException
    {
        return commit(path, message, recurse, false);
    }

    /**
     * @deprecated Use {@link #commit(String[], String, int, boolean, boolean,
     *                                String[], Map)} instead.
     * @since 1.2
     */
    public long commit(String[] path, String message, boolean recurse,
                       boolean noUnlock)
            throws ClientException
    {
        return commit(path, message, Depth.infinityOrEmpty(recurse), noUnlock,
                      false, null, null);
    }

    /**
     * @since 1.5
     */
    public long commit(String[] path, String message, int depth,
                       boolean noUnlock, boolean keepChangelist,
                       String[] changelists, Map revpropTable)
            throws ClientException
    {
        try
        {
            return aSVNClient.commit(path, message, depth, noUnlock,
                                     keepChangelist,
                                     changelists == null ? null
                                       : Arrays.asList(changelists),
                                     revpropTable);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.7
     */
    public void copy(CopySource[] sources, String destPath, String message,
                     boolean copyAsChild, boolean makeParents,
                     boolean ignoreExternals, Map revpropTable)
            throws ClientException
    {
        try
        {
            List<org.apache.subversion.javahl.CopySource> aCopySources =
                new ArrayList<org.apache.subversion.javahl.CopySource>(
                                                            sources.length);

            for (CopySource src : sources)
            {
                aCopySources.add(src.toApache());
            }

            aSVNClient.copy(aCopySources, destPath, message, copyAsChild,
                            makeParents, ignoreExternals, revpropTable);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #copy(CopySource[], String, String, boolean,
     *                              boolean, boolean, Map)} instead.
     * @since 1.5
     */
    public void copy(CopySource[] sources, String destPath, String message,
                     boolean copyAsChild, boolean makeParents,
                     Map revpropTable)
            throws ClientException
    {
        copy(sources, destPath, message, copyAsChild, makeParents, false,
             revpropTable);
    }

    /**
     * @deprecated Use {@link #copy(CopySource[], String, String, boolean,
     *                              boolean, boolean, Map)} instead.
     * @since 1.0
     */
    public void copy(String srcPath, String destPath, String message,
                     Revision revision)
            throws ClientException
    {
        copy(new CopySource[] { new CopySource(srcPath, revision,
                                               Revision.HEAD) },
             destPath, message, true, false, null);
    }

    /**
     * @since 1.5
     */
    public void move(String[] srcPaths, String destPath, String message,
                     boolean force, boolean moveAsChild,
                     boolean makeParents, Map revpropTable)
            throws ClientException
    {
        try
        {
            aSVNClient.move(srcPaths, destPath, message, force, moveAsChild,
                            makeParents, revpropTable);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #move(String[], String, String, boolean, boolean,
     *                              boolean, Map)} instead.
     * @since 1.2
     */
    public void move(String srcPath, String destPath, String message,
                     Revision ignored, boolean force)
            throws ClientException
    {
        move(new String[] { srcPath }, destPath, message, force, true, false,
             null);
    }

    /**
     * @deprecated Use {@link #move(String[], String, String, boolean, boolean,
     *                              boolean, Map)} instead.
     * @since 1.0
     */
    public void move(String srcPath, String destPath, String message,
                     boolean force)
            throws ClientException
    {
        move(new String[] { srcPath }, destPath, message, force, true, false,
             null);
    }

    /**
     * @since 1.5
     */
    public void mkdir(String[] path, String message,
                      boolean makeParents, Map revpropTable)
            throws ClientException
    {
        try
        {
            aSVNClient.mkdir(path, message, makeParents, revpropTable);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #mkdir(String[], String, boolean, Map)} instead.
     * @since 1.0
     */
    public void mkdir(String[] path, String message)
            throws ClientException
    {
        mkdir(path, message, false, null);
    }

    /**
     * @since 1.0
     */
    public void cleanup(String path)
            throws ClientException
    {
        try
        {
            aSVNClient.cleanup(path);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #resolve(String, int, int)} instead.
     * @since 1.0
     */
    public void resolved(String path, boolean recurse)
        throws ClientException
    {
        try
        {
            resolve(path, Depth.infinityOrEmpty(recurse),
                    ConflictResult.chooseMerged);
        }
        catch (SubversionException e)
        {
            throw ClientException.fromException(e);
        }
    }

    /**
     * @since 1.5
     */
    public void resolve(String path, int depth, int conflictResult)
        throws SubversionException
    {
        try
        {
            aSVNClient.resolve(path, depth, conflictResult);
        }
        catch (org.apache.subversion.javahl.SubversionException ex)
        {
            throw new SubversionException(ex);
        }
    }

    /**
     * @deprecated Use {@link #doExport(String, String, Revision, Revision,
     *                                  boolean, boolean, int, String)} instead.
     * @since 1.0
     */
    public long doExport(String srcPath, String destPath,
                                Revision revision, boolean force)
            throws ClientException
    {
        return doExport(srcPath, destPath, revision, revision, force,
                false, true, null);
    }

    /**
     * @deprecated Use {@link #doExport(String, String, Revision, Revision,
     *                                  boolean, boolean, int, String)} instead.
     * @since 1.2
     */
    public long doExport(String srcPath, String destPath, Revision revision,
                         Revision pegRevision, boolean force,
                         boolean ignoreExternals, boolean recurse,
                         String nativeEOL)
            throws ClientException
    {
        return doExport(srcPath, destPath, revision, pegRevision, force,
                        ignoreExternals, Depth.infinityOrFiles(recurse),
                        nativeEOL);
    }

    /**
     * @since 1.5
     */
    public long doExport(String srcPath, String destPath, Revision revision,
                         Revision pegRevision, boolean force,
                         boolean ignoreExternals, int depth, String nativeEOL)
            throws ClientException
    {
        try
        {
            return aSVNClient.doExport(srcPath, destPath,
                          revision == null ? null : revision.toApache(),
                          pegRevision == null ? null : pegRevision.toApache(),
                          force, ignoreExternals, depth, nativeEOL);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #doSwitch(String, String, Revision, boolean)}
     *             instead.
     * @since 1.0
     */
    public long doSwitch(String path, String url, Revision revision,
                         boolean recurse)
            throws ClientException
    {
        return doSwitch(path, url, revision, Revision.HEAD,
                        Depth.unknownOrFiles(recurse), false, false, false);
    }

    /**
     * @since 1.5
     */
    public long doSwitch(String path, String url, Revision revision,
                         Revision pegRevision, int depth,
                         boolean depthIsSticky, boolean ignoreExternals,
                         boolean allowUnverObstructions)
            throws ClientException
    {
        try
        {
            return aSVNClient.doSwitch(path, url,
                          revision == null ? null : revision.toApache(),
                          pegRevision == null ? null : pegRevision.toApache(),
                          depth, depthIsSticky, ignoreExternals,
                          allowUnverObstructions);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #doImport(String, String, String, int, boolean,
     *                                  boolean, Map)} instead.
     * @since 1.0
     */
    public void doImport(String path, String url, String message,
                         boolean recurse)
            throws ClientException
    {
        doImport(path, url, message, Depth.infinityOrFiles(recurse),
                 false, false, null);
    }

    /**
     * @since 1.5
     */
    public void doImport(String path, String url, String message,
                         int depth, boolean noIgnore,
                         boolean ignoreUnknownNodeTypes, Map revpropTable)
            throws ClientException
    {
        try
        {
            aSVNClient.doImport(path, url, message, depth, noIgnore,
                                ignoreUnknownNodeTypes, revpropTable);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.5
     */
    public String[] suggestMergeSources(String path, Revision pegRevision)
            throws SubversionException
    {
        try
        {
            return aSVNClient.suggestMergeSources(path,
                         pegRevision == null ? null : pegRevision.toApache());
        }
        catch (org.apache.subversion.javahl.SubversionException ex)
        {
            throw new SubversionException(ex);
        }
    }

    /**
     * @deprecated Use {@link #merge(String, Revision, String, Revision,
     *                               String, boolean, int, boolean,
     *                               boolean, boolean)} instead.
     * @since 1.0
     */
    public void merge(String path1, Revision revision1, String path2,
                      Revision revision2, String localPath,
                      boolean force, boolean recurse)
            throws ClientException
    {
        merge(path1, revision1, path2, revision2, localPath, force, recurse,
              false, false);
    }

    /**
     * @deprecated Use {@link #merge(String, Revision, String, Revision,
     *                               String, boolean, int, boolean,
     *                               boolean, boolean)} instead.
     * @since 1.2
     */
    public void merge(String path1, Revision revision1, String path2,
                      Revision revision2, String localPath, boolean force,
                      boolean recurse, boolean ignoreAncestry, boolean dryRun)
            throws ClientException
    {
        merge(path1, revision1, path2, revision2, localPath, force,
              Depth.infinityOrFiles(recurse), ignoreAncestry, dryRun, false);
    }

    /**
     * @since 1.5
     */
    public void merge(String path1, Revision revision1, String path2,
                      Revision revision2, String localPath, boolean force,
                      int depth, boolean ignoreAncestry, boolean dryRun,
                      boolean recordOnly)
            throws ClientException
    {
        try
        {
            aSVNClient.merge(path1,
                             revision1 == null ? null : revision1.toApache(),
                             path2,
                             revision2 == null ? null : revision2.toApache(),
                             localPath, force, depth, ignoreAncestry,
                             dryRun, recordOnly);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #merge(String, Revision, RevisionRange[],
     *                               String, boolean, int, boolean,
     *                               boolean, boolean)} instead.
     * @since 1.2
     */
    public void merge(String path, Revision pegRevision, Revision revision1,
                      Revision revision2, String localPath, boolean force,
                      boolean recurse, boolean ignoreAncestry, boolean dryRun)
           throws ClientException
    {
        merge(path, pegRevision, toRevisionRange(revision1, revision2),
              localPath, force, Depth.infinityOrFiles(recurse), ignoreAncestry,
              dryRun, false);
    }

    /**
     * @since 1.5
     */
    public void merge(String path, Revision pegRevision,
                      RevisionRange[] revisions, String localPath,
                      boolean force, int depth, boolean ignoreAncestry,
                      boolean dryRun, boolean recordOnly)
            throws ClientException
    {
        try
        {
            List<org.apache.subversion.javahl.RevisionRange> aRevisions = 
              new ArrayList<org.apache.subversion.javahl.RevisionRange>(revisions.length);

            for (RevisionRange range : revisions )
            {
                aRevisions.add(range.toApache());
            }

            aSVNClient.merge(path,
                         pegRevision == null ? null : pegRevision.toApache(),
                         aRevisions, localPath, force, depth,
                         ignoreAncestry, dryRun, recordOnly);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.5
     */
    public void mergeReintegrate(String path, Revision pegRevision,
                                 String localPath, boolean dryRun)
            throws ClientException
    {
        try
        {
            aSVNClient.mergeReintegrate(path,
                        pegRevision == null ? null : pegRevision.toApache(),
                        localPath, dryRun);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.5
     */
    public Mergeinfo getMergeinfo(String path, Revision pegRevision)
            throws SubversionException
    {
        try
        {
            org.apache.subversion.javahl.Mergeinfo aMergeinfo = 
                         aSVNClient.getMergeinfo(path,
                         pegRevision == null ? null : pegRevision.toApache());

            if (aMergeinfo == null)
                return null;

            return new Mergeinfo(aMergeinfo);
        }
        catch (org.apache.subversion.javahl.SubversionException ex)
        {
            throw new SubversionException(ex);
        }
    }

    /**
     * @since 1.7
     */
    public void getMergeinfoLog(int kind, String pathOrUrl,
                                Revision pegRevision, String mergeSourceUrl,
                                Revision srcPegRevision,
                                boolean discoverChangedPaths, int depth,
                                String[] revprops, LogMessageCallback callback)
        throws ClientException
    {
        class aLogMessageCallback
            implements org.apache.subversion.javahl.callback.LogMessageCallback
        {
            private LogMessageCallback callback;

            aLogMessageCallback(LogMessageCallback callback)
            {
                this.callback = callback;
            }

            public void singleMessage(
                    org.apache.subversion.javahl.ChangePath[] aChangedPaths,
                    long revision, Map revprops, boolean hasChildren)
            {
                ChangePath[] changedPaths;

                if (aChangedPaths != null)
                {
                    changedPaths = new ChangePath[aChangedPaths.length];

                    for (int i = 0; i < aChangedPaths.length; i++)
                    {
                        changedPaths[i] = new ChangePath(aChangedPaths[i]);
                    }
                }
                else
                {
                    changedPaths = null;
                }

                callback.singleMessage(changedPaths, revision, revprops,
                                       hasChildren);
            }
        }

        try
        {
            aSVNClient.getMergeinfoLog(kind, pathOrUrl,
                        pegRevision == null ? null : pegRevision.toApache(),
                        mergeSourceUrl,
                        srcPegRevision == null ? null :
                                                    srcPegRevision.toApache(),
                        discoverChangedPaths, depth,
                        revprops == null ? null
                          : new HashSet<String>(Arrays.asList(revprops)),
                        new aLogMessageCallback(callback));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #getMergeinfoLog(int, String, Revision, String,
     *                                         Revision, boolean, int,
     *                                         String[], LogMessageCallback)}
     *             instead.
     * @since 1.5
     */
    public void getMergeinfoLog(int kind, String pathOrUrl,
                                Revision pegRevision, String mergeSourceUrl,
                                Revision srcPegRevision,
                                boolean discoverChangedPaths,
                                String[] revprops, LogMessageCallback callback)
        throws ClientException
    {
        getMergeinfoLog(kind, pathOrUrl, pegRevision, mergeSourceUrl,
                        srcPegRevision, discoverChangedPaths, Depth.empty,
                        revprops, callback);
    }

    /**
     * @deprecated Use {@link #diff(String, Revision, String, Revision,
     *                              String, String, int, String[], boolean,
     *                              boolean, boolean)} instead.
     * @since 1.0
     */
    public void diff(String target1, Revision revision1, String target2,
                     Revision revision2, String outFileName,
                     boolean recurse)
            throws ClientException
    {
        diff(target1, revision1, target2, revision2, outFileName, recurse,
             true, false, false);
    }

    /**
     * @deprecated Use {@link #diff(String, Revision, String, Revision,
     *                              String, String, int, String[], boolean,
     *                              boolean, boolean)} instead.
     * @since 1.2
     */
    public void diff(String target1, Revision revision1, String target2,
                     Revision revision2, String outFileName, boolean recurse,
                     boolean ignoreAncestry, boolean noDiffDeleted,
                     boolean force)
            throws ClientException
    {
        diff(target1, revision1, target2, revision2, null, outFileName,
             Depth.unknownOrFiles(recurse), null, ignoreAncestry, noDiffDeleted,
             force);
    }

    /**
     * @deprecated Use {@link #diff(String, Revision, String, Revision,
     *                              String, String, int, boolean, boolean,
     *                              boolean, boolean)} instead.
     * @since 1.5
     */
    public void diff(String target1, Revision revision1, String target2,
                     Revision revision2, String relativeToDir,
                     String outFileName, int depth, String[] changelists,
                     boolean ignoreAncestry, boolean noDiffDeleted,
                     boolean force)
            throws ClientException
    {
        diff(target1, revision1, target2, revision2, relativeToDir,
             outFileName, depth, changelists, ignoreAncestry, noDiffDeleted,
             force, false);
    }

    /**
     * @since 1.7
     */
    public void diff(String target1, Revision revision1, String target2,
                     Revision revision2, String relativeToDir,
                     String outFileName, int depth, String[] changelists,
                     boolean ignoreAncestry, boolean noDiffDeleted,
                     boolean force, boolean copiesAsAdds)
            throws ClientException
    {
        try
        {
            aSVNClient.diff(target1,
                        revision1 == null ? null : revision1.toApache(),
                        target2,
                        revision2 == null ? null : revision2.toApache(),
                        relativeToDir, outFileName, depth,
                        changelists == null ? null
                            : Arrays.asList(changelists),
                        ignoreAncestry, noDiffDeleted, force,
                        copiesAsAdds);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #diff(String, Revision, Revision, Revision,
     *                              String, String, int, String[], boolean,
     *                              boolean, boolean)} instead.
     * @since 1.2
     */
    public void diff(String target, Revision pegRevision,
                     Revision startRevision, Revision endRevision,
                     String outFileName, boolean recurse,
                     boolean ignoreAncestry, boolean noDiffDeleted,
                     boolean force)
            throws ClientException
    {
        diff(target, pegRevision, startRevision, endRevision, null,
             outFileName, Depth.unknownOrFiles(recurse), null, ignoreAncestry,
             noDiffDeleted, force);
    }

    /**
     * @deprecated Use {@link #diff(String, Revision, Revision, Revision,
     *                              String, String, int, boolean, boolean,
     *                              boolean, boolean)} instead.
     * @since 1.5
     */
    public void diff(String target, Revision pegRevision,
                     Revision startRevision, Revision endRevision,
                     String relativeToDir, String outFileName, int depth,
                     String[] changelists, boolean ignoreAncestry,
                     boolean noDiffDeleted, boolean force)
            throws ClientException
    {
        diff(target, pegRevision, startRevision, endRevision, relativeToDir,
             outFileName, depth, changelists, ignoreAncestry, noDiffDeleted,
             force, false);
    }

    /**
     * @since 1.7
     */
    public void diff(String target, Revision pegRevision,
                     Revision startRevision, Revision endRevision,
                     String relativeToDir, String outFileName, int depth,
                     String[] changelists, boolean ignoreAncestry,
                     boolean noDiffDeleted, boolean force,
                     boolean copiesAsAdds)
            throws ClientException
    {
        try
        {
            aSVNClient.diff(target,
                     pegRevision == null ? null : pegRevision.toApache(),
                     startRevision == null ? null : startRevision.toApache(),
                     endRevision == null ? null : endRevision.toApache(),
                     relativeToDir, outFileName, depth,
                     changelists == null ? null : Arrays.asList(changelists),
                     ignoreAncestry, noDiffDeleted, force, copiesAsAdds);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.5
     */
    public void diffSummarize(String target1, Revision revision1,
                              String target2, Revision revision2,
                              int depth, String[] changelists,
                              boolean ignoreAncestry,
                              DiffSummaryReceiver receiver)
            throws ClientException
    {
        try
        {
            MyDiffSummaryReceiver aReceiver =
                                        new MyDiffSummaryReceiver(receiver);
            aSVNClient.diffSummarize(target1,
                            revision1 == null ? null : revision1.toApache(),
                            target2,
                            revision2 == null ? null : revision2.toApache(),
                            depth,
                            changelists == null ? null
                              : Arrays.asList(changelists),
                            ignoreAncestry, aReceiver);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.5
     */
    public void diffSummarize(String target, Revision pegRevision,
                              Revision startRevision, Revision endRevision,
                              int depth, String[] changelists,
                              boolean ignoreAncestry,
                              DiffSummaryReceiver receiver)
            throws ClientException
    {
        try
        {
            MyDiffSummaryReceiver aReceiver =
                                        new MyDiffSummaryReceiver(receiver);
            aSVNClient.diffSummarize(target,
                       pegRevision == null ? null : pegRevision.toApache(),
                       startRevision == null ? null : startRevision.toApache(),
                       endRevision == null ? null : endRevision.toApache(),
                       depth, changelists == null ? null
                            : Arrays.asList(changelists),
                       ignoreAncestry, aReceiver);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #properties(String, Revision, Revision,
     *                                    int, String[], ProplistCallback)}
     *             instead.
     * @since 1.0
     */
    public PropertyData[] properties(String path) throws ClientException
    {
        return properties(path, null);
    }

    /**
     * @deprecated Use {@link #properties(String, Revision, Revision,
     *                                    int, String[], ProplistCallback)}
     *             instead.
     * @since 1.2
     */
    public PropertyData[] properties(String path, Revision revision)
            throws ClientException
    {
        return properties(path, revision, revision);
    }

    /**
     * @deprecated Use {@link #properties(String, Revision, Revision,
     *                                    int, String[], ProplistCallback)}
     *             instead.
     * @since 1.2
     */
    public PropertyData[] properties(String path, Revision revision,
                                     Revision pegRevision)
            throws ClientException
    {
        ProplistCallbackImpl callback = new ProplistCallbackImpl();
        properties(path, revision, pegRevision, Depth.empty, null, callback);

        Map<String, String> propMap = callback.getProperties(path);
        if (propMap == null)
            return new PropertyData[0];
        PropertyData[] props = new PropertyData[propMap.size()];

        int i = 0;
        for (String key : propMap.keySet())
        {
            props[i] = new PropertyData(path, key, (String) propMap.get(key));
            i++;
        }

        return props;
    }

    /**
     * @since 1.5
     */
    public void properties(String path, Revision revision,
                           Revision pegRevision, int depth,
                           String[] changelists, ProplistCallback callback)
            throws ClientException
    {
        try
        {
            aSVNClient.properties(path,
                          revision == null ? null : revision.toApache(),
                          pegRevision == null ? null : pegRevision.toApache(),
                          depth, changelists == null ? null
                                : Arrays.asList(changelists), callback);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #propertySet(String, String, String, int,
     *                                     String[], boolean, Map)} instead.
     * @since 1.0
     */
    public void propertySet(String path, String name, String value,
                            boolean recurse)
            throws ClientException
    {
        propertySet(path, name, value, recurse, false);
    }

    /**
     * @deprecated Use {@link #propertySet(String, String, String, int,
     *                                     String[], boolean, Map)} instead.
     * @since 1.2
     */
    public void propertySet(String path, String name, String value,
                                   boolean recurse, boolean force)
            throws ClientException
    {
        propertySet(path, name, value, Depth.infinityOrEmpty(recurse), null,
                    force, null);
    }

    /**
     * @deprecated Use {@link #propertySet(String, String, String, int,
     *                                     String[], boolean, Map)} instead.
     * @since 1.0
     */
    public void propertySet(String path, String name, byte[] value,
                            boolean recurse)
            throws ClientException
    {
        propertySet(path, name, value, recurse, false);
    }

    /**
     * @deprecated Use {@link #propertySet(String, String, String, int,
     *                                     String[], boolean, Map)} instead.
     * @since 1.2
     */
    public void propertySet(String path, String name, byte[] value,
                            boolean recurse, boolean force)
            throws ClientException
    {
        propertySet(path, name, new String(value), recurse, force);
    }

    /**
     * @since 1.5
     */
    public void propertySet(String path, String name, String value, int depth,
                            String[] changelists, boolean force,
                            Map revpropTable)
            throws ClientException
    {
        try
        {
            aSVNClient.propertySet(path, name, value, depth,
                                   changelists == null ? null
                                    : Arrays.asList(changelists),
                                   force, revpropTable);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #propertyRemove(String, String, int, String[])}
                   instead.
     * @since 1.0
     */
    public void propertyRemove(String path, String name, boolean recurse)
            throws ClientException
    {
        propertyRemove(path, name, Depth.infinityOrEmpty(recurse), null);
    }

    /**
     * @since 1.5
     */
    public void propertyRemove(String path, String name, int depth,
                               String[] changelists)
            throws ClientException
    {
        propertySet(path, name, null, depth, changelists, false, null);
    }

    /**
     * @deprecated Use {@link #propertyCreate(String, String, String, int,
     *                                        String[], boolean)} instead.
     * @since 1.0
     */
    public void propertyCreate(String path, String name, String value,
                               boolean recurse)
            throws ClientException
    {
        propertyCreate(path, name, value, recurse, false);
    }

    /**
     * @deprecated Use {@link #propertyCreate(String, String, String, int,
     *                                        String[], boolean)} instead.
     * @since 1.2
     */
    public void propertyCreate(String path, String name, String value,
                               boolean recurse, boolean force)
            throws ClientException
    {
        propertySet(path, name, value, recurse, force);
    }

    /**
     * @deprecated Use {@link #propertyCreate(String, String, String, int,
     *                                        String[], boolean)} instead.
     * @since 1.0
     */
    public void propertyCreate(String path, String name, byte[] value,
                               boolean recurse)
            throws ClientException
    {
        propertyCreate(path, name, value, recurse, false);
    }

    /**
     * @deprecated Use {@link #propertyCreate(String, String, String, int,
     *                                        String[], boolean)} instead.
     * @since 1.2
     */
    public void propertyCreate(String path, String name, byte[] value,
                               boolean recurse, boolean force)
            throws ClientException
    {
        propertyCreate(path, name, new String(value), recurse, force);
    }

    /**
     * @since 1.5
     */
    public void propertyCreate(String path, String name, String value,
                               int depth, String[] changelists, boolean force)
            throws ClientException
    {
        propertySet(path, name, value, depth, changelists, force, null);
    }

    /**
     * @since 1.0
     */
    public PropertyData revProperty(String path, String name, Revision rev)
            throws ClientException
    {
        try
        {
            return new PropertyData(aSVNClient.revProperty(path, name,
                                         rev == null ? null : rev.toApache()));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.2
     */
    public PropertyData[] revProperties(String path, Revision rev)
            throws ClientException
    {
        try
        {
            org.apache.subversion.javahl.PropertyData[] aPropData =
                              aSVNClient.revProperties(path,
                                          rev == null ? null : rev.toApache());
            PropertyData[] propData = new PropertyData[aPropData.length];

            for (int i = 0; i < aPropData.length; i++)
            {
                propData[i] = new PropertyData(aPropData[i]);
            }

            return propData;
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #setRevProperty(String, String, Revision, String,
     *                                        String, boolean)} instead.
     * @since 1.2
     */
    public void setRevProperty(String path, String name, Revision rev,
                               String value, boolean force)
            throws ClientException
    {
        setRevProperty(path, name, rev, value, null, force);
    }

    /**
     * @since 1.6
     */
    public void setRevProperty(String path, String name, Revision rev,
                               String value, String originalValue,
                               boolean force)
            throws ClientException
    {
        try
        {
            aSVNClient.setRevProperty(path, name,
                                      rev == null ? null : rev.toApache(),
                                      value, originalValue, force);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #propertyGet(String, String, Revision)} instead.
     * @since 1.0
     */
    public PropertyData propertyGet(String path, String name)
            throws ClientException
    {
        return propertyGet(path, name, null);
    }

    /**
     * @since 1.2
     */
    public PropertyData propertyGet(String path, String name,
                                    Revision revision)
            throws ClientException
    {
        return propertyGet(path, name, revision, revision);
    }

    /**
     * @since 1.2
     */
    public PropertyData propertyGet(String path, String name,
                                    Revision revision, Revision pegRevision)
            throws ClientException
    {
        try
        {
            return new PropertyData(aSVNClient.propertyGet(path, name,
                        revision == null ? null : revision.toApache(),
                        pegRevision == null ? null : pegRevision.toApache()));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #fileContent(String, Revision, Revision)}
     *             instead.
     * @since 1.0
     */
    public byte[] fileContent(String path, Revision revision)
            throws ClientException
    {
        return fileContent(path, revision, revision);
    }

    /**
     * @since 1.2
     */
    public byte[] fileContent(String path, Revision revision,
                              Revision pegRevision)
            throws ClientException
    {
        try
        {
            return aSVNClient.fileContent(path,
                         revision == null ? null : revision.toApache(),
                         pegRevision == null ? null : pegRevision.toApache());
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.0
     */
    public void streamFileContent(String path, Revision revision,
                                  Revision pegRevision, int bufferSize,
                                  OutputStream stream)
            throws ClientException
    {
        try
        {
            aSVNClient.streamFileContent(path,
                          revision == null ? null : revision.toApache(),
                          pegRevision == null ? null : pegRevision.toApache(),
                          bufferSize, stream);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.0
     */
    public void relocate(String from, String to, String path, boolean recurse)
            throws ClientException
    {
        try
        {
            aSVNClient.relocate(from, to, path, recurse);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #blame(String, Revision, Revision, Revision,
     *                               boolean, boolean, BlameCallback2)}
     *                               instead.
     * @since 1.0
     */
    public byte[] blame(String path, Revision revisionStart,
                        Revision revisionEnd)
            throws ClientException
    {
        BlameCallbackImpl callback = new BlameCallbackImpl();
        blame(path, revisionEnd, revisionStart, revisionEnd, callback);

        StringBuffer sb = new StringBuffer();
        for (int i = 0; i < callback.numberOfLines(); i++)
        {
            BlameCallbackImpl.BlameLine line = callback.getBlameLine(i);
            if (line != null)
            {
                sb.append(line.toString());
                sb.append("\n");
            }
        }
        return sb.toString().getBytes();
    }

    /**
     * @deprecated Use {@link #blame(String, Revision, Revision, Revision,
     *                               boolean, boolean, BlameCallback2)}
     *                               instead.
     * @since 1.0
     */
    public void blame(String path, Revision revisionStart,
                      Revision revisionEnd, BlameCallback callback)
            throws ClientException
    {
        blame(path, revisionEnd, revisionStart, revisionEnd, callback);
    }

    /**
     * @deprecated Use {@link #blame(String, Revision, Revision, Revision,
     *                               boolean, boolean, BlameCallback2)}
     *                               instead.
     * @since 1.2
     */
    public void blame(String path, Revision pegRevision,
                      Revision revisionStart, Revision revisionEnd,
                      BlameCallback callback)
            throws ClientException
    {
        class BlameCallbackWrapper implements BlameCallback2
        {
            private BlameCallback oldCallback;

            public BlameCallbackWrapper(BlameCallback callback)
            {
                oldCallback = callback;
            }

            public void singleLine(Date date, long revision, String author,
                                   Date merged_date, long merged_revision,
                                   String merged_author, String merged_path,
                                   String line)
            {
                oldCallback.singleLine(date, revision, author, line);
            }
        }

        blame(path, pegRevision, revisionStart, revisionEnd, false, false,
              new BlameCallbackWrapper(callback));
    }

    /**
     * @deprecated Use {@link #blame(String, Revision, Revision, Revision,
     *                               boolean, boolean, BlameCallback3)}
     *                               instead.
     * @since 1.5
     */
    public void blame(String path, Revision pegRevision,
                      Revision revisionStart, Revision revisionEnd,
                      boolean ignoreMimeType, boolean includeMergedRevisions,
                      BlameCallback2 callback)
            throws ClientException
    {
        class BlameCallback2Wrapper implements BlameCallback3
        {
            private BlameCallback2 oldCallback;

            public BlameCallback2Wrapper(BlameCallback2 callback)
            {
                oldCallback = callback;
            }

            public void singleLine(long lineNum, long revision, Map revProps,
                                   long mergedRevision, Map mergedRevProps,
                                   String mergedPath, String line,
                                   boolean localChange)
                throws ClientException
            {
                DateFormat df =
                        new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS");

                try
                {
                    oldCallback.singleLine(
                        df.parse((String) revProps.get("svn:date")),
                        revision,
                        (String) revProps.get("svn:author"),
                        mergedRevProps == null ? null
                            : df.parse((String)
                                            mergedRevProps.get("svn:date")),
                        mergedRevision,
                        mergedRevProps == null ? null
                            : (String) mergedRevProps.get("svn:author"),
                        mergedPath, line);
                }
                catch (ParseException e)
                {
                    throw ClientException.fromException(e);
                }
            }
        }

        blame(path, pegRevision, revisionStart, revisionEnd, ignoreMimeType,
              includeMergedRevisions, new BlameCallback2Wrapper(callback));
    }

    /**
     * @since 1.7
     */
    public void blame(String path, Revision pegRevision,
                      Revision revisionStart, Revision revisionEnd,
                      boolean ignoreMimeType, boolean includeMergedRevisions,
                      BlameCallback3 callback)
            throws ClientException
    {
        class MyBlameCallback
            implements org.apache.subversion.javahl.callback.BlameCallback
        {
            private BlameCallback3 callback;

            public MyBlameCallback(BlameCallback3 callback)
            {
                this.callback = callback;
            }

            public void singleLine(long lineNum, long revision, Map revProps,
                                   long mergedRevision, Map mergedRevProps,
                                   String mergedPath, String line,
                                   boolean localChange)
                throws org.apache.subversion.javahl.ClientException
            {
                try
                {
                    callback.singleLine(lineNum, revision, revProps,
                                        mergedRevision, mergedRevProps,
                                        mergedPath, line, localChange);
                }
                catch (ClientException ex)
                {
                    throw org.apache.subversion.javahl.ClientException.fromException(ex);
                }
            }
        }

        try
        {
            MyBlameCallback wrapper = new MyBlameCallback(callback);

            aSVNClient.blame(path,
                     pegRevision == null ? null : pegRevision.toApache(),
                     revisionStart == null ? null : revisionStart.toApache(),
                     revisionEnd == null ? null : revisionEnd.toApache(),
                     ignoreMimeType, includeMergedRevisions, wrapper);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.0
     */
    public void setConfigDirectory(String configDir)
            throws ClientException
    {
        try
        {
            aSVNClient.setConfigDirectory(configDir);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.0
     */
    public String getConfigDirectory()
            throws ClientException
    {
        try
        {
            return aSVNClient.getConfigDirectory();
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.0
     */
    public void cancelOperation()
            throws ClientException
    {
        try
        {
            aSVNClient.cancelOperation();
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #info2(String, Revision, Revision, int, String[],
     *                               InfoCallback)} instead.
     * @since 1.0
     */
    public Info info(String path)
            throws ClientException
    {
        try
        {
            return new Info(aSVNClient.info(path));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.5
     */
    public void addToChangelist(String[] paths, String changelist, int depth,
                                String[] changelists)
            throws ClientException
    {
        try
        {
            aSVNClient.addToChangelist(paths, changelist, depth,
                                       changelists == null ? null
                                        : Arrays.asList(changelists));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.5
     */
    public void removeFromChangelists(String[] paths, int depth,
                                      String[] changelists)
            throws ClientException
    {
        try
        {
            aSVNClient.removeFromChangelists(paths, depth,
                                             changelists == null ? null
                                               : Arrays.asList(changelists));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.5
     */
    public void getChangelists(String rootPath, String[] changelists,
                               int depth, ChangelistCallback callback)
            throws ClientException
    {
        try
        {
            aSVNClient.getChangelists(rootPath, changelists == null ? null
                                        : Arrays.asList(changelists),
                                      depth, callback);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.2
     */
    public String getVersionInfo(String path, String trailUrl,
                                 boolean lastChanged)
            throws ClientException
    {
        try
        {
            return aSVNClient.getVersionInfo(path, trailUrl, lastChanged);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.7
     */
    public void upgrade(String path)
            throws ClientException
    {
        try
        {
            aSVNClient.upgrade(path);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * Enable logging in the JNI-code
     * @param logLevel      the level of information to log (See
     *                      SVNClientLogLevel)
     * @param logFilePath   path of the log file
     */
    public static void enableLogging(int logLevel, String logFilePath)
    {
        org.apache.subversion.javahl.SVNClient.enableLogging(logLevel,
                                                             logFilePath);
    }

    /**
     * class for the constants of the logging levels.
     * The constants are defined in SVNClientLogLevel because of building
     * reasons
     */
    public static final class LogLevel implements SVNClientLogLevel
    {
    }

    /**
     * Returns version information of subversion and the javahl binding
     * @return version information
     */
    public static String version()
    {
        return org.apache.subversion.javahl.SVNClient.version();
    }

    /**
     * Returns the major version of the javahl binding. Same version of the
     * javahl support the same interfaces
     * @return major version number
     */
    public static int versionMajor()
    {
        return org.apache.subversion.javahl.SVNClient.versionMajor();
    }

    /**
     * Returns the minor version of the javahl binding. Same version of the
     * javahl support the same interfaces
     * @return minor version number
     */
    public static int versionMinor()
    {
        return org.apache.subversion.javahl.SVNClient.versionMinor();
    }

    /**
     * Returns the micro (patch) version of the javahl binding. Same version of
     * the javahl support the same interfaces
     * @return micro version number
     */
    public static int versionMicro()
    {
        return org.apache.subversion.javahl.SVNClient.versionMicro();
    }

    /**
     * @since 1.2
     */
    public void lock(String[] path, String comment, boolean force)
            throws ClientException
    {
        try
        {
            aSVNClient.lock(path, comment, force);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @since 1.2
     */
    public void unlock(String[] path, boolean force)
            throws ClientException
    {
        try
        {
            aSVNClient.unlock(path, force);
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * @deprecated Use {@link #info2(String, Revision, Revision, int, String[],
     *                               InfoCallback)} instead.
     * @since 1.2
     */
    public Info2[] info2(String pathOrUrl, Revision revision,
                         Revision pegRevision, boolean recurse)
            throws ClientException
    {
        class MyInfoCallback implements InfoCallback
        {
            private List<Info2> infos = new ArrayList<Info2>();

            public void singleInfo(Info2 info)
            {
                infos.add(info);
            }

            public Info2[] getInfoArray()
            {
                return infos.toArray(new Info2[infos.size()]);
            }
        }

        MyInfoCallback callback = new MyInfoCallback();
        info2(pathOrUrl, revision, pegRevision,
              Depth.infinityOrEmpty(recurse), null, callback);
        return callback.getInfoArray();
    }

    /**
     * @since 1.5
     */
    public void info2(String pathOrUrl, Revision revision,
                             Revision pegRevision, int depth,
                             String[] changelists, InfoCallback callback)
            throws ClientException
    {
        class aInfoCallback
            implements org.apache.subversion.javahl.callback.InfoCallback
        {
            private InfoCallback callback;

            public aInfoCallback(InfoCallback callback)
            {
                this.callback = callback;
            }

            public void singleInfo(org.apache.subversion.javahl.Info2 aInfo)
            {
                callback.singleInfo(aInfo == null ? null : new Info2(aInfo));
            }
        }

        try
        {
            aSVNClient.info2(pathOrUrl,
                          revision == null ? null : revision.toApache(),
                          pegRevision == null ? null : pegRevision.toApache(),
                          depth, changelists == null ? null
                            : Arrays.asList(changelists),
                          new aInfoCallback(callback));
        }
        catch (org.apache.subversion.javahl.ClientException ex)
        {
            throw new ClientException(ex);
        }
    }

    /**
     * A private wrapper function for RevisionRanges.
     * @returns a single-element revision range.
     */
    private RevisionRange[] toRevisionRange(Revision rev1, Revision rev2)
    {
        RevisionRange[] ranges = new RevisionRange[1];
        ranges[0] = new RevisionRange(rev1, rev2);
        return ranges;
    }

    private class MyDiffSummaryReceiver
        implements org.apache.subversion.javahl.callback.DiffSummaryCallback
    {
        private DiffSummaryReceiver callback;

        public MyDiffSummaryReceiver(DiffSummaryReceiver callback)
        {
            this.callback = callback;
        }

        public void onSummary(org.apache.subversion.javahl.DiffSummary summary)
        {
            callback.onSummary(new DiffSummary(summary));
        }
    }
}
