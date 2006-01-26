/*
 * stream.c:   svn_stream operations
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

#include "svn_private_config.h"

#include <assert.h>
#include <stdio.h>

#include <apr.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_file_io.h>
#include <apr_errno.h>
#include <apr_md5.h>

#ifdef SVN_HAVE_ZLIB
#include <zlib.h>
#endif

#include "svn_pools.h"
#include "svn_io.h"
#include "svn_error.h"
#include "svn_string.h"
#include "svn_utf.h"


struct svn_stream_t {
  void *baton;
  svn_read_fn_t read_fn;
  svn_write_fn_t write_fn;
  svn_close_fn_t close_fn;
};



/*** Generic streams. ***/

svn_stream_t *
svn_stream_create (void *baton, apr_pool_t *pool)
{
  svn_stream_t *stream;

  stream = apr_palloc (pool, sizeof (*stream));
  stream->baton = baton;
  stream->read_fn = NULL;
  stream->write_fn = NULL;
  stream->close_fn = NULL;
  return stream;
}


void
svn_stream_set_baton (svn_stream_t *stream, void *baton)
{
  stream->baton = baton;
}


void
svn_stream_set_read (svn_stream_t *stream, svn_read_fn_t read_fn)
{
  stream->read_fn = read_fn;
}


void
svn_stream_set_write (svn_stream_t *stream, svn_write_fn_t write_fn)
{
  stream->write_fn = write_fn;
}


void
svn_stream_set_close (svn_stream_t *stream, svn_close_fn_t close_fn)
{
  stream->close_fn = close_fn;
}


svn_error_t *
svn_stream_read (svn_stream_t *stream, char *buffer, apr_size_t *len)
{
  assert (stream->read_fn != NULL);
  return stream->read_fn (stream->baton, buffer, len);
}


svn_error_t *
svn_stream_write (svn_stream_t *stream, const char *data, apr_size_t *len)
{
  assert (stream->write_fn != NULL);
  return stream->write_fn (stream->baton, data, len);
}


svn_error_t *
svn_stream_close (svn_stream_t *stream)
{
  if (stream->close_fn == NULL)
    return SVN_NO_ERROR;
  return stream->close_fn (stream->baton);
}


svn_error_t *
svn_stream_printf (svn_stream_t *stream,
                   apr_pool_t *pool,
                   const char *fmt,
                   ...)
{
  const char *message;
  va_list ap;
  apr_size_t len;

  va_start (ap, fmt);
  message = apr_pvsprintf (pool, fmt, ap);
  va_end (ap);
  
  len = strlen (message);
  return svn_stream_write (stream, message, &len);
}


svn_error_t *
svn_stream_printf_from_utf8 (svn_stream_t *stream,
                             const char *encoding,
                             apr_pool_t *pool,
                             const char *fmt,
                             ...)
{
  const char *message, *translated;
  va_list ap;
  apr_size_t len;

  va_start (ap, fmt);
  message = apr_pvsprintf (pool, fmt, ap);
  va_end (ap);

  SVN_ERR (svn_utf_cstring_from_utf8_ex (&translated, message, encoding,
                                         NULL, pool));
  
  len = strlen (translated);

  return svn_stream_write (stream, translated, &len);
}


svn_error_t *
svn_stream_readline (svn_stream_t *stream,
                     svn_stringbuf_t **stringbuf,
                     const char *eol,
                     svn_boolean_t *eof,
                     apr_pool_t *pool)
{
  apr_size_t numbytes;
  const char *match;
  char c;
  svn_stringbuf_t *str = svn_stringbuf_create ("", pool);

  /* Since we're reading one character at a time, let's at least
     optimize for the 90% case.  90% of the time, we can avoid the
     stringbuf ever having to realloc() itself if we start it out at
     80 chars.  */
  svn_stringbuf_ensure (str, 80);

  match = eol;
  while (*match)
    {
      numbytes = 1;
      SVN_ERR (svn_stream_read (stream, &c, &numbytes));
      if (numbytes != 1)
        {
          /* a 'short' read means the stream has run out. */
          *eof = TRUE;
          *stringbuf = str;
          return SVN_NO_ERROR;
        }

      if (c == *match)
        match++;
      else
        match = eol;

      svn_stringbuf_appendbytes (str, &c, 1);
    }

  *eof = FALSE;
  svn_stringbuf_chop (str, match - eol);
  *stringbuf = str;
  return SVN_NO_ERROR;
}


