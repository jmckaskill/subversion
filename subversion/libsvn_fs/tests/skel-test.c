/* skel-test.c --- tests for the skeleton functions
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "apr.h"
#include "svn_string.h"
#include "fs.h"
#include "skel.h"


/* Some utility functions.  */


/* A global pool, initialized by `main' for tests to use.  */
apr_pool_t *pool;


/* A place to set a breakpoint.  */
static int
fail (void)
{
  return 1;
}


/* Free everything from pool, and return an empty Subversion string.  */
static svn_string_t *
get_empty_string (void)
{
  apr_clear_pool (pool);

  return svn_string_ncreate (0, 0, pool);
}

/* Parse a skeleton from a Subversion string.  */
static skel_t *
parse_str (svn_string_t *str)
{
  return svn_fs__parse_skel (str->data, str->len, pool);
}


/* Parse a skeleton from a C string.  */
static skel_t *
parse_cstr (char *str)
{
  return svn_fs__parse_skel (str, strlen (str), pool);
}


enum char_type {
  type_nothing = 0,
  type_space = 1,
  type_digit = 2,
  type_paren = 3,
  type_name = 4,
};

static int skel_char_map_initialized;
static enum char_type skel_char_map[256];

static void
init_char_types (void)
{
  int i;
  const char *c;

  if (skel_char_map_initialized)
    return;

  for (i = 0; i < 256; i++)
    skel_char_map[i] = type_nothing;

  for (i = '0'; i <= '9'; i++)
    skel_char_map[i] = type_digit;

  for (c = "\t\n\f\r "; *c; c++)
    skel_char_map[(unsigned char) *c] = type_space;

  for (c = "()[]"; *c; c++)
    skel_char_map[(unsigned char) *c] = type_paren;

  for (i = 'A'; i <= 'Z'; i++)
    skel_char_map[i] = type_name;
  for (i = 'a'; i <= 'z'; i++)
    skel_char_map[i] = type_name;
  
  skel_char_map_initialized = 1;
}

/* Return true iff BYTE is a whitespace byte.  */
static int
skel_is_space (char byte)
{
  init_char_types ();

  return skel_char_map[(unsigned char) byte] == type_space;
}

#if 0
/* Return true iff BYTE is a digit byte.  */
static int
skel_is_digit (char byte)
{
  init_char_types ();

  return skel_char_map[(unsigned char) byte] == type_digit;
}
#endif

/* Return true iff BYTE is a paren byte.  */
static int
skel_is_paren (char byte)
{
  init_char_types ();

  return skel_char_map[(unsigned char) byte] == type_paren;
}

/* Return true iff BYTE is a name byte.  */
static int
skel_is_name (char byte)
{
  init_char_types ();

  return skel_char_map[(unsigned char) byte] == type_name;
}


/* Check that SKEL is an atom, and it's contents match LEN bytes of
   DATA. */
static int
check_atom (skel_t *skel, const char *data, int len)
{
  return (skel
	  && skel->is_atom
	  && skel->len == len
	  && ! memcmp (skel->data, data, len));
}


/* Functions that generate/check interesting implicit-length atoms.  */


/* Append to STR an implicit-length atom consisting of the byte BYTE,
   terminated by the character TERM.  BYTE must be a name byte,
   and TERM must be a valid skel separator, or NUL.  */
static void
put_implicit_length_byte (svn_string_t *str, char byte, char term)
{
  if (! skel_is_name (byte))
    abort ();
  if (term != '\0'
      && ! skel_is_space (term)
      && ! skel_is_paren (term))
    abort ();
  svn_string_appendbytes (str, &byte, 1);
  if (term != '\0')
    svn_string_appendbytes (str, &term, 1);
}


/* Return true iff SKEL is the parsed form of the atom produced by
   calling put_implicit_length with BYTE.  */
static int
check_implicit_length_byte (skel_t *skel, char byte)
{
  if (! skel_is_name (byte))
    abort ();

  return check_atom (skel, &byte, 1);
}


