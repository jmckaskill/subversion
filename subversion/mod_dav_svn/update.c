/*
 * update.c: handle the update-report request and response
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



#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_xml.h>
#include <apr_md5.h>
#include <mod_dav.h>

#include "svn_pools.h"
#include "svn_repos.h"
#include "svn_fs.h"
#include "svn_md5.h"
#include "svn_base64.h"
#include "svn_xml.h"
#include "svn_path.h"
#include "svn_dav.h"

#include "dav_svn.h"
#include <http_request.h>
#include <http_log.h>


typedef struct {
  const dav_resource *resource;

  /* the revision we are updating to. used to generated IDs. */
  svn_fs_root_t *rev_root;

  const char *anchor;

  /* if doing a regular update, then dst_path == anchor.  if this is a
     'switch' operation, then this field is the fs path that is being
     switched to.  This path needs to telescope in the update-editor
     just like 'anchor' above; it's used for retrieving CR's and
     vsn-url's during the edit. */
  const char *dst_path;

  /* this buffers the output for a bit and is automatically flushed,
     at appropriate times, by the Apache filter system. */
  apr_bucket_brigade *bb;

  /* where to deliver the output */
  ap_filter_t *output;

  /* where do these editor paths *really* point to? */
  apr_hash_t *pathmap;

  /* are we doing a resource walk? */
  svn_boolean_t resource_walk;

  /* True iff we've already sent the open tag for the update. */
  svn_boolean_t started_update;

  /* True iff client requested all data inline in the report. */
  svn_boolean_t send_all;

} update_ctx_t;

typedef struct {
  apr_pool_t *pool;
  update_ctx_t *uc;
  const char *name;    /* the single-component name of this item */
  const char *path;    /* a telescoping extension of uc->anchor */
  const char *path2;   /* a telescoping extension of uc->dst_path */
  const char *path3;   /* a telescoping extension of uc->dst_path
                            without dst_path as prefix. */

  const char *base_checksum;   /* base_checksum (from apply_textdelta) */
  const char *text_checksum;   /* text_checksum (from close_file) */

  svn_boolean_t text_changed;        /* Did the file's contents change? */
  svn_boolean_t added;               /* File added? (Implies text_changed.) */
  apr_array_header_t *changed_props; /* array of const char * prop names */
  apr_array_header_t *removed_props; /* array of const char * prop names */

  /* "entry props" */
  const char *committed_rev;
  const char *committed_date;
  const char *last_author;

} item_baton_t;


#define DIR_OR_FILE(is_dir) ((is_dir) ? "directory" : "file")


struct authz_read_baton
{
  /* The original request, needed to generate a subrequest. */
  request_rec *r;

  /* We need this to construct a URI based on a repository abs path. */
  const dav_svn_repos *repos;
};


/* This implements 'svn_repos_authz_func_t'. */
static svn_error_t *authz_read(svn_boolean_t *allowed,
                               svn_fs_root_t *root,
                               const char *path,
                               void *baton,
                               apr_pool_t *pool)
{
  struct authz_read_baton *arb = baton;
  request_rec *subreq = NULL;
  const char *uri;
  svn_revnum_t rev = SVN_INVALID_REVNUM;
  const char *revpath = NULL;

  /* Our ultimate goal here is to create a Version Resource (VR) url,
     which is a url that represents a path within a revision.  We then
     send a subrequest to apache, so that any installed authz modules
     can allow/disallow the path.

     ### That means that we're assuming that any installed authz
     module is *only* paying attention to revision-paths, not paths in
     uncommitted transactions.  Someday we need to widen our horizons. */

  if (svn_fs_is_txn_root(root))
    {
      /* This means svn_repos_dir_delta is comparing two txn trees,
         rather than a txn and revision.  It's probably updating a
         working copy that contains 'disjoint urls'.  

         Because the 2nd transaction is likely to have all sorts of
         paths linked in from random places, we need to find the
         original (rev,path) of each txn path.  That's what needs
         authorization.  */

      svn_stringbuf_t *path_s = svn_stringbuf_create(path, pool);
      const char *lopped_path = "";
      
      /* The path might be copied implicitly, because it's down in a
         copied tree.  So we start at path and walk up its parents
         asking if anyone was copied, and if so where from.  */
      while (! (svn_path_is_empty(path_s->data)
                || ((path_s->len == 1) && (path_s->data[0] == '/'))))
        {
          SVN_ERR (svn_fs_copied_from(&rev, &revpath, root,
                                      path_s->data, pool));

          if (SVN_IS_VALID_REVNUM(rev) && revpath)
            {
              revpath = svn_path_join(revpath, lopped_path, pool);
              break;
            }
          
          /* Lop off the basename and try again. */
          lopped_path = svn_path_join(svn_path_basename
                                      (path_s->data, pool), lopped_path, pool);
          svn_path_remove_component(path_s);
        }

      /* If no copy produced this path, its path in the original
         revision is the same as its path in this txn. */
      if ((rev == SVN_INVALID_REVNUM) && (revpath == NULL))
        {
          const char *txn_name;
          svn_fs_txn_t *txn;

          txn_name = svn_fs_txn_root_name(root, pool);
          SVN_ERR( svn_fs_open_txn (&txn, svn_fs_root_fs(root),
                                    txn_name, pool) );
          rev = svn_fs_txn_base_revision(txn);
          revpath = path;
        }
    }
  else  /* revision root */
    {
      rev = svn_fs_revision_root_revision(root);
      revpath = path;
    }

  /* We have a (rev, path) pair to check authorization on. */

  /* Build a Version Resource uri representing (rev, path). */
  uri = dav_svn_build_uri(arb->repos, DAV_SVN_BUILD_URI_VERSION,
                          rev, revpath, FALSE, pool);
  
  /* Check if GET would work against this uri. */
  subreq = ap_sub_req_method_uri("GET", uri,
                                 arb->r, arb->r->output_filters);
  
  if (subreq && (subreq->status == HTTP_OK))
    *allowed = TRUE;
  else
    *allowed = FALSE;
  
  if (subreq)
    ap_destroy_sub_req(subreq);

  return SVN_NO_ERROR;
}


