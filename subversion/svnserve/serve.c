/*
 * serve.c :  Functions for serving the Subversion protocol
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



#define APR_WANT_STRFUNC
#include <apr_want.h>
#include <apr_general.h>
#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_network_io.h>
#include <apr_user.h>
#include <apr_file_info.h>
#include <apr_md5.h>

#include <svn_types.h>
#include <svn_string.h>
#include <svn_pools.h>
#include <svn_error.h>
#include <svn_ra.h>
#include <svn_ra_svn.h>
#include <svn_repos.h>
#include <svn_path.h>
#include <svn_time.h>
#include <svn_utf.h>
#include <svn_md5.h>

#include "server.h"

typedef struct {
  svn_repos_t *repos;
  const char *url;         /* Original URL passed from client */
  const char *repos_url;   /* Decoded URL to base of repository */
  const char *fs_path;     /* Decoded base path inside repository */
  const char *user;
  svn_boolean_t read_only; /* Disallow commit and change-rev-prop */
  svn_fs_t *fs;            /* For convenience; same as svn_repos_fs(repos) */
} server_baton_t;

typedef struct {
  svn_revnum_t *new_rev;
  const char **date;
  const char **author;
} commit_callback_baton_t;

typedef struct {
  const char *repos_url;
  void *report_baton;
  svn_error_t *err;
} report_driver_baton_t;

typedef struct {
  const char *fs_path;
  svn_ra_svn_conn_t *conn;
} log_baton_t;

/* Verify that URL is inside REPOS_URL and get its fs path. */
static svn_error_t *get_fs_path(const char *repos_url, const char *url,
                                const char **fs_path, apr_pool_t *pool)
{
  apr_size_t len;

  len = strlen(repos_url);
  if (strncmp(url, repos_url, len) != 0)
    return svn_error_createf(SVN_ERR_RA_ILLEGAL_URL, NULL,
                             "'%s'\nis not the same repository as\n'%s'",
                             url, repos_url);
  *fs_path = url + len;
  return SVN_NO_ERROR;
}

/* --- REPORTER COMMAND SET --- */

/* To allow for pipelining, reporter commands have no reponses.  If we
 * get an error, we ignore all subsequent reporter commands and return
 * the error finish_report, to be handled by the calling command.
 */

static svn_error_t *set_path(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                             apr_array_header_t *params, void *baton)
{
  report_driver_baton_t *b = baton;
  const char *path;
  svn_revnum_t rev;
  svn_boolean_t start_empty;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "crb",
                                 &path, &rev, &start_empty));
  if (!b->err)
    b->err = svn_repos_set_path(b->report_baton, path, rev, start_empty, pool);
  return SVN_NO_ERROR;
}

static svn_error_t *delete_path(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                apr_array_header_t *params, void *baton)
{
  report_driver_baton_t *b = baton;
  const char *path;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c", &path));
  if (!b->err)
    b->err = svn_repos_delete_path(b->report_baton, path, pool);
  return SVN_NO_ERROR;
}
    
static svn_error_t *link_path(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                              apr_array_header_t *params, void *baton)
{
  report_driver_baton_t *b = baton;
  const char *path, *url, *fs_path;
  svn_revnum_t rev;
  svn_boolean_t start_empty;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "ccrb",
                                 &path, &url, &rev, &start_empty));
  url = svn_path_uri_decode(url, pool);
  if (!b->err)
    b->err = get_fs_path(b->repos_url, url, &fs_path, pool);
  if (!b->err)
    b->err = svn_repos_link_path(b->report_baton, path, fs_path, rev,
                                 start_empty, pool);
  return SVN_NO_ERROR;
}

static svn_error_t *finish_report(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                  apr_array_header_t *params, void *baton)
{
  report_driver_baton_t *b = baton;

  /* No arguments to parse. */
  if (!b->err)
    b->err = svn_repos_finish_report(b->report_baton);
  return SVN_NO_ERROR;
}

static svn_error_t *abort_report(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                   apr_array_header_t *params, void *baton)
{
  report_driver_baton_t *b = baton;

  /* No arguments to parse. */
  svn_error_clear(svn_repos_abort_report(b->report_baton));
  return SVN_NO_ERROR;
}

