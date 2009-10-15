/*
 * mergeinfo.c:  Mergeinfo parsing and handling
 *
 * ====================================================================
 *    Licensed to the Subversion Corporation (SVN Corp.) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The SVN Corp. licenses this file
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
#include <assert.h>
#include <ctype.h>

#include "svn_path.h"
#include "svn_types.h"
#include "svn_ctype.h"
#include "svn_pools.h"
#include "svn_sorts.h"
#include "svn_error.h"
#include "svn_error_codes.h"
#include "svn_string.h"
#include "svn_mergeinfo.h"
#include "private/svn_mergeinfo_private.h"
#include "svn_private_config.h"
#include "svn_hash.h"

/* Attempt to combine two ranges, IN1 and IN2. If they are adjacent or
   overlapping, and their inheritability allows them to be combined, put
   the result in OUTPUT and return TRUE, otherwise return FALSE.

   CONSIDER_INHERITANCE determines how to account for the inheritability
   of IN1 and IN2 when trying to combine ranges.  If ranges with different
   inheritability are combined (CONSIDER_INHERITANCE must be FALSE for this
   to happen) the result is inheritable.  If both ranges are inheritable the
   result is inheritable.  Only and if both ranges are non-inheritable is
   the result is non-inheritable.

   Range overlapping detection algorithm from
   http://c2.com/cgi-bin/wiki/fullSearch?TestIfDateRangesOverlap
*/
static svn_boolean_t
combine_ranges(svn_merge_range_t *output,
               const svn_merge_range_t *in1,
               const svn_merge_range_t *in2,
               svn_boolean_t consider_inheritance)
{
  if (in1->start <= in2->end && in2->start <= in1->end)
    {
      if (!consider_inheritance
          || (consider_inheritance
              && (in1->inheritable == in2->inheritable)))
        {
          output->start = MIN(in1->start, in2->start);
          output->end = MAX(in1->end, in2->end);
          output->inheritable = (in1->inheritable || in2->inheritable);
          return TRUE;
        }
    }
  return FALSE;
}

/* pathname -> PATHNAME */
static svn_error_t *
parse_pathname(const char **input, const char *end,
               svn_stringbuf_t **pathname, apr_pool_t *pool)
{
  const char *curr = *input;
  const char *last_colon = NULL;

  /* A pathname may contain colons, so find the last colon before END
     or newline.  We'll consider this the divider between the pathname
     and the revisionlist. */
  while (curr < end && *curr != '\n')
    {
      if (*curr == ':')
        last_colon = curr;
      curr++;
    }

  if (!last_colon)
    return svn_error_create(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                            _("Pathname not terminated by ':'"));
  if (last_colon == *input)
    return svn_error_create(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                            _("No pathname preceding ':'"));

  *pathname = svn_stringbuf_ncreate(*input, last_colon - *input, pool);
  *input = last_colon;

  return SVN_NO_ERROR;
}

/* Ways in which two svn_merge_range_t can intersect, if at all. */
typedef enum
{
  /* Ranges don't intersect. */
  svn__no_intersection,

  /* Ranges are equal. */
  svn__equal_intersection,

  /* Ranges adjoin but don't overlap. */
  svn__adjoining_intersection,

  /* Ranges overalp but neither is a subset of the other. */
  svn__overlapping_intersection,

  /* One range is a proper subset of the other. */
  svn__proper_subset_intersection
} intersection_type_t;

/* Given ranges R1 and R2, both of which must be forward merge ranges,
   set *INTERSECTION_TYPE to describe how the ranges intersect, if they
   do at all.  The inheritance type of the ranges is not considered. */
static svn_error_t *
get_type_of_intersection(const svn_merge_range_t *r1,
                         const svn_merge_range_t *r2,
                         intersection_type_t *intersection_type)
{
  SVN_ERR_ASSERT(r1);
  SVN_ERR_ASSERT(r2);
  
  /* Why not use SVN_IS_VALID_REVNUM here?  Because revision 0
     is described START = -1, END = 0.  See svn_merge_range_t. */
  SVN_ERR_ASSERT(r1->start >= -1);
  SVN_ERR_ASSERT(r2->start >= -1);
  
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(r1->end));
  SVN_ERR_ASSERT(SVN_IS_VALID_REVNUM(r2->end));
  SVN_ERR_ASSERT(r1->start < r1->end);
  SVN_ERR_ASSERT(r2->start < r2->end);

  if (!(r1->start <= r2->end && r2->start <= r1->end))
    *intersection_type = svn__no_intersection;
  else if (r1->start == r2->start && r1->end == r2->end)
    *intersection_type = svn__equal_intersection;
  else if (r1->end == r2->start || r2->end == r1->start)
    *intersection_type = svn__adjoining_intersection;
  else if (r1->start <= r2->start && r1->end >= r2->end)
    *intersection_type = svn__proper_subset_intersection;
  else if (r2->start <= r1->start && r2->end >= r1->end)
    *intersection_type = svn__proper_subset_intersection;
  else
    *intersection_type = svn__overlapping_intersection;

  return SVN_NO_ERROR;
}