/* add PATH to the pathmap HASH with a repository path of LINKPATH.
   if LINKPATH is NULL, PATH will map to itself. */
static void add_to_path_map(apr_hash_t *hash,
                            const char *path,
                            const char *linkpath)
{
  /* normalize 'root paths' to have a slash */
  const char *norm_path = strcmp(path, "") ? path : "/";

  /* if there is an actual linkpath given, it is the repos path, else
     our path maps to itself. */
  const char *repos_path = linkpath ? linkpath : norm_path;

  /* now, geez, put the path in the map already! */
  apr_hash_set(hash, path, APR_HASH_KEY_STRING, (void *)repos_path);
}


/* return the actual repository path referred to by the editor's PATH,
   allocated in POOL, determined by examining the pathmap HASH. */
static const char *get_from_path_map(apr_hash_t *hash,
                                     const char *path,
                                     apr_pool_t *pool)
{
  const char *repos_path;
  svn_stringbuf_t *my_path;
  
  /* no hash means no map.  that's easy enough. */
  if (! hash)
    return apr_pstrdup(pool, path);
  
  if ((repos_path = apr_hash_get(hash, path, APR_HASH_KEY_STRING)))
    {
      /* what luck!  this path is a hash key!  if there is a linkpath,
         use that, else return the path itself. */
      return apr_pstrdup(pool, repos_path);
    }

  /* bummer.  PATH wasn't a key in path map, so we get to start
     hacking off components and looking for a parent from which to
     derive a repos_path.  use a stringbuf for convenience. */
  my_path = svn_stringbuf_create(path, pool);
  do 
    {
      svn_path_remove_component(my_path);
      if ((repos_path = apr_hash_get(hash, my_path->data, my_path->len)))
        {
          /* we found a mapping ... but of one of PATH's parents.
             soooo, we get to re-append the chunks of PATH that we
             broke off to the REPOS_PATH we found. */
          return apr_pstrcat(pool, repos_path, "/", 
                             path + my_path->len + 1, NULL);
        }
    }
  while (! svn_path_is_empty(my_path->data)
         && strcmp (my_path->data, "/") != 0);
  
  /* well, we simply never found anything worth mentioning the map.
     PATH is its own default finding, then. */
  return apr_pstrdup(pool, path);
}

static item_baton_t *make_child_baton(item_baton_t *parent, 
                                      const char *path,
                                      apr_pool_t *pool)
{
  item_baton_t *baton;

  baton = apr_pcalloc(pool, sizeof(*baton));
  baton->pool = pool;
  baton->uc = parent->uc;
  baton->name = svn_path_basename(path, pool);

  /* Telescope the path based on uc->anchor.  */
  baton->path = svn_path_join(parent->path, baton->name, pool);

  /* Telescope the path based on uc->dst_path in the exact same way. */
  baton->path2 = svn_path_join(parent->path2, baton->name, pool);

  /* Telescope the third path:  it's relative, not absolute, to dst_path. */
  baton->path3 = svn_path_join(parent->path3, baton->name, pool);

  return baton;
}


struct brigade_write_baton
{
  apr_bucket_brigade *bb;
  ap_filter_t *output;
};


/* This implements 'svn_write_fn_t'. */
static svn_error_t * brigade_write_fn(void *baton,
                                      const char *data,
                                      apr_size_t *len)
{
  struct brigade_write_baton *wb = baton;
  apr_status_t apr_err;

  apr_err = apr_brigade_write(wb->bb, ap_filter_flush, wb->output, data, *len);

  if (apr_err != APR_SUCCESS)
    return svn_error_wrap_apr(apr_err, "Error writing base64 data");

  return SVN_NO_ERROR;
}


static svn_stream_t * make_base64_output_stream(apr_bucket_brigade *bb,
                                                ap_filter_t *output,
                                                apr_pool_t *pool)
{
  struct brigade_write_baton *wb = apr_palloc(pool, sizeof(*wb));
  svn_stream_t *stream = svn_stream_create(wb, pool);

  wb->bb = bb;
  wb->output = output;
  svn_stream_set_write(stream, brigade_write_fn);

  return svn_base64_encode(stream, pool);
}


static svn_error_t * send_xml(update_ctx_t *uc, const char *fmt, ...)
{
  apr_status_t apr_err;
  va_list ap;

  va_start(ap, fmt);
  apr_err = apr_brigade_vprintf(uc->bb, ap_filter_flush, 
                                uc->output, fmt, ap);
  va_end(ap);
  if (apr_err)
    return svn_error_create(apr_err, 0, NULL);
  /* ### check for an aborted connection, since the brigade functions
     don't appear to be return useful errors when the connection is
     dropped. */
  if (uc->output->c->aborted)
    return svn_error_create(SVN_ERR_APMOD_CONNECTION_ABORTED, 0, NULL);
  return SVN_NO_ERROR;
}


/* Get the real filesystem PATH for BATON, and return the value
   allocated from POOL.  This function juggles the craziness of
   updates, switches, and updates of switched things. */
static const char *
get_real_fs_path(item_baton_t *baton, apr_pool_t *pool)
{
  const char *path = get_from_path_map(baton->uc->pathmap, baton->path, pool);
  return strcmp(path, baton->path) ? path : baton->path2;
}


static svn_error_t * send_vsn_url(item_baton_t *baton, apr_pool_t *pool)
{
  const char *href;
  const char *path;
  svn_revnum_t revision;

  /* Try to use the CR, assuming the path exists in CR. */
  path = get_real_fs_path(baton, pool);
  revision = dav_svn_get_safe_cr(baton->uc->rev_root, path, pool);
    
  href = dav_svn_build_uri(baton->uc->resource->info->repos,
                           DAV_SVN_BUILD_URI_VERSION,
                           revision, path, 0 /* add_href */, pool);
  
  return send_xml(baton->uc, "<D:checked-in><D:href>%s</D:href></D:checked-in>"
                  DEBUG_CR, apr_xml_quote_string (pool, href, 1));
}

