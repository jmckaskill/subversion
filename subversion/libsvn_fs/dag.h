/* dag.h : DAG-like interface filesystem, private to libsvn_fs
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

#ifndef SVN_LIBSVN_FS_DAG_H
#define SVN_LIBSVN_FS_DAG_H

#include "svn_fs.h"
#include "db.h"
#include "skel.h"
#include "trail.h"

/* The interface in this file provides all the essential filesystem
   operations, but exposes the filesystem's DAG structure.  This makes
   it simpler to implement than the public interface, since a client
   of this interface has to understand and cope with shared structure
   directly as it appears in the database.  However, it's still a
   self-consistent set of invariants to maintain, making it
   (hopefully) a useful interface boundary.

   In other words:

   - The dag_node_t interface exposes the internal DAG structure of
     the filesystem, while the svn_fs.h interface does any cloning
     necessary to make the filesystem look like a tree.

   - The dag_node_t interface exposes the existence of copy nodes,
     whereas the svn_fs.h handles them transparently.

   - dag_node_t's must be explicitly cloned, whereas the svn_fs.h
     operations make clones implicitly.

   - Callers of the dag_node_t interface use Berkeley DB transactions
     to ensure consistency between operations, while callers of the
     svn_fs.h interface use Subversion transactions.  */


/* Initializing a filesystem.  */


/* Given a filesystem FS, which contains all the necessary tables,
   create the initial revision 0, and the initial root directory.  */
svn_error_t *svn_fs__dag_init_fs (svn_fs_t *fs);



/* Generic DAG node stuff.  */

typedef struct dag_node_t dag_node_t;


/* Fill *NODE with a dag_node_t representing node revision ID in FS,
   allocating in TRAIL->pool.  */
svn_error_t *
svn_fs__dag_get_node (dag_node_t **node,
                      svn_fs_t *fs,
                      svn_fs_id_t *id,
                      trail_t *trail);


/* Return a new dag_node_t object referring to the same node as NODE,
   allocated in TRAIL->pool.  If you're trying to build a structure in
   TRAIL->pool that wants to refer to dag nodes that may have been
   allocated elsewhere, you can call this function, and avoid
   inter-pool pointers.  */
dag_node_t *svn_fs__dag_dup (dag_node_t *node,
                             trail_t *trail);


/* Return the filesystem containing NODE.  */
svn_fs_t *svn_fs__dag_get_fs (dag_node_t *node);


/* Return the node revision ID of NODE.  The value returned is shared
   with NODE, and will be deallocated when NODE is.  */
const svn_fs_id_t *svn_fs__dag_get_id (dag_node_t *node);


/* Set IS_MUTABLE to a non-zero value if NODE is currently mutable in
   TRAIL, or zero otherwise.  */
svn_error_t *svn_fs__dag_check_mutable (svn_boolean_t *is_mutable,
                                        dag_node_t *node,
                                        trail_t *trail);

/* Return true iff NODE is a file/directory/copy.  */
int svn_fs__dag_is_file (dag_node_t *node);
int svn_fs__dag_is_directory (dag_node_t *node);


/* Set *PROPLIST_P to a PROPLIST skel representing the entire property
   list of NODE, as part of TRAIL.  This guarantees that *PROPLIST_P
   is well-formed.

   The caller must not change the returned skel --- it's shared with
   dag.c's internal cache of node contents.

   The returned skel is allocated in *either* TRAIL->pool or the pool
   NODE was allocated in, at this function's discretion; the caller
   must finish using it while both of those remain live.  If the
   caller needs the property list to live longer, it can use
   svn_fs__copy_skel to make its own copy.  */
svn_error_t *svn_fs__dag_get_proplist (skel_t **proplist_p,
                                       dag_node_t *node,
                                       trail_t *trail);


/* Set the property list of NODE to PROPLIST, as part of TRAIL.  The
   node being changed must be mutable.  This verifies that PROPLIST is
   well-formed.  */
svn_error_t *svn_fs__dag_set_proplist (dag_node_t *node,
                                       skel_t *proplist,
                                       trail_t *trail);



/* Revision and transaction roots.  */


/* Open the root of revision REV of filesystem FS, as part of TRAIL.
   Set *NODE_P to the new node.  Allocate the node in TRAIL->pool.  */
svn_error_t *svn_fs__dag_revision_root (dag_node_t **node_p,
                                        svn_fs_t *fs,
                                        svn_revnum_t rev,
                                        trail_t *trail);


/* Set *NODE_P to the root of transaction TXN in FS, as part
   of TRAIL.  Allocate the node in TRAIL->pool.

   Note that the root node of TXN is not necessarily mutable.  If no
   changes have been made in the transaction, then it may share its
   root directory with its base revision.  To get a mutable root node
   for a transaction, call svn_fs__dag_clone_root.  */
svn_error_t *svn_fs__dag_txn_root (dag_node_t **node_p,
                                   svn_fs_t *fs,
                                   const char *txn,
                                   trail_t *trail);


/* Set *NODE_P to the base root of transaction TXN in FS, as part
   of TRAIL.  Allocate the node in TRAIL->pool.  */
svn_error_t *svn_fs__dag_txn_base_root (dag_node_t **node_p,
                                        svn_fs_t *fs,
                                        const char *txn,
                                        trail_t *trail);


/* Clone the root directory of SVN_TXN in FS, and update the
   `transactions' table entry to point to it, unless this has been
   done already.  In either case, set *ROOT_P to a reference to the
   root directory clone.  Do all this as part of TRAIL, and allocate
   *ROOT_P in TRAIL->pool.  */
svn_error_t *svn_fs__dag_clone_root (dag_node_t **root_p,
                                     svn_fs_t *fs,
                                     const char *svn_txn,
                                     trail_t *trail);


/* Commit the transaction SVN_TXN in FS, as part of TRAIL.  Store the
   new revision number in *NEW_REV.  This entails:
   - marking the tree of mutable nodes at SVN_TXN's root as immutable,
     and marking all their contents as stable
   - creating a new revision, with SVN_TXN's root as its root directory
   - deleting SVN_TXN from `transactions'

   Beware!  This does not make sure that SVN_TXN is based on the very
   latest revision in FS.  If the caller doesn't take care of this,
   you may lose people's work!

   Do any necessary temporary allocation in a subpool of TRAIL->pool.
   Consume temporary space at most proportional to the maximum depth
   of SVN_TXN's tree of mutable nodes.  */
svn_error_t *svn_fs__dag_commit_txn (svn_revnum_t *new_rev,
                                     svn_fs_t *fs,
                                     const char *svn_txn,
                                     trail_t *trail);



/* Directories.  */


/* Open the node named NAME in the directory PARENT, as part of TRAIL.
   Set *CHILD_P to the new node, allocated in TRAIL->pool.  NAME must be a
   single path component; it cannot be a slash-separated directory
   path.  */
svn_error_t *svn_fs__dag_open (dag_node_t **child_p,
                               dag_node_t *parent,
                               const char *name,
                               trail_t *trail);


/* Set *ENTRIES_P to the directory entry list skel of NODE, as part of
   TRAIL.  The returned skel has the form (ENTRY ...), as described in
   `structure'; this function guarantees that *ENTRIES_P is well-formed.

   The caller must not modify the returned skel --- it's shared with
   dag.c's internal cache of node contents.

   The returned skel is allocated in *either* TRAIL->pool or the pool
   NODE was allocated in, at this function's discretion; the caller
   must finish using it while both of those remain live.  If the
   caller needs the directory enttry list to live longer, it can use
   svn_fs__copy_skel to make its own copy.  */
svn_error_t *svn_fs__dag_dir_entries_skel (skel_t **entries_p,
                                           dag_node_t *node,
                                           trail_t *trail);


