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
#include "svn_path.h"
#include "svn_props.h"

#include "svn_private_config.h"

#include <assert.h>

/* The metadata associated with a particular revision. */
struct rev
{
  svn_revnum_t revision; /* the revision number */
  const char *author;    /* the author of the revision */
  const char *date;      /* the date of the revision */
  /* Used for merge reporting, and by the pre-1.1 code. */
  const char *path;      /* the absolute repository path */
  struct rev *next;      /* the next revision */
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

/* Used only by the old code. */
/* The baton used for RA->get_log */
struct log_message_baton {
  const char *path;        /* The path to be processed */
  struct rev *eldest;      /* The eldest revision processed */
  char action;             /* The action associated with the eldest */ 
  svn_revnum_t copyrev;    /* The revision the eldest was copied from */
  svn_cancel_func_t cancel_func; /* cancellation callback */ 
  void *cancel_baton;            /* cancellation baton */
  apr_pool_t *pool; 
};

static const svn_diff_output_fns_t output_fns = {
        NULL,
        output_diff_modified
};


/* Callback for log messages: accumulates revision metadata into
   a chronologically ordered list stored in the baton. */
static svn_error_t *
log_message_receiver(void *baton,
                     apr_hash_t *changed_paths,
                     svn_revnum_t revision,
                     const char *author,
                     const char *date,
                     const char *message,
                     apr_pool_t *pool)
{
  struct log_message_baton *lmb = baton;
  struct rev *rev;

  if (lmb->cancel_func)
    SVN_ERR(lmb->cancel_func(lmb->cancel_baton));

  rev = apr_palloc(lmb->pool, sizeof(*rev));
  rev->revision = revision;
  rev->author = apr_pstrdup(lmb->pool, author);
  rev->date = apr_pstrdup(lmb->pool, date);
  rev->path = lmb->path;
  rev->next = lmb->eldest;
  lmb->eldest = rev;

  SVN_ERR(svn_client__prev_log_path(&lmb->path, &lmb->action,
                                    &lmb->copyrev, changed_paths,
                                    lmb->path, svn_node_file, revision,
                                    lmb->pool));

  return SVN_NO_ERROR;
}

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
      assert(chain->blame == NULL);
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
           svn_path_local_style(target, pool));
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
  last_stream = svn_stream_from_aprfile(delta_baton->source_file, pool);

  if (frb->include_merged_revisions && !frb->merged_revision)
    filepool = frb->filepool;
  else
    filepool = frb->currpool;

  SVN_ERR(svn_io_open_unique_file2(&delta_baton->file,
                                   &delta_baton->filename,
                                   frb->tmp_path,
                                   ".tmp", svn_io_file_del_on_pool_cleanup,
                                   filepool));
  cur_stream = svn_stream_from_aprfile(delta_baton->file, frb->currpool);

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
  frb->rev = apr_palloc(frb->mainpool, sizeof(struct rev));

  if (revnum < frb->start_rev)
    {
      /* We shouldn't get more than one revision before start. */
      assert(frb->last_filename == NULL);

      /* The file existed before start_rev; generate no blame info for
         lines from this revision (or before). */
      frb->rev->revision = SVN_INVALID_REVNUM;
      frb->rev->author = NULL;
      frb->rev->date = NULL;
    }
  else
    {
      svn_string_t *str;
      assert(revnum <= frb->end_rev);

      /* Set values from revision props. */
      frb->rev->revision = revnum;

      if ((str = apr_hash_get(rev_props, SVN_PROP_REVISION_AUTHOR,
                              sizeof(SVN_PROP_REVISION_AUTHOR) - 1)))
        frb->rev->author = apr_pstrdup(frb->mainpool, str->data);
      else
        frb->rev->author = NULL;

      if ((str = apr_hash_get(rev_props, SVN_PROP_REVISION_DATE,
                              sizeof(SVN_PROP_REVISION_DATE) - 1)))
        frb->rev->date = apr_pstrdup(frb->mainpool, str->data);
      else
        frb->rev->date = NULL;
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
          struct blame *tmp = blame_create(chain_merged, walk_merged->next->rev,
                                           walk->next->start);
          tmp->next = walk_merged->next->next;
          walk_merged->next = tmp;
        }

      if (walk->next->start > walk_merged->next->start)
        {
          struct blame *tmp = blame_create(chain, walk->next->rev,
                                           walk_merged->next->start);
          tmp->next = walk->next->next;
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

static svn_error_t *
old_blame(const char *target, const char *url,
          svn_ra_session_t *ra_session,
          struct file_rev_baton *frb);

svn_error_t *
svn_client_blame4(const char *target,
                  const svn_opt_revision_t *peg_revision,
                  const svn_opt_revision_t *start,
                  const svn_opt_revision_t *end,
                  const svn_diff_file_options_t *diff_options,
                  svn_boolean_t ignore_mime_type,
                  svn_boolean_t include_merged_revisions,
                  svn_client_blame_receiver2_t receiver,
                  void *receiver_baton,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool)
{
  struct file_rev_baton frb;
  svn_ra_session_t *ra_session;
  const char *url;
  svn_revnum_t start_revnum, end_revnum;
  struct blame *walk, *walk_merged = NULL;
  apr_file_t *file; 
  apr_pool_t *iterpool;
  svn_stream_t *stream;
  svn_error_t *err;

  if (start->kind == svn_opt_revision_unspecified
      || end->kind == svn_opt_revision_unspecified)
    return svn_error_create
      (SVN_ERR_CLIENT_BAD_REVISION, NULL, NULL);
  else if (start->kind == svn_opt_revision_working
           || end->kind == svn_opt_revision_working)
    return svn_error_create
      (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
       _("blame of the WORKING revision is not supported"));

  /* Get an RA plugin for this filesystem object. */
  SVN_ERR(svn_client__ra_session_from_path(&ra_session, &end_revnum,
                                           &url, target, peg_revision, end,
                                           ctx, pool));

  SVN_ERR(svn_client__get_revision_number(&start_revnum, ra_session,
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
  frb.tmp_path = svn_path_join(frb.tmp_path, "tmp", pool),

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
  err = svn_ra_get_file_revs2(ra_session, "",
                              start_revnum - (start_revnum > 0 ? 1 : 0),
                              end_revnum, include_merged_revisions,
                              file_rev_handler, &frb, pool);
  
  /* Fall back if it wasn't supported by the server.  Servers earlier
     than 1.1 need this. */
  if (err && err->apr_err == SVN_ERR_RA_NOT_IMPLEMENTED)
    {
      svn_error_clear(err);
      err = old_blame(target, url, ra_session, &frb);
    }

  SVN_ERR(err);

  /* Report the blame to the caller. */

  /* The callback has to have been called at least once. */
  assert(frb.last_filename != NULL);

  /* Create a pool for the iteration below. */
  iterpool = svn_pool_create(pool);

  /* Open the last file and get a stream. */
  SVN_ERR(svn_io_file_open(&file, frb.last_filename, APR_READ | APR_BUFFERED,
                           APR_OS_DEFAULT, pool));
  stream = svn_subst_stream_translated(svn_stream_from_aprfile(file, pool),
                                       "\n", TRUE, NULL, FALSE, pool);

  /* Perform optional merged chain normalization. */
  if (include_merged_revisions)
    {
      normalize_blames(frb.chain, frb.merged_chain, pool);
      walk_merged = frb.merged_chain->blame;
    }

  /* Process each blame item. */
  for (walk = frb.chain->blame; walk; walk = walk->next)
    {
      apr_off_t line_no;
      svn_revnum_t merged_rev;
      const char *merged_author, *merged_date, *merged_path;

      if (walk_merged)
        {
          merged_rev = walk_merged->rev->revision;
          merged_author = walk_merged->rev->author;
          merged_date = walk_merged->rev->date;
          merged_path = walk_merged->rev->path;
        }
      else
        {
          merged_rev = SVN_INVALID_REVNUM;
          merged_author = NULL;
          merged_date = NULL;
          merged_path = NULL;
        }

      for (line_no = walk->start;
           !walk->next || line_no < walk->next->start;
           ++line_no)
        {
          svn_boolean_t eof;
          svn_stringbuf_t *sb;

          apr_pool_clear(iterpool);
          SVN_ERR(svn_stream_readline(stream, &sb, "\n", &eof, iterpool));
          if (ctx->cancel_func)
            SVN_ERR(ctx->cancel_func(ctx->cancel_baton));
          if (!eof || sb->len)
            SVN_ERR(receiver(receiver_baton, line_no, walk->rev->revision,
                             walk->rev->author, walk->rev->date,
                             merged_rev, merged_author, merged_date,
                             merged_path, sb->data, iterpool));
          if (eof) break;
        }

      if (walk_merged)
        walk_merged = walk_merged->next;
    }

  SVN_ERR(svn_stream_close(stream));

  /* We don't need the temp file any more. */
  SVN_ERR(svn_io_file_close(file, pool));

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

/* Baton for use with wrap_blame_receiver */
struct blame_receiver_wrapper_baton {
  void *baton;
  svn_client_blame_receiver_t receiver;
};

/* This implements svn_client_blame_receiver2_t */
static svn_error_t *
blame_wrapper_receiver(void *baton,
                       apr_int64_t line_no,
                       svn_revnum_t revision,
                       const char *author,
                       const char *date,
                       svn_revnum_t merged_revision,
                       const char *merged_author,
                       const char *merged_date,
                       const char *merged_path,
                       const char *line,
                       apr_pool_t *pool)
{
  struct blame_receiver_wrapper_baton *brwb = baton;

  if (brwb->receiver)
    return brwb->receiver(brwb->baton,
                          line_no, revision, author, date, line, pool);

  return SVN_NO_ERROR;
}

static void
wrap_blame_receiver(svn_client_blame_receiver2_t *receiver2,
                    void **receiver2_baton,
                    svn_client_blame_receiver_t receiver,
                    void *receiver_baton,
                    apr_pool_t *pool)
{
  struct blame_receiver_wrapper_baton *brwb = apr_palloc(pool, sizeof(*brwb));

  /* Set the user provided old format callback in the baton. */
  brwb->baton = receiver_baton;
  brwb->receiver = receiver;

  *receiver2_baton = brwb;
  *receiver2 = blame_wrapper_receiver;
}

svn_error_t *
svn_client_blame3(const char *target,
                  const svn_opt_revision_t *peg_revision,
                  const svn_opt_revision_t *start,
                  const svn_opt_revision_t *end,
                  const svn_diff_file_options_t *diff_options,
                  svn_boolean_t ignore_mime_type,
                  svn_client_blame_receiver_t receiver,
                  void *receiver_baton,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool)
{
  svn_client_blame_receiver2_t receiver2;
  void *receiver2_baton;

  wrap_blame_receiver(&receiver2, &receiver2_baton, receiver, receiver_baton,
                      pool);

  return svn_client_blame4(target, peg_revision, start, end, diff_options,
                           ignore_mime_type, FALSE, receiver2, receiver2_baton,
                           ctx, pool);
}

/* svn_client_blame3 guarantees 'no EOL chars' as part of the receiver
   LINE argument.  Older versions depend on the fact that if a CR is
   required, that CR is already part of the LINE data.

   Because of this difference, we need to trap old receivers and append
   a CR to LINE before passing it on to the actual receiver on platforms
   which want CRLF line termination.

*/

struct wrapped_receiver_baton_s
{
  svn_client_blame_receiver_t orig_receiver;
  void *orig_baton;
};

static svn_error_t *
wrapped_receiver(void *baton,
                 apr_int64_t line_no,
                 svn_revnum_t revision,
                 const char *author,
                 const char *date,
                 const char *line,
                 apr_pool_t *pool)
{
  struct wrapped_receiver_baton_s *b = baton;
  svn_stringbuf_t *expanded_line = svn_stringbuf_create(line, pool);

  svn_stringbuf_appendbytes(expanded_line, "\r", 1);

  return b->orig_receiver(b->orig_baton, line_no, revision, author,
                          date, expanded_line->data, pool);
}

static void
wrap_pre_blame3_receiver(svn_client_blame_receiver_t *receiver,
                   void **receiver_baton,
                   apr_pool_t *pool)
{
  if (strlen(APR_EOL_STR) > 1)
    {
      struct wrapped_receiver_baton_s *b = apr_palloc(pool,sizeof(*b));

      b->orig_receiver = *receiver;
      b->orig_baton = *receiver_baton;

      *receiver_baton = b;
      *receiver = wrapped_receiver;
    }
}

svn_error_t *
svn_client_blame2(const char *target,
                  const svn_opt_revision_t *peg_revision,
                  const svn_opt_revision_t *start,
                  const svn_opt_revision_t *end,
                  svn_client_blame_receiver_t receiver,
                  void *receiver_baton,
                  svn_client_ctx_t *ctx,
                  apr_pool_t *pool)
{
  wrap_pre_blame3_receiver(&receiver, &receiver_baton, pool);
  return svn_client_blame3(target, peg_revision, start, end,
                           svn_diff_file_options_create(pool), FALSE,
                           receiver, receiver_baton, ctx, pool);
}
svn_error_t *
svn_client_blame(const char *target,
                 const svn_opt_revision_t *start,
                 const svn_opt_revision_t *end,
                 svn_client_blame_receiver_t receiver,
                 void *receiver_baton,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *pool)
{
  wrap_pre_blame3_receiver(&receiver, &receiver_baton, pool);
  return svn_client_blame2(target, end, start, end,
                           receiver, receiver_baton, ctx, pool);
}


/* This is used when there is no get_file_revs available. */
static svn_error_t *
old_blame(const char *target, const char *url,
          svn_ra_session_t *ra_session,
          struct file_rev_baton *frb)
{
  const char *reposURL;
  struct log_message_baton lmb;
  apr_array_header_t *condensed_targets;
  apr_file_t *file;
  svn_stream_t *stream;
  struct rev *rev;
  svn_node_kind_t kind;
  apr_pool_t *pool = frb->mainpool;

  SVN_ERR(svn_ra_check_path(ra_session, "", frb->end_rev, &kind, pool));

  if (kind == svn_node_dir)
    return svn_error_createf(SVN_ERR_CLIENT_IS_DIRECTORY, NULL,
                             _("URL '%s' refers to a directory"), url);

  condensed_targets = apr_array_make(pool, 1, sizeof(const char *));
  APR_ARRAY_PUSH(condensed_targets, const char *) = "";

  SVN_ERR(svn_ra_get_repos_root(ra_session, &reposURL, pool));

  /* URI decode the path before placing it in the baton, since changed_paths
     passed into log_message_receiver will not be URI encoded. */
  lmb.path = svn_path_uri_decode(url + strlen(reposURL), pool);

  lmb.cancel_func = frb->ctx->cancel_func;
  lmb.cancel_baton = frb->ctx->cancel_baton;
  lmb.eldest = NULL;
  lmb.pool = pool;

  /* Accumulate revision metadata by walking the revisions
     backwards; this allows us to follow moves/copies
     correctly. */
  SVN_ERR(svn_ra_get_log(ra_session,
                         condensed_targets,
                         frb->end_rev,
                         frb->start_rev,
                         0, /* no limit */
                         TRUE,
                         FALSE,
                         log_message_receiver,
                         &lmb,
                         pool));

  SVN_ERR(svn_client__open_ra_session_internal(&ra_session, reposURL, NULL,
                                               NULL, NULL, FALSE, FALSE,
                                               frb->ctx, pool));

  /* Inspect the first revision's change metadata; if there are any
     prior revisions, compute a new starting revision/path.  If no
     revisions were selected, no blame is assigned.  A modified
     item certainly has a prior revision.  It is reasonable for an
     added item to have none, but anything else is unexpected.  */
  if (!lmb.eldest)
    {
      lmb.eldest = apr_palloc(pool, sizeof(*rev));
      lmb.eldest->revision = frb->end_rev;
      lmb.eldest->path = lmb.path;
      lmb.eldest->next = NULL;
      rev = apr_palloc(pool, sizeof(*rev));
      rev->revision = SVN_INVALID_REVNUM;
      rev->author = NULL;
      rev->date = NULL;
      frb->chain->blame = blame_create(frb->chain, rev, 0);
    }
  else if (lmb.action == 'M' || SVN_IS_VALID_REVNUM(lmb.copyrev))
    {
      rev = apr_palloc(pool, sizeof(*rev));
      if (SVN_IS_VALID_REVNUM(lmb.copyrev))
        rev->revision = lmb.copyrev;
      else
        rev->revision = lmb.eldest->revision - 1;
      rev->path = lmb.path;
      rev->next = lmb.eldest;
      lmb.eldest = rev;
      rev = apr_palloc(pool, sizeof(*rev));
      rev->revision = SVN_INVALID_REVNUM;
      rev->author = NULL;
      rev->date = NULL;
      frb->chain->blame = blame_create(frb->chain, rev, 0);
    }
  else if (lmb.action == 'A')
    {
      frb->chain->blame = blame_create(frb->chain, lmb.eldest, 0);
    }
  else
    return svn_error_createf(APR_EGENERAL, NULL,
                             _("Revision action '%c' for "
                               "revision %ld of '%s' "
                               "lacks a prior revision"),
                             lmb.action, lmb.eldest->revision,
                             svn_path_local_style(lmb.eldest->path, pool));

  /* Walk the revision list in chronological order, downloading
     each fulltext, diffing it with its predecessor, and accumulating
     the blame information into db.blame.  Use two iteration pools
     rather than one, because the diff routines need to look at a
     sliding window of revisions.  Two pools gives us a ring buffer
     of sorts. */
  for (rev = lmb.eldest; rev; rev = rev->next)
    {
      const char *tmp;
      const char *temp_dir;
      apr_hash_t *props;
      svn_string_t *mimetype;
      
      apr_pool_clear(frb->currpool);
      SVN_ERR(svn_io_temp_dir(&temp_dir, frb->currpool));
      SVN_ERR(svn_io_open_unique_file2
              (&file, &tmp,
               svn_path_join(temp_dir, "tmp", frb->currpool), ".tmp",
               svn_io_file_del_on_pool_cleanup, frb->currpool));

      stream = svn_stream_from_aprfile(file, frb->currpool);
      SVN_ERR(svn_ra_get_file(ra_session, rev->path + 1, rev->revision,
                              stream, NULL, &props, frb->currpool));
      SVN_ERR(svn_stream_close(stream));
      SVN_ERR(svn_io_file_close(file, frb->currpool));

      /* If this file has a non-textual mime-type, bail out. */
      if (! frb->ignore_mime_type && props && 
          ((mimetype = apr_hash_get(props, SVN_PROP_MIME_TYPE, 
                                    sizeof(SVN_PROP_MIME_TYPE) - 1))))
        {
          if (svn_mime_type_is_binary(mimetype->data))
            return svn_error_createf 
              (SVN_ERR_CLIENT_IS_BINARY_FILE, 0,
               _("Cannot calculate blame information for binary file '%s'"),
               svn_path_local_style(target, frb->currpool));
        }

      if (frb->ctx->notify_func2)
        {
          svn_wc_notify_t *notify
            = svn_wc_create_notify(rev->path, svn_wc_notify_blame_revision,
                                   pool);
          notify->kind = svn_node_none;
          notify->content_state = notify->prop_state
            = svn_wc_notify_state_inapplicable;
          notify->lock_state = svn_wc_notify_lock_state_inapplicable;
          notify->revision = rev->revision;
          frb->ctx->notify_func2(frb->ctx->notify_baton2, notify, pool);
        }                               

      if (frb->ctx->cancel_func)
        SVN_ERR(frb->ctx->cancel_func(frb->ctx->cancel_baton));

      if (frb->last_filename)
        {
          frb->rev = rev;
          SVN_ERR(add_file_blame(frb->last_filename, tmp, frb->chain,
                                 frb->rev, frb->diff_options, frb->currpool));
        }

      frb->last_filename = tmp;
      {
        apr_pool_t *tmppool = frb->currpool;
        frb->currpool = frb->lastpool;
        frb->lastpool = tmppool;
      }
    }

  return SVN_NO_ERROR;
}
