/*
 * commit.c :  routines for committing changes to the server
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



#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_uuid.h>
#include <apr_portable.h>

#define APR_WANT_STDIO
#define APR_WANT_STRFUNC
#include <apr_want.h>

#if APR_HAVE_STDLIB
#include <stdlib.h>     /* for free() */
#endif

#include <ne_request.h>
#include <ne_props.h>
#include <ne_basic.h>

#include "svn_pools.h"
#include "svn_error.h"
#include "svn_delta.h"
#include "svn_io.h"
#include "svn_ra.h"
#include "svn_wc.h"
#include "svn_path.h"
#include "svn_xml.h"

#include "ra_dav.h"


/*
** resource_t: identify the relevant pieces of a resource on the server
**
** URL refers to the public/viewable/original resource.
** VSN_URL refers to the version resource that we stored locally
** WR_URL refers to a working resource for this resource
**
** Note that VSN_URL is NULL if this resource has just been added, and
** WR_URL can be NULL if the resource has not (yet) been checked out.
**
** LOCAL_PATH is relative to the root of the commit. It will be used
** for the get_func, set_func, and close_func callbacks.
*/
typedef struct
{
  const char *url;
  const char *vsn_url;
  const char *wr_url;

  svn_stringbuf_t *local_path;
} resource_t;

typedef struct
{
  svn_ra_session_t *ras;
  const char *activity_url;

  /* ### resources may not be needed */
  apr_hash_t *resources;        /* URL (const char *) -> RESOURCE_T */

  /* items that have been deleted */
  /* ### not sure if this "deleted" thing is Goodness */
  apr_array_header_t *deleted;

  /* name of local prop to hold the version resource's URL */
  svn_stringbuf_t *vsn_url_name;

  apr_hash_t *valid_targets;
  svn_ra_get_wc_prop_func_t get_func;
  svn_ra_set_wc_prop_func_t set_func;
  svn_ra_close_commit_func_t close_func;
  void *close_baton;

  /* The (potential) author of this commit. */
  const char *user;

  /* Log message for the commit. */
  svn_stringbuf_t *log_msg;

  /* The new revision created by this commit. */
  svn_revnum_t *new_rev;

  /* The date (according to the repository) of this commit. */
  const char **committed_date;

  /* The author (also according to the repository) of this commit. */
  const char **committed_author;

} commit_ctx_t;

typedef struct
{
  commit_ctx_t *cc;
  resource_t *rsrc;
  apr_table_t *prop_changes; /* name/values pairs of changed (or new) properties. */
  apr_array_header_t *prop_deletes; /* names of properties to delete. */
} resource_baton_t;

typedef struct
{
  apr_pool_t *pool;
  apr_file_t *tmpfile;
  svn_stringbuf_t *fname;
  resource_baton_t *file;
} put_baton_t;

/*
** singleton_delete_prop:
**
** The address of this integer is used as a "singleton" value to mark
** properties which must be deleted. Properties which are changed/added
** will use their new values.
*/
static const int singleton_delete_prop = 0;
#define DELETE_THIS_PROP (&singleton_delete_prop)

/* this property will be fetched from the server when we don't find it
   cached in the WC property store. */
static const ne_propname fetch_props[] =
{
  { "DAV:", "checked-in" },
  { NULL }
};

static const ne_propname log_message_prop = { SVN_PROP_PREFIX, "log" };


static svn_stringbuf_t *escape_url(const char *url, apr_pool_t *pool)
{
  svn_string_t url_str;
  url_str.data = url;
  url_str.len = strlen(url);
  return svn_path_uri_encode(&url_str, pool);
}
  

static svn_error_t * simple_request(svn_ra_session_t *ras, const char *method,
                                    const char *url, int *code)
{
  ne_request *req;
  svn_stringbuf_t *url_str = escape_url(url, ras->pool);
  int rv;

  /* create/prep the request */
  req = ne_request_create(ras->sess, method, url_str->data);
  if (req == NULL)
    {
      return svn_error_createf(SVN_ERR_RA_CREATING_REQUEST, 0, NULL,
                               ras->pool,
                               "Could not create a request (%s %s)",
                               method, url_str->data);
    }

  /* run the request and get the resulting status code. */
  rv = ne_request_dispatch(req);
  *code = ne_get_status(req)->code;
  ne_request_destroy(req);

  if (rv != NE_OK)
    {
      const char *msg = apr_pstrcat(ras->pool, method, " failed", NULL);

      return svn_ra_dav__convert_error(ras->sess, msg, rv, ras->pool);
    }

  return NULL;
}

