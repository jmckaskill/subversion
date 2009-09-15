/*
 * mergeinfo.c :  merge history functions for the libsvn_client library
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

#include <apr_pools.h>
#include <apr_strings.h>

#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_string.h"
#include "svn_opt.h"
#include "svn_error.h"
#include "svn_error_codes.h"
#include "svn_props.h"
#include "svn_mergeinfo.h"
#include "svn_sorts.h"
#include "svn_ra.h"
#include "svn_client.h"
#include "svn_hash.h"

#include "private/svn_mergeinfo_private.h"
#include "private/svn_wc_private.h"
#include "private/svn_ra_private.h"
#include "client.h"
#include "mergeinfo.h"
#include "svn_private_config.h"



svn_client__merge_path_t *
svn_client__merge_path_dup(const svn_client__merge_path_t *old,
                           apr_pool_t *pool)
{
  svn_client__merge_path_t *new = apr_pmemdup(pool, old, sizeof(*old));

  new->path = apr_pstrdup(pool, old->path);
  if (new->remaining_ranges)
    new->remaining_ranges = svn_rangelist_dup(old->remaining_ranges, pool);
  if (new->pre_merge_mergeinfo)
    new->pre_merge_mergeinfo = svn_mergeinfo_dup(old->pre_merge_mergeinfo,
                                                 pool);
  if (new->implicit_mergeinfo)
    new->implicit_mergeinfo = svn_mergeinfo_dup(old->implicit_mergeinfo,
                                                 pool);

  return new;
}

svn_error_t *
svn_client__parse_mergeinfo(svn_mergeinfo_t *mergeinfo,
                            svn_wc_context_t *wc_ctx,
                            const char *local_abspath,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  const svn_string_t *propval;

  /* ### Use svn_wc_prop_get() would actually be sufficient for now.
     ### DannyB thinks that later we'll need behavior more like
     ### svn_client__get_prop_from_wc(). */
  SVN_ERR(svn_wc_prop_get2(&propval, wc_ctx, local_abspath, SVN_PROP_MERGEINFO,
                           scratch_pool, scratch_pool));
  if (propval)
    return svn_error_return(
        svn_mergeinfo_parse(mergeinfo, propval->data, result_pool));
  else
    {
      *mergeinfo = NULL;
      return SVN_NO_ERROR;
    }
}

svn_error_t *
svn_client__record_wc_mergeinfo(const char *local_abspath,
                                svn_mergeinfo_t mergeinfo,
                                svn_client_ctx_t *ctx,
                                apr_pool_t *scratch_pool)
{
  svn_string_t *mergeinfo_str;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  /* Convert the mergeinfo (if any) into text for storage as a
     property value. */
  if (mergeinfo)
    {
      /* The WC will contain mergeinfo. */
      SVN_ERR(svn_mergeinfo_to_string(&mergeinfo_str, mergeinfo,
                                      scratch_pool));
    }
  else
    {
      mergeinfo_str = NULL;
    }

  /* Record the new mergeinfo in the WC. */
  /* ### Later, we'll want behavior more analogous to
     ### svn_client__get_prop_from_wc(). */
  SVN_ERR(svn_wc_prop_set4(ctx->wc_ctx, local_abspath, SVN_PROP_MERGEINFO,
                           mergeinfo_str, TRUE /* skip checks */, NULL, NULL,
                           scratch_pool));

  if (ctx->notify_func2)
    {
      ctx->notify_func2(ctx->notify_baton2,
                        svn_wc_create_notify(local_abspath,
                                             svn_wc_notify_merge_record_info,
                                             scratch_pool),
                        scratch_pool);
    }

  return SVN_NO_ERROR;
}

/*-----------------------------------------------------------------------*/

/*** Retrieving mergeinfo. ***/

svn_error_t *
svn_client__adjust_mergeinfo_source_paths(svn_mergeinfo_t adjusted_mergeinfo,
                                          const char *rel_path,
                                          svn_mergeinfo_t mergeinfo,
                                          apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  const char *path;
  apr_array_header_t *copied_rangelist;

  SVN_ERR_ASSERT(adjusted_mergeinfo);
  SVN_ERR_ASSERT(mergeinfo);

  for (hi = apr_hash_first(NULL, mergeinfo); hi; hi = apr_hash_next(hi))
    {
      const char *merge_source = svn_apr_hash_index_key(hi);
      apr_array_header_t *rangelist = svn_apr_hash_index_val(hi);

      /* Copy inherited mergeinfo into our output hash, adjusting the
         merge source as appropriate. */
      path = svn_path_join(merge_source, rel_path, pool);
      copied_rangelist = svn_rangelist_dup(rangelist, pool);
      apr_hash_set(adjusted_mergeinfo, path, APR_HASH_KEY_STRING,
                   copied_rangelist);
    }
  return SVN_NO_ERROR;
}


svn_error_t *
svn_client__get_wc_mergeinfo(svn_mergeinfo_t *mergeinfo,
                             svn_boolean_t *inherited,
                             svn_mergeinfo_inheritance_t inherit,
                             const svn_wc_entry_t *entry,
                             const char *wcpath,
                             const char *limit_path,
                             const char **walked_path,
                             svn_client_ctx_t *ctx,
                             apr_pool_t *pool)
{
  const char *walk_relpath = "";
  svn_mergeinfo_t wc_mergeinfo;
  svn_boolean_t switched;
  svn_revnum_t base_revision = entry->revision;
  const char *local_abspath;
  const char *limit_abspath;
  apr_pool_t *iterpool;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, wcpath, pool));
  if (limit_path)
    SVN_ERR(svn_dirent_get_absolute(&limit_abspath, limit_path, pool));
  else
    limit_abspath = NULL;

  iterpool = svn_pool_create(pool);
  while (TRUE)
    {
      svn_pool_clear(iterpool);

      /* Don't look for explicit mergeinfo on WCPATH if we are only
         interested in inherited mergeinfo. */
      if (inherit == svn_mergeinfo_nearest_ancestor)
        {
          wc_mergeinfo = NULL;
          inherit = svn_mergeinfo_inherited;
        }
      else
        {
          /* Look for mergeinfo on WCPATH.  If there isn't any and we want
             inherited mergeinfo, walk towards the root of the WC until we
             encounter either (a) an unversioned directory, or (b) mergeinfo.
             If we encounter (b), use that inherited mergeinfo as our
             baseline. */
          SVN_ERR(svn_client__parse_mergeinfo(&wc_mergeinfo, ctx->wc_ctx,
                                              local_abspath, pool, iterpool));
        }

      /* If WCPATH is switched, don't look any higher for inherited
         mergeinfo. */
      SVN_ERR(svn_wc__path_switched(&switched, ctx->wc_ctx, local_abspath,
                                    iterpool));
      if (switched)
        break;

      if (wc_mergeinfo == NULL &&
          inherit != svn_mergeinfo_explicit &&
          !svn_dirent_is_root(local_abspath, strlen(local_abspath)))
        {
          svn_error_t *err;
          svn_boolean_t is_wc_root;

          /* Don't look any higher than the limit path. */
          if (limit_abspath && strcmp(limit_abspath, local_abspath) == 0)
            break;
          
          /* If we've reached the root of the working copy don't look any
             higher. */
          SVN_ERR(svn_wc_is_wc_root2(&is_wc_root, ctx->wc_ctx,
                                     local_abspath, iterpool));
          if (is_wc_root)
            break;

          /* No explicit mergeinfo on this path.  Look higher up the
             directory tree while keeping track of what we've walked. */
          walk_relpath = svn_path_join(svn_dirent_basename(local_abspath,
                                                           iterpool),
                                       walk_relpath, pool);
          local_abspath = svn_dirent_dirname(local_abspath, pool);

          err = svn_wc__get_entry_versioned(&entry, ctx->wc_ctx,
                                            local_abspath,
                                            svn_node_unknown, FALSE, FALSE,
                                            pool, iterpool);
          if (err && err->apr_err == SVN_ERR_ENTRY_NOT_FOUND)
            {
              svn_error_clear(err);
              *inherited = FALSE;
              *mergeinfo = wc_mergeinfo;
              return SVN_NO_ERROR;
            }
          else if (err)
            return svn_error_return(err);

          /* Look in WCPATH's parents only if the parents share the same
             working revision. */
          if (base_revision < entry->cmt_rev
              || entry->revision < base_revision)
            break;

          /* We haven't yet risen above the root of the WC. */
          continue;
      }
      break;
    }

  svn_pool_destroy(iterpool);

  if (svn_path_is_empty(walk_relpath))
    {
      /* Mergeinfo is explicit. */
      *inherited = FALSE;
      *mergeinfo = wc_mergeinfo;
    }
  else
    {
      /* Mergeinfo may be inherited. */
      if (wc_mergeinfo)
        {
          *inherited = (wc_mergeinfo != NULL);
          *mergeinfo = apr_hash_make(pool);
          SVN_ERR(svn_client__adjust_mergeinfo_source_paths(*mergeinfo,
                                                            walk_relpath,
                                                            wc_mergeinfo,
                                                            pool));
        }
      else
        {
          *inherited = FALSE;
          *mergeinfo = NULL;
        }
    }

  if (walked_path)
    *walked_path = walk_relpath;

  /* Remove non-inheritable mergeinfo and paths mapped to empty ranges
     which may occur if WCPATH's mergeinfo is not explicit. */
  if (*inherited)
    {
      SVN_ERR(svn_mergeinfo_inheritable(mergeinfo, *mergeinfo, NULL,
              SVN_INVALID_REVNUM, SVN_INVALID_REVNUM, pool));
      svn_mergeinfo__remove_empty_rangelists(*mergeinfo, pool);
    }
  return SVN_NO_ERROR;
}

