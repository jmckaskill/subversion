/* reps-strings.h : interpreting representations with respect to strings
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

#ifndef SVN_LIBSVN_FS_REPS_STRINGS_H
#define SVN_LIBSVN_FS_REPS_STRINGS_H

#define APU_WANT_DB
#include <apu_want.h>

#include "svn_io.h"
#include "svn_fs.h"

#include "trail.h"


#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */



/* Get or create a mutable representation in FS, store the new rep's
   key in *NEW_REP_KEY.

   TXN_ID is the id of the Subversion transaction under which this occurs.

   If REP_KEY is already a mutable representation, set *NEW_REP_KEY to
   REP_KEY, else set *NEW_REP_KEY to a brand new rep key allocated in
   TRAIL->pool. */
svn_error_t *svn_fs__get_mutable_rep (const char **new_rep_key,
                                      const char *rep_key,
                                      svn_fs_t *fs, 
                                      const char *txn_id,
                                      trail_t *trail);


/* Delete REP_KEY from FS if REP_KEY is mutable, as part of trail, or
   do nothing if REP_KEY is immutable.  If a mutable rep is deleted,
   the string it refers to is deleted as well.  TXN_ID is the id of
   the Subversion transaction under which this occurs.

   If no such rep, return SVN_ERR_FS_NO_SUCH_REPRESENTATION.  */ 
svn_error_t *svn_fs__delete_rep_if_mutable (svn_fs_t *fs,
                                            const char *rep_key,
                                            const char *txn_id,
                                            trail_t *trail);




/*** Reading and writing rep contents. ***/

/* Set *SIZE_P to the size of REP_KEY's contents in FS, as part of TRAIL.
   Note: this is the fulltext size, no matter how the contents are
   represented in storage.  */
svn_error_t *svn_fs__rep_contents_size (svn_filesize_t *size_p,
                                        svn_fs_t *fs,
                                        const char *rep_key,
                                        trail_t *trail);


/* Put into DIGEST the MD5 checksum for REP_KEY in FS, as part of TRAIL.
   This is the prerecorded checksum for the rep's contents' fulltext.
   If no checksum is available, do not calculate one dynamically, just
   put all 0's into DIGEST.  (By convention, the all-zero checksum is
   considered to match any checksum.) */
svn_error_t *svn_fs__rep_contents_checksum (unsigned char digest[],
                                            svn_fs_t *fs,
                                            const char *rep_key,
                                            trail_t *trail);


/* Set STR->data to the contents of REP_KEY in FS, and STR->len to the
   contents' length, as part of TRAIL.  The data is allocated in
   TRAIL->pool.  If an error occurs, the effect on STR->data and
   STR->len is undefined.

   Note: this is the fulltext contents, no matter how the contents are
   represented in storage.  */
svn_error_t *svn_fs__rep_contents (svn_string_t *str,
                                   svn_fs_t *fs,
                                   const char *rep_key,
                                   trail_t *trail);


/* Set *RS_P to a stream to read the contents of REP_KEY in FS.
   Allocate the stream in POOL.

   REP_KEY may be null, in which case reads just return 0 bytes.

   If USE_TRAIL_FOR_READS is TRUE, the stream's reads are part
   of TRAIL; otherwise, each read happens in an internal, one-off
   trail (though TRAIL is still required).  POOL may be TRAIL->pool. */
svn_error_t *
svn_fs__rep_contents_read_stream (svn_stream_t **rs_p,
                                  svn_fs_t *fs,
                                  const char *rep_key,
                                  svn_boolean_t use_trail_for_reads,
                                  trail_t *trail,
                                  apr_pool_t *pool);

                                       
/* Set *WS_P to a stream to write the contents of REP_KEY.  Allocate
   the stream in POOL.  TXN_ID is the id of the Subversion transaction
   under which this occurs.

   If USE_TRAIL_FOR_WRITES is TRUE, the stream's writes are part
   of TRAIL; otherwise, each write happens in an internal, one-off
   trail (though TRAIL is still required).  POOL may be TRAIL->pool.

   If REP_KEY is not mutable, writes to *WS_P will return the
   error SVN_ERR_FS_REP_NOT_MUTABLE.  */
svn_error_t *
svn_fs__rep_contents_write_stream (svn_stream_t **ws_p,
                                   svn_fs_t *fs,
                                   const char *rep_key,
                                   const char *txn_id,
                                   svn_boolean_t use_trail_for_writes,
                                   trail_t *trail,
                                   apr_pool_t *pool);



/*** Deltified storage. ***/

/* Offer TARGET the chance to store its contents as a delta against
   SOURCE, in FS, as part of TRAIL.  TARGET and SOURCE are both
   representation keys.

   This usually results in TARGET's data being stored as a diff
   against SOURCE; but it might not, if it turns out to be more
   efficient to store the contents some other way.  */
svn_error_t *svn_fs__rep_deltify (svn_fs_t *fs,
                                  const char *target,
                                  const char *source,
                                  trail_t *trail);


/* Ensure that REP_KEY refers to storage that is maintained as fulltext,
   not as a delta against other strings, in FS, as part of TRAIL.  */
svn_error_t *svn_fs__rep_undeltify (svn_fs_t *fs,
                                    const char *rep_key,
                                    trail_t *trail);



#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_FS_REPS_STRINGS_H */
