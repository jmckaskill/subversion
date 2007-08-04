/*
 * props.c :  routines dealing with properties in the working copy
 *
 * ====================================================================
 * Copyright (c) 2000-2007 CollabNet.  All rights reserved.
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



#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <apr_pools.h>
#include <apr_hash.h>
#include <apr_tables.h>
#include <apr_file_io.h>
#include <apr_strings.h>
#include <apr_general.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_pools.h"
#include "svn_path.h"
#include "svn_xml.h"
#include "svn_error.h"
#include "svn_props.h"
#include "svn_io.h"
#include "svn_hash.h"
#include "svn_mergeinfo.h"
#include "svn_wc.h"
#include "svn_utf.h"

#include "private/svn_wc_private.h"
#include "private/svn_mergeinfo_private.h"

#include "wc.h"
#include "log.h"
#include "adm_files.h"
#include "entries.h"
#include "props.h"
#include "translate.h"
#include "questions.h"
#include "lock.h"

#include "svn_private_config.h"

/*---------------------------------------------------------------------*/

/*** Deducing local changes to properties ***/

/*---------------------------------------------------------------------*/

/*** Reading/writing property hashes from disk ***/

/* The real functionality here is part of libsvn_subr, in hashdump.c.
   But these are convenience routines for use in libsvn_wc. */



/* If PROPFILE_PATH exists (and is a file), assume it's full of
   properties and load this file into HASH.  Otherwise, leave HASH
   untouched.  */
svn_error_t *
svn_wc__load_prop_file(const char *propfile_path,
                       apr_hash_t *hash,
                       apr_pool_t *pool)
{
  svn_error_t *err;

  apr_file_t *propfile = NULL;

  err = svn_io_file_open(&propfile, propfile_path,
                         APR_READ | APR_BUFFERED, APR_OS_DEFAULT,
                         pool);

  if (err && (APR_STATUS_IS_ENOENT(err->apr_err)
              || APR_STATUS_IS_ENOTDIR(err->apr_err)))
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }

  SVN_ERR(err);

  SVN_ERR_W(svn_hash_read(hash, propfile, pool),
            apr_psprintf(pool, _("Can't parse '%s'"),
                         svn_path_local_style(propfile_path, pool)));

  SVN_ERR(svn_io_file_close(propfile, pool));

  return SVN_NO_ERROR;
}



/* Given a HASH full of property name/values, write them to a file
   located at PROPFILE_PATH.  If WRITE_EMPTY is TRUE then writing
   an emtpy property hash will result in an actual empty property
   file on disk, otherwise an empty hash will result in no file
   being written at all. */
svn_error_t *
svn_wc__save_prop_file(const char *propfile_path,
                       apr_hash_t *hash,
                       svn_boolean_t write_empty,
                       apr_pool_t *pool)
{
  apr_file_t *prop_tmp;

  SVN_ERR(svn_io_file_open(&prop_tmp, propfile_path,
                           (APR_WRITE | APR_CREATE | APR_TRUNCATE
                            | APR_BUFFERED), 
                           APR_OS_DEFAULT, pool));

  if (apr_hash_count(hash) != 0 || write_empty)
    SVN_ERR_W(svn_hash_write(hash, prop_tmp, pool),
              apr_psprintf(pool, 
                           _("Can't write property hash to '%s'"),
                           svn_path_local_style(propfile_path, pool)));

  SVN_ERR(svn_io_file_close(prop_tmp, pool));

  return SVN_NO_ERROR;
}


/*---------------------------------------------------------------------*/

/*** Misc ***/

/* Opens reject temporary file for FULL_PATH. */
static svn_error_t *
open_reject_tmp_file(apr_file_t **fp, const char **reject_tmp_path,
                     const char *full_path,
                     svn_wc_adm_access_t *adm_access,
                     svn_boolean_t is_dir, apr_pool_t *pool)
{
  const char *tmp_path, *tmp_name;

  /* Get path to /temporary/ local prop file */
  SVN_ERR(svn_wc__prop_path(&tmp_path, full_path,
                            is_dir ? svn_node_dir : svn_node_file,
                            TRUE, pool));

  /* Reserve a .prej file based on it.  */
  SVN_ERR(svn_io_open_unique_file2(fp, reject_tmp_path, tmp_path,
                                   SVN_WC__PROP_REJ_EXT,
                                   svn_io_file_del_none, pool));

  /* reject_tmp_path is an absolute path at this point,
     but that's no good for us.  We need to convert this
     path to a *relative* path to use in the logfile. */
  tmp_name = svn_path_basename(*reject_tmp_path, pool);

  if (is_dir)
    {
      /* Dealing with directory "path" */
      *reject_tmp_path = svn_wc__adm_path("", TRUE, /* use tmp */ pool,
                                          tmp_name, NULL);
    }
  else
    {
      /* Dealing with file "path/name" */
      *reject_tmp_path = svn_wc__adm_path("", TRUE, pool, SVN_WC__ADM_PROPS,
                                          tmp_name, NULL);
    }

  return SVN_NO_ERROR;
}


/* Assuming FP is a filehandle already open for appending, write
   CONFLICT_DESCRIPTION to file, plus a trailing EOL sequence. */
static svn_error_t *
append_prop_conflict(apr_file_t *fp,
                     const svn_string_t *conflict_description,
                     apr_pool_t *pool)
{
  /* TODO:  someday, perhaps prefix each conflict_description with a
     timestamp or something? */
  apr_size_t written;
  const char *native_text =
    svn_utf_cstring_from_utf8_fuzzy(conflict_description->data, pool);
  SVN_ERR(svn_io_file_write_full(fp, native_text, strlen(native_text),
                                 &written, pool));

  native_text = svn_utf_cstring_from_utf8_fuzzy(APR_EOL_STR, pool);
  SVN_ERR(svn_io_file_write_full(fp, native_text, strlen(native_text),
                                 &written, pool));

  return SVN_NO_ERROR;
}


/* Look up the entry NAME within ADM_ACCESS and see if it has a `current'
   reject file describing a state of conflict.  Set *REJECT_FILE to the
   name of that file, or to NULL if no such file exists. */
static svn_error_t *
get_existing_prop_reject_file(const char **reject_file,
                              svn_wc_adm_access_t *adm_access,
                              const char *name,
                              apr_pool_t *pool)
{
  apr_hash_t *entries;
  const svn_wc_entry_t *the_entry;

  SVN_ERR(svn_wc_entries_read(&entries, adm_access, FALSE, pool));
  the_entry = apr_hash_get(entries, name, APR_HASH_KEY_STRING);

  if (! the_entry)
    return svn_error_createf
      (SVN_ERR_ENTRY_NOT_FOUND, NULL,
       _("Can't find entry '%s' in '%s'"),
       name,
       svn_path_local_style(svn_wc_adm_access_path(adm_access), pool));

  *reject_file = the_entry->prejfile 
    ? apr_pstrdup(pool, the_entry->prejfile)
                 : NULL;
  return SVN_NO_ERROR;
}

/*---------------------------------------------------------------------*/


/* Build a space separated list of properties that are contained in
   the hash PROPS and which we want to cache.
   The string is allocated in POOL. */
static const char *
build_present_props(apr_hash_t *props, apr_pool_t *pool)
{
  apr_array_header_t *cachable;
  svn_stringbuf_t *present_props = svn_stringbuf_create("", pool);
  int i;
  
  if (apr_hash_count(props) == 0)
    return present_props->data;

  cachable = svn_cstring_split(SVN_WC__CACHABLE_PROPS, " ", TRUE, pool);
  for (i = 0; i < cachable->nelts; i++)
    {
      const char *proptolookfor = APR_ARRAY_IDX(cachable, i, 
                                                const char *);

      if (apr_hash_get(props, proptolookfor, APR_HASH_KEY_STRING) != NULL)
        {
          svn_stringbuf_appendcstr(present_props, proptolookfor);
          svn_stringbuf_appendcstr(present_props, " ");
        }
    }

  /* Avoid returning a string with a trailing space. */
  svn_stringbuf_chop(present_props, 1);
  return present_props->data;
}

/*** Loading regular properties. ***/
svn_error_t *
svn_wc__load_props(apr_hash_t **base_props_p,
                   apr_hash_t **props_p,
                   svn_wc_adm_access_t *adm_access,
                   const char *name,
                   apr_pool_t *pool)
{
  const char *full_path;
  const char *access_path = svn_wc_adm_access_path(adm_access);
  svn_node_kind_t kind;
  svn_boolean_t has_propcaching =
    svn_wc__adm_wc_format(adm_access) > SVN_WC__NO_PROPCACHING_VERSION;
  const svn_wc_entry_t *entry;
  apr_hash_t *base_props = NULL; /* Silence uninitialized warning. */

  if (strcmp(name, SVN_WC_ENTRY_THIS_DIR) == 0)
    {
      full_path = access_path;
      kind = svn_node_dir;
    }
  else
    {
      full_path = svn_path_join(access_path, name, pool);
      kind = svn_node_file;
    }

  SVN_ERR(svn_wc_entry(&entry, full_path, adm_access, FALSE, pool));
  /* If there is no entry, we just return empty hashes, since the
     property merging can use this function when there is no entry. */
  if (! entry)
    {
      if (base_props_p)
        *base_props_p = apr_hash_make(pool);
      if (props_p)
        *props_p = apr_hash_make(pool);
      return SVN_NO_ERROR;
    }

  /* We will need the base props if the user requested them, OR,
     our WC has prop caching, the user requested working props and there are no
     prop mods. */
  if (base_props_p
      || (has_propcaching && ! entry->has_prop_mods && entry->has_props))
    {
      const char *prop_base_path;

      SVN_ERR(svn_wc__prop_base_path(&prop_base_path, full_path,
                                     kind, FALSE, pool));
      base_props = apr_hash_make(pool);
      SVN_ERR(svn_wc__load_prop_file(prop_base_path, base_props, pool));

      if (base_props_p)
        *base_props_p = base_props;
    }

  if (props_p)
    {
      if (has_propcaching && ! entry->has_prop_mods && entry->has_props)
        *props_p = apr_hash_copy(pool, base_props);
      else if (! has_propcaching || entry->has_props)
        {
          const char *prop_path;

          SVN_ERR(svn_wc__prop_path(&prop_path, full_path,
                                    kind, FALSE, pool));
          *props_p = apr_hash_make(pool);
          SVN_ERR(svn_wc__load_prop_file(prop_path, *props_p, pool));
        }
      else
        *props_p = apr_hash_make(pool);
    }
      
  return SVN_NO_ERROR;
}


/*---------------------------------------------------------------------*/

