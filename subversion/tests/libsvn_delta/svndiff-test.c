
/* Test driver for text deltas */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <apr_general.h>
#include "svn_delta.h"
#include "svn_error.h"

/* NOTE: Does no error-checking.  */
static svn_error_t *
read_from_file (void *baton, char *buffer, apr_size_t *len, apr_pool_t *pool)
{
  FILE *fp = baton;

  if (!fp || feof (fp) || ferror (fp))
    *len = 0;
  else
    *len = fread (buffer, 1, *len, fp);
  return SVN_NO_ERROR;
}

static svn_error_t *
count_bytes (void *baton, const char *data, apr_size_t *len,
	     apr_pool_t *pool)
{
  apr_size_t *count = (int *) baton;

  *count += *len;
  return SVN_NO_ERROR;
}


int
main (int argc, char **argv)
{
  FILE *source_file;
  FILE *target_file;
  svn_txdelta_stream_t *stream;
  svn_txdelta_window_t *window;
  svn_txdelta_window_handler_t *handler;
  void *baton;
  apr_size_t new_data = 0, total = 0;
  int num_ops = 0, nwindows = 0;

  source_file = fopen (argv[1], "rb");
  target_file = fopen (argv[2], "rb");

  apr_initialize();
  svn_txdelta (&stream, read_from_file, source_file,
	       read_from_file, target_file, NULL);

  svn_txdelta_to_svndiff (count_bytes, &total, NULL, &handler, &baton);
  do {
    svn_txdelta_next_window (&window, stream);
    handler (window, baton);
    if (window != NULL)
      {
        new_data += window->new->len;
        num_ops += window->num_ops;
        nwindows++;
      }
    svn_txdelta_free_window (window);
  } while (window != NULL);

  svn_txdelta_free (stream);
  fclose (source_file);
  fclose (target_file);

  printf("%d %d %lu %lu\n", nwindows, num_ops, (unsigned long) new_data,
         (unsigned long) total);

  apr_terminate();
  exit (0);
}



/*
 * local variables:
 * eval: (load-file "../../svn-dev.el")
 * end:
 */
