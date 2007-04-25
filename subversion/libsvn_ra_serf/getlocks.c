/*
 * getlocks.c :  entry point for get_locks RA functions for ra_serf
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
#include "svn_time.h"
#include "svn_private_config.h"

#include "ra_serf.h"


/*
 * This enum represents the current state of our XML parsing for a REPORT.
 */
typedef enum {
  NONE = 0,
  REPORT,
  LOCK,
  PATH,
  TOKEN,
  OWNER,
  COMMENT,
  CREATION_DATE,
  EXPIRATION_DATE,
} lock_state_e;

typedef struct {
  /* Temporary pool */
  apr_pool_t *pool;

  svn_lock_t *lock;

  /* The currently collected value as we build it up */
  const char *tmp;
  apr_size_t tmp_len;

} lock_info_t;

typedef struct {
  apr_pool_t *pool;

  /* return hash */
  apr_hash_t *hash;

  /* are we done? */
  svn_boolean_t done;

} lock_context_t;


static lock_info_t *
push_state(svn_ra_serf__xml_parser_t *parser,
           lock_context_t *lock_ctx,
           lock_state_e state)
{
  svn_ra_serf__xml_push_state(parser, state);

  if (state == LOCK)
    {
      lock_info_t *info;

      info = apr_pcalloc(parser->state->pool, sizeof(*info));

      info->pool = lock_ctx->pool;
      info->lock = svn_lock_create(lock_ctx->pool);
      info->lock->path = 

      parser->state->private = info;
    }

  return parser->state->private;
}

