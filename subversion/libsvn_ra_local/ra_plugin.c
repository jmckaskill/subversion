/*
 * ra_plugin.c : the main RA module for local repository access
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

#include "ra_local.h"
#include "svn_ra.h"
#include "svn_fs.h"
#include "svn_delta.h"
#include "svn_repos.h"
#include "svn_pools.h"
#include "svn_time.h"
#include "svn_private_config.h"

#define APR_WANT_STRFUNC
#include <apr_want.h>

#include <assert.h>

/*----------------------------------------------------------------*/


/* The reporter vtable needed by do_update() */
typedef struct reporter_baton_t
{
  svn_ra_local__session_baton_t *session;
  void *report_baton;

} reporter_baton_t;


static void *
make_reporter_baton (svn_ra_local__session_baton_t *session,
                     void *report_baton,
                     apr_pool_t *pool)
{
  reporter_baton_t *rbaton = apr_palloc (pool, sizeof (*rbaton));
  rbaton->session = session;
  rbaton->report_baton = report_baton;
  return rbaton;
}


static svn_error_t *
reporter_set_path (void *reporter_baton,
                   const char *path,
                   svn_revnum_t revision,
                   svn_boolean_t start_empty,
                   apr_pool_t *pool)
{
  reporter_baton_t *rbaton = reporter_baton;
  return svn_repos_set_path (rbaton->report_baton, path,
                             revision, start_empty, pool);
}


static svn_error_t *
reporter_delete_path (void *reporter_baton,
                      const char *path,
                      apr_pool_t *pool)
{
  reporter_baton_t *rbaton = reporter_baton;
  return svn_repos_delete_path (rbaton->report_baton, path, pool);
}


static svn_error_t *
reporter_link_path (void *reporter_baton,
                    const char *path,
                    const char *url,
                    svn_revnum_t revision,
                    svn_boolean_t start_empty,
                    apr_pool_t *pool)
{
  reporter_baton_t *rbaton = reporter_baton;
  const char *fs_path = NULL;
  int repos_url_len;

  url = svn_path_uri_decode(url, pool);
  repos_url_len = strlen(rbaton->session->repos_url);
  if (strncmp(url, rbaton->session->repos_url, repos_url_len) != 0)
    return svn_error_createf (SVN_ERR_RA_ILLEGAL_URL, NULL,
                              _("'%s'\n"
                                "is not the same repository as\n"
                                "'%s'"), url, rbaton->session->repos_url);
  fs_path = url + repos_url_len;

  return svn_repos_link_path (rbaton->report_baton, path,
                              fs_path, revision, start_empty, pool);
}


static svn_error_t *
reporter_finish_report (void *reporter_baton,
                        apr_pool_t *pool)
{
  reporter_baton_t *rbaton = reporter_baton;
  return svn_repos_finish_report (rbaton->report_baton, pool);
}


static svn_error_t *
reporter_abort_report (void *reporter_baton,
                       apr_pool_t *pool)
{
  reporter_baton_t *rbaton = reporter_baton;
  return svn_repos_abort_report (rbaton->report_baton, pool);
}


static const svn_ra_reporter_t ra_local_reporter = 
{
  reporter_set_path,
  reporter_delete_path,
  reporter_link_path,
  reporter_finish_report,
  reporter_abort_report
};

static svn_error_t *
svn_ra_local__get_file_revs (void *session_baton,
                             const char *path,
                             svn_revnum_t start,
                             svn_revnum_t end,
                             svn_ra_file_rev_handler_t handler,
                             void *handler_baton,
                             apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *sbaton = session_baton;
  const char *abs_path = sbaton->fs_path;

  /* Concatenate paths */
  abs_path = svn_path_join (abs_path, path, pool);

  return svn_repos_get_file_revs (sbaton->repos, abs_path, start, end,
                                  handler, handler_baton, pool);
}

