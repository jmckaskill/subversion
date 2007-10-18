/*
 * merge.c: merging
 *
 * ====================================================================
 * Copyright (c) 2000-2007 CollabNet.  All rights reserved.
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

/* ==================================================================== */



/*** Includes. ***/

#include <apr_strings.h>
#include <apr_tables.h>
#include <apr_hash.h>
#include "svn_types.h"
#include "svn_hash.h"
#include "svn_wc.h"
#include "svn_delta.h"
#include "svn_diff.h"
#include "svn_mergeinfo.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_io.h"
#include "svn_utf.h"
#include "svn_pools.h"
#include "svn_config.h"
#include "svn_props.h"
#include "svn_time.h"
#include "svn_sorts.h"
#include "client.h"
#include "mergeinfo.h"
#include <assert.h>

#include "private/svn_wc_private.h"
#include "private/svn_mergeinfo_private.h"

#include "svn_private_config.h"

/*-----------------------------------------------------------------------*/

/*** Utilities. ***/

/* Sanity check -- ensure that we have valid revisions to look at. */
#define ENSURE_VALID_REVISION_KINDS(rev1_kind, rev2_kind) \
  if ((rev1_kind == svn_opt_revision_unspecified) \
      || (rev2_kind == svn_opt_revision_unspecified)) \
    { \
      return svn_error_create \
        (SVN_ERR_CLIENT_BAD_REVISION, NULL, \
         _("Not all required revisions are specified")); \
    }


/* Return SVN_ERR_UNSUPPORTED_FEATURE if URL's scheme does not
   match the scheme of the url for ADM_ACCESS's path; return
   SVN_ERR_BAD_URL if no scheme can be found for one or both urls;
   otherwise return SVN_NO_ERROR.  Use ADM_ACCESS's pool for
   temporary allocation. */
static svn_error_t *
check_scheme_match(svn_wc_adm_access_t *adm_access, const char *url)
{
  const char *path = svn_wc_adm_access_path(adm_access);
  apr_pool_t *pool = svn_wc_adm_access_pool(adm_access);
  const svn_wc_entry_t *ent;
  const char *idx1, *idx2;

  SVN_ERR(svn_wc_entry(&ent, path, adm_access, TRUE, pool));

  idx1 = strchr(url, ':');
  idx2 = strchr(ent->url, ':');

  if ((idx1 == NULL) && (idx2 == NULL))
    {
      return svn_error_createf
        (SVN_ERR_BAD_URL, NULL,
         _("URLs have no scheme ('%s' and '%s')"), url, ent->url);
    }
  else if (idx1 == NULL)
    {
      return svn_error_createf
        (SVN_ERR_BAD_URL, NULL,
         _("URL has no scheme: '%s'"), url);
    }
  else if (idx2 == NULL)
    {
      return svn_error_createf
        (SVN_ERR_BAD_URL, NULL,
         _("URL has no scheme: '%s'"), ent->url);
    }
  else if (((idx1 - url) != (idx2 - ent->url))
           || (strncmp(url, ent->url, idx1 - url) != 0))
    {
      return svn_error_createf
        (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
         _("Access scheme mixtures not yet supported ('%s' and '%s')"),
         url, ent->url);
    }

  /* else */

  return SVN_NO_ERROR;
}

static void
get_hi_lo_revs_in_rangelist(apr_array_header_t *merge_sources,
                            svn_revnum_t *revision1,
                            svn_revnum_t *revision2)
{
  int i;
  *revision1 = *revision2 = SVN_INVALID_REVNUM;
  for(i = 0; i < merge_sources->nelts; i++)
    {
      svn_merge_range_t *r =
        APR_ARRAY_IDX(merge_sources, i, svn_merge_range_t *);
      if (*revision1 == SVN_INVALID_REVNUM)
        {
          *revision1 = MIN(r->start, r->end);
          *revision2 = MAX(r->start, r->end);
        }
      else
        {
          if (MIN(r->start, r->end) < *revision1)
            *revision1 = MIN(r->start, r->end);
          if (MAX(r->start, r->end) > *revision2)
            *revision2 = MAX(r->start, r->end);
        }
    }
}

/*-----------------------------------------------------------------------*/

/*** Callbacks for 'svn merge', invoked by the repos-diff editor. ***/


struct merge_cmd_baton {
  svn_boolean_t force;
  svn_boolean_t record_only;          /* Whether to only record mergeinfo. */
  svn_boolean_t dry_run;
  svn_boolean_t same_repos;           /* Whether the merge source repository
                                         is the same repository as the
                                         target.  Defaults to FALSE if DRY_RUN
                                         is TRUE.*/
  svn_boolean_t target_missing_child; /* Whether working copy target of the
                                         merge is missing any immediate
                                         children. */
  svn_boolean_t operative_merge;      /* Whether any changes were actually
                                         made as a result of this merge. */
  const char *added_path;             /* Set to the dir path whenever the
                                         dir is added as a child of a
                                         versioned dir (dry-run only) */
  const char *target;                 /* Working copy target of merge */
  const char *url;                    /* The second URL in the merge */
  const char *path;                   /* The wc path of the second target, this
                                         can be NULL if we don't have one. */
  const svn_opt_revision_t *revision; /* Revision of second URL in the merge */
  svn_client_ctx_t *ctx;              /* Client context for callbacks, etc. */

  /* Whether invocation of the merge_file_added() callback required
     delegation to the merge_file_changed() function for the file
     currently being merged.  This info is used to detect whether a
     file on the left side of a 3-way merge actually exists (important
     because it's created as an empty temp file on disk regardless).*/
  svn_boolean_t add_necessitated_merge;

  /* The list of paths for entries we've deleted, used only when in
     dry_run mode. */
  apr_hash_t *dry_run_deletions;

  /* The list of any paths which remained in conflict after a
     resolution attempt was made.  We track this in-memory, rather
     than just using WC entry state, since the latter doesn't help us
     when in dry_run mode. */
  apr_hash_t *conflicted_paths;

  /* The diff3_cmd in ctx->config, if any, else null.  We could just
     extract this as needed, but since more than one caller uses it,
     we just set it up when this baton is created. */
  const char *diff3_cmd;
  const apr_array_header_t *merge_options;

  /* RA sessions used throughout a merge operation.  Opened/re-parented
     as needed. */
  svn_ra_session_t *ra_session1;
  svn_ra_session_t *ra_session2;
  /* Flag indicating the fact target has everything merged already,
   * for the sake of children's merge to work it sets itself a dummy
   * merge range of requested_end_rev:requested_end_rev. */
  svn_boolean_t target_has_dummy_merge_range;

  apr_pool_t *pool;
};

apr_hash_t *
svn_client__dry_run_deletions(void *merge_cmd_baton)
{
  struct merge_cmd_baton *merge_b = merge_cmd_baton;
  return merge_b->dry_run_deletions;
}

/* Used to avoid spurious notifications (e.g. conflicts) from a merge
   attempt into an existing target which would have been deleted if we
   weren't in dry_run mode (issue #2584).  Assumes that WCPATH is
   still versioned (e.g. has an associated entry). */
static APR_INLINE svn_boolean_t
dry_run_deleted_p(struct merge_cmd_baton *merge_b, const char *wcpath)
{
  return (merge_b->dry_run &&
          apr_hash_get(merge_b->dry_run_deletions, wcpath,
                       APR_HASH_KEY_STRING) != NULL);
}

/* Return whether any WC path was put in conflict by the merge
   operation corresponding to MERGE_B. */
static APR_INLINE svn_boolean_t
is_path_conflicted_by_merge(struct merge_cmd_baton *merge_b)
{
  return (merge_b->conflicted_paths &&
          apr_hash_count(merge_b->conflicted_paths) > 0);
}

/* A svn_wc_diff_callbacks2_t function.  Used for both file and directory
   property merges. */
