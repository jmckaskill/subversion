/*
 * util.i :  SWIG interface file for various SVN and APR utilities
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

%module _util

%include typemaps.i

%import apr.i
%import svn_types.i
%import svn_string.i

/* ----------------------------------------------------------------------- 
   completely ignore a number of functions. the presumption is that the
   scripting language already has facilities for these things (or they
   are relatively trivial).
*/
%ignore svn_io_check_path;
%ignore svn_io_copy_file;
%ignore svn_io_copy_dir_recursively;
%ignore svn_io_append_file;
%ignore svn_io_read_length_line;
%ignore svn_io_file_affected_time;
%ignore svn_io_fd_from_file;
%ignore svn_io_get_dirents;
%ignore svn_io_run_cmd;

%ignore apr_check_dir_empty;
%ignore apr_dir_remove_recursively;

/* ### probably want to keep this one. disabling for now cuz of the
   ### input stringbuf and its "which pool?" problem. */
%ignore svn_io_open_unique_file;

/* -----------------------------------------------------------------------
   apr_size_t * is always an IN/OUT parameter in svn_io.h
*/
%apply apr_size_t *INOUT { apr_size_t * };

/* -----------------------------------------------------------------------
   handle the MIME type return value of svn_io_detect_mimetype()
*/
%apply const char **OUTPUT { const char ** };

/* -----------------------------------------------------------------------
   fix up the svn_stream_read() ptr/len arguments
*/
%typemap(python, in) (char *buffer, apr_size_t *len) ($*2_type temp) {
    if (!PyInt_Check($input)) {
        PyErr_SetString(PyExc_TypeError,
                        "expecting an integer for the buffer size");
        return NULL;
    }
    temp = PyInt_AsLong($input);
    if (temp < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "buffer size must be a positive integer");
        return NULL;
    }
    $1 = malloc(temp);
    $2 = ($2_ltype)&temp;
}

%typemap(python, argout) (char *buffer, apr_size_t *len) {
    $result = t_output_helper($result, PyString_FromStringAndSize($1, *$2));
    free($1);
}

/* -----------------------------------------------------------------------
   fix up the svn_stream_write() ptr/len arguments
*/
%typemap(python, in) (const char *data, apr_size_t *len) ($*2_type temp) {
    if (!PyString_Check($input)) {
        PyErr_SetString(PyExc_TypeError,
                        "expecting a string for the buffer");
        return NULL;
    }
    $1 = PyString_AS_STRING($input);
    temp = PyString_GET_SIZE($input);
    $2 = ($2_ltype)&temp;
}

%typemap(python, argout) (const char *data, apr_size_t *len) {
    $result = t_output_helper($result, PyInt_FromLong(*$2));
}

/* -----------------------------------------------------------------------
   describe how to pass a FILE* as a parameter (svn_stream_from_stdio)
*/
%typemap(python, in) FILE * {
    $1 = PyFile_AsFile($input);
    if ($1 == NULL) {
        PyErr_SetString(PyExc_ValueError, "Must pass in a valid file object");
        return NULL;
    }
}

/* -----------------------------------------------------------------------
   wrap some specific APR functionality
*/

apr_status_t apr_initialize(void);
void apr_terminate(void);

apr_status_t apr_ansi_time_to_apr_time(apr_time_t *result, time_t input);

/* ----------------------------------------------------------------------- */

%include svn_io.h
%include svn_pools.h
%include svn_version.h
%include svn_time.h

%{
#include <apr.h>
#include <apr_general.h>

#include "svn_io.h"
#include "svn_pools.h"
#include "svn_version.h"
#include "svn_time.h"

#include "swigutil.h"
%}
