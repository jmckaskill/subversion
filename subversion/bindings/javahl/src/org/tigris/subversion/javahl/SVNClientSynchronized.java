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

package org.tigris.subversion.javahl;

import java.io.OutputStream;

/**
 * This class provides a threadsafe wrapped for SVNClient
 */
public class SVNClientSynchronized implements SVNClientInterface
{
    /**
     * the wrapped object, which does all the work
     */
    private SVNClient worker;

    /**
     * our class, we synchronize on that.
     */
    static private Class clazz = SVNClientSynchronized.class;

    /**
     * Create our worker
     */
    public SVNClientSynchronized()
    {
        synchronized(clazz)
        {
            worker = new SVNClient();
        }
    }

    /**
     * release the native peer (should not depend on finalize)
     */
    public void dispose()
    {
        worker.dispose();
    }

    /**
     * @return Version information about the underlying native libraries.
     */
    public Version getVersion()
    {
        synchronized(clazz)
        {
            return worker.getVersion();
        }
    }

    /**
     * @return The name of the working copy's administrative
     * directory, which is usually <code>.svn</code>.
     * @see <a 
     * href="http://svn.collab.net/repos/svn/trunk/notes/asp-dot-net-hack.txt">
     * Instructions on changing this as a work-around for the behavior of
     * ASP.Net on Windows.</a>
     * @since 1.3
     */
    public String getAdminDirectoryName()
    {
        synchronized(clazz)
        {
            return worker.getAdminDirectoryName();
        }
    }

    /**
     * @param name The name of the directory to compare.
     * @return Whether <code>name</code> is that of a working copy
     * administrative directory.
     * @since 1.3
     */
    public boolean isAdminDirectory(String name)
    {
        synchronized(clazz)
        {
            return worker.isAdminDirectory(name);
        }
    }

    /**
     * Returns the last destination path submitted.
     * @deprecated
     * @return path in Subversion format.
     */
    public String getLastPath()
    {
        synchronized(clazz)
        {
            return worker.getLastPath();
        }
    }