static svn_error_t * get_version_url(commit_ctx_t *cc, resource_t *rsrc,
                                     svn_revnum_t revision)
{
  svn_ra_dav_resource_t *propres;
  const char *url;

  if (cc->get_func != NULL)
    {
      svn_stringbuf_t *vsn_url_value;

      SVN_ERR( (*cc->get_func)(cc->close_baton, rsrc->local_path,
                               cc->vsn_url_name, &vsn_url_value) );
      if (vsn_url_value != NULL)
        {
          rsrc->vsn_url = vsn_url_value->data;
          return NULL;
        }

      /* whoops. it wasn't there. go grab it from the server. */
    }

  if (revision == SVN_INVALID_REVNUM)
    url = rsrc->url;
  else
    {
      svn_string_t bc_url, bc_relative;
      svn_stringbuf_t *bc;

      SVN_ERR( svn_ra_dav__get_baseline_info(NULL,
                                             &bc_url, &bc_relative, NULL,
                                             cc->ras->sess,
                                             rsrc->url,
                                             revision,
                                             cc->ras->pool));
      bc = svn_stringbuf_create_from_string(&bc_url, cc->ras->pool);
      svn_path_add_component_nts(bc, bc_relative.data,
                                 svn_path_url_style);

      url = bc->data;
    }

  SVN_ERR( svn_ra_dav__get_props_resource(&propres, cc->ras->sess, url,
                                          NULL, fetch_props, cc->ras->pool) );
  rsrc->vsn_url = apr_hash_get(propres->propset,
                               SVN_RA_DAV__PROP_CHECKED_IN,
                               APR_HASH_KEY_STRING);
  if (rsrc->vsn_url == NULL)
    {
      /* ### need a proper SVN_ERR here */
      return svn_error_create(APR_EGENERAL, 0, NULL, cc->ras->pool,
                              "Could not fetch the Version Resource URL "
                              "(needed during an import or when it is "
                              "missing from the local, cached props).");
    }

  return NULL;
}

static svn_error_t * get_activity_url(commit_ctx_t *cc,
                                      svn_stringbuf_t **activity_url)
{

  if (cc->get_func != NULL)
    {
      /* with a get_func, we can just ask for the activity URL from the
         property store. */

      /* ### damn, this is annoying to have to create strings */
      svn_stringbuf_t * propname = svn_stringbuf_create(SVN_RA_DAV__LP_ACTIVITY_URL,
                                                  cc->ras->pool);
      svn_stringbuf_t * path = svn_stringbuf_create(".", cc->ras->pool);

      /* get the URL where we should create activities */
      SVN_ERR( (*cc->get_func)(cc->close_baton, path, propname,
                               activity_url) );

      if (*activity_url != NULL)
        {
          /* the property was there. return it. */

          /* ### urk. copy the thing to get a proper pool in there */
          *activity_url = svn_stringbuf_dup(*activity_url, cc->ras->pool);

          return NULL;
        }

      /* property not found for some reason. get it from the server. */
    }

  /* use our utility function to fetch the activity URL */
  return svn_ra_dav__get_activity_url(activity_url, cc->ras,
                                      cc->ras->root.path, cc->ras->pool);
}

static svn_error_t * create_activity(commit_ctx_t *cc)
{
  svn_stringbuf_t * activity_url;
  apr_uuid_t uuid;
  char uuid_buf[APR_UUID_FORMATTED_LENGTH + 1];
  int code;

  /* get the URL where we'll create activities */
  SVN_ERR( get_activity_url(cc, &activity_url) );

  /* the URL for our activity will be ACTIVITY_URL/UUID */
  apr_uuid_get(&uuid);
  apr_uuid_format(uuid_buf, &uuid);

  /* ### grumble. this doesn't watch out for trailing "/" */
  svn_path_add_component_nts(activity_url, uuid_buf, svn_path_url_style);

  cc->activity_url = activity_url->data;

  /* do a MKACTIVITY request and get the resulting status code. */
  SVN_ERR( simple_request(cc->ras, "MKACTIVITY", cc->activity_url, &code) );
  if (code != 201)
    {
      /* ### need to be more sophisticated with reporting the failure */
      return svn_error_create(SVN_ERR_RA_MKACTIVITY_FAILED, 0, NULL,
                              cc->ras->pool,
                              "The MKACTIVITY request failed.");
    }

  return NULL;
}

static svn_error_t * add_child(resource_t **child,
                               commit_ctx_t *cc,
                               const resource_t *parent,
                               const char *name,
                               int created,
                               svn_revnum_t revision)
{
  apr_pool_t *pool = cc->ras->pool;
  resource_t *rsrc;

  /* ### todo:  This from Yoshiki Hayashi <yoshiki@xemacs.org>:

     Probably created flag in add_child can be removed because
        revision is valid => created is false 
        revision is invalid => created is true
  */

  rsrc = apr_pcalloc(pool, sizeof(*rsrc));
  rsrc->url = apr_pstrcat(pool, parent->url, "/", name, NULL);

  rsrc->local_path = svn_stringbuf_dup(parent->local_path, pool);
  svn_path_add_component_nts(rsrc->local_path, name, svn_path_local_style);

  /* Case 1:  the resource is truly "new".  Either it was added as a
     completely new object, or implicitly created via a COPY.  Either
     way, it has no VR URL anywhere.  However, we *can* derive its WR
     URL by the rules of deltaV:  "copy structure is preserved below
     the WR you COPY to."  */
  if (created || (parent->vsn_url == NULL))
    {
      /* ### does the workcol have a trailing slash? do some extra work */
      rsrc->wr_url = apr_pstrcat(pool, parent->wr_url, "/", name, NULL);
    }

  /* Case 2: the resource is already under version-control somewhere.
     This means it has a VR URL already, and the WR URL won't exist
     until it's "checked out". */
  else
    SVN_ERR( get_version_url(cc, rsrc, revision) );

  apr_hash_set(cc->resources, rsrc->url, APR_HASH_KEY_STRING, rsrc);

  *child = rsrc;
  return NULL;
}