/* A baton for get_subtree_mergeinfo_walk_cb. */
struct get_mergeinfo_catalog_walk_baton
{
  /* Absolute WC target and its path relative to repository root. */
  const char *target_abspath;
  const char *target_repos_root;

  /* The mergeinfo catalog being built. */
  svn_mergeinfo_catalog_t *mergeinfo_catalog;

  svn_wc_context_t *wc_ctx;

  /* Pool in which to allocate additions to MERGEINFO_CATALOG.*/
  apr_pool_t *result_pool;
};

static svn_error_t *
get_subtree_mergeinfo_walk_cb(const char *local_abspath,
                              void *walk_baton,
                              apr_pool_t *scratch_pool)
{
  struct get_mergeinfo_catalog_walk_baton *wb = walk_baton;
  const svn_string_t *propval;

  SVN_ERR(svn_wc_prop_get2(&propval, wb->wc_ctx, local_abspath,
                           SVN_PROP_MERGEINFO, scratch_pool, scratch_pool));

  /* We already have the target path's explicit/inherited mergeinfo, but do
     add any subtree mergeinfo to the WB->MERGEINFO_CATALOG. */
  if (propval && strcmp(local_abspath, wb->target_abspath) != 0)
    {
      const char *key_path;
      svn_mergeinfo_t subtree_mergeinfo;

      SVN_ERR(svn_client__path_relative_to_root(&key_path, wb->wc_ctx,
                                                local_abspath,
                                                wb->target_repos_root, FALSE,
                                                NULL, wb->result_pool,
                                                scratch_pool));
      SVN_ERR(svn_mergeinfo_parse(&subtree_mergeinfo, propval->data,
                                  wb->result_pool));

      /* If the target had no explicit/inherited mergeinfo and this is the
         first subtree with mergeinfo found, then the catalog will still be
         NULL. */
      if (!(*wb->mergeinfo_catalog))
        *(wb->mergeinfo_catalog) = apr_hash_make(wb->result_pool);

      apr_hash_set(*(wb->mergeinfo_catalog), key_path,
                   APR_HASH_KEY_STRING, subtree_mergeinfo);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__get_wc_mergeinfo_catalog(svn_mergeinfo_catalog_t *mergeinfo_cat,
                                     svn_boolean_t *inherited,
                                     svn_boolean_t include_descendants,
                                     svn_mergeinfo_inheritance_t inherit,
                                     const char *wcpath,
                                     const char *limit_path,
                                     const char **walked_path,
                                     svn_client_ctx_t *ctx,
                                     apr_pool_t *result_pool,
                                     apr_pool_t *scratch_pool)
{
  const char *target_repos_rel_path;
  const char *local_abspath;
  svn_mergeinfo_t mergeinfo;
  struct get_mergeinfo_catalog_walk_baton wb;
  const char *repos_root;
  svn_node_kind_t kind;
  const svn_wc_entry_t *entry;

  *mergeinfo_cat = NULL;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, wcpath, scratch_pool));
  SVN_ERR(svn_wc__get_entry_versioned(&entry, ctx->wc_ctx, local_abspath,
                                      svn_node_unknown, FALSE, FALSE,
                                      result_pool, scratch_pool));
  SVN_ERR(svn_client__path_relative_to_root(&target_repos_rel_path,
                                            ctx->wc_ctx,
                                            local_abspath,
                                            entry->repos, FALSE,
                                            NULL, scratch_pool,
                                            scratch_pool));

  /* Get the mergeinfo for the WCPATH target and set *INHERITED and
     *WALKED_PATH. */
  SVN_ERR(svn_client__get_wc_mergeinfo(&mergeinfo, inherited, inherit,
                                       entry, local_abspath, limit_path,
                                       walked_path, ctx, result_pool));

  /* Add any explicit/inherited mergeinfo for WCPATH to *MERGEINFO_CAT. */
  if (mergeinfo)
    {
      *mergeinfo_cat = apr_hash_make(result_pool);
      apr_hash_set(*mergeinfo_cat,
                   apr_pstrdup(result_pool, target_repos_rel_path),
                   APR_HASH_KEY_STRING, mergeinfo);
    }

  /* If WCPATH is a directory and we want the subtree mergeinfo too, then
     get it. */
  SVN_ERR(svn_wc__node_get_kind(&kind, ctx->wc_ctx, local_abspath, FALSE,
                                scratch_pool));
  if (kind == svn_node_dir && include_descendants)
    {
      svn_opt_revision_t working_rev;
      
      working_rev.kind = svn_opt_revision_working;
      SVN_ERR(svn_client__get_repos_root(&repos_root, local_abspath,
                                         &working_rev, ctx,
                                         scratch_pool, scratch_pool));
      wb.target_abspath = local_abspath;
      wb.target_repos_root = repos_root;
      wb.mergeinfo_catalog = mergeinfo_cat;
      wb.wc_ctx = ctx->wc_ctx;
      wb.result_pool = result_pool;
      SVN_ERR(svn_wc__node_walk_children(ctx->wc_ctx, local_abspath, FALSE,
                                         get_subtree_mergeinfo_walk_cb, &wb,
                                         svn_depth_infinity, ctx->cancel_func,
                                         ctx->cancel_baton, scratch_pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__get_repos_mergeinfo(svn_ra_session_t *ra_session,
                                svn_mergeinfo_t *target_mergeinfo,
                                const char *rel_path,
                                svn_revnum_t rev,
                                svn_mergeinfo_inheritance_t inherit,
                                svn_boolean_t squelch_incapable,
                                apr_pool_t *pool)
{
  svn_mergeinfo_catalog_t tgt_mergeinfo_cat;

  SVN_ERR(svn_client__get_repos_mergeinfo_catalog(&tgt_mergeinfo_cat,
                                                  ra_session,
                                                  rel_path, rev, inherit,
                                                  squelch_incapable, FALSE,
                                                  pool, pool));

  if (tgt_mergeinfo_cat && apr_hash_count(tgt_mergeinfo_cat))
    {
      /* We asked only for the REL_PATH's mergeinfo, not any of its
         descendants.  So if there is anything in the catalog it is the
         mergeinfo for REL_PATH. */
      *target_mergeinfo =
        svn_apr_hash_index_val(apr_hash_first(pool, tgt_mergeinfo_cat));
      
    }
  else
    {
      *target_mergeinfo = NULL;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__get_repos_mergeinfo_catalog(
  svn_mergeinfo_catalog_t *target_mergeinfo_cat,
  svn_ra_session_t *ra_session,
  const char *rel_path,
  svn_revnum_t rev,
  svn_mergeinfo_inheritance_t inherit,
  svn_boolean_t squelch_incapable,
  svn_boolean_t include_descendants,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  svn_error_t *err;
  svn_mergeinfo_t repos_mergeinfo;
  const char *old_session_url;
  apr_array_header_t *rel_paths = apr_array_make(scratch_pool, 1,
                                                 sizeof(rel_path));

  APR_ARRAY_PUSH(rel_paths, const char *) = rel_path;

  /* Temporarily point the session at the root of the repository.

     ### BH: This is called from 'svn cp URL1 [URL2..] TOURL' and causes issue
             #3242. As far as I can tell this is the only place in this
             scenario that really needs access to the repository root instead
             of the common parent. If there is any way to handle this via the
             common parent, we should implement this here and we reduce the
             problems caused by issue #3242. */
  SVN_ERR(svn_client__ensure_ra_session_url(&old_session_url, ra_session,
                                            NULL, scratch_pool));

  /* Fetch the mergeinfo. */
  err = svn_ra_get_mergeinfo(ra_session, &repos_mergeinfo, rel_paths, rev,
                             inherit, include_descendants, result_pool);
  if (err)
    {
      if (squelch_incapable && err->apr_err == SVN_ERR_UNSUPPORTED_FEATURE)
        {
          svn_error_clear(err);
          repos_mergeinfo = NULL;
        }
      else
        return svn_error_return(err);
    }

  /* If we reparented the session, put it back where our caller had it. */
  if (old_session_url)
    SVN_ERR(svn_ra_reparent(ra_session, old_session_url, scratch_pool));

  /* Grab only the mergeinfo provided for REL_PATH. */
  if (repos_mergeinfo)
    *target_mergeinfo_cat = repos_mergeinfo;
  else
    *target_mergeinfo_cat = NULL;

  return SVN_NO_ERROR;
}


svn_error_t *
svn_client__get_wc_or_repos_mergeinfo(svn_mergeinfo_t *target_mergeinfo,
                                      svn_boolean_t *indirect,
                                      svn_boolean_t repos_only,
                                      svn_mergeinfo_inheritance_t inherit,
                                      svn_ra_session_t *ra_session,
                                      const char *target_wcpath,
                                      svn_client_ctx_t *ctx,
                                      apr_pool_t *pool)
{
  svn_mergeinfo_catalog_t tgt_mergeinfo_cat;

  SVN_ERR(svn_client__get_wc_or_repos_mergeinfo_catalog(&tgt_mergeinfo_cat,
                                                        indirect, FALSE,
                                                        repos_only,
                                                        inherit, ra_session,
                                                        target_wcpath, ctx,
                                                        pool, pool));
  if (tgt_mergeinfo_cat && apr_hash_count(tgt_mergeinfo_cat))
    {
      /* We asked only for the TARGET_WCPATH's mergeinfo, not any of its
         descendants.  So if there is anything in the catalog it is the
         mergeinfo for TARGET_WCPATH. */
      *target_mergeinfo =
        svn_apr_hash_index_val(apr_hash_first(pool, tgt_mergeinfo_cat));
      
    }
  else
    {
      *target_mergeinfo = NULL;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__get_wc_or_repos_mergeinfo_catalog(
  svn_mergeinfo_catalog_t *target_mergeinfo_catalog,
  svn_boolean_t *indirect,
  svn_boolean_t include_descendants,
  svn_boolean_t repos_only,
  svn_mergeinfo_inheritance_t inherit,
  svn_ra_session_t *ra_session,
  const char *target_wcpath,
  svn_client_ctx_t *ctx,
  apr_pool_t *result_pool,
  apr_pool_t *scratch_pool)
{
  const char *url;
  svn_revnum_t target_rev;
  const svn_wc_entry_t *entry;
  const char *local_abspath;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, target_wcpath,
                                  scratch_pool));

  SVN_ERR(svn_wc__get_entry_versioned(&entry, ctx->wc_ctx, local_abspath,
                                      svn_node_unknown, FALSE, FALSE,
                                      result_pool, scratch_pool));

  /* We may get an entry with abbreviated information from TARGET_WCPATH's
     parent if TARGET_WCPATH is missing.  These limited entries do not have
     a URL and without that we cannot get accurate mergeinfo for
     TARGET_WCPATH. */
  SVN_ERR(svn_client__entry_location(&url, &target_rev, ctx->wc_ctx,
                                     local_abspath, svn_opt_revision_working,
                                     scratch_pool, scratch_pool));

  if (repos_only)
    *target_mergeinfo_catalog = NULL;
  else
    SVN_ERR(svn_client__get_wc_mergeinfo_catalog(target_mergeinfo_catalog,
                                                 indirect,
                                                 include_descendants,
                                                 inherit, 
                                                 local_abspath,
                                                 NULL, NULL, ctx,
                                                 result_pool, scratch_pool));

  /* If there is no WC mergeinfo check the repository for inherited
     mergeinfo, unless TARGET_WCPATH is a local addition or has a
     local modification which has removed all of its pristine mergeinfo. */
  if (*target_mergeinfo_catalog == NULL)
    {
      /* No need to check the repos if this is a local addition. */
      if (entry->schedule != svn_wc_schedule_add)
        {
          apr_hash_t *original_props;

          /* Check to see if we have local modifications which removed all of
             TARGET_WCPATH's pristine mergeinfo.  If that is the case then
             TARGET_WCPATH effetively has no mergeinfo. */
          SVN_ERR(svn_wc_get_prop_diffs2(NULL, &original_props, ctx->wc_ctx,
                                         local_abspath, result_pool,
                                         scratch_pool));
          if (!apr_hash_get(original_props, SVN_PROP_MERGEINFO,
                            APR_HASH_KEY_STRING))
            {
              const char *repos_rel_path;

              if (ra_session == NULL)
                SVN_ERR(svn_client__open_ra_session_internal(&ra_session, url,
                                                             NULL, NULL,
                                                             FALSE, TRUE, ctx,
                                                             scratch_pool));

              SVN_ERR(svn_client__path_relative_to_root(&repos_rel_path,
                                                        ctx->wc_ctx, url,
                                                        entry->repos, FALSE,
                                                        ra_session,
                                                        result_pool,
                                                        scratch_pool));
              SVN_ERR(svn_client__get_repos_mergeinfo_catalog(
                target_mergeinfo_catalog, ra_session,
                repos_rel_path, target_rev, inherit,
                TRUE, FALSE, result_pool, scratch_pool));

              if (*target_mergeinfo_catalog
                  && apr_hash_get(*target_mergeinfo_catalog, repos_rel_path,
                                  APR_HASH_KEY_STRING))
                *indirect = TRUE;
            }
        }
    }
  return SVN_NO_ERROR;
}


svn_error_t *
svn_client__mergeinfo_from_segments(svn_mergeinfo_t *mergeinfo_p,
                                    apr_array_header_t *segments,
                                    apr_pool_t *pool)
{
  svn_mergeinfo_t mergeinfo = apr_hash_make(pool);
  int i;

  /* Translate location segments into merge sources and ranges. */
  for (i = 0; i < segments->nelts; i++)
    {
      svn_location_segment_t *segment =
        APR_ARRAY_IDX(segments, i, svn_location_segment_t *);
      apr_array_header_t *path_ranges;
      svn_merge_range_t *range;
      const char *source_path;

      /* No path segment?  Skip it. */
      if (! segment->path)
        continue;

      /* Prepend a leading slash to our path. */
      source_path = apr_pstrcat(pool, "/", segment->path, NULL);

      /* See if we already stored ranges for this path.  If not, make
         a new list.  */
      path_ranges = apr_hash_get(mergeinfo, source_path, APR_HASH_KEY_STRING);
      if (! path_ranges)
        path_ranges = apr_array_make(pool, 1, sizeof(range));

      /* Build a merge range, push it onto the list of ranges, and for
         good measure, (re)store it in the hash. */
      range = apr_pcalloc(pool, sizeof(*range));
      range->start = MAX(segment->range_start - 1, 0);
      range->end = segment->range_end;
      range->inheritable = TRUE;
      APR_ARRAY_PUSH(path_ranges, svn_merge_range_t *) = range;
      apr_hash_set(mergeinfo, source_path, APR_HASH_KEY_STRING, path_ranges);
    }

  *mergeinfo_p = mergeinfo;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__get_history_as_mergeinfo(svn_mergeinfo_t *mergeinfo_p,
                                     const char *path_or_url,
                                     const svn_opt_revision_t *peg_revision,
                                     svn_revnum_t range_youngest,
                                     svn_revnum_t range_oldest,
                                     svn_ra_session_t *ra_session,
                                     svn_client_ctx_t *ctx,
                                     apr_pool_t *pool)
{
  apr_array_header_t *segments;
  svn_revnum_t peg_revnum = SVN_INVALID_REVNUM;
  const char *url;
  apr_pool_t *sesspool = NULL;  /* only used for an RA session we open */
  svn_ra_session_t *session = ra_session;

  /* If PATH_OR_URL is a local path (not a URL), we need to transform
     it into a URL, open an RA session for it, and resolve the peg
     revision.  Note that if the local item is scheduled for addition
     as a copy of something else, we'll use its copyfrom data to query
     its history.  */
  if (!svn_path_is_url(path_or_url))
    SVN_ERR(svn_dirent_get_absolute(&path_or_url, path_or_url, pool));
  SVN_ERR(svn_client__derive_location(&url, &peg_revnum, path_or_url,
                                      peg_revision, session, ctx, pool, pool));

  if (session == NULL)
    {
      sesspool = svn_pool_create(pool);
      SVN_ERR(svn_client__open_ra_session_internal(&session, url, NULL, NULL,
                                                   FALSE, TRUE, ctx,
                                                   sesspool));
    }

  /* Fetch the location segments for our URL@PEG_REVNUM. */
  if (! SVN_IS_VALID_REVNUM(range_youngest))
    range_youngest = peg_revnum;
  if (! SVN_IS_VALID_REVNUM(range_oldest))
    range_oldest = 0;
  SVN_ERR(svn_client__repos_location_segments(&segments, session, "",
                                              peg_revnum, range_youngest,
                                              range_oldest, ctx, pool));

  SVN_ERR(svn_client__mergeinfo_from_segments(mergeinfo_p, segments, pool));

  /* If we opened an RA session, ensure its closure. */
  if (sesspool)
    svn_pool_destroy(sesspool);

  return SVN_NO_ERROR;
}


/*-----------------------------------------------------------------------*/

/*** Eliding mergeinfo. ***/

/* Given the mergeinfo (CHILD_MERGEINFO) for a path, and the
   mergeinfo of its nearest ancestor with mergeinfo (PARENT_MERGEINFO), compare
   CHILD_MERGEINFO to PARENT_MERGEINFO to see if the former elides to
   the latter, following the elision rules described in
   svn_client__elide_mergeinfo()'s docstring.  Set *ELIDES to whether
   or not CHILD_MERGEINFO is redundant.

   Note: This function assumes that PARENT_MERGEINFO is definitive;
   i.e. if it is NULL then the caller not only walked the entire WC
   looking for inherited mergeinfo, but queried the repository if none
   was found in the WC.  This is rather important since this function
   says empty mergeinfo should be elided if PARENT_MERGEINFO is NULL,
   and we don't want to do that unless we are *certain* that the empty
   mergeinfo on PATH isn't overriding anything.

   If PATH_SUFFIX and PARENT_MERGEINFO are not NULL append PATH_SUFFIX
   to each path in PARENT_MERGEINFO before performing the comparison. */
static svn_error_t *
should_elide_mergeinfo(svn_boolean_t *elides,
                       svn_mergeinfo_t parent_mergeinfo,
                       svn_mergeinfo_t child_mergeinfo,
                       const char *path_suffix,
                       apr_pool_t *pool)
{
  /* Easy out: No child mergeinfo to elide. */
  if (child_mergeinfo == NULL)
    {
      *elides = FALSE;
    }
  else if (apr_hash_count(child_mergeinfo) == 0)
    {
      /* Empty mergeinfo elides to empty mergeinfo or to "nothing",
         i.e. it isn't overriding any parent. Otherwise it doesn't
         elide. */
      if (!parent_mergeinfo || apr_hash_count(parent_mergeinfo) == 0)
        *elides = TRUE;
      else
        *elides = FALSE;
    }
  else if (!parent_mergeinfo || apr_hash_count(parent_mergeinfo) == 0)
    {
      /* Non-empty mergeinfo never elides to empty mergeinfo
         or no mergeinfo. */
      *elides = FALSE;
    }
  else
    {
      /* Both CHILD_MERGEINFO and PARENT_MERGEINFO are non-NULL and
         non-empty. */
      svn_mergeinfo_t path_tweaked_parent_mergeinfo;
      apr_pool_t *subpool = svn_pool_create(pool);

      path_tweaked_parent_mergeinfo = apr_hash_make(subpool);

      /* If we need to adjust the paths in PARENT_MERGEINFO do it now. */
      if (path_suffix)
        SVN_ERR(svn_client__adjust_mergeinfo_source_paths(
          path_tweaked_parent_mergeinfo,
          path_suffix, parent_mergeinfo, subpool));
      else
        path_tweaked_parent_mergeinfo = parent_mergeinfo;

      SVN_ERR(svn_mergeinfo__equals(elides,
                                    path_tweaked_parent_mergeinfo,
                                    child_mergeinfo, TRUE, subpool));
      svn_pool_destroy(subpool);
    }

  return SVN_NO_ERROR;
}

/* Helper for svn_client__elide_mergeinfo().

   Given a working copy PATH, its mergeinfo hash CHILD_MERGEINFO, and
   the mergeinfo of PATH's nearest ancestor PARENT_MERGEINFO, use
   should_elide_mergeinfo() to decide whether or not CHILD_MERGEINFO elides to
   PARENT_MERGEINFO; PATH_SUFFIX means the same as in that function.

   If elision does occur, then update the mergeinfo for PATH (which is
   the child) in the working copy via ADM_ACCESS appropriately.

   If CHILD_MERGEINFO is NULL, do nothing.

   Use SCRATCH_POOL for temporary allocations.
*/
static svn_error_t *
elide_mergeinfo(svn_mergeinfo_t parent_mergeinfo,
                svn_mergeinfo_t child_mergeinfo,
                const char *local_abspath,
                const char *path_suffix,
                svn_client_ctx_t *ctx,
                apr_pool_t *scratch_pool)
{
  svn_boolean_t elides;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(should_elide_mergeinfo(&elides,
                                 parent_mergeinfo, child_mergeinfo,
                                 path_suffix, scratch_pool));

  if (elides)
    {
      SVN_ERR(svn_wc_prop_set4(ctx->wc_ctx, local_abspath, SVN_PROP_MERGEINFO,
                               NULL, TRUE, NULL, NULL, scratch_pool));

      if (ctx->notify_func2)
        {
          svn_wc_notify_t *notify =
                svn_wc_create_notify(
                              svn_dirent_join_many(scratch_pool, local_abspath,
                                                   path_suffix, NULL),
                              svn_wc_notify_merge_record_info, scratch_pool);

          ctx->notify_func2(ctx->notify_baton2, notify, scratch_pool);
        }
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_client__elide_mergeinfo(const char *target_wcpath,
                            const char *wc_elision_limit_path,
                            const svn_wc_entry_t *entry,
                            svn_client_ctx_t *ctx,
                            apr_pool_t *pool)
{
  const char *target_abspath;

  SVN_ERR(svn_dirent_get_absolute(&target_abspath, target_wcpath, pool));

  /* Check for first easy out: We are already at the limit path. */
  if (!wc_elision_limit_path
      || strcmp(target_wcpath, wc_elision_limit_path) != 0)
    {
      svn_mergeinfo_t target_mergeinfo;
      svn_mergeinfo_t mergeinfo = NULL;
      svn_boolean_t inherited;
      const char *walk_path;

      /* Get the TARGET_WCPATH's explicit mergeinfo. */
      SVN_ERR(svn_client__get_wc_mergeinfo(&target_mergeinfo, &inherited,
                                           svn_mergeinfo_inherited,
                                           entry, target_wcpath,
                                           wc_elision_limit_path
                                             ? wc_elision_limit_path
                                             : NULL,
                                           &walk_path, ctx, pool));

     /* If TARGET_WCPATH has no explicit mergeinfo, there's nothing to
         elide, we're done. */
      if (inherited || target_mergeinfo == NULL)
        return SVN_NO_ERROR;

      /* Get TARGET_WCPATH's inherited mergeinfo from the WC. */
      SVN_ERR(svn_client__get_wc_mergeinfo(&mergeinfo, &inherited,
                                           svn_mergeinfo_nearest_ancestor,
                                           entry, target_wcpath,
                                           wc_elision_limit_path
                                             ? wc_elision_limit_path
                                             : NULL,
                                           &walk_path, ctx, pool));

      /* If TARGET_WCPATH inherited no mergeinfo from the WC and we are
         not limiting our search to the working copy then check if it
         inherits any from the repos. */
      if (!mergeinfo && !wc_elision_limit_path)
        {
          SVN_ERR(svn_client__get_wc_or_repos_mergeinfo
                  (&mergeinfo, &inherited, TRUE,
                   svn_mergeinfo_nearest_ancestor,
                   NULL, target_wcpath, ctx, pool));
        }

      /* If there is nowhere to elide TARGET_WCPATH's mergeinfo to and
         the elision is limited, then we are done.*/
      if (!mergeinfo && wc_elision_limit_path)
        return SVN_NO_ERROR;

      SVN_ERR(elide_mergeinfo(mergeinfo, target_mergeinfo, target_abspath,
                              NULL, ctx, pool));
    }
  return SVN_NO_ERROR;
}


/* If the server supports Merge Tracking, set *MERGEINFO to a hash
   mapping const char * root-relative source paths to an
   apr_array_header_t * list of svn_merge_range_t * revision ranges
   representing merge sources and corresponding revision ranges which
   have been merged into PATH_OR_URL as of PEG_REVISION, or NULL if
   there is no mergeinfo.  Set *REPOS_ROOT to the root URL of the
   repository associated with PATH_OR_URL (and to which the paths in
   *MERGEINFO are relative).  If the server does not support Merge Tracking,
   return an error with the code SVN_ERR_UNSUPPORTED_FEATURE.  Use
   POOL for allocation of all returned values.  */
static svn_error_t *
get_mergeinfo(svn_mergeinfo_t *mergeinfo,
              const char **repos_root,
              const char *path_or_url,
              const svn_opt_revision_t *peg_revision,
              svn_client_ctx_t *ctx,
              apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create(pool);
  svn_ra_session_t *ra_session;
  svn_revnum_t rev;

  if (svn_path_is_url(path_or_url))
    {
      const char *repos_rel_path;
      const char *local_abspath;

      SVN_ERR(svn_dirent_get_absolute(&local_abspath, "", subpool));
      SVN_ERR(svn_client__open_ra_session_internal(&ra_session, path_or_url,
                                                   NULL, NULL, FALSE,
                                                   TRUE, ctx, subpool));
      SVN_ERR(svn_client__get_revision_number(&rev, NULL, ctx->wc_ctx,
                                              local_abspath, ra_session,
                                              peg_revision, subpool));
      SVN_ERR(svn_ra_get_repos_root2(ra_session, repos_root, pool));
      SVN_ERR(svn_client__path_relative_to_root(&repos_rel_path, ctx->wc_ctx,
                                                path_or_url, *repos_root, FALSE,
                                                NULL, subpool, subpool));
      SVN_ERR(svn_client__get_repos_mergeinfo(ra_session, mergeinfo,
                                              repos_rel_path, rev,
                                              svn_mergeinfo_inherited, FALSE,
                                              pool));
    }
  else /* ! svn_path_is_url() */
    {
      const char *url;
      svn_boolean_t indirect;
      const char *local_abspath;

      SVN_ERR(svn_dirent_get_absolute(&local_abspath, path_or_url, subpool));

      /* Check server Merge Tracking capability. */
      SVN_ERR(svn_client__entry_location(&url, &rev, ctx->wc_ctx,
                                         local_abspath,
                                         svn_opt_revision_working, subpool,
                                         subpool));
      SVN_ERR(svn_client__open_ra_session_internal(&ra_session, url,
                                                   NULL, NULL, FALSE,
                                                   TRUE, ctx, subpool));
      SVN_ERR(svn_ra__assert_mergeinfo_capable_server(ra_session, path_or_url,
                                                      subpool));

      /* Acquire return values. */
      SVN_ERR(svn_client__get_repos_root(repos_root, local_abspath,
                                         peg_revision, ctx, pool, pool));
      SVN_ERR(svn_client__get_wc_or_repos_mergeinfo(mergeinfo,
                                                    &indirect, FALSE,
                                                    svn_mergeinfo_inherited,
                                                    NULL, path_or_url,
                                                    ctx, pool));
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/*** In-memory mergeinfo elision ***/

/* TODO(reint): Document. */
struct elide_mergeinfo_catalog_dir_baton {
  const char *inherited_mergeinfo_path;
  svn_mergeinfo_t mergeinfo_catalog;
};

/* The root doesn't have mergeinfo (unless it is actually one of the
   paths passed to svn_delta_path_driver, in which case the callback
   is called directly instead of this). */
static svn_error_t *
elide_mergeinfo_catalog_open_root(void *eb,
                                  svn_revnum_t base_revision,
                                  apr_pool_t *dir_pool,
                                  void **root_baton)
{
  struct elide_mergeinfo_catalog_dir_baton *b = apr_pcalloc(dir_pool,
                                                            sizeof(*b));
  b->mergeinfo_catalog = eb;
  *root_baton = b;
  return SVN_NO_ERROR;
}

/* Make a directory baton for PATH.  It should have the same
   inherited_mergeinfo_path as its parent... unless we just called
   elide_mergeinfo_catalog_cb on its parent with its path. */
static svn_error_t *
elide_mergeinfo_catalog_open_directory(const char *path,
                                       void *parent_baton,
                                       svn_revnum_t base_revision,
                                       apr_pool_t *dir_pool,
                                       void **child_baton)
{
  struct elide_mergeinfo_catalog_dir_baton *b, *pb = parent_baton;

  b = apr_pcalloc(dir_pool, sizeof(*b));
  b->mergeinfo_catalog = pb->mergeinfo_catalog;

  if (apr_hash_get(b->mergeinfo_catalog, path, APR_HASH_KEY_STRING))
    b->inherited_mergeinfo_path = apr_pstrdup(dir_pool, path);
  else
    b->inherited_mergeinfo_path = pb->inherited_mergeinfo_path;

  *child_baton = b;
  return SVN_NO_ERROR;
}

/* TODO(reint): Document. */
struct elide_mergeinfo_catalog_cb_baton {
  apr_array_header_t *elidable_paths;
  svn_mergeinfo_t mergeinfo_catalog;
  apr_pool_t *result_pool;
};

/* Implements svn_delta_path_driver_cb_func_t. */
static svn_error_t *
elide_mergeinfo_catalog_cb(void **dir_baton,
                           void *parent_baton,
                           void *callback_baton,
                           const char *path,
                           apr_pool_t *pool)
{
  struct elide_mergeinfo_catalog_cb_baton *cb = callback_baton;
  struct elide_mergeinfo_catalog_dir_baton *pb = parent_baton;
  const char *path_suffix;
  svn_boolean_t elides;

  /* pb == NULL would imply that there was an *empty* path in the
     paths given to the driver (which is different from "/"). */
  SVN_ERR_ASSERT(pb != NULL);

  /* We'll just act like everything is a file. */
  *dir_baton = NULL;

  /* Is there even any inherited mergeinfo to elide? */
  /* (Note that svn_delta_path_driver will call open_directory before
     the callback for the root (only).) */
  if (!pb->inherited_mergeinfo_path
      || strcmp(path, "/") == 0)
    return SVN_NO_ERROR;

  path_suffix = svn_dirent_is_child(pb->inherited_mergeinfo_path,
                                    path, NULL);
  SVN_ERR_ASSERT(path_suffix != NULL);

  SVN_ERR(should_elide_mergeinfo(&elides,
                                 apr_hash_get(cb->mergeinfo_catalog,
                                              pb->inherited_mergeinfo_path,
                                              APR_HASH_KEY_STRING),
                                 apr_hash_get(cb->mergeinfo_catalog,
                                              path,
                                              APR_HASH_KEY_STRING),
                                 path_suffix,
                                 pool));

  if (elides)
    APR_ARRAY_PUSH(cb->elidable_paths, const char *) =
      apr_pstrdup(cb->result_pool, path);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client__elide_mergeinfo_catalog(svn_mergeinfo_t mergeinfo_catalog,
                                    apr_pool_t *pool)
{
  apr_array_header_t *paths;
  apr_array_header_t *elidable_paths = apr_array_make(pool, 1,
                                                      sizeof(const char *));
  svn_delta_editor_t *editor = svn_delta_default_editor(pool);
  struct elide_mergeinfo_catalog_cb_baton cb = { 0 };
  int i;

  cb.elidable_paths = elidable_paths;
  cb.mergeinfo_catalog = mergeinfo_catalog;
  cb.result_pool = pool;

  editor->open_root = elide_mergeinfo_catalog_open_root;
  editor->open_directory = elide_mergeinfo_catalog_open_directory;

  /* Walk over the paths, and build up a list of elidable ones. */
  SVN_ERR(svn_hash_keys(&paths, mergeinfo_catalog, pool));
  SVN_ERR(svn_delta_path_driver(editor,
                                mergeinfo_catalog, /* as edit_baton */
                                SVN_INVALID_REVNUM,
                                paths,
                                elide_mergeinfo_catalog_cb,
                                &cb,
                                pool));

  /* Now remove the elidable paths from the catalog. */
  for (i = 0; i < elidable_paths->nelts; i++)
    {
      const char *path = APR_ARRAY_IDX(elidable_paths, i, const char *);
      apr_hash_set(mergeinfo_catalog, path, APR_HASH_KEY_STRING, NULL);
    }

  return SVN_NO_ERROR;
}


struct filter_log_entry_baton_t
{
  apr_array_header_t *rangelist;
  svn_log_entry_receiver_t log_receiver;
  void *log_receiver_baton;
  svn_client_ctx_t *ctx;
};

/* Implements the svn_log_entry_receiver_t interface.  BATON is a
   `struct filter_log_entry_baton_t *' */
static svn_error_t *
filter_log_entry_with_rangelist(void *baton,
                                svn_log_entry_t *log_entry,
                                apr_pool_t *pool)
{
  struct filter_log_entry_baton_t *fleb = baton;
  svn_merge_range_t *range;
  apr_array_header_t *intersection, *this_rangelist;

  if (fleb->ctx->cancel_func)
    SVN_ERR(fleb->ctx->cancel_func(fleb->ctx->cancel_baton));

  this_rangelist = apr_array_make(pool, 1, sizeof(svn_merge_range_t *));
  range = apr_pcalloc(pool, sizeof(*range));
  range->start = log_entry->revision - 1;
  range->end = log_entry->revision;
  range->inheritable = TRUE;
  APR_ARRAY_PUSH(this_rangelist, svn_merge_range_t *) = range;

  /* Don't consider inheritance yet, see if LOG_ENTRY->REVISION is
     fully or partially represented in BATON->RANGELIST. */
  SVN_ERR(svn_rangelist_intersect(&intersection, fleb->rangelist,
                                  this_rangelist, FALSE, pool));
  if (! (intersection && intersection->nelts))
    return SVN_NO_ERROR;
  
  SVN_ERR_ASSERT(intersection->nelts == 1);

  /* Ok, we know LOG_ENTRY->REVISION is represented in BATON->RANGELIST,
     but is it partially represented, i.e. is the corresponding range in
     BATON->RANGELIST non-inheritable?  Ask for the same intersection as
     above but consider inheritance this time, if the intersection is empty
     we know the range in BATON->RANGELIST in non-inheritable. */
  SVN_ERR(svn_rangelist_intersect(&intersection, fleb->rangelist,
                                  this_rangelist, TRUE, pool));
  log_entry->non_inheritable = !intersection->nelts;

  return fleb->log_receiver(fleb->log_receiver_baton, log_entry, pool);
}

static svn_error_t *
logs_for_mergeinfo_rangelist(const char *source_url,
                             apr_array_header_t *rangelist,
                             svn_boolean_t discover_changed_paths,
                             const apr_array_header_t *revprops,
                             svn_log_entry_receiver_t log_receiver,
                             void *log_receiver_baton,
                             svn_client_ctx_t *ctx,
                             apr_pool_t *pool)
{
  apr_array_header_t *target;
  svn_merge_range_t *oldest_range, *youngest_range;
  apr_array_header_t *revision_ranges;
  svn_opt_revision_t oldest_rev, youngest_rev;
  svn_opt_revision_range_t *range;
  struct filter_log_entry_baton_t fleb;

  if (! rangelist->nelts)
    return SVN_NO_ERROR;

  /* Sort the rangelist. */
  qsort(rangelist->elts, rangelist->nelts,
        rangelist->elt_size, svn_sort_compare_ranges);

  /* Build a single-member log target list using SOURCE_URL. */
  target = apr_array_make(pool, 1, sizeof(const char *));
  APR_ARRAY_PUSH(target, const char *) = source_url;

  /* Calculate and construct the bounds of our log request. */
  youngest_range = APR_ARRAY_IDX(rangelist, rangelist->nelts - 1,
                                 svn_merge_range_t *);
  youngest_rev.kind = svn_opt_revision_number;
  youngest_rev.value.number = youngest_range->end;
  oldest_range = APR_ARRAY_IDX(rangelist, 0, svn_merge_range_t *);
  oldest_rev.kind = svn_opt_revision_number;
  oldest_rev.value.number = oldest_range->start;

  /* Build the log filtering callback baton. */
  fleb.rangelist = rangelist;
  fleb.log_receiver = log_receiver;
  fleb.log_receiver_baton = log_receiver_baton;
  fleb.ctx = ctx;

  /* Drive the log. */
  revision_ranges = apr_array_make(pool, 1, sizeof(svn_opt_revision_range_t *));
  range = apr_pcalloc(pool, sizeof(*range));
  range->end = youngest_rev;
  range->start = oldest_rev;
  APR_ARRAY_PUSH(revision_ranges, svn_opt_revision_range_t *) = range;
  SVN_ERR(svn_client_log5(target, &youngest_rev, revision_ranges,
                          0, discover_changed_paths, FALSE, FALSE, revprops,
                          filter_log_entry_with_rangelist, &fleb, ctx, pool));

  /* Check for cancellation. */
  if (ctx->cancel_func)
    SVN_ERR(ctx->cancel_func(ctx->cancel_baton));

  return SVN_NO_ERROR;
}


/* Set URL and REVISION to the url and revision (of kind
   svn_opt_revision_number) which is associated with PATH_OR_URL at
   PEG_REVISION.  Use POOL for allocations.

   Implementation Note: sometimes this information can be found
   locally via the information in the 'entries' files, such as when
   PATH_OR_URL is a working copy path and PEG_REVISION is of kind
   svn_opt_revision_base.  At other times, this function needs to
   contact the repository, resolving revision keywords into real
   revision numbers and tracing node history to find the correct
   location.

   ### Can this be used elsewhere?  I was *sure* I'd find this same
   ### functionality elsewhere before writing this helper, but I
   ### didn't.  Seems like an operation that we'd be likely to do
   ### often, though.  -- cmpilato
*/
static svn_error_t *
location_from_path_and_rev(const char **url,
                           svn_opt_revision_t **revision,
                           const char *path_or_url,
                           const svn_opt_revision_t *peg_revision,
                           svn_client_ctx_t *ctx,
                           apr_pool_t *pool)
{
  svn_ra_session_t *ra_session;
  apr_pool_t *subpool = svn_pool_create(pool);
  svn_revnum_t rev;

  SVN_ERR(svn_client__ra_session_from_path(&ra_session, &rev, url,
                                           path_or_url,
                                           !svn_path_is_url(path_or_url)
                                             ? path_or_url
                                             : NULL,
                                           peg_revision, peg_revision,
                                           ctx, subpool));
  *url = apr_pstrdup(pool, *url);
  *revision = apr_pcalloc(pool, sizeof(**revision));
  (*revision)->kind = svn_opt_revision_number;
  (*revision)->value.number = rev;

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}


/*** Public APIs ***/

svn_error_t *
svn_client_mergeinfo_log_merged(const char *path_or_url,
                                const svn_opt_revision_t *peg_revision,
                                const char *merge_source_path_or_url,
                                const svn_opt_revision_t *src_peg_revision,
                                svn_log_entry_receiver_t log_receiver,
                                void *log_receiver_baton,
                                svn_boolean_t discover_changed_paths,
                                const apr_array_header_t *revprops,
                                svn_client_ctx_t *ctx,
                                apr_pool_t *pool)
{
  const char *repos_root, *log_target = NULL, *merge_source_url;
  svn_mergeinfo_t tgt_mergeinfo;
  svn_mergeinfo_t source_history;
  svn_mergeinfo_t merged;
  svn_mergeinfo_t mergeinfo_noninheritable;
  svn_mergeinfo_t tgt_inheritable_mergeinfo;
  svn_mergeinfo_t tgt_noninheritable_mergeinfo;
  apr_array_header_t *rangelist;
  svn_opt_revision_t *real_src_peg_revision;
  apr_hash_index_t *hi;
  svn_revnum_t youngest_rev = SVN_INVALID_REVNUM;
  
  /* Step 1: Ensure that we have a merge source URL to work with. */
  SVN_ERR(location_from_path_and_rev(&merge_source_url, &real_src_peg_revision,
                                     merge_source_path_or_url,
                                     src_peg_revision, ctx, pool));

  /* Step 2: We need the union of PATH_OR_URL@PEG_REVISION's mergeinfo
     and MERGE_SOURCE_URL's history.  It's not enough to do path
     matching, because renames in the history of MERGE_SOURCE_URL
     throw that all in a tizzy.  Of course, if there's no mergeinfo on
     the target, that vastly simplifies matters (we'll have nothing to
     do). */
  /* This get_mergeinfo() call doubles as a mergeinfo capabilities check. */
  SVN_ERR(get_mergeinfo(&tgt_mergeinfo, &repos_root, path_or_url,
                        peg_revision, ctx, pool));
  if (! tgt_mergeinfo)
    return SVN_NO_ERROR;
  SVN_ERR(svn_client__get_history_as_mergeinfo(&source_history,
                                               merge_source_url,
                                               real_src_peg_revision,
                                               SVN_INVALID_REVNUM,
                                               SVN_INVALID_REVNUM,
                                               NULL, ctx, pool));

  /* svn_client__get_history_as_mergeinfo() will give us mergeinfo with all
     inheritable ranges, since history has no concept of non-inheritability.
     TGT_MERGEINFO might have non-inheritable ranges however, indicating
     that a range is only partially merged.  We need to keep track of both! */

  /* Separate TGT_MERGEINFO into its inheritable and non-inheritable
     parts. */
  SVN_ERR(svn_mergeinfo_inheritable2(&tgt_inheritable_mergeinfo,
                                     tgt_mergeinfo, NULL,
                                     SVN_INVALID_REVNUM,
                                     SVN_INVALID_REVNUM,
                                     TRUE, pool, pool));
  SVN_ERR(svn_mergeinfo_inheritable2(&tgt_noninheritable_mergeinfo,
                                     tgt_mergeinfo, NULL,
                                     SVN_INVALID_REVNUM,
                                     SVN_INVALID_REVNUM,
                                     FALSE, pool, pool));

  /* Find the intersection of the non-inheritable part of TGT_MERGEINFO
     and SOURCE_HISTORY.  svn_mergeinfo_intersect2() won't consider
     non-inheritable and inheritable ranges intersecting unless we ignore
     inheritance, but in doing so the the resulting intersection has
     all inheritable ranges.  To get around this we set the inheritance
     on the result to all non-inheritable. */
  SVN_ERR(svn_mergeinfo_intersect2(&mergeinfo_noninheritable,
                                   tgt_noninheritable_mergeinfo,
                                   source_history, FALSE, pool, pool));
  svn_mergeinfo__set_inheritance(mergeinfo_noninheritable, FALSE, pool);

  /* Find the intersection of the inheritable part of TGT_MERGEINFO
     and SOURCE_HISTORY. */
  SVN_ERR(svn_mergeinfo_intersect2(&merged, tgt_inheritable_mergeinfo,
                                   source_history, FALSE, pool, pool));

  /* Merge the inheritable and non-inheritable intersections back together.
     This results in mergeinfo that describes both revisions that are fully
     merged as well as those that are only partially merged to PATH_OR_URL. */
  SVN_ERR(svn_mergeinfo_merge(merged, mergeinfo_noninheritable, pool));

  /* Step 3: Now, we iterate over the eligible paths/rangelists to
     find the youngest revision (and its associated path).  Because
     SOURCE_HISTORY had the property that a revision could appear in
     at most one mergeinfo path, that same property is true of
     MERGEINFO (which is a subset of SOURCE_HISTORY).  We'll use this
     information to bound a run of the logs of the source's history so
     we can filter out no-op merge revisions.  While here, we'll
     collapse our rangelists into a single one.  */
  rangelist = apr_array_make(pool, 64, sizeof(svn_merge_range_t *));
  for (hi = apr_hash_first(pool, merged); hi; hi = apr_hash_next(hi))
    {
      const char *key = svn_apr_hash_index_key(hi);
      apr_array_header_t *list = svn_apr_hash_index_val(hi);
      svn_merge_range_t *range;

      range = APR_ARRAY_IDX(list, list->nelts - 1, svn_merge_range_t *);
      if ((! SVN_IS_VALID_REVNUM(youngest_rev))
          || (range->end > youngest_rev))
        {
          youngest_rev = range->end;
          log_target = key;
        }
      SVN_ERR(svn_rangelist_merge(&rangelist, list, pool));
    }

  /* Nothing eligible?  Get outta here. */
  if (! rangelist->nelts)
    return SVN_NO_ERROR;

  /* Step 4: Finally, we run 'svn log' to drive our log receiver, but
     using a receiver filter to only allow revisions to pass through
     that are in our rangelist. */
  log_target = svn_path_url_add_component2(repos_root, log_target + 1, pool);
  SVN_ERR(logs_for_mergeinfo_rangelist(log_target, rangelist,
                                       discover_changed_paths, revprops,
                                       log_receiver, log_receiver_baton,
                                       ctx, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_client_mergeinfo_get_merged(apr_hash_t **mergeinfo_p,
                                const char *path_or_url,
                                const svn_opt_revision_t *peg_revision,
                                svn_client_ctx_t *ctx,
                                apr_pool_t *pool)
{
  const char *repos_root;
  apr_hash_t *full_path_mergeinfo;
  svn_mergeinfo_t mergeinfo;

  SVN_ERR(get_mergeinfo(&mergeinfo, &repos_root, path_or_url,
                        peg_revision, ctx, pool));

  /* Copy the MERGEINFO hash items into another hash, but change
     the relative paths into full URLs. */
  *mergeinfo_p = NULL;
  if (mergeinfo)
    {
      apr_hash_index_t *hi;

      full_path_mergeinfo = apr_hash_make(pool);
      for (hi = apr_hash_first(pool, mergeinfo); hi; hi = apr_hash_next(hi))
        {
          const char *key = svn_apr_hash_index_key(hi);
          void *val = svn_apr_hash_index_val(hi);
          const char *source_url;

          source_url = svn_path_uri_encode(key, pool);
          source_url = svn_path_join(repos_root, source_url + 1, pool);
          apr_hash_set(full_path_mergeinfo, source_url,
                       APR_HASH_KEY_STRING, val);
        }
      *mergeinfo_p = full_path_mergeinfo;
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_client_mergeinfo_log_eligible(const char *path_or_url,
                                  const svn_opt_revision_t *peg_revision,
                                  const char *merge_source_path_or_url,
                                  const svn_opt_revision_t *src_peg_revision,
                                  svn_log_entry_receiver_t log_receiver,
                                  void *log_receiver_baton,
                                  svn_boolean_t discover_changed_paths,
                                  const apr_array_header_t *revprops,
                                  svn_client_ctx_t *ctx,
                                  apr_pool_t *pool)
{
  svn_mergeinfo_t mergeinfo;
  svn_mergeinfo_t mergeinfo_noninheritable;
  svn_mergeinfo_t history;
  svn_mergeinfo_t source_history;
  svn_mergeinfo_t available;
  svn_mergeinfo_t available_noninheritable;
  apr_hash_index_t *hi;
  svn_ra_session_t *ra_session;
  svn_opt_revision_t *real_src_peg_revision;
  const char *repos_root, *merge_source_url;
  apr_pool_t *sesspool;
  svn_revnum_t youngest_rev = SVN_INVALID_REVNUM;
  apr_array_header_t *rangelist;
  const char *log_target = NULL;

  /* Step 1: Ensure that we have a merge source URL to work with. */
  SVN_ERR(location_from_path_and_rev(&merge_source_url, &real_src_peg_revision,
                                     merge_source_path_or_url,
                                     src_peg_revision, ctx, pool));

  /* Step 2: Across the set of possible merges, see what's already
     been merged into PATH_OR_URL@PEG_REVISION (or what's already part
     of the history it shares with that of MERGE_SOURCE_URL.  */
  /* This get_mergeinfo() call doubles as a mergeinfo capabilities check. */
  SVN_ERR(get_mergeinfo(&mergeinfo, &repos_root, path_or_url,
                        peg_revision, ctx, pool));
  SVN_ERR(svn_client__get_history_as_mergeinfo(&history,
                                               path_or_url,
                                               peg_revision,
                                               SVN_INVALID_REVNUM,
                                               SVN_INVALID_REVNUM,
                                               NULL, ctx, pool));
  if (! mergeinfo)
    mergeinfo = history;
  else
    svn_mergeinfo_merge(mergeinfo, history, pool);

  /* Step 3: See what merge sources can be derived from the history of
     MERGE_SOURCE_URL. */
  sesspool = svn_pool_create(pool);
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, merge_source_url,
                                               NULL, NULL, FALSE,
                                               TRUE, ctx, sesspool));
  SVN_ERR(svn_client__get_history_as_mergeinfo(&source_history,
                                               merge_source_url,
                                               real_src_peg_revision,
                                               SVN_INVALID_REVNUM,
                                               SVN_INVALID_REVNUM,
                                               ra_session, ctx, sesspool));

/* svn_client__get_history_as_mergeinfo() will give us mergeinfo with all
   inheritable ranges, since history has no concept of non-inheritability.
   MERGEINFO might have non-inheritable ranges however, indicating
   that a range is only partially merged.  We need to keep track of both! */

  /* Get the non-inheritable part of MERGEINFO. */
  SVN_ERR(svn_mergeinfo_inheritable2(&mergeinfo_noninheritable,
                                     mergeinfo, NULL,
                                     SVN_INVALID_REVNUM,
                                     SVN_INVALID_REVNUM,
                                     FALSE, pool, sesspool));

  /* Find the intersection of the non-inheritable part of MERGEINFO
     and SOURCE_HISTORY.  svn_mergeinfo_intersect2() won't consider
     non-inheritable and inheritable ranges intersecting unless we ignore
     inheritance, but in doing so the the resulting intersection has
     all inheritable ranges.  To get around this we set the inheritance on
     the result to all non-inheritable, leaving us with what has been
     partially merged to PATH_OR_URL*/
  SVN_ERR(svn_mergeinfo_intersect2(&available_noninheritable,
                                   mergeinfo_noninheritable,
                                   source_history,
                                   FALSE, pool, sesspool));
  svn_mergeinfo__set_inheritance(available_noninheritable, FALSE, pool);

  /* Find any part of SOURCE_HISTORY which has not been merged *at all*
     to PATH_OR_URL and then merge in the parts which are partially
     merged. */
  SVN_ERR(svn_mergeinfo_remove2(&available, mergeinfo, source_history,
                                FALSE, pool, sesspool));
  SVN_ERR(svn_mergeinfo_merge(available, available_noninheritable, pool));

  svn_pool_destroy(sesspool);

  /* Step 4: Now, we iterate over the eligible paths/rangelists to
     find the youngest revision (and its associated path).  Because
     SOURCE_HISTORY had the property that a revision could appear in
     at most one mergeinfo path, that same property is true of
     AVAILABLE (which is a subset of SOURCE_HISTORY).  We'll use this
     information to bound a run of the logs of the source's history so
     we can filter out no-op merge revisions.  While here, we'll
     collapse our rangelists into a single one.  */
  rangelist = apr_array_make(pool, 64, sizeof(svn_merge_range_t *));
  for (hi = apr_hash_first(pool, available); hi; hi = apr_hash_next(hi))
    {
      const char *key = svn_apr_hash_index_key(hi);
      apr_array_header_t *list = svn_apr_hash_index_val(hi);
      svn_merge_range_t *range;

      range = APR_ARRAY_IDX(list, list->nelts - 1, svn_merge_range_t *);
      if ((! SVN_IS_VALID_REVNUM(youngest_rev))
          || (range->end > youngest_rev))
        {
          youngest_rev = range->end;
          log_target = key;
        }
      SVN_ERR(svn_rangelist_merge(&rangelist, list, pool));
    }

  /* Nothing eligible?  Get outta here. */
  if (! rangelist->nelts)
    return SVN_NO_ERROR;

  /* Step 5: Finally, we run 'svn log' to drive our log receiver, but
     using a receiver filter to only allow revisions to pass through
     that are in our rangelist. */
  log_target = svn_path_url_add_component2(repos_root, log_target + 1, pool);
  SVN_ERR(logs_for_mergeinfo_rangelist(log_target, rangelist,
                                       discover_changed_paths, revprops,
                                       log_receiver, log_receiver_baton,
                                       ctx, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_client_suggest_merge_sources(apr_array_header_t **suggestions,
                                 const char *path_or_url,
                                 const svn_opt_revision_t *peg_revision,
                                 svn_client_ctx_t *ctx,
                                 apr_pool_t *pool)
{
  const char *repos_root;
  const char *copyfrom_path;
  apr_array_header_t *list;
  svn_revnum_t copyfrom_rev;
  svn_mergeinfo_t mergeinfo;
  apr_hash_index_t *hi;

  list = apr_array_make(pool, 1, sizeof(const char *));

  /* In our ideal algorithm, the list of recommendations should be
     ordered by:

        1. The most recent existing merge source.
        2. The copyfrom source (which will also be listed as a merge
           source if the copy was made with a 1.5+ client and server).
        3. All other merge sources, most recent to least recent.

     However, determining the order of application of merge sources
     requires a new RA API.  Until such an API is available, our
     algorithm will be:

        1. The copyfrom source.
        2. All remaining merge sources (unordered).
  */

  /* ### TODO: Share ra_session batons to improve efficiency? */
  SVN_ERR(get_mergeinfo(&mergeinfo, &repos_root, path_or_url,
                        peg_revision, ctx, pool));
  SVN_ERR(svn_client__get_copy_source(path_or_url, peg_revision,
                                      &copyfrom_path, &copyfrom_rev,
                                      ctx, pool));
  if (copyfrom_path)
    {
      APR_ARRAY_PUSH(list, const char *) =
        svn_path_url_add_component2(repos_root, copyfrom_path, pool);
    }

  if (mergeinfo)
    {
      for (hi = apr_hash_first(NULL, mergeinfo); hi; hi = apr_hash_next(hi))
        {
          const char *rel_path = svn_apr_hash_index_key(hi);

          if (copyfrom_path == NULL || strcmp(rel_path, copyfrom_path) != 0)
            APR_ARRAY_PUSH(list, const char *) = \
              svn_path_url_add_component2(repos_root, rel_path + 1, pool);
        }
    }

  *suggestions = list;
  return SVN_NO_ERROR;
}
