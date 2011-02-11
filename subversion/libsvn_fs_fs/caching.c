/* caching.c : in-memory caching
 *
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
 */

#include "fs.h"
#include "fs_fs.h"
#include "id.h"
#include "dag.h"
#include "../libsvn_fs/fs-loader.h"

#include "svn_config.h"

#include "svn_private_config.h"

/*** Dup/serialize/deserialize functions. ***/


/** Caching SVN_FS_ID_T values. **/
/* Implements svn_cache__dup_func_t */
static svn_error_t *
dup_id(void **out,
       const void *in,
       apr_pool_t *pool)
{
  const svn_fs_id_t *id = in;
  *out = svn_fs_fs__id_copy(id, pool);
  return SVN_NO_ERROR;
}

/* Implements svn_cache__serialize_func_t */
static svn_error_t *
serialize_id(char **data,
             apr_size_t *data_len,
             void *in,
             apr_pool_t *pool)
{
  svn_fs_id_t *id = in;
  svn_string_t *id_str = svn_fs_fs__id_unparse(id, pool);
  *data = (char *) id_str->data;
  *data_len = id_str->len;

  return SVN_NO_ERROR;
}


/* Implements svn_cache__deserialize_func_t */
static svn_error_t *
deserialize_id(void **out,
               const char *data,
               apr_size_t data_len,
               apr_pool_t *pool)
{
  svn_fs_id_t *id = svn_fs_fs__id_parse(data, data_len, pool);
  if (id == NULL)
    {
      return svn_error_create(SVN_ERR_FS_NOT_ID, NULL,
                              _("Bad ID in cache"));
    }

  *out = id;
  return SVN_NO_ERROR;
}


/** Caching directory listings. **/
/* Implements svn_cache__dup_func_t */
static svn_error_t *
dup_dir_listing(void **out,
                const void *in,
                apr_pool_t *pool)
{
  apr_hash_t *new_entries = apr_hash_make(pool);
  apr_hash_t *entries = (void*)in; /* Cast away const only */
  apr_hash_index_t *hi;

  for (hi = apr_hash_first(pool, entries); hi; hi = apr_hash_next(hi))
    {
      svn_fs_dirent_t *dirent = svn__apr_hash_index_val(hi);
      svn_fs_dirent_t *new_dirent;

      new_dirent = apr_palloc(pool, sizeof(*new_dirent));
      new_dirent->name = apr_pstrdup(pool, dirent->name);
      new_dirent->kind = dirent->kind;
      new_dirent->id = svn_fs_fs__id_copy(dirent->id, pool);
      apr_hash_set(new_entries, new_dirent->name, APR_HASH_KEY_STRING,
                   new_dirent);
    }

  *out = new_entries;
  return SVN_NO_ERROR;
}


/** Caching packed rev offsets. **/
/* Implements svn_cache__serialize_func_t */
static svn_error_t *
manifest_serialize(char **data,
                   apr_size_t *data_len,
                   void *in,
                   apr_pool_t *pool)
{
  apr_array_header_t *manifest = in;

  *data_len = sizeof(apr_off_t) * manifest->nelts;
  *data = apr_palloc(pool, *data_len);
  memcpy(*data, manifest->elts, *data_len);

  return SVN_NO_ERROR;
}

/* Implements svn_cache__deserialize_func_t */
static svn_error_t *
manifest_deserialize(void **out,
                     const char *data,
                     apr_size_t data_len,
                     apr_pool_t *pool)
{
  apr_array_header_t *manifest = apr_array_make(pool,
                                       (int) (data_len / sizeof(apr_off_t)),
                                       sizeof(apr_off_t));
  memcpy(manifest->elts, data, data_len);
  manifest->nelts = (int) (data_len / sizeof(apr_off_t));
  *out = manifest;

  return SVN_NO_ERROR;
}

/* Implements svn_cache__dup_func_t */
static svn_error_t *
dup_pack_manifest(void **out,
                  const void *in,
                  apr_pool_t *pool)
{
  const apr_array_header_t *manifest = in;

  *out = apr_array_copy(pool, manifest);
  return SVN_NO_ERROR;
}


/* Return a memcache in *MEMCACHE_P for FS if it's configured to use
   memcached, or NULL otherwise.  Also, sets *FAIL_STOP to a boolean
   indicating whether cache errors should be returned to the caller or
   just passed to the FS warning handler.  Use FS->pool for allocating
   the memcache, and POOL for temporary allocations. */
static svn_error_t *
read_config(svn_memcache_t **memcache_p,
            svn_boolean_t *fail_stop,
            svn_fs_t *fs,
            apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;

  SVN_ERR(svn_cache__make_memcache_from_config(memcache_p, ffd->config,
                                              fs->pool));
  return svn_config_get_bool(ffd->config, fail_stop,
                             CONFIG_SECTION_CACHES, CONFIG_OPTION_FAIL_STOP,
                             FALSE);
}


