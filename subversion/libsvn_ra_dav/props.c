/*
 * props.c :  routines for fetching DAV properties
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
#include <apr_tables.h>
#include <apr_strings.h>
#define APR_WANT_STRFUNC
#include <apr_want.h>

#include <ne_basic.h>
#include <ne_props.h>
#include <ne_xml.h>

#include "svn_error.h"
#include "svn_delta.h"
#include "svn_ra.h"
#include "svn_path.h"

#include "ra_dav.h"


/* some definitions of various properties that may be fetched */
const ne_propname svn_ra_dav__vcc_prop = {
  "DAV:", "version-controlled-configuration"
};
const ne_propname svn_ra_dav__checked_in_prop = {
  "DAV:", "checked-in"
};


typedef struct {
  ne_xml_elmid id;
  const char *name;
  int is_property;      /* is it a property, or part of some structure? */
} elem_defn;

static const elem_defn elem_definitions[] =
{
  /* DAV elements */
  { ELEM_baseline_coll, SVN_RA_DAV__PROP_BASELINE_COLLECTION, 0 },
  { ELEM_checked_in, SVN_RA_DAV__PROP_CHECKED_IN, 0 },
  { ELEM_vcc, SVN_RA_DAV__PROP_VCC, 0 },
  { ELEM_version_name, SVN_RA_DAV__PROP_VERSION_NAME, 1 },

  /* SVN elements */
  { ELEM_baseline_relpath, SVN_RA_DAV__PROP_BASELINE_RELPATH, 1 },

  { 0 }
};

static const struct ne_xml_elm neon_descriptions[] =
{
  /* DAV elements */
  { "DAV:", "baseline-collection", ELEM_baseline_coll, NE_XML_CDATA },
  { "DAV:", "checked-in", ELEM_checked_in, 0 },
  { "DAV:", "collection", ELEM_collection, NE_XML_CDATA },
  { "DAV:", "href", NE_ELM_href, NE_XML_CDATA },
  { "DAV:", "resourcetype", ELEM_resourcetype, 0 },
  { "DAV:", "version-controlled-configuration", ELEM_vcc, 0 },
  { "DAV:", "version-name", ELEM_version_name, NE_XML_CDATA },

  /* SVN elements */
  { SVN_PROP_PREFIX, "baseline-relative-path", ELEM_baseline_relpath,
    NE_XML_CDATA },

  { NULL }
};

typedef struct {
  /* PROPS: URL-PATH -> RESOURCE (const char * -> svn_ra_dav_resource_t *) */
  apr_hash_t *props;

  apr_pool_t *pool;

  ne_propfind_handler *dph;

} prop_ctx_t;

/* when we begin a checkout, we fetch these from the "public" resources to
   steer us towards a Baseline Collection. we fetch the resourcetype to
   verify that we're accessing a collection. */
static const ne_propname starting_props[] =
{
  { "DAV:", "version-controlled-configuration" },
  { SVN_PROP_PREFIX, "baseline-relative-path" },
  { "DAV:", "resourcetype" },
  { NULL }
};

/* when speaking to a Baseline to reach the Baseline Collection, fetch these
   properties. */
static const ne_propname baseline_props[] =
{
  { "DAV:", "baseline-collection" },
  { "DAV:", "version-name" },
  { NULL }
};



/* look up an element definition. may return NULL if the elem is not
   recognized. */
static const elem_defn *defn_from_id(ne_xml_elmid id)
{
  const elem_defn *defn;

  for (defn = elem_definitions; defn->name != NULL; ++defn)
    {
      if (id == defn->id)
        return defn;
    }

  return NULL;
}

static void *create_private(void *userdata, const char *url)
{
  prop_ctx_t *pc = userdata;
  struct uri parsed_url;
  char *url_path;
  svn_ra_dav_resource_t *r = apr_pcalloc(pc->pool, sizeof(*r));
  apr_size_t len;
  svn_string_t my_url = { url, strlen(url) };
  svn_stringbuf_t *url_str = svn_path_uri_decode(&my_url, pc->pool);

  r->pool = pc->pool;

  /* parse the PATH element out of the URL

     Note: mod_dav does not (currently) use an absolute URL, but simply a
     server-relative path (i.e. this uri_parse is effectively a no-op).
  */
  (void) uri_parse(url_str->data, &parsed_url, NULL);
  url_path = apr_pstrdup(pc->pool, parsed_url.path);
  uri_free(&parsed_url);

  /* clean up trailing slashes from the URL */
  len = strlen(url_path);
  if (len > 1 && url_path[len - 1] == '/')
    url_path[len - 1] = '\0';
  r->url = url_path;

  /* the properties for this resource */
  r->propset = apr_hash_make(pc->pool);

  /* store this resource into the top-level hash table */
  apr_hash_set(pc->props, url_path, APR_HASH_KEY_STRING, r);

  return r;
}

