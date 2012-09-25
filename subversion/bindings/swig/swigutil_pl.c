/*
 * swigutil_py.c: utility functions for the SWIG Perl bindings
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
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

#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

#include <stdarg.h>

#include <apr.h>
#include <apr_general.h>
#include <apr_portable.h>

#include "svn_pools.h"
#include "svn_opt.h"

#include "swigutil_pl.h"

/* element convertors for perl -> c */
typedef void *(*pl_element_converter_t)(SV *value, void *ctx, apr_pool_t *pool);

static void *convert_pl_string (SV *value, void *dummy, apr_pool_t *pool)
{
    void **result = apr_palloc(pool, sizeof(void *));
    *result = SvPV_nolen (value);
    return *result;
}

static void *convert_pl_obj (SV *value, swig_type_info *tinfo, apr_pool_t *pool)
{
    void **result = apr_palloc(pool, sizeof(void *));
    if (SWIG_ConvertPtr(value, result, tinfo, 0) < 0) {
        croak("unable to convert from swig object");
    }
    return *result;
}

/* perl -> c hash convertors */
static apr_hash_t *svn_swig_pl_to_hash(SV *source,
                                       pl_element_converter_t cv,
                                       void *ctx, apr_pool_t *pool)
{
    apr_hash_t *hash;
    HV *h;
    char *key;
    I32 cnt, retlen;

    if (!(source && SvROK(source) && SvTYPE(SvRV(source)) == SVt_PVHV)) {
	return NULL;
    }

    hash = apr_hash_make (pool);
    h = (HV *)SvRV(source);
    cnt = hv_iterinit(h);
    while (cnt--) {
	SV* item = hv_iternextsv(h, &key, &retlen);
	void *val = cv (item, ctx, pool);
	apr_hash_set (hash, key, APR_HASH_KEY_STRING, val);
    }

    return hash;
}

apr_hash_t *svn_swig_pl_objs_to_hash(SV *source, swig_type_info *tinfo,
                                     apr_pool_t *pool)
{

    return svn_swig_pl_to_hash(source, (pl_element_converter_t)convert_pl_obj,
                               tinfo, pool);
}

apr_hash_t *svn_swig_pl_strings_to_hash(SV *source, apr_pool_t *pool)
{

    return svn_swig_pl_to_hash(source, convert_pl_string, NULL, pool);
}


apr_hash_t *svn_swig_pl_objs_to_hash_by_name(SV *source,
                                             const char *typename,
                                             apr_pool_t *pool)
{
    swig_type_info *tinfo = SWIG_TypeQuery(typename);
    return svn_swig_pl_objs_to_hash (source, tinfo, pool);
}

/* perl -> c array convertors */
static const apr_array_header_t *svn_swig_pl_to_array (SV *source,
                                                       pl_element_converter_t cv,
                                                       void *ctx, apr_pool_t *pool)
{
    int targlen;
    apr_array_header_t *temp;
    AV* array;

    if (SvROK(source) && SvTYPE(SvRV(source)) == SVt_PVAV) {
        array = (AV *)SvRV (source);
        targlen = av_len (array) + 1;
        temp = apr_array_make (pool, targlen, sizeof(const char *));
        temp->nelts = targlen;

        while (targlen--) {
            /* more error handling here */
            SV **item = av_fetch (array, targlen, 0);
            APR_ARRAY_IDX(temp, targlen, const char *) = cv (*item, ctx, pool);
        }
    } else if (SvOK(source)) {
        targlen = 1;
        temp = apr_array_make (pool, targlen, sizeof(const char *));
        temp->nelts = targlen;
        APR_ARRAY_IDX(temp, 0, const char *) = cv (source, ctx, pool);
    } else {
        croak("Must pass a single value or an array reference");
    }

    return temp;
}

const apr_array_header_t *svn_swig_pl_strings_to_array(SV *source,
                                                       apr_pool_t *pool)
{
    return svn_swig_pl_to_array (source, convert_pl_string, NULL, pool);
}

const apr_array_header_t *svn_swig_pl_objs_to_array(SV *source,
						    swig_type_info *tinfo,
						    apr_pool_t *pool)
{
    return svn_swig_pl_to_array (source,
                                 (pl_element_converter_t)convert_pl_obj,
                                 tinfo, pool);
}

/* element convertors for c -> perl */
typedef SV *(*element_converter_t)(void *value, void *ctx);

static SV *convert_string (const char *value, void *dummy)
{
    SV *obj = sv_2mortal(newSVpv(value, 0));
    return obj;
}

static SV *convert_svn_string_t (svn_string_t *value, void *dummy)
{
    SV *obj = sv_2mortal(newSVpv(value->data, value->len));
    return obj;
}

static SV *convert_to_swig_type (void *ptr, swig_type_info *tinfo)
{
    SV *obj = sv_newmortal();
    SWIG_MakePtr(obj, ptr, tinfo, 0);
    return obj;
}

static SV *convert_int(int value, void *dummy)
{
    return newSViv (value);
}

/* c -> perl hash convertors */
static SV *convert_hash (apr_hash_t *hash, element_converter_t converter_func,
		  void *ctx)
{
    apr_hash_index_t *hi;
    HV *hv;

    hv = newHV();
    for (hi = apr_hash_first(NULL, hash); hi; hi = apr_hash_next(hi)) {
	const char *key;
	void *val;
	int klen;
	SV *obj;

	apr_hash_this(hi, (void *)&key, NULL, &val);
	klen = strlen(key);

	obj = converter_func (val, ctx);
	hv_store(hv, (const char *)key, klen, obj, 0);
	SvREFCNT_inc(obj);
    }
    
    return newRV_inc((SV*)hv);
}