/* Modify or extend RANGELIST (a list of merge ranges) to incorporate
   NEW_RANGE. RANGELIST is a "rangelist" as defined in svn_mergeinfo.h.

   OVERVIEW

   Determine the minimal set of non-overlapping merge ranges required to
   represent the combination of RANGELIST and NEW_RANGE. The result depends
   on whether and how NEW_RANGE overlaps any merge range[*] in RANGELIST,
   and also on any differences in the inheritability of each range,
   according to the rules described below. Modify RANGELIST to represent
   this result, by adjusting the last range in it and/or appending one or
   two more ranges.

   ([*] Due to the simplifying assumption below, only the last range in
   RANGELIST is considered.)

   DETAILS

   If RANGELIST is not empty assume NEW_RANGE does not intersect with any
   range before the last one in RANGELIST.

   If RANGELIST is empty or NEW_RANGE does not intersect with the lastrange
   in RANGELIST, then append a copy of NEW_RANGE, allocated in RESULT_POOL,
   to RANGELIST.

   If NEW_RANGE intersects with the last range in RANGELIST then combine
   these two ranges as described below:

   If the intersecting ranges have the same inheritability then simply
   combine the ranges in place.  Otherwise, if the ranges intersect but
   differ in inheritability, then merge the ranges as dictated by
   CONSIDER_INHERITANCE:

   If CONSIDER_INHERITANCE is false then intersecting ranges are combined
   into a single range.  The inheritability of the resulting range is
   non-inheritable *only* if both ranges are non-inheritable, otherwise the
   combined range is inheritable, e.g.:

     Last range in        NEW_RANGE        RESULTING RANGES
     RANGELIST
     -------------        ---------        ----------------
     4-10*                6-13             4-13
     4-10                 6-13*            4-13
     4-10*                6-13*            4-13*

   If CONSIDER_INHERITANCE is true, then only the intersection between the
   two ranges is combined, with the inheritability of the resulting range
   non-inheritable only if both ranges were non-inheritable.  The
   non-intersecting portions are added as separate ranges allocated in
   RESULT_POOL, e.g.:

     Last range in        NEW_RANGE        RESULTING RANGES
     RANGELIST
     -------------        ---------        ----------------
     4-10*                6                4-5*, 6, 7-10*
     4-10*                6-12             4-5*, 6-12

   Note that the standard rules for rangelists still apply and overlapping
   ranges are not allowed.  So if the above would result in overlapping
   ranges of the same inheritance, the overlapping ranges are merged into a
   single range, e.g.:

     Last range in        NEW_RANGE        RESULTING RANGES
     RANGELIST
     -------------        ---------        ----------------
     4-10                 6*               4-10 (Not 4-5, 6, 7-10)

   When replacing the last range in RANGELIST, either allocate a new range in
   RESULT_POOL or modify the existing range in place.  Any new ranges added
   to RANGELIST are allocated in RESULT_POOL.  SCRATCH_POOL is used for any
   temporary allocations.
*/
static svn_error_t *
combine_with_lastrange(const svn_merge_range_t *new_range,
                       apr_array_header_t *rangelist,
                       svn_boolean_t consider_inheritance,
                       apr_pool_t *result_pool,
                       apr_pool_t *scratch_pool)
{
  svn_merge_range_t *lastrange;
  svn_merge_range_t combined_range;

  /* We don't accept a NULL RANGELIST. */
  SVN_ERR_ASSERT(rangelist);

  if (rangelist->nelts > 0)
    lastrange = APR_ARRAY_IDX(rangelist, rangelist->nelts - 1, svn_merge_range_t *);
  else
    lastrange = NULL;

  if (!lastrange)
    {
      /* No *LASTRANGE so push NEW_RANGE onto RANGELIST and we are done. */
      APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) =
        svn_merge_range_dup(new_range, result_pool);
    }
  else if (!consider_inheritance)
    {
      /* We are not considering inheritance so we can merge intersecting
         ranges of different inheritability.  Of course if the ranges
         don't intersect at all we simply push NEW_RANGE only RANGELIST. */
      if (combine_ranges(&combined_range, lastrange, new_range, FALSE))
        {
          *lastrange = combined_range;
        }
      else
        {
          APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) =
            svn_merge_range_dup(new_range, result_pool);
        }
    }
  else /* Considering inheritance */
    {
      if (combine_ranges(&combined_range, lastrange, new_range, TRUE))
        {
          /* Even when considering inheritance two intersection ranges
             of the same inheritability can simply be combined. */
          *lastrange = combined_range;
        }
      else
        {
          /* If we are here then the ranges either don't intersect or do
             intersect but have differing inheritability.  Check for the
             first case as that is easy to handle. */
          intersection_type_t intersection_type;
          
          SVN_ERR(get_type_of_intersection(new_range, lastrange,
                                           &intersection_type));
              
              switch (intersection_type)
                {
                  case svn__no_intersection:
                    /* NEW_RANGE and *LASTRANGE *really* don't intersect so
                       just push NEW_RANGE only RANGELIST. */
                    APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) =
                      svn_merge_range_dup(new_range, result_pool);
                    break;

                  case svn__equal_intersection:
                    /* They range are equal so all we do is force the
                       inheritability of lastrange to true. */
                    lastrange->inheritable = TRUE;
                    break;

                  case svn__adjoining_intersection:
                    /* They adjoin but don't overlap so just push NEW_RANGE
                       onto RANGELIST. */
                    APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) =
                      svn_merge_range_dup(new_range, result_pool);
                    break;

                  case svn__overlapping_intersection:
                    /* They ranges overlap but neither is a proper subset of
                       the other.  We'll end up pusing two new ranges onto
                       RANGELIST, the intersecting part and the part unique to
                       NEW_RANGE.*/
                    {
                      svn_merge_range_t *r1 = svn_merge_range_dup(lastrange,
                                                                  result_pool);
                      svn_merge_range_t *r2 = svn_merge_range_dup(new_range,
                                                                  result_pool);

                      /* Pop off *LASTRANGE to make our manipulations
                         easier. */
                      apr_array_pop(rangelist);

                      /* Ensure R1 is the older range. */
                      if (r2->start < r1->start)
                        {
                          /* Swap R1 and R2. */
                          *r2 = *r1;
                          *r1 = *new_range;
                        }

                      /* Absorb the intersecting ranges into the
                         inheritable range. */
                      if (r1->inheritable)
                        r2->start = r1->end;
                      else
                        r1->end = r2->start;
                      
                      /* Push everything back onto RANGELIST. */
                      APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) = r1;
                      APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) = r2;

                      break;
                    }

                  default: /* svn__proper_subset_intersection */
                    {
                      /* One range is a proper subset of the other. */
                      svn_merge_range_t *r1 = svn_merge_range_dup(lastrange,
                                                                  result_pool);
                      svn_merge_range_t *r2 = svn_merge_range_dup(new_range,
                                                                  result_pool);
                      svn_merge_range_t *r3 = NULL;
                      svn_revnum_t tmp_revnum;

                      /* Pop off *LASTRANGE to make our manipulations
                         easier. */
                      apr_array_pop(rangelist);

                      /* Ensure R1 is the superset. */
                      if (r2->start < r1->start || r2->end > r1->end)
                        {
                          /* Swap R1 and R2. */
                          *r2 = *r1;
                          *r1 = *new_range;
                        }

                      if (r1->inheritable)
                        {
                          /* The simple case: The superset is inheritable, so
                             just combine r1 and r2. */
                          r1->start = MIN(r1->start, r2->start);
                          r1->end = MAX(r1->end, r2->end);
                          r2 = NULL;
                        }
                      else if (r1->start == r2->start)
                        {
                          /* *LASTRANGE and NEW_RANGE share an end point. */
                          tmp_revnum = r1->end;
                          r1->end = r2->end;
                          r2->inheritable = r1->inheritable;
                          r1->inheritable = TRUE;
                          r2->start = r1->end;
                          r2->end = tmp_revnum;
                        }
                      else if (r1->end == r2->end)
                        {
                          /* *LASTRANGE and NEW_RANGE share an end point. */
                          r1->end = r2->start;
                          r2->inheritable = TRUE;
                        }
                      else
                        {
                          /* NEW_RANGE and *LASTRANGE share neither start
                             nor end points. */
                          r3 = apr_pcalloc(result_pool, sizeof(*r3));
                          r3->start = r2->end;
                          r3->end = r1->end;
                          r3->inheritable = r1->inheritable;
                          r2->inheritable = TRUE;
                          r1->end = r2->start;
                        }

                      /* Push everything back onto RANGELIST. */
                      APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) = r1;
                      if (r2)
                        APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) = r2;
                      if (r3)
                        APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) = r3;

                      break;
                    }
                }

              /* Some of the above cases might have put *RANGELIST out of
                 order, so re-sort.*/
              qsort(rangelist->elts, rangelist->nelts, rangelist->elt_size,
                    svn_sort_compare_ranges);
        }
    }

  return SVN_NO_ERROR;
}

/* Convert a single svn_merge_range_t * back into an svn_string_t *.  */
static svn_error_t *
range_to_string(svn_string_t **result, const svn_merge_range_t *range,
                apr_pool_t *pool)
{
  if (range->start == range->end - 1)
    *result = svn_string_createf(pool, "%ld%s", range->end,
                                 range->inheritable
                                 ? "" : SVN_MERGEINFO_NONINHERITABLE_STR);
  else if (range->start - 1 == range->end)
    *result = svn_string_createf(pool, "-%ld%s", range->start,
                                 range->inheritable
                                 ? "" : SVN_MERGEINFO_NONINHERITABLE_STR);
  else if (range->start < range->end)
    *result = svn_string_createf(pool, "%ld-%ld%s", range->start + 1,
                                 range->end, range->inheritable
                                 ? "" : SVN_MERGEINFO_NONINHERITABLE_STR);
  else
    *result = svn_string_createf(pool, "%ld-%ld%s", range->start,
                                 range->end + 1, range->inheritable
                                 ? "" : SVN_MERGEINFO_NONINHERITABLE_STR);
  return SVN_NO_ERROR;
}