svn_error_t *svn_stream_copy (svn_stream_t *from, svn_stream_t *to,
                              apr_pool_t *pool)
{
  char *buf = apr_palloc (pool, SVN__STREAM_CHUNK_SIZE);
  apr_size_t len;

  /* Read and write chunks until we get a short read, indicating the
     end of the stream.  (We can't get a short write without an
     associated error.) */
  while (1)
    {
      len = SVN__STREAM_CHUNK_SIZE;
      SVN_ERR (svn_stream_read (from, buf, &len));
      if (len > 0)
        SVN_ERR (svn_stream_write (to, buf, &len));
      if (len != SVN__STREAM_CHUNK_SIZE)
        break;
    }
  return SVN_NO_ERROR;
}




/*** Generic readable empty stream ***/

static svn_error_t *
read_handler_empty (void *baton, char *buffer, apr_size_t *len)
{
  *len = 0;
  return SVN_NO_ERROR;
}


static svn_error_t *
write_handler_empty (void *baton, const char *data, apr_size_t *len)
{
  return SVN_NO_ERROR;
}


svn_stream_t *
svn_stream_empty (apr_pool_t *pool)
{
  svn_stream_t *stream;

  stream = svn_stream_create (NULL, pool);
  svn_stream_set_read (stream, read_handler_empty);
  svn_stream_set_write (stream, write_handler_empty);
  return stream;
}




/*** Ownership detaching stream ***/

static svn_error_t *
read_handler_disown (void *baton, char *buffer, apr_size_t *len)
{
  return svn_stream_read ((svn_stream_t *)baton, buffer, len);
}

static svn_error_t *
write_handler_disown (void *baton, const char *buffer, apr_size_t *len)
{
  return svn_stream_write ((svn_stream_t *)baton, buffer, len);
}


svn_stream_t *
svn_stream_disown (svn_stream_t *stream, apr_pool_t *pool)
{
  svn_stream_t *s = svn_stream_create (stream, pool);

  svn_stream_set_read (s, read_handler_disown);
  svn_stream_set_write (s, write_handler_disown);

  return s;
}



/*** Generic stream for APR files ***/
struct baton_apr {
  apr_file_t *file;
  apr_pool_t *pool;
};


static svn_error_t *
read_handler_apr (void *baton, char *buffer, apr_size_t *len)
{
  struct baton_apr *btn = baton;
  svn_error_t *err;

  err = svn_io_file_read_full (btn->file, buffer, *len, len, btn->pool);
  if (err && APR_STATUS_IS_EOF(err->apr_err))
    {
      svn_error_clear (err);
      err = SVN_NO_ERROR;
    }

  return err;
}


static svn_error_t *
write_handler_apr (void *baton, const char *data, apr_size_t *len)
{
  struct baton_apr *btn = baton;

  return svn_io_file_write_full (btn->file, data, *len, len, btn->pool);
}

static svn_error_t *
close_handler_apr (void *baton)
{
  struct baton_apr *btn = baton;

  return svn_io_file_close (btn->file, btn->pool);
}


svn_stream_t *
svn_stream_from_aprfile2 (apr_file_t *file,
                          svn_boolean_t disown,
                          apr_pool_t *pool)
{
  struct baton_apr *baton;
  svn_stream_t *stream;

  if (file == NULL)
    return svn_stream_empty(pool);

  baton = apr_palloc (pool, sizeof (*baton));
  baton->file = file;
  baton->pool = pool;
  stream = svn_stream_create (baton, pool);
  svn_stream_set_read (stream, read_handler_apr);
  svn_stream_set_write (stream, write_handler_apr);

  if (! disown)
    svn_stream_set_close (stream, close_handler_apr);

  return stream;
}

svn_stream_t *
svn_stream_from_aprfile (apr_file_t *file, apr_pool_t *pool)
{
  return svn_stream_from_aprfile2 (file, TRUE, pool);
}



/* Compressed stream support */

#ifdef SVN_HAVE_ZLIB

#define ZBUFFER_SIZE 4096       /* The size of the buffer the
                                   compressed stream uses to read from
                                   the substream. Basically an
                                   arbitrary value, picked to be about
                                   page-sized. */

struct zbaton {
  z_stream *in;                 /* compressed stream for reading */
  z_stream *out;                /* compressed stream for writing */
  svn_read_fn_t read;           /* substream's read function */
  svn_write_fn_t write;         /* substream's write function */
  svn_close_fn_t close;         /* substream's close function */
  void *read_buffer;            /* buffer   used   for  reading   from
                                   substream */
  int read_flush;               /* what flush mode to use while
                                   reading */
  apr_pool_t *pool;             /* The pool this baton is allocated
                                   on */
  void *subbaton;               /* The substream's baton */
};