    /**
     * List a directory or file of the working copy.
     *
     * @param path      Path to explore.
     * @param descend   Recurse into subdirectories if they exist.
     * @param onServer  Request status information from server.
     * @param getAll    get status for uninteristing files (unchanged).
     * @return Array of Status entries.
     */
    public Status[] status(String path, boolean descend, boolean onServer,
                           boolean getAll)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.status(path, descend, onServer, getAll);
        }
    }
    /**
     * List a directory or file of the working copy.
     *
     * @param path      Path to explore.
     * @param descend   Recurse into subdirectories if they exist.
     * @param onServer  Request status information from server.
     * @param getAll    get status for uninteristing files (unchanged).
     * @param noIgnore  get status for normaly ignored files and directories.
     * @return Array of Status entries.
     */
    public Status[] status(String path, boolean descend, boolean onServer,
                           boolean getAll, boolean noIgnore)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.status(path, descend, onServer, getAll, noIgnore);
        }
    }

    /**
     * List a directory or file of the working copy.
     *
     * @param path            Path to explore.
     * @param descend         Recurse into subdirectories if they exist.
     * @param onServer        Request status information from server.
     * @param getAll          get status for uninteristing files (unchanged).
     * @param noIgnore        get status for normaly ignored files and
     *                        directories.
     * @param ignoreExternals if externals are ignored during checkout
     * @return Array of Status entries.
     * @since 1.2
     */
    public Status[] status(String path, boolean descend, boolean onServer,
                           boolean getAll, boolean noIgnore,
                           boolean ignoreExternals)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.status(path, descend, onServer, getAll, noIgnore,
                                 ignoreExternals);
        }
    }

    /**
     * List a directory or file of the working copy.
     *
     * @param path      Path to explore.
     * @param depth     How deep to recurse into subdirectories.
     * @param onServer  Request status information from server.
     * @param getAll    get status for uninteresting (unchanged) files.
     * @param noIgnore  get status for normaly ignored files and directories.
     * @param ignoreExternals if externals are ignored during status
     * @since 1.5
     */
    public void status(String path, int depth, boolean onServer,
                       boolean getAll, boolean noIgnore,
                       boolean ignoreExternals, StatusCallback callback)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.status(path, depth, onServer, getAll, noIgnore,
                          ignoreExternals, callback);
        }
    }

    /**
     * Lists the directory entries of an url on the server.
     * @param url       the url to list
     * @param revision  the revision to list
     * @param recurse   recurse into subdirectories
     * @return  Array of DirEntry objects.
     */
    public DirEntry[] list(String url, Revision revision, boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.list(url, revision, recurse);
        }
    }

    /**
     * Lists the directory entries of an url on the server.
     *
     * @param url         the url to list
     * @param revision    the revision to list
     * @param pegRevision the revision to interpret url
     * @param recurse     recurse into subdirectories
     * @return Array of DirEntry objects.
     * @since 1.2
     */
    public DirEntry[] list(String url, Revision revision, Revision pegRevision,
                           boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.list(url, revision, pegRevision, recurse);
        }
    }

    /**
     * Lists the directory entries of an url on the server.
     * @param url             the url to list
     * @param revision        the revision to list
     * @param pegRevision     the revision to interpret url
     * @param depth           the depth to recurse into subdirectories
     * @param direntFields    the fields to retrieve
     * @param fetchLocks      whether to fetch lock information
     * @param callback        the callback to receive the directory entries
     * @since 1.5
     */
    public void list(String url, Revision revision, Revision pegRevision,
                     int depth, int direntFields, boolean fetchLocks,
                     ListCallback callback)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.list(url, revision, pegRevision, depth, direntFields,
                        fetchLocks, callback);
        }
    }

    /**
     * Returns the status of a single file in the path.
     *
     * @param path      File to gather status.
     * @param onServer  Request status information from the server.
     * @return  the subversion status of the file.
     */
    public Status singleStatus(String path, boolean onServer)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.singleStatus(path, onServer);
        }
    }
    /**
     * Sets the username used for authentification.
     * @param username  the username
     */
    public void username(String username)
    {
        synchronized(clazz)
        {
            worker.username(username);
        }
    }
    /**
     * Sets the password used for authification.
     * @param password  the password
     */
    public void password(String password)
    {
        synchronized(clazz)
        {
            worker.password(password);
        }
    }
    /**
     * Register callback interface to supply username and password on demand
     * @param prompt the callback interface
     */
    public void setPrompt(PromptUserPassword prompt)
    {
        synchronized(clazz)
        {
            worker.setPrompt(prompt);
        }
    }

    /**
     * Retrieve the log messages for an item
     * @param path          path or url to get the log message for.
     * @param revisionStart first revision to show
     * @param revisionEnd   last revision to show
     * @return array of LogMessages
     */
    public LogMessage[] logMessages(String path, Revision revisionStart,
                                    Revision revisionEnd)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.logMessages(path, revisionStart, revisionEnd, true,
                                      false);
        }
    }

    /**
     * Retrieve the log messages for an item
     * @param path          path or url to get the log message for.
     * @param revisionStart first revision to show
     * @param revisionEnd   last revision to show
     * @param stopOnCopy    do not continue on copy operations
     * @return array of LogMessages
     */
    public LogMessage[] logMessages(String path, Revision revisionStart,
                                    Revision revisionEnd, boolean stopOnCopy)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.logMessages(path, revisionStart, revisionEnd,
                                      stopOnCopy, false);
        }
    }

    /**
     * Retrieve the log messages for an item
     * @param path          path or url to get the log message for.
     * @param revisionStart first revision to show
     * @param revisionEnd   last revision to show
     * @param stopOnCopy    do not continue on copy operations
     * @param discoverPath  returns the paths of the changed items in the
     *                      returned objects
     * @return array of LogMessages
     */
    public LogMessage[] logMessages(String path, Revision revisionStart,
                                    Revision revisionEnd, boolean stopOnCopy,
                                    boolean discoverPath)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.logMessages(path, revisionStart, revisionEnd,
                                      stopOnCopy, discoverPath);
        }
    }

    /**
     * Retrieve the log messages for an item
     * @param path          path or url to get the log message for.
     * @param revisionStart first revision to show
     * @param revisionEnd   last revision to show
     * @param stopOnCopy    do not continue on copy operations
     * @param discoverPath  returns the paths of the changed items in the
     *                      returned objects
     * @param limit         limit the number of log messages (if 0 or less no
     *                      limit)
     * @return array of LogMessages
     * @since 1.2
     */
    public LogMessage[] logMessages(String path, Revision revisionStart,
                                    Revision revisionEnd, boolean stopOnCopy,
                                    boolean discoverPath, long limit)
            throws ClientException
    {
        synchronized (clazz)
        {
            return worker.logMessages(path, revisionStart, revisionEnd,
                                      stopOnCopy, discoverPath, limit);
        }
    }

    /**
     * Retrieve the log messages for an item
     * @param path          path or url to get the log message for.
     * @param pegRevision   the revision to interpret path
     * @param revisionStart first revision to show
     * @param revisionEnd   last revision to show
     * @param stopOnCopy    do not continue on copy operations
     * @param discoverPath  returns the paths of the changed items in the
     *                      returned objects
     * @param includeMergedRevisions include log messages for revisions which
     *                               were merged.
     * @param revProps      the revprops to retrieve
     * @param limit         limit the number of log messages (if 0 or less no
     *                      limit)
     * @param callback      the object to receive the messages
     * @since 1.5
     */
    public void logMessages(String path, Revision pegRevision,
                            Revision revisionStart,
                            Revision revisionEnd, boolean stopOnCopy,
                            boolean discoverPath,
                            boolean includeMergedRevisions,
                            String[] revProps, long limit,
                            LogMessageCallback callback)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.logMessages(path, pegRevision, revisionStart,
                               revisionEnd, stopOnCopy, discoverPath,
                               includeMergedRevisions, revProps,
                               limit, callback);
        }
    }

    /**
     * Executes a revision checkout.
     * @param moduleName name of the module to checkout.
     * @param destPath destination directory for checkout.
     * @param revision the revision to checkout.
     * @param pegRevision the peg revision to interpret the path
     * @param depth how deep to checkout files recursively.
     * @param ignoreExternals if externals are ignored during checkout
     * @param allowUnverObstructions allow unversioned paths that obstruct adds
     * @throws ClientException
     * @since 1.5
     */
    public long checkout(String moduleName, String destPath, Revision revision,
                         Revision pegRevision, int depth,
                         boolean ignoreExternals,
                         boolean allowUnverObstructions)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.checkout(moduleName, destPath, revision, pegRevision,
                                   depth, ignoreExternals,
                                   allowUnverObstructions);
        }
    }

    /**
     * Executes a revision checkout.
     * @param moduleName name of the module to checkout.
     * @param destPath destination directory for checkout.
     * @param revision the revision to checkout.
     * @param pegRevision the peg revision to interpret the path
     * @param recurse whether you want it to checkout files recursively.
     * @param ignoreExternals if externals are ignored during checkout
     * @throws ClientException
     * @since 1.2
     */
    public long checkout(String moduleName, String destPath, Revision revision,
                         Revision pegRevision, boolean recurse,
                         boolean ignoreExternals)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.checkout(moduleName, destPath, revision, pegRevision,
                                   recurse, ignoreExternals);
        }
    }

    /**
     * Executes a revision checkout.
     * @param moduleName name of the module to checkout.
     * @param destPath destination directory for checkout.
     * @param revision the revision to checkout.
     * @param recurse whether you want it to checkout files recursively.
     * @throws ClientException
     */
    public long checkout(String moduleName, String destPath, Revision revision,
                         boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.checkout(moduleName, destPath, revision, recurse);
        }
    }
    /**
     * Sets the notification callback used to send processing information back
     * to the calling program.
     * @param notify listener that the SVN library should call on many
     *               file operations.
     * @deprecated use notification2 instead
     */
    public void notification(Notify notify)
    {
        synchronized(clazz)
        {
            worker.notification(notify);
        }
    }

    /**
     * Sets the notification callback used to send processing information back
     * to the calling program.
     *
     * @param notify listener that the SVN library should call on many
     *               file operations.
     * @since 1.2
     */
    public void notification2(Notify2 notify)
    {
        synchronized(clazz)
        {
            worker.notification2(notify);
        }
    }

    /**
     * @see SVNClientInterface#setConflictResolver(ConflictResolverCallback)
     * @since 1.5
     */
    public void setConflictResolver(ConflictResolverCallback listener)
    {
        synchronized (clazz)
        {
            worker.setConflictResolver(listener);
        }
    }

    /**
     * Set the progress callback.
     *
     * @param listener The progress callback.
     * @since 1.5
     */
    public void setProgressListener(ProgressListener listener)
    {
        synchronized (clazz)
        {
            worker.setProgressListener(listener);
        }
    }

    /**
     * Sets the commit message handler. This allows more complex commit message
     * with the list of the elements to be commited as input.
     * @param messageHandler    callback for entering commit messages
     *                          if this is set the message parameter is
     *                          ignored.
     */
    public void commitMessageHandler(CommitMessage messageHandler)
    {
        synchronized(clazz)
        {
            worker.commitMessageHandler(messageHandler);
        }
    }
    /**
     * Sets a file for deletion.
     * @param path      path or url to be deleted
     * @param message   if path is a url, this will be the commit message.
     * @param force     delete even when there are local modifications.
     * @throws ClientException
     */
    public void remove(String[] path, String message, boolean force)
            throws ClientException
    {
        remove(path, message, force, false);
    }

    /**
     * Sets a file for deletion.
     * @param path      path or url to be deleted
     * @param message   if path is a url, this will be the commit message.
     * @param force     delete even when there are local modifications.
     * @param keepLocal only remove the paths from the repository.
     * @throws ClientException
     * @since 1.5
     */
    public void remove(String[] path, String message, boolean force,
                       boolean keepLocal)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.remove(path, message, force, keepLocal);
        }
    }

    /**
     * Reverts a file to a pristine state.
     * @param path      path of the file.
     * @param recurse   recurse into subdirectories
     * @throws ClientException
     */
    public void revert(String path, boolean recurse) throws ClientException
    {
        synchronized(clazz)
        {
            worker.revert(path, recurse);
        }
    }

    /**
     * Reverts a file to a pristine state.
     * @param path      path of the file.
     * @param depth     the depth to recurse into subdirectories
     * @throws ClientException
     * @since 1.5
     */
    public void revert(String path, int depth) throws ClientException
    {
        synchronized(clazz)
        {
            worker.revert(path, depth);
        }
    }

    /**
     * Adds a file to the repository.
     * @param path      path to be added.
     * @param recurse   recurse into subdirectories
     * @throws ClientException
     */
    public void add(String path, boolean recurse) throws ClientException
    {
        synchronized(clazz)
        {
            worker.add(path, recurse);
        }
    }

    /**
     * Adds a file to the repository.
     * @param path      path to be added.
     * @param recurse   recurse into subdirectories
     * @param force     if adding a directory and recurse true and path is a
     *                  directory, all not already managed files are added.
     * @throws ClientException
     * @since 1.2
     */
    public void add(String path, boolean recurse, boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.add(path, recurse, force);
        }
    }

    /**
     * Adds a file to the repository.
     * @param path      path to be added.
     * @param depth     the depth to recurse into subdirectories
     * @param force     if adding a directory and recurse true and path is a
     *                  directory, all not already managed files are added.
     * @param noIgnores if false, don't add files or directories matching
     *                  ignore patterns
     * @param addParents add any intermediate parents to the working copy
     * @throws ClientException
     * @since 1.5
     */
    public void add(String path, int depth, boolean force,
                    boolean noIgnores, boolean addParents)
        throws ClientException
    {
        synchronized (clazz)
        {
            worker.add(path, depth, force, noIgnores, addParents);
        }
    }

    /**
     * Updates the directory or file from repository
     * @param path target file.
     * @param revision the revision number to update.
     *                 Revision.HEAD will update to the
     *                 latest revision.
     * @param recurse recursively update.
     * @throws ClientException
     */
    public long update(String path, Revision revision, boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.update(path, revision, recurse);
        }
    }

    /**
     * Updates the directories or files from repository
     * @param path array of target files.
     * @param revision the revision number to update.
     *                 Revision.HEAD will update to the
     *                 latest revision.
     * @param recurse recursively update.
     * @param ignoreExternals if externals are ignored during update
     * @throws ClientException
     * @since 1.2
     */
    public long[] update(String[] path, Revision revision, boolean recurse,
                         boolean ignoreExternals)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.update(path, revision, recurse, ignoreExternals);
        }
    }

    /**
     * Updates the directory or file from repository
     * @param path target file.
     * @param revision the revision number to update.
     *                 Revision.HEAD will update to the
     *                 latest revision.
     * @param depth  the depth to recursively update.
     * @param ignoreExternals if externals are ignored during update
     * @param allowUnverObstructions allow unversioned paths that obstruct adds
     * @throws ClientException
     * @since 1.5
     */
    public long update(String path, Revision revision, int depth,
                       boolean ignoreExternals, boolean allowUnverObstructions)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.update(path, revision, depth, ignoreExternals,
                                 allowUnverObstructions);
        }
    }

    /**
     * Updates the directories or files from repository
     * @param path array of target files.
     * @param revision the revision number to update.
     *                 Revision.HEAD will update to the
     *                 latest revision.
     * @param depth  the depth to recursively update.
     * @param ignoreExternals if externals are ignored during update
     * @param allowUnverObstructions allow unversioned paths that obstruct adds
     * @throws ClientException
     * @since 1.5
     */
    public long[] update(String[] path, Revision revision, int depth,
                         boolean ignoreExternals,
                         boolean allowUnverObstructions)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.update(path, revision, depth, ignoreExternals,
                                 allowUnverObstructions);
        }
    }

    /**
     * @see SVNClientInterface#commit(String[], String, boolean)
     */
    public long commit(String[] path, String message, boolean recurse)
            throws ClientException
    {
        synchronized (clazz)
        {
            return worker.commit(path, message, recurse, false);
        }
    }

    /**
     * @see SVNClientInterface#commit(String[], String, int, boolean, boolean, String)
     * @since 1.5
     */
    public long commit(String[] path, String message, int depth,
                       boolean noUnlock, boolean keepChangelist,
                       String changelistName)
            throws ClientException
    {
        synchronized (clazz)
        {
            return worker.commit(path, message, depth, noUnlock,
                                 keepChangelist, changelistName);
        }
    }

    /**
     * Copy versioned paths with the history preserved.
     *
     * @see SVNClientInterface#copy(CopySource[], String, String, boolean, boolean, boolean)
     * @since 1.5
     */
    public void copy(CopySource[] sources, String destPath, String message,
                     boolean copyAsChild, boolean makeParents,
                     boolean withMergeHistory)
        throws ClientException
    {
        synchronized (clazz)
        {
            worker.copy(sources, destPath, message, copyAsChild,
                        makeParents, withMergeHistory);
        }
    }

    /**
     * Copy versioned paths with the history preserved (with
     * <code>copyAsChild</code> behavior).
     *
     * @param srcPath   source path or url
     * @param destPath  destination path or url
     * @param message   commit message if destPath is an url
     * @param revision  source revision
     * @throws ClientException
     */
    public void copy(String srcPath, String destPath, String message,
                     Revision revision) throws ClientException
    {
        synchronized(clazz)
        {
            worker.copy(srcPath, destPath, message, revision);
        }
    }

    /**
     * Move or rename versioned paths.
     *
     * @see SVNClientInterface#move(String[], String, String, boolean, boolean, boolean, boolean)
     * @since 1.5
     */
    public void move(String[] srcPaths, String destPath, String message,
                     boolean force, boolean moveAsChild,
                     boolean makeParents, boolean withMergeHistory)
        throws ClientException
    {
        synchronized (clazz)
        {
            worker.move(srcPaths, destPath, message, force, moveAsChild,
                        makeParents, withMergeHistory);
        }
    }

    /**
     * @deprecated Use move() without a Revision parameter.
     * @see SVNClientInterface#move(String, String, String, Revision, boolean)
     * @since 1.2
     */
    public void move(String srcPath, String destPath, String message,
                     Revision revision, boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.move(srcPath, destPath, message, revision, force);
        }
    }

    /**
     * Move or rename versioned paths (with <code>moveAsChild</code>
     * behavior).
     *
     * @param srcPath  source path or url
     * @param destPath destination path or url
     * @param message  commit message if destPath is an url
     * @param force    even with local modifications.
     * @throws ClientException
     *
     */
    public void move(String srcPath, String destPath, String message,
                     boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.move(srcPath, destPath, message, force);
        }
    }

    /**
     * Creates a directory directly in a repository or creates a
     * directory on disk and schedules it for addition.
     * @param path      directories to be created
     * @param message   commit message to used if path contains urls
     * @param makeParents Whether to create intermediate parents
     * @throws ClientException
     * @since 1.5
     */
    public void mkdir(String[] path, String message, boolean makeParents)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.mkdir(path, message, makeParents);
        }
    }

    /**
     * Creates a directory directly in a repository or creates a
     * directory on disk and schedules it for addition.
     * @param path      directories to be created
     * @param message   commit message to used if path contains urls
     * @throws ClientException
     */
    public void mkdir(String[] path, String message) throws ClientException
    {
        synchronized(clazz)
        {
            worker.mkdir(path, message);
        }
    }

    /**
     * Recursively cleans up a local directory, finishing any
     * incomplete operations, removing lockfiles, etc.
     * @param path a local directory.
     * @throws ClientException
     */
    public void cleanup(String path) throws ClientException
    {
        synchronized(clazz)
        {
            worker.cleanup(path);
        }
    }

    /**
     * @see SVNClientInterface#resolved(String, int, int)
     * @since 1.5
     */
    public void resolved(String path, int depth, int conflictResult)
        throws SubversionException
    {
        synchronized (clazz)
        {
            worker.resolved(path, depth, conflictResult);
        }
    }

    /**
     * @see SVNClientInterface#resolved(String, boolean)
     */
    public void resolved(String path, boolean recurse) throws ClientException
    {
        synchronized (clazz)
        {
            worker.resolved(path, recurse);
        }
    }

    /**
     * Exports the contents of either a subversion repository into a
     * 'clean' directory (meaning a directory with no administrative
     * directories).
     * @param srcPath   the url of the repository path to be exported
     * @param destPath  a destination path that must not already exist.
     * @param revision  the revsion to be exported
     * @param force     set if it is ok to overwrite local files
     * @throws ClientException
     */
    public long doExport(String srcPath, String destPath, Revision revision,
                         boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.doExport(srcPath, destPath, revision, force);
        }
    }

    /**
     * Exports the contents of either a subversion repository into a
     * 'clean' directory (meaning a directory with no administrative
     * directories).
     *
     * @param srcPath         the url of the repository path to be exported
     * @param destPath        a destination path that must not already exist.
     * @param revision        the revsion to be exported
     * @param pegRevision     the revision to interpret srcPath
     * @param force           set if it is ok to overwrite local files
     * @param ignoreExternals ignore external during export
     * @param recurse         recurse to subdirectories
     * @param nativeEOL       which EOL characters to use during export
     * @throws ClientException
     * @since 1.2
     */
    public long doExport(String srcPath, String destPath, Revision revision,
                         Revision pegRevision, boolean force,
                         boolean ignoreExternals, boolean recurse,
                         String nativeEOL)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.doExport(srcPath, destPath, revision, pegRevision,
                                   force, ignoreExternals, recurse, nativeEOL);
        }
    }

    /**
     * Exports the contents of either a subversion repository into a
     * 'clean' directory (meaning a directory with no administrative
     * directories).
     *
     * @param srcPath         the url of the repository path to be exported
     * @param destPath        a destination path that must not already exist.
     * @param revision        the revsion to be exported
     * @param pegRevision     the revision to interpret srcPath
     * @param force           set if it is ok to overwrite local files
     * @param ignoreExternals ignore external during export
     * @param depth           how deep to recurse in subdirectories
     * @param nativeEOL       which EOL characters to use during export
     * @throws ClientException
     * @since 1.5
     */
    public long doExport(String srcPath, String destPath, Revision revision,
                  Revision pegRevision, boolean force, boolean ignoreExternals,
                  int depth, String nativeEOL)
            throws ClientException
    {
        synchronized (clazz)
        {
            return worker.doExport(srcPath, destPath, revision, pegRevision,
                                   force, ignoreExternals, depth, nativeEOL);
        }
    }

    /**
     * @see SVNClientInterface#doSwitch(String, String, Revision, int, boolean, boolean)
     * @since 1.5
     */
    public long doSwitch(String path, String url, Revision revision,
                         int depth, boolean ignoreExternals,
                         boolean allowUnverObstructions)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.doSwitch(path, url, revision, depth, ignoreExternals,
                                   allowUnverObstructions);
        }
    }

    /**
     * Update local copy to mirror a new url.
     * @param path      the working copy path
     * @param url       the new url for the working copy
     * @param revision  the new base revision of working copy
     * @param recurse   traverse into subdirectories
     * @throws ClientException
     */
    public long doSwitch(String path, String url, Revision revision,
                         boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.doSwitch(path, url, revision, recurse);
        }
    }

    /**
     * Import a file or directory into a repository directory  at
     * head.
     * @param path      the local path
     * @param url       the target url
     * @param message   the log message.
     * @param recurse   traverse into subdirectories
     * @throws ClientException
     */
    public void doImport(String path, String url, String message,
                         boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.doImport(path, url, message, recurse);
        }
    }

    /**
     * Import a file or directory into a repository directory  at
     * head.
     * @param path      the local path
     * @param url       the target url
     * @param message   the log message.
     * @param depth     depth to traverse into subdirectories
     * @param noIgnore  whether to add files matched by ignore patterns
     * @param ignoreUnknownNodeTypes whether to ignore files which
     *                  the node type is not konwn, just as pipes
     * @throws ClientException
     *
     * @since 1.5
     */
    public void doImport(String path, String url, String message, int depth,
                         boolean noIgnore, boolean ignoreUnknownNodeTypes)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.doImport(path, url, message, depth, noIgnore,
                            ignoreUnknownNodeTypes);
        }
    }

    /**
     * @see SVNClientInterface#suggestMergeSources(String, Revision)
     */
    public String[] suggestMergeSources(String path, Revision pegRevision)
            throws SubversionException
    {
        synchronized (clazz)
        {
            return worker.suggestMergeSources(path, pegRevision);
        }
    }

    /**
     * Merge changes from two paths into a new local path.
     * @param path1         first path or url
     * @param revision1     first revision
     * @param path2         second path or url
     * @param revision2     second revision
     * @param localPath     target local path
     * @param force         overwrite local changes
     * @param recurse       traverse into subdirectories
     * @throws ClientException
     */
    public void merge(String path1, Revision revision1, String path2,
                      Revision revision2, String localPath, boolean force,
                      boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.merge(path1, revision1, path2, revision2, localPath, force,
                         recurse);
        }
    }

    /**
     * Merge changes from two paths into a new local path.
     *
     * @param path1          first path or url
     * @param revision1      first revision
     * @param path2          second path or url
     * @param revision2      second revision
     * @param localPath      target local path
     * @param force          overwrite local changes
     * @param recurse        traverse into subdirectories
     * @param ignoreAncestry ignore if files are not related
     * @param dryRun         do not change anything
     * @throws ClientException
     * @since 1.2
     */
    public void merge(String path1, Revision revision1, String path2,
                      Revision revision2, String localPath, boolean force,
                      boolean recurse, boolean ignoreAncestry, boolean dryRun)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.merge(path1, revision1, path2, revision2, localPath, force,
                         recurse, ignoreAncestry, dryRun);
        }
    }

    /**
     * Merge changes from two paths into a new local path.
     *
     * @param path1          first path or url
     * @param revision1      first revision
     * @param path2          second path or url
     * @param revision2      second revision
     * @param localPath      target local path
     * @param force          overwrite local changes
     * @param depth          how deep to traverse into subdirectories
     * @param ignoreAncestry ignore if files are not related
     * @param dryRun         do not change anything
     * @throws ClientException
     * @since 1.5
     */
    public void merge(String path1, Revision revision1, String path2,
                      Revision revision2, String localPath, boolean force,
                      int depth, boolean ignoreAncestry, boolean dryRun)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.merge(path1, revision1, path2, revision2, localPath, force,
                         depth, ignoreAncestry, dryRun);
        }
    }

    /**
     * Merge changes from two paths into a new local path.
     *
     * @param path           path or url
     * @param pegRevision    revision to interpret path
     * @param revision1      first revision
     * @param revision2      second revision
     * @param localPath      target local path
     * @param force          overwrite local changes
     * @param recurse        traverse into subdirectories
     * @param ignoreAncestry ignore if files are not related
     * @param dryRun         do not change anything
     * @throws ClientException
     * @since 1.2
     */
    public void merge(String path, Revision pegRevision, Revision revision1,
                      Revision revision2, String localPath, boolean force,
                      boolean recurse, boolean ignoreAncestry, boolean dryRun)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.merge(path, pegRevision, revision1, revision2, localPath,
                         force, recurse, ignoreAncestry, dryRun);
        }
    }

    /**
     * Merge set of revisions into a new local path.
     * @param path          path or url
     * @param pegRevision   revision to interpret path
     * @param revisions     revisions to merge
     * @param localPath     target local path
     * @param force         overwrite local changes
     * @param depth         how deep to traverse into subdirectories
     * @param ignoreAncestry ignore if files are not related
     * @param dryRun        do not change anything
     * @throws ClientException
     * @since 1.5
     */
    public void merge(String path, Revision pegRevision,
                      RevisionRange[] revisions, String localPath,
                      boolean force, int depth, boolean ignoreAncestry,
                      boolean dryRun) throws ClientException
    {
        synchronized(clazz)
        {
            worker.merge(path, pegRevision, revisions, localPath, force,
                         depth, ignoreAncestry, dryRun);
        }
    }

    /**
     * @see SVNClientInterface#getMergeInfo(String, Revision)
     * @since 1.5
     */
    public MergeInfo getMergeInfo(String path, Revision pegRevision)
        throws SubversionException
    {
        synchronized (clazz)
        {
            return worker.getMergeInfo(path, pegRevision);
        }
    }

    /**
     * @see SVNClientInterface#getAvailableMerges(String, Revision, String)
     * @since 1.5
     */
    public RevisionRange[] getAvailableMerges(String path,
                                              Revision pegRevision,
                                              String mergeSource)
        throws SubversionException
    {
        synchronized (clazz)
        {
            return worker.getAvailableMerges(path, pegRevision,
                                             mergeSource);
        }
    }

    /**
     * Display the differences between two paths
     * @param target1       first path or url
     * @param revision1     first revision
     * @param target2       second path or url
     * @param revision2     second revision
     * @param outFileName   file name where difference are written
     * @param recurse       traverse into subdirectories
     * @throws ClientException
     */
    public void diff(String target1, Revision revision1, String target2,
                     Revision revision2, String outFileName, boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.diff(target1, revision1, target2, revision2, outFileName,
                        recurse);
        }
    }

    /**
     * Display the differences between two paths
     *
     * @param target1        first path or url
     * @param revision1      first revision
     * @param target2        second path or url
     * @param revision2      second revision
     * @param outFileName    file name where difference are written
     * @param recurse        traverse into subdirectories
     * @param ignoreAncestry ignore if files are not related
     * @param noDiffDeleted  no output on deleted files
     * @param force          diff even on binary files
     * @throws ClientException
     * @since 1.2
     */
    public void diff(String target1, Revision revision1, String target2,
                     Revision revision2, String outFileName, boolean recurse,
                     boolean ignoreAncestry, boolean noDiffDeleted,
                     boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.diff(target1, revision1, target2, revision2, outFileName,
                        recurse, ignoreAncestry, noDiffDeleted, force);
        }
    }

    /**
     * Display the differences between two paths
     * @param target1       first path or url
     * @param revision1     first revision
     * @param target2       second path or url
     * @param revision2     second revision
     * @param outFileName   file name where difference are written
     * @param depth         how deep to traverse into subdirectories
     * @param ignoreAncestry ignore if files are not related
     * @param noDiffDeleted no output on deleted files
     * @param force         diff even on binary files
     * @throws ClientException
     * @since 1.5
     */
    public void diff(String target1, Revision revision1, String target2,
                     Revision revision2, String outFileName, int depth,
                     boolean ignoreAncestry, boolean noDiffDeleted,
                     boolean force)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.diff(target1, revision1, target2, revision2, outFileName,
                        depth, ignoreAncestry, noDiffDeleted, force);
        }
    }

    /**
     * Display the differences between two paths
     *
     * @param target         path or url
     * @param pegRevision    revision tointerpret target
     * @param startRevision  first Revision to compare
     * @param endRevision    second Revision to compare
     * @param outFileName    file name where difference are written
     * @param recurse        traverse into subdirectories
     * @param ignoreAncestry ignore if files are not related
     * @param noDiffDeleted  no output on deleted files
     * @param force          diff even on binary files
     * @throws ClientException
     * @since 1.2
     */
    public void diff(String target, Revision pegRevision,
                     Revision startRevision, Revision endRevision,
                     String outFileName, boolean recurse,
                     boolean ignoreAncestry, boolean noDiffDeleted,
                     boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.diff(target, pegRevision, startRevision, endRevision,
                        outFileName, recurse, ignoreAncestry, noDiffDeleted,
                        force);
        }
    }

    /**
     * Display the differences between two paths
     * @param target        path or url
     * @param pegRevision   revision tointerpret target
     * @param startRevision first Revision to compare
     * @param endRevision   second Revision to compare
     * @param outFileName   file name where difference are written
     * @param depth         how deep to traverse into subdirectories
     * @param ignoreAncestry ignore if files are not related
     * @param noDiffDeleted no output on deleted files
     * @param force         diff even on binary files
     * @throws ClientException
     * @since 1.5
     */
    public void diff(String target, Revision pegRevision,
                     Revision startRevision, Revision endRevision,
                     String outFileName, int depth, boolean ignoreAncestry,
                     boolean noDiffDeleted, boolean force)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.diff(target, pegRevision, startRevision, endRevision,
                        outFileName, depth, ignoreAncestry, noDiffDeleted,
                        force);
        }
    }

    /**
     * Produce a diff summary which lists the items changed between
     * path and revision pairs.
     *
     * @param target1 Path or URL.
     * @param revision1 Revision of <code>target1</code>.
     * @param target2 Path or URL.
     * @param revision2 Revision of <code>target2</code>.
     * @param depth how deep to recurse.
     * @param ignoreAncestry Whether to ignore unrelated files during
     * comparison.  False positives may potentially be reported if
     * this parameter <code>false</code>, since a file might have been
     * modified between two revisions, but still have the same
     * contents.
     * @param receiver As each is difference is found, this callback
     * is invoked with a description of the difference.
     *
     * @throws ClientException
     * @since 1.5
     */
    public void diffSummarize(String target1, Revision revision1,
                              String target2, Revision revision2,
                              int depth, boolean ignoreAncestry,
                              DiffSummaryReceiver receiver)
        throws ClientException
    {
        synchronized (clazz)
        {
            worker.diffSummarize(target1, revision1, target2, revision2,
                                 depth, ignoreAncestry, receiver);
        }
    }

    /**
     * Produce a diff summary which lists the items changed between
     * path and revision pairs.
     *
     * @param target Path or URL.
     * @param pegRevision Revision at which to interpret
     * <code>target</code>.  If {@link RevisionKind#unspecified} or
     * <code>null</code>, behave identically to {@link
     * #diffSummarize(String, Revision, String, Revision, int,
     * boolean, DiffSummaryReceiver)}, using <code>path</code> for
     * both of that method's targets.
     * @param startRevision Beginning of range for comparsion of
     * <code>target</code>.
     * @param endRevision End of range for comparsion of
     * <code>target</code>.
     * @param depth how deep to recurse.
     * @param ignoreAncestry Whether to ignore unrelated files during
     * comparison.  False positives may potentially be reported if
     * this parameter <code>false</code>, since a file might have been
     * modified between two revisions, but still have the same
     * contents.
     * @param receiver As each is difference is found, this callback
     * is invoked with a description of the difference.
     *
     * @throws ClientException
     * @since 1.5
     */
    public void diffSummarize(String target, Revision pegRevision,
                              Revision startRevision, Revision endRevision,
                              int depth, boolean ignoreAncestry,
                              DiffSummaryReceiver receiver)
        throws ClientException
    {
        synchronized (clazz)
        {
            worker.diffSummarize(target, pegRevision, startRevision,
                                 endRevision, depth, ignoreAncestry,
                                 receiver);
        }
    }

    /**
     * Retrieves the properties of an item
     * @param path  the path of the item
     * @return array of property objects
     */
    public PropertyData[] properties(String path) throws ClientException
    {
        synchronized(clazz)
        {
            return worker.properties(path);
        }
    }

    /**
     * Retrieves the properties of an item
     *
     * @param path     the path of the item
     * @param revision the revision of the item
     * @return array of property objects
     * @since 1.2
     */
    public PropertyData[] properties(String path, Revision revision)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.properties(path, revision);
        }
    }

    /**
     * Retrieves the properties of an item
     *
     * @param path        the path of the item
     * @param revision    the revision of the item
     * @param pegRevision the revision to interpret path
     * @return array of property objects
     * @since 1.2
     */
    public PropertyData[] properties(String path, Revision revision,
                                     Revision pegRevision)
            throws ClientException
    {
        synchronized(clazz)
        {
            return properties(path, revision, pegRevision);
        }
    }

    /**
     * Retrieves the properties of an item
     *
     * @param path        the path of the item
     * @param revision    the revision of the item
     * @param pegRevision the revision to interpret path
     * @param depth       the depth to recurse into subdirectories
     * @param callback    the callback to use to return the properties
     * @since 1.5
     */
    public void properties(String path, Revision revision,
                           Revision pegRevision, int depth,
                           ProplistCallback callback)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.properties(path, revision, pegRevision, depth, callback);
        }
    }

    /**
     * Sets one property of an item with a String value
     * @param path      path of the item
     * @param name      name of the property
     * @param value     new value of the property
     * @param recurse   set property also on the subdirectories
     * @throws ClientException
     */
    public void propertySet(String path, String name, String value,
                            boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertySet(path, name, value, recurse);
        }
    }

    /**
     * Sets one property of an item with a String value
     *
     * @param path    path of the item
     * @param name    name of the property
     * @param value   new value of the property
     * @param recurse set property also on the subdirectories
     * @param force   do not check if the value is valid
     * @throws ClientException
     * @since 1.2
     */
    public void propertySet(String path, String name, String value,
                            boolean recurse, boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertySet(path, name, value, recurse, force);
        }
    }

    /**
     * Sets one property of an item with a byte array value
     * @param path      path of the item
     * @param name      name of the property
     * @param value     new value of the property
     * @param recurse   set property also on the subdirectories
     * @throws ClientException
     */
    public void propertySet(String path, String name, byte[] value,
                            boolean recurse) throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertySet(path, name, value, recurse);
        }
    }

    /**
     * Sets one property of an item with a byte array value
     *
     * @param path    path of the item
     * @param name    name of the property
     * @param value   new value of the property
     * @param recurse set property also on the subdirectories
     * @param force   do not check if the value is valid
     * @throws ClientException
     * @since 1.2
     */
    public void propertySet(String path, String name, byte[] value,
                            boolean recurse, boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertySet(path, name, value, recurse, force);
        }
    }

    /**
     * Sets one property of an item with a String value
     *
     * @param path    path of the item
     * @param name    name of the property
     * @param value   new value of the property
     * @param depth   the depth to recurse into subdirectories
     * @param force   do not check if the value is valid
     * @throws ClientException
     * @since 1.5
     */
    public void propertySet(String path, String name, String value, int depth,
                     boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertySet(path, name, value, depth, force);
        }
    }

    /**
     * Remove one property of an item.
     * @param path      path of the item
     * @param name      name of the property
     * @param recurse   remove the property also on subdirectories
     * @throws ClientException
     */
    public void propertyRemove(String path, String name, boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertyRemove(path, name, recurse);
        }
    }

    /**
     * Remove one property of an item.
     * @param path      path of the item
     * @param name      name of the property
     * @param depth     the depth to recurse into subdirectories
     * @throws ClientException
     * @since 1.5
     */
    public void propertyRemove(String path, String name, int depth)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertyRemove(path, name, depth);
        }
    }

    /**
     * Create and sets one property of an item with a String value
     * @param path      path of the item
     * @param name      name of the property
     * @param value     new value of the property
     * @param recurse   set property also on the subdirectories
     * @throws ClientException
     */
    public void propertyCreate(String path, String name, String value,
                               boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertyCreate(path, name, value, recurse);
        }
    }

    /**
     * Create and sets one property of an item with a String value
     *
     * @param path    path of the item
     * @param name    name of the property
     * @param value   new value of the property
     * @param recurse set property also on the subdirectories
     * @param force   do not check if the value is valid
     * @throws ClientException
     * @since 1.2
     */
    public void propertyCreate(String path, String name, String value,
                               boolean recurse, boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertyCreate(path, name, value, recurse, force);
        }
    }

    /**
     * Create and sets one property of an item with a byte array value
     * @param path      path of the item
     * @param name      name of the property
     * @param value     new value of the property
     * @param recurse   set property also on the subdirectories
     * @throws ClientException
     */
    public void propertyCreate(String path, String name, byte[] value,
                               boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertyCreate(path, name, value, recurse);
        }
    }

    /**
     * Create and sets one property of an item with a byte array value
     *
     * @param path    path of the item
     * @param name    name of the property
     * @param value   new value of the property
     * @param recurse set property also on the subdirectories
     * @param force   do not check if the value is valid
     * @throws ClientException
     * @since 1.2
     */
    public void propertyCreate(String path, String name, byte[] value,
                               boolean recurse, boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertyCreate(path, name, value, recurse, force);
        }
    }

    /**
     * Create and sets one property of an item with a byte array value
     *
     * @param path    path of the item
     * @param name    name of the property
     * @param value   new value of the property
     * @param depth   depth to set property on the subdirectories
     * @param force   do not check if the value is valid
     * @throws ClientException
     * @since 1.5
     */
    public void propertyCreate(String path, String name, String value,
                               int depth, boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.propertyCreate(path, name, value, depth, force);
        }
    }

    /**
     * Retrieve one revsision property of one item
     * @param path      path of the item
     * @param name      name of the property
     * @param rev       revision to retrieve
     * @return the Property
     * @throws ClientException
     */
    public PropertyData revProperty(String path, String name, Revision rev)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.revProperty(path, name, rev);
        }
    }

    /**
     * Retrieve all revsision properties of one item
     *
     * @param path path of the item
     * @param rev  revision to retrieve
     * @return the Properties
     * @throws ClientException
     * @since 1.2
     */
    public PropertyData[] revProperties(String path, Revision rev)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.revProperties(path, rev);
        }
    }

    /**
     * set one revsision property of one item
     * @param path      path of the item
     * @param name      name of the property
     * @param rev       revision to retrieve
     * @param value     value of the property
     * @param force
     * @throws ClientException
     * @since 1.2
     */
    public void setRevProperty(String path, String name, Revision rev,
                               String value, boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.setRevProperty(path, name, rev, value, force);
        }
    }

    /**
     * Retrieve one property of one iten
     * @param path      path of the item
     * @param name      name of property
     * @return the Property
     * @throws ClientException
     */
    public PropertyData propertyGet(String path, String name)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.propertyGet(path, name);
        }
    }

    /**
     * Retrieve one property of one iten
     *
     * @param path     path of the item
     * @param name     name of property
     * @param revision revision of the item
     * @return the Property
     * @throws ClientException
     * @since 1.2
     */
    public PropertyData propertyGet(String path,
                                    String name,
                                    Revision revision)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.propertyGet(path, name, revision);
        }
    }

    /**
     * Retrieve one property of one iten
     *
     * @param path     path of the item
     * @param name     name of property
     * @param revision revision of the item
     * @param pegRevision the revision to interpret path
     * @return the Property
     * @throws ClientException
     * @since 1.2
     */
    public PropertyData propertyGet(String path,
                                    String name,
                                    Revision revision,
                                    Revision pegRevision)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.propertyGet(path, name, revision, pegRevision);
        }
    }

    /**
     *  Retrieve the content of a file
     * @param path      the path of the file
     * @param revision  the revision to retrieve
     * @return          the content as byte array
     * @throws ClientException
     */
    public byte[] fileContent(String path, Revision revision)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.fileContent(path, revision);
        }
    }

    /**
     * Retrieve the content of a file
     *
     * @param path        the path of the file
     * @param revision    the revision to retrieve
     * @param pegRevision the revision to interpret path
     * @return the content as byte array
     * @throws ClientException
     * @since 1.2
     */
    public byte[] fileContent(String path, Revision revision,
                              Revision pegRevision)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.fileContent(path, revision, pegRevision);
        }
    }

    /**
     * Write the file's content to the specified output stream.
     *
     * @param path        the path of the file
     * @param revision    the revision to retrieve
     * @param pegRevision the revision at which to interpret the path
     * @param stream      the stream to write the file's content to
     * @throws ClientException
     */
    public void streamFileContent(String path, Revision revision,
                                  Revision pegRevision, int bufferSize,
                                  OutputStream stream)
        throws ClientException
    {
        synchronized(clazz)
        {
            worker.streamFileContent(path, revision, pegRevision, bufferSize,
                                     stream);
        }
    }

    /**
     * Rewrite the url's in the working copy
     * @param from      old url
     * @param to        new url
     * @param path      working copy path
     * @param recurse   recurse into subdirectories
     * @throws ClientException
     */
    public void relocate(String from, String to, String path, boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.relocate(from, to, path, recurse);
        }
    }

    /**
     * Return for each line of the file, the author and the revision of the
     * last together with the content.
     * @deprecated
     * @param path          the path
     * @param revisionStart the first revision to show
     * @param revisionEnd   the last revision to show
     * @return  the content together with author and revision of last change
     * @throws ClientException
     */
    public byte[] blame(String path, Revision revisionStart,
                        Revision revisionEnd)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.blame(path,revisionStart, revisionEnd);
        }
    }

    /**
     * Retrieve the content together with the author, the revision and the date
     * of the last change of each line
     * @param path          the path
     * @param revisionStart the first revision to show
     * @param revisionEnd   the last revision to show
     * @param callback      callback to receive the file content and the other
     *                      information
     * @throws ClientException
     */
    public void blame(String path,
                      Revision revisionStart,
                      Revision revisionEnd,
                      BlameCallback callback)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.blame(path, revisionStart, revisionEnd, callback);
        }
    }

    /**
     * Retrieve the content together with the author, the revision and the date
     * of the last change of each line
     * @param path          the path
     * @param pegRevision   the revision to interpret the path
     * @param revisionStart the first revision to show
     * @param revisionEnd   the last revision to show
     * @param callback      callback to receive the file content and the other
     *                      information
     * @throws ClientException
     * @since 1.2
     */
    public void blame(String path,
                      Revision pegRevision,
                      Revision revisionStart,
                      Revision revisionEnd,
                      BlameCallback callback)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.blame(path, pegRevision, revisionStart, revisionEnd,
                         callback);
        }
    }

    /**
     * Retrieve the content together with the author, the revision and the date
     * of the last change of each line
     * @param path          the path
     * @param pegRevision   the revision to interpret the path
     * @param revisionStart the first revision to show
     * @param revisionEnd   the last revision to show
     * @param ignoreMimeType whether or not to ignore the mime-type
     * @param includeMergedRevisions whether or not to include extra merge
     *                      information
     * @param callback      callback to receive the file content and the other
     *                      information
     * @throws ClientException
     * @since 1.5
     */

    public void blame(String path,
                      Revision pegRevision,
                      Revision revisionStart,
                      Revision revisionEnd,
                      boolean ignoreMimeType,
                      boolean includeMergedRevisions,
                      BlameCallback2 callback)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.blame(path, pegRevision, revisionStart, revisionEnd,
                         ignoreMimeType, includeMergedRevisions, callback);
        }
    }

    /**
     * @see SVNClientInterface#setConfigDirectory(String)
     */
    public void setConfigDirectory(String configDir) throws ClientException
    {
        synchronized(clazz)
        {
            worker.setConfigDirectory(configDir);
        }
    }

    /**
     * Get the configuration directory
     * @return  the directory
     * @throws ClientException
     */
    public String getConfigDirectory() throws ClientException
    {
        synchronized(clazz)
        {
            return worker.getConfigDirectory();
        }
    }

    /**
     * cancel the active operation
     * @throws ClientException
     */
    public void cancelOperation() throws ClientException
    {
        // this method is not synchronized, because it is designed to be called
        // from another thread
        worker.cancelOperation();
    }

    /**
     * Retrieves the working copy information for an item
     * @param path  path of the item
     * @return      the information object
     * @throws ClientException
     */
    public Info info(String path) throws ClientException
    {
        synchronized(clazz)
        {
            return worker.info(path);
        }
    }

    /**
     * Add paths to a changelist
     * @param paths      paths to add to the changelist
     * @param changelist changelist name
     */
    public void addToChangelist(String[] paths, String changelist)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.addToChangelist(paths, changelist);
        }
    }

    /**
     * Remove paths from a changelist
     * @param paths      paths to remove from the changelist
     * @param changelist changelist name
     */
    public void removeFromChangelist(String[] paths, String changelist)
            throws ClientException
    {
        synchronized (clazz)
        {
            worker.removeFromChangelist(paths, changelist);
        }
    }

    /**
     * Recursively get the paths which belong to a changelist
     * @param changelist  changelist name
     * @param rootPath    the wc path under which to check
     */
    public String[] getChangelist(String changelist, String rootPath)
            throws ClientException
    {
        synchronized (clazz)
        {
            return worker.getChangelist(changelist, rootPath);
        }
    }

    /**
     * @see SVNClientInterface#commit(String[], String, boolean, boolean)
     * @since 1.2
     */
    public long commit(String[] path, String message, boolean recurse,
                       boolean noUnlock)
            throws ClientException
    {
        synchronized (clazz)
        {
            return worker.commit(path, message, recurse, noUnlock);
        }
    }

    /**
     * Lock a working copy item
     *
     * @param path  path of the item
     * @param comment
     * @param force break an existing lock
     * @throws ClientException
     * @since 1.2
     */
    public void lock(String[] path, String comment, boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.lock(path, comment, force);
        }
    }

    /**
     * Unlock a working copy item
     *
     * @param path  path of the item
     * @param force break an existing lock
     * @throws ClientException
     * @since 1.2
     */
    public void unlock(String[] path, boolean force)
            throws ClientException
    {
        synchronized(clazz)
        {
            worker.unlock(path, force);
        }
    }

    /**
     * Retrieve information about repository or working copy items.
     *
     * @param pathOrUrl   the path or the url of the item
     * @param revision    the revision of the item to return
     * @param pegRevision the revision to interpret pathOrUrl
     * @param recurse     flag if to recurse, if the item is a directory
     * @return the information objects
     * @since 1.2
     */
    public Info2[] info2(String pathOrUrl, Revision revision,
                         Revision pegRevision, boolean recurse)
            throws ClientException
    {
        synchronized(clazz)
        {
            return worker.info2(pathOrUrl, revision, pegRevision, recurse);
        }
    }

    /**
     * @see SVNClientInterface#info2(String, Revision, Revision, int, InfoCallback)
     * @since 1.5
     */
    public void info2(String pathOrUrl, Revision revision,
                      Revision pegRevision, int depth,
                      InfoCallback callback)
        throws ClientException
    {
        synchronized (clazz)
        {
            worker.info2(pathOrUrl, revision, pegRevision, depth, callback);
        }
    }

    /**
     *  Produce a compact "version number" for a working copy
     * @param path          path of the working copy
     * @param trailUrl      to detect switches of the whole working copy
     * @param lastChanged   last changed rather than current revisions
     * @return      the compact "version number"
     * @throws ClientException
     * @since 1.2
     */
    public String getVersionInfo(String path, String trailUrl,
                                 boolean lastChanged) throws ClientException
    {
        synchronized(clazz)
        {
            return worker.getVersionInfo(path, trailUrl, lastChanged);
        }
    }

}
