/* 
 * text-delta.c -- Internal text delta representation
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

#include <apr_general.h>        /* for APR_INLINE */

#include "svn_delta.h"
#include "svn_io.h"
#include "delta.h"



/* Text delta stream descriptor. */

struct svn_txdelta_stream_t {
  /* These are copied from parameters passed to svn_txdelta. */
  svn_stream_t *source;
  svn_stream_t *target;

  /* Private data */
  apr_pool_t *pool;             /* Pool to allocate stream data from. */
  svn_boolean_t more;           /* TRUE if there are more data in the pool. */
  apr_off_t pos;                /* Offset of next read in source file. */
  char *buf;                    /* Buffer for vdelta data. */
  apr_size_t saved_source_len;  /* Amount of source data saved in buf. */
};


/* Text delta applicator.  */

struct apply_baton {
  /* These are copied from parameters passed to svn_txdelta_apply.  */
  svn_stream_t *source;
  svn_stream_t *target;

  /* Private data.  Between calls, SBUF contains the data from the
   * last window's source view, as specified by SBUF_OFFSET and
   * SBUF_LEN.  The contents of TBUF are not interesting between
   * calls.  */
  apr_pool_t *pool;             /* Pool to allocate data from */
  char *sbuf;                   /* Source buffer */
  apr_size_t sbuf_size;         /* Allocated source buffer space */
  apr_off_t sbuf_offset;        /* Offset of SBUF data in source stream */
  apr_size_t sbuf_len;          /* Length of SBUF data */
  char *tbuf;                   /* Target buffer */
  apr_size_t tbuf_size;         /* Allocated target buffer space */
};



/* Allocate a delta window. */

svn_txdelta_window_t *
svn_txdelta__make_window (apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  svn_txdelta_window_t *window;

  window = apr_palloc (subpool, sizeof (*window));
  window->sview_offset = 0;
  window->sview_len = 0;
  window->tview_len = 0;
  window->num_ops = 0;
  window->ops_size = 0;
  window->ops = NULL;
  window->new_data = svn_string_create ("", subpool);
  window->pool = subpool;
  return window;
}



/* Insert a delta op into a delta window. */

void
svn_txdelta__insert_op (svn_txdelta_window_t *window,
                        int opcode,
                        apr_off_t offset,
                        apr_off_t length,
                        const char *new_data)
{
  svn_txdelta_op_t *op;

  /* Create space for the new op. */
  if (window->num_ops == window->ops_size)
    {
      svn_txdelta_op_t *const old_ops = window->ops;
      int const new_ops_size = (window->ops_size == 0
                                ? 16 : 2 * window->ops_size);
      window->ops =
        apr_palloc (window->pool, new_ops_size * sizeof (*window->ops));

      /* Copy any existing ops into the new array */
      if (old_ops)
        memcpy (window->ops, old_ops,
                window->ops_size * sizeof (*window->ops));
      window->ops_size = new_ops_size;
    }

  /* Insert the op. svn_delta_source and svn_delta_target are
     just inserted. For svn_delta_new, the new data must be
     copied into the window. */
  op = &window->ops[window->num_ops];
  switch (opcode)
    {
    case svn_txdelta_source:
    case svn_txdelta_target:
      op->action_code = opcode;
      op->offset = offset;
      op->length = length;
      break;
    case svn_txdelta_new:
      op->action_code = opcode;
      op->offset = window->new_data->len;
      op->length = length;
      svn_string_appendbytes (window->new_data, new_data, length);
      break;
    default:
      assert (!"unknown delta op.");
    }

  ++window->num_ops;
}



/* Allocate a delta stream descriptor. */

void
svn_txdelta (svn_txdelta_stream_t **stream,
             svn_stream_t *source,
             svn_stream_t *target,
             apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  assert (subpool != NULL);

  *stream = apr_palloc (subpool, sizeof (**stream));
  (*stream)->source = source; 
  (*stream)->target = target;
  (*stream)->pool = subpool;
  (*stream)->more = TRUE;
  (*stream)->pos = 0;
  (*stream)->buf = apr_palloc (subpool, 3 * svn_txdelta__window_size);
  (*stream)->saved_source_len = 0;
}


void
svn_txdelta_free (svn_txdelta_stream_t *stream)
{
  if (stream)
    apr_destroy_pool (stream->pool);
}