/* zlib alloc function. opaque is the pool we need. */
static voidpf
zalloc(voidpf opaque, uInt items, uInt size)
{
  apr_pool_t *pool = opaque;
  
  return apr_palloc(pool, items * size);
}

/* zlib free function */
static void
zfree(voidpf opaque, voidpf address)
{
  /* Empty, since we allocate on the pool */
}

/* Converts a zlib error to an svn_error_t. zerr is the error code,
   function is the function name, and stream is the z_stream we are
   using.  */
static svn_error_t *
zerr_to_svn_error (int zerr, const char *function, z_stream *stream)
{
  apr_status_t status;
  const char *message;

  if (zerr == Z_OK)
    return SVN_NO_ERROR;
  
  switch (zerr)
    {
    case Z_STREAM_ERROR:
      status = SVN_ERR_STREAM_MALFORMED_DATA;
      message = "stream error";
      break;
      
    case Z_MEM_ERROR:
      status = APR_ENOMEM;
      message = "out of memory";
      break;
      
    case Z_BUF_ERROR:
      status = APR_ENOMEM;
      message = "buffer error";
      break;
      
    case Z_VERSION_ERROR:
      status = SVN_ERR_STREAM_UNRECOGNIZED_DATA;
      message = "version error";
      break;
      
    case Z_DATA_ERROR:
      status = SVN_ERR_STREAM_MALFORMED_DATA;
      message = "corrupted data";
      break;
      
    default:
      status = SVN_ERR_STREAM_UNRECOGNIZED_DATA;
      message = "error";
      break;
    }
  
  if (stream->msg != NULL)
    return svn_error_createf (status, NULL, "zlib (%s): %s: %s", function,
                              message, stream->msg);
  else
    return svn_error_createf (status, NULL, "zlib (%s): %s", function, 
                              message);
}

/* Helper function to figure out the sync mode */
static svn_error_t *
read_helper_gz (svn_read_fn_t read_fn,
                void *baton,
                char *buffer, 
                uInt *len, int *zflush)
{
  uInt orig_len = *len;

  /* There's no reason this value should grow bigger than the range of
     uInt, but Subversion's API requires apr_size_t. */
  apr_size_t apr_len = (apr_size_t) *len;
  
  SVN_ERR ((*read_fn) (baton, buffer, &apr_len));
  
  /* Type cast back to uInt type that zlib uses.  On LP64 platforms
     apr_size_t will be bigger than uInt. */
  *len = (uInt) apr_len;
  
  /* I wanted to use Z_FINISH here, but we need to know our buffer is
     big enough */
  *zflush = (*len) < orig_len ? Z_SYNC_FLUSH : Z_SYNC_FLUSH; 
  
  return SVN_NO_ERROR;
}

/* Handle reading from a compressed stream */
static svn_error_t *
read_handler_gz (void *baton, char *buffer, apr_size_t *len)
{
  struct zbaton *btn = baton;
  int zerr;

  if (btn->in == NULL)
    {
      btn->in = apr_palloc (btn->pool, sizeof (z_stream));
      btn->in->zalloc = zalloc;
      btn->in->zfree = zfree;
      btn->in->opaque = btn->pool;
      btn->read_buffer = apr_palloc(btn->pool, ZBUFFER_SIZE);
      btn->in->next_in = btn->read_buffer;
      btn->in->avail_in = ZBUFFER_SIZE;
      
      SVN_ERR (read_helper_gz (btn->read, btn->subbaton, btn->read_buffer,
                               &btn->in->avail_in, &btn->read_flush));
                               
      zerr = inflateInit (btn->in);
      SVN_ERR (zerr_to_svn_error (zerr, "inflateInit", btn->in));
    }
  
  btn->in->next_out = (Bytef *) buffer;
  btn->in->avail_out = *len;
  
  while (btn->in->avail_out > 0) 
    {
      if (btn->in->avail_in <= 0)
        {
          btn->in->avail_in = ZBUFFER_SIZE;
          btn->in->next_in = btn->read_buffer;
          SVN_ERR (read_helper_gz (btn->read, btn->subbaton, btn->read_buffer, 
                                   &btn->in->avail_in, &btn->read_flush));
        }
      
      zerr = inflate (btn->in, btn->read_flush);
      if (zerr == Z_STREAM_END)
        break;
      else if (zerr != Z_OK)
        return zerr_to_svn_error(zerr, "inflate", btn->in);
    }

  *len -= btn->in->avail_out;
  return SVN_NO_ERROR;
}

