/*  svn_path.h: a path manipulation library
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
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


#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef SVN_PATH_H
#define SVN_PATH_H



#include <apr_pools.h>
#include <apr_tables.h>
#include "svn_string.h"
#include "svn_error.h"


/*** Notes:
 * 
 * No result path ever ends with a separator, no matter whether the
 * path is a file or directory, because we always canonicalize() it.
 *
 * todo: this library really needs a test suite!
 ***/

enum svn_path_style {
  svn_path_local_style = 1,  /* parse path using local (client) conventions */
  svn_path_repos_style,      /* parse path using repository conventions */
  svn_path_url_style         /* parse path using URL conventions */
};

/* Add a COMPONENT (a null-terminated C-string) to PATH.  COMPONENT is
   allowed to contain directory separators.

   If PATH is non-empty, append the appropriate directory separator
   character, and then COMPONENT.  If PATH is empty, simply set it to
   COMPONENT; don't add any separator character.

   If the result ends in a separator character, then remove the separator.

   The separator character is chosen according to STYLE.  For
   svn_path_repos_style, it would be '/'.  For svn_path_local_style on
   a Unix system, it would also be '/'.  */
void svn_path_add_component (svn_stringbuf_t *path,
                             const svn_stringbuf_t *component,
                             enum svn_path_style style);

/* Same as `svn_path_add_component', except that the COMPONENT argument is 
   a C-style '\0'-terminated string, not an svn_stringbuf_t.  */
void svn_path_add_component_nts (svn_stringbuf_t *path, 
                                 const char *component,
                                 enum svn_path_style style);

/* Remove one component off the end of PATH. */
void svn_path_remove_component (svn_stringbuf_t *path,
                                enum svn_path_style style);


/* Duplicate and return PATH's last component, w/o separator. 
 *
 * If PATH is the root directory, then its last component is still the
 * root directory.  Else if PATH ends with a separator, then PATH's
 * last component is the empty string.
 */
svn_stringbuf_t *svn_path_last_component (const svn_stringbuf_t *path,
                                       enum svn_path_style style,
                                       apr_pool_t *pool);


/* Return TRUE iff PATH is a single component. */
svn_boolean_t
svn_path_is_single_path_component (svn_stringbuf_t *path,
                                   enum svn_path_style style);

/* Divide PATH into *DIRPATH and *BASENAME, return them by reference,
 * in their own storage in POOL.
 *
 * If DIRPATH or BASENAME is null, then that one will not be used.
 *
 * DIRPATH or BASENAME may be PATH's own address, but they may not
 * both be PATH's address; in fact, in general they must not be the
 * same, or the results are undefined.
 *
 * The separator between DIRPATH and BASENAME is not included in
 * either of the new names.
 */
void svn_path_split (const svn_stringbuf_t *path,
                     svn_stringbuf_t **dirpath,
                     svn_stringbuf_t **basename,
                     enum svn_path_style style,
                     apr_pool_t *pool);


/* Return non-zero iff PATH represents the current directory */
int svn_path_is_thisdir (const svn_stringbuf_t *path, enum svn_path_style style);

/* Return non-zero iff PATH is empty or represents the current
   directory -- that is, if it is NULL or if prepending it as a
   component to an existing path would result in no meaningful
   change. */
int svn_path_is_empty (const svn_stringbuf_t *path, enum svn_path_style style);


/* Remove trailing separators that don't affect the meaning of the path.
   (At some future point, this may make other semantically inoperative
   transformations.) */
void svn_path_canonicalize (svn_stringbuf_t *path,
                            enum svn_path_style style);


/* Return an integer greater than, equal to, or less than 0, according
   as PATH1 is greater than, equal to, or less than PATH2. */
int svn_path_compare_paths (const svn_stringbuf_t *path1,
                            const svn_stringbuf_t *path2,
                            enum svn_path_style style);


/* Return the longest common path shared by both PATH1 and PATH2.  If
   there's no common ancestor, return NULL.  */
svn_stringbuf_t *svn_path_get_longest_ancestor (const svn_stringbuf_t *path1,
                                             const svn_stringbuf_t *path2,
                                             enum svn_path_style style,
                                             apr_pool_t *pool);

/* Convert RELATIVE path to an absolute path and return the results in
   *PABSOLUTE. */
svn_error_t *
svn_path_get_absolute(svn_stringbuf_t **pabsolute,
                      const svn_stringbuf_t *relative,
                      apr_pool_t *pool);

/* Return the path part of PATH in *PDIRECTORY, and the file part in *PFILE.
   If PATH is a directory, it will be returned through PDIRECTORY, and *PFILE
   will be the empty string (not NULL). */
