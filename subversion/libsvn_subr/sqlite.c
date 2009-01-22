/* sqlite.c
 *
 * ====================================================================
 * Copyright (c) 2008 CollabNet.  All rights reserved.
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

#include "svn_types.h"
#include "svn_error.h"
#include "svn_pools.h"
#include "svn_io.h"
#include "svn_path.h"

#include "private/svn_sqlite.h"
#include "svn_private_config.h"
#include "private/svn_dep_compat.h"


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
  fprintf(stderr, "SQLITE SQL is \"%s\"\n", sql);
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


svn_error_t *
svn_sqlite__exec(svn_sqlite__db_t *db, const char *sql)
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
svn_sqlite__transaction_begin(svn_sqlite__db_t *db)
{
  return svn_sqlite__exec(db, "BEGIN TRANSACTION;");
}

svn_error_t *
svn_sqlite__transaction_commit(svn_sqlite__db_t *db)
{
  return svn_sqlite__exec(db, "COMMIT TRANSACTION;");
}

svn_error_t *
svn_sqlite__transaction_rollback(svn_sqlite__db_t *db)
{
  return svn_sqlite__exec(db, "ROLLBACK TRANSACTION;");
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
  return step_with_expectation(stmt, FALSE);
}

svn_error_t *
svn_sqlite__step_row(svn_sqlite__stmt_t *stmt)
{
  return step_with_expectation(stmt, TRUE);
}

svn_error_t *
svn_sqlite__step(svn_boolean_t *got_row, svn_sqlite__stmt_t *stmt)
{
  int sqlite_result = sqlite3_step(stmt->s3stmt);

  if (sqlite_result != SQLITE_DONE && sqlite_result != SQLITE_ROW)
    {
      SQLITE_ERR(sqlite_result, stmt->db);
      /* Reset the statement. */
      svn_error_clear(svn_sqlite__reset(stmt));
    }

  *got_row = (sqlite_result == SQLITE_ROW);

  return SVN_NO_ERROR;
}

