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
#include "svn_error.h"
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
#include "validate.h"


/* Initializing a filesystem.  */

/* Node types */
typedef enum dag_node_kind_t
{
  dag_node_kind_file = 1, /* Purposely reserving 0 for error */
  dag_node_kind_dir,
  dag_node_kind_copy
}
dag_node_kind_t;


struct dag_node_t
{
  /* The filesystem this dag node came from. */
  svn_fs_t *fs;

  /* The pool in which this dag_node_t was allocated.  Unlike
     filesystem and root pools, this is not a private pool for this
     structure!  The caller may have allocated other objects of their
     own in it.  */
  apr_pool_t *pool;

  /* The node revision ID for this dag node, allocated in POOL.  */
  svn_fs_id_t *id;

  /* The node's type (file, dir, copy, etc.) */
  dag_node_kind_t kind;

  /* The node's NODE-REVISION skel, or zero if we haven't read it in
     yet.  This is allocated either in this node's POOL, if the node
     is immutable, or in some trail's pool, if the node is mutable.
     For mutable nodes, this must be reset to zero as soon as the
     trail in which we read it is completed.  Otherwise, we will end
     up with out-of-date content here.

     If you're willing to respect all the rules above, you can munge
     this yourself, but you're probably better off just calling
     `get_node_revision' and `set_node_revision', which take care of
     things for you.  */
  skel_t *node_revision;

};


/* Creating nodes. */

/* Looks at node-revision NODE_REV's 'kind' to see if it matches the
   kind described by KINDSTR. */
static int
node_is_kind_p (skel_t *node_rev,
                const char *kindstr)
{
  /* The first element of the header (which is the first element of
     the node-revision) should be an atom defining the node kind. */
  skel_t *kind = node_rev->children->children;

  return svn_fs__matches_atom (kind, kindstr);
}


/* Helper for svn_fs__dag_check_mutable.  
   WARNING! WARNING! WARNING!  This should not be called by *anything*
   that doesn't first get an up-to-date NODE-REVISION skel! */
static int
has_mutable_flag (skel_t *node_content)
{
  /* The node "header" is the first element of a node-revision skel,
     itself a list. */
  skel_t *header = node_content->children;
  
  /* The 3rd element of the header, IF it exists, is the header's
     first `flag'.  It could be NULL.  */
  skel_t *flag = header->children->next->next;
  
  while (flag)
    {
      /* Looking for the `mutable' flag, which is itself a list. */
      if (svn_fs__matches_atom (flag->children, "mutable"))
        return TRUE;

      /* Move to next header flag. */
      flag = flag->next;
    }
  
  /* Reached the end of the header skel, no mutable flag was found. */
  return FALSE;
}


/* Add the "mutable" flag to node revision CONTENT, using PARENT_ID.
   Allocate the flag in POOL; it is advisable that POOL be at least as
   long-lived as the pool CONTENT is allocated in.  If the mutability
   flag is already set, this function does nothing.  If PARENT_ID is
   null, the mutable flag skel will have the empty string as its
   PARENT-ID element. */
static void
set_mutable_flag (skel_t *content, svn_fs_id_t *parent_id, apr_pool_t *pool)
{
  if (has_mutable_flag (content))
    return;
  else
    {
      skel_t *flag_skel = svn_fs__make_empty_list (pool);
      svn_string_t *parent_id_string
        = (parent_id
           ? svn_fs_unparse_id (parent_id, pool)
           : svn_string_create ("", pool));
      
      svn_fs__prepend (svn_fs__mem_atom (parent_id_string->data,
                                         parent_id_string->len,
                                         pool),
                       flag_skel);
      svn_fs__prepend (svn_fs__str_atom ((char *) "mutable", pool),
                       flag_skel);

      svn_fs__append (flag_skel, content->children);
    }

  return;
}


/* Clear NODE's cache of its node revision.  */
static void
uncache_node_revision (void *baton)
{
  dag_node_t *node = baton;

  node->node_revision = 0;
}


/* Set NODE's node revision cache to SKEL, as part of TRAIL.
   SKEL must be allocated in TRAIL->pool.  */
static void
cache_node_revision (dag_node_t *node,
                     skel_t *skel,
                     trail_t *trail)
{
  if (has_mutable_flag (skel))
    {
      /* Mutable nodes might have other processes change their
         contents, so we must throw away this skel once the trail is
         complete.  */
      svn_fs__record_completion (trail, uncache_node_revision, node);
      node->node_revision = skel;
    }
  else
    {
      /* For immutable nodes, we can cache the contents permanently,
         but we need to copy them over into the node's own pool.  */
      node->node_revision = svn_fs__copy_skel (skel, node->pool);
    }
}
                     

/* Set *SKEL_P to the cached NODE-REVISION skel for NODE, as part of
   TRAIL.  If NODE is immutable, the skel is allocated in NODE->pool.
   If NODE is mutable, the skel is allocated in TRAIL->pool, and the
   cache will be cleared as soon as TRAIL completes.

   If you plan to change the contents of NODE, be careful!  We're
   handing you a pointer directly to our cached skel, not your own
   copy.  If you change the skel as part of some operation, but then
   some Berkeley DB function deadlocks or gets an error, you'll need
   to back out your skel changes, or else the cache will reflect
   changes that never got committed.  It's probably best not to change
   the skel structure at all.  */