static svn_error_t * absent_helper(svn_boolean_t is_dir,
                                   const char *path,
                                   item_baton_t *parent,
                                   apr_pool_t *pool)
{
  update_ctx_t *uc = parent->uc;

  if (! uc->resource_walk)
    {
      const char *elt = apr_psprintf(pool,
                                     "<S:absent-%s name=\"%s\"/>" DEBUG_CR,
                                     DIR_OR_FILE(is_dir),
                                     svn_path_basename(path, pool));
      SVN_ERR( send_xml(uc, "%s", elt) );
    }

  return SVN_NO_ERROR;
}


static svn_error_t * upd_absent_directory(const char *path,
                                          void *parent_baton,
                                          apr_pool_t *pool)
{
  return absent_helper(TRUE, path, parent_baton, pool);
}


static svn_error_t * upd_absent_file(const char *path,
                                     void *parent_baton,
                                     apr_pool_t *pool)
{
  return absent_helper(FALSE, path, parent_baton, pool);
}


static svn_error_t * add_helper(svn_boolean_t is_dir,
                                const char *path,
                                item_baton_t *parent,
                                const char *copyfrom_path,
                                svn_revnum_t copyfrom_revision,
                                apr_pool_t *pool,
                                void **child_baton)
{
  item_baton_t *child;
  update_ctx_t *uc = parent->uc;
  const char *bc_url = NULL;

  child = make_child_baton(parent, path, pool);
  child->added = TRUE;

  if (uc->resource_walk)
    {
      SVN_ERR( send_xml(child->uc, "<S:resource path=\"%s\">" DEBUG_CR, 
                        apr_xml_quote_string(pool, child->path3, 1)) );
    }
  else
    {
      const char *qname = apr_xml_quote_string(pool, child->name, 1);
      const char *elt;
      const char *real_path = get_real_fs_path(child, pool);

      if (! is_dir)
        {
          /* files have checksums */
          unsigned char digest[APR_MD5_DIGESTSIZE];
          SVN_ERR (svn_fs_file_md5_checksum
                   (digest, uc->rev_root, real_path, pool));
          
          child->text_checksum = svn_md5_digest_to_cstring(digest, pool);
        }
      else
        {
          /* we send baseline-collection urls when we add a directory */
          svn_revnum_t revision;
          revision = dav_svn_get_safe_cr(child->uc->rev_root, real_path, pool);
          bc_url = dav_svn_build_uri(child->uc->resource->info->repos,
                                     DAV_SVN_BUILD_URI_BC,
                                     revision, real_path,
                                     0 /* add_href */, pool);

          /* ugh, build_uri ignores the path and just builds the root
             of the baseline collection.  we have to tack the
             real_path on manually, ignoring its leading slash. */
          if (real_path && (! svn_path_is_empty(real_path)))
            bc_url = svn_path_url_add_component(bc_url, real_path+1, pool);

          /* make sure that the BC_URL is xml attribute safe. */
          bc_url = apr_xml_quote_string(pool, bc_url, 1);
        }


      if (copyfrom_path == NULL)
        {
          if (bc_url)            
            elt = apr_psprintf(pool, "<S:add-%s name=\"%s\" "
                               "bc-url=\"%s\">" DEBUG_CR,
                               DIR_OR_FILE(is_dir), qname, bc_url);
          else
            elt = apr_psprintf(pool, "<S:add-%s name=\"%s\">" DEBUG_CR,
                               DIR_OR_FILE(is_dir), qname);
        }
      else
        {
          const char *qcopy = apr_xml_quote_string(pool, copyfrom_path, 1);

          if (bc_url)
            elt = apr_psprintf(pool, "<S:add-%s name=\"%s\" "
                               "copyfrom-path=\"%s\" copyfrom-rev=\"%"
                               SVN_REVNUM_T_FMT "\" "
                               "bc-url=\"%s\">" DEBUG_CR,
                               DIR_OR_FILE(is_dir),
                               qname, qcopy, copyfrom_revision,
                               bc_url);
          else
            elt = apr_psprintf(pool, "<S:add-%s name=\"%s\" "
                               "copyfrom-path=\"%s\" copyfrom-rev=\"%"
                               SVN_REVNUM_T_FMT "\">" DEBUG_CR,
                               DIR_OR_FILE(is_dir),
                               qname, qcopy, copyfrom_revision);
        }

      /* Resist the temptation to pass 'elt' as the format string.
         Because it contains URIs, it might have sequences that look
         like format string insert placeholders.  For example,
         "this%20dir" is a valid printf() format string that means
         "this[insert an integer of width 20 here]ir". */
      SVN_ERR( send_xml(child->uc, "%s", elt) );
    }

  SVN_ERR( send_vsn_url(child, pool) );

  if (uc->resource_walk)
    SVN_ERR( send_xml(child->uc, "</S:resource>" DEBUG_CR) );

  *child_baton = child;

  return SVN_NO_ERROR;
}

static svn_error_t * open_helper(svn_boolean_t is_dir,
                                 const char *path,
                                 item_baton_t *parent,
                                 svn_revnum_t base_revision,
                                 apr_pool_t *pool,
                                 void **child_baton)
{
  item_baton_t *child = make_child_baton(parent, path, pool);
  const char *qname = apr_xml_quote_string(pool, child->name, 1);

  SVN_ERR( send_xml(child->uc, "<S:open-%s name=\"%s\" rev=\"%"
                    SVN_REVNUM_T_FMT "\">" DEBUG_CR,
                    DIR_OR_FILE(is_dir), qname, base_revision));
  SVN_ERR( send_vsn_url(child, pool) );
  *child_baton = child;
  return SVN_NO_ERROR;
}

