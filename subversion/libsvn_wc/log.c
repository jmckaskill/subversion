/*
 * log.c:  handle the adm area's log file.
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



#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_thread_proc.h>

#include "svn_wc.h"
#include "svn_error.h"
#include "svn_xml.h"
#include "wc.h"



/*** Userdata for the callbacks. ***/
struct log_runner
{
  apr_pool_t *pool;
  svn_xml_parser_t *parser;
  svn_string_t *path;  /* the dir in which this is all happening */
};



/*** The XML handlers. ***/

/* Used by file_xfer_under_path(). */
enum svn_wc__xfer_action {
  svn_wc__xfer_append,
  svn_wc__xfer_cp,
  svn_wc__xfer_mv,
};

/* Invoke PROGRAM with ARGS, using PATH as working directory.
 * Connect PROGRAM's stdin, stdout, and stderr to INFILE, OUTFILE, and
 * ERRFILE, except where they are null.
 *
 * ARGS is a list of (const char *)'s, terminated by NULL.
 * ARGS[0] is the name of the program, though it need not be the same
 * as CMD.
 */
static svn_error_t *
run_cmd_in_directory (svn_string_t *path,
                      const char *cmd,
                      const char *const *args,
                      apr_file_t *infile,
                      apr_file_t *outfile,
                      apr_file_t *errfile,
                      apr_pool_t *pool)
{
  apr_status_t apr_err;
  apr_proc_t cmd_proc;
  apr_procattr_t *cmdproc_attr;

  /* Create the process attributes. */
  apr_err = apr_createprocattr_init (&cmdproc_attr, pool); 
  if (! APR_STATUS_IS_SUCCESS (apr_err))
    return svn_error_createf
      (apr_err, 0, NULL, pool,
       "run_cmd_in_directory: error creating %s process attributes",
       cmd);

  /* Make sure we invoke cmd directly, not through a shell. */
  apr_err = apr_setprocattr_cmdtype (cmdproc_attr, APR_PROGRAM);
  if (! APR_STATUS_IS_SUCCESS (apr_err))
    return svn_error_createf 
      (apr_err, 0, NULL, pool,
       "run_cmd_in_directory: error setting %s process cmdtype",
       cmd);

  /* Set the process's working directory. */
  if (path)
    {
      apr_err = apr_setprocattr_dir (cmdproc_attr, path->data);
      if (! APR_STATUS_IS_SUCCESS (apr_err))
        return svn_error_createf 
          (apr_err, 0, NULL, pool,
           "run_cmd_in_directory: error setting %s process directory",
           cmd);
    }

  /* Set io style. */
  apr_err = apr_setprocattr_io (cmdproc_attr, APR_FULL_BLOCK, 
                                APR_CHILD_BLOCK, APR_CHILD_BLOCK);
  if (! APR_STATUS_IS_SUCCESS (apr_err))
    return svn_error_createf
      (apr_err, 0, NULL, pool,
       "run_cmd_in_directory: error setting %s process io attributes",
       cmd);

  /* Use requested inputs and outputs. */
  if (infile)
    {
      apr_err = apr_setprocattr_childin (cmdproc_attr, infile, NULL);
      if (! APR_STATUS_IS_SUCCESS (apr_err))
        return svn_error_createf 
          (apr_err, 0, NULL, pool,
           "run_cmd_in_directory: error setting %s process child input",
           cmd);
    }
  if (outfile)
    {
      apr_err = apr_setprocattr_childout (cmdproc_attr, outfile, NULL);
      if (! APR_STATUS_IS_SUCCESS (apr_err))
        return svn_error_createf 
          (apr_err, 0, NULL, pool,
           "run_cmd_in_directory: error setting %s process child outfile",
           cmd);
    }
  if (errfile)
    {
      apr_err = apr_setprocattr_childerr (cmdproc_attr, errfile, NULL);
      if (! APR_STATUS_IS_SUCCESS (apr_err))
        return svn_error_createf 
          (apr_err, 0, NULL, pool,
           "run_cmd_in_directory: error setting %s process child errfile",
           cmd);
    }

  /* Start the cmd command. */ 
  apr_err = apr_create_process (&cmd_proc, cmd, args, NULL,
                                cmdproc_attr, pool);
  if (! APR_STATUS_IS_SUCCESS (apr_err))
    return svn_error_createf 
      (apr_err, 0, NULL, pool,
       "run_cmd_in_directory: error starting %s process",
       cmd);

  /* Wait for the cmd command to finish. */
  apr_err = apr_wait_proc (&cmd_proc, APR_WAIT);
  if (APR_STATUS_IS_CHILD_NOTDONE (apr_err))
    return svn_error_createf
      (apr_err, 0, NULL, pool,
       "run_cmd_in_directory: error waiting for %s process",
       cmd);

  return SVN_NO_ERROR;
}


/* Copy (or rename, if RENAME is non-zero) NAME to DEST, assuming that
   PATH is the common parent of both locations. */
