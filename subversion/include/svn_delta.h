/*
 * svn_delta.h :  structures related to delta-parsing
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

/* ==================================================================== */



#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef SVN_DELTA_H
#define SVN_DELTA_H

#include <apr_pools.h>
#include <apr_file_io.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_io.h"




/*** Text deltas.  ***/

/* A text delta represents the difference between two strings of
   bytes, the `source' string and the `target' string.  Given a source
   string and a target string, we can compute a text delta; given a
   source string and a delta, we can reconstruct the target string.
   However, note that deltas are not reversible: you cannot always
   reconstruct the source string given the target string and delta.

   Since text deltas can be very large, the interface here allows us
   to produce and consume them in pieces.  Each piece, represented by
   an `svn_txdelta_window_t' structure, describes how to produce the
   next section of the target string.

   To compute a new text delta:

   - We call `svn_txdelta' on the strings we want to compare.  That
     returns us an `svn_txdelta_stream_t' object.

   - We then call `svn_txdelta_next_window' on the stream object
     repeatedly.  Each call returns a new `svn_txdelta_window_t'
     object, which describes the next portion of the target string.
     When `svn_txdelta_next_window' returns zero, we are done building
     the target string.  */


/* An `svn_txdelta_window_t' object describes how to reconstruct a
   contiguous section of the target string (the "target view") using a
   specified contiguous region of the source string (the "source
   view").  It contains a series of instructions which assemble the
   new target string text by pulling together substrings from:
     - the source view,
     - the previously constructed portion of the target view,
     - a string of new data contained within the window structure

   The source view must always slide forward from one window to the
   next; that is, neither the beginning nor the end of the source view
   may move to the left as we read from a window stream.  This
   property allows us to apply deltas to non-seekable source streams
   without making a full copy of the source stream.  */

/* A single text delta instruction.  */
typedef struct svn_txdelta_op_t {
  enum {
    /* Append the LEN bytes at OFFSET in the source view to the
       target.  It must be the case that 0 <= OFFSET < OFFSET + LEN <=
       size of source view.  */
    svn_txdelta_source,

    /* Append the LEN bytes at OFFSET in the target view, to the
       target.  It must be the case that 0 <= OFFSET < current
       position in the target view.

       However!  OFFSET + LEN may be *beyond* the end of the existing
       target data.  "Where the heck does the text come from, then?"
       If you start at OFFSET, and append LEN bytes one at a time,
       it'll work out --- you're adding new bytes to the end at the
       same rate you're reading them from the middle.  Thus, if your
       current target text is "abcdefgh", and you get an
       `svn_delta_target' instruction whose OFFSET is 6 and whose LEN
       is 7, the resulting string is "abcdefghghghghg".  This trick is
       actually useful in encoding long runs of consecutive
       characters, long runs of CR/LF pairs, etc.  */
    svn_txdelta_target,

    /* Append the LEN bytes at OFFSET in the window's NEW string to
       the target.  It must be the case that 0 <= OFFSET < OFFSET +
       LEN <= length of NEW.  Windows MUST use new data in ascending
       order with no overlap at the moment; svn_txdelta_to_svndiff
       depends on this.  */
    svn_txdelta_new
  } action_code;
  
  apr_off_t offset;
  apr_off_t length;
} svn_txdelta_op_t;


/* How to produce the next stretch of the target string.  */
typedef struct svn_txdelta_window_t {

  /* The offset and length of the source view for this window.  */
  apr_off_t sview_offset;
  apr_size_t sview_len;

  /* The length of the target view for this window, i.e. the number of
   * bytes which will be reconstructed by the instruction stream.  */
  apr_size_t tview_len;

  /* The number of instructions in this window.  */
  int num_ops;

  /* The allocated size of the ops array.  */
  int ops_size;
  
  /* The instructions for this window.  */
  svn_txdelta_op_t *ops;

  /* New data, for use by any `svn_delta_new' instructions.  */
  svn_string_t *new_data;

  /* The sub-pool that this window is living in, needed for
     svn_txdelta_free_window() */
  apr_pool_t *pool;

} svn_txdelta_window_t;


