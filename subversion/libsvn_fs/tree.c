/* tree.c : tree-like filesystem, built on DAG filesystem
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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


/* The job of this layer is to take a filesystem with lots of node
   sharing going on --- the real DAG filesystem as it appears in the
   database --- and make it look and act like an ordinary tree
   filesystem, with no sharing.

   We do just-in-time cloning: you can walk from some unfinished
   transaction's root down into directories and files shared with
   committed revisions; as soon as you try to change something, the
   appropriate nodes get cloned (and parent directory entries updated)
   invisibly, behind your back.  Any other references you have to
   nodes that have been cloned by other changes, even made by other
   processes, are automatically updated to point to the right clones.  */


#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_fs.h"
#include "svn_hash.h"
#include "svn_sorts.h"
#include "id.h"
#include "fs.h"
#include "err.h"
#include "trail.h"
#include "node-rev.h"
#include "key-gen.h"
#include "txn.h"
#include "dag.h"
#include "tree.h"
#include "revs-txns.h"
#include "bdb/txn-table.h"
#include "bdb/rev-table.h"
#include "bdb/nodes-table.h"
#include "bdb/changes-table.h"
#include "bdb/copies-table.h"


/* ### I believe this constant will become internal to reps-strings.c.
   ### see the comment in window_consumer() for more information. */

/* ### the comment also seems to need tweaking: the log file stuff
   ### is no longer an issue... */
/* Data written to the filesystem through the svn_fs_apply_textdelta()
   interface is cached in memory until the end of the data stream, or
   until a size trigger is hit.  Define that trigger here (in bytes).
   Setting the value to 0 will result in no filesystem buffering at
   all.  The value only really matters when dealing with file contents
   bigger than the value itself.  Above that point, large values here
   allow the filesystem to buffer more data in memory before flushing
   to the database, which increases memory usage but greatly decreases
   the amount of disk access (and log-file generation) in database.
   Smaller values will limit your overall memory consumption, but can
   drastically hurt throughput by necessitating more write operations
   to the database (which also generates more log-files).  */
#define SVN_FS_WRITE_BUFFER_SIZE   512000


/* The root structure.  */

typedef enum root_kind_t {
  unspecified_root = 0,
  revision_root,
  transaction_root
} root_kind_t;

struct svn_fs_root_t
{

  /* What filesystem does this root belong to?  */
  svn_fs_t *fs;

  /* All data belonging to this root is allocated in this pool.
     Destroying this pool will correctly free all resources the root
     holds.  */
  apr_pool_t *pool;

  /* What kind of root is this?  */
  root_kind_t kind;

  /* For transaction roots (i.e., KIND == transaction_root), the name of
     that transaction, allocated in POOL.  */
  const char *txn;

  /* For revision roots (i.e., KIND == revision_root), the number of
     that revision.  */
  svn_revnum_t rev;

  /* For revision roots, this is a dag node for the revision's root
     directory.  For transaction roots, we open the root directory
     afresh every time, since the root may have been cloned, or
     the transaction may have disappeared altogether.  */
  dag_node_t *root_dir;
};



/* Creating root objects.  */


/* Construct a new root object in FS, allocated from POOL.  */
static svn_fs_root_t *
make_root (svn_fs_t *fs,
           apr_pool_t *pool)
{
  /* We create a subpool for each root object to allow us to implement
     svn_fs_close_root.  */
  apr_pool_t *subpool = svn_pool_create (pool);
  svn_fs_root_t *root = apr_pcalloc (subpool, sizeof (*root));

  root->fs = fs;
  root->pool = pool;

  return root;
}


/* Construct a root object referring to the root of REVISION in FS,
   whose root directory is ROOT_DIR.  Create the new root in POOL.  */
static svn_fs_root_t *
make_revision_root (svn_fs_t *fs,
                    svn_revnum_t rev,
                    dag_node_t *root_dir,
                    apr_pool_t *pool)
{
  svn_fs_root_t *root = make_root (fs, pool);
  root->kind = revision_root;
  root->rev = rev;
  root->root_dir = root_dir;

  return root;
}


/* Construct a root object referring to the root of the transaction
   named TXN in FS.  Create the new root in POOL.  */
static svn_fs_root_t *
make_txn_root (svn_fs_t *fs,
               const char *txn,
               apr_pool_t *pool)
{
  svn_fs_root_t *root = make_root (fs, pool);
  root->kind = transaction_root;
  root->txn = apr_pstrdup (root->pool, txn);

  return root;
}



/* Constructing nice error messages for roots.  */

/* Return a detailed `file not found' error message for PATH in ROOT.  */
static svn_error_t *
not_found (svn_fs_root_t *root, const char *path)
{
  if (root->kind == transaction_root)
    return
      svn_error_createf
      (SVN_ERR_FS_NOT_FOUND, 0,
       "file not found: transaction `%s', path `%s'",
       root->txn, path);
  else if (root->kind == revision_root)
    return
      svn_error_createf
      (SVN_ERR_FS_NOT_FOUND, 0,
       "file not found: revision `%" SVN_REVNUM_T_FMT "', path `%s'",
       root->rev, path);
  else
    abort ();
}


/* Return a detailed `file already exists' message for PATH in ROOT.  */
static svn_error_t *
already_exists (svn_fs_root_t *root, const char *path)
{
  svn_fs_t *fs = root->fs;

  if (root->kind == transaction_root)
    return
      svn_error_createf
      (SVN_ERR_FS_ALREADY_EXISTS, 0,
       "file already exists: filesystem `%s', transaction `%s', path `%s'",
       fs->path, root->txn, path);
  else if (root->kind == revision_root)
    return
      svn_error_createf
      (SVN_ERR_FS_ALREADY_EXISTS, 0,
       "file already exists: filesystem `%s', revision `%" SVN_REVNUM_T_FMT
       "', path `%s'", fs->path, root->rev, path);
  else
    abort ();
}


static svn_error_t *
not_txn (svn_fs_root_t *root)
{
  return svn_error_create
    (SVN_ERR_FS_NOT_TXN_ROOT, NULL,
     "root object must be a transaction root");
}



/* Simple root operations.  */

void
svn_fs_close_root (svn_fs_root_t *root)
{
  svn_pool_destroy (root->pool);
}


svn_fs_t *
svn_fs_root_fs (svn_fs_root_t *root)
{
  return root->fs;
}


int
svn_fs_is_txn_root (svn_fs_root_t *root)
{
  return root->kind == transaction_root;
}


int
svn_fs_is_revision_root (svn_fs_root_t *root)
{
  return root->kind == revision_root;
}


const char *
svn_fs_txn_root_name (svn_fs_root_t *root,
                      apr_pool_t *pool)
{
  if (root->kind == transaction_root)
    return apr_pstrdup (pool, root->txn);
  else
    return 0;
}


svn_revnum_t
svn_fs_revision_root_revision (svn_fs_root_t *root)
{
  if (root->kind == revision_root)
    return root->rev;
  else
    return SVN_INVALID_REVNUM;
}



/* Getting dag nodes for roots.  */


/* Set *NODE_P to a freshly opened dag node referring to the root
   directory of ROOT, as part of TRAIL.  */
static svn_error_t *
root_node (dag_node_t **node_p,
           svn_fs_root_t *root,
           trail_t *trail)
{
  if (root->kind == revision_root)
    {
      /* It's a revision root, so we already have its root directory
         opened.  */
      *node_p = svn_fs__dag_dup (root->root_dir, trail);
      return SVN_NO_ERROR;
    }
  else if (root->kind == transaction_root)
    {
      /* It's a transaction root.  Open a fresh copy.  */
      return svn_fs__dag_txn_root (node_p, root->fs, root->txn, trail);
    }
  else
    abort ();
}


/* Set *NODE_P to a mutable root directory for ROOT, cloning if
   necessary, as part of TRAIL.  ROOT must be a transaction root.  Use
   ERROR_PATH in error messages.  */
static svn_error_t *
mutable_root_node (dag_node_t **node_p,
                   svn_fs_root_t *root,
                   const char *error_path,
                   trail_t *trail)
{
  if (root->kind == transaction_root)
    return svn_fs__dag_clone_root (node_p, root->fs, 
                                   svn_fs_txn_root_name (root, trail->pool),
                                   trail);
  else
    /* If it's not a transaction root, we can't change its contents.  */
    return svn_fs__err_not_mutable (root->fs, root->rev, error_path);
}



/* Traversing directory paths.  */

typedef enum copy_id_inherit_t
{
  copy_id_inherit_self = 0,
  copy_id_inherit_parent,
  copy_id_inherit_new

} copy_id_inherit_t;

/* A linked list representing the path from a node up to a root
   directory.  We use this for cloning, and for operations that need
   to deal with both a node and its parent directory.  For example, a
   `delete' operation needs to know that the node actually exists, but
   also needs to change the parent directory.  */
typedef struct parent_path_t
{
  
  /* A node along the path.  This could be the final node, one of its
     parents, or the root.  Every parent path ends with an element for
     the root directory.  */
  dag_node_t *node;

  /* The name NODE has in its parent directory.  This is zero for the
     root directory, which (obviously) has no name in its parent.  */
  char *entry;

  /* The parent of NODE, or zero if NODE is the root directory.  */
  struct parent_path_t *parent;
  
} parent_path_t;



/* Allocate a new parent_path_t node from POOL, referring to NODE,
   ENTRY, PARENT, and COPY_ID.  */
static parent_path_t *
make_parent_path (dag_node_t *node,
                  char *entry,
                  parent_path_t *parent,
                  apr_pool_t *pool)
{
  parent_path_t *parent_path = apr_pcalloc (pool, sizeof (*parent_path));
  parent_path->node = node;
  parent_path->entry = entry;
  parent_path->parent = parent;
  return parent_path;
}


/* Return a null-terminated copy of the first component of PATH,
   allocated in POOL.  If path is empty, or consists entirely of
   slashes, return the empty string.

   If the component is followed by one or more slashes, we set *NEXT_P
   to point after the slashes.  If the component ends PATH, we set
   *NEXT_P to zero.  This means:
   - If *NEXT_P is zero, then the component ends the PATH, and there
     are no trailing slashes in the path.
   - If *NEXT_P points at PATH's terminating null character, then
     the component returned was the last, and PATH ends with one or more
     slash characters.
   - Otherwise, *NEXT_P points to the beginning of the next component
     of PATH.  You can pass this value to next_entry_name to extract
     the next component.  */

static char *
next_entry_name (const char **next_p,
                 const char *path,
                 apr_pool_t *pool)
{
  const char *end;

  /* Find the end of the current component.  */
  end = strchr (path, '/');

  if (! end)
    {
      /* The path contains only one component, with no trailing
         slashes.  */
      *next_p = 0;
      return apr_pstrdup (pool, path);
    }
  else
    {
      /* There's a slash after the first component.  Skip over an arbitrary
         number of slashes to find the next one.  */
      const char *next = end;
      while (*next == '/')
        next++;
      *next_p = next;
      return apr_pstrndup (pool, path, end - path);
    }
}


/* Flags for open_path.  */
typedef enum open_path_flags_t {

  /* The last component of the PATH need not exist.  (All parent
     directories must exist, as usual.)  If the last component doesn't
     exist, simply leave the `node' member of the bottom parent_path
     component zero.  */
  open_path_last_optional = 1

} open_path_flags_t;


/* Open the node identified by PATH in ROOT, as part of TRAIL.  Set
   *PARENT_PATH_P to a path from the node up to ROOT, allocated in
   TRAIL->pool.  The resulting *PARENT_PATH_P value is guaranteed to
   contain at least one element, for the root directory.

   If FLAGS & open_path_last_optional is zero, return an error if the
   node PATH refers to does not exist.  If it is non-zero, require all
   the parent directories to exist as normal, but if the final path
   component doesn't exist, simply return a path whose bottom `node'
   member is zero.  This option is useful for callers that create new
   nodes --- we find the parent directory for them, and tell them
   whether the entry exists already.  */
static svn_error_t *
open_path (parent_path_t **parent_path_p,
           svn_fs_root_t *root,
           const char *path,
           int flags,
           trail_t *trail)
{
  svn_fs_t *fs = root->fs;
  apr_pool_t *pool = trail->pool;
  const svn_fs_id_t *id;
  dag_node_t *here; /* The directory we're currently looking at.  */
  parent_path_t *parent_path; /* The path from HERE up to the root.  */
  const char *rest; /* The portion of PATH we haven't traversed yet.  */
  const char *canon_path = svn_fs__canonicalize_abspath (path, trail->pool);

  /* Make a parent_path item for the root node, using its own current
     copy id.  */
  SVN_ERR (root_node (&here, root, trail));
  id = svn_fs__dag_get_id (here);
  parent_path = make_parent_path (here, 0, 0, pool);
  rest = canon_path + 1; /* skip the leading '/', it saves in iteration */

  /* Whenever we are at the top of this loop:
     - HERE is our current directory,
     - ID is the node revision ID of HERE,
     - REST is the path we're going to find in HERE, and 
     - PARENT_PATH includes HERE and all its parents.  */
  for (;;)
    {
      const char *next;
      char *entry;
      dag_node_t *child;
      
      /* Parse out the next entry from the path.  */
      entry = next_entry_name (&next, rest, pool);
      
      if (*entry == '\0')
        {
          /* Given the behavior of next_entry_name, this happens when
             the path either starts or ends with a slash.  In either
             case, we stay put: the current directory stays the same,
             and we add nothing to the parent path.  */
          child = here;
        }
      else
        {
          /* If we found a directory entry, follow it.  */
          svn_error_t *err = svn_fs__dag_open (&child, here, entry, trail);

          /* "file not found" requires special handling.  */
          if (err && err->apr_err == SVN_ERR_FS_NOT_FOUND)
            {
              /* If this was the last path component, and the caller
                 said it was optional, then don't return an error;
                 just put a NULL node pointer in the path.  */
              
              svn_error_clear (err);
              
              if ((flags & open_path_last_optional)
                  && (! next || *next == '\0'))
                {
                  parent_path = make_parent_path (NULL, entry, parent_path, 
                                                  pool);
                  break;
                }
              else
                {
                  /* Build a better error message than svn_fs__dag_open
                     can provide, giving the root and full path name.  */
                  return not_found (root, path);
                }
            }
          
          /* Other errors we return normally.  */
          SVN_ERR (err);

          /* Now, make a parent_path item for CHILD. */
          parent_path = make_parent_path (child, entry, parent_path, pool);
        }
      
      /* Are we finished traversing the path?  */
      if (! next)
        break;
      
      /* The path isn't finished yet; we'd better be in a directory.  */
      if (! svn_fs__dag_is_directory (child))
        return svn_fs__err_not_directory (fs, path);
      
      rest = next;
      here = child;
    }

  *parent_path_p = parent_path;
  return SVN_NO_ERROR;
}


