/*
 * copy.c:  copy/move wrappers around wc 'copy' functionality.
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
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

#include <string.h>
#include <assert.h>
#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "client.h"


/*** Code. ***/

/*
 * if (not exist src_path)
 *   return ERR_BAD_SRC error
 *
 * if (exist dst_path)
 *   {
 *     if (dst_path is directory)
 *       copy src_path into dst_path as basename (src_path)
 *     else
 *       return ERR_OBSTRUCTION error
 *   }
 * else
 *   copy src_path into parent_of_dst_path as basename (dst_path)
 *
 * if (this is a move)
 *   delete src_path
 */


/* Copy SRC_PATH into DST_PATH as DST_BASENAME, deleting SRC_PATH
   afterwards if IS_MOVE is TRUE.  Use POOL for all necessary
   allocations. */
static svn_error_t *
wc_to_wc_copy (svn_stringbuf_t *src_path,
               svn_stringbuf_t *dst_path,
               svn_boolean_t is_move,
               apr_pool_t *pool)
{
  svn_node_kind_t src_kind, dst_kind;
  svn_stringbuf_t *unused, *parent = dst_path, *basename;

  /* Verify that SRC_PATH exists. */
  SVN_ERR (svn_io_check_path (src_path, &src_kind, pool));
  if (src_kind == svn_node_none)
    return svn_error_createf (SVN_ERR_UNKNOWN_NODE_KIND, 0, NULL, pool,
                              "path `%s' does not exist.", src_path->data);

  /* If DST_PATH does not exist, then its basename will become a new
     file or dir added to its parent (possibly an implicit '.').  If
     DST_PATH is a dir, then SRC_PATH's basename will become a new
     file or dir within DST_PATH itself.  Else if it's a file, just
     error out. */
  SVN_ERR (svn_io_check_path (dst_path, &dst_kind, pool));
  if (dst_kind == svn_node_none)
    svn_path_split (dst_path, &parent, &basename, svn_path_local_style, pool);
  else if (dst_kind == svn_node_dir)
    svn_path_split (src_path, &unused, &basename, svn_path_local_style, pool);
  else
    return svn_error_createf (SVN_ERR_WC_ENTRY_EXISTS, 0, NULL, pool,
                              "file `%s' already exists.", dst_path->data);

  /* Perform the copy and (optionally) delete. */
  SVN_ERR (svn_wc_copy (src_path, parent, basename, pool));
  if (is_move)
    SVN_ERR (svn_wc_delete (src_path, pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
repos_to_repos_copy (svn_stringbuf_t *src_url, 
                     svn_revnum_t src_rev, 
                     svn_stringbuf_t *dst_url, 
                     svn_client_auth_baton_t *auth_baton,
                     svn_stringbuf_t *message,
                     svn_boolean_t is_move,
                     apr_pool_t *pool)
{
  svn_stringbuf_t *top_url, *src_rel, *dst_rel, *basename, *unused;
  apr_array_header_t *src_pieces = NULL, *dst_pieces = NULL;
  svn_revnum_t youngest;
  void *ra_baton, *sess;
  svn_ra_plugin_t *ra_lib;
  svn_node_kind_t src_kind, dst_kind;
  const svn_delta_edit_fns_t *editor;
  void *edit_baton, *root_baton, *baton;
  void **batons;
  int i = 0;

  /* ### TODO:  Currently, this function will violate the depth-first
     rule of editors when doing a move of something up into one of its
     grandparent directories, such as:

        svn mv http://server/repos/dir1/dir2/file http://server/repos/dir1

     While it seems to work just fine, we might want to evaluate this
     from a purely "correctness" standpoint.
  */

  /* We have to open our session to the longest path common to both
     SRC_URL and DST_URL in the repository so we can do existence
     checks on both paths, and so we can operate on both paths in the
     case of a move. */
  top_url = svn_path_get_longest_ancestor (src_url, dst_url,
                                           svn_path_url_style, pool);

  /* Get the portions of the SRC and DST URLs that are relative to
     TOP_URL. */
  src_rel = svn_path_is_child (top_url, src_url, svn_path_local_style, pool);
  if (src_rel)
    {
      src_pieces = svn_path_decompose (src_rel, svn_path_url_style, pool);
      if ((! src_pieces) || (! src_pieces->nelts))
        return svn_error_createf 
          (SVN_ERR_WC_PATH_NOT_FOUND, 0, NULL, pool,
           "error decomposing relative path `%s'", src_rel->data);
    }

  dst_rel = svn_path_is_child (top_url, dst_url, svn_path_local_style, pool);
  if (dst_rel)
    {
      dst_pieces = svn_path_decompose (dst_rel, svn_path_url_style, pool);
      if ((! dst_pieces) || (! dst_pieces->nelts))
        return svn_error_createf 
          (SVN_ERR_WC_PATH_NOT_FOUND, 0, NULL, pool,
           "error decomposing relative path `%s'", dst_rel->data);
    }

  /* Allocate room for the root baton, the pieces of the
     source's or destination's path, and the destination itself. */
  {
    int num, num2;
    num = src_pieces ? src_pieces->nelts : 0;
    if (((num2 = (dst_pieces ? dst_pieces->nelts : 0))) > num)
      num = num2;

    batons = apr_palloc (pool, sizeof (void *) * (num + 2));
  }

  /* Get the RA vtable that matches URL. */
  SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, top_url->data, pool));

  /* Open an RA session for the URL. Note that we don't have a local
     directory, nor a place to put temp files or store the auth data. */
  SVN_ERR (svn_client__open_ra_session (&sess, ra_lib, top_url, NULL,
                                        FALSE, FALSE, auth_baton, pool));

  SVN_ERR (ra_lib->get_latest_revnum (sess, &youngest));

  /* Use YOUNGEST for copyfrom args if not provided. */
  if (! SVN_IS_VALID_REVNUM (src_rev))
    src_rev = youngest;
  
  /* Verify that SRC_URL exists in the repository. */
  SVN_ERR (ra_lib->check_path (&src_kind, sess,
                               src_rel ? src_rel->data : NULL, src_rev));
  if (src_kind == svn_node_none)
    return svn_error_createf 
      (SVN_ERR_FS_NOT_FOUND, 0, NULL, pool,
       "path `%s' does not exist in revision `%ld'", src_url->data, src_rev);

  /* Figure out the basename that will result from this operation. */
  SVN_ERR (ra_lib->check_path (&dst_kind, sess, 
                               dst_rel ? dst_rel->data : NULL, youngest));
  if (dst_kind == svn_node_none)
    {
      svn_path_split (dst_url, &unused, &basename, svn_path_local_style, pool);
      dst_pieces->nelts--; /* hack - where's apr_array_pop()? */
    }
  else if (dst_kind == svn_node_dir)
    svn_path_split (src_url, &unused, &basename, svn_path_local_style, pool);
  else
    return svn_error_createf (SVN_ERR_FS_ALREADY_EXISTS, 0, NULL, pool,
                              "file `%s' already exists.", dst_url->data);

  /* Fetch RA commit editor. */
  SVN_ERR (ra_lib->get_commit_editor
           (sess, &editor, &edit_baton,
            NULL,  /* change this if ever want to return new_rev */
            NULL,  /* change this if ever want to return commit date */
            NULL,  /* change this if ever want to return commit author */
            message, NULL, NULL, NULL, NULL));

  /* Drive that editor, baby! */
  SVN_ERR (editor->open_root (edit_baton, youngest, &root_baton));

  /* Stuff the root baton here for convenience. */
  batons[i] = root_baton;

  /* Open directories down to the place where we need to make our
     copy. */
  if (dst_pieces && dst_pieces->nelts)
    {
      svn_stringbuf_t *piece;

      /* open_directory() all the way down to DST's parent. */
      while (i < dst_pieces->nelts)
        {
          piece = (((svn_stringbuf_t **)(dst_pieces)->elts)[i]);
          SVN_ERR (editor->open_directory (piece, batons[i], 
                                           youngest, &(batons[i + 1])));
          i++;
        }
    }
  /* Add our file/dir with copyfrom history. */
  if (src_kind == svn_node_dir)
    {
      SVN_ERR (editor->add_directory (basename, batons[i], src_url,
                                      src_rev, &baton));
      SVN_ERR (editor->close_directory (baton));
    }
  else
    {
      SVN_ERR (editor->add_file (basename, batons[i], src_url,
                                 src_rev, &baton));
      SVN_ERR (editor->close_file (baton));
    }

  /* Now, close up all those batons (except the root
     baton). */
  while (i)
    {
      SVN_ERR (editor->close_directory (batons[i]));
      batons[i--] = NULL;
    }

  /* If this was a move, we need to remove the SRC_URL. */
  if (is_move)
    {
      svn_stringbuf_t *piece;

      /* If SRC_PIECES is NULL, we're trying to move a directory into
         itself (or one of its chidren...we should have caught that by
         now). */
      assert (src_pieces != NULL);

      /* open_directory() all the way down to SRC's parent. */
      while (i < (src_pieces->nelts - 1))
        {
          piece = (((svn_stringbuf_t **)(src_pieces)->elts)[i]);
          SVN_ERR (editor->open_directory (piece, batons[i],
                                           youngest, &(batons[i + 1])));
          i++;
        }
          
      /* Delete SRC. */
      piece = (((svn_stringbuf_t **)(src_pieces)->elts)[i]);
      SVN_ERR (editor->delete_entry (piece, SVN_INVALID_REVNUM, batons[i]));

      /* Now, close up all those batons (except the root
         baton). */
      while (i)
        {
          SVN_ERR (editor->close_directory (batons[i--]));
        }
    }

  /* Turn off the lights, close up the shop, and go home. */
  SVN_ERR (editor->close_directory (batons[0]));
  SVN_ERR (editor->close_edit (edit_baton));
  SVN_ERR (ra_lib->close (sess));

  return SVN_NO_ERROR;
}


static svn_error_t *
wc_to_repos_copy (svn_stringbuf_t *src_path, 
                  svn_stringbuf_t *dst_url, 
                  svn_client_auth_baton_t *auth_baton,
                  svn_stringbuf_t *message,
                  const svn_delta_edit_fns_t *before_editor,
                  void *before_edit_baton,
                  const svn_delta_edit_fns_t *after_editor,
                  void *after_edit_baton,
                  apr_pool_t *pool)
{
  svn_stringbuf_t *anchor, *target, *parent, *basename;
  void *ra_baton, *sess;
  svn_ra_plugin_t *ra_lib;
  const svn_delta_edit_fns_t *editor;
  void *edit_baton;
  svn_node_kind_t src_kind, dst_kind;

  /* Check the SRC_PATH. */
  SVN_ERR (svn_io_check_path (src_path, &src_kind, pool));

  /* Split the SRC_PATH into a parent and basename. */
  svn_path_split (src_path, &parent, &basename, svn_path_local_style, pool);
  if (svn_path_is_empty (parent, svn_path_local_style))
    parent = svn_stringbuf_create (".", pool);

  /* Split the DST_URL into an anchor and target. */
  svn_path_split (dst_url, &anchor, &target, svn_path_url_style, pool);

  /* Get the RA vtable that matches URL. */
  SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, anchor->data, pool));

  /* Open an RA session for the anchor URL. */
  SVN_ERR (svn_client__open_ra_session (&sess, ra_lib, anchor, parent,
                                        TRUE, TRUE, auth_baton, pool));

  /* Figure out the basename that will result from this operation. */
  SVN_ERR (ra_lib->check_path (&dst_kind, sess, target->data,
                               SVN_INVALID_REVNUM));
  if (dst_kind == svn_node_none)
    /* use target */;
  else if (dst_kind == svn_node_dir)
    {
      /* We need to re-open the RA session from DST_URL instead of its
         parent directory. */
      anchor = dst_url;
      target = svn_stringbuf_dup (basename, pool);
      SVN_ERR (ra_lib->close (sess));

      SVN_ERR (svn_client__open_ra_session (&sess, ra_lib, anchor, src_path,
                                            TRUE, TRUE, auth_baton, pool));
    }
  else
    return svn_error_createf (SVN_ERR_FS_ALREADY_EXISTS, 0, NULL, pool,
                              "file `%s' already exists.", dst_url->data);

  /* Fetch RA commit editor. */
  SVN_ERR (ra_lib->get_commit_editor
           (sess, &editor, &edit_baton,
            NULL,  /* change this if ever want to return new_rev */
            NULL,  /* change this if ever want to return commit date */
            NULL,  /* change this if ever want to return commit author */
            message, NULL, NULL, NULL, NULL));

  /* Co-mingle the before- and after-editors with the commit
     editor. */
  svn_delta_wrap_editor (&editor, &edit_baton,
                         before_editor, before_edit_baton,
                         editor, edit_baton,
                         after_editor, after_edit_baton, pool);

  /* Crawl the working copy, committing as if SRC_PATH was scheduled
     for a copy. */
  SVN_ERR (svn_wc_crawl_as_copy (parent, basename, target,
                                 editor, edit_baton, pool));

  /* Close the RA session. */
  SVN_ERR (ra_lib->close (sess));
  return SVN_NO_ERROR;
}


