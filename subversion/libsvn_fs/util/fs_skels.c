/* fs_skels.c --- conversion between fs native types and skeletons
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
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

#include <string.h>
#include "svn_error.h"
#include "svn_string.h"
#include "fs_skels.h"
#include "skel.h"
#include "../id.h"
#include "../validate.h"


static svn_error_t *
skel_err (const char *skel_type,
          apr_pool_t *pool)
{
  return svn_error_createf (SVN_ERR_FS_MALFORMED_SKEL, 0, NULL, pool, 
                            "Malformed%s%s skeleton", 
                            skel_type ? " " : "",
                            skel_type ? skel_type : "");
}



/*** Validity Checking ***/

static int 
is_valid_proplist_skel (skel_t *skel)
{
  int len = svn_fs__list_length (skel);

  if ((len >= 0) && (len & 1) == 0)
    {
      skel_t *elt;

      for (elt = skel->children; elt; elt = elt->next)
        if (! elt->is_atom)
          return 0;

      return 1;
    }

  return 0;
}


static int
is_valid_revision_skel (skel_t *skel)
{
  int len = svn_fs__list_length (skel);

  if ((len == 4)
      && svn_fs__matches_atom (skel->children, "revision")
      && skel->children->next->is_atom
      && skel->children->next->next->is_atom
      && is_valid_proplist_skel (skel->children->next->next->next))
    return 1;

  return 0;
}


static int
is_valid_transaction_skel (skel_t *skel)
{
  int len = svn_fs__list_length (skel);

  if (len == 5
      && svn_fs__matches_atom (skel->children, "transaction")
      && skel->children->next->is_atom
      && skel->children->next->next->is_atom
      && is_valid_proplist_skel (skel->children->next->next->next)
      && (! skel->children->next->next->next->next->is_atom))
    return 1;

  if (len == 2
      && svn_fs__matches_atom (skel->children, "committed")
      && skel->children->next->is_atom)
    return 1;

  return 0;
}


static int 
is_valid_representation_skel (skel_t *skel)
{
  int len = svn_fs__list_length (skel);

  if ((len >= 2)
      && (! skel->children->is_atom)
      && (svn_fs__list_length (skel->children) == 2)
      && (skel->children->children->next->is_atom))
    {
      if (svn_fs__matches_atom (skel->children->children, "fulltext"))
        {
          return 1;
        }
      if (svn_fs__matches_atom (skel->children->children, "delta"))
        {
          return 1;
        }
    }

  return 0;
}


static int
is_valid_node_revision_header_skel (skel_t *skel, skel_t **kind_p)
{
  int len = svn_fs__list_length (skel);

  if (len < 1)
    return 0;

  /* set the *KIND_P pointer. */
  *kind_p = skel->children;

  /* without predecessor... */
  if ((len == 1)
      && skel->children->is_atom)
    return 1;

  /* or with predecessor... */
  if ((len == 2)
      && skel->children->is_atom
      && skel->children->next->is_atom)
    return 1;

  return 0;
}


static int
is_valid_node_revision_skel (skel_t *skel)
{
  int len = svn_fs__list_length (skel);

  if (len >= 1)
    {
      skel_t *header = skel->children;
      skel_t *kind;

      if (is_valid_node_revision_header_skel (header, &kind))
        {
          if (svn_fs__matches_atom (kind, "dir")
              && len == 3
              && header->next->is_atom
              && header->next->next->is_atom)
            return 1;
          
          if (svn_fs__matches_atom (kind, "file")
              && ((len == 3) || (len == 4))
              && header->next->is_atom
              && header->next->next->is_atom)
            {
              if ((len == 4) && (! header->next->next->next->is_atom))
                return 0;
              return 1;
            }
        }
    }

  return 0;
}


static int
is_valid_copy_skel (skel_t *skel)
{
  return ((svn_fs__list_length (skel) == 4)
          && svn_fs__matches_atom (skel->children, "copy")
          && skel->children->next->is_atom
          && skel->children->next->next->is_atom
          && skel->children->next->next->next->is_atom);
}



/*** Parsing (conversion from skeleton to native FS type) ***/

