/*
 * checkout-test.c :  testing checkout
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



#include <stdio.h>
#include <stdlib.h>

#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_file_io.h>
#include <apr_general.h>

#include "svn_types.h"
#include "svn_delta.h"
#include "svn_wc.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_hash.h"



static svn_error_t *
test_read_fn (void *baton, char *buffer, apr_size_t *len, apr_pool_t *pool)
{
  apr_file_t *src = (apr_file_t *) baton;
  apr_status_t stat;

  stat = apr_full_read (src, buffer, (apr_size_t) *len, (apr_size_t *) len);

  if (stat && !APR_STATUS_IS_EOF(stat))
    return
      svn_error_create (stat, 0, NULL, pool,
                        "error reading incoming delta stream");
  
  else 
    return 0;  
}


static svn_error_t *
apply_delta (void *delta_src,
             svn_read_fn_t *read_fn,
             svn_string_t *dest,
             svn_string_t *repos,
             svn_revnum_t revision,
             apr_pool_t *pool)
{
  const svn_delta_edit_fns_t *editor;
  void *edit_baton;
  svn_error_t *err;

  /* Get the editor and friends... */
  err = svn_wc_get_checkout_editor (dest,
                                    repos,
                                    /* Assume we're checking out root. */
                                    svn_string_create ("", pool),
                                    revision,
                                    &editor,
                                    &edit_baton,
                                    pool);
  if (err)
    return err;

  /* ... and edit! */
  return svn_delta_xml_auto_parse (read_fn,
                                   delta_src,
                                   editor,
                                   edit_baton,
                                   svn_string_create ("", pool),
                                   revision,
                                   pool);
}


int
main (int argc, char **argv)
{
  apr_pool_t *pool = NULL;
  apr_status_t apr_err = 0;
  apr_file_t *src = NULL;     /* init to NULL very important! */
  svn_error_t *err = NULL;
  svn_string_t *target = NULL;  /* init to NULL also important here,
                                   because NULL implies delta's top dir */
  char *src_file = NULL;

  apr_initialize ();
  pool = svn_pool_create (NULL);

  if ((argc < 2) || (argc > 3))
    {
      fprintf (stderr, "usage: %s DELTA_SRC_FILE [TARGET_NAME]\n", argv[0]);
      return 1;
    }
  else
    src_file = argv[1];

  apr_err = apr_open (&src, src_file,
                      (APR_READ | APR_CREATE),
                      APR_OS_DEFAULT,
                      pool);

  if (apr_err)
    {
      fprintf (stderr, "error opening %s: %d", src_file, apr_err);
      exit (1);
    }

  if (argc == 3)
    target = svn_string_create (argv[2], pool);

  err = apply_delta
    (src, 
     test_read_fn,
     target,
     svn_string_create (":ssh:jrandom@svn.tigris.org/repos", pool),
     1,  /* kff todo: revision must be passed in, right? */
     pool);
  
  if (err)
    {
      svn_handle_error (err, stdout, 0);
      exit (1);
    }

  apr_close (src);

  return 0;
}





/* -----------------------------------------------------------------
 * local variables:
 * eval: (load-file "../../svn-dev.el")
 * end:
 */