static svn_error_t *
repos_to_wc_copy (svn_stringbuf_t *src_url,
                  svn_revnum_t src_rev,
                  svn_stringbuf_t *dst_path, 
                  svn_client_auth_baton_t *auth_baton,
                  svn_stringbuf_t *message,
                  const svn_delta_edit_fns_t *before_editor,
                  void *before_edit_baton,
                  const svn_delta_edit_fns_t *after_editor,
                  void *after_edit_baton,
                  apr_pool_t *pool)
{
  void *ra_baton, *sess;
  svn_ra_plugin_t *ra_lib;
  svn_node_kind_t src_kind, dst_kind;
  const svn_delta_edit_fns_t *editor;
  void *edit_baton;

  /* Get the RA vtable that matches URL. */
  SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, src_url->data, pool));

  /* Open a repository session to the given URL. We do not (yet) have a
     working copy, so we don't have a corresponding path and tempfiles
     cannot go into the admin area. We do want to store the resulting
     auth data, though, once the WC is built. */
  SVN_ERR (svn_client__open_ra_session (&sess, ra_lib, src_url, NULL,
                                        TRUE, FALSE, auth_baton, pool));
      
  /* Verify that SRC_URL exists in the repository. */
  SVN_ERR (ra_lib->check_path (&src_kind, sess, "", src_rev));
  if (src_kind == svn_node_none)
    {
      if (SVN_IS_VALID_REVNUM (src_rev))
        return svn_error_createf
          (SVN_ERR_FS_NOT_FOUND, 0, NULL, pool,
           "path `%s' not found in revision `%ld'", src_url->data, src_rev);
      else
        return svn_error_createf
          (SVN_ERR_FS_NOT_FOUND, 0, NULL, pool,
           "path `%s' not found in head revision", src_url->data);
    }

  /* There are two interfering sets of cases to watch out for here:
   *
   * First set:
   *
   *   1) If DST_PATH does not exist, then great.  We're going to
   *      create a new entry in its parent.
   *   2) If it does exist, then it must be a directory and we're
   *      copying to a new entry inside that dir (the entry's name is
   *      the basename of SRC_URL).
   *
   * But while that's all going on, we must also remember:
   *
   *   A) If SRC_URL is a directory in the repository, we can check
   *      it out directly, no problem.
   *   B) If SRC_URL is a file, we have to manually get the editor
   *      started, since there won't be a root to open.
   *
   * I'm going to ignore B for the moment, and implement cases 1 and
   * 2 under A.
   */

  /* First, figure out about dst. */
  SVN_ERR (svn_io_check_path (dst_path, &dst_kind, pool));
  if (dst_kind == svn_node_dir)
    {
      svn_stringbuf_t *unused, *basename;
      svn_path_split (src_url, &unused, &basename, svn_path_url_style, pool);

      /* We shouldn't affect the caller's dst_path, so dup first and
         then extend. */
      dst_path = svn_stringbuf_dup (dst_path, pool);
      svn_path_add_component (dst_path, basename, svn_path_local_style);
    }
  else if (dst_kind != svn_node_none)  /* must be a file */
    return svn_error_createf (SVN_ERR_WC_ENTRY_EXISTS, 0, NULL, pool,
                              "file `%s' already exists.", dst_path->data);

  /* Now that dst_path has possibly been reset, check that there's
     nothing in the way of the upcoming checkout. */
  SVN_ERR (svn_io_check_path (dst_path, &dst_kind, pool));
  if (dst_kind != svn_node_none)
    return svn_error_createf (SVN_ERR_WC_OBSTRUCTED_UPDATE, 0, NULL, pool,
                              "`%s' is in the way", dst_path->data);

  if (src_kind == svn_node_dir)
    {    
      /* Get a checkout editor and wrap it. */
      SVN_ERR (svn_wc_get_checkout_editor (dst_path,
                                           src_url,
                                           src_rev,
                                           1,
                                           &editor,
                                           &edit_baton,
                                           pool));
      
      svn_delta_wrap_editor (&editor, &edit_baton,
                             before_editor, before_edit_baton,
                             editor, edit_baton,
                             after_editor, after_edit_baton, pool);
      
      /* Check out the new tree.  The parent dir will get no entry, so
         it will be as if the new tree isn't really there yet. */
      SVN_ERR (ra_lib->do_checkout (sess, src_rev, 1, editor, edit_baton));

      if (! SVN_IS_VALID_REVNUM(src_rev))
        {
          /* If we just checked out from the "head" revision, that's fine,
             but we don't want to pass a '-1' as a copyfrom_rev to
             svn_wc_add().  That function will dump it right into the
             entry, and when we try to commit later on, the
             'add-dir-with-history' step will be -very- unhappy; it only
             accepts specific revisions.
             
             On the other hand, we *could* say that -1 is a legitimate
             copyfrom_rev, but I think that's bogus.  Somebody made a copy
             from a particular revision;  if they wait a long time to
             commit, it would be terrible if the copied happened from a
             newer revision!! */
          
          /* We just did a checkout; whatever revision we just got, that
             should be the copyfrom_revision when we commit later. */
          svn_wc_entry_t *d_entry;
          SVN_ERR (svn_wc_entry (&d_entry, dst_path, pool));
          src_rev = d_entry->revision;
        }

    } /* end directory case */

  else if (src_kind == svn_node_file)
    {
      apr_status_t status;
      svn_stream_t *fstream;
      apr_file_t *fp;
      svn_revnum_t fetched_rev = 0;
      
      /* Open DST_PATH for writing. */
      status = apr_file_open (&fp, dst_path->data, (APR_CREATE | APR_WRITE),
                              APR_OS_DEFAULT, pool);
      if (status)
        return svn_error_createf (status, 0, NULL, pool,
                                  "failed to open file '%s' for writing.",
                                  dst_path->data);

      /* Create a generic stream that operates on this file.  */
      fstream = svn_stream_from_aprfile (fp, pool);
      
      /* Have the RA layer 'push' data at this stream.  We pass a
         relative path of "", because we opened SRC_URL, which is
         already the full URL to the file. */         
      SVN_ERR (ra_lib->get_file (sess, "", src_rev, fstream, &fetched_rev));

      /* Close the file. */
      status = apr_file_close (fp);
      if (status)
        return svn_error_createf (status, 0, NULL, pool,
                                  "failed to close file '%s'.",
                                  dst_path->data);   
     
      /* Also, if SRC_REV is invalid ('head'), then FETCHED_REV is now
         equal to the revision that was actually retrieved.  This is
         the value we want to use as 'copyfrom_rev' in the call to
         svn_wc_add() below. */
      if (! SVN_IS_VALID_REVNUM (src_rev))
        src_rev = fetched_rev;
    }

  /* Free the RA session. */
  SVN_ERR (ra_lib->close (sess));
      
  /* Schedule the new item for addition-with-history.

     If the new item is a directory, the URLs will be recursively
     rewritten, wcprops removed, and everything marked as 'copied'.
     See comment in svn_wc_add()'s doc about whether svn_wc_add is the
     appropriate place for this. */
  SVN_ERR (svn_wc_add (dst_path, src_url, src_rev, pool));


  return SVN_NO_ERROR;
}


