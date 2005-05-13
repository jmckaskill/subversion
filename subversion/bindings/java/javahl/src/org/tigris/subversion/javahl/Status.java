/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2003-2005 CollabNet.  All rights reserved.
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

import java.util.Date;
/**
 * Subversion status API.
 * This describes the status of one subversion item (file or directory) in
 * the working copy. Will be returned by SVNClient.status or
 * SVNClient.singleStatus
 * @author Patrick Mayweg
 * @author C&eacute;dric Chabanois
 *         <a href="mailto:cchabanois@ifrance.com">cchabanois@ifrance.com</a>
 */
public class Status
{
    /**
     * the url for accessing the item
     */
    private String url;
    /**
     * the path in the working copy
     */
    private String path;
    /**
     * kind of the item (file, directory or unknonw)
     */
    private int nodeKind;
    /**
     * the base revision of the working copy
     */
    private long revision;
    /**
     * the last revision the item was changed before base
     */
    private long lastChangedRevision;
    /**
     * the last date the item was changed before base
     */
    private long lastChangedDate;
    /**
     * the last author of the last change before base
     */
    private String lastCommitAuthor;
    /**
     * the file or directory status (See StatusKind)
     */
    private int textStatus;
    /**
     * the status of the properties (See StatusKind)
     */
    private int propStatus;
    /**
     * flag is this item is locked locally by subversion
     * (running or aborted operation)
     */
    private boolean locked;
    /**
     * has this item be copied from another item
     */
    private boolean copied;
    /**
     * has the url of the item be switch
     */
    private boolean switched;
    /**
     * the file or directory status of base (See StatusKind)
     */
    private int repositoryTextStatus;
    /**
     * the status of the properties base (See StatusKind)
     */
    private int repositoryPropStatus;
    /**
     * if there is a conflict, the filename of the new version
     * from the repository
     */
    private String conflictNew;
    /**
     * if there is a conflict, the filename of the common base version
     * from the repository
     */
    private String conflictOld;
    /**
     * if there is a conflict, the filename of the former working copy
     * version
     */
    private String conflictWorking;
    /**
     * if copied, the url of the copy source
     */
    private String urlCopiedFrom;
    /**
     * if copied, the revision number of the copy source
     */
    private long revisionCopiedFrom;
    /**
     * @since 1.2
     * token specified for the lock (null if not locked)
     */
    private String lockToken;
    /**
     * @since 1.2
     * owner of the lock (null if not locked)
     */
    private String lockOwner;
    /**
     * @since 1.2
     * comment specified for the lock (null if not locked)
     */
    private String lockComment;
    /**
     * @since 1.2
     * date of the creation of the lock (null if not locked)
     */
    private long lockCreationDate;
    /**
     * @since 1.2
     * the lock in the repository
     */
    private Lock reposLock;

    /**
     * this constructor should only called from JNI code
     * @param path                  the file system path of item
     * @param url                   the url of the item
     * @param nodeKind              kind of item (directory, file or unknown
     * @param revision              the revision number of the base
     * @param lastChangedRevision   the last revision this item was changed
     * @param lastChangedDate       the last date this item was changed
     * @param lastCommitAuthor      the author of the last change
     * @param textStatus            the file or directory status (See
     *                              StatusKind)
     * @param propStatus            the property status (See StatusKind)
     * @param repositoryTextStatus  the file or directory status of the base
     * @param repositoryPropStatus  the property status of the base
     * @param locked                if the item is locked (running or aborted
     *                              operation)
     * @param copied                if the item is copy
     * @param conflictOld           in case of conflict, the file name of the
     *                              the common base version
     * @param conflictNew           in case of conflict, the file name of new
     *                              repository version
     * @param conflictWorking       in case of conflict, the file name of the
     *                              former working copy version
     * @param urlCopiedFrom         if copied, the url of the copy source
     * @param revisionCopiedFrom    if copied, the revision number of the copy
     *                              source
     * @param switched              flag if the node has been switched in the 
     *                              path
     * @param lockToken             the token for the current lock if any
     * @param lockOwner             the owner of the current lock is any
     * @param lockComment           the comment of the current lock if any
     * @param lockCreationDate      the date, the lock was created if any
     * @param reposLock             the lock as stored in the repository if
     *                              any
     */
    public Status(String path, String url, int nodeKind, long revision,
                  long lastChangedRevision, long lastChangedDate,
                  String lastCommitAuthor, int textStatus, int propStatus,
                  int repositoryTextStatus, int repositoryPropStatus,
                  boolean locked, boolean copied, String conflictOld,
                  String conflictNew, String conflictWorking,
                  String urlCopiedFrom, long revisionCopiedFrom,
                  boolean switched, String lockToken, String lockOwner, 
                  String lockComment, long lockCreationDate, Lock reposLock)
    {
        this.path = path;
        this.url = url;
        this.nodeKind = nodeKind;
        this.revision = revision;
        this.lastChangedRevision = lastChangedRevision;
        this.lastChangedDate = lastChangedDate;
        this.lastCommitAuthor = lastCommitAuthor;
        this.textStatus = textStatus;
        this.propStatus = propStatus;
        this.locked = locked;
        this.copied = copied;
        this.repositoryTextStatus = repositoryTextStatus;
        this.repositoryPropStatus = repositoryPropStatus;
        this.conflictOld = conflictOld;
        this.conflictNew = conflictNew;
        this.conflictWorking = conflictWorking;
        this.urlCopiedFrom = urlCopiedFrom;
        this.revisionCopiedFrom = revisionCopiedFrom;
        this.switched = switched;
        this.lockToken = lockToken;
        this.lockOwner = lockOwner;
        this.lockComment = lockComment;
        this.lockCreationDate = lockCreationDate;
        this.reposLock = reposLock;
    }

