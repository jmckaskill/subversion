#include <stdlib.h>
#include <assert.h>

#include <svn_types.h>
#include <svn_pools.h>
#include <svn_path.h>
#include <svn_hash.h>
#include <svn_repos.h>

struct entry
{
  apr_hash_t *children;  /* NULL for files */
  apr_hash_t *props;
  apr_pool_t *props_pool;
  svn_revnum_t text_rev;
  apr_off_t text_off;
  apr_off_t text_len;    /* Serves for both the expanded and rep size */
  svn_revnum_t props_rev;
  apr_off_t props_off;
  apr_off_t props_len;   /* Serves for both the expanded and rep size */
  svn_revnum_t node_rev;
  apr_off_t node_off;
  int pred_count;
  struct entry *pred;
  int node_id;
  int copy_id;
  const char *created_path;
  svn_revnum_t copyfrom_rev;
  const char *copyfrom_path;
  struct entry *copyroot;
  svn_boolean_t soft_copy;
};

struct parse_baton
{
  apr_array_header_t *roots;
  struct entry *current_node;
  svn_revnum_t current_rev;
  apr_file_t *rev_file;
  svn_stream_t *rev_stream;
  int next_node_id;
  int next_copy_id;
  apr_pool_t *pool;
};

static struct entry *
new_entry(apr_pool_t *pool)
{
  struct entry *entry;

  entry = apr_palloc(pool, sizeof(*entry));
  entry->children = NULL;
  entry->props = NULL;
  entry->text_rev = SVN_INVALID_REVNUM;
  entry->text_off = -1;
  entry->text_len = -1;
  entry->props_rev = SVN_INVALID_REVNUM;
  entry->props_off = -1;
  entry->node_rev = SVN_INVALID_REVNUM;
  entry->node_off = -1;
  entry->pred_count = 0;
  entry->pred = NULL;
  entry->node_id = -1;
  entry->copy_id = -1;
  entry->created_path = NULL;
  entry->copyfrom_rev = SVN_INVALID_REVNUM;
  entry->copyfrom_path = NULL;
  entry->copyroot = NULL;
  entry->soft_copy = FALSE;
  return entry;
}

static struct entry *
get_root(struct parse_baton *pb, svn_revnum_t rev)
{
  return APR_ARRAY_IDX(pb->roots, rev, struct entry *);
}

/* Find the entry for PATH under the root ENTRY.  Do not create copies
   for the current rev; this is for looking up copy history. */
static struct entry *
find_entry(struct entry *entry, const char *path, apr_pool_t *pool)
{
  apr_array_header_t *elems;
  int i;
  const char *name;

  elems = svn_path_decompose(path, pool);
  for (i = 0; i < elems->nelts; i++)
    {
      name = APR_ARRAY_IDX(elems, i, const char *);
      assert(entry->children);
      entry = apr_hash_get(entry->children, name, APR_HASH_KEY_STRING);
      assert(entry);
    }
  return entry;
}

static void
copy_entry(struct parse_baton *pb, struct entry *new_entry,
           struct entry *old_entry, svn_boolean_t is_copy,
           svn_boolean_t soft_copy)
{
  *new_entry = *old_entry;
  if (new_entry->children)
    new_entry->children = apr_hash_copy(pb->pool, old_entry->children);
  new_entry->node_rev = pb->current_rev;
  new_entry->node_off = -1;
  new_entry->pred_count = old_entry->pred_count + 1;
  new_entry->pred = old_entry;
  if (is_copy)
    {
      new_entry->copy_id = pb->next_copy_id++;
      new_entry->copyfrom_rev = old_entry->node_rev;
      new_entry->copyfrom_path = old_entry->created_path;
      new_entry->soft_copy = soft_copy;
    }
  else
    {
      /* Make the new node-rev a change of the old one. */
      new_entry->copyfrom_rev = SVN_INVALID_REVNUM;
      new_entry->copyfrom_path = NULL;
      if (SVN_IS_VALID_REVNUM(old_entry->copyfrom_rev) || !old_entry->pred)
        new_entry->copyroot = old_entry;
    }
}

/* Get the child entry for NAME under ENTRY, copying it for the current
   rev if necessary.  Use POOL only for temporary allocations. */