static svn_error_t *
get_node_revision (skel_t **skel_p,
                   dag_node_t *node,
                   trail_t *trail)
{
  skel_t *node_revision;

  /* If we've already got a copy, there's no need to read it in.  */
  if (! node->node_revision)
    {
      /* Read it in, and cache it.  */
      SVN_ERR (svn_fs__get_node_revision (&node_revision, node->fs, node->id,
                                          trail));
      cache_node_revision (node, node_revision, trail);
    }
          
  /* Now NODE->node_revision is set.  */
  *skel_p = node->node_revision;
  return SVN_NO_ERROR;
}


/* Set the NODE-REVISION skel of NODE to SKEL as part of TRAIL, and
   keep NODE's cache up to date.  SKEL must be allocated in
   TRAIL->pool.  */
static svn_error_t *
set_node_revision (dag_node_t *node,
                   skel_t *skel,
                   trail_t *trail)
{
  /* Write it out.  */
  SVN_ERR (svn_fs__put_node_revision (node->fs, node->id, skel, trail));

  /* Since the write succeeded, update the cache.  */
  cache_node_revision (node, skel, trail);

  return SVN_NO_ERROR;
}


/* Constructor function for dag_node_t.
   Create a new *NODE representing the node identified by ID in FS.
   Allocate from TRAIL->pool, use TRAIL->pool for (*NODE)->pool. */
static svn_error_t *
create_node (dag_node_t **node,
             svn_fs_t *fs,
             svn_fs_id_t *id,
             trail_t *trail)
{
  dag_node_t *new_node;
  skel_t *contents;

  /* Construct the node. */
  new_node = apr_pcalloc (trail->pool, sizeof (*new_node));
  new_node->fs = fs;
  new_node->id = svn_fs_copy_id (id, trail->pool); 
  new_node->pool = trail->pool;

  /* Grab the contents so we can inspect the node's kind. */
  SVN_ERR (get_node_revision (&contents, new_node, trail));

  /* Initialize the KIND attribute */
  if (node_is_kind_p (contents, "file"))
    new_node->kind = dag_node_kind_file;
  else if (node_is_kind_p (contents, "dir"))
    new_node->kind = dag_node_kind_dir;
  else if (node_is_kind_p (contents, "copy"))
    new_node->kind = dag_node_kind_copy;
  else
    return svn_error_createf (SVN_ERR_FS_GENERAL, 0, 0, fs->pool,
                              "Attempt to create unknown kind of node");
  
  /* Return a fresh new node */
  *node = new_node;
  
  return SVN_NO_ERROR;
}


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



/* Trivial helper/accessor functions. */
int svn_fs__dag_is_file (dag_node_t *node)
{
  return (node->kind == dag_node_kind_file ? TRUE : FALSE);
}


int svn_fs__dag_is_directory (dag_node_t *node)
{
  return (node->kind == dag_node_kind_dir ? TRUE : FALSE);
}


int svn_fs__dag_is_copy (dag_node_t *node)
{
  return (node->kind == dag_node_kind_copy ? TRUE : FALSE);
}


const svn_fs_id_t *svn_fs__dag_get_id (dag_node_t *node)
{
  return node->id;
}


svn_fs_t *svn_fs__dag_get_fs (dag_node_t *node)
{
  return node->fs;
}




svn_error_t *
svn_fs__dag_check_mutable (svn_boolean_t *is_mutable, 
                           dag_node_t *node, 
                           trail_t *trail)
{
  skel_t *node_rev;
  SVN_ERR (get_node_revision (&node_rev, node, trail));
  *is_mutable = has_mutable_flag (node_rev);
  return SVN_NO_ERROR;
}



svn_error_t *svn_fs__dag_dir_entries (skel_t **entries_p,
                                      dag_node_t *node,
                                      trail_t *trail)
{
  abort();
  /* NOTREACHED */
  return NULL;
}



/* Examines directory PARENT's list of entries, searching for a entry
   named NAME (which is assumed to be a single path component).  If no
   such entry, *ENTRY is set to NULL.  Else *ENTRY is populated with
   that `entry' list skel, allocated in either PARENT->pool or
   TRAIL->pool (you don't get to choose, sorry), and guaranteed to be
   well-formed. */
static svn_error_t *
find_dir_entry (skel_t **entry, 
                dag_node_t *parent, 
                const char *name, 
                trail_t *trail)
{
  skel_t *node_rev;
  skel_t *header;
  
  /* Go get a fresh NODE-REVISION for this node. */
  SVN_ERR (get_node_revision (&node_rev, parent, trail));

  /* The node "header" is the first element of a node-revision skel,
     itself a list. */
  header = node_rev->children;

  if (header)
    {
      /* Make sure we're looking at a directory node here */
      if (svn_fs__matches_atom (header->children, "dir"))
        {
          /* The entry list is the 2nd element of the node-revision
             skel. */
          skel_t *entry_list = header->next;

          /* The entries are the children of the entry list,
             naturally. */
          skel_t *cur_entry = entry_list->children;

          /* search the entry list for one whose name matches NAME.  */
          for (cur_entry = entry_list->children; 
               cur_entry; cur_entry = cur_entry->next)
            {
              if (svn_fs__matches_atom (cur_entry->children, name))
                {
                  if (svn_fs__list_length (cur_entry) != 2)
                    {
                      return svn_error_createf
                        (SVN_ERR_FS_CORRUPT, 0, 0, trail->pool,
                         "directory entry \"%s\" ill-formed", name);
                    }
                  else
                    {
                      *entry = cur_entry;
                      return SVN_NO_ERROR;
                    }
                }
            }
        }
    }

  /* We never found the entry, but this is non-fatal. */
  *entry = (skel_t *)NULL;
  return SVN_NO_ERROR;
}
        

