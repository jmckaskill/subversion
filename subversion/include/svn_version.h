/**
 * @copyright
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
 * @endcopyright
 *
 * @file svn_version.h
 * @brief Version information.
 */

#ifndef SVN_VERSION_H
#define SVN_VERSION_H

/* Hack to prevent the resource compiler from including
   apr_general.h.  It doesn't resolve the include paths
   correctly and blows up without this.
 */
#ifndef APR_STRINGIFY
#include <apr_general.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* Symbols that define the version number. */

/* Version numbers: <major>.<minor>.<micro>
 *
 * The version numbers in this file follow the rules established by:
 *
 *   http://apr.apache.org/versioning.html
 */

/** Major version number.
 *
 * Modify when incompatible changes are made to published interfaces.
 */
#define SVN_VER_MAJOR      1

/** Minor version number.
 *
 * Modify when new functionality is added or new interfaces are
 * defined, but all changes are backward compatible.
 */
#define SVN_VER_MINOR      1

/** Patch number.
 *
 * Modify for every released patch.
 */
#define SVN_VER_MICRO      0

/** Library version number.
 *
 * Modify whenever there's an incompatible change in the library ABI.
 * ### this is semantically equivalent to SVN_VER_MAJOR. fix...
 */
#define SVN_VER_LIBRARY    1


/** Version tag: a string describing the version.
 *
 * This tag remains " (dev build)" in the repository so that we can
 * always see from "svn --version" that the software has been built
 * from the repository rather than a "blessed" distribution.
 *
 * When rolling a tarball, we automatically replace this text with " (r1234)"
 * (where 1234 is the last revision on the branch prior to the release) 
 * for final releases; in prereleases, it becomes " (Alpha)",
 * " (Beta 1)", etc., as appropriate.
 *
 * Always change this at the same time as SVN_VER_NUMTAG.
 */
#define SVN_VER_TAG        " (dev build)"


/** Number tag: a string describing the version.
 *
 * This tag is used to generate a version number string to identify
 * the client and server in HTTP requests, for example. It must not
 * contain any spaces. This value remains "-dev" in the repository.
 *
 * When rolling a tarball, we automatically replace this text with ""
 * for final releases; in prereleases, it becomes "-alpha", "-beta1",
 * etc., as appropriate.
 *
 * Always change this at the same time as SVN_VER_TAG.
 */
#define SVN_VER_NUMTAG     "-dev"


/** Revision number: The repository revision number of this release.
 *
 * This constant is used to generate the build number part of the Windows
 * file version. Its value remains 0 in the repository.
 *
 * When rolling a tarball, we automatically replace it with what we
 * guess to be the correct revision number.
 */
#define SVN_VER_REVISION   0


/* Version strings composed from the above definitions. */

/** Version number */
#define SVN_VER_NUM        APR_STRINGIFY(SVN_VER_MAJOR) \
                           "." APR_STRINGIFY(SVN_VER_MINOR) \
                           "." APR_STRINGIFY(SVN_VER_MICRO)

/** Version number with tag (contains no whitespace) */
#define SVN_VER_NUMBER     SVN_VER_NUM SVN_VER_NUMTAG

/** Complete version string */
#define SVN_VERSION        SVN_VER_NUM SVN_VER_TAG



/* ### need runtime query function(s). see apr_version.h for example. */


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SVN_VERSION_H */