/* Helper for svn_mergeinfo_parse()

   revisionlist -> (revisionelement)(COMMA revisionelement)*
   revisionrange -> REVISION "-" REVISION("*")
   revisionelement -> revisionrange | REVISION("*")

   PATHNAME is the path this revisionlist is mapped to.  It is
   used only for producing a more descriptive error message.
*/
static svn_error_t *
parse_rangelist(const char **input, const char *end,
                apr_array_header_t *rangelist, const char *pathname,
                apr_pool_t *pool)
{
  const char *curr = *input;

  /* Eat any leading horizontal white-space before the rangelist. */
  while (curr < end && *curr != '\n' && isspace(*curr))
    curr++;

  if (*curr == '\n' || curr == end)
    {
      /* Empty range list. */
      *input = curr;
      return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                               _("Mergeinfo for '%s' maps to an "
                                 "empty revision range"), pathname);
    }

  while (curr < end && *curr != '\n')
    {
      /* Parse individual revisions or revision ranges. */
      svn_merge_range_t *mrange = apr_pcalloc(pool, sizeof(*mrange));
      svn_revnum_t firstrev;

      SVN_ERR(svn_revnum_parse(&firstrev, curr, &curr));
      if (*curr != '-' && *curr != '\n' && *curr != ',' && *curr != '*'
          && curr != end)
        return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                                 _("Invalid character '%c' found in revision "
                                   "list"), *curr);
      mrange->start = firstrev - 1;
      mrange->end = firstrev;
      mrange->inheritable = TRUE;

      if (*curr == '-')
        {
          svn_revnum_t secondrev;

          curr++;
          SVN_ERR(svn_revnum_parse(&secondrev, curr, &curr));
          if (firstrev > secondrev)
            return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                                     _("Unable to parse reversed revision "
                                       "range '%ld-%ld'"),
                                       firstrev, secondrev);
          else if (firstrev == secondrev)
            return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                                     _("Unable to parse revision range "
                                       "'%ld-%ld' with same start and end "
                                       "revisions"), firstrev, secondrev);
          mrange->end = secondrev;
        }

      if (*curr == '\n' || curr == end)
        {
          APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) = mrange;
          *input = curr;
          return SVN_NO_ERROR;
        }
      else if (*curr == ',')
        {
          APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) = mrange;
          curr++;
        }
      else if (*curr == '*')
        {
          mrange->inheritable = FALSE;
          curr++;
          if (*curr == ',' || *curr == '\n' || curr == end)
            {
              APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) = mrange;
              if (*curr == ',')
                {
                  curr++;
                }
              else
                {
                  *input = curr;
                  return SVN_NO_ERROR;
                }
            }
          else
            {
              return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                                       _("Invalid character '%c' found in "
                                         "range list"), *curr);
            }
        }
      else
        {
          return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                                   _("Invalid character '%c' found in "
                                     "range list"), *curr);
        }

    }
  if (*curr != '\n')
    return svn_error_create(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                            _("Range list parsing ended before hitting "
                              "newline"));
  *input = curr;
  return SVN_NO_ERROR;
}

/* revisionline -> PATHNAME COLON revisionlist */
static svn_error_t *
parse_revision_line(const char **input, const char *end, svn_mergeinfo_t hash,
                    apr_pool_t *pool)
{
  svn_stringbuf_t *pathname;
  apr_array_header_t *rangelist = apr_array_make(pool, 1,
                                                 sizeof(svn_merge_range_t *));

  SVN_ERR(parse_pathname(input, end, &pathname, pool));

  if (*(*input) != ':')
    return svn_error_create(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                            _("Pathname not terminated by ':'"));

  *input = *input + 1;

  SVN_ERR(parse_rangelist(input, end, rangelist, pathname->data, pool));

  if (*input != end && *(*input) != '\n')
    return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                             _("Could not find end of line in range list line "
                               "in '%s'"), *input);

  if (*input != end)
    *input = *input + 1;

  /* Sort the rangelist, combine adjacent ranges into single ranges,
     and make sure there are no overlapping ranges. */
  if (rangelist->nelts > 1)
    {
      int i;
      svn_merge_range_t *range, *lastrange;

      qsort(rangelist->elts, rangelist->nelts, rangelist->elt_size,
            svn_sort_compare_ranges);
      lastrange = APR_ARRAY_IDX(rangelist, 0, svn_merge_range_t *);

      for (i = 1; i < rangelist->nelts; i++)
        {
          range = APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *);
          if (lastrange->start <= range->end
              && range->start <= lastrange->end)
            {
              /* The ranges are adjacent or intersect. */

              /* svn_mergeinfo_parse promises to combine overlapping
                 ranges as long as their inheritability is the same. */
              if (range->start < lastrange->end
                  && range->inheritable != lastrange->inheritable)
                {
                  svn_string_t *r1, *r2;

                  SVN_ERR(range_to_string(&r1, lastrange, pool));
                  SVN_ERR(range_to_string(&r2, range, pool));
                  return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                                           _("Unable to parse overlapping "
                                             "revision ranges '%s' and '%s' "
                                             "with different inheritance "
                                             "types"), r1->data, r2->data);
                }

              /* Combine overlapping or adjacent ranges with the
                 same inheritability. */
              if (lastrange->inheritable == range->inheritable)
                {
                  lastrange->end = MAX(range->end, lastrange->end);
                  if (i + 1 < rangelist->nelts)
                    memmove(rangelist->elts + (rangelist->elt_size * i),
                            rangelist->elts + (rangelist->elt_size * (i + 1)),
                            rangelist->elt_size * (rangelist->nelts - i));
                  rangelist->nelts--;
                  i--;
                }
            }
          lastrange = APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *);
        }
    }
  apr_hash_set(hash, pathname->data, APR_HASH_KEY_STRING, rangelist);

  return SVN_NO_ERROR;
}

/* top -> revisionline (NEWLINE revisionline)*  */
static svn_error_t *
parse_top(const char **input, const char *end, svn_mergeinfo_t hash,
          apr_pool_t *pool)
{
  while (*input < end)
    SVN_ERR(parse_revision_line(input, end, hash, pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo_parse(svn_mergeinfo_t *mergeinfo,
                    const char *input,
                    apr_pool_t *pool)
{
  svn_error_t *err;

  *mergeinfo = apr_hash_make(pool);
  err = parse_top(&input, input + strlen(input), *mergeinfo, pool);

  /* Always return SVN_ERR_MERGEINFO_PARSE_ERROR as the topmost error. */
  if (err && err->apr_err != SVN_ERR_MERGEINFO_PARSE_ERROR)
    err = svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, err,
                            _("Could not parse mergeinfo string '%s'"),
                            input);
  return err;
}


svn_error_t *
svn_rangelist_merge(apr_array_header_t **rangelist,
                    apr_array_header_t *changes,
                    apr_pool_t *pool)
{
  int i, j;
  apr_array_header_t *output = apr_array_make(pool, 1,
                                              sizeof(svn_merge_range_t *));
  i = 0;
  j = 0;
  while (i < (*rangelist)->nelts && j < changes->nelts)
    {
      svn_merge_range_t *elt1, *elt2;
      int res;

      elt1 = APR_ARRAY_IDX(*rangelist, i, svn_merge_range_t *);
      elt2 = APR_ARRAY_IDX(changes, j, svn_merge_range_t *);

      res = svn_sort_compare_ranges(&elt1, &elt2);
      if (res == 0)
        {
          /* Only when merging two non-inheritable ranges is the result also
             non-inheritable.  In all other cases ensure an inheritiable
             result. */
          if (elt1->inheritable || elt2->inheritable)
            elt1->inheritable = TRUE;
          SVN_ERR(combine_with_lastrange(elt1, output,
                                         TRUE, pool, pool));
          i++;
          j++;
        }
      else if (res < 0)
        {
          SVN_ERR(combine_with_lastrange(elt1, output,
                                         TRUE, pool, pool));
          i++;
        }
      else
        {
          SVN_ERR(combine_with_lastrange(elt2, output,
                                         TRUE, pool, pool));
          j++;
        }
    }
  /* Copy back any remaining elements.
     Only one of these loops should end up running, if anything. */

  SVN_ERR_ASSERT(!(i < (*rangelist)->nelts && j < changes->nelts));

  for (; i < (*rangelist)->nelts; i++)
    {
      svn_merge_range_t *elt = APR_ARRAY_IDX(*rangelist, i,
                                             svn_merge_range_t *);
      SVN_ERR(combine_with_lastrange(elt, output,
                                     TRUE, pool, pool));
    }


  for (; j < changes->nelts; j++)
    {
      svn_merge_range_t *elt = APR_ARRAY_IDX(changes, j, svn_merge_range_t *);
      SVN_ERR(combine_with_lastrange(elt, output,
                                     TRUE, pool, pool));
    }

  *rangelist = output;
  return SVN_NO_ERROR;
}

static svn_boolean_t
range_intersect(const svn_merge_range_t *first, const svn_merge_range_t *second,
                svn_boolean_t consider_inheritance)
{
  return (first->start + 1 <= second->end)
    && (second->start + 1 <= first->end)
    && (!consider_inheritance
        || (!(first->inheritable) == !(second->inheritable)));
}

static svn_boolean_t
range_contains(const svn_merge_range_t *first, const svn_merge_range_t *second,
               svn_boolean_t consider_inheritance)
{
  return (first->start <= second->start) && (second->end <= first->end)
    && (!consider_inheritance
        || (!(first->inheritable) == !(second->inheritable)));
}

/* Swap start and end fields of RANGE. */
static void
range_swap_endpoints(svn_merge_range_t *range)
{
  svn_revnum_t swap = range->start;
  range->start = range->end;
  range->end = swap;
}

svn_error_t *
svn_rangelist_reverse(apr_array_header_t *rangelist, apr_pool_t *pool)
{
  int i, swap_index;
  svn_merge_range_t range;
  for (i = 0; i < rangelist->nelts / 2; i++)
    {
      swap_index = rangelist->nelts - i - 1;
      range = *APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *);
      *APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *) =
        *APR_ARRAY_IDX(rangelist, swap_index, svn_merge_range_t *);
      *APR_ARRAY_IDX(rangelist, swap_index, svn_merge_range_t *) = range;
      range_swap_endpoints(APR_ARRAY_IDX(rangelist, swap_index,
                                         svn_merge_range_t *));
      range_swap_endpoints(APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *));
    }

  /* If there's an odd number of elements, we still need to swap the
     end points of the remaining range. */
  if (rangelist->nelts % 2 == 1)
    range_swap_endpoints(APR_ARRAY_IDX(rangelist, rangelist->nelts / 2,
                                       svn_merge_range_t *));

  return SVN_NO_ERROR;
}