SV *svn_swig_pl_prophash_to_hash (apr_hash_t *hash)
{
    return convert_hash (hash, (element_converter_t)convert_svn_string_t,
			 NULL);
}

SV *svn_swig_pl_convert_hash (apr_hash_t *hash, swig_type_info *tinfo)
{
    return convert_hash (hash, (element_converter_t)convert_to_swig_type,
			 tinfo);
}

/* c -> perl array convertors */
static SV *convert_array(const apr_array_header_t *array,
		  element_converter_t converter_func, void *ctx)
{
    AV *list = newAV();
    int i;

    for (i = 0; i < array->nelts; ++i) {
	void *element = APR_ARRAY_IDX(array, i, void *);
	SV *item = converter_func (element, ctx);
	av_push (list, item);
	SvREFCNT_inc (item);
    }
    return newRV_inc((SV*)list);
}

SV *svn_swig_pl_array_to_list(const apr_array_header_t *array)
{
    return convert_array (array, (element_converter_t)convert_string, NULL);
}

SV *svn_swig_pl_ints_to_list(const apr_array_header_t *array)
{
    return convert_array (array, (element_converter_t)convert_int, NULL);
}

SV *svn_swig_pl_convert_array(const apr_array_header_t *array,
                              swig_type_info *tinfo)
{
    return convert_array (array, (element_converter_t)convert_to_swig_type,
                          tinfo);		          
}

/* put the va_arg in stack and invoke caller_func with func.
   fmt:
   * O: perl object
   * i: apr_int32_t
   * u: apr_uint32_t
   * L: apr_int64_t
   * U: apr_uint64_t
   * s: string
   * S: swigtype
   * r: svn_revnum_t
   * b: svn_boolean_t
   * z: apr_size_t
   
   Please do not add C types here.  Add a new format code if needed.
   Using the underlying C types and not the APR or SVN types can break
   things if these data types change in the future or on platforms which
   use different types.

   put returned value in result if result is not NULL
*/

svn_error_t *svn_swig_pl_callback_thunk (perl_func_invoker_t caller_func,
					 void *func,
					 SV **result,
					 const char *fmt, ...)
{
    const char *fp = fmt;
    va_list ap;
    int count;
    I32 call_flags = result ? G_SCALAR : (G_VOID & G_DISCARD);

    dSP ;
    ENTER ;
    SAVETMPS ;

    PUSHMARK(SP) ;

    va_start(ap, fmt);
    while (*fp) {
	char *c;
	void *o;
	SV *obj;
	swig_type_info *t;

	switch (*fp++) {
	case 'O':
	    XPUSHs (va_arg (ap, SV *));
	    break;
	case 'S': /* swig object */
	    o = va_arg (ap, void *);
	    t = va_arg (ap, swig_type_info *);
  
	    obj = sv_newmortal ();
	    SWIG_MakePtr (obj, o, t, 0);
	    XPUSHs(obj);
	    break;

	case 's': /* string */
	    c = va_arg (ap, char *);
	    XPUSHs(c ? sv_2mortal(newSVpv(c, 0)) : &PL_sv_undef);
	    break;

	case 'i': /* apr_int32_t */
	    XPUSHs(sv_2mortal(newSViv(va_arg(ap, apr_int32_t))));
	    break;

	case 'u': /* apr_uint32_t */
            XPUSHs(sv_2mortal(newSViv(va_arg(ap, apr_uint32_t))));
	    break;

	case 'r': /* svn_revnum_t */
	    XPUSHs(sv_2mortal(newSViv(va_arg(ap, svn_revnum_t))));
	    break;

	case 'b': /* svn_boolean_t */
	    XPUSHs(sv_2mortal(newSViv(va_arg(ap, svn_boolean_t))));
	    break;
	    
	case 'L': /* apr_int64_t */
	    /* Pass into perl as a string because some implementations may
	     * not be able to handle a 64-bit int.  If it's too long to
	     * fit in Perl's interal IV size then perl will only make
	     * it available as a string.  If not then perl will convert
	     * it to an IV for us.  So this handles the problem gracefully */
	    c = malloc(30);
	    snprintf(c,30,"%" APR_INT64_T_FMT,va_arg(ap, apr_int64_t));
	    XPUSHs(sv_2mortal(newSVpv(c, 0)));
	    free(c);
	    break;
	    
	case 'U': /* apr_uint64_t */
	    c = malloc(30);
	    snprintf(c,30,"%" APR_UINT64_T_FMT,va_arg(ap, apr_uint64_t));
	    XPUSHs(sv_2mortal(newSVpv(c, 0)));
	    free(c);
	    break;

	case 'z': /* apr_size_t */
	    if (sizeof(apr_size_t) >= 8)
	      {
	        c = malloc(30);
	        snprintf(c,30,"%" APR_SIZE_T_FMT,va_arg(ap, apr_size_t));
	        XPUSHs(sv_2mortal(newSVpv(c, 0)));
	        free(c);
	      }
	    else
	      {
	        XPUSHs(sv_2mortal(newSViv(va_arg(ap, apr_size_t))));
	      }
	     break;
	}
    }

    va_end (ap);

    PUTBACK;
    switch (caller_func) {
    case CALL_SV:
	count = call_sv (func, call_flags );
	break;
    case CALL_METHOD:
	count = call_method (func, call_flags );
	break;
    default:
	croak ("unkonwn calling type");
	break;
    }
    SPAGAIN ;

    if (((call_flags & G_SCALAR) && count != 1) ||
	((call_flags & G_VOID) && count != 0))
	    croak ("Wrong number of returns");

    if (result) {
	*result = POPs;
	SvREFCNT_inc(*result);
    }

    FREETMPS ;
    LEAVE ;

    return SVN_NO_ERROR;
}

