/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2006 CollabNet.  All rights reserved.
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
 * @endcopyright
 *
 * @file svn_atomic.h
 * @brief Macros and functions for atomic operations
 */

#ifndef SVN_ATOMIC_H
#define SVN_ATOMIC_H

#include <apr_version.h>
#include <apr_atomic.h>

#include "svn_error.h"
#include "private/svn_compat.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @name Macro definitions for atomic types and operations
 *
 * @note These are necessary because the apr_atomic API changed somewhat 
 *       between apr-0.x and apr-1.x.
 * @{
 */

/** The type used by all the other atomic operations. */
#if APR_VERSION_AT_LEAST(1, 0, 0)
#define svn_atomic_t apr_uint32_t
#else
#define svn_atomic_t apr_atomic_t
#endif

/** Atomically read an #svn_atomic_t from memory. */
#if APR_VERSION_AT_LEAST(1, 0, 0)
#define svn_atomic_read(mem) apr_atomic_read32((mem))
#else
#define svn_atomic_read(mem) apr_atomic_read((mem))
#endif

/** Atomically set an #svn_atomic_t in memory. */
#if APR_VERSION_AT_LEAST(1, 0, 0)
#define svn_atomic_set(mem, val) apr_atomic_set32((mem), (val))
#else
#define svn_atomic_set(mem, val) apr_atomic_set((mem), (val))
#endif

/** Atomically increment an #svn_atomic_t. */
#if APR_VERSION_AT_LEAST(1, 0, 0)
#define svn_atomic_inc(mem) apr_atomic_inc32(mem)
#else
#define svn_atomic_inc(mem) apr_atomic_inc(mem)
#endif

/** Atomically decrement an #svn_atomic_t. */
#if APR_VERSION_AT_LEAST(1, 0, 0)
#define svn_atomic_dec(mem) apr_atomic_dec32(mem)
#else
#define svn_atomic_dec(mem) apr_atomic_dec(mem)
#endif

/** 
 * Atomic compare-and-swap.
 *
 * Compare the value that @a mem points to with @a cmp. If they are 
 * the same swap the value with @a with.
 *
 * @note svn_atomic_cas should not be combined with the other
 *       svn_atomic operations.  A comment in apr_atomic.h explains
 *       that on some platforms, the CAS function is implemented in a
 *       way that is incompatible with the other atomic operations.
 */
#if APR_VERSION_AT_LEAST(1, 0, 0)
#define svn_atomic_cas(mem, with, cmp) \
    apr_atomic_cas32((mem), (with), (cmp))
#else
#define svn_atomic_cas(mem, with, cmp) \
    apr_atomic_cas((mem), (with), (cmp))
#endif
/** @} */

/**
 * Call an initialization function in a thread-safe manner.
 *
 * @a global_status must be a pointer to a global, zero-initialized 
 * #svn_atomic_t. @a init_func is a pointer to the function that performs
 * the actual initialization, and @a pool is passed on to the init_func
 * for its use.
 *
 * @since New in 1.5.
 */
svn_error_t *
svn_atomic__init_once(volatile svn_atomic_t *global_status,
                      svn_error_t *(*init_func)(apr_pool_t*), apr_pool_t* pool);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_ATOMIC_H */
