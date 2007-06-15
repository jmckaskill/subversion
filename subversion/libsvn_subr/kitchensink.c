/*
 * kitchensink.c :  When no place else seems to fit...
 *
 * ====================================================================
 * Copyright (c) 2006 CollabNet.  All rights reserved.
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

#include <apr_pools.h>
#include <apr_uuid.h>

#include "svn_types.h"


const char *
svn_uuid_generate(apr_pool_t *pool)
{
  apr_uuid_t uuid;
  char *uuid_str = apr_pcalloc(pool, APR_UUID_FORMATTED_LENGTH + 1);
  apr_uuid_get(&uuid);
  apr_uuid_format(uuid_str, &uuid);
  return uuid_str;
}

svn_accept_t
svn_accept_from_word(const char *word)
{
  if (strcmp(word, "left") == 0)
    return svn_accept_left;
  if (strcmp(word, "working") == 0)
    return svn_accept_working;
  if (strcmp(word, "right") == 0)
    return svn_accept_right;
  /* Return svn_accept_invalid which means that the passed string is not
   * a recognized accept option. */
  return svn_accept_invalid;
}

const char *
svn_depth_to_word(svn_depth_t depth)
{
  switch (depth)
    {
    case svn_depth_exclude:
      return "exclude";
    case svn_depth_unknown:
      return "unknown";
    case svn_depth_empty:
      return "empty";
    case svn_depth_files:
      return "files";
    case svn_depth_immediates:
      return "immediates";
    case svn_depth_infinity:
      return "infinity";
    default:
      return "INVALID-DEPTH";
    }
}


svn_depth_t
svn_depth_from_word(const char *word)
{
  if (strcmp(word, "exclude") == 0)
    return svn_depth_exclude;
  if (strcmp(word, "unknown") == 0)
    return svn_depth_unknown;
  if (strcmp(word, "empty") == 0)
    return svn_depth_empty;
  if (strcmp(word, "files") == 0)
    return svn_depth_files;
  if (strcmp(word, "immediates") == 0)
    return svn_depth_immediates;
  if (strcmp(word, "infinity") == 0)
    return svn_depth_infinity;
  /* There's no special value for invalid depth, and no convincing
     reason to make one yet, so just fall back to unknown depth. */
  return svn_depth_unknown;
}

const char *
svn_inheritance_to_word(svn_mergeinfo_inheritance_t inherit)
{
  switch (inherit)
    {
    case svn_mergeinfo_inherited:
      return "inherited";
    case svn_mergeinfo_nearest_ancestor:
      return "nearest-ancestor";
    default:
      return "explicit";
    }
}


svn_mergeinfo_inheritance_t
svn_inheritance_from_word(const char *word)
{
  if (strcmp(word, "inherited") == 0)
    return svn_mergeinfo_inherited;
  if (strcmp(word, "nearest-ancestor") == 0)
    return svn_mergeinfo_nearest_ancestor;
  return svn_mergeinfo_explicit;
}