svn_error_t *svn_fs__dag_get_proplist (skel_t **proplist_p,
                                       dag_node_t *node,
                                       trail_t *trail)
{
  skel_t *node_rev;
  skel_t *header;
  
  /* Go get a fresh NODE-REVISION for this node. */
  SVN_ERR (get_node_revision (&node_rev, node, trail));

  /* The node "header" is the first element of a node-revision skel,
     itself a list. */
  header = node_rev->children;

  /* The property list is the 2nd item in the header skel. */
  *proplist_p = header->next;

  return SVN_NO_ERROR;
}


svn_error_t *svn_fs__dag_set_proplist (dag_node_t *node,
                                       skel_t *proplist,
                                       trail_t *trail)
{
  skel_t *node_rev;
  skel_t *header;
  
  /* Sanity check: this node better be mutable! */
  {
    svn_boolean_t is_mutable;
    
    SVN_ERR (svn_fs__dag_check_mutable (&is_mutable, node, trail));

    if (! is_mutable)
      {
        svn_string_t *idstr = svn_fs_unparse_id (node->id, node->pool);
        return 
          svn_error_createf 
          (SVN_ERR_FS_NOT_MUTABLE, 0, NULL, trail->pool,
           "Can't set_proplist on *immutable* node-revision %s", 
           idstr->data);
      }
  }

  /* Well-formed tests:  make sure the incoming proplist is of the
     form 
               PROPLIST ::= (PROP ...) ;
                   PROP ::= atom atom ;                     */
  {
    skel_t *this;
    int len = svn_fs__list_length (proplist);

    /* Does proplist contain an even number of elements? (If proplist
       isn't a list in the first place, list_length will return -1,
       which will still fail the test.) */
    if (len % 2 != 0)
      abort ();
    
    /* Is each element an atom? */
    for (this = proplist->children; this; this = this->next)
      {
        if (! this->is_atom)
          abort ();
      }
  }
  
  /* Go get a fresh NODE-REVISION for this node. */
  SVN_ERR (get_node_revision (&node_rev, node, trail));

  /* The node "header" is the first element of a node-revision skel,
     itself a list. */
  header = node_rev->children;

  /* Insert the new proplist into the content_skel.  */
  /* jimb: Watch out!  Once we've got content caching working, this
     will be changing the cached skel.  If the operation below fails
     or deadlocks, the cache will be wrong.  */
  header->children->next = proplist;
  
  /* Commit the new content_skel, within the given trail. */
  SVN_ERR (set_node_revision (node, node_rev, trail));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_revision_root (dag_node_t **node_p,
                           svn_fs_t *fs,
                           svn_revnum_t rev,
                           trail_t *trail)
{
  svn_fs_id_t *root_id;

  SVN_ERR (svn_fs__rev_get_root (&root_id, fs, rev, trail));
  SVN_ERR (create_node (node_p, fs, root_id, trail));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__dag_txn_root (dag_node_t **node_p,
                      svn_fs_t *fs,
                      const char *txn,
                      trail_t *trail)
{
  svn_fs_id_t *root_id, *ignored;
  
  SVN_ERR (svn_fs__get_txn (&root_id, &ignored, fs, txn, trail));
  SVN_ERR (create_node (node_p, fs, root_id, trail));

  return SVN_NO_ERROR;
}
 

svn_error_t *
svn_fs__dag_clone_child (dag_node_t **child_p,
                         dag_node_t *parent,
                         const char *name,
                         trail_t *trail)
{
  /* cmpilato todo:  Make this not suck. */
  dag_node_t *cur_entry; /* parent's current entry named NAME */
  svn_fs_id_t *new_node_id; /* node id we'll put into NEW_NODE */
  skel_t *node_rev; /* NODE-REVISION of parent's current entry */

  {
    svn_boolean_t is_mutable;

    SVN_ERR (svn_fs__dag_check_mutable (&is_mutable, parent, trail));

    if (! is_mutable)
    {
      /* return some nasty error */
      return 
        svn_error_createf 
        (SVN_ERR_FS_NOT_MUTABLE, 0, NULL, trail->pool,
         "Attempted to clone child of non-mutable node");
    }
  }

  /* Make sure that NAME is a single path component. */
  if (! svn_fs__is_single_path_component (name))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_SINGLE_PATH_COMPONENT, 0, NULL, trail->pool,
       "Attempted to make a child clone with an illegal name `%s'", name);

  /* Find the node named NAME in PARENT's entries list if it exists. */
  SVN_ERR (svn_fs__dag_open (&cur_entry,
                             parent,
                             name,
                             trail));

  /* Go get a fresh NODE-REVISION for this node. */
  SVN_ERR (get_node_revision (&node_rev, cur_entry, trail));

  {
    svn_boolean_t is_mutable;

    /* Check for mutability in the node we found.  If it's mutable, we
       don't need to clone it. */
    SVN_ERR (svn_fs__dag_check_mutable (&is_mutable, cur_entry, trail));
      
    if (is_mutable)
      {
        /* This has already been cloned */
        new_node_id = cur_entry->id;
      }
    else
      {
        svn_string_t *id_str;
        skel_t *entry_skel;

        /* Do the clone thingy here. */
        SVN_ERR (svn_fs__create_successor (&new_node_id, 
                                           parent->fs, 
                                           cur_entry->id, 
                                           node_rev,
                                           trail));

        /* We can take our old ENTRY_SKEL and make it's ID point to
           whole new SKEL_T that contains the new clone's ID, now. */
        id_str = svn_fs_unparse_id (new_node_id, trail->pool);
        SVN_ERR (find_dir_entry (&entry_skel, parent, name, trail));
        entry_skel->children->next = svn_fs__str_atom (id_str->data,
                                                       trail->pool);
        /* jimb: don't forget to write the changed directory back into
           the database!  */
      }
  }

  /* Initialize the youngster. */
  SVN_ERR (create_node (child_p, svn_fs__dag_get_fs (parent), 
                        new_node_id, trail));
  
  return SVN_NO_ERROR;
}



svn_error_t *
svn_fs__dag_clone_root (dag_node_t **root_p,
                        svn_fs_t *fs,
                        const char *svn_txn,
                        trail_t *trail)
{
  svn_fs_id_t *base_root_id, *root_id;
  skel_t *root_skel;      /* Skel contents of the node we'll return. */
  
  /* Get the node ID's of the root directories of the transaction and
     its base revision.  */
  SVN_ERR (svn_fs__get_txn (&root_id, &base_root_id, fs, svn_txn, trail));

  /* Oh, give me a clone...
     (If they're the same, we haven't cloned the transaction's root
     directory yet.)  */
  if (svn_fs_id_eq (root_id, base_root_id)) 
    {
      /* Of my own flesh and bone...
         (Get the NODE-REVISION skel for the base node, and then write
         it back out as the clone.) */
      SVN_ERR (svn_fs__get_node_revision (&root_skel, fs, base_root_id,
                                          trail));

      /* With its Y-chromosome changed to X...
         (If it's not mutable already, make it so). */
      if (! has_mutable_flag (root_skel))
        set_mutable_flag (root_skel, NULL, trail->pool);

      /* Store it. */
      SVN_ERR (svn_fs__create_successor (&root_id, fs, base_root_id, root_skel,
                                         trail));
    }

  /* One way or another, root_id now identifies a cloned root node. */
  SVN_ERR (create_node (root_p, fs, root_id, trail));

  /* ... And when it is grown
   *      Then my own little clone
   *        Will be of the opposite sex!
   */
  SVN_ERR (svn_fs__set_txn_root (fs, svn_txn, root_id, trail));

  /*
   * (Sung to the tune of "Home, Home on the Range", with thanks to
   * Randall Garrett and Isaac Asimov.)
   */

  return SVN_NO_ERROR;
}



svn_error_t *svn_fs__dag_delete (dag_node_t *parent,
                                 const char *name,
                                 trail_t *trail)
{
  /* cmpilato todo:  Make this not suck. */
  skel_t *node_rev, *new_dirent_list, *old_entry, *entry;
  int deleted = FALSE;

  /* Make sure we're looking at a directory node. */
  if (! svn_fs__dag_is_directory (parent))
    return 
      svn_error_createf
      (SVN_ERR_FS_NOT_DIRECTORY, 0, NULL, parent->pool,
       "Attempted to delete entry `%s' from *non*-directory node.",
       name);    

  {
    svn_boolean_t is_mutable;

    SVN_ERR (svn_fs__dag_check_mutable (&is_mutable, parent, trail));

    if (! is_mutable)
      {
        return 
          svn_error_createf
          (SVN_ERR_FS_NOT_MUTABLE, 0, NULL, parent->pool,
           "Attempted to delete entry `%s' from *immutable* directory node.",
           name);
      }
  }

  /* Make sure that NAME is a single path component. */
  if (! svn_fs__is_single_path_component (name))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_SINGLE_PATH_COMPONENT, 0, NULL, trail->pool,
       "Attempted to delete a node with an illegal name `%s'", name);

  /* Go get a fresh NODE-REVISION for this node. */
  SVN_ERR (get_node_revision (&node_rev, parent, trail));

  /* Dup the parent's dirent list in trail->pool.  Then we can safely
     munge it all we want. */
  new_dirent_list = svn_fs__copy_skel (node_rev->children->next, 
                                       trail->pool);

  entry = new_dirent_list->children;
  old_entry = NULL;

  while (entry)
    {
      if (svn_fs__matches_atom (entry->children, name))
        {
          /* Aha!  We want to remove this entry from the list. */

          /* We actually have to *retrieve* this entry, however, and
             make sure that we're not trying to remove a non-empty
             dir.  (This is part of this routine's promise.) */
          skel_t *entry_content;
          skel_t *id_skel = entry->children->next;
          svn_fs_id_t *id = svn_fs_parse_id (id_skel->data, id_skel->len,
                                             trail->pool);
          SVN_ERR (svn_fs__get_node_revision (&entry_content,
                                              parent->fs,
                                              id,
                                              trail));

          if (svn_fs__matches_atom (entry_content->children->children,
                                    "dir"))
            {
              if (has_mutable_flag (entry_content))
                {
                  int len = 
                    svn_fs__list_length (entry_content->children->next);
                  if (len != 0)
                    return 
                      svn_error_createf
                      (SVN_ERR_FS_DIR_NOT_EMPTY, 0, NULL, parent->pool,
                       "Attempted to delete *non-empty* directory `%s'.",
                       name);                        
                }
            }

          /* Just "lose" this entry by setting the *previous* entry's
             next ptr to the current entry's next ptr. */          
          if (! old_entry)
            /* Base case:  the very *first* entry matched. */
            new_dirent_list->children = entry->next;
          else
            old_entry->next = entry->next;

          deleted = TRUE;
          break;
        }

      /* No match, move to next entry. */
      old_entry = entry;
      entry = entry->next;
    }
    
  if (! deleted)
    return 
      svn_error_createf
      (SVN_ERR_FS_NO_SUCH_ENTRY, 0, NULL, parent->pool,
       "Can't delete entry `%s', not found in parent dir.",
       name);      
    
  /* Else, the linked list has been appropriately modified.  Hook it
     back into the content skel and re-write the node-revision. */
  node_rev->children->next = new_dirent_list;

  SVN_ERR (svn_fs__put_node_revision (parent->fs,
                                      parent->id,
                                      node_rev,
                                      trail));
  
  return SVN_NO_ERROR;
}


