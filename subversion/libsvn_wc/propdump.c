/*
 * propdump.c :  dumping and undumping property lists from a file
 *
 * ================================================================
 * Copyright (c) 2000 Collab.Net.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. The end-user documentation included with the redistribution, if
 * any, must include the following acknowlegement: "This product includes
 * software developed by Collab.Net (http://www.Collab.Net/)."
 * Alternately, this acknowlegement may appear in the software itself, if
 * and wherever such third-party acknowlegements normally appear.
 * 
 * 4. The hosted project names must not be used to endorse or promote
 * products derived from this software without prior written
 * permission. For written permission, please contact info@collab.net.
 * 
 * 5. Products derived from this software may not use the "Tigris" name
 * nor may "Tigris" appear in their names without prior written
 * permission of Collab.Net.
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL COLLAB.NET OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by many
 * individuals on behalf of Collab.Net.
 */


/* The format of property files is:
 *
 *    N <nlength>
 *    name (a string of <nlength> bytes, followed by a newline)
 *    V <vlength>
 *    val (a string of <vlength> bytes, followed by a newline)
 *
 * For example:
 *
 *    N 5
 *    color
 *    V 3
 *    red
 *    N 11
 *    wine review
 *    V 372
 *    A forthright entrance, yet coquettish on the tongue, its deceptively
 *    fruity exterior hides the warm mahagony undercurrent that is the
 *    hallmark of Chateau Fraisant-Pitre.  Connoisseurs of the region will
 *    be pleased to note the familiar, subtle hints of mulberries and
 *    carburator fluid.  A confident finish, marred only by a barely
 *    detectable suggestion of rancid squid ink.
 *    N 5 
 *    price
 *    V 8
 *    US $6.50
 *
 * and so on.
 *
 */




/* 
 * This code is about storing property lists (hashes whose keys
 * and values are UTF-8 strings) to files, and reading them back
 * again.
 *
 * The format is designed for human readability; that's not
 * necessarily the most efficient thing, but debuggability is worth a
 * lot too.
 */



#include <svn_types.h>
#include <svn_string.h>
#include <svn_error.h>
#include <apr_hash.h>
#include <apr_pools.h>
#include <apr_file_io.h>




/* kff todo: this is probably of general use. */

#define MAX_BASE 16

/* In BUF, convert signed integer NUM to a string, in base BASE.
 * Caller must already have allocated BUF.
 *
 * Return the number of bytes used, not including terminating NULL. 
 *
 * If bad base, or anything else is wrong, returns -1.
 */
static size_t
num_into_string (char *buf, int num, int base)
{
  char *p, *flip;
  size_t len;
  int negative = 0;

  /* 
   * p:     A moving index into buf, as we build the reversed string.
   * flip:  Used when we unreversing the string.
   */
  
  if ((base < 2) || (base > MAX_BASE))
    return -1;

  p = buf;

  /* Handle trivial case first. */
  if (num == 0)
  {
    *p++ = '0';
    *p = '\0';
    return (1);
  }

  /* Why do you always have to be so negative? */
  if (num < 0)
  {
    negative = 1;
    num = -num;
  }

  while (num > 0)
  {
    int i;

    i = (num % base);

    /* Ascii numerology. */
    if (i > 9)
      i += ('A' - ('9' + 1));

    *p++ = '0' + i;   /* '0' + num  ==>  ascii value of num */
    num /= base;
  }

  if (negative)
    *p++ = '-';

  /* Null terminate, and start the turnaround. */
  *p = '\0';

  /* Record the length now. */
  len = (p - buf);

  /* Now we have the string in reverse.  Flip it. */

  flip = buf;
  p--;
  while (p > flip) 
  {
    char c;
    c = *flip;
    *flip++ = *p;
    *p-- = c;
  }

  return (len);
}


/* kff todo: there's that pesky size_t to int conversion happening
 * again.  I think it's safe in the ranges we're dealing with here,
 * but this could be a gotcha... Jim, Greg, anyone want to comment?
 */
static size_t      /* Returns number of bytes in result. */
size_t_into_string (char *buf, size_t num)
{
  return num_into_string (buf, num, 10);
}


/* kff todo: again, of general utility */
void
guaranteed_ap_write (ap_file_t *dest, const void *buf, ap_ssize_t n)
{
  /* kff todo: waiting for answer back from svn-core about ssize_t */
  ap_ssize_t nwritten;

  nwritten = n;
  do {
    ap_write (dest, buf, &nwritten);
  } while ((n - nwritten) > 0);
}



/* kff todo: should it return ap_status_t? */
void
svn_wc_proplist_write (svn_proplist_t *proplist, 
                       svn_string_t *destfile_name)
{
  ap_file_t *destfile = NULL;   /* this init to NULL is actually important */
  ap_status_t res;
  ap_pool_t *pool = NULL;
  ap_hash_index_t *this;      /* current hash entry */
  
  res = ap_create_pool (&pool, NULL);
  if (res != APR_SUCCESS)
    {
      /* kff todo: need to copy CVS's error-handling better, or something.
       * 
       * Example: here we have a mem allocation error, probably, so
       * our exit plan must include more allocation! :-)
       * 
       * Go through code of svn_handle_error, look for stuff like
       * this.
       */
      exit (1);
    }

  /* kff todo: maybe this whole file-opening thing wants to be
     abstracted?  We jump through these same hoops in svn_parse.c as
     well, after all... */

  res = ap_open (&destfile,
                 svn_string_2cstring (destfile_name, pool),
                 (APR_WRITE | APR_CREATE),
                 APR_OS_DEFAULT,  /* kff todo: what's this about? */
                 pool);

  if (res != APR_SUCCESS)
    {
      svn_string_t *msg;
      msg = svn_string_create ("svn_wc_proplist_write(): "
                               "can't open for writing, file ",
                               pool);
      svn_string_appendstr (msg, destfile_name, pool);

      /* Declare this a fatal error! */
      svn_handle_error (svn_create_error (res, SVN_FATAL, msg, pool));
    }

  /* Else file successfully opened.  Continue. */

  for (this = ap_hash_first (proplist); this; this = ap_hash_next (this))
    {
      void *key, *val;
      size_t num_len;
      char buf[100];   /* Only holds lengths expressed in decimal digits. */

      /* Get this key and val. */
      ap_hash_this (this, &key, NULL, &val);

      /* Output the name's length, then the name itself. */
      guaranteed_ap_write (destfile, "N ", 2);
      num_len = size_t_into_string (buf, ((svn_string_t *) key)->len);
      printf ("N buf: %s\nlen is %d\n", buf, ((svn_string_t *) key)->len);
      printf ("N num_len is %d\n", num_len);
      guaranteed_ap_write (destfile, buf, num_len);
      guaranteed_ap_write (destfile, "\n", 1);
      guaranteed_ap_write (destfile, 
                           ((svn_string_t *) key)->data, 
                           ((svn_string_t *) key)->len);
      guaranteed_ap_write (destfile, "\n", 1);

      /* Output the value's length, then the value itself. */
      guaranteed_ap_write (destfile, "V ", 2);
      num_len = size_t_into_string (buf, ((svn_string_t *) val)->len);
      printf ("V buf: %s\nlen is %d\n", buf, ((svn_string_t *) val)->len);
      printf ("V num_len is %d\n", num_len);
      guaranteed_ap_write (destfile, buf, num_len);
      guaranteed_ap_write (destfile, "\n", 1);
      guaranteed_ap_write (destfile,
                           ((svn_string_t *) val)->data,
                           ((svn_string_t *) val)->len);
      guaranteed_ap_write (destfile, "\n", 1);
    }

  res = ap_close (destfile);
  if (res != APR_SUCCESS)
    {
      svn_string_t *msg = svn_string_create 
        ("svn_parse(): warning: can't close file ", pool);
      svn_string_appendstr (msg, destfile_name, pool);
      
      /* Not fatal, just annoying */
      svn_handle_error (svn_create_error (res, SVN_NON_FATAL, msg, pool));
    }
  
  ap_destroy_pool (pool);
}


#if 0
svn_proplist_t *
svn_wc_proplist_read (svn_string_t *propfile, apr_pool_t *pool)
{
  /* kff todo: fooo in progress
}
#endif /* 0 */



#ifdef SVN_TEST

void
main ()
{
  ap_pool_t *pool = NULL;
  svn_proplist_t *proplist;

  /* Our longest piece of test data. */
  char *review =
    "A forthright entrance, yet coquettish on the tongue, its deceptively\n"
    "fruity exterior hides the warm mahagony undercurrent that is the\n"
    "hallmark of Chateau Fraisant-Pitre.  Connoisseurs of the region will\n"
    "be pleased to note the familiar, subtle hints of mulberries and\n"
    "carburator fluid.  A confident finish, marred only by a barely\n"
    "detectable suggestion of rancid squid ink.";

  ap_initialize ();
  ap_create_pool (&pool, NULL);

  proplist = ap_make_hash (pool);
  
  /* Fill it in with test data. */

  ap_hash_set (proplist,
               svn_string_create ("color", pool),
               sizeof (svn_string_t *),
               svn_string_create ("red", pool));
  
  ap_hash_set (proplist,
               svn_string_create ("wine review", pool),
               sizeof (svn_string_t *),
               svn_string_create (review, pool));
  
  ap_hash_set (proplist,
               svn_string_create ("price", pool),
               sizeof (svn_string_t *),
               svn_string_create ("US $6.50", pool));

  svn_wc_proplist_write (proplist, 
                         svn_string_create ("propdump.out", pool));

  ap_destroy_pool (pool);
}

#endif /* SVN_TEST */




/* -----------------------------------------------------------------
 * local variables:
 * eval: (load-file "svn-dev.el")
 * end:
 */