static svn_error_t *
file_xfer_under_path (svn_string_t *path,
                      const char *name,
                      const char *dest,
                      enum svn_wc__xfer_action action,
                      apr_pool_t *pool)
{
  apr_status_t status;
  svn_string_t *full_from_path, *full_dest_path;

  full_from_path = svn_string_dup (path, pool);
  full_dest_path = svn_string_dup (path, pool);

  svn_path_add_component_nts (full_from_path, name, svn_path_local_style);
  svn_path_add_component_nts (full_dest_path, dest, svn_path_local_style);

  switch (action)
  {
  case svn_wc__xfer_append:
    return svn_io_append_file (full_from_path, full_dest_path, pool);
  case svn_wc__xfer_cp:
    return svn_io_copy_file (full_from_path, full_dest_path, pool);
  case svn_wc__xfer_mv:
    status = apr_rename_file (full_from_path->data,
                              full_dest_path->data, pool);
    if (status)
      return svn_error_createf (status, 0, NULL, pool,
                                "file_xfer_under_path: "
                                "can't move %s to %s",
                                name, dest);
  }

  return SVN_NO_ERROR;
}



static svn_error_t *
replace_text_base (svn_string_t *path,
                   const char *name,
                   apr_pool_t *pool)
{
  svn_string_t *filepath;
  svn_string_t *tmp_text_base;
  svn_error_t *err;
  enum svn_node_kind kind;

  filepath = svn_string_dup (path, pool);
  svn_path_add_component_nts (filepath, name, svn_path_local_style);

  tmp_text_base = svn_wc__text_base_path (filepath, 1, pool);
  err = svn_io_check_path (tmp_text_base, &kind, pool);
  if (err)
    return err;

  if (kind == svn_node_none)
    return SVN_NO_ERROR;  /* tolerate mop-up calls gracefully */
  else
    return svn_wc__sync_text_base (filepath, pool);
}


static void
signal_error (struct log_runner *loggy, svn_error_t *err)
{
  svn_xml_signal_bailout (svn_error_createf (SVN_ERR_WC_BAD_ADM_LOG,
                                             0,
                                             err,
                                             loggy->pool,
                                             "in directory %s",
                                             loggy->path->data),
                          loggy->parser);
}


static svn_error_t *
remove_from_revision_control (struct log_runner *loggy, svn_string_t *name)
{
  svn_error_t *err;
  apr_hash_t *entries = NULL;
  
  /* Remove this entry from the entries file. */
  err = svn_wc__entries_read (&entries, loggy->path, loggy->pool);
  if (err)
    return err;
  svn_wc__entry_remove (entries, name);
  err = svn_wc__entries_write (entries, loggy->path, loggy->pool);
  if (err)
    return err;
  
  /* Remove its text-base copy, if any, and conditionally remove
     working file too. */
  {
    svn_string_t *file_full_path;
    svn_string_t *text_base_path;
    enum svn_node_kind kind;
    
    file_full_path = svn_string_dup (loggy->path, loggy->pool);
    svn_path_add_component (file_full_path, name, svn_path_local_style);
    text_base_path
      = svn_wc__text_base_path (file_full_path, 0, loggy->pool);
    err = svn_io_check_path (text_base_path, &kind, loggy->pool);
    if (err && APR_STATUS_IS_ENOENT(err->apr_err))
      return SVN_NO_ERROR;
    else if (err)
      return err;
    
    /* Else we have a text-base copy, so use it. */

    if (kind == svn_node_file)
      {
        apr_status_t apr_err;
        svn_boolean_t same;
        
        {
          /* Aha!  There is a text-base file still around.  Use it
             to check if the working file is modified; if wf is not
             modified, we should remove it too. */
          err = svn_wc__files_contents_same_p (&same,
                                               file_full_path,
                                               text_base_path,
                                               loggy->pool);
          if (err && !APR_STATUS_IS_ENOENT(err->apr_err))
            return err;
          else if (! err)
            {
              apr_err = apr_remove_file (file_full_path->data,
                                         loggy->pool);
              if (apr_err)
                return svn_error_createf
                  (apr_err, 0, NULL,
                   loggy->pool,
                   "log.c:start_handler() (SVN_WC__LOG_DELETE_ENTRY): "
                   "error removing file %s",
                   file_full_path->data);
            }
        }
        
        apr_err = apr_remove_file (text_base_path->data, loggy->pool);
        if (apr_err)
          return svn_error_createf
            (apr_err, 0, NULL,
             loggy->pool,
             "log.c:start_handler() (SVN_WC__LOG_DELETE_ENTRY): "
             "error removing file %s",
             text_base_path->data);
      }
  }

  return SVN_NO_ERROR;
}



/*** Dispatch on the xml opening tag. ***/