/* Helper function for make_entry and svn_fs__dag_link */

/* Adds to PARENT an ENTRY skel which refers to CHILD named NAME.
   Allocations are done in TRAIL.  

   Assumptions:
   - PARENT is a directory.
   - PARENT does not already have an entry named NAME.
   - CHILD is not an ancestor of parent
   - NAME is a single path component
*/
static svn_error_t *
add_new_entry (dag_node_t *parent,
               dag_node_t *child,
               const char *name,
               trail_t *trail)
{
  skel_t *parent_node_rev;
  skel_t *new_node_rev;
  skel_t *entry_list;
  svn_string_t *node_id_str;

  /* Now, we need to tell the parent that it has another new mouth
     to feed.  So, we get the NODE-REVISION skel of the parent... */
  SVN_ERR (get_node_revision (&parent_node_rev, parent, trail));

  /* ...make a copy of it... */
  new_node_rev = svn_fs__copy_skel (parent_node_rev, trail->pool);

  /* ...construct a new ENTRY skel to be added to the
     parent's NODE-REVISION skel... */
  entry_list = svn_fs__make_empty_list (trail->pool);
  node_id_str = svn_fs_unparse_id (svn_fs__dag_get_id (child),
                                   trail->pool);
  svn_fs__prepend (svn_fs__str_atom (node_id_str->data, trail->pool),
                   entry_list);
  svn_fs__prepend (svn_fs__str_atom ((char *) name, trail->pool),
                   entry_list);
      
  /* ...and now we have an ENTRY skel for this new child: (NAME ID).
     Now we get to slap this entry into the parent's list of entries.  */
  svn_fs__append (entry_list, new_node_rev->children->next);
  
  /* Finally, update the parent's stored skel. */
  SVN_ERR (set_node_revision (parent, new_node_rev, trail));

  return SVN_NO_ERROR;
}