static svn_error_t * close_helper(svn_boolean_t is_dir, item_baton_t *baton)
{
  int i;
  
  if (baton->uc->resource_walk)
    return SVN_NO_ERROR;

  /* ### ack!  binary names won't float here! */
  if (baton->removed_props && (! baton->added))
    {
      const char *qname;

      for (i = 0; i < baton->removed_props->nelts; i++)
        {
          /* We already XML-escaped the property name in change_xxx_prop. */
          qname = ((const char **)(baton->removed_props->elts))[i];
          SVN_ERR( send_xml(baton->uc, "<S:remove-prop name=\"%s\"/>" 
                            DEBUG_CR, qname) );
        }
    }

  if ((! baton->uc->send_all) && baton->changed_props && (! baton->added))
    {
      /* Tell the client to fetch all the props */
      SVN_ERR( send_xml(baton->uc, "<S:fetch-props/>" DEBUG_CR) );
    }

  SVN_ERR( send_xml(baton->uc, "<S:prop>") );

  /* Both modern and non-modern clients need the checksum... */
  if (baton->text_checksum)
    {
      SVN_ERR( send_xml(baton->uc, "<V:md5-checksum>%s</V:md5-checksum>", 
                        baton->text_checksum) );
    }

  /* ...but only non-modern clients want the 3 CR-related properties
     sent like here, because they can't handle receiving these special
     props inline like any other prop.
     ### later on, compress via the 'scattered table' solution as
     discussed with gstein.  -bmcs */
  if (! baton->uc->send_all)
    {
      /* ### grrr, these DAV: property names are already #defined in
         ra_dav.h, and statically defined in liveprops.c.  And now
         they're hardcoded here.  Isn't there some header file that both
         sides of the network can share?? */
      
      /* ### special knowledge: svn_repos_dir_delta will never send
       *removals* of the commit-info "entry props". */
      if (baton->committed_rev)
        SVN_ERR( send_xml(baton->uc, "<D:version-name>%s</D:version-name>",
                          baton->committed_rev) );
      
      if (baton->committed_date)
        SVN_ERR( send_xml(baton->uc, "<D:creationdate>%s</D:creationdate>",
                          baton->committed_date) );
      
      if (baton->last_author)
        SVN_ERR( send_xml(baton->uc,
                          "<D:creator-displayname>%s</D:creator-displayname>",
                          baton->last_author) );

    }

  /* Close unconditionally, because we sent checksum unconditionally. */
  SVN_ERR( send_xml(baton->uc, "</S:prop>\n") );
    
  if (baton->added)
    SVN_ERR( send_xml(baton->uc, "</S:add-%s>" DEBUG_CR, 
                      DIR_OR_FILE(is_dir)) );
  else
    SVN_ERR( send_xml(baton->uc, "</S:open-%s>" DEBUG_CR, 
                      DIR_OR_FILE(is_dir)) );
  return SVN_NO_ERROR;
}


/* Send the opening tag of the update-report if it hasn't been sent
   already.

   Note: because send_xml does not return an error, this function
   never returns error either.  However, its prototype anticipates a
   day when send_xml() can return error. */
static svn_error_t * maybe_start_update_report(update_ctx_t *uc)
{
  if ((! uc->resource_walk) && (! uc->started_update))
    {
      SVN_ERR( send_xml(uc,
                        DAV_XML_HEADER DEBUG_CR
                        "<S:update-report xmlns:S=\"" SVN_XML_NAMESPACE "\" "
                        "xmlns:V=\"" SVN_DAV_PROP_NS_DAV "\" "
                        "xmlns:D=\"DAV:\" %s>" DEBUG_CR,
                        uc->send_all ? "send-all=\"true\"" : "") );
      
      uc->started_update = TRUE;
    }

  return SVN_NO_ERROR;
}


static svn_error_t * upd_set_target_revision(void *edit_baton,
                                             svn_revnum_t target_revision,
                                             apr_pool_t *pool)
{
  update_ctx_t *uc = edit_baton;

  SVN_ERR( maybe_start_update_report(uc) );

  if (! uc->resource_walk)
    SVN_ERR( send_xml(uc, "<S:target-revision rev=\"%" SVN_REVNUM_T_FMT 
                      "\"/>" DEBUG_CR, target_revision) );

  return SVN_NO_ERROR;
}

static svn_error_t * upd_open_root(void *edit_baton,
                                   svn_revnum_t base_revision,
                                   apr_pool_t *pool,
                                   void **root_baton)
{
  update_ctx_t *uc = edit_baton;
  item_baton_t *b = apr_pcalloc(pool, sizeof(*b));

  /* note that we create a subpool; the root_baton is passed to the
     close_directory callback, where we will destroy the pool. */

  b->uc = uc;
  b->pool = pool;
  b->path = uc->anchor;
  b->path2 = uc->dst_path;
  b->path3 = "";

  *root_baton = b;

  SVN_ERR( maybe_start_update_report(uc) );

  if (uc->resource_walk)
    {
      const char *qpath = apr_xml_quote_string(pool, b->path3, 1);
      SVN_ERR( send_xml(uc, "<S:resource path=\"%s\">" DEBUG_CR, qpath) );
    }
  else    
    {
      SVN_ERR( send_xml(uc, "<S:open-directory rev=\"%" SVN_REVNUM_T_FMT "\">"
                        DEBUG_CR, base_revision) );
    }

  SVN_ERR( send_vsn_url(b, pool) );

  if (uc->resource_walk)
    SVN_ERR( send_xml(uc, "</S:resource>" DEBUG_CR) );

  return SVN_NO_ERROR;
}

static svn_error_t * upd_delete_entry(const char *path,
                                      svn_revnum_t revision,
                                      void *parent_baton,
                                      apr_pool_t *pool)
{
  item_baton_t *parent = parent_baton;
  const char *qname = apr_xml_quote_string(pool, 
                                           svn_path_basename(path, pool), 1);
  return send_xml(parent->uc, "<S:delete-entry name=\"%s\"/>" DEBUG_CR, qname);
}