static svn_error_t *
log_do_run_cmd (struct log_runner *loggy,
                const char *name,
                const XML_Char **atts)
{
  svn_error_t *err;
  apr_status_t apr_err;
  const char
    *infile_name,
    *outfile_name,
    *errfile_name;
  apr_file_t
    *infile = NULL,
    *outfile = NULL,
    *errfile = NULL;
  const char *args[9];
  
  args[0] = name;
  /* Grab the arguments.
     You want ugly?  I'll give you ugly... */
  args[1] = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_ARG_1, atts);
  args[2] = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_ARG_2, atts);
  args[3] = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_ARG_3, atts);
  args[4] = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_ARG_4, atts);
  args[5] = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_ARG_5, atts);
  args[6] = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_ARG_6, atts);
  args[7] = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_ARG_7, atts);
  args[8] = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_ARG_8, atts);
  args[9] = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_ARG_9, atts);
  
  /* Grab the input and output, if any. */
  infile_name = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_INFILE, atts);;
  outfile_name = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_OUTFILE, atts);;
  errfile_name = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_ERRFILE, atts);;
  
  if (infile_name)
    {
      svn_string_t *infile_path
        = svn_string_dup (loggy->path, loggy->pool);
      svn_path_add_component_nts (infile_path, infile_name,
                                  svn_path_local_style);
      
      apr_err = apr_open (&infile, infile_path->data, APR_READ,
                          APR_OS_DEFAULT, loggy->pool);
      if (apr_err)
        return svn_error_createf (apr_err, 0, NULL, loggy->pool,
                                  "error opening %s", infile_path->data);
    }
  
  if (outfile_name)
    {
      svn_string_t *outfile_path
        = svn_string_dup (loggy->path, loggy->pool);
      svn_path_add_component_nts (outfile_path, outfile_name,
                                  svn_path_local_style);
      
      /* kff todo: always creates and overwrites, currently.
         Could append if file exists... ?  Consider. */
      apr_err = apr_open (&outfile, outfile_path->data, 
                          (APR_WRITE | APR_CREATE),
                          APR_OS_DEFAULT, loggy->pool);
      if (apr_err)
        return svn_error_createf (apr_err, 0, NULL, loggy->pool,
                                  "error opening %s", outfile_path->data);
    }
  
  if (errfile_name)
    {
      svn_string_t *errfile_path
        = svn_string_dup (loggy->path, loggy->pool);
      svn_path_add_component_nts (errfile_path, errfile_name,
                                  svn_path_local_style);
      
      /* kff todo: always creates and overwrites, currently.
         Could append if file exists... ?  Consider. */
      apr_err = apr_open (&errfile, errfile_path->data, 
                          (APR_WRITE | APR_CREATE),
                          APR_OS_DEFAULT, loggy->pool);
      if (apr_err)
        return svn_error_createf (apr_err, 0, NULL, loggy->pool,
                                  "error opening %s", errfile_path->data);
    }
  
  err = run_cmd_in_directory (loggy->path,
                              name,
                              args,
                              infile,
                              outfile,
                              errfile,
                              loggy->pool);
  if (err)
     return svn_error_createf (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
                               "error running %s in %s",
                               name, loggy->path->data);

  return SVN_NO_ERROR;
}


static svn_error_t *
log_do_file_xfer (struct log_runner *loggy,
                  const char *name,
                  enum svn_wc__xfer_action action,
                  const XML_Char **atts)
{
  svn_error_t *err;
  const char *dest = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_DEST, atts);

  if (! dest)
    return svn_error_createf (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
                              "missing dest attr in %s", loggy->path->data);

  /* Else. */

  err = file_xfer_under_path (loggy->path, name, dest, action, loggy->pool);
  if (err)
    signal_error (loggy, err);

  return SVN_NO_ERROR;
}


static svn_error_t *
log_do_rm (struct log_runner *loggy, const char *name)
{
  apr_status_t apr_err;
  svn_string_t *full_path;

  full_path = svn_string_dup (loggy->path, loggy->pool);
  svn_path_add_component_nts (full_path, name, svn_path_local_style);

  apr_err = apr_remove_file (full_path->data, loggy->pool);
  if (apr_err)
    return svn_error_createf (apr_err, 0, NULL, loggy->pool,
                              "apr_remove_file couldn't remove %s", name);

  return SVN_NO_ERROR;
}