static int add_to_hash(void *userdata, const ne_propname *pname,
                       const char *value, const ne_status *status)
{
  svn_ra_dav_resource_t *r = userdata;
  const char *name;
  
  name = apr_pstrcat(r->pool, pname->nspace, pname->name, NULL);
  value = apr_pstrdup(r->pool, value);

  /* ### woah... what about a binary VALUE with a NULL character? */
  apr_hash_set(r->propset, name, APR_HASH_KEY_STRING, value);

  return 0;
}

static void process_results(void *userdata, const char *uri,
                            const ne_prop_result_set *rset)
{
  /*  prop_ctx_t *pc = userdata; */
  svn_ra_dav_resource_t *r = ne_propset_private(rset);

  /* ### should use ne_propset_status(rset) to determine whether the
   * ### PROPFIND failed for the properties we're interested in. */
  (void) ne_propset_iterate(rset, add_to_hash, r);
}

static int validate_element(void *userdata, ne_xml_elmid parent, ne_xml_elmid child)
{
  switch (parent)
    {
    case NE_ELM_prop:
        switch (child)
          {
          case ELEM_baseline_coll:
          case ELEM_baseline_relpath:
          case ELEM_checked_in:
          case ELEM_resourcetype:
          case ELEM_vcc:
          case ELEM_version_name:
            return NE_XML_VALID;

          default:
            /* some other, unrecognized property */
            return NE_XML_DECLINE;
          }
        
    case ELEM_baseline_coll:
    case ELEM_checked_in:
    case ELEM_vcc:
      if (child == NE_ELM_href)
        return NE_XML_VALID;
      else
        return NE_XML_DECLINE; /* not concerned with other types */
      
    case ELEM_resourcetype:
      if (child == ELEM_collection)
        return NE_XML_VALID;
      else
        return NE_XML_DECLINE; /* not concerned with other types (### now) */

    default:
      return NE_XML_DECLINE;
    }

  /* NOTREACHED */
}

static int start_element(void *userdata, const struct ne_xml_elm *elm,
                         const char **atts)
{
  prop_ctx_t *pc = userdata;
  svn_ra_dav_resource_t *r = ne_propfind_current_private(pc->dph);

  switch (elm->id)
    {
    case ELEM_collection:
      r->is_collection = 1;
      break;

    case ELEM_baseline_coll:
    case ELEM_checked_in:
    case ELEM_vcc:
      /* each of these contains a DAV:href element that we want to process */
      r->href_parent = elm->id;
      break;

    default:
      /* nothing to do for these */
      break;
    }

  return 0;
}

static int end_element(void *userdata, const struct ne_xml_elm *elm,
                       const char *cdata)
{
  prop_ctx_t *pc = userdata;
  svn_ra_dav_resource_t *r = ne_propfind_current_private(pc->dph);
  const char *name;

  if (elm->id == NE_ELM_href)
    {
      /* use the parent element's name, not the href */
      const elem_defn *parent_defn = defn_from_id(r->href_parent);

      name = parent_defn ? parent_defn->name : NULL;

      /* if name == NULL, then we don't know about this DAV:href. leave name
         NULL so that we don't store a property. */
    }
  else
    {
      const elem_defn *defn = defn_from_id(elm->id);

      /* if this element isn't a property, then skip it */
      if (defn == NULL || !defn->is_property)
        return 0;

      name = defn->name;
    }

  if (name != NULL)
    apr_hash_set(r->propset, name, APR_HASH_KEY_STRING,
                 apr_pstrdup(pc->pool, cdata));

  return 0;
}

svn_error_t * svn_ra_dav__get_props(apr_hash_t **results,
                                    ne_session *sess,
                                    const char *url,
                                    int depth,
                                    const char *label,
                                    const ne_propname *which_props,
                                    apr_pool_t *pool)
{
  ne_xml_parser *hip;
  int rv;
  prop_ctx_t pc = { 0 };
  svn_string_t my_url = { url, strlen(url) };
  svn_stringbuf_t *url_str = svn_path_uri_encode(&my_url, pool);

  pc.pool = pool;
  pc.props = apr_hash_make(pc.pool);

  pc.dph = ne_propfind_create(sess, url_str->data, depth);
  ne_propfind_set_private(pc.dph, create_private, &pc);
  hip = ne_propfind_get_parser(pc.dph);
  ne_xml_push_handler(hip, neon_descriptions,
                      validate_element, start_element, end_element, &pc);

  if (label != NULL)
    {
      /* get the request pointer and add a Label header */
      ne_request *req = ne_propfind_get_request(pc.dph);
      ne_add_request_header(req, "Label", label);
    }
  
  if (which_props) 
    {
      rv = ne_propfind_named(pc.dph, which_props, process_results, &pc);
    } 
  else
    { 
      rv = ne_propfind_allprop(pc.dph, process_results, &pc);
    }

  ne_propfind_destroy(pc.dph);

  if (rv != NE_OK)
    {
      switch (rv)
        {
        case NE_CONNECT:
          /* ### need an SVN_ERR here */
          return svn_error_createf(APR_EGENERAL, 0, NULL, pool,
                                   "Could not connect to server for '%s'",
                                   url_str->data);
        case NE_AUTH:
          return svn_error_create(SVN_ERR_RA_NOT_AUTHORIZED, 0, NULL, 
                                  pool,
                                  "Authentication failed on server.");
        default:
          /* ### need an SVN_ERR here */
          return svn_error_create(APR_EGENERAL, 0, NULL, pool,
                                  ne_get_error(sess));
        }
    }

  *results = pc.props;

  return SVN_NO_ERROR;
}