/* A typedef for functions that consume a series of delta windows, for
   use in caller-pushes interfaces.  Such functions will typically
   apply the delta windows to produce some file, or save the windows
   somewhere.  At the end of the delta window stream, you must call
   this function passing zero for the WINDOW argument.  */
typedef svn_error_t *(svn_txdelta_window_handler_t)
                     (svn_txdelta_window_t *window, void *baton);


/* A delta stream --- this is the hat from which we pull a series of
   svn_txdelta_window_t objects, which, taken in order, describe the
   entire target string.  This type is defined within libsvn_delta, and
   opaque outside that library.  */
typedef struct svn_txdelta_stream_t svn_txdelta_stream_t;


/* Set *WINDOW to a pointer to the next window from the delta stream
   STREAM.  When we have completely reconstructed the target string,
   set *WINDOW to zero.  */
svn_error_t *svn_txdelta_next_window (svn_txdelta_window_t **window,
                                      svn_txdelta_stream_t *stream);


/* Free the delta window WINDOW.  */
void svn_txdelta_free_window (svn_txdelta_window_t *window);
     

/* Set *STREAM to a pointer to a delta stream that will turn the byte
   string from SOURCE into the byte stream from TARGET.

   SOURCE and TARGET are both readable generic streams.  When we call
   `svn_txdelta_next_window' on *STREAM, it will read from SOURCE and
   TARGET to gather as much data as it needs.

   Do any necessary allocation in a sub-pool of POOL.  */
void svn_txdelta (svn_txdelta_stream_t **stream,
                  svn_stream_t *source,
                  svn_stream_t *target,
                  apr_pool_t *pool);


/* Free the delta stream STREAM.  */
void svn_txdelta_free (svn_txdelta_stream_t *stream);


/* Prepare to apply a text delta.  SOURCE is a readable generic stream
   yielding the source data, TARGET is a writable generic stream to
   write target data to, and allocation takes place in a sub-pool of
   POOL.  On return, *HANDLER is set to a window handler function and
   *HANDLER_BATON is set to the value to pass as the BATON argument to
   *HANDLER.  */
void svn_txdelta_apply (svn_stream_t *source,
                        svn_stream_t *target,
                        apr_pool_t *pool,
                        svn_txdelta_window_handler_t **handler,
                        void **handler_baton);



/*** Producing and consuming svndiff-format text deltas.  ***/

/* Prepare to produce an svndiff-format diff from text delta windows.
   OUTPUT is a writable generic stream to write the svndiff data to.
   Allocation takes place in a sub-pool of POOL.  On return, *HANDLER
   is set to a window handler function and *HANDLER_BATON is set to
   the value to pass as the BATON argument to *HANDLER.  */
void svn_txdelta_to_svndiff (svn_stream_t *output,
                             apr_pool_t *pool,
                             svn_txdelta_window_handler_t **handler,
                             void **handler_baton);

/* Return a writable generic stream which will parse svndiff-format
   data into a text delta, invoking HANDLER with HANDLER_BATON
   whenever a new window is ready.  */
svn_stream_t *svn_txdelta_parse_svndiff (svn_txdelta_window_handler_t *handler,
                                         void *handler_baton,
                                         apr_pool_t *pool);



/*** Traversing tree deltas. ***/


