/*
 * marshal.c :  Marshalling routines for Subversion protocol
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



#include <assert.h>
#include <stdlib.h>

#define APR_WANT_STRFUNC
#include <apr_want.h>
#include <apr_general.h>
#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_network_io.h>
#include <apr_poll.h>

#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "svn_ra_svn.h"
#include "svn_utf.h"
#include "svn_ebcdic.h"

#include "ra_svn.h"

#define svn_iswhitespace(c) ((c) == SVN_UTF8_SPACE || (c) == SVN_UTF8_NEWLINE)

#define FAILURE_STR \
        "\x66\x61\x69\x6c\x75\x72\x65"
        /* "failure"*/

#define SUCCESS_STR \
        "\x73\x75\x63\x63\x65\x73\x73"
        /* "success"*/

#define FALSE_STR \
        "\x66\x61\x6c\x73\x65"
        /*"false"*/

#define TRUE_STR \
        "\x74\x72\x75\x65" \
        /* "true" */

/* --- CONNECTION INITIALIZATION --- */

svn_ra_svn_conn_t *svn_ra_svn_create_conn(apr_socket_t *sock,
                                          apr_file_t *in_file,
                                          apr_file_t *out_file,
                                          apr_pool_t *pool)
{
  svn_ra_svn_conn_t *conn = apr_palloc(pool, sizeof(*conn));

  assert((sock && !in_file && !out_file) || (!sock && in_file && out_file));
  conn->sock = sock;
  conn->in_file = in_file;
  conn->out_file = out_file;
  conn->read_ptr = conn->read_buf;
  conn->read_end = conn->read_buf;
  conn->write_pos = 0;
  conn->block_handler = NULL;
  conn->block_baton = NULL;
  conn->capabilities = apr_hash_make(pool);
  conn->pool = pool;
  return conn;
}

svn_error_t *svn_ra_svn_set_capabilities(svn_ra_svn_conn_t *conn,
                                         apr_array_header_t *list)
{
  int i;
  svn_ra_svn_item_t *item;
  const char *word;

  for (i = 0; i < list->nelts; i++)
    {
      item = &APR_ARRAY_IDX(list, i, svn_ra_svn_item_t);
      if (item->kind != SVN_RA_SVN_WORD)
        return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                                "Capability entry is not a word");
      word = apr_pstrdup(conn->pool, item->u.word);
      apr_hash_set(conn->capabilities, word, APR_HASH_KEY_STRING, word);
    }
  return SVN_NO_ERROR;
}

svn_boolean_t svn_ra_svn_has_capability(svn_ra_svn_conn_t *conn,
                                        const char *capability)
{
  return (apr_hash_get(conn->capabilities, capability,
                       APR_HASH_KEY_STRING) != NULL);
}

void svn_ra_svn__set_block_handler(svn_ra_svn_conn_t *conn,
                                   ra_svn_block_handler_t handler,
                                   void *baton)
{
  apr_interval_time_t interval = (handler) ? 0 : -1;

  conn->block_handler = handler;
  conn->block_baton = baton;
  if (conn->sock)
    apr_socket_timeout_set(conn->sock, interval);
  else
    apr_file_pipe_timeout_set(conn->out_file, interval);
}

svn_boolean_t svn_ra_svn__input_waiting(svn_ra_svn_conn_t *conn,
                                        apr_pool_t *pool)
{
  apr_pollfd_t pfd;
  int n;

  if (conn->sock)
    {
      pfd.desc_type = APR_POLL_SOCKET;
      pfd.desc.s = conn->sock;
    }
  else
    {
      pfd.desc_type = APR_POLL_FILE;
      pfd.desc.f = conn->in_file;
    }
  pfd.p = pool;
  pfd.reqevents = APR_POLLIN;
#ifdef AS400
{
  /* IBM's apr_poll() implmentation behaves badly with some large values of n
   * (apr_palloc fails) so we initialize it. */
  n = 0;
  return ((apr_poll(&pfd, 1, &n, 0, pool) == APR_SUCCESS) && n);
}
#else
  return ((apr_poll(&pfd, 1, &n, 0) == APR_SUCCESS) && n);
#endif  
}

/* --- WRITE BUFFER MANAGEMENT --- */

/* Write bytes into the write buffer until either the write buffer is
 * full or we reach END. */
