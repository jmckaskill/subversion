/* txn-table.c : operations on the `transactions' table
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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

#include <string.h>
#include <assert.h>
#include "bdb_compat.h"

#include "svn_pools.h"
#include "dbt.h"
#include "../err.h"
#include "../fs.h"
#include "../key-gen.h"
#include "../util/skel.h"
#include "../util/fs_skels.h"
#include "../trail.h"
#include "../id.h"
#include "bdb-err.h"
#include "txn-table.h"


static int 
is_committed (svn_fs__transaction_t *txn)
{
  return SVN_IS_VALID_REVNUM (txn->revision);
}


int
svn_fs__bdb_open_transactions_table (DB **transactions_p,
                                     DB_ENV *env,
                                     int create)
{
  const u_int32_t open_flags = (create ? (DB_CREATE | DB_EXCL) : 0);
  DB *txns;

  BDB_ERR (svn_fs__bdb_check_version());
  BDB_ERR (db_create (&txns, env, 0));
  BDB_ERR (txns->open (SVN_BDB_OPEN_PARAMS(txns, NULL),
                      "transactions", 0, DB_BTREE,
                      open_flags | SVN_BDB_AUTO_COMMIT,
                      0666));

  /* Create the `next-id' table entry.  */
  if (create)
  {
    DBT key, value;

    BDB_ERR (txns->put (txns, 0,
                       svn_fs__str_to_dbt (&key, 
                                           (char *) svn_fs__next_key_key),
                       svn_fs__str_to_dbt (&value, (char *) "0"),
                       SVN_BDB_AUTO_COMMIT));
  }

  *transactions_p = txns;
  return 0;
}


svn_error_t *
svn_fs__bdb_put_txn (svn_fs_t *fs,
                     const svn_fs__transaction_t *txn,
                     const char *txn_name,
                     trail_t *trail)
{
  skel_t *txn_skel;
  DBT key, value;

  /* Convert native type to skel. */
  SVN_ERR (svn_fs__unparse_transaction_skel (&txn_skel, txn, trail->pool));

  /* Only in the context of this function do we know that the DB call
     will not attempt to modify txn_name, so the cast belongs here.  */
  svn_fs__str_to_dbt (&key, (char *) txn_name);
  svn_fs__skel_to_dbt (&value, txn_skel, trail->pool);
  SVN_ERR (BDB_WRAP (fs, "storing transaction record",
                    fs->transactions->put (fs->transactions, trail->db_txn,
                                           &key, &value, 0)));

  return SVN_NO_ERROR;
}


/* Allocate a Subversion transaction ID in FS, as part of TRAIL.  Set
   *ID_P to the new transaction ID, allocated in TRAIL->pool.  */
