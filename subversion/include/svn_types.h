/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2000-2007 CollabNet.  All rights reserved.
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
 * @file svn_types.h
 * @brief Subversion's data types
 */

#ifndef SVN_TYPES_H
#define SVN_TYPES_H

/* ### this should go away, but it causes too much breakage right now */
#include <stdlib.h>

#include <apr.h>        /* for apr_size_t */
#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_tables.h>
#include <apr_time.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */



/** Subversion error object.
 *
 * Defined here, rather than in svn_error.h, to avoid a recursive @#include 
 * situation.
 */
typedef struct svn_error_t
{
  /** APR error value, possibly SVN_ custom err */
  apr_status_t apr_err;

  /** details from producer of error */
  const char *message;

  /** ptr to the error we "wrap" */
  struct svn_error_t *child;

  /** The pool holding this error and any child errors it wraps */
  apr_pool_t *pool;

  /** Source file where the error originated.  Only used iff @c SVN_DEBUG. */
  const char *file;

  /** Source line where the error originated.  Only used iff @c SVN_DEBUG. */
  long line;

} svn_error_t;



/** @defgroup APR_ARRAY_compat_macros APR Array Compatibility Helper Macros
 * These macros are provided by APR itself from version 1.3.
 * Definitions are provided here for when using older versions of APR.
 * @{
 */

/** index into an apr_array_header_t */
#ifndef APR_ARRAY_IDX
#define APR_ARRAY_IDX(ary,i,type) (((type *)(ary)->elts)[i])
#endif

/** easier array-pushing syntax */
#ifndef APR_ARRAY_PUSH
#define APR_ARRAY_PUSH(ary,type) (*((type *)apr_array_push(ary)))
#endif

/** @} */

/** The various types of nodes in the Subversion filesystem. */
typedef enum
{
  /* absent */
  svn_node_none,

  /* regular file */
  svn_node_file,

  /* directory */
  svn_node_dir,

  /* something's here, but we don't know what */
  svn_node_unknown
} svn_node_kind_t;

/** About Special Files in Subversion
 *
 * Subversion denotes files that cannot be portably created or
 * modified as "special" files (svn_node_special).  It stores these
 * files in the repository as a plain text file with the svn:special
 * property set.  The file contents contain: a platform-specific type
 * string, a space character, then any information necessary to create
 * the file on a supported platform.  For example, if a symbolic link
 * were being represented, the repository file would have the
 * following contents:
 *
 * "link /path/to/link/target"
 *
 * Where 'link' is the identifier string showing that this special
 * file should be a symbolic link and '/path/to/link/target' is the
 * destination of the symbolic link.
 *
 * Special files are stored in the text-base exactly as they are
 * stored in the repository.  The platform specific files are created
 * in the working copy at EOL/keyword translation time using
 * svn_subst_copy_and_translate2().  If the current platform does not
 * support a specific special file type, the file is copied into the
 * working copy as it is seen in the repository.  Because of this,
 * users of other platforms can still view and modify the special
 * files, even if they do not have their unique properties.
 *
 * New types of special files can be added by:
 *  1. Implementing a platform-dependent routine to create a uniquely
 *     named special file and one to read the special file in
 *     libsvn_subr/io.c.
 *  2. Creating a new textual name similar to
 *     SVN_SUBST__SPECIAL_LINK_STR in libsvn_subr/subst.c.
 *  3. Handling the translation/detranslation case for the new type in
 *     create_special_file and detranslate_special_file, using the
 *     routines from 1.
 */

/** A revision number. */
typedef long int svn_revnum_t;

/** Valid revision numbers begin at 0 */
#define SVN_IS_VALID_REVNUM(n) ((n) >= 0)

/** The 'official' invalid revision num */
#define SVN_INVALID_REVNUM ((svn_revnum_t) -1)

/** Not really invalid...just unimportant -- one day, this can be its
 * own unique value, for now, just make it the same as
 * @c SVN_INVALID_REVNUM.
 */
#define SVN_IGNORED_REVNUM ((svn_revnum_t) -1) 

/** Convert null-terminated C string @a str to a revision number. */
#define SVN_STR_TO_REV(str) ((svn_revnum_t) atol(str))

