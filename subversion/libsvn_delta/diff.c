/*
 * diff.c :  routines for doing diffs
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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


#include <apr.h>
#include <apr_pools.h>
#include <apr_general.h>

#include "svn_pools.h"
#include "svn_error.h"
#include "svn_diff.h"
#include "svn_types.h"


typedef struct svn_diff__node_t svn_diff__node_t;
typedef struct svn_diff__tree_t svn_diff__tree_t;
typedef struct svn_diff__position_t svn_diff__position_t;
typedef struct svn_diff__lcs_t svn_diff__lcs_t;

typedef enum svn_diff__type_e
{
  svn_diff__type_common,
  svn_diff__type_diff_modified,
  svn_diff__type_diff_latest,
  svn_diff__type_diff_common,
  svn_diff__type_conflict
} svn_diff__type_e;

struct svn_diff_t {
  svn_diff_t *next;
  svn_diff__type_e type;
  apr_off_t original_start;
  apr_off_t original_length;
  apr_off_t modified_start;
  apr_off_t modified_length;
  apr_off_t latest_start;
  apr_off_t latest_length;
  svn_diff_t *resolved_diff;
};

struct svn_diff__node_t
{
  svn_diff__node_t     *parent;
  svn_diff__node_t     *left;
  svn_diff__node_t     *right;

  void                 *token;
};

struct svn_diff__tree_t
{
  svn_diff__node_t     *root;
  apr_pool_t           *pool;
};

struct svn_diff__position_t
{
  svn_diff__position_t *next;
  svn_diff__position_t *prev;

  svn_diff__node_t     *node;
  apr_off_t             offset;
};

struct svn_diff__lcs_t
{
  svn_diff__lcs_t      *next;
  svn_diff__position_t *position[2];
  apr_off_t             length;
};


/*
 * Support functions to build a tree of token positions
 */

static
void
svn_diff__tree_create(svn_diff__tree_t **tree, apr_pool_t *pool)
{
  *tree = apr_palloc(pool, sizeof(**tree));
  (*tree)->pool = pool;
  (*tree)->root = NULL;
}

static
svn_diff__position_t *
svn_diff__tree_insert_token(svn_diff__tree_t *tree,
                            void *diff_baton,
                            const svn_diff_fns_t *vtable,
                            void *token,
                            apr_off_t offset,
                            int idx,
                            apr_pool_t *pool)
{
  svn_diff__node_t *node;
  svn_diff__node_t **node_ref;
  svn_diff__node_t *parent;
  svn_diff__position_t *position;
  int rv;

  parent = NULL;
  node_ref = &tree->root;

  while (*node_ref != NULL)
    {
      parent = *node_ref;

      rv = vtable->token_compare(diff_baton, parent->token, token);
      if (rv == 0)
        {
          /* Discard the token */
          if (vtable->token_discard != NULL)
            vtable->token_discard(diff_baton, token);

          /* Create a new position */
          position = apr_palloc(pool, sizeof(svn_diff__position_t));
          position->next = NULL;
          position->node = parent;
          position->offset = offset;

          return position;
        }
      else if (rv > 0)
        {
          node_ref = &parent->left;
        }
      else
        {
          node_ref = &parent->right;
        }
    }

  /* Create a new node */
  node = apr_palloc(tree->pool, sizeof(*node));
  node->parent = parent;
  node->left = NULL;
  node->right = NULL;
  node->token = token;

  *node_ref = node;

  /* Create a new position */
  position = apr_palloc(pool, sizeof(svn_diff__position_t));
  position->next = NULL;
  position->node = node;
  position->offset = offset;

  return position;
}


/*
 * Support function to reverse a linked list; in this case, the LCS.
 */

static
svn_diff__lcs_t *
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


/*
 * Get all tokens from a datasource.  Return the
 * last item in the (circular) list.
 */