/* In Subversion, we've got various producers and consumers of tree
   deltas.

   In processing a `commit' command:
   - The client examines its working copy data, and produces a tree
     delta describing the changes to be committed.
   - The client networking library consumes that delta, and sends them
     across the wire as an equivalent series of WebDAV requests.
   - The Apache WebDAV module receives those requests and produces a
     tree delta --- hopefully equivalent to the one the client
     produced above.
   - The Subversion server module consumes that delta and commits an
     appropriate transaction to the filesystem.

   In processing an `update' command, the process is reversed:
   - The Subversion server module talks to the filesystem and produces
     a tree delta describing the changes necessary to bring the
     client's working copy up to date.
   - The Apache WebDAV module consumes this delta, and assembles a
     WebDAV reply representing the appropriate changes.
   - The client networking library receives that WebDAV reply, and
     produces a tree delta --- hopefully equivalent to the one the
     Subversion server produced above.
   - The working copy library consumes that delta, and makes the
     appropriate changes to the working copy.

   The simplest approach would be to represent tree deltas using the
   obvious data structure.  To do an update, the server would
   construct a delta structure, and the working copy library would
   apply that structure to the working copy; WebDAV's job would simply
   be to get the structure across the net intact.

   However, we expect that these deltas will occasionally be too large
   to fit in a typical workstation's swap area.  For example, in
   checking out a 200Mb source tree, the entire source tree is
   represented by a single tree delta.  So it's important to handle
   deltas that are too large to fit in swap all at once.

   So instead of representing the tree delta explicitly, we define a
   standard way for a consumer to process each piece of a tree delta
   as soon as the producer creates it.  The `svn_delta_edit_fns_t'
   structure is a set of callback functions to be defined by a delta
   consumer, and invoked by a delta producer.  Each invocation of a
   callback function describes a piece of the delta --- a file's
   contents changing, something being renamed, etc.  */


/* A structure full of callback functions the delta source will invoke
   as it produces the delta.  */
