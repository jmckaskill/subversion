/* sqlite.c
 *
 * ====================================================================
 *    Licensed to the Subversion Corporation (SVN Corp.) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The SVN Corp. licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <apr_pools.h>

#include "svn_types.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "svn_io.h"
#include "svn_dirent_uri.h"
#include "svn_checksum.h"

#include "private/svn_sqlite.h"
#include "svn_private_config.h"
#include "private/svn_dep_compat.h"
#include "private/svn_atomic.h"
#include "private/svn_skel.h"
#include "private/svn_token.h"


#ifdef SVN_SQLITE_INLINE
/* Include sqlite3 inline, making all symbols private. */
  #define SQLITE_API static
  #include <sqlite3.c>
#else
  #include <sqlite3.h>
#endif

#ifdef SQLITE3_DEBUG
/* An sqlite query execution callback. */
static void
sqlite_tracer(void *data, const char *sql)
{
  /*  sqlite3 *db3 = data; */
  SVN_DBG(("sql=\"%s\"\n", sql));
}
#endif


struct svn_sqlite__db_t
{
  sqlite3 *db3;
  const char * const *statement_strings;
  int nbr_statements;
  svn_sqlite__stmt_t **prepared_stmts;
  apr_pool_t *result_pool;
};

struct svn_sqlite__stmt_t
{
  sqlite3_stmt *s3stmt;
  svn_sqlite__db_t *db;
};


/* Convert SQLite error codes to SVN */
#define SQLITE_ERROR_CODE(x) ((x) == SQLITE_READONLY    \
                              ? SVN_ERR_SQLITE_READONLY \
                              : SVN_ERR_SQLITE_ERROR )


/* SQLITE->SVN quick error wrap, much like SVN_ERR. */
#define SQLITE_ERR(x, db) do                                     \
{                                                                \
  int sqlite_err__temp = (x);                                    \
  if (sqlite_err__temp != SQLITE_OK)                             \
    return svn_error_create(SQLITE_ERROR_CODE(sqlite_err__temp), \
                            NULL, sqlite3_errmsg((db)->db3));    \
} while (0)

#define SQLITE_ERR_MSG(x, msg) do                                \
{                                                                \
  int sqlite_err__temp = (x);                                    \
  if (sqlite_err__temp != SQLITE_OK)                             \
    return svn_error_create(SQLITE_ERROR_CODE(sqlite_err__temp), \
                            NULL, msg);                          \
} while (0)


