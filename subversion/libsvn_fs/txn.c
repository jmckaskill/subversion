/* txn.c : implementation of transaction functions
 *
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

#include <string.h>

#include "apr_strings.h"
#include "apr_pools.h"
#include "svn_fs.h"
#include "fs.h"
#include "err.h"
#include "trail.h"
#include "rev-table.h"
#include "txn-table.h"
#include "tree.h"


/* The private structure underlying the public svn_fs_txn_t typedef.  */

struct svn_fs_txn_t
{
  /* This transaction's private pool, a subpool of fs->pool.

     Freeing this must completely clean up the transaction object,
     write back any buffered data, and release any database or system
     resources it holds.  (But don't confuse the transaction object
     with the transaction it represents: freeing this does *not* abort
     the transaction.)  */
  apr_pool_t *pool;

  /* The filesystem to which this transaction belongs.  */
  svn_fs_t *fs;

  /* The ID of this transaction --- a null-terminated string.
     This is the key into the `transactions' table.  */
  const char *id;
};



/* Creating transactions.  */


/* Allocate and return a new transaction object in POOL for FS whose
   transaction ID is ID.  ID is not copied.  */
static svn_fs_txn_t *
make_txn (svn_fs_t *fs,
          const char *id,
          apr_pool_t *pool)
{
  apr_pool_t *new_pool = svn_pool_create (pool);
  svn_fs_txn_t *txn = apr_pcalloc (new_pool, sizeof (*txn));

  txn->pool = new_pool;
  txn->fs = fs;
  txn->id = id;

  return txn;
}
          

struct begin_txn_args
{
  svn_fs_txn_t **txn_p;
  svn_fs_t *fs;
  svn_revnum_t rev;
};


static svn_error_t *
txn_body_begin_txn (void *baton,
                    trail_t *trail)
{
  struct begin_txn_args *args = baton;
  svn_fs_id_t *root_id;
  char *svn_txn_id;

  SVN_ERR (svn_fs__rev_get_root (&root_id, args->fs, args->rev, trail));
  SVN_ERR (svn_fs__create_txn (&svn_txn_id, args->fs, root_id, trail));

  *args->txn_p = make_txn (args->fs, svn_txn_id, trail->pool);
  return SVN_NO_ERROR;
}




svn_error_t *
svn_fs_begin_txn (svn_fs_txn_t **txn_p,
                  svn_fs_t *fs,
                  svn_revnum_t rev,
                  apr_pool_t *pool)
{
  svn_fs_txn_t *txn;
  struct begin_txn_args args;

  args.txn_p = &txn;
  args.fs    = fs;
  args.rev   = rev;
  SVN_ERR (svn_fs__retry_txn (fs, txn_body_begin_txn, &args, pool));
  
  *txn_p = txn;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_txn_name (char **name_p,
                 svn_fs_txn_t *txn,
                 apr_pool_t *pool)
{
  *name_p = apr_pstrdup (pool, txn->id);
  return SVN_NO_ERROR;
}


svn_fs_t *
svn_fs_txn_fs (svn_fs_txn_t *txn)
{
  return txn->fs;
}


svn_error_t *
svn_fs_close_txn (svn_fs_txn_t *txn)
{
  /* Anything done with this transaction was written immediately to
     the filesystem (database), so there's no pending state to flush.
     We can just destroy the pool; the transaction will persist, but
     this handle on it will go away, which is the goal. */
  apr_pool_destroy (txn->pool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_commit_txn (svn_revnum_t *new_rev, 
                   svn_fs_txn_t *txn)
{
  *new_rev = SVN_INVALID_REVNUM;
  abort();
  return SVN_NO_ERROR;
}


struct open_txn_args
{
  svn_fs_txn_t **txn_p;
  svn_fs_t *fs;
  const char *name;
};


static svn_error_t *
txn_body_open_txn (void *baton,
                   trail_t *trail)
{
  struct open_txn_args *args = baton;
  svn_fs_id_t *root_id;
  svn_fs_id_t *base_root_id;

  SVN_ERR (svn_fs__get_txn (&root_id, &base_root_id,
                            args->fs, args->name, trail));

  *args->txn_p = make_txn (args->fs, args->name, trail->pool); 
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_open_txn (svn_fs_txn_t **txn_p,
                 svn_fs_t *fs,
                 const char *name,
                 apr_pool_t *pool)
{
  svn_fs_txn_t *txn;
  struct open_txn_args args;
  args.txn_p = &txn;
  args.fs = fs;
  args.name = name;
  SVN_ERR (svn_fs__retry_txn (fs, txn_body_open_txn, &args, pool));
  
  *txn_p = txn;
  return SVN_NO_ERROR;
}


struct list_transactions_args
{
  char ***names_p;
  svn_fs_t *fs;
  apr_pool_t *pool;
};

static svn_error_t *
txn_body_list_transactions (void* baton,
                            trail_t *trail)
{
  struct list_transactions_args *args = baton;
  SVN_ERR (svn_fs__get_txn_list (args->names_p, args->fs, args->pool, trail));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_list_transactions (char ***names_p,
                          svn_fs_t *fs,
                          apr_pool_t *pool)
{
  char **names;
  struct list_transactions_args args;

  args.names_p = &names;
  args.fs = fs;
  args.pool = pool;
  SVN_ERR (svn_fs__retry_txn (fs, txn_body_list_transactions, &args, pool));

  *names_p = names;
  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