/* check out the specified resource (if it hasn't been checked out yet) */
static svn_error_t * checkout_resource(commit_ctx_t *cc, resource_t *res)
{
  ne_request *req;
  int rv;
  int code;
  const char *body;
  const char *locn = NULL;
  struct uri parse;
  svn_stringbuf_t *url_str;

  if (res->wr_url != NULL)
    {
      /* already checked out! */
      return NULL;
    }

  /* assert: res->vsn_url != NULL */
  url_str = escape_url(res->vsn_url, cc->ras->pool);

  /* ### send a CHECKOUT resource on res->vsn_url; include cc->activity_url;
     ### place result into res->wr_url and return it */

  /* create/prep the request */
  req = ne_request_create(cc->ras->sess, "CHECKOUT", url_str->data);
  if (req == NULL)
    {
      return svn_error_createf(SVN_ERR_RA_CREATING_REQUEST, 0, NULL,
                               cc->ras->pool,
                               "Could not create a CHECKOUT request (%s)",
                               url_str->data);
    }

  /* ### store this into cc to avoid pool growth */
  body = apr_psprintf(cc->ras->pool,
                      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<D:checkout xmlns:D=\"DAV:\">"
                      "<D:activity-set>"
                      "<D:href>%s</D:href>"
                      "</D:activity-set></D:checkout>", cc->activity_url);
  ne_set_request_body_buffer(req, body, strlen(body));

  /* 
   * We have different const qualifiers here. locn is const char *,
   * but the prototype is void * (as opposed to const void *).
   */
  ne_add_response_header_handler(req, "location",
                                 ne_duplicate_header, (void *)&locn);

  /* run the request and get the resulting status code. */
  rv = ne_request_dispatch(req);
  code = ne_get_status(req)->code;
  ne_request_destroy(req);

  if (rv != NE_OK)
    {
      /* ### need to be more sophisticated with reporting the failure */
      return svn_error_createf(SVN_ERR_RA_REQUEST_FAILED, 0, NULL,
                               cc->ras->pool,
                               "The CHECKOUT request failed (neon #%d) (%s)",
                               rv, url_str->data);
    }

  if (code != 201)
    {
      /* ### need to be more sophisticated with reporting the failure */
      return svn_error_createf(SVN_ERR_RA_REQUEST_FAILED, 0, NULL,
                               cc->ras->pool,
                               "The CHECKOUT request failed (http #%d) (%s)",
                               code, url_str->data);
    }
  if (locn == NULL)
    {
      return svn_error_create(SVN_ERR_RA_REQUEST_FAILED, 0, NULL,
                              cc->ras->pool,
                              "The CHECKOUT response did not contain a "
                              "Location: header.");
    }

  /* The location is an absolute URI. We want just the path portion. */
  /* ### what to do with the rest? what if it points somewhere other
     ### than the current session? */
  uri_parse(locn, &parse, NULL);
  res->wr_url = apr_pstrdup(cc->ras->pool, parse.path);
  uri_free(&parse);
  free((void *)locn);

  return NULL;
}

static void record_prop_change(apr_pool_t *pool,
                               resource_baton_t *r,
                               const svn_stringbuf_t *name,
                               const svn_stringbuf_t *value)
{
  if (value)
    {
      svn_stringbuf_t *escaped = NULL;

      /* changed/new property */
      if (r->prop_changes == NULL)
        r->prop_changes = apr_table_make(pool, 5);

      svn_xml_escape_stringbuf(&escaped, value, pool);
      apr_table_set(r->prop_changes, name->data, escaped->data);
    }
  else
    {
      /* deleted property. */
      const char **elt;

      if (r->prop_deletes == NULL)
        r->prop_deletes = apr_array_make(pool, 5, sizeof(char *));
  
      elt = apr_array_push(r->prop_deletes);
      *elt = apr_pstrdup(pool, name->data);
    }
}

/* Callback iterator for apr_table_do below. */
static int do_setprop(void *rec, const char *name, const char *value)
{
  ne_buffer *body = rec;

  /* use custom prefix for anything that doesn't start with "svn:" */
  if (strncmp(name, "svn:", 4) == 0)
    ne_buffer_concat(body, "<S:", name + 4, ">", value, "</S:", name + 4, ">",
                     NULL);
  else
    ne_buffer_concat(body, "<C:", name, ">", value, "</C:", name, ">", NULL);

  return 1;
}

