/*
 * subst.c :  generic eol/keyword substitution routines
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
#include <string.h>
#include <assert.h>
#include <apr_general.h>  /* for strcasecmp() */
#include <apr_pools.h>
#include <apr_tables.h>
#include <apr_file_io.h>
#include <apr_strings.h>

#include "svn_cmdline.h"
#include "svn_types.h"
#include "svn_string.h"
#include "svn_time.h"
#include "svn_path.h"
#include "svn_error.h"
#include "svn_utf.h"
#include "svn_io.h"
#include "svn_subst.h"
#include "svn_pools.h"

#include "svn_private_config.h"

/**
 * The textual elements of a detranslated special file.  One of these
 * strings must appear as the first element of any special file as it
 * exists in the repository or the text base.
 */
#define SVN_SUBST__SPECIAL_LINK_STR "link"

void 
svn_subst_eol_style_from_value (svn_subst_eol_style_t *style,
                                const char **eol,
                                const char *value)
{
  if (value == NULL)
    {
      /* property doesn't exist. */
      *eol = NULL;
      if (style)
        *style = svn_subst_eol_style_none;
    }
  else if (! strcmp ("native", value))
    {
      *eol = APR_EOL_STR;       /* whee, a portability library! */
      if (style)
        *style = svn_subst_eol_style_native;
    }
  else if (! strcmp ("LF", value))
    {
      *eol = "\n";
      if (style)
        *style = svn_subst_eol_style_fixed;
    }
  else if (! strcmp ("CR", value))
    {
      *eol = "\r";
      if (style)
        *style = svn_subst_eol_style_fixed;
    }
  else if (! strcmp ("CRLF", value))
    {
      *eol = "\r\n";
      if (style)
        *style = svn_subst_eol_style_fixed;
    }
  else
    {
      *eol = NULL;
      if (style)
        *style = svn_subst_eol_style_unknown;
    }
}

/* A helper function to convert the date property to something suitable for 
   printing out.  If LONG_P is TRUE, use the long format, otherwise use a 
   shorter one.  Returns a UTF8 encoded cstring. */