static const char *writebuf_push(svn_ra_svn_conn_t *conn, const char *data,
                                 const char *end)
{
  apr_ssize_t buflen, copylen;

  buflen = sizeof(conn->write_buf) - conn->write_pos;
  copylen = (buflen < end - data) ? buflen : end - data;
  memcpy(conn->write_buf + conn->write_pos, data, copylen);
  conn->write_pos += copylen;
  return data + copylen;
}

/* Write data to socket or output file as appropriate. */
static svn_error_t *writebuf_output(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                    const char *data, apr_size_t len)
{
  const char *end = data + len;
  apr_status_t status;
  apr_size_t count;
  apr_pool_t *subpool = NULL;

  while (data < end)
    {
      count = end - data;
      if (conn->sock)
        status = apr_socket_send(conn->sock, data, &count);
      else
        status = apr_file_write(conn->out_file, data, &count);
      if (status)
        return svn_error_wrap_apr(status, "Can't write to connection");
      if (count == 0)
        {
          if (!subpool)
            subpool = svn_pool_create(pool);
          else
            apr_pool_clear(subpool);
          SVN_ERR(conn->block_handler(conn, subpool, conn->block_baton));
        }
      data += count;
    }

  if (subpool)
    apr_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* Write data from the write buffer out to the socket. */
static svn_error_t *writebuf_flush(svn_ra_svn_conn_t *conn, apr_pool_t *pool)
{
  int write_pos = conn->write_pos;

  /* Clear conn->write_pos first in case the block handler does a read. */
  conn->write_pos = 0;
  SVN_ERR(writebuf_output(conn, pool, conn->write_buf, write_pos));
  return SVN_NO_ERROR;
}

static svn_error_t *writebuf_write(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                   const char *data, apr_size_t len)
{
  const char *end = data + len;

  if (conn->write_pos > 0 && conn->write_pos + len > sizeof(conn->write_buf))
    {
      /* Fill and then empty the write buffer. */
      data = writebuf_push(conn, data, end);
      SVN_ERR(writebuf_flush(conn, pool));
    }

  if (end - data > (apr_ssize_t)sizeof(conn->write_buf))
    SVN_ERR(writebuf_output(conn, pool, data, end - data));
  else
    writebuf_push(conn, data, end);
  return SVN_NO_ERROR;
}

static svn_error_t *writebuf_printf(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                    const char *fmt, ...)
{
  va_list ap;
  char *str;

  va_start(ap, fmt);
  str = APR_PVSPRINTF2(pool, fmt, ap);
  va_end(ap);
  return writebuf_write(conn, pool, str, strlen(str));
}

/* --- READ BUFFER MANAGEMENT --- */

/* Read bytes into DATA until either the read buffer is empty or
 * we reach END. */
static char *readbuf_drain(svn_ra_svn_conn_t *conn, char *data, char *end)
{
  apr_ssize_t buflen, copylen;

  buflen = conn->read_end - conn->read_ptr;
  copylen = (buflen < end - data) ? buflen : end - data;
  memcpy(data, conn->read_ptr, copylen);
  conn->read_ptr += copylen;
  return data + copylen;
}

/* Read data from socket or input file as appropriate. */
static svn_error_t *readbuf_input(svn_ra_svn_conn_t *conn, char *data,
                                  apr_size_t *len)
{
  apr_status_t status;

  /* Always block for reading. */
  if (conn->sock && conn->block_handler)
    apr_socket_timeout_set(conn->sock, -1);
  if (conn->sock)
    status = apr_socket_recv(conn->sock, data, len);
  else
    status = apr_file_read(conn->in_file, data, len);
  if (conn->sock && conn->block_handler)
    apr_socket_timeout_set(conn->sock, 0);
  if (status && !APR_STATUS_IS_EOF(status))
    return svn_error_wrap_apr(status, "Can't read from connection");
  if (*len == 0)
    return svn_error_create(SVN_ERR_RA_SVN_CONNECTION_CLOSED, NULL,
                            "Connection closed unexpectedly");
  return SVN_NO_ERROR;
}

/* Read data from the socket into the read buffer, which must be empty. */
static svn_error_t *readbuf_fill(svn_ra_svn_conn_t *conn, apr_pool_t *pool)
{
  apr_size_t len;

  assert(conn->read_ptr == conn->read_end);
  writebuf_flush(conn, pool);
  len = sizeof(conn->read_buf);
  SVN_ERR(readbuf_input(conn, conn->read_buf, &len));
  conn->read_ptr = conn->read_buf;
  conn->read_end = conn->read_buf + len;
  return SVN_NO_ERROR;
}

static svn_error_t *readbuf_getchar(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                    char *result)
{
  if (conn->read_ptr == conn->read_end)
    SVN_ERR(readbuf_fill(conn, pool));
  *result = *conn->read_ptr++;
  return SVN_NO_ERROR;
}

static svn_error_t *readbuf_getchar_skip_whitespace(svn_ra_svn_conn_t *conn,
                                                    apr_pool_t *pool,
                                                    char *result)
{
  do
    SVN_ERR(readbuf_getchar(conn, pool, result));
  while (svn_iswhitespace(*result));
  return SVN_NO_ERROR;
}

static svn_error_t *readbuf_read(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                 char *data, apr_size_t len)
{
  char *end = data + len;
  apr_size_t count;

  /* Copy in an appropriate amount of data from the buffer. */
  data = readbuf_drain(conn, data, end);

  /* Read large chunks directly into buffer. */
  while (end - data > (apr_ssize_t)sizeof(conn->read_buf))
    {
      writebuf_flush(conn, pool);
      count = end - data;
      SVN_ERR(readbuf_input(conn, data, &count));
      data += count;
    }

  while (end > data)
    {
      /* The remaining amount to read is small; fill the buffer and
       * copy from that. */
      SVN_ERR(readbuf_fill(conn, pool));
      data = readbuf_drain(conn, data, end);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *readbuf_skip_leading_garbage(svn_ra_svn_conn_t *conn)
{
  char buf[256];  /* Must be smaller than sizeof(conn->read_buf) - 1. */
  const char *p, *end;
  apr_size_t len;
  svn_boolean_t lparen = FALSE;

  assert(conn->read_ptr == conn->read_end);
  while (1)
    {
      /* Read some data directly from the connection input source. */
      len = sizeof(buf);
      SVN_ERR(readbuf_input(conn, buf, &len));
      end = buf + len;

      /* Scan the data for '(' WS with a very simple state machine. */
      for (p = buf; p < end; p++)
        {
          if (lparen && svn_iswhitespace(*p))
            break;
          else
            lparen = (*p == SVN_UTF8_LPAREN);
        }
      if (p < end)
        break;
    }

  /* p now points to the whitespace just after the left paren.  Fake
   * up the left paren and then copy what we have into the read
   * buffer. */
  conn->read_buf[0] = SVN_UTF8_LPAREN;
  memcpy(conn->read_buf + 1, p, end - p);
  conn->read_ptr = conn->read_buf;
  conn->read_end = conn->read_buf + 1 + (end - p);
  return SVN_NO_ERROR;
}

/* --- WRITING DATA ITEMS --- */
 
svn_error_t *svn_ra_svn_write_number(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                     apr_uint64_t number)
{
  return writebuf_printf(conn, pool, "%" APR_UINT64_T_FMT " ", number);                       
}

svn_error_t *svn_ra_svn_write_string(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                     const svn_string_t *str)
{
  SVN_ERR(writebuf_printf(conn, pool, "%" APR_SIZE_T_FMT ":", str->len));                      
  SVN_ERR(writebuf_write(conn, pool, str->data, str->len));
  SVN_ERR(writebuf_write(conn, pool, SVN_UTF8_SPACE_STR, 1));
  return SVN_NO_ERROR;
}

svn_error_t *svn_ra_svn_write_cstring(svn_ra_svn_conn_t *conn,
                                      apr_pool_t *pool, const char *s)
{
  return writebuf_printf(conn, pool, "%" APR_SIZE_T_FMT ":%s ", strlen(s), s);                       
}

svn_error_t *svn_ra_svn_write_word(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                   const char *word)
{
  return writebuf_printf(conn, pool, "%s ", word);
}

svn_error_t *svn_ra_svn_start_list(svn_ra_svn_conn_t *conn, apr_pool_t *pool)
{
  return writebuf_write(conn, pool, "\x28\x20" /* "( " */, 2);
}

svn_error_t *svn_ra_svn_end_list(svn_ra_svn_conn_t *conn, apr_pool_t *pool)
{
  return writebuf_write(conn, pool, "\x29\x20" /* ") " */, 2);
}

svn_error_t *svn_ra_svn_flush(svn_ra_svn_conn_t *conn, apr_pool_t *pool)
{
  return writebuf_flush(conn, pool);
}

/* --- WRITING TUPLES --- */

static svn_error_t *vwrite_tuple(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                 const char *fmt, va_list ap)
{
  svn_boolean_t opt = FALSE;
  svn_revnum_t rev;
  const char *cstr;
  const svn_string_t *str;

  if (*fmt == '!')
    fmt++;
  else
    SVN_ERR(svn_ra_svn_start_list(conn, pool));
  for (; *fmt; fmt++)
    {
      if (*fmt == 'n' && !opt)
        SVN_ERR(svn_ra_svn_write_number(conn, pool, va_arg(ap, apr_uint64_t)));
      else if (*fmt == 'r')
        {
          rev = va_arg(ap, svn_revnum_t);
          assert(opt || SVN_IS_VALID_REVNUM(rev));
          if (SVN_IS_VALID_REVNUM(rev))
            SVN_ERR(svn_ra_svn_write_number(conn, pool, rev));
        }
      else if (*fmt == 's')
        {
          str = va_arg(ap, const svn_string_t *);
          assert(opt || str);
          if (str)
            SVN_ERR(svn_ra_svn_write_string(conn, pool, str));
        }
      else if (*fmt == 'c')
        {
          cstr = va_arg(ap, const char *);
          assert(opt || cstr);
          if (cstr)
            SVN_ERR(svn_ra_svn_write_cstring(conn, pool, cstr));
        }
      else if (*fmt == 'w')
        {
          cstr = va_arg(ap, const char *);
          assert(opt || cstr);
          if (cstr)
            SVN_ERR(svn_ra_svn_write_word(conn, pool, cstr));
        }
      else if (*fmt == 'b' && !opt)
        {
          cstr = va_arg(ap, svn_boolean_t) ? TRUE_STR : FALSE_STR;
          SVN_ERR(svn_ra_svn_write_word(conn, pool, cstr));
        }
      else if (*fmt == '?')
        opt = TRUE;
      else if (*fmt == '(' && !opt)
        SVN_ERR(svn_ra_svn_start_list(conn, pool));
      else if (*fmt == ')')
        {
          SVN_ERR(svn_ra_svn_end_list(conn, pool));
          opt = FALSE;
        }
      else if (*fmt == '!' && !*(fmt + 1))
        return SVN_NO_ERROR;
      else
        abort();
    }
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  return SVN_NO_ERROR;
}

svn_error_t *svn_ra_svn_write_tuple(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                    const char *fmt, ...)
{
  svn_error_t *err;
  va_list ap;

  va_start(ap, fmt);
  err = vwrite_tuple(conn, pool, fmt, ap);
  va_end(ap);
  return err;
}

/* --- READING DATA ITEMS --- */

/* Read LEN bytes from CONN into already-allocated structure ITEM.
 * Afterwards, *ITEM is of type 'SVN_RA_SVN_STRING', and its string
 * data is allocated in POOL. */
static svn_error_t *read_string(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                svn_ra_svn_item_t *item, apr_uint64_t len)
{
  char readbuf[4096];
  apr_size_t readbuf_len;
  svn_stringbuf_t *stringbuf = svn_stringbuf_create ("", pool);

  /* We can't store strings longer than the maximum size of apr_size_t,
   * so check for wrapping */
  if (((apr_size_t) len) < len) 
    return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                            "String length larger than maximum");

  while (len)
    {
      readbuf_len = len > sizeof(readbuf) ? sizeof(readbuf) : len;

      SVN_ERR(readbuf_read(conn, pool, readbuf, readbuf_len));
      /* Read into a stringbuf_t to so we don't allow the sender to allocate
       * an arbitrary amount of memory without actually sending us that much
       * data */
      svn_stringbuf_appendbytes(stringbuf, readbuf, readbuf_len);
      len -= readbuf_len;
    }
  
  item->kind = SVN_RA_SVN_STRING;
  item->u.string = apr_palloc(pool, sizeof(*item->u.string));
  item->u.string->data = stringbuf->data;
  item->u.string->len = stringbuf->len;

  return SVN_NO_ERROR; 
}

/* Given the first non-whitespace character FIRST_CHAR, read an item
 * into the already allocated structure ITEM. */
static svn_error_t *read_item(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                              svn_ra_svn_item_t *item, char first_char)
{
  char c = first_char;
  apr_uint64_t val, prev_val=0;
  svn_stringbuf_t *str;
  svn_ra_svn_item_t *listitem;

  /* Determine the item type and read it in.  Make sure that c is the
   * first character at the end of the item so we can test to make
   * sure it's whitespace. */
  if (APR_IS_ASCII_DIGIT(c))
    {
      /* It's a number or a string.  Read the number part, either way. */
      val = c - SVN_UTF8_0;
      while (1)
        {
          prev_val = val;
          SVN_ERR(readbuf_getchar(conn, pool, &c));
          if (!APR_IS_ASCII_DIGIT(c))
            break;
          val = val * 10 + (c - SVN_UTF8_0);
          if ((val / 10) != prev_val) /* val wrapped past maximum value */
            return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                                    "Number is larger than maximum"); 
        }
      if (c == SVN_UTF8_COLON)
        {
          /* It's a string. */
          SVN_ERR(read_string(conn, pool, item, val));
          SVN_ERR(readbuf_getchar(conn, pool, &c));
        }
      else
        {
          /* It's a number. */
          item->kind = SVN_RA_SVN_NUMBER;
          item->u.number = val;
        }
    }
  else if (APR_IS_ASCII_ALPHA(c))
    {
      /* It's a word. */
      str = svn_stringbuf_ncreate(&c, 1, pool);
      while (1)
        {
          SVN_ERR(readbuf_getchar(conn, pool, &c));
          if (!APR_IS_ASCII_ALNUM(c) && c != SVN_UTF8_MINUS)
            break;
          svn_stringbuf_appendbytes(str, &c, 1);
        }
      item->kind = SVN_RA_SVN_WORD;
      item->u.word = str->data;
    }
  else if (c == SVN_UTF8_LPAREN)
    {
      /* Read in the list items. */
      item->kind = SVN_RA_SVN_LIST;
      item->u.list = apr_array_make(pool, 0, sizeof(svn_ra_svn_item_t));
      while (1)
        {
          SVN_ERR(readbuf_getchar_skip_whitespace(conn, pool, &c));
          if (c == SVN_UTF8_RPAREN)
            break;
          listitem = apr_array_push(item->u.list);
          SVN_ERR(read_item(conn, pool, listitem, c));
        }
      SVN_ERR(readbuf_getchar(conn, pool, &c));
    }

  if (!svn_iswhitespace(c))
    return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                            "Malformed network data");
  return SVN_NO_ERROR;
}

