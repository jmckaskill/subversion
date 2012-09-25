/*
 * err.c : implementation of fs-private error functions
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



#include <stdlib.h>
#include <stdarg.h>

#include "svn_private_config.h"
#include "svn_fs.h"
#include "fs.h"
#include "err.h"
#include "id.h"

#include "../libsvn_fs/fs-loader.h"

svn_error_t *
svn_fs_fs__check_fs (svn_fs_t *fs)
{
  if (fs->path)
    return SVN_NO_ERROR;
  else
    return svn_error_create (SVN_ERR_FS_NOT_OPEN, 0,
                             _("Filesystem object has not been opened yet"));
}



/* Building common error objects.  */


static svn_error_t *
corrupt_id (const char *fmt, const svn_fs_id_t *id, svn_fs_t *fs)
{
  svn_string_t *unparsed_id = svn_fs_fs__id_unparse (id, fs->pool);
  return svn_error_createf (SVN_ERR_FS_CORRUPT, 0,
                            fmt, unparsed_id->data, fs->path);
}


svn_error_t *
svn_fs_fs__err_corrupt_node_revision (svn_fs_t *fs, const svn_fs_id_t *id)
{
  return
    corrupt_id (_("Corrupt node revision for node '%s' in filesystem '%s'"),
                id, fs);
}


svn_error_t *
svn_fs_fs__err_corrupt_fs_revision (svn_fs_t *fs, svn_revnum_t rev)
{
  return svn_error_createf
    (SVN_ERR_FS_CORRUPT, 0,
     _("Corrupt filesystem revision %ld in filesystem '%s'"),
     rev, fs->path);
}


svn_error_t *
svn_fs_fs__err_corrupt_clone (svn_fs_t *fs,
                              const char *svn_txn,
                              const char *base_path)
{
  return
    svn_error_createf
    (SVN_ERR_FS_CORRUPT, 0,
     _("Corrupt clone record for '%s' in transaction '%s' in filesystem '%s'"),
     base_path, svn_txn, fs->path);
}


svn_error_t *
svn_fs_fs__err_corrupt_id (svn_fs_t *fs, const svn_fs_id_t *id)
{
  return
    corrupt_id (_("Corrupt node revision id '%s' appears in filesystem '%s'"),
                id, fs);
}


svn_error_t *
svn_fs_fs__err_dangling_id (svn_fs_t *fs, const svn_fs_id_t *id)
{
  svn_string_t *id_str = svn_fs_fs__id_unparse (id, fs->pool);
  return svn_error_createf
    (SVN_ERR_FS_ID_NOT_FOUND, 0,
     _("Reference to non-existent node '%s' in filesystem '%s'"),
     id_str->data, fs->path);
}


svn_error_t *
svn_fs_fs__err_dangling_rev (svn_fs_t *fs, svn_revnum_t rev)
{
  return svn_error_createf
    (SVN_ERR_FS_NO_SUCH_REVISION, 0,
     _("Reference to non-existent revision %ld in filesystem '%s'"),
     rev, fs->path);
}


svn_error_t *
svn_fs_fs__err_corrupt_nodes_key (svn_fs_t *fs)
{
  return
    svn_error_createf
    (SVN_ERR_FS_CORRUPT, 0,
     _("Malformed ID as key in 'nodes' table of filesystem '%s'"), fs->path);
}


svn_error_t *
svn_fs_fs__err_corrupt_next_id (svn_fs_t *fs, const char *table)
{
  return
    svn_error_createf
    (SVN_ERR_FS_CORRUPT, 0,
     _("Corrupt value for 'next-id' key in '%s' table of filesystem '%s'"), 
     table, fs->path);
}


svn_error_t *
svn_fs_fs__err_corrupt_txn (svn_fs_t *fs,
                            const char *txn)
{
  return
    svn_error_createf
    (SVN_ERR_FS_CORRUPT, 0,
     _("Corrupt entry in 'transactions' table for '%s'"
       " in filesystem '%s'"), txn, fs->path);
}


svn_error_t *
svn_fs_fs__err_corrupt_copy (svn_fs_t *fs, const char *copy_id)
{
  return
    svn_error_createf
    (SVN_ERR_FS_CORRUPT, 0,
     _("Corrupt entry in 'copies' table for '%s' in filesystem '%s'"), 
     copy_id, fs->path);
}