typedef struct svn_delta_edit_fns_t
{
  /* Here's how to use these functions to express a tree delta.

     The delta consumer implements the callback functions described in
     this structure, and the delta producer invokes them.  So the
     caller (producer) is pushing tree delta data at the callee
     (consumer).

     At the start of traversal, the consumer provides EDIT_BATON, a
     baton global to the entire delta edit.  In the case of
     `svn_xml_parse', this would be the EDIT_BATON argument; other
     producers will work differently.  The producer should pass this
     value as the EDIT_BATON argument to the `replace_root' function,
     to get a baton representing root of the tree being edited.

     Most of the callbacks work in the obvious way:

         delete_item
         add_file           add_directory    
         replace_file       replace_directory

     Each of these takes a directory baton, indicating the directory
     in which the change takes place, and a NAME argument, giving the
     name of the file, subdirectory, or directory entry to change.
     (NAME is always a single path component, never a full directory
     path.)

     Since every call requires a parent directory baton, including
     add_directory and replace_directory, where do we ever get our
     initial directory baton, to get things started?  The
     `replace_root' function returns a baton for the top directory of
     the change.  In general, the producer needs to invoke the
     editor's `replace_root' function before it can get anything done.

     While `replace_root' provides a directory baton for the root of
     the tree being changed, the `add_directory' and
     `replace_directory' callbacks provide batons for other
     directories.  Like the callbacks above, they take a PARENT_BATON
     and a single path component NAME, and then return a new baton for
     the subdirectory being created / modified --- CHILD_BATON.  The
     producer can then use CHILD_BATON to make further changes in that
     subdirectory.

     So, if we already have subdirectories named `foo' and `foo/bar',
     then the producer can create a new file named `foo/bar/baz.c' by
     calling:
        replace_root () --- yielding a baton ROOT for the top directory
        replace_directory (ROOT, "foo") --- yielding a baton F for `foo'
        replace_directory (F, "bar") --- yielding a baton B for `foo/bar'
        add_file (B, "baz.c")
     
     When the producer is finished making changes to a directory, it
     should call `close_directory'.  This lets the consumer do any
     necessary cleanup, and free the baton's storage.

     The `add_file' and `replace_file' callbacks each return a baton
     for the file being created or changed.  This baton can then be
     passed to `apply_textdelta' to change the file's contents, or
     `change_file_prop' to change the file's properties.  When the
     producer is finished making changes to a file, it should call
     `close_file', to let the consumer clean up and free the baton.

     The `add_file', `add_directory', `replace_file', and
     `replace_directory' functions all take arguments ANCESTOR_PATH
     and ANCESTOR_REVISION.  If ANCESTOR_PATH is non-zero, then
     ANCESTOR_PATH and ANCESTOR_REVISION indicate the ancestor of the
     resulting object.

     There are six restrictions on the order in which the producer
     may use the batons:

     1. The producer may call `replace_directory', `add_directory',
        `replace_file', `add_file', or `delete_item' at most once on
        any given directory entry.

     2. The producer may not close a directory baton until it has
        closed all batons for its subdirectories.

     3. When a producer calls `replace_directory' or `add_directory',
        it must specify the most recently opened of the currently open
        directory batons.  Put another way, the producer cannot have
        two sibling directory batons open at the same time.

     4. A producer must call `change_dir_prop' on a directory either
        before opening any of the directory's subdirs or after closing
        them, but not in the middle.

     5. When the producer calls `replace_file' or `add_file', either:

        (a) The producer must follow with the changes to the file
        (`change_file_prop' and/or `apply_textdelta', as applicable)
        followed by a `close_file' call, before issuing any other file
        or directory calls, or

        (b) The producer must follow with a `change_file_prop' call if
        it is applicable, before issuing any other file or directory
        calls; later, after all directory batons including the root
        have been closed, the producer must issue `apply_textdelta'
        and `close_file' calls.

     6. When the producer calls `apply_textdelta', it must make all of
        the window handler calls (including the NULL window at the
        end) before issuing any other edit_fns calls.

     So, the producer needs to use directory and file batons as if it
     is doing a single depth-first traversal of the tree, with the
     exception that the producer may keep file batons open in order to
     make apply_textdelta calls at the end.

     These restrictions make it easier to write a consumer that
     generates an XML-style tree delta.  An XML tree delta mentions
     each directory once, and includes all the changes to that
     directory within the <directory> element.  However, it does allow
     text deltas to appear at the end.  */


  /* Set *ROOT_BATON to a baton for the top directory of the change.
     (This is the top of the subtree being changed, not necessarily
     the root of the filesystem.)  Like any other directory baton, the
     producer should call `close_directory' on ROOT_BATON when they're
     done.  */
  svn_error_t *(*replace_root) (void *edit_baton,
                                void **root_baton);


  /* Deleting things.  */
       
  /* Remove the directory entry named NAME.  */
  /* FIXME: this used to be just delete(), but was changed to avoid
   * gratuitous incompatibility with C++, where `delete' is a reserved
   * keyword.  Unfortunately, remove() is taken by the standard C
   * library.  The compromise is delete_item(), but if anyone can
   * think of a better name, tags-query-replace is your friend. :-)
   * If you're reading this comment long after 22 Dec 2000, then
   * apparently no one has thought of a better name, so it's probably
   * time to remove the comment. */
  svn_error_t *(*delete_item) (svn_string_t *name,
                               void *parent_baton);


  /* Creating and modifying directories.  */
  
  /* We are going to add a new subdirectory named NAME.  We will use
     the value this callback stores in *CHILD_BATON as the
     PARENT_BATON for further changes in the new subdirectory.  The
     subdirectory is described as a series of changes to the base; if
     ANCESTOR_PATH is zero, the changes are relative to an empty
     directory. */
  svn_error_t *(*add_directory) (svn_string_t *name,
                                 void *parent_baton,
                                 svn_string_t *ancestor_path,
                                 svn_revnum_t ancestor_revision,
                                 void **child_baton);

  /* We are going to change the directory entry named NAME to a
     subdirectory.  The callback must store a value in *CHILD_BATON
     that will be used as the PARENT_BATON for subsequent changes in
     this subdirectory.  The subdirectory is described as a series of
     changes to the base; if ANCESTOR_PATH is zero, the changes are
     relative to an empty directory.  */
  svn_error_t *(*replace_directory) (svn_string_t *name,
                                     void *parent_baton,
                                     svn_string_t *ancestor_path,
                                     svn_revnum_t ancestor_revision,
                                     void **child_baton);

  /* Change the value of a directory's property.
     - DIR_BATON specifies the directory whose property should change.
     - NAME is the name of the property to change.
     - VALUE is the new value of the property, or zero if the property
     should be removed altogether.  */
  svn_error_t *(*change_dir_prop) (void *dir_baton,
                                   svn_string_t *name,
                                   svn_string_t *value);

  /* We are done processing a subdirectory, whose baton is DIR_BATON
     (set by add_directory or replace_directory).  We won't be using
     the baton any more, so whatever resources it refers to may now be
     freed.  */
  svn_error_t *(*close_directory) (void *dir_baton);


  /* Creating and modifying files.  */

  /* We are going to add a new file named NAME.  The callback can
     store a baton for this new file in **FILE_BATON; whatever value
     it stores there will be passed through to apply_textdelta and/or
     apply_propdelta.  */
  svn_error_t *(*add_file) (svn_string_t *name,
                            void *parent_baton,
                            svn_string_t *ancestor_path,
                            svn_revnum_t ancestor_revision,
                            void **file_baton);

  /* We are going to change the directory entry named NAME to a file.
     The callback can store a baton for this new file in **FILE_BATON;
     whatever value it stores there will be passed through to
     apply_textdelta and/or apply_propdelta.  */
  svn_error_t *(*replace_file) (svn_string_t *name,
                                void *parent_baton,
                                svn_string_t *ancestor_path,
                                svn_revnum_t ancestor_revision,
                                void **file_baton);

  /* Apply a text delta, yielding the new revision of a file.

     FILE_BATON indicates the file we're creating or updating, and the
     ancestor file on which it is based; it is the baton set by some
     prior `add_file' or `replace_file' callback.

     The callback should set *HANDLER to a text delta window
     handler; we will then call *HANDLER on successive text
     delta windows as we receive them.  The callback should set
     *HANDLER_BATON to the value we should pass as the BATON
     argument to *HANDLER.  */
  svn_error_t *(*apply_textdelta) (void *file_baton, 
                                   svn_txdelta_window_handler_t **handler,
                                   void **handler_baton);

  /* Change the value of a file's property.
     - FILE_BATON specifies the file whose property should change.
     - NAME is the name of the property to change.
     - VALUE is the new value of the property, or zero if the property
     should be removed altogether.  */
  svn_error_t *(*change_file_prop) (void *file_baton,
                                    svn_string_t *name,
                                    svn_string_t *value);

  /* We are done processing a file, whose baton is FILE_BATON (set by
     `add_file' or `replace_file').  We won't be using the baton any
     more, so whatever resources it refers to may now be freed.  */
  svn_error_t *(*close_file) (void *file_baton);

  /* All delta processing is done.  Call this, with the EDIT_BATON for
     the entire edit. */
  svn_error_t *(*close_edit) (void *edit_baton);

} svn_delta_edit_fns_t;


/* Compose EDITOR_1 and its baton with EDITOR_2 and its baton. 
 *
 * Returns a new editor in E which each function FUN calls
 * EDITOR_1->FUN and then EDITOR_2->FUN, with the corresponding batons.
 * 
 * If EDITOR_1->FUN returns error, that error is returned from E->FUN
 * and EDITOR_2->FUN is never called; otherwise E->FUN's return value
 * is the same as EDITOR_2->FUN's.
 *
 * If an editor function is null, it is simply never called, and this
 * is not an error.
 */
void
svn_delta_compose_editors (const svn_delta_edit_fns_t **new_editor,
                           void **new_edit_baton,
                           const svn_delta_edit_fns_t *editor_1,
                           void *edit_baton_1,
                           const svn_delta_edit_fns_t *editor_2,
                           void *edit_baton_2,
                           apr_pool_t *pool);


/* Compose BEFORE_EDITOR, BEFORE_EDIT_BATON with MIDDLE_EDITOR,
 * MIDDLE_EDIT_BATON, then compose the result with AFTER_EDITOR,
 * AFTER_EDIT_BATON, all according to the conventions of
 * svn_delta_compose_editors().  Return the resulting editor in
 * *NEW_EDITOR, *NEW_EDIT_BATON.
 *
 * If either BEFORE_EDITOR or AFTER_EDITOR is null, that editor will
 * simply not be included in the composition.  It is advised, though
 * not required, that a null editor pair with a null baton, and a
 * non-null editor with a non-null baton.
 *
 * MIDDLE_EDITOR must not be null.  I'm not going to tell you what
 * happens if it is.
 */
