/* 
 * svndiff.c -- Encoding and decoding svndiff-format deltas.
 * 
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */


#include <assert.h>
#include <string.h>
#include "svn_delta.h"
#include "svn_io.h"
#include "delta.h"

#define NORMAL_BITS 7
#define LENGTH_BITS 5


/* ----- Text delta to svndiff ----- */

/* We make one of these and get it passed back to us in calls to the
   window handler.  We only use it to record the write function and
   baton passed to svn_txdelta_to_svndiff ().  */
struct encoder_baton {
  svn_stream_t *output;
  svn_boolean_t header_done;
  apr_pool_t *pool;
};


/* Encode VAL into the buffer P using the variable-length svndiff
   integer format.  Return the incremented value of P after the
   encoded bytes have been written.

   This encoding uses the high bit of each byte as a continuation bit
   and the other seven bits as data bits.  High-order data bits are
   encoded first, followed by lower-order bits, so the value can be
   reconstructed by concatenating the data bits from left to right and
   interpreting the result as a binary number.  Examples (brackets
   denote byte boundaries, spaces are for clarity only):

           1 encodes as [0 0000001]
          33 encodes as [0 0100001]
         129 encodes as [1 0000001] [0 0000001]
        2000 encodes as [1 0001111] [0 1010000]
*/

static char *
encode_int (char *p, apr_off_t val)
{
  int n;
  apr_off_t v;
  unsigned char cont;

  assert (val >= 0);

  /* Figure out how many bytes we'll need.  */
  v = val >> 7;
  n = 1;
  while (v > 0)
    {
      v = v >> 7;
      n++;
    }

  /* Encode the remaining bytes; n is always the number of bytes
     coming after the one we're encoding.  */
  while (--n >= 0)
    {
      cont = ((n > 0) ? 0x1 : 0x0) << 7;
      *p++ = ((val >> (n * 7)) & 0x7f) | cont;
    }

  return p;
}


/* Append an encoded integer to a string.  */
static void
append_encoded_int (svn_string_t *header, apr_off_t val, apr_pool_t *pool)
{
  char buf[128], *p;

  p = encode_int (buf, val);
  svn_string_appendbytes (header, buf, p - buf);
}


static svn_error_t *
window_handler (svn_txdelta_window_t *window, void *baton)
{
  struct encoder_baton *eb = baton;
  apr_pool_t *pool = svn_pool_create (eb->pool);
  svn_string_t *instructions = svn_string_create ("", pool);
  svn_string_t *header = svn_string_create ("", pool);
  char ibuf[128], *ip;
  svn_txdelta_op_t *op;
  svn_error_t *err;
  apr_size_t len;

  /* Make sure we write the header.  */
  if (eb->header_done == FALSE)
    {
      len = 4;
      err = svn_stream_write (eb->output, "SVN\0", &len);
      if (err != SVN_NO_ERROR)
        return err;
      eb->header_done = TRUE;
    }

  if (window == NULL)
    {
      /* We're done; clean up.  */
      len = 0;
      err = svn_stream_close (eb->output);
      apr_pool_destroy (eb->pool);
      return SVN_NO_ERROR;
    }

  /* Encode the instructions.  */
  for (op = window->ops; op < window->ops + window->num_ops; op++)
    {
      /* Encode the action code and length.  */
      ip = ibuf;
      switch (op->action_code)
        {
        case svn_txdelta_source: *ip = (char)0; break;
        case svn_txdelta_target: *ip = (char)(0x1 << 6); break;
        case svn_txdelta_new:    *ip = (char)(0x2 << 6); break;
        }
      if (op->length >> 6 == 0)
        *ip++ |= op->length;
      else
        ip = encode_int (ip + 1, op->length);
      if (op->action_code != svn_txdelta_new)
        ip = encode_int (ip, op->offset);
      svn_string_appendbytes (instructions, ibuf, ip - ibuf);
    }

  /* Encode the header.  */
  append_encoded_int (header, window->sview_offset, pool);
  append_encoded_int (header, window->sview_len, pool);
  append_encoded_int (header, window->tview_len, pool);
  append_encoded_int (header, instructions->len, pool);
  append_encoded_int (header, window->new_data->len, pool);

  /* Write out the window.  */
  len = header->len;
  err = svn_stream_write (eb->output, header->data, &len);
  if (err == SVN_NO_ERROR && instructions->len > 0)
    {
      len = instructions->len;
      err = svn_stream_write (eb->output, instructions->data, &len);
    }
  if (err == SVN_NO_ERROR && window->new_data->len > 0)
    {
      len = window->new_data->len;
      err = svn_stream_write (eb->output, window->new_data->data, &len);
    }

  apr_pool_destroy (pool);
  return err;
}

void
svn_txdelta_to_svndiff (svn_stream_t *output,
			apr_pool_t *pool,
			svn_txdelta_window_handler_t **handler,
			void **handler_baton)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  struct encoder_baton *eb;

  eb = apr_palloc (subpool, sizeof (*eb));
  eb->output = output;
  eb->header_done = FALSE;
  eb->pool = subpool;

  *handler = window_handler;
  *handler_baton = eb;
}



