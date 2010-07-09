/*
 * parse-diff.c: functions for parsing diff files
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <limits.h>  /* for ULONG_MAX */
#include <stdlib.h>
#include <string.h>

#include "svn_types.h"
#include "svn_error.h"
#include "svn_io.h"
#include "svn_pools.h"
#include "svn_utf.h"
#include "svn_dirent_uri.h"
#include "svn_diff.h"

#include "private/svn_eol_private.h"

/* Helper macro for readability */
#define starts_with(str, start)  \
  (strncmp((str), (start), strlen(start)) == 0)

struct svn_hunk_t {
  /* Hunk texts (see include/svn_diff.h). */
  svn_stream_t *diff_text;
  svn_stream_t *original_text;
  svn_stream_t *modified_text;

  /* Whether the hunk is being interpreted in reverse. */
  svn_boolean_t reverse;

  /* Hunk ranges as they appeared in the patch file.
   * All numbers are lines, not bytes. */
  svn_linenum_t original_start;
  svn_linenum_t original_length;
  svn_linenum_t modified_start;
  svn_linenum_t modified_length;

  /* Number of lines of leading and trailing hunk context. */
  svn_linenum_t leading_context;
  svn_linenum_t trailing_context;
};

svn_error_t *
svn_diff_hunk_reset_diff_text(const svn_hunk_t *hunk)
{
  return svn_error_return(svn_stream_reset(hunk->diff_text));
}

svn_error_t *
svn_diff_hunk_reset_original_text(const svn_hunk_t *hunk)
{
  return svn_error_return(svn_stream_reset(hunk->original_text));
}

svn_error_t *
svn_diff_hunk_reset_modified_text(const svn_hunk_t *hunk)
{
  return svn_error_return(svn_stream_reset(hunk->modified_text));
}

svn_linenum_t
svn_diff_hunk_get_original_start(const svn_hunk_t *hunk)
{
  return hunk->original_start;
}

svn_linenum_t
svn_diff_hunk_get_original_length(const svn_hunk_t *hunk)
{
  return hunk->original_length;
}

svn_linenum_t
svn_diff_hunk_get_modified_start(const svn_hunk_t *hunk)
{
  return hunk->modified_start;
}

svn_linenum_t
svn_diff_hunk_get_modified_length(const svn_hunk_t *hunk)
{
  return hunk->modified_length;
}

svn_linenum_t
svn_diff_hunk_get_leading_context(const svn_hunk_t *hunk)
{
  return hunk->leading_context;
}

svn_linenum_t
svn_diff_hunk_get_trailing_context(const svn_hunk_t *hunk)
{
  return hunk->trailing_context;
}

/* Try to parse a positive number from a decimal number encoded
 * in the string NUMBER. Return parsed number in OFFSET, and return
 * TRUE if parsing was successful. */
static svn_boolean_t
parse_offset(svn_linenum_t *offset, const char *number)
{
  apr_int64_t parsed_offset;

  errno = 0; /* apr_atoi64() in APR-0.9 does not always set errno */
  parsed_offset = apr_atoi64(number);
  if (errno == ERANGE || parsed_offset < 0)
    return FALSE;

  /* In case we cannot fit 64 bits into an svn_linenum_t,
   * check for overflow. */
  if (sizeof(svn_linenum_t) < sizeof(parsed_offset) &&
      parsed_offset > SVN_LINENUM_MAX_VALUE)
    return FALSE;

  *offset = parsed_offset;
  return TRUE;
}

/* Try to parse a hunk range specification from the string RANGE.
 * Return parsed information in *START and *LENGTH, and return TRUE
 * if the range parsed correctly. Note: This function may modify the
 * input value RANGE. */
static svn_boolean_t
parse_range(svn_linenum_t *start, svn_linenum_t *length, char *range)
{
  char *comma;

  if (strlen(range) == 0)
    return FALSE;

  comma = strstr(range, ",");
  if (comma)
    {
      if (strlen(comma + 1) > 0)
        {
          /* Try to parse the length. */
          if (! parse_offset(length, comma + 1))
            return FALSE;

          /* Snip off the end of the string,
           * so we can comfortably parse the line
           * number the hunk starts at. */
          *comma = '\0';
        }
       else
         /* A comma but no length? */
         return FALSE;
    }
  else
    {
      *length = 1;
    }

  /* Try to parse the line number the hunk starts at. */
  return parse_offset(start, range);
}

/* Try to parse a hunk header in string HEADER, putting parsed information
 * into HUNK. Return TRUE if the header parsed correctly. ATAT is the
 * character string used to delimit the hunk header.
 * If REVERSE is TRUE, invert the hunk header while parsing it.
 * Do all allocations in POOL. */