void
svn_rangelist__set_inheritance(apr_array_header_t *rangelist,
                               svn_boolean_t inheritable)
{
  if (rangelist)
    {
      int i;
      svn_merge_range_t *range;

      for (i = 0; i < rangelist->nelts; i++)
        {
          range = APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *);
          range->inheritable = inheritable;
        }
    }
  return;
}

void
svn_mergeinfo__set_inheritance(svn_mergeinfo_t mergeinfo,
                               svn_boolean_t inheritable,
                               apr_pool_t *scratch_pool)
{
  if (mergeinfo)
    {
      apr_hash_index_t *hi;

      for (hi = apr_hash_first(scratch_pool, mergeinfo);
           hi;
           hi = apr_hash_next(hi))
        {
          apr_array_header_t *rangelist;
          const void *path;
          void *val;
          apr_hash_this(hi, &path, NULL, &val);

          rangelist = apr_hash_get(mergeinfo, path, APR_HASH_KEY_STRING);

          if (rangelist)
            svn_rangelist__set_inheritance(rangelist, inheritable);
        }
    }
  return;
}

/* Either remove any overlapping ranges described by ERASER from
   WHITEBOARD (when DO_REMOVE is TRUE), or capture the overlap, and
   place the remaining or overlapping ranges in OUTPUT. */
/*  ### FIXME: Some variables names and inline comments for this method
    ### are legacy from when it was solely the remove() impl. */