static const svn_ra_svn_cmd_entry_t report_commands[] = {
  { "set-path",      set_path },
  { "delete-path",   delete_path },
  { "link-path",     link_path },
  { "finish-report", finish_report, TRUE },
  { "abort-report",  abort_report,  TRUE },
  { NULL }
};

/* Accept a report from the client, drive the network editor with the
 * result, and then write an empty command response.  If there is a
 * non-protocol failure, accept_report will abort the edit and return
 * a command error to be reported by handle_commands(). */
static svn_error_t *accept_report(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                  server_baton_t *b, svn_revnum_t rev,
                                  const char *target, const char *tgt_path,
                                  svn_boolean_t text_deltas,
                                  svn_boolean_t recurse,
                                  svn_boolean_t ignore_ancestry)
{
  const svn_delta_editor_t *editor;
  void *edit_baton, *report_baton;
  report_driver_baton_t rb;
  svn_error_t *err;

  /* Make an svn_repos report baton.  Tell it to drive the network editor
   * when the report is complete. */
  svn_ra_svn_get_editor(&editor, &edit_baton, conn, pool, NULL, NULL);
  SVN_CMD_ERR(svn_repos_begin_report(&report_baton, rev, b->user, b->repos,
                                     b->fs_path, target, tgt_path, text_deltas,
                                     recurse, ignore_ancestry, editor,
                                     edit_baton, pool));

  rb.repos_url = b->repos_url;
  rb.report_baton = report_baton;
  rb.err = NULL;
  err = svn_ra_svn_handle_commands(conn, pool, report_commands, &rb);
  if (err)
    {
      /* Network or protocol error while handling commands. */
      svn_error_clear(rb.err);
      return err;
    }
  else if (rb.err)
    {
      /* Some failure during the reporting or editing operations. */
      editor->abort_edit(edit_baton, pool);
      SVN_CMD_ERR(rb.err);
    }
  SVN_ERR(svn_ra_svn_write_cmd_response(conn, pool, ""));
  return SVN_NO_ERROR;
}

/* --- MAIN COMMAND SET --- */

/* Write out a property list.  PROPS is allowed to be NULL, in which case
 * an empty list will be written out; this happens if the client could
 * have asked for props but didn't. */
static svn_error_t *write_proplist(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                   apr_hash_t *props)
{
  apr_hash_index_t *hi;
  const void *namevar;
  void *valuevar;
  const char *name;
  svn_string_t *value;

  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  if (props)
    {
      for (hi = apr_hash_first(pool, props); hi; hi = apr_hash_next(hi))
        {
          apr_hash_this(hi, &namevar, NULL, &valuevar);
          name = namevar;
          value = valuevar;
          SVN_ERR(svn_ra_svn_write_tuple(conn, pool, "cs", name, value));
        }
    }
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  return SVN_NO_ERROR;
}

static const char *kind_word(svn_node_kind_t kind)
{
  switch (kind)
    {
    case svn_node_none:
      return "none";
    case svn_node_file:
      return "file";
    case svn_node_dir:
      return "dir";
    case svn_node_unknown:
      return "unknown";
    default:
      abort();
    }
}

/* ### This really belongs in libsvn_repos. */
/* Get the properties for a path, with hardcoded committed-info values. */
static svn_error_t *get_props(apr_hash_t **props, svn_fs_root_t *root,
                              const char *path, apr_pool_t *pool)
{
  svn_string_t *str;
  svn_revnum_t crev;
  const char *cdate, *cauthor, *uuid;

  /* Get the properties. */
  SVN_ERR(svn_fs_node_proplist(props, root, path, pool));

  /* Hardcode the values for the committed revision, date, and author. */
  SVN_ERR(svn_repos_get_committed_info(&crev, &cdate, &cauthor, root,
                                       path, pool));
  str = svn_string_create(apr_psprintf(pool, "%" SVN_REVNUM_T_FMT, crev),
                          pool);
  apr_hash_set(*props, SVN_PROP_ENTRY_COMMITTED_REV, APR_HASH_KEY_STRING, str);
  str = (cdate) ? svn_string_create(cdate, pool) : NULL;
  apr_hash_set(*props, SVN_PROP_ENTRY_COMMITTED_DATE, APR_HASH_KEY_STRING,
               str);
  str = (cauthor) ? svn_string_create(cauthor, pool) : NULL;
  apr_hash_set(*props, SVN_PROP_ENTRY_LAST_AUTHOR, APR_HASH_KEY_STRING, str);

  /* Hardcode the values for the UUID. */
  SVN_ERR(svn_fs_get_uuid(svn_fs_root_fs(root), &uuid, pool));
  str = (uuid) ? svn_string_create(uuid, pool) : NULL;
  apr_hash_set(*props, SVN_PROP_ENTRY_UUID, APR_HASH_KEY_STRING, str);
  
  return SVN_NO_ERROR;
}

