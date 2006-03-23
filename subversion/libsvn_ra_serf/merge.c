/*
 * merge.c :  MERGE response parsing functions for ra_serf
 *
 * ====================================================================
 * Copyright (c) 2006 CollabNet.  All rights reserved.
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



#include <apr_uri.h>

#include <expat.h>

#include <serf.h>

#include "svn_pools.h"
#include "svn_ra.h"
#include "svn_dav.h"
#include "svn_xml.h"
#include "../libsvn_ra/ra_loader.h"
#include "svn_config.h"
#include "svn_delta.h"
#include "svn_version.h"
#include "svn_path.h"
#include "svn_private_config.h"

#include "ra_serf.h"


/*
 * This enum represents the current state of our XML parsing for a MERGE.
 */
typedef enum {
  MERGE_RESPONSE,
  UPDATED_SET,
  RESPONSE,
  HREF,
  PROPSTAT,
  PROP,
  RESOURCE_TYPE,
  AUTHOR,
  NAME,
  DATE,
  IGNORE_PROP_NAME,
  NEED_PROP_NAME,
  PROP_VAL,
} merge_state_e;

typedef enum {
  NONE,
  BASELINE,
  COLLECTION,
  CHECKED_IN,
} resource_type_e;

typedef struct {
  /* Temporary allocations here please */
  apr_pool_t *pool;

  resource_type_e type;

  apr_hash_t *props;

  const char *prop_ns;
  const char *prop_name;
  const char *prop_val;
  apr_size_t prop_val_len;
} merge_info_t;

/*
 * Encapsulates all of the REPORT parsing state that we need to know at
 * any given time.
 *
 * Previous states are stored in ->prev field.
 */
typedef struct merge_state_list_t {
   /* The current state that we are in now. */
  merge_state_e state;

  /* Information */
  merge_info_t *info;

  /* Temporary pool */
  apr_pool_t *pool;

  /* Temporary namespace list allocated from ->pool */
  svn_ra_serf__ns_t *ns_list;

  /* The previous state we were in. */
  struct merge_state_list_t *prev;
} merge_state_list_t;

/* Structure associated with a MERGE request. */
struct svn_ra_serf__merge_context_t
{
  apr_pool_t *pool;

  svn_ra_serf__session_t *session;

  const char *activity_url;
  apr_size_t activity_url_len;

  const char *merge_url;
  apr_size_t merge_url_len;

  int status;

  svn_boolean_t done;

  svn_ra_serf__ns_t *ns_list;

  svn_commit_info_t *commit_info;

  /* the current state we are in for parsing the REPORT response.
   *
   * could allocate this as an array rather than a linked list.
   *
   * (We tend to use only about 8 or 9 states in a given update-report,
   * but in theory it could be much larger based on the number of directories
   * we are adding.)
   */
  merge_state_list_t *state;
  /* A list of previous states that we have created but aren't using now. */
  merge_state_list_t *free_state;

};


static void push_state(svn_ra_serf__merge_context_t *ctx, merge_state_e state)
{
  merge_state_list_t *new_state;

  if (!ctx->free_state)
    {
      new_state = apr_palloc(ctx->pool, sizeof(*ctx->state));

      apr_pool_create(&new_state->pool, ctx->pool);
    }
  else
    {
      new_state = ctx->free_state;
      ctx->free_state = ctx->free_state->prev;

      apr_pool_clear(new_state->pool);
    }
  new_state->state = state;

  if (state == RESPONSE)
    {
      new_state->info = apr_palloc(ctx->pool, sizeof(*new_state->info));
      apr_pool_create(&new_state->info->pool, ctx->pool);
      new_state->info->props = apr_hash_make(new_state->info->pool);
    }
  /* if we have state info from our parent, reuse it. */
  else if (ctx->state && ctx->state->info)
    {
      new_state->info = ctx->state->info;
    }

  if (!ctx->state)
    {
      /* Attach to the root state. */
      new_state->ns_list = ctx->ns_list;
    }
  else
    {
      new_state->ns_list = ctx->state->ns_list;
    }

  /* Add it to the state chain. */
  new_state->prev = ctx->state;
  ctx->state = new_state;
}