/**
 * Parse NULL-terminated C string @a str as a revision number and
 * store its value in @a rev.  If @a endptr is non-NULL, then the
 * address of the first non-numeric character in @a str is stored in
 * it.  If there are no digits in @a str, then @a endptr is set (if
 * non-NULL), and the error @c SVN_ERR_REVNUM_PARSE_FAILURE error is
 * returned.  Negative numbers parsed from @a str are considered
 * invalid, and result in the same error.
 *
 * @since New in 1.5.
 */
svn_error_t *
svn_revnum_parse(svn_revnum_t *rev,
                 const char *str,
                 const char **endptr);

/** Originally intended to be used in printf()-style functions to format
 * revision numbers.  Deprecated due to incompatibilities with language
 * translation tools (e.g. gettext).
 *
 * New code should use a bare "%ld" format specifier for formatting revision
 * numbers.
 *
 * @deprecated Provided for backward compatibility with the 1.0 API.
 */
#define SVN_REVNUM_T_FMT "ld"


/** The size of a file in the Subversion FS. */
typedef apr_int64_t svn_filesize_t;

/** The 'official' invalid file size constant. */
#define SVN_INVALID_FILESIZE ((svn_filesize_t) -1)

/** In printf()-style functions, format file sizes using this. */
#define SVN_FILESIZE_T_FMT APR_INT64_T_FMT

#ifndef DOXYGEN_SHOULD_SKIP_THIS
/* Parse a base-10 numeric string into a 64-bit unsigned numeric value. */
/* NOTE: Private. For use by Subversion's own code only. See issue #1644. */
/* FIXME: APR should supply a function to do this, such as "apr_atoui64". */
#define svn__atoui64(X) ((apr_uint64_t) apr_atoi64(X))
#endif


/** YABT:  Yet Another Boolean Type */
typedef int svn_boolean_t;

#ifndef TRUE
/** uhh... true */
#define TRUE 1
#endif /* TRUE */

#ifndef FALSE
/** uhh... false */
#define FALSE 0
#endif /* FALSE */


/** An enum to indicate whether recursion is needed. */
enum svn_recurse_kind
{
  svn_nonrecursive = 1,
  svn_recursive
};

/** The concept of automatic conflict resolution.
 *
 * @since New in 1.5.
 */
typedef enum
{
  /* Invalid accept flag */
  svn_accept_invalid = -1,

  /* Resolve the conflict as usual */
  svn_accept_none,

  /* Resolve the conflict with the pre-conflict base file */
  svn_accept_left,

  /* Resolve the conflict with the pre-conflict working copy file */
  svn_accept_working,

  /* Resolve the conflict with the post-conflict base file */
  svn_accept_right,

} svn_accept_t;

/** Return the appropriate accept for @a accept_str.  @a word is as
 * returned from svn_accept_to_word().
 *
 * @since New in 1.5.
 */
svn_accept_t
svn_accept_from_word(const char *word);

/** The concept of depth for directories.
 * 
 * @note This is similar to, but not exactly the same as, the WebDAV
 * and LDAP concepts of depth.
 *
 * @since New in 1.5.
 */
typedef enum
{
  /* The order of these depths is important: the higher the number,
     the deeper it descends.  This allows us to compare two depths
     numerically to decide which should govern. */

  /* Depth undetermined or ignored. */
  svn_depth_unknown    = -2,

  /* Exclude (i.e., don't descend into) directory D. */
  svn_depth_exclude    = -1,

  /* Just the named directory D, no entries.  Updates will not pull in
     any files or subdirectories not already present. */
  svn_depth_empty      =  0,

  /* D + its file children, but not subdirs.  Updates will pull in any
     files not already present, but not subdirectories. */
  svn_depth_files      =  1,

  /* D + immediate children (D and its entries).  Updates will pull in
     any files or subdirectories not already present; those
     subdirectories' this_dir entries will have depth-empty. */
  svn_depth_immediates =  2,
  
  /* D + all descendants (full recursion from D).  Updates will pull
     in any files or subdirectories not already present; those
     subdirectories' this_dir entries will have depth-infinity.
     Equivalent to the pre-1.5 default update behavior. */
  svn_depth_infinity   =  3,

} svn_depth_t;


