/*
 * xml.c:  xml helper code shared among the Subversion libraries.
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



#include <string.h>
#include <assert.h>
#include "apr_pools.h"
#include "svn_xml.h"




/*** XML escaping. ***/

/* Return an xml-safe version of STRING. */
void
svn_xml_escape_string (svn_string_t **outstr,
                       svn_string_t *string,
                       apr_pool_t *pool)
{
  const char *start = string->data, *end = start + string->len;
  const char *p = start, *q;

  if (*outstr == NULL)
    *outstr = svn_string_create ("", pool);

  while (1)
    {
      /* Find a character which needs to be quoted and append bytes up
         to that point.  Strictly speaking, '>' only needs to be
         quoted if it follows "]]", but it's easier to quote it all
         the time.  */
      q = p;
      while (q < end && *q != '&' && *q != '<' && *q != '>'
             && *q != '"' && *q != '\'')
        q++;
      svn_string_appendbytes (*outstr, p, q - p, pool);

      /* We may already be a winner.  */
      if (q == end)
        break;

      /* Append the entity reference for the character.  */
      if (*q == '&')
        svn_string_appendcstr (*outstr, "&amp;", pool);
      else if (*q == '<')
        svn_string_appendcstr (*outstr, "&lt;", pool);
      else if (*q == '>')
        svn_string_appendcstr (*outstr, "&gt;", pool);
      else if (*q == '"')
        svn_string_appendcstr (*outstr, "&quot;", pool);
      else if (*q == '\'')
        svn_string_appendcstr (*outstr, "&apos;", pool);

      p = q + 1;
    }
}




/*** Making a parser. ***/

svn_xml_parser_t *
svn_xml_make_parser (void *userData,
                     XML_StartElementHandler start_handler,
                     XML_EndElementHandler end_handler,
                     XML_CharacterDataHandler data_handler,
                     apr_pool_t *pool)
{
  svn_xml_parser_t *svn_parser;
  apr_pool_t *subpool;

  XML_Parser parser = XML_ParserCreate (NULL);

  XML_SetUserData (parser, userData);
  XML_SetElementHandler (parser, start_handler, end_handler); 
  XML_SetCharacterDataHandler (parser, data_handler);

  subpool = svn_pool_create (pool);

  svn_parser = apr_pcalloc (subpool, sizeof (svn_xml_parser_t));

  svn_parser->parser = parser;
  svn_parser->pool   = subpool;

  return svn_parser;
}


/* Free a parser */
void
svn_xml_free_parser (svn_xml_parser_t *svn_parser)
{
  /* Free the expat parser */
  XML_ParserFree (svn_parser->parser);        

  /* Free the subversion parser */
  apr_destroy_pool (svn_parser->pool);
}




/* Push LEN bytes of xml data in BUF at SVN_PARSER.  If this is the
   final push, IS_FINAL must be set.  */
svn_error_t *
svn_xml_parse (svn_xml_parser_t *svn_parser,
               const char *buf,
               apr_ssize_t len,
               svn_boolean_t is_final)
{
  svn_error_t *err;
  int success;

  /* Parse some xml data */
  success = XML_Parse (svn_parser->parser, buf, len, is_final);

  /* If expat choked internally, return its error. */
  if (! success)
    {
      err = svn_error_createf
        (SVN_ERR_MALFORMED_XML, 0, NULL, svn_parser->pool, 
         "%s at line %d",
         XML_ErrorString (XML_GetErrorCode (svn_parser->parser)),
         XML_GetCurrentLineNumber (svn_parser->parser));
      
      /* Kill all parsers and return the expat error */
      svn_xml_free_parser (svn_parser);
      return err;
    }

  /* Did an an error occur somewhere *inside* the expat callbacks? */
  if (svn_parser->error)
    {
      err = svn_parser->error;
      svn_xml_free_parser (svn_parser);
      return err;
    }
  
  return SVN_NO_ERROR;
}



/* The way to officially bail out of xml parsing.
   Store ERROR in SVN_PARSER and set all expat callbacks to NULL. */
void svn_xml_signal_bailout (svn_error_t *error,
                             svn_xml_parser_t *svn_parser)
{
  /* This will cause the current XML_Parse() call to finish quickly! */
  XML_SetElementHandler (svn_parser->parser, NULL, NULL);
  XML_SetCharacterDataHandler (svn_parser->parser, NULL);
  
  /* Once outside of XML_Parse(), the existence of this field will
     cause svn_delta_parse()'s main read-loop to return error. */
  svn_parser->error = error;
}








/*** Attribute walking. ***/

/* See svn_xml.h for details. */
const char *
svn_xml_get_attr_value (const char *name, const char **atts)
{
  while (atts && (*atts))
    {
      if (strcmp (atts[0], name) == 0)
        return atts[1];
      else
        atts += 2; /* continue looping */
    }

  /* Else no such attribute name seen. */
  return NULL;
}