static svn_error_t *
get_username (svn_ra_local__session_baton_t *session,
              apr_pool_t *pool)
{
  svn_auth_iterstate_t *iterstate;

  /* If we've already found the username don't ask for it again */
  if (session->username)
    return SVN_NO_ERROR;

  /* Get a username somehow, so we have some svn:author property to
     attach to a commit. */
  if (! session->callbacks->auth_baton)
    {
      session->username = "";
    }
  else
    {
      void *creds;
      svn_auth_cred_username_t *username_creds;
      SVN_ERR (svn_auth_first_credentials (&creds, &iterstate,
                                           SVN_AUTH_CRED_USERNAME,
                                           session->uuid, /* realmstring */
                                           session->callbacks->auth_baton,
                                           pool));

      /* No point in calling next_creds(), since that assumes that the
         first_creds() somehow failed to authenticate.  But there's no
         challenge going on, so we use whatever creds we get back on
         the first try. */
      username_creds = creds;
      if (username_creds && username_creds->username)
        {
          session->username = apr_pstrdup (pool, username_creds->username);
          svn_error_clear (svn_auth_save_credentials (iterstate, pool));
        }
      else
        session->username = "";
    }

  return SVN_NO_ERROR;
}


/*----------------------------------------------------------------*/

/** The RA plugin routines **/


static svn_error_t *
svn_ra_local__open (void **session_baton,
                    const char *repos_URL,
                    const svn_ra_callbacks_t *callbacks,
                    void *callback_baton,
                    apr_hash_t *config,
                    apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *session;
  
  /* Allocate and stash the session_baton args we have already. */
  session = apr_pcalloc (pool, sizeof(*session));
  session->pool = pool;
  session->repository_URL = repos_URL;
  
  /* Look through the URL, figure out which part points to the
     repository, and which part is the path *within* the
     repository. */
  SVN_ERR_W (svn_ra_local__split_URL (&(session->repos),
                                      &(session->repos_url),
                                      &(session->fs_path),
                                      session->repository_URL,
                                      session->pool),
             _("Unable to open an ra_local session to URL"));

  /* Encode the repos_url into repos_root for get_repos_root. */
  session->repos_root = svn_path_uri_encode (session->repos_url, pool);

  /* Cache the filesystem object from the repos here for
     convenience. */
  session->fs = svn_repos_fs (session->repos);

  /* Cache the repository UUID as well */
  SVN_ERR (svn_fs_get_uuid (session->fs, &session->uuid, session->pool));

  /* Stuff the callbacks/baton here. */
  session->callbacks = callbacks;
  session->callback_baton = callback_baton;

  /* Be sure username is NULL so we know to look it up / ask for it */
  session->username = NULL;

  *session_baton = session;
  return SVN_NO_ERROR;
}



static svn_error_t *
svn_ra_local__get_latest_revnum (void *session_baton,
                                 svn_revnum_t *latest_revnum,
                                 apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  SVN_ERR (svn_fs_youngest_rev (latest_revnum, baton->fs, pool));

  return SVN_NO_ERROR;
}