static svn_error_t *
vbindf(svn_sqlite__stmt_t *stmt, const char *fmt, va_list ap)
{
  int count;

  for (count = 1; *fmt; fmt++, count++)
    {
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
  return err;
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

const char *
svn_sqlite__column_text(svn_sqlite__stmt_t *stmt, int column)
{
  return (const char *) sqlite3_column_text(stmt->s3stmt, column);
}

svn_revnum_t
svn_sqlite__column_revnum(svn_sqlite__stmt_t *stmt, int column)
{
  return (svn_revnum_t) sqlite3_column_int64(stmt->s3stmt, column);
}

svn_boolean_t
svn_sqlite__column_boolean(svn_sqlite__stmt_t *stmt, int column)
{
  return (sqlite3_column_int64(stmt->s3stmt, column) == 0
          ? FALSE : TRUE);
}

int
svn_sqlite__column_int(svn_sqlite__stmt_t *stmt, int column)
{
  return sqlite3_column_int(stmt->s3stmt, column);
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
  return SVN_NO_ERROR;
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


static svn_error_t *
upgrade_format(svn_sqlite__db_t *db, int current_schema, int latest_schema,
               const char * const *upgrade_sql, apr_pool_t *scratch_pool)
{
  while (current_schema < latest_schema)
    {
      const char *pragma_cmd;

      /* Go to the next schema */
      current_schema++;

      /* Run the upgrade SQL */
      SVN_ERR(svn_sqlite__exec(db, upgrade_sql[current_schema]));

      /* Update the user version pragma */
      pragma_cmd = apr_psprintf(scratch_pool,
                                "PRAGMA user_version = %d;",
                                current_schema);
      SVN_ERR(svn_sqlite__exec(db, pragma_cmd));
    }

  return SVN_NO_ERROR;
}


/* Check the schema format of the database, upgrading it if necessary.
   Return SVN_ERR_SQLITE_UNSUPPORTED_SCHEMA if the schema format is too new,
   or SVN_ERR_SQLITE_ERROR if an sqlite error occurs during validation.
   Return SVN_NO_ERROR if everything is okay. */
static svn_error_t *
check_format(svn_sqlite__db_t *db, int latest_schema,
             const char * const *upgrade_sql, apr_pool_t *scratch_pool)
{
  svn_sqlite__stmt_t *stmt;
  int current_schema;

  SVN_ERR(svn_sqlite__prepare(&stmt, db, "PRAGMA user_version;", scratch_pool));
  SVN_ERR(svn_sqlite__step_row(stmt));

  /* Validate that the schema exists as expected. */
  current_schema = svn_sqlite__column_int(stmt, 0);

  SVN_ERR(svn_sqlite__finalize(stmt));

  if (current_schema == latest_schema)
    return SVN_NO_ERROR;

  if (current_schema < latest_schema)
    return upgrade_format(db, current_schema, latest_schema, upgrade_sql,
                          scratch_pool);

  return svn_error_createf(SVN_ERR_SQLITE_UNSUPPORTED_SCHEMA, NULL,
                           _("Schema format %d not recognized"),
                           current_schema);
}

/* If possible, verify that SQLite was compiled in a thread-safe
   manner. */
static svn_error_t *
init_sqlite(void)
{
  if (sqlite3_libversion_number() < SQLITE_VERSION_NUMBER) {
    return svn_error_createf(SVN_ERR_SQLITE_ERROR, NULL,
                             _("SQLite compiled for %s, but running with %s"),
                             SQLITE_VERSION, sqlite3_libversion());
  }

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
  SQLITE_ERR_MSG(sqlite3_config(SQLITE_CONFIG_MULTITHREAD),
                 "Could not configure SQLite");
  SQLITE_ERR_MSG(sqlite3_initialize(), "Could not initialize SQLite");
#endif

  /* NOTE: if more work is performed here, then consider using
     svn_atomic__init_once() for the call to this function. */

  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__open(svn_sqlite__db_t **db, const char *path,
                 svn_sqlite__mode_t mode, const char * const statements[],
                 int latest_schema, const char * const *upgrade_sql,
                 apr_pool_t *result_pool, apr_pool_t *scratch_pool)
{
  static svn_boolean_t sqlite_initialized = FALSE;

  if (! sqlite_initialized)
    {
      /* There is a potential initialization race condition here, but
         it currently isn't worth guarding against (e.g. with a mutex). */
      SVN_ERR(init_sqlite());
      sqlite_initialized = TRUE;
    }

  /* ### use a pool cleanup to close this? (instead of __close()) */
  *db = apr_palloc(result_pool, sizeof(**db));

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
       so this is probably redundant... */
    /* ### yeah. remove when autoconf magic is done for init/config. */
#ifdef SQLITE_OPEN_NOMUTEX
    flags |= SQLITE_OPEN_NOMUTEX;
#endif

    /* Open the database. Note that a handle is returned, even when an error
       occurs (except for out-of-memory); thus, we can safely use it to
       extract an error message and construct an svn_error_t. */
    SQLITE_ERR(sqlite3_open_v2(path, &(*db)->db3, flags, NULL), *db);
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
          return svn_error_createf(SVN_ERR_WC_CORRUPT, NULL,
                                   _("Expected SQLite database not found: %s"),
                                   svn_path_local_style(path, scratch_pool));
      }
    }
  else if (mode == svn_sqlite__mode_rwcreate)
    {
      /* do nothing - older SQLite's will create automatically. */
    }
  else
    SVN_ERR_MALFUNCTION();

  SQLITE_ERR(sqlite3_open(path, &(*db)->db3), *db);
#endif

  /* Retry until timeout when database is busy. */
  SQLITE_ERR(sqlite3_busy_timeout((*db)->db3, BUSY_TIMEOUT), *db);
#ifdef SQLITE3_DEBUG
  sqlite3_trace((*db)->db3, sqlite_tracer, (*db)->db3);
#endif

  SVN_ERR(svn_sqlite__exec(*db, "PRAGMA case_sensitive_like=on;"));

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

  return SVN_NO_ERROR;
}

svn_error_t *
svn_sqlite__close(svn_sqlite__db_t *db, svn_error_t *err)
{
  int result;
  int i;

  /* Finalize any existing prepared statements. */
  for (i = 0; i < db->nbr_statements; i++)
    {
      if (db->prepared_stmts[i])
        err = svn_error_compose_create(
                        svn_sqlite__finalize(db->prepared_stmts[i]), err);
    }
  
  result = sqlite3_close(db->db3);
  /* If there's a pre-existing error, return it. */
  /* ### If the connection close also fails, say something about it as well? */
  SVN_ERR(err);
  SQLITE_ERR(result, db);
  return SVN_NO_ERROR;
}
