/*
 * log.c:  handle the adm area's log file.
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */



#include <string.h>

#include <apr_pools.h>
#include <apr_strings.h>

#include "svn_wc.h"
#include "svn_error.h"
#include "svn_string.h"
#include "svn_xml.h"
#include "svn_pools.h"
#include "svn_io.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_time.h"
#include "svn_iter.h"

#include "wc.h"
#include "log.h"
#include "props.h"
#include "adm_files.h"
#include "lock.h"
#include "translate.h"
#include "tree_conflicts.h"
#include "workqueue.h"

#include "private/svn_wc_private.h"
#include "private/svn_skel.h"
#include "svn_private_config.h"


/*** Constant definitions for xml generation/parsing ***/

/* Note: every entry in the logfile is either idempotent or atomic.
 * This allows us to remove the entire logfile when every entry in it
 * has been completed -- if you crash in the middle of running a
 * logfile, and then later are running over it again as part of the
 * recovery, a given entry is "safe" in the sense that you can either
 * tell it has already been done (in which case, ignore it) or you can
 * do it again without ill effect.
 *
 * All log commands are self-closing tags with attributes.
 */


/** Log actions. **/

/* Delete the entry SVN_WC__LOG_ATTR_NAME. */
#define SVN_WC__LOG_DELETE_ENTRY        "delete-entry"
#define SVN_WC__LOG_ATTR_REVISION       "revision"
#define SVN_WC__LOG_ATTR_KIND           "kind"


/** Log attributes.  See the documentation above for log actions for
    how these are used. **/

#define SVN_WC__LOG_ATTR_NAME           "name"
#define SVN_WC__LOG_ATTR_DATA           "data"


/*** Userdata for the callbacks. ***/
struct log_runner
{
  svn_wc__db_t *db;
  const char *adm_abspath;

  apr_pool_t *pool; /* cleared before processing each log element */

  svn_xml_parser_t *parser;
};


/* The log body needs to be wrapped in a single, root element to satisfy
   the Expat parser. These two macros provide the start/end wrapprs.  */
#define LOG_START "<wc-log xmlns=\"http://subversion.tigris.org/xmlns\">\n"
#define LOG_END "</wc-log>\n"

/* For log debugging. Generates output about its operation.  */
/* #define DEBUG_LOG */


/* Helper macro for erroring out while running a logfile.

   This is implemented as a macro so that the error created has a useful
   line number associated with it. */
#define SIGNAL_ERROR(loggy, err)                                        \
  svn_xml_signal_bailout(svn_error_createf(                             \
                           SVN_ERR_WC_BAD_ADM_LOG, err,                 \
                           _("In directory '%s'"),                      \
                           svn_dirent_local_style(loggy->adm_abspath,   \
                                                  loggy->pool)),        \
                         loggy->parser)



/* Ben sez:  this log command is (at the moment) only executed by the
   update editor.  It attempts to forcefully remove working data. */
/* Delete a node from version control, and from disk if unmodified.
 * LOCAL_ABSPATH is the name of the file or directory to be deleted.
 * If it is unversioned,
 * do nothing and return no error. Otherwise, delete its WC entry and, if
 * the working version is unmodified, delete it from disk. */
static svn_error_t *
basic_delete_entry(svn_wc__db_t *db,
                   const char *local_abspath,
                   apr_pool_t *scratch_pool)
{
  svn_wc__db_kind_t kind;
  svn_boolean_t hidden;
  svn_error_t *err;

  /* Figure out if 'name' is a dir or a file */
  SVN_ERR(svn_wc__db_read_kind(&kind, db, local_abspath, TRUE, scratch_pool));
  if (kind == svn_wc__db_kind_unknown)
    return SVN_NO_ERROR; /* Already gone */

  SVN_ERR(svn_wc__db_node_hidden(&hidden, db, local_abspath, scratch_pool));
  if (hidden)
    return SVN_NO_ERROR;

  /* Remove the object from revision control -- whether it's a
     single file or recursive directory removal.  Attempt
     to destroy all working files & dirs too.

     ### We pass NULL, NULL for cancel_func and cancel_baton below.
     ### If they were available, it would be nice to use them. */
  if (kind == svn_wc__db_kind_dir)
    {
      svn_wc__db_status_t status;

      SVN_ERR(svn_wc__db_read_info(&status, NULL, NULL, NULL, NULL, NULL,
                                      NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                      NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                      NULL, NULL, NULL, NULL,
                                   db, local_abspath,
                                   scratch_pool, scratch_pool));
      if (status == svn_wc__db_status_obstructed ||
          status == svn_wc__db_status_obstructed_add ||
          status == svn_wc__db_status_obstructed_delete)
        {
          /* Removing a missing wcroot is easy, just remove its parent entry
             ### BH: I can't tell why we don't use this for adds.
                     We might want to remove WC obstructions?

             We don't have a missing status in the final version of WC-NG,
             so why bother researching its history.
          */
          if (status != svn_wc__db_status_obstructed_add)
            {
              SVN_ERR(svn_wc__db_temp_op_remove_entry(db, local_abspath,
                                                      scratch_pool));

              return SVN_NO_ERROR;
            }
        }
    }

  err = svn_wc__internal_remove_from_revision_control(db,
                                                      local_abspath,
                                                      TRUE, /* destroy */
                                                      FALSE, /* instant_error*/
                                                      NULL, NULL,
                                                      scratch_pool);

  if (err && err->apr_err == SVN_ERR_WC_LEFT_LOCAL_MOD)
    {
      svn_error_clear(err);
      return SVN_NO_ERROR;
    }
  else
    {
      return svn_error_return(err);
    }
}


