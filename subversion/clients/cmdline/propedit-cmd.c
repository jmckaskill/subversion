/*
 * propedit-cmd.c -- Edit properties of files/dirs using $EDITOR
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

/* ==================================================================== */



/*** Includes. ***/

#include "svn_private_config.h"
#include "svn_cmdline.h"
#include "svn_wc.h"
#include "svn_pools.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_error.h"
#include "svn_utf.h"
#include "svn_subst.h"
#include "svn_private_config.h"
#include "cl.h"

#include "svn_private_config.h"


/*** Code. ***/

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__propedit (apr_getopt_t *os,
                  void *baton,
                  apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  const char *pname, *pname_utf8;
  apr_array_header_t *args, *targets;
  int i;

  /* Validate the input and get the property's name (and a UTF-8
     version of that name). */
  SVN_ERR (svn_opt_parse_num_args (&args, os, 1, pool));
  pname = ((const char **) (args->elts))[0];
  SVN_ERR (svn_utf_cstring_to_utf8 (&pname_utf8, pname, pool));

  /* Suck up all the remaining arguments into a targets array */
  SVN_ERR (svn_opt_args_to_target_array (&targets, os, 
                                         opt_state->targets,
                                         &(opt_state->start_revision),
                                         &(opt_state->end_revision),
                                         FALSE, pool));

  if (opt_state->revprop)  /* operate on a revprop */
    {
      svn_revnum_t rev;
      const char *URL, *target;
      svn_string_t *propval;
      const char *new_propval;
      const char *temp_dir;

      /* All property commands insist on a specific revision when
         operating on a revprop. */
      if (opt_state->start_revision.kind == svn_opt_revision_unspecified)
        return svn_cl__revprop_no_rev_error (pool);

      /* Else some revision was specified, so proceed. */

      /* Implicit "." is okay for revision properties; it just helps
         us find the right repository. */
      svn_opt_push_implicit_dot_target (targets, pool);

      /* Either we have a URL target, or an implicit wc-path ('.')
         which needs to be converted to a URL. */
      if (targets->nelts <= 0)
        return svn_error_create(SVN_ERR_CL_INSUFFICIENT_ARGS, NULL,
                                _("No URL target available"));
      target = ((const char **) (targets->elts))[0];
      SVN_ERR (svn_client_url_from_path (&URL, target, pool));  
      if (URL == NULL)
        return svn_error_create
          (SVN_ERR_UNVERSIONED_RESOURCE, NULL,
           _("Either a URL or versioned item is required"));

      /* Fetch the current property. */
      SVN_ERR (svn_client_revprop_get (pname_utf8, &propval,
                                       URL, &(opt_state->start_revision),
                                       &rev, ctx, pool));
      if (! propval)
        propval = svn_string_create ("", pool);
      
      /* Run the editor on a temporary file which contains the
         original property value... */
      SVN_ERR (svn_io_temp_dir (&temp_dir, pool));
      SVN_ERR (svn_cl__edit_externally (&new_propval, NULL,
                                        opt_state->editor_cmd, temp_dir,
                                        propval->data, "svn-prop",
                                        ctx->config, pool));
      
      /* ...and re-set the property's value accordingly. */
      if (new_propval)
        {
          propval->data = new_propval;
          propval->len = strlen (new_propval);

          /* Possibly clean up the new propval before giving it to
             svn_client_revprop_set. */
          if (svn_prop_needs_translation (pname_utf8))
            SVN_ERR (svn_subst_translate_string (&propval, propval,
                                                 opt_state->encoding, pool));
          else 
            if (opt_state->encoding)
              return svn_error_create 
                (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                 _("Bad encoding option: prop value not stored as UTF8"));
          
          SVN_ERR (svn_client_revprop_set (pname_utf8, propval,
                                           URL, &(opt_state->start_revision),
                                           &rev, opt_state->force, ctx, pool));

          SVN_ERR
            (svn_cmdline_printf
             (pool,
              _("Set new value for property '%s' on revision %ld\n"),
              pname_utf8, rev));
        }
      else
        {
          SVN_ERR (svn_cmdline_printf
                   (pool, _("No changes to property '%s' on revision %ld\n"),
                    pname_utf8, rev));
        }
    }
  else if (opt_state->start_revision.kind != svn_opt_revision_unspecified)
    {
      return svn_error_createf
        (SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
         _("Cannot specify revision for editing versioned property '%s'"),
         pname_utf8);
    }
  else  /* operate on a normal, versioned property (not a revprop) */
    {
      apr_pool_t *subpool = svn_pool_create (pool);

      /* The customary implicit dot rule has been prone to user error
       * here.  For example, Jon Trowbridge <trow@gnu.og> did
       * 
       *    $ svn propedit HACKING
       *
       * and then when he closed his editor, he was surprised to see
       *
       *    Set new value for property 'HACKING' on ''
       *
       * ...meaning that the property named 'HACKING' had been set on
       * the current working directory, with the value taken from the
       * editor.  So we don't do the implicit dot thing anymore; an
       * explicit target is always required when editing a versioned
       * property.
       */
      if (targets->nelts == 0)
        {
          return svn_error_create
            (SVN_ERR_CL_INSUFFICIENT_ARGS, NULL,
             _("Explicit target argument required"));
        }

      /* For each target, edit the property PNAME. */
      for (i = 0; i < targets->nelts; i++)
        {
          apr_hash_t *props;
          const char *target = ((const char **) (targets->elts))[i];
          svn_string_t *propval;
          const char *new_propval;
          const char *base_dir = target;
          const char *target_local;
          svn_wc_adm_access_t *adm_access;
          const svn_wc_entry_t *entry;
          
          svn_pool_clear (subpool);
          SVN_ERR (svn_cl__check_cancel (ctx->cancel_baton));
          if (svn_path_is_url (target))
            {
              /* ### If/when svn_client_propset() supports setting
                 properties remotely, this guard can go away.  Also,
                 when we do that, we'll have to pass a real auth baton
                 instead of NULL to svn_client_propget() below. */
              return svn_error_createf
                (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                 _("Editing property on non-local target '%s' "
                 "not yet supported"), target);
            }

          /* Fetch the current property. */
          SVN_ERR (svn_client_propget (&props, pname_utf8, target,
                                       &(opt_state->start_revision),
                                       FALSE,
                                       NULL,  /* ### pass ctx here */
                                       subpool));
          
          /* Get the property value. */
          propval = apr_hash_get (props, target, APR_HASH_KEY_STRING);
          if (! propval)
            propval = svn_string_create ("", subpool);
          
          /* Split the path if it is a file path. */
          SVN_ERR (svn_wc_adm_probe_open2 (&adm_access, NULL, target,
                                           FALSE, 0, subpool));
          SVN_ERR (svn_wc_entry (&entry, target, adm_access, FALSE, subpool));
          if (! entry)
            return svn_error_createf
              (SVN_ERR_ENTRY_NOT_FOUND, NULL, 
               _("'%s' does not appear to be a working copy path"), target);
          if (entry->kind == svn_node_file)
            svn_path_split (target, &base_dir, NULL, subpool);
          
          /* Run the editor on a temporary file which contains the
             original property value... */
          SVN_ERR (svn_cl__edit_externally (&new_propval, NULL,
                                            opt_state->editor_cmd,
                                            base_dir,
                                            propval->data,
                                            "svn-prop",
                                            ctx->config,
                                            subpool));
          
          target_local = svn_path_local_style (target, subpool);

          /* ...and re-set the property's value accordingly. */
          if (new_propval)
            {
              propval->data = new_propval;
              propval->len = strlen (new_propval);

              /* Possibly clean up the new propval before giving it to
                 svn_client_propset. */
              if (svn_prop_needs_translation (pname_utf8))
                SVN_ERR (svn_subst_translate_string (&propval, propval,
                                                     opt_state->encoding,
                                                     subpool));
              else 
                if (opt_state->encoding)
                  return svn_error_create 
                    (SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                     _("Bad encoding option: prop value not stored as UTF8"));
              
              SVN_ERR (svn_client_propset (pname_utf8, propval, target, 
                                           FALSE, subpool));
              SVN_ERR
                (svn_cmdline_printf
                 (subpool, _("Set new value for property '%s' on '%s'\n"),
                  pname_utf8, target_local));
            }
          else
            {
              SVN_ERR
                (svn_cmdline_printf
                 (subpool, _("No changes to property '%s' on '%s'\n"),
                  pname_utf8, target_local));
            }
        }
      svn_pool_destroy (subpool);
    }

  return SVN_NO_ERROR;
}
