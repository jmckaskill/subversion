/*
 * client.h :  shared stuff internal to the client library.
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


#include <apr_pools.h>
#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_xml.h"
#include "svn_wc.h"



svn_error_t *
svn_client__checkout_internal (const svn_delta_edit_fns_t *before_editor,
                               void *before_edit_baton,
                               const svn_delta_edit_fns_t *after_editor,
                               void *after_edit_baton,
                               svn_string_t *path,
                               svn_string_t *xml_src,
                               svn_string_t *ancestor_path,
                               svn_revnum_t ancestor_revision,
                               apr_pool_t *pool);


svn_error_t *
svn_client__update_internal (const svn_delta_edit_fns_t *before_editor,
                             void *before_edit_baton,
                             const svn_delta_edit_fns_t *after_editor,
                             void *after_edit_baton,
                             svn_string_t *path,
                             svn_string_t *xml_src,
                             svn_revnum_t ancestor_revision,
                             apr_pool_t *pool);



/* 
 * local variables:
 * eval: (load-file "../svn-dev.el")
 * end:
 */

