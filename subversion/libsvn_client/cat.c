/*
 * cat.c:  implementation of the 'cat' command
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

/* ==================================================================== */



/*** Includes. ***/

#include "svn_client.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_subst.h"
#include "svn_io.h"
#include "svn_time.h"
#include "svn_path.h"
#include "client.h"


/*** Code. ***/

svn_error_t *
svn_client_cat (svn_stream_t *out,
                const char *path_or_url,
                const svn_opt_revision_t *revision,
                svn_client_ctx_t *ctx,
                apr_pool_t *pool)
{
  svn_ra_plugin_t *ra_lib;
  void *ra_baton, *session;
  svn_revnum_t rev;
  svn_node_kind_t url_kind;
  svn_string_t *eol_style;
  svn_string_t *keywords;
  apr_hash_t *props;
  const char *auth_dir;
  const char *url;

  SVN_ERR (svn_client_url_from_path (&url, path_or_url, pool));
  if (! url)
    return svn_error_createf (SVN_ERR_ENTRY_MISSING_URL, NULL,
                              "'%s' has no URL", path_or_url);


  /* Get the RA library that handles URL. */
  SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, url, pool));

  SVN_ERR (svn_client__dir_if_wc (&auth_dir, "", pool));

  /* Open a repository session to the URL. */
  SVN_ERR (svn_client__open_ra_session (&session, ra_lib, url, auth_dir, NULL,
                                        NULL, FALSE, FALSE,
                                        ctx, pool));

  /* Resolve REVISION into a real revnum. */
  SVN_ERR (svn_client__get_revision_number (&rev, ra_lib, session,
                                            revision, path_or_url, pool));
  if (! SVN_IS_VALID_REVNUM (rev))
    SVN_ERR (ra_lib->get_latest_revnum (session, &rev, pool));

  /* Decide if the URL is a file or directory. */
  SVN_ERR (ra_lib->check_path (session, "", rev, &url_kind, pool));

  if (url_kind == svn_node_dir)
    return svn_error_createf(SVN_ERR_CLIENT_IS_DIRECTORY, NULL,
                             "URL '%s' refers to a directory", url);

  /* Grab some properties we need to know in order to figure out if anything 
     special needs to be done with this file. */
  SVN_ERR (ra_lib->get_file (session, "", rev, NULL, NULL, &props, pool));

  eol_style = apr_hash_get (props, SVN_PROP_EOL_STYLE, APR_HASH_KEY_STRING);
  keywords = apr_hash_get (props, SVN_PROP_KEYWORDS, APR_HASH_KEY_STRING);

  if (! eol_style && ! keywords)
    {
      /* It's a file with no special eol style or keywords. */
      SVN_ERR (ra_lib->get_file (session, "", rev, out, NULL, NULL, pool));
    }
  else
    {
      svn_subst_keywords_t kw = { 0 };
      svn_subst_eol_style_t style;
      const char *temp_dir;
      const char *tmp_filename;
      svn_stream_t *tmp_stream;
      apr_file_t *tmp_file;
      apr_status_t apr_err;
      apr_off_t off = 0;
      const char *eol = NULL;

      /* grab a temporary file to write the target to. */
      SVN_ERR (svn_io_temp_dir (&temp_dir, pool));
      SVN_ERR (svn_io_open_unique_file (&tmp_file, &tmp_filename,
                 svn_path_join (temp_dir, "tmp", pool), ".tmp",
                 TRUE, pool));

      tmp_stream = svn_stream_from_aprfile (tmp_file, pool);

      SVN_ERR (ra_lib->get_file (session, "", rev, tmp_stream, 
                                 NULL, NULL, pool));

      /* rewind our stream. */
      apr_err = apr_file_seek (tmp_file, APR_SET, &off);
      if (apr_err)
        return svn_error_createf (apr_err, NULL, "Seek failed on '%s'.",
                                  tmp_filename);

      if (eol_style)
        svn_subst_eol_style_from_value (&style, &eol, eol_style->data);

      if (keywords)
        {
          svn_string_t *cmt_rev, *cmt_date, *cmt_author;
          apr_time_t when = 0;

          cmt_rev = apr_hash_get (props, SVN_PROP_ENTRY_COMMITTED_REV,
                                  APR_HASH_KEY_STRING);
          cmt_date = apr_hash_get (props, SVN_PROP_ENTRY_COMMITTED_DATE,
                                   APR_HASH_KEY_STRING);
          cmt_author = apr_hash_get (props, SVN_PROP_ENTRY_LAST_AUTHOR,
                                     APR_HASH_KEY_STRING);
          if (cmt_date)
            SVN_ERR (svn_time_from_cstring (&when, cmt_date->data, pool));

          SVN_ERR (svn_subst_build_keywords
                   (&kw, keywords->data, 
                    cmt_rev->data,
                    url,
                    when,
                    cmt_author ? cmt_author->data : NULL,
                    pool));
        }

      SVN_ERR (svn_subst_translate_stream (tmp_stream, out, eol, FALSE, &kw,
                                           TRUE));

      SVN_ERR (svn_stream_close (tmp_stream));
    }

  return SVN_NO_ERROR;
}