/*** Installing new properties. ***/
svn_error_t *
svn_wc__install_props(svn_stringbuf_t **log_accum,
                      svn_wc_adm_access_t *adm_access,
                      const char *name,
                      apr_hash_t *base_props,
                      apr_hash_t *working_props,
                      svn_boolean_t write_base_props,
                      apr_pool_t *pool)
{
  const char *working_propfile_path, *working_prop_tmp_path;
  const char *tmp_props, *real_props;      
  const char *full_path;
  const char *access_path = svn_wc_adm_access_path(adm_access);
  int access_len = strlen(access_path);
  apr_array_header_t *prop_diffs;
  const svn_wc_entry_t *entry;
  svn_wc_entry_t tmp_entry;
  svn_node_kind_t kind;
  svn_boolean_t has_propcaching =
    svn_wc__adm_wc_format(adm_access) > SVN_WC__NO_PROPCACHING_VERSION;

  /* Non-empty path without trailing slash need an extra slash removed */
  if (access_len != 0 && access_path[access_len - 1] != '/')
    access_len++;

  if (strcmp(name, SVN_WC_ENTRY_THIS_DIR) == 0)
    {
      kind = svn_node_dir;
      full_path = access_path;
    }
  else
    {
      kind = svn_node_file;
      full_path = svn_path_join(access_path, name, pool);
    }
  /* Check if the props are modified. */
  SVN_ERR(svn_prop_diffs(&prop_diffs, working_props, base_props, pool));
  tmp_entry.has_prop_mods = (prop_diffs->nelts > 0);
  tmp_entry.has_props = (apr_hash_count(working_props) > 0);
  tmp_entry.cachable_props = SVN_WC__CACHABLE_PROPS;
  tmp_entry.present_props = build_present_props(working_props, pool);

  SVN_ERR(svn_wc__loggy_entry_modify(log_accum, adm_access, name, &tmp_entry,
                                     SVN_WC__ENTRY_MODIFY_HAS_PROPS
                                     | SVN_WC__ENTRY_MODIFY_HAS_PROP_MODS 
                                     | SVN_WC__ENTRY_MODIFY_CACHABLE_PROPS
                                     | SVN_WC__ENTRY_MODIFY_PRESENT_PROPS,
                                     pool));

  if (has_propcaching)
    SVN_ERR(svn_wc_entry(&entry, full_path, adm_access, FALSE, pool));
  else
    entry = NULL;

  /* Write our property hashes into temporary files.  Notice that the
     paths computed are ABSOLUTE pathnames, which is what our disk
     routines require. */
  SVN_ERR(svn_wc__prop_path(&working_propfile_path, full_path,
                            kind, FALSE, pool)); 
  real_props = apr_pstrdup(pool, working_propfile_path + access_len);
  if (tmp_entry.has_prop_mods)
    {
      SVN_ERR(svn_wc__prop_path(&working_prop_tmp_path, full_path,
                                kind, TRUE, pool));

      /* Write the working prop hash to path/.svn/tmp/props/name or
         path/.svn/tmp/dir-props */
      SVN_ERR(svn_wc__save_prop_file(working_prop_tmp_path, working_props,
                                     FALSE, pool));
  
      /* Compute pathnames for the "mv" log entries.  Notice that these
         paths are RELATIVE pathnames (each beginning with ".svn/"), so
         that each .svn subdir remains separable when executing run_log().  */
      tmp_props = apr_pstrdup(pool, working_prop_tmp_path + access_len);
  
      /* Write log entry to move working tmp copy to real working area. */
      SVN_ERR(svn_wc__loggy_move(log_accum, NULL,
                                 adm_access, tmp_props, real_props,
                                 FALSE, pool));

      /* Make props read-only */
      SVN_ERR(svn_wc__loggy_set_readonly(log_accum, adm_access,
                                         real_props, pool));
    }
  else
    {
      /* No property modifications, remove the file instead. */
      if (! has_propcaching || (entry && entry->has_prop_mods))
        SVN_ERR(svn_wc__loggy_remove(log_accum, adm_access, real_props, pool));
    }

  /* Repeat the above steps for the base properties if required. */
  if (write_base_props)
    {
      const char *base_propfile_path, *base_prop_tmp_path;
      const char *tmp_prop_base, *real_prop_base;

      SVN_ERR(svn_wc__prop_base_path(&base_propfile_path, full_path,
                                     kind, FALSE, pool));
      real_prop_base = apr_pstrdup(pool, base_propfile_path + access_len);

      if (apr_hash_count(base_props) > 0)
        {
          SVN_ERR(svn_wc__prop_base_path(&base_prop_tmp_path, full_path,
                                         kind, TRUE, pool));

          SVN_ERR(svn_wc__save_prop_file(base_prop_tmp_path, base_props,
                                         FALSE, pool));

          tmp_prop_base = apr_pstrdup(pool, base_prop_tmp_path + access_len);

          SVN_ERR(svn_wc__loggy_move(log_accum, NULL, adm_access,
                                     tmp_prop_base, real_prop_base,
                                     FALSE, pool));

          SVN_ERR(svn_wc__loggy_set_readonly(log_accum, adm_access,
                                             real_prop_base, pool));
        }
      else
        {
          if (! has_propcaching || (entry && entry->has_props))
            SVN_ERR(svn_wc__loggy_remove(log_accum, adm_access, real_prop_base,
                                         pool));
        }
    }

  return SVN_NO_ERROR;
}

/*---------------------------------------------------------------------*/

/*** Merging propchanges into the working copy ***/


/* Parse FROM_PROP_VAL and TO_PROP_VAL into mergeinfo hashes, and
   calculate the deltas between them. */
static svn_error_t *
diff_mergeinfo_props(apr_hash_t **deleted, apr_hash_t **added,
                     const svn_string_t *from_prop_val,
                     const svn_string_t *to_prop_val, apr_pool_t *pool)
{
  if (svn_string_compare(from_prop_val, to_prop_val))
    {
      /* Don't bothering parsing identical mergeinfo. */
      *deleted = apr_hash_make(pool);
      *added = apr_hash_make(pool);
    }
  else
    {
      apr_hash_t *from, *to;
      SVN_ERR(svn_mergeinfo_parse(&from, from_prop_val->data, pool));
      SVN_ERR(svn_mergeinfo_parse(&to, to_prop_val->data, pool));
      SVN_ERR(svn_mergeinfo_diff(deleted, added, from, to, pool));
    }
  return SVN_NO_ERROR;
}

/* Parse the mergeinfo from PROP_VAL1 and PROP_VAL2, combine it, then
   reconstitute it into *OUTPUT.  Call when the WC's mergeinfo has
   been modified to combine it with incoming mergeinfo from the
   repos. */
static svn_error_t *
combine_mergeinfo_props(const svn_string_t **output,
                        const svn_string_t *prop_val1,
                        const svn_string_t *prop_val2,
                        apr_pool_t *pool)
{
  apr_hash_t *mergeinfo1, *mergeinfo2;
  SVN_ERR(svn_mergeinfo_parse(&mergeinfo1, prop_val1->data, pool));
  SVN_ERR(svn_mergeinfo_parse(&mergeinfo2, prop_val2->data, pool));
  SVN_ERR(svn_mergeinfo_merge(&mergeinfo1, mergeinfo2, pool));
  SVN_ERR(svn_mergeinfo__to_string((svn_string_t **) output,
                                   mergeinfo1, pool));
  return SVN_NO_ERROR;
}

/* Perform a 3-way merge operation on mergeinfo.  FROM_PROP_VAL is
   the "base" property value, WORKING_PROP_VAL is the current value,
   and TO_PROP_VAL is the new value. */
static svn_error_t *
combine_forked_mergeinfo_props(const svn_string_t **output,
                               const svn_string_t *from_prop_val,
                               const svn_string_t *working_prop_val,
                               const svn_string_t *to_prop_val,
                               apr_pool_t *pool)
{
  apr_hash_t *from_mergeinfo, *l_deleted, *l_added, *r_deleted, *r_added;

  /* ### OPTIMIZE: Use from_mergeinfo when diff'ing. */
  SVN_ERR(diff_mergeinfo_props(&l_deleted, &l_added, from_prop_val,
                               working_prop_val, pool));
  SVN_ERR(diff_mergeinfo_props(&r_deleted, &r_added, from_prop_val,
                               to_prop_val, pool));
  SVN_ERR(svn_mergeinfo_merge(&l_deleted, r_deleted, pool));
  SVN_ERR(svn_mergeinfo_merge(&l_added, r_added, pool));

  /* Apply the combined deltas to the base. */
  SVN_ERR(svn_mergeinfo_parse(&from_mergeinfo, from_prop_val->data, pool));
  SVN_ERR(svn_mergeinfo_merge(&from_mergeinfo, l_added, pool));
  SVN_ERR(svn_mergeinfo_remove(&from_mergeinfo, l_deleted,
                               from_mergeinfo, pool));

  return svn_mergeinfo__to_string((svn_string_t **) output, from_mergeinfo,
                                  pool);
}


svn_error_t *
svn_wc_merge_props(svn_wc_notify_state_t *state,
                   const char *path,
                   svn_wc_adm_access_t *adm_access,
                   apr_hash_t *baseprops,
                   const apr_array_header_t *propchanges,
                   svn_boolean_t base_merge,
                   svn_boolean_t dry_run,
                   apr_pool_t *pool)
{
  const svn_wc_entry_t *entry;
  const char *parent, *base_name;
  svn_stringbuf_t *log_accum;

  /* IMPORTANT: svn_wc_merge_prop_diffs relies on the fact that baseprops
     may be NULL. */

  SVN_ERR(svn_wc__entry_versioned(&entry, path, adm_access, FALSE, pool));

  /* Notice that we're not using svn_path_split_if_file(), because
     that looks at the actual working file.  Its existence shouldn't
     matter, so we're looking at entry->kind instead. */
  switch (entry->kind)
    {
    case svn_node_dir:
      parent = path;
      base_name = NULL;
      break;
    case svn_node_file:
      svn_path_split(path, &parent, &base_name, pool);
      break;
    default:
      return SVN_NO_ERROR; /* ### svn_node_none or svn_node_unknown */
    }

  if (! dry_run)
    log_accum = svn_stringbuf_create("", pool);
  
  /* Note that while this routine does the "real" work, it's only
     prepping tempfiles and writing log commands.  */
  SVN_ERR(svn_wc__merge_props(state, adm_access, base_name, baseprops,
                              propchanges, base_merge, dry_run,
                              pool, &log_accum));

  if (! dry_run)
    {
      SVN_ERR(svn_wc__write_log(adm_access, 0, log_accum, pool));
      SVN_ERR(svn_wc__run_log(adm_access, NULL, pool));
    }

  return SVN_NO_ERROR;
}



/* Set the value of *STATE to NEW_VALUE if STATE is not NULL
 * and NEW_VALUE is a higer order value than *STATE's current value
 * using this ordering (lower order first):
 *
 * - unknown, unchanged, inapplicable
 * - changed
 * - merged
 * - missing
 * - obstructed
 * - conflicted
 *
 */
static void
set_prop_merge_state(svn_wc_notify_state_t *state,
                     svn_wc_notify_state_t new_value)
{
  static char ordering[] =
    { svn_wc_notify_state_unknown,
      svn_wc_notify_state_unchanged,
      svn_wc_notify_state_inapplicable,
      svn_wc_notify_state_changed,
      svn_wc_notify_state_merged,
      svn_wc_notify_state_obstructed,
      svn_wc_notify_state_conflicted };
  int state_pos, i;

  if (! state)
    return;

  /* Find *STATE in our ordering */
  for (i = 0; i < sizeof(ordering); i++)
    {
      if (*state == ordering[i])
        {
          state_pos = i;
          break;
        }
    }

  /* Find NEW_VALUE in our ordering
   * We don't need to look further than where we found *STATE though:
   * If we find our value, it's order is too low.
   * If we don't find it, we'll want to set it, no matter its order.
   */

  for (i = 0; i <= state_pos; i++)
    {
      if (new_value == ordering[i])
        return;
    }

  *state = new_value;
}

/* Add the property with name PROPNAME to the set of WORKING_PROPS,
 * setting *STATE or *CONFLICT according to merge outcomes.
 *
 * *STATE is an input and output parameter, its value is to be
 * set using set_merge_prop_state().
 *
 * BASE_VAL contains the working copy base property value
 *
 * NEW_VAL contains the value to be set
 */
