/* strings-table.h : internal interface to `strings' table
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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

#ifndef SVN_LIBSVN_FS_STRINGS_TABLE_H
#define SVN_LIBSVN_FS_STRINGS_TABLE_H

#include <db.h>
#include "svn_io.h"
#include "svn_fs.h"
#include "../trail.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* Special strings-table keys for commonly used strings.  */



/* This interface provides raw access to the `strings' table.  It does
   not deal with deltification, undeltification, or skels.  It just
   reads and writes strings of bytes. */


/* Read *LEN bytes into BUF from OFFSET in string KEY in FS, as part
 * of TRAIL.
 * 
 * On return, *LEN is set to the number of bytes read.  If this value
 * is less than the number requested, the end of the string has been
 * reached (no error is returned on end-of-string).
 *
 * If OFFSET is past the end of the string, then *LEN will be set to
 * zero. Callers which are advancing OFFSET as they read portions of
 * the string can terminate their loop when *LEN is returned as zero
 * (which will occur when OFFSET == length(the string)).
 * 
 * If string KEY does not exist, the error SVN_ERR_FS_NO_SUCH_STRING
 * is returned.
 */
svn_error_t *svn_fs__bdb_string_read (svn_fs_t *fs,
                                      const char *key,
                                      char *buf,
                                      apr_off_t offset,
                                      apr_size_t *len,
                                      trail_t *trail);


/* Set *SIZE to the size in bytes of string KEY in FS, as part of
 * TRAIL.
 *
 * If string KEY does not exist, return SVN_ERR_FS_NO_SUCH_STRING.
 */
svn_error_t *svn_fs__bdb_string_size (apr_size_t *size,
                                      svn_fs_t *fs,
                                      const char *key,
                                      trail_t *trail);


/* Append LEN bytes from BUF to string *KEY in FS, as part of TRAIL.
 *
 * If *KEY is null, then create a new string and store the new key in
 * *KEY (allocating it in TRAIL->pool), and write LEN bytes from BUF
 * as the initial contents of the string.
 *
 * If *KEY is not null but there is no string named *KEY, return
 * SVN_ERR_FS_NO_SUCH_STRING.
 *
 * Note: to overwrite the old contents of a string, call
 * svn_fs__string_clear() and then svn_fs__string_append().  */
svn_error_t *svn_fs__bdb_string_append (svn_fs_t *fs,
                                        const char **key,
                                        apr_size_t len,
                                        const char *buf,
                                        trail_t *trail);


/* Make string KEY in FS zero length, as part of TRAIL.
 * If the string does not exist, return SVN_ERR_FS_NO_SUCH_STRING.
 */
svn_error_t *svn_fs__bdb_string_clear (svn_fs_t *fs,
                                       const char *key,
                                       trail_t *trail);


/* Delete string KEY from FS, as part of TRAIL.
 *
 * If string KEY does not exist, return SVN_ERR_FS_NO_SUCH_STRING.
 *
 * WARNING: Deleting a string renders unusable any representations
 * that refer to it.  Be careful.
 */ 
svn_error_t *svn_fs__bdb_string_delete (svn_fs_t *fs,
                                        const char *key,
                                        trail_t *trail);


/* Copy the contents of the string referred to by KEY in FS into a new
 * record, returning the new record's key in *NEW_KEY.  All
 * allocations (including *NEW_KEY) occur in TRAIL->pool.  */
svn_error_t *svn_fs__bdb_string_copy (svn_fs_t *fs,
                                      const char **new_key,
                                      const char *key,
                                      trail_t *trail);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_STRINGS_TABLE_H */