/* Compress data and write it to the substream */
static svn_error_t *
write_handler_gz (void *baton, const char *buffer, apr_size_t *len)
{
  struct zbaton *btn = baton;
  apr_pool_t *subpool;
  void *write_buf;
  apr_size_t buf_size, write_len;
  int zerr;

  if (btn->out == NULL)
    {
      btn->out = apr_palloc (btn->pool, sizeof (z_stream));
      btn->out->zalloc = zalloc;
      btn->out->zfree = zfree;
      btn->out->opaque =  btn->pool;
      
      zerr = deflateInit (btn->out, Z_DEFAULT_COMPRESSION);
      SVN_ERR (zerr_to_svn_error (zerr, "deflateInit", btn->out));
    }
  
  /* The largest buffer we should need is 0.1% larger than the
     compressed data, + 12 bytes. This info comes from zlib.h.  */
  buf_size = *len + (*len / 1000) + 13;
  subpool = svn_pool_create (btn->pool);
  write_buf = apr_palloc (subpool, buf_size);
  
  btn->out->next_in = (Bytef *) buffer;  /* Casting away const! */
  btn->out->avail_in = *len;
  
  while (btn->out->avail_in > 0)
    {
      btn->out->next_out = write_buf;
      btn->out->avail_out = buf_size;
      
      zerr = deflate (btn->out, Z_NO_FLUSH);
      SVN_ERR (zerr_to_svn_error (zerr, "deflate", btn->out));
      write_len = buf_size - btn->out->avail_out;
      if (write_len > 0)
        SVN_ERR (btn->write (btn->subbaton, write_buf, &write_len));
    }
      
  svn_pool_destroy (subpool);

  return SVN_NO_ERROR;
}

/* Handle flushing and closing the stream */
static svn_error_t *
close_handler_gz (void *baton)
{
  struct zbaton *btn = baton;
  int zerr;
  
  if (btn->in != NULL)
    {
      zerr = inflateEnd(btn->in);
      SVN_ERR (zerr_to_svn_error (zerr, "inflateEnd", btn->in));
    }

  if (btn->out != NULL)
    {
      void *buf;
      apr_size_t write_len;
      
      buf = apr_palloc (btn->pool, ZBUFFER_SIZE);
      
      while (TRUE)
        {
          btn->out->next_out = buf;
          btn->out->avail_out = ZBUFFER_SIZE;
          
          zerr = deflate (btn->out, Z_FINISH);
          if (zerr != Z_STREAM_END && zerr != Z_OK)
            return zerr_to_svn_error (zerr, "deflate", btn->out);
          write_len = ZBUFFER_SIZE - btn->out->avail_out;
          if (write_len > 0)
            SVN_ERR (btn->write (btn->subbaton, buf, &write_len));
          if (zerr == Z_STREAM_END)
            break;
        }
      
      zerr = deflateEnd(btn->out);
      SVN_ERR (zerr_to_svn_error (zerr, "deflateEnd", btn->out));
    }

  if (btn->close != NULL)
    return btn->close (btn->subbaton);
  else
    return SVN_NO_ERROR;
}

#endif /* SVN_HAVE_ZLIB */

svn_stream_t *
svn_stream_compressed (svn_stream_t *stream, apr_pool_t *pool)
{
#ifdef SVN_HAVE_ZLIB

  struct svn_stream_t *zstream;
  struct zbaton *baton;

  assert(stream != NULL);
  
  baton = apr_palloc (pool, sizeof (*baton));
  baton->in = baton->out = NULL;
  baton->read = stream->read_fn;
  baton->write = stream->write_fn;
  baton->close = stream->close_fn;
  baton->subbaton = stream->baton;
  baton->pool = pool;
  baton->read_buffer = NULL;
  baton->read_flush = Z_SYNC_FLUSH;
  
  zstream = svn_stream_create(baton, pool);
  svn_stream_set_read(zstream, read_handler_gz);
  svn_stream_set_write(zstream, write_handler_gz);
  svn_stream_set_close(zstream, close_handler_gz);
  
  return zstream;

#else
  
  return stream;

#endif /* SVN_HAVE_ZLIB */
}


/* MD5 checked stream support */

struct md5_stream_baton
{
  apr_md5_ctx_t read_ctx, write_ctx;
  unsigned char **read_digest;
  unsigned char **write_digest;
  unsigned char read_digest_buf[APR_MD5_DIGESTSIZE];
  unsigned char write_digest_buf[APR_MD5_DIGESTSIZE];
  svn_stream_t *proxy;
};