/*** Editor Wrapping ***/

/* this could be more perlish */
typedef struct {
    SV *editor;     /* the editor handling the callbacks */
    SV *baton;      /* the dir/file baton (or NULL for edit baton) */
} item_baton;

static item_baton * make_baton(apr_pool_t *pool,
                               SV *editor, SV *baton)
{
    item_baton *newb = apr_palloc(pool, sizeof(*newb));

    SvREFCNT_inc(editor);

    newb->editor = editor;
    newb->baton = baton;

    return newb;
}

static svn_error_t * close_baton(void *baton, const char *method)
{
    item_baton *ib = baton;
    dSP ;

    ENTER ;
    SAVETMPS ;

    PUSHMARK(SP) ;
    XPUSHs(ib->editor);

    if (ib->baton)
	XPUSHs(ib->baton);

    PUTBACK;

    call_method(method, G_DISCARD);

    /* check result? */

    SvREFCNT_dec(ib->editor);
    if (ib->baton)
	SvREFCNT_dec(ib->baton);

#ifdef SVN_DEBUG
    ib->editor = ib->baton = NULL;
#endif

    FREETMPS ;
    LEAVE ;

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_set_target_revision(void *edit_baton,
                                               svn_revnum_t target_revision,
                                               apr_pool_t *pool)
{
    item_baton *ib = edit_baton;

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"set_target_revision", NULL,
				         "Or", ib->editor, target_revision));

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_open_root(void *edit_baton,
                                     svn_revnum_t base_revision,
                                     apr_pool_t *dir_pool,
                                     void **root_baton)
{
    item_baton *ib = edit_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    SV *result;

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"open_root", &result,
				         "OrS", ib->editor, base_revision,
				         dir_pool, poolinfo));

    *root_baton = make_baton(dir_pool, ib->editor, result);
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_delete_entry(const char *path,
                                        svn_revnum_t revision,
                                        void *parent_baton,
                                        apr_pool_t *pool)
{
    item_baton *ib = parent_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"delete_entry", NULL,
				         "OsrOS", ib->editor, path, revision,
				         ib->baton, pool, poolinfo));
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_add_directory(const char *path,
                                         void *parent_baton,
                                         const char *copyfrom_path,
                                         svn_revnum_t copyfrom_revision,
                                         apr_pool_t *dir_pool,
                                         void **child_baton)
{
    item_baton *ib = parent_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    SV *result;

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"add_directory", &result,
				         "OsOsrS", ib->editor, path, ib->baton,
				         copyfrom_path, copyfrom_revision, 
				         dir_pool, poolinfo));
    *child_baton = make_baton(dir_pool, ib->editor, result);
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_open_directory(const char *path,
                                          void *parent_baton,
                                          svn_revnum_t base_revision,
                                          apr_pool_t *dir_pool,
                                          void **child_baton)
{
    item_baton *ib = parent_baton;
    SV *result;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"open_directory", &result,
				         "OsOrS", ib->editor, path, ib->baton,
				         base_revision, dir_pool, poolinfo));

    *child_baton = make_baton(dir_pool, ib->editor, result);

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_change_dir_prop(void *dir_baton,
                                           const char *name,
                                           const svn_string_t *value,
                                           apr_pool_t *pool)
{
    item_baton *ib = dir_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"change_dir_prop", NULL,
				         "OOssS", ib->editor, ib->baton, name,
				         value ? value->data : NULL,
				         pool, poolinfo));

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_close_directory(void *dir_baton,
                                           apr_pool_t *pool)
{
    return close_baton(dir_baton, "close_directory");
}

static svn_error_t * thunk_absent_directory(const char *path,
					    void *parent_baton,
					    apr_pool_t *pool)
{
    item_baton *ib = parent_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
	   			         (void *)"absent_directory", NULL,
				         "OsOS", ib->editor, path, ib->baton,
				         pool, poolinfo));

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_add_file(const char *path,
                                    void *parent_baton,
                                    const char *copyfrom_path,
                                    svn_revnum_t copyfrom_revision,
                                    apr_pool_t *file_pool,
                                    void **file_baton)
{
    item_baton *ib = parent_baton;
    SV *result;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"add_file", &result,
				         "OsOsrS", ib->editor, path, ib->baton,
				         copyfrom_path, copyfrom_revision,
				         file_pool, poolinfo));

    *file_baton = make_baton(file_pool, ib->editor, result);
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_open_file(const char *path,
                                     void *parent_baton,
                                     svn_revnum_t base_revision,
                                     apr_pool_t *file_pool,
                                     void **file_baton)
{
    item_baton *ib = parent_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    SV *result;

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"open_file", &result,
				         "OsOrS", ib->editor, path, ib->baton,
				         base_revision, file_pool, poolinfo));

    *file_baton = make_baton(file_pool, ib->editor, result);
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_window_handler(svn_txdelta_window_t *window,
                                          void *baton)
{
    SV *handler = baton;

    if (window == NULL) {
	SVN_ERR (svn_swig_pl_callback_thunk (CALL_SV,
				             handler, NULL, "O",
				             &PL_sv_undef));
    }
    else {
	swig_type_info *tinfo = SWIG_TypeQuery("svn_txdelta_window_t *");
	SVN_ERR (svn_swig_pl_callback_thunk (CALL_SV, handler,
				             NULL, "S", window, tinfo));
    }

    return SVN_NO_ERROR;
}

static svn_error_t *
thunk_apply_textdelta(void *file_baton, 
                      const char *base_checksum,
                      apr_pool_t *pool,
                      svn_txdelta_window_handler_t *handler,
                      void **h_baton)
{
    item_baton *ib = file_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    SV *result;

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"apply_textdelta", &result,
				         "OOsS", ib->editor, ib->baton,
					 base_checksum, pool, poolinfo));
    if (SvOK(result)) {
	if (SvROK(result) && SvTYPE(SvRV(result)) == SVt_PVAV) {
	    swig_type_info *handler_info = SWIG_TypeQuery("svn_txdelta_window_handler_t"), *void_info = SWIG_TypeQuery("void *");
	    AV *array = (AV *)SvRV(result);

	    if (SWIG_ConvertPtr(*av_fetch (array, 0, 0),
				(void **)handler, handler_info,0) < 0) {
		croak("Unable to convert from SWIG Type");
	    }
	    if (SWIG_ConvertPtr(*av_fetch (array, 1, 0),
				h_baton, void_info,0) < 0) {
		croak("Unable to convert from SWIG Type ");
	    }
	}
	else {
	    *handler = thunk_window_handler;
	    *h_baton = result;
	}
    }
    else {
	*handler = svn_delta_noop_window_handler;
	*h_baton = NULL;
    }

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_change_file_prop(void *file_baton,
                                            const char *name,
                                            const svn_string_t *value,
                                            apr_pool_t *pool)
{
    item_baton *ib = file_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"change_file_prop", NULL,
				         "OOssS", ib->editor, ib->baton, name,
				         value ? value->data : NULL,
				         pool, poolinfo));
  
    return SVN_NO_ERROR;
}

static svn_error_t * thunk_close_file(void *file_baton,
                                      const char *text_checksum,
                                      apr_pool_t *pool)
{
    item_baton *ib = file_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"close_file", NULL, "OOsS",
				         ib->editor, ib->baton, text_checksum,
				         pool, poolinfo));

    SvREFCNT_dec(ib->editor);
    SvREFCNT_dec(ib->baton);

#ifdef SVN_DEBUG
    ib->editor = ib->baton = NULL;
#endif

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_absent_file(const char *path,
				       void *parent_baton,
				       apr_pool_t *pool)
{
    item_baton *ib = parent_baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    SVN_ERR (svn_swig_pl_callback_thunk (CALL_METHOD,
				         (void *)"absent_file", NULL,
				         "OsOS", ib->editor, path, ib->baton,
				         pool, poolinfo));

    return SVN_NO_ERROR;
}

static svn_error_t * thunk_close_edit(void *edit_baton,
                                      apr_pool_t *pool)
{
    return close_baton(edit_baton, "close_edit");
}

static svn_error_t * thunk_abort_edit(void *edit_baton,
                                      apr_pool_t *pool)
{
    return close_baton(edit_baton, "abort_edit");
}

void svn_delta_make_editor(svn_delta_editor_t **editor,
			   void **edit_baton,
			   SV *perl_editor,
			   apr_pool_t *pool)
{
    svn_delta_editor_t *thunk_editor = svn_delta_default_editor (pool);
  
    thunk_editor->set_target_revision = thunk_set_target_revision;
    thunk_editor->open_root = thunk_open_root;
    thunk_editor->delete_entry = thunk_delete_entry;
    thunk_editor->add_directory = thunk_add_directory;
    thunk_editor->open_directory = thunk_open_directory;
    thunk_editor->change_dir_prop = thunk_change_dir_prop;
    thunk_editor->close_directory = thunk_close_directory;
    thunk_editor->absent_directory = thunk_absent_directory;
    thunk_editor->add_file = thunk_add_file;
    thunk_editor->open_file = thunk_open_file;
    thunk_editor->apply_textdelta = thunk_apply_textdelta;
    thunk_editor->change_file_prop = thunk_change_file_prop;
    thunk_editor->close_file = thunk_close_file;
    thunk_editor->absent_file = thunk_absent_file;
    thunk_editor->close_edit = thunk_close_edit;
    thunk_editor->abort_edit = thunk_abort_edit;

    *editor = thunk_editor;
    *edit_baton = make_baton(pool, perl_editor, NULL);
}