svn_error_t *
svn_fs__parse_proplist_skel (apr_hash_t **proplist_p,
                             skel_t *skel,
                             apr_pool_t *pool)
{
  apr_hash_t *proplist = NULL;
  skel_t *elt;

  /* Validate the skel. */
  if (! is_valid_proplist_skel (skel))
    return skel_err ("proplist", pool);
  
  /* Create the returned structure */
  if (skel->children)
    proplist = apr_hash_make (pool);
  for (elt = skel->children; elt; elt = elt->next->next)
    {
      svn_string_t *value = svn_string_ncreate (elt->next->data, 
                                                elt->next->len, pool);
      apr_hash_set (proplist, 
                    apr_pstrmemdup (pool, elt->data, elt->len), 
                    elt->len,
                    (void *)value);
    }

  /* Return the structure. */
  *proplist_p = proplist;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__parse_revision_skel (svn_fs__revision_t **revision_p, 
                             skel_t *skel,
                             apr_pool_t *pool)
{
  svn_fs__revision_t *revision;
  skel_t *id, *txn, *proplist;

  /* Validate the skel. */
  if (! is_valid_revision_skel (skel))
    return skel_err ("revision", pool);
  id = skel->children->next;
  txn = skel->children->next->next;
  proplist = skel->children->next->next->next;

  /* Create the returned structure */
  revision = apr_pcalloc (pool, sizeof (*revision));
  revision->id = svn_fs_parse_id (id->data, id->len, pool);
  revision->txn = apr_pstrmemdup (pool, txn->data, txn->len);
  SVN_ERR (svn_fs__parse_proplist_skel (&(revision->proplist), 
                                        proplist, pool));
  
  /* Return the structure. */
  *revision_p = revision;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__parse_transaction_skel (svn_fs__transaction_t **transaction_p, 
                                skel_t *skel,
                                apr_pool_t *pool)
{
  svn_fs__transaction_t *transaction;
  
  /* Validate the skel. */
  if (! is_valid_transaction_skel (skel))
    return skel_err ("transaction", pool);

  /* Create the returned structure */
  transaction = apr_pcalloc (pool, sizeof (*transaction));

  /* Handle committed transactions */
  if (svn_fs__matches_atom (skel->children, "committed"))
    {
      /* REV */
      transaction->revision = atoi (apr_pstrmemdup 
                                    (pool, skel->children->next->data,
                                     skel->children->next->len));
      if (! SVN_IS_VALID_REVNUM (transaction->revision))
        return skel_err ("tranaction", pool);
    }
  /* Handle unfinished transaction */
  else
    {
      skel_t *root_id, *base_id, *proplist, *copies;
      int len;

      root_id = skel->children->next;
      base_id = skel->children->next->next;
      proplist = skel->children->next->next->next;
      copies = skel->children->next->next->next->next;

      transaction->revision = SVN_INVALID_REVNUM;

      /* ROOT-ID */
      transaction->root_id = svn_fs_parse_id (root_id->data, 
                                              root_id->len, pool);

      /* BASE-ID */
      transaction->base_id = svn_fs_parse_id (base_id->data,
                                              base_id->len, pool);

      /* PROPLIST */
      SVN_ERR (svn_fs__parse_proplist_skel (&(transaction->proplist), 
                                            proplist, pool));
      
      /* COPIES */
      if ((len = svn_fs__list_length (copies)))
        {
          const char *copy_id;
          apr_array_header_t *txncopies;
          skel_t *cpy = copies->children;

          txncopies = apr_array_make (pool, len, sizeof (copy_id));
          while (cpy)
            {
              copy_id = apr_pstrmemdup (pool, cpy->data, cpy->len);
              (*((const char **)(apr_array_push (txncopies)))) = copy_id;
              cpy = cpy->next;
            }
          transaction->copies = txncopies;
        }
    }

  /* Return the structure. */
  *transaction_p = transaction;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__parse_representation_skel (svn_fs__representation_t **rep_p,
                                   skel_t *skel,
                                   apr_pool_t *pool)
{
  svn_fs__representation_t *rep;
  skel_t *header_skel;

  /* Validate the skel. */
  if (! is_valid_representation_skel (skel))
    return skel_err ("representation", pool);
  header_skel = skel->children;

  /* Create the returned structure */
  rep = apr_pcalloc (pool, sizeof (*rep));
  
  /* KIND */
  if (svn_fs__matches_atom (header_skel->children, "fulltext"))
    rep->kind = svn_fs__rep_kind_fulltext;
  else
    rep->kind = svn_fs__rep_kind_delta;

  /* TXN */
  rep->txn_id = apr_pstrmemdup (pool, header_skel->children->next->data,
                                header_skel->children->next->len);
  
  /* KIND-SPECIFIC stuff */
  if (rep->kind == svn_fs__rep_kind_fulltext)
    {
      /* "fulltext"-specific. */
      rep->contents.fulltext.string_key 
        = apr_pstrmemdup (pool, 
                          skel->children->next->data,
                          skel->children->next->len);
    }
  else
    {
      /* "delta"-specific. */
      skel_t *chunk_skel = skel->children->next;
      svn_fs__rep_delta_chunk_t *chunk;
      apr_array_header_t *chunks;
      
      /* Alloc the chunk array. */
      chunks = apr_array_make (pool, svn_fs__list_length (skel) - 1, 
                               sizeof (chunk));

      /* Process the chunks. */
      while (chunk_skel)
        {
          skel_t *window_skel = chunk_skel->children->next;
          skel_t *diff_skel = window_skel->children;
          skel_t *checksum_skel = window_skel->children->next->next;

          /* Allocate a chunk and its window */
          chunk = apr_palloc (pool, sizeof (*chunk));

          /* Populate the window */
          chunk->string_key = apr_pstrmemdup (pool,
                                              diff_skel->children->next->data,
                                              diff_skel->children->next->len);
          chunk->size = atoi (apr_pstrmemdup (pool,
                                              diff_skel->next->data,
                                              diff_skel->next->len));
          memcpy (&(chunk->checksum), checksum_skel->children->data, 
                  MD5_DIGESTSIZE);
          chunk->rep_key = apr_pstrmemdup (pool, 
                                           checksum_skel->next->data,
                                           checksum_skel->next->len);

          /* Add this chunk to the array. */
          chunk->offset = atoi (apr_pstrmemdup (pool, 
                                                chunk_skel->children->data,
                                                chunk_skel->children->len));
          (*((svn_fs__rep_delta_chunk_t **)(apr_array_push (chunks)))) = chunk;

          /* Next... */
          chunk_skel = chunk_skel->next;
        }

      /* Add the chunks array to the representation. */
      rep->contents.delta.chunks = chunks;
    }

  /* Return the structure. */
  *rep_p = rep;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__parse_node_revision_skel (svn_fs__node_revision_t **noderev_p,
                                  skel_t *skel,
                                  apr_pool_t *pool)
{
  svn_fs__node_revision_t *noderev;
  skel_t *header_skel;

  /* Validate the skel. */
  if (! is_valid_node_revision_skel (skel))
    return skel_err ("node-revision", pool);
  header_skel = skel->children;

  /* Create the returned structure */
  noderev = apr_pcalloc (pool, sizeof (*noderev));
  
  /* KIND */
  if (svn_fs__matches_atom (header_skel->children, "dir"))
    noderev->kind = svn_node_dir;
  else
    noderev->kind = svn_node_file;

  /* PREDECESSOR-ID */
  if (header_skel->children->next)
    {
      noderev->predecessor_id 
        = svn_fs_parse_id (header_skel->children->next->data,
                           header_skel->children->next->len, pool);
    }
      
  /* PROP-KEY */
  if (skel->children->next->len)
    noderev->prop_key = apr_pstrmemdup (pool, skel->children->next->data,
                                        skel->children->next->len);

  /* DATA-KEY */
  if (skel->children->next->next->len)
    noderev->data_key = apr_pstrmemdup (pool, skel->children->next->next->data,
                                        skel->children->next->next->len);

  /* EDIT-DATA-KEY (optional, files only) */
  if ((noderev->kind == svn_node_file) 
      && skel->children->next->next->next
      && skel->children->next->next->next->len)
    noderev->edit_key 
      = apr_pstrmemdup (pool, skel->children->next->next->next->data,
                        skel->children->next->next->next->len);

  /* Return the structure. */
  *noderev_p = noderev;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__parse_copy_skel (svn_fs__copy_t **copy_p,
                         skel_t *skel,
                         apr_pool_t *pool)
{
  svn_fs__copy_t *copy;

  /* Validate the skel. */
  if (! is_valid_copy_skel (skel))
    return skel_err ("copy", pool);

  /* Create the returned structure */
  copy = apr_pcalloc (pool, sizeof (*copy));

  /* SRC-PATH */
  copy->src_path = apr_pstrmemdup (pool,
                                   skel->children->next->data,
                                   skel->children->next->len);

  /* SRC-REV */
  copy->src_revision = atoi (apr_pstrmemdup (pool,
                                             skel->children->next->next->data,
                                             skel->children->next->next->len));

  /* DST-NODE-ID */
  copy->dst_noderev_id 
    = svn_fs_parse_id (skel->children->next->next->next->data,
                       skel->children->next->next->next->len, pool);

  /* Return the structure. */
  *copy_p = copy;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__parse_entries_skel (apr_hash_t **entries_p,
                            skel_t *skel,
                            apr_pool_t *pool)
{
  apr_hash_t *entries = NULL;
  int len = svn_fs__list_length (skel);
  skel_t *elt;

  if (! (len >= 0))
    return skel_err ("entries", pool);
    
  if (len > 0)
    {
      /* Else, allocate a hash and populate it. */
      entries = apr_hash_make (pool);
      
      /* Check entries are well-formed as we go along. */
      for (elt = skel->children; elt; elt = elt->next)
        {
          const char *name;
          svn_fs_id_t *id;

          /* ENTRY must be a list of two elements. */
          if (svn_fs__list_length (elt) != 2)
            return skel_err ("entries", pool);

          /* Get the entry's name and ID. */
          name = apr_pstrmemdup (pool, elt->children->data, 
                                 elt->children->len);
          id = svn_fs_parse_id (elt->children->next->data, 
                                elt->children->next->len, pool);

          /* Add the entry to the hash. */
          apr_hash_set (entries, name, elt->children->len, (void *) id);
        }
    }

  /* Return the structure. */
  *entries_p = entries;
  return SVN_NO_ERROR;
}




/*** Unparsing (conversion from native FS type to skeleton) ***/

svn_error_t *
svn_fs__unparse_proplist_skel (skel_t **skel_p,
                               apr_hash_t *proplist,
                               apr_pool_t *pool)
{
  skel_t *skel = svn_fs__make_empty_list (pool);
  apr_hash_index_t *hi;

  /* Create the skel. */
  if (proplist)
    {
      /* Loop over hash entries */
      for (hi = apr_hash_first (pool, proplist); hi; hi = apr_hash_next (hi))
        {
          const void *key;
          void *val;
          apr_ssize_t klen;
          svn_string_t *value;
          
          apr_hash_this (hi, &key, &klen, &val);
          value = val;
          
          /* VALUE */
          svn_fs__prepend (svn_fs__mem_atom (value->data, value->len, pool), 
                           skel);
          
          /* NAME */
          svn_fs__prepend (svn_fs__mem_atom (key, klen, pool), skel);
        }
    }
     
  /* Validate and return the skel. */
  if (! is_valid_proplist_skel (skel))
    return skel_err ("proplist", pool);
  *skel_p = skel;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__unparse_revision_skel (skel_t **skel_p,
                               const svn_fs__revision_t *revision,
                               apr_pool_t *pool)
{
  skel_t *skel;
  skel_t *proplist_skel;
  svn_string_t *id_str;

  /* Create the skel. */
  skel = svn_fs__make_empty_list (pool);

  /* PROPLIST */
  svn_fs__unparse_proplist_skel (&proplist_skel, revision->proplist, pool);
  svn_fs__prepend (proplist_skel, skel);

  /* TXN */
  svn_fs__prepend (svn_fs__str_atom (revision->txn, pool), skel);

  /* ID */
  id_str = svn_fs_unparse_id (revision->id, pool);
  svn_fs__prepend (svn_fs__mem_atom (id_str->data, id_str->len, pool), skel);

  /* "revision" */
  svn_fs__prepend (svn_fs__str_atom ("revision", pool), skel);

  /* Validate and return the skel. */
  if (! is_valid_revision_skel (skel))
    return skel_err ("revision", pool);
  *skel_p = skel;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__unparse_transaction_skel (skel_t **skel_p,
                                  const svn_fs__transaction_t *transaction,
                                  apr_pool_t *pool)
{
  skel_t *skel;

  /* Create the skel. */
  skel = svn_fs__make_empty_list (pool);

  /* Handle committed transactions. */
  if (SVN_IS_VALID_REVNUM (transaction->revision))
    {
      svn_stringbuf_t *rev_str;
      
      /* REV */
      rev_str = svn_stringbuf_createf (pool, "%" SVN_REVNUM_T_FMT,
                                       transaction->revision);
      svn_fs__prepend (svn_fs__mem_atom (rev_str->data, rev_str->len, pool), 
                       skel);
      
      /* "committed" */
      svn_fs__prepend (svn_fs__str_atom ("committed", pool), skel);
    }
  /* Handle unfinished transactions. */
  else
    {
      skel_t *proplist_skel, *copies_skel;
      svn_string_t *id_str;

      /* COPIES */
      copies_skel = svn_fs__make_empty_list (pool);
      if (transaction->copies && transaction->copies->nelts) 
        {
          int i;
          for (i = transaction->copies->nelts - 1; i > 0; i--)
            {
              const char *copy_id = APR_ARRAY_IDX (transaction->copies, i, 
                                                   const char *);
              
              svn_fs__prepend (svn_fs__str_atom (copy_id, pool), copies_skel);
            }
        }
      svn_fs__prepend (copies_skel, skel);

      /* PROPLIST */
      svn_fs__unparse_proplist_skel (&proplist_skel, 
                                     transaction->proplist, pool);
      svn_fs__prepend (proplist_skel, skel);

      /* BASE-ID */
      id_str = svn_fs_unparse_id (transaction->base_id, pool);
      svn_fs__prepend (svn_fs__mem_atom (id_str->data, id_str->len, pool), 
                       skel);

      /* ROOT-ID */
      id_str = svn_fs_unparse_id (transaction->root_id, pool);
      svn_fs__prepend (svn_fs__mem_atom (id_str->data, id_str->len, pool), 
                       skel);

      /* "transaction" */
      svn_fs__prepend (svn_fs__str_atom ("transaction", pool), skel);
    }

  /* Validate and return the skel. */
  if (! is_valid_transaction_skel (skel))
    return skel_err ("transaction", pool);
  *skel_p = skel;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__unparse_representation_skel (skel_t **skel_p,
                                     const svn_fs__representation_t *rep,
                                     apr_pool_t *pool)
{
  skel_t *skel;
  skel_t *header_skel;

  /* Create the skel. */
  skel = svn_fs__make_empty_list (pool);

  if (rep->kind == svn_fs__rep_kind_fulltext)
    {
      /*** Fulltext Representation. ***/

      /* Create the header. */
      header_skel = svn_fs__make_empty_list (pool);
      
      /* STRING-KEY */
      if ((! rep->contents.fulltext.string_key) 
          || (! *rep->contents.fulltext.string_key))
        svn_fs__prepend (svn_fs__mem_atom (NULL, 0, pool), skel);
      else
        svn_fs__prepend (svn_fs__str_atom 
                         (rep->contents.fulltext.string_key, pool), skel);
      
      /* TXN */
      if (rep->txn_id)
        svn_fs__prepend (svn_fs__str_atom (rep->txn_id, pool), header_skel);
      else
        svn_fs__prepend (svn_fs__mem_atom (NULL, 0, pool), header_skel);

      /* "fulltext" */
      svn_fs__prepend (svn_fs__str_atom ("fulltext", pool), header_skel);

      /* header */
      svn_fs__prepend (header_skel, skel);
    }
  else if (rep->kind == svn_fs__rep_kind_delta)
    {
      /*** Delta Representation. ***/
      int i;
      apr_array_header_t *chunks = rep->contents.delta.chunks;

      /* Loop backwards through the windows, creating and prepending skels. */
      for (i = chunks->nelts; i > 0; i--)
        {
          skel_t *window_skel = svn_fs__make_empty_list (pool);
          skel_t *chunk_skel = svn_fs__make_empty_list (pool);
          skel_t *diff_skel = svn_fs__make_empty_list (pool);
          skel_t *checksum_skel = svn_fs__make_empty_list (pool);
          const char *size_str;
          const char *offset_str;
          svn_fs__rep_delta_chunk_t *chunk = 
            (((svn_fs__rep_delta_chunk_t **) chunks->elts)[i - 1]);

          /* OFFSET */
          offset_str = apr_psprintf (pool, "%" APR_SIZE_T_FMT,
                                     chunk->offset);

          /* SIZE */
          size_str = apr_psprintf (pool, "%" APR_SIZE_T_FMT, 
                                   chunk->size);

          /* DIFF */
          if ((! chunk->string_key) || (! *chunk->string_key))
            svn_fs__prepend (svn_fs__mem_atom (NULL, 0, pool), diff_skel);
          else
            svn_fs__prepend (svn_fs__str_atom (chunk->string_key,
                                               pool), diff_skel);
          svn_fs__prepend (svn_fs__str_atom ("svndiff", pool), diff_skel);
        
          /* CHECKSUM */
          svn_fs__prepend (svn_fs__mem_atom (chunk->checksum,
                                             MD5_DIGESTSIZE / 
                                             sizeof (*(chunk->checksum)), 
                                             pool), checksum_skel);
          svn_fs__prepend (svn_fs__str_atom ("md5", pool), checksum_skel);
          
          /* REP-KEY */
          if ((! chunk->rep_key) || (! *(chunk->rep_key)))
            svn_fs__prepend (svn_fs__mem_atom (NULL, 0, pool), window_skel);
          else
            svn_fs__prepend (svn_fs__str_atom (chunk->rep_key, pool), 
                             window_skel);
          svn_fs__prepend (checksum_skel, window_skel);
          svn_fs__prepend (svn_fs__str_atom (size_str, pool), window_skel);
          svn_fs__prepend (diff_skel, window_skel);
          
          /* window header. */
          svn_fs__prepend (window_skel, chunk_skel);
          svn_fs__prepend (svn_fs__str_atom (offset_str, pool), chunk_skel);
          
          /* Add this window item to the main skel. */
          svn_fs__prepend (chunk_skel, skel);
        }
      
      /* Create the header. */
      header_skel = svn_fs__make_empty_list (pool);
      
      /* TXN */
      if (rep->txn_id)
        svn_fs__prepend (svn_fs__str_atom (rep->txn_id, pool), header_skel);
      else
        svn_fs__prepend (svn_fs__mem_atom (NULL, 0, pool), header_skel);
      
      /* "delta" */
      svn_fs__prepend (svn_fs__str_atom ("delta", pool), header_skel);

      /* header */
      svn_fs__prepend (header_skel, skel);
    }
  else /* unknown kind */
    abort();

  /* Validate and return the skel. */
  if (! is_valid_representation_skel (skel))
    return skel_err ("representation", pool);
  *skel_p = skel;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__unparse_node_revision_skel (skel_t **skel_p,
                                    const svn_fs__node_revision_t *noderev,
                                    apr_pool_t *pool)
{
  skel_t *skel;
  skel_t *header_skel;

  /* Create the skel. */
  skel = svn_fs__make_empty_list (pool);
  header_skel = svn_fs__make_empty_list (pool);

  /* PREDECESSOR-ID */
  if (noderev->predecessor_id)
    {
      svn_string_t *id_str = svn_fs_unparse_id (noderev->predecessor_id, pool);
      svn_fs__prepend (svn_fs__mem_atom (id_str->data, id_str->len, pool), 
                       header_skel);
    }
  else
    svn_fs__prepend (svn_fs__mem_atom (NULL, 0, pool), header_skel);

  /* KIND */
  if (noderev->kind == svn_node_file)
    svn_fs__prepend (svn_fs__str_atom ("file", pool), header_skel);
  else if (noderev->kind == svn_node_dir)
    svn_fs__prepend (svn_fs__str_atom ("dir", pool), header_skel);
  else
    abort ();

  /* ### do we really need to check *node->FOO_key ? if a key doesn't
     ### exist, then the field should be NULL ...  */

  /* EDIT-DATA-KEY (optional) */
  if ((noderev->edit_key) && (*noderev->edit_key))
    svn_fs__prepend (svn_fs__str_atom (noderev->edit_key, pool), skel);

  /* DATA-KEY */
  if ((noderev->data_key) && (*noderev->data_key))
    svn_fs__prepend (svn_fs__str_atom (noderev->data_key, pool), skel);
  else
    svn_fs__prepend (svn_fs__mem_atom (NULL, 0, pool), skel);
  
  /* PROP-KEY */
  if ((noderev->prop_key) && (*noderev->prop_key))
    svn_fs__prepend (svn_fs__str_atom (noderev->prop_key, pool), skel);
  else
    svn_fs__prepend (svn_fs__mem_atom (NULL, 0, pool), skel);

  /* HEADER */
  svn_fs__prepend (header_skel, skel);

  /* Validate and return the skel. */
  if (! is_valid_node_revision_skel (skel))
    return skel_err ("node-revision", pool);
  *skel_p = skel;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__unparse_copy_skel (skel_t **skel_p,
                           const svn_fs__copy_t *copy,
                           apr_pool_t *pool)
{
  skel_t *skel;
  svn_string_t *tmp_str;

  /* Create the skel. */
  skel = svn_fs__make_empty_list (pool);

  /* DST-NODE-ID */
  tmp_str = svn_fs_unparse_id (copy->dst_noderev_id, pool);
  svn_fs__prepend (svn_fs__mem_atom (tmp_str->data, tmp_str->len, pool), skel);

  /* SRC-REV */
  tmp_str = svn_string_createf (pool, "%" SVN_REVNUM_T_FMT, 
                                copy->src_revision);
  svn_fs__prepend (svn_fs__mem_atom (tmp_str->data, tmp_str->len, pool), skel);

  /* SRC-PATH */
  if ((copy->src_path) && (*copy->src_path))
    svn_fs__prepend (svn_fs__str_atom (copy->src_path, pool), skel);
  else
    svn_fs__prepend (svn_fs__mem_atom (NULL, 0, pool), skel);

  /* "copy" */
  svn_fs__prepend (svn_fs__str_atom ("copy", pool), skel);

  /* Validate and return the skel. */
  if (! is_valid_copy_skel (skel))
    return skel_err ("copy", pool);
  *skel_p = skel;
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__unparse_entries_skel (skel_t **skel_p,
                              apr_hash_t *entries,
                              apr_pool_t *pool)
{
  skel_t *skel = svn_fs__make_empty_list (pool);
  apr_hash_index_t *hi;

  /* Create the skel. */
  if (entries)
    {
      /* Loop over hash entries */
      for (hi = apr_hash_first (pool, entries); hi; hi = apr_hash_next (hi))
        {
          const void *key;
          void *val;
          apr_ssize_t klen;
          svn_fs_id_t *value;
          svn_string_t *id_str;
          skel_t *entry_skel = svn_fs__make_empty_list (pool);

          apr_hash_this (hi, &key, &klen, &val);
          value = val;
          
          /* VALUE */
          id_str = svn_fs_unparse_id (value, pool);
          svn_fs__prepend (svn_fs__mem_atom (id_str->data, id_str->len, pool), 
                           entry_skel);
          
          /* NAME */
          svn_fs__prepend (svn_fs__mem_atom (key, klen, pool), entry_skel);

          /* Add entry to the entries skel. */
          svn_fs__prepend (entry_skel, skel);
        }
    }
     
  /* Return the skel. */
  *skel_p = skel;
  return SVN_NO_ERROR;
}


/* 
 * local variables:
 * eval: (load-file "../../../tools/dev/svn-dev.el")
 * end:
 */
