/*
 * log.c:  handle the adm area's log file.
 *
 * ================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. The end-user documentation included with the redistribution, if
 * any, must include the following acknowlegement: "This product includes
 * software developed by CollabNet (http://www.Collab.Net)."
 * Alternately, this acknowlegement may appear in the software itself, if
 * and wherever such third-party acknowlegements normally appear.
 * 
 * 4. The hosted project names must not be used to endorse or promote
 * products derived from this software without prior written
 * permission. For written permission, please contact info@collab.net.
 * 
 * 5. Products derived from this software may not use the "Tigris" name
 * nor may "Tigris" appear in their names without prior written
 * permission of CollabNet.
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL COLLABNET OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by many
 * individuals on behalf of CollabNet.
 */



#include <apr_pools.h>
#include <apr_strings.h>
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

static svn_error_t *
merge_text (svn_string_t *path,
            const char *name,
            const char *saved_mods,
            apr_pool_t *pool)
{
  svn_string_t *filepath;
  svn_error_t *err;
  void *diff;

  filepath = svn_string_dup (path, pool);
  svn_path_add_component_nts (filepath, name, SVN_PATH_LOCAL_STYLE, pool);

  /* Get the local edits. */
  err = svn_wc__get_local_changes (svn_wc__gnudiff_differ,
                                   &diff,
                                   filepath,
                                   pool);
  if (err)
    return err;

  /* Merge local edits into the updated version. */
  err = svn_wc__merge_local_changes (svn_wc__gnudiff_patcher,
                                     diff,
                                     filepath,
                                     pool);
  if (err)
    return err;

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
  svn_boolean_t exists;

  filepath = svn_string_dup (path, pool);
  svn_path_add_component_nts (filepath, name, SVN_PATH_LOCAL_STYLE, pool);

  tmp_text_base = svn_wc__text_base_path (filepath, 1, pool);
  err = svn_wc__file_exists_p (&exists, tmp_text_base, pool);
  if (err)
    return err;

  if (! exists)
    return SVN_NO_ERROR;  /* tolerate mop-up calls gracefully */
  else
    return svn_wc__sync_text_base (filepath, pool);
}


static svn_error_t *
set_version (svn_string_t *path,
             const char *name,
             svn_vernum_t version,
             apr_pool_t *pool)
{
  /* This operation is idempotent, so just do it and don't worry. */
  return svn_wc__set_versions_entry (path,
                                     pool,
                                     name,
                                     version,
                                     NULL);
}


/* FMT must contain one "%s", which will be expanded to the path. */
static void
signal_error (struct log_runner *loggy, const char *fmt)
{
  svn_xml_signal_bailout 
    (loggy->parser, svn_error_createf (SVN_ERR_WC_BAD_ADM_LOG,
                                       0,
                                       NULL
                                       loggy->pool,
                                       fmt,
                                       loggy->path->data,
                                       NULL));
}


static void
start_handler (void *userData, const XML_Char *eltname, const XML_Char **atts)
{
  struct log_runner *loggy = (struct log_runner *) userData;
  svn_error_t *err = NULL;
  const char *name = svn_xml_get_attr_value ("name", atts);

  if (! name)
    {
      /* Everything has a name attribute, so check for that first. */
      signal_error (loggy->parser, "missing name attr in %s");
    }
  else
    {
      if (strcmp (eltname, "merge-text") == 0)
        {
          const char *saved_mods = svn_xml_get_attr_value ("saved-mods", atts);
          /* Note that saved_mods is allowed to be null. */
          err = merge_text (loggy->path, name, saved_mods, loggy->pool);
        }
      else if (strcmp (eltname, "replace-text-base") == 0)
        {
          err = replace_text_base (loggy->path, name, loggy->pool);
        }
      else if (strcmp (eltname, "set-version") == 0)
        {
          const char *verstr = svn_xml_get_attr_value ("version", atts);
          
          if (! verstr)
            signal_error (loggy->parser, "missing version attr in %s");
          else
            err = set_version (loggy->path, name, atoi (verstr), loggy->pool);
        }
      else
        signal_error (loggy->parser, "unrecognized element in %s");
    }
      
 if (err)
   svn_xml_signal_bailout (loggy->parser, err);
}



/*** Using the parser to run the log file. ***/

svn_error_t *
svn_wc__run_log (svn_string_t *path, apr_pool_t *pool)
{
  svn_error_t *err;
  apr_status_t apr_err;
  svn_xml_parser_t *parser;
  struct log_runner *logress = apr_palloc (pool, sizeof (*logress));
  char buf[BUFSIZ];
  apr_ssize_t buf_len;
  apr_file_t *f = NULL;

  const char *log_start
    = "<svn-wc-log xmlns=\"http://subversion.tigris.org/xmlns\">\n";
  const char *log_end
    = "</svn-wc-log>\n";

  logress->path = path;
  logress->pool = pool;

  parser = svn_xml_make_parser (logress, start_handler, NULL, NULL, pool);
  
  /* Expat wants everything wrapped in a top-level form, so start with
     a ghost open tag. */
  err = svn_xml_parse (parser, log_start, strlen (log_start), 0);
  if (err)
    return err;

  /* Parse the log file's contents. */
  err = svn_wc__open_adm_file (&f, path, SVN_WC__ADM_LOG, APR_READ, pool);
  if (err)
    goto any_error;
  
  do {
    buf_len = sizeof (buf);

    apr_err = apr_read (f, buf, &buf_len);
    if (apr_err && (apr_err != APR_EOF))
      {
        apr_close (f);
        return svn_error_createf (apr_error, 0, NULL, pool,
                                 "error reading adm log file in %s",
                                  path->data);
      }

    err = svn_xml_parse (parser, buf, buf_len, 0);
    if (err)
      {
        apr_close (f);
        return err;
      }

    if (apr_err == APR_EOF)
      {
        /* Not an error, just means we're done. */
        apr_close (f);
        break;
      }
  } while (apr_err == APR_SUCCESS);

  /* Pacify Expat with a pointless closing element tag. */
  err = svn_xml_parse (parser, log_end, sizeof (log_end), 0);
  if (err)
    return err;

  err = svn_xml_parse (parser, NULL, 0, 1);
  if (err)
    return err;

  svn_xml_free_parser (parser);

  err = svn_wc__remove_adm_file (path, pool, SVN_WC__ADM_LOG, NULL);

  return err;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */

