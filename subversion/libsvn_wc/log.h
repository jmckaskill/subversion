/*
 * log.h :  interfaces for running .svn/log files.
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


#ifndef SVN_LIBSVN_WC_LOG_H
#define SVN_LIBSVN_WC_LOG_H

#include <apr_pools.h>

#include "svn_types.h"
#include "svn_error.h"
#include "svn_wc.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


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

/* Set some attributes on SVN_WC__LOG_ATTR_NAME's entry.  Unmentioned
   attributes are unaffected. */
#define SVN_WC__LOG_MODIFY_ENTRY        "modify-entry"

/* Delete the entry SVN_WC__LOG_ATTR_NAME. */
#define SVN_WC__LOG_DELETE_ENTRY        "delete-entry"

/* Move file SVN_WC__LOG_ATTR_NAME to SVN_WC__LOG_ATTR_DEST. */
#define SVN_WC__LOG_MV                  "mv"

/* Copy file SVN_WC__LOG_ATTR_NAME to SVN_WC__LOG_ATTR_DEST. */
#define SVN_WC__LOG_CP                  "cp"

/* Copy file SVN_WC__LOG_ATTR_NAME to SVN_WC__LOG_ATTR_DEST, but
   expand any keywords and use any eol-style defined by properties of
   the DEST. */
#define SVN_WC__LOG_CP_AND_TRANSLATE    "cp-and-translate"

/* Copy file SVN_WC__LOG_ATTR_NAME to SVN_WC__LOG_ATTR_DEST, but
   contract any keywords and convert to LF eol, according to
   properties of NAME. */
#define SVN_WC__LOG_CP_AND_DETRANSLATE    "cp-and-detranslate"

/* Remove file SVN_WC__LOG_ATTR_NAME. */
#define SVN_WC__LOG_RM                  "rm"

/* Append file from SVN_WC__LOG_ATTR_NAME to SVN_WC__LOG_ATTR_DEST. */
#define SVN_WC__LOG_APPEND              "append"

/* Make file SVN_WC__LOG_ATTR_NAME readonly */
#define SVN_WC__LOG_READONLY            "readonly"

/* Set SVN_WC__LOG_ATTR_NAME to have timestamp SVN_WC__LOG_ATTR_TIMESTAMP. */
#define SVN_WC__LOG_SET_TIMESTAMP       "set-timestamp"


/* Handle closure after a commit completes successfully:  
 *
 *   If SVN/tmp/text-base/SVN_WC__LOG_ATTR_NAME exists, then
 *      compare SVN/tmp/text-base/SVN_WC__LOG_ATTR_NAME with working file
 *         if they're the same, use working file's timestamp
 *         else use SVN/tmp/text-base/SVN_WC__LOG_ATTR_NAME's timestamp
 *      set SVN_WC__LOG_ATTR_NAME's revision to N
 */
#define SVN_WC__LOG_COMMITTED           "committed"

/* On target SVN_WC__LOG_ATTR_NAME, set wc property
   SVN_WC__LOG_ATTR_PROPNAME to value SVN_WC__LOG_ATTR_PROPVAL.  If
   SVN_WC__LOG_ATTR_PROPVAL is absent, then remove the property. */
#define SVN_WC__LOG_MODIFY_WCPROP        "modify-wcprop"


/* A log command which runs svn_wc_merge().
   See its documentation for details.

   Here is a map of entry-attributes to svn_wc_merge arguments:

         SVN_WC__LOG_NAME         : MERGE_TARGET
         SVN_WC__LOG_ATTR_ARG_1   : LEFT
         SVN_WC__LOG_ATTR_ARG_2   : RIGHT
         SVN_WC__LOG_ATTR_ARG_3   : LEFT_LABEL
         SVN_WC__LOG_ATTR_ARG_4   : RIGHT_LABEL
         SVN_WC__LOG_ATTR_ARG_5   : TARGET_LABEL

   Of course, the three paths should be *relative* to the directory in
   which the log is running, as with all other log commands.  (Usually
   they're just basenames within loggy->path.)
 */
#define SVN_WC__LOG_MERGE        "merge"


/** Log attributes.  See the documentation above for log actions for
    how these are used. **/
#define SVN_WC__LOG_ATTR_NAME           "name"
#define SVN_WC__LOG_ATTR_DEST           "dest"
#define SVN_WC__LOG_ATTR_PROPNAME       "propname"
#define SVN_WC__LOG_ATTR_PROPVAL        "propval"
#define SVN_WC__LOG_ATTR_REVISION       "revision"
#define SVN_WC__LOG_ATTR_TEXT_REJFILE   "text-rejfile"
#define SVN_WC__LOG_ATTR_PROP_REJFILE   "prop-rejfile"
#define SVN_WC__LOG_ATTR_TIMESTAMP      "timestamp"
/* The rest are for SVN_WC__LOG_MERGE.  Extend as necessary. */
#define SVN_WC__LOG_ATTR_ARG_1          "arg1"
#define SVN_WC__LOG_ATTR_ARG_2          "arg2"
#define SVN_WC__LOG_ATTR_ARG_3          "arg3"
#define SVN_WC__LOG_ATTR_ARG_4          "arg4"
#define SVN_WC__LOG_ATTR_ARG_5          "arg5"



/* Process the instructions in the log file for ADM_ACCESS. 
   DIFF3_CMD is the external differ used by the 'SVN_WC__LOG_MERGE'
   log entry.  It is always safe to pass null for this.

   If the log fails on its first command, return the error
   SVN_ERR_WC_BAD_ADM_LOG_START.  If it fails on some subsequent
   command, return SVN_ERR_WC_BAD_ADM_LOG. */
svn_error_t *svn_wc__run_log (svn_wc_adm_access_t *adm_access,
                              const char *diff3_cmd,
                              apr_pool_t *pool);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_LIBSVN_WC_LOG_H */
