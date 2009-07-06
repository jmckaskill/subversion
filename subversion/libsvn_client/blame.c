/*
 * blame.c:  return blame messages
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

#include <apr_pools.h>

#include "client.h"

#include "svn_client.h"
#include "svn_subst.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_diff.h"
#include "svn_pools.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_props.h"
#include "svn_sorts.h"

#include "svn_private_config.h"

#include <assert.h>

/* The metadata associated with a particular revision. */
struct rev
{
  svn_revnum_t revision; /* the revision number */
  apr_hash_t *rev_props; /* the revision properties */
  /* Used for merge reporting. */
  const char *path;      /* the absolute repository path */
};

/* One chunk of blame */
struct blame
{
  struct rev *rev;    /* the responsible revision */
  apr_off_t start;    /* the starting diff-token (line) */
  struct blame *next; /* the next chunk */
};

/* A chain of blame chunks */
struct blame_chain
{
  struct blame *blame;      /* linked list of blame chunks */
  struct blame *avail;      /* linked list of free blame chunks */
  struct apr_pool_t *pool;  /* Allocate members from this pool. */
};

/* The baton use for the diff output routine. */
struct diff_baton {
  struct blame_chain *chain;
  struct rev *rev;
};

/* The baton used for a file revision. */
struct file_rev_baton {
  svn_revnum_t start_rev, end_rev;
  const char *target;
  svn_client_ctx_t *ctx;
  const svn_diff_file_options_t *diff_options;
  svn_boolean_t ignore_mime_type;
  /* name of file containing the previous revision of the file */
  const char *last_filename;
  struct rev *rev;     /* the rev for which blame is being assigned
                          during a diff */
  struct blame_chain *chain;      /* the original blame chain. */
  const char *tmp_path; /* temp file name to feed svn_io_open_unique_file */
  apr_pool_t *mainpool;  /* lives during the whole sequence of calls */
  apr_pool_t *lastpool;  /* pool used during previous call */
  apr_pool_t *currpool;  /* pool used during this call */

  /* These are used for tracking merged revisions. */
  svn_boolean_t include_merged_revisions;
  svn_boolean_t merged_revision;
  struct blame_chain *merged_chain;  /* the merged blame chain. */
  /* name of file containing the previous merged revision of the file */
  const char *last_original_filename;
  /* pools for files which may need to persist for more than one rev. */
  apr_pool_t *filepool;
  apr_pool_t *prevfilepool;
};

/* The baton used by the txdelta window handler. */
struct delta_baton {
  /* Our underlying handler/baton that we wrap */
  svn_txdelta_window_handler_t wrapped_handler;
  void *wrapped_baton;
  struct file_rev_baton *file_rev_baton;
  apr_file_t *source_file;  /* the delta source */
  apr_file_t *file;  /* the result of the delta */
  const char *filename;
};




/* Return a blame chunk associated with REV for a change starting
   at token START, and allocated in CHAIN->mainpool. */
static struct blame *
blame_create(struct blame_chain *chain,
             struct rev *rev,
             apr_off_t start)
{
  struct blame *blame;
  if (chain->avail)
    {
      blame = chain->avail;
      chain->avail = blame->next;
    }
  else
    blame = apr_palloc(chain->pool, sizeof(*blame));
  blame->rev = rev;
  blame->start = start;
  blame->next = NULL;
  return blame;
}

/* Destroy a blame chunk. */
static void
blame_destroy(struct blame_chain *chain,
              struct blame *blame)
{
  blame->next = chain->avail;
  chain->avail = blame;
}

/* Return the blame chunk that contains token OFF, starting the search at
   BLAME. */
static struct blame *
blame_find(struct blame *blame, apr_off_t off)
{
  struct blame *prev = NULL;
  while (blame)
    {
      if (blame->start > off) break;
      prev = blame;
      blame = blame->next;
    }
  return prev;
}

/* Shift the start-point of BLAME and all subsequence blame-chunks
   by ADJUST tokens */
static void
blame_adjust(struct blame *blame, apr_off_t adjust)
{
  while (blame)
    {
      blame->start += adjust;
      blame = blame->next;
    }
}

/* Delete the blame associated with the region from token START to
   START + LENGTH */
