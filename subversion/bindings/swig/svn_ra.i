/*
 * svn_ra.i :  SWIG interface file for svn_ra.h
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
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

#ifdef SWIGPERL
%module "SVN::_Ra"
#else
%module ra
#endif

%include typemaps.i

%import apr.i
%import svn_types.i
%import svn_string.i
%import svn_delta.i

/* bad pool convention, also these should not be public interface at all
   as commented by sussman. */
%ignore svn_ra_svn_init;
%ignore svn_ra_local_init;
%ignore svn_ra_dav_init;

#ifdef SWIGJAVA
/* Ignore these function pointer members because swig's string
   representations of their types approach the maximum path
   length on windows, causing swig to crash when it outputs 
   java wrapper classes for them. */
%ignore svn_ra_plugin_t::do_diff;
%ignore svn_ra_plugin_t::do_switch;
%ignore svn_ra_plugin_t::do_status;
%ignore svn_ra_plugin_t::do_update;
%ignore svn_ra_plugin_t::get_log;
#endif

/* -----------------------------------------------------------------------
   %apply-ing of typemaps defined elsewhere
*/
%apply SWIGTYPE **OUTPARAM {
    svn_ra_plugin_t **,
    svn_ra_session_t **,
    const svn_ra_reporter2_t **reporter,
    void **report_baton
};

%apply apr_hash_t **PROPHASH { apr_hash_t **props };

#ifdef SWIGPYTHON
%apply svn_stream_t *WRAPPED_STREAM { svn_stream_t * };
#endif

/* -----------------------------------------------------------------------
   thunk ra_callback
*/
%apply const char **OUTPUT {
    const char **url,
    const char **uuid
};

%apply const apr_array_header_t *STRINGLIST {
    const apr_array_header_t *paths
};

%typemap(perl5, in) (const svn_delta_editor_t *update_editor,
		     void *update_baton) {
    svn_delta_make_editor(&$1, &$2, $input, _global_pool);
}
%typemap(perl5, in) (const svn_delta_editor_t *diff_editor,
		     void *diff_baton) {
    svn_delta_make_editor(&$1, &$2, $input, _global_pool);
}

%typemap(perl5, in) (const svn_ra_callbacks_t *callbacks,
		     void *callback_baton) {
    svn_ra_make_callbacks(&$1, &$2, $input, _global_pool);
}

%typemap(perl5, in) apr_hash_t *config {
    $1 = svn_swig_pl_objs_to_hash_by_name ($input, "svn_config_t *",
					   _global_pool);
}

%typemap(perl5, in) apr_hash_t *lock_tokens {
    $1 = svn_swig_pl_strings_to_hash ($input, _global_pool);
}

/* ----------------------------------------------------------------------- */

%{
#include "svn_ra.h"

#ifdef SWIGPYTHON
#include "swigutil_py.h"
#endif

#ifdef SWIGJAVA
#include "swigutil_java.h"
#endif

#ifdef SWIGPERL
#include "swigutil_pl.h"
#endif

#ifdef SWIGRUBY
#include "swigutil_rb.h"
#endif
%}

%include svn_ra.h

#ifdef SWIGPERL
%include ra_reporter.hi
#endif