/* Helper for the next two functions. */

/* Make a new entry named NAME in PARENT, as part of TRAIL.  If IS_DIR
 * is true, then the node revision the new entry points to will be a
 * directory, else it will be a file.  The new node will be allocated
 * in TRAIL->pool.  PARENT must be mutable, and must not have an entry
 * named NAME.
 */
static svn_error_t *
make_entry (dag_node_t **child_p,
            dag_node_t *parent,
            const char *name,
            svn_boolean_t is_dir,
            trail_t *trail)
{
  svn_fs_id_t *new_node_id;
  skel_t *new_node_skel;

  /* Make sure that parent is a directory */
  if (! svn_fs__dag_is_directory (parent))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_DIRECTORY, 0, NULL, trail->pool,
       "Attempted to create entry in non-directory parent");
    
  /* Check that parent does not already have an entry named NAME. */
  {
    skel_t *entry_skel;

    SVN_ERR (find_dir_entry (&entry_skel, parent, name, trail));
    if (entry_skel)
      {
        return 
          svn_error_createf 
          (SVN_ERR_FS_ALREADY_EXISTS, 0, NULL, trail->pool,
           "Attempted to create entry that already exists");
      }
  }

  /* Check that the parent is mutable. */
  {
    svn_boolean_t is_mutable;

    SVN_ERR (svn_fs__dag_check_mutable (&is_mutable, parent, trail));
    if (! is_mutable)
      {
        return 
          svn_error_createf 
          (SVN_ERR_FS_NOT_MUTABLE, 0, NULL, trail->pool,
           "Attempted to clone child of non-mutable node");
      }
  }

  /* Make sure that NAME is a single path component. */
  if (! svn_fs__is_single_path_component (name))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_SINGLE_PATH_COMPONENT, 0, NULL, trail->pool,
       "Attempted to create a node with an illegal name `%s'", name);

  /* Create the new node's NODE-REVISION skel */
  {
    skel_t *header_skel;
    skel_t *flag_skel;
    svn_string_t *id_str;

    /* Call .toString() on parent's id -- oops!  This isn't Java! */
    id_str = svn_fs_unparse_id (parent->id, trail->pool);
    
    /* Create a new skel for our new node, the format of which is
       (HEADER KIND-SPECIFIC).  If we are making a directory, the
       HEADER is (`dir' PROPLIST (`mutable' PARENT-ID)).  If not, then
       this is a file, whose HEADER is (`file' PROPLIST (`mutable'
       PARENT-ID)).  KIND-SPECIFIC is an empty atom for files, an
       empty list for directories. */
    
    /* Step 1: create the FLAG skel. */
    flag_skel = svn_fs__make_empty_list (trail->pool);
    svn_fs__prepend (svn_fs__str_atom (id_str->data, trail->pool),
                     flag_skel);
    svn_fs__prepend (svn_fs__str_atom ((char *) "mutable", trail->pool), 
                     flag_skel);
    /* Now we have a FLAG skel: (`mutable' PARENT-ID) */
    
    /* Step 2: create the HEADER skel. */
    header_skel = svn_fs__make_empty_list (trail->pool);
    svn_fs__prepend (flag_skel, header_skel);
    svn_fs__prepend (svn_fs__make_empty_list (trail->pool),
                     header_skel);
    if (is_dir)
      {
        svn_fs__prepend (svn_fs__str_atom ((char *) "dir", trail->pool),
                         header_skel);
      }
    else
      {
        svn_fs__prepend (svn_fs__str_atom ((char *) "file", trail->pool),
                         header_skel);
      }
    /* Now we have a HEADER skel: (`file'-or-`dir' () FLAG) */
    
    /* Step 3: assemble the NODE-REVISION skel. */
    new_node_skel = svn_fs__make_empty_list (trail->pool);
    if (is_dir)
      {
        svn_fs__prepend (svn_fs__make_empty_list (trail->pool),
                         new_node_skel);
      }
    else
      {
        svn_fs__prepend (svn_fs__str_atom ((char *) "", trail->pool),
                         new_node_skel);
      }
    svn_fs__prepend (header_skel, new_node_skel);
    /* All done, skel-wise.  We have a NODE-REVISION skel as described
       far above. */
    
    /* Time to actually create our new node in the filesystem */
    SVN_ERR (svn_fs__create_node (&new_node_id, parent->fs,
                                  new_node_skel, trail));
  }

  /* Create a new node_dag_t for our new node */
  SVN_ERR (create_node (child_p, svn_fs__dag_get_fs (parent),
                        new_node_id, trail));

  /* We can safely call add_new_entry because we already know that
     PARENT is mutable, and we just created CHILD, so we know it has
     no ancestors (therefore, PARENT cannot be an ancestor of CHILD) */
  SVN_ERR (add_new_entry (parent, *child_p, name, trail ));

  return SVN_NO_ERROR;
}


