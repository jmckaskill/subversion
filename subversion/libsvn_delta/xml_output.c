/*
 * xml_output.c:  output a Subversion "tree-delta" XML stream
 *
 * ================================================================
 * Copyright (c) 2000 CollabNet.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. The end-user documentation included with the redistribution, if
 * any, must include the following acknowlegement: "This product includes
 * software developed by CollabNet (http://www.Collab.Net)."
 * Alternately, this acknowlegement may appear in the software itself, if
 * and wherever such third-party acknowlegements normally appear.
 * 
 * 4. The hosted project names must not be used to endorse or promote
 * products derived from this software without prior written
 * permission. For written permission, please contact info@collab.net.
 * 
 * 5. Products derived from this software may not use the "Tigris" name
 * nor may "Tigris" appear in their names without prior written
 * permission of CollabNet.
 * 
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL COLLABNET OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by many
 * individuals on behalf of CollabNet.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_delta.h"
#include "svn_io.h"
#include "svn_xml.h"
#include "svn_base64.h"
#include "apr_pools.h"


/* TODO:

        - Produce real vcdiff data when we have Branko's text delta ->
          vcdiff routines.
	- Do consistency checking on the order of calls, maybe.
	  (Right now we'll just spit out invalid output if the calls
	  come in an incorrect order.)
	- Indentation?  Not really a priority.
*/




/* The types of some of the elements we output.  The actual range of
   valid values is always narrower than the full set, but they
   overlap, so it doesn't quite make sense to have a separate
   enueration for each use.  */
enum elemtype {
  elem_delta_pkg,
  elem_add,
  elem_replace,
  elem_dir,
  elem_dir_prop_delta,
  elem_tree_delta,
  elem_file,
  elem_file_prop_delta
};

struct edit_baton
{
  svn_write_fn_t *output;
  void *output_baton;
  enum elemtype elem;           /* Current element we are inside at
                                   the end of a call.  One of
                                   elem_dir, elem_dir_prop_delta,
                                   elem_tree_delta, elem_file, or
                                   elem_file_prop_delta.  */
  struct file_baton *curfile;
  apr_pool_t *pool;
  int txdelta_id_counter;
};


struct dir_baton
{
  struct edit_baton *edit_baton;
  enum elemtype addreplace;     /* elem_add or elem_replace, or
                                   elem_delta_pkg for the root
                                   directory.  */
  apr_pool_t *pool;
};


struct file_baton
{
  struct edit_baton *edit_baton;
  enum elemtype addreplace;
  int txdelta_id;               /* ID of deferred text delta;
                                   0 means we're still working on the file,
                                   -1 means we already saw a text delta.  */
  int closed;			/* 1 if we closed the element already.  */
  apr_pool_t *pool;
};


static struct dir_baton *
make_dir_baton (struct edit_baton *eb, enum elemtype addreplace)
{
  apr_pool_t *subpool = svn_pool_create (eb->pool);
  struct dir_baton *db = apr_palloc (subpool, sizeof (*db));

  db->edit_baton = eb;
  db->addreplace = addreplace;
  db->pool = subpool;
  return db;
}


static struct file_baton *
make_file_baton (struct edit_baton *eb, enum elemtype addreplace)
{
  apr_pool_t *subpool = svn_pool_create (eb->pool);
  struct file_baton *fb = apr_palloc (subpool, sizeof (*fb));

  fb->edit_baton = eb;
  fb->addreplace = addreplace;
  fb->txdelta_id = 0;
  fb->closed = 0;
  fb->pool = subpool;
  return fb;
}