static svn_error_t *
log_do_modify_entry (struct log_runner *loggy,
                     const char *name,
                     const XML_Char **atts)
{
  svn_error_t *err;
  apr_hash_t *ah = svn_xml_make_att_hash (atts, loggy->pool);
      
  apr_time_t text_time, prop_time;
  svn_string_t *tfile, *pfile;
  svn_string_t *sname = svn_string_create (name, loggy->pool);
  svn_string_t *revstr = apr_hash_get (ah,
                                       SVN_WC_ENTRY_ATTR_REVISION,
                                       APR_HASH_KEY_STRING);
  svn_revnum_t new_revision = (revstr ? atoi (revstr->data)
                               : SVN_INVALID_REVNUM);
  int state = 0;
          
  enum svn_node_kind kind = svn_node_unknown;
  svn_string_t *kindstr = apr_hash_get (ah,
                                        SVN_WC_ENTRY_ATTR_KIND,
                                        APR_HASH_KEY_STRING);
          
  /* Create a full path to the file's textual component */
  tfile = svn_string_dup (loggy->path, loggy->pool);
  svn_path_add_component (tfile, sname, svn_path_local_style);

  /* Create a full path to the file's property component */
  err = svn_wc__prop_path (&pfile, tfile, 0, loggy->pool);
  if (err)
    signal_error (loggy, err);

  /* kff todo: similar to code in entries.c:handle_start().
             Would be nice to either write a function mapping string
             to kind, and/or write an equivalent of
             svn_wc__entry_merge_sync() that takes a hash and does the
             same thing, without all the specialized args. */
  if (! kindstr)
    kind = svn_node_none;
  else if (strcmp (kindstr->data, "file") == 0)
    kind = svn_node_file;
  else if (strcmp (kindstr->data, "dir") == 0)
    kind = svn_node_dir;
  else
    kind = svn_node_none;
          
  /* Stuff state flags. */
  if (apr_hash_get (ah, SVN_WC_ENTRY_ATTR_ADD,
                    APR_HASH_KEY_STRING))
    state |= SVN_WC_ENTRY_ADDED;
  if (apr_hash_get (ah, SVN_WC_ENTRY_ATTR_DELETE,
                    APR_HASH_KEY_STRING))
    state |= SVN_WC_ENTRY_DELETED;
  if (apr_hash_get (ah, SVN_WC_ENTRY_ATTR_MERGED,
                    APR_HASH_KEY_STRING))
    state |= SVN_WC_ENTRY_MERGED;
  if (apr_hash_get (ah, SVN_WC_ENTRY_ATTR_CONFLICT,
                    APR_HASH_KEY_STRING))
    state |= SVN_WC_ENTRY_CONFLICTED;

          /* Did the log command give us any timestamps?  There are three
             possible scenarios here.  We must check both text_time
             and prop_time for each of the three scenarios.  

             TODO: The next two code blocks might benefit from
             factorization.  Then again, factorization might make them
             more confusing.  :) */

  {
    /* GET VALUE OF TEXT_TIME: */
    svn_string_t *text_timestr = 
      apr_hash_get (ah, SVN_WC_ENTRY_ATTR_TEXT_TIME,
                    APR_HASH_KEY_STRING);
            
    /* Scenario 1:  no timestamp mentioned at all */
    if (! text_timestr)
      text_time = 0;  /* this tells merge_sync to ignore the
                         field */
            
    /* Scenario 2:  use the working copy's timestamp */
    else if (! strcmp (text_timestr->data, SVN_WC_TIMESTAMP_WC))
      {
        enum svn_node_kind tfile_kind;
        err = svn_io_check_path (tfile, &tfile_kind, loggy->pool);
        if (err)
          return svn_error_createf
            (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
             "error checking path %s", tfile->data);

        err = svn_io_file_affected_time (&text_time,
                                         tfile,
                                         loggy->pool);
        if (err)
          return svn_error_createf
            (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
             "error getting file affected time on %s", tfile->data);
      }
            
    /* Scenario 3:  use the integer provided, as-is. */
    else
      /* Is atol appropriate here for converting an apr_time_t
         to a string and then back again?  Or should we just use
         our svn_wc__time_to_string and string_to_time? */
      text_time = (apr_time_t) atol (text_timestr->data);
  }
          
  {
    /* GET VALUE OF PROP_TIME: */
    svn_string_t *prop_timestr = 
      apr_hash_get (ah, SVN_WC_ENTRY_ATTR_PROP_TIME,
                    APR_HASH_KEY_STRING);
            
    /* Scenario 1:  no timestamp mentioned at all */
    if (! prop_timestr)
      prop_time = 0;  /* this tells merge_sync to ignore the
                                 field */
            
            /* Scenario 2:  use the working copy's timestamp */
    else if (! strcmp (prop_timestr->data, SVN_WC_TIMESTAMP_WC))
      {
        enum svn_node_kind pfile_kind;
        err = svn_io_check_path (pfile, &pfile_kind, loggy->pool);
        if (err)
          return svn_error_createf
            (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
             "error checking path %s", pfile->data);

        err = svn_io_file_affected_time (&prop_time,
                                         pfile,
                                         loggy->pool);
        if (err)
          return svn_error_createf
            (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
             "error getting file affected time on %s", pfile->data);
      }
            
            
    /* Scenario 3:  use the integer provided, as-is. */
    else
      /* Is atol appropriate here for converting an apr_time_t
                 to a string and then back again?  Or should we just use
                 our svn_wc__time_to_string and string_to_time? */
      prop_time = (apr_time_t) atol (prop_timestr->data);
  }
          
  /** End of Timestamp deductions **/

          /* Now write the new entry out */
  err = svn_wc__entry_merge_sync (loggy->path,
                                  sname,
                                  new_revision,
                                  kind,
                                  state,
                                  text_time,
                                  prop_time,
                                  loggy->pool,
                                  ah,
                                  NULL);
  if (err)
    return svn_error_createf (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
                              "error merge_syncing entry %s", name);

  return SVN_NO_ERROR;
}


static svn_error_t *
log_do_delete_entry (struct log_runner *loggy, const char *name)
{
  svn_error_t *err;
  svn_string_t *sname = svn_string_create (name, loggy->pool);
  
  err = remove_from_revision_control (loggy, sname);
  if (err)
    return err;

  return SVN_NO_ERROR;
}


/* In PARENT_DIR, if REJFILE exists and is not 0 bytes, then mark
 * ENTRY as being in a state of CONFLICT_TYPE (using REJFILE as the
 * reject file).  Else if REJFILE is 0 bytes, then just remove it (the
 * ENTRY is not in a state of conflict, and REJFILE was never used).
 *
 * If REJFILE does not exist, do nothing.
 *
 * REJFILE_TYPE is either SVN_WC__LOG_ATTR_TEXT_REJFILE or
 * SVN_WC__LOG_ATTR_PROP_REJFILE.
 */
static svn_error_t *
conflict_if_rejfile (svn_string_t *parent_dir,
                     const char *rejfile,
                     const char *entry,
                     const char *rejfile_type,
                     apr_pool_t *pool)
{
  svn_error_t *err;
  svn_string_t *rejfile_full_path;
  enum svn_node_kind kind;

  rejfile_full_path = svn_string_dup (parent_dir, pool);
  svn_path_add_component_nts (rejfile_full_path, rejfile,
                              svn_path_local_style);
  
  /* Check most basic case: no rejfile, not even an empty one. */
  err = svn_io_check_path (rejfile_full_path, &kind, pool);
  if (err)
    return err;

  if (kind == svn_node_none)
    return SVN_NO_ERROR;
  else if (kind != svn_node_file)
    return svn_error_createf
      (SVN_ERR_WC_OBSTRUCTED_UPDATE, 0, NULL, pool,
       "conflict_if_rejfile: %s exists, but is not a reject file",
       rejfile_full_path->data);
  else  /* a (possibly empty) reject file exists, proceed */
    {
      apr_status_t apr_err;
      apr_finfo_t finfo;
      apr_err = apr_stat (&finfo, rejfile_full_path->data, pool);
      
      if (! APR_STATUS_IS_SUCCESS (apr_err))
        return svn_error_createf
          (apr_err, 0, NULL, pool,
           "conflict_if_rejfile: trouble stat()'ing %s",
           rejfile_full_path->data);
      
      if (finfo.size == 0)
        {
          apr_err = apr_remove_file (rejfile_full_path->data, pool);
          if (apr_err)
            return svn_error_createf
              (apr_err, 0, NULL, pool,
               "conflict_if_rejfile: trouble removing %s",
               rejfile_full_path->data);

          err = svn_wc__entry_merge_sync
            (parent_dir,
             svn_string_create (entry, pool),
             SVN_INVALID_REVNUM,
             svn_node_none,
             (SVN_WC_ENTRY_CLEAR_NAMED | SVN_WC_ENTRY_CONFLICTED),
             0,
             0,
             pool,
             NULL,
             rejfile_type,
             NULL);
          if (err)
            return err;
        }
      else  /* reject file size > 0 means the entry has conflicts. */
        {
          apr_hash_t *att_overlay = apr_make_hash (pool);

          apr_hash_set (att_overlay,
                        rejfile_type, APR_HASH_KEY_STRING,
                        svn_string_create (rejfile, pool));

          err = svn_wc__entry_merge_sync 
            (parent_dir,
             svn_string_create (entry, pool),
             SVN_INVALID_REVNUM,
             svn_node_none,
             SVN_WC_ENTRY_CONFLICTED,
             0,
             0,
             pool,
             att_overlay,
             NULL);
          if (err)
            return err;
        } 
    }

  return SVN_NO_ERROR;
}


static svn_error_t *
log_do_updated (struct log_runner *loggy,
                const char *name,
                const XML_Char **atts)
{
  svn_error_t *err;
  const char *t = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_TEXT_REJFILE, atts);
  const char *p = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_PROP_REJFILE, atts);

  if (t)
    {
      err = conflict_if_rejfile (loggy->path, t, name,
                                 SVN_WC__LOG_ATTR_TEXT_REJFILE,
                                 loggy->pool);
      if (err)
        return err;
    }

  if (p)
    {
      err = conflict_if_rejfile (loggy->path, p, name,
                                 SVN_WC__LOG_ATTR_PROP_REJFILE,
                                 loggy->pool);
      if (err)
        return err;
    }

  return SVN_NO_ERROR;
}


/* Note:  assuming that svn_wc__log_commit() is what created all of
   the <committed...> commands, the `name' attribute will either be a
   file or SVN_WC_ENTRY_THIS_DIR. */
static svn_error_t *
log_do_committed (struct log_runner *loggy,
                  const char *name,
                  const XML_Char **atts)
{
  svn_error_t *err;
  const char *revstr
    = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_REVISION, atts);

  if (! revstr)
    return svn_error_createf (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
                              "missing revision attr for %s", name);
  else
    {
      svn_string_t *working_file;
      svn_string_t *tmp_base;
      apr_time_t text_time = 0; /* By default, don't override old stamp. */
      apr_time_t prop_time = 0; /* By default, don't override old stamp. */
      enum svn_node_kind kind;
      svn_string_t *sname = svn_string_create (name, loggy->pool);
      apr_hash_t *entries = NULL;
      svn_wc_entry_t *entry;
      svn_string_t *prop_path, *tmp_prop_path, *prop_base_path;

      err = svn_wc__entries_read (&entries, loggy->path, loggy->pool);
      if (err)
        return err;
          
      entry = apr_hash_get (entries, sname->data, sname->len);
      if (entry && (entry->state & SVN_WC_ENTRY_DELETED))
        {
          err = remove_from_revision_control (loggy, sname);
          if (err)
            return err;
        }
      else   /* entry not being deleted, so mark commited-to-date */
        {
          /* `name' will either be a file's basename, or
             SVN_WC_ENTRY_THIS_DIR. */
          svn_boolean_t is_this_dir;

          if (! strcmp (name, SVN_WC_ENTRY_THIS_DIR))
            is_this_dir = TRUE;
          else
            is_this_dir = FALSE;

          if (! is_this_dir)
            {
              /* `name' is a file's basename.  check for textual
                 changes. */
              working_file = svn_string_dup (loggy->path, loggy->pool);
              svn_path_add_component (working_file,
                                      sname,
                                      svn_path_local_style);
              tmp_base = svn_wc__text_base_path (working_file, 1, loggy->pool);
              
              err = svn_io_check_path (tmp_base, &kind, loggy->pool);
              if (err)
                return svn_error_createf
                  (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
                   "error checking existence of %s", name);
              
              if (kind == svn_node_file)
                {
                  svn_boolean_t same;
                  err = svn_wc__files_contents_same_p (&same,
                                                       working_file,
                                                       tmp_base,
                                                       loggy->pool);
                  if (err)
                    return svn_error_createf 
                      (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
                       "error comparing %s and %s",
                       working_file->data, tmp_base->data);
                  
                  /* What's going on here: the working copy has been
                     copied to tmp/text-base/ during the commit.  That's
                     what `tmp_base' points to.  If we get here, we know
                     the commit was successful, and we need make tmp_base
                     into the real text-base.  *However*, which timestamp
                     do we put on the entry?  It's possible that during
                     the commit the working file may have changed.  If
                     that's the case, use tmp_base's timestamp.  If
                     there's been no local mod, it's okay to use the
                     working file's timestamp. */
                  err = svn_io_file_affected_time 
                    (&text_time, same ? working_file : tmp_base, loggy->pool);
                  if (err)
                    return svn_error_createf 
                      (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
                       "error getting file_affected_time on %s",
                       same ? working_file->data : tmp_base->data);
                  
                  err = replace_text_base (loggy->path, name, loggy->pool);
                  if (err)
                    return svn_error_createf 
                      (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
                       "error replacing text base for %s", name);
                }
            }
              
          /* Now check for property commits. */

          /* Get property file pathnames, depending on whether we're
             examining a file or THIS_DIR */
          err = svn_wc__prop_path (&prop_path,
                                   is_this_dir ? loggy->path : working_file,
                                   0 /* not tmp */, loggy->pool);
          if (err) return err;
          
          err = svn_wc__prop_path (&tmp_prop_path, 
                                   is_this_dir ? loggy->path : working_file,
                                   1 /* tmp */, loggy->pool);
          if (err) return err;
          
          err = svn_wc__prop_base_path (&prop_base_path,
                                        is_this_dir ? 
                                          loggy->path : working_file,
                                        0 /* not tmp */, loggy->pool);
          if (err) return err;

          /* Check for existence of tmp_prop_path */
          err = svn_io_check_path (tmp_prop_path, &kind, loggy->pool);
          if (err)
            return svn_error_createf
              (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
               "error checking existence of %s", name);
          
          if (kind == svn_node_file)
            {
              /* Magic inference: if there's a working property file
                 sitting in the tmp area, then we must have committed
                 properties on this file or dir.  Time to sync. */
              
              /* We need to decide which prop-timestamp to use, just
                 like we did with text-time. */             
              svn_boolean_t same;
              apr_status_t status;
              err = svn_wc__files_contents_same_p (&same,
                                                   prop_path,
                                                   tmp_prop_path,
                                                   loggy->pool);
              if (err)
                return svn_error_createf 
                  (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
                   "error comparing %s and %s",
                   prop_path->data, tmp_prop_path->data);

              err = svn_io_file_affected_time 
                (&prop_time, same ? prop_path : tmp_prop_path, loggy->pool);
              if (err)
                return svn_error_createf 
                  (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
                   "error getting file_affected_time on %s",
                   same ? prop_path->data : tmp_prop_path->data);

              /* Make the tmp prop file the new pristine one. */
              status = apr_rename_file (tmp_prop_path->data,
                                        prop_base_path->data,
                                        loggy->pool);
              if (status)
                return svn_error_createf (status, 0, NULL, loggy->pool,
                                          "error renaming %s to %s",
                                          tmp_prop_path->data,
                                          prop_base_path->data);
            }
          

          /* Files have been moved, and timestamps are found.  Time
             for The Big Merge Sync. */
          err = svn_wc__entry_merge_sync (loggy->path,
                                          sname,
                                          atoi (revstr),
                                          svn_node_file,
                                          SVN_WC_ENTRY_CLEAR_ALL,
                                          text_time,
                                          prop_time,
                                          loggy->pool,
                                          NULL,
                                          /* remove the rejfile atts! */
                                          SVN_WC_ENTRY_ATTR_REJFILE,
                                          SVN_WC_ENTRY_ATTR_PREJFILE,
                                          NULL);
          if (err)
            return svn_error_createf
              (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
               "error merge_syncing %s", name);
        }
    }

  return SVN_NO_ERROR;
}


static void
start_handler (void *userData, const XML_Char *eltname, const XML_Char **atts)
{
  svn_error_t *err = NULL;
  struct log_runner *loggy = userData;

  /* All elements use the `name' attribute, so grab it now. */
  const char *name = svn_xml_get_attr_value (SVN_WC__LOG_ATTR_NAME, atts);

  if (strcmp (eltname, "wc-log") == 0)   /* ignore expat pacifier */
    return;
  else if (! name)
    {
      signal_error
        (loggy, svn_error_createf 
         (SVN_ERR_WC_BAD_ADM_LOG, 0, NULL, loggy->pool,
          "log entry missing name attribute (entry %s for dir %s)",
          eltname, loggy->path->data));
      return;
    }
  
  /* Dispatch. */
  if (strcmp (eltname, SVN_WC__LOG_RUN_CMD) == 0) {
    err = log_do_run_cmd (loggy, name, atts);
  }
  else if (strcmp (eltname, SVN_WC__LOG_MODIFY_ENTRY) == 0) {
    err = log_do_modify_entry (loggy, name, atts);
  }
  else if (strcmp (eltname, SVN_WC__LOG_DELETE_ENTRY) == 0) {
    err = log_do_delete_entry (loggy, name);
  }
  else if (strcmp (eltname, SVN_WC__LOG_UPDATED) == 0) {
    err = log_do_updated (loggy, name, atts);
  }
  else if (strcmp (eltname, SVN_WC__LOG_COMMITTED) == 0) {
    err = log_do_committed (loggy, name, atts);
  }
  else if (strcmp (eltname, SVN_WC__LOG_RM) == 0) {
    err = log_do_rm (loggy, name);
  }
  else if (strcmp (eltname, SVN_WC__LOG_MV) == 0) {
    err = log_do_file_xfer (loggy, name, svn_wc__xfer_mv, atts);
  }
  else if (strcmp (eltname, SVN_WC__LOG_CP) == 0) {
    err = log_do_file_xfer (loggy, name, svn_wc__xfer_cp, atts);
  }
  else if (strcmp (eltname, SVN_WC__LOG_APPEND) == 0) {
    err = log_do_file_xfer (loggy, name, svn_wc__xfer_append, atts);
  }
  else
    {
      signal_error
        (loggy, svn_error_createf (SVN_ERR_WC_BAD_ADM_LOG,
                                   0,
                                   NULL,
                                   loggy->pool,
                                   "unrecognized logfile element in %s: `%s'",
                                   loggy->path->data, eltname));
      return;
    }

  if (err)
    signal_error
      (loggy, svn_error_createf
       (SVN_ERR_WC_BAD_ADM_LOG, 0, err, loggy->pool,
        "start_handler: error processing element %s in %s",
        eltname, loggy->path->data));
  
  return;
}



/*** Using the parser to run the log file. ***/

svn_error_t *
svn_wc__run_log (svn_string_t *path, apr_pool_t *pool)
{
  svn_error_t *err;
  apr_status_t apr_err;
  svn_xml_parser_t *parser;
  struct log_runner *loggy = apr_pcalloc (pool, sizeof (*loggy));
  char buf[BUFSIZ];
  apr_size_t buf_len;
  apr_file_t *f = NULL;

  /* kff todo: use the tag-making functions here, now. */
  const char *log_start
    = "<wc-log xmlns=\"http://subversion.tigris.org/xmlns\">\n";
  const char *log_end
    = "</wc-log>\n";

  parser = svn_xml_make_parser (loggy, start_handler, NULL, NULL, pool);
  loggy->path   = path;
  loggy->pool   = pool;
  loggy->parser = parser;
  
  /* Expat wants everything wrapped in a top-level form, so start with
     a ghost open tag. */
  err = svn_xml_parse (parser, log_start, strlen (log_start), 0);
  if (err)
    return err;

  /* Parse the log file's contents. */
  err = svn_wc__open_adm_file (&f, path, SVN_WC__ADM_LOG, APR_READ, pool);
  if (err)
    return svn_error_quick_wrap (err, "svn_wc__run_log: couldn't open log.");
  
  do {
    buf_len = sizeof (buf);

    apr_err = apr_read (f, buf, &buf_len);
    if (apr_err && !APR_STATUS_IS_EOF(apr_err))
      {
        apr_close (f);
        return svn_error_createf (apr_err, 0, NULL, pool,
                                 "error reading adm log file in %s",
                                  path->data);
      }

    err = svn_xml_parse (parser, buf, buf_len, 0);
    if (err)
      {
        apr_close (f);
        return err;
      }

    if (APR_STATUS_IS_EOF(apr_err))
      {
        /* Not an error, just means we're done. */
        apr_close (f);
        break;
      }
  } while (apr_err == APR_SUCCESS);

  /* Pacify Expat with a pointless closing element tag. */
  err = svn_xml_parse (parser, log_end, strlen (log_end), 1);
  if (err)
    return err;

  svn_xml_free_parser (parser);

  err = svn_wc__remove_adm_file (path, pool, SVN_WC__ADM_LOG, NULL);

  return err;
}



/*** Recursively do log things. ***/