static svn_error_t *
allocate_txn_id (const char **id_p,
                 svn_fs_t *fs,
                 trail_t *trail)
{
  DBT query, result;
  apr_size_t len;
  char next_key[SVN_FS__MAX_KEY_SIZE];
  int db_err;

  svn_fs__str_to_dbt (&query, (char *) svn_fs__next_key_key);

  /* Get the current value associated with the `next-key' key in the table.  */
  SVN_ERR (BDB_WRAP (fs, "allocating new txn ID (getting `next-key')",
                    fs->transactions->get (fs->transactions, trail->db_txn,
                                           &query, 
                                           svn_fs__result_dbt (&result), 
                                           0)));
  svn_fs__track_dbt (&result, trail->pool);

  /* Set our return value. */
  *id_p = apr_pstrmemdup (trail->pool, result.data, result.size);

  /* Bump to future key. */
  len = result.size;
  svn_fs__next_key (result.data, &len, next_key);
  svn_fs__str_to_dbt (&query, (char *) svn_fs__next_key_key);
  svn_fs__str_to_dbt (&result, (char *) next_key);
  db_err = fs->transactions->put (fs->transactions, trail->db_txn,
                                  &query, &result, 0);

  SVN_ERR (BDB_WRAP (fs, "bumping next txn key", db_err));
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__bdb_create_txn (const char **txn_name_p,
                        svn_fs_t *fs,
                        const svn_fs_id_t *root_id,
                        trail_t *trail)
{
  const char *txn_name;
  svn_fs__transaction_t txn;

  SVN_ERR (allocate_txn_id (&txn_name, fs, trail));
  txn.root_id = root_id;
  txn.base_id = root_id;
  txn.proplist = NULL;
  txn.copies = NULL;
  txn.revision = SVN_INVALID_REVNUM;
  SVN_ERR (svn_fs__bdb_put_txn (fs, &txn, txn_name, trail));

  *txn_name_p = txn_name; 
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__bdb_delete_txn (svn_fs_t *fs,
                        const char *txn_name,
                        trail_t *trail)
{
  DBT key;
  svn_fs__transaction_t *txn;
  
  /* Make sure TXN is not a committed transaction. */
  SVN_ERR (svn_fs__bdb_get_txn (&txn, fs, txn_name, trail));
  if (is_committed (txn))
    return svn_fs__err_txn_not_mutable (fs, txn_name);
  
  /* Delete the transaction from the `transactions' table. */
  svn_fs__str_to_dbt (&key, (char *) txn_name);
  SVN_ERR (BDB_WRAP (fs, "deleting entry from `transactions' table",
                    fs->transactions->del (fs->transactions,
                                           trail->db_txn, &key, 0)));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__bdb_get_txn (svn_fs__transaction_t **txn_p,
                     svn_fs_t *fs,
                     const char *txn_name,
                     trail_t *trail)
{
  DBT key, value;
  int db_err;
  skel_t *skel;
  svn_fs__transaction_t *transaction;

  /* Only in the context of this function do we know that the DB call
     will not attempt to modify txn_name, so the cast belongs here.  */
  db_err = fs->transactions->get (fs->transactions, trail->db_txn,
                                  svn_fs__str_to_dbt (&key, (char *) txn_name),
                                  svn_fs__result_dbt (&value),
                                  0);
  svn_fs__track_dbt (&value, trail->pool);

  if (db_err == DB_NOTFOUND)
    return svn_fs__err_no_such_txn (fs, txn_name);
  SVN_ERR (BDB_WRAP (fs, "reading transaction", db_err));

  /* Parse TRANSACTION skel */
  skel = svn_fs__parse_skel (value.data, value.size, trail->pool);
  if (! skel)
    return svn_fs__err_corrupt_txn (fs, txn_name);

  /* Convert skel to native type. */
  SVN_ERR (svn_fs__parse_transaction_skel (&transaction, skel, trail->pool));
  *txn_p = transaction;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__bdb_get_txn_list (apr_array_header_t **names_p,
                          svn_fs_t *fs,
                          apr_pool_t *pool,
                          trail_t *trail)
{
  apr_size_t const next_id_key_len = strlen (svn_fs__next_key_key);
  apr_pool_t *subpool = svn_pool_create (trail->pool);
  apr_array_header_t *names;

  DBC *cursor;
  DBT key, value;
  int db_err, db_c_err;

  /* Allocate the initial names array */
  names = apr_array_make (pool, 4, sizeof (const char *));

  /* Create a database cursor to list the transaction names. */
  SVN_ERR (BDB_WRAP (fs, "reading transaction list (opening cursor)",
                    fs->transactions->cursor (fs->transactions, trail->db_txn,
                                              &cursor, 0)));

  /* Build a null-terminated array of keys in the transactions table. */
  for (db_err = cursor->c_get (cursor,
                               svn_fs__result_dbt (&key),
                               svn_fs__result_dbt (&value),
                               DB_FIRST);
       db_err == 0;
       db_err = cursor->c_get (cursor,
                               svn_fs__result_dbt (&key),
                               svn_fs__result_dbt (&value),
                               DB_NEXT))
    {
      svn_fs__transaction_t *txn;
      skel_t *txn_skel;
      svn_error_t *err;

      /* Clear the per-iteration subpool */
      svn_pool_clear (subpool);

      /* Track the memory alloc'd for fetching the key and value here
         so that when the containing pool is cleared, this memory is
         freed. */
      svn_fs__track_dbt (&key, subpool);
      svn_fs__track_dbt (&value, subpool);

      /* Ignore the "next-id" key. */
      if (key.size == next_id_key_len
          && 0 == memcmp (key.data, svn_fs__next_key_key, next_id_key_len))
        continue;

      /* Parse TRANSACTION skel */
      txn_skel = svn_fs__parse_skel (value.data, value.size, subpool);
      if (! txn_skel)
        {
          cursor->c_close (cursor);
          return svn_fs__err_corrupt_txn 
            (fs, apr_pstrmemdup (trail->pool, key.data, key.size));
        }

      /* Convert skel to native type. */
      if ((err = svn_fs__parse_transaction_skel (&txn, txn_skel, subpool)))
        {
          cursor->c_close (cursor);
          return err;
        }

      /* If this is a immutable "committed" transaction, ignore it. */
      if (is_committed (txn))
        continue;

      /* Add the transaction name to the NAMES array, duping it into POOL. */
      (*((const char **) apr_array_push (names)))
        = apr_pstrmemdup (pool, key.data, key.size);
    }

  /* Check for errors, but close the cursor first. */
  db_c_err = cursor->c_close (cursor);
  if (db_err != DB_NOTFOUND)
    {
      SVN_ERR (BDB_WRAP (fs, "reading transaction list (listing keys)",
                        db_err));
    }
  SVN_ERR (BDB_WRAP (fs, "reading transaction list (closing cursor)",
                    db_c_err));

  /* Destroy the per-iteration subpool */
  svn_pool_destroy (subpool);

  *names_p = names;
  return SVN_NO_ERROR;
}