svn_error_t *svn_swig_pl_thunk_log_receiver(void *baton,
					    apr_hash_t *changed_paths,
					    svn_revnum_t rev,
					    const char *author,
					    const char *date,
					    const char *msg,
					    apr_pool_t *pool)
{
    SV *receiver = baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    swig_type_info *tinfo = SWIG_TypeQuery("svn_log_changed_path_t *");

    if (!SvOK(receiver))
	return SVN_NO_ERROR;

    svn_swig_pl_callback_thunk (CALL_SV,
			        receiver, NULL,
			        "OrsssS", (changed_paths) ?
			        svn_swig_pl_convert_hash(changed_paths, tinfo)
			        : &PL_sv_undef,
			        rev, author, date, msg, pool, poolinfo);

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_history_func(void *baton,
                                            const char *path,
                                            svn_revnum_t revision,
                                            apr_pool_t *pool)
{
    SV *func = baton;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");

    if (!SvOK(func))
	return SVN_NO_ERROR;

    svn_swig_pl_callback_thunk (CALL_SV,
			        func, NULL,
			        "srS", path, revision, pool, poolinfo);

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_authz_func (svn_boolean_t *allowed,
                                           svn_fs_root_t *root,
                                           const char *path,
                                           void *baton,
                                           apr_pool_t *pool)
{
    SV *func = baton, *result;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
    swig_type_info *rootinfo = SWIG_TypeQuery("svn_fs_root_t *");

    if (!SvOK(func))
	return SVN_NO_ERROR;

    svn_swig_pl_callback_thunk (CALL_SV,
			        func, &result,
			        "SsS", root, rootinfo, path, pool, poolinfo);

    *allowed = SvIV (result);
    SvREFCNT_dec (result);

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_commit_callback(svn_revnum_t new_revision,
					       const char *date,
					       const char *author,
					       void *baton)
{
    if (!SvOK((SV *)baton))
	return SVN_NO_ERROR;

    svn_swig_pl_callback_thunk (CALL_SV, baton, NULL,
			        "rss", new_revision, date, author);

    return SVN_NO_ERROR;
}

/* Wrap RA */

static svn_error_t * thunk_open_tmp_file(apr_file_t **fp,
					 void *callback_baton,
					 apr_pool_t *pool)
{
    SV *result;
    swig_type_info *tinfo = SWIG_TypeQuery("apr_file_t *");

    svn_swig_pl_callback_thunk (CALL_METHOD, (void *)"open_tmp_file",
			        &result, "O", callback_baton);

    if (SWIG_ConvertPtr(result, (void *)fp, tinfo,0) < 0) {
	croak("Unable to convert from SWIG Type");
    }

    return SVN_NO_ERROR;
}

svn_error_t *thunk_get_wc_prop (void *baton,
				const char *relpath,
				const char *name,
				const svn_string_t **value,
				apr_pool_t *pool)
{
    SV *result;
    swig_type_info *tinfo = SWIG_TypeQuery("apr_pool_t *");

    svn_swig_pl_callback_thunk (CALL_METHOD, (void *)"get_wc_prop",
			        &result, "OssS", baton, relpath, name,
			       	pool, tinfo);

    /* this is svn_string_t * typemap in */
    if (!SvOK (result) || result == &PL_sv_undef) {
	*value = NULL;
    }
    else if (SvPOK(result)) {
	*value = svn_string_create (SvPV_nolen (result), pool);
    }
    else {
	SvREFCNT_dec(result);
	croak("not a string");
    }

    SvREFCNT_dec(result);
    return SVN_NO_ERROR;
}


svn_error_t *svn_ra_make_callbacks(svn_ra_callbacks_t **cb,
				   void **c_baton,
				   SV *perl_callbacks,
				   apr_pool_t *pool)
{
    swig_type_info *tinfo = SWIG_TypeQuery("svn_auth_baton_t *");
    SV *auth_baton;

    *cb = apr_pcalloc (pool, sizeof(**cb));

    (*cb)->open_tmp_file = thunk_open_tmp_file;
    (*cb)->get_wc_prop = thunk_get_wc_prop;
    (*cb)->set_wc_prop = NULL;
    (*cb)->push_wc_prop = NULL;
    (*cb)->invalidate_wc_props = NULL;
    auth_baton = *hv_fetch((HV *)SvRV(perl_callbacks), "auth", 4, 0);

    if (SWIG_ConvertPtr(auth_baton, (void **)&(*cb)->auth_baton, tinfo,0) < 0) {
	croak("Unable to convert from SWIG Type");
    }
    *c_baton = perl_callbacks;
    SvREFCNT_inc(perl_callbacks);

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_simple_prompt(svn_auth_cred_simple_t **cred,
                                             void *baton,
                                             const char *realm,
                                             const char *username,
                                             svn_boolean_t may_save,
                                             apr_pool_t *pool)
{
    swig_type_info *poolinfo = SWIG_TypeQuery ("apr_pool_t *");
    swig_type_info *credinfo = SWIG_TypeQuery ("svn_auth_cred_simple_t *");

    /* Be nice and allocate the memory for the cred structure before passing it
     * off to the perl space */
    *cred = apr_pcalloc (pool, sizeof (**cred));
    if (!*cred) {
        croak ("Could not allocate memory for cred structure");
    }
    svn_swig_pl_callback_thunk (CALL_SV,
                                baton, NULL,
                                "SssbS", *cred, credinfo,
                                realm, username, may_save, pool, poolinfo);

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_username_prompt(svn_auth_cred_username_t **cred,
                                               void *baton,
                                               const char *realm,
                                               svn_boolean_t may_save,
                                               apr_pool_t *pool)
{
    swig_type_info *poolinfo = SWIG_TypeQuery ("apr_pool_t *");
    swig_type_info *credinfo = SWIG_TypeQuery ("svn_auth_cred_username_t *");

    /* Be nice and allocate the memory for the cred structure before passing it
     * off to the perl space */
    *cred = apr_pcalloc (pool, sizeof (**cred));
    if (!*cred) {
        croak ("Could not allocate memory for cred structure");
    }
    svn_swig_pl_callback_thunk (CALL_SV,
                                baton, NULL,
                                "SsbS", *cred, credinfo,
                                realm, may_save, pool, poolinfo);

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_ssl_server_trust_prompt(
                              svn_auth_cred_ssl_server_trust_t **cred,
                              void *baton,
                              const char *realm,
                              apr_uint32_t failures,
                              const svn_auth_ssl_server_cert_info_t *cert_info,
                              svn_boolean_t may_save,
                              apr_pool_t *pool)
{
    swig_type_info *poolinfo = SWIG_TypeQuery ("apr_pool_t *");
    swig_type_info *credinfo = SWIG_TypeQuery (
                                 "svn_auth_cred_ssl_server_trust_t *");
    swig_type_info *cert_info_info = SWIG_TypeQuery (
                                 "svn_auth_ssl_server_cert_info_t *");

    /* Be nice and allocate the memory for the cred structure before passing it
     * off to the perl space */
    *cred = apr_pcalloc (pool, sizeof (**cred));
    if (!*cred) {
        croak ("Could not allocate memory for cred structure");
    }
    svn_swig_pl_callback_thunk (CALL_SV,
                                baton, NULL,
                                "SsiSbS", *cred, credinfo,
                                realm, failures, 
                                cert_info, cert_info_info,
                                may_save, pool, poolinfo);

    /* Allow the perl callback to indicate failure by setting all vars to 0 
     * or by simply doing nothing.  While still allowing them to indicate
     * failure by setting the cred strucutre's pointer to 0 via $$cred = 0 */
    if (*cred) {
        if ((*cred)->may_save == 0 && (*cred)->accepted_failures == 0) {
            *cred = NULL;
        }
    }

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_ssl_client_cert_prompt(
                svn_auth_cred_ssl_client_cert_t **cred,
                void *baton,
                const char * realm,
                svn_boolean_t may_save,
                apr_pool_t *pool)
{
    swig_type_info *poolinfo = SWIG_TypeQuery ("apr_pool_t *");
    swig_type_info *credinfo = SWIG_TypeQuery (
                                 "svn_auth_cred_ssl_client_cert_t *");
    
    /* Be nice and allocate the memory for the cred structure before passing it
     * off to the perl space */
    *cred = apr_pcalloc (pool, sizeof (**cred));
    if (!*cred) {
        croak ("Could not allocate memory for cred structure");
    }
    svn_swig_pl_callback_thunk (CALL_SV,
                                baton, NULL,
                                "SsbS", *cred, credinfo,
                                realm, may_save, pool, poolinfo);

    return SVN_NO_ERROR;
}

svn_error_t *svn_swig_pl_thunk_ssl_client_cert_pw_prompt(
                                     svn_auth_cred_ssl_client_cert_pw_t **cred,
                                     void *baton,
                                     const char *realm,
                                     svn_boolean_t may_save,
                                     apr_pool_t *pool)
{
    swig_type_info *poolinfo = SWIG_TypeQuery ("apr_pool_t *");
    swig_type_info *credinfo = SWIG_TypeQuery (
                                 "svn_auth_cred_ssl_client_cert_pw_t *");

    /* Be nice and allocate the memory for the cred structure before passing it
     * off to the perl space */
    *cred = apr_pcalloc (pool, sizeof (**cred));
    if (!*cred) {
        croak ("Could not allocate memory for cred structure");
    }
    svn_swig_pl_callback_thunk (CALL_SV,
                                baton, NULL,
                                "SsbS", *cred, credinfo,
                                realm, may_save, pool, poolinfo);

    return SVN_NO_ERROR;
}

/* Thunked version of svn_wc_notify_func_t callback type */
void svn_swig_pl_notify_func(void * baton,
		             const char *path,
			     svn_wc_notify_action_t action,
			     svn_node_kind_t kind,
			     const char *mime_type,
			     svn_wc_notify_state_t content_state,
			     svn_wc_notify_state_t prop_state,
			     svn_revnum_t revision)
{
    if (!SvOK((SV *)baton)) {
        return; 
    }

    svn_swig_pl_callback_thunk (CALL_SV,
		                baton, NULL,
				"siisiir", path, action, kind, mime_type,
				content_state, prop_state, revision);
    
}

/* Thunked version of svn_client_get_commit_log_t callback type. */
svn_error_t *svn_swig_pl_get_commit_log_func(const char **log_msg,
                                             const char **tmp_file,
                                             apr_array_header_t *commit_items,
                                             void *baton,
                                             apr_pool_t *pool)
{
    SV *result;
    svn_error_t *ret_val = SVN_NO_ERROR;
    SV *log_msg_sv;
    SV *tmp_file_sv;
    SV *commit_items_sv;
    swig_type_info *commit_itemsinfo = SWIG_TypeQuery 
	                                ("svn_client_commit_item_t *");
    swig_type_info *poolinfo = SWIG_TypeQuery ("apr_pool_t *");

    if (!SvOK((SV *)baton)) {
        *log_msg = apr_pstrdup (pool, "");
	*tmp_file = NULL;
        return SVN_NO_ERROR;
    }
    
    log_msg_sv = newRV_noinc (sv_newmortal ());
    tmp_file_sv = newRV_noinc (sv_newmortal ());
    commit_items_sv = svn_swig_pl_convert_array (commit_items, 
			                         commit_itemsinfo);

    svn_swig_pl_callback_thunk (CALL_SV,
	                        baton, &result,
				"OOOS", log_msg_sv, tmp_file_sv,
				commit_items_sv, pool, poolinfo);

    if (!SvOK(SvRV(log_msg_sv))) {
        /* client returned undef to us */
        *log_msg = NULL;
    } else if (SvPOK(SvRV(log_msg_sv))) {
	/* client returned string so get the string and then duplicate
	 * it using pool memory */
        *log_msg = apr_pstrdup(pool, SvPV_nolen(SvRV(log_msg_sv)));
    } else {
        croak("Invalid value in log_msg reference, must be undef or a string");
    }

    if (!SvOK(SvRV(tmp_file_sv))) {
	*tmp_file = NULL;
    } else if (SvPOK(SvRV(tmp_file_sv))) {
	*tmp_file = apr_pstrdup(pool, SvPV_nolen(SvRV(tmp_file_sv)));
    } else {
        croak("Invalid value in tmp_file reference, must be undef or a string");    
    }

    if (sv_derived_from (result, "_p_svn_error_t")) {
        swig_type_info *errorinfo = SWIG_TypeQuery("svn_error_t *");
	if (SWIG_ConvertPtr(result, (void *)&ret_val, errorinfo, 0) < 0) {
            SvREFCNT_dec(result);
	    croak("Unable to convert from SWIG Type");
        }
    }

    SvREFCNT_dec(result);
    return ret_val;
}

/* Thunked version of svn_wc_cancel_func_t callback type. */
svn_error_t *svn_swig_pl_cancel_func(void *cancel_baton) {
    SV *result;
    svn_error_t *ret_val;

    if (!SvOK((SV *)cancel_baton)) {
        return SVN_NO_ERROR;
    }
    svn_swig_pl_callback_thunk(CALL_SV, cancel_baton, &result, "");

    if (sv_derived_from(result,"_p_svn_error_t")) { 
        swig_type_info *errorinfo = SWIG_TypeQuery("svn_error_t *");
	if (SWIG_ConvertPtr(result, (void *)&ret_val, errorinfo, 0) < 0) {
	    SvREFCNT_dec(result);
	    croak("Unable to convert from SWIG Type");
	}
    } else if (SvIOK(result) && SvIV(result)) {
        ret_val = svn_error_create(SVN_ERR_CANCELLED, NULL,"By cancel callback");
    } else if (SvTRUE(result) && SvPOK(result)) {
        ret_val = svn_error_create(SVN_ERR_CANCELLED, NULL, SvPV_nolen(result));
    } else {
        ret_val = SVN_NO_ERROR;
    }
    SvREFCNT_dec(result);
    return ret_val;
}

/* Thunked version of svn_wc_status_func_t callback type. */
void svn_swig_pl_status_func(void *baton,
                             const char *path,
                             svn_wc_status_t *status)
{
  swig_type_info *statusinfo = SWIG_TypeQuery ("svn_wc_status_t *");

  if (!SvOK ((SV *)baton)) {
    return;
  }
   
  svn_swig_pl_callback_thunk (CALL_SV, baton, NULL, "sS",
                              path, status, statusinfo); 

}

/* Thunked version of svn_client_blame_receiver_t callback type. */
svn_error_t *svn_swig_pl_blame_func (void *baton,
		                     apr_int64_t line_no,
				     svn_revnum_t revision,
				     const char *author,
				     const char *date,
				     const char *line,
				     apr_pool_t *pool)
{
    SV *result;
    svn_error_t *ret_val = SVN_NO_ERROR;
    swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
 
    svn_swig_pl_callback_thunk (CALL_SV, baton, &result, "LrsssS",
		                line_no, revision, author, date, line,
				pool, poolinfo);

    if (sv_derived_from (result, "_p_svn_error_t")) {
        swig_type_info *errorinfo = SWIG_TypeQuery("svn_error_t *");
	if (SWIG_ConvertPtr(result, (void *)&ret_val, errorinfo, 0) < 0) {
            SvREFCNT_dec(result);
	    croak("Unable to convert from SWIG Type");
        }
    }
    
    SvREFCNT_dec(result);
    return ret_val;
}

/* Thunked config enumerator */
svn_boolean_t svn_swig_pl_thunk_config_enumerator (const char *name, const char *value, void *baton)
{
    SV *result;
    if (!SvOK((SV *)baton))
	return 0;

    svn_swig_pl_callback_thunk (CALL_SV, baton, &result,
			        "ss", name, value);

    return SvOK(result);
}


/* default pool support */
apr_pool_t *current_pool;

apr_pool_t *svn_swig_pl_make_pool (SV *obj)
{
    apr_pool_t *pool;

    if (obj && sv_isobject (obj)) {
	swig_type_info *poolinfo = SWIG_TypeQuery("apr_pool_t *");
	if (sv_derived_from (obj, "SVN::Pool")) {
	    obj = SvRV(obj);
	}
	if (sv_derived_from(obj, "_p_apr_pool_t")) {
	    SWIG_ConvertPtr(obj, (void **)&pool, poolinfo, 0);
	    return pool;
	}
    }

    if (!current_pool)
	svn_swig_pl_callback_thunk (CALL_METHOD, (void *)"new_default",
			            &obj, "s", "SVN::Pool");
    pool = current_pool;

    return pool;
}

/* stream interpolability with io::handle */

typedef struct  {
    SV *obj;
    IO *io;
} io_baton_t;

static svn_error_t *io_handle_read (void *baton,
				    char *buffer,
				    apr_size_t *len)
{
    io_baton_t *io = baton;
    MAGIC *mg;

    if ((mg = SvTIED_mg((SV*)io->io, PERL_MAGIC_tiedscalar))) {
	SV *ret;
	SV *buf = sv_newmortal();

	svn_swig_pl_callback_thunk (CALL_METHOD, (void *)"READ", &ret, "OOz",
			            SvTIED_obj((SV*)io->io, mg),
			            buf, *len);
	*len = SvIV (ret);
	SvREFCNT_dec (ret);
	memmove (buffer, SvPV_nolen(buf), *len);
    }
    else
	*len = PerlIO_read (IoIFP (io->io), buffer, *len);
    return SVN_NO_ERROR;
}

static svn_error_t *io_handle_write (void *baton,
				     const char *data,
				     apr_size_t *len)
{
    io_baton_t *io = baton;
    MAGIC *mg;

    if ((mg = SvTIED_mg((SV*)io->io, PERL_MAGIC_tiedscalar))) {
	SV *ret, *pv;
        pv = sv_2mortal (newSVpvn (data, *len));
	svn_swig_pl_callback_thunk (CALL_METHOD, (void *)"WRITE", &ret, "OOz",
			            SvTIED_obj((SV*)io->io, mg), pv, *len);
	*len = SvIV (ret);
	SvREFCNT_dec (ret);
    }
    else
	*len = PerlIO_write (IoIFP (io->io), data, *len);
    return SVN_NO_ERROR;
}

static svn_error_t *io_handle_close (void *baton)
{
    io_baton_t *io = baton;
    MAGIC *mg;

    if ((mg = SvTIED_mg((SV*)io->io, PERL_MAGIC_tiedscalar))) {
	svn_swig_pl_callback_thunk (CALL_METHOD, (void *)"CLOSE", NULL, "O",
			            SvTIED_obj((SV*)io->io, mg));
    }
    else {
	PerlIO_close (IoIFP (io->io));
    }

    return SVN_NO_ERROR;
}

static apr_status_t io_handle_cleanup (void *baton)
{
    io_baton_t *io = baton;
    SvREFCNT_dec (io->obj);
    return APR_SUCCESS;
}

svn_error_t *svn_swig_pl_make_stream (svn_stream_t **stream, SV *obj)
{
    swig_type_info *tinfo = SWIG_TypeQuery("svn_stream_t *");
    IO *io;
    int simple_type = 1;

    if (!SvOK (obj)) {
        *stream = NULL;
        return SVN_NO_ERROR;
    }

    if (obj && sv_isobject(obj)) {
        if (sv_derived_from (obj, "SVN::Stream"))
	    svn_swig_pl_callback_thunk (CALL_METHOD, (void *)"svn_stream",
			                &obj, "O", obj);
	else if (!sv_derived_from(obj, "_p_svn_stream_t"))
            simple_type = 0;

        if (simple_type) {
            SWIG_ConvertPtr(obj, (void **)stream, tinfo, 0);
            return SVN_NO_ERROR;
        }
    }

    if (obj && SvROK(obj) && SvTYPE(SvRV(obj)) == SVt_PVGV &&
	(io = GvIO(SvRV(obj)))) {
	apr_pool_t *pool = current_pool;
	io_baton_t *iob = apr_palloc (pool, sizeof(io_baton_t));
	SvREFCNT_inc (obj);
	iob->obj = obj;
	iob->io = io;
	*stream = svn_stream_create (iob, pool);
	svn_stream_set_read (*stream, io_handle_read);
	svn_stream_set_write (*stream, io_handle_write);
	svn_stream_set_close (*stream, io_handle_close);
	apr_pool_cleanup_register (pool, iob, io_handle_cleanup,
                                   io_handle_cleanup);

    }
    else
	croak ("unknown type for svn_stream_t");

    return SVN_NO_ERROR;
}

SV *svn_swig_pl_from_stream (svn_stream_t *stream)
{
    swig_type_info *tinfo = SWIG_TypeQuery("svn_stream_t *");
    SV *ret;

    svn_swig_pl_callback_thunk (CALL_METHOD, (void *)"new", &ret, "sS",
			        "SVN::Stream", stream, tinfo);

    return sv_2mortal (ret);
}

apr_file_t *svn_swig_pl_make_file (SV *file, apr_pool_t *pool)
{
    apr_file_t *apr_file = NULL;

    if (!SvOK(file) || file == &PL_sv_undef)
	return NULL;

    if (SvPOKp(file)) {
      apr_file_open(&apr_file, SvPV_nolen(file),
                    APR_CREATE | APR_READ | APR_WRITE,
                    APR_OS_DEFAULT,
                    pool);
    } else if (SvROK(file) && SvTYPE(SvRV(file)) == SVt_PVGV) {
        apr_status_t status;
        apr_os_file_t osfile = PerlIO_fileno(IoIFP(sv_2io(file)));
        status = apr_os_file_put (&apr_file, &osfile, O_CREAT | O_WRONLY, pool);
        if (status)
            return NULL;
    }
    return apr_file;
}

static apr_status_t cleanup_refcnt (void *data)
{
    SV *sv = data;
    SvREFCNT_dec (sv);
    return APR_SUCCESS;
}

void svn_swig_pl_hold_ref_in_pool (apr_pool_t *pool, SV *sv)
{
    SvREFCNT_inc(sv);
    apr_pool_cleanup_register (pool, sv, cleanup_refcnt, apr_pool_cleanup_null);
}