static struct entry *
get_child(struct parse_baton *pb, struct entry *entry, const char *name,
          apr_pool_t *pool)
{
  struct entry *child, *new_child;
  const char *path;

  assert(entry->children);
  child = apr_hash_get(entry->children, name, APR_HASH_KEY_STRING);
  assert(child);
  if (child->node_rev != pb->current_rev)
    {
      path = svn_path_join(entry->created_path, name, pb->pool);
      /* Copy the child entry.  Create a "soft copy" if our created
         path does not match the old child entry's created path. */
      new_child = new_entry(pb->pool);
      copy_entry(pb, new_child, child,
                 (strcmp(path, child->created_path) != 0), TRUE);
      new_child->created_path = path;
      name = apr_pstrdup(pb->pool, name);
      apr_hash_set(entry->children, name, APR_HASH_KEY_STRING, new_child);
      child = new_child;
    }
  return child;
}

/* Get the entry for PATH in the current rev of PB.  Only use POOL
   only for temporary allocations. */
static struct entry *
follow_path(struct parse_baton *pb, const char *path, apr_pool_t *pool)
{
  apr_array_header_t *elems;
  int i;
  struct entry *entry;

  entry = get_root(pb, pb->current_rev);
  elems = svn_path_decompose(path, pool);
  for (i = 0; i < elems->nelts; i++)
    entry = get_child(pb, entry, APR_ARRAY_IDX(elems, i, const char *), pool);
  return entry;
}

/* Return the node-rev ID of ENTRY in string form. */
static const char *
node_rev_id(struct entry *entry, apr_pool_t *pool)
{
  return apr_psprintf(pool, "%d.%d.r%" SVN_REVNUM_T_FMT "/%" APR_OFF_T_FMT,
                      entry->node_id, entry->copy_id, entry->node_rev,
                      entry->node_off);
}

static void
get_node_info(apr_hash_t *headers, const char **path, svn_node_kind_t *kind,
              enum svn_node_action *action, svn_revnum_t *copyfrom_rev,
              const char **copyfrom_path)
{
  const char *val;

  /* Then add info from the headers.  */
  assert(*path = apr_hash_get(headers, SVN_REPOS_DUMPFILE_NODE_PATH,
                              APR_HASH_KEY_STRING));

  val = apr_hash_get(headers, SVN_REPOS_DUMPFILE_NODE_KIND,
                     APR_HASH_KEY_STRING);
  *kind = !val ? svn_node_unknown
    : (strcmp(val, "file") == 0) ? svn_node_file : svn_node_dir;

  assert(val = apr_hash_get(headers, SVN_REPOS_DUMPFILE_NODE_ACTION,
                            APR_HASH_KEY_STRING));
  if (strcmp(val, "change") == 0)
    *action = svn_node_action_change;
  else if (strcmp(val, "add") == 0)
    *action = svn_node_action_add;
  else if (strcmp(val, "delete") == 0)
    *action = svn_node_action_delete;
  else if (strcmp(val, "replace") == 0)
    *action = svn_node_action_replace;
  else
    abort();

  val = apr_hash_get(headers, SVN_REPOS_DUMPFILE_NODE_COPYFROM_REV,
                     APR_HASH_KEY_STRING);
  *copyfrom_rev = val ? SVN_STR_TO_REV(val) : SVN_INVALID_REVNUM;

  *copyfrom_path = apr_hash_get(headers, SVN_REPOS_DUMPFILE_NODE_COPYFROM_PATH,
                                APR_HASH_KEY_STRING);
}

static svn_error_t *
write_directory_rep(struct parse_baton *pb, struct entry *entry,
                    apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  const void *key;
  void *val;
  svn_stream_t *out = pb->rev_stream;
  struct entry *child;
  const char *name, *id;
  apr_off_t offset;

  /* Record the rev file offset of the directory data. */
  entry->text_rev = pb->current_rev;
  entry->text_off = 0;
  SVN_ERR(svn_io_file_seek(pb->rev_file, APR_CUR, &entry->text_off, pool));

  /* Write out a rep header. */
  svn_stream_printf(out, pool, "PLAIN\n");

  for (hi = apr_hash_first(pool, entry->children); hi; hi = apr_hash_next(hi))
    {
      apr_hash_this(hi, &key, NULL, &val);
      name = key;
      child = val;
      id = node_rev_id(child, pool);
      SVN_ERR(svn_stream_printf(out, pool, "K %" APR_SIZE_T_FMT "\n%s\n"
                                "V %" APR_SIZE_T_FMT "\n%s\n",
                                strlen(name), name, strlen(id), id));
    }

  /* Record the length of the directory data. */
  offset = 0;
  SVN_ERR(svn_io_file_seek(pb->rev_file, APR_CUR, &offset, pool));
  entry->text_len = offset - entry->text_off - 6;

  SVN_ERR(svn_stream_printf(out, pool, "ENDREP\n"));
  return SVN_NO_ERROR;
}

