/* rev-table.c : working with the `revisions' table
 *
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

#include <db.h>
#include "svn_fs.h"
#include "fs.h"
#include "err.h"
#include "dbt.h"
#include "skel.h"
#include "validate.h"
#include "rev-table.h"


/* Opening/creating the `revisions' table.  */

int svn_fs__open_revisions_table (DB **revisions_p,
                                  DB_ENV *env,
                                  int create)
{
  DB *revisions;

  DB_ERR (db_create (&revisions, env, 0));
  DB_ERR (revisions->open (revisions, "revisions", 0, DB_RECNO,
                           create ? (DB_CREATE | DB_EXCL) : 0,
                           0666));

  *revisions_p = revisions;
  return 0;
}



/* Storing and retrieving filesystem revisions.  */


static int
is_valid_filesystem_revision (skel_t *skel)
{
  int len = svn_fs__list_length (skel);

  if (len == 3)
    {
      if (svn_fs__matches_atom (skel->children, "revision")
          && skel->children->next != NULL
          && svn_fs__is_valid_proplist (skel->children->next->next))
        {
          skel_t *id = skel->children->next;
          if (id->is_atom
              && 0 == (1 & svn_fs__count_id_components (id->data, id->len)))
            return 1;
        }
    }

  return 0;
}


svn_error_t *
svn_fs__get_rev (skel_t **skel_p,
                 svn_fs_t *fs,
                 svn_revnum_t rev,
                 trail_t *trail)
{
  int db_err;
  DBT key, value;
  skel_t *skel;

  /* Turn the revision number into a Berkeley DB record number.
     Revisions are numbered starting with zero; Berkeley DB record
     numbers begin with one.  */
  db_recno_t recno = rev + 1;

  db_err = fs->revisions->get (fs->revisions, trail->db_txn,
                               svn_fs__set_dbt (&key, &recno, sizeof (recno)),
                               svn_fs__result_dbt (&value),
                               0);
  svn_fs__track_dbt (&value, trail->pool);

  /* If there's no such revision, return an appropriately specific error.  */
  if (db_err == DB_NOTFOUND)
    return svn_fs__err_dangling_rev (fs, rev);

  /* Handle any other error conditions.  */
  SVN_ERR (DB_WRAP (fs, "reading filesystem revision", db_err));

  /* Parse and check the REVISION skel.  */
  skel = svn_fs__parse_skel (value.data, value.size, trail->pool);
  if (! skel
      || ! is_valid_filesystem_revision (skel))
    return svn_fs__err_corrupt_fs_revision (fs, rev);

  *skel_p = skel;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__put_rev (svn_revnum_t *rev,
                 svn_fs_t *fs,
                 skel_t *skel,
                 trail_t *trail)
{
  int db_err;
  DBT key, value;
  db_recno_t recno = 0;

  /* xbc FIXME: Need a useful revision number here. */
  if (! is_valid_filesystem_revision (skel))
    return svn_fs__err_corrupt_fs_revision (fs, -1);

  db_err = fs->revisions->put (fs->revisions, trail->db_txn,
                               svn_fs__recno_dbt(&key, &recno),
                               svn_fs__skel_to_dbt (&value, skel, trail->pool),
                               DB_APPEND);
  SVN_ERR (DB_WRAP (fs, "storing filesystem revision", db_err));

  /* Turn the record number into a Subversion revision number.
     Revisions are numbered starting with zero; Berkeley DB record
     numbers begin with one.  */
  *rev = recno - 1;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__rev_get_root (svn_fs_id_t **root_id_p,
                      svn_fs_t *fs,
                      svn_revnum_t rev,
                      trail_t *trail)
{
  skel_t *skel;
  svn_fs_id_t *id;

  SVN_ERR (svn_fs__get_rev (&skel, fs, rev, trail));

  id = svn_fs_parse_id (skel->children->next->data,
                        skel->children->next->len,
                        trail->pool);

  /* The skel validator doesn't check the ID format. */
  if (id == NULL)
    return svn_fs__err_corrupt_fs_revision (fs, -1);

  *root_id_p = id;
  return SVN_NO_ERROR;
}




/* Getting the youngest revision.  */


struct youngest_rev_args {
  svn_revnum_t *youngest_p;
  svn_fs_t *fs;
};


static svn_error_t *
txn_body_youngest_rev (void *baton,
                       trail_t *trail)
{
  struct youngest_rev_args *args = baton;

  int db_err;
  DBC *cursor = 0;
  DBT key, value;
  db_recno_t recno;

  svn_fs_t *fs = args->fs;

  /* Create a database cursor.  */
  SVN_ERR (DB_WRAP (fs, "getting youngest revision (creating cursor)",
                    fs->revisions->cursor (fs->revisions, trail->db_txn,
                                           &cursor, 0)));

  /* Find the last entry in the `revisions' table.  */
  db_err = cursor->c_get (cursor,
                          svn_fs__recno_dbt (&key, &recno),
                          svn_fs__nodata_dbt (&value),
                          DB_LAST);

  if (db_err)
    {
      /* Free the cursor.  Ignore any error value --- the error above
         is more interesting.  */
      cursor->c_close (cursor);

      if (db_err == DB_NOTFOUND)
        /* The revision 0 should always be present, at least.  */
        return
          svn_error_createf
          (SVN_ERR_FS_CORRUPT, 0, 0, fs->pool,
           "revision 0 missing from `revisions' table, in filesystem `%s'",
           fs->env_path);
      
      SVN_ERR (DB_WRAP (fs, "getting youngest revision (finding last entry)",
                        db_err));
    }

  /* You can't commit a transaction with open cursors, because:
     1) key/value pairs don't get deleted until the cursors referring
     to them are closed, so closing a cursor can fail for various
     reasons, and txn_commit shouldn't fail that way, and 
     2) using a cursor after committing its transaction can cause
     undetectable database corruption.  */
  SVN_ERR (DB_WRAP (fs, "getting youngest revision (closing cursor)",
                    cursor->c_close (cursor)));

  /* Turn the record number into a Subversion revision number.
     Revisions are numbered starting with zero; Berkeley DB record
     numbers begin with one.  */
  *args->youngest_p = recno - 1;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_youngest_rev (svn_revnum_t *youngest_p,
                     svn_fs_t *fs,
                     apr_pool_t *pool)
{
  svn_revnum_t youngest;
  struct youngest_rev_args args;

  SVN_ERR (svn_fs__check_fs (fs));

  args.youngest_p = &youngest;
  args.fs = fs;

  SVN_ERR (svn_fs__retry_txn (fs, txn_body_youngest_rev, &args, pool));

  *youngest_p = youngest;
  return SVN_NO_ERROR;
}



/* Generic revision operations.  */


struct revision_prop_args {
  svn_string_t **value_p;
  svn_fs_t *fs;
  svn_revnum_t rev;
  svn_string_t *propname;
};


static svn_error_t *
txn_body_revision_prop (void *baton,
                        trail_t *trail)
{
  struct revision_prop_args *args = baton;

  skel_t *skel;
  skel_t *proplist, *prop;

  SVN_ERR (svn_fs__get_rev (&skel, args->fs, args->rev, trail));

  /* PROPLIST is the third element of revision skel.  */
  proplist = skel->children->next->next;

  /* Search the proplist for a property with the right name.  */
  for (prop = proplist->children; prop; prop = prop->next->next)
    {
      skel_t *name = prop;
      skel_t *value = prop->next;

      if (svn_fs__atom_matches_string (name, args->propname))
        {
          *(args->value_p) = svn_string_ncreate (value->data, value->len,
                                                 trail->pool);
          return SVN_NO_ERROR;
        }
    }

  *(args->value_p) = 0;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_revision_prop (svn_string_t **value_p,
                      svn_fs_t *fs,
                      svn_revnum_t rev,
                      svn_string_t *propname,
                      apr_pool_t *pool)
{
  struct revision_prop_args args;
  svn_string_t *value;

  SVN_ERR (svn_fs__check_fs (fs));

  args.value_p = &value;
  args.fs = fs;
  args.rev = rev;
  args.propname = propname;
  SVN_ERR (svn_fs__retry_txn (fs, txn_body_revision_prop, &args, pool));

  *value_p = value;
  return SVN_NO_ERROR;
}


struct revision_proplist_args {
  apr_hash_t **table_p;
  svn_fs_t *fs;
  svn_revnum_t rev;
};


static svn_error_t *
txn_body_revision_proplist (void *baton, trail_t *trail)
{
  struct revision_proplist_args *args = baton;

  skel_t *skel;
  skel_t *proplist, *prop;
  apr_hash_t *table;

  SVN_ERR (svn_fs__get_rev (&skel, args->fs, args->rev, trail));

  /* PROPLIST is the third element of revision skel.  */
  proplist = skel->children->next->next;

  /* Build a hash table from the property list.  */
  table = apr_hash_make (trail->pool);
  for (prop = proplist->children; prop; prop = prop->next->next)
    {
      skel_t *name = prop;
      skel_t *value = prop->next;

      apr_hash_set (table, name->data, name->len,
                    svn_string_ncreate (value->data, value->len,
                                        trail->pool));
    }

  *args->table_p = table;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_revision_proplist (apr_hash_t **table_p,
                          svn_fs_t *fs,
                          svn_revnum_t rev,
                          apr_pool_t *pool)
{
  struct revision_proplist_args args;
  apr_hash_t *table;

  SVN_ERR (svn_fs__check_fs (fs));

  args.table_p = &table;
  args.fs = fs;
  args.rev = rev;
  SVN_ERR (svn_fs__retry_txn (fs, txn_body_revision_proplist, &args, pool));

  *table_p = table;
  return SVN_NO_ERROR;
}


struct change_rev_prop_args {
  svn_fs_t *fs;
  svn_revnum_t rev;
  svn_string_t *name;
  svn_string_t *value;
};


static svn_error_t *
txn_body_change_rev_prop (void *baton, trail_t *trail)
{
  struct change_rev_prop_args *args = baton;

  svn_fs_t *fs = args->fs;
  skel_t *skel;
  skel_t *proplist, *prop;
  skel_t *prev = NULL;

  SVN_ERR (svn_fs__get_rev (&skel, fs, args->rev, trail));

  /* PROPLIST is the third element of revision skel.  */
  proplist = skel->children->next->next;

  /* Delete the skel, either replacing or adding the given property.  */
  for (prop = proplist->children; prop; prop = prop->next->next)
    {
      skel_t *name = prop;
      skel_t *value = prop->next;

      if (svn_fs__atom_matches_string (name, args->name))
        {
          /* We've found the property we wish to change. */
          if (! args->value)
            {
              /* If our new value for this is NULL, we'll remove the
                 property altogether by effectively routing our linked
                 list of properties around the current property
                 name/value pair. */

              if (prev)
                {
                  /* If this isn't the first pair in the list, this
                     can be done by setting the previous value's next
                     pointer to the name of the following property
                     pair, if one exists, or zero if we are removing
                     the last name/value pair currently in the
                     list. */
                  if (prop->next)
                    prev->next->next = prop->next->next;
                  else
                    prev->next->next = 0;
                }
              else
                {
                  /* If, however, this is the first item in the list,
                     we'll set the children pointer of the PROPLIST
                     skel to the following name/value pair, if one
                     exists, or zero if we're removing the only
                     property pair in the list. */
                  if (prop->next)
                    proplist->children = prop->next->next;
                  else
                    proplist->children = 0;
                }
            }
          else
            {
              value->data = args->value->data;
              value->len = args->value->len;
            }

          /* Regardless of what we changed, we're done editing the
             list now that we've acted on the property we found. */
          break;
        }

      /* Squirrel away a pointer to this property name/value pair, as
         we may need this in the next iteration of this loop. */
      prev = prop;
    }

  if (! prop)
    {
      /* The property we were seeking to change is not currently in
         the property list, so well add its name and desired value to
         the beginning of the property list. */
      svn_fs__prepend (svn_fs__mem_atom (args->value->data,
                                         args->value->len,
                                         trail->pool),
                       proplist);
      svn_fs__prepend (svn_fs__mem_atom (args->name->data,
                                         args->name->len,
                                         trail->pool),
                       proplist);
    }

  {
    int db_err;
    DBT key, value;
    db_recno_t recno = args->rev + 1;

    /* Update the filesystem revision with the new skel that reflects
       our property edits. */
    db_err = fs->revisions->put 
      (fs->revisions, trail->db_txn,
       svn_fs__set_dbt (&key, &recno, sizeof (recno)),
       svn_fs__skel_to_dbt (&value, skel, trail->pool), 0);
    SVN_ERR (DB_WRAP (fs, "updating filesystem revision", db_err));
  }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_change_rev_prop (svn_fs_t *fs,
                        svn_revnum_t rev,
                        svn_string_t *name,
                        svn_string_t *value,
                        apr_pool_t *pool)
{
  struct change_rev_prop_args args;

  SVN_ERR (svn_fs__check_fs (fs));

  args.fs = fs;
  args.rev = rev;
  args.name = name;
  args.value = value;
  SVN_ERR (svn_fs__retry_txn (fs, txn_body_change_rev_prop, &args, pool));

  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