/* Subroutine for the *_implicit_length_all_chars functions.  */
static char *
gen_implicit_length_all_chars (int *len_p)
{
  int i, pos;
  static char name[256];

  /* Gotta start with a valid name character.  */
  pos = 0;
  name[pos++] = 'x';
  for (i = 0; i < 256; i++)
    if (! skel_is_space (i)
        && ! skel_is_paren (i))
      name[pos++] = i;

  *len_p = pos;
  return name;
}


/* Append to STR an implicit-length atom containing every character
   that's legal in such atoms, terminated by the valid atom terminator
   TERM.  */
static void
put_implicit_length_all_chars (svn_string_t *str, char term)
{
  int len;
  char *name = gen_implicit_length_all_chars (&len);

  if (term != '\0'
      && ! skel_is_space (term)
      && ! skel_is_paren (term))
    abort ();

  svn_string_appendbytes (str, name, len);
  if (term != '\0')
    svn_string_appendbytes (str, &term, 1);
}


/* Return true iff SKEL is the parsed form of the atom produced by
   calling put_implicit_length_all_chars.  */
static int
check_implicit_length_all_chars (skel_t *skel)
{
  int len;
  char *name = gen_implicit_length_all_chars (&len);

  return check_atom (skel, name, len);
}



/* Test parsing of implicit-length atoms.  */

static int
parse_implicit_length (const char **msg)
{
  svn_string_t *str = get_empty_string ();
  skel_t *skel;

  *msg = "parse implicit-length atoms";

  /* Try all valid single-byte atoms.  */
  {
    const char *c;
    int i;

    for (c = "\t\n\f\r ()[]"; *c; c++)
      for (i = 0; i < 256; i++)
        if (skel_is_name(i))
          {
            svn_string_setempty (str);
            put_implicit_length_byte (str, i, *c);
            skel = parse_str (str);
            if (! check_implicit_length_byte (skel, i))
              return fail ();
          }
  }

  /* Try an atom that contains every character that's legal in an
     implicit-length atom.  */
  svn_string_setempty (str);
  put_implicit_length_all_chars (str, '\0');
  skel = parse_str (str);
  if (! check_implicit_length_all_chars (skel))
    return fail ();

  return 0;
}


/* Functions that generate/check interesting explicit-length atoms.  */


/* Append to STR the representation of the atom containing the LEN
   bytes at DATA, in explicit-length form, using SEP as the separator
   between the length and the data.  */
static void
put_explicit_length (svn_string_t *str, const char *data, int len, char sep)
{
  char *buf = (char *) alloca (len + 100);
  int length_len;

  if (! skel_is_space (sep))
    abort ();

  /* Generate the length and separator character.  */
  sprintf (buf, "%d%c", len, sep);
  length_len = strlen(buf);
  
  /* Copy in the real data (which may contain nulls).  */
  memcpy (buf + length_len, data, len);

  svn_string_appendbytes (str, buf, length_len + len);
}


/* Return true iff SKEL is the parsed form of an atom generated by
   put_explicit_length.  */
static int
check_explicit_length (skel_t *skel, const char *data, int len)
{
  return check_atom (skel, data, len);
}


/* Test parsing of explicit-length atoms.  */

static int
try_explicit_length (const char *data, int len, int check_len)
{
  int i;
  svn_string_t *str = get_empty_string ();
  skel_t *skel;

  /* Try it with every possible separator character.  */
  for (i = 0; i < 256; i++)
    if (skel_is_space (i))
      {
	svn_string_setempty (str);
	put_explicit_length (str, data, len, i);
	skel = parse_str (str);
	if (! check_explicit_length (skel, data, check_len))
	  return fail (); 
      }

  return 0;
}


static int
parse_explicit_length (const char **msg)
{
  *msg = "parse explicit-length atoms";

  /* Try to parse the empty atom.  */
  try_explicit_length ("", 0, 0);

  /* Try to parse every one-character atom.  */
  {
    int i;

    for (i = 0; i < 256; i++)
      {
	char buf[1];
	
	buf[0] = i;
	if (try_explicit_length (buf, 1, 1))
	  return fail ();
      }
  }

  /* Try to parse an atom containing every character.  */
  {
    int i;
    char data[256];

    for (i = 0; i < 256; i++)
      data[i] = i;

    if (try_explicit_length (data, 256, 256))
      return fail ();
  }

  return 0;
}



