/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_string.h
 * @brief Counted-length strings for Subversion, plus some C string goodies.
 * 
 * There are two string datatypes: @c svn_string_t and @c svn_stringbuf_t.
 * The former is a simple pointer/length pair useful for passing around
 * strings (or arbitrary bytes) with a counted length. @c svn_stringbuf_t is
 * buffered to enable efficient appending of strings without an allocation
 * and copy for each append operation.
 *
 * @c svn_string_t contains a <tt>const char *</tt> for its data, so it is 
 * most appropriate for constant data and for functions which expect constant,
 * counted data. Functions should generally use <tt>const @c svn_string_t 
 * *</tt> as their parameter to indicate they are expecting a constant, 
 * counted string.
 *
 * @c svn_stringbuf_t uses a plain <tt>char *</tt> for its data, so it is 
 * most appropriate for modifiable data.
 *
 * <h3>Invariants</h3>
 *
 *   1. Null termination:
 *
 *      Both structures maintain a significant invariant:
 *
 *         <tt>s->data[s->len] == '\\0'</tt>
 *
 *      The functions defined within this header file will maintain
 *      the invariant (which does imply that memory is
 *      allocated/defined as @c len+1 bytes).  If code outside of the
 *      @c svn_string.h functions manually builds these structures,
 *      then they must enforce this invariant.
 *
 *      Note that an @c svn_string(buf)_t may contain binary data,
 *      which means that @c strlen(s->data) does not have to equal @c
 *      s->len. The null terminator is provided to make it easier to
 *      pass @c s->data to C string interfaces.
 *
 *
 *   2. Non-null input:
 *
 *      All the functions below assume their input data is non-null,
 *      unless otherwise documented, and may seg fault if passed
 *      null.  The input data may *contain* null bytes, of course, just
 *      the data pointer itself must not be null.
 */


#ifndef SVN_STRING_H
#define SVN_STRING_H

#include <apr.h>
#include <apr_tables.h>
#include <apr_pools.h>       /* APR memory pools for everyone. */
#include <apr_strings.h>

#include "svn_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */




/** A simple counted string. */
typedef struct svn_string_t
{
  const char *data;
  apr_size_t len;
} svn_string_t;

/** A buffered string, capable of appending without an allocation and copy 
 * for each append. */
typedef struct svn_stringbuf_t
{
  /** a pool from which this string was originally allocated, and is not 
   * necessarily specific to this string.  This is used only for allocating 
   * more memory from when the string needs to grow.
   */
  apr_pool_t *pool;

  /** pointer to the bytestring */
  char *data;

  /** length of bytestring */
  apr_size_t len;

  /** total size of buffer allocated */
  apr_size_t blocksize;
} svn_stringbuf_t;


/** svn_string_t functions.
 *
 * @defgroup svn_string_svn_string_t svn_string_t functions
 * @{
 */

/** Create a new bytestring containing a C string (null-terminated). */
svn_string_t *svn_string_create (const char *cstring, 
                                 apr_pool_t *pool);

/** Create a new bytestring containing a generic string of bytes 
 * (NOT null-terminated) */
svn_string_t *svn_string_ncreate (const char *bytes,
                                  apr_size_t size, 
                                  apr_pool_t *pool);

/** Create a new string with the contents of the given stringbuf */
svn_string_t *svn_string_create_from_buf (const svn_stringbuf_t *strbuf,
                                          apr_pool_t *pool);

/** Create a new bytestring by formatting @a cstring (null-terminated)
 * from varargs, which are as appropriate for @c apr_psprintf.
 */
svn_string_t *svn_string_createf (apr_pool_t *pool,
                                  const char *fmt,
                                  ...)
       __attribute__ ((format (printf, 2, 3)));

/** Create a new bytestring by formatting @a cstring (null-terminated)
 * from a @c va_list (see @c svn_stringbuf_createf).
 */
svn_string_t *svn_string_createv (apr_pool_t *pool,
                                  const char *fmt,
                                  va_list ap)
       __attribute__ ((format (printf, 2, 0)));

/** Return true if a bytestring is empty (has length zero). */
svn_boolean_t svn_string_isempty (const svn_string_t *str);

/** Return a duplicate of @a original_string. */
svn_string_t *svn_string_dup (const svn_string_t *original_string,
                              apr_pool_t *pool);

/** Return @c TRUE iff @a str1 and @c str2 have identical length and data. */
svn_boolean_t svn_string_compare (const svn_string_t *str1, 
                                  const svn_string_t *str2);

/** Return offset of first non-whitespace character in @a str, or return
 * @a str->len if none.
 */
apr_size_t svn_string_first_non_whitespace (const svn_string_t *str);

/** Return position of last occurrence of @a char in @a str, or return
 * @a str->len if no occurrence.
 */ 
apr_size_t svn_string_find_char_backward (const svn_string_t *str, char ch);

/** @} */


/** svn_stringbuf_t functions.
 *
 * @defgroup svn_string_svn_stringbuf_t svn_stringbuf_t functions
 * @{
 */

/** Create a new bytestring containing a C string (null-terminated). */
svn_stringbuf_t *svn_stringbuf_create (const char *cstring, 
                                       apr_pool_t *pool);
/** Create a new bytestring containing a generic string of bytes 
 * (NON-null-terminated)
 */
svn_stringbuf_t *svn_stringbuf_ncreate (const char *bytes,
                                        apr_size_t size, 
                                        apr_pool_t *pool);

