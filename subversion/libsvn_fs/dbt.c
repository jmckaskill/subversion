/* dbt.c --- DBT-frobbing functions
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

#include <stdlib.h>
#include <string.h>
#include "apr_pools.h"
#include "db.h"
#include "dbt.h"


DBT *
svn_fs__clear_dbt (DBT *dbt)
{
  memset (dbt, 0, sizeof (*dbt));

  return dbt;
}


DBT *
svn_fs__set_dbt (DBT *dbt, char *data, u_int32_t size)
{
  svn_fs__clear_dbt (dbt);

  dbt->data = data;
  dbt->size = size;

  return dbt;
}


DBT *
svn_fs__result_dbt (DBT *dbt)
{
  svn_fs__clear_dbt (dbt);
  dbt->flags |= DB_DBT_MALLOC;

  return dbt;
}


/* An APR pool cleanup function that simply applies `free' to its
   argument.  */
static apr_status_t
apr_free_cleanup (void *arg)
{
  free (arg);

  return 0;
}


DBT *
svn_fs__track_dbt (DBT *dbt, apr_pool_t *pool)
{
  if (dbt->data)
    apr_register_cleanup (pool, dbt->data, apr_free_cleanup, 0);

  return dbt;
}