static svn_error_t *
apply_single_prop_add(svn_wc_notify_state_t *state,
                      apr_hash_t *working_props,
                      svn_string_t **conflict,
                      const char *propname,
                      const svn_string_t *base_val,
                      const svn_string_t *new_val,
                      apr_pool_t *pool)

{
  svn_string_t *working_val
    = apr_hash_get(working_props, propname, APR_HASH_KEY_STRING);

  if (base_val)
    {
       if (working_val)
         {
           if (svn_string_compare(working_val, new_val))
             set_prop_merge_state(state, svn_wc_notify_state_merged);
           else
             *conflict = svn_string_createf
               (pool, _("Trying to create property '%s' with value '%s',\n"
                        "but it already exists."),
                propname, new_val->data);
         }
       else
          *conflict = svn_string_createf
            (pool, _("Trying to create property '%s' with value '%s',\n"
                     "but it has been locally deleted."),
             propname, new_val->data);
    }
  else if (working_val)
    {
      /* the property already exists in working_props... */

      if (svn_string_compare(working_val, new_val))
        /* The value we want is already there, so it's a merge. */
        set_prop_merge_state(state, svn_wc_notify_state_merged);

      else
        {
          /* The WC difference doesn't match the new value.
           We only merge merge info, conflicts for other props */
          if (strcmp(propname, SVN_PROP_MERGE_INFO) == 0)
            {
              SVN_ERR(combine_mergeinfo_props(&new_val, working_val,
                                              new_val, pool));
              apr_hash_set(working_props, propname,
                           APR_HASH_KEY_STRING, new_val);
              set_prop_merge_state(state, svn_wc_notify_state_merged);
            }
          else
            *conflict = svn_string_createf
              (pool,
               _("Trying to add new property '%s' with value "
                 "'%s',\nbut property already exists with value '%s'."),
               propname, new_val->data, working_val->data);
        }
    }
  else  /* property doesn't yet exist in working_props...  */
    /* so just set it */
    apr_hash_set(working_props, propname, APR_HASH_KEY_STRING, new_val);

  return SVN_NO_ERROR;
}

/* Delete the property with name PROPNAME from the set of WORKING_PROPS,
 * setting *STATE or *CONFLICT according to merge outcomes.
 *
 * *STATE is an input and output parameter, its value is to be
 * set using set_merge_prop_state().
 *
 * BASE_VAL contains the working copy base property value
 *
 * OLD_VAL contains the value the of the property the server
 * thinks it's deleting
 */
static svn_error_t *
apply_single_prop_delete(svn_wc_notify_state_t *state,
                         apr_hash_t *working_props,
                         svn_string_t **conflict,
                         const char *propname,
                         const svn_string_t *base_val,
                         const svn_string_t *old_val,
                         apr_pool_t *pool)
{
  svn_string_t *working_val
    = apr_hash_get(working_props, propname, APR_HASH_KEY_STRING);

  if (! base_val)
    {
      apr_hash_set(working_props, propname, APR_HASH_KEY_STRING, NULL);
      if (old_val)
        /* This is a merge, merging a delete into non-existent */
        set_prop_merge_state(state, svn_wc_notify_state_merged);
    }

  else if (svn_string_compare(base_val, old_val))
    {
       if (working_val)
         {
           if (svn_string_compare(working_val, old_val))
             /* they have the same values, so it's an update */
             apr_hash_set(working_props, propname, APR_HASH_KEY_STRING, NULL);
           else
             *conflict = svn_string_createf
               (pool,
                _("Trying to delete property '%s' with value '%s'\n"
                  "but it has been modified from '%s' to '%s'."),
                propname, old_val->data, base_val->data, working_val->data);
         }
       else
         /* The property is locally deleted, so it's a merge */
         set_prop_merge_state(state, svn_wc_notify_state_merged);
    }

  else
    *conflict = svn_string_createf
      (pool,
       _("Trying to delete property '%s' with value '%s'\n"
         "but the local value is '%s'."),
       propname, base_val->data, working_val->data);

  return SVN_NO_ERROR;
}

/* Change the property with name PROPNAME in the set of WORKING_PROPS,
 * setting *STATE or *CONFLICT according to merge outcomes.
 *
 * *STATE is an input and output parameter, its value is to be
 * set using set_merge_prop_state().
 *
 * BASE_VAL contains the working copy base property value
 *
 * OLD_VAL contains the value the of the property the server
 * thinks it's overwriting
 *
 * NEW_VAL contains the value to be set
 */