static svn_boolean_t
parse_hunk_header(const char *header, svn_hunk_t *hunk,
                  const char *atat, svn_boolean_t reverse, apr_pool_t *pool)
{
  const char *p;
  svn_stringbuf_t *range;

  p = header + strlen(atat);
  if (*p != ' ')
    /* No. */
    return FALSE;
  p++;
  if (*p != '-')
    /* Nah... */
    return FALSE;
  /* OK, this may be worth allocating some memory for... */
  range = svn_stringbuf_create_ensure(31, pool);
  p++;
  while (*p && *p != ' ')
    {
      svn_stringbuf_appendbytes(range, p, 1);
      p++;
    }
  if (*p != ' ')
    /* No no no... */
    return FALSE;

  /* Try to parse the first range. */
  if (reverse)
    {
      if (! parse_range(&hunk->modified_start, &hunk->modified_length, range->data))
        return FALSE;
    }
  else
    {
      if (! parse_range(&hunk->original_start, &hunk->original_length, range->data))
        return FALSE;
    }

  /* Clear the stringbuf so we can reuse it for the second range. */
  svn_stringbuf_setempty(range);
  p++;
  if (*p != '+')
    /* Eeek! */
    return FALSE;
  /* OK, this may be worth copying... */
  p++;
  while (*p && *p != ' ')
    {
      svn_stringbuf_appendbytes(range, p, 1);
      p++;
    }
  if (*p != ' ')
    /* No no no... */
    return FALSE;

  /* Check for trailing @@ */
  p++;
  if (! starts_with(p, atat))
    return FALSE;

  /* There may be stuff like C-function names after the trailing @@,
   * but we ignore that. */

  /* Try to parse the second range. */
  if (reverse)
    {
      if (! parse_range(&hunk->original_start, &hunk->original_length, range->data))
        return FALSE;
    }
  else
    {
      if (! parse_range(&hunk->modified_start, &hunk->modified_length, range->data))
        return FALSE;
    }

  /* Hunk header is good. */
  return TRUE;
}

/* Set *EOL to the first end-of-line string found in the stream
 * accessed through READ_FN, MARK_FN and SEEK_FN, whose stream baton
 * is BATON.  Leave the stream read position unchanged.
 * Allocate *EOL statically; POOL is a scratch pool. */
static svn_error_t *
scan_eol(const char **eol, svn_stream_t *stream, apr_pool_t *pool)
{
  const char *eol_str;
  svn_stream_mark_t *mark;

  SVN_ERR(svn_stream_mark(stream, &mark, pool));

  eol_str = NULL;
  while (! eol_str)
    {
      char buf[512];
      apr_size_t len;

      len = sizeof(buf);
      SVN_ERR(svn_stream_read(stream, buf, &len));
      if (len == 0)
        break; /* EOF */
      eol_str = svn_eol__detect_eol(buf, buf + len);
    }

  SVN_ERR(svn_stream_seek(stream, mark));

  *eol = eol_str;

  return SVN_NO_ERROR;
}

/* A helper function similar to svn_stream_readline_detect_eol(),
 * suitable for reading original or modified hunk text from a STREAM
 * which has been mapped onto a hunk region within a unidiff patch file.
 *
 * Allocate *STRINGBUF in RESULT_POOL, and read into it one line from STREAM.
 *
 * STREAM is expected to contain unidiff text.
 * Leading unidiff symbols ('+', '-', and ' ') are removed from the line,
 * Any lines commencing with the VERBOTEN character are discarded.
 * VERBOTEN should be '+' or '-', depending on which form of hunk text
 * is being read.
 *
 * The line-terminator is detected automatically and stored in *EOL
 * if EOL is not NULL. If EOF is reached and the stream does not end
 * with a newline character, and EOL is not NULL, *EOL is set to NULL.
 *
 * SCRATCH_POOL is used for temporary allocations.
 */