svn_error_t *svn_ra_svn_read_item(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                  svn_ra_svn_item_t **item)
{
  char c;

  /* Allocate space, read the first character, and then do the rest of
   * the work.  This makes sense because of the way lists are read. */
  *item = apr_palloc(pool, sizeof(**item));
  SVN_ERR(readbuf_getchar_skip_whitespace(conn, pool, &c));
  return read_item(conn, pool, *item, c);
}

svn_error_t *svn_ra_svn_skip_leading_garbage(svn_ra_svn_conn_t *conn,
                                             apr_pool_t *pool)
{
  return readbuf_skip_leading_garbage(conn);
}

/* --- READING AND PARSING TUPLES --- */

/* Parse a tuple.  Advance *FMT to the end of the tuple specification
 * and advance AP by the corresponding arguments. */
static svn_error_t *vparse_tuple(apr_array_header_t *list, apr_pool_t *pool,
                                 const char **fmt, va_list *ap)
{
  int count, list_level;
  svn_ra_svn_item_t *elt;

  for (count = 0; **fmt && count < list->nelts; (*fmt)++, count++)
    {
      /* '?' just means the tuple may stop; skip past it. */
      if (**fmt == '?')
        (*fmt)++;
      elt = &((svn_ra_svn_item_t *) list->elts)[count];
      if (**fmt == 'n' && elt->kind == SVN_RA_SVN_NUMBER)
        *va_arg(*ap, apr_uint64_t *) = elt->u.number;
      else if (**fmt == 'r' && elt->kind == SVN_RA_SVN_NUMBER)
        *va_arg(*ap, svn_revnum_t *) = elt->u.number;
      else if (**fmt == 's' && elt->kind == SVN_RA_SVN_STRING)
        *va_arg(*ap, svn_string_t **) = elt->u.string;
      else if (**fmt == 'c' && elt->kind == SVN_RA_SVN_STRING)
        *va_arg(*ap, const char **) = elt->u.string->data;
      else if (**fmt == 'w' && elt->kind == SVN_RA_SVN_WORD)
        *va_arg(*ap, const char **) = elt->u.word;
      else if (**fmt == 'b' && elt->kind == SVN_RA_SVN_WORD)
        {
          if (strcmp(elt->u.word, TRUE_STR) == 0)
            *va_arg(*ap, svn_boolean_t *) = TRUE;
          else if (strcmp(elt->u.word, FALSE_STR) == 0)
            *va_arg(*ap, svn_boolean_t *) = FALSE;
          else
            break;
        }
      else if (**fmt == 'l' && elt->kind == SVN_RA_SVN_LIST)
        *va_arg(*ap, apr_array_header_t **) = elt->u.list;
      else if (**fmt == '(' && elt->kind == SVN_RA_SVN_LIST)
        {
          (*fmt)++;
          SVN_ERR(vparse_tuple(elt->u.list, pool, fmt, ap));
        }
      else if (**fmt == ')')
        return SVN_NO_ERROR;
      else
        break;
    }
  if (**fmt == '?')
    {
      list_level = 0;
      for (; **fmt; (*fmt)++)
        {
          switch (**fmt)
            {
            case '?':
              break;
            case 'r':
              *va_arg(*ap, svn_revnum_t *) = SVN_INVALID_REVNUM;
              break;
            case 's':
              *va_arg(*ap, svn_string_t **) = NULL;
              break;
            case 'c':
            case 'w':
              *va_arg(*ap, const char **) = NULL;
              break;
            case 'l':
              *va_arg(*ap, apr_array_header_t **) = NULL;
              break;
            case '(':
              list_level++;
              break;
            case ')':
              if (--list_level < 0)
                return SVN_NO_ERROR;
              break;
            default:
              abort();
            }
        }
    }
  if (**fmt && **fmt != ')')
    return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                            "Malformed network data");
  return SVN_NO_ERROR;
}

