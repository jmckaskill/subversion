/*
 * target.c:  functions which operate on a list of targets supplied to 
 *              a subversion subcommand.
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

/* ==================================================================== */



/*** Includes. ***/

#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "apr_file_info.h"


/*** Code. ***/

svn_error_t *
svn_path_get_absolute(svn_string_t **pabsolute,
                      const svn_string_t *relative,
                      apr_pool_t *pool)
{
#ifdef WIN32
  char buffer[_MAX_PATH];
  if (_fullpath(buffer, relative->data, _MAX_PATH) != NULL)
    {
      *pabsolute = svn_string_create(buffer, pool);
    }
  else 
    {
      /* TODO: (kevin) Create better error messages, once I learn about
         the errors returned from _fullpath() */
      return svn_error_createf(APR_SUCCESS, SVN_ERR_BAD_FILENAME,
                               NULL, pool, "Could not determine absolute "
                               "path of %s", relative->data);
    }
#else
  char buffer[PATH_MAX];
  if (realpath(relative->data, buffer) != NULL)
    {
      *pabsolute = svn_string_create(buffer, pool);
    }
  else 
    {
      switch (errno)
        {
        case EACCES:
            return svn_error_createf(APR_SUCCESS, SVN_ERR_NOT_AUTHORIZED,
                                     NULL, pool, "Could not get absolute path "
                                     "for %s, because you lack permissions",
                                     relative->data);
            break;
        case EINVAL: /* FALLTHRU */
        case EIO: /* FALLTHRU */
        case ELOOP: /* FALLTHRU */
        case ENAMETOOLONG: /* FALLTHRU */
        case ENOENT: /* FALLTHRU */
        case ENOTDIR:
            return svn_error_createf(APR_SUCCESS, SVN_ERR_BAD_FILENAME,
                                     NULL, pool, "Could not get absolute path "
                                     "for %s, because it is not a valid file "
                                     "name.", relative->data);
        default:
            return svn_error_createf(APR_SUCCESS, SVN_ERR_BAD_FILENAME,
                                     NULL, pool, "Could not determine if %s "
                                     "is a file or directory.", relative->data);
            break;
        }
    }
#endif
  return SVN_NO_ERROR;
}

svn_error_t *
svn_path_split_if_file(svn_string_t *path,
                       svn_string_t **pdirectory,
                       svn_string_t **pfile,
                       apr_pool_t * pool)
{
  apr_finfo_t finfo;
  apr_status_t apr_err = apr_stat(&finfo, path->data, APR_FINFO_TYPE, pool);
  if (apr_err != APR_SUCCESS)
    {
      return svn_error_createf(apr_err, SVN_ERR_BAD_FILENAME, NULL,
                              pool, "Couldn't determine if %s was a file or "
                              "directory.", path->data);
    }
  else
    {
      if (finfo.filetype == APR_DIR)
        {
          *pdirectory = path;
          *pfile = svn_string_create("", pool);
        }
      else if (finfo.filetype == APR_REG)
        {
          svn_path_split(path, pdirectory, pfile, svn_path_local_style, pool);
        }
      else 
        {
          return svn_error_createf(APR_SUCCESS, SVN_ERR_BAD_FILENAME, NULL, pool,
                                  "%s is neither a file nor a directory name.",
                                  path->data);
        }
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_path_condense_targets(svn_string_t **pbasedir,
                          apr_array_header_t ** pcondensed_targets,
                          const apr_array_header_t *targets,
                          apr_pool_t *pool)
{
  if (targets->nelts <=0)
    {
      *pbasedir = NULL;
      if (pcondensed_targets)
        *pcondensed_targets = NULL;
    }
  else
    {
      int i, j, num_condensed = targets->nelts;
      svn_string_t *file;
      svn_boolean_t *removed = apr_pcalloc(pool,
                                           targets->nelts*sizeof(svn_boolean_t));

      /* Copy the targets array, but with absolute paths instead of relative.
         Also, find the pbasedir argument by finding what is common in all
         of the absolute paths. NOTE: This is not as efficient as it could be
         The calculation of the the basedir could be done in the loop below, which would
         save some calls to svn_path_get_longest_ancestor.  I decided to do it this way
         because I thought it would simpler, since this way, we don't even do the loop
         if we don't need to condense the targets. */
      apr_array_header_t *abs_targets = apr_array_make(pool,
                                                       targets->nelts,
                                                       sizeof(svn_string_t*));
      SVN_ERR(svn_path_get_absolute(pbasedir, ((svn_string_t **)targets->elts)[0], pool));
      (*((svn_string_t**)apr_array_push(abs_targets))) = *pbasedir;
      for(i = 1; i < targets->nelts; ++i)
        {
          svn_string_t *rel = ((svn_string_t **)targets->elts)[i];
          svn_string_t *absolute;
          SVN_ERR(svn_path_get_absolute(&absolute, rel, pool));
          (*((svn_string_t **)apr_array_push(abs_targets))) = absolute;
          *pbasedir = svn_path_get_longest_ancestor(*pbasedir, absolute, pool);
        }

      /* If we need to find the targets, find the common part of each pair
         of targets.  If common part is equal to one of the paths, the other
         is a child of it, and can be removed. */
      if (pcondensed_targets != NULL)
        {
          for (i = 0; i < abs_targets->nelts - 1; ++i)
            {
              if (!removed[i])
                {
                  for (j = i + 1; j < abs_targets->nelts; ++j)
                    {
                      if (!removed[i] && !removed[j])
                        {
                          svn_string_t *abs_targets_i = ((svn_string_t **)
                                                         abs_targets->elts)[i];
                          svn_string_t *abs_targets_j = ((svn_string_t **)
                                                         abs_targets->elts)[j];
                          svn_string_t *ancestor
                            = svn_path_get_longest_ancestor(abs_targets_i,
                                                            abs_targets_j,
                                                            pool);
                          if (ancestor != NULL)
                            {
                              if (svn_string_compare(ancestor, abs_targets_i))
                                {
                                  removed[j] = TRUE;
                                  num_condensed--;
                                }
                              else if (svn_string_compare(ancestor,abs_targets_j))
                                {
                                  removed[i] = TRUE;
                                  num_condensed--;
                                }
                            }
                        }
                    }
                }
            }

          /* Now create the return array, and copy the non-removed items */
          *pcondensed_targets = apr_array_make(pool, num_condensed,
                                               sizeof(svn_string_t*));
          for (i = 0; i < abs_targets->nelts; ++i)
            {
              if (!removed[i])
                {
                  char * rel_item = ((svn_string_t**)abs_targets->elts)[i]->data;
                  rel_item += (*pbasedir)->len + 1;
                  (*((svn_string_t**)apr_array_push(*pcondensed_targets)))
                    = svn_string_create(rel_item, pool);
                }
            }
        }

      /* Finally check if pbasedir is a dir or a file. */
      SVN_ERR(svn_path_split_if_file(*pbasedir, pbasedir, &file, pool));
      if (pcondensed_targets != NULL)
        {
          /* If we have only one element, then it is currently the empty string.
             Set it to the file if we found one, or the empty string, if not. */
          if (num_condensed == 1)
            ((svn_string_t **)(*pcondensed_targets)->elts)[0] = file;
        }
    }
  return SVN_NO_ERROR;
}


/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: */