static svn_error_t *
rangelist_intersect_or_remove(apr_array_header_t **output,
                              apr_array_header_t *eraser,
                              apr_array_header_t *whiteboard,
                              svn_boolean_t do_remove,
                              svn_boolean_t consider_inheritance,
                              apr_pool_t *pool)
{
  int i, j, lasti;
  svn_merge_range_t wboardelt;

  *output = apr_array_make(pool, 1, sizeof(svn_merge_range_t *));

  i = 0;
  j = 0;
  lasti = -1;  /* Initialized to a value that "i" will never be. */

  while (i < whiteboard->nelts && j < eraser->nelts)
    {
      svn_merge_range_t *elt1, *elt2;

      elt2 = APR_ARRAY_IDX(eraser, j, svn_merge_range_t *);

      /* Instead of making a copy of the entire array of whiteboard
         elements, we just keep a copy of the current whiteboard element
         that needs to be used, and modify our copy if necessary. */
      if (i != lasti)
        {
          wboardelt = *(APR_ARRAY_IDX(whiteboard, i, svn_merge_range_t *));
          lasti = i;
        }

      elt1 = &wboardelt;

      /* If the whiteboard range is contained completely in the
         eraser, we increment the whiteboard.
         If the ranges intersect, and match exactly, we increment both
         eraser and whiteboard.
         Otherwise, we have to generate a range for the left part of
         the removal of eraser from whiteboard, and possibly change
         the whiteboard to the remaining portion of the right part of
         the removal, to test against. */
      if (range_contains(elt2, elt1, consider_inheritance))
        {
          if (!do_remove)
            SVN_ERR(combine_with_lastrange(elt1, *output,
                                           consider_inheritance, pool,
                                           pool));

          i++;

          if (elt1->start == elt2->start && elt1->end == elt2->end)
            j++;
        }
      else if (range_intersect(elt2, elt1, consider_inheritance))
        {
          if (elt1->start < elt2->start)
            {
              /* The whiteboard range starts before the eraser range. */
              svn_merge_range_t tmp_range;
              tmp_range.inheritable = elt1->inheritable;
              if (do_remove)
                {
                  /* Retain the range that falls before the eraser start. */
                  tmp_range.start = elt1->start;
                  tmp_range.end = elt2->start;
                }
              else
                {
                  /* Retain the range that falls between the eraser
                     start and whiteboard end. */
                  tmp_range.start = elt2->start;
                  tmp_range.end = MIN(elt1->end, elt2->end);
                }

              SVN_ERR(combine_with_lastrange(&tmp_range,
                                             *output, consider_inheritance,
                                             pool, pool));
            }

          /* Set up the rest of the whiteboard range for further
             processing.  */
          if (elt1->end > elt2->end)
            {
              /* The whiteboard range ends after the eraser range. */
              if (!do_remove)
                {
                  /* Partial overlap. */
                  svn_merge_range_t tmp_range;
                  tmp_range.start = MAX(elt1->start, elt2->start);
                  tmp_range.end = elt2->end;
                  tmp_range.inheritable = elt1->inheritable;
                  SVN_ERR(combine_with_lastrange(&tmp_range,
                                                 *output,
                                                 consider_inheritance,
                                                 pool, pool));
                }

              wboardelt.start = elt2->end;
              wboardelt.end = elt1->end;
            }
          else
            i++;
        }
      else  /* ranges don't intersect */
        {
          /* See which side of the whiteboard the eraser is on.  If it
             is on the left side, we need to move the eraser.

             If it is on past the whiteboard on the right side, we
             need to output the whiteboard and increment the
             whiteboard.  */
          if (svn_sort_compare_ranges(&elt2, &elt1) < 0)
            j++;
          else
            {
              svn_merge_range_t *lastrange;

              if ((*output)->nelts > 0)
                lastrange = APR_ARRAY_IDX(*output, (*output)->nelts - 1,
                                          svn_merge_range_t *);
              else
                lastrange = NULL;

              if (do_remove && !(lastrange &&
                                 combine_ranges(lastrange, lastrange, elt1,
                                                consider_inheritance)))
                {
                  lastrange = svn_merge_range_dup(elt1, pool);
                  APR_ARRAY_PUSH(*output, svn_merge_range_t *) = lastrange;
                }
              i++;
            }
        }
    }

  if (do_remove)
    {
      /* Copy the current whiteboard element if we didn't hit the end
         of the whiteboard, and we still had it around.  This element
         may have been touched, so we can't just walk the whiteboard
         array, we have to use our copy.  This case only happens when
         we ran out of eraser before whiteboard, *and* we had changed
         the whiteboard element. */
      if (i == lasti && i < whiteboard->nelts)
        {
          SVN_ERR(combine_with_lastrange(&wboardelt, *output,
                                         consider_inheritance, pool, pool));
          i++;
        }

      /* Copy any other remaining untouched whiteboard elements.  */
      for (; i < whiteboard->nelts; i++)
        {
          svn_merge_range_t *elt = APR_ARRAY_IDX(whiteboard, i,
                                                 svn_merge_range_t *);

          SVN_ERR(combine_with_lastrange(elt, *output,
                                         consider_inheritance, pool, pool));
        }
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_rangelist_intersect(apr_array_header_t **output,
                        apr_array_header_t *rangelist1,
                        apr_array_header_t *rangelist2,
                        svn_boolean_t consider_inheritance,
                        apr_pool_t *pool)
{
  return rangelist_intersect_or_remove(output, rangelist1, rangelist2, FALSE,
                                       consider_inheritance, pool);
}

svn_error_t *
svn_rangelist_remove(apr_array_header_t **output,
                     apr_array_header_t *eraser,
                     apr_array_header_t *whiteboard,
                     svn_boolean_t consider_inheritance,
                     apr_pool_t *pool)
{
  return rangelist_intersect_or_remove(output, eraser, whiteboard, TRUE,
                                       consider_inheritance, pool);
}

svn_error_t *
svn_rangelist_diff(apr_array_header_t **deleted, apr_array_header_t **added,
                   apr_array_header_t *from, apr_array_header_t *to,
                   svn_boolean_t consider_inheritance,
                   apr_pool_t *pool)
{
  /* The following diagrams illustrate some common range delta scenarios:

     (from)           deleted
     r0 <===========(=========)============[=========]===========> rHEAD
     [to]                                    added

     (from)           deleted                deleted
     r0 <===========(=========[============]=========)===========> rHEAD
     [to]

     (from)           deleted
     r0 <===========(=========[============)=========]===========> rHEAD
     [to]                                    added

     (from)                                  deleted
     r0 <===========[=========(============]=========)===========> rHEAD
     [to]             added

     (from)
     r0 <===========[=========(============)=========]===========> rHEAD
     [to]             added                  added

     (from)  d                                  d             d
     r0 <===(=[=)=]=[==]=[=(=)=]=[=]=[=(===|===(=)==|=|==[=(=]=)=> rHEAD
     [to]        a   a    a   a   a   a                   a
  */

  /* The items that are present in from, but not in to, must have been
     deleted. */
  SVN_ERR(svn_rangelist_remove(deleted, to, from, consider_inheritance,
                               pool));
  /* The items that are present in to, but not in from, must have been
     added.  */
  return svn_rangelist_remove(added, from, to, consider_inheritance, pool);
}

struct mergeinfo_diff_baton
{
  svn_mergeinfo_t from;
  svn_mergeinfo_t to;
  svn_mergeinfo_t deleted;
  svn_mergeinfo_t added;
  svn_boolean_t consider_inheritance;
  apr_pool_t *pool;
};

/* This implements the 'svn_hash_diff_func_t' interface.
   BATON is of type 'struct mergeinfo_diff_baton *'.
*/
static svn_error_t *
mergeinfo_hash_diff_cb(const void *key, apr_ssize_t klen,
                       enum svn_hash_diff_key_status status,
                       void *baton)
{
  /* hash_a is FROM mergeinfo,
     hash_b is TO mergeinfo. */
  struct mergeinfo_diff_baton *cb = baton;
  apr_array_header_t *from_rangelist, *to_rangelist;
  const char *path = key;
  if (status == svn_hash_diff_key_both)
    {
      /* Record any deltas (additions or deletions). */
      apr_array_header_t *deleted_rangelist, *added_rangelist;
      from_rangelist = apr_hash_get(cb->from, path, APR_HASH_KEY_STRING);
      to_rangelist = apr_hash_get(cb->to, path, APR_HASH_KEY_STRING);
      svn_rangelist_diff(&deleted_rangelist, &added_rangelist,
                         from_rangelist, to_rangelist,
                         cb->consider_inheritance, cb->pool);
      if (cb->deleted && deleted_rangelist->nelts > 0)
        apr_hash_set(cb->deleted, apr_pstrdup(cb->pool, path),
                     APR_HASH_KEY_STRING, deleted_rangelist);
      if (cb->added && added_rangelist->nelts > 0)
        apr_hash_set(cb->added, apr_pstrdup(cb->pool, path),
                     APR_HASH_KEY_STRING, added_rangelist);
    }
  else if ((status == svn_hash_diff_key_a) && cb->deleted)
    {
      from_rangelist = apr_hash_get(cb->from, path, APR_HASH_KEY_STRING);
      apr_hash_set(cb->deleted, apr_pstrdup(cb->pool, path),
                   APR_HASH_KEY_STRING,
                   svn_rangelist_dup(from_rangelist, cb->pool));
    }
  else if ((status == svn_hash_diff_key_b) && cb->added)
    {
      to_rangelist = apr_hash_get(cb->to, path, APR_HASH_KEY_STRING);
      apr_hash_set(cb->added, apr_pstrdup(cb->pool, path), APR_HASH_KEY_STRING,
                   svn_rangelist_dup(to_rangelist, cb->pool));
    }
  return SVN_NO_ERROR;
}

/* Record deletions and additions of entire range lists (by path
   presence), and delegate to svn_rangelist_diff() for delta
   calculations on a specific path.  */
static svn_error_t *
walk_mergeinfo_hash_for_diff(svn_mergeinfo_t from, svn_mergeinfo_t to,
                             svn_mergeinfo_t deleted, svn_mergeinfo_t added,
                             svn_boolean_t consider_inheritance,
                             apr_pool_t *result_pool,
                             apr_pool_t *scratch_pool)
{
  struct mergeinfo_diff_baton mdb;
  mdb.from = from;
  mdb.to = to;
  mdb.deleted = deleted;
  mdb.added = added;
  mdb.consider_inheritance = consider_inheritance;
  mdb.pool = result_pool;

  return svn_hash_diff(from, to, mergeinfo_hash_diff_cb, &mdb, scratch_pool);
}

svn_error_t *
svn_mergeinfo_diff(svn_mergeinfo_t *deleted, svn_mergeinfo_t *added,
                   svn_mergeinfo_t from, svn_mergeinfo_t to,
                   svn_boolean_t consider_inheritance,
                   apr_pool_t *pool)
{
  if (from && to == NULL)
    {
      *deleted = svn_mergeinfo_dup(from, pool);
      *added = apr_hash_make(pool);
    }
  else if (from == NULL && to)
    {
      *deleted = apr_hash_make(pool);
      *added = svn_mergeinfo_dup(to, pool);
    }
  else
    {
      *deleted = apr_hash_make(pool);
      *added = apr_hash_make(pool);

      if (from && to)
        {
          SVN_ERR(walk_mergeinfo_hash_for_diff(from, to, *deleted, *added,
                                               consider_inheritance, pool,
                                               pool));
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo__equals(svn_boolean_t *is_equal,
                      svn_mergeinfo_t info1,
                      svn_mergeinfo_t info2,
                      svn_boolean_t consider_inheritance,
                      apr_pool_t *pool)
{
  if (apr_hash_count(info1) == apr_hash_count(info2))
    {
      svn_mergeinfo_t deleted, added;
      SVN_ERR(svn_mergeinfo_diff(&deleted, &added, info1, info2,
                                 consider_inheritance, pool));
      *is_equal = apr_hash_count(deleted) == 0 && apr_hash_count(added) == 0;
    }
  else
    {
      *is_equal = FALSE;
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo_merge(svn_mergeinfo_t mergeinfo, svn_mergeinfo_t changes,
                    apr_pool_t *pool)
{
  apr_array_header_t *sorted1, *sorted2;
  int i, j;

  sorted1 = svn_sort__hash(mergeinfo, svn_sort_compare_items_as_paths, pool);
  sorted2 = svn_sort__hash(changes, svn_sort_compare_items_as_paths, pool);

  i = 0;
  j = 0;
  while (i < sorted1->nelts && j < sorted2->nelts)
    {
      svn_sort__item_t elt1, elt2;
      int res;

      elt1 = APR_ARRAY_IDX(sorted1, i, svn_sort__item_t);
      elt2 = APR_ARRAY_IDX(sorted2, j, svn_sort__item_t);
      res = svn_sort_compare_items_as_paths(&elt1, &elt2);

      if (res == 0)
        {
          apr_array_header_t *rl1, *rl2;

          rl1 = elt1.value;
          rl2 = elt2.value;

          SVN_ERR(svn_rangelist_merge(&rl1, rl2,
                                      pool));
          apr_hash_set(mergeinfo, elt1.key, elt1.klen, rl1);
          i++;
          j++;
        }
      else if (res < 0)
        {
          i++;
        }
      else
        {
          apr_hash_set(mergeinfo, elt2.key, elt2.klen, elt2.value);
          j++;
        }
    }

  /* Copy back any remaining elements from the second hash. */
  for (; j < sorted2->nelts; j++)
    {
      svn_sort__item_t elt = APR_ARRAY_IDX(sorted2, j, svn_sort__item_t);
      apr_hash_set(mergeinfo, elt.key, elt.klen, elt.value);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo_intersect(svn_mergeinfo_t *mergeinfo,
                        svn_mergeinfo_t mergeinfo1,
                        svn_mergeinfo_t mergeinfo2,
                        apr_pool_t *pool)
{
  return svn_mergeinfo_intersect2(mergeinfo, mergeinfo1, mergeinfo2,
                                  TRUE, pool, pool);
}

svn_error_t *
svn_mergeinfo_intersect2(svn_mergeinfo_t *mergeinfo,
                         svn_mergeinfo_t mergeinfo1,
                         svn_mergeinfo_t mergeinfo2,
                         svn_boolean_t consider_ineheritance,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;

  *mergeinfo = apr_hash_make(result_pool);

  /* ### TODO(reint): Do we care about the case when a path in one
     ### mergeinfo hash has inheritable mergeinfo, and in the other
     ### has non-inhertiable mergeinfo?  It seems like that path
     ### itself should really be an intersection, while child paths
     ### should not be... */
  for (hi = apr_hash_first(apr_hash_pool_get(mergeinfo1), mergeinfo1);
       hi; hi = apr_hash_next(hi))
    {
      apr_array_header_t *rangelist;
      const void *path;
      void *val;
      apr_hash_this(hi, &path, NULL, &val);

      rangelist = apr_hash_get(mergeinfo2, path, APR_HASH_KEY_STRING);
      if (rangelist)
        {
          SVN_ERR(svn_rangelist_intersect(&rangelist,
                                          (apr_array_header_t *) val,
                                          rangelist, consider_ineheritance,
                                          scratch_pool));
          if (rangelist->nelts > 0)
            apr_hash_set(*mergeinfo,
                         apr_pstrdup(result_pool, path),
                         APR_HASH_KEY_STRING,
                         svn_rangelist_dup(rangelist, result_pool));
        }
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo_remove(svn_mergeinfo_t *mergeinfo, svn_mergeinfo_t eraser,
                     svn_mergeinfo_t whiteboard, apr_pool_t *pool)
{
  return svn_mergeinfo_remove2(mergeinfo, eraser, whiteboard, TRUE, pool,
                               pool);
}

svn_error_t *
svn_mergeinfo_remove2(svn_mergeinfo_t *mergeinfo,
                      svn_mergeinfo_t eraser,
                      svn_mergeinfo_t whiteboard,
                      svn_boolean_t consider_ineritance,
                      apr_pool_t *result_pool,
                      apr_pool_t *scratch_pool)
{
  *mergeinfo = apr_hash_make(result_pool);
  return walk_mergeinfo_hash_for_diff(whiteboard, eraser, *mergeinfo, NULL,
                                      consider_ineritance, result_pool,
                                      scratch_pool);
}

svn_error_t *
svn_rangelist_to_string(svn_string_t **output,
                        const apr_array_header_t *rangelist,
                        apr_pool_t *pool)
{
  svn_stringbuf_t *buf = svn_stringbuf_create("", pool);

  if (rangelist->nelts > 0)
    {
      int i;
      svn_merge_range_t *range;
      svn_string_t *toappend;

      /* Handle the elements that need commas at the end.  */
      for (i = 0; i < rangelist->nelts - 1; i++)
        {
          range = APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *);
          SVN_ERR(range_to_string(&toappend, range, pool));
          svn_stringbuf_appendcstr(buf, toappend->data);
          svn_stringbuf_appendcstr(buf, ",");
        }

      /* Now handle the last element, which needs no comma.  */
      range = APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *);
      SVN_ERR(range_to_string(&toappend, range, pool));
      svn_stringbuf_appendcstr(buf, toappend->data);
    }

  *output = svn_string_create_from_buf(buf, pool);

  return SVN_NO_ERROR;
}

/* Converts a mergeinfo INPUT to an unparsed mergeinfo in OUTPUT.  If PREFIX
   is not NULL then prepend PREFIX to each line in OUTPUT.  If INPUT contains
   no elements, return the empty string.
 */
static svn_error_t *
mergeinfo_to_stringbuf(svn_stringbuf_t **output,
                       svn_mergeinfo_t input,
                       const char *prefix,
                       apr_pool_t *pool)
{
  *output = svn_stringbuf_create("", pool);

  if (apr_hash_count(input) > 0)
    {
      apr_array_header_t *sorted =
        svn_sort__hash(input, svn_sort_compare_items_as_paths, pool);
      int i;

      for (i = 0; i < sorted->nelts; i++)
        {
          svn_sort__item_t elt = APR_ARRAY_IDX(sorted, i, svn_sort__item_t);
          svn_string_t *revlist;

          SVN_ERR(svn_rangelist_to_string(&revlist, elt.value, pool));
          svn_stringbuf_appendcstr(*output,
                                   apr_psprintf(pool, "%s%s:%s",
                                                prefix ? prefix : "",
                                                (const char *) elt.key,
                                                revlist->data));
          if (i < sorted->nelts - 1)
            svn_stringbuf_appendcstr(*output, "\n");
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo_to_string(svn_string_t **output, svn_mergeinfo_t input,
                        apr_pool_t *pool)
{
  if (apr_hash_count(input) > 0)
    {
      svn_stringbuf_t *mergeinfo_buf;
      SVN_ERR(mergeinfo_to_stringbuf(&mergeinfo_buf, input, NULL, pool));
      *output = svn_string_create_from_buf(mergeinfo_buf, pool);
    }
  else
    {
      *output = svn_string_create("", pool);
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo_sort(svn_mergeinfo_t input, apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  void *val;

  for (hi = apr_hash_first(pool, input); hi; hi = apr_hash_next(hi))
    {
      apr_array_header_t *rl;
      apr_hash_this(hi, NULL, NULL, &val);

      rl = val;
      qsort(rl->elts, rl->nelts, rl->elt_size, svn_sort_compare_ranges);
    }
  return SVN_NO_ERROR;
}

svn_mergeinfo_catalog_t
svn_mergeinfo_catalog_dup(svn_mergeinfo_catalog_t mergeinfo_catalog,
                          apr_pool_t *pool)
{
  svn_mergeinfo_t new_mergeinfo_catalog = apr_hash_make(pool);
  apr_hash_index_t *hi;

  for (hi = apr_hash_first(pool, mergeinfo_catalog);
       hi;
       hi = apr_hash_next(hi))
    {
      const void *key;
      void *val;
      apr_hash_this(hi, &key, NULL, &val);
      apr_hash_set(new_mergeinfo_catalog,
                   apr_pstrdup(pool, key),
                   APR_HASH_KEY_STRING,
                   svn_mergeinfo_dup(val, pool));
    }

  return new_mergeinfo_catalog;
}

svn_mergeinfo_t
svn_mergeinfo_dup(svn_mergeinfo_t mergeinfo, apr_pool_t *pool)
{
  svn_mergeinfo_t new_mergeinfo = apr_hash_make(pool);
  apr_hash_index_t *hi;
  const void *path;
  apr_ssize_t pathlen;
  void *rangelist;

  for (hi = apr_hash_first(pool, mergeinfo); hi; hi = apr_hash_next(hi))
    {
      apr_hash_this(hi, &path, &pathlen, &rangelist);
      apr_hash_set(new_mergeinfo, apr_pstrmemdup(pool, path, pathlen), pathlen,
                   svn_rangelist_dup((apr_array_header_t *) rangelist, pool));
    }

  return new_mergeinfo;
}

svn_error_t *
svn_mergeinfo_inheritable2(svn_mergeinfo_t *output,
                           svn_mergeinfo_t mergeinfo,
                           const char *path,
                           svn_revnum_t start,
                           svn_revnum_t end,
                           svn_boolean_t inheritable,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  apr_hash_index_t *hi;
  const void *key;
  apr_ssize_t keylen;
  void *rangelist;

  svn_mergeinfo_t inheritable_mergeinfo = apr_hash_make(result_pool);
  for (hi = apr_hash_first(scratch_pool, mergeinfo);
       hi;
       hi = apr_hash_next(hi))
    {
      apr_array_header_t *inheritable_rangelist;
      apr_hash_this(hi, &key, &keylen, &rangelist);
      if (!path || svn_path_compare_paths(path, (const char *)key) == 0)
        SVN_ERR(svn_rangelist_inheritable2(&inheritable_rangelist,
                                           (apr_array_header_t *) rangelist,
                                           start, end, inheritable,
                                           result_pool, scratch_pool));
      else
        inheritable_rangelist =
          svn_rangelist_dup((apr_array_header_t *)rangelist, result_pool);

      /* Only add this rangelist if some ranges remain.  A rangelist with
         a path mapped to an empty rangelist is not syntactically valid */
      if (inheritable_rangelist->nelts)
        apr_hash_set(inheritable_mergeinfo,
                     apr_pstrmemdup(result_pool, key, keylen), keylen,
                     inheritable_rangelist);
    }
  *output = inheritable_mergeinfo;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo_inheritable(svn_mergeinfo_t *output,
                          svn_mergeinfo_t mergeinfo,
                          const char *path,
                          svn_revnum_t start,
                          svn_revnum_t end,
                          apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  const void *key;
  apr_ssize_t keylen;
  void *rangelist;

  svn_mergeinfo_t inheritable_mergeinfo = apr_hash_make(pool);
  for (hi = apr_hash_first(pool, mergeinfo); hi; hi = apr_hash_next(hi))
    {
      apr_array_header_t *inheritable_rangelist;
      apr_hash_this(hi, &key, &keylen, &rangelist);
      if (!path || svn_path_compare_paths(path, (const char *)key) == 0)
        SVN_ERR(svn_rangelist_inheritable(&inheritable_rangelist,
                                          (apr_array_header_t *) rangelist,
                                          start, end, pool));
      else
        inheritable_rangelist =
          svn_rangelist_dup((apr_array_header_t *)rangelist, pool);

      /* Only add this rangelist if some ranges remain.  A rangelist with
         a path mapped to an empty rangelist is not syntactically valid */
      if (inheritable_rangelist->nelts)
        apr_hash_set(inheritable_mergeinfo,
                     apr_pstrmemdup(pool, key, keylen), keylen,
                     inheritable_rangelist);
    }
  *output = inheritable_mergeinfo;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_rangelist_inheritable2(apr_array_header_t **inheritable_rangelist,
                           apr_array_header_t *rangelist,
                           svn_revnum_t start,
                           svn_revnum_t end,
                           svn_boolean_t inheritable,
                           apr_pool_t *result_pool,
                           apr_pool_t *scratch_pool)
{
  *inheritable_rangelist = apr_array_make(result_pool, 1,
                                          sizeof(svn_merge_range_t *));
  if (rangelist->nelts)
    {
      if (!SVN_IS_VALID_REVNUM(start)
          || !SVN_IS_VALID_REVNUM(end)
          || end < start)
        {
          int i;
          /* We want all non-inheritable ranges removed. */
          for (i = 0; i < rangelist->nelts; i++)
            {
              svn_merge_range_t *range = APR_ARRAY_IDX(rangelist, i,
                                                       svn_merge_range_t *);
              if (range->inheritable == inheritable)
                {
                  svn_merge_range_t *inheritable_range =
                    apr_palloc(result_pool, sizeof(*inheritable_range));
                  inheritable_range->start = range->start;
                  inheritable_range->end = range->end;
                  inheritable_range->inheritable = TRUE;
                  APR_ARRAY_PUSH(*inheritable_rangelist,
                                 svn_merge_range_t *) = range;
                }
            }
        }
      else
        {
          /* We want only the non-inheritable ranges bound by START
             and END removed. */
          apr_array_header_t *ranges_inheritable =
            apr_array_make(scratch_pool, 0, sizeof(svn_merge_range_t *));
          svn_merge_range_t *range = apr_palloc(scratch_pool, sizeof(*range));

          range->start = start;
          range->end = end;
          range->inheritable = inheritable;
          APR_ARRAY_PUSH(ranges_inheritable, svn_merge_range_t *) = range;

          if (rangelist->nelts)
            SVN_ERR(svn_rangelist_remove(inheritable_rangelist,
                                         ranges_inheritable,
                                         rangelist,
                                         TRUE,
                                         result_pool));
        }
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_rangelist_inheritable(apr_array_header_t **inheritable_rangelist,
                          apr_array_header_t *rangelist,
                          svn_revnum_t start,
                          svn_revnum_t end,
                          apr_pool_t *pool)
{
  return svn_rangelist_inheritable2(inheritable_rangelist, rangelist,
                                    start, end, TRUE, pool, pool);
}

svn_boolean_t
svn_mergeinfo__remove_empty_rangelists(svn_mergeinfo_t mergeinfo,
                                       apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  svn_boolean_t removed_some_ranges = FALSE;

  if (mergeinfo)
    {
      for (hi = apr_hash_first(pool, mergeinfo); hi; hi = apr_hash_next(hi))
        {
          const void *key;
          void *value;
          const char *path;
          apr_array_header_t *rangelist;

          apr_hash_this(hi, &key, NULL, &value);
          path = key;
          rangelist = value;

          if (rangelist->nelts == 0)
            {
              apr_hash_set(mergeinfo, path, APR_HASH_KEY_STRING, NULL);
              removed_some_ranges = TRUE;
            }
        }
    }
  return removed_some_ranges;
}

svn_error_t *
svn_mergeinfo__remove_prefix_from_catalog(svn_mergeinfo_catalog_t *out_catalog,
                                          svn_mergeinfo_catalog_t in_catalog,
                                          const char *prefix,
                                          apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  size_t prefix_len = strlen(prefix);

  *out_catalog = apr_hash_make(pool);

  for (hi = apr_hash_first(pool, in_catalog); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      const char *original_path;
      void *value;
      apr_ssize_t klen;

      apr_hash_this(hi, &key, &klen, &value);
      original_path = key;
      SVN_ERR_ASSERT(klen >= prefix_len);
      SVN_ERR_ASSERT(strncmp(key, prefix, prefix_len) == 0);

      apr_hash_set(*out_catalog, original_path + prefix_len,
                   klen-prefix_len, value);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo__add_suffix_to_mergeinfo(svn_mergeinfo_t *out_mergeinfo,
                                       svn_mergeinfo_t mergeinfo,
                                       const char *suffix,
                                       apr_pool_t *result_pool,
                                       apr_pool_t *scratch_pool)
{
  if (!suffix || svn_dirent_is_absolute(suffix))
    {
      *out_mergeinfo = svn_mergeinfo_dup(mergeinfo, result_pool);
    }
  else
    {
      apr_hash_index_t *hi;
      const char *canonical_suffix = svn_uri_canonicalize(suffix,
                                                          scratch_pool);
      *out_mergeinfo = apr_hash_make(result_pool);

      for (hi = apr_hash_first(scratch_pool, mergeinfo);
           hi;
           hi = apr_hash_next(hi))
        {
          const char *path = svn_apr_hash_index_key(hi);
          apr_array_header_t *rangelist = svn_apr_hash_index_val(hi);

          apr_hash_set(*out_mergeinfo,
                       svn_dirent_join(path, canonical_suffix, result_pool),
                       APR_HASH_KEY_STRING,
                       svn_rangelist_dup(rangelist, result_pool));
        }
    }

  return SVN_NO_ERROR;
}

apr_array_header_t *
svn_rangelist_dup(apr_array_header_t *rangelist, apr_pool_t *pool)
{
  apr_array_header_t *new_rl = apr_array_make(pool, rangelist->nelts,
                                              sizeof(svn_merge_range_t *));
  int i;

  for (i = 0; i < rangelist->nelts; i++)
    {
      APR_ARRAY_PUSH(new_rl, svn_merge_range_t *) =
        svn_merge_range_dup(APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *),
                            pool);
    }

  return new_rl;
}

svn_merge_range_t *
svn_merge_range_dup(const svn_merge_range_t *range, apr_pool_t *pool)
{
  svn_merge_range_t *new_range = apr_palloc(pool, sizeof(*new_range));
  memcpy(new_range, range, sizeof(*new_range));
  return new_range;
}

svn_boolean_t
svn_merge_range_contains_rev(const svn_merge_range_t *range, svn_revnum_t rev)
{
  assert(SVN_IS_VALID_REVNUM(range->start));
  assert(SVN_IS_VALID_REVNUM(range->end));
  assert(range->start != range->end);

  if (range->start < range->end)
    return rev > range->start && rev <= range->end;
  else
    return rev > range->end && rev <= range->start;
}

svn_error_t *
svn_mergeinfo__catalog_to_formatted_string(svn_string_t **output,
                                           svn_mergeinfo_catalog_t catalog,
                                           const char *key_prefix,
                                           const char *val_prefix,
                                           apr_pool_t *pool)
{
  svn_stringbuf_t *output_buf = NULL;

  if (catalog && apr_hash_count(catalog))
    {
      int i;
      apr_array_header_t *sorted_catalog =
        svn_sort__hash(catalog, svn_sort_compare_items_as_paths, pool);

      output_buf = svn_stringbuf_create("", pool);
      for (i = 0; i < sorted_catalog->nelts; i++)
        {
          svn_sort__item_t elt =
            APR_ARRAY_IDX(sorted_catalog, i, svn_sort__item_t);
          const char *path1;
          svn_mergeinfo_t mergeinfo;
          svn_stringbuf_t *mergeinfo_output_buf;

          path1 = elt.key;
          mergeinfo = elt.value;
          if (key_prefix)
            svn_stringbuf_appendcstr(output_buf, key_prefix);
          svn_stringbuf_appendcstr(output_buf, path1);
          svn_stringbuf_appendcstr(output_buf, "\n");
          SVN_ERR(mergeinfo_to_stringbuf(&mergeinfo_output_buf, mergeinfo,
                                         val_prefix ? val_prefix : "", pool));
          svn_stringbuf_appendstr(output_buf, mergeinfo_output_buf);
          svn_stringbuf_appendcstr(output_buf, "\n");
        }
    }
#if SVN_DEBUG
  else if (!catalog)
    {
      output_buf = svn_stringbuf_create(key_prefix ? key_prefix : "", pool);
      svn_stringbuf_appendcstr(output_buf, _("NULL mergeinfo catalog\n"));
    }
  else if (apr_hash_count(catalog) == 0)
    {
      output_buf = svn_stringbuf_create(key_prefix ? key_prefix : "", pool);
      svn_stringbuf_appendcstr(output_buf, _("empty mergeinfo catalog\n"));
    }
#endif

  /* If we have an output_buf, convert it to an svn_string_t;
     otherwise, return a new string containing only a newline
     character.  */
  if (output_buf)
    *output = svn_string_create_from_buf(output_buf, pool);
  else
    *output = svn_string_create("\n", pool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo__to_formatted_string(svn_string_t **output,
                                   svn_mergeinfo_t mergeinfo,
                                   const char *prefix,
                                   apr_pool_t *pool)
{
  svn_stringbuf_t *output_buf = NULL;

  if (mergeinfo && apr_hash_count(mergeinfo))
    {
      SVN_ERR(mergeinfo_to_stringbuf(&output_buf, mergeinfo,
                                     prefix ? prefix : "", pool));
      svn_stringbuf_appendcstr(output_buf, "\n");
    }
#if SVN_DEBUG
  else if (!mergeinfo)
    {
      output_buf = svn_stringbuf_create(prefix ? prefix : "", pool);
      svn_stringbuf_appendcstr(output_buf, _("NULL mergeinfo\n"));
    }
  else if (apr_hash_count(mergeinfo) == 0)
    {
      output_buf = svn_stringbuf_create(prefix ? prefix : "", pool);
      svn_stringbuf_appendcstr(output_buf, _("empty mergeinfo\n"));
    }
#endif

  *output = output_buf ? svn_string_create_from_buf(output_buf, pool)
                       : svn_string_create("", pool);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo__get_range_endpoints(svn_revnum_t *youngest_rev,
                                   svn_revnum_t *oldest_rev,
                                   svn_mergeinfo_t mergeinfo,
                                   apr_pool_t *pool)
{
  *youngest_rev = *oldest_rev = SVN_INVALID_REVNUM;
  if (mergeinfo)
    {
      apr_hash_index_t *hi;

      for (hi = apr_hash_first(pool, mergeinfo); hi; hi = apr_hash_next(hi))
        {
          const void *key;
          void *value;
          const char *path;
          apr_array_header_t *rangelist;

          apr_hash_this(hi, &key, NULL, &value);
          path = key;
          rangelist = value;

          if (rangelist->nelts)
            {
              svn_merge_range_t *range = APR_ARRAY_IDX(rangelist,
                                                       rangelist->nelts - 1,
                                                       svn_merge_range_t *);
              if (!SVN_IS_VALID_REVNUM(*youngest_rev)
                  || (range->end > *youngest_rev))
                *youngest_rev = range->end;

              range = APR_ARRAY_IDX(rangelist, 0, svn_merge_range_t *);
              if (!SVN_IS_VALID_REVNUM(*oldest_rev)
                  || (range->start < *oldest_rev))
                *oldest_rev = range->start;
            }
        }
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo__filter_catalog_by_ranges(svn_mergeinfo_catalog_t *filtered_cat,
                                        svn_mergeinfo_catalog_t catalog,
                                        svn_revnum_t youngest_rev,
                                        svn_revnum_t oldest_rev,
                                        apr_pool_t *pool)
{
  apr_hash_index_t *hi;

  *filtered_cat = apr_hash_make(pool);
  for (hi = apr_hash_first(pool, catalog);
       hi;
       hi = apr_hash_next(hi))
    {
      const void *key;
      void *val;
      const char *path;

      svn_mergeinfo_t mergeinfo, filtered_mergeinfo;
      apr_hash_this(hi, &key, NULL, &val);
      path = key;
      mergeinfo = val;
      SVN_ERR(svn_mergeinfo__filter_mergeinfo_by_ranges(&filtered_mergeinfo,
                                                        mergeinfo,
                                                        youngest_rev,
                                                        oldest_rev,
                                                        pool));
      if (apr_hash_count(filtered_mergeinfo))
        apr_hash_set(*filtered_cat,
                     apr_pstrdup(pool, path),
                     APR_HASH_KEY_STRING,
                     filtered_mergeinfo);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo__filter_mergeinfo_by_ranges(svn_mergeinfo_t *filtered_mergeinfo,
                                          svn_mergeinfo_t mergeinfo,
                                          svn_revnum_t youngest_rev,
                                          svn_revnum_t oldest_rev,
                                          apr_pool_t *pool)
{
  *filtered_mergeinfo = apr_hash_make(pool);

  if (mergeinfo)
    {
      apr_hash_index_t *hi;
      svn_merge_range_t *range = apr_palloc(pool, sizeof(*range));
      apr_array_header_t *filter_rangelist =
        apr_array_make(pool, 1, sizeof(svn_merge_range_t *));

      range->start = oldest_rev;
      range->end = youngest_rev;
      range->inheritable = TRUE;
      APR_ARRAY_PUSH(filter_rangelist, svn_merge_range_t *) = range;

      for (hi = apr_hash_first(pool, mergeinfo); hi; hi = apr_hash_next(hi))
        {
          const void *key;
          void *value;
          const char *path;
          apr_array_header_t *rangelist;

          apr_hash_this(hi, &key, NULL, &value);
          path = key;
          rangelist = value;

          if (rangelist->nelts)
            {
              apr_array_header_t *new_rangelist;

              svn_rangelist_intersect(&new_rangelist, rangelist,
                                      filter_rangelist, FALSE, pool);
              if (new_rangelist->nelts)
                apr_hash_set(*filtered_mergeinfo,
                             apr_pstrdup(pool, path),
                             APR_HASH_KEY_STRING,
                             new_rangelist);
            }
        }
    }
  return SVN_NO_ERROR;
}