static
svn_error_t *
svn_diff__get_tokens(svn_diff__position_t **position_list,
                     svn_diff__tree_t *tree,
                     void *diff_baton,
                     const svn_diff_fns_t *vtable,
                     svn_diff_datasource_e datasource,
                     int position_idx,
                     apr_pool_t *pool)
{
  svn_diff__position_t sentinel_position;
  svn_diff__position_t *position = &sentinel_position;
  void *token;
  apr_off_t offset;

  *position_list = NULL;

  SVN_ERR(vtable->datasource_open(diff_baton, datasource));

  sentinel_position.next = &sentinel_position;
  sentinel_position.prev = &sentinel_position;

  offset = 0;
  while (1)
    {
      SVN_ERR(vtable->datasource_get_next_token(&token, diff_baton, datasource));
      if (token == NULL)
        break;

      offset++;
      position->next = svn_diff__tree_insert_token(tree,
                                                   diff_baton, vtable,
                                                   token, offset,
                                                   position_idx,
                                                   pool);
      position->next->prev = position;
      position = position->next;
    }

  SVN_ERR(vtable->datasource_close(diff_baton, datasource));

  if (position != &sentinel_position)
    {
      position->next = sentinel_position.next;
      position->next->prev = position;
    
      *position_list = position;
    }

  return NULL;
}

/*
 * Actual diff routines
 */

/*
 * Calculate the Longest Common Subsequence between two datasources.
 * This function is what makes the diff code tick.
 *
 * The LCS algorithm implemented here is described by Sun Wu,
 * Udi Manber and Gene Meyers in "An O(NP) Sequence Comparison Algorithm"
 *
 * It is mixed with variation 4b of "An O(ND) Difference Algorithm
 * and Its Variations" by Meyers.  It doesn't implement the linear space
 * LCS recovery algorithm, but does take advantage of scanning
 * from both the start aswell as the end of sequences being compared.
 *
 */

typedef struct svn_diff__snake_t svn_diff__snake_t;

struct svn_diff__snake_t
{
    apr_off_t             y;
    svn_diff__lcs_t      *lcs;
    svn_diff__position_t *position[2];
};

static APR_INLINE
void
svn_diff__snake_forward(apr_off_t k,
                        svn_diff__snake_t *fp,
                        int idx,
                        apr_pool_t *pool)
{
  svn_diff__position_t *start_position[2];
  svn_diff__position_t *position[2];
  svn_diff__lcs_t *lcs;
  svn_diff__lcs_t *previous_lcs;

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
      lcs = apr_palloc(pool, sizeof(*lcs));

      lcs->position[idx] = start_position[0];
      lcs->position[abs(1 - idx)] = start_position[1];
      lcs->length = position[1]->offset - start_position[1]->offset;

      lcs->next = previous_lcs;
      fp[k].lcs = lcs;
    }
  else
    {
      fp[k].lcs = previous_lcs;
    }

  fp[k].position[0] = position[0];
  fp[k].position[1] = position[1];

  fp[k].y = position[1]->offset;
}

static APR_INLINE
void
svn_diff__snake_reverse(apr_off_t k,
                        svn_diff__snake_t *fp,
                        int idx,
                        apr_pool_t *pool)
{
  svn_diff__position_t *start_position[2];
  svn_diff__position_t *position[2];
  svn_diff__lcs_t *lcs;
  svn_diff__lcs_t *previous_lcs;

  if (fp[k - 1].y < fp[k + 1].y - 1)
    {
      start_position[0] = fp[k - 1].position[0]->prev;
      start_position[1] = fp[k - 1].position[1];

      previous_lcs = fp[k - 1].lcs;
    }
  else
    {
      start_position[0] = fp[k + 1].position[0];
      start_position[1] = fp[k + 1].position[1]->prev;

      previous_lcs = fp[k + 1].lcs;
    }

  /* ### Optimization, skip all positions that don't have matchpoints
   * ### anyway. Beware of the sentinel, don't skip it!
   */

  position[0] = start_position[0];
  position[1] = start_position[1];

  while (position[0]->node == position[1]->node)
    {
      position[0] = position[0]->prev;
      position[1] = position[1]->prev;
    }

  if (position[1] != start_position[1])
    {
      lcs = apr_palloc(pool, sizeof(*lcs));

      lcs->position[idx] = position[0]->next;
      lcs->position[abs(1 - idx)] = position[1]->next;
      lcs->length = start_position[1]->offset - position[1]->offset;

      lcs->next = previous_lcs;
      fp[k].lcs = lcs;
    }
  else
    {
      fp[k].lcs = previous_lcs;
    }

  fp[k].position[0] = position[0];
  fp[k].position[1] = position[1];

  fp[k].y = position[1]->offset;
}