static svn_error_t *
read_handler_md5 (void *baton, char *buffer, apr_size_t *len)
{
  struct md5_stream_baton *btn = baton;

  SVN_ERR (svn_stream_read (btn->proxy, buffer, len));

  if (btn->read_digest)
    {
      apr_status_t apr_err = apr_md5_update (&btn->read_ctx, buffer, *len);

      if (apr_err)
        return svn_error_create (apr_err, NULL, NULL);
    }

  return SVN_NO_ERROR;
}


static svn_error_t *
write_handler_md5 (void *baton, const char *buffer, apr_size_t *len)
{
  struct md5_stream_baton *btn = baton;

  if (btn->write_digest && *len > 0)
    {
      apr_status_t apr_err = apr_md5_update (&btn->write_ctx, buffer, *len);

      if (apr_err)
        return svn_error_create (apr_err, NULL, NULL);
    }

  return svn_stream_write (btn->proxy, buffer, len);
}


static svn_error_t *
close_handler_md5 (void *baton)
{
  struct md5_stream_baton *btn = baton;

  if (btn->read_digest)
    {
      apr_status_t apr_err
        = apr_md5_final (btn->read_digest_buf, &btn->read_ctx);

      if (apr_err)
        return svn_error_create (apr_err, NULL, NULL);

      *btn->read_digest = btn->read_digest_buf;
    }

  if (btn->write_digest)
    {
      apr_status_t apr_err
        = apr_md5_final (btn->write_digest_buf, &btn->write_ctx);

      if (apr_err)
        return svn_error_create (apr_err, NULL, NULL);

      *btn->write_digest = btn->write_digest_buf;
    }

  return svn_stream_close (btn->proxy);
}


svn_stream_t *
svn_stream_checksummed (svn_stream_t *stream,
                        unsigned char **read_digest,
                        unsigned char **write_digest,
                        apr_pool_t *pool)
{
  svn_stream_t *s;
  struct md5_stream_baton *baton;

  if (! read_digest && ! write_digest)
    return stream;

  baton = apr_palloc (pool, sizeof (*baton));
  apr_md5_init (&baton->read_ctx);
  apr_md5_init (&baton->write_ctx);
  baton->read_digest = read_digest;
  baton->write_digest = write_digest;
  baton->proxy = stream;

  s = svn_stream_create (baton, pool);
  svn_stream_set_read (s, read_handler_md5);
  svn_stream_set_write (s, write_handler_md5);
  svn_stream_set_close (s, close_handler_md5);
  return s;
}




/* Miscellaneous stream functions. */
struct string_stream_baton
{
  svn_stringbuf_t *str;
  apr_size_t amt_read;
};

static svn_error_t *
read_handler_string (void *baton, char *buffer, apr_size_t *len)
{
  struct string_stream_baton *btn = baton;
  apr_size_t left_to_read = btn->str->len - btn->amt_read;

  *len = (*len > left_to_read) ? left_to_read : *len;
  memcpy (buffer, btn->str->data + btn->amt_read, *len);
  btn->amt_read += *len;
  return SVN_NO_ERROR;
}

static svn_error_t *
write_handler_string (void *baton, const char *data, apr_size_t *len)
{
  struct string_stream_baton *btn = baton;

  svn_stringbuf_appendbytes (btn->str, data, *len);
  return SVN_NO_ERROR;
}

svn_stream_t *
svn_stream_from_stringbuf (svn_stringbuf_t *str,
                           apr_pool_t *pool)
{
  svn_stream_t *stream;
  struct string_stream_baton *baton;

  if (! str)
    return svn_stream_empty (pool);

  baton = apr_palloc (pool, sizeof (*baton));
  baton->str = str;
  baton->amt_read = 0;
  stream = svn_stream_create (baton, pool);
  svn_stream_set_read (stream, read_handler_string);
  svn_stream_set_write (stream, write_handler_string);
  return stream;
}


svn_error_t *
svn_stream_for_stdout (svn_stream_t **out, apr_pool_t *pool)
{
  apr_file_t *stdout_file;
  apr_status_t apr_err;

  apr_err = apr_file_open_stdout (&stdout_file, pool);
  if (apr_err)
    return svn_error_wrap_apr (apr_err, "Can't open stdout");

  *out = svn_stream_from_aprfile (stdout_file, pool);

  return SVN_NO_ERROR;
}