/* Test parsing of invalid atoms. */

static struct invalid_atoms
{
  int type;
  int len;
  const char *data;
} invalid_atoms[] = { { 1,  1, "(" },
                      { 1,  1, ")" },
                      { 1,  1, "[" },
                      { 1,  1, "]" },
                      { 1,  1, " " },
                      { 1, 13, "Hello, World!" },
                      { 1,  8, "1mplicit" },

                      { 2,  2, "1" },
                      { 2,  1, "12" },

                      { 7,  0, NULL } };

static int
parse_invalid_atoms (const char **msg)
{
  struct invalid_atoms *ia = invalid_atoms;

  *msg = "parse invalid atoms";

  while (ia->type != 7)
    {
      if (ia->type == 1)
        {
          skel_t *skel = parse_cstr ((char *) ia->data);
          if (check_atom (skel, ia->data, ia->len))
            return fail ();
        }
      else
        {
          if (!try_explicit_length (ia->data, ia->len,
                                    strlen (ia->data)))
            return fail ();
        }

      ia++;
    }

  return 0;
}



/* Functions that generate/check interesting lists.  */

/* Append the start of a list to STR, using LEN bytes of the
   whitespace character SPACE.  */
static void
put_list_start (svn_string_t *str, char space, int len)
{
  int i;

  if (len > 0 && ! skel_is_space (space))
    abort ();

  svn_string_appendcstr (str, "(");
  for (i = 0; i < len; i++)
    svn_string_appendbytes (str, &space, 1);
}


/* Append the end of a list to STR, using LEN bytes of the
   whitespace character SPACE.  */
static void
put_list_end (svn_string_t *str, char space, int len)
{
  int i;

  if (len > 0 && ! skel_is_space (space))
    abort ();

  for (i = 0; i < len; i++)
    svn_string_appendbytes (str, &space, 1);
  svn_string_appendcstr (str, ")");
}


/* Return true iff SKEL is a list of length DESIRED_LEN.  */
static int
check_list (skel_t *skel, int desired_len)
{
  int len;
  skel_t *child;

  if (! (skel
	 && ! skel->is_atom))
    return 0;

  len = 0;
  for (child = skel->children; child; child = child->next)
    len++;

  return len == desired_len;
}



/* Parse lists.  */

