/*
 * mergeinfo.c:  Mergeinfo parsing and handling
 *
 * ====================================================================
 * Copyright (c) 2006-2007 CollabNet.  All rights reserved.
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

/* Attempt to combine two adjacent or overlapping ranges, IN1 and IN2, and put
   the result in OUTPUT.  Return whether they could be combined.

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
combine_ranges(svn_merge_range_t **output, svn_merge_range_t *in1,
               svn_merge_range_t *in2,
               svn_boolean_t consider_inheritance)
{
  if (in1->start <= in2->end && in2->start <= in1->end)
    {
      if (!consider_inheritance
          || (consider_inheritance
              && ((in1->inheritable ? TRUE : FALSE)
                   == (in2->inheritable ? TRUE : FALSE))))
        {
          (*output)->start = MIN(in1->start, in2->start);
          (*output)->end = MAX(in1->end, in2->end);
          (*output)->inheritable =
            (in1->inheritable || in2->inheritable) ? TRUE : FALSE;
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
  *pathname = svn_stringbuf_create("", pool);

  while (curr < end && *curr != ':')
    {
      svn_stringbuf_appendbytes(*pathname, curr, 1);
      curr++;
    }

  if ((*pathname)->len == 0)
    return svn_error_create(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                            _("No pathname preceeding ':'"));
  *input = curr;

  return SVN_NO_ERROR;
}

/* Helper for svn_rangelist_merge() and rangelist_intersect_or_remove().

   If *LASTRANGE is not NULL it should point to the last element in REVLIST.
   REVLIST must be sorted from lowest to highest revision and contain no
   overlapping revision ranges.  Any changes made to REVLIST will maintain
   this guarantee.

   If *LASTRANGE is NULL then push MRANGE to REVLIST.

   If *LASTRANGE and MRANGE don't intersect then push MRANGE to REVLIST.
   If they do intersect and have the same inheritability then combine the
   ranges, updating *LASTRANGE to reflect the new combined range.  If the
   ranges intersect but differ in inheritability, then merge the ranges - see
   the doc string for svn_mergeinfo_merge.  This may result in a change to
   *LASTRANGE's end field and the pushing of up to two new ranges on REVLIST.

     e.g.  *LASTRANGE: '4-10*' merged with MRANGE: '6'________
                  |                           |               |
             Update end field               Push       Account for trimmed 
                  |                           |        range from *LASTRANGE.
                  |                           |        Push it last to
                  |                           |        maintain sort order.
                  |                           |               |
                  V                           V               V
           *LASTRANGE: '4-5*'              MRANGE: '6'   NEWRANGE: '6-10*'

   Upon return, if any new ranges were pushed onto REVLIST, then set
   *LASTRANGE to the last range pushed.

   CONSIDER_INHERITANCE determines how to account for the inheritability of
   MRANGE and *LASTRANGE when determining if they intersect.  If
   CONSIDER_INHERITANCE is TRUE, then only ranges with the same
   inheritability can intersect and therefore be combined.

   If DUP_MRANGE is TRUE then allocate a copy of MRANGE before pushing it
   onto REVLIST.
*/
static APR_INLINE void
combine_with_lastrange(svn_merge_range_t** lastrange,
                       svn_merge_range_t *mrange, svn_boolean_t dup_mrange,
                       apr_array_header_t *revlist,
                       svn_boolean_t consider_inheritance,
                       apr_pool_t *pool)
{
  svn_merge_range_t *pushed_mrange_1 = NULL;
  svn_merge_range_t *pushed_mrange_2 = NULL;
  svn_boolean_t ranges_intersect = FALSE;
  svn_boolean_t ranges_have_same_inheritance = FALSE;
  
  if (*lastrange)
    {
      if ((*lastrange)->start <= mrange->end
          && mrange->start <= (*lastrange)->end)
        ranges_intersect = TRUE;
      if ((*lastrange)->inheritable == mrange->inheritable)
        ranges_have_same_inheritance = TRUE;
    }

  if (!(*lastrange)
      || (!ranges_intersect || (!ranges_have_same_inheritance
                                && consider_inheritance)))

    {
      /* No *LASTRANGE
           or
         LASTRANGE and MRANGE don't intersect
           or
         LASTRANGE and MRANGE "intersect" but have different
         inheritability and we are considering inheritance so
         can't combined them...
         
         ...In all these cases just push MRANGE onto *LASTRANGE. */
      if (dup_mrange)
        pushed_mrange_1 = svn_merge_range_dup(mrange, pool);
      else
        pushed_mrange_1 = mrange;
    }
  else /* MRANGE and *LASTRANGE intersect */
    {
      if (ranges_have_same_inheritance)
        {
          /* Intersecting ranges have the same inheritability
             so just combine them. */
          (*lastrange)->start = MIN((*lastrange)->start, mrange->start);
          (*lastrange)->end = MAX((*lastrange)->end, mrange->end);
          (*lastrange)->inheritable =
            ((*lastrange)->inheritable || mrange->inheritable) ? TRUE : FALSE;
        }
      else /* Ranges intersect but have different
              inheritability so merge the ranges. */
        {
          svn_revnum_t tmp_revnum;

          /* Ranges have same starting revision. */
          if ((*lastrange)->start == mrange->start)
            {
              if ((*lastrange)->end == mrange->end)
                {
                  (*lastrange)->inheritable = TRUE;
                }
              else if ((*lastrange)->end > mrange->end)
                {
                  if (!(*lastrange)->inheritable)
                    {
                      tmp_revnum = (*lastrange)->end;
                      (*lastrange)->end = mrange->end;
                      (*lastrange)->inheritable = TRUE;
                      if (dup_mrange)
                        pushed_mrange_1 = svn_merge_range_dup(mrange, pool);
                      else
                        pushed_mrange_1 = mrange;
                      pushed_mrange_1->start = pushed_mrange_1->start;
                      pushed_mrange_1->end = tmp_revnum;
                      *lastrange = pushed_mrange_1;
                    } 
                }
              else /* (*lastrange)->end < mrange->end) */
                {
                  if (mrange->inheritable)
                    {
                      (*lastrange)->inheritable = TRUE;
                      (*lastrange)->end = mrange->end;
                    }
                  else
                    {
                      if (dup_mrange)
                        pushed_mrange_1 = svn_merge_range_dup(mrange, pool);
                      else
                        pushed_mrange_1 = mrange;
                      pushed_mrange_1->start = (*lastrange)->end;
                    }
                }
            }
          /* Ranges have same ending revision. (Same starting
             and ending revisions already handled above.) */
          else if ((*lastrange)->end == mrange->end)
            {
              if ((*lastrange)->start < mrange->start)
                {
                  if (!(*lastrange)->inheritable)
                    {
                      (*lastrange)->end = mrange->start;
                      if (dup_mrange)
                        pushed_mrange_1 = svn_merge_range_dup(mrange, pool);
                      else
                        pushed_mrange_1 = mrange;
                      *lastrange = pushed_mrange_1;
                    }
                }
              else /* (*lastrange)->start > mrange->start */
                {
                  (*lastrange)->start = mrange->start;
                  (*lastrange)->end = mrange->end;
                  (*lastrange)->inheritable = mrange->inheritable;
                  if (dup_mrange)
                    pushed_mrange_1 = svn_merge_range_dup(mrange, pool);
                  else
                    pushed_mrange_1 = mrange;
                  pushed_mrange_1->start = (*lastrange)->end;
                  pushed_mrange_1->inheritable = TRUE;

                }
            }
          else /* Ranges have different starting and ending revisions. */
            {
              if ((*lastrange)->start < mrange->start)
                {
                  /* If MRANGE is a proper subset of *LASTRANGE and
                     *LASTRANGE is inheritable there is nothing more
                     to do. */
                  if (!((*lastrange)->end > mrange->end
                        && (*lastrange)->inheritable))
                    {
                      tmp_revnum = (*lastrange)->end;
                      if (!(*lastrange)->inheritable)
                        (*lastrange)->end = mrange->start;
                      else
                        mrange->start = (*lastrange)->end;
                      if (dup_mrange)
                        pushed_mrange_1 = svn_merge_range_dup(mrange, pool);
                      else
                        pushed_mrange_1 = mrange;

                      if (tmp_revnum > mrange->end)
                        {
                          pushed_mrange_2 =
                            apr_palloc(pool, sizeof(*pushed_mrange_2));
                          pushed_mrange_2->start = mrange->end;
                          pushed_mrange_2->end = tmp_revnum;
                          pushed_mrange_2->inheritable =
                            (*lastrange)->inheritable;
                        }
                      mrange->inheritable = TRUE;
                    }
                }
              else /* ((*lastrange)->start > mrange->start) */
                {
                  if ((*lastrange)->end < mrange->end)
                    {
                      pushed_mrange_2->start = (*lastrange)->end;
                      pushed_mrange_2->end = mrange->end;
                      pushed_mrange_2->inheritable = mrange->inheritable;

                      tmp_revnum = (*lastrange)->start;
                      (*lastrange)->start = mrange->start;
                      (*lastrange)->end = tmp_revnum;
                      (*lastrange)->inheritable = mrange->inheritable;

                      mrange->start = tmp_revnum;
                      mrange->end = pushed_mrange_2->start;
                      mrange->inheritable = TRUE;
                    }
                  else /* (*lastrange)->end > mrange->end */
                    {
                      pushed_mrange_2->start = mrange->end;
                      pushed_mrange_2->end = (*lastrange)->end;
                      pushed_mrange_2->inheritable =
                        (*lastrange)->inheritable;

                      tmp_revnum = (*lastrange)->start;
                      (*lastrange)->start = mrange->start;
                      (*lastrange)->end = tmp_revnum;
                      (*lastrange)->inheritable = mrange->inheritable;

                      mrange->start = tmp_revnum;
                      mrange->end = pushed_mrange_2->start;
                      mrange->inheritable = TRUE;
                    }
                }
            }
        }
    }
  if (pushed_mrange_1)
    {
      APR_ARRAY_PUSH(revlist, svn_merge_range_t *) = pushed_mrange_1;
      *lastrange = pushed_mrange_1;
    }
  if (pushed_mrange_2)
    {
      APR_ARRAY_PUSH(revlist, svn_merge_range_t *) = pushed_mrange_2;
      *lastrange = pushed_mrange_2;
    }
}