static svn_error_t *must_not_be_read_only(server_baton_t *b)
{
  if (b->read_only)
    return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, NULL,
                            "Connection is read-only, cannot modify "
                            "repository.");
  return SVN_NO_ERROR;
}

static svn_error_t *get_latest_rev(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                   apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  svn_revnum_t rev;

  SVN_CMD_ERR(svn_fs_youngest_rev(&rev, b->fs, pool));
  SVN_ERR(svn_ra_svn_write_cmd_response(conn, pool, "r", rev));
  return SVN_NO_ERROR;
}

static svn_error_t *get_dated_rev(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                  apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  svn_revnum_t rev;
  apr_time_t tm;
  const char *timestr;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c", &timestr));
  SVN_CMD_ERR(svn_time_from_cstring(&tm, timestr, pool));
  SVN_CMD_ERR(svn_repos_dated_revision(&rev, b->repos, tm, pool));
  SVN_ERR(svn_ra_svn_write_cmd_response(conn, pool, "r", rev));
  return SVN_NO_ERROR;
}

static svn_error_t *change_rev_prop(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                    apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  svn_revnum_t rev;
  const char *name;
  svn_string_t *value;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "rcs", &rev, &name, &value));
  SVN_CMD_ERR(must_not_be_read_only(b));
  SVN_CMD_ERR(svn_repos_fs_change_rev_prop(b->repos, rev, b->user, name, value,
                                           pool));
  SVN_ERR(svn_ra_svn_write_cmd_response(conn, pool, ""));
  return SVN_NO_ERROR;
}

static svn_error_t *rev_proplist(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                 apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  svn_revnum_t rev;
  apr_hash_t *props;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "r", &rev));
  SVN_CMD_ERR(svn_fs_revision_proplist(&props, b->fs, rev, pool));
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(svn_ra_svn_write_word(conn, pool, "success"));
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(write_proplist(conn, pool, props));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  return SVN_NO_ERROR;
}

static svn_error_t *rev_prop(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                             apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  svn_revnum_t rev;
  const char *name;
  svn_string_t *value;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "rc", &rev, &name));
  SVN_CMD_ERR(svn_fs_revision_prop(&value, b->fs, rev, name, pool));
  SVN_ERR(svn_ra_svn_write_cmd_response(conn, pool, "(?s)", value));
  return SVN_NO_ERROR;
}

static svn_error_t *commit_done(svn_revnum_t new_rev, const char *date,
                                const char *author, void *baton)
{
  commit_callback_baton_t *ccb = baton;

  *ccb->new_rev = new_rev;
  *ccb->date = date;
  *ccb->author = author;
  return SVN_NO_ERROR;
}

static svn_error_t *commit(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                           apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  const char *log_msg, *date, *author;
  const svn_delta_editor_t *editor;
  void *edit_baton;
  svn_boolean_t aborted;
  commit_callback_baton_t ccb;
  svn_revnum_t new_rev;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c", &log_msg));
  SVN_CMD_ERR(must_not_be_read_only(b));
  ccb.new_rev = &new_rev;
  ccb.date = &date;
  ccb.author = &author;
  SVN_CMD_ERR(svn_repos_get_commit_editor(&editor, &edit_baton, b->repos,
                                          b->repos_url, b->fs_path, b->user,
                                          log_msg, commit_done, &ccb, pool));
  SVN_ERR(svn_ra_svn_write_cmd_response(conn, pool, ""));
  SVN_ERR(svn_ra_svn_drive_editor(conn, pool, editor, edit_baton, &aborted));
  if (!aborted)
    SVN_ERR(svn_ra_svn_write_tuple(conn, pool, "r(?c)(?c)",
                                   new_rev, date, author));
  return SVN_NO_ERROR;
}