static svn_error_t *
blame_delete_range(struct blame_chain *chain,
                   apr_off_t start,
                   apr_off_t length)
{
  struct blame *first = blame_find(chain->blame, start);
  struct blame *last = blame_find(chain->blame, start + length);
  struct blame *tail = last->next;

  if (first != last)
    {
      struct blame *walk = first->next;
      while (walk != last)
        {
          struct blame *next = walk->next;
          blame_destroy(chain, walk);
          walk = next;
        }
      first->next = last;
      last->start = start;
      if (first->start == start)
        {
          *first = *last;
          blame_destroy(chain, last);
          last = first;
        }
    }

  if (tail && tail->start == last->start + length)
    {
      *last = *tail;
      blame_destroy(chain, tail);
      tail = last->next;
    }

  blame_adjust(tail, -length);

  return SVN_NO_ERROR;
}

/* Insert a chunk of blame associated with REV starting
   at token START and continuing for LENGTH tokens */
static svn_error_t *
blame_insert_range(struct blame_chain *chain,
                   struct rev *rev,
                   apr_off_t start,
                   apr_off_t length)
{
  struct blame *head = chain->blame;
  struct blame *point = blame_find(head, start);
  struct blame *insert;

  if (point->start == start)
    {
      insert = blame_create(chain, point->rev, point->start + length);
      point->rev = rev;
      insert->next = point->next;
      point->next = insert;
    }
  else
    {
      struct blame *middle;
      middle = blame_create(chain, rev, start);
      insert = blame_create(chain, point->rev, start + length);
      middle->next = insert;
      insert->next = point->next;
      point->next = middle;
    }
  blame_adjust(insert->next, length);

  return SVN_NO_ERROR;
}

/* Callback for diff between subsequent revisions */
static svn_error_t *
output_diff_modified(void *baton,
                     apr_off_t original_start,
                     apr_off_t original_length,
                     apr_off_t modified_start,
                     apr_off_t modified_length,
                     apr_off_t latest_start,
                     apr_off_t latest_length)
{
  struct diff_baton *db = baton;

  if (original_length)
    SVN_ERR(blame_delete_range(db->chain, modified_start, original_length));

  if (modified_length)
    SVN_ERR(blame_insert_range(db->chain, db->rev, modified_start,
                               modified_length));

  return SVN_NO_ERROR;
}

static const svn_diff_output_fns_t output_fns = {
        NULL,
        output_diff_modified
};

/* Add the blame for the diffs between LAST_FILE and CUR_FILE with the rev
   specified in FRB.  LAST_FILE may be NULL in which
   case blame is added for every line of CUR_FILE. */