svn_error_t *svn_ra_svn_parse_tuple(apr_array_header_t *list,
                                    apr_pool_t *pool,
                                    const char *fmt, ...)
{
  svn_error_t *err;
  va_list ap;

  va_start(ap, fmt);
  err = vparse_tuple(list, pool, &fmt, &ap);
  va_end(ap);
  return err;
}

svn_error_t *svn_ra_svn_read_tuple(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                   const char *fmt, ...)
{
  va_list ap;
  svn_ra_svn_item_t *item;
  svn_error_t *err;

  SVN_ERR(svn_ra_svn_read_item(conn, pool, &item));
  if (item->kind != SVN_RA_SVN_LIST)
    return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                            "Malformed network data");
  va_start(ap, fmt);
  err = vparse_tuple(item->u.list, pool, &fmt, &ap);
  va_end(ap);
  return err;
}

/* --- READING AND WRITING COMMANDS AND RESPONSES --- */

svn_error_t *svn_ra_svn_read_cmd_response(svn_ra_svn_conn_t *conn,
                                          apr_pool_t *pool,
                                          const char *fmt, ...)
{
  va_list ap;
  const char *status, *message, *file;
  apr_array_header_t *params;
  svn_error_t *err;
  svn_ra_svn_item_t *elt;
  int i;
  apr_uint64_t apr_err, line;

  SVN_ERR(svn_ra_svn_read_tuple(conn, pool, "wl", &status, &params));
  if (strcmp(status, SUCCESS_STR) == 0)
    {
      va_start(ap, fmt);
      err = vparse_tuple(params, pool, &fmt, &ap);
      va_end(ap);
      return err;
    }
  else if (strcmp(status, FAILURE_STR) == 0)
    {
      /* Rebuild the error list from the end, to avoid reversing the order. */
      if (params->nelts == 0)
        return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                                "Empty error list");
      err = NULL;
      for (i = params->nelts - 1; i >= 0; i--)
        {
          elt = &((svn_ra_svn_item_t *) params->elts)[i];
          if (elt->kind != SVN_RA_SVN_LIST)
            return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                                    "Malformed error list");
          SVN_ERR(svn_ra_svn_parse_tuple(elt->u.list, pool, "nccn", &apr_err,
                                         &message, &file, &line));
          /* The message field should have been optional, but we can't
             easily change that, so "" means a nonexistent message. */
          if (!*message)
            message = NULL;
#if APR_CHARSET_EBCDIC
          /* On ebcdic platforms we always assume errors are created with 
           * natively encoded messages. */
          SVN_ERR (svn_utf_cstring_from_utf8(&message, message, pool));
#endif            
          err = svn_error_create(apr_err, err, message);
          err->file = apr_pstrdup(err->pool, file);
          err->line = line;
        }
      return err;
    }

  return svn_error_createf(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                           "Unknown status '%s' in command response", status);
}