static svn_error_t *get_file(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                             apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  const char *path, *full_path, *hex_digest;
  svn_revnum_t rev;
  svn_fs_root_t *root;
  svn_stream_t *contents;
  apr_hash_t *props = NULL;
  svn_string_t write_str;
  char buf[4096];
  apr_size_t len;
  svn_boolean_t want_props, want_contents;
  unsigned char digest[MD5_DIGESTSIZE];
  svn_error_t *err, *write_err;

  /* Parse arguments. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c(?r)bb", &path, &rev,
                                 &want_props, &want_contents));
  if (!SVN_IS_VALID_REVNUM(rev))
    SVN_CMD_ERR(svn_fs_youngest_rev(&rev, b->fs, pool));
  full_path = svn_path_join(b->fs_path, path, pool);

  /* Fetch the properties and a stream for the contents. */
  SVN_CMD_ERR(svn_fs_revision_root(&root, b->fs, rev, pool));
  SVN_CMD_ERR(svn_fs_file_md5_checksum(digest, root, full_path, pool));
  hex_digest = svn_md5_digest_to_cstring(digest, pool);
  if (want_props)
    SVN_CMD_ERR(get_props(&props, root, full_path, pool));
  if (want_contents)
    SVN_CMD_ERR(svn_fs_file_contents(&contents, root, full_path, pool));

  /* Send successful command response with revision and props. */
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(svn_ra_svn_write_word(conn, pool, "success"));
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  if (hex_digest)
    SVN_ERR(svn_ra_svn_write_cstring(conn, pool, hex_digest));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  SVN_ERR(svn_ra_svn_write_number(conn, pool, rev));
  SVN_ERR(write_proplist(conn, pool, props));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));

  /* Now send the file's contents. */
  if (want_contents)
    {
      err = SVN_NO_ERROR;
      while (1)
        {
          len = sizeof(buf);
          err = svn_stream_read(contents, buf, &len);
          if (err)
            break;
          if (len > 0)
            {
              write_str.data = buf;
              write_str.len = len;
              SVN_ERR(svn_ra_svn_write_string(conn, pool, &write_str));
            }
          if (len < sizeof(buf))
            {
              err = svn_stream_close(contents);
              break;
            }
        }
      write_err = svn_ra_svn_write_cstring(conn, pool, "");
      if (write_err)
        {
          svn_error_clear(err);
          return write_err;
        }
      SVN_CMD_ERR(err);
      SVN_ERR(svn_ra_svn_write_cmd_response(conn, pool, ""));
    }

  return SVN_NO_ERROR;
}

