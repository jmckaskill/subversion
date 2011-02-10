/*
 * lcs.c :  routines for creating an lcs
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */


#include <apr.h>
#include <apr_pools.h>
#include <apr_general.h>

#include "diff.h"


/*
 * Calculate the Longest Common Subsequence (LCS) between two datasources.
 * This function is what makes the diff code tick.
 *
 * The LCS algorithm implemented here is described by Sun Wu,
 * Udi Manber and Gene Meyers in "An O(NP) Sequence Comparison Algorithm"
 *
 */

typedef struct svn_diff__snake_t svn_diff__snake_t;

struct svn_diff__snake_t
{
    apr_off_t             y;
    svn_diff__lcs_t      *lcs;
    svn_diff__position_t *position[2];
};

static APR_INLINE void
svn_diff__snake(apr_off_t k,
                svn_diff__snake_t *fp,
                int idx,
                svn_diff__lcs_t **freelist,
                apr_pool_t *pool)
{
  svn_diff__position_t *start_position[2];
  svn_diff__position_t *position[2];
  svn_diff__lcs_t *lcs;
  svn_diff__lcs_t *previous_lcs;

  /* The previous entry at fp[k] is going to be replaced.  See if we
   * can mark that lcs node for reuse, because the sequence up to this
   * point was a dead end.
   */
  lcs = fp[k].lcs;
  while (lcs)
    {
      lcs->refcount--;
      if (lcs->refcount)
        break;

      previous_lcs = lcs->next;
      lcs->next = *freelist;
      *freelist = lcs;
      lcs = previous_lcs;
    }

  if (fp[k - 1].y + 1 > fp[k + 1].y)
    {
      start_position[0] = fp[k - 1].position[0];
      start_position[1] = fp[k - 1].position[1]->next;

      previous_lcs = fp[k - 1].lcs;
    }
  else
    {
      start_position[0] = fp[k + 1].position[0]->next;
      start_position[1] = fp[k + 1].position[1];

      previous_lcs = fp[k + 1].lcs;
    }


  /* ### Optimization, skip all positions that don't have matchpoints
   * ### anyway. Beware of the sentinel, don't skip it!
   */

  position[0] = start_position[0];
  position[1] = start_position[1];

  while (position[0]->node == position[1]->node)
    {
      position[0] = position[0]->next;
      position[1] = position[1]->next;
    }

  if (position[1] != start_position[1])
    {
      lcs = *freelist;
      if (lcs)
        {
          *freelist = lcs->next;
        }
      else
        {
          lcs = apr_palloc(pool, sizeof(*lcs));
        }

      lcs->position[idx] = start_position[0];
      lcs->position[abs(1 - idx)] = start_position[1];
      lcs->length = position[1]->offset - start_position[1]->offset;
      lcs->next = previous_lcs;
      lcs->refcount = 1;
      fp[k].lcs = lcs;
    }
  else
    {
      fp[k].lcs = previous_lcs;
    }

  if (previous_lcs)
    {
      previous_lcs->refcount++;
    }

  fp[k].position[0] = position[0];
  fp[k].position[1] = position[1];

  fp[k].y = position[1]->offset;
}


static svn_diff__lcs_t *
svn_diff__lcs_reverse(svn_diff__lcs_t *lcs)
{
  svn_diff__lcs_t *next;
  svn_diff__lcs_t *prev;

  next = NULL;
  while (lcs != NULL)
    {
      prev = lcs->next;
      lcs->next = next;
      next = lcs;
      lcs = prev;
    }

  return next;
}


/* Prepends a new lcs chunk for the amount of PREFIX_LINES to the given LCS
 * chain, and returns it. This function assumes PREFIX_LINES > 0. */
