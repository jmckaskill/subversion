/*
 * utf.c:  UTF-8 conversion routines
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



#include <string.h>
#include <ctype.h>
#include <assert.h>

#include <apr_strings.h>
#include <apr_lib.h>
#include <apr_xlate.h>

#include "svn_string.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "svn_utf.h"
#include "utf_impl.h"



#define SVN_UTF_NTOU_XLATE_HANDLE "svn-utf-ntou-xlate-handle"
#define SVN_UTF_UTON_XLATE_HANDLE "svn-utf-uton-xlate-handle"

/* Return an apr_xlate handle for converting from FROMPAGE to
   TOPAGE. Create one if it doesn't exist in USERDATA_KEY. If
   unable to find a handle, or unable to create one because
   apr_xlate_open returned APR_EINVAL, then set *RET to null and
   return SVN_NO_ERROR; if fail for some other reason, return
   error. */
static svn_error_t *
get_xlate_handle (apr_xlate_t **ret,
                  const char *topage, const char *frompage,
                  const char *userdata_key, apr_pool_t *pool)
{
  void *old_handle = NULL;
  apr_status_t apr_err;

  /* If we already have a handle, just return it. */
  if (userdata_key)
    {
      apr_pool_userdata_get (&old_handle, userdata_key, pool);
      if (old_handle != NULL)
        {
          *ret = old_handle;
          return SVN_NO_ERROR;
        }
    }

  /* Try to create one. */
  apr_err = apr_xlate_open (ret, topage, frompage, pool);

  if (APR_STATUS_IS_EINVAL (apr_err) || APR_STATUS_IS_ENOTIMPL (apr_err))
    {
      *ret = NULL;
      return SVN_NO_ERROR;
    }
  if (apr_err != APR_SUCCESS)
    /* Can't use svn_error_wrap_apr here because it calls functions in
       this file, leading to infinite recursion. */
    return svn_error_createf
      (apr_err, NULL, "Can't create a converter from '%s' to '%s'",
       (topage == APR_LOCALE_CHARSET ? "native" : topage),
       (frompage == APR_LOCALE_CHARSET ? "native" : frompage));

  /* Save it for later. */
  if (userdata_key)
    {
      apr_pool_userdata_set (*ret, userdata_key, apr_pool_cleanup_null, pool);
    }

  return SVN_NO_ERROR;
}


/* Return the apr_xlate handle for converting native characters to UTF-8. */
static svn_error_t *
get_ntou_xlate_handle (apr_xlate_t **ret, apr_pool_t *pool)
{
  return get_xlate_handle(ret, "UTF-8", APR_LOCALE_CHARSET,
                          SVN_UTF_NTOU_XLATE_HANDLE, pool);
}


/* Return the apr_xlate handle for converting UTF-8 to native characters.
   Create one if it doesn't exist.  If unable to find a handle, or
   unable to create one because apr_xlate_open returned APR_EINVAL, then
   set *RET to null and return SVN_NO_ERROR; if fail for some other
   reason, return error. */
static svn_error_t *
get_uton_xlate_handle (apr_xlate_t **ret, apr_pool_t *pool)
{
  return get_xlate_handle(ret, APR_LOCALE_CHARSET, "UTF-8",
                          SVN_UTF_UTON_XLATE_HANDLE, pool);
}


/* Convert SRC_LENGTH bytes of SRC_DATA in CONVSET, store the result
   in *DEST, which is allocated in POOL. */