/** Return a constant string expressing @a depth as an English word,
 * e.g., "infinity", "immediates", etc.  The string is not localized,
 * as it may be used for client<->server communications.
 *
 * @since New in 1.5.
 */
const char *
svn_depth_to_word(svn_depth_t depth);


/** Return the appropriate depth for @a depth_str.  @a word is as
 * returned from svn_depth_to_word().
 *
 * @since New in 1.5.
 */
svn_depth_t
svn_depth_from_word(const char *word);


/* Return an @c svn_depth_t depth based on boolean @a recurse.
 *
 * @note New code should never need to use this, it is called only
 * from pre-depth APIs, for compatibility.
 *
 * @since New in 1.5.
 */
#define SVN_DEPTH_FROM_RECURSE(recurse) \
  ((recurse) ? svn_depth_infinity : svn_depth_files)


/* Return an @c svn_depth_t depth based on boolean @a recurse.
 * Use this only for the status command, as it has a unique interpretation
 * of recursion.
 *
 * @note New code should never need to use this, it is called only
 * from pre-depth APIs, for compatibility.
 *
 * @since New in 1.5.
 */
#define SVN_DEPTH_FROM_RECURSE_STATUS(recurse) \
  ((recurse) ? svn_depth_infinity : svn_depth_immediates)


/* Return a recursion boolean based on @a depth.
 *
 * Although much code has been converted to use depth, some code still
 * takes a recurse boolean.  In most cases, it makes sense to treat
 * unknown or infinite depth as recursive, and any other depth as
 * non-recursive (which in turn usually translates to @c svn_depth_files).
 */
#define SVN_DEPTH_TO_RECURSE(depth)                                \
  (((depth) == svn_depth_infinity || (depth) == svn_depth_unknown) \
   ? TRUE : FALSE)


/**
 * It is sometimes convenient to indicate which parts of an @c svn_dirent_t
 * object you are actually interested in, so that calculating and sending
 * the data corresponding to the other fields can be avoided.  These values
 * can be used for that purpose.
 *
 * @defgroup svn_dirent_fields dirent fields
 * @{
 */

/** An indication that you are interested in the @c kind field */
#define SVN_DIRENT_KIND        0x00001

/** An indication that you are interested in the @c size field */
#define SVN_DIRENT_SIZE        0x00002

/** An indication that you are interested in the @c has_props field */
#define SVN_DIRENT_HAS_PROPS   0x00004

/** An indication that you are interested in the @c created_rev field */
#define SVN_DIRENT_CREATED_REV 0x00008

/** An indication that you are interested in the @c time field */
#define SVN_DIRENT_TIME        0x00010

/** An indication that you are interested in the @c last_author field */
#define SVN_DIRENT_LAST_AUTHOR 0x00020

/** A combination of all the dirent fields */
#define SVN_DIRENT_ALL ~((apr_uint32_t ) 0)

/** @} */

/** A general subversion directory entry. */
typedef struct svn_dirent_t
{
  /** node kind */
  svn_node_kind_t kind;

  /** length of file text, or 0 for directories */
  svn_filesize_t size;

  /** does the node have props? */
  svn_boolean_t has_props;

  /** last rev in which this node changed */
  svn_revnum_t created_rev;

  /** time of created_rev (mod-time) */
  apr_time_t time;

  /** author of created_rev */
  const char *last_author;

  /* IMPORTANT: If you extend this struct, check svn_dirent_dup(). */
} svn_dirent_t;


/** Return a deep copy of @a dirent, allocated in @a pool.
 *
 * @since New in 1.4.
 */
svn_dirent_t *svn_dirent_dup(const svn_dirent_t *dirent,
                             apr_pool_t *pool);



