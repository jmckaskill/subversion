/* dag.c : DAG-like interface filesystem, private to libsvn_fs
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

#include "svn_path.h"
#include "svn_fs.h"
#include "dag.h"
#include "err.h"
#include "fs.h"
#include "node-rev.h"
#include "nodes-table.h"
#include "txn-table.h"
#include "rev-table.h"
#include "skel.h"
#include "trail.h"



/* Initializing a filesystem.  */

struct dag_node_t
{
  /* The filesystem this dag node came from. */
  svn_fs_t *fs;

  /* The node revision ID for this dag node.  */
  svn_fs_id_t *id;

  /* The node's NODE-REVISION skel.  
     jimb todo: the contents of mutable nodes could be changed by
     other processes, so we should fetch them afresh within each
     trail.  */
  skel_t *contents;

  /* The transaction trail pool in which this dag_node_t was
     allocated. */
  apr_pool_t *pool;
};


/* Trail body for svn_fs__dag_init_fs. */
static svn_error_t *
txn_body_dag_init_fs (void *fs_baton, trail_t *trail)
{
  svn_fs_t *fs = fs_baton;

  /* Create empty root directory with node revision 0.0:
     "nodes" : "0.0" -> "(fulltext [(dir ()) ()])" */
  {
    static char unparsed_node_rev[] = "((dir ()) ())";
    skel_t *node_rev = svn_fs__parse_skel (unparsed_node_rev,
                                           sizeof (unparsed_node_rev) - 1,
                                           trail->pool);
    svn_fs_id_t *root_id = svn_fs_parse_id ("0.0", 3, trail->pool);

    SVN_ERR (svn_fs__put_node_revision (fs, root_id, node_rev, trail));
    SVN_ERR (svn_fs__stable_node (fs, root_id, trail));
  } 

  /* Link it into filesystem revision 0:
     "revisions" : 0 -> "(revision 3 0.0 ())" */
  {
    static char rev_skel[] = "(revision 3 0.0 ())";
    svn_revnum_t rev = 0;
    SVN_ERR (svn_fs__put_rev (&rev, fs,
                              svn_fs__parse_skel (rev_skel,
                                                  sizeof (rev_skel) - 1,
                                                  trail->pool),
                              trail->db_txn,
                              trail->pool));

    if (rev != 0)
      return svn_error_createf (SVN_ERR_FS_CORRUPT, 0, 0, fs->pool,
                                "initial revision number is not `0'"
                                " in filesystem `%s'",
                                fs->env_path);
  }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_init_fs (svn_fs_t *fs)
{
  return svn_fs__retry_txn (fs, txn_body_dag_init_fs, fs, fs->pool);
}



/* ### these functions are defined so that we can load the library.
   ### without them, we get undefined references from tree.c
   ### obviously, they don't work and will need to be filled in...
*/


const svn_fs_id_t *svn_fs__dag_get_id (dag_node_t *node)
{
  return node->id;
}


svn_fs_t *svn_fs__dag_get_fs (dag_node_t *node)
{
  return node->fs;
}


/* Helper for next three funcs */
static int
node_is_kind_p (dag_node_t *node, const char *kindstr)
{
  /* No gratutitous syntax (or null-value) checks in here, because
     we're assuming that lower layers have already scanned the content
     skel for validity. */

  /* The node "header" is the first element of a node-revision skel,
     itself a list. */
  skel_t *header = node->contents->children;
  
  /* The first element of the header should be an atom defining the
     node kind. */
  skel_t *kind = header->children;
  
  if (! memcmp (kind->data, kindstr, kind->len))
    return TRUE;
  else
    return FALSE;
}

int svn_fs__dag_is_file (dag_node_t *node)
{
  return node_is_kind_p (node, "file");
}

int svn_fs__dag_is_directory (dag_node_t *node)
{
  return node_is_kind_p (node, "dir");
}

int svn_fs__dag_is_copy (dag_node_t *node)
{
  return node_is_kind_p (node, "copy");
}


svn_error_t *svn_fs__dag_get_proplist (skel_t **proplist_p,
                                       dag_node_t *node,
                                       trail_t *trail)
{
  abort();
  /* NOTREACHED */
  return NULL;
}
svn_error_t *svn_fs__dag_set_proplist (dag_node_t *node,
                                       skel_t *proplist,
                                       trail_t *trail)
{
  abort();
  /* NOTREACHED */
  return NULL;
}
svn_error_t *svn_fs__dag_clone_child (dag_node_t **child_p,
                                      dag_node_t *parent,
                                      const char *name,
                                      trail_t *trail)
{
  abort();
  /* NOTREACHED */
  return NULL;
}
svn_error_t *
svn_fs__dag_revision_root (dag_node_t **node_p,
                           svn_fs_t *fs,
                           svn_revnum_t rev,
                           trail_t *trail)
{
  abort();
  /* NOTREACHED */
  return NULL;
}


svn_error_t *
svn_fs__dag_clone_root (dag_node_t **root_p,
                        svn_fs_t *fs,
                        const char *svn_txn,
                        trail_t *trail)
{
  svn_error_t *err;
  svn_fs_id_t *base_root_id, *root_id;
  dag_node_t *root_node;  /* The node we'll return. */
  skel_t *root_skel;      /* Skel contents of the node we'll return. */

  /* See if this transaction's root has already been cloned.
     If it has, return the clone.  Else clone it, and return the
     clone. */

  err = svn_fs__get_txn (&root_id, &base_root_id, fs, svn_txn, trail);
  if (err)
    return err;

  /* Oh, give me a clone... */
  if (svn_fs_id_eq (root_id, base_root_id))  /* root as yet uncloned */
    {
      skel_t *base_skel;

      /* Of my own flesh and bone...
         (Get the skel for the base node.) */
      err = svn_fs__get_node_revision (&base_skel, fs, base_root_id, trail);
      if (err)
        return err;

      /* With its Y-chromosome changed to X
         (Create the new, mutable root node.) */
      /* kff todo: put_representation_skel doesn't seem to do anything
         with mutability yet... */
      err = svn_fs__create_successor
        (&root_id, fs, base_root_id, base_skel, trail);
      if (err)
        return err;
    }

  /* One way or another, base_root_id now identifies a cloned root node. */

  /* Get the skel for the new root node. */
  err = svn_fs__get_node_revision (&root_skel, fs, root_id, trail);
  if (err)
    return err;

  /* kff todo: Hmm, time for a constructor?  Do any of these need to
     be copied?  I don't think so... */
  root_node = apr_pcalloc (trail->pool, sizeof (*root_node));
  root_node->fs = fs;
  root_node->id = root_id;
  root_node->contents = root_skel;
  root_node->pool = trail->pool;
  
  /* ... And when it is grown
   *      Then my own little clone
   *        Will be of the opposite sex!
   *
   * (Sung to the tune of "Home, Home on the Range", with thanks to
   * Randall Garrett and Isaac Asimov.)
   */

  *root_p = root_node;
  return SVN_NO_ERROR;
}


svn_error_t *svn_fs__dag_open (dag_node_t **child_p,
                               dag_node_t *parent,
                               const char *name,
                               trail_t *trail)
{
  abort();
  /* NOTREACHED */
  return NULL;
}

svn_error_t *svn_fs__dag_delete (dag_node_t *parent,
                                 const char *name,
                                 trail_t *trail)
{
  abort();
  /* NOTREACHED */
  return NULL;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