/* ----- svndiff to text delta ----- */

/* An svndiff parser object.  */
struct decode_baton
{
  /* Once the svndiff parser has enough data buffered to create a
     "window", it passes this window to the caller's consumer routine.  */
  svn_txdelta_window_handler_t *consumer_func;
  void *consumer_baton;

  /* Pool to create subpools from; each developing window will be a
     subpool.  */
  apr_pool_t *pool;

  /* The current subpool which contains our current window-buffer.  */
  apr_pool_t *subpool;

  /* The actual svndiff data buffer, living within subpool.  */
  svn_string_t *buffer;

  /* The offset and size of the last source view, so that we can check
     to make sure the next one isn't sliding backwards.  */
  apr_off_t last_sview_offset;
  apr_size_t last_sview_len;

  /* We have to discard four bytes at the beginning for the header.
     This field keeps track of how many of those bytes we have read.  */
  int header_bytes;
};


/* Decode an svndiff-encoded integer into VAL and return a pointer to
   the byte after the integer.  The bytes to be decoded live in the
   range [P..END-1].  See the comment for encode_int earlier in this
   file for more detail on the encoding format.  */

static const unsigned char *
decode_int (apr_off_t *val,
            const unsigned char *p,
            const unsigned char *end)
{
  /* Decode bytes until we're done.  */
  *val = 0;
  while (p < end)
    {
      *val = (*val << 7) | (*p & 0x7f);
      if (((*p++ >> 7) & 0x1) == 0)
        return p;
    }
  return NULL;
}


/* Decode an instruction into OP, returning a pointer to the text
   after the instruction.  Note that if the action code is
   svn_txdelta_new, the opcode field of *OP will not be set.  */

static const unsigned char *
decode_instruction (svn_txdelta_op_t *op,
                    const unsigned char *p,
                    const unsigned char *end)
{
  apr_off_t val;

  if (p == end)
    return NULL;

  /* Decode the instruction selector.  */
  switch ((*p >> 6) & 0x3)
    {
    case 0x0: op->action_code = svn_txdelta_source; break;
    case 0x1: op->action_code = svn_txdelta_target; break;
    case 0x2: op->action_code = svn_txdelta_new; break;
    case 0x3: return NULL;
    }

  /* Decode the length and offset.  */
  op->length = *p++ & 0x3f;
  if (op->length == 0)
    {
      p = decode_int (&val, p, end);
      if (p == NULL)
        return NULL;
      op->length = val;
    }
  if (op->action_code != svn_txdelta_new)
    {
      p = decode_int (&val, p, end);
      if (p == NULL)
        return NULL;
      op->offset = val;
    }

  return p;
}

/* Count the instructions in the range [P..END-1] and make sure they
   are valid for the given window lengths.  Return -1 if the
   instructions are invalid; otherwise return the number of
   instructions.  */
static int
count_and_verify_instructions (const unsigned char *p,
                               const unsigned char *end,
                               apr_size_t sview_len,
                               apr_size_t tview_len,
                               apr_size_t new_len)
{
  int n = 0;
  svn_txdelta_op_t op;
  apr_size_t tpos = 0, npos = 0;

  while (p < end)
    {
      p = decode_instruction (&op, p, end);
      if (p == NULL || op.length < 0 || op.length > tview_len - tpos)
        return -1;
      switch (op.action_code)
        {
        case svn_txdelta_source:
          if (op.offset < 0 || op.length > sview_len - op.offset)
            return -1;
          break;
        case svn_txdelta_target:
          if (op.offset < 0 || op.offset >= tpos)
            return -1;
          break;
        case svn_txdelta_new:
          if (op.length > new_len - npos)
            return -1;
          npos += op.length;
          break;
        }
      tpos += op.length;
      if (tpos < 0)
        return -1;
      n++;
    }
  if (tpos != tview_len || npos != new_len)
    return -1;
  return n;
}