static svn_error_t *
write_props(struct parse_baton *pb, struct entry *entry, apr_pool_t *pool)
{
  apr_off_t offset;

  /* Record the rev file offset of the prop data. */
  entry->props_rev = pb->current_rev;
  entry->props_off = 0;
  SVN_ERR(svn_io_file_seek(pb->rev_file, APR_CUR, &entry->props_off, pool));

  /* Write out a rep header. */
  svn_stream_printf(pb->rev_stream, pool, "PLAIN\n");

  /* Write the props hash out to the rev file. */
  SVN_ERR(svn_hash_write(entry->props, pb->rev_file, pool));

  /* Record the length of the props data. */
  offset = 0;
  SVN_ERR(svn_io_file_seek(pb->rev_file, APR_CUR, &offset, pool));
  entry->props_len = offset - entry->props_off - 6;

  /* We don't need the props hash any more. */
  entry->props = NULL;
  svn_pool_destroy(entry->props_pool);

  SVN_ERR(svn_stream_printf(pb->rev_stream, pool, "ENDREP\n"));
  return SVN_NO_ERROR;
}

static svn_error_t *
write_node_rev(struct parse_baton *pb, struct entry *entry, apr_pool_t *pool)
{
  svn_stream_t *out = pb->rev_stream;

  /* Get the rev file offset of the node-rev. */
  entry->node_off = 0;
  SVN_ERR(svn_io_file_seek(pb->rev_file, APR_CUR, &entry->node_off, pool));

  SVN_ERR(svn_stream_printf(out, pool, "id: %s\n", node_rev_id(entry, pool)));
  SVN_ERR(svn_stream_printf(out, pool, "type: %s\n",
                            entry->children ? "dir" : "file"));
  if (entry->pred)
    SVN_ERR(svn_stream_printf(out, pool, "pred: %s\n",
                              node_rev_id(entry->pred, pool)));
  SVN_ERR(svn_stream_printf(out, pool, "count: %d\n", entry->pred_count));
  SVN_ERR(svn_stream_printf(out, pool, "text: %" SVN_REVNUM_T_FMT
                            " %" APR_OFF_T_FMT " %" APR_OFF_T_FMT
                            " %" APR_OFF_T_FMT "\n",
                            entry->text_rev, entry->text_off,
                            entry->text_len, entry->text_len));
  if (SVN_IS_VALID_REVNUM(entry->props_rev))
    SVN_ERR(svn_stream_printf(out, pool, "rep: %" SVN_REVNUM_T_FMT
                              " %" APR_OFF_T_FMT " %" APR_OFF_T_FMT
                              " %" APR_OFF_T_FMT "\n",
                              entry->props_rev, entry->props_off,
                              entry->props_len, entry->props_len));
  /* XXX use length-counted field format */
  SVN_ERR(svn_stream_printf(out, pool, "cpath: %s\n", entry->created_path));
  if (SVN_IS_VALID_REVNUM(entry->copyfrom_rev))
    /* XXX use length-counted field format. */
    SVN_ERR(svn_stream_printf(out, pool, "copyfrom: %s %" SVN_REVNUM_T_FMT
                              "%s\n", (entry->soft_copy) ? "soft" : "hard",
                              entry->copyfrom_rev, entry->copyfrom_path));
  else
    SVN_ERR(svn_stream_printf(out, pool, "copyroot: %s\n",
                              node_rev_id(entry->copyroot, pool)));

  return SVN_NO_ERROR;
}