static svn_error_t * upd_add_directory(const char *path,
                                       void *parent_baton,
                                       const char *copyfrom_path,
                                       svn_revnum_t copyfrom_revision,
                                       apr_pool_t *pool,
                                       void **child_baton)
{
  return add_helper(TRUE /* is_dir */,
                    path, parent_baton, copyfrom_path, copyfrom_revision, pool,
                    child_baton);
}

static svn_error_t * upd_open_directory(const char *path,
                                        void *parent_baton,
                                        svn_revnum_t base_revision,
                                        apr_pool_t *pool,
                                        void **child_baton)
{
  open_helper(TRUE /* is_dir */,
              path, parent_baton, base_revision, pool, child_baton);
  return SVN_NO_ERROR;
}

static svn_error_t * upd_change_xxx_prop(void *baton,
                                         const char *name,
                                         const svn_string_t *value,
                                         apr_pool_t *pool)
{
  item_baton_t *b = baton;
  const char *qname;

  /* Resource walks say nothing about props. */
  if (b->uc->resource_walk)
    return SVN_NO_ERROR;

  /* Else this not a resource walk, so either send props or cache them
     to send later, depending on whether this is a modern report
     response or not. */

  qname = apr_xml_quote_string (b->pool, name, 1);

  /* apr_xml_quote_string doesn't realloc if there is nothing to
     quote, so dup the name, but only if necessary. */
  if (qname == name)
    qname = apr_pstrdup (b->pool, name);


  if (b->uc->send_all)
    {
      if (value)
        {
          const svn_string_t *qval;
          
          if (svn_xml_is_xml_safe(value->data, value->len))
            {
              svn_stringbuf_t *tmp = NULL;
              svn_xml_escape_cdata_string(&tmp, value, pool);
              qval = svn_string_create (tmp->data, pool);
              SVN_ERR( send_xml(b->uc, "<S:set-prop name=\"%s\">", qname) );
            }
          else
            {
              qval = svn_base64_encode_string(value, pool);
              SVN_ERR( send_xml(b->uc, 
                                "<S:set-prop name=\"%s\" encoding=\"base64\">" 
                                DEBUG_CR, qname) );
            }
          
          SVN_ERR( send_xml(b->uc, "%s", qval->data) );
          SVN_ERR( send_xml(b->uc, "</S:set-prop>" DEBUG_CR) );
        }
      else  /* value is null, so this is a prop removal */
        {
          SVN_ERR( send_xml(b->uc, "<S:remove-prop name=\"%s\"/>" DEBUG_CR, 
                            qname) );
        }
    }
  else  /* don't do inline response, just cache prop names for close_helper */
    {
      /* For now, store certain entry props, because we'll need to send
         them later as standard DAV ("D:") props.  ### this should go
         away and we should just tunnel those props on through for the
         client to deal with. */
#define NSLEN (sizeof(SVN_PROP_ENTRY_PREFIX) - 1)
      if (! strncmp(name, SVN_PROP_ENTRY_PREFIX, NSLEN))
        {
          if (! strcmp(name, SVN_PROP_ENTRY_COMMITTED_REV))
            {
              b->committed_rev = value ?
                apr_pstrdup(b->pool, value->data) : NULL;
            }
          else if (! strcmp(name, SVN_PROP_ENTRY_COMMITTED_DATE))
            {
              b->committed_date = value ?
                apr_pstrdup(b->pool, value->data) : NULL;
            }
          else if (! strcmp(name, SVN_PROP_ENTRY_LAST_AUTHOR))
            {
              b->last_author = value ?
                apr_pstrdup(b->pool, value->data) : NULL;
            }
      
          return SVN_NO_ERROR;
        }
#undef NSLEN

      if (value)
        {
          if (! b->changed_props)
            b->changed_props = apr_array_make (b->pool, 1, sizeof (name));
          
          (*((const char **)(apr_array_push (b->changed_props)))) = qname;
        }
      else
        {
          if (! b->removed_props)
            b->removed_props = apr_array_make (b->pool, 1, sizeof (name));
          
          (*((const char **)(apr_array_push (b->removed_props)))) = qname;
        }
    }

  return SVN_NO_ERROR;
}

static svn_error_t * upd_close_directory(void *dir_baton,
                                         apr_pool_t *pool)
{
  return close_helper(TRUE /* is_dir */, dir_baton);
}

static svn_error_t * upd_add_file(const char *path,
                                  void *parent_baton,
                                  const char *copyfrom_path,
                                  svn_revnum_t copyfrom_revision,
                                  apr_pool_t *pool,
                                  void **file_baton)
{
  return add_helper(FALSE /* is_dir */,
                    path, parent_baton, copyfrom_path, copyfrom_revision, pool,
                    file_baton);
}

static svn_error_t * upd_open_file(const char *path,
                                   void *parent_baton,
                                   svn_revnum_t base_revision,
                                   apr_pool_t *pool,
                                   void **file_baton)
{
  return open_helper(FALSE /* is_dir */,
                     path, parent_baton, base_revision, pool, file_baton);
}


/* We have our own window handler and baton as a simple wrapper around
   the real handler (which converts vdelta windows to base64-encoded
   svndiff data).  The wrapper is responsible for sending the opening
   and closing XML tags around the svndiff data. */
struct window_handler_baton
{
  svn_boolean_t seen_first_window;  /* False until first window seen. */
  update_ctx_t *uc;

  /* The _real_ window handler and baton. */
  svn_txdelta_window_handler_t handler;
  void *handler_baton;
};


/* This implements 'svn_txdelta_window_handler_t'. */
static svn_error_t * window_handler(svn_txdelta_window_t *window, void *baton)
{
  struct window_handler_baton *wb = baton;

  if (! wb->seen_first_window)
    {
      wb->seen_first_window = TRUE;
      SVN_ERR( send_xml(wb->uc, "<S:txdelta>") );
    }

  SVN_ERR( wb->handler(window, wb->handler_baton) );

  if (window == NULL)
    SVN_ERR( send_xml(wb->uc, "</S:txdelta>") );

  return SVN_NO_ERROR;
}