static svn_error_t *get_dir(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                            apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  const char *path, *full_path, *file_path, *name, *cauthor, *cdate;
  svn_revnum_t rev;
  apr_hash_t *entries, *props = NULL, *file_props;
  apr_hash_index_t *hi;
  svn_fs_dirent_t *fsent;
  svn_dirent_t *entry;
  const void *key;
  void *val;
  svn_fs_root_t *root;
  apr_pool_t *subpool;
  svn_boolean_t want_props, want_contents;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c(?r)bb", &path, &rev,
                                 &want_props, &want_contents));
  if (!SVN_IS_VALID_REVNUM(rev))
    SVN_CMD_ERR(svn_fs_youngest_rev(&rev, b->fs, pool));
  full_path = svn_path_join(b->fs_path, path, pool);

  /* Fetch the root of the appropriate revision. */
  SVN_CMD_ERR(svn_fs_revision_root(&root, b->fs, rev, pool));

  /* Fetch the directory properties if requested. */
  if (want_props)
    SVN_CMD_ERR(get_props(&props, root, full_path, pool));

  /* Fetch the directory entries if requested. */
  if (want_contents)
    {
      SVN_CMD_ERR(svn_fs_dir_entries(&entries, root, full_path, pool));

      /* Transform the hash table's FS entries into dirents.  This probably
       * belongs in libsvn_repos. */
      subpool = svn_pool_create(pool);
      for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
        {
          apr_hash_this(hi, &key, NULL, &val);
          name = key;
          fsent = val;

          file_path = svn_path_join(full_path, name, subpool);
          entry = apr_pcalloc(pool, sizeof(*entry));

          /* kind */
          entry->kind = fsent->kind;

          /* size */
          if (entry->kind == svn_node_dir)
            entry->size = 0;
          else
            SVN_CMD_ERR(svn_fs_file_length(&entry->size, root, file_path,
                                           subpool));

          /* has_props */
          SVN_CMD_ERR(svn_fs_node_proplist(&file_props, root, file_path,
                                           subpool));
          entry->has_props = (apr_hash_count(file_props) > 0) ? TRUE : FALSE;

          /* created_rev, last_author, time */
          SVN_CMD_ERR(svn_repos_get_committed_info(&entry->created_rev, &cdate,
                                                   &cauthor, root, file_path,
                                                   subpool));
          entry->last_author = apr_pstrdup (pool, cauthor);
          if (cdate)
            SVN_CMD_ERR(svn_time_from_cstring(&entry->time, cdate, subpool));
          else
            entry->time = (time_t) -1;

          /* Store the entry. */
          apr_hash_set(entries, name, APR_HASH_KEY_STRING, entry);
          svn_pool_clear(subpool);
        }
      svn_pool_destroy(subpool);
    }

  /* Write out response. */
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(svn_ra_svn_write_word(conn, pool, "success"));
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(svn_ra_svn_write_number(conn, pool, rev));
  SVN_ERR(write_proplist(conn, pool, props));
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  if (want_contents)
    {
      for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
        {
          apr_hash_this(hi, &key, NULL, &val);
          name = key;
          entry = val;
          cdate = (entry->time == (time_t) -1) ? NULL
            : svn_time_to_cstring(entry->time, pool);
          SVN_ERR(svn_ra_svn_write_tuple(conn, pool, "cwnbr(?c)(?c)", name,
                                         kind_word(entry->kind),
                                         (apr_uint64_t) entry->size,
                                         entry->has_props, entry->created_rev,
                                         cdate, entry->last_author));
        }
    }
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  return SVN_NO_ERROR;
}


static svn_error_t *update(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                           apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  svn_revnum_t rev;
  const char *target;
  svn_boolean_t recurse;

  /* Parse the arguments. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "(?r)cb", &rev, &target,
                                 &recurse));
  if (svn_path_is_empty(target))
    target = NULL;  /* ### Compatibility hack, shouldn't be needed */
  if (!SVN_IS_VALID_REVNUM(rev))
    SVN_CMD_ERR(svn_fs_youngest_rev(&rev, b->fs, pool));

  return accept_report(conn, pool, b, rev, target, NULL, TRUE, recurse, FALSE);
}

static svn_error_t *switch_cmd(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                               apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  svn_revnum_t rev;
  const char *target;
  const char *switch_url, *switch_path;
  svn_boolean_t recurse;

  /* Parse the arguments. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "(?r)cbc", &rev, &target,
                                 &recurse, &switch_url));
  if (svn_path_is_empty(target))
    target = NULL;  /* ### Compatibility hack, shouldn't be needed */
  if (!SVN_IS_VALID_REVNUM(rev))
    SVN_CMD_ERR(svn_fs_youngest_rev(&rev, b->fs, pool));
  SVN_CMD_ERR(get_fs_path(b->repos_url, switch_url, &switch_path, pool));

  return accept_report(conn, pool, b, rev, target, switch_path, TRUE, recurse,
                       TRUE);
}

static svn_error_t *status(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                           apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  svn_revnum_t rev;
  const char *target;
  svn_boolean_t recurse;

  /* Parse the arguments. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "cb", &target, &recurse));
  if (svn_path_is_empty(target))
    target = NULL;  /* ### Compatibility hack, shouldn't be needed */

  SVN_CMD_ERR(svn_fs_youngest_rev(&rev, b->fs, pool));
  return accept_report(conn, pool, b, rev, target, NULL, FALSE, recurse,
                       FALSE);
}