svn_error_t * svn_ra_dav__get_props_resource(svn_ra_dav_resource_t **rsrc,
                                             ne_session *sess,
                                             const char *url,
                                             const char *label,
                                             const ne_propname *which_props,
                                             apr_pool_t *pool)
{
  apr_hash_t *props;
  char * url_path = apr_pstrdup(pool, url);
  int len = strlen(url);
  /* Clean up any trailing slashes. */
  if (len > 1 && url[len - 1] == '/')
      url_path[len - 1] = '\0';

  SVN_ERR( svn_ra_dav__get_props(&props, sess, url_path, NE_DEPTH_ZERO,
                                 label, which_props, pool) );

  if (label != NULL)
    {
      /* pick out the first response: the URL requested will not match
       * the response href. */
      apr_hash_index_t *hi = apr_hash_first(pool, props);

      if (hi)
        {
          void *ent;
          apr_hash_this(hi, NULL, NULL, &ent);
          *rsrc = ent;
        }
    }
  else
    {
      *rsrc = apr_hash_get(props, url_path, APR_HASH_KEY_STRING);
    }

  if (*rsrc == NULL)
    {
      /* ### hmmm, should have been in there... */
      return svn_error_createf(APR_EGENERAL, 0, NULL, pool,
                               "failed to find label \"%s\" for url \"%s\"",
                               label, url_path);
    }

  return SVN_NO_ERROR;
}

svn_error_t * svn_ra_dav__get_one_prop(const svn_string_t **propval,
                                       ne_session *sess,
                                       const char *url,
                                       const char *label,
                                       const ne_propname *propname,
                                       apr_pool_t *pool)
{
  svn_ra_dav_resource_t *rsrc;
  ne_propname props[2] = { { 0 } };
  const char *name;
  const char *value;
  svn_string_t *sv;

  props[0] = *propname;
  SVN_ERR( svn_ra_dav__get_props_resource(&rsrc, sess, url, label, props,
                                          pool) );

  name = apr_pstrcat(pool, propname->nspace, propname->name, NULL);
  value = apr_hash_get(rsrc->propset, name, APR_HASH_KEY_STRING);
  if (value == NULL)
    {
      /* ### need an SVN_ERR here */
      return svn_error_createf(APR_EGENERAL, 0, NULL, pool,
                               "%s was not present on the resource.", name);
    }

  /* ### hmm. we can't deal with embedded NULLs right now... */
  sv = apr_palloc(pool, sizeof(*sv));
  sv->data = value;
  sv->len = strlen(value);
  *propval = sv;

  return SVN_NO_ERROR;
}

svn_error_t *svn_ra_dav__get_baseline_info(svn_boolean_t *is_dir,
                                           svn_string_t *bc_url,
                                           svn_string_t *bc_relative,
                                           svn_revnum_t *latest_rev,
                                           ne_session *sess,
                                           const char *url,
                                           svn_revnum_t revision,
                                           apr_pool_t *pool)
{
  svn_ra_dav_resource_t *rsrc;
  const char *vcc;

  /* ### we may be able to replace some/all of this code with an
     ### expand-property REPORT when that is available on the server. */

  /* -------------------------------------------------------------------
     STEP 1

     Fetch the following properties from the given URL:

     *) DAV:version-controlled-configuration so that we can reach the
        baseline information.

     *) svn:baseline-relative-path so that we can find this resource
        within a Baseline Collection.

     *) DAV:resourcetype so that we can identify whether this resource
        is a collection or not.
  */

  /* ### do we want to optimize the props we fetch, based on what the
     ### user has requested? i.e. omit resourcetype when is_dir is NULL
     ### and omit relpath when bc_relative is NULL. */
  SVN_ERR( svn_ra_dav__get_props_resource(&rsrc, sess, url,
                                          NULL, starting_props, pool) );

  if (is_dir != NULL)
    *is_dir = rsrc->is_collection;

  vcc = apr_hash_get(rsrc->propset, SVN_RA_DAV__PROP_VCC, APR_HASH_KEY_STRING);
  if (vcc == NULL)
    {
      /* ### better error reporting... */

      /* ### need an SVN_ERR here */
      return svn_error_create(APR_EGENERAL, 0, NULL, pool,
                              "The VCC property was not found on the "
                              "resource.");
    }

  /* if they want the relative path (could be, they're just trying to find
     the baseline collection), then return it */
  if (bc_relative != NULL)
    {
      bc_relative->data = apr_hash_get(rsrc->propset,
                                       SVN_RA_DAV__PROP_BASELINE_RELPATH,
                                       APR_HASH_KEY_STRING);
      if (bc_relative->data == NULL)
        {
          /* ### better error reporting... */

          /* ### need an SVN_ERR here */
          return svn_error_create(APR_EGENERAL, 0, NULL, pool,
                                  "The relative-path property was not "
                                  "found on the resource.");
        }

      bc_relative->len = strlen(bc_relative->data);
    }

  /* shortcut: no need to do more work if the data isn't needed. */
  if (bc_url == NULL && latest_rev == NULL)
    return SVN_NO_ERROR;

  /* -------------------------------------------------------------------
     STEP 2

     We have the Version Controlled Configuration (BCC). From here, we
     need to reach the Baseline for specified revision.

     If the revision is SVN_INVALID_REVNUM, then we're talking about
     the HEAD revision. We have one extra step to reach the Baseline:

     *) Fetch the DAV:checked-in from the VCC; it points to the Baseline.

     If we have a specific revision, then we use a Label header when
     fetching props from the VCC. This will direct us to the Baseline
     with that label (in this case, the label == the revision number).

     From the Baseline, we fetch the followig properties:

     *) DAV:baseline-collection, which is a complete tree of the Baseline
        (in SVN terms, this tree is rooted at a specific revision)

     *) DAV:version-name to get the revision of the Baseline that we are
        querying. When asking about the HEAD, this tells us its revision.
  */

  if (revision == SVN_INVALID_REVNUM)
    {
      /* Fetch the latest revision */

      const svn_string_t *baseline;

      /* Get the Baseline from the DAV:checked-in value, then fetch its
         DAV:baseline-collection property. */
      /* ### should wrap this with info about rsrc==VCC */
      SVN_ERR( svn_ra_dav__get_one_prop(&baseline, sess, vcc, NULL,
                                        &svn_ra_dav__checked_in_prop, pool) );

      /* ### do we want to optimize the props we fetch, based on what the
         ### user asked for? i.e. omit version-name if latest_rev is NULL */
      SVN_ERR( svn_ra_dav__get_props_resource(&rsrc, sess, 
                                              baseline->data, NULL,
                                              baseline_props, pool) );
    }
  else
    {
      /* Fetch a specific revision */

      char label[20];

      /* ### send Label hdr, get DAV:baseline-collection [from the baseline] */

      apr_snprintf(label, sizeof(label), "%ld", revision);

      /* ### do we want to optimize the props we fetch, based on what the
         ### user asked for? i.e. omit version-name if latest_rev is NULL */
      SVN_ERR( svn_ra_dav__get_props_resource(&rsrc, sess, vcc, label,
                                              baseline_props, pool) );
    }

  /* rsrc now points at the Baseline. We will checkout from the
     DAV:baseline-collection.  The revision we are checking out is in
     DAV:version-name */

  if (bc_url != NULL)
    {
      bc_url->data = apr_hash_get(rsrc->propset,
                                  SVN_RA_DAV__PROP_BASELINE_COLLECTION,
                                  APR_HASH_KEY_STRING);
      if (bc_url->data == NULL)
        {
          /* ### better error reporting... */

          /* ### need an SVN_ERR here */
          return svn_error_create(APR_EGENERAL, 0, NULL, pool,
                                  "DAV:baseline-collection was not present "
                                  "on the baseline resource.");
        }
      bc_url->len = strlen(bc_url->data);
    }

  if (latest_rev != NULL)
    {
      const char *vsn_name;

      vsn_name = apr_hash_get(rsrc->propset,
                              SVN_RA_DAV__PROP_VERSION_NAME,
                              APR_HASH_KEY_STRING);
      if (vsn_name == NULL)
        {
          /* ### better error reporting... */

          /* ### need an SVN_ERR here */
          return svn_error_create(APR_EGENERAL, 0, NULL, pool,
                                  "DAV:version-name was not present on the "
                                  "baseline resource.");
        }
      *latest_rev = atol(vsn_name);
    }

  return SVN_NO_ERROR;
}


/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
