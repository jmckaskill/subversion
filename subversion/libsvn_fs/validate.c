/* validate.c : internal structure validators
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

#include "validate.h"



/* Validating node and node revision IDs. */

int
svn_fs__count_id_components (const char *data, apr_size_t data_len)
{
  int i;
  int id_len = 1;
  int last_start = 0;

  for (i = 0; i < data_len; i++)
    if (data[i] == '.')
      {
        /* There must be at least one digit before and after each dot.  */
        if (i == last_start)
          return 0;
        last_start = i + 1;
        id_len++;
      }
    else if ('0' <= data[i] && data[i] <= '9')
      ;
    else
      return 0;

  /* Make sure there was at least one digit in the last number.  */
  if (i == last_start)
    return 0;

  return id_len;
}



/* Validating SKELs. */

int
svn_fs__is_valid_proplist (skel_t *skel)
{
  int len = svn_fs__list_length (skel);

  if (len >= 0
      && (len & 1) == 0)
    {
      skel_t *elt;

      for (elt = skel->children; elt; elt = elt->next)
        if (! elt->is_atom)
          return 0;

      return 1;
    }

  return 0;
}



/* Validating paths. */

int 
svn_fs__is_single_path_component (const char *name)
{
  const char *c = name;
  const char *eos = name + strlen (name);

  /* Can't be empty */
  if (c == eos) return 0;

  /* Can't be `.' */
  if (! strcmp (".", name)) return 0;

  /* Can't be `..' */
  if (! strcmp ("..", name)) return 0;

  /* Run the string, looking for icky stuff. */
  while (c < eos)
    {
      /* Can't have any forward slashes `/' */
      if (*c == '/') return 0;

      c++;
    }
  return 1;
}




/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