static svn_error_t *
log_do_delete_entry(struct log_runner *loggy,
                    const char *name,
                    svn_revnum_t revision,
                    svn_node_kind_t kind)
{
  const char *local_abspath;
  const char *repos_relpath, *repos_root, *repos_uuid;

  local_abspath = svn_dirent_join(loggy->adm_abspath, name, loggy->pool);

  if (SVN_IS_VALID_REVNUM(revision))
    SVN_ERR(svn_wc__db_scan_base_repos(&repos_relpath, &repos_root,
                                       &repos_uuid, loggy->db, local_abspath,
                                       loggy->pool, loggy->pool));

  SVN_ERR(basic_delete_entry(loggy->db, local_abspath, loggy->pool));

  if (SVN_IS_VALID_REVNUM(revision))
    {
      SVN_ERR(svn_wc__db_base_add_absent_node(loggy->db,
                                              local_abspath,
                                              repos_relpath,
                                              repos_root,
                                              repos_uuid,
                                              revision,
                                              kind == svn_node_dir 
                                                   ? svn_wc__db_kind_dir
                                                   : svn_wc__db_kind_file,
                                              svn_wc__db_status_not_present,
                                              NULL,
                                              NULL,
                                              loggy->pool));
    }

  return SVN_NO_ERROR;
}

/* */
static void
start_handler(void *userData, const char *eltname, const char **atts)
{
  svn_error_t *err = SVN_NO_ERROR;
  struct log_runner *loggy = userData;

  /* Most elements use the `name' attribute, so grab it now. */
  const char *name = svn_xml_get_attr_value(SVN_WC__LOG_ATTR_NAME, atts);

  /* Clear the per-log-item pool. */
  svn_pool_clear(loggy->pool);

  if (strcmp(eltname, "wc-log") == 0)   /* ignore expat pacifier */
    return;

  /* The NAME attribute should be present.  */
  SVN_ERR_ASSERT_NO_RETURN(name != NULL);

#ifdef DEBUG_LOG
  SVN_DBG(("start_handler: name='%s'\n", eltname));
#endif

  /* Dispatch. */
  if (strcmp(eltname, SVN_WC__LOG_DELETE_ENTRY) == 0) {
    const char *attr;
    svn_revnum_t revision;
    svn_node_kind_t kind;

    attr = svn_xml_get_attr_value(SVN_WC__LOG_ATTR_REVISION, atts);
    revision = SVN_STR_TO_REV(attr);
    attr = svn_xml_get_attr_value(SVN_WC__LOG_ATTR_KIND, atts);
    if (strcmp(attr, "dir") == 0)
      kind = svn_node_dir;
    else
      kind = svn_node_file;
    err = log_do_delete_entry(loggy, name, revision, kind);
  }
  else
    {
      SIGNAL_ERROR
        (loggy, svn_error_createf
         (SVN_ERR_WC_BAD_ADM_LOG, NULL,
          _("Unrecognized logfile element '%s' in '%s'"),
          eltname,
          svn_dirent_local_style(loggy->adm_abspath, loggy->pool)));
      return;
    }

  if (err)
    SIGNAL_ERROR
      (loggy, svn_error_createf
       (SVN_ERR_WC_BAD_ADM_LOG, err,
        _("Error processing command '%s' in '%s'"),
        eltname,
        svn_dirent_local_style(loggy->adm_abspath, loggy->pool)));

  return;
}



/*** Using the parser to run the log file. ***/


