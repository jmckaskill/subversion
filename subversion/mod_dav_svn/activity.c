/*
 * activity.c: DeltaV activity handling
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



#include <httpd.h>
#include <mod_dav.h>

#include <apr_dbm.h>

#include "svn_string.h"
#include "svn_path.h"

#include "dav_svn.h"

#define ACTIVITY_DB     "activities"


const char *dav_svn_get_txn(dav_svn_repos *repos, const char *activity_id)
{
  apr_dbm_t *dbm;
  apr_status_t status;
  svn_string_t *pathname;
  apr_datum_t key;
  apr_datum_t value;
  const char *txn_name;

  pathname = svn_string_create(repos->fs_path, repos->pool);
  svn_path_add_component_nts(pathname, ACTIVITY_DB, svn_path_local_style);
  status = apr_dbm_open(&dbm, pathname->data, APR_DBM_READONLY, repos->pool);
  if (status != APR_SUCCESS)
    {
      /* ### let's just assume that any error means the DB doesn't exist,
         ### therefore, the activity/transaction doesn't exist */
      return NULL;
    }

  key.dptr = (char *)activity_id;
  key.dsize = strlen(activity_id) + 1;  /* null-term'd */
  status = apr_dbm_fetch(dbm, key, &value);
  if (status != APR_SUCCESS)
    {
      /* ### again: assume failure means it doesn't exist */
      apr_dbm_close(dbm);
      return NULL;
    }

  txn_name = apr_pstrdup(repos->pool, value.dptr);   /* null-term'd */
  apr_dbm_freedatum(dbm, value);

  apr_dbm_close(dbm);

  return txn_name;
}

dav_error *dav_svn_store_activity(dav_svn_repos *repos,
                                  const char *activity_id,
                                  const char *txn_name)
{
  apr_dbm_t *dbm;
  apr_status_t status;
  svn_string_t *pathname;
  apr_datum_t key;
  apr_datum_t value;

  pathname = svn_string_create(repos->fs_path, repos->pool);
  svn_path_add_component_nts(pathname, ACTIVITY_DB, svn_path_local_style);
  status = apr_dbm_open(&dbm, pathname->data, APR_DBM_RWCREATE, repos->pool);
  if (status != APR_SUCCESS)
    {
      /* ### return an error */
      return NULL;
    }

  key.dptr = (char *)activity_id;
  key.dsize = strlen(activity_id) + 1;  /* null-term'd */
  value.dptr = (char *)txn_name;
  value.dsize = strlen(txn_name) + 1;   /* null-term'd */
  status = apr_dbm_store(dbm, key, value);
  apr_dbm_close(dbm);
  if (status != APR_SUCCESS)
    {
      /* ### return an error */
      return NULL;
    }

  return NULL;
}


/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