static APR_INLINE
svn_diff__lcs_t *
svn_diff__recover_lcs(apr_off_t k,
                      svn_diff__snake_t *fp_forward,
                      svn_diff__snake_t *fp_reverse)
{
  svn_diff__lcs_t *lcs_forward;
  svn_diff__lcs_t *lcs_reverse;

  lcs_forward = fp_forward[k].lcs;
  lcs_reverse = fp_reverse[k].lcs;

  if (lcs_forward == NULL)
    return lcs_reverse;

  if (lcs_reverse->length > 0
      && lcs_forward->position[0]->offset + lcs_forward->length
         == lcs_reverse->position[0]->offset + lcs_reverse->length
      && lcs_forward->position[1]->offset + lcs_forward->length
         == lcs_reverse->position[1]->offset + lcs_reverse->length)
    {
      /* Don't record the same common range twice, skip the reverse */
      lcs_reverse = lcs_reverse->next;
    }

  lcs_forward = svn_diff__lcs_reverse(lcs_forward);
  fp_forward[k].lcs->next = lcs_reverse;

  return lcs_forward;
}

static
svn_diff__lcs_t *
svn_diff__lcs(svn_diff__position_t *position_list1, /* pointer to tail (ring) */
              svn_diff__position_t *position_list2, /* pointer to tail (ring) */
              int idx1, int idx2,
              apr_pool_t *pool)
{
  int idx;
  apr_off_t length[2];
  svn_diff__snake_t *fp_forward;
  svn_diff__snake_t *fp_reverse;
  apr_off_t d;
  apr_off_t k;
  apr_off_t p = 0;
  svn_diff__lcs_t *lcs;
  int check_for_overlap;

  svn_diff__position_t sentinel_position[2];
  svn_diff__node_t     sentinel_node[2];

  /* Calculate length of both sequences to be compared */
  length[0] = position_list1->offset - position_list1->next->offset + 1;
  length[1] = position_list2->offset - position_list2->next->offset + 1;

  idx = length[0] > length[1] ? 1 : 0;

  fp_forward = apr_pcalloc(pool,
                           sizeof(*fp_forward)
                           * (apr_size_t)(length[0] + length[1] + 3));
  fp_forward += length[idx] + 1;

  fp_reverse = apr_palloc(pool,
                          sizeof(*fp_reverse)
                          * (apr_size_t)(length[0] + length[1] + 3));
  fp_reverse += length[idx] + 1;
  
  sentinel_position[idx].next = position_list1->next;
  sentinel_position[idx].prev = position_list1;
  sentinel_position[idx].next->prev = &sentinel_position[idx];
  sentinel_position[idx].prev->next = &sentinel_position[idx];
  
  sentinel_position[abs(1 - idx)].next = position_list2->next;
  sentinel_position[abs(1 - idx)].prev = position_list2;
  sentinel_position[abs(1 - idx)].next->prev = &sentinel_position[abs(1 - idx)];
  sentinel_position[abs(1 - idx)].prev->next = &sentinel_position[abs(1 - idx)];

  sentinel_position[0].node = &sentinel_node[0];
  sentinel_position[1].node = &sentinel_node[1];

  d = length[abs(1 - idx)] - length[idx];
  
  /* Since EOF is always a sync point we tack on an EOF link
   * with sentinel positions
   */
  lcs = apr_palloc(pool, sizeof(*lcs));
  lcs->position[0] = apr_pcalloc(pool, sizeof(*lcs->position[0]));
  lcs->position[0]->offset = position_list1->offset + 1;
  lcs->position[1] = apr_pcalloc(pool, sizeof(*lcs->position[1]));
  lcs->position[1]->offset = position_list2->offset + 1;
  lcs->length = 0;
  lcs->next = NULL;
  
  /* k = -1 will be the first to be used to get previous
   * position information from, make sure it holds sane
   * data
   */
  fp_forward[-1].position[0] = sentinel_position[0].next;
  fp_forward[-1].position[1] = &sentinel_position[1];

  for (k = 0; k <= d; k++)
    {
      fp_reverse[k].y = sentinel_position[1].offset;
      fp_reverse[k].lcs = lcs;
    }

  /* k = d + 1 will be the first to be used to get previous
   * position information from, make sure it holds sane
   * data.
   */
  fp_reverse[d + 1].position[0] = sentinel_position[0].prev;
  fp_reverse[d + 1].position[1] = &sentinel_position[1];
  fp_reverse[d + 1].lcs = lcs;

  p = 0;
  while (1)
    {
      /* Initialize the fp_reverse points now within reach */
      fp_reverse[-p - 1].y = sentinel_position[1].offset;
      fp_reverse[-p - 1].lcs = lcs;
      fp_reverse[d + p + 1].y = sentinel_position[1].offset;
      fp_reverse[d + p + 1].lcs = lcs;

      /* Forward */
      check_for_overlap = !(d & 1);
      for (k = -p; k < d; k++)
        {
          svn_diff__snake_forward(k, fp_forward, idx, pool);

          if (check_for_overlap
              && fp_forward[k].y >= fp_reverse[k].y)
            {
              return svn_diff__recover_lcs(k, fp_forward, fp_reverse);
            }

          check_for_overlap ^= 1;
        }

      check_for_overlap = 1;
      for (k = d + p; k >= d; k--)
        {
          svn_diff__snake_forward(k, fp_forward, idx, pool);
  
          if (check_for_overlap
              && fp_forward[k].y >= fp_reverse[k].y)
            {
              return svn_diff__recover_lcs(k, fp_forward, fp_reverse);
            }

          check_for_overlap ^= 1;
        }

      /* Reverse */
      check_for_overlap = 0;
      for (k = d + p; k > 0; k--)
        {
          svn_diff__snake_reverse(k, fp_reverse, idx, pool);
  
          if (check_for_overlap
              && fp_forward[k].y >= fp_reverse[k].y)
            {
              return svn_diff__recover_lcs(k, fp_forward, fp_reverse);
            }

          check_for_overlap ^= 1;
        }

      check_for_overlap = d & 1;
      for (k = -p; k <= 0; k++)
        {
          svn_diff__snake_reverse(k, fp_reverse, idx, pool);
  
          if (check_for_overlap
              && fp_forward[k].y >= fp_reverse[k].y)
            {
              return svn_diff__recover_lcs(k, fp_forward, fp_reverse);
            }

          check_for_overlap ^= 1;
        }

      p++;
    }
}

