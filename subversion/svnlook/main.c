/*
 * main.c: Subversion server inspection tool.
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

/* ### todo:

   Not sure how to display changes in the txn case.  What is the diff
   against?  My first guess would be the base-rev of the txn, but I
   don't think I have access to that revision number.  Am I missing
   some API call (svn_fs_txn_base_revision() or something)?  Not
   likely, since we don't even store the base-revision in the txn
   (either in the FS or in the data struct.  Is this something we'd
   like to keep track of?  This would require:

   - modifying svn_fs_txn_t to include `svn_revnum_t base_rev'

   - modifying the transaction skel to include the base revision,
     which would mean it no longer needs to store the base root ID
     (since I think that can be derived from the base revision).
     Perhaps the following:

        TRANSACTION ::= ("transaction" ROOT-ID BASE-REV PROPLIST) ;

   - modifying stuff in txn-table.* to use this new skel structure.

   - adding a new API call to retrieve the base revision from a txn.
     Again, I think this would do the trick:

        svn_error_t *svn_fs_txn_base_revision (svn_revnum_t *rev,
                                               svn_fs_t *fs,
                                               svn_fs_txn_t *txn,
                                               apr_pool_t *pool);
*/

#include <apr_general.h>
#include <apr_pools.h>
#include <apr_time.h>

#define APR_WANT_STDIO
#define APR_WANT_STRFUNC
#include <apr_want.h>

#include "svn_types.h"
#include "svn_pools.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_fs.h"
#include "svn_repos.h"
#include "svn_time.h"
#include "svnlook.h"



/*** Some convenience macros and types. ***/

#define INT_ERR(expr)                              \
  do {                                             \
    svn_error_t *svn_err__temp = (expr);           \
    if (svn_err__temp)                             \
      svn_handle_error (svn_err__temp, stdout, 0); \
  } while (0)


typedef enum svnlook_cmd_t
{
  svnlook_cmd_default = 0,
  svnlook_cmd_log,
  svnlook_cmd_author,
  svnlook_cmd_date,
  svnlook_cmd_dirs_changed,
  svnlook_cmd_changed,
  svnlook_cmd_diff
  
} svnlook_cmd_t;


typedef struct svnlook_ctxt_t
{
  svn_fs_t *fs;
  svn_boolean_t is_revision;
  svn_revnum_t rev_id;
  svn_fs_txn_t *txn;
  
} svnlook_ctxt_t;



/*** Helper functions. ***/
static svn_error_t *
get_property (svn_stringbuf_t **prop_value, 
              svnlook_ctxt_t *c, 
              svn_string_t *prop_name,
              apr_pool_t *pool)
{
  /* Fetch transaction property... */
  if (! c->is_revision)
    return svn_fs_txn_prop (prop_value, c->txn, prop_name, pool);

  /* ...or revision property -- it's your call. */
  return svn_fs_revision_prop (prop_value, c->fs, c->rev_id, prop_name, pool);
}


static svn_error_t *
get_root (svn_fs_root_t **root,
          svnlook_ctxt_t *c,
          apr_pool_t *pool)
{
  /* Open up the appropriate root (revision or transaction). */
  if (c->is_revision)
    {
      /* If we didn't get a valid revision number, we'll look at the
         youngest revision. */
      if (! SVN_IS_VALID_REVNUM (c->rev_id))
        SVN_ERR (svn_fs_youngest_rev (&(c->rev_id), c->fs, pool));

      SVN_ERR (svn_fs_revision_root (root, c->fs, c->rev_id, pool));
    }
  else
    {
      SVN_ERR (svn_fs_txn_root (root, c->txn, pool));
    }

  return SVN_NO_ERROR;
}



/*** Subcommand handlers. ***/

/* Print the revision's log message to stdout, followed by a newline. */
static svn_error_t *
do_log (svnlook_ctxt_t *c, svn_boolean_t print_size, apr_pool_t *pool)
{
  svn_stringbuf_t *prop_value;
  svn_string_t prop_name;
  const char *name = SVN_PROP_REVISION_LOG;

  prop_name.data = name;
  prop_name.len = strlen (name);

  SVN_ERR (get_property (&prop_value, c, &prop_name, pool));

  if (prop_value && prop_value->data)
    {
      if (print_size)
        {
          printf ("%lu\n", (unsigned long)prop_value->len);
        }

      printf ("%s", prop_value->data);
    }
  else if (print_size)
    {
      printf ("0");
    }
  
  printf ("\n");
  return SVN_NO_ERROR;
}


/* Print the timestamp of the commit (in the revision case) or the
   empty string (in the transaction case) to stdout, followed by a
   newline. */
static svn_error_t *
do_date (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  if (c->is_revision)
    {
      svn_stringbuf_t *prop_value;
      svn_string_t prop_name;
      const char *name = SVN_PROP_REVISION_DATE;
      
      prop_name.data = name;
      prop_name.len = strlen (name);
      
      SVN_ERR (get_property (&prop_value, c, &prop_name, pool));

      if (prop_value && prop_value->data)
        {
          /* The date stored in the repository is in a really complex
             and precise format...and we don't care.  Let's convert
             that to just "YYYY-MM-DD hh:mm".

             ### todo: Right now, "svn dates" are not GMT, but the
             results of svn_time_from_string are.  This sucks. */
          apr_exploded_time_t extime;
          apr_time_t time;
          apr_status_t apr_err;
              
          time = svn_time_from_string (prop_value);
          apr_err = apr_explode_time (&extime, time, 0);
          if (apr_err)
            return svn_error_create (apr_err, 0, NULL, pool,
                                     "do_date: error exploding time");
              
          printf ("%04lu-%02lu-%02lu %02lu:%02lu GMT",
                  (unsigned long)(extime.tm_year + 1900),
                  (unsigned long)(extime.tm_mon + 1),
                  (unsigned long)(extime.tm_mday),
                  (unsigned long)(extime.tm_hour),
                  (unsigned long)(extime.tm_min));
        }
    }

  printf ("\n");
  return SVN_NO_ERROR;
}


/* Print the author of the commit to stdout, followed by a newline. */
static svn_error_t *
do_author (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  svn_stringbuf_t *prop_value;
  svn_string_t prop_name;
  const char *name = SVN_PROP_REVISION_AUTHOR;

  prop_name.data = name;
  prop_name.len = strlen (name);

  SVN_ERR (get_property (&prop_value, c, &prop_name, pool));

  if (prop_value && prop_value->data)
    printf ("%s", prop_value->data);
  
  printf ("\n");
  return SVN_NO_ERROR;
}


/* Recursively print only directory nodes that either a) have property
   mods, or b) contains files that have changes. */
static void
print_dirs_changed_tree (repos_node_t *root, 
                         svn_stringbuf_t *path,
                         apr_pool_t *pool)
{
  repos_node_t *tmp_node;
  int print_me = 0;
  svn_stringbuf_t *full_path;

  if (! root)
    return;

  /* Not a directory?  We're not interested. */
  if (root->kind != svn_node_dir)
    return;

  /* Got prop mods?  Excellent. */
  if (root->prop_mod)
    print_me = 1;

  if (! print_me)
    {
      /* Fly through the list of children, checking for modified files. */
      tmp_node = root->child;
      if (tmp_node)
        {
          if ((tmp_node->kind == svn_node_file)
              || (tmp_node->text_mod)
              || (tmp_node->action == 'A')
              || (tmp_node->action == 'D'))
            {
              print_me = 1;
            }
          while (tmp_node->sibling && (! print_me ))
            {
              tmp_node = tmp_node->sibling;
              if ((tmp_node->kind == svn_node_file)
                  || (tmp_node->text_mod)
                  || (tmp_node->action == 'A')
                  || (tmp_node->action == 'D'))
                {
                  print_me = 1;
                }
            }
        }
    }
  
  /* Print the node if it qualifies. */
  if (print_me)
    {
      printf ("%s/\n", path->data);
    }

  /* Recursively handle the node's children. */
  tmp_node = root->child;
  if (! tmp_node)
    return;

  full_path = svn_stringbuf_dup (path, pool);
  svn_path_add_component_nts (full_path, tmp_node->name, svn_path_repos_style);
  print_dirs_changed_tree (tmp_node, full_path, pool);
  while (tmp_node->sibling)
    {
      tmp_node = tmp_node->sibling;
      svn_stringbuf_set (full_path, path->data);
      svn_path_add_component_nts 
        (full_path, tmp_node->name, svn_path_repos_style);
      print_dirs_changed_tree (tmp_node, full_path, pool);
    }

  return;
}