    /**
     * Returns the file system path of the item
     * @return path of status entry
     */
    public String getPath()
    {
        return path;
    }

    /**
     * Returns the revision as a Revision object
     * @return revision if versioned, otherwise SVN_INVALID_REVNUM
     */
    public Revision.Number getRevision()
    {
        return Revision.createNumber(revision);
    }

    /**
     * Returns the revision as a long integer
     * @return revision if versioned, otherwise SVN_INVALID_REVNUM
     */
    public long getRevisionNumber()
    {
        return revision;
    }

    /**
     * Returns the last date the item was changed or null
     * @return the last time the item was changed.
     * or null if not available
     */
    public Date getLastChangedDate()
    {
        if (lastChangedDate == 0)
            return null;
        else
            return new Date(lastChangedDate / 1000);
    }

    /**
     * Returns the author of the last changed or null
     * @return name of author if versioned, null otherwise
     */
    public String getLastCommitAuthor()
    {
        return lastCommitAuthor;
    }

    /**
     * Returns the status of the item (See StatusKind)
     * @return file status property enum of the "textual" component.
     */
    public int getTextStatus()
    {
        return textStatus;
    }

    /**
     * Returns the status of the item as text.
     * @return english text
     */
    public String getTextStatusDescription()
    {
        return Kind.getDescription(textStatus);
    }

    /**
     * Returns the status of the properties (See Status Kind)
     * @return file status property enum of the "property" component.
     */
    public int getPropStatus()
    {
        return propStatus;
    }

    /**
     * Returns the status of the properties as text
     * @return english text
     */
    public String getPropStatusDescription()
    {
        return Kind.getDescription(propStatus);
    }

    /**
     * Returns the status of the item in the repository (See StatusKind)
     * @return file status property enum of the "textual" component in the
     * repository.
     */
    public int getRepositoryTextStatus()
    {
        return repositoryTextStatus;
    }

    /**
     * Returns test status of the properties in the repository (See StatusKind)
     * @return file status property enum of the "property" component im the
     * repository.
     */
    public int getRepositoryPropStatus()
    {
        return repositoryPropStatus;
    }

    /**
     * Returns if the item is locked (running or aborted subversion operation)
     * @return true if locked
     */
    public boolean isLocked()
    {
        return locked;
    }

    /**
     * Returns if the item has been copied
     * @return true if copied
     */
    public boolean isCopied()
    {
        return copied;
    }

    /**
     * Returns in case of conflict, the filename of the most recent repository
     * version
     * @return the filename of the most recent repository version
     */
    public String getConflictNew()
    {
        return conflictNew;
    }

    /**
     * Returns in case of conflict, the filename of the common base version
     * @return the filename of the common base version
     */
    public String getConflictOld()
    {
        return conflictOld;
    }

    /**
     * Returns in case of conflict, the filename of the former working copy
     * version
     * @return the filename of the former working copy version
     */
    public String getConflictWorking()
    {
        return conflictWorking;
    }

    /**
     * Returns the repository url if any
     * @return url in repository or null if not known
     */
    public String getUrl()
    {
        return url;
    }


    /**
     * Returns the last revision the file was changed as a Revision object
     * @return last changed revision
     */
    public Revision.Number getLastChangedRevision()
    {
        return Revision.createNumber(lastChangedRevision);
    }

