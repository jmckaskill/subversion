/* trail.c : backing out of aborted Berkeley DB transactions
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

#include "db.h"
#include "apr_pools.h"
#include "svn_fs.h"
#include "fs.h"
#include "err.h"
#include "trail.h"


/* A single action to be undone.  Actions are chained so that later
   actions point to earlier actions.  Thus, walking the chain and
   applying the functions should undo actions in the reverse of the
   order they were performed.  */
struct undo {
  void (*func) (void *baton);
  void *baton;
  struct undo *prev;
};


static svn_error_t *
begin_trail (trail_t **trail_p,
             svn_fs_t *fs,
             apr_pool_t *pool)
{
  trail_t *trail = apr_palloc (pool, sizeof (*trail));

  trail->pool = pool;
  trail->undo = 0;
  SVN_ERR (DB_WRAP (fs, "beginning Berkeley DB transaction",
                    txn_begin (fs->env, 0, &trail->db_txn, 0)));

  *trail_p = trail;
  return SVN_NO_ERROR;
}


static svn_error_t *
abort_trail (trail_t *trail,
             svn_fs_t *fs)
{
  struct undo *undo;

  /* Revert any in-memory changes we made as part of this transaction.  */
  for (undo = trail->undo; undo; undo = undo->prev)
    undo->func (undo->baton);

  SVN_ERR (DB_WRAP (fs, "aborting Berkeley DB transaction",
                    txn_abort (trail->db_txn)));
 
  apr_pool_destroy (trail->pool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__retry_txn (svn_fs_t *fs,
                   svn_error_t *(*txn_body) (void *baton, trail_t *trail),
                   void *baton,
                   apr_pool_t *pool)
{
  for (;;)
    {
      trail_t *trail;
      svn_error_t *svn_err;
      
      SVN_ERR (begin_trail (&trail, fs, pool));

      /* Do the body of the transaction.  */
      svn_err = (*txn_body) (baton, trail);

      if (! svn_err)
        {
          /* The transaction succeeded!  Commit it.
             According to the example in the Berkeley DB manual,
             txn_commit doesn't return DB_LOCK_DEADLOCK --- all
             deadlocks are reported earlier.  */
          SVN_ERR (DB_WRAP (fs,
                            "committing Berkeley DB transaction",
                            txn_commit (trail->db_txn, 0)));
          return SVN_NO_ERROR;
        }

      /* Is this a real error, or do we just need to retry?  */
      if (svn_err->apr_err != SVN_ERR_BERKELEY_DB
          || svn_err->src_err != DB_LOCK_DEADLOCK)
        {
          /* Ignore any error returns.  The first error is more valuable.  */
          abort_trail (trail, fs);
          return svn_err;
        }

      /* We deadlocked.  Abort the transaction, and try again.  */
      SVN_ERR (abort_trail (trail, fs));
    }
}


void
svn_fs__record_undo (trail_t *trail,
                     void (*func) (void *baton),
                     void *baton)
{
  struct undo *undo = apr_palloc (trail->pool, sizeof (*undo));

  undo->func = func;
  undo->baton = baton;
  undo->prev = trail->undo;
  trail->undo = undo;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