static void pop_state(svn_ra_serf__merge_context_t *ctx)
{
  merge_state_list_t *free_state;
  free_state = ctx->state;
  /* advance the current state */
  ctx->state = ctx->state->prev;
  free_state->prev = ctx->free_state;
  ctx->free_state = free_state;
  ctx->free_state->info = NULL;
}

static void XMLCALL
start_merge(void *userData, const char *name, const char **attrs)
{
  svn_ra_serf__merge_context_t *ctx = userData;
  svn_ra_serf__dav_props_t prop_name;
  apr_pool_t *pool;
  svn_ra_serf__ns_t **ns_list;

  if (!ctx->state)
    {
      pool = ctx->pool;
      ns_list = &ctx->ns_list;
    }
  else
    {
      pool = ctx->state->pool;
      ns_list = &ctx->state->ns_list;
    }

  /* check for new namespaces */
  svn_ra_serf__define_ns(ns_list, attrs, pool);

  /* look up name space if present */
  prop_name = svn_ra_serf__expand_ns(*ns_list, name);

  if (!ctx->state && strcmp(prop_name.name, "merge-response") == 0)
    {
      push_state(ctx, MERGE_RESPONSE);
    }
  else if (!ctx->state)
    {
      /* do nothing as we haven't seen our valid start tag yet. */
    }
  else if (ctx->state->state == MERGE_RESPONSE &&
           strcmp(prop_name.name, "updated-set") == 0)
    {
      push_state(ctx, UPDATED_SET);
    }
  else if (ctx->state->state == UPDATED_SET &&
           strcmp(prop_name.name, "response") == 0)
    {
      push_state(ctx, RESPONSE);
    }
  else if (ctx->state->state == RESPONSE &&
           strcmp(prop_name.name, "href") == 0)
    {
      ctx->state->info->prop_ns = prop_name.namespace;
      ctx->state->info->prop_name = apr_pstrdup(ctx->state->pool,
                                                prop_name.name);
      ctx->state->info->prop_val = NULL;
      push_state(ctx, PROP_VAL);
    }
  else if (ctx->state->state == RESPONSE &&
           strcmp(prop_name.name, "propstat") == 0)
    {
      push_state(ctx, PROPSTAT);
    }
  else if (ctx->state->state == PROPSTAT &&
           strcmp(prop_name.name, "prop") == 0)
    {
      push_state(ctx, PROP);
    }
  else if (ctx->state->state == PROPSTAT &&
           strcmp(prop_name.name, "status") == 0)
    {
      /* Do nothing for now. */
    }
  else if (ctx->state->state == PROP &&
           strcmp(prop_name.name, "resourcetype") == 0)
    {
      push_state(ctx, RESOURCE_TYPE);
    }
  else if (ctx->state->state == RESOURCE_TYPE &&
           strcmp(prop_name.name, "baseline") == 0)
    {
      ctx->state->info->type = BASELINE;
    }
  else if (ctx->state->state == RESOURCE_TYPE &&
           strcmp(prop_name.name, "collection") == 0)
    {
      ctx->state->info->type = COLLECTION;
    }
  else if (ctx->state->state == PROP &&
           strcmp(prop_name.name, "checked-in") == 0)
    {
      ctx->state->info->prop_ns = prop_name.namespace;
      ctx->state->info->prop_name = apr_pstrdup(ctx->state->info->pool,
                                                prop_name.name);
      ctx->state->info->prop_val = NULL;
      push_state(ctx, IGNORE_PROP_NAME);
    }
  else if (ctx->state->state == PROP)
    {
      push_state(ctx, PROP_VAL);
    }
  else if (ctx->state->state == IGNORE_PROP_NAME)
    {
      push_state(ctx, PROP_VAL);
    }
  else if (ctx->state->state == NEED_PROP_NAME)
    {
      ctx->state->info->prop_ns = prop_name.namespace;
      ctx->state->info->prop_name = apr_pstrdup(ctx->state->info->pool,
                                                prop_name.name);
      ctx->state->info->prop_val = NULL;
      push_state(ctx, PROP_VAL);
    }
  else
    {
      abort();
    }
}