svn_error_t *
svn_diff(svn_diff_t **diff,
         void *diff_baton,
         const svn_diff_fns_t *vtable,
         apr_pool_t *pool)
{
  svn_diff__tree_t *tree;
  svn_diff__position_t *position_list[2];
  apr_pool_t *subpool;
  svn_diff__lcs_t *lcs;

  *diff = NULL;

  subpool = svn_pool_create(pool);

  svn_diff__tree_create(&tree, subpool);

  /* Insert the data into the tree */
  SVN_ERR(svn_diff__get_tokens(&position_list[0],
                               tree,
                               diff_baton, vtable,
                               svn_diff_datasource_original, 0,
                               subpool));

  SVN_ERR(svn_diff__get_tokens(&position_list[1],
                               tree,
                               diff_baton, vtable,
                               svn_diff_datasource_modified, 1,
                               subpool));

  /* The cool part is that we don't need the tokens anymore.
   * Allow the app to clean them up if it wants to.
   */
  if (vtable->token_discard_all != NULL)
    vtable->token_discard_all(diff_baton);

  /* ### We don't need the nodes in the tree either anymore.  However,
   * ### we are using the same pool for the tree and the positions,
   * ### so destroying/clearing that pool is not an option.
   */

  /* Get the lcs */
  lcs = svn_diff__lcs(position_list[0], position_list[1],
                      0, 1,
                      subpool);

  /* Produce a diff */
  {
    apr_off_t original_start;
    apr_off_t modified_start;

    svn_diff_t **diff_ref = diff;

    original_start = 1;
    modified_start = 1;
    while (1)
      {
        if (original_start < lcs->position[0]->offset
            || modified_start < lcs->position[1]->offset)
        {
            (*diff_ref) = apr_palloc(pool, sizeof(**diff_ref));

            (*diff_ref)->type = svn_diff__type_diff_modified;
            (*diff_ref)->original_start = original_start - 1;
            (*diff_ref)->original_length = lcs->position[0]->offset - original_start;
            (*diff_ref)->modified_start = modified_start - 1;
            (*diff_ref)->modified_length = lcs->position[1]->offset - modified_start;
            (*diff_ref)->latest_start = 0;
            (*diff_ref)->latest_length = 0;

            diff_ref = &(*diff_ref)->next;
        }

        /* Detect the EOF */
        if (lcs->length == 0)
            break;

        original_start = lcs->position[0]->offset;
        modified_start = lcs->position[1]->offset;

        (*diff_ref) = apr_palloc(pool, sizeof(**diff_ref));

        (*diff_ref)->type = svn_diff__type_common;
        (*diff_ref)->original_start = original_start - 1;
        (*diff_ref)->original_length = lcs->length;
        (*diff_ref)->modified_start = modified_start - 1;
        (*diff_ref)->modified_length = lcs->length;
        (*diff_ref)->latest_start = 0;
        (*diff_ref)->latest_length = 0;

        diff_ref = &(*diff_ref)->next;

        original_start += lcs->length;
        modified_start += lcs->length;

        lcs = lcs->next;
      }

    *diff_ref = NULL;
  }

  /* Get rid of all the data we don't have a use for anymore */
  svn_pool_destroy(subpool);

  return NULL;
}