svn_error_t *
svn_wc__cleanup (svn_string_t *path,
                 apr_hash_t *targets,
                 svn_boolean_t bail_on_lock,
                 apr_pool_t *pool)
{
  svn_error_t *err;
  apr_hash_t *entries = NULL;
  apr_hash_index_t *hi;
  svn_boolean_t care_about_this_dir = 0;

  /* Recurse on versioned subdirs first, oddly enough. */
  err = svn_wc__entries_read (&entries, path, pool);
  if (err)
    return err;

  for (hi = apr_hash_first (entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      apr_size_t keylen;
      void *val;
      svn_wc_entry_t *entry;
      svn_boolean_t is_this_dir = FALSE;

      apr_hash_this (hi, &key, &keylen, &val);
      entry = val;

      if ((keylen == strlen (SVN_WC_ENTRY_THIS_DIR))
          && (strcmp ((char *) key, SVN_WC_ENTRY_THIS_DIR) == 0))
        is_this_dir = TRUE;

      /* If TARGETS tells us to care about this dir, we may need to
         clean up locks later.  So find out in advance. */
      if (targets)
        {
          if (! care_about_this_dir)
            {
              svn_string_t *target = svn_string_dup (path, pool);
              svn_path_add_component 
                (target,
                 svn_string_ncreate ((char *) key, keylen, pool),
                 svn_path_local_style);
              
              if (apr_hash_get (targets, target->data, target->len))
                care_about_this_dir = 1;
            }
        }
      else
        care_about_this_dir = 1;

      if ((entry->kind == svn_node_dir) && (! is_this_dir))
        {
          /* Recurse */
          svn_string_t *subdir = svn_string_dup (path, pool);
          svn_path_add_component (subdir,
                                  svn_string_create ((char *) key, pool),
                                  svn_path_local_style);

          err = svn_wc__cleanup (subdir, targets, bail_on_lock, pool);
          if (err)
            return err;
        }
    }


  if (care_about_this_dir)
    {
      if (bail_on_lock)
        {
          svn_boolean_t locked;
          err = svn_wc__locked (&locked, path, pool);
          if (err)
            return err;
          
          if (locked)
            return svn_error_createf (SVN_ERR_WC_LOCKED,
                                      0,
                                      NULL,
                                      pool,
                                      "svn_wc__cleanup: %s locked",
                                      path->data);
        }
      
      /* Is there a log?  If so, run it and then remove it. */
      {
        enum svn_node_kind kind;
        svn_string_t *log_path = svn_wc__adm_path (path, 0, pool,
                                                   SVN_WC__ADM_LOG, NULL);
        
        err = svn_io_check_path (log_path, &kind, pool);
        if (err) return err;

        if (kind == svn_node_file)
          {
            err = svn_wc__run_log (path, pool);
            if (err) return err;
          }
      }

      /* Remove any lock here.  But we couldn't even be here if there were
         a lock file and bail_on_lock were set, so do the obvious check
         first. */
      if (! bail_on_lock)
        {
          err = svn_wc__unlock (path, pool);
          if (err && !APR_STATUS_IS_ENOENT(err->apr_err))
            return err;
        }
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_wc__log_commit (svn_string_t *path,
                    apr_hash_t *targets,
                    svn_revnum_t revision,
                    apr_pool_t *pool)
{
  svn_error_t *err;
  apr_status_t apr_err;
  apr_hash_t *entries = NULL;
  apr_hash_index_t *hi;

  err = svn_wc__entries_read (&entries, path, pool);
  if (err)
    return err;

  for (hi = apr_hash_first (entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      apr_size_t keylen;
      void *val;
      svn_wc_entry_t *entry;

      svn_string_t *full_path;
      svn_string_t *logtag = svn_string_create ("", pool);
      char *revstr = apr_psprintf (pool, "%ld", revision);
      apr_file_t *log_fp = NULL;

      apr_hash_this (hi, &key, &keylen, &val);
      entry = val;

      /* Construct the full path of the entry we're examining. */
      full_path = svn_string_dup (path, pool);
      svn_path_add_component
        (full_path,
         svn_string_ncreate ((char *) key, keylen, pool),
         svn_path_local_style);

      /* If we're looking at directory which is not `.', recurse. */
      if ((entry->kind == svn_node_dir)
          && (strcmp ((char *) key, SVN_WC_ENTRY_THIS_DIR)))
        {
          err = svn_wc__log_commit (full_path, targets, revision, pool);
          if (err) return err;
        }

      if (targets)
        {
          /* If `full_path' isn't an affected target, move along. */
          if (! apr_hash_get (targets, full_path->data, full_path->len))
            continue;
          else
            /* `full_path' is an affected target, but we want to
               ignore certain cases of it.  For example, in the case
               of an affected directory "foo/bar/baz/", we want to
               ignore
                
                     full_path = "foo/bar" + "baz"

               but we want to recognize
 
                     full_path = "foo/bar/baz" + ""
            */
            if ((entry->kind == svn_node_dir)
                && (! svn_string_compare (path, full_path)))
              continue;
        }

      /* The current entry was committed.  Append a log command that
         will update the entry appropriately. */

      err = svn_wc__open_adm_file (&log_fp, path, SVN_WC__ADM_LOG,
                                   (APR_WRITE | APR_APPEND | APR_CREATE),
                                   pool);
      if (err)
        return
          svn_error_quick_wrap
          (err, "svn_wc__log_commit: can't append to log file.");
      
      svn_xml_make_open_tag (&logtag,
                             pool,
                             svn_xml_self_closing,
                             SVN_WC__LOG_COMMITTED,
                             SVN_WC__LOG_ATTR_NAME,
                             svn_string_create ((char *) key, pool),
                             SVN_WC__LOG_ATTR_REVISION,
                             svn_string_create (revstr, pool),
                             NULL);
      
      apr_err = apr_full_write (log_fp, logtag->data, logtag->len, NULL);
      if (apr_err)
        {
          apr_close (log_fp);
          return svn_error_createf (apr_err, 0, NULL, pool,
                                    "svn_wc__log_commit: "
                                    "error writing %s's log file", 
                                    full_path->data);
        }
      
      err = svn_wc__close_adm_file (log_fp,
                                    path,
                                    SVN_WC__ADM_LOG,
                                    1, /* sync */
                                    pool);
      if (err)
        return
          svn_error_quick_wrap
          (err, "svn_wc__log_commit: can't close log file.");
    }
    
  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */

