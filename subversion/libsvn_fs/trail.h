/* trail.h : internal interface to backing out of aborted Berkeley DB txns
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

#ifndef SVN_LIBSVN_FS_TRAIL_H
#define SVN_LIBSVN_FS_TRAIL_H

#include "db.h"
#include "apr_pools.h"
#include "svn_fs.h"


/* "How do I get a trail object?  All these functions in the
   filesystem expect them, and I can't find a function that returns
   one."

   Well, there isn't a function that returns a trail.  All trails come
   from svn_fs__retry_txn.  Here's how to use that:

   When using Berkeley DB transactions to protect the integrity of a
   database, there are several things you need to keep in mind:

   - Any Berkeley DB operation you perform as part of a Berkeley DB
     transaction may return DB_LOCK_DEADLOCK, meaning that your
     operation interferes with some other transaction in progress.
     When this happens, you must abort the transaction, which undoes
     all the changes you've made so far, and try it again.  So every
     piece of code you ever write to bang on the DB needs to be
     wrapped up in a retry loop.

   - If, while you're doing your database operations, you also change
     some in-memory data structures, then you may want to revert those
     changes if the transaction deadlocks and needs to be retried.

   - If you get a `real' error (i.e., something other than
     DB_LOCK_DEADLOCK), you must abort your DB transaction, to release
     its locks and return the database to its previous state.
     Similarly, you may want to unroll some changes you've made to
     in-memory data structures.

   - Since a transaction insulates you from database changes made by
     other processes, it's often possible to cache information about
     database contents while the transaction lasts.  However, this
     cache may become stale once your transaction is over.  So you may
     need to clear your cache once the transaction completes, either
     successfully or unsuccessfully.

   The `svn_fs__retry_txn' function and its friends help you manage
   all of that, in one nice package.
   
   To use it, write your code in a function like this:
  
       static svn_error_t *
       txn_body_do_my_thing (void *baton,
                             trail_t *trail)
       {
         ... 
         Do everything which needs to be protected by a Berkeley DB
         transaction here.  Use TRAIL->db_txn as your Berkeley DB
         transaction, and do your allocation in TRAIL->pool.  Pass
         TRAIL on through to any functions which require one.

         If a Berkeley DB operation returns DB_LOCK_DEADLOCK, just
         return that using the normal Subversion error mechanism
         (using DB_ERR, for example); don't write a retry loop.  If you
         encounter some other kind of error, return it in the normal
         fashion.
         ...
       }

   Now, call svn_fs__retry_txn, passing a pointer to your function as
   an argument:

       err = svn_fs__retry_txn (fs, txn_body_do_my_thing, baton, pool);

   This will simply invoke your function `txn_body_do_my_thing',
   passing BATON through unchanged, and providing a fresh TRAIL
   object, containing a Berkeley DB transaction and an APR pool --- a
   subpool of POOL --- you should use.

   If your function returns a Subversion error wrapping a Berkeley DB
   DB_LOCK_DEADLOCK error, `svn_fs__retry_txn' will abort the trail's
   Berkeley DB transaction for you (thus undoing any database changes
   you've made), free the trail's subpool (thus undoing any allocation
   you may have done), and try the whole thing again with a new trail,
   containing a new Berkeley DB transaction and pool.

   If your function returns any other kind of Subversion error,
   `svn_fs__retry_txn' will abort the trail's Berkeley DB transaction,
   free the subpool, and return your error to its caller.

   If, heavens forbid, your function actually succeeds, returning
   SVN_NO_ERROR, `svn_fs__retry_txn' commits the trail's Berkeley DB
   transaction, thus making your DB changes permanent, leaves the
   trail's pool alone, so all the objects it contains are still
   around, and returns SVN_NO_ERROR.

   If you're making changes to in-memory data structures which should
   be reverted if the transaction doesn't complete successfully, you
   can call `svn_fs__record_undo' as you make your changes to register
   functions that will undo them.  On failure (either due to deadlock
   or a real error), `svn_fs__retry_txn' will invoke your undo
   functions, youngest first, to restore your data structures to the
   state they were in when you started the transaction.

   If you're caching things in in-memory data structures, which may go
   stale once the transaction is complete, you can call
   `svn_fs__record_completion' to register functions that will clear
   your caches.  When the trail completes, successfully or
   unsuccessfully, `svn_fs__retry_txn' will invoke your completion
   functions, youngest first, to remove whatever cached information
   you like.  */