static svn_error_t *
merge_props_changed(svn_wc_adm_access_t *adm_access,
                    svn_wc_notify_state_t *state,
                    const char *path,
                    const apr_array_header_t *propchanges,
                    apr_hash_t *original_props,
                    void *baton)
{
  apr_array_header_t *props;
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create(merge_b->pool);
  svn_error_t *err;

  SVN_ERR(svn_categorize_props(propchanges, NULL, NULL, &props, subpool));

  /* We only want to merge "regular" version properties:  by
     definition, 'svn merge' shouldn't touch any data within .svn/  */
  if (props->nelts)
    {
      /* svn_wc_merge_props() requires ADM_ACCESS to be the access for
         the parent of PATH. Since the advent of merge tracking,
         discover_and_merge_children() may call this (indirectly) with
         the access for the merge_b->target instead (issue #2781).
         So, if we have the wrong access, get the right one. */
      if (svn_path_compare_paths(svn_wc_adm_access_path(adm_access),
                                 path) != 0)
        SVN_ERR(svn_wc_adm_probe_try3(&adm_access, adm_access, path,
                                      TRUE, -1, merge_b->ctx->cancel_func,
                                      merge_b->ctx->cancel_baton, subpool));

      err = svn_wc_merge_props(state, path, adm_access, original_props, props,
                               FALSE, merge_b->dry_run, subpool);
      if (err && (err->apr_err == SVN_ERR_ENTRY_NOT_FOUND
                  || err->apr_err == SVN_ERR_UNVERSIONED_RESOURCE))
        {
          /* if the entry doesn't exist in the wc, just 'skip' over
             this part of the tree-delta. */
          if (state)
            *state = svn_wc_notify_state_missing;
          svn_error_clear(err);
          svn_pool_destroy(subpool);
          return SVN_NO_ERROR;
        }
      else if (err)
        return err;
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* Contains any state collected while resolving conflicts. */
typedef struct
{
  /* The wrapped callback and baton. */
  svn_wc_conflict_resolver_func_t wrapped_func;
  void *wrapped_baton;

  /* The list of any paths which remained in conflict after a
     resolution attempt was made. */
  apr_hash_t **conflicted_paths;

  /* Pool used in notification_receiver() to avoid the iteration
     sub-pool which is passed in, then subsequently destroyed. */
  apr_pool_t *pool;
} conflict_resolver_baton_t;

/* An implementation of the svn_wc_conflict_resolver_func_t interface.
   We keep a record of paths which remain in conflict after any
   resolution attempt from BATON->wrapped_func. */
static svn_error_t *
conflict_resolver(svn_wc_conflict_result_t **result,
                  const svn_wc_conflict_description_t *description,
                  void *baton, apr_pool_t *pool)
{
  svn_error_t *err;
  conflict_resolver_baton_t *conflict_b = baton;

  if (conflict_b->wrapped_func)
    err = (*conflict_b->wrapped_func)(result, description,
                                      conflict_b->wrapped_baton, pool);
  else
    err = SVN_NO_ERROR;

  /* Keep a record of paths still in conflict after the resolution attempt. */
  if ((! conflict_b->wrapped_func)
      || (*result && ((*result)->choice == svn_wc_conflict_choose_postpone)))
    {
      const char *conflicted_path = apr_pstrdup(conflict_b->pool,
                                                description->path);

      if (*conflict_b->conflicted_paths == NULL)
        *conflict_b->conflicted_paths = apr_hash_make(conflict_b->pool);

      apr_hash_set(*conflict_b->conflicted_paths, conflicted_path,
                   APR_HASH_KEY_STRING, conflicted_path);
    }

  return err;
}

/* A svn_wc_diff_callbacks2_t function. */
static svn_error_t *
merge_file_changed(svn_wc_adm_access_t *adm_access,
                   svn_wc_notify_state_t *content_state,
                   svn_wc_notify_state_t *prop_state,
                   const char *mine,
                   const char *older,
                   const char *yours,
                   svn_revnum_t older_rev,
                   svn_revnum_t yours_rev,
                   const char *mimetype1,
                   const char *mimetype2,
                   const apr_array_header_t *prop_changes,
                   apr_hash_t *original_props,
                   void *baton)
{
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create(merge_b->pool);
  svn_boolean_t merge_required = TRUE;
  enum svn_wc_merge_outcome_t merge_outcome;

  /* Easy out:  no access baton means there ain't no merge target */
  if (adm_access == NULL)
    {
      if (content_state)
        *content_state = svn_wc_notify_state_missing;
      if (prop_state)
        *prop_state = svn_wc_notify_state_missing;
      svn_pool_destroy(subpool);
      return SVN_NO_ERROR;
    }

  /* Other easy outs:  if the merge target isn't under version
     control, or is just missing from disk, fogettaboutit.  There's no
     way svn_wc_merge3() can do the merge. */
  {
    const svn_wc_entry_t *entry;
    svn_node_kind_t kind;

    SVN_ERR(svn_wc_entry(&entry, mine, adm_access, FALSE, subpool));
    SVN_ERR(svn_io_check_path(mine, &kind, subpool));

    /* ### a future thought:  if the file is under version control,
       but the working file is missing, maybe we can 'restore' the
       working file from the text-base, and then allow the merge to run?  */

    if ((! entry) || (kind != svn_node_file))
      {
        if (content_state)
          *content_state = svn_wc_notify_state_missing;
        if (prop_state)
          *prop_state = svn_wc_notify_state_missing;
        svn_pool_destroy(subpool);
        return SVN_NO_ERROR;
      }
  }

  /* ### TODO: Thwart attempts to merge into a path that has
     ### unresolved conflicts.  This needs to be smart enough to deal
     ### with tree conflicts!
  if (is_path_conflicted_by_merge(merge_b, mine))
    {
      *content_state = svn_wc_notify_state_conflicted;
      return svn_error_createf(SVN_ERR_WC_FOUND_CONFLICT, NULL,
                               _("Path '%s' is in conflict, and must be "
                                 "resolved before the remainder of the "
                                 "requested merge can be applied"), mine);
    }
  */

  /* This callback is essentially no more than a wrapper around
     svn_wc_merge3().  Thank goodness that all the
     diff-editor-mechanisms are doing the hard work of getting the
     fulltexts! */

  /* Do property merge before text merge so that keyword expansion takes
     into account the new property values. */
  if (prop_changes->nelts > 0)
    SVN_ERR(merge_props_changed(adm_access, prop_state, mine, prop_changes,
                                original_props, baton));
  else
    if (prop_state)
      *prop_state = svn_wc_notify_state_unchanged;

  if (older)
    {
      svn_boolean_t has_local_mods;
      SVN_ERR(svn_wc_text_modified_p(&has_local_mods, mine, FALSE,
                                     adm_access, subpool));

      /* Special case:  if a binary file isn't locally modified, and is
         exactly identical to the 'left' side of the merge, then don't
         allow svn_wc_merge to produce a conflict.  Instead, just
         overwrite the working file with the 'right' side of the merge.

         Alternately, if the 'left' side of the merge doesn't exist in
         the repository, and the 'right' side of the merge is
         identical to the WC, pretend we did the merge (a no-op). */
      if ((! has_local_mods)
          && ((mimetype1 && svn_mime_type_is_binary(mimetype1))
              || (mimetype2 && svn_mime_type_is_binary(mimetype2))))
        {
          /* For adds, the 'left' side of the merge doesn't exist. */
          svn_boolean_t older_revision_exists =
              !merge_b->add_necessitated_merge;
          svn_boolean_t same_contents;
          SVN_ERR(svn_io_files_contents_same_p(&same_contents,
                                               (older_revision_exists ?
                                                older : yours),
                                               mine, subpool));
          if (same_contents)
            {
              if (older_revision_exists && !merge_b->dry_run)
                SVN_ERR(svn_io_file_rename(yours, mine, subpool));
              merge_outcome = svn_wc_merge_merged;
              merge_required = FALSE;
            }
        }

      if (merge_required)
        {
          /* xgettext: the '.working', '.merge-left.r%ld' and
             '.merge-right.r%ld' strings are used to tag onto a file
             name in case of a merge conflict */
          const char *target_label = _(".working");
          const char *left_label = apr_psprintf(subpool,
                                                _(".merge-left.r%ld"),
                                                older_rev);
          const char *right_label = apr_psprintf(subpool,
                                                 _(".merge-right.r%ld"),
                                                 yours_rev);
          conflict_resolver_baton_t conflict_baton =
            { merge_b->ctx->conflict_func, merge_b->ctx->conflict_baton,
              &merge_b->conflicted_paths, merge_b->pool };
          SVN_ERR(svn_wc_merge3(&merge_outcome,
                                older, yours, mine, adm_access,
                                left_label, right_label, target_label,
                                merge_b->dry_run, merge_b->diff3_cmd,
                                merge_b->merge_options, prop_changes,
                                conflict_resolver, &conflict_baton,
                                subpool));
        }

      if (content_state)
        {
          if (merge_outcome == svn_wc_merge_conflict)
            *content_state = svn_wc_notify_state_conflicted;
          else if (has_local_mods
                   && merge_outcome != svn_wc_merge_unchanged)
            *content_state = svn_wc_notify_state_merged;
          else if (merge_outcome == svn_wc_merge_merged)
            *content_state = svn_wc_notify_state_changed;
          else if (merge_outcome == svn_wc_merge_no_merge)
            *content_state = svn_wc_notify_state_missing;
          else /* merge_outcome == svn_wc_merge_unchanged */
            *content_state = svn_wc_notify_state_unchanged;
        }
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* A svn_wc_diff_callbacks2_t function. */
static svn_error_t *
merge_file_added(svn_wc_adm_access_t *adm_access,
                 svn_wc_notify_state_t *content_state,
                 svn_wc_notify_state_t *prop_state,
                 const char *mine,
                 const char *older,
                 const char *yours,
                 svn_revnum_t rev1,
                 svn_revnum_t rev2,
                 const char *mimetype1,
                 const char *mimetype2,
                 const apr_array_header_t *prop_changes,
                 apr_hash_t *original_props,
                 void *baton)
{
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create(merge_b->pool);
  svn_node_kind_t kind;
  const char *copyfrom_url;
  const char *child;
  int i;
  apr_hash_t *new_props;

  /* In most cases, we just leave prop_state as unknown, and let the
     content_state what happened, so we set prop_state here to avoid that
     below. */
  if (prop_state)
    *prop_state = svn_wc_notify_state_unknown;

  /* Apply the prop changes to a new hash table. */
  new_props = apr_hash_copy(subpool, original_props);
  for (i = 0; i < prop_changes->nelts; ++i)
    {
      const svn_prop_t *prop = &APR_ARRAY_IDX(prop_changes, i, svn_prop_t);
      apr_hash_set(new_props, prop->name, APR_HASH_KEY_STRING, prop->value);
    }

  /* Easy out:  if we have no adm_access for the parent directory,
     then this portion of the tree-delta "patch" must be inapplicable.
     Send a 'missing' state back;  the repos-diff editor should then
     send a 'skip' notification. */
  if (! adm_access)
    {
      if (merge_b->dry_run && merge_b->added_path
          && svn_path_is_child(merge_b->added_path, mine, subpool))
        {
          if (content_state)
            *content_state = svn_wc_notify_state_changed;
          if (prop_state && apr_hash_count(new_props))
            *prop_state = svn_wc_notify_state_changed;
        }
      else
        *content_state = svn_wc_notify_state_missing;
      svn_pool_destroy(subpool);
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_io_check_path(mine, &kind, subpool));
  switch (kind)
    {
    case svn_node_none:
      {
        const svn_wc_entry_t *entry;
        SVN_ERR(svn_wc_entry(&entry, mine, adm_access, FALSE, subpool));
        if (entry && entry->schedule != svn_wc_schedule_delete)
          {
            /* It's versioned but missing. */
            if (content_state)
              *content_state = svn_wc_notify_state_obstructed;
            svn_pool_destroy(subpool);
            return SVN_NO_ERROR;
          }
        if (! merge_b->dry_run)
          {
            child = svn_path_is_child(merge_b->target, mine, subpool);
            if (child != NULL)
              copyfrom_url = svn_path_url_add_component(merge_b->url, child,
                                                        subpool);
            else
              copyfrom_url = merge_b->url;
            SVN_ERR(check_scheme_match(adm_access, copyfrom_url));

            /* Since 'mine' doesn't exist, and this is
               'merge_file_added', I hope it's safe to assume that
               'older' is empty, and 'yours' is the full file.  Merely
               copying 'yours' to 'mine', isn't enough; we need to get
               the whole text-base and props installed too, just as if
               we had called 'svn cp wc wc'. */
            SVN_ERR(svn_wc_add_repos_file2(mine, adm_access, yours, NULL,
                                           new_props, NULL, copyfrom_url,
                                           rev2, subpool));
          }
        if (content_state)
          *content_state = svn_wc_notify_state_changed;
        if (prop_state && apr_hash_count(new_props))
          *prop_state = svn_wc_notify_state_changed;
      }
      break;
    case svn_node_dir:
      if (content_state)
        {
          /* directory already exists, is it under version control? */
          const svn_wc_entry_t *entry;
          SVN_ERR(svn_wc_entry(&entry, mine, adm_access, FALSE, subpool));

          if (entry && dry_run_deleted_p(merge_b, mine))
            *content_state = svn_wc_notify_state_changed;
          else
            /* this will make the repos_editor send a 'skipped' message */
            *content_state = svn_wc_notify_state_obstructed;
        }
      break;
    case svn_node_file:
      {
        /* file already exists, is it under version control? */
        const svn_wc_entry_t *entry;
        SVN_ERR(svn_wc_entry(&entry, mine, adm_access, FALSE, subpool));

        /* If it's an unversioned file, don't touch it.  If it's scheduled
           for deletion, then rm removed it from the working copy and the
           user must have recreated it, don't touch it */
        if (!entry || entry->schedule == svn_wc_schedule_delete)
          {
            /* this will make the repos_editor send a 'skipped' message */
            if (content_state)
              *content_state = svn_wc_notify_state_obstructed;
          }
        else
          {
            if (dry_run_deleted_p(merge_b, mine))
              {
                if (content_state)
                  *content_state = svn_wc_notify_state_changed;
              }
            else
              {
                /* Indicate that we merge because of an add to handle a
                   special case for binary files with no local mods. */
                  merge_b->add_necessitated_merge = TRUE;

                  SVN_ERR(merge_file_changed(adm_access, content_state,
                                             prop_state, mine, older, yours,
                                             rev1, rev2,
                                             mimetype1, mimetype2,
                                             prop_changes, original_props,
                                             baton));

                /* Reset the state so that the baton can safely be reused
                   in subsequent ops occurring during this merge. */
                  merge_b->add_necessitated_merge = FALSE;
              }
          }
        break;
      }
    default:
      if (content_state)
        *content_state = svn_wc_notify_state_unknown;
      break;
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* A svn_wc_diff_callbacks2_t function. */
static svn_error_t *
merge_file_deleted(svn_wc_adm_access_t *adm_access,
                   svn_wc_notify_state_t *state,
                   const char *mine,
                   const char *older,
                   const char *yours,
                   const char *mimetype1,
                   const char *mimetype2,
                   apr_hash_t *original_props,
                   void *baton)
{
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create(merge_b->pool);
  svn_node_kind_t kind;
  svn_wc_adm_access_t *parent_access;
  const char *parent_path;
  svn_error_t *err;

  /* Easy out:  if we have no adm_access for the parent directory,
     then this portion of the tree-delta "patch" must be inapplicable.
     Send a 'missing' state back;  the repos-diff editor should then
     send a 'skip' notification. */
  if (! adm_access)
    {
      if (state)
        *state = svn_wc_notify_state_missing;
      svn_pool_destroy(subpool);
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_io_check_path(mine, &kind, subpool));
  switch (kind)
    {
    case svn_node_file:
      svn_path_split(mine, &parent_path, NULL, subpool);
      SVN_ERR(svn_wc_adm_retrieve(&parent_access, adm_access, parent_path,
                                  subpool));
      /* Passing NULL for the notify_func and notify_baton because
         repos_diff.c:delete_entry() will do it for us. */
      err = svn_client__wc_delete(mine, parent_access, merge_b->force,
                                  merge_b->dry_run, FALSE, NULL, NULL,
                                  merge_b->ctx, subpool);
      if (err && state)
        {
          *state = svn_wc_notify_state_obstructed;
          svn_error_clear(err);
        }
      else if (state)
        {
          *state = svn_wc_notify_state_changed;
        }
      break;
    case svn_node_dir:
      if (state)
        *state = svn_wc_notify_state_obstructed;
      break;
    case svn_node_none:
      /* file is already non-existent, this is a no-op. */
      if (state)
        *state = svn_wc_notify_state_missing;
      break;
    default:
      if (state)
        *state = svn_wc_notify_state_unknown;
      break;
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* A svn_wc_diff_callbacks2_t function. */
static svn_error_t *
merge_dir_added(svn_wc_adm_access_t *adm_access,
                svn_wc_notify_state_t *state,
                const char *path,
                svn_revnum_t rev,
                void *baton)
{
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create(merge_b->pool);
  svn_node_kind_t kind;
  const svn_wc_entry_t *entry;
  const char *copyfrom_url, *child;

  /* Easy out:  if we have no adm_access for the parent directory,
     then this portion of the tree-delta "patch" must be inapplicable.
     Send a 'missing' state back;  the repos-diff editor should then
     send a 'skip' notification. */
  if (! adm_access)
    {
      if (state)
        {
          if (merge_b->dry_run && merge_b->added_path
              && svn_path_is_child(merge_b->added_path, path, subpool))
            *state = svn_wc_notify_state_changed;
          else
            *state = svn_wc_notify_state_missing;
        }
      svn_pool_destroy(subpool);
      return SVN_NO_ERROR;
    }

  child = svn_path_is_child(merge_b->target, path, subpool);
  assert(child != NULL);
  copyfrom_url = svn_path_url_add_component(merge_b->url, child, subpool);
  SVN_ERR(check_scheme_match(adm_access, copyfrom_url));

  SVN_ERR(svn_io_check_path(path, &kind, subpool));
  switch (kind)
    {
    case svn_node_none:
      SVN_ERR(svn_wc_entry(&entry, path, adm_access, FALSE, subpool));
      if (entry && entry->schedule != svn_wc_schedule_delete)
        {
          /* Versioned but missing */
          if (state)
            *state = svn_wc_notify_state_obstructed;
          svn_pool_destroy(subpool);
          return SVN_NO_ERROR;
        }
      if (merge_b->dry_run)
        merge_b->added_path = apr_pstrdup(merge_b->pool, path);
      else
        {
          SVN_ERR(svn_io_make_dir_recursively(path, subpool));
          SVN_ERR(svn_wc_add2(path, adm_access,
                              copyfrom_url, rev,
                              merge_b->ctx->cancel_func,
                              merge_b->ctx->cancel_baton,
                              NULL, NULL, /* don't pass notification func! */
                              subpool));

        }
      if (state)
        *state = svn_wc_notify_state_changed;
      break;
    case svn_node_dir:
      /* Adding an unversioned directory doesn't destroy data */
      SVN_ERR(svn_wc_entry(&entry, path, adm_access, TRUE, subpool));
      if (! entry || entry->schedule == svn_wc_schedule_delete)
        {
          if (!merge_b->dry_run)
            SVN_ERR(svn_wc_add2(path, adm_access,
                                copyfrom_url, rev,
                                merge_b->ctx->cancel_func,
                                merge_b->ctx->cancel_baton,
                                NULL, NULL, /* no notification func! */
                                subpool));
          if (merge_b->dry_run)
            merge_b->added_path = apr_pstrdup(merge_b->pool, path);
          if (state)
            *state = svn_wc_notify_state_changed;
        }
      else if (state)
        {
          if (dry_run_deleted_p(merge_b, path))
            *state = svn_wc_notify_state_changed;
          else
            *state = svn_wc_notify_state_obstructed;
        }
      break;
    case svn_node_file:
      if (merge_b->dry_run)
        merge_b->added_path = NULL;

      if (state)
        {
          SVN_ERR(svn_wc_entry(&entry, path, adm_access, FALSE, subpool));

          if (entry && dry_run_deleted_p(merge_b, path))
            /* ### TODO: Retain record of this dir being added to
               ### avoid problems from subsequent edits which try to
               ### add children. */
            *state = svn_wc_notify_state_changed;
          else
            *state = svn_wc_notify_state_obstructed;
        }
      break;
    default:
      if (merge_b->dry_run)
        merge_b->added_path = NULL;
      if (state)
        *state = svn_wc_notify_state_unknown;
      break;
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* Struct used for as the baton for calling merge_delete_notify_func(). */
typedef struct merge_delete_notify_baton_t
{
  svn_client_ctx_t *ctx;

  /* path to skip */
  const char *path_skip;
} merge_delete_notify_baton_t;

/* Notify callback function that wraps the normal callback
 * function to remove a notification that will be sent twice
 * and set the proper action. */
static void
merge_delete_notify_func(void *baton,
                         const svn_wc_notify_t *notify,
                         apr_pool_t *pool)
{
  merge_delete_notify_baton_t *mdb = baton;
  svn_wc_notify_t *new_notify;

  /* Skip the notification for the path we called svn_client__wc_delete() with,
   * because it will be outputed by repos_diff.c:delete_item */
  if (strcmp(notify->path, mdb->path_skip) == 0)
    return;

  /* svn_client__wc_delete() is written primarily for scheduling operations not
   * update operations.  Since merges are update operations we need to alter
   * the delete notification to show as an update not a schedule so alter
   * the action. */
  if (notify->action == svn_wc_notify_delete)
    {
      /* We need to copy it since notify is const. */
      new_notify = svn_wc_dup_notify(notify, pool);
      new_notify->action = svn_wc_notify_update_delete;
      notify = new_notify;
    }

  if (mdb->ctx->notify_func2)
    (*mdb->ctx->notify_func2)(mdb->ctx->notify_baton2, notify, pool);
}

/* A svn_wc_diff_callbacks2_t function. */
static svn_error_t *
merge_dir_deleted(svn_wc_adm_access_t *adm_access,
                  svn_wc_notify_state_t *state,
                  const char *path,
                  void *baton)
{
  struct merge_cmd_baton *merge_b = baton;
  apr_pool_t *subpool = svn_pool_create(merge_b->pool);
  svn_node_kind_t kind;
  svn_wc_adm_access_t *parent_access;
  const char *parent_path;
  svn_error_t *err;

  /* Easy out:  if we have no adm_access for the parent directory,
     then this portion of the tree-delta "patch" must be inapplicable.
     Send a 'missing' state back;  the repos-diff editor should then
     send a 'skip' notification. */
  if (! adm_access)
    {
      if (state)
        *state = svn_wc_notify_state_missing;
      svn_pool_destroy(subpool);
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_io_check_path(path, &kind, subpool));
  switch (kind)
    {
    case svn_node_dir:
      {
        merge_delete_notify_baton_t mdb;

        mdb.ctx = merge_b->ctx;
        mdb.path_skip = path;

        svn_path_split(path, &parent_path, NULL, subpool);
        SVN_ERR(svn_wc_adm_retrieve(&parent_access, adm_access, parent_path,
                                    subpool));
        err = svn_client__wc_delete(path, parent_access, merge_b->force,
                                    merge_b->dry_run, FALSE,
                                    merge_delete_notify_func, &mdb,
                                    merge_b->ctx, subpool);
        if (err && state)
          {
            *state = svn_wc_notify_state_obstructed;
            svn_error_clear(err);
          }
        else if (state)
          {
            *state = svn_wc_notify_state_changed;
          }
      }
      break;
    case svn_node_file:
      if (state)
        *state = svn_wc_notify_state_obstructed;
      break;
    case svn_node_none:
      /* dir is already non-existent, this is a no-op. */
      if (state)
        *state = svn_wc_notify_state_missing;
      break;
    default:
      if (state)
        *state = svn_wc_notify_state_unknown;
      break;
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* The main callback table for 'svn merge'.  */
static const svn_wc_diff_callbacks2_t
merge_callbacks =
  {
    merge_file_changed,
    merge_file_added,
    merge_file_deleted,
    merge_dir_added,
    merge_dir_deleted,
    merge_props_changed
  };


/*-----------------------------------------------------------------------*/

/*** Doing the actual merging. ***/

/* *UNREFINED_RANGES is an array of svn_merge_range_t *.  For each
   RANGE in *UNREFINED_RANGES find any merged revision ranges that
   the merge history for the merge source SRC_URL (between RANGE->start
   and RANGE->end) has recorded for the merge target ENTRY.  Get the
   mergeinfo for the source, then get the rangelist for the target (ENTRY)
   from that mergeinfo, subtract it from the RANGE and record the
   result in *REQUESTED_RANGELIST. */
static svn_error_t *
calculate_requested_ranges(apr_array_header_t **requested_rangelist,
                           apr_array_header_t *unrefined_ranges,
                           const char *src_url, const svn_wc_entry_t *entry,
                           svn_ra_session_t *ra_session,
                           svn_client_ctx_t *ctx, apr_pool_t *pool)
{
  apr_array_header_t *src_rangelist_for_tgt = NULL;
  apr_hash_t *added_mergeinfo, *deleted_mergeinfo,
    *start_mergeinfo, *end_mergeinfo;
  svn_revnum_t min_rev, max_rev;
  const char *repos_rel_path;
  int i;
  apr_pool_t *iterpool;

  SVN_ERR(svn_client__path_relative_to_root(&repos_rel_path, src_url,
                                            entry->repos, ra_session,
                                            NULL, pool));
  *requested_rangelist = apr_array_make(pool, 1, sizeof(svn_merge_range_t *));
  iterpool = svn_pool_create(pool);
  for (i = 0; i < unrefined_ranges->nelts; i++)
    {
      svn_merge_range_t *unrefined_range =
        APR_ARRAY_IDX(unrefined_ranges, i, svn_merge_range_t *);

      svn_pool_clear(iterpool);

      /* Find any mergeinfo added in RANGE. */
      min_rev = MIN(unrefined_range->start, unrefined_range->end);
      SVN_ERR(svn_client__get_repos_mergeinfo(ra_session, &start_mergeinfo,
                                              repos_rel_path, min_rev,
                                              svn_mergeinfo_inherited,
                                              iterpool));
      max_rev = MAX(unrefined_range->start, unrefined_range->end);
      SVN_ERR(svn_client__get_repos_mergeinfo(ra_session, &end_mergeinfo,
                                              repos_rel_path, max_rev,
                                              svn_mergeinfo_inherited,
                                              iterpool));

      SVN_ERR(svn_mergeinfo_diff(&deleted_mergeinfo, &added_mergeinfo,
                                 start_mergeinfo, end_mergeinfo,
                                 svn_rangelist_equal_inheritance,
                                 iterpool));

      if (apr_hash_count(added_mergeinfo))
        {
          const char *target_rel_path;
          SVN_ERR(svn_client__path_relative_to_root(&target_rel_path,
                                                    entry->url, entry->repos,
                                                    ra_session, NULL,
                                                    iterpool));
          src_rangelist_for_tgt = apr_hash_get(added_mergeinfo,
                                               target_rel_path,
                                               APR_HASH_KEY_STRING);
        }

      APR_ARRAY_PUSH(*requested_rangelist, svn_merge_range_t *) =
        unrefined_range;
      if (src_rangelist_for_tgt)
      /* Remove overlapping revision ranges from the requested range. */
      SVN_ERR(svn_rangelist_remove(requested_rangelist, src_rangelist_for_tgt,
                                   *requested_rangelist,
                                   svn_rangelist_equal_inheritance, pool));
    }
  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

/* Callback for qsort() calls which need to sort svn_merge_range_t *.
   Wraps svn_sort_compare_ranges() but first "normalizes" all ranges
   so range->end > range->start. */
static int
compare_merge_ranges(const void *a,
                      const void *b)
{
  /* ### Are all these pointer gymnastics necessary?
     ### There's gotta be a simpler way... */
  svn_merge_range_t *s1 = *((svn_merge_range_t * const *) a);
  svn_merge_range_t *s2 = *((svn_merge_range_t * const *) b);

  /* Convert the svn_merge_range_t to svn_merge_range_t and leverage our
     existing comparison function. */
  svn_merge_range_t r1 = { MIN(s1->start, s1->end),
                           MAX(s1->start, s1->end),
                           TRUE};
  svn_merge_range_t r2 = { MIN(s2->start, s2->end),
                           MAX(s2->start, s2->end),
                           TRUE};
  svn_merge_range_t *r1p = &r1; 
  svn_merge_range_t *r2p = &r2;
  return svn_sort_compare_ranges(&r1p, &r2p);
}

/* Another qsort() callback.  Wraps compare_merge_ranges(), but only
   for ranges that share a common "direction", e.g. additive or
   subtractive ranges.  If both ranges are subtractive, the range with
   the lowest (highest absolute) range value is consider the lesser.
   If the direction is not the same, then consider additive merges to
   always be less than subtractive merges. */
static int
compare_merge_ranges2(const void *a,
                       const void *b)
{
  svn_merge_range_t *s1 = *((svn_merge_range_t * const *) a);
  svn_merge_range_t *s2 = *((svn_merge_range_t * const *) b);
  svn_boolean_t s1_reversed = s1->start > s1->end;
  svn_boolean_t s2_reversed = s2->start > s2->end;

  if (s1_reversed && s2_reversed)
    return -(compare_merge_ranges(a, b));
  else if (s1_reversed)
    return 1;
  else if (s2_reversed)
    return -1;
  else
    return compare_merge_ranges(a, b);
}

/* Calculate a rangelist of svn_merge_range_t *'s -- for use by
   do_merge()'s application of the editor to the WC -- by subtracting
   revisions which have already been merged into the WC from the
   requested range(s) REQUESTED_RANGES, and storing what's left in
   REMAINING_RANGES.  TARGET_MERGEINFO may be NULL.  The ranges in
   *REMAINING_RANGES are guaranteed to be sorted as per
   compare_merge_ranges2. */
static svn_error_t *
calculate_merge_ranges(apr_array_header_t **remaining_ranges,
                       const char *rel_path,
                       apr_hash_t *target_mergeinfo,
                       apr_array_header_t *requested_ranges,
                       svn_boolean_t is_three_way_merge,
                       apr_pool_t *pool)
{
  apr_array_header_t *target_rangelist;

  if (!is_three_way_merge)
    /* As we monkey with this data, make a copy of it. */
    requested_ranges = svn_rangelist_dup(requested_ranges, pool);

  /* If we don't end up removing any revisions from the requested
     range, it'll end up as our sole remaining range. */
  *remaining_ranges = requested_ranges;

  /* Subtract the revision ranges which have already been merged into
     the WC (if any) from the ranges requested for merging (to avoid
     repeated merging). */
  if (target_mergeinfo)
    {
    target_rangelist = apr_hash_get(target_mergeinfo, rel_path,
                                    APR_HASH_KEY_STRING);
      if (target_rangelist)
        {
          int i;
          apr_array_header_t *additive_merges, *subtractive_merges,
            *after_subtractive_merges, *after_additive_merges;
          /* Split the requested ranges into additive and
             subtractive ranges. */
          if (is_three_way_merge)
            {
              additive_merges = requested_ranges;
              subtractive_merges = NULL;
            }
          else
            {
              additive_merges =
                apr_array_make(pool, 0, sizeof(svn_merge_range_t *));
              subtractive_merges =
                apr_array_make(pool, 0, sizeof(svn_merge_range_t *));
              for (i = 0; i < requested_ranges->nelts; i++)
                {
                  svn_merge_range_t *range =
                    APR_ARRAY_IDX(requested_ranges, i, svn_merge_range_t *);
                  if (range->start > range->end)
                    APR_ARRAY_PUSH(subtractive_merges, svn_merge_range_t *) =
                      range;
                  else
                    APR_ARRAY_PUSH(additive_merges, svn_merge_range_t *) =
                      range;
                }
            }

          if (subtractive_merges && subtractive_merges->nelts)
            {
              /* Return the intersection of the revs which are both
                 already represented by the WC and are requested for
                 revert.  The revert range will need to be reversed
                 for our APIs to work properly, as will the output for the
                 revert to work properly. */
              SVN_ERR(svn_rangelist_reverse(subtractive_merges, pool));
              qsort(subtractive_merges->elts, subtractive_merges->nelts,
              subtractive_merges->elt_size, svn_sort_compare_ranges);
              SVN_ERR(svn_rangelist_intersect(&after_subtractive_merges,
                                              target_rangelist,
                                              subtractive_merges, pool));
              SVN_ERR(svn_rangelist_reverse(subtractive_merges, pool));
              SVN_ERR(svn_rangelist_reverse(after_subtractive_merges, pool));
            }
          else
            {
              after_subtractive_merges = NULL;
            }

          if (additive_merges && additive_merges->nelts)
            {
              /* Return only those revs not already represented by this WC. */
              SVN_ERR(svn_rangelist_remove(&after_additive_merges,
                                           target_rangelist, additive_merges,
                                           svn_rangelist_ignore_inheritance,
                                           pool));
            }
          else
            after_additive_merges = NULL;

          /* We must have either additive or subtractive merges or both. */
          if (!after_subtractive_merges)
            {
              *remaining_ranges = after_additive_merges;
            }
          else
            {
              if (after_additive_merges)
                SVN_ERR(svn_rangelist_merge(&after_subtractive_merges,
                                            after_additive_merges,
                                            svn_rangelist_ignore_inheritance,
                                            pool));
              *remaining_ranges = after_subtractive_merges;
            }
        } /* if (target_rangelist) */
  } /* if (target_mergeinfo) */

  /* Sort the final results so all additive merges are in order at the start */
  qsort((*remaining_ranges)->elts, (*remaining_ranges)->nelts,
        (*remaining_ranges)->elt_size, compare_merge_ranges2);
  return SVN_NO_ERROR;
}

/* Contains any state collected while receiving path notifications. */
typedef struct
{
  /* The wrapped callback and baton. */
  svn_wc_notify_func2_t wrapped_func;
  void *wrapped_baton;

  /* Whether the operation's URL1 and URL2 are the same. */
  svn_boolean_t same_urls;

  /* The number of notifications received. */
  apr_uint32_t nbr_notifications;

  /* The number of operative notifications received. */
  apr_uint32_t nbr_operative_notifications;

  /* The list of merged paths. */
  apr_hash_t *merged_paths;

  /* The list of any skipped paths, which should be examined and
     cleared after each invocation of the callback. */
  apr_hash_t *skipped_paths;

  /* Flag indicating whether it is a single file merge or not. */
  svn_boolean_t is_single_file_merge;

  /* Depth first ordered list of paths that needs special care while merging.
   * This defaults to NULL. For 'same_url' merge alone we set it to 
   * proper array. This is used by notification_receiver to put a 
   * merge notification begin lines. */
  apr_array_header_t *children_with_mergeinfo;
  
  /* The index in CHILDREN_WITH_MERGEINFO where we found the nearest ancestor
     for merged path. Default value is '-1'.*/
  int cur_ancestor_index;

  /* We use this to make a decision on merge begin line notifications. */
  struct merge_cmd_baton *merge_b;

  /* Pool used in notification_receiver() to avoid the iteration
     sub-pool which is passed in, then subsequently destroyed. */
  apr_pool_t *pool;
} notification_receiver_baton_t;

/* Finds a nearest ancestor in CHILDREN_WITH_MERGEINFO for PATH.
 * CHILDREN_WITH_MERGEINFO is expected to be sorted in Depth first order
 * of path. Search starts from START_INDEX. Nearest ancestor's index from 
 * CHILDREN_WITH_MERGEINFO is returned. */
static int
find_nearest_ancestor(apr_array_header_t *children_with_mergeinfo,
                      const char *path, int start_index)
{
  int i;
  int ancestor_index;
  ancestor_index = start_index;
  /* This if condition is not needed as this function should be used from 
   * the context of same_url merge where CHILDREN_WITH_MERGEINFO will not be 
   * NULL and of size atleast 1. We have this if condition just to protect
   * the wrong caller. */
  if (!children_with_mergeinfo)
    return 0;
  for (i = start_index; i < children_with_mergeinfo->nelts; i++)
    {
      svn_client__merge_path_t *child =
                                APR_ARRAY_IDX(children_with_mergeinfo, i,
                                              svn_client__merge_path_t *);
      if (svn_path_is_ancestor(child->path, path))
        ancestor_index = i;
    }
  return ancestor_index;
}
/* Our svn_wc_notify_func2_t wrapper.*/
static void
notification_receiver(void *baton, const svn_wc_notify_t *notify,
                      apr_pool_t *pool)
{
  notification_receiver_baton_t *notify_b = baton;

  if (notify_b->same_urls)
    {
      int new_nearest_ancestor_index;
      int start_index = (notify_b->cur_ancestor_index == -1)
                        ? 0 : notify_b->cur_ancestor_index;
      notify_b->nbr_notifications++;

      if (!(notify_b->is_single_file_merge))
        {
          new_nearest_ancestor_index = 
                      find_nearest_ancestor(notify_b->children_with_mergeinfo,
                                            notify->path, start_index);
          if (new_nearest_ancestor_index != notify_b->cur_ancestor_index)
            {
              svn_client__merge_path_t *child =
                              APR_ARRAY_IDX(notify_b->children_with_mergeinfo,
                                            new_nearest_ancestor_index,
                                            svn_client__merge_path_t *);
              notify_b->cur_ancestor_index = new_nearest_ancestor_index;
              if (!child->absent && child->remaining_ranges->nelts > 0
                  && !(new_nearest_ancestor_index == 0
                       && (notify_b->merge_b)->target_has_dummy_merge_range))
                {
                  svn_wc_notify_t *notify_merge_begin;
                  notify_merge_begin = 
                               svn_wc_create_notify(child->path,
                                                    svn_wc_notify_merge_begin,
                                                    pool);
                  notify_merge_begin->merge_range = 
                                   APR_ARRAY_IDX(child->remaining_ranges, 0,
                                                 svn_merge_range_t *);
                  if (notify_b->wrapped_func)
                    (*notify_b->wrapped_func)(notify_b->wrapped_baton,
                                              notify_merge_begin, pool);
                }
            }
        }

      if (notify->content_state == svn_wc_notify_state_conflicted
          || notify->content_state == svn_wc_notify_state_merged
          || notify->content_state == svn_wc_notify_state_changed
          || notify->prop_state == svn_wc_notify_state_conflicted
          || notify->prop_state == svn_wc_notify_state_merged
          || notify->prop_state == svn_wc_notify_state_changed
          || notify->action == svn_wc_notify_update_add)
        notify_b->nbr_operative_notifications++;

      if (notify->content_state == svn_wc_notify_state_merged
          || notify->content_state == svn_wc_notify_state_changed
          || notify->prop_state == svn_wc_notify_state_merged
          || notify->prop_state == svn_wc_notify_state_changed
          || notify->action == svn_wc_notify_update_add)
        {
          const char *merged_path = apr_pstrdup(notify_b->pool, notify->path);

          if (notify_b->merged_paths == NULL)
            notify_b->merged_paths = apr_hash_make(notify_b->pool);

          apr_hash_set(notify_b->merged_paths, merged_path,
                       APR_HASH_KEY_STRING, merged_path);
        }

      if (notify->action == svn_wc_notify_skip)
        {
          const char *skipped_path = apr_pstrdup(notify_b->pool, notify->path);

          if (notify_b->skipped_paths == NULL)
            notify_b->skipped_paths = apr_hash_make(notify_b->pool);

          apr_hash_set(notify_b->skipped_paths, skipped_path,
                       APR_HASH_KEY_STRING, skipped_path);
        }
    }

  if (notify_b->wrapped_func)
    (*notify_b->wrapped_func)(notify_b->wrapped_baton, notify, pool);
}

/* Create mergeinfo describing the merge of MERGE_RANGES, an array
   of svn_merge_range_t * into our target, accounting
   for paths unaffected by the merge due to skips or conflicts from
   NOTIFY_B. For 'immediates' merge it sets an inheritable mergeinfo 
   corresponding to current merge on merge target. For 'files' merge it sets
   an inheritable mergeinfo corrsponding to current merge on merged files.
   Note in MERGE_B->OPERATIVE_MERGE if an operative merge
   is discovered.  If TARGET_WCPATH is a directory and it is missing
   an immediate child then TARGET_MISSING_CHILD should be true,
   otherwise it is false.*/
static svn_error_t *
determine_merges_performed(apr_hash_t **merges, const char *target_wcpath,
                           apr_array_header_t *merge_ranges,
                           svn_depth_t depth,
                           svn_wc_adm_access_t *adm_access,
                           notification_receiver_baton_t *notify_b,
                           struct merge_cmd_baton *merge_b,
                           apr_pool_t *pool)
{
  apr_size_t nbr_skips = (notify_b->skipped_paths != NULL ?
                          apr_hash_count(notify_b->skipped_paths) : 0);
  *merges = apr_hash_make(pool);

  /* If there have been no operative merges on any subtree merged so far and
     we are determining the merges performed on the merge target (i.e. the
     last such determination to be made), *and* there are no operative merges
     on the target either, then don't calculate anything.  Just return the
     empty hash because this whole merge has been a no-op and we don't change
     the mergeinfo in that case (issue #2883). */
   if (!notify_b->nbr_operative_notifications
       && !merge_b->operative_merge
       && svn_path_compare_paths(target_wcpath, merge_b->target) == 0)
     return SVN_NO_ERROR;


  /* Note in the merge baton when the first operative merge is found. */
  if (notify_b->nbr_operative_notifications > 0
      && !merge_b->operative_merge)
    merge_b->operative_merge = TRUE;

  apr_hash_set(*merges, target_wcpath, APR_HASH_KEY_STRING, merge_ranges);
  if (nbr_skips > 0)
    {
      apr_hash_index_t *hi;

      /* Override the mergeinfo for child paths which weren't
         actually merged. */
      for (hi = apr_hash_first(NULL, notify_b->skipped_paths); hi;
           hi = apr_hash_next(hi))
        {
          const void *skipped_path;
          apr_hash_this(hi, &skipped_path, NULL, NULL);

          /* Add an empty range list for this path. */
          apr_hash_set(*merges, (const char *) skipped_path,
                       APR_HASH_KEY_STRING,
                       apr_array_make(pool, 0, sizeof(svn_merge_range_t *)));

          if (nbr_skips < notify_b->nbr_notifications)
            /* ### Use RANGELIST as the mergeinfo for all children of
               ### this path which were not also explicitly
               ### skipped? */
            ;
        }
    }
  if ((depth != svn_depth_infinity) && notify_b->merged_paths)
    {
      apr_hash_index_t *hi;
      const void *merged_path;

      for (hi = apr_hash_first(NULL, notify_b->merged_paths); hi;
           hi = apr_hash_next(hi))
        {
          const svn_wc_entry_t *child_entry;
          apr_array_header_t *child_merge_ranges;
          apr_array_header_t *rangelist_of_child;
          
          apr_hash_this(hi, &merged_path, NULL, NULL);
          child_merge_ranges = svn_rangelist_dup(merge_ranges, pool);
          SVN_ERR(svn_wc__entry_versioned(&child_entry,
                                          merged_path,
                                          adm_access, FALSE,
                                          pool));
          if (((child_entry->kind == svn_node_dir) && 
               (strcmp(merge_b->target, merged_path) == 0) &&
               (depth == svn_depth_immediates))
              || ((child_entry->kind == svn_node_file) &&
                  (depth == svn_depth_files)))
            {
              /* Set the explicit inheritable mergeinfo for, 
                 1. Merge target directory if depth is immediates.
                 2. If merge is on a file and requested depth is 'files'.
               */
              int i;
              rangelist_of_child =
                apr_array_make(pool, 1, sizeof(svn_merge_range_t *));
              for (i = 0; i < child_merge_ranges->nelts; i++)
                {
                  svn_merge_range_t *r =
                    APR_ARRAY_IDX(child_merge_ranges, i, svn_merge_range_t *);
                  r->inheritable = TRUE;
                  APR_ARRAY_PUSH(rangelist_of_child, svn_merge_range_t *) = r;
            }
              apr_hash_set(*merges, (const char *) merged_path,
                           APR_HASH_KEY_STRING, rangelist_of_child);
            }
        }
    }

  return SVN_NO_ERROR;
}

/* Calculate the new mergeinfo for the target tree based on the merge
   info for TARGET_WCPATH and MERGES (a mapping of WC paths to range
   lists), and record it in the WC (at, and possibly below,
   TARGET_WCPATH). */
static svn_error_t *
update_wc_mergeinfo(const char *target_wcpath, const svn_wc_entry_t *entry,
                    const char *repos_rel_path, apr_hash_t *merges,
                    svn_boolean_t is_three_way_merge,
                    svn_wc_adm_access_t *adm_access,
                    svn_client_ctx_t *ctx, apr_pool_t *pool)
{
  apr_pool_t *subpool = svn_pool_create(pool);
  const char *rel_path;
  apr_hash_t *mergeinfo;
  apr_hash_index_t *hi;
  apr_array_header_t *subtractive_rangelist = NULL;

  /* Combine the mergeinfo for the revision range just merged into
     the WC with its on-disk mergeinfo. */
  for (hi = apr_hash_first(pool, merges); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      void *value;
      const char *path;
      apr_array_header_t *ranges, *rangelist;
      int len;
      svn_error_t *err;

      svn_pool_clear(subpool);

      apr_hash_this(hi, &key, NULL, &value);
      path = key;
      ranges = value;

      /* As some of the merges may've changed the WC's mergeinfo, get
         a fresh copy before using it to update the WC's mergeinfo. */
      SVN_ERR(svn_client__parse_mergeinfo(&mergeinfo, entry, path, FALSE,
                                          adm_access, ctx, subpool));

      /* If we are attempting to set empty revision range override mergeinfo
         on a path with no explicit mergeinfo, we first need the pristine
         mergeinfo that path inherits. */
      if (mergeinfo == NULL && ranges->nelts == 0)
        {
          svn_boolean_t inherited;
          SVN_ERR(svn_client__get_wc_mergeinfo(&mergeinfo, &inherited, TRUE,
                                               svn_mergeinfo_nearest_ancestor, 
                                               entry, path, NULL, NULL, 
                                               adm_access, ctx, subpool));
        }

      if (mergeinfo == NULL)
        mergeinfo = apr_hash_make(subpool);

      /* ASSUMPTION: "target_wcpath" is always both a parent and
         prefix of "path". */
      len = strlen(target_wcpath);
      if (len < strlen(path))
        {
          const char *path_relative_to_target = len?(path + len + 1):(path);
          rel_path = apr_pstrcat(subpool, repos_rel_path, "/",
                                 path_relative_to_target, NULL);
        }
      else
        rel_path = repos_rel_path;
      rangelist = apr_hash_get(mergeinfo, rel_path, APR_HASH_KEY_STRING);
      if (rangelist == NULL)
        rangelist = apr_array_make(subpool, 0, sizeof(svn_merge_range_t *));

      if (is_three_way_merge)
        {
          /* The easy path! */
          SVN_ERR(svn_rangelist_merge(&rangelist, ranges,
                                      svn_rangelist_equal_inheritance,
                                       subpool));
        }
      else
        {
          /* The less easy path.  Split the rangelists into additive
             and subtractive ranges. */
          int i;
          apr_array_header_t *additive_rangelist =
            apr_array_make(subpool, 0, sizeof(svn_merge_range_t *));
          subtractive_rangelist =
            apr_array_make(subpool, 0, sizeof(svn_merge_range_t *));
          for (i = 0; i < ranges->nelts; i++)
            {
              svn_merge_range_t *range =
                APR_ARRAY_IDX(ranges, i, svn_merge_range_t *);
              if (range->start > range->end)
                APR_ARRAY_PUSH(subtractive_rangelist, svn_merge_range_t *) =
                  range;
              else
                APR_ARRAY_PUSH(additive_rangelist, svn_merge_range_t *) =
                  range;
            }

          if (subtractive_rangelist->nelts)
            {
              /* Return the intersection of the revs which are both
                 already represented by the WC and are requested for
                 revert.  The revert range will need to be reversed
                 for our APIs to work properly, as will the output for the
                 revert to work properly. */
              apr_array_header_t *subtractive_rangelist_copy =
                svn_rangelist_dup(subtractive_rangelist, subpool);
              SVN_ERR(svn_rangelist_reverse(subtractive_rangelist_copy,
                                            subpool));
              qsort(subtractive_rangelist_copy->elts,
                    subtractive_rangelist_copy->nelts,
                    subtractive_rangelist_copy->elt_size,
                    svn_sort_compare_ranges);
              SVN_ERR(svn_rangelist_remove(&rangelist,
                                           subtractive_rangelist_copy,
                                           rangelist,
                                           svn_rangelist_ignore_inheritance,
                                           subpool));
            }
          if (additive_rangelist->nelts)
            /* Return only those revs not already represented by this WC. */
            SVN_ERR(svn_rangelist_merge(&rangelist, additive_rangelist,
                                      svn_rangelist_equal_inheritance,
                                      subpool));
        }

      /* Update the mergeinfo by adjusting the path's rangelist. */
      apr_hash_set(mergeinfo, rel_path, APR_HASH_KEY_STRING, rangelist);

      err = svn_client__record_wc_mergeinfo(path, mergeinfo,
                                            adm_access, subpool);

      if (err && err->apr_err == SVN_ERR_ENTRY_NOT_FOUND)
        {
          /* PATH isn't just missing, it's not even versioned as far as this
             working copy knows.  But it was included in MERGES, which means
             that the server knows about it.  Likely we don't have access to
             the source due to authz restrictions.  For now just clear the
             error and continue...

             ### TODO:  Set non-inheritable mergeinfo on PATH's immediate
             ### parent and normal mergeinfo on PATH's siblings which we
             ### do have access to. */
          svn_error_clear(err);
        }
      else
        SVN_ERR(err);
    }

  svn_pool_destroy(subpool);
  return SVN_NO_ERROR;
}

/* A quad-state value returned by grok_range_info_from_opt_revisions(). */
enum merge_type
{
  /* Merge of diffs between same URL */
  merge_type_merge,     /* additive */
  merge_type_rollback,  /* subtractive */
  merge_type_no_op,     /* no change */

  /* Merge of diffs between different URLs */
  merge_type_3_way_merge /* diff between two different URLs */
};

/* Resolve requested revisions for REVISION1 and REVISION2
   (using RA_SESSION1 and RA_SESSION2), convert them into a merge
   range, determine whether that range represents a merge/revert/no-op
   if SAME_URLS (assume merge otherwise), and store that knowledge in
   *RANGE and *MERGE_TYPE (respectively).  If the resulting revisions
   would result in the merge being a no-op, RANGE->START and
   RANGE->END are set to SVN_INVALID_REVNUM.  RANGE->INHERITABLE is
   always set to TRUE. */
static svn_error_t *
grok_range_info_from_opt_revisions(svn_merge_range_t *range,
                                   enum merge_type *merge_type,
                                   svn_boolean_t same_urls,
                                   svn_ra_session_t *ra_session1,
                                   const svn_opt_revision_t *revision1,
                                   svn_ra_session_t *ra_session2,
                                   const svn_opt_revision_t *revision2,
                                   apr_pool_t *pool)
{
  /* Resolve the revision numbers. */
  SVN_ERR(svn_client__get_revision_number
          (&range->start, ra_session1, revision1, NULL, pool));
  SVN_ERR(svn_client__get_revision_number
          (&range->end, ra_session2, revision2, NULL, pool));

  /* If comparing revisions from different URLs when doing a 3-way
     merge, there's no way to determine the merge type on the
     client-side from the peg revs of the URLs alone (history tracing
     would be required). */
  if (same_urls)
    {
      if (range->start < range->end)
        {
          *merge_type = merge_type_merge;
        }
      else if (range->start > range->end)
        {
          *merge_type = merge_type_rollback;
        }
      else  /* No revisions to merge. */
        {
          *merge_type = merge_type_no_op;
          range->start = range->end = SVN_INVALID_REVNUM;
        }
    }
  else
    {
      *merge_type = merge_type_3_way_merge;
    }
  range->inheritable = TRUE;
  return SVN_NO_ERROR;
}

/* Default the values of REVISION1 and REVISION2 to be oldest rev at
   which ra_session's root got created and HEAD (respectively), if
   REVISION1 and REVISION2 are unspecified.  This assumed value is set
   at *ASSUMED_REVISION1 and *ASSUMED_REVISION2.  RA_SESSION is used
   to retrieve the revision of the current HEAD revision.  Use POOL
   for temporary allocations. */
static svn_error_t *
assume_default_rev_range(const svn_opt_revision_t *revision1,
                         svn_opt_revision_t *assumed_revision1,
                         const svn_opt_revision_t *revision2,
                         svn_opt_revision_t *assumed_revision2,
                         svn_ra_session_t *ra_session,
                         apr_pool_t *pool)
{
  svn_opt_revision_t head_rev_opt;
  svn_revnum_t head_revnum = SVN_INVALID_REVNUM;
  head_rev_opt.kind = svn_opt_revision_head;
  /* Provide reasonable defaults for unspecified revisions. */
  if (revision1->kind == svn_opt_revision_unspecified)
    {
      SVN_ERR(svn_client__get_revision_number(&head_revnum, ra_session,
                                              &head_rev_opt, "", pool));
      SVN_ERR(svn_client__oldest_rev_at_path(&assumed_revision1->value.number,
                                             ra_session, "",
                                             head_revnum,
                                             pool));
      if (SVN_IS_VALID_REVNUM(assumed_revision1->value.number))
        {
          assumed_revision1->kind = svn_opt_revision_number;
        }
    }
  else
    {
      *assumed_revision1 = *revision1;
    }
  if (revision2->kind == svn_opt_revision_unspecified)
    {
      if (SVN_IS_VALID_REVNUM(head_revnum))
        {
          assumed_revision2->value.number = head_revnum;
          assumed_revision2->kind = svn_opt_revision_number;
        }
      else
        {
          assumed_revision2->kind = svn_opt_revision_head;
        }
    }
  else
    {
      *assumed_revision2 = *revision2;
    }
  return SVN_NO_ERROR;
}

/* Create and return an error structure appropriate for the unmerged
   revisions range(s). */
static APR_INLINE svn_error_t *
make_merge_conflict_error(const char *target_wcpath,
                          svn_merge_range_t *r,
                          apr_pool_t *pool)
{
  return svn_error_createf
    (SVN_ERR_WC_FOUND_CONFLICT, NULL,
     _("One or more conflicts were produced while merging r%ld:%ld into\n"
       "'%s' --\n"
       "resolve all conflicts and rerun the merge to apply the remaining\n"
       "unmerged revisions"),
     r->start, r->end, svn_path_local_style(target_wcpath, pool));
}

/* Helper for do_merge().

   TARGET_WCPATH is a directory and CHILDREN_WITH_MERGEINFO is filled
   with paths (svn_client__merge_path_t *) arranged in depth first order,
   which have mergeinfo set on them or are absent from the WC (see
   discover_and_merge_children and get_mergeinfo_paths).  Remove any
   absent paths in CHILDREN_WITH_MERGEINFO which are equal to or are
   descendents of TARGET_WCPATH by setting those children to NULL.
   Also remove the path from the NOTIFY_B->SKIPPED_PATHS hash. */
static void
remove_absent_children(const char *target_wcpath,
                       apr_array_header_t *children_with_mergeinfo,
                       notification_receiver_baton_t *notify_b)
{
  /* Before we try to override mergeinfo for skipped paths, make sure
     the path isn't absent due to authz restrictions, because there's
     nothing we can do about those. */
  int i;
  for (i = 0; i < children_with_mergeinfo->nelts; i++)
    {
      svn_client__merge_path_t *child =
        APR_ARRAY_IDX(children_with_mergeinfo, 
                      i, svn_client__merge_path_t *);
      if (child
          && child->absent
          && svn_path_is_ancestor(target_wcpath, child->path))
        {
          if (notify_b->skipped_paths)
            apr_hash_set(notify_b->skipped_paths, child->path,
                         APR_HASH_KEY_STRING, NULL);
          APR_ARRAY_IDX(children_with_mergeinfo, i, 
                        svn_client__merge_path_t *) = NULL;
        }
    }
}

/* Populate *REMAINING_RANGES with after removing reflective merge ranges
 * and already merged ranges from *RANGE.
 * Cascade TARGET_MERGEINFO, IS_ROLLBACK, REL_PATH, URL, RA_SESSION,
 * ENTRY, CTX, POOL.
 */
static svn_error_t *
calculate_remaining_ranges(apr_array_header_t **remaining_ranges,
                           apr_array_header_t *ranges,
                           apr_hash_t *target_mergeinfo,
                           svn_boolean_t is_three_way_merge,
                           const char *rel_path,
                           const char *url,
                           svn_ra_session_t *ra_session,
                           const svn_wc_entry_t *entry,
                           svn_client_ctx_t *ctx,
                           apr_pool_t *pool)
{
  apr_array_header_t *requested_rangelist;
  /* Determine which of the requested ranges to consider merging... */
  SVN_ERR(calculate_requested_ranges(&requested_rangelist, ranges, url, entry,
                                     ra_session, ctx, pool));

  /* ...and of those ranges, determine which ones actually still
     need merging. */
  SVN_ERR(calculate_merge_ranges(remaining_ranges, rel_path,
                                 target_mergeinfo, requested_rangelist,
                                 is_three_way_merge, pool));
  return SVN_NO_ERROR;
}

/* Sets up the diff editor report and drives it by properly negating
   subtree that could have a conflicting merge history.*/
static svn_error_t *
drive_merge_report_editor(const char *target_wcpath,
                          const char *url1,
                          const char *url2,
                          apr_array_header_t *children_with_mergeinfo,
                          svn_revnum_t start,
                          svn_revnum_t end,
                          svn_boolean_t is_rollback,
                          svn_depth_t depth,
                          svn_boolean_t ignore_ancestry,
                          notification_receiver_baton_t *notify_b,
                          svn_wc_adm_access_t *adm_access,
                          const svn_wc_diff_callbacks2_t *callbacks,
                          struct merge_cmd_baton *merge_b,
                          apr_pool_t *pool)
{
  const svn_ra_reporter3_t *reporter;
  const svn_delta_editor_t *diff_editor;
  void *diff_edit_baton;
  void *report_baton;
  svn_revnum_t default_start = start; 
  if (merge_b->target_has_dummy_merge_range)
    default_start = end;
  else if (children_with_mergeinfo && children_with_mergeinfo->nelts)
    {
      svn_client__merge_path_t *target_merge_path_t;
      target_merge_path_t = APR_ARRAY_IDX(children_with_mergeinfo, 0,
                                          svn_client__merge_path_t *);
      if (target_merge_path_t->remaining_ranges->nelts)
        {
          svn_merge_range_t *range = 
                         APR_ARRAY_IDX(target_merge_path_t->remaining_ranges,
                                       0, svn_merge_range_t *);
          default_start = range->start;
        }
    }
  /* Open a second session used to request individual file
     contents. Although a session can be used for multiple requests, it
     appears that they must be sequential. Since the first request, for
     the diff, is still being processed the first session cannot be
     reused. This applies to ra_neon, ra_local does not appears to have
     this limitation. */
  SVN_ERR(svn_client__open_ra_session_internal(&merge_b->ra_session2, url1,
                                               NULL, NULL, NULL, FALSE, TRUE,
                                               merge_b->ctx, pool));
  SVN_ERR(svn_client__get_diff_editor(target_wcpath, adm_access, callbacks,
                                      merge_b, depth, merge_b->dry_run,
                                      merge_b->ra_session2, default_start,
                                      notification_receiver, notify_b,
                                      merge_b->ctx->cancel_func,
                                      merge_b->ctx->cancel_baton,
                                      &diff_editor, &diff_edit_baton,
                                      pool));

  SVN_ERR(svn_ra_do_diff3(merge_b->ra_session1, &reporter, &report_baton, end,
                          "", depth, ignore_ancestry, TRUE,  /* text_deltas */
                          url2, diff_editor, diff_edit_baton, pool));



  SVN_ERR(reporter->set_path(report_baton, "", default_start, depth,
                             FALSE, NULL, pool));

  if (notify_b->same_urls && children_with_mergeinfo)
    {
      /* Describe children with mergeinfo overlapping this merge
         operation such that no repeated diff is retrieved for them from
         the repository. */
      apr_size_t target_wcpath_len = strlen(target_wcpath);
      int i;
      for (i = 1; i < children_with_mergeinfo->nelts; i++)
        {
          svn_merge_range_t *range;
          svn_client__merge_path_t *child = 
            APR_ARRAY_IDX(children_with_mergeinfo, i, 
                          svn_client__merge_path_t *);
          if (!child || child->absent || (child->remaining_ranges->nelts == 0))
            continue;

          range = APR_ARRAY_IDX(child->remaining_ranges, 0,
                                svn_merge_range_t *);
          if (range->start == default_start)
            continue;
          else
            {
              const char *child_repos_path = child->path +
                               (target_wcpath_len ? target_wcpath_len + 1 : 0);
              if ((is_rollback && (range->start < end)) 
                  || (!is_rollback && (range->start > end)))
                SVN_ERR(reporter->set_path(report_baton, child_repos_path,
                                           end, depth, FALSE, NULL, pool));
              else
                SVN_ERR(reporter->set_path(report_baton, child_repos_path,
                                           range->start, depth, FALSE, NULL,
                                           pool));
            }
        }
      }

  SVN_ERR(reporter->finish_report(report_baton, pool));

  return SVN_NO_ERROR;
}

/* For each child in children_with_mergeinfo, it populates the remaing_ranges.
 * All persistent allocations are from children_with_mergeinfo->pool.
 */
static svn_error_t *
populate_remaining_ranges(apr_array_header_t *children_with_mergeinfo,
                          const char *parent_merge_source_url,
                          svn_ra_session_t *ra_session,
                          apr_array_header_t *merge_sources,
                          const char *parent_merge_src_canon_path,
                          svn_wc_adm_access_t *adm_access,
                          struct merge_cmd_baton *merge_b)
{
  int i;
  apr_pool_t *iterpool, *persistent_pool;
  int merge_target_len = strlen(merge_b->target);
  persistent_pool = children_with_mergeinfo->pool;
  iterpool = svn_pool_create(persistent_pool);
  for (i = 0; i < children_with_mergeinfo->nelts; i++)
    {
      const char *child_repos_path;
      const svn_wc_entry_t *child_entry;
      const char *child_merge_src_canon_path;

      svn_client__merge_path_t *child =
                                APR_ARRAY_IDX(children_with_mergeinfo, i,
                                              svn_client__merge_path_t *);
      /* If the path is absent don't do subtree merge either. */
      if (!child || child->absent)
        continue;
      svn_pool_clear(iterpool);
      if (strlen(child->path) == merge_target_len)
        child_repos_path = "";
      else
        child_repos_path = child->path +
                                 (merge_target_len ? merge_target_len + 1 : 0);
      child_merge_src_canon_path = svn_path_join(parent_merge_src_canon_path,
                                                 child_repos_path, iterpool);

      SVN_ERR(svn_wc__entry_versioned(&child_entry, child->path, adm_access,
                                      FALSE, iterpool));

      SVN_ERR(svn_client__get_wc_or_repos_mergeinfo(
                                                 &(child->pre_merge_mergeinfo),
                                                 child_entry,
                                                 &(child->indirect_mergeinfo), FALSE,
                                                 svn_mergeinfo_inherited,
                                                 NULL, child->path,
                                                 adm_access, merge_b->ctx,
                                                 persistent_pool));

      SVN_ERR(calculate_remaining_ranges(&(child->remaining_ranges),
                                         merge_sources,
                                         child->pre_merge_mergeinfo,
                                         FALSE,
                                         child_merge_src_canon_path,
                                         child_entry->url,
                                         ra_session, child_entry, merge_b->ctx,
                                         persistent_pool));
      if ((strcmp(child->path, merge_b->target) == 0)
          && child->remaining_ranges->nelts == 0)
        {
          svn_merge_range_t *dummy_merge_range = 
            apr_palloc(persistent_pool, sizeof(*dummy_merge_range));
            dummy_merge_range->start = dummy_merge_range->end =
              APR_ARRAY_IDX(merge_sources, 0, svn_merge_range_t *)->end;
          child->remaining_ranges = apr_array_make(persistent_pool, 1,
                                                   sizeof(dummy_merge_range));
          APR_ARRAY_PUSH(child->remaining_ranges, svn_merge_range_t *) =
            dummy_merge_range;
          merge_b->target_has_dummy_merge_range = TRUE;
        }
    }

  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}

/* Gets the smallest end_rev from all the ranges from remaining_ranges[0].
 * If all childs have empty remaining_ranges returns SVN_INVALID_REVNUM.
 */

static svn_revnum_t
get_nearest_end_rev(apr_array_header_t *children_with_mergeinfo,
                    svn_merge_range_t *previous_range)
{
  /* ### TODO: Combine this function with get_farthest_end_rev() */
  int i;
  svn_revnum_t nearest_end_rev = SVN_INVALID_REVNUM;
  for (i = 0; i < children_with_mergeinfo->nelts; i++)
    {
      svn_client__merge_path_t *child =
                                  APR_ARRAY_IDX(children_with_mergeinfo, i,
                                                svn_client__merge_path_t *);
      if (!child || child->absent)
        continue;
      if (child->remaining_ranges->nelts > 0)
        {
          svn_merge_range_t *range = APR_ARRAY_IDX(child->remaining_ranges, 0,
                                                   svn_merge_range_t *);
          /* If the PREVIOUS_RANGE isn't a dummy range and it and RANGE are
             not in the same direction (i.e. both rollbacks or both additive)
             then we are done. */
          if ((range->start != range->end)
              && ((range->start > range->end)
                  != (previous_range->start > previous_range->end)))
            continue;

          /* Don't pull nearest_end_rev from a range that doesn't
             intersect with PREVIOUS_RANGE. */
          if (range->start <= previous_range->end
              && previous_range->start <= range->end)
            {
              if (nearest_end_rev == SVN_INVALID_REVNUM)
                nearest_end_rev = range->end;
              else if (range->end < nearest_end_rev)
                nearest_end_rev = range->end;
            }
        }
    }
  return nearest_end_rev;
}

/* Gets the biggest end_rev from all the ranges from remaining_ranges[0].
 * If all childs have empty remaining_ranges returns SVN_INVALID_REVNUM.
 */

static svn_revnum_t
get_farthest_end_rev(apr_array_header_t *children_with_mergeinfo,
                     svn_merge_range_t *previous_range)
{
  int i;
  svn_revnum_t farthest_end_rev = SVN_INVALID_REVNUM;
  for (i = 0; i < children_with_mergeinfo->nelts; i++)
    {
      svn_client__merge_path_t *child =
                                  APR_ARRAY_IDX(children_with_mergeinfo, i,
                                                svn_client__merge_path_t *);
      if (!child || child->absent)
        continue;
      if (child->remaining_ranges->nelts > 0)
        {
          svn_merge_range_t *range = APR_ARRAY_IDX(child->remaining_ranges, 0,
                                                   svn_merge_range_t *);

          /* If the PREVIOUS_RANGE isn't a dummy range and it and RANGE are
             not in the same direction (i.e. both rollbacks or both additive)
             then we are done. */
          if ((range->start != range->end)
              && ((range->start > range->end)
                  != (previous_range->start > previous_range->end)))
            continue;

          /* Don't pull nearest_end_rev from a range that doesn't
             intersect with PREVIOUS_RANGE. Since this function is only
             called for rollbacks we test for range intersection slightly
             differently from get_nearest_end_rev(). */
          if (range->start >= previous_range->end
              && previous_range->start >= range->end)
            {
              if (farthest_end_rev == SVN_INVALID_REVNUM)
                farthest_end_rev = range->end;
              else if (range->end > farthest_end_rev)
                farthest_end_rev = range->end;
            }
        }
    }
  return farthest_end_rev;
}

/* If first item in each child of CHILDREN_WITH_MERGEINFO's 
 * remaining_ranges is inclusive of END_REV, Slice the first range in to two 
 * at END_REV. All the allocations are persistent and allocated from POOL. */
static void
slice_remaining_ranges(apr_array_header_t *children_with_mergeinfo,
                       svn_boolean_t is_rollback, svn_revnum_t end_rev,
                       apr_pool_t *pool)
{
  int i;
  for (i = 0; i < children_with_mergeinfo->nelts; i++)
    {
      svn_client__merge_path_t *child =
                                     APR_ARRAY_IDX(children_with_mergeinfo, i,
                                                   svn_client__merge_path_t *);
      if (!child || child->absent)
        continue;
      if (child->remaining_ranges->nelts > 0)
        {
          svn_merge_range_t *range = APR_ARRAY_IDX(child->remaining_ranges, 0,
                                                   svn_merge_range_t *);
          if ((is_rollback && (range->start > end_rev)
               && (range->end < end_rev))
              || (!is_rollback && (range->start < end_rev)
                  && (range->end > end_rev)))
            {
              int j;
              svn_merge_range_t *split_range1, *split_range2;
              apr_array_header_t *orig_remaining_ranges =
                                                     child->remaining_ranges;
              split_range1 = svn_merge_range_dup(range, pool);
              split_range2 = svn_merge_range_dup(range, pool);
              split_range1->end = end_rev;
              split_range2->start = end_rev;
              child->remaining_ranges =
                     apr_array_make(pool, (child->remaining_ranges->nelts + 1),
                                    sizeof(svn_merge_range_t *));
              APR_ARRAY_PUSH(child->remaining_ranges,
                             svn_merge_range_t *) = split_range1;
              APR_ARRAY_PUSH(child->remaining_ranges,
                             svn_merge_range_t *) = split_range2;
              for (j = 1; j < orig_remaining_ranges->nelts; j++)
                {
                  svn_merge_range_t *orig_range =
                                     APR_ARRAY_IDX(orig_remaining_ranges, j,
                                                   svn_merge_range_t *);
                  APR_ARRAY_PUSH(child->remaining_ranges,
                                 svn_merge_range_t *) = orig_range;
                }
            }
        }
    }
}

/* For each child of CHILDREN_WITH_MERGEINFO, the direction of the first child
 * agrees with the last range processed by do_merge(), as indicated by
 * IS_ROLLBACK, then create a new remaining_ranges
 * by removing the first item from the original range list and overwrite the
 * original remaining_ranges with this new list.
 * All the allocations are persistent from a POOL.
 * TODO, we should have remaining_ranges in reverse order to avoid recreating
 * the remaining_ranges every time instead of one 'pop' operation.
 */
static void
remove_first_range_from_remaining_ranges(
  apr_array_header_t *children_with_mergeinfo,
  svn_boolean_t is_rollback,
  apr_pool_t *pool)
{
  int i, j;
  for (i = 0; i < children_with_mergeinfo->nelts; i++)
    {
      svn_client__merge_path_t *child =
                                APR_ARRAY_IDX(children_with_mergeinfo, i,
                                              svn_client__merge_path_t *);
      if (!child || child->absent)
        continue;
      if (child->remaining_ranges->nelts > 0)
        {
          apr_array_header_t *orig_remaining_ranges = child->remaining_ranges;
          svn_merge_range_t *r = APR_ARRAY_IDX(child->remaining_ranges, 0,
                                               svn_merge_range_t *);
          /* Don't remove a rollback range if we just processed an
             additive range. */
          if (r->start != r->end
              && ((is_rollback ? TRUE : FALSE )
                  != (r->start > r->end ? TRUE : FALSE)))
            continue;
          child->remaining_ranges =
            apr_array_make(pool, (child->remaining_ranges->nelts - 1),
                           sizeof(svn_merge_range_t *));
          for (j = 1; j < orig_remaining_ranges->nelts; j++)
            {
              svn_merge_range_t *range = APR_ARRAY_IDX(orig_remaining_ranges,
                                                       j,
                                                       svn_merge_range_t *);
              APR_ARRAY_PUSH(child->remaining_ranges, svn_merge_range_t *)
                                                                  = range;
            }
        }
    }
}


/* Blindly record the range specified by the user (rather than refining it
   as we do for actual merges). */
static svn_error_t *
record_mergeinfo_for_record_only_merge(const char *URL1,
                                       svn_merge_range_t *range,
                                       svn_boolean_t is_three_way_merge,
                                       const svn_wc_entry_t *entry,
                                       svn_wc_adm_access_t *adm_access,
                                       struct merge_cmd_baton *merge_b,
                                       apr_pool_t *pool)
{
  apr_array_header_t *rangelist;
  const char *rel_path;
  apr_hash_t *target_mergeinfo;
  svn_boolean_t indirect;
  apr_hash_t *merges = apr_hash_make(pool);
  /* Temporarily reparent ra_session to WC target URL. */
  SVN_ERR(svn_ra_reparent(merge_b->ra_session1, entry->url, pool));
  SVN_ERR(svn_client__get_wc_or_repos_mergeinfo(&target_mergeinfo, entry,
                                                &indirect, FALSE,
                                                svn_mergeinfo_inherited,
                                                merge_b->ra_session1,
                                                merge_b->target,
                                                adm_access, merge_b->ctx,
                                                pool));
  /* Reparent ra_session back to URL1. */
  SVN_ERR(svn_ra_reparent(merge_b->ra_session1, URL1, pool));
  SVN_ERR(svn_client__path_relative_to_root(&rel_path, URL1, NULL,
                                            merge_b->ra_session1,
                                            adm_access, pool));
  rangelist = apr_array_make(pool, 1, sizeof(range));
  APR_ARRAY_PUSH(rangelist, svn_merge_range_t *) = range;
  apr_hash_set(merges, merge_b->target, APR_HASH_KEY_STRING, rangelist);

  /* If merge target has indirect mergeinfo set it. */
  if (indirect)
    SVN_ERR(svn_client__record_wc_mergeinfo(merge_b->target, target_mergeinfo,
                                            adm_access, pool));

  return update_wc_mergeinfo(merge_b->target, entry, rel_path, merges,
                             is_three_way_merge, adm_access, merge_b->ctx, pool);
}

/* Marks 'inheritable' RANGE to TARGET_WCPATH by wiping off the 
 * corresponding 'non-inheritable' RANGE from TARGET_MERGEINFO for the
 * merge source REL_PATH. 
 * It does such marking only for same URLs from same Repository, 
 * not a dry run, target having existing mergeinfo(TARGET_MERGEINFO) and 
 * target being part of CHILDREN_WITH_MERGEINFO.
*/
static svn_error_t *
mark_mergeinfo_as_inheritable_for_a_range(
                                   apr_hash_t *target_mergeinfo,
                                   svn_boolean_t same_urls,
                                   apr_array_header_t *merge_ranges,
                                   const char *rel_path,
                                   const char *target_wcpath,
                                   svn_wc_adm_access_t *adm_access,
                                   struct merge_cmd_baton *merge_b,
                                   apr_array_header_t *children_with_mergeinfo,
                                   int target_index, apr_pool_t *pool)
{
  /* Check if we need to make non-inheritable ranges inheritable. */
  if (target_mergeinfo && same_urls
      && !merge_b->dry_run
      && merge_b->same_repos
      && target_index >= 0)
    {
      svn_client__merge_path_t *merge_path = 
        APR_ARRAY_IDX(children_with_mergeinfo,
                      target_index, svn_client__merge_path_t *);

      /* If a path has no missing children, has non-inheritable ranges,
         *and* those non-inheritable ranges intersect with the merge being
         performed (i.e. this is a repeat merge where a previously missing
         child is now present) then those non-inheritable ranges are made
         inheritable. */
      if (merge_path
          && merge_path->has_noninheritable && !merge_path->missing_child)
        {
          svn_boolean_t is_equal;
          apr_hash_t *merges;
          apr_hash_t *inheritable_merges = apr_hash_make(pool);
          apr_array_header_t *inheritable_ranges =
            svn_rangelist_dup(merge_ranges, pool);
          svn_revnum_t hi_rev, low_rev;
          apr_hash_set(inheritable_merges, rel_path, APR_HASH_KEY_STRING,
                       inheritable_ranges);
          get_hi_lo_revs_in_rangelist(merge_ranges, &hi_rev, &low_rev);
          /* Try to remove any non-inheritable ranges bound by the merge
             being performed. */
          SVN_ERR(svn_mergeinfo_inheritable(&merges, target_mergeinfo,
                                            rel_path, low_rev,
                                            hi_rev, pool));
          /* If any non-inheritable ranges were removed put them back as
             inheritable ranges. */
          SVN_ERR(svn_mergeinfo__equals(&is_equal, merges, target_mergeinfo,
                                        FALSE, pool));
          if (!is_equal)
            {
              SVN_ERR(svn_mergeinfo_merge(&merges, inheritable_merges,
                                          svn_rangelist_equal_inheritance,
                                          pool));
              SVN_ERR(svn_client__record_wc_mergeinfo(target_wcpath, merges,
                                                      adm_access, pool));
            }
        }
    }
  return SVN_NO_ERROR;
}

/* For shallow merges record the explicit *indirect* mergeinfo on the 
 * 1. merged files *merged* with a depth 'files'. 
 * 2. merged target directory *merged* with a depth 'immediates'.
 * i.e all subtrees which are going to get a 'inheritable merge range'
 * because of this 'shallow' merge should have the explicit mergeinfo
 * recorded on them.
*/
static svn_error_t *
record_mergeinfo_on_merged_children(svn_depth_t depth,
                                    svn_wc_adm_access_t *adm_access,
                                    notification_receiver_baton_t *notify_b,
                                    struct merge_cmd_baton *merge_b,
                                    apr_pool_t *pool)
{
  if ((depth != svn_depth_infinity) && notify_b->merged_paths)
    {
      svn_boolean_t indirect_child_mergeinfo = FALSE;
      apr_hash_index_t *hi;
      apr_hash_t *child_target_mergeinfo;
      const void *merged_path;

      for (hi = apr_hash_first(NULL, notify_b->merged_paths); hi;
           hi = apr_hash_next(hi))
        {
          const svn_wc_entry_t *child_entry;
          apr_hash_this(hi, &merged_path, NULL, NULL);
          SVN_ERR(svn_wc__entry_versioned(&child_entry, merged_path,
                                          adm_access, FALSE, pool));
          if (((child_entry->kind == svn_node_dir)
                && (strcmp(merge_b->target, merged_path) == 0)
                && (depth == svn_depth_immediates))
              || ((child_entry->kind == svn_node_file)
                   && (depth == svn_depth_files)))
            {
              /* Set the explicit inheritable mergeinfo for, 
               *  1. Merge target directory if depth is 
               *     'immediates'.
               *  2. If merge is on a file and requested depth 
               *     is 'files'.
               */
              SVN_ERR(svn_client__get_wc_or_repos_mergeinfo
                                      (&child_target_mergeinfo, child_entry,
                                       &indirect_child_mergeinfo,
                                       FALSE, svn_mergeinfo_inherited,
                                       merge_b->ra_session1, merged_path,
                                       adm_access, merge_b->ctx, pool));
              if (indirect_child_mergeinfo)
                SVN_ERR(svn_client__record_wc_mergeinfo(merged_path,
                                                        child_target_mergeinfo,
                                                        adm_access, pool));
            }
        }
    }
  return SVN_NO_ERROR;
}

/* URL1, URL2, and TARGET_WCPATH all better be directories.  For the
   single file case, the caller does the merging manually.

   TARGET_MISSING_CHILD indicates whether TARGET_WCPATH is missing any
   immediate children.  If TRUE this signifies that the mergeinfo resulting
   from the merge must be non-inheritable.

   Handle DEPTH as documented for svn_client_merge3().

   CHILDREN_WITH_MERGEINFO may contain child paths (svn_client__merge_path_t *)
   which are switched or which have mergeinfo which differs from that of the
   merge target root (ignored if empty or NULL).  CHILDREN_WITH_MERGEINFO
   list should have entries sorted in depth first order as mandated by the
   reporter API. Because of this, we drive the diff editor in such a way that
   it avoids merging child paths when a merge is driven for their parent path.

   MERGE_RANGES is an array of svn_merge_range_t *'s to be merged into
   TARGET_WCPATH and/or CHILDREN_WITH_MERGEINFO.

   CHILDREN_WITH_MERGEINFO may contain TARGET_WCPATH (which may be
   MERGE_B->TARGET), in that case TARGET_INDEX is the array index for
   TARGET_WCPATH, otherwise it should be set to a negative value.
*/
static svn_error_t *
do_merge(apr_array_header_t *merge_ranges,
         const char *url1,
         const char *url2,
         svn_boolean_t is_three_way_merge,
         svn_boolean_t target_missing_child,
         const char *target_wcpath,
         svn_wc_adm_access_t *adm_access,
         svn_depth_t depth,
         svn_boolean_t ignore_ancestry,
         const svn_wc_diff_callbacks2_t *callbacks,
         notification_receiver_baton_t *notify_b,
         struct merge_cmd_baton *merge_b,
         apr_array_header_t *children_with_mergeinfo,
         apr_pool_t *pool)
{
  svn_merge_range_t range;
  int i;

  /* Establish first RA session to URL1. */
  SVN_ERR(svn_client__open_ra_session_internal(&merge_b->ra_session1, url1,
                                               NULL, NULL, NULL, FALSE, TRUE,
                                               merge_b->ctx, pool));

  for (i = 0; i < merge_ranges->nelts; i++)
    {
      range.start =
        (APR_ARRAY_IDX(merge_ranges, i, svn_merge_range_t *))->start;
      range.end =
        (APR_ARRAY_IDX(merge_ranges, i, svn_merge_range_t *))->end;
      range.inheritable = ((!target_missing_child
                           && ((depth == svn_depth_infinity)
                               || (depth == svn_depth_immediates)))
                               ? TRUE : FALSE);


  /* When using this merge range, account for the exclusivity of
     its low value (which is indicated by this operation being a
     merge vs. revert). */

  if (!notify_b->same_urls)
    {
      svn_wc_notify_t *notify;
      notify = svn_wc_create_notify(target_wcpath, svn_wc_notify_merge_begin,
                                    pool);
      notification_receiver(notify_b, notify, pool);
    }

  /* ### TODO: Drill code to avoid merges for files which are
     ### already in conflict down into the API which requests or
     ### applies the diff. */

  SVN_ERR(drive_merge_report_editor(target_wcpath, url1, url2,
                                    children_with_mergeinfo, range.start,
                                    range.end,
                                    !is_three_way_merge
                                    && (range.start > range.end),
                                    depth,
                                    ignore_ancestry, notify_b, adm_access,
                                    callbacks, merge_b, pool));
    }

  /* Sleep to ensure timestamp integrity. */
  svn_sleep_for_timestamps();

  return SVN_NO_ERROR;
}

/* Helper for compact_merge_ranges.  Take *RANGES, an array of
   svn_merge_range_t *, and and remove any redundant ranges, possibly
   removing items from *RANGES.  *RANGES must be sorted per
   compare_merge_ranges() and is guaranteed to be sorted thusly
   upon completion.  All range in RANGES must also be of the same
   "direction" (additive or subtractive). */
static void
remove_redundant_ranges(apr_array_header_t **ranges)
{
  svn_merge_range_t *range_1 = NULL;
  svn_merge_range_t *range_2;
  int i;

  for (i = 0; i < (*ranges)->nelts; i++)
    {
      if (range_1 == NULL)
        {
          range_1 = APR_ARRAY_IDX(*ranges, i, svn_merge_range_t *);
          continue;
        }
      else
        {
          range_2 = APR_ARRAY_IDX(*ranges, i, svn_merge_range_t *);      
        }
      if (svn_range_compact(&range_1, &range_2))
        {
          if (!range_2)
            {
              /* Able to compact the two ranges into one.
                 Remove merge_ranges[i] and from array. */
              int j;
              for (j = i; j < (*ranges)->nelts - 1; j++)
                {
                  APR_ARRAY_IDX(*ranges, j, svn_merge_range_t *) =
                    APR_ARRAY_IDX(*ranges, j + 1, svn_merge_range_t *);
                }
              apr_array_pop(*ranges);
              i--; /* Reprocess this element */
            }
        }
    }
}

/* Helper for compact_merge_ranges.  SOURCES is array of svn_merge_range_t *
   sorted per compare_merge_ranges().  Remove any redundant ranges between
   adjacent ranges and store the result in *COMPACTED_RANGES, allocated out
   of pool.  The ranges in *COMPACTED_RANGES will remain sorted as per
   compare_merge_ranges.  Range in RANGES can be of either direction
   (additive and/or subtractive). */
static void
compact_add_sub_ranges(apr_array_header_t **compacted_sources,
                       apr_array_header_t *sources,
                       apr_pool_t *pool)
{
  int i;
  svn_merge_range_t *range_1 = NULL;
  svn_merge_range_t *range_2;
  apr_array_header_t *merge_ranges = apr_array_copy(pool, sources);

  for (i = 0; i < merge_ranges->nelts; i++)
    {
      if (range_1 == NULL)
        {
          range_1 = APR_ARRAY_IDX(merge_ranges, i, svn_merge_range_t *);
          continue;
        }
      else
        {
          range_2 = APR_ARRAY_IDX(merge_ranges, i, svn_merge_range_t *);      
        }

      if (svn_range_compact(&range_1, &range_2))
        {
          if (range_1 == NULL && range_2 == NULL) /* ranges cancel each out */
            {
              /* Remove merge_ranges[i] and merge_ranges[i + 1]
                 from the array. */
              int j;
              for (j = i - 1; j < merge_ranges->nelts - 2; j++)
                {
                  APR_ARRAY_IDX(merge_ranges, j, svn_merge_range_t *) =
                    APR_ARRAY_IDX(merge_ranges, j + 2, svn_merge_range_t *);
                }
              apr_array_pop(merge_ranges);
              apr_array_pop(merge_ranges);
              /* Make range_1 the last range processed if one exists. */
              if (i > 1)
                range_1 = APR_ARRAY_IDX(merge_ranges, i - 2,
                                        svn_merge_range_t *);
            }
          else if (!range_2) /* ranges compacted into range_ 1 */
            {
              /* Remove merge_ranges[i] and from array. */
              int j;
              for (j = i; j < merge_ranges->nelts - 1; j++)
                {
                  APR_ARRAY_IDX(merge_ranges, j, svn_merge_range_t *) =
                    APR_ARRAY_IDX(merge_ranges, j + 1, svn_merge_range_t *);
                }
              apr_array_pop(merge_ranges);

              i--; /* Reprocess merge_ranges[i] */
            }
          else /* ranges compacted */
            {
              range_1 = range_2;
            }
        } /* if (svn_range_compact(&range_1, &range_2)) */

    }

  *compacted_sources = merge_ranges;
}

/* SOURCES is array of svn_merge_range_t *sorted per compare_merge_ranges(). */
static svn_error_t *
compact_merge_ranges(apr_array_header_t **compacted_sources,
                     apr_array_header_t *merge_ranges,
                     apr_pool_t *pool)
{
  apr_array_header_t *additive_sources =
   apr_array_make(pool, 0, sizeof(svn_merge_range_t *));
  apr_array_header_t *subtractive_sources =
    apr_array_make(pool, 0, sizeof(svn_merge_range_t *));
  int i;

  for (i = 0; i < merge_ranges->nelts; i++)
    {
      svn_merge_range_t *range = svn_merge_range_dup(
        APR_ARRAY_IDX(merge_ranges, i, svn_merge_range_t *), pool);
      if (range->start > range->end)
        APR_ARRAY_PUSH(subtractive_sources, svn_merge_range_t *) = range;
      else
        APR_ARRAY_PUSH(additive_sources, svn_merge_range_t *) = range;
    }

  qsort(additive_sources->elts, additive_sources->nelts, additive_sources->elt_size,
        compare_merge_ranges);
  remove_redundant_ranges(&additive_sources);
  qsort(subtractive_sources->elts, subtractive_sources->nelts, subtractive_sources->elt_size,
        compare_merge_ranges);
  remove_redundant_ranges(&subtractive_sources);
  
  for (i = 0; i < subtractive_sources->nelts; i++)
    {
      svn_merge_range_t *range = svn_merge_range_dup(
        APR_ARRAY_IDX(subtractive_sources, i, svn_merge_range_t *), pool);
      APR_ARRAY_PUSH(additive_sources, svn_merge_range_t *) = range;
    }
  qsort(additive_sources->elts, additive_sources->nelts,
        additive_sources->elt_size, compare_merge_ranges);
  compact_add_sub_ranges(compacted_sources, additive_sources, pool);
  qsort((*compacted_sources)->elts, (*compacted_sources)->nelts,
        (*compacted_sources)->elt_size, compare_merge_ranges2);
  return SVN_NO_ERROR;
}

/* Get REVISION of the file at URL.  SOURCE is a path that refers to that
   file's entry in the working copy, or NULL if we don't have one.  Return in
   *FILENAME the name of a file containing the file contents, in *PROPS a hash
   containing the properties and in *REV the revision.  All allocation occurs
   in POOL. */
static svn_error_t *
single_file_merge_get_file(const char **filename,
                           svn_ra_session_t *ra_session,
                           apr_hash_t **props,
                           svn_revnum_t rev,
                           const char *wc_target,
                           apr_pool_t *pool)
{
  apr_file_t *fp;
  svn_stream_t *stream;

  /* ### Create this temporary file under .svn/tmp/ instead of next to
     ### the working file.*/
  SVN_ERR(svn_io_open_unique_file2(&fp, filename,
                                   wc_target, ".tmp",
                                   svn_io_file_del_none, pool));
  stream = svn_stream_from_aprfile2(fp, FALSE, pool);
  SVN_ERR(svn_ra_get_file(ra_session, "", rev,
                          stream, NULL, props, pool));
  SVN_ERR(svn_stream_close(stream));

  return SVN_NO_ERROR;
}


/* Send a notification specific to a single-file merge. */
static APR_INLINE void
single_file_merge_notify(void *notify_baton, const char *target_wcpath,
                         svn_wc_notify_action_t action,
                         svn_wc_notify_state_t text_state,
                         svn_wc_notify_state_t prop_state, apr_pool_t *pool)
{
  svn_wc_notify_t *notify = svn_wc_create_notify(target_wcpath, action, pool);
  notify->kind = svn_node_file;
  notify->content_state = text_state;
  notify->prop_state = prop_state;
  if (notify->content_state == svn_wc_notify_state_missing)
    notify->action = svn_wc_notify_skip;
  notification_receiver(notify_baton, notify, pool);
}


/* The single-file, simplified version of do_merge. */
static svn_error_t *
do_single_file_merge(apr_array_header_t *merge_sources,
                     const char *url1,
                     const char *url2,
                     svn_boolean_t is_three_way_merge,
                     const char *target_wcpath,
                     svn_wc_adm_access_t *adm_access,
                     notification_receiver_baton_t *notify_b,
                     struct merge_cmd_baton *merge_b,
                     svn_boolean_t ignore_ancestry,
                     apr_pool_t *pool)
{
  svn_error_t *err = SVN_NO_ERROR;
  apr_hash_t *props1, *props2;
  const char *tmpfile1, *tmpfile2;
  const char *mimetype1, *mimetype2;
  svn_string_t *pval;
  apr_array_header_t *propchanges, *remaining_ranges;
  svn_wc_notify_state_t prop_state = svn_wc_notify_state_unknown;
  svn_wc_notify_state_t text_state = svn_wc_notify_state_unknown;
  svn_client_ctx_t *ctx = merge_b->ctx;
  const char *rel_path;
  apr_hash_t *target_mergeinfo;
  const svn_wc_entry_t *entry;
  int i;
  svn_boolean_t indirect = FALSE, is_replace = FALSE;
  apr_pool_t *subpool;

  /* Ensure that the adm_access we're playing with is our TARGET_WCPATH's
     parent, as required by some of underlying helper functions. */
  SVN_ERR(svn_wc_adm_probe_try3(&adm_access, adm_access, target_wcpath,
                                TRUE, -1, merge_b->ctx->cancel_func,
                                merge_b->ctx->cancel_baton,
                                pool));

  SVN_ERR(svn_wc__entry_versioned(&entry, target_wcpath, adm_access, FALSE,
                                  pool));

  /* If we are not ignoring ancestry, then we need to check the
     relationship between the two sides of our merge.  Otherwise, just
     accept our input as-is. */
  if (! ignore_ancestry)
    {
      int j;
      for (j = 0; j < merge_sources->nelts; j++)
        {
          svn_merge_range_t *r = APR_ARRAY_IDX(merge_sources, j, svn_merge_range_t *);
          const char *location_url;
          svn_opt_revision_t *location_rev;
          svn_opt_revision_t unspecified_revision, rev1_opt, rev2_opt;
          unspecified_revision.kind = svn_opt_revision_unspecified;
          rev1_opt.value.number = r->start;
          rev1_opt.kind = svn_opt_revision_number;
          rev2_opt.value.number = r->end;
          rev2_opt.kind = svn_opt_revision_number;

          /* Try to locate the left side of the merge location by tracing the
             history of right side.  We do this only do verify that one of
             these locations is an ancestor of the other. */
          err = svn_client__repos_locations(&location_url, &location_rev,
                                            NULL, NULL,
                                            NULL,
                                            url2,
                                            &rev2_opt,
                                            &rev1_opt,
                                            &unspecified_revision,
                                            ctx, pool);

          /* If the two sides don't have an ancestral relationship, that's
             okay.  But because we are preserving ancestry, we have to
             treat a merge across those locations as a deletion of the one
             and addition of the other. */
          if (err && err->apr_err == SVN_ERR_CLIENT_UNRELATED_RESOURCES)
            {
              is_replace = TRUE;
              svn_error_clear(err);
              err = SVN_NO_ERROR;
            }
          SVN_ERR(err);
        }
    }

  /* Establish RA sessions to our URLs. */
  SVN_ERR(svn_client__open_ra_session_internal(&merge_b->ra_session1, url1,
                                               NULL, NULL, NULL, FALSE, TRUE,
                                               ctx, pool));
  SVN_ERR(svn_client__open_ra_session_internal(&merge_b->ra_session2, url2,
                                               NULL, NULL, NULL, FALSE, TRUE,
                                               ctx, pool));

  if (notify_b->same_urls && merge_b->same_repos)
    {
      /* Temporarily reparent ra_session1 to WC target URL. */
      SVN_ERR(svn_ra_reparent(merge_b->ra_session1, entry->url, pool));

      SVN_ERR(svn_client__get_wc_or_repos_mergeinfo(&target_mergeinfo, entry,
                                                    &indirect, FALSE,
                                                    svn_mergeinfo_inherited, 
                                                    merge_b->ra_session1,
                                                    target_wcpath, adm_access,
                                                    ctx, pool));

      /* Reparent ra_session1 back to URL1. */
      SVN_ERR(svn_ra_reparent(merge_b->ra_session1, url1, pool));

      SVN_ERR(svn_client__path_relative_to_root(&rel_path, url1, NULL,
                                                merge_b->ra_session1,
                                                adm_access, pool));
      SVN_ERR(calculate_remaining_ranges(&remaining_ranges, merge_sources,
                                         target_mergeinfo, is_three_way_merge,
                                         rel_path, url1,
                                         merge_b->ra_session1,
                                         entry, ctx, pool));
    }
  else
    {
        remaining_ranges = merge_sources;
    }

  subpool = svn_pool_create(pool);

  for (i = 0; i < remaining_ranges->nelts; i++)
    {
      svn_wc_notify_t *n;

      /* When using this merge range, account for the exclusivity of
         its low value (which is indicated by this operation being a
         merge vs. revert). */
      svn_merge_range_t *r = APR_ARRAY_IDX(remaining_ranges, i,
                                           svn_merge_range_t *);

      svn_pool_clear(subpool);

      n = svn_wc_create_notify(target_wcpath,
                               svn_wc_notify_merge_begin,
                               subpool);
      if (notify_b->same_urls)
        n->merge_range = r;
      notification_receiver(notify_b, n, subpool);

      /* While we currently don't allow it, in theory we could be
         fetching two fulltexts from two different repositories here. */
      SVN_ERR(single_file_merge_get_file(&tmpfile1, merge_b->ra_session1,
                                         &props1, r->start, target_wcpath,
                                         subpool));
      SVN_ERR(single_file_merge_get_file(&tmpfile2, merge_b->ra_session2,
                                         &props2, r->end, target_wcpath,
                                         subpool));

      /* Discover any svn:mime-type values in the proplists */
      pval = apr_hash_get(props1, SVN_PROP_MIME_TYPE,
                          strlen(SVN_PROP_MIME_TYPE));
      mimetype1 = pval ? pval->data : NULL;

      pval = apr_hash_get(props2, SVN_PROP_MIME_TYPE,
                          strlen(SVN_PROP_MIME_TYPE));
      mimetype2 = pval ? pval->data : NULL;

      /* Deduce property diffs. */
      SVN_ERR(svn_prop_diffs(&propchanges, props2, props1, subpool));

      if (is_replace)
        {
          /* Delete... */
          SVN_ERR(merge_file_deleted(adm_access,
                                     &text_state,
                                     target_wcpath,
                                     NULL,
                                     NULL,
                                     mimetype1, mimetype2,
                                     props1,
                                     merge_b));
          single_file_merge_notify(notify_b, target_wcpath,
                                   svn_wc_notify_update_delete, text_state,
                                   svn_wc_notify_state_unknown, subpool);

          /* ...plus add... */
          SVN_ERR(merge_file_added(adm_access,
                                   &text_state, &prop_state,
                                   target_wcpath,
                                   tmpfile1,
                                   tmpfile2,
                                   r->start,
                                   r->end,
                                   mimetype1, mimetype2,
                                   propchanges, props1,
                                   merge_b));
          single_file_merge_notify(notify_b, target_wcpath,
                                   svn_wc_notify_update_add, text_state,
                                   prop_state, subpool);
          /* ... equals replace. */
        }
      else
        {
          SVN_ERR(merge_file_changed(adm_access,
                                     &text_state, &prop_state,
                                     target_wcpath,
                                     tmpfile1,
                                     tmpfile2,
                                     r->start,
                                     r->end,
                                     mimetype1, mimetype2,
                                     propchanges, props1,
                                     merge_b));
          single_file_merge_notify(notify_b, target_wcpath,
                                   svn_wc_notify_update_update, text_state,
                                   prop_state, subpool);
        }

      /* Ignore if temporary file not found. It may have been renamed. */
      /* (This is where we complain about missing Lisp, or better yet,
         Python...) */
      err = svn_io_remove_file(tmpfile1, subpool);
      if (err && ! APR_STATUS_IS_ENOENT(err->apr_err))
        return err;
      svn_error_clear(err);
      err = SVN_NO_ERROR;
      err = svn_io_remove_file(tmpfile2, subpool);
      if (err && ! APR_STATUS_IS_ENOENT(err->apr_err))
        return err;
      svn_error_clear(err);
      err = SVN_NO_ERROR;

      if (notify_b->same_urls)
        {
          if (!merge_b->dry_run && merge_b->same_repos)
            {
              /* Update the WC mergeinfo here to account for our new
                 merges, minus any unresolved conflicts and skips. */
              apr_hash_t *merges;
              SVN_ERR(determine_merges_performed(&merges, target_wcpath,
                                                 merge_sources,
                                                 svn_depth_infinity,
                                                 adm_access, notify_b, merge_b,
                                                 subpool));
              /* If this whole merge was simply a no-op merge to a file then
                 we don't touch the local mergeinfo. */
              if (merge_b->operative_merge)
                {
                  /* If merge target has indirect mergeinfo set it before
                     recording the first merge range. */
                  if (i == 0 && indirect)
                    SVN_ERR(svn_client__record_wc_mergeinfo(target_wcpath,
                                                            target_mergeinfo,
                                                            adm_access,
                                                            subpool));

                  SVN_ERR(update_wc_mergeinfo(target_wcpath, entry, rel_path,
                                              merges, is_three_way_merge,
                                              adm_access, ctx, subpool));
                }
            }

          /* Clear the notification counter and list of skipped paths
             in preparation for the next revision range merge. */
          notify_b->nbr_notifications = 0;
          if (notify_b->skipped_paths != NULL)
            svn_hash__clear(notify_b->skipped_paths);
          if (notify_b->merged_paths != NULL)
            svn_hash__clear(notify_b->merged_paths);
        }

      if (i < remaining_ranges->nelts - 1 &&
          is_path_conflicted_by_merge(merge_b))
        {
          err = make_merge_conflict_error(target_wcpath, r, pool);
          break;
        }
    }

  apr_pool_destroy(subpool);

  /* Sleep to ensure timestamp integrity. */
  svn_sleep_for_timestamps();

  return err;
}

/* A baton for get_mergeinfo_walk_cb. */
struct get_mergeinfo_walk_baton
{
  /* Access for the tree being walked. */
  svn_wc_adm_access_t *base_access;
  /* Array of paths that have explicit mergeinfo and/or are switched. */
  apr_array_header_t *children_with_mergeinfo;
  /* Merge source canonical path. */
  const char* merge_src_canon_path;
  /* Merge target path. */
  const char* merge_target_path;
  /* merge depth requested. */
  svn_depth_t depth;
};


/* svn_wc_entry_callbacks2_t found_entry() callback for get_mergeinfo_paths.

   Given PATH, its corresponding ENTRY, and WB, where WB is the WALK_BATON
   of type "struct get_mergeinfo_walk_baton *":  If PATH is switched,
   has explicit working svn:mergeinfo from a corresponding merge source, is
   missing a child due to a sparse checkout, or is absent from disk, then
   create a svn_client__merge_path_t * representing *PATH, allocated in
   WB->CHILDREN_WITH_MERGEINFO->POOL, and push it onto the
   WB->CHILDREN_WITH_MERGEINFO array. */
static svn_error_t *
get_mergeinfo_walk_cb(const char *path,
                      const svn_wc_entry_t *entry,
                      void *walk_baton,
                      apr_pool_t *pool)
{
  struct get_mergeinfo_walk_baton *wb = walk_baton;
  const svn_string_t *propval;
  apr_hash_t *mergehash;
  svn_boolean_t switched = FALSE;
  svn_boolean_t has_mergeinfo_from_merge_src = FALSE;
  const char *parent_path = svn_path_dirname(path, pool);

  /* We're going to receive dirents twice;  we want to ignore the
     first one (where it's a child of a parent dir), and only use
     the second one (where we're looking at THIS_DIR).  The exception
     is absent dirs, these only come through once, so continue. */
  if ((entry->kind == svn_node_dir)
      && (strcmp(entry->name, SVN_WC_ENTRY_THIS_DIR) != 0)
      && !entry->absent)
    return SVN_NO_ERROR;

  /* Ignore the entry if it does not exist at the time of interest. */
  if (entry->schedule == svn_wc_schedule_delete || entry->deleted)
    return SVN_NO_ERROR;

  if (entry->absent)
    {
      propval = NULL;
      switched = FALSE;
    }
  else
    {
      SVN_ERR(svn_wc_prop_get(&propval, SVN_PROP_MERGE_INFO, path,
                              wb->base_access, pool));
      if (propval)
        {
          const char* path_relative_to_merge_target;
          int merge_target_len;
          svn_stringbuf_t *merge_src_child_path =
                          svn_stringbuf_create(wb->merge_src_canon_path, pool);
          /* Note: Merge target is an empty string for '' and explicit '.'.
           * Such relative merge targets makes path entries to be relative
           * to current directory and hence for merge src '/trunk'
           * "path of value 'subdir'" can cause merge_src_child_path to
           * '/trunksubdir' instead of '/trunk/subdir'.
           * For such merge targets insert '/' between merge_src_canon_path
           * and path_relative_to_merge_target.
           */
          merge_target_len = strlen(wb->merge_target_path);
          /* Need to append '/' only for subtrees. */
          if (!merge_target_len && strcmp(path, wb->merge_target_path) != 0)
            svn_stringbuf_appendbytes(merge_src_child_path, "/", 1);
          path_relative_to_merge_target = path + merge_target_len;
          svn_stringbuf_appendbytes(merge_src_child_path,
                                    path_relative_to_merge_target,
                                    strlen(path_relative_to_merge_target));

          SVN_ERR(svn_mergeinfo_parse(&mergehash, propval->data, pool));
          if (apr_hash_get(mergehash, merge_src_child_path->data,
                           APR_HASH_KEY_STRING))
            has_mergeinfo_from_merge_src = TRUE;
        }
      /* Regardless of whether PATH has explicit mergeinfo or not, we must
         determine if PATH is switched.  This is so get_mergeinfo_paths()
         can later tweak PATH's parent to reflect a missing child (implying it
         needs non-inheritable mergeinfo ranges) and PATH's siblings so they
         get their own complete set of mergeinfo. */
      SVN_ERR(svn_wc__path_switched(path, &switched, entry, pool));
    }

  /* Store PATHs with explict mergeinfo, which are switched, are missing
     children due to a sparse checkout, and/or are absent from the WC,
     first level sub directory relative to merge target if depth is
     immediates. */
  if (has_mergeinfo_from_merge_src
      || switched
      || entry->depth == svn_depth_empty
      || entry->depth == svn_depth_files
      || entry->absent
      || ((wb->depth == svn_depth_immediates) && 
          (entry->kind == svn_node_dir) &&
          (strcmp(parent_path, path) != 0) &&
          (strcmp(parent_path, wb->merge_target_path) == 0)))
    {
      svn_client__merge_path_t *child = 
        apr_palloc(wb->children_with_mergeinfo->pool, sizeof(*child));
      child->path = apr_pstrdup(wb->children_with_mergeinfo->pool, path);
      child->missing_child = (entry->depth == svn_depth_empty
                              || entry->depth == svn_depth_files
                              || ((wb->depth == svn_depth_immediates) &&
                                  (entry->kind == svn_node_dir) &&
                                  (strcmp(parent_path, 
                                          wb->merge_target_path) == 0)))
                              ? TRUE : FALSE;
      child->switched = switched;
      child->absent = entry->absent;
      if (propval)
        {
          if (strstr(propval->data, SVN_MERGEINFO_NONINHERITABLE_STR))
            child->has_noninheritable = TRUE;
          else
            child->has_noninheritable = FALSE;
        }
      else
        child->has_noninheritable = FALSE;

      /* A little trickery: If PATH doesn't have any mergeinfo or has
         only inheritable mergeinfo, we still describe it as having
         non-inheritable mergeinfo if it is missing a child.  Why?  Because
         the mergeinfo we'll add to PATH as a result of the merge will need
         to be non-inheritable (since PATH is missing children) and doing
         this now allows get_mergeinfo_paths() to properly account for PATH's
         other children. */
      if (!child->has_noninheritable
          && (entry->depth == svn_depth_empty
              || entry->depth == svn_depth_files))
      child->has_noninheritable = TRUE;

      APR_ARRAY_PUSH(wb->children_with_mergeinfo, 
                     svn_client__merge_path_t *) = child;
    }

  return SVN_NO_ERROR;
}

/* svn_wc_entry_callbacks2_t handle_error() callback for
   get_mergeinfo_paths().

   Squelch ERR by returning SVN_NO_ERROR if ERR is caused by a missing
   path (i.e. SVN_ERR_WC_PATH_NOT_FOUND) or an unversioned path
   (i.e. SVN_ERR_WC_NOT_LOCKED). */
static svn_error_t *
get_mergeinfo_error_handler(const char *path,
                            svn_error_t *err,
                            void *walk_baton,
                            apr_pool_t *pool)
{
  svn_error_t *root_err = svn_error_root_cause(err);
  if (root_err == SVN_NO_ERROR)
    return err;

  switch (root_err->apr_err)
    {
    case SVN_ERR_WC_PATH_NOT_FOUND:
    case SVN_ERR_WC_NOT_LOCKED:
      svn_error_clear(err);
      return SVN_NO_ERROR;

    default:
      return err;
    }
}

/* Helper for get_mergeinfo_paths()

   CHILDREN_WITH_MERGEINFO is a depth first sorted array filled with
   svn_client__merge_path_t *.  Starting at the element in
   CHILDREN_WITH_MERGEINFO located at START_INDEX look for that
   element's child/parent (as indicated by LOOKING_FOR_CHILD) named
   PATH. If the child/parent is found, set *CHILD_OR_PARENT to that
   element and return the index at which if was found.  If the
   child/parent is not found set *CHILD_OR_PARENT to NULL and return
   the index at which it should be inserted. */
static int
find_child_or_parent(apr_array_header_t *children_with_mergeinfo,
                     svn_client__merge_path_t **child_or_parent,
                     const char *path,
                     svn_boolean_t looking_for_child,
                     int start_index,
                     apr_pool_t *pool)
{
  int j = 0;
  *child_or_parent = NULL;

  /* If possible, search forwards in the depth first sorted array
     to find a child PATH or backwards to find a parent PATH. */
  if (start_index >= 0 && start_index < children_with_mergeinfo->nelts)
    {
      for (j = looking_for_child ? start_index + 1 : start_index;
           looking_for_child ? j < children_with_mergeinfo->nelts : j >= 0;
           j = looking_for_child ? j + 1 : j - 1)
        {
          /* If this potential child is neither the child we are looking for
             or another one of PARENT's children then CHILD_PATH doesn't
             exist in CHILDREN_WITH_MERGEINFO. */
          svn_client__merge_path_t *potential_child_or_parent =
            APR_ARRAY_IDX(children_with_mergeinfo, j, 
                          svn_client__merge_path_t *);
          int cmp = svn_path_compare_paths(path,
                                           potential_child_or_parent->path);
          if (cmp == 0)
            {
              /* Found child or parent. */
              *child_or_parent = potential_child_or_parent;
              break;
            }
          else if ((looking_for_child && cmp < 0)
                   || (!looking_for_child && cmp > 0))
            {
              /* PATH doesn't exist, but found where it should be inserted. */
              if (!looking_for_child)
                j++;
              break;
            }
          else if (!looking_for_child && j == 0)
            {
              /* Looking for a parent but are at start of the array so we know
                 where to insert the parent. */
              break;
            }
          /* else we are looking for a child but found one of its
             siblings...keep looking. */
        }
    }
  return j;
}

/* Helper for get_mergeinfo_paths()

   CHILDREN_WITH_MERGEINFO is a depth first sorted array filled with
   svn_client__merge_path_t *.  Insert INSERT_ELEMENT into the
   CHILDREN_WITH_MERGEINFO array at index INSERT_INDEX. */
static void
insert_child_to_merge(apr_array_header_t *children_with_mergeinfo,
                      svn_client__merge_path_t *insert_element,
                      int insert_index)
{
  if (insert_index == children_with_mergeinfo->nelts)
    {
      APR_ARRAY_PUSH(children_with_mergeinfo, 
                     svn_client__merge_path_t *) = insert_element;
    }
  else
    {
      /* Copy the last element of CHILDREN_WITH_MERGEINFO and add it to the
         end of the array. */
      int j;
      svn_client__merge_path_t *curr = 
        APR_ARRAY_IDX(children_with_mergeinfo,
                      children_with_mergeinfo->nelts - 1,
                      svn_client__merge_path_t *);
      svn_client__merge_path_t *curr_copy = 
        apr_palloc(children_with_mergeinfo->pool, sizeof(*curr_copy));

      *curr_copy = *curr;
      APR_ARRAY_PUSH(children_with_mergeinfo, 
                     svn_client__merge_path_t *) = curr_copy;

      /* Move all elements from INSERT_INDEX to the end of the array forward
         one spot then insert the new element. */
      for (j = children_with_mergeinfo->nelts - 2; j >= insert_index; j--)
        {
          svn_client__merge_path_t *prev;
          curr = APR_ARRAY_IDX(children_with_mergeinfo, j, 
                               svn_client__merge_path_t *);
          if (j == insert_index)
            *curr = *insert_element;
          else
            {
              prev = APR_ARRAY_IDX(children_with_mergeinfo, j - 1,
                                   svn_client__merge_path_t *);
              *curr = *prev;
            }
        }
    }
}

/* Helper for get_mergeinfo_paths()'s qsort() call. */
static int
compare_merge_path_t_as_paths(const void *a,
                              const void *b)
{
  svn_client__merge_path_t *child1 = *((svn_client__merge_path_t * const *) a);
  svn_client__merge_path_t *child2 = *((svn_client__merge_path_t * const *) b);

  return svn_path_compare_paths(child1->path, child2->path);
}

/* Helper for get_mergeinfo_paths(). 
 * If CHILD->PATH is switched or absent make sure its
 * parent is marked as missing a child.
 * Start looking up for parent from *CURR_INDEX in CHILDREN_WITH_MERGEINFO.
 * Create the parent and insert it into CHILDREN_WITH_MERGEINFO if necessary
 * (and increment *CURR_INDEX so that caller don't process the inserted 
 *  element).
 * Also ensure that CHILD->PATH's
 * siblings which are not already present in CHILDREN_WITH_MERGEINFO
 * are also added to the array. Use POOL for all temporary allocations.*/
static svn_error_t *
insert_parent_and_siblings_of_switched_or_absent_entry(
                                   apr_array_header_t *children_with_mergeinfo,
                                   struct merge_cmd_baton *merge_cmd_baton,
                                   int *curr_index,
                                   svn_client__merge_path_t *child,
                                   svn_wc_adm_access_t *adm_access,
                                   apr_pool_t *pool)
{
  svn_client__merge_path_t *parent;
  const char *parent_path = svn_path_dirname(child->path, pool);
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  svn_wc_adm_access_t *parent_access;
  int insert_index, parent_index;

  if (!(child->absent
          || (child->switched
              && strcmp(merge_cmd_baton->target, child->path) != 0)))
    return SVN_NO_ERROR;

  parent_index = find_child_or_parent(children_with_mergeinfo, &parent,
                                      parent_path, FALSE, *curr_index, pool);
  if (parent)
    {
      parent->missing_child = TRUE;
    }
  else
    {
      /* Create a new element to insert into CHILDREN_WITH_MERGEINFO. */
      parent = apr_palloc(children_with_mergeinfo->pool, sizeof(*parent));
      parent->path = apr_pstrdup(children_with_mergeinfo->pool, parent_path);
      parent->missing_child = TRUE;
      parent->switched = FALSE;
      parent->has_noninheritable = FALSE;
      parent->absent = FALSE;
      /* Insert PARENT into CHILDREN_WITH_MERGEINFO. */
      insert_child_to_merge(children_with_mergeinfo, parent, parent_index);
      /* Increment for loop index so we don't process the inserted element. */
      (*curr_index)++;
    } /*(parent == NULL) */

  /* Add all of PARENT's non-missing children that are not already present.*/
  SVN_ERR(svn_wc_adm_probe_try3(&parent_access, adm_access, parent->path,
                                TRUE, -1, merge_cmd_baton->ctx->cancel_func,
                                merge_cmd_baton->ctx->cancel_baton, pool));
  SVN_ERR(svn_wc_entries_read(&entries, parent_access, FALSE, pool));
  for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      svn_client__merge_path_t *sibling_of_missing;
      const char *child_path;

      apr_hash_this(hi, &key, NULL, NULL);

      if (strcmp(key, SVN_WC_ENTRY_THIS_DIR) == 0)
        continue;

      /* Does this child already exist in CHILDREN_WITH_MERGEINFO? */
      child_path = svn_path_join(parent->path, key, pool);
      insert_index = find_child_or_parent(children_with_mergeinfo,
                                          &sibling_of_missing, child_path,
                                          TRUE, parent_index, pool);
      /* Create the missing child and insert it into CHILDREN_WITH_MERGEINFO.*/
      if (!sibling_of_missing)
        {
          sibling_of_missing = apr_palloc(children_with_mergeinfo->pool,
                                          sizeof(*sibling_of_missing));
          sibling_of_missing->path = apr_pstrdup(children_with_mergeinfo->pool,
                                                 child_path);
          sibling_of_missing->missing_child = FALSE;
          sibling_of_missing->switched = FALSE;
          sibling_of_missing->has_noninheritable = FALSE;
          sibling_of_missing->absent = FALSE;
          insert_child_to_merge(children_with_mergeinfo, sibling_of_missing,
                                insert_index);
        }
    }
  return SVN_NO_ERROR;
}
/* Helper for discover_and_merge_children()

   Perform a depth first walk of the working copy tree rooted at
   TARGET (with the corresponding ENTRY).  Create an
   svn_client__merge_path_t for any path which meets one or more of
   the following criteria:

     1) Path has working svn:mergeinfo from corresponding merge source.
     2) Path is switched.
     3) Path has no mergeinfo of it's own but it's parent has mergeinfo with
        non-inheritable ranges (in this case the function will actually set
        override mergeinfo on the path if this isn't a dry-run and the merge
        is between differences in the same repository).
     4) Path has an immediate child (or children) missing from the WC because
        the child is switched or absent from the WC, or due to a sparse
        checkout.
     5) Path has a sibling (or siblings) missing from the WC because the
        sibling is switched or absent, or missing due to a sparse checkout.
     6) Path is absent from disk due to an authz restriction.

   Criteria 4 and 5 are handled by
   'insert_parent_and_siblings_of_switched_or_absent_entry'.  Store
   the svn_client__merge_path_t's in *CHILDREN_WITH_MERGEINFO.
   *CHILDREN_WITH_MERGEINFO is guaranteed to be in depth-first order
   based on the svn_client__merge_path_t *s path member.  Cascade
   MERGE_SRC_CANON_PATH. 
   *CHILDREN_WITH_MERGEINFO is guaranteed to be non-empty with its first 
   member being 'target' itself. */
static svn_error_t *
get_mergeinfo_paths(apr_array_header_t *children_with_mergeinfo,
                    struct merge_cmd_baton *merge_cmd_baton,
                    const char *target, const char* merge_src_canon_path,
                    const svn_wc_entry_t *entry,
                    svn_wc_adm_access_t *adm_access,
                    svn_client_ctx_t *ctx,
                    svn_depth_t depth,
                    apr_pool_t *pool)
{
  int i;
  apr_pool_t *iterpool;
  static const svn_wc_entry_callbacks2_t walk_callbacks =
    { get_mergeinfo_walk_cb, get_mergeinfo_error_handler };
  struct get_mergeinfo_walk_baton wb =
    { adm_access, children_with_mergeinfo, 
      merge_src_canon_path, target, depth};

  /* Cover cases 1), 2), and 6) by walking the WC to get all paths which have
     mergeinfo and/or are switched or are absent from disk. */
  if (entry->kind == svn_node_file)
    SVN_ERR(walk_callbacks.found_entry(merge_cmd_baton->target, entry, &wb,
                                       pool));
  else
    SVN_ERR(svn_wc_walk_entries3(merge_cmd_baton->target, adm_access,
                                 &walk_callbacks, &wb, depth, TRUE,
                                 merge_cmd_baton->ctx->cancel_func,
                                 merge_cmd_baton->ctx->cancel_baton,
                                 pool));

  /* CHILDREN_WITH_MERGEINFO must be in depth first order, but
     svn_wc_walk_entries3() relies on svn_wc_entries_read() which means the
     paths at a given directory level are not in any particular order.  Also,
     we may need to add elements to the array to cover case 3) through 5) from
     the docstring.  If so, it is more efficient to find and insert these
     paths if the sibling paths are in a guaranteed depth-first order.  For
     the first reason we sort the array, for the second reason we do it now
     rather than at the end of this function. */
  qsort(children_with_mergeinfo->elts,
        children_with_mergeinfo->nelts,
        children_with_mergeinfo->elt_size,
        compare_merge_path_t_as_paths);

  iterpool = svn_pool_create(pool);
  for (i = 0; i < children_with_mergeinfo->nelts; i++)
    {
      int insert_index;
      svn_client__merge_path_t *child = 
        APR_ARRAY_IDX(children_with_mergeinfo, i, svn_client__merge_path_t *);
      svn_pool_clear(iterpool);

      /* Case 3) Where merging to a path with a switched child the path gets
         non-inheritable mergeinfo for the merge range performed and the child
         gets it's own set of mergeinfo.  If the switched child later
         "returns", e.g. a switched path is unswitched, the child may not have
         any explicit mergeinfo.  If the initial merge is repeated we don't
         want to repeat the merge for the path, but we do want to repeat it
         for the previously switched child.  To ensure this we check if all
         of CHILD's non-missing children have explicit mergeinfo (they should
         already be present in CHILDREN_WITH_MERGEINFO if they do).  If not,
         add the children without mergeinfo to CHILDREN_WITH_MERGEINFO so
         discover_and_merge_children() will merge them independently.

         But that's not enough!  Since discover_and_merge_children() performs
         the merges on the paths in CHILDREN_WITH_MERGEINFO in a depth first
         manner it will merge the previously switched path's parent first.  As
         part of this merge it will update the parent's previously
         non-inheritable mergeinfo and make it inheritable (since it notices
         the path has no missing children), then when
         discover_and_merge_children() finally merges the previously missing
         child it needs to get mergeinfo from the child's nearest ancestor,
         but since discover_and_merge_children() already tweaked that
         mergeinfo, removing the non-inheritable flag, it appears that the
         child already has been merged to.  To prevent this we set override
         mergeinfo on the child now, before any merging is done, so it has
         explicit mergeinfo that reflects only CHILD's inheritable mergeinfo. */
      if (child->has_noninheritable)
        {
          apr_hash_t *entries;
          apr_hash_index_t *hi;
          svn_wc_adm_access_t *child_access;
          SVN_ERR(svn_wc_adm_probe_try3(&child_access, adm_access,
                                        child->path, TRUE, -1,
                                        merge_cmd_baton->ctx->cancel_func,
                                        merge_cmd_baton->ctx->cancel_baton,
                                        iterpool));
          SVN_ERR(svn_wc_entries_read(&entries, child_access, FALSE,
                                      iterpool));
          for (hi = apr_hash_first(iterpool, entries); hi;
               hi = apr_hash_next(hi))
            {
              const void *key;
              svn_client__merge_path_t *child_of_noninheritable;
              const char *child_path;

              apr_hash_this(hi, &key, NULL, NULL);

              if (strcmp(key, SVN_WC_ENTRY_THIS_DIR) == 0)
                continue;

              /* Does this child already exist in CHILDREN_WITH_MERGEINFO?  If
                 not, create it and insert it into CHILDREN_WITH_MERGEINFO and
                 set override mergeinfo on it. */
              child_path = svn_path_join(child->path, key, iterpool);
              insert_index = find_child_or_parent(children_with_mergeinfo,
                                                  &child_of_noninheritable,
                                                  child_path, TRUE, i,
                                                  iterpool);
              if (!child_of_noninheritable)
                {
                  child_of_noninheritable =
                    apr_palloc(children_with_mergeinfo->pool,
                               sizeof(*child_of_noninheritable));
                  child_of_noninheritable->path =
                    apr_pstrdup(children_with_mergeinfo->pool, child_path);
                  child_of_noninheritable->missing_child = FALSE;
                  child_of_noninheritable->switched = FALSE;
                  child_of_noninheritable->has_noninheritable = FALSE;
                  child_of_noninheritable->absent = FALSE;
                  insert_child_to_merge(children_with_mergeinfo,
                                        child_of_noninheritable,
                                        insert_index);
                  if (!merge_cmd_baton->dry_run
                      && merge_cmd_baton->same_repos)
                    {
                      svn_boolean_t inherited;
                      apr_hash_t *mergeinfo;
                      SVN_ERR(svn_client__get_wc_mergeinfo
                              (&mergeinfo, &inherited, FALSE,
                               svn_mergeinfo_nearest_ancestor,
                               entry, child_of_noninheritable->path,
                               merge_cmd_baton->target, NULL, adm_access,
                               merge_cmd_baton->ctx, iterpool));
                      SVN_ERR(svn_client__record_wc_mergeinfo(
                        child_of_noninheritable->path, mergeinfo, adm_access,
                        iterpool));
                    }
                }
            }
        }
      /* Case 4 and Case 5 are handled by the following function.*/
      SVN_ERR(insert_parent_and_siblings_of_switched_or_absent_entry(
                                                      children_with_mergeinfo,
                                                      merge_cmd_baton, &i,
                                                      child, adm_access,
                                                      iterpool));
    } /* i < children_with_mergeinfo->nelts */

  /* Push default target */
  if (children_with_mergeinfo->nelts == 0)
    {
      svn_client__merge_path_t *item = 
                      apr_palloc(children_with_mergeinfo->pool, sizeof(*item));
      item->path = apr_pstrdup(children_with_mergeinfo->pool,
                               merge_cmd_baton->target);
      item->missing_child = (entry->depth == svn_depth_empty 
                             || entry->depth == svn_depth_files)
                             ? TRUE : FALSE;
      item->switched = FALSE;
      item->absent = FALSE;
      item->has_noninheritable = FALSE;
      if (item->missing_child)
        item->has_noninheritable = TRUE;
      APR_ARRAY_PUSH(children_with_mergeinfo,
                     svn_client__merge_path_t *) = item;
    }
  else
    {
      /* See whether children_with_mergeinfo has target itself,
         if it does not add the same if it has indirect mergeinfo */
      svn_client__merge_path_t *item = 
         APR_ARRAY_IDX(children_with_mergeinfo, 0, svn_client__merge_path_t *);
      if (strcmp(item->path, merge_cmd_baton->target) != 0)
        {
          svn_client__merge_path_t *target_item = 
               apr_palloc(children_with_mergeinfo->pool, sizeof(*target_item));
          target_item->path = apr_pstrdup(children_with_mergeinfo->pool,
                                          merge_cmd_baton->target);
          target_item->missing_child = (entry->depth == svn_depth_empty
                                        || entry->depth == svn_depth_files)
                                        ? TRUE : FALSE;
          target_item->switched = FALSE;
          target_item->absent = FALSE;
          target_item->has_noninheritable = FALSE;
          if (target_item->missing_child)
            target_item->has_noninheritable = TRUE;
          insert_child_to_merge(children_with_mergeinfo, target_item, 0);
        }
    }
  svn_pool_destroy(iterpool);
  return SVN_NO_ERROR;
}


/* Fill CHILDREN_WITH_MERGEINFO with child paths
   (const svn_client__merge_path_t *) which might have intersecting merges
   because they meet one or more of the criteria
   described in get_mergeinfo_paths(). Here the paths are arranged in a depth
   first order. Use PARENT_ENTRY and ADM_ACCESS to fill 
   CHILDREN_WITH_MERGEINFO.
   Cascade PARENT_MERGE_SOURCE_URL, REV1, REV2, DEPTH,
   IGNORE_ANCESTRY, ADM_ACCESS, IS_ROLLBACK and MERGE_B to do_merge().
   All allocation occurs in POOL.

   From PARENT_MERGE_SOURCE_URL and WC_ROOT_URL deduce the
   MERGE_SRC_CANON_PATH.

   For each item in CHILDREN_WITH_MERGEINFO it calculates remaining revision
   ranges to merge.
   
   From the remaining ranges of each item in CHILDREN_WITH_MERGEINFO,
   it picks the smallest end_rev(for forward merge) 
   or biggest end_rev(for rollback merge).

   MERGE_ONE_RANGE: RETURN IF END_REV == SVN_INVALID_REVNUM
   Slices each remaining_ranges[0] around this 'end_rev'.
   Starts with start_rev = REV1, Calls do_merge on MERGE_B->target for 
   start_rev:end_rev. 
   Removes the first item from each remaining_ranges.
   Sets start_rev=end_rev and picks the next end_rev, repeats this whole 
   process(MERGE_ONE_RANGE) 

   Records the mergeinfo.

   Note that any paths in CHILDREN_WITH_MERGEINFO which were switched
   but had no explicit working mergeinfo at the start of the call, will have
   some at the end of it if merge is not a no-op merge.
*/
static svn_error_t *
discover_and_merge_children(const svn_wc_entry_t *parent_entry,
                            const char *parent_merge_source_url,
                            const char *wc_root_url,
                            apr_array_header_t *merge_ranges,
                            svn_depth_t depth,
                            svn_boolean_t ignore_ancestry,
                            svn_wc_adm_access_t *adm_access,
                            notification_receiver_baton_t *notify_b,
                            struct merge_cmd_baton *merge_b,
                            apr_pool_t *pool)
{
  svn_error_t *err = SVN_NO_ERROR;
  apr_array_header_t *children_with_mergeinfo;
  int merge_target_len = strlen(merge_b->target);
  int i;
  svn_ra_session_t *ra_session;
  svn_revnum_t start_rev, end_rev;
  apr_pool_t *iterpool;
  svn_client__merge_path_t *target_merge_path_t;
  const char *parent_merge_src_canon_path;
  apr_array_header_t *merge_source =
    apr_array_make(pool, 1, sizeof(svn_merge_range_t *));
  svn_boolean_t ranges_are_inheritable;

  if (strcmp(parent_merge_source_url, wc_root_url) == 0)
    parent_merge_src_canon_path = apr_pstrdup(pool, "/");
  else
    parent_merge_src_canon_path = apr_pstrdup(pool,
                                parent_merge_source_url + strlen(wc_root_url));
  SVN_ERR(svn_client__open_ra_session_internal(&ra_session,
                                               parent_merge_source_url, NULL,
                                               NULL, NULL, FALSE, TRUE,
                                               merge_b->ctx, pool));
  children_with_mergeinfo = 
    apr_array_make(pool, 0, sizeof(svn_client__merge_path_t *));
  SVN_ERR(get_mergeinfo_paths(children_with_mergeinfo,
                              merge_b, merge_b->target,
                              parent_merge_src_canon_path,
                              parent_entry, adm_access,
                              merge_b->ctx, depth, pool));
  notify_b->children_with_mergeinfo = children_with_mergeinfo;

  /* First item from the CHILDREN_WITH_MERGEINFO is the target
   * due to *Depth First ordering*. */
  target_merge_path_t = APR_ARRAY_IDX(children_with_mergeinfo, 0,
                                      svn_client__merge_path_t *);
  merge_b->target_missing_child = target_merge_path_t->missing_child;

  ranges_are_inheritable =
    (!(merge_b->target_missing_child)
     && ((depth == svn_depth_infinity)
         || (depth == svn_depth_immediates))) ? TRUE : FALSE;

  if (!ranges_are_inheritable)
    for (i = 0; i < merge_ranges->nelts; i++)
      (APR_ARRAY_IDX(merge_ranges, 0, svn_merge_range_t *))->inheritable =
        FALSE;

  SVN_ERR(populate_remaining_ranges(children_with_mergeinfo,
                                    parent_merge_source_url, ra_session,
                                    merge_ranges,
                                    parent_merge_src_canon_path,
                                    adm_access, merge_b));
  for (i = 0; i < merge_ranges->nelts; i++)
    {
      svn_boolean_t is_rollback;
      svn_merge_range_t range;
      svn_merge_range_t *r =
        APR_ARRAY_IDX(merge_ranges, i, svn_merge_range_t *);
      
      is_rollback = r->start > r->end;
     
      if (is_rollback)
        end_rev = get_farthest_end_rev(children_with_mergeinfo, r);
      else
        end_rev = get_nearest_end_rev(children_with_mergeinfo, r);
      
      start_rev = r->start;
      iterpool = svn_pool_create(pool);

      while (end_rev != SVN_INVALID_REVNUM)
        {
          svn_revnum_t next_end_rev;

          svn_pool_clear(iterpool);
          range.start = start_rev;
          range.end = end_rev;
          range.inheritable = ranges_are_inheritable;
          APR_ARRAY_PUSH(merge_source, svn_merge_range_t *) = &range;

          /* Use persistent pool while playing with remaining_ranges. */
          slice_remaining_ranges(children_with_mergeinfo, is_rollback,
                                 end_rev, pool);
          notify_b->cur_ancestor_index = -1;
          SVN_ERR(do_merge(merge_source,
                           parent_merge_source_url,
                           parent_merge_source_url,
                           FALSE,
                           merge_b->target_missing_child, merge_b->target,
                           adm_access, depth, ignore_ancestry,
                           &merge_callbacks, notify_b, merge_b,
                           children_with_mergeinfo, iterpool));
          remove_first_range_from_remaining_ranges(children_with_mergeinfo,
                                                   is_rollback, pool);
          if (is_rollback)
            next_end_rev = get_farthest_end_rev(children_with_mergeinfo, r);
          else
            next_end_rev = get_nearest_end_rev(children_with_mergeinfo, r);
          apr_array_pop(merge_source);
          if (next_end_rev != SVN_INVALID_REVNUM &&
              is_path_conflicted_by_merge(merge_b))
            {
              svn_merge_range_t conflicted_range;
              conflicted_range.start = start_rev;
              conflicted_range.end = end_rev;
              err = make_merge_conflict_error(merge_b->target,
                                              &conflicted_range, pool);
              range.end = end_rev;
              break;
            }
          start_rev = end_rev;
          end_rev = next_end_rev;
        } /* while (end_rev != SVN_INVALID_REVNUM) */
    } /* for (i = 0; i < merge_ranges->nelts; i++) */

  if (!merge_b->dry_run && merge_b->same_repos)
    {
      /* Update the WC mergeinfo here to account for our new
         merges, minus any unresolved conflicts and skips. */
      apr_hash_t *merges;

      /* Remove absent children at or under TARGET_WCPATH from
         NOTIFY_B->SKIPPED_PATHS and CHILDREN_WITH_MERGEINFO before we
         calculate the merges performed. */
      remove_absent_children(merge_b->target,
                             children_with_mergeinfo, notify_b);
      SVN_ERR(determine_merges_performed(&merges, merge_b->target,
                                         merge_ranges,
                                         depth, adm_access, notify_b, merge_b,
                                         iterpool));
      if (!merge_b->operative_merge) 
        {
          svn_pool_destroy(iterpool);
          return err;
        }
      SVN_ERR(record_mergeinfo_on_merged_children(depth, adm_access, notify_b,
                                                  merge_b, iterpool));
      SVN_ERR(update_wc_mergeinfo(merge_b->target, parent_entry,
                                  parent_merge_src_canon_path, merges,
                                  FALSE, adm_access,
                                  merge_b->ctx, iterpool));
      for (i = 0; i < children_with_mergeinfo->nelts; i++)
        {
          const char *child_repos_path;
          const char *child_merge_src_canon_path;
          const svn_wc_entry_t *child_entry;
          svn_client__merge_path_t *child =
                         APR_ARRAY_IDX(children_with_mergeinfo, i,
                                       svn_client__merge_path_t *);
          if (!child || child->absent)
            continue;

          if (strlen(child->path) == merge_target_len)
            child_repos_path = "";
          else
            child_repos_path = child->path +
                                 (merge_target_len ? merge_target_len + 1 : 0);
           child_merge_src_canon_path = 
                                    svn_path_join(parent_merge_src_canon_path,
                                                  child_repos_path, iterpool);

          SVN_ERR(svn_wc__entry_versioned(&child_entry, child->path, 
                                          adm_access, FALSE, iterpool));

          if (merge_b->operative_merge)
            {
              apr_array_header_t *child_merge_rangelist;
              apr_hash_t *child_merges = apr_hash_make(iterpool);
              int j;
              child_merge_rangelist =
                apr_array_make(iterpool, 1, sizeof(svn_merge_range_t *));
              for (j = 0; j < merge_ranges->nelts; j++)
                {
                  svn_merge_range_t *child_merge_range = svn_merge_range_dup(
                    APR_ARRAY_IDX(merge_ranges, j, svn_merge_range_t *),
                    iterpool);
                  if (child_entry->kind == svn_node_file)
                    child_merge_range->inheritable = TRUE;
                  else
                    child_merge_range->inheritable =
                      (!(child->missing_child)
                       && (depth == svn_depth_infinity
                       || depth == svn_depth_immediates)) ? TRUE : FALSE;
                  APR_ARRAY_PUSH(child_merge_rangelist, svn_merge_range_t *) =
                    child_merge_range;
                }

              apr_hash_set(child_merges, child->path, APR_HASH_KEY_STRING,
                           child_merge_rangelist);
              /* If merge target has indirect mergeinfo set it before
                 recording the first merge range. */
              if (child->indirect_mergeinfo)
                {
                  SVN_ERR(svn_client__record_wc_mergeinfo(
                                                   child->path,
                                                   child->pre_merge_mergeinfo,
                                                   adm_access,
                                                   iterpool));
                }
              SVN_ERR(update_wc_mergeinfo(child->path, child_entry,
                                          child_merge_src_canon_path, 
                                          child_merges, FALSE,
                                          adm_access, merge_b->ctx, iterpool));
            }
          SVN_ERR(mark_mergeinfo_as_inheritable_for_a_range(
                                                   child->pre_merge_mergeinfo,
                                                   TRUE,
                                                   merge_ranges,
                                                   child_merge_src_canon_path,
                                                   child->path,
                                                   adm_access,
                                                   merge_b,
                                                   children_with_mergeinfo,
                                                   i, iterpool));
          if (i > 0)
            SVN_ERR(svn_client__elide_mergeinfo(child->path, merge_b->target,
                                                child_entry, adm_access, 
                                                merge_b->ctx, iterpool));
        } /* (i = 0; i < children_with_mergeinfo->nelts; i++) */
    } /* (!merge_b->dry_run && merge_b->same_repos) */

  svn_pool_destroy(iterpool);
  return err;
}

/* Return whether the merge source (MERGE_B->ra_session1) is from a
   different repository from the merge target (ENTRY), to avoid later
   erroneously setting mergeinfo on the target. */
static APR_INLINE svn_error_t *
from_same_repos(struct merge_cmd_baton *merge_b, const svn_wc_entry_t *entry,
                svn_client_ctx_t *ctx, apr_pool_t *pool)
{
  const char *src_root;
  SVN_ERR(svn_ra_get_repos_root(merge_b->ra_session1, &src_root, pool));
  merge_b->same_repos = svn_path_is_ancestor(src_root, entry->repos);
  return SVN_NO_ERROR;
}

/*-----------------------------------------------------------------------*/

/*** Public APIs. ***/

svn_error_t *
svn_client_merge3(const char *source1,
                  const svn_opt_revision_t *revision1,
                  const char *source2,
                  const svn_opt_revision_t *revision2,
                  const char *target_wcpath,
                  svn_depth_t depth,
                  svn_boolean_t ignore_ancestry,
                  svn_boolean_t force,
                  svn_boolean_t record_only,
                  svn_boolean_t dry_run,
                  const apr_array_header_t *merge_options,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool)
{
  svn_wc_adm_access_t *adm_access;
  const svn_wc_entry_t *entry;
  struct merge_cmd_baton merge_cmd_baton;
  const char *URL1, *URL2;
  svn_config_t *cfg;
  const char *wc_repos_root;
  svn_merge_range_t range;
  enum merge_type merge_type;
  svn_boolean_t is_rollback;
  svn_opt_revision_t working_rev;
  notification_receiver_baton_t notify_b =
    {ctx->notify_func2, ctx->notify_baton2, FALSE, 0, 
     0, NULL, NULL, FALSE, NULL, -1, NULL, pool};
  apr_array_header_t *merge_ranges =
    apr_array_make(pool, 1, sizeof(svn_merge_range_t *));
  svn_merge_range_t rev_range;
  APR_ARRAY_PUSH(merge_ranges, svn_merge_range_t *) = &rev_range;

  /* If source1 or source2 are paths, we need to get the underlying
     URL from the wc and save the initial path we were passed so we
     can use it as a path parameter (either in the baton or not).
     otherwise, the path will just be NULL, which means we won't be
     able to figure out some kind of revision specifications, but in
     that case it won't matter, because those ways of specifying a
     revision are meaningless for a url. */
  SVN_ERR(svn_client_url_from_path(&URL1, source1, pool));
  if (! URL1)
    return svn_error_createf(SVN_ERR_ENTRY_MISSING_URL, NULL,
                             _("'%s' has no URL"),
                             svn_path_local_style(source1, pool));

  SVN_ERR(svn_client_url_from_path(&URL2, source2, pool));
  if (! URL2)
    return svn_error_createf(SVN_ERR_ENTRY_MISSING_URL, NULL,
                             _("'%s' has no URL"),
                             svn_path_local_style(source2, pool));

  SVN_ERR(svn_wc_adm_probe_open3(&adm_access, NULL, target_wcpath,
                                 ! dry_run, -1, ctx->cancel_func,
                                 ctx->cancel_baton, pool));

  SVN_ERR(svn_wc__entry_versioned(&entry, target_wcpath, adm_access, FALSE,
                                  pool));

  /* Get the repository root URL. */
  working_rev.kind = svn_opt_revision_working;
  SVN_ERR(svn_client__get_repos_root(&wc_repos_root, target_wcpath, 
                                     &working_rev, adm_access, ctx, pool));

  if (depth == svn_depth_unknown)
    depth = entry->depth;

  merge_cmd_baton.force = force;
  merge_cmd_baton.record_only = record_only;
  merge_cmd_baton.dry_run = dry_run;
  merge_cmd_baton.target_missing_child = FALSE;
  merge_cmd_baton.merge_options = merge_options;
  merge_cmd_baton.target = target_wcpath;
  merge_cmd_baton.url = URL2;
  merge_cmd_baton.revision = revision2;
  merge_cmd_baton.path = (source2 != URL2 ? source2 : NULL);
  merge_cmd_baton.added_path = NULL;
  merge_cmd_baton.add_necessitated_merge = FALSE;
  merge_cmd_baton.dry_run_deletions = (dry_run ? apr_hash_make(pool) : NULL);
  merge_cmd_baton.conflicted_paths = NULL;
  merge_cmd_baton.ctx = ctx;
  merge_cmd_baton.pool = pool;
  merge_cmd_baton.operative_merge = FALSE;
  merge_cmd_baton.target_has_dummy_merge_range = FALSE;
  notify_b.merge_b = &merge_cmd_baton;

  SVN_ERR(svn_client__open_ra_session_internal(&merge_cmd_baton.ra_session1,
                                               URL1, NULL, NULL, NULL,
                                               FALSE, FALSE, ctx, pool));
  SVN_ERR(svn_client__open_ra_session_internal(&merge_cmd_baton.ra_session2,
                                               URL2, NULL, NULL, NULL,
                                               FALSE, FALSE, ctx, pool));

  /* No need to check URL2, since if it's from a different repository
     than URL1, then the whole merge will fail anyway. */
  SVN_ERR(from_same_repos(&merge_cmd_baton, entry, ctx, pool));

  /* Set up the diff3 command, so various callers don't have to. */
  cfg = ctx->config ? apr_hash_get(ctx->config, SVN_CONFIG_CATEGORY_CONFIG,
                                   APR_HASH_KEY_STRING) : NULL;
  svn_config_get(cfg, &(merge_cmd_baton.diff3_cmd),
                 SVN_CONFIG_SECTION_HELPERS,
                 SVN_CONFIG_OPTION_DIFF3_CMD, NULL);

  notify_b.same_urls = (strcmp(URL1, URL2) == 0);
  if (!notify_b.same_urls && record_only)
    return svn_error_create(SVN_ERR_INCORRECT_PARAMS, NULL,
                            _("Use of two URLs is not compatible with "
                              "mergeinfo modification"));
  /* Transform opt revisions to actual revision numbers. */
  ENSURE_VALID_REVISION_KINDS(revision1->kind, revision2->kind);
  SVN_ERR(grok_range_info_from_opt_revisions(&range, &merge_type, 
                                             notify_b.same_urls,
                                             merge_cmd_baton.ra_session1,
                                             revision1,
                                             merge_cmd_baton.ra_session2,
                                             revision2, pool));

  if ((merge_type == merge_type_no_op) || (record_only && dry_run))
    return SVN_NO_ERROR;

  (APR_ARRAY_IDX(merge_ranges, 0, svn_merge_range_t *))->start = range.start;
  (APR_ARRAY_IDX(merge_ranges, 0, svn_merge_range_t *))->end = range.end;

  is_rollback = (merge_type == merge_type_rollback);

  if (merge_cmd_baton.same_repos && record_only)
    {
      return record_mergeinfo_for_record_only_merge(URL1, &range, is_rollback,
                                                    entry, adm_access, 
                                                    &merge_cmd_baton,
                                                    pool);
    }

  /* If our target_wcpath is a single file, assume that the merge
     sources are files as well, and do a single-file merge. */
  if (entry->kind == svn_node_file)
    {
      notify_b.is_single_file_merge = TRUE;
      SVN_ERR(do_single_file_merge(merge_ranges,
                                   URL1,
                                   URL2,
                                   !notify_b.same_urls,
                                   target_wcpath,
                                   adm_access,
                                   &notify_b,
                                   &merge_cmd_baton,
                                   ignore_ancestry,
                                   pool));
    }
  /* Otherwise, this must be a directory merge.  Do the fancy
     recursive diff-editor thing. */
  else if (entry->kind == svn_node_dir)
    {
      if (notify_b.same_urls)
        {
          /* Merge children with differing mergeinfo. */
          SVN_ERR(discover_and_merge_children(entry,
                                              URL1,
                                              wc_repos_root,
                                              merge_ranges,
                                              depth,
                                              ignore_ancestry,
                                              adm_access,
                                              &notify_b,
                                              &merge_cmd_baton,
                                              pool));
        }
      else
        {
          SVN_ERR(do_merge(merge_ranges,
                           URL1,
                           URL2,
                           !notify_b.same_urls,
                           merge_cmd_baton.target_missing_child,
                           target_wcpath,
                           adm_access,
                           depth,
                           ignore_ancestry,
                           &merge_callbacks,
                           &notify_b,
                           &merge_cmd_baton,
                           NULL,
                           pool));
        }
    }

  /* The final mergeinfo on TARGET_WCPATH may itself elide. */
  if (! dry_run && merge_cmd_baton.operative_merge)
    SVN_ERR(svn_client__elide_mergeinfo(target_wcpath, NULL, entry,
                                        adm_access, ctx, pool));

  SVN_ERR(svn_wc_adm_close(adm_access));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_merge2(const char *source1,
                  const svn_opt_revision_t *revision1,
                  const char *source2,
                  const svn_opt_revision_t *revision2,
                  const char *target_wcpath,
                  svn_boolean_t recurse,
                  svn_boolean_t ignore_ancestry,
                  svn_boolean_t force,
                  svn_boolean_t dry_run,
                  const apr_array_header_t *merge_options,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool)
{
  return svn_client_merge3(source1, revision1, source2, revision2,
                           target_wcpath,
                           SVN_DEPTH_INFINITY_OR_FILES(recurse),
                           ignore_ancestry, force, FALSE, dry_run,
                           merge_options, ctx, pool);
}

svn_error_t *
svn_client_merge(const char *source1,
                 const svn_opt_revision_t *revision1,
                 const char *source2,
                 const svn_opt_revision_t *revision2,
                 const char *target_wcpath,
                 svn_boolean_t recurse,
                 svn_boolean_t ignore_ancestry,
                 svn_boolean_t force,
                 svn_boolean_t dry_run,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool)
{
  return svn_client_merge2(source1, revision1, source2, revision2,
                           target_wcpath, recurse, ignore_ancestry, force,
                           dry_run, NULL, ctx, pool);
}

svn_error_t *
svn_client_merge_peg3(const char *source,
                      const apr_array_header_t *ranges_to_merge,
                      const svn_opt_revision_t *peg_revision,
                      const char *target_wcpath,
                      svn_depth_t depth,
                      svn_boolean_t ignore_ancestry,
                      svn_boolean_t force,
                      svn_boolean_t record_only,
                      svn_boolean_t dry_run,
                      const apr_array_header_t *merge_options,
                      svn_client_ctx_t *ctx,
                      apr_pool_t *pool)
{
  svn_wc_adm_access_t *adm_access;
  const svn_wc_entry_t *entry;
  struct merge_cmd_baton merge_cmd_baton;
  const char *URL;
  const char *path = NULL;
  const char *URL1, *URL2;
  svn_config_t *cfg;
  const char *wc_repos_root;
  enum merge_type merge_type = merge_type_no_op;
  svn_opt_revision_t working_rev;
  notification_receiver_baton_t notify_b =
    {ctx->notify_func2, ctx->notify_baton2, FALSE, 0,
     0, NULL, NULL, FALSE, NULL, -1, NULL, pool};
  apr_array_header_t *initial_merge_sources, *explicit_sources,
    *compacted_sources;
  int i;


  SVN_ERR(svn_wc_adm_probe_open3(&adm_access, NULL, target_wcpath,
                                 ! dry_run, -1, ctx->cancel_func,
                                 ctx->cancel_baton, pool));

  SVN_ERR(svn_wc__entry_versioned(&entry, target_wcpath, adm_access, FALSE,
                                  pool));

  /* Get the repository root URL. */
  working_rev.kind = svn_opt_revision_working;
  SVN_ERR(svn_client__get_repos_root(&wc_repos_root, target_wcpath, 
                                     &working_rev, adm_access, ctx, pool));


  /* If source is a path, we need to get the underlying URL from
     the wc and save the initial path we were passed so we can use
     it as a path parameter (either in the baton or not).
     otherwise, the path will just be NULL, which means we won't
     be able to figure out some kind of revision specifications,
     but in that case it won't matter, because those ways of
     specifying a revision are meaningless for a URL. */
  SVN_ERR(svn_client_url_from_path(&URL, source, pool));
  if (! URL)
    return svn_error_createf(SVN_ERR_ENTRY_MISSING_URL, NULL,
                             _("'%s' has no URL"),
                             svn_path_local_style(source, pool));
  if (URL != source)
    path = source;

  if (depth == svn_depth_unknown)
    depth = entry->depth;

  merge_cmd_baton.force = force;
  merge_cmd_baton.record_only = record_only;
  merge_cmd_baton.dry_run = dry_run;
  merge_cmd_baton.target_missing_child = FALSE;
  merge_cmd_baton.merge_options = merge_options;
  merge_cmd_baton.target = target_wcpath;
  merge_cmd_baton.url = URL;
  /* ### TODO: This revision field is not used anywhere.
     ### Maybe it had a purpose once? Setting it to a rather
     ### arbitrary value for now. Can we get rid of it? */
  merge_cmd_baton.revision =
    &(APR_ARRAY_IDX(ranges_to_merge, 0, svn_opt_revision_range_t *)->end);
  merge_cmd_baton.path = path;
  merge_cmd_baton.added_path = NULL;
  merge_cmd_baton.add_necessitated_merge = FALSE;
  merge_cmd_baton.dry_run_deletions = (dry_run ? apr_hash_make(pool) : NULL);
  merge_cmd_baton.conflicted_paths = NULL;
  merge_cmd_baton.ctx = ctx;
  merge_cmd_baton.pool = pool;
  merge_cmd_baton.operative_merge = FALSE;
  merge_cmd_baton.target_has_dummy_merge_range = FALSE;
  notify_b.merge_b = &merge_cmd_baton;

  SVN_ERR(svn_client__open_ra_session_internal(&merge_cmd_baton.ra_session1,
                                               URL, NULL, NULL, NULL,
                                               FALSE, FALSE, ctx, pool));
  merge_cmd_baton.ra_session2 = NULL;
  SVN_ERR(from_same_repos(&merge_cmd_baton, entry, ctx, pool));

  /* Set up the diff3 command, so various callers don't have to. */
  cfg = ctx->config ? apr_hash_get(ctx->config, SVN_CONFIG_CATEGORY_CONFIG,
                                   APR_HASH_KEY_STRING) : NULL;
  svn_config_get(cfg, &(merge_cmd_baton.diff3_cmd),
                 SVN_CONFIG_SECTION_HELPERS,
                 SVN_CONFIG_OPTION_DIFF3_CMD, NULL);

  /* Come up with some reasonable defaults for missing revisions. */
  initial_merge_sources =
    apr_array_make(pool, ranges_to_merge->nelts,
                   sizeof(svn_opt_revision_range_t *));
  for (i = 0; i < ranges_to_merge->nelts; i++)
    {
      const svn_opt_revision_t *opt_r1 =
        &((APR_ARRAY_IDX(ranges_to_merge, i,
                         svn_opt_revision_range_t *))->start);
      const svn_opt_revision_t *opt_r2 =
        &((APR_ARRAY_IDX(ranges_to_merge, i,
                         svn_opt_revision_range_t *))->end);
      svn_opt_revision_t *assumed_r1, *assumed_r2;
      svn_opt_revision_range_t *range =
        apr_palloc(initial_merge_sources->pool, sizeof(*range));
      APR_ARRAY_PUSH(initial_merge_sources, svn_opt_revision_range_t *) =
        range;
      assumed_r1 = &((APR_ARRAY_IDX(initial_merge_sources, i,
                                    svn_opt_revision_range_t *))->start);
      assumed_r2 = &((APR_ARRAY_IDX(initial_merge_sources, i,
                                    svn_opt_revision_range_t *))->end);
      SVN_ERR(assume_default_rev_range(opt_r1, assumed_r1, opt_r2, assumed_r2,
                                       merge_cmd_baton.ra_session1, pool));
    }

  explicit_sources = apr_array_make(pool, 1, sizeof(svn_merge_range_t *));

  /* Transform the peg-rev syntax into two explicit merge source
     locations. */
  for (i = 0; i < initial_merge_sources->nelts; i++)
    {
      enum merge_type merge_type_tmp;
      svn_opt_revision_t *opt_r1, *opt_r2;
      svn_opt_revision_t *opt_r1_explicit =
        apr_palloc(pool, sizeof(*opt_r1_explicit));
      svn_opt_revision_t *opt_r2_explicit =
        apr_palloc(pool, sizeof(*opt_r2_explicit));
      svn_merge_range_t *merge_range = apr_palloc(explicit_sources->pool,
                                                  sizeof(*merge_range));
      APR_ARRAY_PUSH(explicit_sources, svn_merge_range_t *) = merge_range;
      opt_r1 = &((APR_ARRAY_IDX(initial_merge_sources, i,
                                svn_opt_revision_range_t *))->start);
      opt_r2 = &((APR_ARRAY_IDX(initial_merge_sources, i,
                                svn_opt_revision_range_t *))->end);
      SVN_ERR(svn_client__repos_locations(&URL1, &opt_r1_explicit,
                                          &URL2, &opt_r2_explicit,
                                      NULL,
                                          path ? path : URL, peg_revision,
                                          opt_r1, opt_r2, ctx, pool));
        
        if (i == 0)
          {
            /* Transform opt revisions to actual revision numbers. */
            SVN_ERR(svn_ra_reparent(merge_cmd_baton.ra_session1, URL1, pool));
            notify_b.same_urls = (strcmp(URL1, URL2) == 0);
            if (!notify_b.same_urls && record_only)
              return svn_error_create(SVN_ERR_INCORRECT_PARAMS, NULL,
                                      _("Use of two URLs is not compatible "
                                        "with mergeinfo modification"));
          }

        ENSURE_VALID_REVISION_KINDS(opt_r1_explicit->kind,
                                    opt_r2_explicit->kind);
        SVN_ERR(grok_range_info_from_opt_revisions(
          APR_ARRAY_IDX(explicit_sources, i, svn_merge_range_t *),
          &merge_type_tmp,
          TRUE,
          merge_cmd_baton.ra_session1,
          opt_r1_explicit,
          merge_cmd_baton.ra_session1,
          opt_r2_explicit,
                                             pool));
        /* Any other type of merge overrides a no-op. */
        if (merge_type_tmp != merge_type_no_op)
          merge_type = merge_type_tmp;
    }

  SVN_ERR(compact_merge_ranges(&compacted_sources, explicit_sources, pool));

  if (!compacted_sources->nelts /* ranges_to_merge compacted into a no-op. */
      ||(merge_type == merge_type_no_op)
      || (record_only && dry_run))
    return SVN_NO_ERROR;

  if (merge_cmd_baton.same_repos && record_only)
    {
      int j;
      for (j = 0; j < compacted_sources->nelts; j++)
        {
          svn_merge_range_t *range =
            APR_ARRAY_IDX(compacted_sources, j, svn_merge_range_t *);
          SVN_ERR(record_mergeinfo_for_record_only_merge(
            URL1, range, merge_type == merge_type_3_way_merge,
            entry, adm_access, &merge_cmd_baton, pool));
        }
      return SVN_NO_ERROR;
    }

  /* If our target_wcpath is a single file, assume that the merge
     sources are files as well, and do a single-file merge. */
  if (entry->kind == svn_node_file)
    {
      notify_b.is_single_file_merge = TRUE;
      SVN_ERR(do_single_file_merge(compacted_sources,
                                   URL1, URL2,
                                   !notify_b.same_urls,
                                   target_wcpath,
                                   adm_access,
                                   &notify_b,
                                   &merge_cmd_baton,
                                   ignore_ancestry,
                                   pool));
    }

  /* Otherwise, this must be a directory merge.  Do the fancy
     recursive diff-editor thing. */
  else if (entry->kind == svn_node_dir)
    {
      if (notify_b.same_urls)
        {
          /* Merge children with differing mergeinfo. */
          SVN_ERR(discover_and_merge_children(entry,
                                              URL1,
                                              wc_repos_root,
                                              compacted_sources,
                                              depth,
                                              ignore_ancestry,
                                              adm_access,
                                              &notify_b,
                                              &merge_cmd_baton,
                                              pool));
        }
      else
        {
          SVN_ERR(do_merge(compacted_sources,
                           URL1,
                           URL2,
                           (merge_type == merge_type_rollback),
                           merge_cmd_baton.target_missing_child,
                           target_wcpath,
                           adm_access,
                           depth,
                           ignore_ancestry,
                           &merge_callbacks,
                           &notify_b,
                           &merge_cmd_baton,
                           NULL,
                           pool));
        }
    }

  /* The final mergeinfo on TARGET_WCPATH may itself elide. */
  if (!dry_run && merge_cmd_baton.operative_merge)
    SVN_ERR(svn_client__elide_mergeinfo(target_wcpath, NULL, entry,
                                        adm_access, ctx, pool));

  SVN_ERR(svn_wc_adm_close(adm_access));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_merge_peg2(const char *source,
                      const svn_opt_revision_t *revision1,
                      const svn_opt_revision_t *revision2,
                      const svn_opt_revision_t *peg_revision,
                      const char *target_wcpath,
                      svn_boolean_t recurse,
                      svn_boolean_t ignore_ancestry,
                      svn_boolean_t force,
                      svn_boolean_t dry_run,
                      const apr_array_header_t *merge_options,
                      svn_client_ctx_t *ctx,
                      apr_pool_t *pool)
{
  svn_opt_revision_range_t range;
  apr_array_header_t *ranges_to_merge =
    apr_array_make(pool, 1, sizeof(svn_opt_revision_range_t *));

  range.start = *revision1;
  range.end = *revision2;
  APR_ARRAY_PUSH(ranges_to_merge, svn_opt_revision_range_t *) = &range;
  return svn_client_merge_peg3(source, ranges_to_merge,
                               peg_revision,
                               target_wcpath,
                               SVN_DEPTH_INFINITY_OR_FILES(recurse),
                               ignore_ancestry, force, FALSE, dry_run,
                               merge_options, ctx, pool);
}

svn_error_t *
svn_client_merge_peg(const char *source,
                     const svn_opt_revision_t *revision1,
                     const svn_opt_revision_t *revision2,
                     const svn_opt_revision_t *peg_revision,
                     const char *target_wcpath,
                     svn_boolean_t recurse,
                     svn_boolean_t ignore_ancestry,
                     svn_boolean_t force,
                     svn_boolean_t dry_run,
                     svn_client_ctx_t *ctx,
                     apr_pool_t *pool)
{
  return svn_client_merge_peg2(source, revision1, revision2, peg_revision,
                               target_wcpath, recurse, ignore_ancestry, force,
                               dry_run, NULL, ctx, pool);
}