static svn_error_t *
write_handler (void *baton,
               const char *buffer,
               apr_size_t *len)
{
  struct decode_baton *db = (struct decode_baton *) baton;
  const unsigned char *p, *end;
  apr_off_t val, sview_offset;
  apr_size_t sview_len, tview_len, inslen, newlen, remaining, npos;
  svn_txdelta_window_t *window;
  svn_error_t *err;
  svn_txdelta_op_t *op;
  int ninst;

  /* Chew up four bytes at the beginning for the header.  */
  if (db->header_bytes < 4)
    {
      int nheader = 4 - db->header_bytes;
      if (nheader > *len)
        nheader = *len;
      if (memcmp (buffer, "SVN\0" + db->header_bytes, nheader) != 0)
        return svn_error_create (SVN_ERR_MALFORMED_FILE, 0, NULL, db->pool,
                                 "svndiff has invalid header");
      *len -= nheader;
      buffer += nheader;
      db->header_bytes += nheader;
    }

  /* Concatenate the old with the new.  */
  svn_string_appendbytes (db->buffer, buffer, *len);

  /* Read the header, if we have enough bytes for that.  */
  p = (const unsigned char *) db->buffer->data;
  end = (const unsigned char *) db->buffer->data + db->buffer->len;

  p = decode_int (&val, p, end);
  if (p == NULL)
    return SVN_NO_ERROR;
  sview_offset = val;

  p = decode_int (&val, p, end);
  if (p == NULL)
    return SVN_NO_ERROR;
  sview_len = val;

  p = decode_int (&val, p, end);
  if (p == NULL)
    return SVN_NO_ERROR;
  tview_len = val;

  p = decode_int (&val, p, end);
  if (p == NULL)
    return SVN_NO_ERROR;
  inslen = val;

  p = decode_int (&val, p, end);
  if (p == NULL)
    return SVN_NO_ERROR;
  newlen = val;

  /* Check for integer overflow (don't want to let the input trick us
     into invalid pointer games using negative numbers).  */
  if (sview_offset < 0 || sview_len < 0 || tview_len < 0 || inslen < 0
      || newlen < 0 || inslen + newlen < 0 || sview_offset + sview_len < 0)
    return svn_error_create (SVN_ERR_MALFORMED_FILE, 0, NULL, db->pool,
                             "svndiff contains corrupt window header");

  /* Check for source windows which slide backwards.  */
  if (sview_offset < db->last_sview_offset
      || (sview_offset + sview_len
          < db->last_sview_offset + db->last_sview_len))
    return svn_error_create (SVN_ERR_MALFORMED_FILE, 0, NULL, db->pool,
                             "svndiff has backwards-sliding source views");

  /* Wait for more data if we don't have enough bytes for the whole window.  */
  if (end - p < inslen + newlen)
    return SVN_NO_ERROR;

  /* Count the instructions and make sure they are all valid.  */
  end = p + inslen;
  ninst = count_and_verify_instructions (p, end, sview_len, tview_len, newlen);
  if (ninst == -1)
    return svn_error_create (SVN_ERR_MALFORMED_FILE, 0, NULL, db->pool,
                             "svndiff contains invalid instructions");

  /* Build the window structure.  */
  window = apr_palloc (db->subpool, sizeof (*window));
  window->sview_offset = sview_offset;
  window->sview_len = sview_len;
  window->tview_len = tview_len;
  window->num_ops = ninst;
  window->ops_size = ninst;
  window->ops = apr_palloc (db->subpool, ninst * sizeof (*window->ops));
  npos = 0;
  for (op = window->ops; op < window->ops + ninst; op++)
    {
      p = decode_instruction (op, p, end);
      if (op->action_code == svn_txdelta_new)
        {
          op->offset = npos;
          npos += op->length;
        }
    }
  window->new_data
    = svn_string_ncreate ((const char *) p, newlen, db->subpool);
  window->pool = db->subpool;

  /* Send it off.  */
  err = db->consumer_func (window, db->consumer_baton);

  /* Make a new subpool and buffer, saving aside the remaining data in
     the old buffer.  */
  db->subpool = svn_pool_create (db->pool);
  p += newlen;
  remaining = db->buffer->data + db->buffer->len - (const char *) p;
  db->buffer = svn_string_ncreate ((const char *) p, remaining, db->subpool);

  /* Remember the offset and length of the source view for next time.  */
  db->last_sview_offset = sview_offset;
  db->last_sview_len = sview_len;

  /* Free the window; this will also free up our old buffer.  */
  svn_txdelta_free_window (window);

  return err;
}


static svn_error_t *
close_handler (void *baton)
{
  struct decode_baton *db = (struct decode_baton *) baton;
  svn_error_t *err;

  /* Make sure that we're at a plausible end of stream.  */
  if (db->header_bytes < 4 || db->buffer->len != 0)
    return svn_error_create (SVN_ERR_MALFORMED_FILE, 0, NULL, db->pool,
                             "unexpected end of svndiff input");

  /* Tell the window consumer that we're done, and clean up.  */
  err = db->consumer_func (NULL, db->consumer_baton);
  apr_pool_destroy (db->pool);
  return err;
}


svn_stream_t *
svn_txdelta_parse_svndiff (svn_txdelta_window_handler_t *handler,
                           void *handler_baton,
                           apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  struct decode_baton *db = apr_palloc (pool, sizeof (*db));
  svn_stream_t *stream;

  db->consumer_func = handler;
  db->consumer_baton = handler_baton;
  db->pool = subpool;
  db->subpool = svn_pool_create (subpool);
  db->buffer = svn_string_create ("", db->subpool);
  db->last_sview_offset = 0;
  db->last_sview_len = 0;
  db->header_bytes = 0;
  stream = svn_stream_create (db, pool);
  svn_stream_set_write (stream, write_handler);
  svn_stream_set_close (stream, close_handler);
  return stream;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: */