/* Run a sequence of log files. */
svn_error_t *
svn_wc__run_xml_log(svn_wc__db_t *db,
                    const char *adm_abspath,
                    const char *log_contents,
                    apr_size_t log_len,
                    apr_pool_t *scratch_pool)
{
  svn_xml_parser_t *parser;
  struct log_runner *loggy;

  loggy = apr_pcalloc(scratch_pool, sizeof(*loggy));

  parser = svn_xml_make_parser(loggy, start_handler, NULL, NULL,
                               scratch_pool);

  loggy->db = db;
  loggy->adm_abspath = adm_abspath;
  loggy->pool = svn_pool_create(scratch_pool);
  loggy->parser = parser;

  /* Expat wants everything wrapped in a top-level form, so start with
     a ghost open tag. */
  SVN_ERR(svn_xml_parse(parser, LOG_START, strlen(LOG_START), 0));

  SVN_ERR(svn_xml_parse(parser, log_contents, log_len, 0));

  /* Pacify Expat with a pointless closing element tag. */
  SVN_ERR(svn_xml_parse(parser, LOG_END, strlen(LOG_END), 1));

  svn_xml_free_parser(parser);

  return SVN_NO_ERROR;
}


/* Return (in *RELPATH) the portion of ABSPATH that is relative to the
   working copy directory ADM_ABSPATH, or SVN_WC_ENTRY_THIS_DIR if ABSPATH
   is that directory. ABSPATH must within ADM_ABSPATH.  */