/* Implements svn_cache__error_handler_t */
static svn_error_t *
warn_on_cache_errors(svn_error_t *err,
                     void *baton,
                     apr_pool_t *pool)
{
  svn_fs_t *fs = baton;
  (fs->warning)(fs->warning_baton, err);
  svn_error_clear(err);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_fs__initialize_caches(svn_fs_t *fs,
                             apr_pool_t *pool)
{
  fs_fs_data_t *ffd = fs->fsap_data;
  const char *prefix = apr_pstrcat(pool,
                                   "fsfs:", ffd->uuid,
                                   "/", fs->path, ":",
                                   (char *)NULL);
  svn_memcache_t *memcache;
  svn_boolean_t no_handler;

  SVN_ERR(read_config(&memcache, &no_handler, fs, pool));

  /* Make the cache for revision roots.  For the vast majority of
   * commands, this is only going to contain a few entries (svnadmin
   * dump/verify is an exception here), so to reduce overhead let's
   * try to keep it to just one page.  I estimate each entry has about
   * 72 bytes of overhead (svn_revnum_t key, svn_fs_id_t +
   * id_private_t + 3 strings for value, and the cache_entry); the
   * default pool size is 8192, so about a hundred should fit
   * comfortably. */
  if (memcache)
    SVN_ERR(svn_cache__create_memcache(&(ffd->rev_root_id_cache),
                                       memcache,
                                       serialize_id,
                                       deserialize_id,
                                       sizeof(svn_revnum_t),
                                       apr_pstrcat(pool, prefix, "RRI",
                                                   (char *)NULL),
                                       fs->pool));
  else
    SVN_ERR(svn_cache__create_inprocess(&(ffd->rev_root_id_cache),
                                        dup_id, sizeof(svn_revnum_t),
                                        1, 100, FALSE, fs->pool));
  if (! no_handler)
    SVN_ERR(svn_cache__set_error_handler(ffd->rev_root_id_cache,
                                         warn_on_cache_errors, fs, pool));


  /* Rough estimate: revision DAG nodes have size around 320 bytes, so
   * let's put 16 on a page. */
  if (memcache)
    SVN_ERR(svn_cache__create_memcache(&(ffd->rev_node_cache),
                                       memcache,
                                       svn_fs_fs__dag_serialize,
                                       svn_fs_fs__dag_deserialize,
                                       APR_HASH_KEY_STRING,
                                       apr_pstrcat(pool, prefix, "DAG",
                                                   (char *)NULL),
                                       fs->pool));
  else
    SVN_ERR(svn_cache__create_inprocess(&(ffd->rev_node_cache),
                                        svn_fs_fs__dag_dup_for_cache,
                                        APR_HASH_KEY_STRING,
                                        1024, 16, FALSE, fs->pool));
  if (! no_handler)
    SVN_ERR(svn_cache__set_error_handler(ffd->rev_node_cache,
                                         warn_on_cache_errors, fs, pool));


  /* Very rough estimate: 1K per directory. */
  if (memcache)
    SVN_ERR(svn_cache__create_memcache(&(ffd->dir_cache),
                                       memcache,
                                       svn_fs_fs__dir_entries_serialize,
                                       svn_fs_fs__dir_entries_deserialize,
                                       APR_HASH_KEY_STRING,
                                       apr_pstrcat(pool, prefix, "DIR",
                                                   (char *)NULL),
                                       fs->pool));
  else
    SVN_ERR(svn_cache__create_inprocess(&(ffd->dir_cache),
                                        dup_dir_listing, APR_HASH_KEY_STRING,
                                        1024, 8, FALSE, fs->pool));

  if (! no_handler)
    SVN_ERR(svn_cache__set_error_handler(ffd->dir_cache,
                                         warn_on_cache_errors, fs, pool));

  /* Only 16 bytes per entry (a revision number + the corresponding offset).
     Since we want ~8k pages, that means 512 entries per page. */
  if (memcache)
    SVN_ERR(svn_cache__create_memcache(&(ffd->packed_offset_cache),
                                       memcache,
                                       manifest_serialize,
                                       manifest_deserialize,
                                       sizeof(svn_revnum_t),
                                       apr_pstrcat(pool, prefix, "PACK-MANIFEST",
                                                   (char *)NULL),
                                       fs->pool));
  else
    SVN_ERR(svn_cache__create_inprocess(&(ffd->packed_offset_cache),
                                        dup_pack_manifest, sizeof(svn_revnum_t),
                                        32, 1, FALSE, fs->pool));

  if (! no_handler)
    SVN_ERR(svn_cache__set_error_handler(ffd->packed_offset_cache,
                                         warn_on_cache_errors, fs, pool));

  if (memcache)
    {
      SVN_ERR(svn_cache__create_memcache(&(ffd->fulltext_cache),
                                         memcache,
                                         /* Values are svn_string_t */
                                         NULL, NULL,
                                         APR_HASH_KEY_STRING,
                                         apr_pstrcat(pool, prefix, "TEXT",
                                                     (char *)NULL),
                                         fs->pool));
    }
  else if (svn_fs__get_global_membuffer_cache() && 
           svn_fs_get_cache_config()->cache_fulltexts)
    {
      SVN_ERR(svn_cache__create_membuffer_cache(&(ffd->fulltext_cache),
                                                svn_fs__get_global_membuffer_cache(),
                                                /* Values are svn_string_t */
                                                NULL, NULL,
                                                APR_HASH_KEY_STRING,
                                                apr_pstrcat(pool, prefix, "TEXT",
                                                            NULL),
                                                fs->pool));
    }
  else
    {
      ffd->fulltext_cache = NULL;
    }

  if (ffd->fulltext_cache && ! no_handler)
    SVN_ERR(svn_cache__set_error_handler(ffd->fulltext_cache,
            warn_on_cache_errors, fs, pool));

  return SVN_NO_ERROR;
}