static svn_error_t *
add_file_blame(const char *last_file,
               const char *cur_file,
               struct blame_chain *chain,
               struct rev *rev,
               const svn_diff_file_options_t *diff_options,
               apr_pool_t *pool)
{
  if (!last_file)
    {
      SVN_ERR_ASSERT(chain->blame == NULL);
      chain->blame = blame_create(chain, rev, 0);
    }
  else
    {
      svn_diff_t *diff;
      struct diff_baton diff_baton;

      diff_baton.chain = chain;
      diff_baton.rev = rev;

      /* We have a previous file.  Get the diff and adjust blame info. */
      SVN_ERR(svn_diff_file_diff_2(&diff, last_file, cur_file,
                                   diff_options, pool));
      SVN_ERR(svn_diff_output(diff, &diff_baton, &output_fns));
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
window_handler(svn_txdelta_window_t *window, void *baton)
{
  struct delta_baton *dbaton = baton;
  struct file_rev_baton *frb = dbaton->file_rev_baton;
  struct blame_chain *chain;

  /* Call the wrapped handler first. */
  SVN_ERR(dbaton->wrapped_handler(window, dbaton->wrapped_baton));

  /* We patiently wait for the NULL window marking the end. */
  if (window)
    return SVN_NO_ERROR;

  /* Close the files used for the delta.
     It is important to do this early, since otherwise, they will be deleted
     before all handles are closed, which leads to failures on some platforms
     when new tempfiles are to be created. */
  if (dbaton->source_file)
    SVN_ERR(svn_io_file_close(dbaton->source_file, frb->currpool));
  SVN_ERR(svn_io_file_close(dbaton->file, frb->currpool));

  /* If we are including merged revisions, we need to add each rev to the
     merged chain. */
  if (frb->include_merged_revisions)
    chain = frb->merged_chain;
  else
    chain = frb->chain;

  /* Process this file. */
  SVN_ERR(add_file_blame(frb->last_filename,
                         dbaton->filename, chain, frb->rev,
                         frb->diff_options, frb->currpool));

  /* If we are including merged revisions, and the current revision is not a
     merged one, we need to add its blame info to the chain for the original
     line of history. */
  if (frb->include_merged_revisions && ! frb->merged_revision)
    {
      apr_pool_t *tmppool;

      SVN_ERR(add_file_blame(frb->last_original_filename,
                             dbaton->filename, frb->chain, frb->rev,
                             frb->diff_options, frb->currpool));

      /* This filename could be around for a while, potentially, so
         use the longer lifetime pool, and switch it with the previous one*/
      svn_pool_clear(frb->prevfilepool);
      tmppool = frb->filepool;
      frb->filepool = frb->prevfilepool;
      frb->prevfilepool = tmppool;

      frb->last_original_filename = apr_pstrdup(frb->filepool,
                                                dbaton->filename);
    }

  /* Prepare for next revision. */

  /* Remember the file name so we can diff it with the next revision. */
  frb->last_filename = dbaton->filename;

  /* Switch pools. */
  {
    apr_pool_t *tmp_pool = frb->lastpool;
    frb->lastpool = frb->currpool;
    frb->currpool = tmp_pool;
  }

  return SVN_NO_ERROR;
}

/* Throw an SVN_ERR_CLIENT_IS_BINARY_FILE error if PROP_DIFFS indicates a
   binary MIME type.  Else, return SVN_NO_ERROR. */
static svn_error_t *
check_mimetype(apr_array_header_t *prop_diffs, const char *target,
               apr_pool_t *pool)
{
  int i;

  for (i = 0; i < prop_diffs->nelts; ++i)
    {
      const svn_prop_t *prop = &APR_ARRAY_IDX(prop_diffs, i, svn_prop_t);
      if (strcmp(prop->name, SVN_PROP_MIME_TYPE) == 0
          && prop->value
          && svn_mime_type_is_binary(prop->value->data))
        return svn_error_createf
          (SVN_ERR_CLIENT_IS_BINARY_FILE, 0,
           _("Cannot calculate blame information for binary file '%s'"),
           svn_dirent_local_style(target, pool));
    }
  return SVN_NO_ERROR;
}


static svn_error_t *
file_rev_handler(void *baton, const char *path, svn_revnum_t revnum,
                 apr_hash_t *rev_props,
                 svn_boolean_t merged_revision,
                 svn_txdelta_window_handler_t *content_delta_handler,
                 void **content_delta_baton,
                 apr_array_header_t *prop_diffs,
                 apr_pool_t *pool)
{
  struct file_rev_baton *frb = baton;
  svn_stream_t *last_stream;
  svn_stream_t *cur_stream;
  struct delta_baton *delta_baton;
  apr_pool_t *filepool;

  /* Clear the current pool. */
  svn_pool_clear(frb->currpool);

  /* If this file has a non-textual mime-type, bail out. */
  if (! frb->ignore_mime_type)
    SVN_ERR(check_mimetype(prop_diffs, frb->target, frb->currpool));

  if (frb->ctx->notify_func2)
    {
      svn_wc_notify_t *notify
        = svn_wc_create_notify(path, svn_wc_notify_blame_revision, pool);
      notify->kind = svn_node_none;
      notify->content_state = notify->prop_state
        = svn_wc_notify_state_inapplicable;
      notify->lock_state = svn_wc_notify_lock_state_inapplicable;
      notify->revision = revnum;
      notify->rev_props = rev_props;
      frb->ctx->notify_func2(frb->ctx->notify_baton2, notify, pool);
    }

  if (frb->ctx->cancel_func)
    SVN_ERR(frb->ctx->cancel_func(frb->ctx->cancel_baton));

  /* If there were no content changes, we couldn't care less about this
     revision now.  Note that we checked the mime type above, so things
     work if the user just changes the mime type in a commit.
     Also note that we don't switch the pools in this case.  This is important,
     since the tempfile will be removed by the pool and we need the tempfile
     from the last revision with content changes. */
  if (!content_delta_handler)
    return SVN_NO_ERROR;

  frb->merged_revision = merged_revision;

  /* Create delta baton. */
  delta_baton = apr_palloc(frb->currpool, sizeof(*delta_baton));

  /* Prepare the text delta window handler. */
  if (frb->last_filename)
    SVN_ERR(svn_io_file_open(&delta_baton->source_file, frb->last_filename,
                             APR_READ, APR_OS_DEFAULT, frb->currpool));
  else
    /* Means empty stream below. */
    delta_baton->source_file = NULL;
  last_stream = svn_stream_from_aprfile2(delta_baton->source_file, TRUE, pool);

  if (frb->include_merged_revisions && !frb->merged_revision)
    filepool = frb->filepool;
  else
    filepool = frb->currpool;

  SVN_ERR(svn_io_open_unique_file3(&delta_baton->file,
                                   &delta_baton->filename,
                                   NULL,
                                   svn_io_file_del_on_pool_cleanup,
                                   filepool, filepool));
  cur_stream = svn_stream_from_aprfile2(delta_baton->file, TRUE, frb->currpool);

  /* Get window handler for applying delta. */
  svn_txdelta_apply(last_stream, cur_stream, NULL, NULL,
                    frb->currpool,
                    &delta_baton->wrapped_handler,
                    &delta_baton->wrapped_baton);

  /* Wrap the window handler with our own. */
  delta_baton->file_rev_baton = frb;
  *content_delta_handler = window_handler;
  *content_delta_baton = delta_baton;

  /* Create the rev structure. */
  frb->rev = apr_pcalloc(frb->mainpool, sizeof(struct rev));

  if (revnum < frb->start_rev)
    {
      /* We shouldn't get more than one revision before start. */
      SVN_ERR_ASSERT(frb->last_filename == NULL);

      /* The file existed before start_rev; generate no blame info for
         lines from this revision (or before). */
      frb->rev->revision = SVN_INVALID_REVNUM;
    }
  else
    {
      SVN_ERR_ASSERT(revnum <= frb->end_rev);

      /* Set values from revision props. */
      frb->rev->revision = revnum;
      frb->rev->rev_props = svn_prop_hash_dup(rev_props, frb->mainpool);
    }

  if (frb->include_merged_revisions)
    frb->rev->path = apr_pstrdup(frb->mainpool, path);

  return SVN_NO_ERROR;
}

/* Ensure that CHAIN_ORIG and CHAIN_MERGED have the same number of chunks,
   and that for every chunk C, CHAIN_ORIG[C] and CHAIN_MERGED[C] have the
   same starting value.  Both CHAIN_ORIG and CHAIN_MERGED should not be
   NULL.  */
static void
normalize_blames(struct blame_chain *chain,
                 struct blame_chain *chain_merged,
                 apr_pool_t *pool)
{
  struct blame *walk, *walk_merged;

  /* Walk over the CHAIN's blame chunks and CHAIN_MERGED's blame chunks,
     creating new chunks as needed. */
  for (walk = chain->blame, walk_merged = chain_merged->blame;
       walk->next && walk_merged->next;
       walk = walk->next, walk_merged = walk_merged->next)
    {
      /* The current chunks should always be starting at the same offset. */
      assert(walk->start == walk_merged->start);

      if (walk->next->start < walk_merged->next->start)
        {
          /* insert a new chunk in CHAIN_MERGED. */
          struct blame *tmp = blame_create(chain_merged, walk_merged->rev,
                                           walk->next->start);
          tmp->next = walk_merged->next;
          walk_merged->next = tmp;
        }

      if (walk->next->start > walk_merged->next->start)
        {
          /* insert a new chunk in CHAIN. */
          struct blame *tmp = blame_create(chain, walk->rev,
                                           walk_merged->next->start);
          tmp->next = walk->next;
          walk->next = tmp;
        }
    }

  /* If both next pointers are null, we have an equally long list. */
  if (walk->next == NULL && walk_merged->next == NULL)
    return;

  if (walk_merged->next == NULL)
    {
      /* Make new walk_merged chunks as needed at the end of the list so that
         the length matches that of walk. */
      while (walk->next != NULL)
        {
          struct blame *tmp = blame_create(chain_merged, walk_merged->rev,
                                           walk->next->start);
          walk_merged->next = tmp;
          walk_merged = walk_merged->next;
          walk = walk->next;
        }
    }

  if (walk->next == NULL)
    {
      /* Same as above, only create walk chunks as needed. */
      while (walk_merged->next != NULL)
        {
          struct blame *tmp = blame_create(chain, walk->rev,
                                           walk_merged->next->start);
          walk->next = tmp;
          walk = walk->next;
          walk_merged = walk_merged->next;
        }
    }
}

svn_error_t *
svn_client_blame5(const char *target,
                  const svn_opt_revision_t *peg_revision,
                  const svn_opt_revision_t *start,
                  const svn_opt_revision_t *end,
                  const svn_diff_file_options_t *diff_options,
                  svn_boolean_t ignore_mime_type,
                  svn_boolean_t include_merged_revisions,
                  svn_client_blame_receiver3_t receiver,
                  void *receiver_baton,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool)
{
  struct file_rev_baton frb;
  svn_ra_session_t *ra_session;
  const char *url;
  svn_revnum_t start_revnum, end_revnum;
  struct blame *walk, *walk_merged = NULL;
  apr_pool_t *iterpool;
  svn_stream_t *last_stream;
  svn_stream_t *stream;

  if (start->kind == svn_opt_revision_unspecified
      || end->kind == svn_opt_revision_unspecified)
    return svn_error_create
      (SVN_ERR_CLIENT_BAD_REVISION, NULL, NULL);

  /* Get an RA plugin for this filesystem object. */
  SVN_ERR(svn_client__ra_session_from_path(&ra_session, &end_revnum,
                                           &url, target, NULL,
                                           peg_revision, end,
                                           ctx, pool));

  SVN_ERR(svn_client__get_revision_number(&start_revnum, NULL, ra_session,
                                          start, target, pool));

  if (end_revnum < start_revnum)
    return svn_error_create
      (SVN_ERR_CLIENT_BAD_REVISION, NULL,
       _("Start revision must precede end revision"));

  frb.start_rev = start_revnum;
  frb.end_rev = end_revnum;
  frb.target = target;
  frb.ctx = ctx;
  frb.diff_options = diff_options;
  frb.ignore_mime_type = ignore_mime_type;
  frb.include_merged_revisions = include_merged_revisions;
  frb.last_filename = NULL;
  frb.last_original_filename = NULL;
  frb.chain = apr_palloc(pool, sizeof(*frb.chain));
  frb.chain->blame = NULL;
  frb.chain->avail = NULL;
  frb.chain->pool = pool;
  if (include_merged_revisions)
    {
      frb.merged_chain = apr_palloc(pool, sizeof(*frb.merged_chain));
      frb.merged_chain->blame = NULL;
      frb.merged_chain->avail = NULL;
      frb.merged_chain->pool = pool;
    }

  SVN_ERR(svn_io_temp_dir(&frb.tmp_path, pool));
  frb.tmp_path = svn_dirent_join(frb.tmp_path, "tmp", pool),

  frb.mainpool = pool;
  /* The callback will flip the following two pools, because it needs
     information from the previous call.  Obviously, it can't rely on
     the lifetime of the pool provided by get_file_revs. */
  frb.lastpool = svn_pool_create(pool);
  frb.currpool = svn_pool_create(pool);
  if (include_merged_revisions)
    {
      frb.filepool = svn_pool_create(pool);
      frb.prevfilepool = svn_pool_create(pool);
    }

  /* Collect all blame information.
     We need to ensure that we get one revision before the start_rev,
     if available so that we can know what was actually changed in the start
     revision. */
  SVN_ERR(svn_ra_get_file_revs2(ra_session, "",
                                start_revnum - (start_revnum > 0 ? 1 : 0),
                                end_revnum, include_merged_revisions,
                                file_rev_handler, &frb, pool));

  if (end->kind == svn_opt_revision_working)
    {
      /* If the local file is modified we have to call the handler on the
         working copy file with keywords unexpanded */
      svn_wc_adm_access_t *adm_access;
      svn_wc_status2_t *status;
      const char *target_abspath;

      SVN_ERR(svn_dirent_get_absolute(&target_abspath, target, pool));

      SVN_ERR(svn_wc_adm_open3(&adm_access, NULL,
                               svn_dirent_dirname(target, pool), FALSE,
                               0, ctx->cancel_func, ctx->cancel_baton,
                               pool));

      SVN_ERR(svn_wc_status2(&status, target, adm_access, pool));

      if (status->text_status != svn_wc_status_normal)
        {
          apr_hash_t *props;
          svn_stream_t *wcfile;
          svn_string_t *keywords;
          svn_stream_t *tempfile;
          const char *temppath;
          apr_hash_t *kw = NULL;

          SVN_ERR(svn_wc_prop_list2(&props, ctx->wc_ctx, target_abspath, pool,
                                    pool));
          SVN_ERR(svn_stream_open_readonly(&wcfile, target, pool, pool));
          
          keywords = apr_hash_get(props, SVN_PROP_KEYWORDS,
                                  APR_HASH_KEY_STRING);

          if (keywords)
            SVN_ERR(svn_subst_build_keywords2(&kw, keywords->data, NULL, NULL,
                                              0, NULL, pool));
  
          wcfile = svn_subst_stream_translated(wcfile, "\n", TRUE, kw, FALSE,
                                               pool);

          SVN_ERR(svn_stream_open_unique(&tempfile, &temppath, NULL,
                                         svn_io_file_del_on_pool_cleanup,
                                         pool, pool));

          SVN_ERR(svn_stream_copy3(wcfile, tempfile, ctx->cancel_func,
                                   ctx->cancel_baton, pool));

          SVN_ERR(add_file_blame(frb.last_filename, temppath, frb.chain, NULL,
                                 frb.diff_options, pool));

          frb.last_filename = temppath;
        }

      SVN_ERR(svn_wc_adm_close2(adm_access, pool));
    }

  /* Report the blame to the caller. */

  /* The callback has to have been called at least once. */
  SVN_ERR_ASSERT(frb.last_filename != NULL);

  /* Create a pool for the iteration below. */
  iterpool = svn_pool_create(pool);

  /* Open the last file and get a stream. */
  SVN_ERR(svn_stream_open_readonly(&last_stream, frb.last_filename,
                                   pool, pool));
  stream = svn_subst_stream_translated(last_stream,
                                       "\n", TRUE, NULL, FALSE, pool);

  /* Perform optional merged chain normalization. */
  if (include_merged_revisions)
    {
      /* If we never created any blame for the original chain, create it now,
         with the most recent changed revision.  This could occur if a file
         was created on a branch and them merged to another branch.  This is
         semanticly a copy, and we want to use the revision on the branch as
         the most recently changed revision.  ### Is this really what we want
         to do here?  Do the sematics of copy change? */
      if (!frb.chain->blame)
        frb.chain->blame = blame_create(frb.chain, frb.rev, 0);

      normalize_blames(frb.chain, frb.merged_chain, pool);
      walk_merged = frb.merged_chain->blame;
    }

  /* Process each blame item. */
  for (walk = frb.chain->blame; walk; walk = walk->next)
    {
      apr_off_t line_no;
      svn_revnum_t merged_rev;
      const char *merged_path;
      apr_hash_t *merged_rev_props;

      if (walk_merged)
        {
          merged_rev = walk_merged->rev->revision;
          merged_rev_props = walk_merged->rev->rev_props;
          merged_path = walk_merged->rev->path;
        }
      else
        {
          merged_rev = SVN_INVALID_REVNUM;
          merged_rev_props = NULL;
          merged_path = NULL;
        }

      for (line_no = walk->start;
           !walk->next || line_no < walk->next->start;
           ++line_no)
        {
          svn_boolean_t eof;
          svn_stringbuf_t *sb;

          svn_pool_clear(iterpool);
          SVN_ERR(svn_stream_readline(stream, &sb, "\n", &eof, iterpool));
          if (ctx->cancel_func)
            SVN_ERR(ctx->cancel_func(ctx->cancel_baton));
          if (!eof || sb->len)
            {
              if (walk->rev)
                SVN_ERR(receiver(receiver_baton, line_no, walk->rev->revision,
                                 walk->rev->rev_props, merged_rev,
                                 merged_rev_props, merged_path,
                                 sb->data, FALSE, iterpool));
              else
                SVN_ERR(receiver(receiver_baton, line_no, SVN_INVALID_REVNUM,
                                 NULL, SVN_INVALID_REVNUM, NULL, NULL,
                                 sb->data, TRUE, iterpool));
            }
          if (eof) break;
        }

      if (walk_merged)
        walk_merged = walk_merged->next;
    }

  SVN_ERR(svn_stream_close(stream));

  svn_pool_destroy(frb.lastpool);
  svn_pool_destroy(frb.currpool);
  if (include_merged_revisions)
    {
      svn_pool_destroy(frb.filepool);
      svn_pool_destroy(frb.prevfilepool);
    }
  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}