/* Convert a single svn_merge_range_t * back into an svn_stringbuf_t *.  */
static svn_error_t *
range_to_stringbuf(svn_stringbuf_t **result, svn_merge_range_t *range,
                       apr_pool_t *pool)
{
  if (range->start == range->end - 1)
    *result = svn_stringbuf_createf(pool, "%ld%s", range->end,
                                    range->inheritable
                                    ? "" : SVN_MERGEINFO_NONINHERITABLE_STR);
  else
    *result = svn_stringbuf_createf(pool, "%ld-%ld%s", range->start + 1,
                                    range->end, range->inheritable
                                    ? "" : SVN_MERGEINFO_NONINHERITABLE_STR);
  return SVN_NO_ERROR;
}

/* Helper for svn_mergeinfo_parse() via parse_revlist().

  Similar to combine_with_lastrange() but enforces the some of the
  restrictions noted in svn_mergeinfo_parse() on otherwise grammatically
  correct rangelists, specifically the prohibitions on:

    1) Overlapping revision ranges

    2) Unordered revision ranges

  Returns an SVN_ERR_MERGEINFO_PARSE_ERROR error if any of these rules
  are violated.  The restriction on revision ranges with a start revision
  greater than or equal to its end revision is handled in parse_revlist().

  Unlike combine_with_lastrange() this function *always* considers
  inheritance, so only adjacent revision ranges with the same
  inheritability are ever combined. */