static svn_error_t *
write_entry(struct parse_baton *pb, struct entry *entry, apr_pool_t *pool)
{
  apr_hash_index_t *hi;
  void *val;
  apr_pool_t *subpool;

  /* We can prune here if this node was not copied for the current rev. */
  if (entry->node_rev != pb->current_rev)
    return SVN_NO_ERROR;

  if (entry->children)
    {
      /* This is a directory; write out all the changed child entries. */
      subpool = svn_pool_create(pool);
      for (hi = apr_hash_first(pool, entry->children); hi;
           hi = apr_hash_next(hi))
        {
          svn_pool_clear(subpool);
          apr_hash_this(hi, NULL, NULL, &val);
          SVN_ERR(write_entry(pb, val, subpool));
        }
      svn_pool_destroy(subpool);

      if (entry->node_rev == pb->current_rev)
        SVN_ERR(write_directory_rep(pb, entry, pool));
    }

  if (entry->props)
    SVN_ERR(write_props(pb, entry, pool));

  if (entry->node_rev == pb->current_rev)
    SVN_ERR(write_node_rev(pb, entry, pool));

  return SVN_NO_ERROR;
}

/* --- The parser functions --- */

static svn_error_t *
new_revision_record(void **revision_baton, apr_hash_t *headers, void *baton,
                    apr_pool_t *pool)
{
  struct parse_baton *pb = baton;
  const char *revstr;
  svn_revnum_t rev;
  struct entry *root;
 
  /* Get the number of this revision in string and integral form. */
  revstr = apr_hash_get(headers, SVN_REPOS_DUMPFILE_REVISION_NUMBER,
                       APR_HASH_KEY_STRING);
  rev = SVN_STR_TO_REV(revstr);
  assert(rev == pb->roots->nelts);
  assert(rev == pb->current_rev + 1);
  pb->current_rev = rev;

  /* Open a file for this revision. */
  SVN_ERR(svn_io_file_open(&pb->rev_file, revstr,
                            APR_WRITE|APR_CREATE|APR_TRUNCATE|APR_BUFFERED,
                            APR_OS_DEFAULT, pb->pool));
  pb->rev_stream = svn_stream_from_aprfile(pb->rev_file, pb->pool);

  /* Set up a new root for this rev. */
  root = new_entry(pb->pool);
  if (rev != 0)
    copy_entry(pb, root, get_root(pb, rev - 1), FALSE, FALSE);
  else
    {
      root->node_id = pb->next_node_id++;
      root->copy_id = pb->next_copy_id++;
      root->children = apr_hash_make(pb->pool);
      root->copyroot = root;
      root->node_rev = 0;
    }
  root->created_path = "";
  APR_ARRAY_PUSH(pb->roots, struct entry *) = root;

  *revision_baton = pb;
  return SVN_NO_ERROR;
}

static svn_error_t *
uuid_record(const char *uuid, void *parse_baton, apr_pool_t *pool)
{
  /* Nothing yet. */
  return SVN_NO_ERROR;
}

static svn_error_t *
new_node_record(void **node_baton, apr_hash_t *headers, void *baton,
                apr_pool_t *pool)
{
  struct parse_baton *pb = baton;
  svn_node_kind_t kind;
  enum svn_node_action action;
  svn_revnum_t copyfrom_rev;
  const char *path, *copyfrom_path, *parent_path, *name;
  struct entry *parent, *entry, *copy_src;

  get_node_info(headers, &path, &kind, &action, &copyfrom_rev, &copyfrom_path);
  svn_path_split(path, &parent_path, &name, pool);
  parent = follow_path(pb, parent_path, pool);
  switch (action)
    {
    case svn_node_action_change:
      pb->current_node = get_child(pb, parent, name, pool);
      break;
    case svn_node_action_delete:
      apr_hash_set(parent->children, name, APR_HASH_KEY_STRING, NULL);
      pb->current_node = NULL;
      break;
    case svn_node_action_add:
    case svn_node_action_replace:
      entry = new_entry(pb->pool);
      if (SVN_IS_VALID_REVNUM(copyfrom_rev))
        {
          copy_src = find_entry(get_root(pb, copyfrom_rev), copyfrom_path,
                                pool);
          copy_entry(pb, entry, copy_src, TRUE, FALSE);
        }
      else
        {
          entry->node_id = pb->next_node_id++;
          entry->copy_id = parent->copy_id;
          if (kind == svn_node_dir)
            entry->children = apr_hash_make(pb->pool);
          entry->node_rev = pb->current_rev;
          entry->node_off = -1;
          entry->copyroot = parent->copyroot;
        }
      entry->created_path = apr_pstrdup(pb->pool, path);
      name = apr_pstrdup(pb->pool, name);
      apr_hash_set(parent->children, name, APR_HASH_KEY_STRING, entry);
      pb->current_node = entry;
      break;
    default:
      abort();
    }

  *node_baton = pb;
  return SVN_NO_ERROR;
}