svn_error_t *
svn_fs_fs__err_corrupt_lockfile (svn_fs_t *fs, const char *path)
{
  return
    svn_error_createf
    (SVN_ERR_FS_CORRUPT, 0,
     _("Corrupt lockfile for path '%s' in filesystem '%s'"), 
     path, fs->path);
}


svn_error_t *
svn_fs_fs__err_lock_expired (svn_fs_t *fs, const char *token)
{
  return
    svn_error_createf
    (SVN_ERR_FS_LOCK_EXPIRED, 0,
     _("Lock has expired:  lock-token '%s' in filesystem '%s'"),
     token, fs->path);
}


svn_error_t *
svn_fs_fs__err_not_mutable (svn_fs_t *fs, svn_revnum_t rev, const char *path)
{
  return
    svn_error_createf
    (SVN_ERR_FS_NOT_MUTABLE, 0,
     _("File is not mutable: filesystem '%s', revision %ld, path '%s'"),
     fs->path, rev, path);
}


svn_error_t *
svn_fs_fs__err_path_syntax (svn_fs_t *fs, const char *path)
{
  return
    svn_error_createf
    (SVN_ERR_FS_PATH_SYNTAX, 0,
     _("Search for malformed path '%s' in filesystem '%s'"),
     path, fs->path);
}


svn_error_t *
svn_fs_fs__err_no_such_txn (svn_fs_t *fs, const char *txn)
{
  return
    svn_error_createf
    (SVN_ERR_FS_NO_SUCH_TRANSACTION, 0,
     _("No transaction named '%s' in filesystem '%s'"),
     txn, fs->path);
}


svn_error_t *
svn_fs_fs__err_txn_not_mutable (svn_fs_t *fs, const char *txn)
{
  return
    svn_error_createf
    (SVN_ERR_FS_TRANSACTION_NOT_MUTABLE, 0,
     _("Cannot modify transaction named '%s' in filesystem '%s'"),
     txn, fs->path);
}


svn_error_t *
svn_fs_fs__err_no_such_copy (svn_fs_t *fs, const char *copy_id)
{
  return
    svn_error_createf
    (SVN_ERR_FS_NO_SUCH_COPY, 0,
     _("No copy with id '%s' in filesystem '%s'"), copy_id, fs->path);
}


svn_error_t *
svn_fs_fs__err_no_such_lock (svn_fs_t *fs, const char *path)
{
  return
    svn_error_createf
    (SVN_ERR_FS_NO_SUCH_LOCK, 0,
     _("No lock on path '%s' in filesystem '%s'"),
     path, fs->path);
}


svn_error_t *
svn_fs_fs__err_no_user (svn_fs_t *fs)
{
  return
    svn_error_createf
    (SVN_ERR_FS_NO_USER, 0,
     _("No username is currently associated with filesystem '%s'"),
     fs->path);
}


svn_error_t *
svn_fs_fs__err_not_directory (svn_fs_t *fs, const char *path)
{
  return
    svn_error_createf
    (SVN_ERR_FS_NOT_DIRECTORY, 0,
     _("'%s' is not a directory in filesystem '%s'"),
     path, fs->path);
}


svn_error_t *
svn_fs_fs__err_bad_lock_token (svn_fs_t *fs, const char *lock_token)
{
  return
    svn_error_createf
    (SVN_ERR_FS_BAD_LOCK_TOKEN, 0,
     _("Token '%s' does not point to any existing lock in filesystem '%s'"),
     lock_token, fs->path);
}


svn_error_t *
svn_fs_fs__err_not_file (svn_fs_t *fs, const char *path)
{
  return
    svn_error_createf
    (SVN_ERR_FS_NOT_FILE, 0,
     _("'%s' is not a file in filesystem '%s'"),
     path, fs->path);
}


svn_error_t *
svn_fs_fs__err_path_already_locked (svn_fs_t *fs, svn_lock_t *lock)
{
  return
    svn_error_createf
    (SVN_ERR_FS_PATH_ALREADY_LOCKED, 0,
     _("Path '%s' is already locked by user '%s' in filesystem '%s'"),
     lock->path, lock->owner, fs->path);
}


svn_error_t *
svn_fs_fs__err_lock_owner_mismatch (svn_fs_t *fs,
                                    const char *username,
                                    const char *lock_owner)
{
  return
    svn_error_createf
    (SVN_ERR_FS_LOCK_OWNER_MISMATCH, 0,
     _("User '%s' is trying to use a lock owned by '%s' in filesystem '%s'"),
     username, lock_owner, fs->path);
}