/* Pull the next delta window from a stream.

   Our current algorithm for picking source and target views is one
   step up from the dumbest algorithm of "compare corresponding blocks
   of each file."  A problem with that algorithm is that an insertion
   or deletion of N bytes near the beginning of the file will result
   in N bytes of non-overlap in each window from then on.  Our
   algorithm lessens this problem by "padding" the source view with
   half a target view's worth of data on each side.

   For example, suppose the target view size is 16K.  The dumbest
   algorithm would use bytes 0-16K for the first source view, 16-32K
   for the second source view, etc..  Our algorithm uses 0-24K for the
   first source view, 8-40K for the second source view, etc..
   Obviously, we're chewing some extra memory by doubling the source
   view size, but small (less than 8K) insertions or deletions won't
   result in non-overlap in every window.

   If we run out of source data before we run out of target data, we
   reuse the final chunk of data for the remaining windows.  No grand
   scheme at work there; that's just how the code worked out. */
svn_error_t *
svn_txdelta_next_window (svn_txdelta_window_t **window,
                         svn_txdelta_stream_t *stream)
{
  if (!stream->more)
    {
      *window = NULL;
      return SVN_NO_ERROR;
    }
  else
    {
      svn_error_t *err;
      apr_size_t total_source_len;
      apr_size_t new_source_len = svn_txdelta__window_size;
      apr_size_t target_len = svn_txdelta__window_size;

      /* If there is no saved source data yet, read an extra half
         window of data this time to get things started. */
      if (stream->saved_source_len == 0)
        new_source_len += svn_txdelta__window_size / 2;

      /* Read the source stream. */
      err = svn_stream_read (stream->source,
                             stream->buf + stream->saved_source_len,
                             &new_source_len);
      total_source_len = stream->saved_source_len + new_source_len;

      /* Read the target stream. */
      if (err == SVN_NO_ERROR)
        err = svn_stream_read (stream->target, stream->buf + total_source_len,
                               &target_len);
      if (err != SVN_NO_ERROR)
        return err;
      stream->pos += new_source_len;

      /* Forget everything if there's no target data. */
      if (target_len == 0)
        {
          *window = NULL;
          stream->more = FALSE;
          return SVN_NO_ERROR;
        }

      /* Create the delta window. */
      *window = svn_txdelta__make_window (stream->pool);
      (*window)->sview_offset = stream->pos - total_source_len;
      (*window)->sview_len = total_source_len;
      (*window)->tview_len = target_len;
      svn_txdelta__vdelta (*window, stream->buf,
                           total_source_len, target_len,
                           stream->pool);

      /* Save the last window's worth of data from the source view. */
      stream->saved_source_len = (total_source_len < svn_txdelta__window_size)
        ? total_source_len : svn_txdelta__window_size;
      memmove (stream->buf,
               stream->buf + total_source_len - stream->saved_source_len,
               stream->saved_source_len);

      /* That's it. */
      return SVN_NO_ERROR;
    }
}


void
svn_txdelta_free_window (svn_txdelta_window_t *window)
{
  if (window)
    apr_destroy_pool (window->pool);
}



/* Functions for applying deltas.  */

/* Ensure that BUF has enough space for VIEW_LEN bytes.  */
static APR_INLINE void
size_buffer (char **buf, apr_size_t *buf_size,
             apr_size_t view_len, apr_pool_t *pool)
{
  if (view_len > *buf_size)
    {
      *buf_size *= 2;
      if (*buf_size < view_len)
        *buf_size = view_len;
      *buf = apr_palloc (pool, *buf_size);
    }
}


/* Apply the instructions from WINDOW to a source view SBUF to produce
   a target view TBUF.  SBUF is assumed to have WINDOW->sview_len
   bytes of data and TBUF is assumed to have room for
   WINDOW->tview_len bytes of output.  This is purely a memory
   operation; nothing can go wrong as long as we have a valid window.  */