static svn_error_t *
svn_ra_local__get_dated_revision (void *session_baton,
                                  svn_revnum_t *revision,
                                  apr_time_t tm,
                                  apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  SVN_ERR (svn_repos_dated_revision (revision, baton->repos, tm, pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
svn_ra_local__change_rev_prop (void *session_baton,
                               svn_revnum_t rev,
                               const char *name,
                               const svn_string_t *value,
                               apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  SVN_ERR (get_username (baton, pool));

  SVN_ERR (svn_repos_fs_change_rev_prop (baton->repos, rev, baton->username,
                                         name, value, pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
svn_ra_local__get_uuid (void *session_baton,
                        const char **uuid,
                        apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  *uuid = baton->uuid;

  return SVN_NO_ERROR;
}

static svn_error_t *
svn_ra_local__get_repos_root (void *session_baton,
                              const char **url,
                              apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  *url = baton->repos_root;

  return SVN_NO_ERROR;
}

static svn_error_t *
svn_ra_local__rev_proplist (void *session_baton,
                            svn_revnum_t rev,
                            apr_hash_t **props,
                            apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  SVN_ERR (svn_fs_revision_proplist (props, baton->fs, rev, pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
svn_ra_local__rev_prop (void *session_baton,
                        svn_revnum_t rev,
                        const char *name,
                        svn_string_t **value,
                        apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *baton = 
    (svn_ra_local__session_baton_t *) session_baton;

  SVN_ERR (svn_fs_revision_prop (value, baton->fs, rev, name, pool));

  return SVN_NO_ERROR;
}


struct deltify_etc_baton
{
  svn_fs_t *fs;                    /* the fs to deltify in */
  apr_pool_t *pool;                /* pool for scratch work */
  svn_commit_callback_t callback;  /* the original callback */
  void *callback_baton;            /* the original callback's baton */
};

/* This implements 'svn_commit_callback_t'.  Its invokes the original
   (wrapped) callback, but also does deltification on the new revision.
   BATON is 'struct deltify_etc_baton *'. */
static svn_error_t * 
deltify_etc (svn_revnum_t new_revision,
             const char *date,
             const char *author,
             void *baton)
{
  struct deltify_etc_baton *db = baton;
  svn_error_t *err1, *err2;

  /* Invoke the original callback first, in case someone's waiting to
     know the revision number so they can go off and annotate an
     issue or something. */
  err1 = (*db->callback) (new_revision, date, author, db->callback_baton);

  /* But, deltification shouldn't be stopped just because someone's
     random callback failed, so proceed unconditionally on to
     deltification. */
  err2 = svn_fs_deltify_revision (db->fs, new_revision, db->pool);

  /* It's more interesting if the original callback failed, so let
     that one dominate. */
  if (err1)
    {
      svn_error_clear (err2);
      return err1;
    }

  return err2;
}


static svn_error_t *
svn_ra_local__get_commit_editor (void *session_baton,
                                 const svn_delta_editor_t **editor,
                                 void **edit_baton,
                                 const char *log_msg,
                                 svn_commit_callback_t callback,
                                 void *callback_baton,
                                 apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *sess = session_baton;
  struct deltify_etc_baton *db = apr_palloc (pool, sizeof(*db));

  db->fs = sess->fs;
  db->pool = pool;
  db->callback = callback;
  db->callback_baton = callback_baton;

  SVN_ERR (get_username (sess, pool));

  /* Get the repos commit-editor */     
  SVN_ERR (svn_repos_get_commit_editor (editor, edit_baton, sess->repos,
                                        sess->repos_url, sess->fs_path,
                                        sess->username, log_msg,
                                        deltify_etc, db, pool));

  return SVN_NO_ERROR;
}



static svn_error_t *
make_reporter (void *session_baton,
               const svn_ra_reporter_t **reporter,
               void **report_baton,
               svn_revnum_t revision,
               const char *target,
               const char *other_url,
               svn_boolean_t text_deltas,
               svn_boolean_t recurse,
               svn_boolean_t ignore_ancestry,
               const svn_delta_editor_t *editor,
               void *edit_baton,
               apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *sbaton = session_baton;
  void *rbaton;
  int repos_url_len;
  const char *other_fs_path = NULL;

  /* Get the HEAD revision if one is not supplied. */
  if (! SVN_IS_VALID_REVNUM(revision))
    SVN_ERR (svn_ra_local__get_latest_revnum (sbaton, &revision, pool));

  /* If OTHER_URL was provided, validate it and convert it into a
     regular filesystem path. */
  if (other_url)
    {
      other_url = svn_path_uri_decode (other_url, pool);
      repos_url_len = strlen(sbaton->repos_url);
      
      /* Sanity check:  the other_url better be in the same repository as
         the original session url! */
      if (strncmp (other_url, sbaton->repos_url, repos_url_len) != 0)
        return svn_error_createf 
          (SVN_ERR_RA_ILLEGAL_URL, NULL,
           _("'%s'\n"
             "is not the same repository as\n"
             "'%s'"), other_url, sbaton->repos_url);

      other_fs_path = other_url + repos_url_len;
    }

  /* Pass back our reporter */
  *reporter = &ra_local_reporter;

  SVN_ERR (get_username (sbaton, pool));
  
  /* Build a reporter baton. */
  SVN_ERR (svn_repos_begin_report (&rbaton,
                                   revision,
                                   sbaton->username,
                                   sbaton->repos, 
                                   sbaton->fs_path,
                                   target, 
                                   other_fs_path,
                                   text_deltas,
                                   recurse,
                                   ignore_ancestry,
                                   editor, 
                                   edit_baton,
                                   NULL,
                                   NULL,
                                   pool));
  
  /* Wrap the report baton given us by the repos layer with our own
     reporter baton. */
  *report_baton = make_reporter_baton (sbaton, rbaton, pool);

  return SVN_NO_ERROR;
}


static svn_error_t *
svn_ra_local__do_update (void *session_baton,
                         const svn_ra_reporter_t **reporter,
                         void **report_baton,
                         svn_revnum_t update_revision,
                         const char *update_target,
                         svn_boolean_t recurse,
                         const svn_delta_editor_t *update_editor,
                         void *update_baton,
                         apr_pool_t *pool)
{
  return make_reporter (session_baton,
                        reporter,
                        report_baton,
                        update_revision,
                        update_target,
                        NULL,
                        TRUE,
                        recurse,
                        FALSE,
                        update_editor,
                        update_baton,
                        pool);
}


static svn_error_t *
svn_ra_local__do_switch (void *session_baton,
                         const svn_ra_reporter_t **reporter,
                         void **report_baton,
                         svn_revnum_t update_revision,
                         const char *update_target,
                         svn_boolean_t recurse,
                         const char *switch_url,
                         const svn_delta_editor_t *update_editor,
                         void *update_baton,
                         apr_pool_t *pool)
{
  return make_reporter (session_baton,
                        reporter,
                        report_baton,
                        update_revision,
                        update_target,
                        switch_url,
                        TRUE,
                        recurse,
                        TRUE,
                        update_editor,
                        update_baton,
                        pool);
}


static svn_error_t *
svn_ra_local__do_status (void *session_baton,
                         const svn_ra_reporter_t **reporter,
                         void **report_baton,
                         const char *status_target,
                         svn_revnum_t revision,
                         svn_boolean_t recurse,
                         const svn_delta_editor_t *status_editor,
                         void *status_baton,
                         apr_pool_t *pool)
{
  return make_reporter (session_baton,
                        reporter,
                        report_baton,
                        revision,
                        status_target,
                        NULL,
                        FALSE,
                        recurse,
                        FALSE,
                        status_editor,
                        status_baton,
                        pool);
}


static svn_error_t *
svn_ra_local__do_diff (void *session_baton,
                       const svn_ra_reporter_t **reporter,
                       void **report_baton,
                       svn_revnum_t update_revision,
                       const char *update_target,
                       svn_boolean_t recurse,
                       svn_boolean_t ignore_ancestry,
                       const char *switch_url,
                       const svn_delta_editor_t *update_editor,
                       void *update_baton,
                       apr_pool_t *pool)
{
  return make_reporter (session_baton,
                        reporter,
                        report_baton,
                        update_revision,
                        update_target,
                        switch_url,
                        TRUE,
                        recurse,
                        ignore_ancestry,
                        update_editor,
                        update_baton,
                        pool);
}


static svn_error_t *
svn_ra_local__get_log (void *session_baton,
                       const apr_array_header_t *paths,
                       svn_revnum_t start,
                       svn_revnum_t end,
                       svn_boolean_t discover_changed_paths,
                       svn_boolean_t strict_node_history,
                       svn_log_message_receiver_t receiver,
                       void *receiver_baton,
                       apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *sbaton = session_baton;
  apr_array_header_t *abs_paths
    = apr_array_make (sbaton->pool, paths->nelts, sizeof (const char *));
  int i;

  for (i = 0; i < paths->nelts; i++)
    {
      const char *abs_path = "";
      const char *relative_path = (((const char **)(paths)->elts)[i]);

      /* Append the relative paths to the base FS path to get an
         absolute repository path. */
      abs_path = svn_path_join (sbaton->fs_path, relative_path, sbaton->pool);
      (*((const char **)(apr_array_push (abs_paths)))) = abs_path;
    }

  return svn_repos_get_logs (sbaton->repos,
                             abs_paths,
                             start,
                             end,
                             discover_changed_paths,
                             strict_node_history,
                             receiver,
                             receiver_baton,
                             sbaton->pool);
}


static svn_error_t *
svn_ra_local__do_check_path (void *session_baton,
                             const char *path,
                             svn_revnum_t revision,
                             svn_node_kind_t *kind,
                             apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *sbaton = session_baton;
  svn_fs_root_t *root;
  const char *abs_path = sbaton->fs_path;

  /* ### Not sure if this counts as a workaround or not.  The
     session baton uses the empty string to mean root, and not
     sure that should change.  However, it would be better to use
     a path library function to add this separator -- hardcoding
     it is totally bogus.  See issue #559, though it may be only
     tangentially related. */
  if (abs_path[0] == '\0')
    abs_path = "/";

  /* If we were given a relative path to append, append it. */
  if (path)
    abs_path = svn_path_join (abs_path, path, pool);

  if (! SVN_IS_VALID_REVNUM (revision))
    SVN_ERR (svn_fs_youngest_rev (&revision, sbaton->fs, pool));
  SVN_ERR (svn_fs_revision_root (&root, sbaton->fs, revision, pool));
  SVN_ERR (svn_fs_check_path (kind, root, abs_path, pool));
  return SVN_NO_ERROR;
}



static svn_error_t *
get_node_props (apr_hash_t **props,
                svn_ra_local__session_baton_t *sbaton,
                svn_fs_root_t *root,
                const char *path,
                apr_pool_t *pool)
{
  svn_revnum_t cmt_rev;
  const char *cmt_date, *cmt_author;

  /* Create a hash with props attached to the fs node. */
  SVN_ERR (svn_fs_node_proplist (props, root, path, pool));
      
  /* Now add some non-tweakable metadata to the hash as well... */
    
  /* The so-called 'entryprops' with info about CR & friends. */
  SVN_ERR (svn_repos_get_committed_info (&cmt_rev, &cmt_date,
                                         &cmt_author, root, path, pool));

  apr_hash_set (*props, 
                SVN_PROP_ENTRY_COMMITTED_REV, 
                APR_HASH_KEY_STRING, 
                svn_string_createf (pool, "%ld", cmt_rev));
  apr_hash_set (*props, 
                SVN_PROP_ENTRY_COMMITTED_DATE, 
                APR_HASH_KEY_STRING, 
                cmt_date ? svn_string_create (cmt_date, pool) : NULL);
  apr_hash_set (*props, 
                SVN_PROP_ENTRY_LAST_AUTHOR, 
                APR_HASH_KEY_STRING, 
                cmt_author ? svn_string_create (cmt_author, pool) : NULL);
  apr_hash_set (*props, 
                SVN_PROP_ENTRY_UUID,
                APR_HASH_KEY_STRING, 
                svn_string_create (sbaton->uuid, pool));

  /* We have no 'wcprops' in ra_local, but might someday. */
  
  return SVN_NO_ERROR;
}


/* Getting just one file. */
static svn_error_t *
svn_ra_local__get_file (void *session_baton,
                        const char *path,
                        svn_revnum_t revision,
                        svn_stream_t *stream,
                        svn_revnum_t *fetched_rev,
                        apr_hash_t **props,
                        apr_pool_t *pool)
{
  svn_fs_root_t *root;
  svn_stream_t *contents;
  svn_revnum_t youngest_rev;
  svn_ra_local__session_baton_t *sbaton = session_baton;
  const char *abs_path = sbaton->fs_path;

  /* ### Not sure if this counts as a workaround or not.  The
     session baton uses the empty string to mean root, and not
     sure that should change.  However, it would be better to use
     a path library function to add this separator -- hardcoding
     it is totally bogus.  See issue #559, though it may be only
     tangentially related. */
  if (abs_path[0] == '\0')
    abs_path = "/";

  /* If we were given a relative path to append, append it. */
  if (path)
    abs_path = svn_path_join (abs_path, path, pool);

  /* Open the revision's root. */
  if (! SVN_IS_VALID_REVNUM (revision))
    {
      SVN_ERR (svn_fs_youngest_rev (&youngest_rev, sbaton->fs, pool));
      SVN_ERR (svn_fs_revision_root (&root, sbaton->fs, youngest_rev, pool));
      if (fetched_rev != NULL)
        *fetched_rev = youngest_rev;
    }
  else
    SVN_ERR (svn_fs_revision_root (&root, sbaton->fs, revision, pool));

  if (stream)
    {
      /* Get a stream representing the file's contents. */
      SVN_ERR (svn_fs_file_contents (&contents, root, abs_path, pool));
      
      /* Now push data from the fs stream back at the caller's stream.
         Note that this particular RA layer does not computing a
         checksum as we go, and confirming it against the repository's
         checksum when done.  That's because it calls
         svn_fs_file_contents() directly, which already checks the
         stored checksum, and all we're doing here is writing bytes in
         a loop.  Truly, Nothing Can Go Wrong :-).  But RA layers that
         go over a network should confirm the checksum. */
      SVN_ERR (svn_stream_copy (contents, stream, pool));
    }

  /* Handle props if requested. */
  if (props)
    SVN_ERR (get_node_props (props, sbaton, root, abs_path, pool));

  return SVN_NO_ERROR;
}



/* Getting a directory's entries */
static svn_error_t *
svn_ra_local__get_dir (void *session_baton,
                       const char *path,
                       svn_revnum_t revision,
                       apr_hash_t **dirents,
                       svn_revnum_t *fetched_rev,
                       apr_hash_t **props,
                       apr_pool_t *pool)
{
  svn_fs_root_t *root;
  svn_revnum_t youngest_rev;
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  svn_ra_local__session_baton_t *sbaton = session_baton;
  const char *abs_path = sbaton->fs_path;
  apr_pool_t *subpool;

  /* ### Not sure if this counts as a workaround or not.  The
     session baton uses the empty string to mean root, and not
     sure that should change.  However, it would be better to use
     a path library function to add this separator -- hardcoding
     it is totally bogus.  See issue #559, though it may be only
     tangentially related. */
  if (abs_path[0] == '\0')
    abs_path = "/";

  /* If we were given a relative path to append, append it. */
  if (path)
    abs_path = svn_path_join (abs_path, path, pool);

  /* Open the revision's root. */
  if (! SVN_IS_VALID_REVNUM (revision))
    {
      SVN_ERR (svn_fs_youngest_rev (&youngest_rev, sbaton->fs, pool));
      SVN_ERR (svn_fs_revision_root (&root, sbaton->fs, youngest_rev, pool));
      if (fetched_rev != NULL)
        *fetched_rev = youngest_rev;
    }
  else
    SVN_ERR (svn_fs_revision_root (&root, sbaton->fs, revision, pool));

  if (dirents)
    {
      /* Get the dir's entries. */
      SVN_ERR (svn_fs_dir_entries (&entries, root, abs_path, pool));
      
      /* Loop over the fs dirents, and build a hash of general
         svn_dirent_t's. */
      *dirents = apr_hash_make (pool);
      subpool = svn_pool_create (pool);
      for (hi = apr_hash_first (pool, entries); hi; hi = apr_hash_next (hi))
        {
          const void *key;
          void *val;
          apr_hash_t *prophash;
          const char *datestring, *entryname, *fullpath;
          svn_fs_dirent_t *fs_entry;
          svn_dirent_t *entry = apr_pcalloc (pool, sizeof(*entry));

          apr_hash_this (hi, &key, NULL, &val);
          entryname = (const char *) key;
          fs_entry = (svn_fs_dirent_t *) val;
          
          /* node kind */
          fullpath = svn_path_join (abs_path, entryname, subpool);
          entry->kind = fs_entry->kind;
          
          /* size  */
          if (entry->kind == svn_node_dir)
            entry->size = 0;
          else
            SVN_ERR (svn_fs_file_length (&(entry->size), root,
                                         fullpath, subpool));
          
          /* has_props? */
          SVN_ERR (svn_fs_node_proplist (&prophash, root, fullpath, subpool));
          entry->has_props = (apr_hash_count (prophash)) ? TRUE : FALSE;
          
          /* created_rev & friends */
          SVN_ERR (svn_repos_get_committed_info (&(entry->created_rev),
                                                 &datestring,
                                                 &(entry->last_author),
                                                 root, fullpath, subpool));
          if (datestring)
            SVN_ERR (svn_time_from_cstring (&(entry->time), datestring, pool));
          if (entry->last_author)
            entry->last_author = apr_pstrdup (pool, entry->last_author);
          
          /* Store. */
          apr_hash_set (*dirents, entryname, APR_HASH_KEY_STRING, entry);
          
          svn_pool_clear (subpool);
        }
    }

  /* Handle props if requested. */
  if (props)
    SVN_ERR (get_node_props (props, sbaton, root, abs_path, pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
svn_ra_local__get_locations (void *session_baton,
                             apr_hash_t **locations,
                             const char *relative_path,
                             svn_revnum_t peg_revision,
                             apr_array_header_t *location_revisions,
                             apr_pool_t *pool)
{
  svn_ra_local__session_baton_t *sbaton = session_baton;
  const char *abs_path;

  /* Append the relative path to the base FS path to get an
     absolute repository path. */
  abs_path = svn_path_join (sbaton->fs_path, relative_path, pool);

  SVN_ERR (svn_repos_trace_node_locations (sbaton->fs, locations, abs_path,
                                           peg_revision, location_revisions,
                                           pool));

  return SVN_NO_ERROR;
}


/*----------------------------------------------------------------*/

/** The ra_plugin **/

static const svn_ra_plugin_t ra_local_plugin = 
{
  "ra_local",
  N_("Module for accessing a repository on local disk."),
  svn_ra_local__open,
  svn_ra_local__get_latest_revnum,
  svn_ra_local__get_dated_revision,
  svn_ra_local__change_rev_prop,
  svn_ra_local__rev_proplist,
  svn_ra_local__rev_prop,
  svn_ra_local__get_commit_editor,
  svn_ra_local__get_file,
  svn_ra_local__get_dir,
  svn_ra_local__do_update,
  svn_ra_local__do_switch,
  svn_ra_local__do_status,
  svn_ra_local__do_diff,
  svn_ra_local__get_log,
  svn_ra_local__do_check_path,
  svn_ra_local__get_uuid,
  svn_ra_local__get_repos_root,
  svn_ra_local__get_locations,
  svn_ra_local__get_file_revs
};


/*----------------------------------------------------------------*/

/** The One Public Routine, called by libsvn_client **/

svn_error_t *
svn_ra_local_init (int abi_version,
                   apr_pool_t *pool,
                   apr_hash_t *hash)
{
  if (abi_version < 1
      || abi_version > SVN_RA_ABI_VERSION)
    return svn_error_createf (SVN_ERR_RA_UNSUPPORTED_ABI_VERSION, NULL,
                              _("Unsupported RA plugin ABI version (%d) "
                                "for ra_local"), abi_version);

  apr_hash_set (hash, "file", APR_HASH_KEY_STRING, &ra_local_plugin);

  return SVN_NO_ERROR;
}