static svn_error_t *
hunk_readline(svn_stream_t *stream,
              svn_stringbuf_t **stringbuf,
              const char **eol,
              svn_boolean_t *eof,
              char verboten,
              apr_pool_t *result_pool,
              apr_pool_t *scratch_pool)
{
  svn_stringbuf_t *str;
  apr_pool_t *iterpool;
  svn_boolean_t filtered;
  const char *eol_str;

  *eof = FALSE;

  iterpool = svn_pool_create(scratch_pool);
  do
    {
      apr_size_t numbytes;
      const char *match;
      char c;

      svn_pool_clear(iterpool);

      /* Since we're reading one character at a time, let's at least
         optimize for the 90% case.  90% of the time, we can avoid the
         stringbuf ever having to realloc() itself if we start it out at
         80 chars.  */
      str = svn_stringbuf_create_ensure(80, iterpool);

      SVN_ERR(scan_eol(&eol_str, stream, iterpool));
      if (eol)
        *eol = eol_str;
      if (eol_str == NULL)
        {
          /* No newline until EOF, EOL_STR can be anything. */
          eol_str = APR_EOL_STR;
        }

      /* Read into STR up to and including the next EOL sequence. */
      match = eol_str;
      numbytes = 1;
      while (*match)
        {
          SVN_ERR(svn_stream_read(stream, &c, &numbytes));
          if (numbytes != 1)
            {
              /* a 'short' read means the stream has run out. */
              *eof = TRUE;
              /* We know we don't have a whole EOL sequence, but ensure we
               * don't chop off any partial EOL sequence that we may have. */
              match = eol_str;
              /* Process this short (or empty) line just like any other
               * except with *EOF set. */
              break;
            }

          if (c == *match)
            match++;
          else
            match = eol_str;

          svn_stringbuf_appendbytes(str, &c, 1);
        }

      svn_stringbuf_chop(str, match - eol_str);
      filtered = (str->data[0] == verboten || str->data[0] == '\\');
    }
  while (filtered && ! *eof);
  /* Not destroying the iterpool just yet since we still need STR
   * which is allocated in it. */

  if (filtered)
    {
      /* EOF, return an empty string. */
      *stringbuf = svn_stringbuf_create_ensure(0, result_pool);
    }
  else if (str->data[0] == '+' || str->data[0] == '-' || str->data[0] == ' ')
    {
      /* Shave off leading unidiff symbols. */
      *stringbuf = svn_stringbuf_create(str->data + 1, result_pool);
    }
  else
    {
      /* Return the line as-is. */
      *stringbuf = svn_stringbuf_dup(str, result_pool);
    }

  /* Done. RIP iterpool. */
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_diff_hunk_readline_original_text(const svn_hunk_t *hunk,
                                     svn_stringbuf_t **stringbuf,
                                     const char **eol,
                                     svn_boolean_t *eof,
                                     apr_pool_t *result_pool,
                                     apr_pool_t *scratch_pool)
{
  return svn_error_return(hunk_readline(hunk->original_text, stringbuf,
                                        eol, eof, hunk->reverse ? '-' : '+',
                                        result_pool, scratch_pool));
}

svn_error_t *
svn_diff_hunk_readline_modified_text(const svn_hunk_t *hunk,
                                     svn_stringbuf_t **stringbuf,
                                     const char **eol,
                                     svn_boolean_t *eof,
                                     apr_pool_t *result_pool,
                                     apr_pool_t *scratch_pool)
{
  return svn_error_return(hunk_readline(hunk->modified_text, stringbuf,
                                        eol, eof, hunk->reverse ? '+' : '-',
                                        result_pool, scratch_pool));
}

svn_error_t *
svn_diff_hunk_readline_diff_text(const svn_hunk_t *hunk,
                                 svn_stringbuf_t **stringbuf,
                                 const char **eol,
                                 svn_boolean_t *eof,
                                 apr_pool_t *result_pool,
                                 apr_pool_t *scratch_pool)
{
  svn_hunk_t dummy;
  svn_stringbuf_t *line;

  SVN_ERR(svn_stream_readline_detect_eol(hunk->diff_text, &line, eol, eof,
                                         result_pool));
  
  if (hunk->reverse)
    {
      if (parse_hunk_header(line->data, &dummy, "@@", FALSE, scratch_pool))
        {
          /* Line is a hunk header, reverse it. */
          *stringbuf = svn_stringbuf_createf(result_pool,
                                             "@@ -%lu,%lu +%lu,%lu @@",
                                             hunk->modified_start,
                                             hunk->modified_length,
                                             hunk->original_start,
                                             hunk->original_length);
        }
      else if (parse_hunk_header(line->data, &dummy, "##", FALSE, scratch_pool))
        {
          /* Line is a hunk header, reverse it. */
          *stringbuf = svn_stringbuf_createf(result_pool,
                                             "## -%lu,%lu +%lu,%lu ##",
                                             hunk->modified_start,
                                             hunk->modified_length,
                                             hunk->original_start,
                                             hunk->original_length);
        }
      else
        {
          if (line->data[0] == '+')
            line->data[0] = '-';
          else if (line->data[0] == '-')
            line->data[0] = '+';

          *stringbuf = line;
        }
    }
  else
    *stringbuf = line;

  return SVN_NO_ERROR;
}

/* Parse PROP_NAME from HEADER as the part after the INDICATOR line. */
static svn_error_t *
parse_prop_name(const char **prop_name, const char *header, 
                const char *indicator, apr_pool_t *result_pool)
{
  SVN_ERR(svn_utf_cstring_to_utf8(prop_name,
                                  header + strlen(indicator),
                                  result_pool));

  /* ### Are we guarenteed that there are no trailing or leading
   * ### whitespaces in the name? */

  return SVN_NO_ERROR;
}

/* Return the next *HUNK from a PATCH, using STREAM to read data
 * from the patch file. If no hunk can be found, set *HUNK to NULL. If we
 * have a property hunk, PROP_NAME will be set. If we have a text hunk,
 * PROP_NAME will be NULL. If REVERSE is TRUE, invert the hunk while
 * parsing it. If IGNORE_WHiTESPACES is TRUE, let lines without leading
 * spaces be recognized as context lines.  Allocate results in RESULT_POOL.
 * Use SCRATCH_POOL for all other allocations. */
static svn_error_t *
parse_next_hunk(svn_hunk_t **hunk,
                const char **prop_name,
                svn_patch_t *patch,
                svn_stream_t *stream,
                svn_boolean_t reverse,
                svn_boolean_t ignore_whitespace,
                apr_pool_t *result_pool,
                apr_pool_t *scratch_pool)
{
  static const char * const minus = "--- ";
  static const char * const text_atat = "@@";
  static const char * const prop_atat = "##";
  svn_stringbuf_t *line;
  svn_boolean_t eof, in_hunk, hunk_seen;
  apr_off_t pos, last_line;
  apr_off_t start, end;
  svn_stream_t *diff_text;
  svn_stream_t *original_text;
  svn_stream_t *modified_text;
  svn_linenum_t original_lines;
  svn_linenum_t modified_lines;
  svn_linenum_t leading_context;
  svn_linenum_t trailing_context;
  svn_boolean_t changed_line_seen;
  apr_pool_t *iterpool;

  /* We only set this if we have a property hunk. 
   * ### prop_name acts as both a state flag inside this function and a
   * ### qualifier to discriminate between props and text hunks. Is that
   * ### kind of overloading ok? */
  *prop_name = NULL;

  if (apr_file_eof(patch->patch_file) == APR_EOF)
    {
      /* No more hunks here. */
      *hunk = NULL;
      return SVN_NO_ERROR;
    }

  in_hunk = FALSE;
  hunk_seen = FALSE;
  leading_context = 0;
  trailing_context = 0;
  changed_line_seen = FALSE;
  *hunk = apr_pcalloc(result_pool, sizeof(**hunk));

  /* Get current seek position -- APR has no ftell() :( */
  pos = 0;
  SVN_ERR(svn_io_file_seek(patch->patch_file, APR_CUR, &pos, scratch_pool));

  iterpool = svn_pool_create(scratch_pool);
  do
    {

      svn_pool_clear(iterpool);

      /* Remember the current line's offset, and read the line. */
      last_line = pos;
      SVN_ERR(svn_stream_readline_detect_eol(stream, &line, NULL, &eof,
                                             iterpool));

      if (! eof)
        {
          /* Update line offset for next iteration.
           * APR has no ftell() :( */
          pos = 0;
          SVN_ERR(svn_io_file_seek(patch->patch_file, APR_CUR, &pos, iterpool));
        }

      /* Lines starting with a backslash are comments, such as
       * "\ No newline at end of file". */
      if (line->data[0] == '\\')
        continue;

      if (in_hunk)
        {
          char c;
          char add;
          char del;

          if (reverse)
            {
              add = '-';
              del = '+';
            }
          else
            {
              add = '+';
              del = '-';
            }

          if (! hunk_seen)
            {
              /* We're reading the first line of the hunk, so the start
               * of the line just read is the hunk text's byte offset. */
              start = last_line;
            }

          c = line->data[0];
          /* Tolerate chopped leading spaces on empty lines. */
          if (original_lines > 0 && modified_lines > 0 
              && ((c == ' ')
              || (! eof && line->len == 0)
              || (ignore_whitespace && c != del && c != add)))
            {
              hunk_seen = TRUE;
              original_lines--;
              modified_lines--;
              if (changed_line_seen)
                trailing_context++;
              else
                leading_context++;
            }
          else if (original_lines > 0 && c == del)
            {
              hunk_seen = TRUE;
              changed_line_seen = TRUE;

              /* A hunk may have context in the middle. We only want
                 trailing lines of context. */
              if (trailing_context > 0)
                trailing_context = 0;

              original_lines--;
            }
          else if (modified_lines > 0 && c == add)
            {
              hunk_seen = TRUE;
              changed_line_seen = TRUE;

              /* A hunk may have context in the middle. We only want
                 trailing lines of context. */
              if (trailing_context > 0)
                trailing_context = 0;

              modified_lines--;
            }
          else
            {
              in_hunk = FALSE;

              /* The start of the current line marks the first byte
               * after the hunk text. */
              end = last_line;

              break; /* Hunk was empty or has been read. */
            }
        }
      else
        {
          if (starts_with(line->data, text_atat))
            {
              /* Looks like we have a hunk header, try to rip it apart. */
              in_hunk = parse_hunk_header(line->data, *hunk, text_atat,
                                          reverse, iterpool);
              if (in_hunk)
                {
                  original_lines = (*hunk)->original_length;
                  modified_lines = (*hunk)->modified_length;
                  *prop_name = NULL;
                }
              }
          else if (starts_with(line->data, prop_atat) && *prop_name)
            {
              /* Looks like we have a property hunk header, try to rip it
               * apart. */
              in_hunk = parse_hunk_header(line->data, *hunk, prop_atat,
                                          reverse, iterpool);
              if (in_hunk)
                {
                  original_lines = (*hunk)->original_length;
                  modified_lines = (*hunk)->modified_length;
                }
            }
          else if (starts_with(line->data, "Added: "))
            {
              SVN_ERR(parse_prop_name(prop_name, line->data, "Added: ",
                                      result_pool));
            }
          else if (starts_with(line->data, "Deleted: "))
            {
              SVN_ERR(parse_prop_name(prop_name, line->data, "Deleted: ",
                                      result_pool));
            }
          else if (starts_with(line->data, "Modified: "))
            {
              SVN_ERR(parse_prop_name(prop_name, line->data, "Modified: ",
                                      result_pool));
            }
          else if (starts_with(line->data, minus)
                   || starts_with(line->data, "git --diff "))
            /* This could be a header of another patch. Bail out. */
            break;
        }
    }
  /* Check for the line length since a file may not have a newline at the
   * end and we depend upon the last line to be an empty one. */
  while (! eof || line->len > 0);
  svn_pool_destroy(iterpool);

  if (! eof)
    /* Rewind to the start of the line just read, so subsequent calls
     * to this function or svn_diff_parse_next_patch() don't end
     * up skipping the line -- it may contain a patch or hunk header. */
    SVN_ERR(svn_io_file_seek(patch->patch_file, APR_SET, &last_line,
                             scratch_pool));

  if (hunk_seen && start < end)
    {
      apr_file_t *f;
      apr_int32_t flags = APR_READ | APR_BUFFERED;

      /* Create a stream which returns the hunk text itself. */
      SVN_ERR(svn_io_file_open(&f, patch->path, flags, APR_OS_DEFAULT,
                               result_pool));
      diff_text = svn_stream_from_aprfile_range_readonly(f, FALSE,
                                                         start, end,
                                                         result_pool);

      /* Create a stream which returns the original hunk text. */
      SVN_ERR(svn_io_file_open(&f, patch->path, flags, APR_OS_DEFAULT,
                               result_pool));
      original_text = svn_stream_from_aprfile_range_readonly(f, FALSE,
                                                             start, end,
                                                             result_pool);

      /* Create a stream which returns the modified hunk text. */
      SVN_ERR(svn_io_file_open(&f, patch->path, flags, APR_OS_DEFAULT,
                               result_pool));
      modified_text = svn_stream_from_aprfile_range_readonly(f, FALSE,
                                                             start, end,
                                                             result_pool);

      (*hunk)->diff_text = diff_text;
      (*hunk)->reverse = reverse;
      (*hunk)->original_text = original_text;
      (*hunk)->modified_text = modified_text;
      (*hunk)->leading_context = leading_context;
      (*hunk)->trailing_context = trailing_context;
    }
  else
    /* Something went wrong, just discard the result. */
    *hunk = NULL;

  return SVN_NO_ERROR;
}

/* Compare function for sorting hunks after parsing.
 * We sort hunks by their original line offset. */
static int
compare_hunks(const void *a, const void *b)
{
  const svn_hunk_t *ha = *((const svn_hunk_t **)a);
  const svn_hunk_t *hb = *((const svn_hunk_t **)b);

  if (ha->original_start < hb->original_start)
    return -1;
  if (ha->original_start > hb->original_start)
    return 1;
  return 0;
}

/*
 * Ensure that all streams which were opened for HUNK are closed.
 */
static svn_error_t *
close_hunk(const svn_hunk_t *hunk)
{
  SVN_ERR(svn_stream_close(hunk->original_text));
  SVN_ERR(svn_stream_close(hunk->modified_text));
  SVN_ERR(svn_stream_close(hunk->diff_text));
  return SVN_NO_ERROR;
}

enum parse_state
{
   state_start,
   state_git_diff_seen,
  /* if we have an add || del || cp src+dst || mv src+dst */
   state_git_tree_seen, 
   state_git_minus_seen,
   state_git_plus_seen,
   state_move_from_seen,
   state_copy_from_seen,
   state_minus_seen,
   state_unidiff_found,
   state_add_seen,
   state_del_seen,
   state_git_header_found
};

struct transition
{
  const char *expected_input;
  enum parse_state required_state;
  svn_error_t *(*fn)(enum parse_state *new_state, const char *input, 
                     svn_patch_t *patch, apr_pool_t *result_pool,
                     apr_pool_t *scratch_pool);
};

/* UTF-8 encode and canonicalize the content of LINE as FILE_NAME. */
static svn_error_t *
grab_filename(const char **file_name, const char *line, apr_pool_t *result_pool,
              apr_pool_t *scratch_pool)
{
  const char *utf8_path;
  const char *canon_path;

  /* Grab the filename and encode it in UTF-8. */
  /* TODO: Allow specifying the patch file's encoding.
   *       For now, we assume its encoding is native. */
  SVN_ERR(svn_utf_cstring_to_utf8(&utf8_path,
                                  line,
                                  scratch_pool));

  /* Canonicalize the path name. */
  canon_path = svn_dirent_canonicalize(utf8_path, scratch_pool);

  *file_name = apr_pstrdup(result_pool, canon_path);

  return SVN_NO_ERROR;
}

/* Parse the '--- ' line of a regular unidiff. */
static svn_error_t *
diff_minus(enum parse_state *new_state, const char *line, svn_patch_t *patch,
           apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  /* If we can find a tab, it separates the filename from
   * the rest of the line which we can discard. */
  char *tab = strchr(line, '\t');
  if (tab)
    *tab = '\0';

  SVN_ERR(grab_filename(&patch->old_filename, line + strlen("--- "),
                        result_pool, scratch_pool));

  *new_state = state_minus_seen;

  return SVN_NO_ERROR;
}

/* Parse the '+++ ' line of a regular unidiff. */
static svn_error_t *
diff_plus(enum parse_state *new_state, const char *line, svn_patch_t *patch,
           apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  /* If we can find a tab, it separates the filename from
   * the rest of the line which we can discard. */
  char *tab = strchr(line, '\t');
  if (tab)
    *tab = '\0';

  SVN_ERR(grab_filename(&patch->new_filename, line + strlen("+++ "),
                        result_pool, scratch_pool));

  *new_state = state_unidiff_found;

  return SVN_NO_ERROR;
}

/* Parse the first line of a git extended unidiff. */
static svn_error_t *
git_start(enum parse_state *new_state, const char *line, svn_patch_t *patch,
          apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  const char *old_path;
  const char *new_path;
  char *end_old_path;
  char *slash;

  /* ### Add handling of escaped paths
   * http://www.kernel.org/pub/software/scm/git/docs/git-diff.html: 
   *
   * TAB, LF, double quote and backslash characters in pathnames are
   * represented as \t, \n, \" and \\, respectively. If there is need for
   * such substitution then the whole pathname is put in double quotes.
   */

  /* Our line should look like this: 'git --diff a/path b/path'. 
   * If we find any deviations from that format, we return with state reset
   * to start.
   *
   * ### We can't handle paths with spaces!
   */
  slash = strchr(line, '/');

  if (! slash)
    {
      *new_state = state_start;
      return SVN_NO_ERROR;
    }

  old_path = slash + 1;

  if (! *old_path)
    {
      *new_state = state_start;
      return SVN_NO_ERROR;
    }

  end_old_path = strchr(old_path, ' ');

  if (end_old_path)
    *end_old_path = '\0';
  else
    {
      *new_state = state_start;
      return SVN_NO_ERROR;
    }

  /* The new path begins after the first slash after the old path. */
  slash = strchr(end_old_path + 1, '/');

  if (! slash)
    {
      *new_state = state_start;
      return SVN_NO_ERROR;
    }

  /* The path starts after the slash */
  new_path = slash + 1;

  if (! *new_path)
    {
      *new_state = state_start;
      return SVN_NO_ERROR;
    }

  SVN_ERR(grab_filename(&patch->old_filename, old_path,
                        result_pool, scratch_pool));

  SVN_ERR(grab_filename(&patch->new_filename, new_path,
                        result_pool, scratch_pool));

  /* We assume that the path is only modified until we've found a 'tree'
   * header */
  patch->operation = svn_diff_op_modified;

  *new_state = state_git_diff_seen;
  return SVN_NO_ERROR;
}

/* Parse the '--- ' line of a git extended unidiff. */
static svn_error_t *
git_minus(enum parse_state *new_state, const char *line, svn_patch_t *patch,
          apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  /* ### Check that the path is consistent with the 'git --diff ' line. */

  *new_state = state_git_minus_seen;
  return SVN_NO_ERROR;
}

/* Parse the '+++ ' line of a git extended unidiff. */
static svn_error_t *
git_plus(enum parse_state *new_state, const char *line, svn_patch_t *patch,
          apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  /* ### Check that the path is consistent with the 'git --diff ' line. */

  *new_state = state_git_header_found;
  return SVN_NO_ERROR;
}

/* Parse the 'move from ' line of a git extended unidiff. */
static svn_error_t *
git_move_from(enum parse_state *new_state, const char *line, svn_patch_t *patch,
              apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  /* ### Check that the path is consistent with the 'git --diff ' line. */

  *new_state = state_move_from_seen;
  return SVN_NO_ERROR;
}

/* Parse the 'move to ' line fo a git extended unidiff. */
static svn_error_t *
git_move_to(enum parse_state *new_state, const char *line, svn_patch_t *patch,
            apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  /* ### Check that the path is consistent with the 'git --diff ' line. */

  patch->operation = svn_diff_op_moved;

  *new_state = state_git_tree_seen;
  return SVN_NO_ERROR;
}

/* Parse the 'copy from ' line of a git extended unidiff. */
static svn_error_t *
git_copy_from(enum parse_state *new_state, const char *line, svn_patch_t *patch,
              apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  /* ### Check that the path is consistent with the 'git --diff ' line. */

  *new_state = state_copy_from_seen; 
  return SVN_NO_ERROR;
}

/* Parse the 'copy to ' line of a git extended unidiff. */
static svn_error_t *
git_copy_to(enum parse_state *new_state, const char *line, svn_patch_t *patch,
            apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  /* ### Check that the path is consistent with the 'git --diff ' line. */

  patch->operation = svn_diff_op_copied;

  *new_state = state_git_tree_seen;
  return SVN_NO_ERROR;
}

/* Parse the 'new file ' line of a git extended unidiff. */
static svn_error_t *
git_new_file(enum parse_state *new_state, const char *line, svn_patch_t *patch,
             apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  patch->operation = svn_diff_op_added;

  *new_state = state_git_header_found;
  return SVN_NO_ERROR;
}

/* Parse the 'deleted file ' line of a git extended unidiff. */
static svn_error_t *
git_deleted_file(enum parse_state *new_state, const char *line, svn_patch_t *patch,
                 apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  patch->operation = svn_diff_op_deleted;

  *new_state = state_git_header_found;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_diff_parse_next_patch(svn_patch_t **patch,
                          apr_file_t *patch_file,
                          svn_boolean_t reverse,
                          svn_boolean_t ignore_whitespace,
                          apr_pool_t *result_pool,
                          apr_pool_t *scratch_pool)
{
  const char *fname;
  svn_stream_t *stream;
  apr_off_t pos, last_line;
  svn_boolean_t eof;
  svn_boolean_t line_after_tree_header_read = FALSE;

  apr_pool_t *iterpool;

  enum parse_state state = state_start;

  /* ### dannas: As I've understood the git diff format, the first line
   * ### contains both paths and the paths in the headers that follow are only
   * ### there to ensure that the path is valid. Not sure though, the
   * ### research continues... */

  /* Our table consisting of:
   * Expected Input     Required state          Function to call */
  struct transition transitions[] = 
    {
      {"--- ",          state_start,            diff_minus},
      {"+++ ",          state_minus_seen,       diff_plus},
      {"git --diff",    state_start,            git_start},
      {"--- a/",        state_git_diff_seen,    git_minus},
      {"--- a/",        state_git_tree_seen,    git_minus},
      {"+++ b/",        state_git_minus_seen,   git_plus},
      {"move from ",    state_git_diff_seen,    git_move_from},
      {"move to ",      state_move_from_seen,   git_move_to},
      {"copy from ",    state_git_diff_seen,    git_copy_from},
      {"copy to ",      state_copy_from_seen,   git_copy_to},
      {"new file ",     state_git_diff_seen,    git_new_file},
      {"deleted file ", state_git_diff_seen,    git_deleted_file},
    };

  if (apr_file_eof(patch_file) == APR_EOF)
    {
      /* No more patches here. */
      *patch = NULL;
      return SVN_NO_ERROR;
    }

  /* Get the patch's filename. */
  SVN_ERR(svn_io_file_name_get(&fname, patch_file, result_pool));

  /* Record what we already know about the patch. */
  *patch = apr_pcalloc(result_pool, sizeof(**patch));
  (*patch)->patch_file = patch_file;
  (*patch)->path = fname;

  /* Get a stream to read lines from the patch file.
   * The file should not be closed when we close the stream so
   * make sure it is disowned. */
  stream = svn_stream_from_aprfile2(patch_file, TRUE, scratch_pool);

  /* Get current seek position -- APR has no ftell() :( */
  pos = 0;
  SVN_ERR(svn_io_file_seek((*patch)->patch_file, APR_CUR, &pos, scratch_pool));

  iterpool = svn_pool_create(scratch_pool);

  do
    {
      svn_stringbuf_t *line;
      int i;

      svn_pool_clear(iterpool);

      /* Remember the current line's offset, and read the line. */
      last_line = pos;
      SVN_ERR(svn_stream_readline_detect_eol(stream, &line, NULL, &eof,
                                             iterpool));

      if (! eof)
        {
          /* Update line offset for next iteration.
           * APR has no ftell() :( */
          pos = 0;
          SVN_ERR(svn_io_file_seek((*patch)->patch_file, APR_CUR, &pos, iterpool));
        }

      /* Run the state machine. */
      for (i = 0; i < (sizeof(transitions) / sizeof(transitions[0])); i++)
        {
          if (line->len > strlen(transitions[i].expected_input) 
              && starts_with(line->data, transitions[i].expected_input)
              && state == transitions[i].required_state)
            {
              SVN_ERR(transitions[i].fn(&state, line->data, *patch,
                                        result_pool, iterpool));
              break;
            }
        }

      if (state == state_unidiff_found
          || state == state_git_header_found)
        {
          /* We have a valid diff header, yay! */
          break; 
        }
      else if (state == state_git_tree_seen 
               && line_after_tree_header_read)
        {
          /* We have a valid diff header for a patch with only tree changes.
           * Rewind to the start of the line just read, so subsequent calls
           * to this function don't end up skipping the line -- it may
           * contain a patch. */
          SVN_ERR(svn_io_file_seek((*patch)->patch_file, APR_SET, &last_line,
                                   scratch_pool));
          break;
        }
      else if (state == state_git_tree_seen)
          line_after_tree_header_read = TRUE;

    } while (! eof);

  if (reverse)
    {
      const char *temp;
      temp = (*patch)->old_filename;
      (*patch)->old_filename = (*patch)->new_filename;
      (*patch)->new_filename = temp;
    }

  if ((*patch)->old_filename == NULL || (*patch)->new_filename == NULL)
    {
      /* Something went wrong, just discard the result. */
      *patch = NULL;
    }
  else
    {
      svn_hunk_t *hunk;
      const char *prop_name;

      /* Parse hunks. */
      (*patch)->hunks = apr_array_make(result_pool, 10, sizeof(svn_hunk_t *));
      (*patch)->property_hunks = apr_hash_make(result_pool);
      do
        {
          svn_pool_clear(iterpool);

          SVN_ERR(parse_next_hunk(&hunk, &prop_name, *patch, stream,
                                  reverse, ignore_whitespace,
                                  result_pool, iterpool));
          if (hunk && prop_name)
            {
              apr_array_header_t *hunks;

              hunks = apr_hash_get((*patch)->property_hunks, prop_name,
                                    APR_HASH_KEY_STRING);
              if (! hunks)
                {
                  hunks = apr_array_make(result_pool, 1, 
                                          sizeof(svn_hunk_t *));
                  apr_hash_set((*patch)->property_hunks, prop_name,
                                       APR_HASH_KEY_STRING, hunks);
                }

              APR_ARRAY_PUSH(hunks, svn_hunk_t *) = hunk;
            }
          else if (hunk)
            APR_ARRAY_PUSH((*patch)->hunks, svn_hunk_t *) = hunk;

        }
      while (hunk);
    }

  svn_pool_destroy(iterpool);
  SVN_ERR(svn_stream_close(stream));

  if (*patch)
    {
      /* Usually, hunks appear in the patch sorted by their original line
       * offset. But just in case they weren't parsed in this order for
       * some reason, we sort them so that our caller can assume that hunks
       * are sorted as if parsed from a usual patch. */
      qsort((*patch)->hunks->elts, (*patch)->hunks->nelts,
            (*patch)->hunks->elt_size, compare_hunks);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_diff_close_patch(const svn_patch_t *patch)
{
  int i;

  for (i = 0; i < patch->hunks->nelts; i++)
    {
      const svn_hunk_t *hunk = APR_ARRAY_IDX(patch->hunks, i, svn_hunk_t *);
      SVN_ERR(close_hunk(hunk));
    }

  return SVN_NO_ERROR;
}