svn_error_t *svn_fs__dag_make_file (dag_node_t **child_p,
                                    dag_node_t *parent,
                                    const char *name,
                                    trail_t *trail)
{
  /* Call our little helper function */
  return make_entry (child_p, parent, name, FALSE, trail);
}


svn_error_t *svn_fs__dag_make_dir (dag_node_t **child_p,
                                   dag_node_t *parent,
                                   const char *name,
                                   trail_t *trail)
{
  /* Call our little helper function */
  return make_entry (child_p, parent, name, TRUE, trail);
}


svn_error_t *svn_fs__dag_link (dag_node_t *parent,
                               dag_node_t *child,
                               const char *name,
                               trail_t *trail)
{
  /* Make sure that parent is a directory */
  if (! svn_fs__dag_is_directory (parent))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_DIRECTORY, 0, NULL, trail->pool,
       "Attempted to create entry in non-directory parent");
    
  {
    svn_boolean_t is_mutable;

    /* Make sure parent is mutable */
    SVN_ERR (svn_fs__dag_check_mutable (&is_mutable, parent, trail));
    if (! is_mutable)
      return 
        svn_error_createf 
        (SVN_ERR_FS_NOT_MUTABLE, 0, NULL, trail->pool,
         "Can't add a link from an immutable parent");

    /* Make sure child is IMmutable */
    SVN_ERR (svn_fs__dag_check_mutable (&is_mutable, child, trail));
    if (is_mutable)
      return 
        svn_error_createf 
        (SVN_ERR_FS_NOT_MUTABLE, 0, NULL, trail->pool,
         "Can't add a link to a mutable child");
  }

  /* Make sure that NAME is a single path component. */
  if (! svn_fs__is_single_path_component (name))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_SINGLE_PATH_COMPONENT, 0, NULL, trail->pool,
       "Attempted to link to a node with an illegal name `%s'", name);

  {
    skel_t *entry_skel;

    /* Verify that this parent node does not already have an entry named
       NAME. */
    SVN_ERR (find_dir_entry (&entry_skel, parent, name, trail));
    if (entry_skel)
      return 
        svn_error_createf 
        (SVN_ERR_FS_ALREADY_EXISTS, 0, NULL, trail->pool,
         "Attempted to create entry that already exists");
  }

  /* We can safely call add_new_entry because we already know that
     PARENT is mutable, and we know that CHILD is immutable (since
     every parent of a mutable node is mutable itself, we know that
     CHILD can't be equal to, or a parent of, PARENT).  */
  SVN_ERR (add_new_entry (parent, child, name, trail ));

  return SVN_NO_ERROR;
}


/* svn_fs__dag_get_contents():

   Right now, we *always* hold an entire node-revision skel in
   memory.  Someday this routine will evolve to incrementally read
   large file contents from disk. 
*/

/* Local typedef for __dag_get_contents */
typedef struct file_content_baton_t
{
  /* Yum, the entre contents of the file in RAM.  This is all
     allocated in trail->pool (the trail passed to __dag_get_contents). */
  skel_t *text;
  
  /* How many bytes have been read already. */
  apr_size_t offset;

} file_content_baton_t;


/* Helper func of type svn_read_func_t, used to read the CONTENTS
   stream in __dag_get_contents below. */
static svn_error_t *
read_file_contents (void *baton, char *buffer, apr_size_t *len)
{
  file_content_baton_t *fbaton = (file_content_baton_t *) baton;

  /* To be perfectly clear... :) */
  apr_size_t want_len = *len;
  apr_size_t len_remaining = (fbaton->text->len) - (fbaton->offset);
  apr_size_t min_len = want_len <= len_remaining ? want_len : len_remaining;

  /* Sanity check */
  if (! fbaton->text->is_atom)
    abort();

  memcpy (buffer, (fbaton->text->data + fbaton->offset), min_len);
  fbaton->offset += min_len;
  *len = min_len;
  
  return SVN_NO_ERROR;
}