static void XMLCALL
end_merge(void *userData, const char *raw_name)
{
  svn_ra_serf__merge_context_t *ctx = userData;
  svn_ra_serf__dav_props_t prop_name;

  if (!ctx->state)
    {
      /* nothing to close yet. */
      return;
    }

  prop_name = svn_ra_serf__expand_ns(ctx->state->ns_list, raw_name);

  if (ctx->state->state == RESPONSE &&
      strcmp(prop_name.name, "response") == 0)
    {
      merge_info_t *info = ctx->state->info;

      if (info->type == BASELINE)
        {
          const char *ver_str;

          ver_str = apr_hash_get(info->props, "version-name",
                                 APR_HASH_KEY_STRING);
          if (ver_str)
            {
              ctx->commit_info->revision = SVN_STR_TO_REV(ver_str);
            }
          else
            {
              ctx->commit_info->revision = SVN_INVALID_REVNUM;
            }

          ctx->commit_info->date = apr_hash_get(info->props,
                                                "creationdate",
                                                APR_HASH_KEY_STRING);
          ctx->commit_info->author = apr_hash_get(info->props,
                                                  "creator-displayname",
                                                  APR_HASH_KEY_STRING);
          ctx->commit_info->post_commit_err = apr_hash_get(info->props,
                                                           "post-commit-err",
                                                           APR_HASH_KEY_STRING);
        }
      else if (ctx->session->wc_callbacks->push_wc_prop)
        {
          const char *href, *checked_in;
          svn_string_t checked_in_str;

          href = apr_hash_get(info->props, "href", APR_HASH_KEY_STRING);
          checked_in = apr_hash_get(info->props, "checked-in",
                                    APR_HASH_KEY_STRING);

          /* Be more precise than this? */
          href += ctx->merge_url_len + 1;

          checked_in_str.data = checked_in;
          checked_in_str.len = strlen(checked_in);

          /* We now need to dive all the way into the WC to update the
           * base VCC url.
           */
          ctx->session->wc_callbacks->push_wc_prop(
                                       ctx->session->wc_callback_baton,
                                       href,
                                       SVN_RA_SERF__WC_CHECKED_IN_URL,
                                       &checked_in_str,
                                       ctx->state->info->pool);

        }

      pop_state(ctx);
    }
  else if (ctx->state->state == PROPSTAT &&
           strcmp(prop_name.name, "propstat") == 0)
    {
      pop_state(ctx);
    }
  else if (ctx->state->state == PROP &&
           strcmp(prop_name.name, "prop") == 0)
    {
      pop_state(ctx);
    }
  else if (ctx->state->state == RESOURCE_TYPE &&
           strcmp(prop_name.name, "resourcetype") == 0)
    {
      pop_state(ctx);
    }
  else if ((ctx->state->state == IGNORE_PROP_NAME ||
            ctx->state->state == NEED_PROP_NAME))
    {
      pop_state(ctx);
    }
  else if (ctx->state->state == PROP_VAL)
    {
      merge_info_t *info = ctx->state->info;

      if (!info->prop_name)
        {
          info->prop_name = apr_pstrdup(info->pool, prop_name.name);
        }
      info->prop_val = apr_pstrmemdup(info->pool, info->prop_val,
                                      info->prop_val_len);

      /* Set our property. */
      apr_hash_set(info->props, info->prop_name, APR_HASH_KEY_STRING,
                   info->prop_val);

      info->prop_ns = NULL;
      info->prop_name = NULL;
      info->prop_val = NULL;

      pop_state(ctx);
    }
}

