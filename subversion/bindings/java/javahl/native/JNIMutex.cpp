/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2003 QintSoft.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://svnup.tigris.org/.
 * ====================================================================
 * @endcopyright
 *
 * @file JNIMutex.cpp
 * @brief Implementation of the class JNIMutex
 */

#include "JNIMutex.h"
#include <apr_strings.h>
#include <apr_tables.h>
#include <apr_general.h>
#include <apr_lib.h>
#include "JNIUtil.h"
//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

JNIMutex::JNIMutex(apr_pool_t *pool)
{
	apr_status_t apr_err = apr_thread_mutex_create (&f_mutex, APR_THREAD_MUTEX_NESTED, pool);
	if (apr_err)
	{
		JNIUtil::handleAPRError(apr_err, "apr_thread_mutex_create");
	}
}

JNIMutex::~JNIMutex()
{
	apr_status_t apr_err = apr_thread_mutex_destroy (f_mutex);
	if (apr_err)
	{
		JNIUtil::handleAPRError(apr_err, "apr_thread_mutex_destroy");
	}

}
