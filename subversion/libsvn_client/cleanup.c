/*
 * cleanup.c:  wrapper around wc cleanup functionality.
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
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

#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_config.h"
#include "client.h"

#include "svn_private_config.h"


/*** Code. ***/

svn_error_t *
svn_client_cleanup (const char *dir,
                    svn_client_ctx_t *ctx,
                    apr_pool_t *pool)
{
  svn_node_kind_t kind;
  const char *diff3_cmd;
  svn_config_t *cfg = ctx->config
    ? apr_hash_get (ctx->config, SVN_CONFIG_CATEGORY_CONFIG,  
                    APR_HASH_KEY_STRING)
    : NULL;

  SVN_ERR (svn_io_check_path (dir, &kind, pool));
  if (kind != svn_node_dir)
    return svn_error_createf (SVN_ERR_WC_NOT_DIRECTORY, NULL,
                              _("Cannot cleanup '%s': not a directory"),
                              dir);

  svn_config_get (cfg, &diff3_cmd, SVN_CONFIG_SECTION_HELPERS,
                  SVN_CONFIG_OPTION_DIFF3_CMD, NULL);

  return svn_wc_cleanup (dir, NULL, diff3_cmd,
                         ctx->cancel_func, ctx->cancel_baton, pool);
}