static svn_error_t *diff(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                         apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  svn_revnum_t rev;
  const char *target, *versus_url, *versus_path;
  svn_boolean_t recurse, ignore_ancestry;

  /* Parse the arguments. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "(?r)cbbc", &rev, &target,
                                 &recurse, &ignore_ancestry, &versus_url));
  if (svn_path_is_empty(target))
    target = NULL;  /* ### Compatibility hack, shouldn't be needed */
  if (!SVN_IS_VALID_REVNUM(rev))
    SVN_CMD_ERR(svn_fs_youngest_rev(&rev, b->fs, pool));
  SVN_CMD_ERR(get_fs_path(b->repos_url, versus_url, &versus_path, pool));

  return accept_report(conn, pool, b, rev, target, versus_path, TRUE, recurse,
                       ignore_ancestry);
}

/* Send a log entry to the client. */
static svn_error_t *log_receiver(void *baton, apr_hash_t *changed_paths,
                                 svn_revnum_t rev, const char *author,
                                 const char *date, const char *message,
                                 apr_pool_t *pool)
{
  log_baton_t *b = baton;
  svn_ra_svn_conn_t *conn = b->conn;
  apr_hash_index_t *h;
  const void *key;
  void *val;
  const char *path;
  svn_log_changed_path_t *change;
  char action[2];

  SVN_ERR(svn_ra_svn_start_list(conn, pool));

  /* Element 1: changed paths */
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  if (changed_paths)
    {
      for (h = apr_hash_first(pool, changed_paths); h; h = apr_hash_next(h))
        {
          apr_hash_this(h, &key, NULL, &val);
          path = key;
          change = val;
          action[0] = change->action;
          action[1] = '\0';
          SVN_ERR(svn_ra_svn_write_tuple(conn, pool, "cw(?cr)", path, action,
                                         change->copyfrom_path,
                                         change->copyfrom_rev));
        }
    }
  SVN_ERR(svn_ra_svn_end_list(conn, pool));

  /* Element 2: revision number */
  SVN_ERR(svn_ra_svn_write_number(conn, pool, rev));

  /* Element 3: author */
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  if (author)
    SVN_ERR(svn_ra_svn_write_cstring(conn, pool, author));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));

  /* Element 4: date */
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  if (date)
    SVN_ERR(svn_ra_svn_write_cstring(conn, pool, date));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));

  /* Element 5: message */
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  if (message)
    SVN_ERR(svn_ra_svn_write_cstring(conn, pool, message));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));

  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  return SVN_NO_ERROR;
}

static svn_error_t *log_cmd(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                            apr_array_header_t *params, void *baton)
{
  svn_error_t *err, *write_err;
  server_baton_t *b = baton;
  svn_revnum_t start_rev, end_rev;
  const char *full_path;
  svn_boolean_t changed_paths, strict_node;
  apr_array_header_t *paths, *full_paths;
  svn_ra_svn_item_t *elt;
  int i;
  log_baton_t lb;

  /* Parse the arguments. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "l(?r)(?r)bb", &paths,
                                 &start_rev, &end_rev, &changed_paths,
                                 &strict_node));
  full_paths = apr_array_make(pool, paths->nelts, sizeof(const char *));
  for (i = 0; i < paths->nelts; i++)
    {
      elt = &((svn_ra_svn_item_t *) paths->elts)[i];
      if (elt->kind != SVN_RA_SVN_STRING)
        return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                                "Log path entry not a string");
      full_path = svn_path_join(b->fs_path, elt->u.string->data, pool);
      *((const char **) apr_array_push(full_paths)) = full_path;
    }

  /* Get logs.  (Can't report errors back to the client at this point.) */
  lb.fs_path = b->fs_path;
  lb.conn = conn;
  err = svn_repos_get_logs(b->repos, full_paths, start_rev, end_rev,
                           changed_paths, strict_node, log_receiver, &lb,
                           pool);

  write_err = svn_ra_svn_write_word(conn, pool, "done");
  if (write_err)
    {
      svn_error_clear(err);
      return write_err;
    }
  SVN_CMD_ERR(err);
  SVN_ERR(svn_ra_svn_write_cmd_response(conn, pool, ""));
  return SVN_NO_ERROR;
}

