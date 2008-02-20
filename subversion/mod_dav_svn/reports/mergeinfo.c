/*
 * mergeinfo.c :  routines for getting mergeinfo
 *
 * ====================================================================
 * Copyright (c) 2006 CollabNet.  All rights reserved.
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

#include <http_request.h>
#include <http_log.h>
#include <mod_dav.h>

#include "svn_pools.h"
#include "svn_repos.h"
#include "svn_xml.h"
#include "svn_path.h"
#include "svn_dav.h"
#include "private/svn_dav_protocol.h"

#include "../dav_svn.h"


dav_error *
dav_svn__get_mergeinfo_report(const dav_resource *resource,
                              const apr_xml_doc *doc,
                              ap_filter_t *output)
{
  svn_error_t *serr;
  apr_status_t apr_err;
  dav_error *derr = NULL;
  apr_xml_elem *child;
  apr_hash_t *mergeinfo;
  svn_boolean_t include_descendants = FALSE;
  dav_svn__authz_read_baton arb;
  const dav_svn_repos *repos = resource->info->repos;
  const char *action;
  int ns;
  apr_bucket_brigade *bb;
  svn_boolean_t sent_anything = FALSE;

  /* These get determined from the request document. */
  svn_revnum_t rev = SVN_INVALID_REVNUM;
  /* By default look for explicit mergeinfo only. */
  svn_mergeinfo_inheritance_t inherit = svn_mergeinfo_explicit;
  apr_array_header_t *paths
    = apr_array_make(resource->pool, 0, sizeof(const char *));
  /* for high-level logging */
  svn_stringbuf_t *space_separated_paths =
    svn_stringbuf_create("", resource->pool);

  /* Sanity check. */
  ns = dav_svn__find_ns(doc->namespaces, SVN_XML_NAMESPACE);
  if (ns == -1)
    {
      return dav_svn__new_error_tag(resource->pool, HTTP_BAD_REQUEST, 0,
                                    "The request does not contain the 'svn:' "
                                    "namespace, so it is not going to have "
                                    "certain required elements.",
                                    SVN_DAV_ERROR_NAMESPACE,
                                    SVN_DAV_ERROR_TAG);
    }

  for (child = doc->root->first_child; child != NULL; child = child->next)
    {
      /* if this element isn't one of ours, then skip it */
      if (child->ns != ns)
        continue;

      if (strcmp(child->name, SVN_DAV__REVISION) == 0)
        rev = SVN_STR_TO_REV(dav_xml_get_cdata(child, resource->pool, 1));
      else if (strcmp(child->name, SVN_DAV__INHERIT) == 0)
        {
          inherit = svn_inheritance_from_word(
            dav_xml_get_cdata(child, resource->pool, 1));
        }
      else if (strcmp(child->name, SVN_DAV__PATH) == 0)
        {
          const char *target;
          const char *rel_path = dav_xml_get_cdata(child, resource->pool, 0);
          if ((derr = dav_svn__test_canonical(rel_path, resource->pool)))
            return derr;
          target = svn_path_join(resource->info->repos_path, rel_path,
                                 resource->pool);
          (*((const char **)(apr_array_push(paths)))) = target;
          /* Gather a formatted list of paths to include in our
             operational logging. */
          if (space_separated_paths->len > 1)
            svn_stringbuf_appendcstr(space_separated_paths, " ");
          svn_stringbuf_appendcstr(space_separated_paths,
                                   svn_path_uri_encode(target,
                                                       resource->pool));
        }
      else if (strcmp(child->name, SVN_DAV__INCLUDE_DESCENDANTS) == 0)
        {
          const char *word = dav_xml_get_cdata(child, resource->pool, 1);
          if (strcmp(word, "yes") == 0)
            include_descendants = TRUE;
          /* Else the client isn't supposed to send anyway, so just
             leave it false. */
        }
      /* else unknown element; skip it */
    }

  /* Build authz read baton */
  arb.r = resource->info->r;
  arb.repos = resource->info->repos;

  /* Build mergeinfo brigade */
  bb = apr_brigade_create(resource->pool, output->c->bucket_alloc);

  serr = svn_repos_fs_get_mergeinfo(&mergeinfo, repos->repos, paths, rev,
                                    inherit, include_descendants,
                                    dav_svn__authz_read_func(&arb),
                                    &arb, resource->pool);
  if (serr)
    {
      derr = dav_svn__convert_err(serr, HTTP_BAD_REQUEST, serr->message,
                                  resource->pool);
      goto cleanup;
    }

  /* Ideally, dav_svn__send_xml() would set a flag in bb (or rather,
     in r->sent_bodyct, see dav_method_report()), and ap_fflush()
     would not set that flag unless it actually sent something.  But
     we are condemned to live in another universe, so we must keep
     track ourselves of whether we've sent anything or not.  See the
     long comment after the 'cleanup' label for more details. */
  sent_anything = TRUE;
  serr = dav_svn__send_xml(bb, output,
                           DAV_XML_HEADER DEBUG_CR
                           "<S:" SVN_DAV__MERGEINFO_REPORT " "
                           "xmlns:S=\"" SVN_XML_NAMESPACE "\" "
                           "xmlns:D=\"DAV:\">" DEBUG_CR);
  if (serr)
    {
      derr = dav_svn__convert_err(serr, HTTP_BAD_REQUEST, serr->message,
                                  resource->pool);
      goto cleanup;
    }

  if (mergeinfo != NULL && apr_hash_count (mergeinfo) > 0)
    {
      const void *key;
      void *value;
      apr_hash_index_t *hi;

      for (hi = apr_hash_first(resource->pool, mergeinfo); hi;
           hi = apr_hash_next(hi))
        {
          const char *path, *info;
          const char itemformat[] = "<S:" SVN_DAV__MERGEINFO_ITEM ">"
            DEBUG_CR
            "<S:" SVN_DAV__MERGEINFO_PATH ">%s</S:" SVN_DAV__MERGEINFO_PATH ">"
            DEBUG_CR
            "<S:" SVN_DAV__MERGEINFO_INFO ">%s</S:" SVN_DAV__MERGEINFO_INFO ">"
            DEBUG_CR
            "</S:" SVN_DAV__MERGEINFO_ITEM ">";

          apr_hash_this(hi, &key, NULL, &value);
          path = (const char *)key + strlen(resource->info->repos_path);
          info = value;
          serr = dav_svn__send_xml(bb, output, itemformat,
                                   apr_xml_quote_string(resource->pool,
                                                        path, 0),
                                   apr_xml_quote_string(resource->pool,
                                                        info, 0));
          if (serr)
            {
              derr = dav_svn__convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                          "Error ending REPORT response.",
                                          resource->pool);
              goto cleanup;
            }
        }
    }

  if ((serr = dav_svn__send_xml(bb, output,
                                "</S:" SVN_DAV__MERGEINFO_REPORT ">"
                                DEBUG_CR)))
    {
      derr = dav_svn__convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                  "Error ending REPORT response.",
                                  resource->pool);
      goto cleanup;
    }

 cleanup:

  /* We've detected a 'high level' svn action to log. */
  action = apr_psprintf(resource->pool, "get-mergeinfo (%s) %s",
                        space_separated_paths->data,
                        svn_inheritance_to_word(inherit));
  dav_svn__operational_log(resource->info, action);

  /* We don't flush the brigade unless there's something in it to
     flush; that way, if we jumped to 'cleanup' before sending
     anything, we will be able to report derr accurately to the
     client.

     To understand this, see mod_dav.c:dav_method_report(): as long as
     it doesn't think we've sent anything to the client, it'll send
     the real error, which is what we'd prefer.  This situation is
     described in httpd-2.2.6/modules/dav/main/mod_dav.c, line 4066,
     in the comment in dav_method_report() that says:

        If an error occurred during the report delivery, there's
        basically nothing we can do but abort the connection and
        log an error.  This is one of the limitations of HTTP; it
        needs to "know" the entire status of the response before
        generating it, which is just impossible in these streamy
        response situations.

     In other words, flushing the brigade here would cause
     r->sent_bodyct (see dav_method_report()) to become non-zero,
     *even* if we hadn't tried to send any data to the brigade yet.
     So we don't flush unless data was actually sent. */
  apr_err = 0;
  if (sent_anything)
    apr_err = ap_fflush(output, bb);
      
  if (apr_err && !derr)
    derr = dav_svn__convert_err(svn_error_create(apr_err, 0, NULL),
                                HTTP_INTERNAL_SERVER_ERROR,
                                "Error flushing brigade.",
                                resource->pool);
  return derr;
}
