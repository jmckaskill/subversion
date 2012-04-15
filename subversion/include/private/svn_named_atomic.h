/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_named_atomics.h
 * @brief Structures and functions for machine-wide named atomics.
 *        These atomics store 64 bit signed integer values and provide
 *        a number of basic operations on them. Instead of an address,
 *        these atomics are identified by strings / names.  We also support
 *        namespaces - mainly to separate debug from production data.
 *        SVN-internal functionality uses the default namespace (see below).
 */

#ifndef SVN_NAMED_ATOMICS_H
#define SVN_NAMED_ATOMICS_H

#include "svn_error.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** An opaque structure that represents a namespace, i.e. a container
 * for named atomics.
 */
typedef struct svn_atomic_namespace__t svn_atomic_namespace__t;

/** An opaque structure that represents a named, system-wide visible
 * 64 bit integer with atomic access routines.
 */
typedef struct svn_named_atomic__t svn_named_atomic__t;

/** Maximum length of the name of any atomic (excluding the terminal NUL).
 */
#define SVN_NAMED_ATOMIC__MAX_NAME_LENGTH 30

/** Returns #TRUE on platforms that don't need expensive synchronization
 * objects to serialize access to named atomics. If this returns #FALSE,
 * reading from or modifying a #svn_named_atomic__t may be as expensive
 * as a file system operation.
 */
svn_boolean_t
svn_named_atomic__is_efficient();

/** Create a namespace (i.e. access object) with the given @a name and
 * return it in @a *ns.  If @a name is @c NULL, return the name of the
 * default namespace will be used.
 *
 * Multiple access objects with the same name may be created.  They access
 * the same shared memory region but have independent lifetimes.
 *
 * The access object will be allocated in @a result_pool and atomics gotten
 * from this object will become invalid when the pool is being cleaned.
 */
svn_error_t *
svn_atomic_namespace__create(svn_atomic_namespace__t **ns,
                             const char *name,
                             apr_pool_t *result_pool);

/** Find the atomic with the specified @a name in namespace @a ns and
 * return it in @a *atomic.  If @a ns is @c NULL, the default namespace
 * will be used.  If no object with that name can be found, the behavior
 * depends on @a auto_create.  If it is @c FALSE, @a *atomic will be set
 * to @c NULL. Otherwise, a new atomic will be created, its value set to 0
 * and the access structure be returned in @a *atomic.
 * 
 * Note that @a name must not exceed @ref SVN_NAMED_ATOMIC__MAX_NAME_LENGTH
 * characters and an error will be returned if the specified name is longer
 * than supported.
 *
 * If necessary, this function will automatically initialize the default
 * shared memory region. Therefore, this may fail with a variety of errors.
 *
 * Please note that the lifetime of the atomic is bound to the lifetime
 * of the @a ns object, i.e. the pool the latter was created in.
 * The default namespace (for @c ns = NULL) remains valid until APR gets
 * cleaned up.
 */
svn_error_t *
svn_named_atomic__get(svn_named_atomic__t **atomic,
                      svn_atomic_namespace__t *ns,
                      const char *name,
                      svn_boolean_t auto_create);

/** Read the @a atomic and return its current @a *value.
 * An error will be returned if @a atomic is @c NULL.
 */
svn_error_t *
svn_named_atomic__read(apr_int64_t *value,
                       svn_named_atomic__t *atomic);

/** Set the data in @a atomic to @a NEW_VALUE and return its old content
 * in @a OLD_VALUE.  @a OLD_VALUE may be NULL.
 * 
 * An error will be returned if @a atomic is @c NULL.
 */
svn_error_t *
svn_named_atomic__write(apr_int64_t *old_value,
                        apr_int64_t new_value,
                        svn_named_atomic__t *atomic);

/** Add @a delta to the data in @a atomic and return its new value in
 * @a NEW_VALUE.  @a NEW_VALUE may be NULL.
 * 
 * An error will be returned if @a atomic is @c NULL.
 */
svn_error_t *
svn_named_atomic__add(apr_int64_t *new_value,
                      apr_int64_t delta,
                      svn_named_atomic__t *atomic);

/** If the current data in @a atomic equals @a comperand, set it to
 * @a NEW_VALUE.  Return the initial value in @a OLD_VALUE.
 * @a OLD_VALUE may be NULL.
 * 
 * An error will be returned if @a atomic is @c NULL.
 */
svn_error_t *
svn_named_atomic__cmpxchg(apr_int64_t *old_value,
                          apr_int64_t new_value,
                          apr_int64_t comperand,
                          svn_named_atomic__t *atomic);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_NAMED_ATOMICS_H */