svn_error_t *
svn_diff3(svn_diff_t **diff,
          void *diff_baton,
          const svn_diff_fns_t *vtable,
          apr_pool_t *pool)
{
  apr_pool_t *subpool;
  svn_diff__tree_t *tree;
  svn_diff__position_t *position_list[3];
  svn_diff__lcs_t *lcs_om;
  svn_diff__lcs_t *lcs_ol;

  *diff = NULL;

  subpool = svn_pool_create(pool);

  svn_diff__tree_create(&tree, subpool);

  SVN_ERR(svn_diff__get_tokens(&position_list[0],
                               tree,
                               diff_baton, vtable,
                               svn_diff_datasource_original, 0,
                               subpool));

  SVN_ERR(svn_diff__get_tokens(&position_list[1],
                               tree,
                               diff_baton, vtable,
                               svn_diff_datasource_modified, 1,
                               subpool));

  SVN_ERR(svn_diff__get_tokens(&position_list[2],
                               tree,
                               diff_baton, vtable,
                               svn_diff_datasource_latest, 2,
                               subpool));

  /* Get rid of the tokens, we don't need them to calc the diff */
  if (vtable->token_discard_all != NULL)
    vtable->token_discard_all(diff_baton);

  /* ### We don't need the nodes in the tree either anymore.  However,
   * ### we are using the same pool for the tree and the positions,
   * ### so destroying/clearing that pool is not an option.
   */

  /* Get the lcs for original-modified and original-latest */
  lcs_om = svn_diff__lcs(position_list[0], position_list[1],
                         0, 1,
                         subpool);
  lcs_ol = svn_diff__lcs(position_list[0], position_list[2],
                         0, 2,
                         subpool);

  /* Produce a merged diff */
  {
    svn_diff_t **diff_ref = diff;

    apr_off_t original_start = 1;
    apr_off_t modified_start = 1;
    apr_off_t latest_start = 1;
    apr_off_t original_sync;
    apr_off_t modified_sync;
    apr_off_t latest_sync;
    apr_off_t common_length;
    apr_off_t original_length;
    apr_off_t modified_length;
    apr_off_t latest_length;
    svn_boolean_t is_modified;
    svn_boolean_t is_latest;
    svn_diff__type_e type;

    /* Point the position lists to the start of the list
     * so that common_diff/conflict detection actually is
     * able to work
     */
    position_list[1] = position_list[1]->next;
    position_list[2] = position_list[2]->next;

    while (1)
      {
        /* Find the sync points */
        while (1)
          {
            if (lcs_om->position[0]->offset > lcs_ol->position[0]->offset)
              {
                original_sync = lcs_om->position[0]->offset;

                while (lcs_ol->position[0]->offset + lcs_ol->length
                       < original_sync)
                  lcs_ol = lcs_ol->next;

                if (lcs_ol->position[0]->offset <= original_sync)
                    break;
              }
            else
              {
                original_sync = lcs_ol->position[0]->offset;

                while (lcs_om->position[0]->offset + lcs_om->length
                       < original_sync)
                  lcs_om = lcs_om->next;

                if (lcs_om->position[0]->offset <= original_sync)
                    break;
              }
          }

        modified_sync = lcs_om->position[1]->offset
                      + (original_sync - lcs_om->position[0]->offset);
        latest_sync = lcs_ol->position[1]->offset
                    + (original_sync - lcs_ol->position[0]->offset);

        /* Determine what is modified, if anything */
        is_modified = lcs_om->position[0]->offset - original_start > 0
                      || lcs_om->position[1]->offset - modified_start > 0;

        is_latest = lcs_ol->position[0]->offset - original_start > 0
                    || lcs_ol->position[1]->offset - latest_start > 0;

        if (is_modified || is_latest)
          {
            svn_diff_t *resolved_diff = NULL;

            original_length = original_sync - original_start;
            modified_length = modified_sync - modified_start;
            latest_length = latest_sync - latest_start;

            if (is_modified && is_latest)
              {
#if USE_SMART_CONFLICT_DETECTION
                svn_diff__position_t *start_position[2];

                type = svn_diff__type_diff_common;

                /* First find the starting positions for the
                 * comparison
                 */
                while (position_list[1]->offset < modified_start)
                  position_list[1] = position_list[1]->next;

                while (position_list[2]->offset < latest_start)
                  position_list[2] = position_list[2]->next;

                start_position[0] = position_list[0];
                start_position[1] = position_list[1];

                common_length = modified_length < latest_length
                              ? modified_length : latest_length;

                if (modified_length != latest_length)
                  {
                    type = svn_diff__type_conflict;
                  }

                while (common_length
                       && position_list[1]->node == position_list[2]->node)
                  {
                    position_list[1] = position_list[1]->next;
                    position_list[2] = position_list[2]->next;

                    common_length--;
                  }

                if (common_length)
                  {
                    type = svn_diff__type_conflict;
                  }

                if (type == svn_diff__type_conflict)
                  {
                    /* ### If we have a conflict we can try to find the
                     * ### common parts in it by getting an lcs between
                     * ### modified (start to start + length) and
                     * ### latest (start to start + length).
                     * ### We use this lcs to create a simple diff.  Only
                     * ### where there is a diff between the two, we have
                     * ### a conflict.
                     * ### This raises a problem; several common diffs and
                     * ### conflicts can occur within the same original
                     * ### block.  This needs some thought.
                     * ###
                     * ### NB: We can use the node _pointers_ to identify
                     * ###     different tokens
                     */

                    svn_diff__position_t *position[2];
                    svn_diff__lcs_t *lcs = NULL;
                    svn_diff__lcs_t **lcs_ref = &lcs;
                    apr_pool_t *subpool2;

                    subpool2 = svn_pool_create(pool);

                    /* If there were matching symbols at the start of
                     * both sequences, record that fact.
                     */
                    if (common_length < modified_length
                        && common_length < latest_length)
                      {
                        lcs = apr_palloc(subpool2, sizeof(*lcs));
                        lcs->next = NULL;
                        lcs->position[0] = start_position[0];
                        lcs->position[1] = start_position[1];
                        lcs->length =  (modified_length < latest_length
                                        ? modified_length : latest_length)
                                    - common_length;

                        lcs_ref = &lcs->next;

                        start_position[0] = position_list[0];
                        start_position[1] = position_list[1];
                      }

                    modified_length -= common_length;
                    latest_length -= common_length;

                    if (modified_length == 0 || latest_length == 0)
                      {
                        /* The second part of the lcs is a conflict,
                         * simply tack on an EOF sentinel
                         */

                        while (modified_length--)
                          position_list[1] = position_list[1]->next;

                        while (latest_length--)
                          position_list[2] = position_list[2]->next;

                        *lcs_ref = apr_palloc(subpool2, sizeof(**lcs_ref));
                        (*lcs_ref)->next = NULL;
                        (*lcs_ref)->length = 0;
                        (*lcs_ref)->position[0] = position_list[0];
                        (*lcs_ref)->position[1] = position_list[1];
                      }
                    else
                      {
                        while (--modified_length)
                          position_list[1] = position_list[1]->next;

                        while (--latest_length)
                          position_list[2] = position_list[2]->next;

                        /* Create a new ring for svn_diff__lcs to grok.
                         * We can safely do this given we don't need the
                         * positions we processed anymore.
                         */
                        position[0] = position_list[1];
                        position[1] = position_list[2];
                        position_list[1] = position_list[1]->next;
                        position_list[2] = position_list[2]->next;
                        position[0]->next = start_position[0];
                        position[1]->next = start_position[1];

                        *lcs_ref = svn_diff__lcs(position[0], position[1],
                                                 1, 2,
                                                 subpool2);
                      }

                    /* Restore modified_length and latest_length */
                    modified_length = modified_sync - modified_start;
                    latest_length = latest_sync - latest_start;

                    if (lcs != NULL)
                      {
                        svn_diff_t **rdiff_ref = &resolved_diff;
                        apr_off_t cm_start;
                        apr_off_t cl_start;

                        cm_start = 1;
                        cl_start = 1;
                        while (1)
                          {
                            if (cm_start < lcs->position[0]->offset
                                || cl_start < lcs->position[1]->offset)
                              {
                                (*rdiff_ref) = apr_palloc(pool,
                                                          sizeof(**rdiff_ref));

                                (*rdiff_ref)->type
                                  = svn_diff__type_conflict;
                                (*rdiff_ref)->original_start = original_start;
                                (*rdiff_ref)->original_length = original_length;
                                (*rdiff_ref)->modified_start
                                  = cm_start - 1;
                                (*rdiff_ref)->modified_length
                                  = lcs->position[0]->offset
                                  - cm_start;
                                (*rdiff_ref)->latest_start
                                  = cl_start - 1;
                                (*rdiff_ref)->latest_length
                                  = lcs->position[1]->offset
                                  - cl_start;

                                rdiff_ref = &(*rdiff_ref)->next;
                              }

                            /* Detect the EOF */
                            if (lcs->length == 0)
                              break;

                            cm_start = lcs->position[0]->offset;
                            cl_start = lcs->position[1]->offset;

                            (*rdiff_ref) = apr_palloc(pool,
                                                      sizeof(**rdiff_ref));

                            (*rdiff_ref)->type
                              = svn_diff__type_diff_common;
                            (*rdiff_ref)->original_start = original_start;
                            (*rdiff_ref)->original_length = original_length;
                            (*rdiff_ref)->modified_start
                              = modified_start - 1;
                            (*rdiff_ref)->modified_length
                              = lcs->length;
                            (*rdiff_ref)->latest_start
                              = latest_start - 1;
                            (*rdiff_ref)->latest_length
                              = lcs->length;

                            rdiff_ref = &(*rdiff_ref)->next;

                            cm_start += lcs->length;
                            cl_start += lcs->length;

                            lcs = lcs->next;
                          }

                        *rdiff_ref = NULL;
                      }

                    svn_pool_destroy(subpool2);
                  }
#else /* !USE_SMART_CONFLICT_DETECTION */
                type = svn_diff__type_diff_common;

                if (modified_length != latest_length)
                  {
                    type = svn_diff__type_conflict;
                  }
                else if (modified_length > 0)
                  {
                    /* First find the starting positions for the
                     * comparison
                     */
                    while (position_list[1]->offset < modified_start)
                         position_list[1] = position_list[1]->next;

                    while (position_list[2]->offset < latest_start)
                         position_list[2] = position_list[2]->next;

                      while (modified_length)
                        {
                          if (position_list[1]->node != position_list[2]->node)
                            {
                              type = svn_diff__type_conflict;
                              break;
                            }

                          position_list[1] = position_list[1]->next;
                          position_list[2] = position_list[2]->next;

                          modified_length--;
                        }

                    modified_length = latest_length;
                  }
#endif /* !USE_SMART_CONFLICT_DETECTION */
              }
            else if (is_modified)
              {
                type = svn_diff__type_diff_modified;
              }
            else
              {
                type = svn_diff__type_diff_latest;
              }

            (*diff_ref) = apr_palloc(pool, sizeof(**diff_ref));

            (*diff_ref)->type = type;
            (*diff_ref)->original_start = original_start;
            (*diff_ref)->original_length = original_sync - original_start;
            (*diff_ref)->modified_start = modified_start;
            (*diff_ref)->modified_length = modified_length;
            (*diff_ref)->latest_start = latest_start;
            (*diff_ref)->latest_length = latest_length;
            (*diff_ref)->resolved_diff = resolved_diff;

            diff_ref = &(*diff_ref)->next;
          }

        /* Detect EOF */
        if (lcs_om->length == 0 && lcs_ol->length == 0)
            break;

        modified_length = lcs_om->length
                          - (original_sync - lcs_om->position[0]->offset);
        latest_length = lcs_ol->length
                        - (original_sync - lcs_ol->position[0]->offset);
        common_length = modified_length < latest_length
                        ? modified_length : latest_length;

        (*diff_ref) = apr_palloc(pool, sizeof(**diff_ref));

        (*diff_ref)->type = svn_diff__type_common;
        (*diff_ref)->original_start = original_start;
        (*diff_ref)->original_length = common_length;
        (*diff_ref)->modified_start = modified_start;
        (*diff_ref)->modified_length = common_length;
        (*diff_ref)->latest_start = latest_start;
        (*diff_ref)->latest_length = common_length;
        (*diff_ref)->resolved_diff = NULL;

        diff_ref = &(*diff_ref)->next;

        /* Set the new offsets */
        original_start = original_sync + common_length;
        modified_start = modified_sync + common_length;
        latest_start = latest_sync + common_length;

        /* Make it easier for diff_common/conflict detection
           by recording last lcs start positions
         */
        position_list[1] = lcs_om->position[1];
        position_list[2] = lcs_ol->position[1];

        /* Make sure we are pointing to lcs entries beyond
         * the range we just processed
         */
        while (original_start > lcs_om->position[0]->offset)
          lcs_om = lcs_om->next;

        while (original_start > lcs_ol->position[0]->offset)
          lcs_ol = lcs_ol->next;
      }

    *diff_ref = NULL;
  }

  svn_pool_destroy(subpool);

  return NULL;
}