/** Keyword substitution.
 *
 * All the keywords Subversion recognizes.
 * 
 * Note that there is a better, more general proposal out there, which
 * would take care of both internationalization issues and custom
 * keywords (e.g., $NetBSD$).  See
 * 
 *<pre>    http://subversion.tigris.org/servlets/ReadMsg?list=dev&msgNo=8921
 *    =====
 *    From: "Jonathan M. Manning" <jmanning@alisa-jon.net>
 *    To: dev@subversion.tigris.org
 *    Date: Fri, 14 Dec 2001 11:56:54 -0500
 *    Message-ID: <87970000.1008349014@bdldevel.bl.bdx.com>
 *    Subject: Re: keywords</pre>
 *
 * and Eric Gillespie's support of same:
 *
 *<pre>    http://subversion.tigris.org/servlets/ReadMsg?list=dev&msgNo=8757
 *    =====
 *    From: "Eric Gillespie, Jr." <epg@pretzelnet.org>
 *    To: dev@subversion.tigris.org
 *    Date: Wed, 12 Dec 2001 09:48:42 -0500
 *    Message-ID: <87k7vsebp1.fsf@vger.pretzelnet.org>
 *    Subject: Re: Customizable Keywords</pre>
 *
 * However, it is considerably more complex than the scheme below.
 * For now we're going with simplicity, hopefully the more general
 * solution can be done post-1.0.
 *
 * @defgroup svn_types_keywords keywords
 * @{
 */

/** The maximum size of an expanded or un-expanded keyword. */
#define SVN_KEYWORD_MAX_LEN    255

/** The most recent revision in which this file was changed. */
#define SVN_KEYWORD_REVISION_LONG    "LastChangedRevision"

/** Short version of LastChangedRevision */
#define SVN_KEYWORD_REVISION_SHORT   "Rev"

/** Medium version of LastChangedRevision, matching the one CVS uses.
 * @since New in 1.1. */
#define SVN_KEYWORD_REVISION_MEDIUM  "Revision"

/** The most recent date (repository time) when this file was changed. */
#define SVN_KEYWORD_DATE_LONG        "LastChangedDate"

/** Short version of LastChangedDate */
#define SVN_KEYWORD_DATE_SHORT       "Date"

/** Who most recently committed to this file. */
#define SVN_KEYWORD_AUTHOR_LONG      "LastChangedBy"

/** Short version of LastChangedBy */
#define SVN_KEYWORD_AUTHOR_SHORT     "Author"

/** The URL for the head revision of this file. */
#define SVN_KEYWORD_URL_LONG         "HeadURL"

/** Short version of HeadURL */
#define SVN_KEYWORD_URL_SHORT        "URL"

/** A compressed combination of the other four keywords. */
#define SVN_KEYWORD_ID               "Id"

/** @} */


/** All information about a commit.
 *
 * @note Objects of this type should always be created using the
 * svn_create_commit_info() function.
 *
 * @since New in 1.3.
 */
typedef struct svn_commit_info_t
{
  /** just-committed revision. */
  svn_revnum_t revision;

  /** server-side date of the commit. */
  const char *date;

  /** author of the commit. */
  const char *author;

  /** error message from post-commit hook, or NULL. */
  const char *post_commit_err;

} svn_commit_info_t;


/**
 * Allocate an object of type @c svn_commit_info_t in @a pool and
 * return it.
 *
 * The @c revision field of the new struct is set to @c
 * SVN_INVALID_REVNUM.  All other fields are initialized to @c NULL.
 *
 * @note Any object of the type @c svn_commit_info_t should
 * be created using this function.
 * This is to provide for extending the svn_commit_info_t in
 * the future.
 *
 * @since New in 1.3.
 */
svn_commit_info_t *
svn_create_commit_info(apr_pool_t *pool);


/**
 * Return a deep copy @a src_commit_info allocated in @a pool.
 *
 * @since New in 1.4.
 */
svn_commit_info_t *
svn_commit_info_dup(const svn_commit_info_t *src_commit_info,
                    apr_pool_t *pool);


/** A structure to represent a path that changed for a log entry. */
typedef struct svn_log_changed_path_t
{
  /** 'A'dd, 'D'elete, 'R'eplace, 'M'odify */
  char action;

  /** Source path of copy (if any). */
  const char *copyfrom_path;

  /** Source revision of copy (if any). */
  svn_revnum_t copyfrom_rev;

} svn_log_changed_path_t;


/**
 * Return a deep copy of @a changed_path, allocated in @a pool.
 *
 * @since New in 1.3.
 */