static svn_error_t *
start_getlocks(svn_ra_serf__xml_parser_t *parser,
               void *userData,
               svn_ra_serf__dav_props_t name,
               const char **attrs)
{
  lock_context_t *lock_ctx = userData;
  lock_state_e state;

  state = parser->state->current_state;

  if (state == NONE &&
      strcmp(name.name, "get-locks-report") == 0)
    {
      push_state(parser, lock_ctx, REPORT);
    }
  else if (state == REPORT &&
           strcmp(name.name, "lock") == 0)
    {
      push_state(parser, lock_ctx, LOCK);
    }
  else if (state == LOCK)
    {
      if (strcmp(name.name, "path") == 0)
        {
          push_state(parser, lock_ctx, PATH);
        }
      else if (strcmp(name.name, "token") == 0)
        {
          push_state(parser, lock_ctx, TOKEN);
        }
      else if (strcmp(name.name, "owner") == 0)
        {
          push_state(parser, lock_ctx, OWNER);
        }
      else if (strcmp(name.name, "comment") == 0)
        {
          push_state(parser, lock_ctx, COMMENT);
        }
      else if (strcmp(name.name, "creationdate") == 0)
        {
          push_state(parser, lock_ctx, CREATION_DATE);
        }
      else if (strcmp(name.name, "expirationdate") == 0)
        {
          push_state(parser, lock_ctx, EXPIRATION_DATE);
        }
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
end_getlocks(svn_ra_serf__xml_parser_t *parser,
             void *userData,
             svn_ra_serf__dav_props_t name)
{
  lock_context_t *lock_ctx = userData;
  lock_state_e state;
  lock_info_t *info;

  state = parser->state->current_state;
  info = parser->state->private;

  if (state == REPORT &&
      strcmp(name.name, "get-locks-report") == 0)
    {
      svn_ra_serf__xml_pop_state(parser);
    }
  else if (state == LOCK &&
           strcmp(name.name, "lock") == 0)
    {
      apr_hash_set(lock_ctx->hash, info->lock->path, APR_HASH_KEY_STRING,
                   info->lock);

      svn_ra_serf__xml_pop_state(parser);
    }
  else if (state == PATH &&
           strcmp(name.name, "path") == 0)
    {
      info->lock->path = apr_pstrmemdup(info->pool, info->tmp, info->tmp_len);
      info->tmp_len = 0;
      svn_ra_serf__xml_pop_state(parser);
    }
  else if (state == TOKEN &&
           strcmp(name.name, "token") == 0)
    {
      info->lock->token = apr_pstrmemdup(info->pool, info->tmp, info->tmp_len);
      info->tmp_len = 0;
      svn_ra_serf__xml_pop_state(parser);
    }
  else if (state == OWNER &&
           strcmp(name.name, "owner") == 0)
    {
      info->lock->owner = apr_pstrmemdup(info->pool, info->tmp, info->tmp_len);
      info->tmp_len = 0;
      svn_ra_serf__xml_pop_state(parser);
    }
  else if (state == COMMENT &&
           strcmp(name.name, "comment") == 0)
    {
      info->lock->comment = apr_pstrmemdup(info->pool,
                                           info->tmp, info->tmp_len);
      info->tmp_len = 0;
      svn_ra_serf__xml_pop_state(parser);
    }
  else if (state == CREATION_DATE &&
           strcmp(name.name, "creationdate") == 0)
    {
      SVN_ERR(svn_time_from_cstring(&info->lock->creation_date,
                                    info->tmp, info->pool));
      info->tmp_len = 0;
      svn_ra_serf__xml_pop_state(parser);
    }
  else if (state == EXPIRATION_DATE &&
           strcmp(name.name, "expirationdate") == 0)
    {
      SVN_ERR(svn_time_from_cstring(&info->lock->expiration_date,
                                    info->tmp, info->pool));
      info->tmp_len = 0;
      svn_ra_serf__xml_pop_state(parser);
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
cdata_getlocks(svn_ra_serf__xml_parser_t *parser,
               void *userData,
               const char *data,
               apr_size_t len)
{
  lock_context_t *lock_ctx = userData;
  lock_state_e state;
  lock_info_t *info;

  state = parser->state->current_state;
  info = parser->state->private;

  switch (state)
    {
    case PATH:
    case TOKEN:
    case OWNER:
    case COMMENT:
    case CREATION_DATE:
    case EXPIRATION_DATE:
        svn_ra_serf__expand_string(&info->tmp, &info->tmp_len,
                                   data, len, parser->state->pool);
        break;
      default:
        break;
    }

  return SVN_NO_ERROR;
}

static serf_bucket_t*
create_getlocks_body(void *baton,
                     serf_bucket_alloc_t *alloc,
                     apr_pool_t *pool)
{
  serf_bucket_t *buckets, *tmp;

  buckets = serf_bucket_aggregate_create(alloc);

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("<S:get-locks-report xmlns:S=\"",
                                  sizeof("<S:get-locks-report xmlns:S=\"")-1,
                                  alloc);
  serf_bucket_aggregate_append(buckets, tmp);

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN(SVN_XML_NAMESPACE,
                                      sizeof(SVN_XML_NAMESPACE)-1,
                                      alloc);
  serf_bucket_aggregate_append(buckets, tmp);

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("\">",
                                      sizeof("\">")-1,
                                      alloc);
  serf_bucket_aggregate_append(buckets, tmp);

  tmp = SERF_BUCKET_SIMPLE_STRING_LEN("</S:get-locks-report>",
                                      sizeof("</S:get-locks-report>")-1,
                                      alloc);
  serf_bucket_aggregate_append(buckets, tmp);

  return buckets;
}

svn_error_t *
svn_ra_serf__get_locks(svn_ra_session_t *ra_session,
                       apr_hash_t **locks,
                       const char *path,
                       apr_pool_t *pool)
{
  lock_context_t *lock_ctx;
  svn_ra_serf__session_t *session = ra_session->priv;
  svn_ra_serf__handler_t *handler;
  svn_ra_serf__xml_parser_t *parser_ctx;
  serf_bucket_t *buckets, *tmp;
  const char *req_url;
  int status_code;

  lock_ctx = apr_pcalloc(pool, sizeof(*lock_ctx));
  lock_ctx->pool = pool;
  lock_ctx->hash = apr_hash_make(pool);
  lock_ctx->done = FALSE;

  req_url = svn_path_url_add_component(session->repos_url.path, path, pool);

  handler = apr_pcalloc(pool, sizeof(*handler));

  handler->method = "REPORT";
  handler->path = req_url;
  handler->body_type = "text/xml";
  handler->conn = session->conns[0];
  handler->session = session;

  parser_ctx = apr_pcalloc(pool, sizeof(*parser_ctx));

  parser_ctx->pool = pool;
  parser_ctx->user_data = lock_ctx;
  parser_ctx->start = start_getlocks;
  parser_ctx->end = end_getlocks;
  parser_ctx->cdata = cdata_getlocks;
  parser_ctx->done = &lock_ctx->done;
  parser_ctx->status_code = &status_code;

  handler->body_delegate = create_getlocks_body;
  handler->body_delegate_baton = lock_ctx;

  handler->response_handler = svn_ra_serf__handle_xml_parser;
  handler->response_baton = parser_ctx;

  svn_ra_serf__request_create(handler);

  SVN_ERR(svn_ra_serf__context_run_wait(&lock_ctx->done, session, pool));

  *locks = lock_ctx->hash;

  return SVN_NO_ERROR;
}