static svn_error_t *
exec_sql(svn_sqlite__db_t *db, const char *sql)
{
  char *err_msg;
  int sqlite_err = sqlite3_exec(db->db3, sql, NULL, NULL, &err_msg);

  if (sqlite_err != SQLITE_OK)
    {
      svn_error_t *err = svn_error_create(SQLITE_ERROR_CODE(sqlite_err), NULL,
                                          err_msg);
      sqlite3_free(err_msg);
      return err;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__get_statement(svn_sqlite__stmt_t **stmt, svn_sqlite__db_t *db,
                          int stmt_idx)
{
  SVN_ERR_ASSERT(stmt_idx < db->nbr_statements);

  if (db->prepared_stmts[stmt_idx] == NULL)
    SVN_ERR(svn_sqlite__prepare(&db->prepared_stmts[stmt_idx], db,
                                db->statement_strings[stmt_idx],
                                db->result_pool));

  *stmt = db->prepared_stmts[stmt_idx];
  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__prepare(svn_sqlite__stmt_t **stmt, svn_sqlite__db_t *db,
                    const char *text, apr_pool_t *result_pool)
{
  *stmt = apr_palloc(result_pool, sizeof(**stmt));
  (*stmt)->db = db;

  SQLITE_ERR(sqlite3_prepare_v2(db->db3, text, -1, &(*stmt)->s3stmt, NULL), db);

  return SVN_NO_ERROR;
}

static svn_error_t *
step_with_expectation(svn_sqlite__stmt_t* stmt,
                      svn_boolean_t expecting_row)
{
  svn_boolean_t got_row;

  SVN_ERR(svn_sqlite__step(&got_row, stmt));
  if ((got_row && !expecting_row)
      ||
      (!got_row && expecting_row))
    return svn_error_create(SVN_ERR_SQLITE_ERROR, NULL,
                            expecting_row
                            ? _("Expected database row missing")
                            : _("Extra database row found"));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__step_done(svn_sqlite__stmt_t *stmt)
{
  SVN_ERR(step_with_expectation(stmt, FALSE));
  return svn_error_return(svn_sqlite__reset(stmt));
}

svn_error_t *
svn_sqlite__step_row(svn_sqlite__stmt_t *stmt)
{
  return svn_error_return(step_with_expectation(stmt, TRUE));
}


svn_error_t *
svn_sqlite__step(svn_boolean_t *got_row, svn_sqlite__stmt_t *stmt)
{
  int sqlite_result = sqlite3_step(stmt->s3stmt);

  if (sqlite_result != SQLITE_DONE && sqlite_result != SQLITE_ROW)
    {
      svn_error_t *err1, *err2;

      err1 = svn_error_create(SQLITE_ERROR_CODE(sqlite_result), NULL,
                              sqlite3_errmsg(stmt->db->db3));
      err2 = svn_sqlite__reset(stmt);
      return svn_error_compose_create(err1, err2);
    }

  *got_row = (sqlite_result == SQLITE_ROW);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__insert(apr_int64_t *row_id, svn_sqlite__stmt_t *stmt)
{
  svn_boolean_t got_row;

  SVN_ERR(svn_sqlite__step(&got_row, stmt));
  if (row_id)
    *row_id = sqlite3_last_insert_rowid(stmt->db->db3);

  return svn_error_return(svn_sqlite__reset(stmt));
}

static svn_error_t *
vbindf(svn_sqlite__stmt_t *stmt, const char *fmt, va_list ap)
{
  int count;

  for (count = 1; *fmt; fmt++, count++)
    {
      const void *blob;
      apr_size_t blob_size;
      const svn_token_map_t *map;

      switch (*fmt)
        {
          case 's':
            SVN_ERR(svn_sqlite__bind_text(stmt, count,
                                          va_arg(ap, const char *)));
            break;

          case 'i':
            SVN_ERR(svn_sqlite__bind_int64(stmt, count,
                                           va_arg(ap, apr_int64_t)));
            break;

          case 'b':
            blob = va_arg(ap, const void *);
            blob_size = va_arg(ap, apr_size_t);
            SVN_ERR(svn_sqlite__bind_blob(stmt, count, blob, blob_size));
            break;

          case 't':
            map = va_arg(ap, const svn_token_map_t *);
            SVN_ERR(svn_sqlite__bind_token(stmt, count, map, va_arg(ap, int)));
            break;

          default:
            SVN_ERR_MALFUNCTION();
        }
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__bindf(svn_sqlite__stmt_t *stmt, const char *fmt, ...)
{
  svn_error_t *err;
  va_list ap;

  va_start(ap, fmt);
  err = vbindf(stmt, fmt, ap);
  va_end(ap);
  return svn_error_return(err);
}

svn_error_t *
svn_sqlite__bind_int(svn_sqlite__stmt_t *stmt,
                     int slot,
                     int val)
{
  SQLITE_ERR(sqlite3_bind_int(stmt->s3stmt, slot, val), stmt->db);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__bind_int64(svn_sqlite__stmt_t *stmt,
                       int slot,
                       apr_int64_t val)
{
  SQLITE_ERR(sqlite3_bind_int64(stmt->s3stmt, slot, val), stmt->db);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__bind_text(svn_sqlite__stmt_t *stmt,
                      int slot,
                      const char *val)
{
  SQLITE_ERR(sqlite3_bind_text(stmt->s3stmt, slot, val, -1, SQLITE_TRANSIENT),
             stmt->db);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__bind_blob(svn_sqlite__stmt_t *stmt,
                      int slot,
                      const void *val,
                      apr_size_t len)
{
  SQLITE_ERR(sqlite3_bind_blob(stmt->s3stmt, slot, val, (int) len,
                               SQLITE_TRANSIENT),
             stmt->db);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__bind_token(svn_sqlite__stmt_t *stmt,
                       int slot,
                       const svn_token_map_t *map,
                       int value)
{
  const char *word = svn_token__to_word(map, value);

  SQLITE_ERR(sqlite3_bind_text(stmt->s3stmt, slot, word, -1, SQLITE_STATIC),
             stmt->db);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__bind_properties(svn_sqlite__stmt_t *stmt,
                            int slot,
                            const apr_hash_t *props,
                            apr_pool_t *scratch_pool)
{
  svn_skel_t *skel;
  svn_stringbuf_t *properties;

  if (props == NULL)
    return svn_error_return(svn_sqlite__bind_blob(stmt, slot, NULL, 0));

  SVN_ERR(svn_skel__unparse_proplist(&skel, (apr_hash_t *)props,
                                     scratch_pool));
  properties = svn_skel__unparse(skel, scratch_pool);
  return svn_error_return(svn_sqlite__bind_blob(stmt,
                                                slot,
                                                properties->data,
                                                properties->len));
}

svn_error_t *
svn_sqlite__bind_checksum(svn_sqlite__stmt_t *stmt,
                          int slot,
                          const svn_checksum_t *checksum,
                          apr_pool_t *scratch_pool)
{
  const char *csum_str;

  if (checksum == NULL)
    csum_str = NULL;
  else
    csum_str = svn_checksum_serialize(checksum, scratch_pool, scratch_pool);

  return svn_error_return(svn_sqlite__bind_text(stmt, slot, csum_str));
}


const void *
svn_sqlite__column_blob(svn_sqlite__stmt_t *stmt, int column,
                        apr_size_t *len, apr_pool_t *result_pool)
{
  const void *val = sqlite3_column_blob(stmt->s3stmt, column);
  *len = sqlite3_column_bytes(stmt->s3stmt, column);

  if (result_pool && val != NULL)
    val = apr_pmemdup(result_pool, val, *len);

  return val;
}

const char *
svn_sqlite__column_text(svn_sqlite__stmt_t *stmt, int column,
                        apr_pool_t *result_pool)
{
  /* cast from 'unsigned char' to regular 'char'  */
  const char *result = (const char *)sqlite3_column_text(stmt->s3stmt, column);

  if (result_pool && result != NULL)
    result = apr_pstrdup(result_pool, result);

  return result;
}

svn_revnum_t
svn_sqlite__column_revnum(svn_sqlite__stmt_t *stmt, int column)
{
  if (svn_sqlite__column_is_null(stmt, column))
    return SVN_INVALID_REVNUM;
  return (svn_revnum_t) sqlite3_column_int64(stmt->s3stmt, column);
}

svn_boolean_t
svn_sqlite__column_boolean(svn_sqlite__stmt_t *stmt, int column)
{
  return sqlite3_column_int64(stmt->s3stmt, column) != 0;
}

int
svn_sqlite__column_int(svn_sqlite__stmt_t *stmt, int column)
{
  return sqlite3_column_int(stmt->s3stmt, column);
}

apr_int64_t
svn_sqlite__column_int64(svn_sqlite__stmt_t *stmt, int column)
{
  return sqlite3_column_int64(stmt->s3stmt, column);
}

int
svn_sqlite__column_token(svn_sqlite__stmt_t *stmt,
                         int column,
                         const svn_token_map_t *map)
{
  /* cast from 'unsigned char' to regular 'char'  */
  const char *word = (const char *)sqlite3_column_text(stmt->s3stmt, column);

  return svn_token__from_word_strict(map, word);
}

svn_error_t *
svn_sqlite__column_properties(apr_hash_t **props,
                              svn_sqlite__stmt_t *stmt,
                              int column,
                              apr_pool_t *result_pool,
                              apr_pool_t *scratch_pool)
{
  apr_size_t len;
  const void *val;

  /* svn_skel__parse_proplist copies everything needed to result_pool */
  val = svn_sqlite__column_blob(stmt, column, &len, NULL);
  if (val == NULL)
    {
      *props = NULL;
      return SVN_NO_ERROR;
    }

  return svn_error_return(svn_skel__parse_proplist(
                            props,
                            svn_skel__parse(val, len, scratch_pool),
                            result_pool));
}

svn_error_t *
svn_sqlite__column_checksum(const svn_checksum_t **checksum,
                            svn_sqlite__stmt_t *stmt, int column,
                            apr_pool_t *result_pool)
{
  const char *digest = svn_sqlite__column_text(stmt, column, NULL);

  if (digest == NULL)
    *checksum = NULL;
  else
    SVN_ERR(svn_checksum_deserialize(checksum, digest,
                                     result_pool, result_pool));

  return SVN_NO_ERROR;
}

svn_boolean_t
svn_sqlite__column_is_null(svn_sqlite__stmt_t *stmt, int column)
{
  return sqlite3_column_type(stmt->s3stmt, column) == SQLITE_NULL;
}


svn_error_t *
svn_sqlite__finalize(svn_sqlite__stmt_t *stmt)
{
  SQLITE_ERR(sqlite3_finalize(stmt->s3stmt), stmt->db);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__reset(svn_sqlite__stmt_t *stmt)
{
  SQLITE_ERR(sqlite3_reset(stmt->s3stmt), stmt->db);
  SQLITE_ERR(sqlite3_clear_bindings(stmt->s3stmt), stmt->db);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_sqlite__set_schema_version(svn_sqlite__db_t *db,
                               int version,
                               apr_pool_t *scratch_pool)
{
  const char *pragma_cmd = apr_psprintf(scratch_pool,
                                        "PRAGMA user_version = %d;",
                                        version);

  return svn_error_return(exec_sql(db, pragma_cmd));
}


/* Time (in milliseconds) to wait for sqlite locks before giving up. */
#define BUSY_TIMEOUT 10000


#if 0
/*
 * EXAMPLE
 *
 * The following provide an example for a series of SQL statements to
 * create/upgrade a SQLite schema across multiple "formats". This array
 * and integer would be passed into svn_sqlite__open().
 */
static const char *schema_create_sql[] = {
  NULL, /* An empty database is format 0 */

  /* USER_VERSION 1 */
  "PRAGMA auto_vacuum = 1;"
  APR_EOL_STR
  "CREATE TABLE mergeinfo (revision INTEGER NOT NULL, mergedfrom TEXT NOT "
  "NULL, mergedto TEXT NOT NULL, mergedrevstart INTEGER NOT NULL, "
  "mergedrevend INTEGER NOT NULL, inheritable INTEGER NOT NULL);"
  APR_EOL_STR
  "CREATE INDEX mi_mergedfrom_idx ON mergeinfo (mergedfrom);"
  APR_EOL_STR
  "CREATE INDEX mi_mergedto_idx ON mergeinfo (mergedto);"
  APR_EOL_STR
  "CREATE INDEX mi_revision_idx ON mergeinfo (revision);"
  APR_EOL_STR
  "CREATE TABLE mergeinfo_changed (revision INTEGER NOT NULL, path TEXT "
  "NOT NULL);"
  APR_EOL_STR
  "CREATE UNIQUE INDEX mi_c_revpath_idx ON mergeinfo_changed (revision, path);"
  APR_EOL_STR
  "CREATE INDEX mi_c_path_idx ON mergeinfo_changed (path);"
  APR_EOL_STR
  "CREATE INDEX mi_c_revision_idx ON mergeinfo_changed (revision);"
  APR_EOL_STR,

  /* USER_VERSION 2 */
  "CREATE TABLE node_origins (node_id TEXT NOT NULL, node_rev_id TEXT NOT "
  "NULL);"
  APR_EOL_STR
  "CREATE UNIQUE INDEX no_ni_idx ON node_origins (node_id);"
  APR_EOL_STR
};

static const int latest_schema_format =
  sizeof(schema_create_sql)/sizeof(schema_create_sql[0]) - 1;

#endif

struct upgrade_baton
{
  int current_schema;
  int latest_schema;
  const char * const *upgrade_sql;
};


/* This implements svn_sqlite__transaction_callback_t */
static svn_error_t *
upgrade_format(void *baton,
               svn_sqlite__db_t *db,
               apr_pool_t *scratch_pool)
{
  struct upgrade_baton *ub = baton;
  int current_schema = ub->current_schema;
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  while (current_schema < ub->latest_schema)
    {
      svn_pool_clear(iterpool);

      /* Go to the next schema */
      current_schema++;

      /* Run the upgrade SQL */
      if (ub->upgrade_sql[current_schema])
        SVN_ERR(exec_sql(db, ub->upgrade_sql[current_schema]));

      /* Update the user version pragma */
      SVN_ERR(svn_sqlite__set_schema_version(db, current_schema, iterpool));
    }

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__read_schema_version(int *version,
                                svn_sqlite__db_t *db,
                                apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;

  SVN_ERR(svn_sqlite__prepare(&stmt, db, "PRAGMA user_version;", scratch_pool));
  SVN_ERR(svn_sqlite__step_row(stmt));

  *version = svn_sqlite__column_int(stmt, 0);

  return svn_error_return(svn_sqlite__finalize(stmt));
}


/* Check the schema format of the database, upgrading it if necessary.
   Return SVN_ERR_SQLITE_UNSUPPORTED_SCHEMA if the schema format is too new,
   or SVN_ERR_SQLITE_ERROR if an sqlite error occurs during validation.
   Return SVN_NO_ERROR if everything is okay. */
static svn_error_t *
check_format(svn_sqlite__db_t *db,
             int latest_schema,
             const char * const *upgrade_sql, 
             apr_pool_t *scratch_pool)
{
  int current_schema;

  /* Validate that the schema exists as expected. */
  SVN_ERR(svn_sqlite__read_schema_version(&current_schema, db, scratch_pool));

  if (current_schema == latest_schema)
    return SVN_NO_ERROR;

  if (current_schema < latest_schema)
    {
      struct upgrade_baton ub;

      ub.current_schema = current_schema;
      ub.latest_schema = latest_schema;
      ub.upgrade_sql = upgrade_sql;

      return svn_error_return(svn_sqlite__with_transaction(
                                db, upgrade_format, &ub, scratch_pool));
    }

  return svn_error_createf(SVN_ERR_SQLITE_UNSUPPORTED_SCHEMA, NULL,
                           _("Schema format %d not recognized"),
                           current_schema);
}

static volatile svn_atomic_t sqlite_init_state;

/* If possible, verify that SQLite was compiled in a thread-safe
   manner. */
/* Don't call this function directly!  Use svn_atomic__init_once(). */
static svn_error_t *
init_sqlite(apr_pool_t *pool)
{
  if (sqlite3_libversion_number() < SQLITE_VERSION_NUMBER)
    {
      return svn_error_createf(
                    SVN_ERR_SQLITE_ERROR, NULL,
                    _("SQLite compiled for %s, but running with %s"),
                    SQLITE_VERSION, sqlite3_libversion());
    }

#if APR_HAS_THREADS
#if SQLITE_VERSION_AT_LEAST(3,5,0)
  /* SQLite 3.5 allows verification of its thread-safety at runtime.
     Older versions are simply expected to have been configured with
     --enable-threadsafe, which compiles with -DSQLITE_THREADSAFE=1
     (or -DTHREADSAFE, for older versions). */
  if (! sqlite3_threadsafe())
    return svn_error_create(SVN_ERR_SQLITE_ERROR, NULL,
                            _("SQLite is required to be compiled and run in "
                              "thread-safe mode"));
#endif
#if SQLITE_VERSION_AT_LEAST(3,6,0)
  /* If SQLite has been already initialized, sqlite3_config() returns
     SQLITE_MISUSE. */
  {
    int err = sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
    if (err != SQLITE_OK && err != SQLITE_MISUSE)
      return svn_error_create(SQLITE_ERROR_CODE(err), NULL,
                              _("Could not configure SQLite"));
  }
  SQLITE_ERR_MSG(sqlite3_initialize(), _("Could not initialize SQLite"));
#endif
#endif /* APR_HAS_THRADS */

#if SQLITE_VERSION_AT_LEAST(3,5,0)
  /* SQLite 3.5 allows sharing cache instances, even in a multithreaded
   * environment. This allows sharing cached data when we open a database
   * more than once (Very common in the current pre-single-database state) */
  SQLITE_ERR_MSG(sqlite3_enable_shared_cache(TRUE),
                 _("Could not initialize SQLite shared cache"));
#endif

  return SVN_NO_ERROR;
}

static svn_error_t *
internal_open(sqlite3 **db3, const char *path, svn_sqlite__mode_t mode,
              apr_pool_t *scratch_pool)
{
#if SQLITE_VERSION_AT_LEAST(3,5,0)
  {
    int flags;

    if (mode == svn_sqlite__mode_readonly)
      flags = SQLITE_OPEN_READONLY;
    else if (mode == svn_sqlite__mode_readwrite)
      flags = SQLITE_OPEN_READWRITE;
    else if (mode == svn_sqlite__mode_rwcreate)
      flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    else
      SVN_ERR_MALFUNCTION();

    /* If this flag is defined (3.6.x), then let's turn off SQLite's mutexes.
       All svn objects are single-threaded, so we can already guarantee that
       our use of the SQLite handle will be serialized properly.
       Note: in 3.6.x, we've already config'd SQLite into MULTITHREAD mode,
       so this is probably redundant, but if we are running in a process where
       somebody initialized SQLite before us it is needed anyway. */
#ifdef SQLITE_OPEN_NOMUTEX
    flags |= SQLITE_OPEN_NOMUTEX;
#endif

    /* Open the database. Note that a handle is returned, even when an error
       occurs (except for out-of-memory); thus, we can safely use it to
       extract an error message and construct an svn_error_t. */
    {
      /* We'd like to use SQLITE_ERR_MSG here, but we can't since it would
         just return an error and leave the database open.  So, we need to
         do this manually. */
      /* ### SQLITE_CANTOPEN */
      int err_code = sqlite3_open_v2(path, db3, flags, NULL);
      if (err_code != SQLITE_OK)
        {
          char *msg = apr_pstrdup(scratch_pool, sqlite3_errmsg(*db3));

          /* We don't catch the error here, since we care more about the open
             error than the close error at this point. */
          sqlite3_close(*db3);

          msg = apr_pstrcat(scratch_pool, msg, ": '", path, "'", NULL);
          return svn_error_create(SQLITE_ERROR_CODE(err_code), NULL, msg);
        }
    }
  }
#else
  /* Older versions of SQLite (pre-3.5.x) will always create the database
     if it doesn't exist.  So, if we are asked to be read-only or read-write,
     we ensure the database already exists - if it doesn't, then we will
     explicitly error out before asking SQLite to do anything.

     Pre-3.5.x SQLite versions also don't support read-only ops either.
   */
  if (mode == svn_sqlite__mode_readonly || mode == svn_sqlite__mode_readwrite)
    {
      svn_node_kind_t kind;

      SVN_ERR(svn_io_check_path(path, &kind, scratch_pool));
      if (kind != svn_node_file) {
          return svn_error_createf(APR_ENOENT, NULL,
                                   _("Expected SQLite database not found: %s"),
                                   svn_dirent_local_style(path, scratch_pool));
      }
    }
  else if (mode == svn_sqlite__mode_rwcreate)
    {
      /* do nothing - older SQLite's will create automatically. */
    }
  else
    SVN_ERR_MALFUNCTION();
  {
    /* We'd like to use SQLITE_ERR_MSG here, but we can't since it would
       just return an error and leave the database open.  So, we need to
       do this manually. */
    int err_code = sqlite3_open(path, db3);
    if (err_code != SQLITE_OK)
      {
        char *msg = apr_pstrdup(scratch_pool, sqlite3_errmsg(*db3));

        /* We don't catch the error here, since we care more about the open
           error than the close error at this point. */
        sqlite3_close(*db3);

        return svn_error_create(SQLITE_ERROR_CODE(err_code), NULL, msg);
      }
  }
#endif

  /* Retry until timeout when database is busy. */
  SQLITE_ERR_MSG(sqlite3_busy_timeout(*db3, BUSY_TIMEOUT),
                 sqlite3_errmsg(*db3));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__get_schema_version(int *version,
                               const char *path,
                               apr_pool_t *scratch_pool)
{
  svn_sqlite__db_t db;

  SVN_ERR(svn_atomic__init_once(&sqlite_init_state, init_sqlite, scratch_pool));
  SVN_ERR(internal_open(&db.db3, path, svn_sqlite__mode_readonly,
                        scratch_pool));
  SVN_ERR(svn_sqlite__read_schema_version(version, &db, scratch_pool));
  SQLITE_ERR(sqlite3_close(db.db3), &db);

  return SVN_NO_ERROR;
}

/* APR cleanup function used to close the database when its pool is destroyed.
   DATA should be the svn_sqlite__db_t handle for the database. */
static apr_status_t
close_apr(void *data)
{
  svn_sqlite__db_t *db = data;
  svn_error_t *err = SVN_NO_ERROR;
  apr_status_t result;
  int i;

  /* Check to see if we've already closed this database. */
  if (db->db3 == NULL)
    return APR_SUCCESS;

  /* Finalize any existing prepared statements. */
  for (i = 0; i < db->nbr_statements; i++)
    {
      if (db->prepared_stmts[i])
        err = svn_error_compose_create(
                        svn_sqlite__finalize(db->prepared_stmts[i]), err);
    }

  result = sqlite3_close(db->db3);

  /* If there's a pre-existing error, return it. */
  if (err)
    {
      result = err->apr_err;
      svn_error_clear(err);
      return result;
    }

  if (result != SQLITE_OK)
    return SQLITE_ERROR_CODE(result);

  db->db3 = NULL;

  return APR_SUCCESS;
}

svn_error_t *
svn_sqlite__open(svn_sqlite__db_t **db, const char *path,
                 svn_sqlite__mode_t mode, const char * const statements[],
                 int latest_schema, const char * const *upgrade_sql,
                 apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  SVN_ERR(svn_atomic__init_once(&sqlite_init_state, init_sqlite, scratch_pool));

  *db = apr_pcalloc(result_pool, sizeof(**db));

  SVN_ERR(internal_open(&(*db)->db3, path, mode, scratch_pool));

#ifdef SQLITE3_DEBUG
  sqlite3_trace((*db)->db3, sqlite_tracer, (*db)->db3);
#endif

  SVN_ERR(exec_sql(*db, 
              "PRAGMA case_sensitive_like=1;"
              /* Disable synchronization to disable the explicit disk flushes
                 that make Sqlite up to 50 times slower; especially on small
                 transactions.

                 This removes some stability guarantees on specific hardware
                 and power failures, but still guarantees atomic commits on
                 application crashes. With our dependency on external data
                 like pristine files (Wc) and revision files (repository),
                 we can't keep up these additional guarantees anyway.

                 ### Maybe switch to NORMAL(1) when we use larger transaction
                     scopes */
              "PRAGMA synchronous=OFF;"));

  /* Validate the schema, upgrading if necessary. */
  SVN_ERR(check_format(*db, latest_schema, upgrade_sql, scratch_pool));

  /* Store the provided statements. */
  if (statements)
    {
      (*db)->statement_strings = statements;
      (*db)->nbr_statements = 0;
      while (*statements != NULL)
        {
          statements++;
          (*db)->nbr_statements++;
        }
      (*db)->prepared_stmts = apr_pcalloc(result_pool, (*db)->nbr_statements
                                                * sizeof(svn_sqlite__stmt_t *));
    }
  else
    (*db)->nbr_statements = 0;

  (*db)->result_pool = result_pool;
  apr_pool_cleanup_register(result_pool, *db, close_apr, apr_pool_cleanup_null);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__close(svn_sqlite__db_t *db)
{
  apr_status_t result = apr_pool_cleanup_run(db->result_pool, db, close_apr);

  if (result == APR_SUCCESS)
    return SVN_NO_ERROR;

  return svn_error_wrap_apr(result, NULL);
}

svn_error_t *
svn_sqlite__with_transaction(svn_sqlite__db_t *db,
                             svn_sqlite__transaction_callback_t cb_func,
                             void *cb_baton,
                             apr_pool_t *scratch_pool /* NULL allowed */)
{
  svn_error_t *err;

  SVN_ERR(exec_sql(db, "BEGIN TRANSACTION;"));
  err = cb_func(cb_baton, db, scratch_pool);

  /* Commit or rollback the sqlite transaction. */
  if (err)
    {
      svn_error_clear(exec_sql(db, "ROLLBACK TRANSACTION;"));
      return svn_error_return(err);
    }

  return svn_error_return(exec_sql(db, "COMMIT TRANSACTION;"));
}