static svn_error_t * do_proppatch(svn_ra_session_t *ras,
                                  const resource_t *rsrc,
                                  resource_baton_t *rb)
{
  ne_request *req;
  int rv;
  int code;
  ne_buffer *body; /* ### using an ne_buffer because it can realloc */
  svn_stringbuf_t *url_str;

  /* just punt if there are no changes to make. */
  if ((rb->prop_changes == NULL || apr_is_empty_table(rb->prop_changes))
      && (rb->prop_deletes == NULL || rb->prop_deletes->nelts == 0))
    return NULL;

  /* easier to roll our own PROPPATCH here than use ne_proppatch(), which 
   * doesn't really do anything clever. */
  body = ne_buffer_create();

  ne_buffer_zappend(body,
                    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>" DEBUG_CR
                    "<D:propertyupdate xmlns:D=\"DAV:\" xmlns:C=\""
                    SVN_PROP_CUSTOM_PREFIX "\" xmlns:S=\"svn:\">");

  if (rb->prop_changes != NULL)
    {
      ne_buffer_zappend(body, "<D:set><D:prop>");
      apr_table_do(do_setprop, body, rb->prop_changes, NULL);      
      ne_buffer_zappend(body, "</D:prop></D:set>");
    }
  
  if (rb->prop_deletes != NULL)
    {
      int n;

      ne_buffer_zappend(body, "<D:remove><D:prop>");
      
      for (n = 0; n < rb->prop_deletes->nelts; n++) 
        {
          const char *name = APR_ARRAY_IDX(rb->prop_deletes, n, const char *);

          /* use custom prefix for anything that doesn't start with "svn:" */
          if (strncmp(name, "svn:", 4) == 0)
            ne_buffer_concat(body, "<S:", name + 4, "/>", NULL);
          else
            ne_buffer_concat(body, "<C:", name, "/>", NULL);
        }

      ne_buffer_zappend(body, "</D:prop></D:remove>");

    }

  ne_buffer_zappend(body, "</D:propertyupdate>");

  url_str = escape_url(rsrc->wr_url, ras->pool);
  req = ne_request_create(ras->sess, "PROPPATCH", url_str->data);

  ne_set_request_body_buffer(req, body->data, ne_buffer_size(body));
  ne_add_request_header(req, "Content-Type", "text/xml; charset=UTF-8");

  rv = ne_request_dispatch(req);
  code = ne_get_status(req)->code;
  ne_request_destroy(req);
  ne_buffer_destroy(body);

  if (rv != NE_OK || code != 207)
    {
      return svn_error_createf(SVN_ERR_RA_REQUEST_FAILED, 0, NULL,
                               ras->pool,
                               "The PROPPATCH request failed (neon: %d) (%s)",
                               rv, url_str->data);
    }

  return NULL;
}

static svn_error_t * commit_open_root(void *edit_baton,
                                      svn_revnum_t base_revision, 
                                      void **root_baton)
{
  commit_ctx_t *cc = edit_baton;
  resource_baton_t *root;
  resource_t *rsrc;

  /* create the root resource. no wr_url (yet) */
  rsrc = apr_pcalloc(cc->ras->pool, sizeof(*rsrc));
  rsrc->url = cc->ras->root.path;

  /* ### should we use the WC symbol? (SVN_WC_ENTRY_THIS_DIR) */
  rsrc->local_path = svn_stringbuf_create("", cc->ras->pool);

  SVN_ERR( get_version_url(cc, rsrc, SVN_INVALID_REVNUM) );

  apr_hash_set(cc->resources, rsrc->url, APR_HASH_KEY_STRING, rsrc);

  root = apr_pcalloc(cc->ras->pool, sizeof(*root));
  root->cc = cc;
  root->rsrc = rsrc;

  *root_baton = root;

  return NULL;
}

static svn_error_t * commit_delete_entry(svn_stringbuf_t *name,
                                         svn_revnum_t revision,
                                         void *parent_baton)
{
  resource_baton_t *parent = parent_baton;
  apr_pool_t *pool = parent->cc->ras->pool;
  const char *child;
  int code;

  /* get the URL to the working collection */
  SVN_ERR( checkout_resource(parent->cc, parent->rsrc) );

  /* create the URL for the child resource */
  /* ### does the workcol have a trailing slash? do some extra work */
  child = apr_pstrcat(pool, parent->rsrc->wr_url, "/", name->data, NULL);

  /* Note: the child cannot have a resource stored in the resources table
     because of the editor traversal rules. That is: this is the first time
     we have seen anything about the child, and we're deleting it. As a
     corollary, we know the child hasn't been checked out. */

  /* delete the child resource */
  SVN_ERR( simple_request(parent->cc->ras, "DELETE", child, &code) );

  /* ## 404 is ignored, because mod_dav_svn is effectively merging
     against the HEAD revision on-the-fly.  In such a universe, a
     failed deletion (because it's already missing) is OK;  deletion
     is an omnipotent merge operation. */
  if ((code != 204) && (code != 404))
    {
      /* ### need to be more sophisticated with reporting the failure */
      return svn_error_createf(SVN_ERR_RA_DELETE_FAILED, 0, NULL, pool,
                               "Could not DELETE %s", child);
    }

  *(const char **)apr_array_push(parent->cc->deleted) =
    apr_pstrcat(pool, parent->rsrc->local_path->data, "/", name->data, NULL);

  return NULL;
}

