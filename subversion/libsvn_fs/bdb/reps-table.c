/* reps-table.c : operations on the `representations' table
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

#include "bdb_compat.h"
#include "svn_fs.h"
#include "../fs.h"
#include "../util/fs_skels.h"
#include "../err.h"
#include "dbt.h"
#include "../trail.h"
#include "../key-gen.h"
#include "bdb-fs.h"
#include "bdb-err.h"
#include "reps-table.h"
#include "strings-table.h"



/*** Creating and opening the representations table. ***/

int
svn_fs__bdb_open_reps_table (DB **reps_p,
                             DB_ENV *env,
                             int create)
{
  const u_int32_t open_flags = (create ? (DB_CREATE | DB_EXCL) : 0);
  DB *reps;

  BDB_ERR (svn_fs__bdb_check_version());
  BDB_ERR (db_create (&reps, env, 0));
  BDB_ERR (reps->open (SVN_BDB_OPEN_PARAMS(reps, NULL),
                      "representations", 0, DB_BTREE,
                      open_flags | SVN_BDB_AUTO_COMMIT,
                      0666));

  /* Create the `next-key' table entry.  */
  if (create)
  {
    DBT key, value;

    BDB_ERR (reps->put
            (reps, 0,
             svn_fs__str_to_dbt (&key, (char *) svn_fs__next_key_key),
             svn_fs__str_to_dbt (&value, (char *) "0"),
             SVN_BDB_AUTO_COMMIT));
  }

  *reps_p = reps;
  return 0;
}



/*** Storing and retrieving reps.  ***/

svn_error_t *
svn_fs__bdb_read_rep (svn_fs__representation_t **rep_p,
                      svn_fs_t *fs,
                      const char *key,
                      trail_t *trail)
{
  skel_t *skel;
  int db_err;
  DBT query, result;

  db_err = fs->representations->get
    (fs->representations,
     trail->db_txn,
     svn_fs__str_to_dbt (&query, (char *) key),
     svn_fs__result_dbt (&result), 0);

  svn_fs__track_dbt (&result, trail->pool);

  /* If there's no such node, return an appropriately specific error.  */
  if (db_err == DB_NOTFOUND)
    return svn_error_createf
      (SVN_ERR_FS_NO_SUCH_REPRESENTATION, 0,
       "svn_fs__bdb_read_rep: no such representation `%s'", key);

  /* Handle any other error conditions.  */
  SVN_ERR (BDB_WRAP (fs, "reading representation", db_err));

  /* Parse the REPRESENTATION skel.  */
  skel = svn_fs__parse_skel (result.data, result.size, trail->pool);

  /* Convert to a native type.  */
  SVN_ERR (svn_fs__parse_representation_skel (rep_p, skel, trail->pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__bdb_write_rep (svn_fs_t *fs,
                       const char *key,
                       const svn_fs__representation_t *rep,
                       trail_t *trail)
{
  DBT query, result;
  skel_t *skel;

  /* Convert from native type to skel. */
  SVN_ERR (svn_fs__unparse_representation_skel (&skel, rep, trail->pool));

  /* Now write the record. */
  SVN_ERR (BDB_WRAP (fs, "storing representation",
                    fs->representations->put
                    (fs->representations, trail->db_txn,
                     svn_fs__str_to_dbt (&query, (char *) key),
                     svn_fs__skel_to_dbt (&result, skel, trail->pool), 0)));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__bdb_write_new_rep (const char **key,
                           svn_fs_t *fs,
                           const svn_fs__representation_t *rep,
                           trail_t *trail)
{
  DBT query, result;
  int db_err;
  apr_size_t len;
  char next_key[SVN_FS__MAX_KEY_SIZE];
  
  /* ### todo: see issue #409 for why bumping the key as part of this
     trail is problematic. */

  /* Get the current value associated with `next-key'.  */
  svn_fs__str_to_dbt (&query, (char *) svn_fs__next_key_key);
  SVN_ERR (BDB_WRAP (fs, "allocating new representation (getting next-key)",
                    fs->representations->get (fs->representations,
                                              trail->db_txn,
                                              &query,
                                              svn_fs__result_dbt (&result),
                                              0)));

  svn_fs__track_dbt (&result, trail->pool);

  /* Store the new rep. */
  *key = apr_pstrmemdup (trail->pool, result.data, result.size);
  SVN_ERR (svn_fs__bdb_write_rep (fs, *key, rep, trail));

  /* Bump to future key. */
  len = result.size;
  svn_fs__next_key (result.data, &len, next_key);
  db_err = fs->representations->put
    (fs->representations, trail->db_txn,
     svn_fs__str_to_dbt (&query, (char *) svn_fs__next_key_key),
     svn_fs__str_to_dbt (&result, (char *) next_key),
     0);

  SVN_ERR (BDB_WRAP (fs, "bumping next representation key", db_err));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__bdb_delete_rep (svn_fs_t *fs, const char *key, trail_t *trail)
{
  int db_err;
  DBT query;

  db_err = fs->representations->del
    (fs->representations, trail->db_txn,
     svn_fs__str_to_dbt (&query, (char *) key), 0);

  /* If there's no such node, return an appropriately specific error.  */
  if (db_err == DB_NOTFOUND)
    return svn_error_createf
      (SVN_ERR_FS_NO_SUCH_REPRESENTATION, 0,
       "svn_fs__bdb_delete_rep: no such representation `%s'", key);

  /* Handle any other error conditions.  */
  SVN_ERR (BDB_WRAP (fs, "deleting representation", db_err));

  return SVN_NO_ERROR;
}
