/*
 * properties.c:  stuff related to Subversion properties
 *
 * ====================================================================
 * Copyright (c) 2000-2006 CollabNet.  All rights reserved.
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
#include <apr_hash.h>
#include <apr_tables.h>
#include <string.h>       /* for strncmp() */
#include <assert.h>
#include "svn_string.h"
#include "svn_props.h"
#include "svn_error.h"


svn_boolean_t
svn_prop_is_svn_prop(const char *prop_name)
{
  return strncmp(prop_name, SVN_PROP_PREFIX, (sizeof(SVN_PROP_PREFIX) - 1)) 
         ? FALSE 
         : TRUE;
}


svn_prop_kind_t
svn_property_kind(int *prefix_len,
                  const char *prop_name)
{
  apr_size_t wc_prefix_len = sizeof(SVN_PROP_WC_PREFIX) - 1;
  apr_size_t entry_prefix_len = sizeof(SVN_PROP_ENTRY_PREFIX) - 1;

  if (strncmp(prop_name, SVN_PROP_WC_PREFIX, wc_prefix_len) == 0)
    {
      if (prefix_len)
        *prefix_len = wc_prefix_len;
      return svn_prop_wc_kind;     
    }

  if (strncmp(prop_name, SVN_PROP_ENTRY_PREFIX, entry_prefix_len) == 0)
    {
      if (prefix_len)
        *prefix_len = entry_prefix_len;
      return svn_prop_entry_kind;     
    }

  /* else... */
  if (prefix_len)
    *prefix_len = 0;
  return svn_prop_regular_kind;
}


svn_error_t *
svn_categorize_props(const apr_array_header_t *proplist,
                     apr_array_header_t **entry_props,
                     apr_array_header_t **wc_props,
                     apr_array_header_t **regular_props,
                     apr_pool_t *pool)
{
  int i;
  if (entry_props)
    *entry_props = apr_array_make(pool, 1, sizeof(svn_prop_t));
  if (wc_props)
    *wc_props = apr_array_make(pool, 1, sizeof(svn_prop_t));
  if (regular_props)
    *regular_props = apr_array_make(pool, 1, sizeof(svn_prop_t));

  for (i = 0; i < proplist->nelts; i++)
    {
      svn_prop_t *prop, *newprop;
      enum svn_prop_kind kind;
      
      prop = &APR_ARRAY_IDX(proplist, i, svn_prop_t);      
      kind = svn_property_kind(NULL, prop->name);
      newprop = NULL;

      if (kind == svn_prop_regular_kind)
        {
          if (regular_props)
            newprop = apr_array_push(*regular_props);
        }
      else if (kind == svn_prop_wc_kind)
        {
          if (wc_props)
            newprop = apr_array_push(*wc_props);
        }
      else if (kind == svn_prop_entry_kind)
        {
          if (entry_props)
            newprop = apr_array_push(*entry_props);
        }
      else
        /* Technically this can't happen, but might as well have the
           code ready in case that ever changes. */
        return svn_error_createf(SVN_ERR_BAD_PROP_KIND, NULL,
                                 "Bad property kind for property '%s'",
                                 prop->name);

      if (newprop)
        {
          newprop->name = prop->name;
          newprop->value = prop->value;
        }
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_prop_diffs(apr_array_header_t **propdiffs,
               apr_hash_t *target_props,
               apr_hash_t *source_props,
               apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  apr_array_header_t *ary = apr_array_make(pool, 1, sizeof(svn_prop_t));

  /* Note: we will be storing the pointers to the keys (from the hashes)
     into the propdiffs array.  It is acceptable for us to
     reference the same memory as the base/target_props hash. */

  /* Loop over SOURCE_PROPS and examine each key.  This will allow us to
     detect any `deletion' events or `set-modification' events.  */
  for (hi = apr_hash_first(pool, source_props); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      apr_ssize_t klen;
      void *val;
      const svn_string_t *propval1, *propval2;

      /* Get next property */
      apr_hash_this(hi, &key, &klen, &val);
      propval1 = val;

      /* Does property name exist in TARGET_PROPS? */
      propval2 = apr_hash_get(target_props, key, klen);

      if (propval2 == NULL)
        {
          /* Add a delete event to the array */
          svn_prop_t *p = apr_array_push(ary);
          p->name = key;
          p->value = NULL;
        }
      else if (! svn_string_compare(propval1, propval2))
        {
          /* Add a set (modification) event to the array */
          svn_prop_t *p = apr_array_push(ary);
          p->name = key;
          p->value = svn_string_dup(propval2, pool);
        }
    }

  /* Loop over TARGET_PROPS and examine each key.  This allows us to
     detect `set-creation' events */
  for (hi = apr_hash_first(pool, target_props); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      apr_ssize_t klen;
      void *val;
      const svn_string_t *propval;

      /* Get next property */
      apr_hash_this(hi, &key, &klen, &val);
      propval = val;

      /* Does property name exist in SOURCE_PROPS? */
      if (NULL == apr_hash_get(source_props, key, klen))
        {
          /* Add a set (creation) event to the array */
          svn_prop_t *p = apr_array_push(ary);
          p->name = key;
          p->value = svn_string_dup(propval, pool);
        }
    }

  /* Done building our array of user events. */
  *propdiffs = ary;

  return SVN_NO_ERROR;
}


svn_boolean_t
svn_prop_needs_translation(const char *propname)
{
  /* ### Someday, we may want to be picky and choosy about which
     properties require UTF8 and EOL conversion.  For now, all "svn:"
     props need it.  */

  return svn_prop_is_svn_prop(propname);
}