static svn_diff__lcs_t *
prepend_prefix_lcs(svn_diff__lcs_t *lcs,
                   apr_off_t prefix_lines,
                   apr_pool_t *pool)
{
  svn_diff__lcs_t *prefix_lcs;

  SVN_ERR_ASSERT_NO_RETURN(prefix_lines > 0);

  prefix_lcs = apr_palloc(pool, sizeof(*prefix_lcs));
  prefix_lcs->position[0] = apr_pcalloc(pool, 
                                        sizeof(*prefix_lcs->position[0]));
  prefix_lcs->position[0]->offset = 1;
  prefix_lcs->position[1] = apr_pcalloc(pool,
                                        sizeof(*prefix_lcs->position[1]));
  prefix_lcs->position[1]->offset = 1;
  prefix_lcs->length = prefix_lines;
  prefix_lcs->refcount = 1;
  prefix_lcs->next = lcs;

  return prefix_lcs;
}


svn_diff__lcs_t *
svn_diff__lcs(svn_diff__position_t *position_list1, /* pointer to tail (ring) */
              svn_diff__position_t *position_list2, /* pointer to tail (ring) */
              apr_off_t prefix_lines,
              apr_pool_t *pool)
{
  int idx;
  apr_off_t length[2];
  svn_diff__snake_t *fp;
  apr_off_t d;
  apr_off_t k;
  apr_off_t p = 0;
  svn_diff__lcs_t *lcs, *lcs_freelist = NULL;

  svn_diff__position_t sentinel_position[2];

  /* Since EOF is always a sync point we tack on an EOF link
   * with sentinel positions
   */
  lcs = apr_palloc(pool, sizeof(*lcs));
  lcs->position[0] = apr_pcalloc(pool, sizeof(*lcs->position[0]));
  lcs->position[0]->offset = position_list1 ? 
    position_list1->offset + 1 : prefix_lines + 1;
  lcs->position[1] = apr_pcalloc(pool, sizeof(*lcs->position[1]));
  lcs->position[1]->offset = position_list2 ?
    position_list2->offset + 1 : prefix_lines + 1;
  lcs->length = 0;
  lcs->refcount = 1;
  lcs->next = NULL;

  if (position_list1 == NULL || position_list2 == NULL)
    {
      if (prefix_lines)
        return prepend_prefix_lcs(lcs, prefix_lines, pool);
      else
        return lcs;
    }

  /* Calculate length of both sequences to be compared */
  length[0] = position_list1->offset - position_list1->next->offset + 1;
  length[1] = position_list2->offset - position_list2->next->offset + 1;
  idx = length[0] > length[1] ? 1 : 0;

  /* strikerXXX: here we allocate the furthest point array, which is
   * strikerXXX: sized M + N + 3 (!)
   */
  fp = apr_pcalloc(pool,
                   sizeof(*fp) * (apr_size_t)(length[0] + length[1] + 3));
  fp += length[idx] + 1;

  sentinel_position[idx].next = position_list1->next;
  position_list1->next = &sentinel_position[idx];
  sentinel_position[idx].offset = position_list1->offset + 1;

  sentinel_position[abs(1 - idx)].next = position_list2->next;
  position_list2->next = &sentinel_position[abs(1 - idx)];
  sentinel_position[abs(1 - idx)].offset = position_list2->offset + 1;

  /* These are never dereferenced, only compared by value, so we
   * can safely fake these up and the void* cast is OK.
   */
  sentinel_position[0].node = (void*)&sentinel_position[0];
  sentinel_position[1].node = (void*)&sentinel_position[1];

  d = length[abs(1 - idx)] - length[idx];

  /* k = -1 will be the first to be used to get previous
   * position information from, make sure it holds sane
   * data
   */
  fp[-1].position[0] = sentinel_position[0].next;
  fp[-1].position[1] = &sentinel_position[1];

  p = 0;
  do
    {
      /* Forward */
      for (k = -p; k < d; k++)
        {
          svn_diff__snake(k, fp, idx, &lcs_freelist, pool);
        }

      for (k = d + p; k >= d; k--)
        {
          svn_diff__snake(k, fp, idx, &lcs_freelist, pool);
        }

      p++;
    }
  while (fp[d].position[1] != &sentinel_position[1]);

  lcs->next = fp[d].lcs;
  lcs = svn_diff__lcs_reverse(lcs);

  position_list1->next = sentinel_position[idx].next;
  position_list2->next = sentinel_position[abs(1 - idx)].next;

  if (prefix_lines)
    return prepend_prefix_lcs(lcs, prefix_lines, pool);
  else
    return lcs;
}