static svn_error_t *
convert_to_stringbuf (apr_xlate_t *convset,
                      const char *src_data,
                      apr_size_t src_length,
                      svn_stringbuf_t **dest,
                      apr_pool_t *pool)
{
  apr_size_t buflen = src_length;
  apr_status_t apr_err;
  apr_size_t srclen = src_length;
  apr_size_t destlen = 0;
  char *destbuf;

  /* Initialize *DEST to an empty stringbuf. */
  *dest = svn_stringbuf_create ("", pool);
  destbuf = (*dest)->data;

  /* Not only does it not make sense to convert an empty string, but
     apr-iconv is quite unreasonable about not allowing that. */
  if (src_length == 0)
    return SVN_NO_ERROR;

  do 
    {
      /* A 1:2 ratio of input characters to output characters should
         be enough for most translations, and conveniently enough, if
         it isn't, we'll grow the buffer size by 2 again. */
      if (destlen == 0)
        buflen *= 2;

      /* Ensure that *DEST has sufficient storage for the translated
         result. */
      svn_stringbuf_ensure (*dest, buflen + 1);

      /* Update the destination buffer pointer to the first character
         after already-converted output. */
      destbuf = (*dest)->data + (*dest)->len;

      /* Set up state variables for xlate. */
      destlen = buflen - (*dest)->len;

      /* Attempt the conversion. */
      apr_err = apr_xlate_conv_buffer (convset, 
                                       src_data + (src_length - srclen), 
                                       &srclen,
                                       destbuf, 
                                       &destlen);

      /* Now, updated the *DEST->len to track the amount of output data
         churned out so far from this loop. */
      (*dest)->len += ((buflen - (*dest)->len) - destlen);

    } while ((! apr_err || apr_err == APR_INCOMPLETE) && srclen);

  /* If we exited the loop with an error, return the error. */
  if (apr_err)
    /* Can't use svn_error_wrap_apr here because it calls functions in
       this file, leading to infinite recursion. */
    return svn_error_create (apr_err, NULL, "Can't recode string");
  
  /* Else, exited do to success.  Trim the result buffer down to the
     right length. */
  (*dest)->data[(*dest)->len] = '\0';

  return SVN_NO_ERROR;
}


/* Return APR_EINVAL if the first LEN bytes of DATA contain anything
   other than seven-bit, non-control (except for whitespace) ascii
   characters, finding the error pool from POOL.  Otherwise, return
   SVN_NO_ERROR. */
static svn_error_t *
check_non_ascii (const char *data, apr_size_t len, apr_pool_t *pool)
{
  const char *data_start = data;

  for (; len > 0; --len, data++)
    {
      if ((! apr_isascii (*((const unsigned char *) data)))
          || ((! apr_isspace (*((const unsigned char *) data)))
              && apr_iscntrl (*((const unsigned char *) data))))
        {
          /* Show the printable part of the data, followed by the
             decimal code of the questionable character.  Because if a
             user ever gets this error, she's going to have to spend
             time tracking down the non-ascii data, so we want to help
             as much as possible.  And yes, we just call the unsafe
             data "non-ascii", even though the actual constraint is
             somewhat more complex than that. */ 

          if (data - data_start)
            {
              const char *error_data
                = apr_pstrndup (pool, data_start, (data - data_start));

              return svn_error_createf
                (APR_EINVAL, NULL,
                 "Safe data:\n"
                 "\"%s\"\n"
                 "... was followed by non-ascii byte %d.\n"
                 "\n"
                 "Non-ascii character detected (see above), "
                 "and unable to convert to/from UTF-8",
                 error_data, *((const unsigned char *) data));
            }
          else
            {
              return svn_error_createf
                (APR_EINVAL, NULL,
                 "Non-ascii character (code %d) detected, "
                 "and unable to convert to/from UTF-8",
                 *((const unsigned char *) data));
            }
        }
    }

  return SVN_NO_ERROR;
}

/* Construct an error with a suitable message to describe the invalid UTF-8
 * sequence DATA of length LEN (which may have embedded NULLs).  We can't
 * simply print the data, almost by definition we don't really know how it
 * is encoded.
 */
static svn_error_t *
invalid_utf8 (const char *data, apr_size_t len, apr_pool_t *pool)
{
  const char *last = svn_utf__last_valid (data, len);
  const char *msg = "Valid UTF-8 data\n(hex:";
  int i, valid, invalid;

  /* We will display at most 24 valid octets (this may split a leading
     multi-byte character) as that should fit on one 80 character line. */
  valid = last - data;
  if (valid > 24)
    valid = 24;
  for (i = 0; i < valid; ++i)
    msg = apr_pstrcat (pool, msg, apr_psprintf (pool, " %02x",
                                                (unsigned char)last[i-valid]),
                       NULL);
  msg = apr_pstrcat (pool, msg,
                     ")\nfollowed by invalid UTF-8 sequence\n(hex:", NULL);

  /* 4 invalid octets will guarantee that the faulty octet is displayed */
  invalid = data + len - last;
  if (invalid > 4)
    invalid = 4;
  for (i = 0; i < invalid; ++i)
    msg = apr_pstrcat (pool, msg, apr_psprintf (pool, " %02x",
                                                (unsigned char)last[i]), NULL);
  msg = apr_pstrcat (pool, msg, ")", NULL);

  return svn_error_create (APR_EINVAL, NULL, msg);
}