    /**
     * Returns the last revision the file was changed as a long integer
     * @return last changed revision
     */
    public long getLastChangedRevisionNumber()
    {
        return lastChangedRevision;
    }

    /**
     * Returns the kind of the node (file, directory or unknown, see NodeKind)
     * @return the node kind
     */
    public int getNodeKind()
    {
        return nodeKind;
    }

    /**
     * Returns if copied the copy source url or null
     * @return the source url
     */
    public String getUrlCopiedFrom()
    {
        return urlCopiedFrom;
    }

    /**
     * Returns if copied the source revision as a Revision object
     * @return the source revision
     */
    public Revision.Number getRevisionCopiedFrom()
    {
        return Revision.createNumber(revisionCopiedFrom);
    }

    /**
     * Returns if copied the source revision as s long integer
     * @return the source revision
     */
    public long getRevisionCopiedFromNumber()
    {
        return revisionCopiedFrom;
    }

    /**
     * Returns if the repository url has been switched
     * @return is the item has been switched
     */
    public boolean isSwitched()
    {
        return switched;
    }

    /**
     * Returns if is managed by svn (added, normal, modified ...)
     * @return if managed by svn
     */
    public boolean isManaged()
    {
        int status = getTextStatus();
        return (status != Status.Kind.unversioned &&
                status != Status.Kind.none &&
                status != Status.Kind.ignored);
    }

    /**
     * Returns if the resource has a remote counter-part
     * @return has version in repository
     */
    public boolean hasRemote()
    {
        return (isManaged() && getTextStatus() != Status.Kind.added);
    }

    /**
     * Returns if the resource just has been added
     * @return if added
     */
    public boolean isAdded()
    {
        return getTextStatus() == Status.Kind.added;
    }

    /**
     * Returns if the resource is schedules for delete
     * @return if deleted
     */
    public boolean isDeleted()
    {
        return getTextStatus() == Status.Kind.deleted;
    }

    /**
     * Returns if the resource has been merged
     * @return if merged
     */
    public boolean isMerged()
    {
        return getTextStatus() == Status.Kind.merged;
    }

    /**
     * Returns if the resource is ignored by svn (only returned if noIgnore
     * is set on SVNClient.list)
     * @return if ignore
     */
    public boolean isIgnored()
    {
        return getTextStatus() == Status.Kind.ignored;
    }

    /**
     * Returns if the resource itself is modified
     * @return if modified
     */
    public boolean isModified()
    {
        return getTextStatus() == Status.Kind.modified;
    }

    /**
     * Returns the lock token
     * @return the lock token
     * @since 1.2
     */
    public String getLockToken()
    {
        return lockToken;
    }

    /**
     * Returns the lock  owner
     * @return the lock owner
     * @since 1.2
     */
    public String getLockOwner()
    {
        return lockOwner;
    }

    /**
     * Returns the lock comment
     * @return the lock comment
     * @since 1.2
     */
    public String getLockComment()
    {
        return lockComment;
    }

    /**
     * Returns the lock creation date
     * @return the lock creation date
     * @since 1.2
     */
    public Date getLockCreationDate()
    {
        if (lockCreationDate == 0)
            return null;
        else
            return new Date(lockCreationDate / 1000);
    }

    /**
     * Returns the lock as in the repository
     * @return the lock as in the repository
     * @since 1.2
     */
    public Lock getReposLock()
    {
        return reposLock;
    }
    /**
     * class for kind status of the item or its properties
     * the constants are defined in the interface StatusKind for building
     * reasons
     */
    public static final class Kind implements StatusKind
    {
        /**
         * Returns the textual representation of the status
         * @param kind of status
         * @return english status
         */
        public static final String getDescription(int kind)
        {
            switch (kind)
            {
            case StatusKind.none:
                return "non-svn";
            case StatusKind.normal:
                return "normal";
            case StatusKind.added:
                return "added";
            case StatusKind.missing:
                return "missing";
            case StatusKind.deleted:
                return "deleted";
            case StatusKind.replaced:
                return "replaced";
            case StatusKind.modified:
                return "modified";
            case StatusKind.merged:
                return "merged";
            case StatusKind.conflicted:
                return "conflicted";
            case StatusKind.ignored:
                return "ignored";
            case StatusKind.incomplete:
                return "incomplete";
            case StatusKind.external:
                return "external";
            case StatusKind.unversioned:
            default:
                return "unversioned";
            }
        }
    }
}