static const char *
parent_path_path (parent_path_t *parent_path,
                  apr_pool_t *pool)
{
  const char *path_so_far = "/";
  if (parent_path->parent)
    path_so_far = parent_path_path (parent_path->parent, pool);
  return parent_path->entry 
         ? svn_path_join (path_so_far, parent_path->entry, pool) 
         : path_so_far;
}

static svn_error_t *
get_id_path (const char **path,
             svn_fs_t *fs,
             const svn_fs_id_t *id,
             trail_t *trail)
{
  apr_hash_t *changes;
  apr_hash_index_t *hi;

  /* Initialize returned value. */
  *path = NULL;

  /* Fetch all the changes that occured in the transaction that child
     appeared in.  Find the change whose node revision id is ID, and
     return the path associated with it.  If no such change exists,
     return the default value. */
  SVN_ERR (svn_fs__bdb_changes_fetch (&changes, fs, 
                                      svn_fs__id_txn_id (id), trail));
  for (hi = apr_hash_first (trail->pool, changes); hi; hi = apr_hash_next (hi))
    {
      svn_fs_path_change_t *change;
      void *val;
      const void *key;
      const char *change_path;

      apr_hash_this (hi, &key, NULL, &val);
      change_path = key;
      change = val;
      if (svn_fs_compare_ids (change->node_rev_id, id) == 0)
        {
          *path = change_path;
          break;
        }
    }

  return SVN_NO_ERROR;
}


/* Choose a copy ID inheritance method *INHERIT_P to be used in the
   event that immutable node CHILD in FS needs to be made mutable.  If
   the inheritance method is copy_id_inherit_new, also return a
   *COPY_SRC_PATH on which to base the new copy ID (else return NULL
   for that path).  CHILD must have a parent (it cannot be the root
   node), and must be immutable (if it is mutable, it has already
   inherited its copy ID).  */
static svn_error_t *
choose_copy_id (copy_id_inherit_t *inherit_p,
                const char **copy_src_path,
                svn_fs_t *fs,
                parent_path_t *child,
                trail_t *trail)
{
  const svn_fs_id_t *child_id, *parent_id;
  const char *child_copy_id, *parent_copy_id;
  const char *child_path = NULL, *id_path = NULL;
  svn_fs__copy_t *copy;

  /* Make some assertions about the function input. */
  assert (child && child->parent);

  /* Initialize some convenience variables. */
  child_id = svn_fs__dag_get_id (child->node);
  parent_id = svn_fs__dag_get_id (child->parent->node);
  child_copy_id = svn_fs__id_copy_id (child_id);
  parent_copy_id = svn_fs__id_copy_id (parent_id);

  /* From this point on, we'll assume that the child will just take
     its copy ID from its parent. */
  *inherit_p = copy_id_inherit_parent;
  *copy_src_path = NULL;

  /* Special case: if the child's copy ID is '0', use the parent's
     copy ID. */
  if (strcmp (child_copy_id, "0") == 0)
    return SVN_NO_ERROR;

  /* Compare the copy IDs of the child and its parent.  If they are
     the same, then the child is already on the same branch as the
     parent, and should use the same mutability copy ID that the
     parent will use. */
  if (svn_fs__key_compare (child_copy_id, parent_copy_id) == 0)
    return SVN_NO_ERROR;

  /* If the child is on the same branch that the parent is on, the
     child should just use the same copy ID that the parent would use.
     Else, the child needs to generate a new copy ID to use should it
     need to be made mutable.  We will claim that child is on the same
     branch as its parent if the child itself is not a branch point,
     or if it is a branch point that we are accessing via its original
     copy destination path. */
  SVN_ERR (svn_fs__bdb_get_copy (&copy, fs, child_copy_id, trail));
  if (svn_fs_compare_ids (copy->dst_noderev_id, child_id) == -1)
    return SVN_NO_ERROR;

  /* Fetch all the changes that occured in the transaction that child
     appeared in.  Find the change whose node revision ID is the
     child, so we can determine if we are looking at the child via its
     original path or as a subtree item of a copied tree. */
  SVN_ERR (get_id_path (&id_path, fs, child_id, trail));
  child_path = parent_path_path (child, trail->pool);
  if (id_path && child_path && (strcmp (child_path, id_path) == 0))
    {
      *inherit_p = copy_id_inherit_self;
      return SVN_NO_ERROR;
    }

  /* We are pretty sure that the child node is an unedited nested
     branched node.  When it needs to be made mutable, it should claim
     a new copy ID. */
  *inherit_p = copy_id_inherit_new;
  *copy_src_path = id_path;
  return SVN_NO_ERROR;
}


/* Make the node referred to by PARENT_PATH mutable, if it isn't
   already, as part of TRAIL.  ROOT must be the root from which
   PARENT_PATH descends.  Clone any parent directories as needed.
   Adjust the dag nodes in PARENT_PATH to refer to the clones.  Use
   ERROR_PATH in error messages.  */
static svn_error_t *
make_path_mutable (svn_fs_root_t *root,
                   parent_path_t *parent_path,
                   const char *error_path,
                   trail_t *trail)
{
  dag_node_t *clone;
  const char *txn_id = svn_fs_txn_root_name (root, trail->pool);
  svn_fs_t *fs = svn_fs_root_fs (root);

  /* Is the node mutable already?  */
  if (svn_fs__dag_check_mutable (parent_path->node, txn_id))
    return SVN_NO_ERROR;

  /* Are we trying to clone the root, or somebody's child node?  */
  if (parent_path->parent)
    {
      const svn_fs_id_t *parent_id;
      const svn_fs_id_t *node_id = svn_fs__dag_get_id (parent_path->node);
      const char *copy_id = NULL;
      const char *copy_src_path;
      copy_id_inherit_t inherit;

      /* Figure out what type of inheritance to use for our copy ID. */
      SVN_ERR (choose_copy_id (&inherit, &copy_src_path, fs,
                               parent_path, trail));

      /* We're trying to clone somebody's child.  Make sure our parent
         is mutable.  */
      SVN_ERR (make_path_mutable (root, parent_path->parent, 
                                  error_path, trail));

      switch (inherit)
        {
        case copy_id_inherit_parent:
          parent_id = svn_fs__dag_get_id (parent_path->parent->node);
          copy_id = svn_fs__id_copy_id (parent_id);
          break;
          
        case copy_id_inherit_new:
          SVN_ERR (svn_fs__bdb_reserve_copy_id (&copy_id, fs, trail));
          break;

        case copy_id_inherit_self:
        default:
          copy_id = NULL;
          break;
        }
          
      /* Now make this node mutable.  */
      SVN_ERR (svn_fs__dag_clone_child (&clone,
                                        parent_path->parent->node,
                                        parent_path->entry, 
                                        copy_id, txn_id,
                                        trail));
      
      /* If we just created a brand new copy ID, we need to store a
         `copies' table entry for it, as well as a notation in the
         transaction that should this transaction be terminated, our
         new copy needs to be removed. */
      if (inherit == copy_id_inherit_new)
        {
          const svn_fs_id_t *new_node_id = svn_fs__dag_get_id (clone);
          SVN_ERR (svn_fs__bdb_create_copy (copy_id, fs, copy_src_path, 
                                            svn_fs__id_txn_id (node_id),
                                            new_node_id, trail));
          SVN_ERR (svn_fs__add_txn_copy (fs, txn_id, copy_id, trail));
        }
    }
  else
    /* We're trying to clone the root directory.  */
    SVN_ERR (mutable_root_node (&clone, root, error_path, trail));

  /* Update the PARENT_PATH link to refer to the clone.  */
  parent_path->node = clone;
  return SVN_NO_ERROR;
}


/* Open the node identified by PATH in ROOT, as part of TRAIL.  Set
   *DAG_NODE_P to the node we find, allocated in TRAIL->pool.  Return
   an error if this node doesn't exist. */
static svn_error_t *
get_dag (dag_node_t **dag_node_p,
         svn_fs_root_t *root,
         const char *path,
         trail_t *trail)
{
  parent_path_t *parent_path;

  /* Call open_path with no flags, as we want this to return an error
     if the node for which we are searching doesn't exist. */
  SVN_ERR (open_path (&parent_path, root, path, 0, trail));
  *dag_node_p = parent_path->node;

  return SVN_NO_ERROR;
}



/* Populating the `changes' table. */

/* Add a change to the changes table in FS, keyed on transaction id
   TXN_ID, and indicated that a change of kind CHANGE_KIND occured on
   PATH (whose node revision id is--or was, in the case of a
   deletion--NODEREV_ID), and optionally that TEXT_MODs or PROP_MODs
   occured.  Do all this as part of TRAIL.  */
static svn_error_t *
add_change (svn_fs_t *fs,
            const char *txn_id,
            const char *path,
            const svn_fs_id_t *noderev_id,
            svn_fs_path_change_kind_t change_kind,
            int text_mod,
            int prop_mod,
            trail_t *trail)
{
  svn_fs__change_t change;
  change.path = svn_fs__canonicalize_abspath (path, trail->pool);
  change.noderev_id = noderev_id;
  change.kind = change_kind;
  change.text_mod = text_mod;
  change.prop_mod = prop_mod;
  return svn_fs__bdb_changes_add (fs, txn_id, &change, trail);
}



/* Generic node operations.  */


struct node_id_args {
  const svn_fs_id_t **id_p;
  svn_fs_root_t *root;
  const char *path;
};