static svn_error_t *check_path(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                               apr_array_header_t *params, void *baton)
{
  server_baton_t *b = baton;
  svn_revnum_t rev;
  const char *path, *full_path;
  svn_fs_root_t *root;
  svn_node_kind_t kind;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c(?r)", &path, &rev));
  if (!SVN_IS_VALID_REVNUM(rev))
    SVN_CMD_ERR(svn_fs_youngest_rev(&rev, b->fs, pool));
  full_path = svn_path_join(b->fs_path, path, pool);
  SVN_CMD_ERR(svn_fs_revision_root(&root, b->fs, rev, pool));
  SVN_CMD_ERR(svn_fs_check_path(&kind, root, full_path, pool));
  SVN_ERR(svn_ra_svn_write_cmd_response(conn, pool, "w", kind_word(kind)));
  return SVN_NO_ERROR;
}

static const svn_ra_svn_cmd_entry_t main_commands[] = {
  { "get-latest-rev",  get_latest_rev },
  { "get-dated-rev",   get_dated_rev },
  { "change-rev-prop", change_rev_prop },
  { "rev-proplist",    rev_proplist },
  { "rev-prop",        rev_prop },
  { "commit",          commit },
  { "get-file",        get_file },
  { "get-dir",         get_dir },
  { "update",          update },
  { "switch",          switch_cmd },
  { "status",          status },
  { "diff",            diff },
  { "log",             log_cmd },
  { "check-path",      check_path },
  { NULL }
};

/* Skip past the scheme part of a URL, including the tunnel specification
 * if present.  Return NULL if the scheme part is invalid for ra_svn. */
static const char *skip_scheme_part(const char *url)
{
  if (strncmp(url, "svn", 3) != 0)
    return NULL;
  url += 3;
  if (*url == '+')
    url += strcspn(url, ":");
  if (strncmp(url, "://", 3) != 0)
    return NULL;
  return url + 3;
}

static svn_error_t *find_repos(const char *url, const char *root,
                               svn_repos_t **repos, const char **repos_url,
                               const char **fs_path, apr_pool_t *pool)
{
  svn_error_t *err;
  apr_status_t apr_err;
  const char *client_path, *full_path, *candidate;
  const char *client_path_apr, *root_apr;
  char *buffer;

  /* Decode any escaped characters in the URL. */
  url = svn_path_uri_decode(url, pool);

  /* Skip past the scheme and authority part. */
  client_path = skip_scheme_part(url);
  if (client_path == NULL)
    return svn_error_createf(SVN_ERR_BAD_URL, NULL,
                             "Non-svn URL passed to svn server: '%s'", url);
  client_path = strchr(client_path, '/');
  client_path = (client_path == NULL) ? "" : client_path + 1;

  SVN_ERR(svn_path_cstring_from_utf8(&client_path_apr,
                                     svn_path_canonicalize(client_path, pool),
                                     pool));

  SVN_ERR(svn_path_cstring_from_utf8(&root_apr,
                                     svn_path_canonicalize(root, pool),
                                     pool));

  /* Join the server-configured root with the client path. */
  apr_err = apr_filepath_merge(&buffer, root_apr, client_path_apr,
                               APR_FILEPATH_SECUREROOT, pool);

  if(apr_err)
    return svn_error_create(SVN_ERR_BAD_FILENAME, NULL,
                            "Couldn't determine repository path.");

  SVN_ERR(svn_path_cstring_to_utf8(&full_path, buffer, pool));
  full_path = svn_path_canonicalize(full_path, pool);

  /* Search for a repository in the full path. */
  candidate = full_path;
  while (1)
    {
      err = svn_repos_open(repos, candidate, pool);
      if (err == SVN_NO_ERROR)
        break;
      if (!*candidate || strcmp(candidate, "/") == 0)
        return svn_error_createf(SVN_ERR_RA_SVN_REPOS_NOT_FOUND, NULL,
                                 "No repository found in '%s'", url);
      candidate = svn_path_dirname(candidate, pool);
    }
  *fs_path = apr_pstrdup(pool, full_path + strlen(candidate));
  *repos_url = apr_pstrmemdup(pool, url, strlen(url) - strlen(*fs_path));
  return SVN_NO_ERROR;
}