void svn_delta_wrap_editor (const svn_delta_edit_fns_t **new_editor,
                            void **new_edit_baton,
                            const svn_delta_edit_fns_t *before_editor,
                            void *before_edit_baton,
                            const svn_delta_edit_fns_t *middle_editor,
                            void *middle_edit_baton,
                            const svn_delta_edit_fns_t *after_editor,
                            void *after_edit_baton,
                            apr_pool_t *pool);



/* Creates an editor which outputs XML delta streams to OUTPUT.  On
   return, *EDITOR and *EDITOR_BATON will be set to the editor and its
   associate baton.  The editor's memory will live in a sub-pool of
   POOL. */
svn_error_t *
svn_delta_get_xml_editor (svn_stream_t *output,
                          const svn_delta_edit_fns_t **editor,
                          void **edit_baton,
                          apr_pool_t *pool);



/* An opaque object that represents a Subversion Delta XML parser. */

typedef struct svn_delta_xml_parser_t svn_delta_xml_parser_t;

/* Given a precreated svn_delta_edit_fns_t EDITOR, return a custom xml
   PARSER that will call into it (and feed EDIT_BATON to its
   callbacks.)  Additionally, this XML parser will use BASE_PATH and
   BASE_REVISION as default "context variables" when computing ancestry
   within a tree-delta. */
svn_error_t  *svn_delta_make_xml_parser (svn_delta_xml_parser_t **parser,
                                         const svn_delta_edit_fns_t *editor,
                                         void *edit_baton,
                                         svn_string_t *base_path, 
                                         svn_revnum_t base_revision,
                                         apr_pool_t *pool);


/* Destroy an svn_delta_xml_parser_t when finished with it. */
void svn_delta_free_xml_parser (svn_delta_xml_parser_t *parser);


/* Push LEN bytes of xml data in BUFFER at SVN_XML_PARSER.  As xml is
   parsed, EDITOR callbacks will be executed (using context variables
   and batons that were used to create the parser.)  If this is the
   final parser "push", ISFINAL must be set to true (so that both
   expat and local cleanup can occur). */
svn_error_t *
svn_delta_xml_parsebytes (const char *buffer, apr_size_t len, int isFinal, 
                          svn_delta_xml_parser_t *svn_xml_parser);


/* Reads an XML stream from SOURCE using expat internally, validating
   the XML as it goes (according to Subversion's own tree-delta DTD).
   Whenever an interesting event happens, it calls a caller-specified
   callback routine from EDITOR.  
   
   Once called, it retains control and "pulls" data from SOURCE
   until either the stream runs out or an error occurs. */
svn_error_t *svn_delta_xml_auto_parse (svn_stream_t *source,
                                       const svn_delta_edit_fns_t *editor,
                                       void *edit_baton,
                                       svn_string_t *base_path,
                                       svn_revnum_t base_revision,
                                       apr_pool_t *pool);





/* A general in-memory representation of a single property.  Most of
   the time, property lists will be stored completely in hashes.  But
   sometimes it's useful to have an "ordered" collection of
   properties, in which case we use an apr_array of the type below. */
typedef struct svn_prop_t
{
  svn_string_t *name;
  svn_string_t *value;
} svn_prop_t;



#endif  /* SVN_DELTA_H */

#ifdef __cplusplus
}
#endif /* __cplusplus */


/* ----------------------------------------------------------------
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */

