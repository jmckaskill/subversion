/*
 * wc.h :  shared stuff internal to the svn_wc library.
 *
 * ================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. The end-user documentation included with the redistribution, if
 * any, must include the following acknowlegement: "This product includes
 * software developed by CollabNet (http://www.Collab.Net)."
 * Alternately, this acknowlegement may appear in the software itself, if
 * and wherever such third-party acknowlegements normally appear.
 * 
 * 4. The hosted project names must not be used to endorse or promote
 * products derived from this software without prior written
 * permission. For written permission, please contact info@collab.net.
 * 
 * 5. Products derived from this software may not use the "Tigris" name
 * nor may "Tigris" appear in their names without prior written
 * permission of CollabNet.
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL COLLABNET OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by many
 * individuals on behalf of CollabNet.
 */


#include <apr_pools.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_xml.h"
#include "svn_wc.h"



/*** Asking questions about a working copy. ***/

/* Return an error unless PATH is a valid working copy.
   kff todo: make it compare repository too. */
svn_error_t *svn_wc__check_wc (svn_string_t *path, apr_pool_t *pool);


/* Set *APR_TIME to the later of PATH's (a regular file) mtime or ctime.
 *
 * Unix traditionally distinguishes between "mod time", which is when
 * someone last modified the contents of the file, and "change time",
 * when someone changed something else about the file (such as
 * permissions).
 *
 * Since Subversion versions both kinds of information, our timestamp
 * comparisons have to notice either kind of change.  That's why this
 * function gives the time of whichever kind came later.  APR will
 * hopefully make sure that both ctime and mtime always have useful
 * values, even on OS's that do things differently. (?)
 */
svn_error_t *svn_wc__file_affected_time (apr_time_t *apr_time,
                                         svn_string_t *path,
                                         apr_pool_t *pool);


/* Set *SAME to non-zero if file1 and file2 have the same contents,
   else set it to zero. */
svn_error_t *svn_wc__files_contents_same_p (svn_boolean_t *same,
                                            svn_string_t *file1,
                                            svn_string_t *file2,
                                            apr_pool_t *pool);


/* Set *MODIFIED_P to non-zero if FILENAME has been locally modified,
   else set to zero. */
svn_error_t *svn_wc__file_modified_p (svn_boolean_t *modified_p,
                                      svn_string_t *filename,
                                      apr_pool_t *pool);


/*** Locking. ***/

/* Lock the working copy administrative area.
   Wait for WAIT seconds if encounter another lock, trying again every
   second, then return 0 if success or an SVN_ERR_WC_LOCKED error if
   failed to obtain the lock. */
svn_error_t *svn_wc__lock (svn_string_t *path, int wait, apr_pool_t *pool);

/* Unlock PATH, or error if can't. */
svn_error_t *svn_wc__unlock (svn_string_t *path, apr_pool_t *pool);

/* Set *LOCKED to non-zero if PATH is locked, else set it to zero. */
svn_error_t *svn_wc__locked (svn_boolean_t *locked, 
                             svn_string_t *path,
                             apr_pool_t *pool);


/*** Names and file/dir operations in the administrative area. ***/

/* Create DIR as a working copy directory. */
svn_error_t *svn_wc__set_up_new_dir (svn_string_t *path,
                                     svn_string_t *ancestor_path,
                                     svn_vernum_t ancestor_vernum,
                                     apr_pool_t *pool);


/* kff todo: namespace-protecting these #defines so we never have to
   worry about them conflicting with future all-caps symbols that may
   be defined in svn_wc.h. */

/** The files within the administrative subdir. **/
#define SVN_WC__ADM_FORMAT              "format"
#define SVN_WC__ADM_README              "README"
#define SVN_WC__ADM_REPOSITORY          "repository"
#define SVN_WC__ADM_ENTRIES             "entries"
#define SVN_WC__ADM_PROPERTIES          "properties"
#define SVN_WC__ADM_LOCK                "lock"
#define SVN_WC__ADM_TMP                 "tmp"
#define SVN_WC__ADM_TEXT_BASE           "text-base"
#define SVN_WC__ADM_PROP_BASE           "prop-base"
#define SVN_WC__ADM_DPROP_BASE          "dprop-base"
#define SVN_WC__ADM_LOG                 "log"

/* Return a string containing the admin subdir name. */
svn_string_t *svn_wc__adm_subdir (apr_pool_t *pool);


/* Return a path to something in PATH's administrative area.
 * Return path to the thing in the tmp area if TMP is non-zero.
 * Varargs are (const char *)'s, the final one must be NULL.
 */
svn_string_t * svn_wc__adm_path (svn_string_t *path,
                                 svn_boolean_t tmp,
                                 apr_pool_t *pool,
                                 ...);


/* Make `PATH/<adminstrative_subdir>/THING'. */
svn_error_t *svn_wc__make_adm_thing (svn_string_t *path,
                                     const char *thing,
                                     int type,
                                     svn_boolean_t tmp,
                                     apr_pool_t *pool);



/*** Opening all kinds of adm files ***/

/* Yo, read this if you open and close files in the adm area:
 *
 * When you open a file for writing with svn_wc__open_foo(), the file
 * is actually opened in the corresponding location in the tmp/
 * directory (and if you're appending as well, then the tmp file
 * starts out as a copy of the original file). 
 *
 * Somehow, this tmp file must eventually get renamed to its real
 * destination in the adm area.  You can do it either by passing the
 * SYNC flag to svn_wc__close_foo(), or by calling
 * svn_wc__sync_foo() (though of course you should still have
 * called svn_wc__close_foo() first, just without the SYNC flag).
 *
 * In other words, the adm area is only capable of modifying files
 * atomically, but you get some control over when the rename happens.
 */

/* Open `PATH/<adminstrative_subdir>/FNAME'. */
svn_error_t *svn_wc__open_adm_file (apr_file_t **handle,
                                    svn_string_t *path,
                                    const char *fname,
                                    apr_int32_t flags,
                                    apr_pool_t *pool);


/* Close `PATH/<adminstrative_subdir>/FNAME'. */
svn_error_t *svn_wc__close_adm_file (apr_file_t *fp,
                                     svn_string_t *path,
                                     const char *fname,
                                     int sync,
                                     apr_pool_t *pool);

/* Remove `PATH/<adminstrative_subdir>/THING'. */
svn_error_t *svn_wc__remove_adm_file (svn_string_t *path,
                                      apr_pool_t *pool,
                                      ...);

/* Open the text-base for FILE.
 * FILE can be any kind of path ending with a filename.
 * Behaves like svn_wc__open_adm_file(), which see.
 */
svn_error_t *svn_wc__open_text_base (apr_file_t **handle,
                                     svn_string_t *file,
                                     apr_int32_t flags,
                                     apr_pool_t *pool);

/* Close the text-base for FILE.
 * FP was obtained from svn_wc__open_text_base().
 * Behaves like svn_wc__close_adm_file(), which see.
 */
svn_error_t *svn_wc__close_text_base (apr_file_t *fp,
                                      svn_string_t *file,
                                      int sync,
                                      apr_pool_t *pool);


/* Atomically rename a temporary text-base file to its canonical
   location.  The tmp file should be closed already. */
svn_error_t *
svn_wc__sync_text_base (svn_string_t *path, apr_pool_t *pool);


/* Return a path to PATH's text-base file.
   If TMP is set, return a path to the tmp text-base file. */
svn_string_t *svn_wc__text_base_path (svn_string_t *path,
                                      svn_boolean_t tmp,
                                      apr_pool_t *pool);


/* Ensure that PATH is a locked working copy directory.
 *
 * (In practice, this means creating an adm area if none exists, in
 * which case it is locked from birth, or else locking an adm area
 * that's already there.)
 * 
 * REPOSITORY is a repository string for initializing the adm area.
 *
 * VERSION is the version for this directory.  kff todo: ancestor_path?
 */
svn_error_t *svn_wc__ensure_wc (svn_string_t *path,
                                svn_string_t *repository,
                                svn_string_t *ancestor_path,
                                svn_vernum_t ancestor_version,
                                apr_pool_t *pool);


/* Ensure that an administrative area exists for PATH, so that PATH is
 * a working copy subdir.
 *
 * Use REPOSITORY for the wc's repository.
 *
 * Does not ensure existence of PATH itself; if PATH does not exist,
 * an error will result. 
 */
svn_error_t *svn_wc__ensure_adm (svn_string_t *path,
                                 svn_string_t *repository,
                                 svn_string_t *ancestor_path,
                                 svn_vernum_t ancestor_version,
                                 apr_pool_t *pool);


/*** The log file. ***/

/* Note: every entry in the logfile is either idempotent or atomic.
 * This allows us to remove the entire logfile when every entry in it
 * has been completed -- if you crash in the middle of running a
 * logfile, and then later are running over it again as part of the
 * recovery, a given entry is "safe" in the sense that you can either
 * tell it has already been done (in which case, ignore it) or you can
 * do it again without ill effect.
 */

/** Log actions. **/

/* Merge the mods saved in SVN_WC__LOG_ATTR_SAVED_MODS into the
   working file SVN_WC__LOG_ATTR_NAME. */
#define SVN_WC__LOG_MERGE_TEXT          "merge-text"

/* Copy SVN/tmp/text-base/SVN_WC__LOG_ATTR_NAME to
   SVN/text-base/SVN_WC__LOG_ATTR_NAME. */
#define SVN_WC__LOG_REPLACE_TEXT_BASE   "replace-text-base"

/* Merge property changes for SVN_WC__LOG_ATTR_NAME.  todo: not yet
   done. */
#define SVN_WC__LOG_MERGE_PROPS         "merge-props"

/* Merge property changes for SVN_WC__LOG_ATTR_NAME.  todo: not yet
   done. */
#define SVN_WC__LOG_REPLACE_PROP_BASE   "replace-prop-base"

/* Bump SVN_WC__LOG_ATTR_NAME's version to SVN_WC__LOG_ATTR_VERSION */
#define SVN_WC__LOG_SET_VERSION         "set-entry"

/* A commit completed successfully, so:  
 *   if SVN/tmp/text-base/SVN_WC__LOG_ATTR_NAME exists, then
 *      compare SVN/tmp/text-base/SVN_WC__LOG_ATTR_NAME with working file
 *         if they're the same, use working file's timestamp
 *         else use SVN/tmp/text-base/SVN_WC__LOG_ATTR_NAME's timestamp
 *      set SVN_WC__LOG_ATTR_NAME's version to N
 */
#define SVN_WC__LOG_COMMITTED           "committed"

/** Log attributes. **/
#define SVN_WC__LOG_ATTR_NAME           "name"
#define SVN_WC__LOG_ATTR_VERSION        "version"
#define SVN_WC__LOG_ATTR_SAVED_MODS     "saved-mods"

/* Starting at PATH, write out log entries indicating that a commit
 * succeeded, using VERSION as the new version number.  run_log will
 * use these log items to complete the commit. 
 * 
 * Targets is a hash of files/dirs that actually got committed --
 * these are the only ones who we can write log items for, and whose
 * version numbers will get set.  todo: eventually this hash will be
 * of the sort used by svn_wc__compose_paths(), as with all entries
 * recursers.
 */
svn_error_t *svn_wc__log_commit (svn_string_t *path,
                                 apr_hash_t *targets,
                                 svn_vernum_t version,
                                 apr_pool_t *pool);


/* Recurse from path, cleaning up unfinished log business. 
 * In each directory, starting from PATH, do the following:
 *
 *   1. If TARGETS is non-null but nothing in it indicates that this
 *      directory is relevant, then return immediately (if this dir or
 *      a file child of it appears in TARGETS, then this dir is
 *      relevant).  Else if TARGETS is null, then proceed to 2.
 *
 *   2. If the dir is locked, error out if BAIL_ON_LOCK is set.
 *      Otherwise, proceed to step 3.
 * 
 *   3. If there is a log, run each item in the log, in order.  When
 *      done, rm the log.
 *
 *   4. Clean out any remaining regular files in SVN/tmp/.
 *      And if BAIL_ON_LOCK is not set, remove any lock file as well.
 *
 * todo: this, along with all other recursers, will want to use the
 * svn_wc__compose_paths() convention for TARGETS eventually. 
 */
svn_error_t *svn_wc__cleanup (svn_string_t *path,
                              apr_hash_t *targets,
                              svn_boolean_t bail_on_lock,
                              apr_pool_t *pool);

/* Process the instructions in the log file for PATH. */
svn_error_t *svn_wc__run_log (svn_string_t *path, apr_pool_t *pool);



/*** Handling the `entries' file. ***/

#define SVN_WC__ENTRIES_TOPLEVEL       "wc-entries"
#define SVN_WC__ENTRIES_ENTRY          "entry"
#define SVN_WC__ENTRIES_ATTR_NAME      "name"
#define SVN_WC__ENTRIES_ATTR_VERSION   "version"
#define SVN_WC__ENTRIES_ATTR_KIND      "kind"
#define SVN_WC__ENTRIES_ATTR_TIMESTAMP "timestamp"
#define SVN_WC__ENTRIES_ATTR_CHECKSUM  "checksum"
#define SVN_WC__ENTRIES_ATTR_ADD       "add"
#define SVN_WC__ENTRIES_ATTR_DELETE    "delete"
#define SVN_WC__ENTRIES_ATTR_ANCESTOR  "ancestor"

/* How an entries file's owner dir is named in the entries file. */
#define SVN_WC__ENTRIES_THIS_DIR       ""

/* Initialize contents of `entries' for a new adm area. */
svn_error_t *svn_wc__entries_init (svn_string_t *path,
                                   svn_string_t *ancestor_path,
                                   apr_pool_t *pool);


/* A data structure representing an entry from the `entries' file. */
typedef struct svn_wc__entry_t
{
  /* Note that the entry's name is not stored here, because it is the
     hash key for which this is the value. */

  svn_vernum_t version;        /* Base version.  (Required) */
  svn_string_t *ancestor;      /* Base path.  (Required) */
  enum svn_node_kind kind;     /* Is it a file, a dir, or... ? (Required) */

  int flags;                   /* Is entry marked for addition, deletion? */

  apr_time_t timestamp;        /* When the entries file thinks the
                                  local working file last changed.
                                  (NULL means not available) */ 

  apr_hash_t *attributes;      /* All XML attributes, both those
                                  duplicated above and any others.
                                  (Required) */
} svn_wc__entry_t;


/* (Bitmasks). */
#define SVN_WC__ENTRY_ADD     1
#define SVN_WC__ENTRY_DELETE  2


/* Parse the `entries' file for PATH and return a hash ENTRIES, whose
   keys are entry names and values are (svn_wc__entry_t *). */
svn_error_t *svn_wc__entries_read (apr_hash_t **entries,
                                   svn_string_t *path,
                                   apr_pool_t *pool);

/* Create or overwrite an `entries' file for PATH using the contents
   of ENTRIES. */
svn_error_t *svn_wc__entries_write (apr_hash_t *entries,
                                    svn_string_t *path,
                                    apr_pool_t *pool);


/* Create a new entry NAME in ENTRIES with appropriate fields.
   Varargs specify any other xml attributes, as alternating pairs of
   key (char *), value (svn_string_t *).  

   An entry name of null means the dir's own entry, as usual.
   
   An error SVN_ERR_WC_ENTRY_EXISTS is returned if the entry already
   exists. */
