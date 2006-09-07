/*
 * sorts.c:   all sorts of sorts
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
#include <stdlib.h>       /* for qsort()   */
#include <assert.h>
#include "svn_path.h"
#include "svn_sorts.h"
#include "svn_error.h"



/*** svn_sort__hash() ***/

/* (Should this be a permanent part of APR?)

   OK, folks, here's what's going on.  APR hash tables hash on
   key/klen objects, and store associated generic values.  They work
   great, but they have no ordering.

   The point of this exercise is to somehow arrange a hash's keys into
   an "ordered list" of some kind -- in this case, a nicely sorted
   one.

   We're using APR arrays, therefore, because that's what they are:
   ordered lists.  However, what "keys" should we put in the array?
   Clearly, (const char *) objects aren't general enough.  Or rather,
   they're not as general as APR's hash implementation, which stores
   (void *)/length as keys.  We don't want to lose this information.

   Therefore, it makes sense to store pointers to {void *, size_t}
   structures in our array.  No such apr object exists... BUT... if we
   can use a new type svn_sort__item_t which contains {char *, size_t, void
   *}.  If store these objects in our array, we get the hash value
   *for free*.  When looping over the final array, we don't need to
   call apr_hash_get().  Major bonus!
 */


int
svn_sort_compare_items_as_paths(const svn_sort__item_t *a,
                                const svn_sort__item_t *b)
{
  const char *astr, *bstr;

  astr = a->key;
  bstr = b->key;
  assert(astr[a->klen] == '\0');
  assert(bstr[b->klen] == '\0');
  return svn_path_compare_paths(astr, bstr);
}


int
svn_sort_compare_items_lexically(const svn_sort__item_t *a,
                                 const svn_sort__item_t *b)
{
  int val;
  apr_size_t len;

  /* Compare bytes of a's key and b's key up to the common length. */
  len = (a->klen < b->klen) ? a->klen : b->klen;
  val = memcmp(a->key, b->key, len);
  if (val != 0)
    return val;

  /* They match up until one of them ends; whichever is longer is greater. */
  return (a->klen < b->klen) ? -1 : (a->klen > b->klen) ? 1 : 0;
}


int
svn_sort_compare_revisions(const void *a, const void *b)
{
  svn_revnum_t a_rev = *(const svn_revnum_t *)a;
  svn_revnum_t b_rev = *(const svn_revnum_t *)b;

  if (a_rev == b_rev)
    return 0;

  return a_rev < b_rev ? 1 : -1;
}


int 
svn_sort_compare_paths(const void *a, const void *b)
{
  const char *item1 = *((const char * const *) a);
  const char *item2 = *((const char * const *) b);

  return svn_path_compare_paths(item1, item2);
}



apr_array_header_t *
svn_sort__hash(apr_hash_t *ht,
               int (*comparison_func)(const svn_sort__item_t *,
                                      const svn_sort__item_t *),
               apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  apr_array_header_t *ary;

  /* allocate an array with enough elements to hold all the keys. */
  ary = apr_array_make(pool, apr_hash_count(ht), sizeof(svn_sort__item_t));

  /* loop over hash table and push all keys into the array */
  for (hi = apr_hash_first(pool, ht); hi; hi = apr_hash_next(hi))
    {
      svn_sort__item_t *item = apr_array_push(ary);

      apr_hash_this(hi, &item->key, &item->klen, &item->value);
    }
  
  /* now quicksort the array.  */
  qsort(ary->elts, ary->nelts, ary->elt_size,
        (int (*)(const void *, const void *))comparison_func);

  return ary;
}