static svn_error_t *
setup_copy (svn_stringbuf_t *src_path,
            svn_revnum_t src_rev,
            svn_stringbuf_t *dst_path,
            svn_client_auth_baton_t *auth_baton,
            svn_stringbuf_t *message,
            const svn_delta_edit_fns_t *before_editor,
            void *before_edit_baton,
            const svn_delta_edit_fns_t *after_editor,
            void *after_edit_baton,
            svn_boolean_t is_move,
            apr_pool_t *pool)
{
  svn_boolean_t src_is_url, dst_is_url;
  svn_string_t path_str;

  /* Are either of our paths URLs? */
  path_str.data = src_path->data;
  path_str.len = src_path->len;
  src_is_url = svn_path_is_url (&path_str);
  path_str.data = dst_path->data;
  path_str.len = dst_path->len;
  dst_is_url = svn_path_is_url (&path_str);

  /* Disallow moves between the working copy and the repository. */
  if (is_move)
    {
      if (SVN_IS_VALID_REVNUM (src_rev))
        return svn_error_create 
          (SVN_ERR_UNSUPPORTED_FEATURE, 0, NULL, pool,
           "move operations are only allowed on the HEAD revision");

      if (src_is_url == dst_is_url)
        {
          if (svn_path_is_child (src_path, dst_path, svn_path_url_style, pool))
            return svn_error_createf
              (SVN_ERR_UNSUPPORTED_FEATURE, 0, NULL, pool,
               "cannot move path '%s' into its own child '%s'",
               src_path->data, dst_path->data);
          if (svn_stringbuf_compare (src_path, dst_path))
            return svn_error_createf
              (SVN_ERR_UNSUPPORTED_FEATURE, 0, NULL, pool,
               "cannot move path '%s' into itself",
               src_path->data);
        }
      else
        {
          return svn_error_create 
            (SVN_ERR_UNSUPPORTED_FEATURE, 0, NULL, pool,
             "no support for repos <--> working copy moves");
        }
    }

  /* Make sure our log_msg is non-NULL. */
  if (! message)
    message = svn_stringbuf_create ("", pool);

  /* Now, call the right handler for the operation. */
  if ((! src_is_url) && (! dst_is_url))
    SVN_ERR (wc_to_wc_copy (src_path, dst_path, is_move, pool));

  else if ((! src_is_url) && (dst_is_url))
    SVN_ERR (wc_to_repos_copy (src_path, dst_path, 
                               auth_baton, message, 
                               before_editor, before_edit_baton,
                               after_editor, after_edit_baton,
                               pool));

  else if ((src_is_url) && (! dst_is_url))
    SVN_ERR (repos_to_wc_copy (src_path, src_rev, dst_path, auth_baton,
                               message,
                               before_editor, before_edit_baton,
                               after_editor, after_edit_baton,
                               pool));

  else
    SVN_ERR (repos_to_repos_copy (src_path, src_rev, dst_path, auth_baton,
                                  message, is_move, pool));

  return SVN_NO_ERROR;
}



/* Public Interfaces */

svn_error_t *
svn_client_copy (svn_stringbuf_t *src_path,
                 svn_revnum_t src_rev,
                 svn_stringbuf_t *dst_path,
                 svn_client_auth_baton_t *auth_baton,
                 svn_stringbuf_t *message,
                 const svn_delta_edit_fns_t *before_editor,
                 void *before_edit_baton,
                 const svn_delta_edit_fns_t *after_editor,
                 void *after_edit_baton,
                 apr_pool_t *pool)
{
  return setup_copy (src_path, src_rev, dst_path, auth_baton, message,
                     before_editor, before_edit_baton,
                     after_editor, after_edit_baton,
                     FALSE /* is_move */, pool);
}


svn_error_t *
svn_client_move (svn_stringbuf_t *src_path,
                 svn_revnum_t src_rev,
                 svn_stringbuf_t *dst_path,
                 svn_client_auth_baton_t *auth_baton,
                 svn_stringbuf_t *message,
                 apr_pool_t *pool)
{
  return setup_copy (src_path, src_rev, dst_path, auth_baton, message,
                     NULL, NULL,  /* no before_editor, before_edit_baton */
                     NULL, NULL,  /* no after_editor, after_edit_baton */
                     TRUE /* is_move */, pool);
}







/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end: */