static svn_error_t *
combine_with_adjacent_lastrange(svn_merge_range_t **lastrange,
                                svn_merge_range_t *mrange,
                                svn_boolean_t dup_mrange,
                                apr_array_header_t *revlist,
                                apr_pool_t *pool)
{
  svn_merge_range_t *pushed_mrange = mrange;

  if (*lastrange)
    {
      svn_stringbuf_t *r1, *r2;

      if ((*lastrange)->start <= mrange->end
          && mrange->start <= (*lastrange)->end)
        {
          /* The ranges intersect. */
          SVN_ERR(range_to_stringbuf(&r1, *lastrange, pool));
          SVN_ERR(range_to_stringbuf(&r2, mrange, pool));

          /* svn_mergeinfo_parse promises to combine adjacent
             ranges, but not overlapping ranges. */
          if (mrange->start < (*lastrange)->end)
            {
              return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                                       _("Parsing of overlapping revision "
                                         "ranges '%s' and '%s' is not "
                                         "supported"), r1->data, r2->data);
            }
          else if ((*lastrange)->inheritable == mrange->inheritable)
            {
              /* Combine adjacent ranges with the same inheritability. */
              (*lastrange)->end = mrange->end;
              return SVN_NO_ERROR;
            }
        }
      else if ((*lastrange)->start > mrange->start)
        {
          SVN_ERR(range_to_stringbuf(&r1, *lastrange, pool));
          SVN_ERR(range_to_stringbuf(&r2, mrange, pool));
          return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                                   _("Unable to parse unordered revision "
                                     "ranges '%s' and '%s'"),
                                     r1->data, r2->data);
        }
    }

  if (dup_mrange)
    pushed_mrange = svn_merge_range_dup(mrange, pool);
  APR_ARRAY_PUSH(revlist, svn_merge_range_t *) = pushed_mrange;
  *lastrange = pushed_mrange;
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
parse_revlist(const char **input, const char *end,
              apr_array_header_t *revlist, const char *pathname,
              apr_pool_t *pool)
{
  const char *curr = *input;
  svn_merge_range_t *lastrange = NULL;

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
          SVN_ERR(combine_with_adjacent_lastrange(&lastrange, mrange, FALSE,
                                                  revlist, pool));
          *input = curr;
          return SVN_NO_ERROR;
        }
      else if (*curr == ',')
        {
          SVN_ERR(combine_with_adjacent_lastrange(&lastrange, mrange, FALSE,
                                                  revlist, pool));
          curr++;
        }
      else if (*curr == '*')
        {
          mrange->inheritable = FALSE;
          curr++;
          if (*curr == ',' || *curr == '\n' || curr == end)
            {
              SVN_ERR(combine_with_adjacent_lastrange(&lastrange, mrange,
                                                      FALSE, revlist, pool));
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
parse_revision_line(const char **input, const char *end, apr_hash_t *hash,
                    apr_pool_t *pool)
{
  svn_stringbuf_t *pathname;
  apr_array_header_t *revlist = apr_array_make(pool, 1,
                                               sizeof(svn_merge_range_t *));

  SVN_ERR(parse_pathname(input, end, &pathname, pool));

  if (*(*input) != ':')
    return svn_error_create(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                            _("Pathname not terminated by ':'"));

  *input = *input + 1;

  SVN_ERR(parse_revlist(input, end, revlist, pathname->data, pool));

  if (*input != end && *(*input) != '\n')
    return svn_error_createf(SVN_ERR_MERGEINFO_PARSE_ERROR, NULL,
                             _("Could not find end of line in range list line "
                               "in '%s'"), *input);

  if (*input != end)
    *input = *input + 1;

  qsort(revlist->elts, revlist->nelts, revlist->elt_size,
        svn_sort_compare_ranges);
  apr_hash_set(hash, pathname->data, APR_HASH_KEY_STRING, revlist);

  return SVN_NO_ERROR;
}

/* top -> revisionline (NEWLINE revisionline)*  */
static svn_error_t *
parse_top(const char **input, const char *end, apr_hash_t *hash,
          apr_pool_t *pool)
{
  while (*input < end)
    SVN_ERR(parse_revision_line(input, end, hash, pool));

  return SVN_NO_ERROR;
}

/* Parse mergeinfo.  */
svn_error_t *
svn_mergeinfo_parse(apr_hash_t **mergeinfo,
                    const char *input,
                    apr_pool_t *pool)
{
  *mergeinfo = apr_hash_make(pool);
  return parse_top(&input, input + strlen(input), *mergeinfo, pool);
}


/* Merge revision list RANGELIST into *MERGEINFO, doing some trivial
   attempts to combine ranges as we go. */
svn_error_t *
svn_rangelist_merge(apr_array_header_t **rangelist,
                    apr_array_header_t *changes,
                    apr_pool_t *pool)
{
  int i, j;
  svn_merge_range_t *lastrange = NULL;
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
          combine_with_lastrange(&lastrange, elt1, TRUE, output,
                                 FALSE, pool);
          i++;
          j++;
        }
      else if (res < 0)
        {
          combine_with_lastrange(&lastrange, elt1, TRUE, output,
                                 FALSE, pool);
          i++;
        }
      else
        {
          combine_with_lastrange(&lastrange, elt2, TRUE, output,
                                 FALSE, pool);
          j++;
        }
    }
  /* Copy back any remaining elements.
     Only one of these loops should end up running, if anything. */

  assert (!(i < (*rangelist)->nelts && j < changes->nelts));

  for (; i < (*rangelist)->nelts; i++)
    {
      svn_merge_range_t *elt = APR_ARRAY_IDX(*rangelist, i,
                                             svn_merge_range_t *);
      combine_with_lastrange(&lastrange, elt, TRUE, output,
                             FALSE, pool);
    }


  for (; j < changes->nelts; j++)
    {
      svn_merge_range_t *elt = APR_ARRAY_IDX(changes, j, svn_merge_range_t *);
      combine_with_lastrange(&lastrange, elt, TRUE, output,
                             FALSE, pool);
    }

  *rangelist = output;
  return SVN_NO_ERROR;
}