/* This implements 'svn_txdelta_window_handler_t'.
   During a resource walk, the driver sends an empty window as a
   boolean indicating that a change happened to this file, but we
   don't want to send anything over the wire as a result. */
static svn_error_t * dummy_window_handler(svn_txdelta_window_t *window,
                                          void *baton)
{
  return SVN_NO_ERROR;
}


static svn_error_t * upd_apply_textdelta(void *file_baton, 
                                         const char *base_checksum,
                                         apr_pool_t *pool,
                                         svn_txdelta_window_handler_t *handler,
                                         void **handler_baton)
{
  item_baton_t *file = file_baton;
  struct window_handler_baton *wb = apr_palloc(file->pool, sizeof(*wb));
  svn_stream_t *base64_stream;

  if (file->uc->resource_walk)
    {
      *handler = dummy_window_handler;
      *handler_baton = NULL;
      return SVN_NO_ERROR;
    }

  file->base_checksum = apr_pstrdup(file->pool, base_checksum);
  file->text_changed = TRUE;

  wb->seen_first_window = FALSE;
  wb->uc = file->uc;
  base64_stream = make_base64_output_stream(wb->uc->bb, wb->uc->output,
                                            file->pool);

  svn_txdelta_to_svndiff(base64_stream, file->pool,
                         &(wb->handler), &(wb->handler_baton));

  *handler = window_handler;
  *handler_baton = wb;

  return SVN_NO_ERROR;
}


static svn_error_t * upd_close_file(void *file_baton,
                                    const char *text_checksum,
                                    apr_pool_t *pool)
{
  item_baton_t *file = file_baton;

  file->text_checksum = apr_pstrdup(file->pool, text_checksum);

  /* If we are not in "send all" mode, and this file is not a new
     addition or didn't otherwise have changed text, tell the client
     to fetch it. */
  if ((! file->uc->send_all) && (! file->added) && file->text_changed)
    {
      const char *elt;
      elt = apr_psprintf(pool, "<S:fetch-file%s%s%s/>" DEBUG_CR,
                         file->base_checksum ? " base-checksum=\"" : "",
                         file->base_checksum ? file->base_checksum : "",
                         file->base_checksum ? "\"" : "");
      SVN_ERR( send_xml(file->uc, elt) );
    }

  return close_helper(FALSE /* is_dir */, file);
}


static svn_error_t * upd_close_edit(void *edit_baton,
                                    apr_pool_t *pool)
{
  update_ctx_t *uc = edit_baton;

  /* Our driver will unconditionally close the update report... So if
     the report hasn't even been started yet, start it now. */
  return maybe_start_update_report(uc);
}