/* The meshing between the edit_fns interface and the XML delta format
   is such that we can't usually output the end of an element until we
   go on to the next thing, and for a given call we may or may not
   have already output the beginning of the element we're working on.
   This function takes care of "unwinding" and "winding" from the
   current element to the kind of element we need to work on next.  We
   never have to unwind past a dir element, so the unwinding steps are
   bounded in number and easy to visualize.  The nesting of the
   elements we care about looks like:
  
        dir -> prop_delta
            -> tree_delta -> add/replace -> file -> prop_delta

   We cannot be in an add/replace element at the end of a call, so
   add/replace and file are treated as a unit by this function.  Note
   that although there is no replace or dir element corresponding to
   the root directory (the root directory's tree-delta and/or
   prop-delta elements live directly inside the delta-pkg element), we
   pretend that there is for the sake of regularity.

   This function will "unwind" arbitrarily within that little tree,
   but will only "wind" from dir to tree_delta or prop_delta or from
   file to prop_delta.  Winding through add/replace/file would require
   extra information.

   ELEM specifies the element type we want to get to, with prop_delta
   split out into elem_dir_prop_delta and elem_file_prop_delta
   depending on where the prop_delta is in the little tree.  The
   element type we are currently in is recorded inside EB.  */

static svn_string_t *
get_to_elem (struct edit_baton *eb, enum elemtype elem, apr_pool_t *pool)
{
  svn_string_t *str = svn_string_create ("", pool);
  struct file_baton *fb;

  /* Unwind.  Start from the leaves and go back as far as necessary.  */
  if (eb->elem == elem_file_prop_delta && elem != elem_file_prop_delta)
    {
      svn_xml_make_close_tag (&str, pool, "prop-delta");
      eb->elem = elem_file;
    }
  if (eb->elem == elem_file && elem != elem_file
      && elem != elem_file_prop_delta)
    {
      const char *outertag;

      fb = eb->curfile;
      if (fb->txdelta_id == 0)
        {
          char buf[128];
          svn_string_t *idstr;

          /* Leak a little memory from pool to create idstr; all of our
             callers are using temporary pools anyway.  */
          fb->txdelta_id = eb->txdelta_id_counter++;
          sprintf (buf, "%d", fb->txdelta_id);
          idstr = svn_string_create (buf, pool);
          svn_xml_make_open_tag (&str, pool, svn_xml_self_closing,
                                 "text-delta-ref", "id", idstr, NULL);
        }
      svn_xml_make_close_tag (&str, pool, "file");
      outertag = (fb->addreplace == elem_add) ? "add" : "replace";
      svn_xml_make_close_tag (&str, pool, outertag);
      fb->closed = 1;
      eb->curfile = NULL;
      eb->elem = elem_tree_delta;
    }
  if (eb->elem == elem_tree_delta
      && (elem == elem_dir || elem == elem_dir_prop_delta))
    {
      svn_xml_make_close_tag (&str, pool, "tree-delta");
      eb->elem = elem_dir;
    }
  if (eb->elem == elem_dir_prop_delta && elem != elem_dir_prop_delta)
    {
      svn_xml_make_close_tag (&str, pool, "prop-delta");
      eb->elem = elem_dir;
    }

  /* Now wind.  */
  if (eb->elem == elem_dir && elem == elem_tree_delta)
    {
      svn_xml_make_open_tag (&str, pool, svn_xml_normal, "tree-delta", NULL);
      eb->elem = elem_tree_delta;
    }
  if ((eb->elem == elem_dir && elem == elem_dir_prop_delta)
      || (eb->elem == elem_file && elem == elem_file_prop_delta))
    {
      svn_xml_make_open_tag (&str, pool, svn_xml_normal, "prop-delta", NULL);
      eb->elem = elem;
    }

  /* If we didn't make it to the type of element the caller asked for,
     either the caller wants us to do something we don't do or we have
     a bug. */
  assert (eb->elem == elem);

  return str;
}


/* Output XML for adding or replacing a file or directory.  Also set
   EB->elem to the value of DIRFILE for consistency.  */