svn_error_t *svn_wc__entry_add (apr_hash_t *entries,
                                svn_string_t *name,
                                svn_vernum_t version,
                                enum svn_node_kind kind,
                                int flags,
                                apr_time_t timestamp,
                                apr_pool_t *pool,
                                ...);

/* For PATH's entries file, create or modify an entry NAME, using the
   explicit fields and, secondarily, varargs.
   Varargs are alternating pairs of key (char *), value (svn_string_t *).

   An entry name of null means the dir's own entry, as usual.
   
   The entries file will be read, tweaked, and written back out.  This
   is your one-stop shopping for changing the entries file. */
svn_error_t *svn_wc__entry_merge_sync (svn_string_t *path,
                                       svn_string_t *name,
                                       svn_vernum_t version,
                                       enum svn_node_kind kind,
                                       int flags,
                                       apr_time_t timestamp,
                                       apr_pool_t *pool,
                                       ...);


/* Remove entry NAME from ENTRIES, unconditionally. */
void svn_wc__entry_remove (apr_hash_t *entries, svn_string_t *name);


/*** General utilities that may get moved upstairs at some point. */

/* Ensure that DIR exists. */
svn_error_t *svn_wc__ensure_directory (svn_string_t *path, apr_pool_t *pool);


/* Convert TIME to an svn string representation, which can be
   converted back by svn_wc__string_to_time(). */
svn_string_t *svn_wc__time_to_string (apr_time_t time, apr_pool_t *pool);


/* Convert TIMESTR to an apr_time_t.  TIMESTR should be of the form
   returned by svn_wc__time_to_string(). */
apr_time_t svn_wc__string_to_time (svn_string_t *timestr);



/* Copy SRC to DST.  DST will be overwritten if it exists. */
svn_error_t *svn_wc__copy_file (svn_string_t *src,
                                svn_string_t *dst,
                                apr_pool_t *pool);



/*** Diffing and merging ***/

/* Nota bene: here, diffing and merging is about discovering local changes
 * to a file and merging them back into an updated version of that
 * file, not about txdeltas.
 */

/* Get local changes to a working copy file.
 *
 * DIFF_FN stores its results in *RESULT, which will later be passed
 * to a matching patch function.  (Note that DIFF_FN will be invoked
 * on two filenames, a source and a target). 
 */
svn_error_t *svn_wc__get_local_changes (svn_wc_diff_fn_t *diff_fn,
                                        void **result,
                                        svn_string_t *path,
                                        apr_pool_t *pool);


/* An implementation of the `svn_wc_diff_fn_t' interface.
 * Store the diff between SRC and TARGET in *RESULT.  (What gets
 * stored isn't necessarily the actual diff data -- it might be the
 * name of a tmp file containing the data, for example.)
 */
svn_error_t *svn_wc__gnudiff_differ (void **result,
                                     svn_string_t *src,
                                     svn_string_t *target,
                                     apr_pool_t *pool);


/* Re-apply local changes to a working copy file that may have been
 * updated.
 *
 * PATCH_FN is a function, such as svn_wc__gnudiff_patcher, that can
 * use CHANGES to patch the file PATH (note that PATCH_FN will be
 * invoked on two filenames, a source and a target).
 */
svn_error_t *svn_wc__merge_local_changes (svn_wc_patch_fn_t *patch_fn,
                                          void *changes,
                                          svn_string_t *path,
                                          apr_pool_t *pool);



/* An implementation of the `svn_wc_patch_fn_t' interface.
 * Patch SRC with DIFF to yield TARGET.
 */
svn_error_t *svn_wc__gnudiff_patcher (void *diff,
                                      svn_string_t *src,
                                      svn_string_t *target,
                                      apr_pool_t *pool);



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */

