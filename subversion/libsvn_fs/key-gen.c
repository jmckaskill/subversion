/* key-gen.c --- manufacturing sequential keys for some db tables
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

#include "apr.h"
#include "key-gen.h"


/* Converting text to numbers.  */

apr_size_t
svn_fs__getsize (const char *data, apr_size_t len,
                 const char **endptr,
                 apr_size_t max)
{
  /* We can't detect overflow by simply comparing value against max,
     since multiplying value by ten can overflow in strange ways if
     max is close to the limits of apr_size_t.  For example, suppose
     that max is 54, and apr_size_t is six bits long; its range is
     0..63.  If we're parsing the number "502", then value will be 50
     after parsing the first two digits.  50 * 10 = 500.  But 500
     doesn't fit in an apr_size_t, so it'll be truncated to 500 mod 64
     = 52, which is less than max, so we'd fail to recognize the
     overflow.  Furthermore, it *is* greater than 50, so you can't
     detect overflow by checking whether value actually increased
     after each multiplication --- sometimes it does increase, but
     it's still wrong.

     So we do the check for overflow before we multiply value and add
     in the new digit.  */
  int max_prefix = max / 10;
  int max_digit = max % 10;
  int i;
  apr_size_t value = 0;

  for (i = 0; i < len && '0' <= data[i] && data[i] <= '9'; i++)
    {
      int digit = data[i] - '0';

      /* Check for overflow.  */
      if (value > max_prefix
          || (value == max_prefix && digit > max_digit))
        {
          *endptr = 0;
          return 0;
        }

      value = (value * 10) + digit;
    }

  /* There must be at least one digit there.  */
  if (i == 0)
    {
      *endptr = 0;
      return 0;
    }
  else
    {
      *endptr = data + i;
      return value;
    }
}



/* Converting numbers to text.  */

int
svn_fs__putsize (char *data, apr_size_t len, apr_size_t value)
{
  int i = 0;

  /* Generate the digits, least-significant first.  */
  do 
    {
      if (i >= len)
        return 0;

      data[i] = (value % 10) + '0';
      value /= 10;
      i++;
    }
  while (value > 0);

  /* Put the digits in most-significant-first order.  */
  {
    int left, right;

    for (left = 0, right = i-1; left < right; left++, right--)
      {
        char t = data[left];
        data[left] = data[right];
        data[right] = t;
      }
  }

  return i;
}



/*** Keys for reps and strings. ***/

static const char next_key_key[] = "next-key";


void
svn_fs__next_key (const char *this, apr_size_t *len, char *next)
{
  apr_size_t olen = *len;     /* remember the first length */
  int i = olen - 1;           /* initial index; we work backwards */
  char c;                     /* current char */
  int carry = 1;              /* boolean: do we have a carry or not?
                                 We start with a carry, because we're
                                 incrementing the number, after all. */
  
  /* Leading zeros are not allowed, except for the string "0". */
  if ((*len > 1) && (this[0] == '0'))
    {
      *len = 0;
      return;
    }
  
  for (i = (olen - 1); i >= 0; i--)
    {
      c = this[i];

      /* Validate as we go. */
      if (! (((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'z'))))
        {
          *len = 0;
          return;
        }

      if (carry)
        {
          if (c == 'z')
            next[i] = '0';
          else
            {
              carry = 0;
              
              if (c == '9')
                next[i] = 'a';
              else
                next[i] = c + 1;
            }
        }
      else
        next[i] = c;
    }

  /* Do all possible null terminations in advance... */
  next[olen] = '\0';
  next[olen + 1] = '\0';

  /* ... then handle any leftover carry. */
  if (carry)
    {
      for (i = (olen - 1); i > 0; i--)
        next[i + 1] = next[i];
      next[0] = '1';
      *len = olen + 1;
    }
}




/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