/* Verify that the sequence DATA of length LEN is valid UTF-8 */
static svn_error_t *
check_utf8 (const char *data, apr_size_t len, apr_pool_t *pool)
{
  if (! svn_utf__is_valid (data, len))
    return invalid_utf8 (data, len, pool);
  return SVN_NO_ERROR;
}

/* Verify that the NULL terminated sequence DATA is valid UTF-8 */
static svn_error_t *
check_cstring_utf8 (const char *data, apr_pool_t *pool)
{

  if (! svn_utf__cstring_is_valid (data))
    return invalid_utf8 (data, strlen (data), pool);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_stringbuf_to_utf8 (svn_stringbuf_t **dest,
                           const svn_stringbuf_t *src,
                           apr_pool_t *pool)
{
  apr_xlate_t *convset;

  SVN_ERR (get_ntou_xlate_handle (&convset, pool));

  if (convset)
    {
      SVN_ERR (convert_to_stringbuf (convset, src->data, src->len, dest, pool));
      return check_utf8 ((*dest)->data, (*dest)->len, pool);
    }
  else
    {
      SVN_ERR (check_non_ascii (src->data, src->len, pool));
      *dest = svn_stringbuf_dup (src, pool);
      return SVN_NO_ERROR;
    }
}


svn_error_t *
svn_utf_string_to_utf8 (const svn_string_t **dest,
                        const svn_string_t *src,
                        apr_pool_t *pool)
{
  svn_stringbuf_t *destbuf;
  apr_xlate_t *convset;

  SVN_ERR (get_ntou_xlate_handle (&convset, pool));

  if (convset)
    {
      SVN_ERR (convert_to_stringbuf (convset, src->data, src->len, 
                                     &destbuf, pool));
      SVN_ERR (check_utf8 (destbuf->data, destbuf->len, pool));
      *dest = svn_string_create_from_buf (destbuf, pool);
    }
  else
    {
      SVN_ERR (check_non_ascii (src->data, src->len, pool));
      *dest = svn_string_dup (src, pool);
    }

  return SVN_NO_ERROR;
}


/* Common implementation for svn_utf_cstring_to_utf8,
   svn_utf_cstring_to_utf8_ex, svn_utf_cstring_from_utf8 and
   svn_utf_cstring_from_utf8_ex. Convert SRC to DEST using CONVSET as
   the translator and allocating from POOL. */
static svn_error_t *
convert_cstring (const char **dest,
                 const char *src,
                 apr_xlate_t *convset,
                 apr_pool_t *pool)
{
  if (convset)
    {
      svn_stringbuf_t *destbuf;
      SVN_ERR (convert_to_stringbuf (convset, src, strlen (src),
                                     &destbuf, pool));
      *dest = destbuf->data;
    }
  else
    {
      apr_size_t len = strlen (src);
      SVN_ERR (check_non_ascii (src, len, pool));
      *dest = apr_pstrmemdup (pool, src, len);
    }
  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_cstring_to_utf8 (const char **dest,
                         const char *src,
                         apr_pool_t *pool)
{
  apr_xlate_t *convset;

  SVN_ERR (get_ntou_xlate_handle (&convset, pool));
  SVN_ERR (convert_cstring (dest, src, convset, pool));
  SVN_ERR (check_cstring_utf8 (*dest, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_cstring_to_utf8_ex (const char **dest,
                            const char *src,
                            const char *frompage,
                            const char *convset_key,
                            apr_pool_t *pool)
{
  apr_xlate_t *convset;

  SVN_ERR (get_xlate_handle (&convset, "UTF-8", frompage, convset_key, pool));
  SVN_ERR (convert_cstring (dest, src, convset, pool));
  SVN_ERR (check_cstring_utf8 (*dest, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_stringbuf_from_utf8 (svn_stringbuf_t **dest,
                             const svn_stringbuf_t *src,
                             apr_pool_t *pool)
{
  apr_xlate_t *convset;

  SVN_ERR (get_uton_xlate_handle (&convset, pool));

  if (convset)
    return convert_to_stringbuf (convset, src->data, src->len, dest, pool);
  else
    {
      SVN_ERR (check_non_ascii (src->data, src->len, pool));
      *dest = svn_stringbuf_dup (src, pool);
      return SVN_NO_ERROR;
    }
}


svn_error_t *
svn_utf_string_from_utf8 (const svn_string_t **dest,
                          const svn_string_t *src,
                          apr_pool_t *pool)
{
  svn_stringbuf_t *dbuf;
  apr_xlate_t *convset;

  SVN_ERR (get_uton_xlate_handle (&convset, pool));

  if (convset)
    {
      SVN_ERR (convert_to_stringbuf (convset, src->data, src->len,
                                     &dbuf, pool));
      *dest = svn_string_create_from_buf (dbuf, pool);
    }
  else
    {
      SVN_ERR (check_non_ascii (src->data, src->len, pool));
      *dest = svn_string_dup (src, pool);
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_cstring_from_utf8 (const char **dest,
                           const char *src,
                           apr_pool_t *pool)
{
  apr_xlate_t *convset;

  SVN_ERR (get_uton_xlate_handle (&convset, pool));
  SVN_ERR (convert_cstring (dest, src, convset, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_cstring_from_utf8_ex (const char **dest,
                              const char *src,
                              const char *topage,
                              const char *convset_key,
                              apr_pool_t *pool)
{
  apr_xlate_t *convset;

  SVN_ERR (get_xlate_handle (&convset, topage, "UTF-8", convset_key, pool));
  SVN_ERR (convert_cstring (dest, src, convset, pool));

  return SVN_NO_ERROR;
}


const char *
svn_utf__cstring_from_utf8_fuzzy (const char *src,
                                  apr_pool_t *pool,
                                  svn_error_t *(*convert_from_utf8)
                                  (const char **, const char *, apr_pool_t *))
{
  const char *src_orig = src;
  apr_size_t new_len = 0;
  char *new;
  const char *new_orig;
  svn_error_t *err;

  /* First count how big a dest string we'll need. */
  while (*src)
    {
      if (! apr_isascii (*src))
        new_len += 5;  /* 5 slots, for "?\XXX" */
      else
        new_len += 1;  /* one slot for the 7-bit char */

      src++;
    }

  /* Allocate that amount. */
  new = apr_palloc (pool, new_len + 1);

  /* All right, Brane.  We allocated it, we're building it, we're
     returning it.  We can cast it, right? :-) */ 
  new_orig = (const char *) new;

  /* And fill it up. */
  while (*src_orig)
    {
      if (! apr_isascii (*src_orig))
        {
          sprintf (new, "?\\%03u", (unsigned char) *src_orig);
          new += 5;
        }
      else
        {
          *new = *src_orig;
          new += 1;
        }

      src_orig++;
    }

  *new = '\0';

  /* Okay, now we have a *new* UTF-8 string, one that's guaranteed to
     contain only 7-bit bytes :-).  Recode to native... */
  err = convert_from_utf8 (((const char **) &new), new_orig, pool);

  if (err)
    {
      svn_error_clear (err);
      return new_orig;
    }
  else
    return (const char *) new;

  /* ### Check the client locale, maybe we can avoid that second
   * conversion!  See Ulrich Drepper's patch at
   * http://subversion.tigris.org/issues/show_bug.cgi?id=807.
   */
}


const char *
svn_utf_cstring_from_utf8_fuzzy (const char *src,
                                 apr_pool_t *pool)
{
  return svn_utf__cstring_from_utf8_fuzzy (src, pool,
                                           svn_utf_cstring_from_utf8);
}


svn_error_t *
svn_utf_cstring_from_utf8_stringbuf (const char **dest,
                                     const svn_stringbuf_t *src,
                                     apr_pool_t *pool)
{
  svn_stringbuf_t *destbuf;

  SVN_ERR (svn_utf_stringbuf_from_utf8 (&destbuf, src, pool));
  *dest = destbuf->data;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_utf_cstring_from_utf8_string (const char **dest,
                                  const svn_string_t *src,
                                  apr_pool_t *pool)
{
  svn_stringbuf_t *dbuf;
  apr_xlate_t *convset;

  SVN_ERR (get_uton_xlate_handle (&convset, pool));

  if (convset)
    {
      SVN_ERR (convert_to_stringbuf (convset, src->data, src->len,
                                     &dbuf, pool));
      *dest = dbuf->data;
      return SVN_NO_ERROR;
    }
  else
    {
      SVN_ERR (check_non_ascii (src->data, src->len, pool));
      *dest = apr_pstrmemdup (pool, src->data, src->len);
      return SVN_NO_ERROR;
    }
}