svn_error_t *svn_ra_svn_handle_commands(svn_ra_svn_conn_t *conn,
                                        apr_pool_t *pool,
                                        const svn_ra_svn_cmd_entry_t *commands,
                                        void *baton)
{
  apr_pool_t *subpool = svn_pool_create(pool);
  const char *cmdname;
  int i;
  svn_error_t *err, *write_err;
  apr_array_header_t *params;

  while (1)
    {
      apr_pool_clear(subpool);
      SVN_ERR(svn_ra_svn_read_tuple(conn, subpool, "wl", &cmdname, &params));
      for (i = 0; commands[i].cmdname; i++)
        {
          if (strcmp(cmdname, commands[i].cmdname) == 0)
            break;
        }
      if (commands[i].cmdname)
        err = (*commands[i].handler)(conn, subpool, params, baton);
      else
        {
          err = svn_error_createf(SVN_ERR_RA_SVN_UNKNOWN_CMD, NULL,
                                  "Unknown command '%s'", cmdname);
          err = svn_error_create(SVN_ERR_RA_SVN_CMD_ERR, err, NULL);
        }

      if (err && err->apr_err == SVN_ERR_RA_SVN_CMD_ERR)
        {
          write_err = svn_ra_svn_write_cmd_failure(conn, subpool, err->child);
          svn_error_clear(err);
          if (write_err)
            return write_err;
        }
      else if (err)
        return err;

      if (commands[i].terminate)
        break;
    }
  apr_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

svn_error_t *svn_ra_svn_write_cmd(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                  const char *cmdname, const char *fmt, ...)
{
  va_list ap;
  svn_error_t *err;

  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(svn_ra_svn_write_word(conn, pool, cmdname));
  va_start(ap, fmt);
  err = vwrite_tuple(conn, pool, fmt, ap);
  va_end(ap);
  if (err)
    return err;
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  return SVN_NO_ERROR;
}

svn_error_t *svn_ra_svn_write_cmd_response(svn_ra_svn_conn_t *conn,
                                           apr_pool_t *pool,
                                           const char *fmt, ...)
{
  va_list ap;
  svn_error_t *err;

  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(svn_ra_svn_write_word(conn, pool, SUCCESS_STR));
  va_start(ap, fmt);
  err = vwrite_tuple(conn, pool, fmt, ap);
  va_end(ap);
  if (err)
    return err;
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  return SVN_NO_ERROR;
}

svn_error_t *svn_ra_svn_write_cmd_failure(svn_ra_svn_conn_t *conn,
                                          apr_pool_t *pool, svn_error_t *err)
{
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(svn_ra_svn_write_word(conn, pool, FAILURE_STR));
                                            
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  for (; err; err = err->child)
    {
#if APR_CHARSET_EBCDIC
      /* ebcdic platforms must convert the string representation of 
       * err->message and err->file to utf8. */
      SVN_ERR(svn_utf_cstring_to_utf8(&(err->message), err->message, pool));
      SVN_ERR(svn_utf_cstring_to_utf8(&(err->file), err->file, pool)); 
#endif      
      /* The message string should have been optional, but we can't
         easily change that, so marshal nonexistent messages as "". */
      SVN_ERR(svn_ra_svn_write_tuple(conn, pool, "nccn",
                                     (apr_uint64_t) err->apr_err,
                                     err->message ? err->message : "",
                                     err->file, (apr_uint64_t) err->line));
    }
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  return SVN_NO_ERROR;
}