static svn_error_t *
loggy_path(const char **relpath,
           const char *abspath,
           const char *adm_abspath,
           apr_pool_t *scratch_pool)
{
  *relpath = svn_dirent_is_child(adm_abspath, abspath, NULL);

  if (*relpath == NULL)
    {
      SVN_ERR_ASSERT(strcmp(abspath, adm_abspath) == 0);

      *relpath = "";
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__loggy_delete_entry(svn_skel_t **work_item,
                           svn_wc__db_t *db,
                           const char *adm_abspath,
                           const char *local_abspath,
                           svn_revnum_t revision,
                           svn_wc__db_kind_t kind,
                           apr_pool_t *result_pool)
{
  const char *loggy_path1;
  svn_stringbuf_t *log_accum = NULL;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(loggy_path(&loggy_path1, local_abspath, adm_abspath, result_pool));
  svn_xml_make_open_tag(&log_accum, result_pool, svn_xml_self_closing,
                        SVN_WC__LOG_DELETE_ENTRY,
                        SVN_WC__LOG_ATTR_NAME,
                        loggy_path1,
                        SVN_WC__LOG_ATTR_REVISION,
                        apr_psprintf(result_pool, "%ld", revision),
                        SVN_WC__LOG_ATTR_KIND,
                        kind == svn_wc__db_kind_dir ? "dir" : "file",
                        NULL);

  return svn_error_return(svn_wc__wq_build_loggy(work_item,
                                                 db, adm_abspath, log_accum,
                                                 result_pool));
}

/*** Recursively do log things. ***/

/* */
static svn_error_t *
can_be_cleaned(int *wc_format,
               svn_wc__db_t *db,
               const char *local_abspath,
               apr_pool_t *scratch_pool)
{
  SVN_ERR(svn_wc__internal_check_wc(wc_format, db,
                                    local_abspath, FALSE, scratch_pool));

  /* a "version" of 0 means a non-wc directory */
  if (*wc_format == 0)
    return svn_error_createf(SVN_ERR_WC_NOT_WORKING_COPY, NULL,
                             _("'%s' is not a working copy directory"),
                             svn_dirent_local_style(local_abspath,
                                                    scratch_pool));

  if (*wc_format < SVN_WC__WC_NG_VERSION)
    return svn_error_create(SVN_ERR_WC_UNSUPPORTED_FORMAT, NULL,
                            _("Log format too old, please use "
                              "Subversion 1.6 or earlier"));

  return SVN_NO_ERROR;
}


/* */
static svn_error_t *
cleanup_internal(svn_wc__db_t *db,
                 const char *adm_abspath,
                 svn_cancel_func_t cancel_func,
                 void *cancel_baton,
                 apr_pool_t *scratch_pool)
{
  int wc_format;
#ifdef SVN_WC__SINGLE_DB
  const char *cleanup_abspath;
#else
  const apr_array_header_t *children;
  int i;
#endif
  apr_pool_t *iterpool = svn_pool_create(scratch_pool);

  /* Check cancellation; note that this catches recursive calls too. */
  if (cancel_func)
    SVN_ERR(cancel_func(cancel_baton));

  /* Can we even work with this directory?  */
  SVN_ERR(can_be_cleaned(&wc_format, db, adm_abspath, iterpool));

#ifdef SVN_WC__SINGLE_DB
  /* ### This fails if ADM_ABSPATH is locked indirectly via a
     ### recursive lock on an ancestor. */
  SVN_ERR(svn_wc__db_wclock_obtain(db, adm_abspath, -1, TRUE, iterpool));
#else
  /* Lock this working copy directory, or steal an existing lock */
  SVN_ERR(svn_wc__db_wclock_obtain(db, adm_abspath, 0, TRUE, iterpool));
#endif

  /* Run our changes before the subdirectories. We may not have to recurse
     if we blow away a subdir.  */
  if (wc_format >= SVN_WC__HAS_WORK_QUEUE)
    SVN_ERR(svn_wc__wq_run(db, adm_abspath, cancel_func, cancel_baton,
                           iterpool));

#ifndef SVN_WC__SINGLE_DB
  /* Recurse on versioned, existing subdirectories.  */
  SVN_ERR(svn_wc__db_read_children(&children, db, adm_abspath,
                                   scratch_pool, iterpool));
  for (i = 0; i < children->nelts; i++)
    {
      const char *name = APR_ARRAY_IDX(children, i, const char *);
      const char *entry_abspath;
      svn_wc__db_kind_t kind;

      svn_pool_clear(iterpool);
      entry_abspath = svn_dirent_join(adm_abspath, name, iterpool);

      SVN_ERR(svn_wc__db_read_kind(&kind, db, entry_abspath, FALSE, iterpool));

      if (kind == svn_wc__db_kind_dir)
        {
          svn_node_kind_t disk_kind;

          SVN_ERR(svn_io_check_path(entry_abspath, &disk_kind, iterpool));
          if (disk_kind == svn_node_dir)
            SVN_ERR(cleanup_internal(db, entry_abspath,
                                     cancel_func, cancel_baton,
                                     iterpool));
        }
    }
#endif

#ifndef SVN_WC__SINGLE_DB
  /* Purge the DAV props at and under ADM_ABSPATH. */
  /* ### in single-db mode, we need do this purge at the top-level only. */
  SVN_ERR(svn_wc__db_base_clear_dav_cache_recursive(db, adm_abspath, iterpool));
#else
  SVN_ERR(svn_wc__db_get_wcroot(&cleanup_abspath, db, adm_abspath,
                                iterpool, iterpool));

  /* Perform these operations if we lock the entire working copy.
     Note that we really need to check a wcroot value and not
     svn_wc__check_wcroot() as that function, will just return true
     once we start sharing databases with externals.
   */
  if (strcmp(cleanup_abspath, adm_abspath) == 0)
    {
#endif

    /* Cleanup the tmp area of the admin subdir, if running the log has not
       removed it!  The logs have been run, so anything left here has no hope
       of being useful. */
      SVN_ERR(svn_wc__adm_cleanup_tmp_area(db, adm_abspath, iterpool));

      /* Remove unreferenced pristine texts */
      SVN_ERR(svn_wc__db_pristine_cleanup(db, adm_abspath, iterpool));
#ifdef SVN_WC__SINGLE_DB
    }
#endif

  /* All done, toss the lock */
  SVN_ERR(svn_wc__db_wclock_release(db, adm_abspath, iterpool));

  svn_pool_destroy(iterpool);

  return SVN_NO_ERROR;
}


/* ### possibly eliminate the WC_CTX parameter? callers really shouldn't
   ### be doing anything *but* running a cleanup, and we need a special
   ### DB anyway. ... *shrug* ... consider later.  */
svn_error_t *
svn_wc_cleanup3(svn_wc_context_t *wc_ctx,
                const char *local_abspath,
                svn_cancel_func_t cancel_func,
                void *cancel_baton,
                apr_pool_t *scratch_pool)
{
  svn_wc__db_t *db;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  /* We need a DB that allows a non-empty work queue (though it *will*
     auto-upgrade). We'll handle everything manually.  */
  SVN_ERR(svn_wc__db_open(&db, svn_wc__db_openmode_readwrite,
                          NULL /* ### config */, TRUE, FALSE,
                          scratch_pool, scratch_pool));

  SVN_ERR(cleanup_internal(db, local_abspath, cancel_func, cancel_baton,
                           scratch_pool));

#ifdef SINGLE_DB
  /* Purge the DAV props at and under LOCAL_ABSPATH. */
  /* ### in single-db mode, we need do this purge at the top-level only. */
  SVN_ERR(svn_wc__db_base_clear_dav_cache_recursive(db, local_abspath,
                                                    scratch_pool));
#endif

  /* We're done with this DB, so proactively close it.  */
  SVN_ERR(svn_wc__db_close(db));

  return SVN_NO_ERROR;
}