static int
parse_list (const char **msg)
{
  *msg = "parse lists";

  {
    /* Try lists of varying length.  */
    int list_len;

    for (list_len = 0;
	 list_len < 30;
	 list_len < 4 ? list_len++ : (list_len *= 3))
      {
	/* Try lists with different separators.  */
	int sep;

	for (sep = 0; sep < 256; sep++)
	  if (skel_is_space (sep))
	    {
	      /* Try lists with different numbers of separator
                 characters between the elements.  */
	      int sep_count;

	      for (sep_count = 0;
		   sep_count < 30;
		   sep_count < 4 ? sep_count++ : (sep_count *= 3))
		{
		  /* Try various single-byte implicit-length atoms
		     for elements.  */
		  int atom_byte;

		  for (atom_byte = 0; atom_byte < 256; atom_byte++)
		    if (skel_is_name (atom_byte))
		      {
			int i;
			svn_string_t *str = get_empty_string ();
			skel_t *skel;
			skel_t *child;

			put_list_start (str, sep, sep_count);
			for (i = 0; i < list_len; i++)
			  put_implicit_length_byte (str, atom_byte, sep);
			put_list_end (str, sep, sep_count);

			skel = parse_str (str);
			if (! check_list (skel, list_len))
			  return fail ();
			for (child = skel->children;
			     child;
			     child = child->next)
			  if (! check_implicit_length_byte (child, atom_byte))
			    return fail ();
		      }

		  /* Try the atom containing every character that's
                     legal in an implicit-length atom as the element.  */
		  {
		    int i;
		    svn_string_t *str = get_empty_string ();
		    skel_t *skel;
		    skel_t *child;

		    put_list_start (str, sep, sep_count);
		    for (i = 0; i < list_len; i++)
		      put_implicit_length_all_chars (str, sep);
		    put_list_end (str, sep, sep_count);

		    skel = parse_str (str);
		    if (! check_list (skel, list_len))
		      return fail ();
		    for (child = skel->children;
			 child;
			 child = child->next)
		      if (! check_implicit_length_all_chars (child))
			return fail ();
		  }

		  /* Try using every one-byte explicit-length atom as
                     an element.  */
		  for (atom_byte = 0; atom_byte < 256; atom_byte++)
		    {
		      int i;
		      svn_string_t *str = get_empty_string ();
		      skel_t *skel;
		      skel_t *child;
		      char buf[1];

		      buf[0] = atom_byte;

		      put_list_start (str, sep, sep_count);
		      for (i = 0; i < list_len; i++)
			put_explicit_length (str, buf, 1, sep);
		      put_list_end (str, sep, sep_count);

		      skel = parse_str (str);
		      if (! check_list (skel, list_len))
			return fail ();
		      for (child = skel->children;
			   child;
			   child = child->next)
			if (! check_explicit_length (child, buf, 1))
			  return fail ();
		    }

		  /* Try using an atom containing every character as
		     an element.  */
		  {
		    int i;
		    svn_string_t *str = get_empty_string ();
		    skel_t *skel;
		    skel_t *child;
		    char data[256];

		    for (i = 0; i < 256; i++)
		      data[i] = i;

		    put_list_start (str, sep, sep_count);
		    for (i = 0; i < list_len; i++)
		      put_explicit_length (str, data, 256, sep);
		    put_list_end (str, sep, sep_count);

		    skel = parse_str (str);
		    if (! check_list (skel, list_len))
		      return fail ();
		    for (child = skel->children;
			 child;
			 child = child->next)
		      if (! check_explicit_length (child, data, 256))
			return fail ();
		  }
		}
	    }
      }
  }

  /* Try to parse some invalid lists.  */
  {
    int sep;

    /* Try different separators.  */ 
    for (sep = 0; sep < 256; sep++)
      if (skel_is_space (sep))
	{
	  /* Try lists with different numbers of separator
	     characters between the elements.  */
	  int sep_count;

	  for (sep_count = 0;
	       sep_count < 100;
	       sep_count < 10 ? sep_count++ : (sep_count *= 3))
	    {
	      svn_string_t *str;

	      /* A list with only a separator.  */
	      str = get_empty_string ();
	      put_list_start (str, sep, sep_count);
	      if (parse_str (str))
		return fail ();

	      /* A list with only a terminator.  */
	      str = get_empty_string ();
	      put_list_end (str, sep, sep_count);
	      if (parse_str (str))
		return fail ();

	      /* A list containing an invalid element.  */
	      str = get_empty_string ();
	      put_list_start (str, sep, sep_count);
	      svn_string_appendcstr (str, "100 ");
	      put_list_end (str, sep, sep_count);
	      if (parse_str (str))
		return fail ();
	    }
	}
  }
	      
  return 0;
}



/* Building interesting skels.  */

/* Build an atom skel containing the LEN bytes at DATA.  */
static skel_t *
build_atom (apr_size_t len, char *data)
{
  char *copy = apr_palloc (pool, len);
  skel_t *skel = apr_palloc (pool, sizeof (*skel));

  memcpy (copy, data, len);
  skel->is_atom = 1;
  skel->len = len;
  skel->data = copy;

  return skel;
}

/* Build an empty list skel.  */
static skel_t *
empty (void)
{
  skel_t *skel = apr_palloc (pool, sizeof (*skel));

  skel->is_atom = 0;
  skel->children = 0;

  return skel;
}