svn_log_changed_path_t *
svn_log_changed_path_dup(const svn_log_changed_path_t *changed_path,
                         apr_pool_t *pool);

/** 
 * A structure to represent all the information about a particular log entry. 
 *
 * @note To allow for extending the @c svn_log_entry_t structure in future
 * releases, always use svn_log_entry_create() to allocate the structure.
 */
typedef struct svn_log_entry_t
{
  /** A hash containing as keys every path committed in @a revision; the
   * values are (@c svn_log_changed_path_t *) stuctures.
   *
   * ### The only reason @a changed_paths is not qualified with `const' is
   * that we usually want to loop over it, and apr_hash_first() doesn't
   * take a const hash, for various reasons.  I'm not sure that those
   * "various reasons" are actually even relevant anymore, and if
   * they're not, it might be nice to change apr_hash_first() so
   * read-only uses of hashes can be protected via the type system.
   */
  apr_hash_t *changed_paths;

  /** The revision of the commit. */
  svn_revnum_t revision;

  /** The author of the commit. */
  const char *author;

  /** The date of the commit. */
  const char *date;

  /** The log message of the commit. */
  const char *message;

  /** The number of children of this log entry.
   * When a log operation requests additional merge information, extra log
   * entries may be returned as a result of this entry.  The new entries, are
   * considered children of the original entry, with CHILD_COUNT cardinality
   * The child entries are returned through the receiver interface right after
   * the parent.
   *
   * For log operations which do not request additional merge information, the
   * CHILD_COUNT is always zero.
   *
   * For more information see:
   * http://subversion.tigris.org/merge-tracking/design.html#commutative-reporting
   */
  apr_uint64_t nbr_children;
} svn_log_entry_t;

/**
 * Returns an @c svn_log_entry_t, allocated in @a pool with all fields
 * initialized to null values.
 *
 * @note To allow for extending the @c svn_log_entry_t structure in future
 * releases, this function should always be used to allocate the structure.
 *
 * @since New in 1.5.
 */
svn_log_entry_t *
svn_log_entry_create(apr_pool_t *pool);

/** The callback invoked by log message loopers, such as
 * @c svn_ra_plugin_t.get_log() and svn_repos_get_logs().
 *
 * This function is invoked once on each log message, in the order
 * determined by the caller (see above-mentioned functions).
 *
 * @a baton is what you think it is, and @a log_entry contains relevent 
 * information for the log message.  Any of @a log_entry->author,
 * @a log_entry->date, or @a log_entry->message may be @c NULL.
 *
 * If @a log_entry->date is neither null nor the empty string, it was
 * generated by svn_time_to_cstring() and can be converted to
 * @c apr_time_t with svn_time_from_cstring().
 *
 * If @a log_entry->changed_paths is non-@c NULL, then it contains as keys
 * every path committed in @a log_entry->revision; the values are
 * (@c svn_log_changed_path_t *) structures.
 *
 * Use @a pool for temporary allocation.  If the caller is iterating
 * over log messages, invoking this receiver on each, we recommend the
 * standard pool loop recipe: create a subpool, pass it as @a pool to
 * each call, clear it after each iteration, destroy it after the loop
 * is done.  (For allocation that must last beyond the lifetime of a
 * given receiver call, use a pool in @a baton.)
 *
 * @since New in 1.5.
 */

typedef svn_error_t *(*svn_log_message_receiver2_t)
  (void *baton,
   svn_log_entry_t *log_entry,
   apr_pool_t *pool);

/**
 * Similar to svn_log_message_receiver2_t, except this uses separate
 * parameters for each part of the log entry.
 *
 * @deprecated Provided for backward compatibility with the 1.4 API.
 */
typedef svn_error_t *(*svn_log_message_receiver_t)
  (void *baton,
   apr_hash_t *changed_paths,
   svn_revnum_t revision,
   const char *author,
   const char *date,  /* use svn_time_from_cstring() if need apr_time_t */
   const char *message,
   apr_pool_t *pool);


/** Callback function type for commits.
 *
 * When a commit succeeds, an instance of this is invoked with the
 * @a commit_info, along with the @a baton closure.
 * @a pool can be used for temporary allocations.
 *
 * @since New in 1.4.
 */