/* Print a list of all directories in which files, or directory
   properties, have been modified. */
static svn_error_t *
do_dirs_changed (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  svn_fs_root_t *root, *base_root;
  svn_revnum_t base_rev_id;
  const svn_delta_edit_fns_t *editor;
  void *edit_baton;
  apr_hash_t *src_revs = apr_hash_make (pool);
  repos_node_t *tree;

  SVN_ERR (get_root (&root, c, pool));
  base_rev_id = c->rev_id - 1;
  apr_hash_set (src_revs, "", APR_HASH_KEY_STRING, &(c->rev_id));
  SVN_ERR (svn_fs_revision_root (&base_root, c->fs, base_rev_id, pool));
  SVN_ERR (svnlook_rev_changes_editor (&editor, &edit_baton, c->fs,
                                       root, base_root, pool));
  
  SVN_ERR (svn_repos_dir_delta (base_root, 
                                svn_stringbuf_create ("", pool), 
                                NULL, src_revs, root, 
                                svn_stringbuf_create ("", pool), 
                                editor, edit_baton, pool));

  tree = svnlook_edit_baton_tree (edit_baton);
  if (tree)
    print_dirs_changed_tree (tree, svn_stringbuf_create ("", pool), pool);

  return SVN_NO_ERROR;
}


/* Recursively print all nodes in the tree. */
static void
print_changed_tree (repos_node_t *root, 
                    svn_stringbuf_t *path,
                    apr_pool_t *pool)
{
  repos_node_t *tmp_node;
  svn_stringbuf_t *full_path;

  if (! root)
    return;

  /* Print the node. */
  tmp_node = root;
  printf ("%s%s\n", 
          path->data,
          tmp_node->kind == svn_node_dir ? "/" : "");
  
  /* Return here if the node has no children. */
  tmp_node = tmp_node->child;
  if (! tmp_node)
    return;

  /* Recursively handle the node's children. */
  full_path = svn_stringbuf_dup (path, pool);
  svn_path_add_component_nts (full_path, tmp_node->name, svn_path_repos_style);
  print_changed_tree (tmp_node, full_path, pool);
  while (tmp_node->sibling)
    {
      tmp_node = tmp_node->sibling;
      svn_stringbuf_set (full_path, path->data);
      svn_path_add_component_nts 
        (full_path, tmp_node->name, svn_path_repos_style);
      print_changed_tree (tmp_node, full_path, pool);
    }

  return;
}


/* Print a list of all paths modified in a format compatible with `svn
   update'. */
static svn_error_t *
do_changed (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  svn_fs_root_t *root, *base_root;
  svn_revnum_t base_rev_id;
  const svn_delta_edit_fns_t *editor;
  void *edit_baton;
  apr_hash_t *src_revs = apr_hash_make (pool);
  repos_node_t *tree;

  SVN_ERR (get_root (&root, c, pool));

  base_rev_id = c->rev_id - 1;
  apr_hash_set (src_revs, "", APR_HASH_KEY_STRING, &base_rev_id);

  SVN_ERR (svn_fs_revision_root (&base_root, c->fs, base_rev_id, pool));
  SVN_ERR (svnlook_rev_changes_editor (&editor, &edit_baton, c->fs,
                                       root, base_root, pool));
  
  SVN_ERR (svn_repos_dir_delta (base_root, 
                                svn_stringbuf_create ("", pool), 
                                NULL, src_revs, root, 
                                svn_stringbuf_create ("", pool), 
                                editor, edit_baton, pool));

  tree = svnlook_edit_baton_tree (edit_baton);
  if (tree)
    print_changed_tree (tree, svn_stringbuf_create ("", pool), pool);

  return SVN_NO_ERROR;
}


/* Print some diff-y stuff in a TBD way. :-) */
static svn_error_t *
do_diff (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  return SVN_NO_ERROR;
}


/* Recursively print all nodes in the tree. */
static void
print_tree (repos_node_t *root, 
            int indentation)
{
  repos_node_t *tmp_node;
  int i;

  if (! root)
    return;

  /* Print the indentation. */
  for (i = 0; i < indentation; i++)
    {
      printf (" ");
    }

  /* Print the node. */
  tmp_node = root;
  printf ("%s%s\n", 
          tmp_node->name, 
          tmp_node->kind == svn_node_dir ? "/" : "");
  
  /* Return here if the node has no children. */
  tmp_node = tmp_node->child;
  if (! tmp_node)
    return;

  /* Recursively handle the node's children. */
  print_tree (tmp_node, indentation + 1);
  while (tmp_node->sibling)
    {
      tmp_node = tmp_node->sibling;
      print_tree (tmp_node, indentation + 1);
    }

  return;
}


/* Print the diff between revision 0 and our our root. */
static svn_error_t *
do_tree (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  svn_fs_root_t *root, *base_root;
  svn_revnum_t base_rev_id;
  const svn_delta_edit_fns_t *editor;
  void *edit_baton;
  apr_hash_t *src_revs = apr_hash_make (pool);
  repos_node_t *tree;

  SVN_ERR (get_root (&root, c, pool));
  base_rev_id = c->rev_id - 1;
  apr_hash_set (src_revs, "", APR_HASH_KEY_STRING, &base_rev_id);
  SVN_ERR (svn_fs_revision_root (&base_root, c->fs, 0, pool));
  SVN_ERR (svnlook_rev_changes_editor (&editor, &edit_baton, c->fs,
                                       root, base_root, pool));
  
  SVN_ERR (svn_repos_dir_delta (base_root, 
                                svn_stringbuf_create ("", pool), 
                                NULL, src_revs, root, 
                                svn_stringbuf_create ("", pool), 
                                editor, edit_baton, pool));

  tree = svnlook_edit_baton_tree (edit_baton);
  if (tree)
    print_tree (tree, 0);

  return SVN_NO_ERROR;
}


/* Print author, date, log-size, log, and the tree associated with the
   given revision or transaction. */
static svn_error_t *
do_default (svnlook_ctxt_t *c, apr_pool_t *pool)
{
  SVN_ERR (do_author (c, pool));
  SVN_ERR (do_date (c, pool));
  SVN_ERR (do_log (c, TRUE, pool));
  SVN_ERR (do_tree (c, pool));
  return SVN_NO_ERROR;
}



/*** Argument parsing and usage. ***/
static void
usage (const char *progname, int exit_code)
{
  fprintf
    (exit_code ? stderr : stdout,
     "usage: %s REPOS_PATH rev REV [COMMAND] - inspect revision REV\n"
     "       %s REPOS_PATH txn TXN [COMMAND] - inspect transaction TXN\n"
     "       %s REPOS_PATH [COMMAND] - inspect the youngest revision\n"
     "\n"
     "REV is a revision number > 0.\n"
     "TXN is a transaction name.\n"
     "\n"
     "If no command is given, the default output lines (author, date,\n"
     "logsize, log, then the directory tree) will be printed.\n"
     "\n"
     "COMMAND can be one of: \n"
     "\n"
     "   log:           print log message to stdout.\n"
     "   author:        print author to stdout\n"
     "   date:          date to stdout (only for revs, not txns)\n"
     "   dirs-changed:  directories in which things were changed\n"
     "   changed:       full change summary: all dirs & files changed\n"
     "   diff:          GNU diffs of changed files, prop diffs too\n"
     "\n",
     progname,
     progname,
     progname);

  exit (exit_code);
}



/*** Main. ***/

int
main (int argc, const char * const *argv)
{
  apr_pool_t *pool;
  const char *repos_path = NULL, *txn_name = NULL;
  int cmd_offset = 4;
  svnlook_cmd_t command;
  svnlook_ctxt_t c;

  /* Initialize context variable. */
  c.fs = NULL;
  c.rev_id = SVN_INVALID_REVNUM;
  c.is_revision = FALSE;
  c.txn = NULL;

  /* We require at least 1 arguments. */
  if (argc < 2)
    {
      usage (argv[0], 1);
      return EXIT_FAILURE;
    }

  /* Argument 1 is the repository path. */
  repos_path = argv[1];

  /* Argument 2 could be "rev" or "txn".  If "rev", Argument 3 is a
     numerical revision number.  If "txn", Argument 3 is a transaction
     name string.  If neither, this is an inspection of the youngest
     revision.  */
  if (argc > 3)
    {
      if (! strcmp (argv[2], "txn")) /* transaction */
        {
          c.is_revision = FALSE;
          txn_name = argv[3];
        }
      else if (! strcmp (argv[2], "rev")) /* revision */
        {
          c.is_revision = TRUE;
          c.rev_id = atoi (argv[3]);
          if (c.rev_id < 1)
            {
              usage (argv[0], 1);
              return EXIT_FAILURE;
            }
        }
      else
        {
          c.is_revision = TRUE;
          cmd_offset = 2;
        }
    }
  else
    {
      c.is_revision = TRUE;
      cmd_offset = 2;
    }

  /* If there is a subcommand, parse it. */
  if (argc > cmd_offset)
    {
      if (! strcmp (argv[cmd_offset], "log"))
        command = svnlook_cmd_log;
      else if (! strcmp (argv[cmd_offset], "author"))
        command = svnlook_cmd_author;
      else if (! strcmp (argv[cmd_offset], "date"))
        command = svnlook_cmd_date;
      else if (! strcmp (argv[cmd_offset], "dirs-changed"))
        command = svnlook_cmd_dirs_changed;
      else if (! strcmp (argv[cmd_offset], "changed"))
        command = svnlook_cmd_changed;
      else if (! strcmp (argv[cmd_offset], "diff"))
        command = svnlook_cmd_diff;
      else
        {
          usage (argv[0], 2);
          return EXIT_FAILURE;
        }
    }
  else
    {
      command = svnlook_cmd_default;
    }

  /* Now, let's begin processing.  */

  /* Initialize APR and our top-level pool. */
  apr_initialize ();
  pool = svn_pool_create (NULL);

  /* Allocate a new filesystem object. */
  c.fs = svn_fs_new (pool);

  /* Open the repository with the given path. */
  INT_ERR (svn_fs_open_berkeley (c.fs, repos_path));

  /* If this is a transaction, open the transaction. */
  if (! c.is_revision)
    INT_ERR (svn_fs_open_txn (&(c.txn), c.fs, txn_name, pool));
 
  /* If this is a revision with an invalid revision number, just use
     the head revision. */
  if (c.is_revision && (! SVN_IS_VALID_REVNUM (c.rev_id)))
    INT_ERR (svn_fs_youngest_rev (&(c.rev_id), c.fs, pool));

  /* Now, out context variable is full of all the stuff we might need
     to know.  Get to work.  */
  switch (command)
    {
    case svnlook_cmd_log:
      INT_ERR (do_log (&c, FALSE, pool));
      break;

    case svnlook_cmd_author:
      INT_ERR (do_author (&c, pool));
      break;

    case svnlook_cmd_date:
      INT_ERR (do_date (&c, pool));
      break;

    case svnlook_cmd_dirs_changed:
      INT_ERR (do_dirs_changed (&c, pool));
      break;

    case svnlook_cmd_changed:
      INT_ERR (do_changed (&c, pool));
      break;

    case svnlook_cmd_diff:
      INT_ERR (do_diff (&c, pool));
      break;

    case svnlook_cmd_default:
    default:
      INT_ERR (do_default (&c, pool));
      break;
    }

  /* Cleanup after ourselves. */
  if (! c.is_revision)
    svn_fs_close_txn (c.txn);

  svn_pool_destroy (pool);
  apr_terminate ();

  return EXIT_SUCCESS;
}




/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