/** Create a new stringbuf with the contents of the given string */
svn_stringbuf_t *svn_stringbuf_create_from_string (const svn_string_t *str,
                                                   apr_pool_t *pool);

/** Create a new bytestring by formatting @a cstring (null-terminated)
 * from varargs, which are as appropriate for @c apr_psprintf.
 */
svn_stringbuf_t *svn_stringbuf_createf (apr_pool_t *pool,
                                        const char *fmt,
                                        ...)
       __attribute__ ((format (printf, 2, 3)));

/** Create a new bytestring by formatting @a cstring (null-terminated)
 * from a @c va_list (see svn_stringbuf_createf).
 */
svn_stringbuf_t *svn_stringbuf_createv (apr_pool_t *pool,
                                        const char *fmt,
                                        va_list ap)
       __attribute__ ((format (printf, 2, 0)));

/** Make sure that the string @a str has at least @a minimum_size bytes of
 * space available in the memory block.
 *
 * (@a minimum_size should include space for the terminating null character.)
 */
void svn_stringbuf_ensure (svn_stringbuf_t *str,
                           apr_size_t minimum_size);

/** Set a bytestring @a str to @a value */
void svn_stringbuf_set (svn_stringbuf_t *str, const char *value);

/** Set a bytestring @a str to empty (0 length). */
void svn_stringbuf_setempty (svn_stringbuf_t *str);

/** Return @c TRUE if a bytestring is empty (has length zero). */
svn_boolean_t svn_stringbuf_isempty (const svn_stringbuf_t *str);

/** Chop @a nbytes bytes off end of @a str, but not more than @a str->len. */
void svn_stringbuf_chop (svn_stringbuf_t *str, apr_size_t bytes);

/** Fill bytestring @a str with character @a c. */
void svn_stringbuf_fillchar (svn_stringbuf_t *str, unsigned char c);

/** Append an array of bytes onto @a targetstr.
 *
 * @c reallocs() if necessary. @a targetstr is affected, nothing else is.
 */
void svn_stringbuf_appendbytes (svn_stringbuf_t *targetstr,
                                const char *bytes, 
                                apr_size_t count);

/** Append an @c svn_stringbuf_t onto @a targetstr.
 *
 * @c reallocs() if necessary. @a targetstr is affected, nothing else is.
 */
void svn_stringbuf_appendstr (svn_stringbuf_t *targetstr, 
                              const svn_stringbuf_t *appendstr);

/** Append a C string onto @a targetstr.
 *
 * @c reallocs() if necessary. @a targetstr is affected, nothing else is.
 */
void svn_stringbuf_appendcstr (svn_stringbuf_t *targetstr,
                               const char *cstr);

/** Return a duplicate of @a original_string. */
svn_stringbuf_t *svn_stringbuf_dup (const svn_stringbuf_t *original_string,
                                    apr_pool_t *pool);


/** Return @c TRUE iff @a str1 and @a str2 have identical length and data. */
svn_boolean_t svn_stringbuf_compare (const svn_stringbuf_t *str1, 
                                     const svn_stringbuf_t *str2);

/** Return offset of first non-whitespace character in @a str, or return
 * @a str->len if none.
 */
apr_size_t svn_stringbuf_first_non_whitespace (const svn_stringbuf_t *str);

/** Strip whitespace from both sides of @a str (modified in place). */
void svn_stringbuf_strip_whitespace (svn_stringbuf_t *str);

/** Return position of last occurrence of @a ch in @a str, or return
 * @a str->len if no occurrence.
 */ 
apr_size_t svn_stringbuf_find_char_backward (const svn_stringbuf_t *str, 
                                             char ch);

/** Return @c TRUE iff @a str1 and @a str2 have identical length and data. */
svn_boolean_t svn_string_compare_stringbuf (const svn_string_t *str1,
                                            const svn_stringbuf_t *str2);

/** @} */


/** C strings.
 *
 * @defgroup svn_string_cstrings c string functions
 * @{
 */

/** Divide @a input into substrings along @a sep_char boundaries, return an
 * array of copies of those substrings, allocating both the array and
 * the copies in @a pool.
 *
 * None of the elements added to the array contain any of the
 * characters in @a sep_chars, and none of the new elements are empty
 * (thus, it is possible that the returned array will have length
 * zero).
 *
 * If @a chop_whitespace is true, then remove leading and trailing
 * whitespace from the returned strings.
 */
apr_array_header_t *svn_cstring_split (const char *input,
                                       const char *sep_chars,
                                       svn_boolean_t chop_whitespace,
                                       apr_pool_t *pool);

/** Like @c svn_cstring_split(), but append to existing @a array instead of
 * creating a new one.  Allocate the copied substrings in @a pool
 * (i.e., caller decides whether or not to pass @a array->pool as @a pool).
 */
void svn_cstring_split_append (apr_array_header_t *array,
                               const char *input,
                               const char *sep_chars,
                               svn_boolean_t chop_whitespace,
                               apr_pool_t *pool);


/** Return @c TRUE iff @a str matches any of the elements of @a list, a list 
 * of zero or more glob patterns.
 *
 * Use @a pool for temporary allocation.
 */
svn_boolean_t svn_cstring_match_glob_list (const char *str,
                                           apr_array_header_t *list);

/** @since New in 1.2.
 *
 * Return the number of line breaks in @a msg, allowing any kind of newline
 * termination (CR, CRLF, or LFCR), even inconsistent.
 */
int svn_cstring_count_newlines (const char *msg);

/** @} */


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif  /* SVN_STRING_H */