static svn_error_t * commit_add_dir(svn_stringbuf_t *name,
                                    void *parent_baton,
                                    svn_stringbuf_t *copyfrom_path,
                                    svn_revnum_t copyfrom_revision,
                                    void **child_baton)
{
  resource_baton_t *parent = parent_baton;
  apr_pool_t *pool = parent->cc->ras->pool;
  resource_baton_t *child;
  int code;

  /* check out the parent resource so that we can create the new collection
     as one of its children. */
  SVN_ERR( checkout_resource(parent->cc, parent->rsrc) );

  /* create a child object that contains all the resource urls */
  child = apr_pcalloc(pool, sizeof(*child));
  child->cc = parent->cc;
  SVN_ERR( add_child(&child->rsrc, parent->cc, parent->rsrc,
                     name->data, 1, SVN_INVALID_REVNUM) );

  if (! copyfrom_path)
    {
      /* This a new directory with no history, so just create a new,
         empty collection */
      SVN_ERR( simple_request(parent->cc->ras, "MKCOL", child->rsrc->wr_url,
                              &code) );

      if (code != 201) /* "created" */
        {
          /* ### need to be more sophisticated with reporting the failure */
          return 
            svn_error_createf(SVN_ERR_RA_REQUEST_FAILED, 0, NULL, pool,
                              "MKCOL request failed for '%s'", name->data);
        }
    }
  else
    {
      svn_string_t bc_url, bc_relative;
      svn_stringbuf_t *src_url;
      int status;

      /* This add has history, so we need to do a COPY. */
      
      /* Convert the copyfrom_* url/rev "public" pair into a Baseline
         Collection (BC) URL that represents the revision -- and a
         relative path under that BC.  */
      SVN_ERR( svn_ra_dav__get_baseline_info(NULL,
                                             &bc_url, &bc_relative, NULL,
                                             parent->cc->ras->sess,
                                             copyfrom_path->data,
                                             copyfrom_revision,
                                             pool));


      /* Combine the BC-URL and relative path; this is the main
         "source" argument to the COPY request.  The "Destination:"
         header given to COPY is simply the wr_url that is already
         part of the child object. */
      src_url = svn_stringbuf_create(bc_url.data, pool);
      svn_path_add_component_nts(src_url, bc_relative.data,
                                 svn_path_url_style);

      /* Have neon do the COPY. */
      status = ne_copy(parent->cc->ras->sess,
                       1,                   /* overwrite */
                       NE_DEPTH_INFINITE,   /* always copy dirs deeply */
                       src_url->data,       /* source URI */
                       child->rsrc->wr_url); /* dest URI */

      if (status != NE_OK)
        {
          return 
            svn_error_createf(SVN_ERR_RA_REQUEST_FAILED, 0, NULL, pool,
                              "COPY request failed for '%s'", name->data);
        }
    }

  *child_baton = child;
  return NULL;
}

static svn_error_t * commit_open_dir(svn_stringbuf_t *name,
                                     void *parent_baton,
                                     svn_revnum_t base_revision,
                                     void **child_baton)
{
  resource_baton_t *parent = parent_baton;
  apr_pool_t *pool = parent->cc->ras->pool;
  resource_baton_t *child = apr_pcalloc(pool, sizeof(*child));

  child->cc = parent->cc;
  SVN_ERR( add_child(&child->rsrc, parent->cc, parent->rsrc,
                     name->data, 0, base_revision) );

  /*
  ** Note: open_dir simply means that a change has occurred somewhere
  **       within this directory. We have nothing to do, to prepare for
  **       those changes (each will be considered independently).
  **
  ** Note: if a directory is replaced by something else, then this callback
  **       will not be used: a true replacement is modeled with a "delete"
  **       followed by an "add".
  */

  *child_baton = child;
  return NULL;
}

static svn_error_t * commit_change_dir_prop(void *dir_baton,
                                            svn_stringbuf_t *name,
                                            svn_stringbuf_t *value)
{
  resource_baton_t *dir = dir_baton;

  /* record the change. it will be applied at close_dir time. */
  record_prop_change(dir->cc->ras->pool, dir, name, value);

  /* do the CHECKOUT sooner rather than later */
  SVN_ERR( checkout_resource(dir->cc, dir->rsrc) );

  return NULL;
}

static svn_error_t * commit_close_dir(void *dir_baton)
{
  resource_baton_t *dir = dir_baton;

  /* Perform all of the property changes on the directory. Note that we
     checked out the directory when the first prop change was noted. */
  SVN_ERR( do_proppatch(dir->cc->ras, dir->rsrc, dir) );

  return NULL;
}

