/*
 * svn_hash.h :  dumping and reading hash tables to/from files.
 *
 * ====================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */




#ifndef SVN_HASH_H
#define SVN_HASH_H

#include <apr_pools.h>
#include <apr_tables.h>         /* for apr_array_header_t */
#include <apr_hash.h>
#include <apr_file_io.h>


/*----------------------------------------------------*/

/*** Reading/writing hashtables to disk ***/

/* svn_hash_read() and svn_hash_write() each take a "helper" routine
   to encode/decode hash values. */

/*  Read a hash table from a file.
 *  Input:  a hash, a "pack" function, an opened file pointer, a pool
 *  Returns:  error status
 *
 *     The "pack" routine should take a specific-length bytestring and
 *     return a pointer to something meant to be stored in the hash.
 *
 *     The hash should be ready to receive key/val pairs.
 */
apr_status_t svn_hash_read (apr_hash_t *hash, 
                            void *(*pack_func) (size_t len, const char *val,
                                                apr_pool_t *pool),
                            apr_file_t *srcfile,
                            apr_pool_t *pool);

/*  Dump a hash table to a file.
 *  Input:  a hash, an "unpack" function (see above), an opened file pointer
 *  Returns:  error status
 *
 *     The "unpack" routine knows how to convert a hash value into a
 *     printable bytestring of a certain length.
 */
apr_status_t svn_hash_write (apr_hash_t *hash, 
                             apr_size_t (*unpack_func) (char **unpacked_data,
                                                        void *val),
                             apr_file_t *destfile);



/*** Helper routines specific to Subversion proplists. ***/

/* A helper for hash_write(): 
 * Input:   a hash value which points to an svn_string_t
 * Returns: the size of the svn_string_t, and (by indirection) the
 *          string data itself 
 */
apr_size_t svn_unpack_bytestring (char **returndata, void *value);

/* A helper for hash_read():
 * Input:   some bytes, a length, a pool
 * Returns: an svn_string_t containing them, to store as a hash value.
 *
 * Just copies the pointer, does not duplicate the data!
 */
void *svn_pack_bytestring (size_t len, const char *val, apr_pool_t *pool);


/*----------------------------------------------------*/

/*** Converting a hash into a sorted array ***/

/* FIXME: this may go away in a moment, please ignore it for now. */
int svn_sort_compare_as_paths (const void *obj1, const void *obj2);

#ifndef apr_hash_sorted_keys

/* Grab the keys (and values) in apr_hash HT and return them in an a
   sorted apr_array_header_t ARRAY allocated from POOL.  The array
   will contain pointers of type (apr_item_t *).  */
apr_array_header_t *
apr_hash_sorted_keys (apr_hash_t *ht,
                      int (*comparison_func) (const void *, const void *),
                      apr_pool_t *pool);
#endif /* apr_hash_sorted_keys */

#endif /* SVN_HASH_H */



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