static svn_error_t *
date_prop_to_human (const char **human, svn_boolean_t long_p, apr_time_t when,
                    apr_pool_t *pool)
{
  if (long_p)
    *human = svn_time_to_human_cstring (when, pool);
  else
    {
      apr_time_exp_t exploded_time;

      apr_time_exp_gmt (&exploded_time, when);

      *human = apr_psprintf (pool, "%04d-%02d-%02d %02d:%02d:%02dZ",
                             exploded_time.tm_year + 1900,
                             exploded_time.tm_mon + 1,
                             exploded_time.tm_mday,
                             exploded_time.tm_hour,
                             exploded_time.tm_min,
                             exploded_time.tm_sec);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_subst_build_keywords (svn_subst_keywords_t *kw,
                          const char *keywords_val,
                          const char *rev,
                          const char *url,
                          apr_time_t date,
                          const char *author,
                          apr_pool_t *pool)
{
  apr_array_header_t *keyword_tokens;
  int i;

  keyword_tokens = svn_cstring_split (keywords_val, " \t\v\n\b\r\f",
                                      TRUE /* chop */, pool);

  for (i = 0; i < keyword_tokens->nelts; ++i)
    {
      const char *keyword = APR_ARRAY_IDX (keyword_tokens, i, const char *);

      if ((! strcmp (keyword, SVN_KEYWORD_REVISION_LONG))
          || (! strcmp (keyword, SVN_KEYWORD_REVISION_MEDIUM))
          || (! strcasecmp (keyword, SVN_KEYWORD_REVISION_SHORT)))
        {
          kw->revision = svn_string_create (rev, pool);
        }      
      else if ((! strcmp (keyword, SVN_KEYWORD_DATE_LONG))
               || (! strcasecmp (keyword, SVN_KEYWORD_DATE_SHORT)))
        {
          if (date)
            {
              const char *human_date;

              SVN_ERR (date_prop_to_human (&human_date, TRUE, date, pool));

              kw->date = svn_string_create (human_date, pool);
            }
          else
            kw->date = svn_string_create ("", pool);
        }
      else if ((! strcmp (keyword, SVN_KEYWORD_AUTHOR_LONG))
               || (! strcasecmp (keyword, SVN_KEYWORD_AUTHOR_SHORT)))
        {
          kw->author = svn_string_create (author ? author : "", pool);
        }
      else if ((! strcmp (keyword, SVN_KEYWORD_URL_LONG))
               || (! strcasecmp (keyword, SVN_KEYWORD_URL_SHORT)))
        {
          kw->url = svn_string_create (url ? url : "", pool);
        }
      else if ((! strcasecmp (keyword, SVN_KEYWORD_ID)))
        {
          const char *base_name = url ? svn_path_basename (url, pool) : "";
          const char *human_date = NULL;

          if (date)
            SVN_ERR (date_prop_to_human (&human_date, FALSE, date, pool));

          kw->id = svn_string_createf (pool, "%s %s %s %s",
                                       base_name,
                                       rev,
                                       human_date ? human_date : "",
                                       author ? author : "");
        }
    }

  return SVN_NO_ERROR;
}


/*** Helpers for svn_subst_translate_stream ***/


/* Write out LEN bytes of BUF into STREAM. */
static svn_error_t *
translate_write (svn_stream_t *stream,
                 const void *buf,
                 apr_size_t len)
{
  apr_size_t wrote = len;
  svn_error_t *write_err = svn_stream_write (stream, buf, &wrote);
  if ((write_err) || (len != wrote))
    return write_err;

  return SVN_NO_ERROR;
}


/* Perform the substition of VALUE into keyword string BUF (with len
   *LEN), given a pre-parsed KEYWORD (and KEYWORD_LEN), and updating
   *LEN to the new size of the substituted result.  Return TRUE if all
   goes well, FALSE otherwise.  If VALUE is NULL, keyword will be
   contracted, else it will be expanded.  */
static svn_boolean_t
translate_keyword_subst (char *buf,
                         apr_size_t *len,
                         const char *keyword,
                         apr_size_t keyword_len,
                         const svn_string_t *value)
{
  char *buf_ptr;

  /* Make sure we gotz good stuffs. */
  assert (*len <= SVN_KEYWORD_MAX_LEN);
  assert ((buf[0] == '$') && (buf[*len - 1] == '$'));

  /* Need at least a keyword and two $'s. */
  if (*len < keyword_len + 2)
    return FALSE;

  /* The keyword needs to match what we're looking for. */
  if (strncmp (buf + 1, keyword, keyword_len))
    return FALSE;

  buf_ptr = buf + 1 + keyword_len;

  /* Check for fixed-length expansion. 
   * The format of fixed length keyword and its data is
   * Unexpanded keyword:         "$keyword::       $"
   * Expanded keyword:           "$keyword:: value $"
   * Expanded kw with filling:   "$keyword:: value   $"
   * Truncated keyword:          "$keyword:: longval#$"
   */
  if ((buf_ptr[0] == ':') /* first char after keyword is ':' */
      && (buf_ptr[1] == ':') /* second char after keyword is ':' */
      && (buf_ptr[2] == ' ') /* third char after keyword is ' ' */
      && ((buf[*len - 2] == ' ')  /* has ' ' for next to last character */
          || (buf[*len - 2] == '#')) /* .. or has '#' for next to last character */
      && ((6 + keyword_len) < *len))  /* holds "$kw:: x $" at least */
    {
      /* This is fixed length keyword, so *len remains unchanged */
      apr_size_t max_value_len = *len - (6 + keyword_len);

      if (! value)
        {
            /* no value, so unexpand */
            buf_ptr += 2;
            while (*buf_ptr != '$')
                *(buf_ptr++) = ' ';
        }
      else 
        {
          if (value->len <= max_value_len) 
            { /* replacement not as long as template, pad with spaces */
              strncpy (buf_ptr + 3, value->data, value->len);
              buf_ptr += 3 + value->len;
              while (*buf_ptr != '$')
                *(buf_ptr++) = ' ';
            }
          else
            {
              /* replacement needs truncating */
              strncpy (buf_ptr + 3, value->data, max_value_len);
              buf[*len - 2] = '#';
              buf[*len - 1] = '$';
            }
        }
      return TRUE;
    }

  /* Check for unexpanded keyword. */
  else if ((buf_ptr[0] == '$')          /* "$keyword$" */
           || ((buf_ptr[0] == ':') 
               && (buf_ptr[1] == '$'))) /* "$keyword:$" */
    {
      /* unexpanded... */
      if (value)
        {
          /* ...so expand. */
          buf_ptr[0] = ':';
          buf_ptr[1] = ' ';
          if (value->len)
            {
              apr_size_t vallen = value->len;

              /* "$keyword: value $" */
              if (vallen > (SVN_KEYWORD_MAX_LEN - 5))
                vallen = SVN_KEYWORD_MAX_LEN - 5;
              strncpy (buf_ptr + 2, value->data, vallen);
              buf_ptr[2 + vallen] = ' ';
              buf_ptr[2 + vallen + 1] = '$';
              *len = 5 + keyword_len + vallen;
            }
          else
            {
              /* "$keyword: $"  */
              buf_ptr[2] = '$';
              *len = 4 + keyword_len;
            }
        }
      else
        {
          /* ...but do nothing. */
        }
      return TRUE;
    }

  /* Check for expanded keyword. */
  else if ((*len >= 4 + keyword_len ) /* holds at least "$keyword: $" */
           && (buf_ptr[0] == ':')     /* first char after keyword is ':' */
           && (buf_ptr[1] == ' ')     /* second char after keyword is ' ' */
           && (buf[*len - 2] == ' ')) /* has ' ' for next to last character */
    {
      /* expanded... */
      if (! value)
        {
          /* ...so unexpand. */
          buf_ptr[0] = '$';
          *len = 2 + keyword_len;
        }
      else
        {
          /* ...so re-expand. */
          buf_ptr[0] = ':';
          buf_ptr[1] = ' ';
          if (value->len)
            {
              apr_size_t vallen = value->len;

              /* "$keyword: value $" */
              if (vallen > (SVN_KEYWORD_MAX_LEN - 5))
                vallen = SVN_KEYWORD_MAX_LEN - 5;
              strncpy (buf_ptr + 2, value->data, vallen);
              buf_ptr[2 + vallen] = ' ';
              buf_ptr[2 + vallen + 1] = '$';
              *len = 5 + keyword_len + vallen;
            }
          else
            {
              /* "$keyword: $"  */
              buf_ptr[2] = '$';
              *len = 4 + keyword_len;
            }
        }
      return TRUE;
    }
  
  return FALSE;
}                         

/* Parse BUF (whose length is *LEN) for Subversion keywords.  If a
   keyword is found, optionally perform the substitution on it in
   place, update *LEN with the new length of the translated keyword
   string, and return TRUE.  If this buffer doesn't contain a known
   keyword pattern, leave BUF and *LEN untouched and return FALSE.

   See the docstring for svn_subst_copy_and_translate for how the
   EXPAND and KEYWORDS parameters work.

   NOTE: It is assumed that BUF has been allocated to be at least
   SVN_KEYWORD_MAX_LEN bytes longs, and that the data in BUF is less
   than or equal SVN_KEYWORD_MAX_LEN in length.  Also, any expansions
   which would result in a keyword string which is greater than
   SVN_KEYWORD_MAX_LEN will have their values truncated in such a way
   that the resultant keyword string is still valid (begins with
   "$Keyword:", ends in " $" and is SVN_KEYWORD_MAX_LEN bytes long).  */
static svn_boolean_t
translate_keyword (char *buf,
                   apr_size_t *len,
                   svn_boolean_t expand,
                   const svn_subst_keywords_t *keywords)
{
  /* Make sure we gotz good stuffs. */
  assert (*len <= SVN_KEYWORD_MAX_LEN);
  assert ((buf[0] == '$') && (buf[*len - 1] == '$'));

  /* Early return for ignored keywords */
  if (! keywords)
    return FALSE;

  /* Revision */
  if (keywords->revision)
    {
      if (translate_keyword_subst (buf, len,
                                   SVN_KEYWORD_REVISION_LONG,
                                   (sizeof (SVN_KEYWORD_REVISION_LONG)) - 1,
                                   expand ? keywords->revision : NULL))
        return TRUE;

      if (translate_keyword_subst (buf, len,
                                   SVN_KEYWORD_REVISION_MEDIUM,
                                   (sizeof (SVN_KEYWORD_REVISION_MEDIUM)) - 1,
                                   expand ? keywords->revision : NULL))
        return TRUE;

      if (translate_keyword_subst (buf, len,
                                   SVN_KEYWORD_REVISION_SHORT,
                                   (sizeof (SVN_KEYWORD_REVISION_SHORT)) - 1,
                                   expand ? keywords->revision : NULL))
        return TRUE;
    }

  /* Date */
  if (keywords->date)
    {
      if (translate_keyword_subst (buf, len,
                                   SVN_KEYWORD_DATE_LONG,
                                   (sizeof (SVN_KEYWORD_DATE_LONG)) - 1,
                                   expand ? keywords->date : NULL))
        return TRUE;

      if (translate_keyword_subst (buf, len,
                                   SVN_KEYWORD_DATE_SHORT,
                                   (sizeof (SVN_KEYWORD_DATE_SHORT)) - 1,
                                   expand ? keywords->date : NULL))
        return TRUE;
    }

  /* Author */
  if (keywords->author)
    {
      if (translate_keyword_subst (buf, len,
                                   SVN_KEYWORD_AUTHOR_LONG,
                                   (sizeof (SVN_KEYWORD_AUTHOR_LONG)) - 1,
                                   expand ? keywords->author : NULL))
        return TRUE;

      if (translate_keyword_subst (buf, len,
                                   SVN_KEYWORD_AUTHOR_SHORT,
                                   (sizeof (SVN_KEYWORD_AUTHOR_SHORT)) - 1,
                                   expand ? keywords->author : NULL))
        return TRUE;
    }

  /* URL */
  if (keywords->url)
    {
      if (translate_keyword_subst (buf, len,
                                   SVN_KEYWORD_URL_LONG,
                                   (sizeof (SVN_KEYWORD_URL_LONG)) - 1,
                                   expand ? keywords->url : NULL))
        return TRUE;

      if (translate_keyword_subst (buf, len,
                                   SVN_KEYWORD_URL_SHORT,
                                   (sizeof (SVN_KEYWORD_URL_SHORT)) - 1,
                                   expand ? keywords->url : NULL))
        return TRUE;
    }

  /* Id */
  if (keywords->id)
    {
      if (translate_keyword_subst (buf, len,
                                   SVN_KEYWORD_ID,
                                   (sizeof (SVN_KEYWORD_ID)) - 1,
                                   expand ? keywords->id : NULL))
        return TRUE;
    }

  /* No translations were successful.  Return FALSE. */
  return FALSE;
}


/* Translate NEWLINE_BUF (length of NEWLINE_LEN) to the newline format
   specified in EOL_STR (length of EOL_STR_LEN), and write the
   translated thing to FILE (whose path is DST_PATH).  

   SRC_FORMAT (length *SRC_FORMAT_LEN) is a cache of the first newline
   found while processing SRC_PATH.  If the current newline is not the
   same style as that of SRC_FORMAT, look to the REPAIR parameter.  If
   REPAIR is TRUE, ignore the inconsistency, else return an
   SVN_ERR_IO_INCONSISTENT_EOL error.  If we are examining the first
   newline in the file, copy it to {SRC_FORMAT, *SRC_FORMAT_LEN} to
   use for later consistency checks. */
static svn_error_t *
translate_newline (const char *eol_str,
                   apr_size_t eol_str_len,
                   char *src_format,
                   apr_size_t *src_format_len,
                   char *newline_buf,
                   apr_size_t newline_len,
                   svn_stream_t *dst,
                   svn_boolean_t repair)
{
  /* If this is the first newline we've seen, cache it
     future comparisons, else compare it with our cache to
     check for consistency. */
  if (*src_format_len)
    {
      /* Comparing with cache.  If we are inconsistent and
         we are NOT repairing the file, generate an error! */
      if ((! repair) &&
          ((*src_format_len != newline_len) ||
           (strncmp (src_format, newline_buf, newline_len)))) 
        return svn_error_create (SVN_ERR_IO_INCONSISTENT_EOL, NULL, NULL);
    }
  else
    {
      /* This is our first line ending, so cache it before
         handling it. */
      strncpy (src_format, newline_buf, newline_len);
      *src_format_len = newline_len;
    }
  /* Translate the newline */
  return translate_write (dst, eol_str, eol_str_len);
}



/*** Public interfaces. ***/

svn_boolean_t
svn_subst_keywords_differ (const svn_subst_keywords_t *a,
                           const svn_subst_keywords_t *b,
                           svn_boolean_t compare_values)
{
  if (((a == NULL) && (b == NULL)) /* no A or B */
      /* no A, and B has no contents */
      || ((a == NULL) 
          && (b->revision == NULL)
          && (b->date == NULL)
          && (b->author == NULL)
          && (b->url == NULL))
      /* no B, and A has no contents */
      || ((b == NULL)           && (a->revision == NULL)
          && (a->date == NULL)
          && (a->author == NULL)
          && (a->url == NULL))
      /* neither A nor B has any contents */
      || ((a != NULL) && (b != NULL) 
          && (b->revision == NULL)
          && (b->date == NULL)
          && (b->author == NULL)
          && (b->url == NULL)
          && (a->revision == NULL)
          && (a->date == NULL)
          && (a->author == NULL)
          && (a->url == NULL)))
    {
      return FALSE;
    }
  else if ((a == NULL) || (b == NULL))
    return TRUE;
  
  /* Else both A and B have some keywords. */
  
  if ((! a->revision) != (! b->revision))
    return TRUE;
  else if ((compare_values && (a->revision != NULL))
           && (strcmp (a->revision->data, b->revision->data) != 0))
    return TRUE;
    
  if ((! a->date) != (! b->date))
    return TRUE;
  else if ((compare_values && (a->date != NULL))
           && (strcmp (a->date->data, b->date->data) != 0))
    return TRUE;
    
  if ((! a->author) != (! b->author))
    return TRUE;
  else if ((compare_values && (a->author != NULL))
           && (strcmp (a->author->data, b->author->data) != 0))
    return TRUE;
  
  if ((! a->url) != (! b->url))
    return TRUE;
  else if ((compare_values && (a->url != NULL))
           && (strcmp (a->url->data, b->url->data) != 0))
    return TRUE;
  
  /* Else we never found a difference, so they must be the same. */  
  
  return FALSE;
}


svn_error_t *
svn_subst_translate_stream (svn_stream_t *s, /* src stream */
                            svn_stream_t *d, /* dst stream */
                            const char *eol_str,
                            svn_boolean_t repair,
                            const svn_subst_keywords_t *keywords,
                            svn_boolean_t expand)
{
  char buf[SVN_STREAM_CHUNK_SIZE + 1];
  const char *p, *interesting;
  apr_size_t len, readlen;
  apr_size_t eol_str_len = eol_str ? strlen (eol_str) : 0;
  char       newline_buf[2] = { 0 };
  apr_size_t newline_off = 0;
  char       keyword_buf[SVN_KEYWORD_MAX_LEN] = { 0 };
  apr_size_t keyword_off = 0;
  char       src_format[2] = { 0 };
  apr_size_t src_format_len = 0;

  /* The docstring requires that *some* translation be requested. */
  assert (eol_str || keywords);
  interesting = (eol_str && keywords) ? "$\r\n" : eol_str ? "\r\n" : "$";

  readlen = sizeof (buf) - 1;
  while (readlen == sizeof (buf) - 1)
    {
      SVN_ERR (svn_stream_read (s, buf, &readlen));
      buf[readlen] = '\0';

      /* At the beginning of this loop, assume that we might be in an
       * interesting state, i.e. with data in the newline or keyword
       * buffer.  First try to get to the boring state so we can copy
       * a run of boring characters; then try to get back to the
       * interesting state by processing an interesting character,
       * and repeat. */
      for (p = buf; p < buf + readlen;)
        {
          /* Try to get to the boring state, if necessary. */
          if (newline_off)
            {
              if (*p == '\n')
                newline_buf[newline_off++] = *p++;
              SVN_ERR (translate_newline (eol_str, eol_str_len, src_format,
                                          &src_format_len, newline_buf,
                                          newline_off, d, repair));
              newline_off = 0;
            }
          else if (keyword_off && *p == '$')
            {
              /* If translation fails, treat this '$' as a starting '$'. */
              keyword_buf[keyword_off++] = '$';
              if (translate_keyword (keyword_buf, &keyword_off, expand,
                                     keywords))
                p++;
              else
                keyword_off--;
              SVN_ERR (translate_write (d, keyword_buf, keyword_off));
              keyword_off = 0;
            }
          else if (keyword_off == SVN_KEYWORD_MAX_LEN - 1
                   || (keyword_off && (*p == '\r' || *p == '\n')))
            {
              /* No closing '$' found; flush the keyword buffer. */
              SVN_ERR (translate_write (d, keyword_buf, keyword_off));
              keyword_off = 0;
            }
          else if (keyword_off)
            {
              keyword_buf[keyword_off++] = *p++;
              continue;
            }

          /* We're in the boring state; look for interest characters.
           * For lack of a memcspn(), manually skip past NULs. */
          len = 0;
          while (1)
            {
              len += strcspn (p + len, interesting);
              if (p[len] != '\0' || p + len == buf + readlen)
                break;
              len++;
            }
          if (len)
            SVN_ERR (translate_write (d, p, len));
          p += len;

          /* Set up state according to the interesting character, if any. */
          switch (*p)
            {
            case '$':
              keyword_buf[keyword_off++] = *p++;
              break;
            case '\r':
              newline_buf[newline_off++] = *p++;
              break;
            case '\n':
              newline_buf[newline_off++] = *p++;
              SVN_ERR (translate_newline (eol_str, eol_str_len, src_format,
                                          &src_format_len, newline_buf,
                                          newline_off, d, repair));
              newline_off = 0;
              break;
            }
        }
    }

  if (newline_off)
    SVN_ERR (translate_newline (eol_str, eol_str_len, src_format,
                                &src_format_len, newline_buf, newline_off, d,
                                repair));
  if (keyword_off)
    SVN_ERR (translate_write (d, keyword_buf, keyword_off));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_subst_translate_cstring (const char *src,
                             const char **dst,
                             const char *eol_str,
                             svn_boolean_t repair,
                             const svn_subst_keywords_t *keywords,
                             svn_boolean_t expand,
                             apr_pool_t *pool)
{
  svn_stringbuf_t *src_stringbuf, *dst_stringbuf;
  svn_stream_t *src_stream, *dst_stream;
  svn_error_t *err;

  src_stringbuf = svn_stringbuf_create (src, pool);
  
  /* The easy way out:  no translation needed, just copy. */
  if (! (eol_str || keywords))
    {
      dst_stringbuf = svn_stringbuf_dup (src_stringbuf, pool);
      goto all_good;
    }

  /* Convert our stringbufs into streams. */
  src_stream = svn_stream_from_stringbuf (src_stringbuf, pool);
  dst_stringbuf = svn_stringbuf_create ("", pool);
  dst_stream = svn_stream_from_stringbuf (dst_stringbuf, pool);

  /* Translate src stream into dst stream. */
  err = svn_subst_translate_stream (src_stream, dst_stream,
                                    eol_str, repair, keywords, expand);
  if (err)
    {
      svn_error_clear (svn_stream_close (src_stream));
      svn_error_clear (svn_stream_close (dst_stream));
      return err;
    }

  /* clean up nicely. */
  SVN_ERR (svn_stream_close (src_stream));
  SVN_ERR (svn_stream_close (dst_stream));

 all_good:
  *dst = dst_stringbuf->data;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_subst_copy_and_translate (const char *src,
                              const char *dst,
                              const char *eol_str,
                              svn_boolean_t repair,
                              const svn_subst_keywords_t *keywords,
                              svn_boolean_t expand,
                              apr_pool_t *pool)
{
  return svn_subst_copy_and_translate2 (src, dst, eol_str, repair, keywords,
                                        expand, FALSE, pool);
}


/* Given a special file at SRC, generate a textual representation of
   it in a normal file at DST.  Perform all allocations in POOL. */
static svn_error_t *
detranslate_special_file (const char *src,
                          const char *dst,
                          apr_pool_t *pool)
{
  const char *dst_tmp;
  svn_string_t *buf;
  apr_file_t *s, *d;
  svn_stream_t *src_stream, *dst_stream;
  apr_finfo_t finfo;
  
  /* First determine what type of special file we are
     detranslating. */
  SVN_ERR (svn_io_stat (&finfo, src, APR_FINFO_MIN | APR_FINFO_LINK, pool));

  /* Open a temporary destination that we will eventually atomically
     rename into place. */
  SVN_ERR (svn_io_open_unique_file (&d, &dst_tmp, dst,
                                    ".tmp", FALSE, pool));

  dst_stream = svn_stream_from_aprfile (d, pool);
  
  switch (finfo.filetype) {
  case APR_REG:
    /* Nothing special to do here, just copy the original file's
       contents. */
    SVN_ERR (svn_io_file_open (&s, src, APR_READ | APR_BUFFERED,
                               APR_OS_DEFAULT, pool));
    src_stream = svn_stream_from_aprfile (s, pool);

    SVN_ERR (svn_stream_copy (src_stream, dst_stream, pool));
    break;
  case APR_LNK:
    /* Determine the destination of the link. */
    SVN_ERR (svn_io_read_link (&buf, src, pool));

    SVN_ERR (svn_stream_printf (dst_stream, pool, "link %s",
                                buf->data));
    break;
  default:
    abort ();
  }

  SVN_ERR (svn_io_file_close (d, pool));

  /* Do the atomic rename from our temporary location. */
  SVN_ERR (svn_io_file_rename (dst_tmp, dst, pool));
  
  return SVN_NO_ERROR;
}


/* Given a file containing a repository representation of a special
   file in SRC, create the appropriate special file at location DST.
   Perform all allocations in POOL. */
static svn_error_t *
create_special_file (const char *src,
                     const char *dst,
                     apr_pool_t *pool)
{
  svn_stringbuf_t *contents;
  char *identifier, *remainder;
  const char *dst_tmp, *src_tmp = NULL;
  svn_error_t *err;
  svn_node_kind_t kind;
  svn_boolean_t is_special;

  /* Check to see if we are being asked to create a special file from
     a special file.  If so, do a temporary detranslation and work
     from there. */
  SVN_ERR (svn_io_check_special_path (src, &kind, &is_special, pool));

  if (is_special)
    {
      apr_file_t *fp;
      
      SVN_ERR (svn_io_open_unique_file (&fp, &src_tmp, dst, ".tmp", FALSE,
                                        pool));
      SVN_ERR (svn_io_file_close (fp, pool));
      SVN_ERR (detranslate_special_file (src, src_tmp, pool));
      src = src_tmp;
    }
  
  /* Read in the detranslated file. */
  SVN_ERR (svn_stringbuf_from_file (&contents, src, pool));

  /* If there was just a temporary detranslation, remove it now. */
  if (src_tmp)
    SVN_ERR (svn_io_remove_file (src_tmp, pool));
      
  /* Separate off the identifier.  The first space character delimits
     the identifier, after which any remaining characters are specific
     to the actual special device being created. */
  identifier = contents->data;
  for (remainder = identifier; *remainder; remainder++)
    {
      if (*remainder == ' ')
        {
          *remainder = '\0';
          remainder++;
          break;
        }
    }
           
  if (! strcmp (identifier, SVN_SUBST__SPECIAL_LINK_STR))
    {
      /* For symlinks, the type specific data is just a filesystem
         path that the symlink should reference. */
      err = svn_io_create_unique_link (&dst_tmp, dst, remainder,
                                       ".tmp", pool);
    }
  else
    {
      /* We should return a valid error here. */
      return svn_error_createf (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                                "Unsupported special file type '%s'",
                                identifier);
    }

  /* If we had an error, check to see if it was because this type of
     special device is not supported. */
  if (err)
    {
      if (err->apr_err == SVN_ERR_UNSUPPORTED_FEATURE)
        {
          apr_file_t *fp;
          
          svn_error_clear (err);
          /* Fall back to just copying the text-base. */
          SVN_ERR (svn_io_open_unique_file (&fp, &dst_tmp, dst, ".tmp", FALSE,
                                            pool));
          SVN_ERR (svn_io_file_close (fp, pool));
          SVN_ERR (svn_io_copy_file (src, dst_tmp, TRUE, pool));
        }
      else
        return err;
    }

  /* Do the atomic rename from our temporary location. */
  SVN_ERR (svn_io_file_rename (dst_tmp, dst, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_subst_copy_and_translate2 (const char *src,
                               const char *dst,
                               const char *eol_str,
                               svn_boolean_t repair,
                               const svn_subst_keywords_t *keywords,
                               svn_boolean_t expand,
                               svn_boolean_t special,
                               apr_pool_t *pool)
{
  const char *dst_tmp = NULL;
  svn_stream_t *src_stream, *dst_stream;
  apr_file_t *s = NULL, *d = NULL;  /* init to null important for APR */
  svn_error_t *err;
  apr_pool_t *subpool;
  svn_node_kind_t kind;
  svn_boolean_t path_special;

  SVN_ERR (svn_io_check_special_path (src, &kind, &path_special, pool));

  /* If this is a 'special' file, we may need to create it or
     detranslate it. */
  if (special || path_special)
    {
      if (expand)
        SVN_ERR (create_special_file (src, dst, pool));
      else
        SVN_ERR (detranslate_special_file (src, dst, pool));
      
      return SVN_NO_ERROR;
    }

  /* The easy way out:  no translation needed, just copy. */
  if (! (eol_str || keywords))
    return svn_io_copy_file (src, dst, FALSE, pool);

  subpool = svn_pool_create (pool);

  /* Open source file. */
  err = svn_io_file_open (&s, src, APR_READ | APR_BUFFERED,
                          APR_OS_DEFAULT, subpool);
  if (err)
    goto error;

  /* For atomicity, we translate to a tmp file and
     then rename the tmp file over the real destination. */

  err = svn_io_open_unique_file (&d, &dst_tmp, dst,
                                 ".tmp", FALSE, subpool);

  /* Move the file name to a more permanent pool. */
  if (dst_tmp)
    dst_tmp = apr_pstrdup(pool, dst_tmp);

  if (err)
    goto error;

  /* Now convert our two open files into streams. */
  src_stream = svn_stream_from_aprfile (s, subpool);
  dst_stream = svn_stream_from_aprfile (d, subpool);

  /* Translate src stream into dst stream. */
  err = svn_subst_translate_stream (src_stream, dst_stream,
                                    eol_str, repair, keywords, expand);
  if (err)
    {
      if (err->apr_err == SVN_ERR_IO_INCONSISTENT_EOL)
        err = svn_error_createf 
          (SVN_ERR_IO_INCONSISTENT_EOL, err,
           _("File '%s' has inconsistent newlines"),
           svn_path_local_style (src, pool));
      goto error;
    }

  /* clean up nicely. */
  err = svn_stream_close (src_stream);
  if (err)
    goto error;

  err = svn_stream_close (dst_stream);
  if (err)
    goto error;

  err = svn_io_file_close (s, subpool);
  if (err)
    goto error;

  err = svn_io_file_close (d, subpool);
  if (err)
    goto error;

  /* Now that dst_tmp contains the translated data, do the atomic rename. */
  err = svn_io_file_rename (dst_tmp, dst, subpool);
  if (err)
    goto error;

  svn_pool_destroy (subpool);
  return SVN_NO_ERROR;

 error:
  svn_pool_destroy (subpool);   /* Make sure all files are closed first. */
  if (dst_tmp)
    svn_error_clear (svn_io_remove_file (dst_tmp, pool));
  return err;
}



svn_error_t *
svn_subst_translate_string (svn_string_t **new_value,
                            const svn_string_t *value,
                            const char *encoding,
                            apr_pool_t *pool)
{
  const char *val_utf8;
  const char *val_utf8_lf;

  if (value == NULL)
    {
      *new_value = NULL;
      return SVN_NO_ERROR;
    }

  if (encoding)
    {
      SVN_ERR (svn_utf_cstring_to_utf8_ex (&val_utf8, value->data,
                                           encoding, NULL, pool));
    }
  else
    {
      SVN_ERR (svn_utf_cstring_to_utf8 (&val_utf8, value->data, pool));
    }

  SVN_ERR (svn_subst_translate_cstring (val_utf8,
                                        &val_utf8_lf,
                                        "\n",  /* translate to LF */
                                        FALSE, /* no repair */
                                        NULL,  /* no keywords */
                                        FALSE, /* no expansion */
                                        pool));
  
  *new_value = svn_string_create (val_utf8_lf, pool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_subst_detranslate_string (svn_string_t **new_value,
                              const svn_string_t *value,
                              svn_boolean_t for_output,
                              apr_pool_t *pool)
{
  svn_error_t *err;
  const char *val_neol;
  const char *val_nlocale_neol;

  if (value == NULL)
    {
      *new_value = NULL;
      return SVN_NO_ERROR;
    }

  SVN_ERR (svn_subst_translate_cstring (value->data,
                                        &val_neol,
                                        APR_EOL_STR,  /* 'native' eol */
                                        FALSE, /* no repair */
                                        NULL,  /* no keywords */
                                        FALSE, /* no expansion */
                                        pool));

  if (for_output)
    {
      err = svn_cmdline_cstring_from_utf8 (&val_nlocale_neol, val_neol, pool);
      if (err && (APR_STATUS_IS_EINVAL (err->apr_err)))
        {
          val_nlocale_neol =
            svn_cmdline_cstring_from_utf8_fuzzy (val_neol, pool);
          svn_error_clear (err);
        }
      else if (err)
        return err;
    }
  else
    {
      err = svn_utf_cstring_from_utf8 (&val_nlocale_neol, val_neol, pool);
      if (err && (APR_STATUS_IS_EINVAL (err->apr_err)))
        {
          val_nlocale_neol = svn_utf_cstring_from_utf8_fuzzy (val_neol, pool);
          svn_error_clear (err);
        }
      else if (err)
        return err;
    }

  *new_value = svn_string_create (val_nlocale_neol, pool);

  return SVN_NO_ERROR;
}