static svn_error_t * commit_add_file(svn_stringbuf_t *name,
                                     void *parent_baton,
                                     svn_stringbuf_t *copyfrom_path,
                                     svn_revnum_t copyfrom_revision,
                                     void **file_baton)
{
  resource_baton_t *parent = parent_baton;
  apr_pool_t *pool = parent->cc->ras->pool;
  resource_baton_t *file;

  /* ### store name, parent? */

  /*
  ** To add a new file into the repository, we CHECKOUT the parent
  ** collection, then PUT the file as a member of the resuling working
  ** collection.
  **
  ** If the file was copied from elsewhere, then we will use the COPY
  ** method to copy into the working collection.
  */

  /* Do the parent CHECKOUT first */
  SVN_ERR( checkout_resource(parent->cc, parent->rsrc) );

  /* Construct a file_baton that contains all the resource urls. */
  file = apr_pcalloc(pool, sizeof(*file));
  file->cc = parent->cc;
  SVN_ERR( add_child(&file->rsrc, parent->cc, parent->rsrc,
                     name->data, 1, SVN_INVALID_REVNUM) );

  if (! copyfrom_path)
    {
      /* This a truly new file. */

      /* ### wait for apply_txdelta before doing a PUT. it might arrive a
         ### "long time" from now. certainly after many other operations, so
         ### we don't want to start a PUT just yet.
         ### so... anything else to do here?
      */
    }
  else
    {
      svn_string_t bc_url, bc_relative;
      svn_stringbuf_t *src_url;
      int status;

      /* This add has history, so we need to do a COPY. */
      
      /* Convert the copyfrom_* url/rev "public" pair into a Baseline
         Collection (BC) URL that represents the revision -- and a
         relative path under that BC.  */
      SVN_ERR( svn_ra_dav__get_baseline_info(NULL,
                                             &bc_url, &bc_relative, NULL,
                                             parent->cc->ras->sess,
                                             copyfrom_path->data,
                                             copyfrom_revision,
                                             pool));


      /* Combine the BC-URL and relative path; this is the main
         "source" argument to the COPY request.  The "Destination:"
         header given to COPY is simply the wr_url that is already
         part of the file_baton. */
      src_url = svn_stringbuf_create(bc_url.data, pool);
      svn_path_add_component_nts(src_url, bc_relative.data,
                                 svn_path_url_style);

      /* Have neon do the COPY. */
      status = ne_copy(parent->cc->ras->sess,
                       1,                   /* overwrite */
                       NE_DEPTH_ZERO,       /* for a file, does it care? */
                       src_url->data,       /* source URI */
                       file->rsrc->wr_url); /* dest URI */

      if (status != NE_OK)
        {
          return 
            svn_error_createf(SVN_ERR_RA_REQUEST_FAILED, 0, NULL, pool,
                              "COPY request failed for '%s'", name->data);
        }
    }

  /* return the file_baton */
  *file_baton = file;
  return NULL;
}

static svn_error_t * commit_open_file(svn_stringbuf_t *name,
                                      void *parent_baton,
                                      svn_revnum_t base_revision,
                                      void **file_baton)
{
  resource_baton_t *parent = parent_baton;
  apr_pool_t *pool = parent->cc->ras->pool;
  resource_baton_t *file;

  file = apr_pcalloc(pool, sizeof(*file));
  file->cc = parent->cc;
  SVN_ERR( add_child(&file->rsrc, parent->cc, parent->rsrc,
                     name->data, 0, base_revision) );

  /* do the CHECKOUT now. we'll PUT the new file contents later on. */
  SVN_ERR( checkout_resource(parent->cc, file->rsrc) );

  /* ### wait for apply_txdelta before doing a PUT. it might arrive a
     ### "long time" from now. certainly after many other operations, so
     ### we don't want to start a PUT just yet.
     ### so... anything else to do here? what about the COPY case?
  */

  *file_baton = file;
  return NULL;
}

static svn_error_t * commit_stream_write(void *baton,
                                         const char *data, apr_size_t *len)
{
  put_baton_t *pb = baton;
  apr_status_t status;

  /* drop the data into our temp file */
  status = apr_file_write_full(pb->tmpfile, data, *len, NULL);
  if (status)
    return svn_error_create(status, 0, NULL, pb->pool,
                            "Could not write svndiff to temp file.");

  return NULL;
}

static svn_error_t * commit_stream_close(void *baton)
{
  put_baton_t *pb = baton;
  commit_ctx_t *cc = pb->file->cc;
  resource_t *rsrc = pb->file->rsrc;
  ne_request *req;
  int fdesc;
  int rv;
  int code;
  apr_status_t status;
  apr_off_t offset = 0;
  svn_stringbuf_t *url_str = escape_url(rsrc->wr_url, cc->ras->pool);

  /* create/prep the request */
  req = ne_request_create(cc->ras->sess, "PUT", url_str->data);
  if (req == NULL)
    {
      return svn_error_createf(SVN_ERR_RA_CREATING_REQUEST, 0, NULL,
                               pb->pool,
                               "Could not create a PUT request (%s)",
                               url_str->data);
    }

  /* ### use a symbolic name somewhere for this MIME type? */
  ne_add_request_header(req, "Content-Type", "application/vnd.svn-svndiff");

  /* Rewind the tmpfile. */
  status = apr_file_seek(pb->tmpfile, APR_SET, &offset);
  if (status)
    {
      (void) apr_file_close(pb->tmpfile);
      return svn_error_create(status, 0, NULL, pb->pool,
                              "Couldn't rewind tmpfile.");
    }
  /* Convert the (apr_file_t *)tmpfile into a file descriptor for neon. */
  status = svn_io_fd_from_file(&fdesc, pb->tmpfile);
  if (status)
    {
      (void) apr_file_close(pb->tmpfile);
      return svn_error_create(status, 0, NULL, pb->pool,
                              "Couldn't get file-descriptor of tmpfile.");
    }

  /* Give the filedescriptor to neon. */
  ne_set_request_body_fd(req, fdesc);

  /* run the request and get the resulting status code. */
  rv = ne_request_dispatch(req);

  /* we're done with the file.  this should delete it. */
  (void) apr_file_close(pb->tmpfile);

  /* fetch the status, then clean up the request */
  code = ne_get_status(req)->code;
  ne_request_destroy(req);

  /* toss the pool. all things pb are now history */
  svn_pool_destroy(pb->pool);

  if (rv != NE_OK)
    {
      /* ### need to be more sophisticated with reporting the failure */
      return svn_error_createf(SVN_ERR_RA_REQUEST_FAILED, 0, NULL,
                               cc->ras->pool,
                               "The PUT request failed (neon: %d) (%s)",
                               rv, url_str->data);
    }

  /* if it didn't returned 201 (Created) or 204 (No Content), then puke */
  /* ### is that right? what else might we see? be more robust. */
  if (code != 201 && code != 204)
    {
      /* ### need to be more sophisticated with reporting the failure */
      return svn_error_createf(SVN_ERR_RA_REQUEST_FAILED, 0, NULL,
                               cc->ras->pool,
                               "The PUT request did not complete "
                               "properly (status: %d) (%s)",
                               code, url_str->data);
    }

  return NULL;
}

static svn_error_t * 
commit_apply_txdelta(void *file_baton, 
                     svn_txdelta_window_handler_t *handler,
                     void **handler_baton)
{
  resource_baton_t *file = file_baton;
  apr_pool_t *subpool;
  put_baton_t *baton;
  svn_stream_t *stream;

  /* construct a writable stream that gathers its contents into a buffer */
  subpool = svn_pool_create(file->cc->ras->pool);

  baton = apr_pcalloc(subpool, sizeof(*baton));
  baton->pool = subpool;
  baton->file = file;

  /* ### oh, hell. Neon's request body support is either text (a C string),
     ### or a FILE*. since we are getting binary data, we must use a FILE*
     ### for now. isn't that special? */

  /* Use the client callback to create a tmpfile. */
  SVN_ERR(file->cc->ras->callbacks->open_tmp_file 
          (&baton->tmpfile, 
           file->cc->ras->callback_baton));

  /* ### register a cleanup on our subpool which closes the file. this
     ### will ensure that the file always gets tossed, even if we exit
     ### with an error. */

  stream = svn_stream_create(baton, subpool);
  svn_stream_set_write(stream, commit_stream_write);
  svn_stream_set_close(stream, commit_stream_close);

  svn_txdelta_to_svndiff(stream, subpool, handler, handler_baton);

  return NULL;
}

static svn_error_t * commit_change_file_prop(
  void *file_baton,
  svn_stringbuf_t *name,
  svn_stringbuf_t *value)
{
  resource_baton_t *file = file_baton;

  /* record the change. it will be applied at close_file time. */
  record_prop_change(file->cc->ras->pool, file, name, value);

  /* do the CHECKOUT sooner rather than later */
  SVN_ERR( checkout_resource(file->cc, file->rsrc) );

  return NULL;
}

static svn_error_t * commit_close_file(void *file_baton)
{
  resource_baton_t *file = file_baton;

  /* Perform all of the property changes on the file. Note that we
     checked out the file when the first prop change was noted. */
  SVN_ERR( do_proppatch(file->cc->ras, file->rsrc, file) );

  return NULL;
}


static svn_error_t * commit_close_edit(void *edit_baton)
{
  commit_ctx_t *cc = edit_baton;

  /* ### different pool? */
  SVN_ERR( svn_ra_dav__merge_activity(cc->new_rev,
                                      cc->committed_date,
                                      cc->committed_author,
                                      cc->ras,
                                      cc->ras->root.path,
                                      cc->activity_url,
                                      cc->valid_targets,
                                      cc->set_func,
                                      cc->close_func,
                                      cc->close_baton,
                                      cc->deleted,
                                      cc->ras->pool) );

  SVN_ERR( svn_ra_dav__maybe_store_auth_info(cc->ras) );

  return NULL;
}