/* Set *ENTRIES_P to a hash table of NODE's entries, as part of
   TRAIL.  The keys of the table are entry names, and the values are
   svn_fs_dirent_t's.

   The returned table is allocated in *either* TRAIL->pool or the pool
   NODE was allocated in, at this function's discretion; the caller
   must finish using it while both of those remain live.  If the
   caller needs the table to live longer, it should copy the hash.  */
svn_error_t *svn_fs__dag_dir_entries_hash (apr_hash_t **entries_p,
                                           dag_node_t *node,
                                           trail_t *trail);


/* Set ENTRY_NAME in NODE to point to ID, as part of TRAIL.
   NODE must be a mutable directory.  ID can refer to a mutable or
   immutable node.  If ENTRY_NAME does not exist, it will be 
   created.  */
svn_error_t *svn_fs__dag_set_entry (dag_node_t *node,
                                    const char *entry_name,
                                    const svn_fs_id_t *id,
                                    trail_t *trail);


/* Make a new mutable clone of the node named NAME in PARENT, and
   adjust PARENT's directory entry to point to it, as part of TRAIL,
   unless NAME in PARENT already refers to a mutable node.  In either
   case, set *CHILD_P to a reference to the new node, allocated in
   TRAIL->pool.  PARENT must be mutable.  NAME must be a single path
   component; it cannot be a slash-separated directory path.  */
svn_error_t *svn_fs__dag_clone_child (dag_node_t **child_p,
                                      dag_node_t *parent,
                                      const char *name,
                                      trail_t *trail);


/* Why do we have both svn_fs__dag_link and svn_fs__dag_rename?

   They each have different limitations and abilities (kind of like
   superheroes!) that allow them to ensure the consistency of the
   filesystem.

   - svn_fs__dag_link can't rename mutable nodes.  But since CHILD is
     immutable, it knows that it's safe to create a new link to it:
     mutable nodes must have exactly one parent, while immutable nodes
     can be shared arbitrarily.

   - svn_fs__dag_rename always deletes one link, and adds another, as
     a single atomic operation.  Since it preserves the total number
     of links to the node being renamed, you can use it on both
     mutable nodes (which must always have one parent) and immutable
     nodes (which can have as many parents as they please).  But by
     the same token, you can't use it to create virtual copies, one of
     the filesystem's defining features.  */


/* Create a link to CHILD in PARENT named NAME, as part of TRAIL.
   PARENT must be mutable.  CHILD must be immutable.  NAME must be a
   single path component; it cannot be a slash-separated directory
   path.  

   Note that it is impossible to use this function to create cyclic
   directory structures.  Since PARENT is mutable, and every parent of
   a mutable node is mutable itself, and CHILD is immutable, we know
   that CHILD can't be equal to, or a parent of, PARENT.  */
svn_error_t *svn_fs__dag_link (dag_node_t *parent,
                               dag_node_t *child,
                               const char *name,
                               trail_t *trail);


/* Rename the node named FROM_NAME in FROM_DIR to TO_NAME in TO_DIR,
   as part of TRAIL.  FROM_DIR and TO_DIR must both be mutable; the
   node being renamed may be either mutable or immutable.  FROM_NAME
   and TO_NAME must be single path components; they cannot be
   slash-separated directory paths.

   This function ensures that the rename does not create a cyclic
   directory structure, by checking that TO_DIR is not a child of
   FROM_DIR.  */
svn_error_t *svn_fs__dag_rename (dag_node_t *from_dir, const char *from_name,
                                 dag_node_t *  to_dir, const char *  to_name,
                                 trail_t *trail);


/* Delete the directory entry named NAME from PARENT, as part of
   TRAIL.  PARENT must be mutable.  NAME must be a single path
   component; it cannot be a slash-separated directory path.  If the
   node being deleted is a directory, it must be empty.  */
svn_error_t *svn_fs__dag_delete (dag_node_t *parent,
                                 const char *name,
                                 trail_t *trail);


/* Delete the directory entry named NAME from PARENT, as part of
   TRAIL.  PARENT must be mutable.  NAME must be a single path
   component; it cannot be a slash-separated directory path.  If the
   node being deleted is a mutable directory, remove all mutable nodes
   reachable from it.  */
svn_error_t *svn_fs__dag_delete_tree (dag_node_t *parent,
                                      const char *name,
                                      trail_t *trail);


/* Delete all mutable node revisions reachable from node ID, including
   ID itself, from FS's `nodes' table, as part of TRAIL.  ID may refer
   to a file or directory, which may be mutable or immutable.  */
svn_error_t *svn_fs__dag_delete_if_mutable (svn_fs_t *fs,
                                            svn_fs_id_t *id,
                                            trail_t *trail);


/* Create a new mutable directory named NAME in PARENT, as part of
   TRAIL.  Set *CHILD_P to a reference to the new node, allocated in
   TRAIL->pool.  The new directory has no contents, and no properties.
   PARENT must be mutable.  NAME must be a single path component; it
   cannot be a slash-separated directory path.  PARENT must not
   currently have an entry named NAME.  Do any temporary allocation in
   TRAIL->pool.  */
svn_error_t *svn_fs__dag_make_dir (dag_node_t **child_p,
                                   dag_node_t *parent,
                                   const char *name,
                                   trail_t *trail);



/* Files.  */


/* Set *CONTENTS to a readable generic stream which yields the
   contents of FILE, as part of TRAIL.  Allocate the stream in POOL,
   which may or may not be TRAIL->pool.

   If FILE is not a file, return SVN_ERR_FS_NOT_FILE.  */
svn_error_t *svn_fs__dag_get_contents (svn_stream_t **contents,
                                       dag_node_t *file,
                                       apr_pool_t *pool,
                                       trail_t *trail);


/* Set the contents of FILE to CONTENTS, as part of TRAIL.  (Yes, this
   interface will need to be revised to handle large files; let's get
   things working first.)  */
svn_error_t *svn_fs__dag_set_contents (dag_node_t *file,
                                       svn_stringbuf_t *contents,
                                       trail_t *trail);


/* Set *LENGTH to the length of the contents of FILE, as part of TRAIL. */
svn_error_t *svn_fs__dag_file_length (apr_size_t *length,
                                      dag_node_t *file,
                                      trail_t *trail);


/* Create a new mutable file named NAME in PARENT, as part of TRAIL.
   Set *CHILD_P to a reference to the new node, allocated in
   TRAIL->pool.  The new file's contents are the empty string, and it
   has no properties.  PARENT must be mutable.  NAME must be a single
   path component; it cannot be a slash-separated directory path.  */
svn_error_t *svn_fs__dag_make_file (dag_node_t **child_p,
                                    dag_node_t *parent,
                                    const char *name,
                                    trail_t *trail);



/* Copies */

/* Make ENTRY in TO_NODE be a copy of FROM_NODE, as part of TRAIL.
   TO_NODE must be mutable.

   The new node will record the fact that it was copied from FROM_PATH
   in FROM_REV; therefore, FROM_NODE should be the node found at
   FROM_PATH in FROM_REV, although this is not checked.  */
svn_error_t *svn_fs__dag_copy (dag_node_t *to_node,
                               const char *entry,
                               dag_node_t *from_node,
                               svn_revnum_t from_rev,
                               const char *from_path,
                               trail_t *trail);


/* If NODE was copied from some other node, set *REV_P and *PATH_P to
   the revision and path of the other node, as part of TRAIL.
   Allocate *PATH_P in TRAIL->pool.

   Else if NODE is not a copy, set *REV_P to SVN_INVALID_REVNUM and
   *PATH_P to null.  */
svn_error_t *svn_fs__dag_copied_from (svn_revnum_t *rev_p,
                                      const char **path_p,
                                      dag_node_t *node,
                                      trail_t *trail);


#endif /* SVN_LIBSVN_FS_DAG_H */



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