static svn_error_t *
txn_body_node_id (void *baton, trail_t *trail)
{
  struct node_id_args *args = baton;
  dag_node_t *node;

  SVN_ERR (get_dag (&node, args->root, args->path, trail));
  *args->id_p = svn_fs__id_copy (svn_fs__dag_get_id (node), trail->pool);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_node_id (const svn_fs_id_t **id_p,
                svn_fs_root_t *root,
                const char *path,
                apr_pool_t *pool)
{
  const svn_fs_id_t *id;
  struct node_id_args args;

  args.id_p = &id;
  args.root = root;
  args.path = path;

  SVN_ERR (svn_fs__retry_txn (root->fs, txn_body_node_id, &args, pool));
  *id_p = id;
  return SVN_NO_ERROR;
}


struct node_created_rev_args {
  svn_revnum_t revision;
  svn_fs_root_t *root;
  const char *path;
};


static svn_error_t *
txn_body_node_created_rev (void *baton, trail_t *trail)
{
  struct node_created_rev_args *args = baton;
  dag_node_t *node;

  SVN_ERR (get_dag (&node, args->root, args->path, trail));
  SVN_ERR (svn_fs__dag_get_revision (&(args->revision), node, trail));
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_node_created_rev (svn_revnum_t *revision,
                         svn_fs_root_t *root,
                         const char *path,
                         apr_pool_t *pool)
{
  struct node_created_rev_args args;

  args.revision = SVN_INVALID_REVNUM;
  args.root = root;
  args.path = path;
  SVN_ERR (svn_fs__retry_txn 
           (root->fs, txn_body_node_created_rev, &args, pool));
  *revision = args.revision;
  return SVN_NO_ERROR;
}


struct node_kind_args {
  svn_fs_root_t *root;
  const char *path;

  svn_node_kind_t kind; /* OUT parameter */
};


static svn_error_t *
txn_body_node_kind (void *baton, trail_t *trail)
{
  struct node_kind_args *args = baton;
  dag_node_t *node;

  SVN_ERR (get_dag (&node, args->root, args->path, trail));
  args->kind = svn_fs__dag_node_kind (node);
  
  return SVN_NO_ERROR;
}

svn_node_kind_t svn_fs_check_path (svn_fs_root_t *root,
                                   const char *path,
                                   apr_pool_t *pool)
{
  struct node_kind_args args;
  svn_error_t *err;

  args.root = root;
  args.path = path;

  err = svn_fs__retry_txn (root->fs, txn_body_node_kind, &args, pool);
  if (err)
    {
      svn_error_clear (err);
      return svn_node_none;
    }

  return args.kind;
}

svn_error_t *
svn_fs_is_dir (int *is_dir,
               svn_fs_root_t *root,
               const char *path,
               apr_pool_t *pool)
{
  struct node_kind_args args;

  args.root = root;
  args.path = path;

  SVN_ERR (svn_fs__retry_txn (root->fs, txn_body_node_kind, &args, pool));
  *is_dir = (args.kind == svn_node_dir);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_fs_is_file (int *is_file,
                svn_fs_root_t *root,
                const char *path,
                apr_pool_t *pool)
{
  struct node_kind_args args;

  args.root = root;
  args.path = path;

  SVN_ERR (svn_fs__retry_txn (root->fs, txn_body_node_kind, &args, pool));
  *is_file = (args.kind == svn_node_file);

  return SVN_NO_ERROR;
}


struct is_different_args
{
  int is_different;
  svn_fs_root_t *root1;
  const char *path1;
  svn_fs_root_t *root2;
  const char *path2;
};


static svn_error_t *
txn_body_is_different (void *baton, trail_t *trail)
{
  struct is_different_args *args = baton;
  dag_node_t *node1, *node2;
  svn_node_kind_t kind;
  int props_differ, contents_differ;

  /* Assume the paths *are* different. */
  args->is_different = 1;

  /* Get the node revisions for these paths. */
  SVN_ERR (get_dag (&node1, args->root1, args->path1, trail));
  SVN_ERR (get_dag (&node2, args->root2, args->path2, trail));

  /* If they have the same node-ID, they're the same! */
  if (svn_fs__dag_get_id (node1) == svn_fs__dag_get_id (node2))
    {
      args->is_different = 0;
      return SVN_NO_ERROR;
    }

  /* If their kinds differ, they differ. */
  if ((kind = svn_fs__dag_node_kind (node1)) != svn_fs__dag_node_kind (node2))
    return SVN_NO_ERROR;

  /* Now call our internal differencing checker thingamabob.  This
     will only compare representation IDs, though, so it only tells us
     when things have the same contents, really, not when they have
     different contents. 

     ### todo: Finish svn_fs__things_different to do the full content
     comparison it was intended to do.  */
  SVN_ERR (svn_fs__things_different (&props_differ,
                                     &contents_differ,
                                     node1, node2, trail));

  if (! (props_differ || contents_differ))
    {
      args->is_different = 0;
      return SVN_NO_ERROR;
    }
      
  return SVN_NO_ERROR;
}


/* Note:  it is acceptable for this function to call back into
   public FS API interfaces because it does not itself use trails.  */
svn_error_t *
svn_fs_is_different (int *is_different,
                     svn_fs_root_t *root1,
                     const char *path1,
                     svn_fs_root_t *root2,
                     const char *path2,
                     apr_pool_t *pool)
{
  struct is_different_args args;

  if ((svn_fs_root_fs (root1)) != (svn_fs_root_fs (root2)))
    return svn_error_create
      (SVN_ERR_FS_GENERAL, NULL,
       "Asking is different in two different filesystems.");

  args.root1 = root1;
  args.path1 = path1;
  args.root2 = root2;
  args.path2 = path2;

  SVN_ERR (svn_fs__retry_txn (root1->fs, txn_body_is_different, &args, pool));
  *is_different = args.is_different;
  return SVN_NO_ERROR;
}


struct node_prop_args
{
  svn_string_t **value_p;
  svn_fs_root_t *root;
  const char *path;
  const char *propname;
};


static svn_error_t *
txn_body_node_prop (void *baton,
                    trail_t *trail)
{
  struct node_prop_args *args = baton;
  dag_node_t *node;
  apr_hash_t *proplist;

  SVN_ERR (get_dag (&node, args->root, args->path, trail));
  SVN_ERR (svn_fs__dag_get_proplist (&proplist, node, trail));
  *(args->value_p) = NULL;
  if (proplist)
    *(args->value_p) = apr_hash_get (proplist, args->propname, 
                                     APR_HASH_KEY_STRING);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_node_prop (svn_string_t **value_p,
                  svn_fs_root_t *root,
                  const char *path,
                  const char *propname,
                  apr_pool_t *pool)
{
  struct node_prop_args args;
  svn_string_t *value;

  args.value_p  = &value;
  args.root     = root;
  args.path     = path;
  args.propname = propname;
  SVN_ERR (svn_fs__retry_txn (root->fs, txn_body_node_prop, &args, pool));

  *value_p = value;
  return SVN_NO_ERROR;
}


struct node_proplist_args {
  apr_hash_t **table_p;
  svn_fs_root_t *root;
  const char *path;
};


static svn_error_t *
txn_body_node_proplist (void *baton, trail_t *trail)
{
  struct node_proplist_args *args = baton;
  parent_path_t *parent_path;
  apr_hash_t *proplist;

  SVN_ERR (open_path (&parent_path, args->root, args->path, 0, trail));
  SVN_ERR (svn_fs__dag_get_proplist (&proplist, parent_path->node, trail));
  *args->table_p = proplist ? proplist : apr_hash_make (trail->pool);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_node_proplist (apr_hash_t **table_p,
                      svn_fs_root_t *root,
                      const char *path,
                      apr_pool_t *pool)
{
  apr_hash_t *table;
  struct node_proplist_args args;

  args.table_p = &table;
  args.root = root;
  args.path = path;

  SVN_ERR (svn_fs__retry_txn (root->fs, txn_body_node_proplist, &args, pool));

  *table_p = table;
  return SVN_NO_ERROR;
}


struct change_node_prop_args {
  svn_fs_root_t *root;
  const char *path;
  const char *name;
  const svn_string_t *value;
};


static svn_error_t *
txn_body_change_node_prop (void *baton,
                           trail_t *trail)
{
  struct change_node_prop_args *args = baton;
  parent_path_t *parent_path;
  apr_hash_t *proplist;
  const char *txn_id = svn_fs_txn_root_name (args->root, trail->pool);

  SVN_ERR (open_path (&parent_path, args->root, args->path, 0, trail));
  SVN_ERR (make_path_mutable (args->root, parent_path, args->path, trail));
  SVN_ERR (svn_fs__dag_get_proplist (&proplist, parent_path->node, trail));

  /* If there's no proplist, but we're just deleting a property, exit now. */
  if ((! proplist) && (! args->value))
    return SVN_NO_ERROR;

  /* Now, if there's no proplist, we know we need to make one. */
  if (! proplist)
    proplist = apr_hash_make (trail->pool);

  /* Set the property. */
  apr_hash_set (proplist, args->name, APR_HASH_KEY_STRING, args->value);

  /* Overwrite the node's proplist. */
  SVN_ERR (svn_fs__dag_set_proplist (parent_path->node, proplist, 
                                     txn_id, trail));

  /* Make a record of this modification in the changes table. */
  SVN_ERR (add_change (svn_fs_root_fs (args->root), txn_id, 
                       args->path, svn_fs__dag_get_id (parent_path->node),
                       svn_fs_path_change_modify, 0, 1, trail));
  
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_change_node_prop (svn_fs_root_t *root,
                         const char *path,
                         const char *name,
                         const svn_string_t *value,
                         apr_pool_t *pool)
{
  struct change_node_prop_args args;

  if (! svn_fs_is_txn_root (root))
    return not_txn (root);

  args.root  = root;
  args.path  = path;
  args.name  = name;
  args.value = value;
  SVN_ERR (svn_fs__retry_txn (root->fs, txn_body_change_node_prop, &args,
                              pool));

  return SVN_NO_ERROR;
}


struct things_changed_args
{
  int *changed_p;
  svn_fs_root_t *root1;
  svn_fs_root_t *root2;
  const char *path1;
  const char *path2;
  apr_pool_t *pool;
};


static svn_error_t *
txn_body_props_changed (void *baton, trail_t *trail)
{
  struct things_changed_args *args = baton;
  parent_path_t *parent_path_1, *parent_path_2;

  SVN_ERR (open_path (&parent_path_1, args->root1, args->path1, 0, trail));
  SVN_ERR (open_path (&parent_path_2, args->root2, args->path2, 0, trail));

  SVN_ERR (svn_fs__things_different (args->changed_p,
                                     NULL,
                                     parent_path_1->node,
                                     parent_path_2->node,
                                     trail));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_props_changed (int *changed_p,
                      svn_fs_root_t *root1,
                      const char *path1,
                      svn_fs_root_t *root2,
                      const char *path2,
                      apr_pool_t *pool)
{
  struct things_changed_args args;
  
  /* Check that roots are in the same fs. */
  if ((svn_fs_root_fs (root1)) != (svn_fs_root_fs (root2)))
    return svn_error_create
      (SVN_ERR_FS_GENERAL, NULL,
       "Asking props changed in two different filesystems.");
  
  args.root1      = root1;
  args.root2      = root2;
  args.path1      = path1;
  args.path2      = path2;
  args.changed_p  = changed_p;
  args.pool       = pool;

  SVN_ERR (svn_fs__retry_txn (root1->fs, txn_body_props_changed,
                              &args, pool));

  return SVN_NO_ERROR;
}



/* Merges and commits. */


struct deltify_committed_args
{
  svn_fs_t *fs; /* the filesystem */
  svn_revnum_t rev; /* revision just committed */
  const char *txn_id; /* transaction just committed */
};


/* Redeltify predecessor node-revisions of the one we added.  The idea
   is to require at most 2*lg(N) deltas to be applied to get to any
   node-revision in a chain of N predecessors.  We do this using a
   technique derived from skip lists:

     Always redeltify the immediate parent
     If the number of predecessors is divisible by 2, deltify
      the revision two predecessors back
     If the number of predecessors is divisible by 4, deltify
      the revision four predecessors back
     etc.

   That's the theory, anyway.  Unfortunately, if we strictly follow
   that theory we get a bunch of overhead up front and no great
   benefit until the number of predecessors gets large.  So, stop at
   redeltifying the parent if the number of predecessors is less than
   32, and also skip the second level (redeltifying two predecessors
   back), since that doesn't help much.  Also, don't redeltify the
   oldest node-revision; it's potentially expensive and doesn't help
   retrieve any other revision.  (Retrieving the oldest node-revision
   will still be fast, just not as blindingly so.)  */
static svn_error_t *
txn_deltify (dag_node_t *node, int pred_count, int props_only, trail_t *trail)
{
  int nlevels, lev, count;
  dag_node_t *prednode;
  svn_fs_t *fs;

  /* Decide how many predecessors to redeltify.  To save overhead,
     don't redeltify anything but the immediate parent if there are
     less than 32 predecessors. */
  nlevels = 1;
  if (pred_count >= 32)
    {
      while (pred_count % 2 == 0)
        {
          pred_count /= 2;
          nlevels++;
        }

      /* Don't redeltify the oldest revision. */
      if (1 << (nlevels - 1) == pred_count)
        nlevels--;
    }

  /* Redeltify the desired number of predecessors. */
  count = 0;
  prednode = node;
  fs = svn_fs__dag_get_fs (node);
  for (lev = 0; lev < nlevels; lev++)
    {
      /* To save overhead, skip the second level (that is, never
         redeltify the node-revision two predecessors back). */
      if (lev == 1)
        continue;

      /* Note that COUNT is not reset between levels, and neither is
         PREDNODE; we just keep counting from where we were up to
         where we're supposed to get. */
      while (count < (1 << lev))
        {
          const svn_fs_id_t *pred_id;

          SVN_ERR (svn_fs__dag_get_predecessor_id (&pred_id, prednode, trail));
          if (pred_id == NULL)
            return svn_error_create (SVN_ERR_FS_CORRUPT, 0,
                                     "faulty predecessor count");
          SVN_ERR (svn_fs__dag_get_node (&prednode, fs, pred_id, trail));
          count++;
        }
      SVN_ERR (svn_fs__dag_deltify (prednode, node, props_only, trail));
    }

  return SVN_NO_ERROR;
}


/* Deltify ID's predecessor iff ID is mutable under TXN_ID in FS.  If
   ID is a mutable directory, recurse.  Do this as part of TRAIL. */
static svn_error_t *
deltify_if_mutable_under_txn_id (svn_fs_t *fs,
                                 const svn_fs_id_t *id,
                                 const char *txn_id,
                                 trail_t *trail)
{
  svn_fs__node_revision_t *noderev;
  dag_node_t *node;
  apr_hash_t *entries;
  int is_dir;

  /* Not mutable?  Go no further.  This is safe to do because for
     items in the tree to be mutable, their parent dirs must also be
     mutable.  Therefore, if a directory is not mutable under TXN_ID,
     its children cannot be.  */
  if (strcmp (svn_fs__id_txn_id (id), txn_id))
    return SVN_NO_ERROR;

  /* Get the NODE and node revision for ID. */
  SVN_ERR (svn_fs__dag_get_node (&node, fs, id, trail));
  SVN_ERR (svn_fs__bdb_get_node_revision (&noderev, fs, id, trail));

  /* If this is a directory, recurse on its entries. */
  if ((is_dir = svn_fs__dag_is_directory (node)))
    {
      SVN_ERR (svn_fs__dag_dir_entries (&entries, node, trail));
      if (entries)
        {
          apr_hash_index_t *hi;

          for (hi = apr_hash_first (trail->pool, entries); 
               hi; 
               hi = apr_hash_next (hi))
            {
              const svn_fs_dirent_t *dirent;
              void *val;
              
              /* KEY will be the entry name (about which we really
                 don't care), VAL the dirent */
              apr_hash_this (hi, NULL, NULL, &val);
              dirent = val;

              SVN_ERR (deltify_if_mutable_under_txn_id (fs, dirent->id,
                                                        txn_id, trail));
            }
        }
    }

  if (noderev->predecessor_id)
    SVN_ERR (txn_deltify (node, noderev->predecessor_count, is_dir, trail));

  return SVN_NO_ERROR;
}


static svn_error_t *
txn_body_deltify_committed (void *baton, trail_t *trail)
{
  struct deltify_committed_args *args = baton;
  const svn_fs_id_t *id;
  dag_node_t *root_dir;

  /* Get the ID of the root dir of the revision. */
  SVN_ERR (svn_fs__dag_revision_root (&root_dir, args->fs, args->rev, trail));
  id = svn_fs__dag_get_id (root_dir);

  /* Now...deltify! */
  return deltify_if_mutable_under_txn_id (args->fs, id, args->txn_id, trail);
}

struct get_root_args
{
  svn_fs_root_t *root;
  dag_node_t *node;
};


/* Set ARGS->node to the root node of ARGS->root.  */
static svn_error_t *
txn_body_get_root (void *baton, trail_t *trail)
{
  struct get_root_args *args = baton;
  SVN_ERR (get_dag (&(args->node), args->root, "", trail));
  return SVN_NO_ERROR;
}


/* Set *IS_ANCESTOR to non-zero iff ID1 is an ancestor of ID2 in FS,
   as part of TRAIL. */
static svn_error_t *
id_check_ancestor (int *is_ancestor,
                   svn_fs_t *fs, 
                   const svn_fs_id_t *id1, 
                   const svn_fs_id_t *id2,
                   trail_t *trail)
{
  dag_node_t *node1, *node2;

  /* Get the nodes. */
  SVN_ERR (svn_fs__dag_get_node (&node1, fs, id1, trail));
  SVN_ERR (svn_fs__dag_get_node (&node2, fs, id2, trail));
  
  /* Do the test.  If the test fails, we'll just go with "not an
     ancestor" for now.  ### better come back and check this out.  */
  return svn_fs__dag_is_ancestor (is_ancestor, node1, node2, trail);
}


static svn_error_t *
update_ancestry (svn_fs_t *fs,
                 const svn_fs_id_t *source_id,
                 const svn_fs_id_t *target_id,
                 const char *txn_id,
                 const char *target_path,
                 int source_pred_count,
                 trail_t *trail)
{
  svn_fs__node_revision_t *noderev;
  
  /* Set target's predecessor-id to source_id.  */
  if (strcmp (svn_fs__id_txn_id (target_id), txn_id))
    return svn_error_createf
      (SVN_ERR_FS_NOT_MUTABLE, NULL,
       "unexpected immutable node at \"%s\"", target_path);
  SVN_ERR (svn_fs__bdb_get_node_revision (&noderev, fs, target_id, trail));
  noderev->predecessor_id = source_id;
  noderev->predecessor_count = source_pred_count;
  if (noderev->predecessor_count != -1)
    noderev->predecessor_count++;
  return svn_fs__bdb_put_node_revision (fs, target_id, noderev, trail);
}


/* Possibly */
static svn_error_t *
undelete_change (svn_fs_t *fs,
                 const char *path,
                 const char *txn_id,
                 trail_t *trail)
{
  apr_hash_t *changes;
  svn_fs_path_change_t *this_change;

  /* Canonicalize PATH. */
  path = svn_fs__canonicalize_abspath (path, trail->pool);

  /* First, get the changes associated with TXN_ID. */
  SVN_ERR (svn_fs__bdb_changes_fetch (&changes, fs, txn_id, trail));
  
  /* Now, do any of those changes apply to path and indicate deletion? */
  this_change = apr_hash_get (changes, path, APR_HASH_KEY_STRING);
  if (this_change
      && ((this_change->change_kind == svn_fs_path_change_delete)
          || (this_change->change_kind == svn_fs_path_change_replace)))
    {
      /* If so, reset the changes and re-add everything except the
         deletion. */
      SVN_ERR (add_change (fs, txn_id, path, NULL, 
                           svn_fs_path_change_reset, 0, 0, trail));
      if (this_change->change_kind == svn_fs_path_change_replace)
        {
          SVN_ERR (add_change (fs, txn_id, path, this_change->node_rev_id, 
                               svn_fs_path_change_add, this_change->text_mod, 
                               this_change->prop_mod, trail));
        }
    }
  else
    {
      /* Else, this function was called in error, OR something is not
         as we expected it to be in the changes table. */
      return svn_error_createf 
        (SVN_ERR_FS_CORRUPT, NULL,
         "undelete_change: no deletion changes for path `%s' "
         "in transaction `%s' of filesystem `%s'",
         path, txn_id, fs->path);
    }

  return SVN_NO_ERROR;
}
                       

/* Set the contents of CONFLICT_PATH to PATH, and return an
   SVN_ERR_FS_CONFLICT error that indicates that there was a conflict
   at PATH.  Perform all allocations in POOL (except the allocation of
   CONFLICT_PATH, which should be handled outside this function).  */
static svn_error_t *
conflict_err (svn_stringbuf_t *conflict_path,
              const char *path)
{
  svn_stringbuf_set (conflict_path, path);
  return svn_error_createf (SVN_ERR_FS_CONFLICT, NULL,
                            "conflict at \"%s\"", path);
}


/* Merge changes between ANCESTOR and SOURCE into TARGET, as part of
 * TRAIL.  ANCESTOR and TARGET must be distinct node revisions.
 * TARGET_PATH should correspond to TARGET's full path in its
 * filesystem, and is used for reporting conflict location.
 *
 * SOURCE, TARGET, and ANCESTOR are generally directories; this
 * function recursively merges the directories' contents.  If any are
 * files, this function simply returns an error whenever SOURCE,
 * TARGET, and ANCESTOR are all distinct node revisions.
 *
 * If there are differences between ANCESTOR and SOURCE that conflict
 * with changes between ANCESTOR and TARGET, this function returns an
 * SVN_ERR_FS_CONFLICT error, and updates CONFLICT_P to the name of the
 * conflicting node in TARGET, with TARGET_PATH prepended as a path.
 *
 * If there are no conflicting differences, CONFLICT_P is updated to
 * the empty string.
 *
 * CONFLICT_P must point to a valid svn_stringbuf_t.
 *
 * Do any necessary temporary allocation in TRAIL->pool.
 */
static svn_error_t *
merge (svn_stringbuf_t *conflict_p,
       const char *target_path,
       dag_node_t *target,
       dag_node_t *source,
       dag_node_t *ancestor,
       const char *txn_id,
       trail_t *trail)
{
  const svn_fs_id_t *source_id, *target_id, *ancestor_id;
  apr_hash_t *s_entries, *t_entries, *a_entries;
  apr_hash_index_t *hi;
  svn_fs_t *fs;

  /* Make sure everyone comes from the same filesystem. */
  fs = svn_fs__dag_get_fs (ancestor);
  if ((fs != svn_fs__dag_get_fs (source))
      || (fs != svn_fs__dag_get_fs (target)))
    {
      return svn_error_create
        (SVN_ERR_FS_CORRUPT, NULL,
         "Bad merge -- ancestor, source, and target not all in same fs");
    }

  /* We have the same fs, now check it. */
  SVN_ERR (svn_fs__check_fs (fs));

  source_id   = svn_fs__dag_get_id (source);
  target_id   = svn_fs__dag_get_id (target);
  ancestor_id = svn_fs__dag_get_id (ancestor);

  /* It's improper to call this function with ancestor == target. */
  if (svn_fs__id_eq (ancestor_id, target_id))
    {
      svn_string_t *id_str = svn_fs_unparse_id (target_id, trail->pool);
      return svn_error_createf
        (SVN_ERR_FS_GENERAL, NULL,
         "Bad merge call -- target `%s' has id `%s', same as ancestor.",
         target_path, id_str->data);
    }

  svn_stringbuf_setempty (conflict_p);

  /* Base cases:
   * Either no change made in source, or same change as made in target.
   * Both mean nothing to merge here.
   */
  if (svn_fs__id_eq (ancestor_id, source_id)
      || (svn_fs__id_eq (source_id, target_id)))
    return SVN_NO_ERROR;

  /* Else proceed, knowing all three are distinct node revisions.
   *
   * How to merge from this point: 
   *
   * if (not all 3 are directories)
   *   {
   *     early exit with conflict;
   *   }
   *
   * // Property changes may only be made to up-to-date
   * // directories, because once the client commits the prop
   * // change, it bumps the directory's revision, and therefore
   * // must be able to depend on there being no other changes to
   * // that directory in the repository.
   * if (target's property list differs from ancestor's)
   *    conflict;
   *
   * for (each entry E in ancestor)
   *   {
   *     if (E exists in target and source)
   *       {
   *         if (source entry points to different id than E)
   *           {
   *             if (target entry points to same id as ancestor E)
   *               change target to point to same id as source entry;
   *             else if ((target entry id different from source)
   *                      && (target entry not descended from source))
   *               {
   *                 if (not all 3 entries point to directories)
   *                   {
   *                     early exit with conflict;
   *                   }
   *
   *                 // We know they are different directories, so...
   *                 recursively merge;
   *               }
   *             // Else target entry same as source entry, or is
   *             // descendant of source entry; either way, leave it.
   *           }
   *       }
   *     else if (E exists in source but not target)
   *       { 
   *         if (E changed between ancestor and source)
   *           conflict;
   *         else if (E did not change between ancestor and source)
   *           // do nothing 
   *     else if (E exists in target but not source)
   *       {
   *         if (E points the same node rev in target and ancestor)
   *            delete E from target;
   *         else // E points to different node revs in target & ancestor
   *           {
   *             if (E in target is not related to E in ancestor)
   *               conflict;
   *             else
   *               // do nothing
   *           }
   *       }
   *     else
   *       {
   *         // E exists in neither target nor source, so it's a
   *         // double delete -- do nothing, since E is already
   *         // absent from target.  ### kff todo: but it would be
   *         // nice to handle the rename case better.  How?
   *       }
   *   }
   *
   * // This next loop is over those entries in source that were
   * // not already covered in the loop over ancestor above.
   * for (each remaining entry E in source)
   *   {
   *     if (E does not exist in target)
   *       add it to target, based on source;
   *     else if (E exists in target but different id than E in source)
   *       conflict;
   *   }
   *         
   *     // All entries in ancestor and source are accounted for.
   *     // Remaining entries in target should be left as-is.
   *   }
   *
   *
   * A WORD ABOUT MERGE RECURSION AND ANCESTRY TRACKING
   *
   * After we do the merge into target, target has absorbed the
   * history between ancestor and source, but there is no record of
   * this absorbtion having happened.  For example, when generating a
   * log message for target, you'd want to include all the changes
   * between ancestor and source.
   *
   * In the general case, this is the same genetic merge problem that
   * we'll have to deal with when we do full ancestry tracking.
   * (Hello, changesets.)
   *
   * Bill Tutt explains that knowing when to update the predecessor-id
   * is when S and T are related, but as cousins in the ancestry tree.
   * That is:
   *
   *    ((S.NodeId == T.NodeId)
   *     && (! S == T)
   *     && (! S ancestorof T)
   *     && (! T ancestorof S))
   *
   * See the following message for the full details:
   * http://subversion.tigris.org/servlets/ReadMsg?list=dev&msgId=166183 */

  if ((! svn_fs__dag_is_directory (source))
      || (! svn_fs__dag_is_directory (target))
      || (! svn_fs__dag_is_directory (ancestor)))
    {
      return conflict_err (conflict_p, target_path);
    }

      
  /* Possible early merge failure: if target and ancestor have
     different property lists, then the merge should fail.
     Propchanges can *only* be committed on an up-to-date directory.

     ### TODO: Please see issue #418 about the inelegance of this. */
  {
    svn_fs__node_revision_t *tgt_nr, *anc_nr;

    /* Get node revisions for our id's. */
    SVN_ERR (svn_fs__bdb_get_node_revision (&tgt_nr, fs, target_id, trail));
    SVN_ERR (svn_fs__bdb_get_node_revision (&anc_nr, fs, ancestor_id, trail));
        
    /* Now compare the prop-keys of the skels.  Note that just because
       the keys are different -doesn't- mean the proplists have
       different contents.  But merge() isn't concerned with contents;
       it doesn't do a brute-force comparison on textual contents, so
       it won't do that here either.  Checking to see if the propkey
       atoms are `equal' is enough. */
    if (! svn_fs__same_keys (tgt_nr->prop_key, anc_nr->prop_key))
      {
        return conflict_err (conflict_p, target_path);
      }
  }

  /* ### todo: it would be more efficient to simply check for a NULL
     entries hash where necessary below than to allocate an empty hash
     here, but another day, another day... */
  SVN_ERR (svn_fs__dag_dir_entries (&s_entries, source, trail));
  if (! s_entries)
    s_entries = apr_hash_make (trail->pool);
  SVN_ERR (svn_fs__dag_dir_entries (&t_entries, target, trail));
  if (! t_entries)
    t_entries = apr_hash_make (trail->pool);
  SVN_ERR (svn_fs__dag_dir_entries (&a_entries, ancestor, trail));
  if (! a_entries)
    a_entries = apr_hash_make (trail->pool);

  /* for each entry E in a_entries... */
  for (hi = apr_hash_first (trail->pool, a_entries); 
       hi; 
       hi = apr_hash_next (hi))
    {
      svn_fs_dirent_t *s_entry, *t_entry, *a_entry;

      const void *key;
      void *val;
      apr_ssize_t klen;
          
      /* KEY will be the entry name in ancestor, VAL the dirent */
      apr_hash_this (hi, &key, &klen, &val);
      a_entry = val;
          
      /* E exists in target and source (as well as ancestor) */
      if ((s_entry = apr_hash_get (s_entries, key, klen))
          && (t_entry = apr_hash_get (t_entries, key, klen)))
        {
          /* If source entry has changed since ancestor entry... */
          if (! svn_fs__id_eq (a_entry->id, s_entry->id))
            {
              int a_ancestorof_t = 0, t_ancestorof_s = 0;
              int s_ancestorof_t = 0;
              int a_is_t = 0;
              int logic_case = 0;

              /*** The id_check_ancestor calls are rather expensive,
                   so reproduce the logic below up here so we only ask
                   the questions that need to be asked.  This would be
                   a heckuva lot easier if id_check_ancestor could
                   return an int instead of an svn_error_t *, but
                   that's just life, I suppose.  

                   This could very well be the ugliest code in
                   Subversion. */

              a_is_t = svn_fs__id_eq (a_entry->id, t_entry->id);
              if (a_is_t)
                {
                  /* This is Case 1.  */
                  logic_case = 1;
                }
              else
                {
                  SVN_ERR (id_check_ancestor (&a_ancestorof_t, fs, a_entry->id,
                                              t_entry->id, trail));
                  if (a_ancestorof_t)
                    {
                      /* this is an &&, so we need both ancestor checks. */
                      SVN_ERR (id_check_ancestor (&t_ancestorof_s, fs, 
                                                  t_entry->id, s_entry->id, 
                                                  trail));
                      if (t_ancestorof_s)
                        {
                          /* This is Case 1.  */
                          logic_case = 1;
                        }
                    }
                }

              /* if we didn't choose Case 1, try for Case 2. */
              if (! logic_case)
                {
                  SVN_ERR (id_check_ancestor (&s_ancestorof_t, fs, 
                                              s_entry->id, t_entry->id, 
                                              trail));
                  if (! s_ancestorof_t)
                    {
                      /* This is Case 2. */
                      logic_case = 2;
                    }
                }

              /*** Now, actually use our findings to do real work. ***/

              /* ... and if target entry has not changed, - OR - if
                 target descends from ancestor, and source descends
                 from target... (Case 1) */
              if (logic_case == 1)
                {
                  /* ### kff todo: what about svn_fs__dag_link()
                     instead of svn_fs__dag_set_entry()?  The cycle
                     protection guaranteed by the former would be
                     guaranteed "for free" anyway, if this function
                     demanded that SOURCE and ANCESTOR always be
                     immutable nodes.  But we don't demand that,
                     although it happens to be true of our only caller
                     right now, since merges are only done as part of
                     commits. */

                  /* ... target takes source. */
                  if (! svn_fs__dag_check_mutable (target, txn_id))
                    return svn_error_createf
                      (SVN_ERR_FS_NOT_MUTABLE, NULL,
                       "unexpected immutable node at \"%s\"", target_path);

                  SVN_ERR (svn_fs__dag_set_entry
                           (target, t_entry->name, s_entry->id, 
                            txn_id, trail));
                }
              /* or if target entry is different from both and
                 unrelated to source, and all three entries are
                 dirs... (Case 2) */
              else if (logic_case == 2)
                {
                  dag_node_t *s_ent_node, *t_ent_node, *a_ent_node;
                  const char *new_tpath;
                  int pred_count;
                      
                  SVN_ERR (svn_fs__dag_get_node (&s_ent_node, fs,
                                                 s_entry->id, trail));
                  SVN_ERR (svn_fs__dag_get_node (&t_ent_node, fs,
                                                 t_entry->id, trail));
                  SVN_ERR (svn_fs__dag_get_node (&a_ent_node, fs,
                                                 a_entry->id, trail));
                      
                  if ((! svn_fs__dag_is_directory (s_ent_node))
                      || (! svn_fs__dag_is_directory (t_ent_node))
                      || (! svn_fs__dag_is_directory (a_ent_node)))
                    {
                      /* Not all of these entries is a directory. Conflict. */
                      return conflict_err (conflict_p,
                                           svn_path_join (target_path,
                                                          a_entry->name,
                                                          trail->pool));
                    }

                  /* ... just recurse. */
                  new_tpath = svn_path_join (target_path, t_entry->name,
                                             trail->pool);
                  SVN_ERR (merge (conflict_p, new_tpath,
                                  t_ent_node, s_ent_node, a_ent_node,
                                  txn_id, trail));

                  SVN_ERR (svn_fs__dag_get_predecessor_count (&pred_count,
                                                              s_ent_node,
                                                              trail));

                  /* If target is an immediate descendant of ancestor,
                     and source is also a descendant of ancestor, we
                     need to point target's predecessor-id to
                     source. */
                  SVN_ERR (update_ancestry (fs, s_entry->id,
                                            t_entry->id, txn_id, 
                                            new_tpath, pred_count, trail));
                }
              /* Else target entry has changed since ancestor entry,
                 but it changed either to source entry or to a
                 successor of source entry, so we need do nothing. */
            }
        }
      /* E exists in source but not target */
      else if ((s_entry = apr_hash_get (s_entries, key, klen))
               && (! apr_hash_get (t_entries, key, klen)))
        {
          /* If E changed between ancestor and source, then that
             conflicts with E's having been removed from target. */
          if (! svn_fs__id_eq (a_entry->id, s_entry->id))
            {
              return conflict_err (conflict_p,
                                   svn_path_join (target_path,
                                                  a_entry->name,
                                                  trail->pool));
            }

          /* Else if E did not change between ancestor and source,
             then E's removal from target holds, so do nothing. */
        }
      /* E exists in target but not source */
      else if ((t_entry = apr_hash_get (t_entries, key, klen))
               && (! apr_hash_get (s_entries, key, klen)))
        {
          int distance = svn_fs_compare_ids (t_entry->id, a_entry->id);
          
          if (distance == 0)
            {
              /* If E is same in target as ancestor, then it has not
                 changed, and the deletion in source should be
                 honored. */
              if (! svn_fs__dag_check_mutable (target, txn_id))
                return svn_error_createf
                  (SVN_ERR_FS_NOT_MUTABLE, NULL,
                   "unexpected immutable node at \"%s\"", target_path);
              
              SVN_ERR (svn_fs__dag_delete_tree (target, t_entry->name, 
                                                txn_id, trail));

              /* Seems cleanest to remove it from the target entries
                 hash now, even though no code would break if we
                 didn't.  It feels more robust if t_entries reflects
                 the state of the target at all times. */
              apr_hash_set (t_entries, key, klen, NULL);
            }
          else if (distance != -1)
            {
              /* E is an attempt to modify ancestor, so it's a
                 conflict with the deletion of E in source.  If E
                 were unrelated to ancestor, it would not be an
                 attempt to modify ancestor (it might just be a copy
                 or rename of something unrelated), in which case we
                 wouldn't conflict.  It's because E is *related* to
                 the ancestor that we conflict here.

                 ### TODO: see issue #418 about this inelegance. */
              return conflict_err (conflict_p,
                                   svn_path_join (target_path,
                                                  t_entry->name,
                                                  trail->pool));
            }
          else
            {
              /* It's a double delete (plus an add), so do nothing
                 except un-record the deletion of E so that this
                 transaction isn't given credit for that portion of
                 this change. */
              SVN_ERR (undelete_change (fs, svn_path_join (target_path, 
                                                           t_entry->name, 
                                                           trail->pool),
                                        txn_id, trail));
            }
        }
      /* E exists in neither target nor source */
      else
        {
          /* It's a double delete, so do nothing except un-record the
             deletion of E so that this transaction isn't given credit
             for that change. */
          SVN_ERR (undelete_change (fs, svn_path_join (target_path, 
                                                       a_entry->name, 
                                                       trail->pool),
                                    txn_id, trail));

          /* ### kff todo: what about the rename case? */
        }
          
      /* We've taken care of any possible implications E could have.
         Remove it from source_entries, so it's easy later to loop
         over all the source entries that didn't exist in
         ancestor_entries. */
      apr_hash_set (s_entries, key, klen, NULL);
    }
      
  /* For each entry E in source but not in ancestor */
  for (hi = apr_hash_first (trail->pool, s_entries); 
       hi; 
       hi = apr_hash_next (hi))
    {
      svn_fs_dirent_t *s_entry, *t_entry;
      const void *key;
      void *val;
      apr_ssize_t klen;
      int s_ancestorof_t = 0;

      apr_hash_this (hi, &key, &klen, &val);
      s_entry = val;
      t_entry = apr_hash_get (t_entries, key, klen);

      /* The id_check_ancestor calls are rather expensive, so
         reproduce the logic below up here so we only ask the
         questions that need to be asked.  This would be a
         heckuva lot easier if id_check_ancestor could return
         an int instead of an svn_error_t *, but that's just
         life, I suppose.  */
      if (t_entry)
        {
          SVN_ERR (id_check_ancestor (&s_ancestorof_t, fs, s_entry->id,
                                      t_entry->id, trail));
        }

      /* E does not exist in target */
      if (! t_entry)
        {
          /* target takes source */
          if (! svn_fs__dag_check_mutable (target, txn_id))
            return svn_error_createf
              (SVN_ERR_FS_NOT_MUTABLE, NULL,
               "unexpected immutable node at \"%s\"", target_path);
              
          SVN_ERR (svn_fs__dag_set_entry
                   (target, s_entry->name, s_entry->id, txn_id, trail));
        }
      /* E exists in target but is different from E in source */
      else if (! s_ancestorof_t)
        {
          return conflict_err (conflict_p,
                               svn_path_join (target_path,
                                              t_entry->name,
                                              trail->pool));

          /* The remaining case would be: E exists in target and is
           * same as in source.  This implies a twin add, so target
           * just stays as is.  
           */
        }
    }
      
  /* All entries in ancestor and source have been accounted for.
   *
   * Any entry E in target that does not exist in ancestor or source
   * is a non-conflicting add, so we don't need to do anything about
   * it.  
   */

  return SVN_NO_ERROR;
}


struct merge_args
{
  /* The ancestor for the merge.  If this is null, then TXN's base is
     used as the ancestor for the merge. */
  dag_node_t *ancestor_node;

  /* This is the SOURCE node for the merge.  It may not be null. */
  dag_node_t *source_node;

  /* This is the TARGET of the merge.  It may not be null.  If
     ancestor_node above is null, then this txn's base is used as the
     ancestor for the merge. */
  svn_fs_txn_t *txn;

  /* If a conflict results, this is updated to the path in the txn that
     conflicted.  It must point to a valid svn_stringbuf_t before calling
     svn_fs__retry_txn, as this determines the pool used to allocate any
     required memory. */
  svn_stringbuf_t *conflict;
};


/* Merge changes between an ancestor and BATON->source_node into
   BATON->txn.  The ancestor is either BATON->ancestor_node, or if
   that is null, BATON->txn's base node.

   If the merge is successful, BATON->txn's base will become
   BATON->source_node, and its root node will have a new ID, a
   successor of BATON->source_node. */
static svn_error_t *
txn_body_merge (void *baton, trail_t *trail)
{
  struct merge_args *args = baton;
  dag_node_t *source_node, *txn_root_node, *ancestor_node;
  const svn_fs_id_t *source_id;
  svn_fs_t *fs = svn_fs__txn_fs (args->txn);
  const char *txn_id = svn_fs__txn_id (args->txn);

  source_node = args->source_node;
  ancestor_node = args->ancestor_node;
  source_id = svn_fs__dag_get_id (source_node);
  
  SVN_ERR (svn_fs__dag_txn_root (&txn_root_node, fs, txn_id, trail));

  if (ancestor_node == NULL)
    {
      SVN_ERR (svn_fs__dag_txn_base_root (&ancestor_node, fs,
                                          txn_id, trail));
    }
  
  if (svn_fs__id_eq (svn_fs__dag_get_id (ancestor_node),
                    svn_fs__dag_get_id (txn_root_node)))
    {
      /* If no changes have been made in TXN since its current base,
         then it can't conflict with any changes since that base.  So
         we just set *both* its base and root to source, making TXN
         in effect a repeat of source. */
      
      /* ### kff todo: this would, of course, be a mighty silly thing
         for the caller to do, and we might want to consider whether
         this response is really appropriate. */
      
      SVN_ERR (svn_fs__set_txn_base (fs, txn_id, source_id, trail));
      SVN_ERR (svn_fs__set_txn_root (fs, txn_id, source_id, trail));
    }
  else
    {
      int pred_count;

      SVN_ERR (merge (args->conflict, "/", txn_root_node,
                      source_node, ancestor_node, txn_id, trail));

      SVN_ERR (svn_fs__dag_get_predecessor_count (&pred_count, source_node,
                                                  trail));

      /* After the merge, txn's new "ancestor" is now really the node
         at source_id, so record that fact.  Think of this as
         ratcheting the txn forward in time, so it can't backslide and
         forget the merging work that's already been done. */
      SVN_ERR (update_ancestry (fs, source_id, 
                                svn_fs__dag_get_id (txn_root_node),
                                txn_id, "/", pred_count, trail));
      SVN_ERR (svn_fs__set_txn_base (fs, txn_id, source_id, trail));
    }
  
  return SVN_NO_ERROR;
}


struct commit_args
{
  svn_fs_txn_t *txn;
  svn_revnum_t new_rev;
};


/* Commit ARGS->txn, setting ARGS->new_rev to the resulting new
 * revision, if ARGS->txn is up-to-date w.r.t. the repository.
 *
 * Up-to-date means that ARGS->txn's base root is the same as the root
 * of the youngest revision.  If ARGS->txn is not up-to-date, the
 * error SVN_ERR_FS_TXN_OUT_OF_DATE is returned, and the commit fails: no
 * new revision is created, and ARGS->new_rev is not touched.
 *
 * If the commit succeeds, ARGS->txn is destroyed.
 */
static svn_error_t *
txn_body_commit (void *baton, trail_t *trail)
{
  struct commit_args *args = baton;

  svn_fs_txn_t *txn = args->txn;
  svn_fs_t *fs = svn_fs__txn_fs (txn);
  const char *txn_name = svn_fs__txn_id (txn);

  svn_revnum_t youngest_rev;
  const svn_fs_id_t *y_rev_root_id;
  dag_node_t *txn_base_root_node;

  /* Getting the youngest revision locks the revisions table until
     this trail is done. */
  SVN_ERR (svn_fs__bdb_youngest_rev (&youngest_rev, fs, trail));

  /* If the root of the youngest revision is the same as txn's base,
     then no further merging is necessary and we can commit. */
  SVN_ERR (svn_fs__rev_get_root (&y_rev_root_id, fs, youngest_rev, trail));
  SVN_ERR (svn_fs__dag_txn_base_root (&txn_base_root_node, fs, txn_name,
                                      trail));
  /* ### kff todo: it seems weird to grab the ID for one, and the node
     for the other.  We can certainly do the comparison we need, but
     it would be nice to grab the same type of information from the
     start, instead of having to transform one of them. */ 
  if (! svn_fs__id_eq (y_rev_root_id, svn_fs__dag_get_id (txn_base_root_node)))
    {
      svn_string_t *id_str = svn_fs_unparse_id (y_rev_root_id, trail->pool);
      return svn_error_createf
        (SVN_ERR_FS_TXN_OUT_OF_DATE, NULL,
         "txn `%s' out of date w.r.t. revision `%s'", txn_name, id_str->data);
    }
  
  /* Else, commit the txn. */
  SVN_ERR (svn_fs__dag_commit_txn (&(args->new_rev), fs, txn_name, trail));

  return SVN_NO_ERROR;
}


/* Note:  it is acceptable for this function to call back into
   public FS API interfaces because it does not itself use trails.  */
svn_error_t *
svn_fs_commit_txn (const char **conflict_p,
                   svn_revnum_t *new_rev, 
                   svn_fs_txn_t *txn)
{
  /* How do commits work in Subversion?
   *
   * When you're ready to commit, here's what you have:
   *
   *    1. A transaction, with a mutable tree hanging off it.
   *    2. A base revision, against which TXN_TREE was made.
   *    3. A latest revision, which may be newer than the base rev.
   *
   * The problem is that if latest != base, then one can't simply
   * attach the txn root as the root of the new revision, because that
   * would lose all the changes between base and latest.  It is also
   * not acceptable to insist that base == latest; in a busy
   * repository, commits happen too fast to insist that everyone keep
   * their entire tree up-to-date at all times.  Non-overlapping
   * changes should not interfere with each other.
   *
   * The solution is to merge the changes between base and latest into
   * the txn tree [see the function merge()].  The txn tree is the
   * only one of the three trees that is mutable, so it has to be the
   * one to adjust.
   *
   * You might have to adjust it more than once, if a new latest
   * revision gets committed while you were merging in the previous
   * one.  For example:
   *
   *    1. Jane starts txn T, based at revision 6.
   *    2. Someone commits (or already committed) revision 7.
   *    3. Jane's starts merging the changes between 6 and 7 into T.
   *    4. Meanwhile, someone commits revision 8.
   *    5. Jane finishes the 6-->7 merge.  T could now be committed
   *       against a latest revision of 7, if only that were still the
   *       latest.  Unfortunately, 8 is now the latest, so... 
   *    6. Jane starts merging the changes between 7 and 8 into T.
   *    7. Meanwhile, no one commits any new revisions.  Whew.
   *    8. Jane commits T, creating revision 9, whose tree is exactly
   *       T's tree, except immutable now.
   *
   * Lather, rinse, repeat.
   */

  svn_error_t *err;
  svn_fs_t *fs = svn_fs__txn_fs (txn);
  apr_pool_t *pool = svn_fs__txn_pool (txn);
  const char *txn_id;

  /* Initialize returned revision number to an invalid value. */
  *new_rev = SVN_INVALID_REVNUM;

  /* Get the transaction's name.  We'll need it later. */
  SVN_ERR (svn_fs_txn_name (&txn_id, txn, pool));

  while (1729)
    {
      struct get_root_args get_root_args;
      struct merge_args merge_args;
      struct commit_args commit_args;
      svn_revnum_t youngish_rev;
      svn_fs_root_t *youngish_root;
      dag_node_t *youngish_root_node;

      /* Get the *current* youngest revision, in one short-lived
         Berkeley transaction.  (We don't want the revisions table
         locked while we do the main merge.)  We call it "youngish"
         because new revisions might get committed after we've
         obtained it. */

      SVN_ERR (svn_fs_youngest_rev (&youngish_rev, fs, pool));
      SVN_ERR (svn_fs_revision_root (&youngish_root, fs, youngish_rev, pool));

      /* Get the dag node for the youngest revision, also in one
         Berkeley transaction.  Later we'll use it as the SOURCE
         argument to a merge, and if the merge succeeds, this youngest
         root node will become the new base root for the svn txn that
         was the target of the merge (but note that the youngest rev
         may have changed by then -- that's why we're careful to get
         this root in its own bdb txn here). */
      get_root_args.root = youngish_root;
      SVN_ERR (svn_fs__retry_txn (fs, txn_body_get_root,
                                  &get_root_args, pool));
      youngish_root_node = get_root_args.node;
      
      /* Try to merge.  If the merge succeeds, the base root node of
         TARGET's txn will become the same as youngish_root_node, so
         any future merges will only be between that node and whatever
         the root node of the youngest rev is by then. */ 
      merge_args.ancestor_node = NULL;
      merge_args.source_node = youngish_root_node;
      merge_args.txn = txn;
      merge_args.conflict = svn_stringbuf_create ("", pool);
      err = svn_fs__retry_txn (fs, txn_body_merge, &merge_args, pool);
      if (err)
        {
          if ((err->apr_err == SVN_ERR_FS_CONFLICT) && conflict_p)
            *conflict_p = merge_args.conflict->data;
          return err;
        }
      
      /* Try to commit. */
      commit_args.txn = txn;
      err = svn_fs__retry_txn (fs, txn_body_commit, &commit_args, pool);
      if (err && (err->apr_err == SVN_ERR_FS_TXN_OUT_OF_DATE))
        {
          /* Did someone else finish committing a new revision while we
             were in mid-merge or mid-commit?  If so, we'll need to
             loop again to merge the new changes in, then try to
             commit again.  Or if that's not what happened, then just
             return the error. */
          svn_revnum_t youngest_rev;
          SVN_ERR (svn_fs_youngest_rev (&youngest_rev, fs, pool));
          if (youngest_rev == youngish_rev)
            return err;
          else
            svn_error_clear (err);
        }
      else if (err)
        return err;
      else
        {
          struct deltify_committed_args dc_args;

          /* Set the return value, the new revision. */
          *new_rev = commit_args.new_rev;

          /* Final step: after a successful commit of the transaction,
             deltify the new revision. */
          dc_args.fs = fs;
          dc_args.rev = *new_rev;
          dc_args.txn_id = txn_id;

          SVN_ERR_W (svn_fs__retry_txn (fs, txn_body_deltify_committed, 
                                        &dc_args, pool),
                     "Commit succeeded, deltification failed");

          return SVN_NO_ERROR;
        }
    }

  return SVN_NO_ERROR;
}


/* Note:  it is acceptable for this function to call back into
   public FS API interfaces because it does not itself use trails.  */
svn_error_t *
svn_fs_merge (const char **conflict_p,
              svn_fs_root_t *source_root,
              const char *source_path,
              svn_fs_root_t *target_root,
              const char *target_path,
              svn_fs_root_t *ancestor_root,
              const char *ancestor_path,
              apr_pool_t *pool)
{
  dag_node_t *source, *ancestor;
  struct get_root_args get_root_args;
  struct merge_args merge_args;
  svn_fs_txn_t *txn;
  svn_error_t *err;
  svn_fs_t *fs;

  if (! svn_fs_is_txn_root (target_root))
    return not_txn (target_root);

  /* Paranoia. */
  fs = svn_fs_root_fs (ancestor_root);
  if ((svn_fs_root_fs (source_root) != fs)
      || (svn_fs_root_fs (target_root) != fs))
    {
      return svn_error_create
        (SVN_ERR_FS_CORRUPT, NULL,
         "Bad merge -- ancestor, source, and target not all in same fs");
    }

  /* ### kff todo: is there any compelling reason to get the nodes in
     one db transaction?  Right now we don't; txn_body_get_root() gets
     one node at a time.  This will probably need to change:

     Jim Blandy <jimb@zwingli.cygnus.com> writes:
     > svn_fs_merge needs to be a single transaction, to protect it against
     > people deleting parents of nodes it's working on, etc.
  */

  /* Get the ancestor node. */
  get_root_args.root = ancestor_root;
  SVN_ERR (svn_fs__retry_txn (fs, txn_body_get_root, &get_root_args, pool));
  ancestor = get_root_args.node;

  /* Get the source node. */
  get_root_args.root = source_root;
  SVN_ERR (svn_fs__retry_txn (fs, txn_body_get_root, &get_root_args, pool));
  source = get_root_args.node;
  
  /* Open a txn for the txn root into which we're merging. */
  SVN_ERR (svn_fs_open_txn (&txn, fs,
                            svn_fs_txn_root_name (target_root, pool),
                            pool));

  /* Merge changes between ANCESTOR and SOURCE into TXN. */
  merge_args.source_node = source;
  merge_args.ancestor_node = ancestor;
  merge_args.txn = txn;
  merge_args.conflict = svn_stringbuf_create ("", pool);
  err = svn_fs__retry_txn (fs, txn_body_merge, &merge_args, pool);
  if (err)
    {
      if ((err->apr_err == SVN_ERR_FS_CONFLICT) && conflict_p)
        *conflict_p = merge_args.conflict->data;
      return err;
    }

  return SVN_NO_ERROR;
}



/* Directories.  */


struct dir_entries_args
{
  apr_hash_t **table_p;
  svn_fs_root_t *root;
  const char *path;
};


static svn_error_t *
txn_body_dir_entries (void *baton,
                      trail_t *trail)
{
  struct dir_entries_args *args = baton;
  parent_path_t *parent_path;
  apr_hash_t *entries;

  SVN_ERR (open_path (&parent_path, args->root, args->path, 0, trail));

  /* Get the entries for PARENT_PATH. */
  SVN_ERR (svn_fs__dag_dir_entries (&entries, parent_path->node, trail));

  /* Potentially initialize the return value to an empty hash. */
  *args->table_p = entries ? entries : apr_hash_make (trail->pool);
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_dir_entries (apr_hash_t **table_p,
                    svn_fs_root_t *root,
                    const char *path,
                    apr_pool_t *pool)
{
  struct dir_entries_args args;
  apr_hash_t *table;

  args.table_p = &table;
  args.root    = root;
  args.path    = path;
  SVN_ERR (svn_fs__retry_txn (root->fs, txn_body_dir_entries, &args, pool));

  if (! table)
    table = apr_hash_make (pool);
  *table_p = table;
  return SVN_NO_ERROR;
}



struct make_dir_args
{
  svn_fs_root_t *root;
  const char *path;
};


static svn_error_t *
txn_body_make_dir (void *baton,
                   trail_t *trail)
{
  struct make_dir_args *args = baton;
  svn_fs_root_t *root = args->root;
  const char *path = args->path;
  parent_path_t *parent_path;
  dag_node_t *sub_dir;
  const char *txn_id = svn_fs_txn_root_name (root, trail->pool);

  SVN_ERR (open_path (&parent_path, root, path, open_path_last_optional,
                      trail));

  /* If there's already a sub-directory by that name, complain.  This
     also catches the case of trying to make a subdirectory named `/'.  */
  if (parent_path->node)
    return already_exists (root, path);

  /* Create the subdirectory.  */
  SVN_ERR (make_path_mutable (root, parent_path->parent, path, trail));
  SVN_ERR (svn_fs__dag_make_dir (&sub_dir,
                                 parent_path->parent->node, 
                                 parent_path->entry,
                                 txn_id,
                                 trail));

  /* Make a record of this modification in the changes table. */
  SVN_ERR (add_change (svn_fs_root_fs (root), txn_id, 
                       path, svn_fs__dag_get_id (sub_dir),
                       svn_fs_path_change_add, 0, 0, trail));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_make_dir (svn_fs_root_t *root,
                 const char *path,
                 apr_pool_t *pool)
{
  struct make_dir_args args;

  if (! svn_fs_is_txn_root (root))
    return not_txn (root);

  args.root = root;
  args.path = path;
  return svn_fs__retry_txn (root->fs, txn_body_make_dir, &args, pool);
}
                              

struct delete_args
{
  svn_fs_root_t *root;
  const char *path;
  svn_boolean_t delete_tree;
};


/* If this returns SVN_ERR_FS_NO_SUCH_ENTRY, it means that the
   basename of PATH is missing from its parent, that is, the final
   target of the deletion is missing.  */
static svn_error_t *
txn_body_delete (void *baton,
                 trail_t *trail)
{
  struct delete_args *args = baton;
  svn_fs_root_t *root = args->root;
  const char *path = args->path;
  parent_path_t *parent_path;
  const char *txn_id = svn_fs_txn_root_name (root, trail->pool);

  SVN_ERR (open_path (&parent_path, root, path, 0, trail));

  if (! svn_fs_is_txn_root (root))
    return not_txn (root);

  /* We can't remove the root of the filesystem.  */
  if (! parent_path->parent)
    return svn_error_create (SVN_ERR_FS_ROOT_DIR, NULL,
                             "the root directory cannot be deleted");

  /* Make the parent directory mutable.  */
  SVN_ERR (make_path_mutable (root, parent_path->parent, path, trail));

  if (args->delete_tree)
    {
      SVN_ERR (svn_fs__dag_delete_tree (parent_path->parent->node,
                                        parent_path->entry,
                                        txn_id, trail));
    }
  else
    {
      SVN_ERR (svn_fs__dag_delete (parent_path->parent->node,
                                   parent_path->entry,
                                   txn_id, trail));
    }
  
  /* Make a record of this modification in the changes table. */
  SVN_ERR (add_change (svn_fs_root_fs (root), txn_id, 
                       path, svn_fs__dag_get_id (parent_path->node),
                       svn_fs_path_change_delete, 0, 0, trail));
  
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_delete (svn_fs_root_t *root,
               const char *path,
               apr_pool_t *pool)
{
  struct delete_args args;

  args.root        = root;
  args.path        = path;
  args.delete_tree = FALSE;
  return svn_fs__retry_txn (root->fs, txn_body_delete, &args, pool);
}


svn_error_t *
svn_fs_delete_tree (svn_fs_root_t *root,
                    const char *path,
                    apr_pool_t *pool)
{
  struct delete_args args;

  args.root        = root;
  args.path        = path;
  args.delete_tree = TRUE;
  return svn_fs__retry_txn (root->fs, txn_body_delete, &args, pool);
}


svn_error_t *
svn_fs_rename (svn_fs_root_t *root,
               const char *from,
               const char *to,
               apr_pool_t *pool)
{
  abort ();
}


struct copy_args
{
  svn_fs_root_t *from_root;
  const char *from_path;
  svn_fs_root_t *to_root;
  const char *to_path;
  svn_boolean_t preserve_history;
};


static svn_error_t *
txn_body_copy (void *baton,
               trail_t *trail)
{
  struct copy_args *args = baton;
  svn_fs_root_t *from_root = args->from_root;
  const char *from_path = args->from_path;
  svn_fs_root_t *to_root = args->to_root;
  const char *to_path = args->to_path;
  parent_path_t *from_parent_path;
  parent_path_t *to_parent_path;

  if (! svn_fs_is_revision_root (from_root))
    return svn_error_create (SVN_ERR_FS_GENERAL, 
                             NULL,
                             "copy from mutable tree not currently supported");

  /* Build up the parent path from FROM_PATH, making sure that it
     exists in FROM_ROOT */
  SVN_ERR (open_path (&from_parent_path, from_root, from_path, 
                      0, trail));

  /* Build up the parent path from TO_PATH in TO_ROOT.  If the last
     component does not exist, it's not that big a deal.  We'll just
     make one there. */
  SVN_ERR (open_path (&to_parent_path, to_root, to_path, 
                      open_path_last_optional, trail));

  if (svn_fs_is_revision_root (from_root))
    {
      svn_fs_path_change_kind_t kind;
      const char *txn_id = svn_fs_txn_root_name (to_root, trail->pool);
      dag_node_t *new_node;

      /* If TO_PATH already existed prior to the copy, note that this
         operation is a replacement, not an addition. */
      if (to_parent_path->node)
        kind = svn_fs_path_change_replace;
      else
        kind = svn_fs_path_change_add;

      /* Make sure the target node's parents are mutable.  */
      SVN_ERR (make_path_mutable (to_root, to_parent_path->parent, 
                                  to_path, trail));

      SVN_ERR (svn_fs__dag_copy (to_parent_path->parent->node,
                                 to_parent_path->entry,
                                 from_parent_path->node,
                                 args->preserve_history,
                                 svn_fs_revision_root_revision (from_root),
                                 from_path, txn_id, trail));

      /* Make a record of this modification in the changes table. */
      SVN_ERR (get_dag (&new_node, to_root, to_path, trail));
      SVN_ERR (add_change (svn_fs_root_fs (to_root), txn_id, 
                           to_path, svn_fs__dag_get_id (new_node),
                           kind, 0, 0, trail));
    }
  else
    {
      /* See IZ Issue #436 */
      /* Copying from transaction roots not currently available.

         ### cmpilato todo someday: make this not so. :-) Note that
         when copying from mutable trees, you have to make sure that
         you aren't creating a cyclic graph filesystem, and a simple
         referencing operation won't cut it.  Currently, we should not
         be able to reach this clause, and the interface reports that
         this only works from immutable trees anyway, but JimB has
         stated that this requirement need not be necessary in the
         future. */

      abort ();
    }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_copy (svn_fs_root_t *from_root,
             const char *from_path,
             svn_fs_root_t *to_root,
             const char *to_path,
             apr_pool_t *pool)
{
  struct copy_args args;

  if (! svn_fs_is_txn_root (to_root))
    return not_txn (to_root);

  args.from_root         = from_root;
  args.from_path         = from_path;
  args.to_root           = to_root;
  args.to_path           = to_path;
  args.preserve_history  = 1;

  return svn_fs__retry_txn (to_root->fs, txn_body_copy, &args, pool);
}


svn_error_t *
svn_fs_revision_link (svn_fs_root_t *from_root,
                      svn_fs_root_t *to_root,
                      const char *path,
                      apr_pool_t *pool)
{
  struct copy_args args;

  if (! svn_fs_is_txn_root (to_root))
    return not_txn (to_root);

  args.from_root         = from_root;
  args.from_path         = path;
  args.to_root           = to_root;
  args.to_path           = path;
  args.preserve_history  = 0;

  return svn_fs__retry_txn (to_root->fs, txn_body_copy, &args, pool);
}


struct copied_from_args
{
  svn_fs_root_t *root;      /* Root for the node whose ancestry we seek. */
  const char *path;         /* Path for the node whose ancestry we seek. */

  svn_revnum_t result_rev;  /* Revision, if any, of the ancestor. */
  const char *result_path;  /* Path, if any, of the ancestor. */

  apr_pool_t *pool;         /* Allocate `result_path' here. */
};


static svn_error_t *
txn_body_copied_from (void *baton, trail_t *trail)
{
  struct copied_from_args *args = baton;
  parent_path_t *path_down;

  SVN_ERR (open_path (&path_down, args->root, args->path, 0, trail));
  SVN_ERR (svn_fs__dag_copied_from (&(args->result_rev),
                                    &(args->result_path),
                                    path_down->node,
                                    trail));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_copied_from (svn_revnum_t *rev_p,
                    const char **path_p,
                    svn_fs_root_t *root,
                    const char *path,
                    apr_pool_t *pool)
{
  struct copied_from_args args;

  args.root = root;
  args.path = path;
  args.pool = pool;

  SVN_ERR (svn_fs__retry_txn (root->fs, txn_body_copied_from, &args, pool));

  *rev_p  = args.result_rev;
  *path_p = args.result_path;

  return SVN_NO_ERROR;
}



/* Files.  */


struct make_file_args
{
  svn_fs_root_t *root;
  const char *path;
};


static svn_error_t *
txn_body_make_file (void *baton,
                    trail_t *trail)
{
  struct make_file_args *args = baton;
  svn_fs_root_t *root = args->root;
  const char *path = args->path;
  parent_path_t *parent_path;
  dag_node_t *child;
  const char *txn_id = svn_fs_txn_root_name (root, trail->pool);

  SVN_ERR (open_path (&parent_path, root, path, open_path_last_optional,
                      trail));

  /* If there's already a file by that name, complain.
     This also catches the case of trying to make a file named `/'.  */
  if (parent_path->node)
    return already_exists (root, path);

  /* Create the file.  */
  SVN_ERR (make_path_mutable (root, parent_path->parent, path, trail));
  SVN_ERR (svn_fs__dag_make_file (&child,
                                  parent_path->parent->node, 
                                  parent_path->entry,
                                  txn_id,
                                  trail));

  /* Make a record of this modification in the changes table. */
  SVN_ERR (add_change (svn_fs_root_fs (root), txn_id, 
                       path, svn_fs__dag_get_id (child),
                       svn_fs_path_change_add, 0, 0, trail));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_make_file (svn_fs_root_t *root,
                  const char *path,
                  apr_pool_t *pool)
{
  struct make_file_args args;

  args.root = root;
  args.path = path;
  return svn_fs__retry_txn (root->fs, txn_body_make_file, &args, pool);
}



struct file_length_args
{
  svn_fs_root_t *root;
  const char *path;
  apr_size_t length;     /* OUT parameter */
};

static svn_error_t *
txn_body_file_length (void *baton,
                      trail_t *trail)
{
  struct file_length_args *args = baton;
  dag_node_t *file;
  
  /* First create a dag_node_t from the root/path pair. */
  SVN_ERR (get_dag (&file, args->root, args->path, trail));

  /* Now fetch its length */
  return svn_fs__dag_file_length (&args->length, file, trail);
}

svn_error_t *
svn_fs_file_length (apr_off_t *length_p,
                    svn_fs_root_t *root,
                    const char *path,
                    apr_pool_t *pool)
{
  struct file_length_args args;

  args.root = root;
  args.path = path;
  SVN_ERR (svn_fs__retry_txn (root->fs, txn_body_file_length, &args, pool));

  *length_p = args.length;
  return SVN_NO_ERROR;
}


struct file_checksum_args
{
  svn_fs_root_t *root;
  const char *path;
  unsigned char *digest;  /* OUT parameter, MD5_DIGESTSIZE bytes long */
};

static svn_error_t *
txn_body_file_checksum (void *baton,
                        trail_t *trail)
{
  struct file_checksum_args *args = baton;
  dag_node_t *file;
  
  SVN_ERR (get_dag (&file, args->root, args->path, trail));
  return svn_fs__dag_file_checksum (args->digest, file, trail);
}

svn_error_t *
svn_fs_file_md5_checksum (unsigned char digest[],
                          svn_fs_root_t *root,
                          const char *path,
                          apr_pool_t *pool)
{
  struct file_checksum_args args;

  args.root = root;
  args.path = path;
  args.digest = digest;
  SVN_ERR (svn_fs__retry_txn (root->fs, txn_body_file_checksum, &args, pool));

  return SVN_NO_ERROR;
}


/* --- Machinery for svn_fs_file_contents() ---  */


/* Local baton type for txn_body_get_file_contents. */
typedef struct file_contents_baton_t
{
  /* The file we want to read. */
  svn_fs_root_t *root;
  const char *path;

  /* The dag_node that will be made from the above. */
  dag_node_t *node;
    
  /* The pool in which `file_stream' (below) is allocated. */
  apr_pool_t *pool;

  /* The readable file stream that will be made from the
     dag_node. (And returned to the caller.) */
  svn_stream_t *file_stream;

} file_contents_baton_t;


/* Main body of svn_fs_file_contents;  converts a root/path pair into
   a readable file stream (in the context of a db txn). */
static svn_error_t *
txn_body_get_file_contents (void *baton, trail_t *trail)
{
  file_contents_baton_t *fb = (file_contents_baton_t *) baton;

  /* First create a dag_node_t from the root/path pair. */
  SVN_ERR (get_dag (&(fb->node), fb->root, fb->path, trail));
  
  /* Then create a readable stream from the dag_node_t. */
  SVN_ERR (svn_fs__dag_get_contents (&(fb->file_stream),
                                     fb->node,
                                     fb->pool,
                                     trail));
  return SVN_NO_ERROR;
}     



svn_error_t *
svn_fs_file_contents (svn_stream_t **contents,
                      svn_fs_root_t *root,
                      const char *path,
                      apr_pool_t *pool)
{
  file_contents_baton_t *fb = apr_pcalloc (pool, sizeof(*fb));
  fb->root = root;
  fb->path = path;
  fb->pool = pool;

  /* Create the readable stream in the context of a db txn.  */
  SVN_ERR (svn_fs__retry_txn (svn_fs_root_fs (root),
                              txn_body_get_file_contents, fb, pool));
  
  *contents = fb->file_stream;
  return SVN_NO_ERROR;
}

/* --- End machinery for svn_fs_file_contents() ---  */



/* --- Machinery for svn_fs_apply_textdelta() ---  */


/* Local baton type for all the helper functions below. */
typedef struct txdelta_baton_t
{
  /* This is the custom-built window consumer given to us by the delta
     library;  it uniquely knows how to read data from our designated
     "source" stream, interpret the window, and write data to our
     designated "target" stream (in this case, our repos file.) */
  svn_txdelta_window_handler_t interpreter;
  void *interpreter_baton;

  /* The original file info */
  svn_fs_root_t *root;
  const char *path;
  
  /* Derived from the file info */
  dag_node_t *node;

  svn_stream_t *source_stream;
  svn_stream_t *target_stream;
  svn_stream_t *string_stream;
  svn_stringbuf_t *target_string;

  /* Pool used by db txns */
  apr_pool_t *pool;

} txdelta_baton_t;


/* A trail-ready wrapper around svn_fs__dag_finalize_edits. */
static svn_error_t *
txn_body_finalize_edits (void *baton, trail_t *trail)
{
  txdelta_baton_t *tb = (txdelta_baton_t *) baton;
  return svn_fs__dag_finalize_edits (tb->node, 
                                     svn_fs_txn_root_name (tb->root,
                                                           trail->pool),
                                     trail);
}


/* ### see comment in window_consumer() regarding this function. */

/* Helper function of generic type `svn_write_fn_t'.  Implements a
   writable stream which appends to an svn_stringbuf_t. */
static svn_error_t *
write_to_string (void *baton, const char *data, apr_size_t *len)
{
  txdelta_baton_t *tb = (txdelta_baton_t *) baton;


  svn_stringbuf_appendbytes (tb->target_string, data, *len);

  return SVN_NO_ERROR;
}



/* The main window handler returned by svn_fs_apply_textdelta. */
static svn_error_t *
window_consumer (svn_txdelta_window_t *window, void *baton)
{
  txdelta_baton_t *tb = (txdelta_baton_t *) baton;

  /* Send the window right through to the custom window interpreter.
     In theory, the interpreter will then write more data to
     cb->target_string. */
  SVN_ERR (tb->interpreter (window, tb->interpreter_baton));

  /* ### the write_to_string() callback for the txdelta's output stream
     ### should be doing all the flush determination logic, not here.
     ### in a drastic case, a window could generate a LOT more than the
     ### maximum buffer size. we want to flush to the underlying target
     ### stream much sooner (e.g. also in a streamy fashion). also, by
     ### moving this logic inside the stream, the stream becomes nice
     ### and encapsulated: it holds all the logic about buffering and
     ### flushing.
     ###
     ### further: I believe the buffering should be removed from tree.c
     ### the buffering should go into the target_stream itself, which
     ### is defined by reps-string.c. Specifically, I think the
     ### rep_write_contents() function will handle the buffering and
     ### the spill to the underlying DB. by locating it there, then
     ### anybody who gets a writable stream for FS content can take
     ### advantage of the buffering capability. this will be important
     ### when we export an FS API function for writing a fulltext into
     ### the FS, rather than forcing that fulltext thru apply_textdelta.
  */

  /* Check to see if we need to purge the portion of the contents that
     have been written thus far. */
  if ((! window) || (tb->target_string->len > SVN_FS_WRITE_BUFFER_SIZE))
    {
      apr_size_t len = tb->target_string->len;
      svn_stream_write (tb->target_stream,
                        tb->target_string->data,
                        &len);
      svn_stringbuf_set (tb->target_string, "");
    }

  /* Is the window NULL?  If so, we're done, and we need to tell the
     dag subsystem that we're finished with our edits. */
  if (! window)
    {
      SVN_ERR (svn_fs__retry_txn (svn_fs_root_fs (tb->root),
                                  txn_body_finalize_edits, tb, tb->pool));
      SVN_ERR (svn_stream_close (tb->target_stream));
    }

  return SVN_NO_ERROR;
}


static svn_error_t *
txn_body_apply_textdelta (void *baton, trail_t *trail)
{
  txdelta_baton_t *tb = (txdelta_baton_t *) baton;
  parent_path_t *parent_path;
  const char *txn_id = svn_fs_txn_root_name (tb->root, trail->pool);

  /* Call open_path with no flags, as we want this to return an error
     if the node for which we are searching doesn't exist. */
  SVN_ERR (open_path (&parent_path, tb->root, tb->path, 0, trail));

  /* Now, make sure this path is mutable. */
  SVN_ERR (make_path_mutable (tb->root, parent_path, tb->path, trail));
  tb->node = parent_path->node;

  /* Make a readable "source" stream out of the current contents of
     ROOT/PATH; obviously, this must done in the context of a db_txn.
     The stream is returned in tb->source_stream. */
  SVN_ERR (svn_fs__dag_get_contents (&(tb->source_stream),
                                     tb->node, tb->pool, trail));

  /* Make a writable "target" stream */
  SVN_ERR (svn_fs__dag_get_edit_stream (&(tb->target_stream), tb->node, 
                                        tb->pool, txn_id, trail));

  /* Make a writable "string" stream which writes data to
     tb->target_string. */
  tb->target_string = svn_stringbuf_create ("", tb->pool);
  tb->string_stream = svn_stream_create (tb, tb->pool);
  svn_stream_set_write (tb->string_stream, write_to_string);

  /* Now, create a custom window handler that uses our two streams. */
  svn_txdelta_apply (tb->source_stream,
                     tb->string_stream,
                     tb->pool,
                     &(tb->interpreter),
                     &(tb->interpreter_baton));

  /* Make a record of this modification in the changes table. */
  SVN_ERR (add_change (svn_fs_root_fs (tb->root), txn_id, 
                       tb->path, svn_fs__dag_get_id (tb->node),
                       svn_fs_path_change_modify, 1, 0, trail));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_apply_textdelta (svn_txdelta_window_handler_t *contents_p,
                        void **contents_baton_p,
                        svn_fs_root_t *root,
                        const char *path,
                        apr_pool_t *pool)
{
  txdelta_baton_t *tb = apr_pcalloc (pool, sizeof(*tb));

  tb->root = root;
  tb->path = path;
  tb->pool = pool;

  /* See IZ Issue #438 */
  SVN_ERR (svn_fs__retry_txn (svn_fs_root_fs (root),
                              txn_body_apply_textdelta, tb, pool));
  
  *contents_p = window_consumer;
  *contents_baton_p = tb;
  return SVN_NO_ERROR;
}

/* --- End machinery for svn_fs_apply_textdelta() ---  */

/* --- Machinery for svn_fs_apply_text() ---  */

/* Baton for svn_fs_apply_text(). */
struct text_baton_t
{
  /* The original file info */
  svn_fs_root_t *root;
  const char *path;
  
  /* Derived from the file info */
  dag_node_t *node;
  
  /* The returned stream that will accept the file's new contents. */
  svn_stream_t *stream;

  /* The actual fs stream that the returned stream will write to. */
  svn_stream_t *file_stream;

  /* Pool used by db txns */
  apr_pool_t *pool;
};


/* A different trail-ready wrapper around svn_fs__dag_finalize_edits. */
static svn_error_t *
another_txn_body_finalize_edits (void *baton, trail_t *trail)
{
  struct text_baton_t *tb = baton;
  return svn_fs__dag_finalize_edits (tb->node, 
                                     svn_fs_txn_root_name (tb->root, 
                                                           trail->pool),
                                     trail);
}

/* Write function for the publically returned stream. */
static svn_error_t *
text_stream_writer (void *baton,
                    const char *data,
                    apr_size_t *len)
{
  struct text_baton_t *tb = baton;

  /* Psst, here's some data.  Pass it on to the -real- file stream. */
  return svn_stream_write (tb->file_stream, data, len);
}

/* Close function for the publically returned stream. */
static svn_error_t *
text_stream_closer (void *baton)
{
  struct text_baton_t *tb = baton;

  /* Need to tell fs that we're done sending text */
  SVN_ERR (svn_fs__retry_txn (svn_fs_root_fs (tb->root),
                              another_txn_body_finalize_edits, tb, tb->pool));

  /* Close the -real- file stream. */
  return svn_stream_close (tb->file_stream);
}


static svn_error_t *
txn_body_apply_text (void *baton, trail_t *trail)
{
  struct text_baton_t *tb = baton;
  parent_path_t *parent_path;
  const char *txn_id = svn_fs_txn_root_name (tb->root, trail->pool);

  /* Call open_path with no flags, as we want this to return an error
     if the node for which we are searching doesn't exist. */
  SVN_ERR (open_path (&parent_path, tb->root, tb->path, 0, trail));

  /* Now, make sure this path is mutable. */
  SVN_ERR (make_path_mutable (tb->root, parent_path, tb->path, trail));
  tb->node = parent_path->node;

  /* Make a writable stream for replacing the file's text. */
  SVN_ERR (svn_fs__dag_get_edit_stream (&(tb->file_stream), tb->node, 
                                        tb->pool, txn_id, trail));

  /* Create a 'returnable' stream which writes to the file_stream. */
  tb->stream = svn_stream_create (tb, tb->pool);
  svn_stream_set_write (tb->stream, text_stream_writer);
  svn_stream_set_close (tb->stream, text_stream_closer);

  /* Make a record of this modification in the changes table. */
  SVN_ERR (add_change (svn_fs_root_fs (tb->root), txn_id, 
                       tb->path, svn_fs__dag_get_id (tb->node),
                       svn_fs_path_change_modify, 1, 0, trail));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_apply_text (svn_stream_t **contents_p,
                   svn_fs_root_t *root,
                   const char *path,
                   apr_pool_t *pool)
{
  struct text_baton_t *tb = apr_pcalloc (pool, sizeof(*tb));

  tb->root = root;
  tb->path = path;
  tb->pool = pool;

  SVN_ERR (svn_fs__retry_txn (svn_fs_root_fs (root),
                              txn_body_apply_text, tb, pool));
  
  *contents_p = tb->stream;
  return SVN_NO_ERROR;
}

/* --- End machinery for svn_fs_apply_text() ---  */


/* Note: we're sharing the `things_changed_args' struct with
   svn_fs_props_changed(). */

static svn_error_t *
txn_body_contents_changed (void *baton, trail_t *trail)
{
  struct things_changed_args *args = baton;
  parent_path_t *parent_path_1, *parent_path_2;

  SVN_ERR (open_path (&parent_path_1, args->root1, args->path1, 0, trail));
  SVN_ERR (open_path (&parent_path_2, args->root2, args->path2, 0, trail));

  SVN_ERR (svn_fs__things_different (NULL,
                                     args->changed_p,
                                     parent_path_1->node,
                                     parent_path_2->node,
                                     trail));

  return SVN_NO_ERROR;
}


/* Note:  it is acceptable for this function to call back into
   public FS API interfaces because it does not itself use trails.  */
svn_error_t *
svn_fs_contents_changed (int *changed_p,
                         svn_fs_root_t *root1,
                         const char *path1,
                         svn_fs_root_t *root2,
                         const char *path2,
                         apr_pool_t *pool)
{
  struct things_changed_args args;

  /* Check that roots are in the same fs. */
  if ((svn_fs_root_fs (root1)) != (svn_fs_root_fs (root2)))
    return svn_error_create
      (SVN_ERR_FS_GENERAL, NULL,
       "Asking props changed in two different filesystems.");
  
  /* Check that both paths are files. */
  {
    int is_file;

    SVN_ERR (svn_fs_is_file (&is_file, root1, path1, pool));
    if (! is_file)
      return svn_error_createf
        (SVN_ERR_FS_GENERAL, NULL, "`%s' is not a file.", path1);
      
    SVN_ERR (svn_fs_is_file (&is_file, root2, path2, pool));
    if (! is_file)
      return svn_error_createf
        (SVN_ERR_FS_GENERAL, NULL, "`%s' is not a file.", path2);
  }

  args.root1      = root1;
  args.root2      = root2;
  args.path1      = path1;
  args.path2      = path2;
  args.changed_p  = changed_p;
  args.pool       = pool;

  SVN_ERR (svn_fs__retry_txn (root1->fs, txn_body_contents_changed,
                              &args, pool));

  return SVN_NO_ERROR;
}



/* Public interface to computing file text deltas.  */

/* Note:  it is acceptable for this function to call back into
   public FS API interfaces because it does not itself use trails.  */
svn_error_t *
svn_fs_get_file_delta_stream (svn_txdelta_stream_t **stream_p,
                              svn_fs_root_t *source_root,
                              const char *source_path,
                              svn_fs_root_t *target_root,
                              const char *target_path,
                              apr_pool_t *pool)
{
  svn_stream_t *source, *target;
  svn_txdelta_stream_t *delta_stream;

  /* Get read functions for the source file contents.  */
  if (source_root && source_path)
    SVN_ERR (svn_fs_file_contents (&source, source_root, source_path, pool));
  else
    source = svn_stream_empty (pool);

  /* Get read functions for the target file contents.  */
  SVN_ERR (svn_fs_file_contents (&target, target_root, target_path, pool));

  /* Create a delta stream that turns the ancestor into the target.  */
  svn_txdelta (&delta_stream, source, target, pool);

  *stream_p = delta_stream;
  return SVN_NO_ERROR;
}



/* Determining the revisions in which a given set of paths were changed. */

struct revisions_changed_baton
{
  apr_hash_t *revs;
  int cross_copy_history;
  const svn_fs_id_t *successor_id;
};


static svn_error_t *
revisions_changed_callback (void *baton,
                            dag_node_t *node,
                            int *done,
                            trail_t *trail)
{
  struct revisions_changed_baton *b = baton;

  /* If there is no NODE, then this is the last call, so we need to
     flush our B->SUCCESSOR_ID cache. */
  if (! node)
    {
      b->successor_id = NULL;
    }
  else
    {
      svn_revnum_t *rev 
        = apr_palloc (apr_hash_pool_get (b->revs), sizeof (*rev));

      /* Check B->CROSS_COPY_HISTORY.  If we are not supposed to cross
         copy history, then compare NODE's ID against B->SUCCESSOR_ID
         to see if we have done so, and if so, set the DONE flag and
         exit. */
      if ((! b->cross_copy_history) && (b->successor_id))
        {
          if (strcmp (svn_fs__id_copy_id (svn_fs__dag_get_id (node)), 
                      svn_fs__id_copy_id (b->successor_id)))
            {
              *done = 1;
              return SVN_NO_ERROR;
            }
        }

      /* See what NODE's created revision is. */
      SVN_ERR (svn_fs__dag_get_revision (rev, node, trail));

      /* If it's a valid revision, then add this to the baton's hash. */
      if (SVN_IS_VALID_REVNUM (*rev))
        apr_hash_set (b->revs, (void *)rev, sizeof (rev), (void *)1);

      /* Copy NODE's ID into B->SUCCESSOR_ID and cache it so we can do
         the proper copy history crossing detection mentioned above on
         the next iteration of this callback. */
      b->successor_id = svn_fs__id_copy (svn_fs__dag_get_id (node), 
                                         trail->pool);
    }

  return SVN_NO_ERROR;
}


struct revisions_changed_args
{
  apr_hash_t *revs;
  svn_fs_t *fs;
  const svn_fs_id_t *id;
  int cross_copy_history;
};


static svn_error_t *
txn_body_revisions_changed (void *baton, trail_t *trail)
{
  struct revisions_changed_args *args = baton;
  struct revisions_changed_baton b;
  dag_node_t *node;
  svn_revnum_t *rev 
    = apr_palloc (apr_hash_pool_get (args->revs), sizeof (*rev));

  /* Our callback baton will use the REVS array in our caller's
     baton. */
  b.revs = args->revs;

  /* Also, note in our baton whether we wish to cross copy history. */
  b.cross_copy_history = args->cross_copy_history;

  /* Flush the SUCCESSOR_ID member of the callback baton as we are
     about to start working on a new node. */
  b.successor_id = NULL;

  /* Get the NODE for ARGS->id.  */
  SVN_ERR (svn_fs__dag_get_node (&node, args->fs, args->id, trail));

  /* Add NODE's created rev to the array in the baton. */
  SVN_ERR (svn_fs__dag_get_revision (rev, node, trail));
  if (SVN_IS_VALID_REVNUM (*rev))
    apr_hash_set (b.revs, (void *)rev, sizeof (rev), (void *)1);

  /* Walk NODE's predecessors, harvesting revisions changed. */
  return svn_fs__dag_walk_predecessors (node, revisions_changed_callback,
                                        &b, trail);
}



/* Note:  it is acceptable for this function to call back into
   public FS API interfaces because it does not itself use trails.  */
svn_error_t *
svn_fs_revisions_changed (apr_array_header_t **revs,
                          svn_fs_root_t *root,
                          const apr_array_header_t *paths,
                          int cross_copy_history,
                          apr_pool_t *pool)
{
  struct revisions_changed_args args;
  svn_fs_t *fs = svn_fs_root_fs (root);
  int i;
  apr_hash_t *all_revs = apr_hash_make (pool);
  apr_pool_t *subpool = svn_pool_create (pool);
  apr_hash_index_t *hi;

  /* Populate the common baton members. */
  args.revs = all_revs;
  args.fs = fs;
  args.cross_copy_history = cross_copy_history;

  /* Get the node revision id for each PATH under ROOT, and find out
     in which revisions that node revision id was changed.  */
  for (i = 0; i < paths->nelts; i++)
    {
      SVN_ERR (svn_fs_node_id (&(args.id), root, 
                               APR_ARRAY_IDX (paths, i, const char *), 
                               subpool));
      SVN_ERR (svn_fs__retry_txn (fs, txn_body_revisions_changed,
                                  &args, subpool));
      svn_pool_clear (subpool);
    }

  /* Destroy all memory used, except the revisions hash. */
  svn_pool_destroy (subpool);
  
  /* Now build the return array from the keys in the hash table.  The
     items in the array share storage with the hash keys, but that's
     alright because the hash keys are alloced in POOL.  */
  *revs = apr_array_make (pool, apr_hash_count (all_revs), 
                          sizeof (svn_revnum_t));
  for (hi = apr_hash_first (pool, all_revs); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      svn_revnum_t revision;

      apr_hash_this (hi, &key, NULL, NULL);
      revision = *((const svn_revnum_t *)key);
      (*((svn_revnum_t *) apr_array_push (*revs))) = revision;
    }

  /* Now sort the array */
  qsort ((*revs)->elts, (*revs)->nelts, (*revs)->elt_size, 
         svn_sort_compare_revisions);

  /* Return the array. */
  return SVN_NO_ERROR;
}


struct paths_changed_args
{
  apr_hash_t *changes;
  svn_fs_root_t *root;
};


static svn_error_t *
txn_body_paths_changed (void *baton,
                        trail_t *trail)
{
  struct paths_changed_args *args = baton;
  const char *txn_id;
  svn_fs_t *fs = svn_fs_root_fs (args->root);

  /* Get the transaction ID from ROOT. */
  if (svn_fs_is_revision_root (args->root))
    SVN_ERR (svn_fs__rev_get_txn_id 
             (&txn_id, fs, svn_fs_revision_root_revision (args->root), trail));
  else
    txn_id = svn_fs_txn_root_name (args->root, trail->pool);

  return svn_fs__bdb_changes_fetch (&(args->changes), fs, txn_id, trail);
}


svn_error_t *
svn_fs_paths_changed (apr_hash_t **changed_paths_p,
                      svn_fs_root_t *root,
                      apr_pool_t *pool)
{
  struct paths_changed_args args;
  args.root = root;
  args.changes = NULL;
  SVN_ERR (svn_fs__retry_txn (svn_fs_root_fs (root), txn_body_paths_changed,
                              &args, pool));
  *changed_paths_p = args.changes;
  return SVN_NO_ERROR;
}



/* Creating transaction and revision root nodes.  */


struct txn_root_args
{
  svn_fs_root_t **root_p;
  svn_fs_txn_t *txn;
};


static svn_error_t *
txn_body_txn_root (void *baton,
                   trail_t *trail)
{
  struct txn_root_args *args = baton;
  svn_fs_root_t **root_p = args->root_p;
  svn_fs_txn_t *txn = args->txn;
  svn_fs_t *fs = svn_fs_txn_fs (txn);
  const char *svn_txn_id = svn_fs__txn_id (txn);
  const svn_fs_id_t *root_id, *base_root_id;
  svn_fs_root_t *root;

  /* Verify that the transaction actually exists.  */
  SVN_ERR (svn_fs__get_txn_ids (&root_id, &base_root_id, fs, 
                                svn_txn_id, trail));

  root = make_txn_root (fs, svn_txn_id, trail->pool);

  *root_p = root;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_txn_root (svn_fs_root_t **root_p,
                 svn_fs_txn_t *txn,
                 apr_pool_t *pool)
{
  svn_fs_root_t *root;
  struct txn_root_args args;

  args.root_p = &root;
  args.txn    = txn;
  SVN_ERR (svn_fs__retry_txn (svn_fs_txn_fs (txn), txn_body_txn_root,
                              &args, pool));

  *root_p = root;
  return SVN_NO_ERROR;
}


struct revision_root_args
{
  svn_fs_root_t **root_p;
  svn_fs_t *fs;
  svn_revnum_t rev;
};


static svn_error_t *
txn_body_revision_root (void *baton,
                        trail_t *trail)
{
  struct revision_root_args *args = baton;
  dag_node_t *root_dir;
  svn_fs_root_t *root;

  SVN_ERR (svn_fs__dag_revision_root (&root_dir, args->fs, args->rev, trail));
  root = make_revision_root (args->fs, args->rev, root_dir, trail->pool);

  *args->root_p = root;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs_revision_root (svn_fs_root_t **root_p,
                      svn_fs_t *fs,
                      svn_revnum_t rev,
                      apr_pool_t *pool)
{
  struct revision_root_args args;
  svn_fs_root_t *root;

  SVN_ERR (svn_fs__check_fs (fs));

  args.root_p = &root;
  args.fs     = fs;
  args.rev    = rev;
  SVN_ERR (svn_fs__retry_txn (fs, txn_body_revision_root, &args, pool));

  *root_p = root;
  return SVN_NO_ERROR;
}
