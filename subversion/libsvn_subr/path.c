/*
 * paths.c:   a path manipulation library using svn_string_t
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



#include <string.h>
#include <assert.h>
#include "svn_string.h"
#include "svn_path.h"



static char
get_separator_from_style (enum svn_path_style style)
{
  switch (style)
    {
    case svn_path_local_style:
      /* local style - path separators used by local filesystem */
      return (SVN_PATH_LOCAL_SEPARATOR);

    case svn_path_url_style:
      /* url style - path separators used in urls */
      return (SVN_PATH_URL_SEPARATOR);

    default:
    case svn_path_repos_style:
      /* repos style - separators used in repository paths */
      return (SVN_PATH_REPOS_SEPARATOR);
    }
  /* default case = repos style (we should never hit this...) */
  return (SVN_PATH_REPOS_SEPARATOR);
}
 


void
svn_path_canonicalize (svn_string_t *path, enum svn_path_style style)
{
  char dirsep = get_separator_from_style (style);

  /* At some point this could eliminiate redundant components.
     For now, it just makes sure there is no trailing slash. */

  /* kff todo: maybe should be implemented with a new routine in
     libsvn_string. */

  while ((path->len > 0)
         && (path->data[(path->len - 1)] == dirsep))
    {
      path->data[(path->len - 1)] = '\0';
      path->len--;
    }
}


static void
add_component_internal (svn_string_t *path,
                        const char *component,
                        size_t len,
                        enum svn_path_style style)
{
  char dirsep = get_separator_from_style (style);

  if (! svn_string_isempty (path))
    svn_string_appendbytes (path, &dirsep, sizeof (dirsep));

  svn_string_appendbytes (path, component, len);
  svn_path_canonicalize (path, style);
}


void
svn_path_add_component_nts (svn_string_t *path, 
                            const char *component,
                            enum svn_path_style style)
{
  add_component_internal (path, component, strlen (component), style);
}


void
svn_path_add_component (svn_string_t *path, 
                        const svn_string_t *component,
                        enum svn_path_style style)
{
  add_component_internal (path, component->data, component->len, style);
}


void
svn_path_remove_component (svn_string_t *path, enum svn_path_style style)
{
  char dirsep = get_separator_from_style (style);

  svn_path_canonicalize (path, style);

  if (! svn_string_chop_back_to_char (path, dirsep))
    svn_string_setempty (path);
}


svn_string_t *
svn_path_last_component (const svn_string_t *path,
                         enum svn_path_style style,
                         apr_pool_t *pool)
{
  char dirsep = get_separator_from_style (style);

  apr_size_t i
    = svn_string_find_char_backward (path, dirsep);

  if (i < path->len)
    {
      i += 1;  /* Get past the separator char. */
      return svn_string_ncreate (path->data + i, (path->len - i), pool);
    }
  else
    return svn_string_dup (path, pool);
}



void
svn_path_split (const svn_string_t *path, 
                svn_string_t **dirpath,
                svn_string_t **basename,
                enum svn_path_style style,
                apr_pool_t *pool)
{
  svn_string_t *n_dirpath, *n_basename;

  assert (dirpath != basename);

  if (dirpath)
    {
      n_dirpath = svn_string_dup (path, pool);
      svn_path_remove_component (n_dirpath, style);
    }

  if (basename)
    n_basename = svn_path_last_component (path, style, pool);

  if (dirpath)
    *dirpath = n_dirpath;

  if (basename)
    *basename = n_basename;
}


int
svn_path_is_empty (const svn_string_t *path, enum svn_path_style style)
{
  char dirsep = get_separator_from_style (style);

  char buf[3];
  buf[0] = '.';
  buf[0] = dirsep;
  buf[0] = '\0';

  return ((path == NULL)
          || (svn_string_isempty (path))
          || (strcmp (path->data, buf) == 0));
}


int
svn_path_compare_paths (const svn_string_t *path1,
                        const svn_string_t *path2,
                        enum svn_path_style style)
{
  size_t min_len = ((path1->len) < (path2->len)) ? path1->len : path2->len;
  size_t i;
  char dirsep = get_separator_from_style (style);
  
  /* Skip past common prefix. */
  for (i = 0; (i < min_len) && (path1->data[i] == path2->data[i]); i++)
    ;

  if ((path1->len == path2->len) && (i >= min_len))
    return 0;     /* the paths are the same */
  else if (path1->data[i] == dirsep)
    return 1;     /* path1 child of path2, parent always comes before child */
  else if (path2->data[i] == dirsep)
    return -1;    /* path2 child of path1, parent always comes before child */
  else
    return strncmp (path1->data + i, path2->data + i, (min_len - i));
}



svn_string_t *
svn_path_get_longest_ancestor (const svn_string_t *path1,
                               const svn_string_t *path2,
                               apr_pool_t *pool)
{
  svn_string_t *common_path;
  int i = 0;

  if ((! path1) || (! path2)
      || (svn_string_isempty (path1)) || (svn_string_isempty (path2)))
    return NULL;
  
  while (path1->data[i] == path2->data[i])
    {
      i++;
      if ((i > path1->len) || (i > path2->len))
        break;
    }

  /* i is now the offset of the first _non_-matching byte. */
  common_path = svn_string_ncreate (path1->data, i, pool);  

  svn_path_canonicalize (common_path, svn_path_local_style);

  return common_path;
}





/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