/*** Printing XML ***/

void
svn_xml_make_header (svn_string_t **str, apr_pool_t *pool)
{
  if (*str == NULL)
    *str = svn_string_create ("", pool);
  svn_string_appendcstr (*str, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n",
                         pool);
}



/*** Creating attribute hashes. ***/

/* Combine an existing attribute list ATTS with a HASH that itself
   represents an attribute list.  Iff PRESERVE is true, then no value
   already in HASH will be changed, else values from ATTS will
   override previous values in HASH. */
static void
amalgamate (const char **atts,
            apr_hash_t *ht,
            svn_boolean_t preserve,
            apr_pool_t *pool)
{
  const char *key;

  if (atts)
    for (key = *atts; key; key = *(++atts))
      {
        const char *val = *(++atts);
        size_t keylen;
        assert (key != NULL);
        /* kff todo: should we also insist that val be non-null here? 
           Probably. */

        keylen = strlen (key);
        if (preserve && ((apr_hash_get (ht, key, keylen)) != NULL))
          continue;
        else
          apr_hash_set (ht, key, keylen, 
                        val ? svn_string_create (val, pool) : NULL);
      }
}


apr_hash_t *
svn_xml_ap_to_hash (va_list ap, apr_pool_t *pool)
{
  apr_hash_t *ht = apr_make_hash (pool);
  const char *key;
  
  while ((key = va_arg (ap, char *)) != NULL)
    {
      svn_string_t *val = va_arg (ap, svn_string_t *);
      apr_hash_set (ht, key, strlen (key), val);
    }

  return ht;
}


apr_hash_t *
svn_xml_make_att_hash (const char **atts, apr_pool_t *pool)
{
  apr_hash_t *ht = apr_make_hash (pool);
  amalgamate (atts, ht, 0, pool);  /* third arg irrelevant in this case */
  return ht;
}


void
svn_xml_hash_atts_overlaying (const char **atts,
                              apr_hash_t *ht,
                              apr_pool_t *pool)
{
  amalgamate (atts, ht, 0, pool);
}


void
svn_xml_hash_atts_preserving (const char **atts,
                              apr_hash_t *ht,
                              apr_pool_t *pool)
{
  amalgamate (atts, ht, 1, pool);
}



/*** Making XML tags. ***/


void
svn_xml_make_open_tag_hash (svn_string_t **str,
                            apr_pool_t *pool,
                            enum svn_xml_open_tag_style style,
                            const char *tagname,
                            apr_hash_t *attributes)
{
  apr_hash_index_t *hi;

  if (*str == NULL)
    *str = svn_string_create ("", pool);

  svn_string_appendcstr (*str, "<", pool);
  svn_string_appendcstr (*str, tagname, pool);

  for (hi = apr_hash_first (attributes); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      void *val;
      size_t keylen;

      apr_hash_this (hi, &key, &keylen, &val);
      assert (val != NULL);

      svn_string_appendcstr (*str, "\n   ", pool);
      svn_string_appendcstr (*str, (char *) key, pool);
      svn_string_appendcstr (*str, "=\"", pool);
      svn_xml_escape_string (str, (svn_string_t *) val, pool);
      svn_string_appendcstr (*str, "\"", pool);
    }

  if (style == svn_xml_self_closing)
    svn_string_appendcstr (*str, "/", pool);
  svn_string_appendcstr (*str, ">", pool);
  if (style != svn_xml_protect_pcdata)
    svn_string_appendcstr (*str, "\n", pool);
}


void
svn_xml_make_open_tag_v (svn_string_t **str,
                         apr_pool_t *pool,
                         enum svn_xml_open_tag_style style,
                         const char *tagname,
                         va_list ap)
{
  apr_pool_t *subpool = svn_pool_create (pool);
  apr_hash_t *ht = svn_xml_ap_to_hash (ap, subpool);

  svn_xml_make_open_tag_hash (str, pool, style, tagname, ht);
  apr_destroy_pool (subpool);
}



void
svn_xml_make_open_tag (svn_string_t **str,
                       apr_pool_t *pool,
                       enum svn_xml_open_tag_style style,
                       const char *tagname,
                       ...)
{
  va_list ap;

  va_start (ap, tagname);
  svn_xml_make_open_tag_v (str, pool, style, tagname, ap);
  va_end (ap);
}


void svn_xml_make_close_tag (svn_string_t **str,
                             apr_pool_t *pool,
                             const char *tagname)
{
  if (*str == NULL)
    *str = svn_string_create ("", pool);

  svn_string_appendcstr (*str, "</", pool);
  svn_string_appendcstr (*str, tagname, pool);
  svn_string_appendcstr (*str, ">\n", pool);
}



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */
