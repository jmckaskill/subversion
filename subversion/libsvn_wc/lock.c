/*
 * wc_lock.c:  routines for locking working copy subdirectories.
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
#include <apr_time.h>
#include "wc.h"




svn_error_t *
svn_wc__lock (svn_string_t *path, int wait, apr_pool_t *pool)
{
  svn_error_t *err = NULL;

  /* kff todo: hmmm, feel kind of bad about this -- we're allocating
     another error for every time we try and fail to get a lock.  But
     it's not that much memory, and it happens rarely, and the number
     of retries is likely to be very small.  Really cannot get used to
     this pool stuff. :-) */

  do {
    err = svn_wc__make_adm_thing (path, SVN_WC__ADM_LOCK,
                                  svn_file_kind, 0, pool);
    if (err)
      {
        if (err->apr_err == APR_EEXIST)
          {
            /* kff todo: hey, apr_sleep() is broken. */
            apr_sleep (1000);  /* micro-seconds */
            wait--;
          }
        else
          return err;
      }
    else
      return SVN_NO_ERROR;
  } while (wait > 0);

  /* If haven't returned by now, then must have encountered a lock. */
  {
    svn_string_t *msg = svn_string_create ("working copy locked: ", pool);
    svn_string_appendstr (msg, path, pool);
    return svn_create_error (SVN_ERR_WC_LOCKED, 0, msg->data, NULL, pool);
  }
}


svn_error_t *
svn_wc__unlock (svn_string_t *path, apr_pool_t *pool)
{
  return svn_wc__remove_adm_thing (path, SVN_WC__ADM_LOCK, pool);
}



/*
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