static svn_error_t *
output_addreplace (struct edit_baton *eb, enum elemtype addreplace,
                   enum elemtype dirfile, svn_string_t *name,
                   svn_string_t *ancestor_path, svn_vernum_t ancestor_version)
{
  svn_string_t *str;
  apr_pool_t *pool = svn_pool_create (eb->pool);
  svn_error_t *err;
  apr_size_t len;
  apr_hash_t *att;
  const char *outertag = (addreplace == elem_add) ? "add" : "replace";
  const char *innertag = (dirfile == elem_dir) ? "dir" : "file";

  str = get_to_elem (eb, elem_tree_delta, pool);
  svn_xml_make_open_tag (&str, pool, svn_xml_normal, outertag,
                         "name", name, NULL);

  att = apr_make_hash (pool);
  if (ancestor_path != NULL)
    {
      char buf[128];
      apr_hash_set (att, "ancestor", strlen("ancestor"), ancestor_path);
      sprintf (buf, "%lu", (unsigned long) ancestor_version);
      apr_hash_set (att, "ver", strlen("ver"), svn_string_create (buf, pool));
    }
  svn_xml_make_open_tag_hash (&str, pool, svn_xml_normal, innertag, att);

  eb->elem = dirfile;

  len = str->len;
  err = eb->output (eb->output_baton, str->data, &len, pool);
  apr_destroy_pool (pool);
  return err;
}


/* Output a set or delete element.  ELEM is the type of prop-delta
   (elem_dir_prop_delta or elem_file_prop_delta) the element lives
   in.  This function sets EB->elem to ELEM for consistency.  */
static svn_error_t *
output_propset (struct edit_baton *eb, enum elemtype elem,
                svn_string_t *name, svn_string_t *value)
{
  svn_string_t *str;
  apr_pool_t *pool = svn_pool_create (eb->pool);
  svn_error_t *err;
  apr_size_t len;

  str = get_to_elem (eb, elem, pool);
  if (value != NULL)
    {
      svn_xml_make_open_tag (&str, pool, svn_xml_protect_pcdata, "set",
                             "name", name, NULL);
      svn_xml_escape_string (&str, value, pool);
      svn_xml_make_close_tag (&str, pool, "set");
    }
  else
    svn_xml_make_open_tag (&str, pool, svn_xml_self_closing, "delete",
                           "name", name, NULL);

  len = str->len;
  err = eb->output (eb->output_baton, str->data, &len, eb->pool);
  apr_destroy_pool (pool);
  return err;
}


static svn_error_t *
replace_root (void *edit_baton,
              void **dir_baton)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  apr_pool_t *pool = svn_pool_create (eb->pool);
  svn_string_t *str = NULL;
  apr_size_t len;
  svn_error_t *err;

  svn_xml_make_header (&str, pool);
  svn_xml_make_open_tag (&str, pool, svn_xml_normal, "delta-pkg", NULL);

  *dir_baton = make_dir_baton (eb, elem_delta_pkg);
  eb->elem = elem_dir;

  len = str->len;
  err = eb->output (eb->output_baton, str->data, &len, eb->pool);
  apr_destroy_pool (pool);
  return err;
}


static svn_error_t *
delete (svn_string_t *name, void *parent_baton)
{
  struct dir_baton *db = (struct dir_baton *) parent_baton;
  struct edit_baton *eb = db->edit_baton;
  svn_string_t *str;
  apr_pool_t *pool = svn_pool_create (eb->pool);
  svn_error_t *err;
  apr_size_t len;

  str = get_to_elem (eb, elem_tree_delta, pool);
  svn_xml_make_open_tag (&str, pool, svn_xml_self_closing, "delete",
                         "name", name, NULL);

  len = str->len;
  err = eb->output (eb->output_baton, str->data, &len, eb->pool);
  apr_destroy_pool (pool);
  return err;
}


static svn_error_t *
add_directory (svn_string_t *name,
               void *parent_baton,
               svn_string_t *ancestor_path,
               svn_vernum_t ancestor_version,
               void **child_baton)
{
  struct dir_baton *db = (struct dir_baton *) parent_baton;
  struct edit_baton *eb = db->edit_baton;

  *child_baton = make_dir_baton (eb, elem_add);
  return output_addreplace (eb, elem_add, elem_dir, name,
                            ancestor_path, ancestor_version);
}


static svn_error_t *
replace_directory (svn_string_t *name,
                   void *parent_baton,
                   svn_string_t *ancestor_path,
                   svn_vernum_t ancestor_version,
                   void **child_baton)
{
  struct dir_baton *db = (struct dir_baton *) parent_baton;
  struct edit_baton *eb = db->edit_baton;

  *child_baton = make_dir_baton (eb, elem_replace);
  return output_addreplace (eb, elem_replace, elem_dir, name,
                            ancestor_path, ancestor_version);
}


static svn_error_t *
change_dir_prop (void *dir_baton,
                 svn_string_t *name,
                 svn_string_t *value)
{
  struct dir_baton *db = (struct dir_baton *) dir_baton;
  struct edit_baton *eb = db->edit_baton;

  return output_propset (eb, elem_dir_prop_delta, name, value);
}


static svn_error_t *
close_directory (void *dir_baton)
{
  struct dir_baton *db = (struct dir_baton *) dir_baton;
  struct edit_baton *eb = db->edit_baton;
  svn_string_t *str;
  svn_error_t *err;
  apr_size_t len;

  str = get_to_elem (eb, elem_dir, db->pool);
  if (db->addreplace != elem_delta_pkg)
    {
      /* Not the root directory.  */
      const char *outertag = (db->addreplace == elem_add) ? "add" : "replace";
      svn_xml_make_close_tag (&str, db->pool, "dir");
      svn_xml_make_close_tag (&str, db->pool, outertag);
      eb->elem = elem_tree_delta;
    }
  else
    eb->elem = elem_delta_pkg;

  len = str->len;
  err = eb->output (eb->output_baton, str->data, &len, db->pool);
  apr_destroy_pool (db->pool);
  return err;
}


static svn_error_t *
add_file (svn_string_t *name,
          void *parent_baton,
          svn_string_t *ancestor_path,
          svn_vernum_t ancestor_version,
          void **file_baton)
{
  struct dir_baton *db = (struct dir_baton *) parent_baton;
  struct edit_baton *eb = db->edit_baton;

  *file_baton = make_file_baton (eb, elem_add);
  eb->curfile = *file_baton;
  return output_addreplace (eb, elem_add, elem_file, name,
                            ancestor_path, ancestor_version);
}


static svn_error_t *
replace_file (svn_string_t *name,
              void *parent_baton,
              svn_string_t *ancestor_path,
              svn_vernum_t ancestor_version,
              void **file_baton)
{
  struct dir_baton *db = (struct dir_baton *) parent_baton;
  struct edit_baton *eb = db->edit_baton;

  *file_baton = make_file_baton (eb, elem_replace);
  eb->curfile = *file_baton;
  return output_addreplace (eb, elem_replace, elem_file, name,
                            ancestor_path, ancestor_version);
}


static svn_error_t *
output_svndiff_data (void *baton, const char *data, apr_size_t *len,
                     apr_pool_t *pool)
{
  struct file_baton *fb = (struct file_baton *) baton;
  struct edit_baton *eb = fb->edit_baton;
  apr_pool_t *subpool = svn_pool_create (eb->pool);
  svn_string_t *str = NULL;
  svn_error_t *err;
  apr_size_t slen;

  if (*len == 0)
    svn_xml_make_close_tag (&str, subpool, "text-delta");
  else
    str = svn_string_ncreate (data, *len, subpool);

  slen = str->len;
  err = eb->output (eb->output_baton, str->data, &slen, subpool);
  apr_destroy_pool (subpool);
  return err;
}


