/*
 * err.h : interface to routines for returning Berkeley DB errors
 *
 * ================================================================
 * Copyright (c) 2000 Collab.Net.  All rights reserved.
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
 * software developed by Collab.Net (http://www.Collab.Net/)."
 * Alternately, this acknowlegement may appear in the software itself, if
 * and wherever such third-party acknowlegements normally appear.
 *
 * 4. The hosted project names must not be used to endorse or promote
 * products derived from this software without prior written
 * permission. For written permission, please contact info@collab.net.
 *
 * 5. Products derived from this software may not use the "Tigris" name
 * nor may "Tigris" appear in their names without prior written
 * permission of Collab.Net.
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
 * individuals on behalf of Collab.Net.
 */



#ifndef SVN_LIBSVN_FS_ERR_H
#define SVN_LIBSVN_FS_ERR_H

#include "apr_pools.h"
#include "svn_error.h"

/* Return an svn_error_t object that reports a Berkeley DB error.
   DB_ERR is the error value returned by the Berkeley DB routine.
   Allocate the error object from POOL.  */
extern svn_error_t *svn_fs__dberr (apr_pool_t *pool, int db_err);

/* Allocate an error object for a Berkeley DB error, with a formatted message.

   POOL is the APR pool to allocate the svn_error_t object from.
   DB_ERR is the Berkeley DB error code.
   FMT is a printf-style format string, describing how to format any
      subsequent arguments.

   The svn_error_t object returned has a message consisting of:
   - the text specified by FMT and the subsequent arguments, and
   - the Berkeley DB error message for the error code DB_ERR.

   There is no separator between the two messages; if you want one,
   you should include it in FMT.  */
extern svn_error_t *svn_fs__dberrf (apr_pool_t *pool, int db_err,
				    char *fmt, ...);


/* A dumb abort function for use with pools.  */
extern int svn_fs__pool_abort (int retcode);

#endif /* SVN_LIBSVN_FS_ERR_H */
