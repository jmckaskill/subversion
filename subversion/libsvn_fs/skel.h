/* skel.h : interface to `skeleton' functions
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
 * software developed by CollabNet (http://www.Collab.Net/)."
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

#ifndef SVN_LIBSVN_FS_SKEL_H
#define SVN_LIBSVN_FS_SKEL_H

#include "apr.h"


/* What is a skel?  */

/* Subversion needs to read a lot of structured data from database
   records.  Instead of writing a half-dozen parsers and getting lazy
   about error-checking, we define a reasonably dense, open-ended
   syntax for strings and lists, and then use that for the concrete
   representation of files, directories, property lists, etc.  This
   lets us handle all the fussy character-by-character testing and
   sanity checks all in one place, allowing the users of this library
   to focus on higher-level consistency.

   A `skeleton' (or `skel') is either an atom, or a list.  A list may
   contain zero or more elements, each of which may be an atom or a
   list.

   Here's a description of the syntax of a skel:

   A "whitespace" byte is either 9, 10, 12, 13, or 32 (ASCII tab,
   newline, form feed, and space).

   A "digit" byte is 48 -- 57 (ASCII digits).

   A "name" byte is 65 -- 90, or 97 -- 122 (ASCII upper- and
   lower-case characters).

   An atom has one the following two forms:
   - any string of bytes whose first byte is a name character, and
     which contains no whitespace, followed by a (not optional!)
     whitespace character (`implicit-length form'), or
   - a string of digit bytes, followed by exactly one whitespace
     character, followed by N bytes, where N is the value of the digit
     bytes as a decimal number (`explicit-length form').

   In the first case, the `contents' of the atom are everything except
   the final whitespace character.  In the second case, the contents
   of the atom are the N bytes after the count and whitespace.

   A list consists of a byte 40 (ASCII '('), followed by a series of
   atoms or lists, followed by a byte 41 (ASCII ')').  There
   may be zero or more whitespace characters after the '(' and before
   the ')', and between any pair of elements.  */


/* The `skel' structure.  */

/* A structure representing the results of parsing an array of bytes
   as a skel.  */
struct skel_t {

  /* True if the string was an atom, false if it was a list.

     If the string is an atom, DATA points to the beginning of its
     contents, and LEN gives the content length, in bytes.

     If the string is a list, DATA and LEN delimit the entire body of
     the list.  */
  int is_atom;

  const char *data;
  apr_size_t len;

  /* If the string is a list, CHILDREN is a pointer to a
     null-terminated linked list of skel objects representing the
     elements of the list, linked through their NEXT pointers.  */
  struct skel_t *children;
  struct skel_t *next;
};
typedef struct skel_t skel_t;



/* Operations on skels.  */


/* Parse the LEN bytes at DATA as the concrete representation of a
   skel, and return a skel object allocated from POOL describing its
   contents.  If the data is not a properly-formed SKEL object, return
   zero.

   The returned skel objects point into the block indicated by DATA
   and LEN; we don't copy the contents.

   You'd think that DATA would be a `const char *', but we want to
   create `skel' structures that point into it, and a skel's DATA
   pointer shouldn't be a `const char *', since that would constrain
   how the caller can use the structure.  We only want to say that
   *we* won't change it --- we don't want to prevent the caller from
   changing it --- but C's type system doesn't allow us to say that.  */
skel_t *svn_fs__parse_skel (const char *data, apr_size_t len,
			    apr_pool_t *pool);


/* Create an atom skel whose contents are the C string STR, allocated
   from POOL.  */
skel_t *svn_fs__make_atom (const char *str, apr_pool_t *pool);


/* Create an empty list skel, allocated from POOL.  */
skel_t *svn_fs__make_empty_list (apr_pool_t *pool);


/* Prepend SKEL to LIST.  */
void svn_fs__prepend (skel_t *skel, skel_t *list);


/* Return a string whose contents are a concrete representation of
   SKEL.  Allocate the string from POOL.  */
svn_string_t *svn_fs__unparse_skel (const skel_t *skel, apr_pool_t *pool);


/* Return true iff SKEL is an atom whose data is the same as STR.  */
int svn_fs__is_atom (skel_t *skel, const char *str);


/* Return the length of the list skel SKEL.  Atoms have a length of -1.  */
int svn_fs__list_length (skel_t *skel);


/* Make a copy of SKEL and its data in POOL.  */
skel_t *svn_fs__copy_skel (skel_t *skel, apr_pool_t *pool);

#endif /* SVN_LIBSVN_FS_SKEL_H */