static svn_error_t *
set_revision_property(void *baton, const char *name, const svn_string_t *value)
{
  /* Nothing yet. */
  return SVN_NO_ERROR;
}

static svn_error_t *
set_node_property(void *baton, const char *name, const svn_string_t *value)
{
  struct parse_baton *pb = baton;

  assert(pb->current_node);
  assert(pb->current_node->props);
  name = apr_pstrdup(pb->pool, name);
  value = svn_string_dup(value, pb->pool);
  apr_hash_set(pb->current_node->props, name, APR_HASH_KEY_STRING, value);
  return SVN_NO_ERROR;
}

static svn_error_t *
delete_node_property(void *node_baton, const char *name)
{
  /* We can't handle incremental dumps. */
  abort();
}

static svn_error_t *
remove_node_props(void *baton)
{
  struct parse_baton *pb = baton;
  struct entry *entry = pb->current_node;

  entry->props_pool = svn_pool_create(pb->pool);
  entry->props = apr_hash_make(entry->props_pool);
  return SVN_NO_ERROR;
}

static svn_error_t *
set_fulltext(svn_stream_t **stream, void *baton)
{
  struct parse_baton *pb = baton;
  struct entry *entry = pb->current_node;

  /* Record the current offset of the rev file as the text rep location. */
  entry->text_rev = pb->current_rev;
  entry->text_off = 0;
  SVN_ERR(svn_io_file_seek(pb->rev_file, APR_CUR, &entry->text_off, pb->pool));

  /* Write a representation header to the rev file. */
  SVN_ERR(svn_io_file_write_full(pb->rev_file, "PLAIN\n", 6, NULL, pb->pool));

  /* Have the caller write the contents into the rev file. */
  *stream = svn_stream_from_aprfile(pb->rev_file, pb->pool);
  return SVN_NO_ERROR;
}

static svn_error_t *
apply_textdelta(svn_txdelta_window_handler_t *handler, void **handler_baton,
                void *baton)
{
  /* We can't handle incremental dumps. */
  abort();
}

svn_error_t *
close_node(void *baton)
{
  struct parse_baton *pb = baton;
  apr_off_t offset;

  if (pb->current_node && pb->current_node->text_rev == pb->current_rev)
    {
      /* The caller is done writing the contents to the rev file.
         Record the length of the data written (subtract six for the
         header line). */
      offset = 0;
      SVN_ERR(svn_io_file_seek(pb->rev_file, APR_CUR, &offset, pb->pool));
      pb->current_node->text_len = offset - pb->current_node->text_off - 6;

      /* Write a representation trailer to the rev file. */
      SVN_ERR(svn_stream_printf(pb->rev_stream, pb->pool, "ENDREP\n"));
    }

  return SVN_NO_ERROR;
}

svn_error_t *close_revision(void *baton)
{
  struct parse_baton *pb = baton;
  apr_pool_t *pool = svn_pool_create(pb->pool);

  SVN_ERR(write_entry(pb, get_root(pb, pb->current_rev), pool));
  SVN_ERR(svn_io_file_close(pb->rev_file, pool));
  /* XXX changed-path data goes here */
  /* XXX offsets to root node and changed-path data go here */
  svn_pool_destroy(pool);
  return SVN_NO_ERROR;
}

static svn_repos_parser_fns2_t parser = {
  new_revision_record,
  uuid_record,
  new_node_record,
  set_revision_property,
  set_node_property,
  delete_node_property,
  remove_node_props,
  set_fulltext,
  apply_textdelta,
  close_node,
  close_revision
};

int main()
{
  apr_pool_t *pool;
  apr_file_t *infile;
  svn_stream_t *instream;
  struct parse_baton pb;
  svn_error_t *err;

  apr_initialize();
  pool = svn_pool_create(NULL);
  apr_file_open_stdin(&infile, pool);
  instream = svn_stream_from_aprfile(infile, pool);
  pb.roots = apr_array_make(pool, 1, sizeof(struct entry *));
  pb.current_rev = SVN_INVALID_REVNUM;
  pb.rev_file = NULL;
  pb.rev_stream = NULL;
  pb.next_node_id = 0;
  pb.next_copy_id = 0;
  pb.pool = pool;
  err = svn_repos_parse_dumpstream2(instream, &parser, &pb, NULL, NULL, pool);
  if (err)
    svn_handle_error(err, stderr, TRUE);
  return 0;
}