typedef svn_error_t *(*svn_commit_callback2_t)
  (const svn_commit_info_t *commit_info,
   void *baton,
   apr_pool_t *pool);

/** Same as @c svn_commit_callback2_t, but uses individual
 * data elements instead of the @c svn_commit_info_t structure
 *
 * @deprecated Provided for backward compatibility with the 1.3 API.
 */
typedef svn_error_t *(*svn_commit_callback_t)
  (svn_revnum_t new_revision,
   const char *date,
   const char *author,
   void *baton);


/** Return, in @a *callback2 and @a *callback2_baton a function/baton that
 * will call @a callback/@a callback_baton, allocating the @a *callback2_baton
 * in @a pool.
 *
 * @note This is used by compatibility wrappers, which exist in more than
 * Subversion core library.
 *
 * @since New in 1.4.
 */
void svn_compat_wrap_commit_callback(svn_commit_callback2_t *callback2,
                                     void **callback2_baton,
                                     svn_commit_callback_t callback,
                                     void *callback_baton,
                                     apr_pool_t *pool);

/** Return, in @a *receiver2 and @a *receiver2_baton a function/baton that
 * will call @a receiver/@a receiver_baton, allocating the @a *receiver2_baton
 * in @a pool.
 *
 * @note This is used by compatibility wrappers, which exist in more than
 * Subversion core library.
 *
 * @since New in 1.5.
 */
void svn_compat_wrap_log_receiver(svn_log_message_receiver2_t *receiver2,
                                  void **receiver2_baton,
                                  svn_log_message_receiver_t receiver,
                                  void *receiver_baton,
                                  apr_pool_t *pool);


/** A buffer size that may be used when processing a stream of data.
 *
 * @note We don't use this constant any longer, since it is considered to be
 * unnecessarily large.
 *
 * @deprecated Provided for backwards compatibility with the 1.3 API.
 */
#define SVN_STREAM_CHUNK_SIZE 102400

#ifndef DOXYGEN_SHOULD_SKIP_THIS
/*
 * The maximum amount we (ideally) hold in memory at a time when
 * processing a stream of data.
 *
 * For example, when copying data from one stream to another, do it in
 * blocks of this size.
 *
 * NOTE: This is an internal macro, put here for convenience.
 * No public API may depend on the particular value of this macro.
 */
#define SVN__STREAM_CHUNK_SIZE 16384
#endif

/** The maximum amount we can ever hold in memory. */
/* FIXME: Should this be the same as SVN_STREAM_CHUNK_SIZE? */
#define SVN_MAX_OBJECT_SIZE (((apr_size_t) -1) / 2)



/* ### Note: despite being about mime-TYPES, these probably don't
 * ### belong in svn_types.h.  However, no other header is more
 * ### appropriate, and didn't feel like creating svn_validate.h for
 * ### so little.
 */

/** Validate @a mime_type.
 *
 * If @a mime_type does not contain a "/", or ends with non-alphanumeric
 * data, return @c SVN_ERR_BAD_MIME_TYPE, else return success.
 * 
 * Use @a pool only to find error allocation.
 *
 * Goal: to match both "foo/bar" and "foo/bar; charset=blah", without
 * being too strict about it, but to disallow mime types that have
 * quotes, newlines, or other garbage on the end, such as might be
 * unsafe in an HTTP header.
 */
svn_error_t *svn_mime_type_validate(const char *mime_type,
                                    apr_pool_t *pool);


/** Return false iff @a mime_type is a textual type.
 *
 * All mime types that start with "text/" are textual, plus some special 
 * cases (for example, "image/x-xbitmap").
 */
svn_boolean_t svn_mime_type_is_binary(const char *mime_type);



/** A user defined callback that subversion will call with a user defined 
 * baton to see if the current operation should be continued.  If the operation 
 * should continue, the function should return @c SVN_NO_ERROR, if not, it 
 * should return @c SVN_ERR_CANCELLED.
 */
typedef svn_error_t *(*svn_cancel_func_t)(void *cancel_baton);