svn_error_t *svn_fs__dag_get_contents (svn_stream_t **contents,
                                       dag_node_t *file,
                                       trail_t *trail)
{ 
  skel_t *node_rev;
  file_content_baton_t *baton;

  /* Make sure our node is a file. */
  if (! svn_fs__dag_is_file (file))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_FILE, 0, NULL, trail->pool,
       "Attempted to get textual contents of a *non*-file node.");
  
  /* Build a read baton in trail->pool. */
  baton = apr_pcalloc (trail->pool, sizeof(*baton));
  baton->offset = 0;

  /* Go get a fresh node-revision for FILE. */
  SVN_ERR (get_node_revision (&node_rev, file, trail));
  
  /* This node-rev *might* be allocated in node's pool, or it *might*
     be allocated in trail's pool, depending on mutability.  However,
     because this routine promises to allocate the stream in trail's
     pool, the only *safe* thing to do is dup the skel there. */
  baton->text = svn_fs__copy_skel (node_rev->children->next, trail->pool);

  /* Create a stream object in trail->pool, and make it use our read
     func and baton. */
  *contents = svn_stream_create (baton, trail->pool);
  svn_stream_set_read (*contents, read_file_contents);

  /* Note that we're not registering any `close' func, because there's
     nothing to cleanup outside of our trail.  When the trail is
     freed, the stream/baton will be too. */ 

  return SVN_NO_ERROR;
}



svn_error_t *
svn_fs__dag_set_contents (dag_node_t *file,
                          svn_string_t *contents,
                          trail_t *trail)
{
  /* This whole routine will have to be reincarnated as a "streamy"
     interface someday. */
  skel_t *content_skel;

  /* Make sure our node is a file. */
  if (! svn_fs__dag_is_file (file))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_FILE, 0, NULL, trail->pool,
       "Attempted to set textual contents of a *non*-file node.");
  
  /* Get the node's current contents... */
  SVN_ERR (get_node_revision (&content_skel, file, trail));
  
  /* ...aaaaaaaaand then swap 'em out with some new ones! */
  content_skel->children->next = svn_fs__str_atom (contents->data,
                                                   trail->pool);

  /* Stash the file's new contents in the db. */
  SVN_ERR (svn_fs__put_node_revision (file->fs, file->id,
                                      content_skel, trail));

  return SVN_NO_ERROR;
}



dag_node_t *svn_fs__dag_dup (dag_node_t *node,
                             trail_t *trail)
{
  /* Allocate our new node. */
  dag_node_t *new_node = apr_pcalloc (trail->pool, sizeof (*new_node));

  new_node->fs = node->fs;
  new_node->pool = trail->pool;
  new_node->id = svn_fs_copy_id (node->id, node->pool);
  
  /* Leave new_node->node_revision zero for now, so it'll get read in.
     We can get fancy and duplicate node's cache later.  */

  return new_node;
}


/* Open the node named NAME in the directory PARENT, as part of TRAIL.
   Set *CHILD_P to the new node, allocated in TRAIL->pool.  NAME must be a
   single path component; it cannot be a slash-separated directory
   path.  */
svn_error_t *svn_fs__dag_open (dag_node_t **child_p,
                               dag_node_t *parent,
                               const char *name,
                               trail_t *trail)
{
  skel_t *entry_skel;
  svn_fs_id_t *node_id;

  /* Find the entry named NAME in PARENT if it exists. */
  SVN_ERR (find_dir_entry (&entry_skel, parent, name, trail));
  if (! entry_skel)
    {
      /* return some other nasty error */
      return 
        svn_error_createf 
        (SVN_ERR_FS_NOT_FOUND, 0, NULL, trail->pool,
         "Attempted to open non-existant child node \"%s\"", name);
    }
  
  /* Make sure that NAME is a single path component. */
  if (! svn_fs__is_single_path_component (name))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_SINGLE_PATH_COMPONENT, 0, NULL, trail->pool,
       "Attempted to open node with an illegal name `%s'", name);

  /* Get the node id this entry points to. */
  {
    skel_t *id_skel = entry_skel->children->next;
    node_id = svn_fs_parse_id (id_skel->data, 
                               id_skel->len,
                               trail->pool);
  }

  SVN_ERR (create_node (child_p, svn_fs__dag_get_fs (parent),
                        node_id, trail));

  return SVN_NO_ERROR;
}




/* Rename the node named FROM_NAME in FROM_DIR to TO_NAME in TO_DIR,
   as part of TRAIL.  FROM_DIR and TO_DIR must both be mutable; the
   node being renamed may be either mutable or immutable.  FROM_NAME
   and TO_NAME must be single path components; they cannot be
   slash-separated directory paths.

   This function ensures that the rename does not create a cyclic
   directory structure, by checking that TO_DIR is not a child of
   FROM_DIR.  */
svn_error_t *svn_fs__dag_rename (dag_node_t *from_dir, 
                                 const char *from_name,
                                 dag_node_t *to_dir, 
                                 const char *to_name,
                                 trail_t *trail)
{
  abort();
  /* NOTREACHED */
  return NULL;
}



/* Create a copy node named NAME in PARENT which refers to SOURCE_PATH
   in SOURCE_REVISION, as part of TRAIL.  Set *CHILD_P to a reference
   to the new node, allocated in TRAIL->pool.  PARENT must be mutable.
   NAME must be a single path component; it cannot be a slash-
   separated directory path.  */
svn_error_t *svn_fs__dag_make_copy (dag_node_t **child_p,
                                    dag_node_t *parent,
                                    const char *name,
                                    svn_revnum_t source_revision,
                                    const char *source_path,
                                    trail_t *trail)
{
  skel_t *new_node_skel;
  svn_fs_id_t *new_node_id;

  /* Make sure that parent is a directory */
  if (! svn_fs__dag_is_directory (parent))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_DIRECTORY, 0, NULL, trail->pool,
       "Attempted to create entry in non-directory parent");
    
  {
    svn_boolean_t is_mutable;
    
    /* Make sure the parent is mutable */
    SVN_ERR (svn_fs__dag_check_mutable (&is_mutable, parent, trail));
    if (! is_mutable) 
      return 
        svn_error_createf 
        (SVN_ERR_FS_NOT_MUTABLE, 0, NULL, trail->pool,
         "Attempted to make a copy node under a non-mutable parent");
  }

  /* Check that parent does not already have an entry named NAME. */
  {
    skel_t *entry_skel;

    SVN_ERR (find_dir_entry (&entry_skel, parent, name, trail));
    if (entry_skel)
      {
        return 
          svn_error_createf 
          (SVN_ERR_FS_ALREADY_EXISTS, 0, NULL, trail->pool,
           "Attempted to create entry that already exists");
      }
  }

  /* Make sure that NAME is a single path component. */
  if (! svn_fs__is_single_path_component (name))
    return 
      svn_error_createf 
      (SVN_ERR_FS_NOT_SINGLE_PATH_COMPONENT, 0, NULL, trail->pool,
       "Attempted to make a copy node with an illegal name `%s'", name);
    
  /* cmpilato todo: Need to validate SOURCE_REVISION and SOURCE_PATH
     with some degree of intelligence, I'm sure.  Should we make sure
     that SOURCE_REVISION is an existing revising?  Should we traverse
     the SOURCE_PATH in that revision to make sure that it really
     exists? */
  if (! SVN_IS_VALID_REVNUM(source_revision))
    return 
      svn_error_createf 
      (SVN_ERR_FS_GENERAL, 0, NULL, trail->pool,
       "Attempted to make a copy node with an invalid source revision");

  if ((! source_path) || (! strlen (source_path)))
    return 
      svn_error_createf 
      (SVN_ERR_FS_GENERAL, 0, NULL, trail->pool,
       "Attempted to make a copy node with an invalid source path");

  /* Create the new node's NODE-REVISION skel */
  {
    skel_t *header_skel;
    skel_t *flag_skel;
    skel_t *base_path_skel;
    svn_string_t *id_str;
    svn_string_t *rev;

    /* Create a string containing the SOURCE_REVISION */
    rev = svn_string_createf (trail->pool, "%lu", 
                              (unsigned long) source_revision);

    /* Get a string representation of the PARENT's node ID */
    id_str = svn_fs_unparse_id (parent->id, trail->pool);
    
    /* Create a new skel for our new copy node, the format of which is
       (HEADER SOURCE-REVISION (NAME ...)).  HEADER is (`copy'
       PROPLIST (`mutable' PARENT-ID)).  The list of NAMEs describes
       the path to the source file, described as a series of single
       path components (imagine a '/' between each successive NAME in
       thelist, if you will). */
    
    /* Step 1: create the FLAG skel. */
    flag_skel = svn_fs__make_empty_list (trail->pool);
    svn_fs__prepend (svn_fs__str_atom (id_str->data, trail->pool),
                     flag_skel);
    svn_fs__prepend (svn_fs__str_atom ((char *) "mutable", trail->pool), 
                     flag_skel);
    /* Now we have a FLAG skel: (`mutable' PARENT-ID) */
    
    /* Step 2: create the HEADER skel. */
    header_skel = svn_fs__make_empty_list (trail->pool);
    svn_fs__prepend (flag_skel, header_skel);
    /* cmpilato todo:  Find out of this is supposed to be an empty
       PROPLIST, or a copy of the PROPLIST from the source file. */
    svn_fs__prepend (svn_fs__make_empty_list (trail->pool),
                     header_skel);
    svn_fs__prepend (svn_fs__str_atom ((char *) "copy", trail->pool),
                     header_skel);
    /* Now we have a HEADER skel: (`copy' () FLAG) */

    /* Step 3: assemble the source path list. */
    base_path_skel = svn_fs__make_empty_list (trail->pool);
    /* cmpilato todo: Need to find out more on the topic of base
       paths.  Can they be relative, or only absolute, and what's the
       format of the skel in either case? */
    /* We now have a list of path components, (NAME ...) */

    /* Step 4: assemble the NODE-REVISION skel. */
    new_node_skel = svn_fs__make_empty_list (trail->pool);
    svn_fs__prepend (base_path_skel, new_node_skel);
    svn_fs__prepend (svn_fs__str_atom (rev->data, trail->pool), 
                     new_node_skel);
    svn_fs__prepend (header_skel, new_node_skel);
    /* All done, skel-wise.  We have a NODE-REVISION skel as described
       far above. */
    
    /* Time to actually create our new node in the filesystem */
    SVN_ERR (svn_fs__create_node (&new_node_id, parent->fs,
                                  new_node_skel, trail));
  }

  /* Create a new node_dag_t for our new node */
  SVN_ERR (create_node (child_p, svn_fs__dag_get_fs (parent),
                        new_node_id, trail));

  /* We can safely call add_new_entry because we already know that
     PARENT is mutable, and we just created CHILD, so we know it has
     no ancestors (therefore, PARENT cannot be an ancestor of CHILD) */
  SVN_ERR (add_new_entry (parent, *child_p, name, trail ));

  return SVN_NO_ERROR;

  abort();
  /* NOTREACHED */
  return NULL;
}


/* Set *REV_P and *PATH_P to the revision and path of NODE, which must
   be a copy node, as part of TRAIL.  Allocate *PATH_P in TRAIL->pool.  */
svn_error_t *svn_fs__dag_get_copy (svn_revnum_t *rev_p,
                                   char **path_p,
                                   dag_node_t *node,
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
