/*
 * utility functions to handle the java class
 * org.tigris.subversion.lib.Status
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

#ifndef SVN_JNI_STATUS_H
#define SVN_JNI_STATUS_H

/*** Includes ***/
#include <jni.h>
#include <svn_wc.h>

/*** Code ***/
jobject
status__create(JNIEnv *env, svn_wc_status_t *status, 
               jboolean *hasException);

void
status__set_entry(JNIEnv *env, jboolean *hasException,
                  jobject jstatus, jobject jentry);

void
status__set_text_status(JNIEnv *env, jboolean *hasException,
                        jobject jstatus, jint jtext_status);

void 
status__set_prop_status(JNIEnv *env, jboolean *hasException,
                        jobject jstatus, jint jprop_status);

void
status__set_locked(JNIEnv *env, jboolean *hasException,
                   jobject jstatus, jboolean jlocked);

void 
status__set_repos_text_status(JNIEnv *env, jboolean *hasException,
                              jobject jstatus, 
                              jint jrepos_text_status);

void
status__set_repos_prop_status(JNIEnv *env, jboolean *hasException,
                              jobject jstatus,
                              jint jrepos_prop_status);

#endif

/* 
 * local variables:
 * eval: (load-file "../../../svn-dev.el")
 * end: 
 */