static svn_error_t *
apply_textdelta (void *file_baton, 
                 svn_txdelta_window_handler_t **handler,
                 void **handler_baton)
{
  struct file_baton *fb = (struct file_baton *) file_baton;
  struct edit_baton *eb = fb->edit_baton;
  svn_string_t *str = NULL;
  apr_pool_t *pool = svn_pool_create (eb->pool);
  svn_error_t *err;
  apr_size_t len;
  svn_write_fn_t *base64_encoder;
  void *base64_baton;

  if (fb->txdelta_id == 0)
    {
      /* We are inside a file element (possibly in a prop-delta) and
         are outputting a text-delta inline.  */
      str = get_to_elem (eb, elem_file, pool);
      svn_xml_make_open_tag (&str, pool, svn_xml_protect_pcdata,
                             "text-delta", NULL);
    }
  else
    {
      /* We should be at the end of the delta (after the root
         directory has been closed) and are outputting a deferred
         text-delta.  */
      char buf[128];
      sprintf(buf, "%d", fb->txdelta_id);
      svn_xml_make_open_tag (&str, pool, svn_xml_protect_pcdata, "text-delta",
                             "id", svn_string_create (buf, pool), NULL);
    }
  fb->txdelta_id = -1;

  len = str->len;
  err = eb->output (eb->output_baton, str->data, &len, pool);
  apr_destroy_pool (pool);

  svn_base64_encode (output_svndiff_data, fb, eb->pool,
                     &base64_encoder, &base64_baton);
  svn_txdelta_to_svndiff (base64_encoder, base64_baton, eb->pool,
                          handler, handler_baton);

  return err;
}


static svn_error_t *
change_file_prop (void *file_baton,
                  svn_string_t *name,
                  svn_string_t *value)
{
  struct file_baton *fb = (struct file_baton *) file_baton;
  struct edit_baton *eb = fb->edit_baton;

  return output_propset (eb, elem_file_prop_delta, name, value);
}


static svn_error_t *
close_file (void *file_baton)
{
  struct file_baton *fb = (struct file_baton *) file_baton;
  struct edit_baton *eb = fb->edit_baton;
  svn_string_t *str;
  svn_error_t *err = SVN_NO_ERROR;
  apr_size_t len;

  /* Close the file element if we are still working on it.  */
  if (!fb->closed)
    {
      const char *outertag = (fb->addreplace == elem_add) ? "add" : "replace";
      str = get_to_elem (eb, elem_file, fb->pool);
      svn_xml_make_close_tag (&str, fb->pool, "file");
      svn_xml_make_close_tag (&str, fb->pool, outertag);

      len = str->len;
      err = eb->output (eb->output_baton, str->data, &len, fb->pool);
      eb->curfile = NULL;
      eb->elem = elem_tree_delta;
    }
  apr_destroy_pool (fb->pool);
  return err;
}


static svn_error_t *
close_edit (void *edit_baton)
{
  struct edit_baton *eb = (struct edit_baton *) edit_baton;
  svn_error_t *err;
  svn_string_t *str = NULL;
  apr_size_t len;

  svn_xml_make_close_tag (&str, eb->pool, "delta-pkg");
  len = str->len;
  err = eb->output (eb->output_baton, str->data, &len, eb->pool);
  apr_destroy_pool (eb->pool);
  return err;
}


static const svn_delta_edit_fns_t tree_editor =
{
  replace_root,
  delete,
  add_directory,
  replace_directory,
  change_dir_prop,
  close_directory,
  add_file,
  replace_file,
  apply_textdelta,
  change_file_prop,
  close_file,
  close_edit
};


svn_error_t *
svn_delta_get_xml_editor (svn_write_fn_t *output,
			  void *output_baton,
			  const svn_delta_edit_fns_t **editor,
			  void **edit_baton,
			  apr_pool_t *pool)
{
  struct edit_baton *eb;
  apr_pool_t *subpool = svn_pool_create (pool);

  *editor = &tree_editor;
  eb = apr_palloc (subpool, sizeof (*eb));
  eb->pool = subpool;
  eb->output = output;
  eb->output_baton = output_baton;
  eb->curfile = NULL;
  eb->txdelta_id_counter = 1;

  *edit_baton = eb;

  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