struct trail_t
{
  /* A Berkeley DB transaction.  */
  DB_TXN *db_txn;

  /* A pool to allocate things in as part of that transaction --- a
     subpool of the one passed to `begin_trail'.  We destroy this pool
     if we abort the transaction, and leave it around otherwise.  */
  apr_pool_t *pool;

  /* A record of the side-effects to be undone in various
     circumstances.  */
  struct undo *undo;
};
typedef struct trail_t trail_t;


/* Try a Berkeley DB transaction repeatedly until it doesn't deadlock.

   That is:
   - Begin a new Berkeley DB transaction, DB_TXN, in the filesystem FS.
   - Allocate a subpool of POOL, TXN_POOL.
   - Start a new trail, TRAIL, based on DB_TXN and TXN_POOL.
   - Apply TXN_BODY to BATON and TRAIL.  TXN_BODY should try to do
     some series of DB operations which needs to be atomic, using
     TRAIL->db_txn as the transaction, and TRAIL->pool for allocation.
     If a DB operation deadlocks, or if any other kind of error
     happens, TXN_BODY should simply return with an appropriate
     svn_error_t, E.
   - If TXN_BODY returns SVN_NO_ERROR, then commit the transaction,
     run any completion functions, and return SVN_NO_ERROR.
   - If E is a Berkeley DB error indicating that a deadlock occurred,
     run all undo and completion functions, abort the DB transaction,
     and free TXN_POOL.  Then retry the whole thing from the top.
   - If E is any other kind of error, run all undo and completion
     functions, free TXN_POOL, and return E.

   One benefit of using this function is that it makes it easy to
   ensure that whatever transactions a filesystem function starts, it
   either aborts or commits before it returns.  If we don't somehow
   complete all our transactions, later operations could deadlock.  */
svn_error_t *svn_fs__retry_txn (svn_fs_t *fs,
                                svn_error_t *(*txn_body) (void *baton,
                                                          trail_t *trail),
                                void *baton,
                                apr_pool_t *pool);


/* Record a change which should be undone if TRAIL is aborted, either
   because of a deadlock or an error.

   The beauty of a Berkeley DB transaction (like any database
   transaction) is that, if you encounter an error partway through an
   operation, aborting the DB transaction automatically undoes
   whatever changes you've already made to the database.  Your
   error-handling code doesn't need to clean everything up.

   However, a Berkeley DB transaction only protects on-disk
   structures.  If the operation changed in-memory data structures as
   well, those may also need to be undone when an error occurs, or the
   transaction deadlocks.

   When you make a such a change, call this function with a FUNC and
   BATON that, if invoked, will undo the change.  If TRAIL fails to
   complete (deadlock, error, etc.), svn_fs__retry_txn will invoke the
   FUNC/BATON pairs that were registered via this function.

   Younger undo and completion functions get invoked before older
   functions.  Undo and completion functions are ordered with respect
   to each other.  */
void svn_fs__record_undo (trail_t *trail,
                          void (*func) (void *baton),
                          void *baton);


/* Record a change which should be undone when TRAIL is completed,
   either successfully (the transaction is committed) or
   unsuccessfully (the transaction deadlocked, or an error occurred).

   You can use this to free caches of information that might become
   stale once the transaction is complete.

   Younger undo and completion functions get invoked before older
   functions.  Undo and completion functions are ordered with respect
   to each other.  */
void svn_fs__record_completion (trail_t *trail,
                                void (*func) (void *baton),
                                void *baton);
                                     


#endif /* SVN_LIBSVN_FS_TRAIL_H */



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