/**
 * A lock object, for client & server to share.
 *
 * A lock represents the exclusive right to add, delete, or modify a
 * path.  A lock is created in a repository, wholly controlled by the
 * repository.  A "lock-token" is the lock's UUID, and can be used to
 * learn more about a lock's fields, and or/make use of the lock.
 * Because a lock is immutable, a client is free to not only cache the
 * lock-token, but the lock's fields too, for convenience.
 *
 * Note that the 'is_dav_comment' field is wholly ignored by every
 * library except for mod_dav_svn.  The field isn't even marshalled
 * over the network to the client.  Assuming lock structures are
 * created with apr_pcalloc(), a default value of 0 is universally safe.
 *
 * @note in the current implementation, only files are lockable.
 *
 * @since New in 1.2.
 */
typedef struct svn_lock_t
{
  const char *path;              /**< the path this lock applies to */
  const char *token;             /**< unique URI representing lock */
  const char *owner;             /**< the username which owns the lock */
  const char *comment;           /**< (optional) description of lock  */
  svn_boolean_t is_dav_comment;  /**< was comment made by generic DAV client? */
  apr_time_t creation_date;      /**< when lock was made */
  apr_time_t expiration_date;    /**< (optional) when lock will expire;
                                      If value is 0, lock will never expire. */
} svn_lock_t;

/**
 * Returns an @c svn_lock_t, allocated in @a pool with all fields initialized
 * to null values.
 *
 * @note To allow for extending the @c svn_lock_t structure in the future
 * releases, this function should always be used to allocate the structure.
 *
 * @since New in 1.2.
 */
svn_lock_t *
svn_lock_create(apr_pool_t *pool);

/**
 * Return a deep copy of @a lock, allocated in @a pool.
 *
 * @since New in 1.2.
 */
svn_lock_t *
svn_lock_dup(const svn_lock_t *lock, apr_pool_t *pool);

/**
 * Return a formatted Universal Unique IDentifier (UUID) string.
 *
 * @since New in 1.4.
 */
const char *
svn_uuid_generate(apr_pool_t *pool);

/** 
 * Merge info representing a merge of a range of revisions.
 * @since New in 1.5
 */ 
typedef struct svn_merge_range_t
{
  svn_revnum_t start;
  svn_revnum_t end;
  svn_boolean_t inheritable;
} svn_merge_range_t;

/**
 * The three ways to consider the inheritable member when
 * comparing @c svn_merge_range_t.
 *
 * @since New in 1.5.
 */
typedef enum
{
  /* Don't take inheritability into consideration. */
  svn_rangelist_ignore_inheritance,

  /* Inheritability of both ranges must be the same. */
  svn_rangelist_equal_inheritance,

  /* Inheritability of both ranges must be the @c TRUE. */
  svn_rangelist_only_inheritable,
} svn_merge_range_inheritance_t;

/**
 * Return a copy of @a range, allocated in @a pool.
 *
 * @since New in 1.5.
 */
svn_merge_range_t *
svn_merge_range_dup(svn_merge_range_t *range, apr_pool_t *pool);

/**
 * The three ways to request mergeinfo affecting a given path.
 *
 * @since New in 1.5.
 */
typedef enum
{
  /* Explicit mergeinfo only */
  svn_mergeinfo_explicit,

  /* Explicit mergeinfo, or if that doesn't exist, the inherited mergeinfo
     from a target's nearest ancestor */
  svn_mergeinfo_inherited,

  /* Mergeinfo on target's nearest ancestor, regardless of whether target
     has explict mergeinfo */
  svn_mergeinfo_nearest_ancestor
} svn_mergeinfo_inheritance_t;

/** Return a constant string expressing @a inherit as an English word,
 * i.e., "explicit" (default), "inherited", or "nearest_ancestor".
 * The string is not localized, as it may be used for client<->server
 * communications.
 *
 * @since New in 1.5.
 */
const char *
svn_inheritance_to_word(svn_mergeinfo_inheritance_t inherit);


/** Return the appropriate @c svn_mergeinfo_inheritance_t for @a word.
 * @a word is as returned from svn_inheritance_to_word().  Defaults to
 * @c svn_mergeinfo_explicit.
 *
 * @since New in 1.5.
 */
svn_mergeinfo_inheritance_t
svn_inheritance_from_word(const char *word);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_TYPES_H */
