/*
 * utility functions to handle the java class
 * java.util.Date
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
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

/* includes */
#include <jni.h>
#include <apr_time.h>
#include "j.h"

/* defines */
#define SVN_JNI_DATE__CLASS "java/util/Date"
#define SVN_JNI_DATE__SIG "(J)V"

/* we can be lucky apr_time_t matches the constructor 
 * java.util.Date(long) so conversion is an easy job
 */
jobject 
date__apr_to_j(JNIEnv *env, jboolean *hasException,
		       apr_time_t time)
{
  jobject result = NULL;
  jboolean _hasException = JNI_FALSE;

  /*
   * references needed:
   * - class
   * - constructor
   * - object
   * = 3
   */
  if( (*env)->PushLocalFrame(env, 3) < 0 )
    {
      /* failed */
      _hasException = JNI_TRUE;
    }
  else
    {
      jclass class = NULL;
      jmethodID constructor = NULL;
      jobject jdate = NULL;

      /* get class reference */
      class = j__get_class(env, &_hasException,
                          SVN_JNI_DATE__CLASS);

      /* get method reference */
      if( !_hasException )
	{
	  constructor = j__get_method(env, &_hasException,
                                     class,
                                     "<init>",
                                     SVN_JNI_DATE__SIG);
	}

      /* create new instance */
      if( !_hasException )
	{
	  /* the apr_time_t parameter time may be passed
	   * directly to the java.util.Date(long) constructor
	   */
	  result = (*env)->NewObject(env, class, constructor, time);

	  if( result == NULL )
	    {
	      _hasException = JNI_TRUE;
	    }
	}

      (*env)->PopLocalFrame(env, result);
    }
				    
  if( hasException != NULL )
    {
      *hasException = _hasException;
    }

  return result;
}

/* 
 * local variables:
 * eval: (load-file "../../../svn-dev.el")
 * end: 
 */




