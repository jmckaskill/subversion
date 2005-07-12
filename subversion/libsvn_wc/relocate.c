/*
 * relocate.c: do wc repos relocation
 *
 * ====================================================================
 * Copyright (c) 2002-2004 CollabNet.  All rights reserved.
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



#include "svn_wc.h"
#include "svn_error.h"
#include "svn_path.h"

#include "wc.h"
#include "entries.h"
#include "props.h"

#include "svn_private_config.h"


/* Relocate the main URL and the copyfrom URL for ENTRY by changing FROM to
 * TO.  ADM_ACCESS is the access baton for ENTRY.  If DO_SYNC is set then
 * the new entry will be written to disk immediately, otherwise only the
 * entries cache will be affected.  Calls VALIDATOR passing VALIDATOR_BATON
 * to validate new URLs.
 */
static svn_error_t *
relocate_entry (svn_wc_adm_access_t *adm_access,
                const svn_wc_entry_t *entry,
                const char *from,
                const char *to,
                svn_wc_relocation_validator_t validator,
                void *validator_baton,
                svn_boolean_t do_sync,
                apr_pool_t *pool)
{
  svn_wc_entry_t entry2;
  apr_uint32_t flags = 0;
  apr_size_t from_len = strlen (from);

  if (entry->repos)
    {
      /* We can't relocate beyond the repository root.  Do no URL rewriting
         in this case. */
      if (from_len > strlen (entry->repos))
        return SVN_NO_ERROR;
      if (strncmp (from, entry->repos, from_len) == 0)
        {
          entry2.repos = apr_psprintf (svn_wc_adm_access_pool (adm_access),
                                       "%s%s", to, entry->repos + from_len);
          flags |= SVN_WC__ENTRY_MODIFY_REPOS;
        }
    }

  if (entry->url && ! strncmp (entry->url, from, from_len))
    {
      entry2.url = apr_psprintf (svn_wc_adm_access_pool (adm_access),
                                 "%s%s", to, entry->url + from_len);
      if (entry->uuid)
        SVN_ERR (validator (validator_baton, entry->uuid, entry2.url));
      flags |= SVN_WC__ENTRY_MODIFY_URL;
    }

  if (entry->copyfrom_url && ! strncmp (entry->copyfrom_url, from, from_len))
    {
      entry2.copyfrom_url = apr_psprintf (svn_wc_adm_access_pool (adm_access),
                                          "%s%s", to,
                                          entry->copyfrom_url + from_len);
      if (entry->uuid)
        SVN_ERR (validator (validator_baton, entry->uuid, entry2.copyfrom_url));
      flags |= SVN_WC__ENTRY_MODIFY_COPYFROM_URL;
    }

  if (flags)
    SVN_ERR (svn_wc__entry_modify (adm_access, entry->name,
                                   &entry2, flags, do_sync, pool));
  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_relocate (const char *path,
                 svn_wc_adm_access_t *adm_access,
                 const char *from,
                 const char *to,
                 svn_boolean_t recurse,
                 svn_wc_relocation_validator_t validator,
                 void *validator_baton,
                 apr_pool_t *pool)
{
  apr_hash_t *entries;
  apr_hash_index_t *hi;
  const svn_wc_entry_t *entry;

  SVN_ERR (svn_wc_entry (&entry, path, adm_access, TRUE, pool));
  if (! entry)
    return svn_error_create (SVN_ERR_ENTRY_NOT_FOUND, NULL, NULL);

  if (entry->kind == svn_node_file)
    {
      SVN_ERR (relocate_entry (adm_access, entry, from, to,
                               validator, validator_baton, TRUE /* sync */,
                               pool));
      return SVN_NO_ERROR;
    }

  /* Relocate THIS_DIR first, in order to pre-validate the relocated URL
     of all of the other entries.  This is technically cheating because
     it relies on knowledge of the libsvn_client implementation, but it
     significantly cuts down on the number of expensive validations the
     validator has to do.  ### Should svn_wc.h document the ordering? */
  SVN_ERR (svn_wc_entries_read (&entries, adm_access, TRUE, pool));
  entry = apr_hash_get (entries, SVN_WC_ENTRY_THIS_DIR, APR_HASH_KEY_STRING);
  SVN_ERR (relocate_entry (adm_access, entry, from, to,
                           validator, validator_baton, FALSE, pool));

  for (hi = apr_hash_first (pool, entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;

      apr_hash_this (hi, &key, NULL, &val);
      entry = val;

      if (strcmp (key, SVN_WC_ENTRY_THIS_DIR) == 0)
        continue;

      if (recurse && (entry->kind == svn_node_dir))
        {
          svn_wc_adm_access_t *subdir_access;
          const char *subdir = svn_path_join (path, key, pool);
          if (svn_wc__adm_missing (adm_access, subdir))
            continue;
          SVN_ERR (svn_wc_adm_retrieve (&subdir_access, adm_access, 
                                        subdir, pool));
          SVN_ERR (svn_wc_relocate (subdir, subdir_access, from, to,
                                    recurse, validator, 
                                    validator_baton, pool));
        }
      SVN_ERR (relocate_entry (adm_access, entry, from, to,
                               validator, validator_baton, FALSE, pool));
    }

  SVN_ERR (svn_wc__remove_wcprops (adm_access, FALSE, pool));
  SVN_ERR (svn_wc__entries_write (entries, adm_access, pool));
  return SVN_NO_ERROR;
}