svn_error_t *serve(svn_ra_svn_conn_t *conn, const char *root,
                   svn_boolean_t tunnel, svn_boolean_t read_only,
                   apr_pool_t *pool)
{
  svn_error_t *err, *io_err;
  apr_uint64_t ver;
  const char *mech, *mecharg, *user = NULL, *client_url, *repos_url, *fs_path;
  apr_array_header_t *caplist;
  svn_repos_t *repos;
  server_baton_t b;
  const char *uuid;
  svn_boolean_t valid_mech = FALSE;

  /* Send greeting, saying we only support protocol version 1, the
   * anonymous authentication mechanism, and no extensions. */
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(svn_ra_svn_write_word(conn, pool, "success"));
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  /* Our minimum and maximum supported protocol version is 1. */
  SVN_ERR(svn_ra_svn_write_number(conn, pool, 1));
  SVN_ERR(svn_ra_svn_write_number(conn, pool, 1));
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  /* We support anonymous and maybe external authentication. */
  SVN_ERR(svn_ra_svn_write_word(conn, pool, "ANONYMOUS"));
#if APR_HAS_USER
  if (tunnel)
    SVN_ERR(svn_ra_svn_write_word(conn, pool, "EXTERNAL"));
#endif
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  /* We have no special capabilities. */
  SVN_ERR(svn_ra_svn_start_list(conn, pool));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));
  SVN_ERR(svn_ra_svn_end_list(conn, pool));

  /* Read client response.  This should specify version 1, the
   * mechanism, a mechanism argument, and possibly some
   * capabilities. */
  SVN_ERR(svn_ra_svn_read_tuple(conn, pool, "nw(?c)l", &ver, &mech, &mecharg,
                                &caplist));

#if APR_HAS_USER
  if (tunnel && strcmp(mech, "EXTERNAL") == 0)
    {
      apr_uid_t uid;
      apr_gid_t gid;

      if (!mecharg)  /* Must be present */
        return SVN_NO_ERROR;
      if (apr_uid_current(&uid, &gid, pool) != APR_SUCCESS
          || apr_uid_name_get((char **) &user, uid, pool) != APR_SUCCESS)
        {
          SVN_ERR(svn_ra_svn_write_tuple(conn, pool, "w(c)", "failure",
                                         "Can't determine username"));
          return SVN_NO_ERROR;
        }
      if (*mecharg && strcmp(mecharg, user) != 0)
        {
          SVN_ERR(svn_ra_svn_write_tuple(conn, pool, "w(c)", "failure",
                                         "Requested username does not match"));
          return SVN_NO_ERROR;
        }
      valid_mech = TRUE;
    }
#endif

  if (strcmp(mech, "ANONYMOUS") == 0)
    valid_mech = TRUE;

  if (!valid_mech)  /* Client gave us an unlisted mech. */
    return SVN_NO_ERROR;

  /* Write back a success notification. */
  SVN_ERR(svn_ra_svn_write_tuple(conn, pool, "w()", "success"));

  /* This is where the security layer would go into effect if we
   * supported security layers, which is a ways off. */

  /* Read client's URL. */
  SVN_ERR(svn_ra_svn_read_tuple(conn, pool, "c", &client_url));

  err = find_repos(client_url, root, &repos, &repos_url, &fs_path, pool);
  if (err)
    {
      io_err = svn_ra_svn_write_cmd_failure(conn, pool, err);
      svn_error_clear(err);
      if (io_err)
        return io_err;
      SVN_ERR(svn_ra_svn_flush(conn, pool));
      return SVN_NO_ERROR;
    }
  
  b.repos = repos;
  b.url = client_url;
  b.repos_url = repos_url;
  b.fs_path = fs_path;
  b.user = user;
  b.fs = svn_repos_fs(repos);
  b.read_only = read_only;

  SVN_ERR(svn_fs_get_uuid(b.fs, &uuid, pool));
  svn_ra_svn_write_cmd_response(conn, pool, "c", uuid);

  return svn_ra_svn_handle_commands(conn, pool, main_commands, &b);
}