svn_error_t *
svn_path_split_if_file(svn_stringbuf_t *path,
                       svn_stringbuf_t **pdirectory, 
                       svn_stringbuf_t **pfile,
                       apr_pool_t *pool);

/* Find the common prefix of the paths in TARGETS, and remove redundancies.
 *
 * The elements in TARGETS must be existing files or directories, in
 * local path style.
 *
 * If there are multiple targets, or exactly one target and it's not a
 * directory, then 
 *
 *   - *PBASENAME is set to the absolute path of the common parent
 *     directory of all of those targets, and
 *
 *   - If PCONDENSED_TARGETS is non-null, *PCONDENSED_TARGETS is set
 *     to an array of targets relative to *PBASENAME, with
 *     redundancies removed (meaning that none of these targets will
 *     be the same as, nor have an ancestor/descendant relationship
 *     with, any of the other targets; nor will any of them be the
 *     same as *PBASENAME).  Else if PCONDENSED_TARGETS is null, it is
 *     left untouched.
 *
 * Else if there is exactly one directory target, then
 *
 *   - *PBASENAME is set to that directory, and
 *
 *   - If PCONDENSED_TARGETS is non-null, *PCONDENSED_TARGETS is set
 *     to an array containing zero elements.  Else if
 *     PCONDENSED_TARGETS is null, it is left untouched.
 *
 * If there are no items in TARGETS, *PBASENAME and (if applicable)
 * *PCONDENSED_TARGETS will be NULL.
 *
 * NOTE: There is no guarantee that *PBASENAME is within a working
 * copy.
 */
svn_error_t *
svn_path_condense_targets (svn_stringbuf_t **pbasename,
                           apr_array_header_t **pcondensed_targets,
                           const apr_array_header_t *targets,
                           enum svn_path_style style,
                           apr_pool_t *pool);


/* Copy a list of TARGETS, one at a time, into PCONDENSED_TARGETS,
   omitting any targets that are found earlier in the list, or whose
   ancestor is found earlier in the list.  Ordering of targets in the
   original list is preserved in the condensed list of targets.  Use
   POOL for any allocations.  

   How does this differ in functionality from svn_path_condense_targets?

   Here's the short version:
   
   1.  Disclaimer: if you wish to debate the following, talk to Karl. :-)
       Order matters for updates because a multi-arg update is not
       atomic, and CVS users are used to, when doing 'cvs up targetA
       targetB' seeing targetA get updated, then targetB.  I think the
       idea is that if you're in a time-sensitive or flaky-network
       situation, a user can say, "I really *need* to update
       wc/A/D/G/tau, but I might as well update my whole working copy if
       I can."  So that user will do 'svn up wc/A/D/G/tau wc', and if
       something dies in the middles of the 'wc' update, at least the
       user has 'tau' up-to-date.
   
   2.  Also, we have this notion of an anchor and a target for updates
       (the anchor is where the update editor is rooted, the target is
       the actual thing we want to update).  I needed a function that
       would NOT screw with my input paths so that I could tell the
       difference between someone being in A/D and saying 'svn up G' and
       being in A/D/G and saying 'svn up .' -- believe it or not, these
       two things don't mean the same thing.  svn_path_condense_targets
       plays with absolute paths (which is fine, so does
       svn_path_remove_redundancies), but the difference is that it
       actually tweaks those targets to be relative to the "grandfather
       path" common to all the targets.  Updates don't require a
       "grandfather path" at all, and even if it did, the whole
       conversion to an absolute path drops the crucial difference
       between saying "i'm in foo, update bar" and "i'm in foo/bar,
       update '.'"
*/
svn_error_t *
svn_path_remove_redundancies (apr_array_header_t **pcondensed_targets,
                              const apr_array_header_t *targets,
                              enum svn_path_style style,
                              apr_pool_t *pool);


/* Decompose PATH into an array of svn_stringbuf_t components, allocated
   in POOL.  STYLE indicates the dir separator to split the string on.
   If PATH is absolute, the first component will be a lone dir
   separator (the root directory). */
apr_array_header_t *svn_path_decompose (const svn_stringbuf_t *path,
                                        enum svn_path_style style,
                                        apr_pool_t *pool);



/* Test if PATH2 is a child of PATH1.
   If not, return NULL.
   If so, return the "remainder" path.  (The substring which, when
   appended to PATH1, yields PATH2 -- minus the dirseparator. ) */
svn_stringbuf_t * svn_path_is_child (const svn_stringbuf_t *path1,
                                  const svn_stringbuf_t *path2,
                                  enum svn_path_style style,
                                  apr_pool_t *pool);


#endif /* SVN_PATH_H */

#ifdef __cplusplus
}
#endif /* __cplusplus */


/* ----------------------------------------------------------------
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