static svn_error_t *
apply_single_prop_change(svn_wc_notify_state_t *state,
                         apr_hash_t *working_props,
                         svn_string_t **conflict,
                         const char *propname,
                         const svn_string_t *base_val,
                         const svn_string_t *old_val,
                         const svn_string_t *new_val,
                         apr_pool_t *pool)
{
  svn_string_t *working_val
    = apr_hash_get(working_props, propname, APR_HASH_KEY_STRING);

  if ((working_val && ! base_val)
      || (! working_val && base_val)
      || (working_val && base_val
          && !svn_string_compare(working_val, base_val)))
    {
      /* Locally changed property */
      if (working_val)
        {
          if (svn_string_compare(working_val, new_val))
            /* The new value equals the changed value: a merge */
            set_prop_merge_state(state, svn_wc_notify_state_merged);
          else
            {
              if (strcmp(propname, SVN_PROP_MERGE_INFO) == 0)
                {
                  /* We have base, WC, and new values.  Discover
                     deltas between base <-> WC, and base <->
                     incoming.  Combine those deltas, and apply
                     them to base to get the new value. */
                  SVN_ERR(combine_forked_mergeinfo_props(&new_val, old_val,
                                                         working_val,
                                                         new_val, pool));
                  apr_hash_set(working_props, propname,
                               APR_HASH_KEY_STRING, new_val);
                  set_prop_merge_state(state, svn_wc_notify_state_merged);
                }

              else
                {
                  if (base_val)
                    *conflict = svn_string_createf
                      (pool,
                       _("Trying to change property '%s' from '%s' to '%s',\n"
                         "but property has been locally changed "
                         "from '%s' to '%s'."),
                       propname, old_val->data, new_val->data,
                       base_val->data, working_val->data);
                  else
                    *conflict = svn_string_createf
                      (pool,
                       _("Trying to change property '%s' from '%s' to '%s',\n"
                         "but property has been locally added with value '%s'"),
                       propname, old_val->data, new_val->data,
                       working_val->data);
                }
            }
        }

      else
        *conflict = svn_string_createf
          (pool,
           _("Trying to change property '%s' from '%s' to '%s',\n"
             "but it has been locally deleted."),
           propname, old_val->data, new_val->data);

    }

  else if (! working_val) /* means !working_val && !base_val due
                             to conditions above: no prop at all */
    {
      if (strcmp(propname, SVN_PROP_MERGE_INFO) == 0)
        {
          /* Discover any mergeinfo additions in the
             incoming value relative to the base, and
             "combine" those with the empty WC value. */
          apr_hash_t *deleted_mergeinfo, *added_mergeinfo;
          SVN_ERR(diff_mergeinfo_props(&deleted_mergeinfo,
                                       &added_mergeinfo,
                                       old_val, new_val, pool));
          SVN_ERR(svn_mergeinfo__to_string((svn_string_t **)
                                           &new_val,
                                           added_mergeinfo, pool));
          apr_hash_set(working_props, propname, APR_HASH_KEY_STRING, new_val);
        }
      else
        *conflict = svn_string_createf
          (pool,
           _("Trying to change property '%s' from '%s' to '%s',\n"
             "but the property does not exist."),
           propname, old_val->data, new_val->data);
    }

  else /* means working && base && svn_string_compare(working, base) */
    {
      if (svn_string_compare(old_val, base_val))
        apr_hash_set(working_props, propname, APR_HASH_KEY_STRING, new_val);

      else
        {
          if (strcmp(propname, SVN_PROP_MERGE_INFO) == 0)
            {
              /* We have base, WC, and new values.  Discover
                 deltas between base <-> WC, and base <->
                 incoming.  Combine those deltas, and apply
                 them to base to get the new value. */
              SVN_ERR(combine_forked_mergeinfo_props(&new_val, old_val,
                                                     working_val,
                                                     new_val, pool));
              apr_hash_set(working_props, propname,
                           APR_HASH_KEY_STRING, new_val);
              set_prop_merge_state(state, svn_wc_notify_state_merged);
            }
          else
            *conflict = svn_string_createf
              (pool,
               _("Trying to change property '%s' from '%s' to '%s',\n"
                 "but property already exists with value '%s'."),
               propname, old_val->data, new_val->data,
               working_val->data);
        }
    }


  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__merge_props(svn_wc_notify_state_t *state,
                    svn_wc_adm_access_t *adm_access,
                    const char *name,
                    apr_hash_t *server_baseprops,
                    const apr_array_header_t *propchanges,
                    svn_boolean_t base_merge,
                    svn_boolean_t dry_run,
                    apr_pool_t *pool,
                    svn_stringbuf_t **entry_accum)
{
  int i;
  svn_boolean_t is_dir;

  const char *entryname;
  const char *full_path;
  apr_hash_t *working_props;   /* all `working' properties */
  apr_hash_t *base_props;    /* all `pristine' properties */

  const char *access_path = svn_wc_adm_access_path(adm_access);
  int access_len = strlen(access_path);
  const char *reject_path = NULL;
  apr_file_t *reject_tmp_fp = NULL;       /* the temporary conflicts file */
  const char *reject_tmp_path = NULL;

  /* Non-empty path without trailing slash need an extra slash removed */
  if (access_len != 0 && access_path[access_len - 1] != '/')
    access_len++;

  if (name == NULL)
    {
      /* We must be merging props on the directory PATH  */
      entryname = SVN_WC_ENTRY_THIS_DIR;
      full_path = access_path;
      is_dir = TRUE;
    }
  else
    {
      /* We must be merging props on the file PATH/NAME */
      entryname = name;
      full_path = svn_path_join(access_path, name, pool);
      is_dir = FALSE;
    }

  /* Load the base & working property files into hashes */
  SVN_ERR(svn_wc__load_props(&base_props, &working_props,
                             adm_access, entryname, pool));
  if (!server_baseprops)
    server_baseprops = base_props;

  if (state)
    {
      /* Start out assuming no changes or conflicts.  Don't bother to
         examine propchanges->nelts yet; even if we knew there were
         propchanges, we wouldn't yet know if they are "normal" props,
         as opposed wc or entry props.  */
      *state = svn_wc_notify_state_unchanged;
    }

  /* Looping over the array of incoming propchanges we want to apply: */
  for (i = 0; i < propchanges->nelts; i++)
    {
      const char *propname;
      svn_string_t *conflict = NULL;
      const svn_prop_t *incoming_change;
      const svn_string_t *from_val, *to_val, *working_val, *base_val;
      svn_boolean_t is_normal;

      /* For the incoming propchange, figure out the TO and FROM values. */
      incoming_change = &APR_ARRAY_IDX(propchanges, i, svn_prop_t);
      propname = incoming_change->name;
      is_normal = svn_wc_is_normal_prop(propname);
      to_val = incoming_change->value
        ? svn_string_dup(incoming_change->value, pool) : NULL;
      from_val = apr_hash_get(server_baseprops, propname, APR_HASH_KEY_STRING);

      working_val = apr_hash_get(working_props, propname, APR_HASH_KEY_STRING);
      base_val = apr_hash_get(base_props, propname, APR_HASH_KEY_STRING);

      if (base_merge)
        apr_hash_set(base_props, propname, APR_HASH_KEY_STRING, to_val);

      /* We already know that state is at least `changed', so mark
         that, but remember that we may later upgrade to `merged' or
         even `conflicted'. */
      if (is_normal)
        set_prop_merge_state(state, svn_wc_notify_state_changed);

      if (! from_val)  /* adding a new property */
        SVN_ERR(apply_single_prop_add(is_normal ? state : NULL,
                                      working_props, &conflict,
                                      propname, base_val, to_val, pool));

      else if (! to_val) /* delete an existing property */
        SVN_ERR(apply_single_prop_delete(is_normal ? state : NULL,
                                         working_props, &conflict,
                                         propname, base_val, from_val, pool));

      else  /* changing an existing property */
        SVN_ERR(apply_single_prop_change(is_normal ? state : NULL,
                                         working_props, &conflict,
                                         propname, base_val, from_val,
                                         to_val, pool));


      /* merging logic complete, now we need to possibly log conflict
         data to tmpfiles.  */

      if (conflict)
        {
          if (is_normal)
            set_prop_merge_state(state, svn_wc_notify_state_conflicted);

          if (dry_run)
            continue;   /* skip to next incoming change */

          if (! reject_tmp_fp)
            /* This is the very first prop conflict found on this item. */
            SVN_ERR(open_reject_tmp_file(&reject_tmp_fp, &reject_tmp_path,
                                         full_path, adm_access, is_dir,
                                         pool));

          /* Append the conflict to the open tmp/PROPS/---.prej file */
          SVN_ERR(append_prop_conflict(reject_tmp_fp, conflict, pool));
        }

    }  /* foreach propchange ... */

  /* Finished applying all incoming propchanges to our hashes! */

  if (dry_run)
    return SVN_NO_ERROR;

  SVN_ERR(svn_wc__install_props(entry_accum, adm_access, entryname,
                                base_props, working_props, base_merge,
                                pool));

  if (reject_tmp_fp)
    {
      /* There's a .prej file sitting in .svn/tmp/ somewhere.  Deal
         with the conflicts.  */

      /* First, _close_ this temporary conflicts file.  We've been
         appending to it all along. */
      SVN_ERR(svn_io_file_close(reject_tmp_fp, pool));

      /* Now try to get the name of a pre-existing .prej file from the
         entries file */
      SVN_ERR(get_existing_prop_reject_file(&reject_path,
                                            adm_access,
                                            entryname,
                                            pool));

      if (! reject_path)
        {
          /* Reserve a new .prej file *above* the .svn/ directory by
             opening and closing it. */
          const char *reserved_path;
          const char *full_reject_path;

          full_reject_path = svn_path_join
            (access_path, is_dir ? SVN_WC__THIS_DIR_PREJ : name, pool);

          SVN_ERR(svn_io_open_unique_file2(NULL, &reserved_path,
                                           full_reject_path,
                                           SVN_WC__PROP_REJ_EXT,
                                           svn_io_file_del_none, pool));

          /* This file will be overwritten when the log is run; that's
             ok, because at least now we have a reservation on
             disk. */

          /* Now just get the name of the reserved file.  This is the
             "relative" path we will use in the log entry. */
          reject_path = svn_path_basename(reserved_path, pool);
        }

      /* We've now guaranteed that some kind of .prej file exists
         above the .svn/ dir.  We write log entries to append our
         conflicts to it. */
      SVN_ERR(svn_wc__loggy_append(entry_accum, adm_access,
                                   reject_tmp_path, reject_path, pool));

      /* And of course, delete the temporary reject file. */
      SVN_ERR(svn_wc__loggy_remove(entry_accum, adm_access,
                                   reject_tmp_path, pool));

      /* Mark entry as "conflicted" with a particular .prej file. */
      {
        svn_wc_entry_t entry;

        entry.prejfile = reject_path;
        SVN_ERR(svn_wc__loggy_entry_modify(entry_accum,
                                           adm_access,
                                           entryname,
                                           &entry,
                                           SVN_WC__ENTRY_MODIFY_PREJFILE,
                                           pool));
      }

    } /* if (reject_tmp_fp) */

  return SVN_NO_ERROR;
}


/* This is DEPRECATED, use svn_wc_merge_props() instead. */
svn_error_t *
svn_wc_merge_prop_diffs(svn_wc_notify_state_t *state,
                        const char *path,
                        svn_wc_adm_access_t *adm_access,
                        const apr_array_header_t *propchanges,
                        svn_boolean_t base_merge,
                        svn_boolean_t dry_run,
                        apr_pool_t *pool)
{
  /* NOTE: Here, we use implementation knowledge.  The public
     svn_wc_merge_props doesn't allow NULL as baseprops argument, but we know
     that it works. */
  return svn_wc_merge_props(state, path, adm_access, NULL, propchanges,
                            base_merge, dry_run, pool);
}



/*------------------------------------------------------------------*/

/*** Private 'wc prop' functions ***/

/* If wcprops are stored in a single file in this working copy, read that file
   and store it in the cache of ADM_ACCESS.   Use POOL for temporary
   allocations. */
static svn_error_t *
read_wcprops(svn_wc_adm_access_t *adm_access, apr_pool_t *pool)
{
  apr_file_t *file;
  apr_pool_t *cache_pool = svn_wc_adm_access_pool(adm_access);
  apr_hash_t *all_wcprops;
  apr_hash_t *proplist;
  svn_stream_t *stream;
  svn_error_t *err;

  /* If the WC format is too old, there is nothing to cache. */
  if (svn_wc__adm_wc_format(adm_access) <= SVN_WC__WCPROPS_MANY_FILES_VERSION)
    return SVN_NO_ERROR;

  all_wcprops = apr_hash_make(cache_pool);

  err = svn_wc__open_adm_file(&file, svn_wc_adm_access_path(adm_access),
                              SVN_WC__ADM_ALL_WCPROPS,
                              APR_READ | APR_BUFFERED, pool);

  /* A non-existent file means there are no props. */
  if (err && APR_STATUS_IS_ENOENT(err->apr_err))
    {
      svn_error_clear(err);
      svn_wc__adm_access_set_wcprops(adm_access, all_wcprops);
      return SVN_NO_ERROR;
    }
  SVN_ERR(err);

  stream = svn_stream_from_aprfile2(file, TRUE, pool);

  /* Read the proplist for THIS_DIR. */
  proplist = apr_hash_make(cache_pool);
  SVN_ERR(svn_hash_read2(proplist, stream, SVN_HASH_TERMINATOR, cache_pool));
  apr_hash_set(all_wcprops, SVN_WC_ENTRY_THIS_DIR, APR_HASH_KEY_STRING,
               proplist);

  /* And now, the children. */
  while (1729)
    {
      svn_stringbuf_t *line;
      svn_boolean_t eof;
      SVN_ERR(svn_stream_readline(stream, &line, "\n", &eof, cache_pool));
      if (eof)
        {
          if (line->len > 0)
            return svn_error_createf
              (SVN_ERR_WC_CORRUPT, NULL,
               _("Missing end of line in wcprops file for '%s'"),
               svn_path_local_style(svn_wc_adm_access_path(adm_access), pool));
          break;
        }
      proplist = apr_hash_make(cache_pool);
      SVN_ERR(svn_hash_read2(proplist, stream, SVN_HASH_TERMINATOR,
                             cache_pool));
      apr_hash_set(all_wcprops, line->data, APR_HASH_KEY_STRING, proplist);
    }

  svn_wc__adm_access_set_wcprops(adm_access, all_wcprops);

  SVN_ERR(svn_wc__close_adm_file(file, svn_wc_adm_access_path(adm_access),
                                 SVN_WC__ADM_ALL_WCPROPS, FALSE, pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__wcprops_write(svn_wc_adm_access_t *adm_access, apr_pool_t *pool)
{
  apr_hash_t *wcprops = svn_wc__adm_access_wcprops(adm_access);
  apr_file_t *file;
  svn_stream_t *stream;
  apr_hash_t *proplist;
  apr_hash_index_t *hi;
  apr_pool_t *subpool = svn_pool_create(pool);
  svn_boolean_t any_props = FALSE;

  /* If there are no cached wcprops, there is nothing to do. */
  if (! wcprops)
    return SVN_NO_ERROR;

  /* Check if there are any properties at all. */
  for (hi = apr_hash_first(pool, wcprops); hi && ! any_props;
       hi = apr_hash_next(hi))
    {
      void *val;

      apr_hash_this(hi, NULL, NULL, &val);
      proplist = val;
      if (apr_hash_count(proplist) > 0)
        any_props = TRUE;
    }

  /* If there are no props, remove the file. */
  if (! any_props)
    {
      svn_error_t *err;

      err = svn_wc__remove_adm_file(svn_wc_adm_access_path(adm_access), pool,
                                    SVN_WC__ADM_ALL_WCPROPS, NULL);
      if (err && APR_STATUS_IS_ENOENT(err->apr_err))
        {
          svn_error_clear(err);
          return SVN_NO_ERROR;
        }
      else
        return err;
    }

  SVN_ERR(svn_wc__open_adm_file(&file, svn_wc_adm_access_path(adm_access),
                                SVN_WC__ADM_ALL_WCPROPS,
                                APR_WRITE | APR_BUFFERED, pool));
  stream = svn_stream_from_aprfile2(file, TRUE, pool);

  /* First, the props for this_dir. */
  proplist = apr_hash_get(wcprops, SVN_WC_ENTRY_THIS_DIR, APR_HASH_KEY_STRING);
  if (! proplist)
    proplist = apr_hash_make(subpool);
  SVN_ERR(svn_hash_write2(proplist, stream, SVN_HASH_TERMINATOR, subpool));

  /* Write children. */
  for (hi = apr_hash_first(pool, wcprops); hi; hi = apr_hash_next(hi))
    {
      const void *key;
      void *val;
      const char *name;

      apr_hash_this(hi, &key, NULL, &val);
      name = key;
      proplist = val;

      /* We already wrote this_dir, and writing empty hashes makes me
         feel silly... */
      if (strcmp(SVN_WC_ENTRY_THIS_DIR, name) == 0
          || apr_hash_count(proplist) == 0)
        continue;

      svn_pool_clear(subpool);

      SVN_ERR(svn_stream_printf(stream, subpool, "%s\n", name));
      SVN_ERR(svn_hash_write2(proplist, stream, SVN_HASH_TERMINATOR, subpool));
    }

  SVN_ERR(svn_wc__close_adm_file(file, svn_wc_adm_access_path(adm_access),
                                 SVN_WC__ADM_ALL_WCPROPS, TRUE, pool));

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__wcprop_list(apr_hash_t **wcprops,
                    const char *entryname,
                    svn_wc_adm_access_t *adm_access,
                    apr_pool_t *pool)
{
  const char *prop_path;
  const svn_wc_entry_t *entry;
  apr_hash_t *all_wcprops;
  apr_pool_t *cache_pool = svn_wc_adm_access_pool(adm_access);
  const char *path = svn_path_join(svn_wc_adm_access_path(adm_access),
                                   entryname, pool);

  SVN_ERR(svn_wc_entry(&entry, path, adm_access, FALSE, pool));
  if (! entry)
    {
      /* No entry exists, therefore no wcprop-file can exist */
      *wcprops = apr_hash_make(pool);
      return SVN_NO_ERROR;
    }

  /* Try the cache first. */
  all_wcprops = svn_wc__adm_access_wcprops(adm_access);
  if (! all_wcprops)
    {
      SVN_ERR(read_wcprops(adm_access, pool));
      all_wcprops = svn_wc__adm_access_wcprops(adm_access);
    }
  if (all_wcprops)
    {
      *wcprops = apr_hash_get(all_wcprops, entryname, APR_HASH_KEY_STRING);
      /* The cache contains no hash tables for empty proplist, so we just
         create one here if that's the case. */
      if (! *wcprops)
        {
          *wcprops = apr_hash_make(cache_pool);
          entryname = apr_pstrdup(cache_pool, entryname);
          apr_hash_set(all_wcprops, entryname, APR_HASH_KEY_STRING, *wcprops);
        }
      return SVN_NO_ERROR;
    }

  /* Fall back on individual files for backwards compatibility. */

  /* Construct a path to the relevant property file */
  SVN_ERR(svn_wc__wcprop_path(&prop_path, path, entry->kind, FALSE, pool));
  *wcprops = apr_hash_make(pool);
  SVN_ERR(svn_wc__load_prop_file(prop_path, *wcprops, pool));

  return SVN_NO_ERROR;
}


/* Get a single 'wcprop' NAME for versioned object PATH, return in
   *VALUE.  ADM_ACCESS is an access baton set that contains PATH. */
static svn_error_t *
wcprop_get(const svn_string_t **value,
                   const char *name,
                   const char *path,
                   svn_wc_adm_access_t *adm_access,
                   apr_pool_t *pool)
{
  svn_error_t *err;
  apr_hash_t *prophash;
  const svn_wc_entry_t *entry;

  SVN_ERR(svn_wc_entry(&entry, path, adm_access, FALSE, pool));
  if (! entry)
    {
      *value = NULL;
      return SVN_NO_ERROR;
    }
  if (entry->kind == svn_node_dir)
    SVN_ERR(svn_wc_adm_retrieve(&adm_access, adm_access, path, pool));
  else
    SVN_ERR(svn_wc_adm_retrieve(&adm_access, adm_access,
                                svn_path_dirname(path, pool), pool));

  err = svn_wc__wcprop_list(&prophash, entry->name, adm_access, pool);
  if (err)
    return
      svn_error_quick_wrap
      (err, _("Failed to load properties from disk"));

  *value = apr_hash_get(prophash, name, APR_HASH_KEY_STRING);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__wcprop_set(const char *name,
                   const svn_string_t *value,
                   const char *path,
                   svn_wc_adm_access_t *adm_access,
                   svn_boolean_t force_write,
                   apr_pool_t *pool)
{
  svn_error_t *err;
  apr_hash_t *prophash;
  apr_file_t *fp = NULL;
  apr_pool_t *cache_pool = svn_wc_adm_access_pool(adm_access);
  const svn_wc_entry_t *entry;

  SVN_ERR(svn_wc__entry_versioned(&entry, path, adm_access, FALSE, pool));

  if (entry->kind == svn_node_dir)
    SVN_ERR(svn_wc_adm_retrieve(&adm_access, adm_access, path, pool));
  else
    SVN_ERR(svn_wc_adm_retrieve(&adm_access, adm_access,
                                svn_path_dirname(path, pool), pool));
  err = svn_wc__wcprop_list(&prophash, entry->name, adm_access, pool);
  if (err)
    return
      svn_error_quick_wrap
      (err, _("Failed to load properties from disk"));

  /* Now we have all the properties in our hash.  Simply merge the new
     property into it. */
  name = apr_pstrdup(cache_pool, name);
  if (value)
    value = svn_string_dup(value, cache_pool);
  apr_hash_set(prophash, name, APR_HASH_KEY_STRING, value);

  if (svn_wc__adm_wc_format(adm_access) > SVN_WC__WCPROPS_MANY_FILES_VERSION)
    {
      if (force_write)
        SVN_ERR(svn_wc__wcprops_write(adm_access, pool));
    }
  else
    {
      /* For backwards compatibility.  We don't use the cache in this case,
         so write to disk regardless of force_write. */
      /* Open the propfile for writing. */
      SVN_ERR(svn_wc__open_props(&fp, 
                                 path, /* open in PATH */
                                 (APR_WRITE | APR_CREATE | APR_BUFFERED),
                                 0, /* not base props */
                                 1, /* we DO want wcprops */
                                 pool));
      /* Write. */
      SVN_ERR_W(svn_hash_write(prophash, fp, pool),
                apr_psprintf(pool,
                             _("Cannot write property hash for '%s'"),
                             svn_path_local_style(path, pool)));
  
      /* Close file, doing an atomic "move". */
      SVN_ERR(svn_wc__close_props(fp, path, 0, 1,
                                  1, /* sync! */
                                  pool));
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc__remove_wcprops(svn_wc_adm_access_t *adm_access,
                       const char *name,
                       svn_boolean_t recurse,
                       apr_pool_t *pool)
{
  apr_hash_t *all_wcprops = svn_wc__adm_access_wcprops(adm_access);
  svn_boolean_t write_needed = FALSE;

  if (! name)
    {
      /* There is no point in reading the props just to determine if we
         need to rewrite them:-), so assume a write is needed if the props
         aren't already cached. */
      if (! all_wcprops || apr_hash_count(all_wcprops) > 0)
        {
          svn_wc__adm_access_set_wcprops
            (adm_access, apr_hash_make(svn_wc_adm_access_pool(adm_access)));
          write_needed = TRUE;
        }
    }
  else
    {
      apr_hash_t *wcprops;
      if (! all_wcprops)
        {
          SVN_ERR(read_wcprops(adm_access, pool));
          all_wcprops = svn_wc__adm_access_wcprops(adm_access);
        }
      if (all_wcprops)
        wcprops = apr_hash_get(all_wcprops, name, APR_HASH_KEY_STRING);
      else
        wcprops = NULL;
      if (wcprops && apr_hash_count(wcprops) > 0)
        {
          apr_hash_set(all_wcprops, name, APR_HASH_KEY_STRING, NULL);
          write_needed = TRUE;
        }
    }
  if (write_needed)
    SVN_ERR(svn_wc__wcprops_write(adm_access, pool));

  if (recurse)
    {
      apr_hash_t *entries;
      apr_hash_index_t *hi;
      apr_pool_t *subpool = svn_pool_create(pool);

      /* Read PATH's entries. */
      SVN_ERR(svn_wc_entries_read(&entries, adm_access, FALSE, subpool));

      /* Recursively loop over all children. */
      for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
        {
          const void *key;
          void *val;
          const char *entryname;
          const svn_wc_entry_t *current_entry;
          const char *child_path;
          svn_wc_adm_access_t *child_access;

          apr_hash_this(hi, &key, NULL, &val);
          entryname = key;
          current_entry = val;

          /* Ignore files and the "this dir" entry. */
          if (current_entry->kind != svn_node_dir
              || ! strcmp(entryname, SVN_WC_ENTRY_THIS_DIR))
            continue;

          svn_pool_clear(subpool);

          child_path = svn_path_join(svn_wc_adm_access_path(adm_access),
                                     entryname, subpool);

          SVN_ERR(svn_wc_adm_retrieve(&child_access, adm_access, child_path,
                                      subpool));
          SVN_ERR(svn_wc__remove_wcprops(child_access, NULL, TRUE, subpool));
        }

        /* Cleanup */
        svn_pool_destroy(subpool);
    }

  return SVN_NO_ERROR;
}


/*------------------------------------------------------------------*/

/*** Public Functions ***/


svn_error_t *
svn_wc_prop_list(apr_hash_t **props,
                 const char *path,
                 svn_wc_adm_access_t *adm_access,
                 apr_pool_t *pool)
{
  const svn_wc_entry_t *entry;

  SVN_ERR(svn_wc_entry(&entry, path, adm_access, TRUE, pool));

  /* if there is no entry, 'path' is not under version control and
     therefore has no props */
  if (! entry)
    {
      *props = apr_hash_make(pool);
      return SVN_NO_ERROR;
    }

  if (entry->kind == svn_node_dir)
    SVN_ERR(svn_wc_adm_retrieve(&adm_access, adm_access, path, pool));
  else
    SVN_ERR(svn_wc_adm_retrieve(&adm_access, adm_access,
                                svn_path_dirname(path, pool), pool));

  return svn_wc__load_props(NULL, props, adm_access, entry->name, pool);
}

/* Determine if PROPNAME is contained in the list of space separated
   values STRING.  */

static svn_boolean_t
string_contains_prop(const char *string, const char *propname)
{
  const char *place = strstr(string, propname);
  int proplen = strlen(propname);

  if (!place)
    return FALSE;
  
  while (place)
    {      
      if (place[proplen] == ' ' || place[proplen] == 0)
        return TRUE;
      place = strstr(place + 1, propname);
    }
  return FALSE;
}

svn_error_t *
svn_wc_prop_get(const svn_string_t **value,
                const char *name,
                const char *path,
                svn_wc_adm_access_t *adm_access,
                apr_pool_t *pool)
{
  svn_error_t *err;
  apr_hash_t *prophash;
  enum svn_prop_kind kind = svn_property_kind(NULL, name);
  const svn_wc_entry_t *entry;

  SVN_ERR(svn_wc_entry(&entry, path, adm_access, TRUE, pool));

  if (entry == NULL)
    {
      *value = NULL;
      return SVN_NO_ERROR;
    }

  if (entry->cachable_props
      && string_contains_prop(entry->cachable_props, name))
    {
      /* We separate these two cases so that we can return the correct
         value for booleans if they exist in the string.  */
      if (!entry->present_props
          || !string_contains_prop(entry->present_props, name))
        {
          *value = NULL;
          return SVN_NO_ERROR;
        }
      if (svn_prop_is_boolean(name))
        {
          *value = svn_string_create(SVN_PROP_BOOLEAN_TRUE, pool);
          assert(*value != NULL);
          return SVN_NO_ERROR;
        }
    }

  if (kind == svn_prop_wc_kind)
    {
      return wcprop_get(value, name, path, adm_access, pool);
    }
  if (kind == svn_prop_entry_kind)
    {
      return svn_error_createf   /* we don't do entry properties here */
        (SVN_ERR_BAD_PROP_KIND, NULL,
         _("Property '%s' is an entry property"), name);
    }
  else  /* regular prop */
    {
      err = svn_wc_prop_list(&prophash, path, adm_access, pool);
      if (err)
        return
          svn_error_quick_wrap
          (err, _("Failed to load properties from disk"));
      
      *value = apr_hash_get(prophash, name, APR_HASH_KEY_STRING);

      return SVN_NO_ERROR;
    }
}


/* The special Subversion properties are not valid for all node kinds.
   Return an error if NAME is an invalid Subversion property for PATH which
   is of kind NODE_KIND. */
static svn_error_t *
validate_prop_against_node_kind(const char *name,
                                const char *path,
                                svn_node_kind_t node_kind,
                                apr_pool_t *pool)
{

  const char *file_prohibit[] = { SVN_PROP_IGNORE,
                                  SVN_PROP_EXTERNALS,
                                  NULL };
  const char *dir_prohibit[] = { SVN_PROP_EXECUTABLE,
                                 SVN_PROP_KEYWORDS,
                                 SVN_PROP_EOL_STYLE,
                                 SVN_PROP_MIME_TYPE,
                                 SVN_PROP_NEEDS_LOCK,
                                 NULL };
  const char **node_kind_prohibit;
  const char *path_display
    = svn_path_is_url(path) ? path : svn_path_local_style(path, pool);

  switch (node_kind)
    {
    case svn_node_dir:
      node_kind_prohibit = dir_prohibit;
      while (*node_kind_prohibit)
        if (strcmp(name, *node_kind_prohibit++) == 0)
          return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                                   _("Cannot set '%s' on a directory ('%s')"),
                                   name, path_display);
      break;
    case svn_node_file:
      node_kind_prohibit = file_prohibit;
      while (*node_kind_prohibit)
        if (strcmp(name, *node_kind_prohibit++) == 0)
          return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                                   _("Cannot set '%s' on a file ('%s')"),
                                   name,
                                   path_display);
      break;
    default:
      return svn_error_createf(SVN_ERR_NODE_UNEXPECTED_KIND, NULL,
                               _("'%s' is not a file or directory"),
                               path_display);
    }

  return SVN_NO_ERROR;
}                             


struct getter_baton {
  const char *path;
  svn_wc_adm_access_t *adm_access;
};


static svn_error_t *
get_file_for_validation(const svn_string_t **mime_type,
                        svn_stream_t *stream,
                        void *baton,
                        apr_pool_t *pool)
{
  struct getter_baton *gb = baton;
  apr_file_t *fp;
  svn_stream_t *read_stream;

  SVN_ERR(svn_wc_prop_get(mime_type, SVN_PROP_MIME_TYPE,
                          gb->path, gb->adm_access, pool));

  /* Open PATH. */
  SVN_ERR(svn_io_file_open(&fp, gb->path, 
                           (APR_READ | APR_BINARY | APR_BUFFERED),
                           0, pool));

  /* Get a READ_STREAM from the file we just opened. */
  read_stream = svn_stream_from_aprfile(fp, pool);

  /* Copy from the file into the translating stream. */
  SVN_ERR(svn_stream_copy(read_stream, stream, pool));

  SVN_ERR(svn_stream_close(read_stream));
  SVN_ERR(svn_io_file_close(fp, pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
validate_eol_prop_against_file(const char *path, 
                               svn_wc_canonicalize_svn_prop_get_file_t getter,
                               void *getter_baton,
                               apr_pool_t *pool)
{
  svn_stream_t *translating_stream;
  svn_error_t *err;
  const svn_string_t *mime_type;
  const char *path_display
    = svn_path_is_url(path) ? path : svn_path_local_style(path, pool);

  /* The "getter" will do a newline translation.  All we really care
     about here is whether or not the function fails on inconsistent
     line endings.  The function is "translating" to an empty stream.
     This is sneeeeeeeeeeeaky. */
  translating_stream = svn_subst_stream_translated(svn_stream_empty(pool),
                                                   "", FALSE, NULL, FALSE,
                                                   pool);

  err = getter(&mime_type, translating_stream, getter_baton, pool);

  if (!err)
    err = svn_stream_close(translating_stream);

  if (err && err->apr_err == SVN_ERR_IO_INCONSISTENT_EOL)
    return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, err,
                             _("File '%s' has inconsistent newlines"),
                             path_display);
  else if (err)
    return err;

  /* See if this file has been determined to be binary. */
  if (mime_type && svn_mime_type_is_binary(mime_type->data))
    return svn_error_createf 
      (SVN_ERR_ILLEGAL_TARGET, NULL,
       _("File '%s' has binary mime type property"),
       path_display);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_prop_set2(const char *name,
                 const svn_string_t *value,
                 const char *path,
                 svn_wc_adm_access_t *adm_access,
                 svn_boolean_t skip_checks,
                 apr_pool_t *pool)
{
  svn_error_t *err;
  apr_hash_t *prophash, *base_prophash;
  enum svn_prop_kind prop_kind = svn_property_kind(NULL, name);
  const char *base_name;
  svn_stringbuf_t *log_accum = svn_stringbuf_create("", pool);
  const svn_wc_entry_t *entry;
  
  if (prop_kind == svn_prop_wc_kind)
    return svn_wc__wcprop_set(name, value, path, adm_access, TRUE, pool);
  else if (prop_kind == svn_prop_entry_kind)
    return svn_error_createf   /* we don't do entry properties here */
      (SVN_ERR_BAD_PROP_KIND, NULL,
       _("Property '%s' is an entry property"), name);

  /* Else, handle a regular property: */

  /* Get the entry and name for this path. */
  SVN_ERR(svn_wc__entry_versioned(&entry, path, adm_access, FALSE, pool));

  base_name = entry->name;

  /* Get the access baton for the entry's directory. */
  if (entry->kind == svn_node_dir)
    SVN_ERR(svn_wc_adm_retrieve(&adm_access, adm_access, path, pool));
  else
    SVN_ERR(svn_wc_adm_retrieve(&adm_access, adm_access,
                                svn_path_dirname(path, pool), pool));

  /* Setting an inappropriate property is not allowed (unless
     overridden by 'skip_checks', in some circumstances).  Deleting an
     inappropriate property is allowed, however, since older clients
     allowed (and other clients possibly still allow) setting it in
     the first place. */
  if (value && svn_prop_is_svn_prop(name))
    {
      const svn_string_t *new_value;
      struct getter_baton *gb = apr_pcalloc(pool, sizeof(*gb));
      
      gb->path = path;
      gb->adm_access = adm_access;

      SVN_ERR(svn_wc_canonicalize_svn_prop(&new_value, name, value, path,
                                           entry->kind, skip_checks,
                                           get_file_for_validation, gb, pool));
      value = new_value;
    }

  if (entry->kind == svn_node_file && strcmp(name, SVN_PROP_EXECUTABLE) == 0)
    {
      /* If the svn:executable property was set, then chmod +x.
         If the svn:executable property was deleted (NULL value passed
         in), then chmod -x. */
      if (value == NULL)
        SVN_ERR(svn_io_set_file_executable(path, FALSE, TRUE, pool));
      else
        SVN_ERR(svn_io_set_file_executable(path, TRUE, TRUE, pool));
    }

  if (entry->kind == svn_node_file && strcmp(name, SVN_PROP_NEEDS_LOCK) == 0)
    {
      /* If the svn:needs-lock property was set to NULL, set the file
         to read-write */
      if (value == NULL)
        SVN_ERR(svn_io_set_file_read_write(path, FALSE, pool));
        
      /* If not, we'll set the file to read-only at commit time. */
    }

  err = svn_wc__load_props(&base_prophash, &prophash, adm_access, base_name,
                           pool);
  if (err)
    return
      svn_error_quick_wrap
      (err, _("Failed to load properties from disk"));

  /* If we're changing this file's list of expanded keywords, then
   * we'll need to invalidate its text timestamp, since keyword
   * expansion affects the comparison of working file to text base.
   *
   * Here we retrieve the old list of expanded keywords; after the
   * property is set, we'll grab the new list and see if it differs
   * from the old one.
   */
  if (entry->kind == svn_node_file && strcmp(name, SVN_PROP_KEYWORDS) == 0)
    {
      svn_string_t *old_value = apr_hash_get(prophash, SVN_PROP_KEYWORDS,
                                             APR_HASH_KEY_STRING);
      apr_hash_t *old_keywords, *new_keywords;

      SVN_ERR(svn_wc__get_keywords(&old_keywords, path, adm_access,
                                   old_value ? old_value->data : "", pool));
      SVN_ERR(svn_wc__get_keywords(&new_keywords, path, adm_access,
                                   value ? value->data : "", pool));

      if (svn_subst_keywords_differ2(old_keywords, new_keywords, FALSE, pool))
        {
          svn_wc_entry_t tmp_entry;

          /* If we changed the keywords or newlines, void the entry
             timestamp for this file, so svn_wc_text_modified_p() does
             a real (albeit slow) check later on. */
          tmp_entry.kind = svn_node_file;
          tmp_entry.text_time = 0;
          SVN_ERR(svn_wc__loggy_entry_modify(&log_accum, adm_access,
                                             base_name, &tmp_entry,
                                             SVN_WC__ENTRY_MODIFY_TEXT_TIME,
                                             pool));
        }
    }

  /* Now we have all the properties in our hash.  Simply merge the new
     property into it. */
  apr_hash_set(prophash, name, APR_HASH_KEY_STRING, value);
  
  SVN_ERR(svn_wc__install_props(&log_accum, adm_access, base_name,
                                base_prophash, prophash, FALSE, pool));
  SVN_ERR(svn_wc__write_log(adm_access, 0, log_accum, pool));
  SVN_ERR(svn_wc__run_log(adm_access, NULL, pool));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_prop_set(const char *name,
                const svn_string_t *value,
                const char *path,
                svn_wc_adm_access_t *adm_access,
                apr_pool_t *pool)
{
  return svn_wc_prop_set2(name, value, path, adm_access, FALSE, pool);
}


svn_error_t *
svn_wc_canonicalize_svn_prop(const svn_string_t **propval_p,
                             const char *propname,
                             const svn_string_t *propval,
                             const char *path,
                             svn_node_kind_t kind,
                             svn_boolean_t skip_some_checks,
                             svn_wc_canonicalize_svn_prop_get_file_t getter,
                             void *getter_baton,
                             apr_pool_t *pool)
{
  svn_stringbuf_t *new_value = NULL;

  /* Keep this static, it may get stored (for read-only purposes) in a
     hash that outlives this function. */
  static const svn_string_t boolean_value =
    {
      SVN_PROP_BOOLEAN_TRUE,
      sizeof(SVN_PROP_BOOLEAN_TRUE) - 1
    };

  SVN_ERR(validate_prop_against_node_kind(propname, path, kind, pool));
  
  if (!skip_some_checks && (strcmp(propname, SVN_PROP_EOL_STYLE) == 0))
    {
      new_value = svn_stringbuf_create_from_string(propval, pool);
      svn_stringbuf_strip_whitespace(new_value);
      SVN_ERR(validate_eol_prop_against_file(path, getter, getter_baton, pool));
    }
  else if (!skip_some_checks && (strcmp(propname, SVN_PROP_MIME_TYPE) == 0))
    {
      new_value = svn_stringbuf_create_from_string(propval, pool);
      svn_stringbuf_strip_whitespace(new_value);
      SVN_ERR(svn_mime_type_validate(new_value->data, pool));
    }
  else if (strcmp(propname, SVN_PROP_IGNORE) == 0
           || strcmp(propname, SVN_PROP_EXTERNALS) == 0)
    {
      /* Make sure that the last line ends in a newline */
      if (propval->data[propval->len - 1] != '\n')
        {
          new_value = svn_stringbuf_create_from_string(propval, pool);
          svn_stringbuf_appendbytes(new_value, "\n", 1);
        }
      
      /* Make sure this is a valid externals property.  Do not
         allow 'skip_some_checks' to override, as there is no circumstance in
         which this is proper (because there is no circumstance in
         which Subversion can handle it). */
      if (strcmp(propname, SVN_PROP_EXTERNALS) == 0)
        {
          /* We don't allow "." nor ".." as target directories in
             an svn:externals line.  As it happens, our parse code
             checks for this, so all we have to is invoke it --
             we're not interested in the parsed result, only in
             whether or the parsing errored. */
          SVN_ERR(svn_wc_parse_externals_description2
                  (NULL, path, propval->data, pool));
        }
    }
  else if (strcmp(propname, SVN_PROP_KEYWORDS) == 0)
    {
      new_value = svn_stringbuf_create_from_string(propval, pool);
      svn_stringbuf_strip_whitespace(new_value);
    }
  else if (strcmp(propname, SVN_PROP_EXECUTABLE) == 0
        || strcmp(propname, SVN_PROP_NEEDS_LOCK) == 0)
    {
      new_value = svn_stringbuf_create_from_string(&boolean_value, pool);
    }

  if (new_value)
    *propval_p = svn_string_create_from_buf(new_value, pool);
  else
    *propval_p = propval;

  return SVN_NO_ERROR;
}


svn_boolean_t
svn_wc_is_normal_prop(const char *name)
{
  enum svn_prop_kind kind = svn_property_kind(NULL, name);
  return (kind == svn_prop_regular_kind);
}


svn_boolean_t
svn_wc_is_wc_prop(const char *name)
{
  enum svn_prop_kind kind = svn_property_kind(NULL, name);
  return (kind == svn_prop_wc_kind);
}


svn_boolean_t
svn_wc_is_entry_prop(const char *name)
{
  enum svn_prop_kind kind = svn_property_kind(NULL, name);
  return (kind == svn_prop_entry_kind);
}


/* Helper to optimize svn_wc_props_modified_p().

   If PATH_TO_PROP_FILE is nonexistent, is empty, or is of size 4 bytes
   ("END\n"), then set EMPTY_P to true.   Otherwise set EMPTY_P to false,
   which means that the file must contain real properties.  */
static svn_error_t *
empty_props_p(svn_boolean_t *empty_p,
              const char *path_to_prop_file,
              apr_pool_t *pool)
{
  svn_error_t *err;
  apr_finfo_t finfo;

  err = svn_io_stat(&finfo, path_to_prop_file, APR_FINFO_MIN | APR_FINFO_TYPE,
                    pool);
  if (err)
    {
      if (! APR_STATUS_IS_ENOENT(err->apr_err)
          && ! APR_STATUS_IS_ENOTDIR(err->apr_err))
        return err;

      /* nonexistent */
      svn_error_clear(err);
      *empty_p = TRUE;
    }
  else
    {


      /* If we remove props from a propfile, eventually the file will
         be empty, or, for working copies written by pre-1.3 libraries, will
         contain nothing but "END\n" */
      if (finfo.filetype == APR_REG && (finfo.size == 4 || finfo.size == 0))
        *empty_p = TRUE;
      else
        *empty_p = FALSE;

      /* If the size is between 1 and 4, then something is corrupt.
         If the size is between 4 and 16, then something is corrupt,
         because 16 is the -smallest- the file can possibly be if it
         contained only one property.  So long as we say it is "not
         empty", we will discover such corruption later when we try
         to read the properties from the file. */
    }

  return SVN_NO_ERROR;
}


/* Simple wrapper around empty_props_p, and inversed. */
svn_error_t *
svn_wc__has_props(svn_boolean_t *has_props,
                  const char *path,
                  svn_wc_adm_access_t *adm_access,
                  apr_pool_t *pool)
{
  svn_boolean_t is_empty;
  const char *prop_path;
  const svn_wc_entry_t *entry;
  svn_boolean_t has_propcaching =
    svn_wc__adm_wc_format(adm_access) > SVN_WC__NO_PROPCACHING_VERSION;

  SVN_ERR(svn_wc_entry(&entry, path, adm_access, FALSE, pool));

  /*### Maybe assert (entry); calling svn_wc__has_props
    for an unversioned path is bogus */
  if (! entry)
    {
      *has_props = FALSE;
      return SVN_NO_ERROR;
    }

  /* Use the flag in the entry if the WC is recent enough. */
  if (has_propcaching)
    {
      *has_props = entry->has_props;
      return SVN_NO_ERROR;
    }

  /* The rest is for compatibility with WCs that don't have propcaching. */

  SVN_ERR(svn_wc__prop_path(&prop_path, path, entry->kind, FALSE, pool));
  SVN_ERR(empty_props_p(&is_empty, prop_path, pool));

  if (is_empty)
    *has_props = FALSE;
  else
    *has_props = TRUE;

  return SVN_NO_ERROR;
}


/* Common implementation for svn_wc_props_modified_p()
   and svn_wc__props_modified().

   Set *MODIFIED_P to true if PATH's properties are modified
   with regard to the base revision, else set MODIFIED_P to false.

   If WHICH_PROPS is non-null and there are prop mods then set
   *WHICH_PROPS to a (const char *propname) ->
   (const svn_string_t *propvalue) key:value mapping of only
   the modified properties. */
static svn_error_t *
modified_props(svn_boolean_t *modified_p,
               const char *path,
               apr_hash_t **which_props,
               svn_wc_adm_access_t *adm_access,
               apr_pool_t *pool)
{
  const char *prop_path;
  const char *prop_base_path;
  const svn_wc_entry_t *entry;
  apr_pool_t *subpool = svn_pool_create(pool);
  int wc_format = svn_wc__adm_wc_format(adm_access);
  svn_boolean_t want_props = which_props ? TRUE : FALSE;

  if (want_props)
    *which_props = apr_hash_make(pool);

  SVN_ERR(svn_wc_entry(&entry, path, adm_access, TRUE, subpool));  

  /* If we have no entry, we can't have any prop mods. */
  if (! entry)
    {
      *modified_p = FALSE;
      goto cleanup;
    }

  /* For newer WCs, if there is an entry for the path, we have a fast
   * and nice way to retrieve the information from the entry. */
  if (wc_format > SVN_WC__NO_PROPCACHING_VERSION)
    {
      /* Only continue if there are prop mods
         and we want to know the details. */
      *modified_p = entry->has_prop_mods;
      if (!*modified_p || !want_props)
        goto cleanup;
    }
      
  /* So, we have a WC in an older format or we have propcaching
     but need to find the specific prop changes.  Either way we
     have some work to do... */

  /* First, get the paths of the working and 'base' prop files. */
  SVN_ERR(svn_wc__prop_path(&prop_path, path, entry->kind, FALSE, subpool));
  SVN_ERR(svn_wc__prop_base_path(&prop_base_path, path, entry->kind, FALSE,
                                 subpool));

  /* Check for numerous easy outs on older WC formats before we
     resort to svn_prop_diffs(). */
  if (wc_format <= SVN_WC__NO_PROPCACHING_VERSION)
    {
      svn_boolean_t bempty, wempty;
      /* Decide if either path is "empty" of properties. */
      SVN_ERR(empty_props_p(&wempty, prop_path, subpool));
      SVN_ERR(empty_props_p(&bempty, prop_base_path, subpool));

      /* If something is scheduled for replacement, we do *not* want to
         pay attention to any base-props;  they might be residual from the
         old deleted file. */
      if (entry->schedule == svn_wc_schedule_replace)
        {
          *modified_p = wempty ? FALSE : TRUE;

          /* Only continue if there are prop mods
             and we want to know the details. */
          if (!*modified_p || !want_props)
            goto cleanup;
        }

      /* Easy out:  if the base file is empty, we know the answer
         immediately. */
      if (bempty)
        {
          if (! wempty)
            {
              /* base is empty, but working is not */
              *modified_p = TRUE;

              /* Only continue if we want to know the details. */
              if (!want_props)
                goto cleanup;
            }
          else
            {
              /* base and working are both empty */
              *modified_p = FALSE;
              goto cleanup;
            }
        }
      /* OK, so the base file is non-empty.  One more easy out: */
      else if (wempty)
        {
          /* base exists, working is empty */
          *modified_p = TRUE;

          /* Only continue if we want to know the details. */
          if (!want_props)
            goto cleanup;
        }
      else
        {
          svn_boolean_t different_filesizes;

          /* At this point, we know both files exists.  Therefore we have no
             choice but to start checking their contents. */

          /* There are at least three tests we can try in succession. */

          /* Easy-answer attempt #1:  (### this stat's the files again) */

          /* Check if the local and prop-base file have *definitely*
             different filesizes. */
          SVN_ERR(svn_io_filesizes_different_p(&different_filesizes,
                                               prop_path,
                                               prop_base_path,
                                               subpool));
          if (different_filesizes) 
            {
              *modified_p = TRUE;

              /* Only continue if we want to know the details. */
              if (!want_props)
                goto cleanup;
            }
          else
            {
              svn_boolean_t equal_timestamps;

              /* Easy-answer attempt #2: (### this stat's the files again) */

              /* See if the local file's prop timestamp is the same as the
                 one recorded in the administrative directory.  */
              SVN_ERR(svn_wc__timestamps_equal_p(&equal_timestamps, path,
                                                 adm_access,
                                                 svn_wc__prop_time,
                                                 subpool));
              if (equal_timestamps)
                {
                  *modified_p = FALSE;
                  goto cleanup;
                }
            }
        }
    } /* wc_format <= SVN_WC__NO_PROPCACHING_VERSION */

  /* If we get here, then we either known we have prop changes and want
     the specific changed props or we have a pre-propcaching WC version
     and still haven't figured out if we even have changes.  Regardless,
     our approach is the same in both cases.
     
     In the pre-propcaching case:
     
       We know that the filesizes are the same,
       but the timestamps are different.  That's still not enough
       evidence to make a correct decision;  we need to look at the
       files' contents directly.

       However, doing a byte-for-byte comparison won't work.  The two
       properties files may have the *exact* same name/value pairs, but
       arranged in a different order.  (Our hashdump format makes no
       guarantees about ordering.)

       Therefore, rather than use contents_identical_p(), we use
       svn_prop_diffs(). */
  {
    apr_array_header_t *local_propchanges;
    apr_hash_t *localprops = apr_hash_make(subpool);
    apr_hash_t *baseprops = apr_hash_make(subpool);

    /* ### Amazingly, this stats the files again! */
    SVN_ERR(svn_wc__load_prop_file(prop_path, localprops, subpool));
    SVN_ERR(svn_wc__load_prop_file(prop_base_path,
                                   baseprops,
                                   subpool));

    /* Don't use the subpool is we are hanging on to the changed props. */
    SVN_ERR(svn_prop_diffs(&local_propchanges, localprops, 
                           baseprops,
                           want_props ? pool : subpool));
                                         
    if (local_propchanges->nelts == 0)
      {
        *modified_p = FALSE;
      }
    else
      {
        *modified_p = TRUE;

        /* Record the changed props if that's what we want. */
        if (want_props)
          {
            int i;
            for (i = 0; i < local_propchanges->nelts; i++)
              {
                svn_prop_t *propt = &APR_ARRAY_IDX(local_propchanges, i,
                                                   svn_prop_t);
                apr_hash_set(*which_props, propt->name,
                             APR_HASH_KEY_STRING, propt->value);
              }
           }
      }
  }
 
 cleanup:
  svn_pool_destroy(subpool);
  
  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__props_modified(const char *path,
                       apr_hash_t **which_props,
                       svn_wc_adm_access_t *adm_access,
                       apr_pool_t *pool)
{
  svn_boolean_t modified_p;
  return modified_props(&modified_p, path, which_props, adm_access, pool);
}


svn_error_t *
svn_wc_props_modified_p(svn_boolean_t *modified_p,
                        const char *path,
                        svn_wc_adm_access_t *adm_access,
                        apr_pool_t *pool)
{
  return modified_props(modified_p, path, NULL, adm_access, pool);
}


svn_error_t *
svn_wc_get_prop_diffs(apr_array_header_t **propchanges,
                      apr_hash_t **original_props,
                      const char *path,
                      svn_wc_adm_access_t *adm_access,
                      apr_pool_t *pool)
{
  const svn_wc_entry_t *entry;
  apr_hash_t *baseprops, *props;
  const char *entryname;

  /*### Maybe assert (entry); calling svn_wc_get_prop_diffs
    for an unversioned path is bogus */
  SVN_ERR(svn_wc_entry(&entry, path, adm_access, FALSE, pool));

  if (! entry)
    {
      if (original_props)
        *original_props = apr_hash_make(pool);

      if (propchanges)
        *propchanges = apr_array_make(pool, 0, sizeof(svn_prop_t));

      return SVN_NO_ERROR;
    }

  if (entry->kind == svn_node_dir)
    {
      SVN_ERR(svn_wc_adm_retrieve(&adm_access, adm_access, path, pool));
      entryname = SVN_WC_ENTRY_THIS_DIR;
    }
  else
    {
      const char *dirname;
      svn_path_split(path, &dirname, &entryname, pool);
      SVN_ERR(svn_wc_adm_retrieve(&adm_access, adm_access, dirname, pool));
    }

  SVN_ERR(svn_wc__load_props(&baseprops, propchanges ? &props : NULL,
                             adm_access, entryname, pool));

  if (original_props != NULL)
    *original_props = baseprops;

  if (propchanges != NULL)
    SVN_ERR(svn_prop_diffs(propchanges, props,
                           baseprops, pool));      

  return SVN_NO_ERROR;
}



/** Externals **/

/* Parse external definitions of the forms:
 *   [rN]   URL  TARGET_DIR
 *   [r N]  URL  TARGET_DIR
 *
 * Return FALSE if there is an error, TRUE otherwise.
 * (We avoid actually creating an error in this function, so that a generic
 *  INVALID_EXTERNALS_DESCRIPTION can be created by the caller.)
 */
static svn_boolean_t
parse_external_parts_with_peg_rev(apr_array_header_t *line_parts,
                                  svn_wc_external_item2_t *item,
                                  apr_pool_t *pool)
{
  svn_error_t *err;
  const char *url;

  if (line_parts->nelts == 2)
    {
      /* No "r REV" given. */
      url = APR_ARRAY_IDX(line_parts, 0, const char *);
      item->target_dir = APR_ARRAY_IDX(line_parts, 1, const char *);
      item->revision.kind = svn_opt_revision_unspecified;
    }
  else if ((line_parts->nelts == 3) || (line_parts->nelts == 4))
    {
      /* We're dealing with one of these two forms:
       *
       *    rN   URL  TARGET_DIR
       *    r N  URL  TARGET_DIR
       * 
       * Handle either way.
       */
      const char *r_part_1 = NULL, *r_part_2 = NULL;

      item->revision.kind = svn_opt_revision_number;

      if (line_parts->nelts == 3)
        {
          r_part_1 = APR_ARRAY_IDX(line_parts, 0, const char *);
          url = APR_ARRAY_IDX(line_parts, 1, const char *);
          item->target_dir = APR_ARRAY_IDX(line_parts, 2, const char *);
        }
      else
        {
          r_part_1 = APR_ARRAY_IDX(line_parts, 0, const char *);
          r_part_2 = APR_ARRAY_IDX(line_parts, 1, const char *);
          url = APR_ARRAY_IDX(line_parts, 2, const char *);
          item->target_dir = APR_ARRAY_IDX(line_parts, 3, const char *);
        }

      if ((! r_part_1) || (r_part_1[0] != '-') || (r_part_1[1] != 'r'))
        return FALSE;

      if (! r_part_2)  /* "rN" */
        {
          if (strlen(r_part_1) < 2)
            return FALSE;
          else
            item->revision.value.number = SVN_STR_TO_REV(r_part_1 + 1);
        }
      else             /* "r N" */
        {
          if (strlen(r_part_2) < 1)
            return FALSE;
          else
            item->revision.value.number = SVN_STR_TO_REV(r_part_2);
        }
    }
  else  /* too many items */
    return FALSE;

  err = svn_opt_parse_path(&item->peg_revision, &item->url, url, pool);
  if (err)
    {
      svn_error_clear(err);
      return FALSE;
    }

  return TRUE;
}

/* Parse one of these two forms:
 * 
 *    TARGET_DIR  [-rN]  URL
 *    TARGET_DIR  [-r N]  URL
 *
 * Return FALSE if there is an error, TRUE otherwise.
 * (We avoid actually creating an error in this function, so that a generic
 *  INVALID_EXTERNALS_DESCRIPTION can be created by the caller.)
 */
static svn_boolean_t
parse_external_parts(apr_array_header_t *line_parts,
                     svn_wc_external_item2_t *item,
                     apr_pool_t *pool)
{

  if (line_parts->nelts == 2)
    {
      /* No "-r REV" given. */
      item->target_dir = APR_ARRAY_IDX(line_parts, 0, const char *);
      item->url = APR_ARRAY_IDX(line_parts, 1, const char *);
      item->revision.kind = svn_opt_revision_head;
    }
  else if ((line_parts->nelts == 3) || (line_parts->nelts == 4))
    {
      /* We're dealing with one of these two forms:
       *
       *    TARGET_DIR  -rN  URL
       *    TARGET_DIR  -r N  URL
       * 
       * Handle either way.
       */
      const char *r_part_1 = NULL, *r_part_2 = NULL;

      item->target_dir = APR_ARRAY_IDX(line_parts, 0, const char *);
      item->revision.kind = svn_opt_revision_number;

      if (line_parts->nelts == 3)
        {
          r_part_1 = APR_ARRAY_IDX(line_parts, 1, const char *);
          item->url = APR_ARRAY_IDX(line_parts, 2, const char *);
        }
      else
        {
          r_part_1 = APR_ARRAY_IDX(line_parts, 1, const char *);
          r_part_2 = APR_ARRAY_IDX(line_parts, 2, const char *);
          item->url = APR_ARRAY_IDX(line_parts, 3, const char *);
        }

      if ((! r_part_1) || (r_part_1[0] != '-') || (r_part_1[1] != 'r'))
        return FALSE;

      if (! r_part_2)  /* "-rN" */
        {
          if (strlen(r_part_1) < 3)
            return FALSE;
          else
            item->revision.value.number = SVN_STR_TO_REV(r_part_1 + 2);
        }
      else             /* "-r N" */
        {
          if (strlen(r_part_2) < 1)
            return FALSE;
          else
            item->revision.value.number = SVN_STR_TO_REV(r_part_2);
        }
    }
  else  /* too many items */
    return FALSE;

  item->peg_revision = item->revision;

  return TRUE;
}

svn_error_t *
svn_wc_parse_externals_description3(apr_array_header_t **externals_p,
                                    const char *parent_directory,
                                    const char *desc,
                                    apr_pool_t *pool)
{
  apr_array_header_t *lines = svn_cstring_split(desc, "\n\r", TRUE, pool);
  int i;
  const char *parent_directory_display = svn_path_is_url(parent_directory) ?
    parent_directory : svn_path_local_style(parent_directory, pool);
  
  if (externals_p)
    *externals_p = apr_array_make(pool, 1, sizeof(svn_wc_external_item2_t *));

  for (i = 0; i < lines->nelts; i++)
    {
      const char *line = APR_ARRAY_IDX(lines, i, const char *);
      apr_array_header_t *line_parts;
      svn_wc_external_item2_t *item;

      if ((! line) || (line[0] == '#'))
        continue;

      /* else proceed */

      line_parts = svn_cstring_split(line, " \t", TRUE, pool);

      SVN_ERR(svn_wc_external_item_create
              ((const svn_wc_external_item2_t **) &item, pool));
      item->revision.kind = svn_opt_revision_unspecified;

      if (line_parts->nelts < 2)
        goto parse_error;

      else 
        {
          const char *token = APR_ARRAY_IDX(line_parts, 0, const char *);

          if ( (token[0] == '-' && token[1] == 'r') || svn_path_is_url(token) )
            {
              if (! parse_external_parts_with_peg_rev(line_parts, item, pool) )
                goto parse_error;

              SVN_ERR(svn_opt_resolve_revisions(&item->peg_revision,
                                                &item->revision, TRUE, FALSE,
                                                pool));
            }
          else
            {
              if (! parse_external_parts(line_parts, item, pool) )
                goto parse_error;
            }
        }

      if (0)
        {
        parse_error:
          return svn_error_createf
            (SVN_ERR_CLIENT_INVALID_EXTERNALS_DESCRIPTION, NULL,
             _("Error parsing %s property on '%s': '%s'"),
             SVN_PROP_EXTERNALS,
             parent_directory_display,
             line);
        }

      item->target_dir = svn_path_canonicalize
        (svn_path_internal_style(item->target_dir, pool), pool);
      {
        if (item->target_dir[0] == '\0' || item->target_dir[0] == '/'
            || svn_path_is_backpath_present(item->target_dir))
          return svn_error_createf
            (SVN_ERR_CLIENT_INVALID_EXTERNALS_DESCRIPTION, NULL,
             _("Invalid %s property on '%s': "
               "target involves '.' or '..' or is an absolute path"),
             SVN_PROP_EXTERNALS,
             parent_directory_display);
      }

      SVN_ERR(svn_opt_parse_path(&item->peg_revision, &item->url, item->url,
                                 pool));
    
      item->url = svn_path_canonicalize(item->url, pool);

      if (externals_p)
        APR_ARRAY_PUSH(*externals_p, svn_wc_external_item2_t *) = item;
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_wc_parse_externals_description2(apr_array_header_t **externals_p,
                                    const char *parent_directory,
                                    const char *desc,
                                    apr_pool_t *pool)
{
  apr_array_header_t *list;
  apr_pool_t *subpool = svn_pool_create(pool);
  int i;

  SVN_ERR(svn_wc_parse_externals_description3(externals_p ? &list : NULL,
                                              parent_directory, desc, subpool));

  if (externals_p)
    {
      *externals_p = apr_array_make(pool, list->nelts,
                                    sizeof(svn_wc_external_item_t *));
      for (i = 0; i < list->nelts; i++)
        {
          svn_wc_external_item2_t *item2 = APR_ARRAY_IDX(list, i,
                                             svn_wc_external_item2_t *);
          svn_wc_external_item_t *item = apr_palloc(pool, sizeof (*item));

          if (item->target_dir)
            item->target_dir = apr_pstrdup(pool, item2->target_dir);
          if (item->url)
            item->url = apr_pstrdup(pool, item2->url);
          item->revision = item2->revision;

          APR_ARRAY_PUSH(*externals_p, svn_wc_external_item_t *) = item;
        }
    }

  svn_pool_destroy(subpool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc_parse_externals_description(apr_hash_t **externals_p,
                                   const char *parent_directory,
                                   const char *desc,
                                   apr_pool_t *pool)
{
  apr_array_header_t *list;
  int i;

  SVN_ERR(svn_wc_parse_externals_description2(externals_p ? &list : NULL,
                                              parent_directory, desc, pool));

  /* Store all of the items into the hash if that was requested. */
  if (externals_p)
    {
      *externals_p = apr_hash_make(pool);
      for (i = 0; i < list->nelts; i++)
        {
          svn_wc_external_item_t *item;
          item = APR_ARRAY_IDX(list, i, svn_wc_external_item_t *);

          apr_hash_set(*externals_p, item->target_dir,
                       APR_HASH_KEY_STRING, item);
        }
    }
  return SVN_NO_ERROR;
}

svn_boolean_t
svn_wc__has_special_property(apr_hash_t *props)
{
  return apr_hash_get(props, SVN_PROP_SPECIAL, APR_HASH_KEY_STRING) != NULL;
}

svn_boolean_t
svn_wc__has_magic_property(const apr_array_header_t *properties)
{
  int i;

  for (i = 0; i < properties->nelts; i++)
    {
      const svn_prop_t *property = &APR_ARRAY_IDX(properties, i, svn_prop_t);

      if (strcmp(property->name, SVN_PROP_EXECUTABLE) == 0
          || strcmp(property->name, SVN_PROP_KEYWORDS) == 0
          || strcmp(property->name, SVN_PROP_EOL_STYLE) == 0
          || strcmp(property->name, SVN_PROP_SPECIAL) == 0
          || strcmp(property->name, SVN_PROP_NEEDS_LOCK) == 0)
        return TRUE;
    }
  return FALSE;
}