svn_boolean_t
svn_diff_contains_conflicts(svn_diff_t *diff)
{
  while (diff != NULL)
    {
      if (diff->type == svn_diff__type_conflict)
        {
          return TRUE;
        }

      diff = diff->next;
    }

  return FALSE;
}

svn_boolean_t
svn_diff_contains_diffs(svn_diff_t *diff)
{
  while (diff != NULL)
    {
      if (diff->type != svn_diff__type_common)
        {
          return TRUE;
        }

      diff = diff->next;
    }

  return FALSE;
}

svn_error_t *
svn_diff_output(svn_diff_t *diff,
                void *output_baton,
                const svn_diff_output_fns_t *vtable)
{
  svn_error_t *(*output_fn)(void *,
                            apr_off_t, apr_off_t,
                            apr_off_t, apr_off_t,
                            apr_off_t, apr_off_t);

  while (diff != NULL)
    {
      switch (diff->type)
        {
        case svn_diff__type_common:
          output_fn = vtable->output_common;
          break;

        case svn_diff__type_diff_common:
          output_fn = vtable->output_diff_common;
          break;

        case svn_diff__type_diff_modified:
          output_fn = vtable->output_diff_modified;
          break;

        case svn_diff__type_diff_latest:
          output_fn = vtable->output_diff_latest;
          break;

        case svn_diff__type_conflict:
          output_fn = NULL;
          if (vtable->output_conflict != NULL)
            {
              SVN_ERR(vtable->output_conflict(output_baton,
                               diff->original_start, diff->original_length,
                               diff->modified_start, diff->modified_length,
                               diff->latest_start, diff->latest_length,
                               diff->resolved_diff));
            }
          break;

        default:
          output_fn = NULL;
          break;
        }

      if (output_fn != NULL)
        {
          SVN_ERR(output_fn(output_baton,
                            diff->original_start, diff->original_length,
                            diff->modified_start, diff->modified_length,
                            diff->latest_start, diff->latest_length));
        }

      diff = diff->next;
    }

  return NULL;
}