dav_error * dav_svn__update_report(const dav_resource *resource,
                                   const apr_xml_doc *doc,
                                   ap_filter_t *output)
{
  svn_delta_editor_t *editor;
  apr_xml_elem *child;
  void *rbaton = NULL;
  update_ctx_t uc = { 0 };
  svn_revnum_t revnum = SVN_INVALID_REVNUM;
  int ns;
  svn_error_t *serr;
  dav_error *derr = NULL;
  apr_status_t apr_err;
  const char *src_path = NULL;
  const char *dst_path = NULL;
  const dav_svn_repos *repos = resource->info->repos;
  const char *target = "";
  svn_boolean_t recurse = TRUE;
  svn_boolean_t resource_walk = FALSE;
  svn_boolean_t ignore_ancestry = FALSE;
  struct authz_read_baton arb;
  apr_pool_t *subpool = svn_pool_create(resource->pool);

  /* Construct the authz read check baton. */
  arb.r = resource->info->r;
  arb.repos = repos;

  if (resource->info->restype != DAV_SVN_RESTYPE_VCC)
    {
      return dav_new_error(resource->pool, HTTP_CONFLICT, 0,
                           "This report can only be run against a VCC.");
    }

  ns = dav_svn_find_ns(doc->namespaces, SVN_XML_NAMESPACE);
  if (ns == -1)
    {
      return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
                           "The request does not contain the 'svn:' "
                           "namespace, so it is not going to have an "
                           "svn:target-revision element. That element "
                           "is required.");
    }
  
  /* Look to see if client wants a report with props and textdeltas
     inline, rather than placeholder tags that tell the client to do
     further fetches.  Modern clients prefer inline. */
  {
    apr_xml_attr *this_attr;

    for (this_attr = doc->root->attr; this_attr; this_attr = this_attr->next)
      {
        if ((strcmp(this_attr->name, "send-all") == 0)
            && (strcmp(this_attr->value, "true") == 0))
          {
            uc.send_all = TRUE;
            break;
          }
      }
  }

  for (child = doc->root->first_child; child != NULL; child = child->next)
    {
      /* Note that child->name might not match any of the cases below.
         Thus, the check for non-empty cdata in each of these cases
         cannot be moved to the top of the loop, because then it would
         wrongly catch other elements that do allow empty cdata. */ 

      if (child->ns == ns && strcmp(child->name, "target-revision") == 0)
        {
          if (! child->first_cdata.first)
            return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
              "The request's 'target-revision' element contains empty cdata; "
              "there is a problem with the client.");

          /* ### assume no white space, no child elems, etc */
          revnum = SVN_STR_TO_REV(child->first_cdata.first->text);
        }

      if (child->ns == ns && strcmp(child->name, "src-path") == 0)
        {
          /* ### assume no white space, no child elems, etc */
          dav_svn_uri_info this_info;

          if (! child->first_cdata.first)
            return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
              "The request's 'src-path' element contains empty cdata; "
              "there is a problem with the client.");

          /* split up the 1st public URL. */
          serr = dav_svn_simple_parse_uri(&this_info, resource,
                                          child->first_cdata.first->text,
                                          resource->pool);
          if (serr != NULL)
            {
              return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                         "Could not parse src-path URL.");
            }
          src_path = this_info.repos_path;
        }

      if (child->ns == ns && strcmp(child->name, "dst-path") == 0)
        {
          /* ### assume no white space, no child elems, etc */
          dav_svn_uri_info this_info;

          if (! child->first_cdata.first)
            return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
              "The request's 'dst-path' element contains empty cdata; "
              "there is a problem with the client.  See "
              "http://subversion.tigris.org/issues/show_bug.cgi?id=1055");

          /* split up the 2nd public URL. */
          serr = dav_svn_simple_parse_uri(&this_info, resource,
                                          child->first_cdata.first->text,
                                          resource->pool);
          if (serr != NULL)
            {
              return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                         "Could not parse dst-path URL.");
            }
          dst_path = this_info.repos_path;
        }

      if (child->ns == ns && strcmp(child->name, "update-target") == 0)
        {
          if (! child->first_cdata.first)
            return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
              "The request's 'update-target' element contains empty cdata; "
              "there is a problem with the client.");

          /* ### assume no white space, no child elems, etc */
          target = child->first_cdata.first->text;
        }
      if (child->ns == ns && strcmp(child->name, "recursive") == 0)
        {
          if (! child->first_cdata.first)
            return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
              "The request's 'recursive' element contains empty cdata; "
              "there is a problem with the client.");

          /* ### assume no white space, no child elems, etc */
          if (strcmp(child->first_cdata.first->text, "no") == 0)
            recurse = FALSE;
        }
      if (child->ns == ns && strcmp(child->name, "ignore-ancestry") == 0)
        {
          if (! child->first_cdata.first)
            return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
              "The request's 'ignore-ancestry' element contains empty cdata; "
              "there is a problem with the client.");

          /* ### assume no white space, no child elems, etc */
          ignore_ancestry = TRUE;
          if (strcmp(child->first_cdata.first->text, "no") == 0)
            ignore_ancestry = FALSE;
        }
      if (child->ns == ns && strcmp(child->name, "resource-walk") == 0)
        {
          if (! child->first_cdata.first)
            return dav_new_error(resource->pool, HTTP_BAD_REQUEST, 0,
              "The request's 'resource-walk' element contains empty cdata; "
              "there is a problem with the client.");

          /* ### assume no white space, no child elems, etc */
          if (strcmp(child->first_cdata.first->text, "no") != 0)
            resource_walk = TRUE;
        }
    }

  if (revnum == SVN_INVALID_REVNUM)
    {
      serr = svn_fs_youngest_rev(&revnum, repos->fs, resource->pool);
      if (serr != NULL)
        {
          return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                     "Could not determine the youngest "
                                     "revision for the update process.");
        }
    }

  editor = svn_delta_default_editor(resource->pool);
  editor->set_target_revision = upd_set_target_revision;
  editor->open_root = upd_open_root;
  editor->delete_entry = upd_delete_entry;
  editor->add_directory = upd_add_directory;
  editor->open_directory = upd_open_directory;
  editor->change_dir_prop = upd_change_xxx_prop;
  editor->close_directory = upd_close_directory;
  editor->absent_directory = upd_absent_directory;
  editor->add_file = upd_add_file;
  editor->open_file = upd_open_file;
  editor->apply_textdelta = upd_apply_textdelta;
  editor->change_file_prop = upd_change_xxx_prop;
  editor->close_file = upd_close_file;
  editor->absent_file = upd_absent_file;
  editor->close_edit = upd_close_edit;

  /* If the client never sent a <src-path> element, it's old and
     sending a style of report that we no longer allow. */
  if (! src_path)
    {
      return dav_new_error
        (resource->pool, HTTP_BAD_REQUEST, 0,
         "The request did not contain the '<src-path>' element.\n"
         "This may indicate that your client is too old.");
    }

  uc.resource = resource;
  uc.output = output;  
  uc.anchor = src_path;
  uc.bb = apr_brigade_create(resource->pool, output->c->bucket_alloc);
  uc.pathmap = NULL;
  if (dst_path) /* we're doing a 'switch' */
    {      
      if (*target)
        {
          /* if the src is split into anchor/target, so must the
             telescoping dst_path be. */
          uc.dst_path = svn_path_dirname(dst_path, resource->pool);

          /* Also, the svn_repos_dir_delta() is going to preserve our
             target's name, so we need a pathmap entry for that. */
          if (! uc.pathmap)
            uc.pathmap = apr_hash_make(resource->pool);
          add_to_path_map(uc.pathmap, 
                          svn_path_join(src_path, target, resource->pool),
                          dst_path);
        }
      else
        {
          uc.dst_path = dst_path;
        }
    }
  else  /* we're doing an update, so src and dst are the same. */
    uc.dst_path = uc.anchor;

  /* Get the root of the revision we want to update to. This will be used
     to generated stable id values. */
  if ((serr = svn_fs_revision_root(&uc.rev_root, repos->fs, 
                                   revnum, resource->pool)))
    {
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "The revision root could not be created.");
    }

  /* When we call svn_repos_finish_report, it will ultimately run
     dir_delta() between REPOS_PATH/TARGET and TARGET_PATH.  In the
     case of an update or status, these paths should be identical.  In
     the case of a switch, they should be different. */
  if ((serr = svn_repos_begin_report(&rbaton, revnum, repos->username, 
                                     repos->repos, 
                                     src_path, target,
                                     dst_path,
                                     uc.send_all,
                                     recurse,
                                     ignore_ancestry,
                                     editor, &uc,
                                     authz_read,
                                     &arb,
                                     resource->pool)))
    {
      return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "The state report gatherer could not be "
                                 "created.");
    }

  /* scan the XML doc for state information */
  for (child = doc->root->first_child; child != NULL; child = child->next)
    if (child->ns == ns)
      {
        /* Clear our subpool. */
        svn_pool_clear(subpool);

        if (strcmp(child->name, "entry") == 0)
          {
            const char *path;
            svn_revnum_t rev = SVN_INVALID_REVNUM;
            const char *linkpath = NULL;
            svn_boolean_t start_empty = FALSE;
            apr_xml_attr *this_attr = child->attr;

            while (this_attr)
              {
                if (! strcmp(this_attr->name, "rev"))
                  rev = SVN_STR_TO_REV(this_attr->value);
                else if (! strcmp(this_attr->name, "linkpath"))
                  linkpath = this_attr->value;
                else if (! strcmp(this_attr->name, "start-empty"))
                  start_empty = TRUE;

                this_attr = this_attr->next;
              }
            
            /* we require the `rev' attribute for this to make sense */
            if (! SVN_IS_VALID_REVNUM (rev))
              {
                serr = svn_error_create (SVN_ERR_XML_ATTRIB_NOT_FOUND, 
                                         NULL, "Missing XML attribute: rev");
                derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                           "A failure occurred while "
                                           "recording one of the items of "
                                           "working copy state.");
                goto cleanup;
              }

            /* get cdata, stipping whitespace */
            path = dav_xml_get_cdata(child, subpool, 1);
            
            if (! linkpath)
              serr = svn_repos_set_path(rbaton, path, rev,
                                        start_empty, subpool);
            else
              serr = svn_repos_link_path(rbaton, path, linkpath, rev,
                                         start_empty, subpool);
            if (serr != NULL)
              {
                derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                           "A failure occurred while "
                                           "recording one of the items of "
                                           "working copy state.");
                goto cleanup;
              }

            /* now, add this path to our path map, but only if we are
               doing a regular update (not a `switch') */
            if (linkpath && (! dst_path))
              {
                const char *this_path;
                if (! uc.pathmap)
                  uc.pathmap = apr_hash_make(resource->pool);
                this_path = svn_path_join_many(apr_hash_pool_get(uc.pathmap),
                                               src_path, target, path, NULL);
                add_to_path_map(uc.pathmap, this_path, linkpath);
              }
          }
        else if (strcmp(child->name, "missing") == 0)
          {
            /* get cdata, stipping whitespace */
            const char *path = dav_xml_get_cdata(child, subpool, 1);
            serr = svn_repos_delete_path(rbaton, path, subpool);
            if (serr != NULL)
              {
                derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                           "A failure occurred while "
                                           "recording one of the (missing) "
                                           "items of working copy state.");
                goto cleanup;
              }
          }
      }

  /* this will complete the report, and then drive our editor to generate
     the response to the client. */
  serr = svn_repos_finish_report(rbaton, resource->pool);
  if (serr)
    {
      derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                 "A failure occurred while "
                                 "driving the update report editor");
      goto cleanup;
    }

  /* We're finished with the report baton.  Note that so we don't try
     to abort this report later. */
  rbaton = NULL;

  /* The potential "resource walk" part of the update-report. */
  if (dst_path && resource_walk)  /* this was a 'switch' operation */
    {
      /* Sanity check: if we switched a file, we can't do a resource
         walk.  dir_delta would choke if we pass a filepath as the
         'target'.  Also, there's no need to do the walk, since the
         new vsn-rsc-url was already in the earlier part of the report. */
      svn_node_kind_t dst_kind;

      serr = svn_fs_check_path(&dst_kind, uc.rev_root, dst_path,
                               resource->pool);
      if (serr)
        {
          derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                     "Failed to find the kind of a path");
          goto cleanup;
        }

      if (dst_kind == svn_node_dir)
        {
          /* send a second embedded <S:resource-walk> tree that contains
             the new vsn-rsc-urls for the switched dir.  this walk
             contains essentially nothing but <add> tags. */
          svn_fs_root_t *zero_root;
          serr = svn_fs_revision_root(&zero_root, repos->fs, 0,
                                      resource->pool);
          if (serr)
            {
              derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                         "Failed to find the revision root");
              goto cleanup;
            }

          serr = send_xml(&uc, "<S:resource-walk>" DEBUG_CR);
          if (serr)
            {
              derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                         "Unable to begin resource walk");
              goto cleanup;
            }

          uc.resource_walk = TRUE;

          /* Compare subtree DST_PATH within a pristine revision to
             revision 0.  This should result in nothing but 'add' calls
             to the editor. */
          serr = svn_repos_dir_delta(/* source is revision 0: */
                                     zero_root, "", "",
                                     /* target is 'switch' location: */
                                     uc.rev_root, dst_path,
                                     /* re-use the editor */
                                     editor, &uc,
                                     authz_read,
                                     &arb,
                                     FALSE, /* no text deltas */
                                     recurse,
                                     TRUE, /* send entryprops */
                                     FALSE, /* don't ignore ancestry */
                                     resource->pool);
          if (serr)
            {
              derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                         "Resource walk failed.");
              goto cleanup;
            }
          
          serr = send_xml(&uc, "</S:resource-walk>" DEBUG_CR);
          if (serr)
            {
              derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                         "Unable to complete resource walk.");
              goto cleanup;
            }
        }
    }

  /* Close the report body, unless some error prevented it from being
     started in the first place. */
  if (uc.started_update)
    {
      serr = send_xml(&uc, "</S:update-report>" DEBUG_CR);
      if (serr)
        {
          derr = dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                     "Unable to complete update report.");
          goto cleanup;
        }
    }

 cleanup:

  /* Flush the contents of the brigade (returning an error only if we
     don't already have one). */
  if (((apr_err = ap_fflush(output, uc.bb))) && (! derr))
    derr = dav_svn_convert_err(svn_error_create(apr_err, 0, NULL),
                               HTTP_INTERNAL_SERVER_ERROR,
                               "Error flushing brigade.");

  /* if an error was produced EITHER by the dir_delta drive or the
     resource-walker... */
  if (derr)
    {
      if (rbaton)
        svn_error_clear(svn_repos_abort_report(rbaton, resource->pool));
      return derr;
    }

  /* Destroy our subpool. */
  svn_pool_destroy(subpool);

  return NULL;
}