static svn_boolean_t
range_intersect(svn_merge_range_t *first, svn_merge_range_t *second,
                svn_boolean_t consider_inheritance)
{
  return (first->start + 1 <= second->end)
    && (second->start + 1 <= first->end)
    && (!consider_inheritance
        || (!(first->inheritable) == !(second->inheritable)));
}

static svn_boolean_t
range_contains(svn_merge_range_t *first, svn_merge_range_t *second,
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
  svn_merge_range_t *lastrange = NULL;
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
              combine_with_lastrange(&lastrange, elt1, TRUE, *output,
                                     consider_inheritance, pool);

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

              combine_with_lastrange(&lastrange, &tmp_range, TRUE,
                                     *output, consider_inheritance, pool);
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
                  combine_with_lastrange(&lastrange, &tmp_range, TRUE,
                                         *output, consider_inheritance, pool);
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
              if (do_remove && !(lastrange &&
                                 combine_ranges(&lastrange, lastrange, elt1,
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
          combine_with_lastrange(&lastrange, &wboardelt, TRUE, *output,
                                 consider_inheritance, pool);
          i++;
        }

      /* Copy any other remaining untouched whiteboard elements.  */
      for (; i < whiteboard->nelts; i++)
        {
          svn_merge_range_t *elt = APR_ARRAY_IDX(whiteboard, i,
                                                 svn_merge_range_t *);

          combine_with_lastrange(&lastrange, elt, TRUE, *output,
                                 consider_inheritance, pool);
        }
    }

  return SVN_NO_ERROR;
}


/* Expected to handle all the range overlap cases: non, partial, full */
svn_error_t *
svn_rangelist_intersect(apr_array_header_t **output,
                        apr_array_header_t *rangelist1,
                        apr_array_header_t *rangelist2,
                        apr_pool_t *pool)
{
  return rangelist_intersect_or_remove(output, rangelist1, rangelist2, FALSE,
                                       TRUE, pool);
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

/* Output deltas via *DELETED and *ADDED, which will never be @c NULL.

   The following diagrams illustrate some common range delta scenarios:

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
svn_error_t *
svn_rangelist_diff(apr_array_header_t **deleted, apr_array_header_t **added,
                   apr_array_header_t *from, apr_array_header_t *to,
                   svn_boolean_t consider_inheritance,
                   apr_pool_t *pool)
{
  /* The items that are present in from, but not in to, must have been
     deleted. */
  SVN_ERR(svn_rangelist_remove(deleted, to, from, consider_inheritance,
                               pool));
  /* The items that are present in to, but not in from, must have been
     added.  */
  SVN_ERR(svn_rangelist_remove(added, from, to, consider_inheritance, pool));
  return SVN_NO_ERROR;
}

apr_uint64_t
svn_rangelist_count_revs(apr_array_header_t *rangelist)
{
  apr_uint64_t nbr_revs = 0;
  int i;

  for (i = 0; i < rangelist->nelts; i++)
    {
      svn_merge_range_t *range = APR_ARRAY_IDX(rangelist, i,
                                               svn_merge_range_t *);
      nbr_revs += range->end - range->start;
    }

  return nbr_revs;
}

svn_error_t *
svn_rangelist_to_revs(apr_array_header_t **revs,
                      const apr_array_header_t *rangelist,
                      apr_pool_t *pool)
{
  int i;

  *revs = apr_array_make(pool, rangelist->nelts, sizeof(svn_revnum_t));

  for (i = 0; i < rangelist->nelts; i++)
    {
      svn_merge_range_t *range = APR_ARRAY_IDX(rangelist, i,
                                               svn_merge_range_t *);
      svn_revnum_t rev = range->start + 1;

      while (rev <= range->end)
        {
          APR_ARRAY_PUSH(*revs, svn_revnum_t) = rev;
          rev += 1;
        }
    }

  return SVN_NO_ERROR;
}

/* Record deletions and additions of entire range lists (by path
   presence), and delegate to svn_rangelist_diff() for delta
   calculations on a specific path. */
/* ### TODO: Merge implementation with
   ### libsvn_subr/sorts.c:svn_prop_diffs().  Factor out a generic
   ### hash diffing function for addition to APR's apr_hash.h API. */
static svn_error_t *
walk_mergeinfo_hash_for_diff(apr_hash_t *from, apr_hash_t *to,
                             apr_hash_t *deleted, apr_hash_t *added,
                             svn_boolean_t consider_inheritance,
                             apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  const void *key;
  void *val;
  const char *path;
  apr_array_header_t *from_rangelist, *to_rangelist;

  /* Handle path deletions and differences. */
  for (hi = apr_hash_first(pool, from); hi; hi = apr_hash_next(hi))
    {
      apr_hash_this(hi, &key, NULL, &val);
      path = key;
      from_rangelist = val;

      /* If the path is not present at all in the "to" hash, the
         entire "from" rangelist is a deletion.  Paths which are
         present in the "to" hash require closer scrutiny. */
      to_rangelist = apr_hash_get(to, path, APR_HASH_KEY_STRING);
      if (to_rangelist)
        {
          /* Record any deltas (additions or deletions). */
          apr_array_header_t *deleted_rangelist, *added_rangelist;
          svn_rangelist_diff(&deleted_rangelist, &added_rangelist,
                             from_rangelist, to_rangelist,
                             consider_inheritance, pool);
          if (deleted && deleted_rangelist->nelts > 0)
            apr_hash_set(deleted, apr_pstrdup(pool, path),
                         APR_HASH_KEY_STRING, deleted_rangelist);
          if (added && added_rangelist->nelts > 0)
            apr_hash_set(added, apr_pstrdup(pool, path),
                         APR_HASH_KEY_STRING, added_rangelist);
        }
      else if (deleted)
        apr_hash_set(deleted, apr_pstrdup(pool, path), APR_HASH_KEY_STRING,
                     svn_rangelist_dup(from_rangelist, pool));
    }

  /* Handle path additions. */
  if (!added)
    return SVN_NO_ERROR;

  for (hi = apr_hash_first(pool, to); hi; hi = apr_hash_next(hi))
    {
      apr_hash_this(hi, &key, NULL, &val);
      path = key;
      to_rangelist = val;

      /* If the path is not present in the "from" hash, the entire
         "to" rangelist is an addition. */
      if (apr_hash_get(from, path, APR_HASH_KEY_STRING) == NULL)
        apr_hash_set(added, apr_pstrdup(pool, path), APR_HASH_KEY_STRING,
                     svn_rangelist_dup(to_rangelist, pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo_diff(apr_hash_t **deleted, apr_hash_t **added,
                   apr_hash_t *from, apr_hash_t *to,
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
                                               consider_inheritance, pool));
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo__equals(svn_boolean_t *is_equal,
                      apr_hash_t *info1,
                      apr_hash_t *info2,
                      svn_boolean_t consider_inheritance,
                      apr_pool_t *pool)
{
  if (apr_hash_count(info1) == apr_hash_count(info2))
    {
      apr_hash_t *deleted, *added;
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
svn_mergeinfo_merge(apr_hash_t *mergeinfo, apr_hash_t *changes,
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
svn_mergeinfo_intersect(apr_hash_t **mergeinfo,
                        apr_hash_t *mergeinfo1,
                        apr_hash_t *mergeinfo2,
                        apr_pool_t *pool)
{
  apr_hash_index_t *hi;

  *mergeinfo = apr_hash_make(pool);

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
                                          rangelist, pool));
          if (rangelist->nelts > 0)
            apr_hash_set(*mergeinfo, path, APR_HASH_KEY_STRING, rangelist);
        }
    }
  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo_remove(apr_hash_t **mergeinfo, apr_hash_t *eraser,
                     apr_hash_t *whiteboard, apr_pool_t *pool)
{
  *mergeinfo = apr_hash_make(pool);
  SVN_ERR(walk_mergeinfo_hash_for_diff(whiteboard, eraser, *mergeinfo, NULL,
                                       TRUE, pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_rangelist_to_stringbuf(svn_stringbuf_t **output,
                           const apr_array_header_t *rangelist,
                           apr_pool_t *pool)
{
  *output = svn_stringbuf_create("", pool);

  if (rangelist->nelts > 0)
    {
      int i;
      svn_merge_range_t *range;
      svn_stringbuf_t *toappend;

      /* Handle the elements that need commas at the end.  */
      for (i = 0; i < rangelist->nelts - 1; i++)
        {
          range = APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *);
          SVN_ERR(range_to_stringbuf(&toappend, range, pool));
          svn_stringbuf_appendstr(*output, toappend);
          svn_stringbuf_appendcstr(*output, ",");
        }

      /* Now handle the last element, which needs no comma.  */
      range = APR_ARRAY_IDX(rangelist, i, svn_merge_range_t *);
      SVN_ERR(range_to_stringbuf(&toappend, range, pool));
      svn_stringbuf_appendstr(*output, toappend);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo_to_stringbuf(svn_stringbuf_t **output, apr_hash_t *input,
                           apr_pool_t *pool)
{
  *output = svn_stringbuf_create("", pool);

  if (apr_hash_count(input) > 0)
    {
      apr_array_header_t *sorted =
        svn_sort__hash(input, svn_sort_compare_items_as_paths, pool);
      svn_sort__item_t elt;
      svn_stringbuf_t *revlist, *combined;
      int i;

      /* Handle the elements that need newlines at the end.  */
      for (i = 0; i < sorted->nelts - 1; i++)
        {
          elt = APR_ARRAY_IDX(sorted, i, svn_sort__item_t);

          SVN_ERR(svn_rangelist_to_stringbuf(&revlist, elt.value, pool));
          combined = svn_stringbuf_createf(pool, "%s:%s\n", (char *) elt.key,
                                           revlist->data);
          svn_stringbuf_appendstr(*output, combined);
        }

      /* Now handle the last element, which is not newline terminated.  */
      elt = APR_ARRAY_IDX(sorted, i, svn_sort__item_t);

      SVN_ERR(svn_rangelist_to_stringbuf(&revlist, elt.value, pool));
      combined = svn_stringbuf_createf(pool, "%s:%s", (char *) elt.key,
                                       revlist->data);
      svn_stringbuf_appendstr(*output, combined);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_mergeinfo__to_string(svn_string_t **output, apr_hash_t *input,
                         apr_pool_t *pool)
{
  if (apr_hash_count(input) > 0)
    {
      svn_stringbuf_t *mergeinfo_buf;
      SVN_ERR(svn_mergeinfo_to_stringbuf(&mergeinfo_buf, input, pool));
      *output = svn_string_create_from_buf(mergeinfo_buf, pool);
    }
  else
    {
      *output = svn_string_create("", pool);
    }
  return SVN_NO_ERROR;
}

/* Perform an in-place sort of the rangelists in a mergeinfo hash.  */
svn_error_t*
svn_mergeinfo_sort(apr_hash_t *input, apr_pool_t *pool)
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

apr_hash_t *
svn_mergeinfo_dup(apr_hash_t *mergeinfo, apr_pool_t *pool)
{
  apr_hash_t *new_mergeinfo = apr_hash_make(pool);
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
svn_mergeinfo_inheritable(apr_hash_t **output,
                          apr_hash_t *mergeinfo,
                          const char *path,
                          svn_revnum_t start,
                          svn_revnum_t end,
                          apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  const void *key;
  apr_ssize_t keylen;
  void *rangelist;

  apr_hash_t *inheritable_mergeinfo = apr_hash_make(pool);
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
      apr_hash_set(inheritable_mergeinfo,
                   apr_pstrmemdup(pool, key, keylen), keylen,
                   inheritable_rangelist);
    }
  *output = inheritable_mergeinfo;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_rangelist_inheritable(apr_array_header_t **inheritable_rangelist,
                          apr_array_header_t *rangelist,
                          svn_revnum_t start,
                          svn_revnum_t end,
                          apr_pool_t *pool)
{
  *inheritable_rangelist = apr_array_make(pool, 1,
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
              if (range->inheritable)
                {
                  svn_merge_range_t *inheritable_range =
                    apr_palloc(pool, sizeof(*inheritable_range));
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
            apr_array_make(pool, 0, sizeof(svn_merge_range_t *));
          svn_merge_range_t *range = apr_palloc(pool, sizeof(*range));

          range->start = start;
          range->end = end;
          range->inheritable = FALSE;
          APR_ARRAY_PUSH(ranges_inheritable, svn_merge_range_t *) = range;

          if (rangelist->nelts)
            SVN_ERR(svn_rangelist_remove(inheritable_rangelist,
                                         ranges_inheritable,
                                         rangelist,
                                         TRUE,
                                         pool));
        }
    }
  return SVN_NO_ERROR;
}

svn_boolean_t
svn_mergeinfo__remove_empty_rangelists(apr_hash_t *mergeinfo,
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

svn_boolean_t
svn_range_compact(svn_merge_range_t **range_1,
                  svn_merge_range_t **range_2)
{
  svn_boolean_t is_compacted = FALSE;
  svn_boolean_t range_1_is_reversed, range_2_is_reversed;

  /* Wave the white flag if out preconditions are not met. */
  if (!*range_1
      || !*range_2
      || !SVN_IS_VALID_REVNUM(*range_1)
      || !SVN_IS_VALID_REVNUM(*range_2))
    return FALSE;

  /* "Normalize" the ranges so start <= end. */
  range_1_is_reversed = (*range_1)->start > (*range_1)->end ? TRUE : FALSE;
  range_2_is_reversed = (*range_2)->start > (*range_2)->end ? TRUE : FALSE;
  if (range_1_is_reversed)
    range_swap_endpoints(*range_1);
  if (range_2_is_reversed)
    range_swap_endpoints(*range_2);

  /* Do the ranges overlap?
     Range overlapping detection algorithm from
     http://c2.com/cgi-bin/wiki/fullSearch?TestIfDateRangesOverlap */
  if ((*range_1)->start <= (*range_2)->end
      && (*range_2)->start <= (*range_1)->end)
    {
      /* If the ranges are both in the same direction simply combine them. */
      if (range_1_is_reversed == range_2_is_reversed)
        {
          (*range_1)->start = MIN((*range_1)->start, (*range_2)->start);
          (*range_1)->end = MAX((*range_1)->end, (*range_2)->end);
          *range_2 = NULL;
        }
      else /* *Exactly* one of the ranges is a reverse range. */
        {
          svn_revnum_t range_tmp;

          if ((*range_1)->start == (*range_2)->start)
            {
              if ((*range_1)->end == (*range_2)->end)
                {
                  /* A range and its reverse just cancel each other out. */
                  *range_1 = *range_2 = NULL;
                }
              else
                {
                  (*range_1)->start = (*range_1)->end;
                  (*range_1)->end = (*range_2)->end;
                  *range_2 = NULL;
                  /* RANGE_2 is a superset of RANGE_1, the intersecting
                     portions of each range cancel each other out.
                     The resulting compacted range is stored in RANGE_1 so
                     it takes on the reversed property of  RANGE_2. */
                  range_1_is_reversed = range_2_is_reversed;
                }
            }
          else /* (*range_1)->start != (*range_2)->start) */
            {
              if ((*range_1)->end > (*range_2)->end)
                {
                  /* RANGE_1 is a superset of RANGE_2 */
                  if ((*range_1)->start < (*range_2)->start)
                    {
                      range_tmp = (*range_1)->end;
                      (*range_1)->end = (*range_2)->start;
                      (*range_2)->start = (*range_2)->end;
                      (*range_2)->end = range_tmp;
                      range_2_is_reversed = range_1_is_reversed;
                    }
                  else
                    {
                      range_tmp = (*range_1)->start;
                      (*range_1)->start = (*range_2)->end;
                      (*range_2)->end = range_tmp;
                    }
                }
              else if ((*range_1)->end < (*range_2)->end)
                {
                  /* RANGE_1 and RANGE_2 intersect. */
                  range_tmp = (*range_1)->end;
                  (*range_1)->end = (*range_2)->start;
                  (*range_2)->start = range_tmp;
                  (*range_2)->end = (*range_2)->end;
                }
              else /* (*range_1)->end == (*range_2)->end */
                {
                  /* RANGE_2 is a proper subset of RANGE_1. */
                  (*range_1)->end = (*range_2)->start;
                  *range_2 = NULL;
                }
            }
        }
      is_compacted = TRUE;
    } /* ranges overlap */

  /* Return compacted ranges to their original direction. */
  if (*range_1 && range_1_is_reversed)
    range_swap_endpoints(*range_1);
  if (*range_2 && range_2_is_reversed)
    range_swap_endpoints(*range_2);
  return is_compacted;
}

svn_merge_range_t *
svn_merge_range_dup(svn_merge_range_t *range, apr_pool_t *pool)
{
  svn_merge_range_t *new_range = apr_palloc(pool, sizeof(*new_range));
  memcpy(new_range, range, sizeof(*new_range));
  return new_range;
}

svn_boolean_t
svn_merge_range_contains_rev(svn_merge_range_t *range, svn_revnum_t rev)
{
  assert(SVN_IS_VALID_REVNUM(range->start));
  assert(SVN_IS_VALID_REVNUM(range->end));
  assert(range->start != range->end);

  if (range->start < range->end)
    return rev > range->start && rev <= range->end;
  else
    return rev > range->end && rev <= range->start;
}
