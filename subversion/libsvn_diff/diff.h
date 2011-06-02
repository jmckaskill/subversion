/*
 * diff.h :  private header file
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

#if !defined(DIFF_H)
#define DIFF_H

#include <apr.h>
#include <apr_pools.h>
#include <apr_general.h>

#include "svn_diff.h"
#include "svn_types.h"

#define SVN_DIFF__UNIFIED_CONTEXT_SIZE 3

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

struct svn_diff__position_t
{
  svn_diff__position_t *next;
  svn_diff__node_t     *node;
  apr_off_t             offset;
};

struct svn_diff__lcs_t
{
  svn_diff__lcs_t      *next;
  svn_diff__position_t *position[2];
  apr_off_t             length;
  int                   refcount;
};


/* State used when normalizing whitespace and EOL styles. */
typedef enum svn_diff__normalize_state_t
{
  /* Initial state; not in a sequence of whitespace. */
  svn_diff__normalize_state_normal,
  /* We're in a sequence of whitespace characters.  Only entered if
     we ignore whitespace. */
  svn_diff__normalize_state_whitespace,
  /* The previous character was CR. */
  svn_diff__normalize_state_cr
} svn_diff__normalize_state_t;


/*
 * Calculate the Longest Common Subsequence (LCS) between two datasources,
 * POSITION_LIST1 and POSITION_LIST2. From the beginning of each list, 
 * PREFIX_LINES lines will be assumed to be equal and be excluded from the 
 * comparison process. Similarly, SUFFIX_LINES at the end of both sequences
 * will be skipped. The resulting lcs structure will be the return value
 * of this function. Allocations will be made from POOL.
 */
svn_diff__lcs_t *
svn_diff__lcs(svn_diff__position_t *position_list1, /* pointer to tail (ring) */
              svn_diff__position_t *position_list2, /* pointer to tail (ring) */
              apr_off_t prefix_lines,
              apr_off_t suffix_lines,
              apr_pool_t *pool);


/*
 * Support functions to build a tree of token positions
 */
void
svn_diff__tree_create(svn_diff__tree_t **tree, apr_pool_t *pool);


/*
 * Get all tokens from a datasource.  Return the
 * last item in the (circular) list.
 */
svn_error_t *
svn_diff__get_tokens(svn_diff__position_t **position_list,
                     svn_diff__tree_t *tree,
                     void *diff_baton,
                     const svn_diff_fns2_t *vtable,
                     svn_diff_datasource_e datasource,
                     apr_off_t prefix_lines,
                     apr_pool_t *pool);


/* Morph a svn_lcs_t into a svn_diff_t. */
svn_diff_t *
svn_diff__diff(svn_diff__lcs_t *lcs,
               apr_off_t original_start, apr_off_t modified_start,
               svn_boolean_t want_common,
               apr_pool_t *pool);

void
svn_diff__resolve_conflict(svn_diff_t *hunk,
                           svn_diff__position_t **position_list1,
                           svn_diff__position_t **position_list2,
                           apr_pool_t *pool);


/* Normalize the characters pointed to by the buffer BUF (of length *LENGTHP)
 * according to the options *OPTS, starting in the state *STATEP.
 *
 * Adjust *LENGTHP and *STATEP to be the length of the normalized buffer and
 * the final state, respectively.
 * Normalized data is written to the memory at *TGT. BUF and TGT may point
 * to the same memory area.  The memory area pointed to by *TGT should be
 * large enough to hold *LENGTHP bytes.
 * When on return *TGT is not equal to the value passed in, it points somewhere
 * into the memory region designated by BUF and *LENGTHP.
 */
void
svn_diff__normalize_buffer(char **tgt,
                           apr_off_t *lengthp,
                           svn_diff__normalize_state_t *statep,
                           const char *buf,
                           const svn_diff_file_options_t *opts);


#endif /* DIFF_H */