static void
apply_instructions (svn_txdelta_window_t *window, const char *sbuf, char *tbuf)
{
  svn_txdelta_op_t *op;
  apr_size_t i, tpos = 0;

  for (op = window->ops; op < window->ops + window->num_ops; op++)
    {
      /* Check some invariants common to all instructions.  */
      assert (op->offset >= 0 && op->length >= 0);
      assert (tpos + op->length <= window->tview_len);

      switch (op->action_code)
        {
        case svn_txdelta_source:
          /* Copy from source area.  */
          assert (op->offset + op->length <= window->sview_len);
          memcpy (tbuf + tpos, sbuf + op->offset, op->length);
          tpos += op->length;
          break;

        case svn_txdelta_target:
          /* Copy from target area.  Don't use memcpy() since its
             semantics aren't guaranteed for overlapping memory areas,
             and target copies are allowed to overlap to generate
             repeated data.  */
          assert (op->offset < tpos);
          for (i = op->offset; i < op->offset + op->length; i++)
            tbuf[tpos++] = tbuf[i];
          break;

        case svn_txdelta_new:
          /* Copy from window new area.  */
          assert (op->offset + op->length <= window->new_data->len);
          memcpy (tbuf + tpos,
                  window->new_data->data + op->offset,
                  op->length);
          tpos += op->length;
          break;

        default:
          assert ("Invalid delta instruction code" == NULL);
        }
    }

  /* Check that we produced the right amount of data.  */
  assert (tpos == window->tview_len);
}


/* Apply WINDOW to the streams given by APPL.  */
static svn_error_t *
apply_window (svn_txdelta_window_t *window, void *baton)
{
  struct apply_baton *ab = (struct apply_baton *) baton;
  apr_size_t len;
  svn_error_t *err;

  if (window == NULL)
    {
      /* We're done; just clean up.  */
      apr_destroy_pool (ab->pool);
      return SVN_NO_ERROR;
    }

  /* Make sure the source view didn't slide backwards.  */
  assert (window->sview_offset >= ab->sbuf_offset
          && (window->sview_offset + window->sview_len
              >= ab->sbuf_offset + ab->sbuf_len));

  /* Make sure there's enough room in the target buffer.  */
  size_buffer (&ab->tbuf, &ab->tbuf_size, window->tview_len, ab->pool);

  /* Prepare the source buffer for reading from the input stream.  */
  if (window->sview_offset != ab->sbuf_offset
      || window->sview_len > ab->sbuf_size)
    {
      char *old_sbuf = ab->sbuf;

      /* Make sure there's enough room.  */
      size_buffer (&ab->sbuf, &ab->sbuf_size, window->sview_len, ab->pool);

      /* If the existing view overlaps with the new view, copy the
       * overlap to the beginning of the new buffer.  */
      if (ab->sbuf_offset + ab->sbuf_len > window->sview_offset)
        {
          apr_size_t start = window->sview_offset - ab->sbuf_offset;
          memmove (ab->sbuf, old_sbuf + start, ab->sbuf_len - start);
          ab->sbuf_len -= start;
        }
      else
        ab->sbuf_len = 0;
      ab->sbuf_offset = window->sview_offset;
    }

  /* Read the remainder of the source view into the buffer.  */
  if (ab->sbuf_len < window->sview_len)
    {
      len = window->sview_len - ab->sbuf_len;
      err = svn_stream_read (ab->source, ab->sbuf + ab->sbuf_len, &len);
      if (err == SVN_NO_ERROR && len != window->sview_len - ab->sbuf_len)
        err = svn_error_create (SVN_ERR_INCOMPLETE_DATA, 0, NULL, ab->pool,
                                "Delta source ended unexpectedly");
      if (err != SVN_NO_ERROR)
        return err;
      ab->sbuf_len = window->sview_len;
    }

  /* Apply the window instructions to the source view to generate
     the target view.  */
  apply_instructions (window, ab->sbuf, ab->tbuf);

  /* Write out the output. */
  len = window->tview_len;
  err = svn_stream_write (ab->target, ab->tbuf, &len);
  return err;
}


void
svn_txdelta_apply (svn_stream_t *source,
                   svn_stream_t *target,
                   apr_pool_t *pool,
                   svn_txdelta_window_handler_t **handler,
                   void **handler_baton)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  struct apply_baton *ab;
  assert (pool != NULL);

  ab = apr_palloc (subpool, sizeof (*ab));
  ab->source = source;
  ab->target = target;
  ab->pool = subpool;
  ab->sbuf = NULL;
  ab->sbuf_size = 0;
  ab->sbuf_offset = 0;
  ab->sbuf_len = 0;
  ab->tbuf = NULL;
  ab->tbuf_size = 0;
  *handler = apply_window;
  *handler_baton = ab;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