static void XMLCALL
cdata_merge(void *userData, const char *data, int len)
{
  svn_ra_serf__merge_context_t *ctx = userData;
  if (ctx->state && ctx->state->state == PROP_VAL)
    {
      svn_ra_serf__expand_string(&ctx->state->info->prop_val,
                                 &ctx->state->info->prop_val_len,
                                 data, len, ctx->state->pool);

    }
}

#define MERGE_HEADER "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:merge xmlns:D=\"DAV:\"><D:source><D:href>"
 
#define MERGE_TRAILER "</D:href></D:source><D:no-auto-merge/><D:no-checkout/><D:prop><D:checked-in/><D:version-name/><D:resourcetype/><D:creationdate/><D:creator-displayname/></D:prop></D:merge>"

static serf_bucket_t*
create_merge_body(void *baton,
                  serf_bucket_alloc_t *alloc,
                  apr_pool_t *pool)
{
  svn_ra_serf__merge_context_t *ctx = baton;
  serf_bucket_t *body_bkt, *tmp_bkt;

  body_bkt = serf_bucket_aggregate_create(alloc);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(MERGE_HEADER,
                                          sizeof(MERGE_HEADER) - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(ctx->activity_url,
                                          ctx->activity_url_len,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  tmp_bkt = SERF_BUCKET_SIMPLE_STRING_LEN(MERGE_TRAILER,
                                          sizeof(MERGE_TRAILER) - 1,
                                          alloc);
  serf_bucket_aggregate_append(body_bkt, tmp_bkt);

  return body_bkt;
}

svn_error_t *
svn_ra_serf__merge_create_req(svn_ra_serf__merge_context_t **ret_ctx,
                              svn_ra_serf__session_t *session,
                              svn_ra_serf__connection_t *conn,
                              const char *path,
                              const char *activity_url,
                              apr_size_t activity_url_len,
                              apr_pool_t *pool)
{
  svn_ra_serf__merge_context_t *merge_ctx;
  svn_ra_serf__handler_t *handler;
  svn_ra_serf__xml_parser_t *parser_ctx;

  merge_ctx = apr_pcalloc(pool, sizeof(*merge_ctx));

  merge_ctx->pool = pool;
  merge_ctx->session = session;

  merge_ctx->activity_url = activity_url;
  merge_ctx->activity_url_len = activity_url_len;

  merge_ctx->commit_info = svn_create_commit_info(pool);

  merge_ctx->merge_url = session->repos_url.path;
  merge_ctx->merge_url_len = strlen(merge_ctx->merge_url);

  handler = apr_pcalloc(pool, sizeof(*handler));

  handler->method = "MERGE";
  handler->path = merge_ctx->merge_url;
  handler->body_delegate = create_merge_body;
  handler->body_delegate_baton = merge_ctx;
  handler->conn = conn;
  handler->session = session;

  parser_ctx = apr_pcalloc(pool, sizeof(*parser_ctx));

  parser_ctx->user_data = merge_ctx;
  parser_ctx->start = start_merge;
  parser_ctx->end = end_merge;
  parser_ctx->cdata = cdata_merge;
  parser_ctx->done = &merge_ctx->done;
  parser_ctx->status_code = &merge_ctx->status;

  handler->response_handler = svn_ra_serf__handle_xml_parser;
  handler->response_baton = parser_ctx;

  svn_ra_serf__request_create(handler);

  *ret_ctx = merge_ctx;

  return SVN_NO_ERROR;
}

svn_boolean_t*
svn_ra_serf__merge_get_done_ptr(svn_ra_serf__merge_context_t *ctx)
{
  return &ctx->done;
}

svn_commit_info_t*
svn_ra_serf__merge_get_commit_info(svn_ra_serf__merge_context_t *ctx)
{
  return ctx->commit_info;
}

int
svn_ra_serf__merge_get_status(svn_ra_serf__merge_context_t *ctx)
{
  return ctx->status;
}