static svn_error_t * apply_log_message(commit_ctx_t *cc,
                                       svn_stringbuf_t *log_msg)
{
  apr_pool_t *pool = cc->ras->pool;
  const svn_string_t *vcc;
  const svn_string_t *baseline_url;
  resource_t baseline_rsrc = { 0 };
  ne_proppatch_operation po[2] = { { 0 } };
  int rv;
  svn_stringbuf_t *xml_data;

  /* ### this whole sequence can/should be replaced with an expand-property
     ### REPORT when that is available on the server. */

  /* fetch the DAV:version-controlled-configuration from the session's URL */
  SVN_ERR( svn_ra_dav__get_one_prop(&vcc, cc->ras->sess, cc->ras->root.path, 
                                    NULL, &svn_ra_dav__vcc_prop, pool) );

  /* ### we should use DAV:apply-to-version on the CHECKOUT so we can skip
     ### retrieval of the baseline */

  /* Get the Baseline from the DAV:checked-in value */
  SVN_ERR( svn_ra_dav__get_one_prop(&baseline_url, cc->ras->sess, vcc->data, 
                                    NULL, &svn_ra_dav__checked_in_prop, pool));

  baseline_rsrc.vsn_url = baseline_url->data;
  SVN_ERR( checkout_resource(cc, &baseline_rsrc) );

  /* XML-Escape the log message. */
  xml_data = NULL;              /* Required by svn_xml_escape_stringbuf. */
  svn_xml_escape_stringbuf(&xml_data,
                           svn_stringbuf_create(log_msg->data, cc->ras->pool),
                           cc->ras->pool);
  po[0].name = &log_message_prop;
  po[0].type = ne_propset;
  po[0].value = xml_data->data;

  rv = ne_proppatch(cc->ras->sess, baseline_rsrc.wr_url, po);
  if (rv != NE_OK)
    {
      return svn_error_createf(SVN_ERR_RA_REQUEST_FAILED, 0, NULL,
                               pool,
                               "The log message's PROPPATCH request failed "
                               "(neon: %d) (%s)",
                               rv, baseline_rsrc.wr_url);
    }


  return SVN_NO_ERROR;
}

svn_error_t * svn_ra_dav__get_commit_editor(
  void *session_baton,
  const svn_delta_edit_fns_t **editor,
  void **edit_baton,
  svn_revnum_t *new_rev,
  const char **committed_date,
  const char **committed_author,
  svn_stringbuf_t *log_msg,
  svn_ra_get_wc_prop_func_t get_func,
  svn_ra_set_wc_prop_func_t set_func,
  svn_ra_close_commit_func_t close_func,
  void *close_baton)
{
  svn_ra_session_t *ras = session_baton;

  /* The main commit editor and its baton. */
  svn_delta_edit_fns_t *commit_editor;
  commit_ctx_t *cc;

  /* The editor we return will be composed with a tracking editor,
     which will keep track of which targets should get their revisions
     and wcprops set after the commit.  Thus we avoid overzealously
     bumping directories on commit. */
  svn_delta_edit_fns_t *tracking_editor;
  void *tracking_baton;

  /* Build the main commit editor's baton. */
  cc = apr_pcalloc(ras->pool, sizeof(*cc));
  cc->ras = ras;
  cc->resources = apr_hash_make(ras->pool);
  cc->vsn_url_name = svn_stringbuf_create(SVN_RA_DAV__LP_VSN_URL, ras->pool);
  cc->get_func = get_func;
  cc->valid_targets = apr_hash_make(ras->pool);
  cc->set_func = set_func;
  cc->close_func = close_func;
  cc->close_baton = close_baton;
  cc->log_msg = log_msg;
  cc->new_rev = new_rev;
  cc->committed_date = committed_date;
  cc->committed_author = committed_author;

  cc->deleted = apr_array_make(ras->pool, 10, sizeof(const char *));

  /* ### should we perform an OPTIONS to validate the server we're about
     ### to talk to? */

  /*
  ** Create an Activity. This corresponds directly to an FS transaction.
  ** We will check out all further resources within the context of this
  ** activity.
  */
  SVN_ERR( create_activity(cc) );

  /*
  ** Find the latest baseline resource, check it out, and then apply the
  ** log message onto the thing.
  */
  SVN_ERR( apply_log_message(cc, log_msg) );

  /*
  ** Set up the editor.
  **
  ** This structure is used during the commit process. An external caller
  ** uses these callbacks to describe all the changes in the working copy
  ** that must be committed to the server.
  */
  commit_editor = svn_delta_default_editor(ras->pool);
  commit_editor->open_root = commit_open_root;
  commit_editor->delete_entry = commit_delete_entry;
  commit_editor->add_directory = commit_add_dir;
  commit_editor->open_directory = commit_open_dir;
  commit_editor->change_dir_prop = commit_change_dir_prop;
  commit_editor->close_directory = commit_close_dir;
  commit_editor->add_file = commit_add_file;
  commit_editor->open_file = commit_open_file;
  commit_editor->apply_textdelta = commit_apply_txdelta;
  commit_editor->change_file_prop = commit_change_file_prop;
  commit_editor->close_file = commit_close_file;
  commit_editor->close_edit = commit_close_edit;

  /* Get the tracking editor. */
  SVN_ERR( svn_delta_get_commit_track_editor(&tracking_editor,
                                             &tracking_baton,
                                             ras->pool,
                                             cc->valid_targets,
                                             SVN_INVALID_REVNUM,
                                             NULL,
                                             NULL) );

  /* Compose the two editors, returning the composition by reference. */
  svn_delta_compose_editors(editor, edit_baton,
                            tracking_editor, tracking_baton,
                            commit_editor, cc,
                            ras->pool);

  return NULL;
}






/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