/* Stick ELEMENT at the beginning of the list skeleton LIST.  */
static void
add (skel_t *element, skel_t *list)
{
  element->next = list->children;
  list->children = element;
}


/* Return true if the contents of skel A are identical to those of
   skel B.  */
static int
skel_equal (skel_t *a, skel_t *b)
{
  if (a->is_atom != b->is_atom)
    return 0;

  if (a->is_atom)
    return (a->len == b->len
	    && ! memcmp (a->data, b->data, a->len));
  else
    {
      skel_t *a_child, *b_child;

      for (a_child = a->children, b_child = b->children;
	   a_child && b_child;
	   a_child = a_child->next, b_child = b_child->next)
	if (! skel_equal (a_child, b_child))
	  return 0;

      if (a_child || b_child)
	return 0;
    }

  return 1;
}


/* Unparsing implicit-length atoms.  */

static int
unparse_implicit_length (const char **msg)
{
  *msg = "unparse implicit-length atoms";

  /* Unparse and check every single-byte implicit-length atom.  */
  {
    int byte;

    for (byte = 0; byte < 256; byte++)
      if (skel_is_name (byte))
	{
	  svn_string_t *str = get_empty_string ();
	  char buf = byte;
	  skel_t *skel = build_atom (1, &buf);

	  str = svn_fs__unparse_skel (skel, pool);
	  
	  if (! (str
		 && str->len == 2
		 && str->data[0] == byte
		 && skel_is_space (str->data[1])))
	    return fail ();
	}
  }

  return 0;
}



/* Unparse some lists.  */

static int
unparse_list (const char **msg)
{
  *msg = "unparse lists";

  /* Make a list of all the single-byte implicit-length atoms.  */
  {
    svn_string_t *str = get_empty_string ();
    int byte;
    skel_t *list = empty ();
    skel_t *reparsed, *elt;

    for (byte = 0; byte < 256; byte++)
      if (skel_is_name (byte))
	{
	  char buf = byte;
	  add (build_atom (1, &buf), list);
	}

    /* Unparse that, parse it again, and see if we got the same thing
       back.  */
    str = svn_fs__unparse_skel (list, pool);
    reparsed = svn_fs__parse_skel (str->data, str->len, pool);

    if (! reparsed || reparsed->is_atom)
      return fail ();

    if (! skel_equal (list, reparsed))
      return fail ();

    elt = reparsed->children;
    for (byte = 255; byte >= 0; byte--)
      if (skel_is_name (byte))
	{
	  if (! (elt
		 && elt->is_atom
		 && elt->len == 1
		 && elt->data[0] == byte))
	    return fail ();

	  /* Verify that each element's data falls within the string.  */
	  if (elt->data < str->data
	      || elt->data + elt->len > str->data + str->len)
	    return fail ();

	  elt = elt->next;
	}

    /* We should have reached the end of the list at this point.  */
    if (elt)
      return fail ();
  }

  /* Make a list of lists.  */
  {
    svn_string_t *str = get_empty_string ();
    skel_t *top = empty ();
    skel_t *reparsed;
    int i;

    for (i = 0; i < 10; i++)
      {
	skel_t *middle = empty ();
	int j;

	for (j = 0; j < 10; j++)
	  {
	    char buf[10];
	    int k, val;

	    /* Make some interesting atom, containing lots of binary
               characters.  */
	    val = i * 10 + j;
	    for (k = 0; k < sizeof (buf); k++)
	      {
		buf[k] = val;
		val += j;
	      }

	    add (build_atom (sizeof (buf), buf), middle);
	  }

	add (middle, top);
      }

    str = svn_fs__unparse_skel (top, pool);
    reparsed = svn_fs__parse_skel (str->data, str->len, pool);

    if (! skel_equal (top, reparsed))
      return fail ();
  }

  return 0;
}


/* The test table.  */

int (*test_funcs[]) (const char **msg) = {
  0,
  parse_implicit_length,
  parse_explicit_length,
  parse_invalid_atoms,
  parse_list,
  unparse_implicit_length,
  unparse_list,
  0
};
