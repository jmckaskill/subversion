/* GLIB - Library of useful routines for C programming
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Modified by the GLib Team and others 1997-1999.  See the AUTHORS
 * file for a list of people on the GLib Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GLib at ftp://ftp.gtk.org/pub/gtk/. 
 */

#undef G_LOG_DOMAIN

#include <stdio.h>
#include <string.h>
#include "glib.h"

int array[10000];
gboolean failed = FALSE;

#define	TEST(m,cond)	G_STMT_START { failed = !(cond); \
if (failed) \
  { if (!m) \
      g_print ("\n(%s:%d) failed for: %s\n", __FILE__, __LINE__, ( # cond )); \
    else \
      g_print ("\n(%s:%d) failed for: %s: (%s)\n", __FILE__, __LINE__, ( # cond ), (gchar*)m); \
  } \
else \
  g_print ("."); fflush (stdout); \
} G_STMT_END

#define	C2P(c)		((gpointer) ((long) (c)))
#define	P2C(p)		((gchar) ((long) (p)))

#define GLIB_TEST_STRING "el dorado "
#define GLIB_TEST_STRING_5 "el do"

int
main (int   argc,
      char *argv[])
{
  gint i;
  struct {
    gchar *filename;
    gchar *dirname;
  } dirname_checks[] = {
#ifndef NATIVE_WIN32
    { "/", "/" },
    { "////", "/" },
    { ".////", "." },
    { ".", "." },
    { "..", "." },
    { "../", ".." },
    { "..////", ".." },
    { "", "." },
    { "a/b", "a" },
    { "a/b/", "a/b" },
    { "c///", "c" },
#else
    { "\\", "\\" },
    { ".\\\\\\\\", "." },
    { ".", "." },
    { "..", "." },
    { "..\\", ".." },
    { "..\\\\\\\\", ".." },
    { "", "." },
    { "a\\b", "a" },
    { "a\\b\\", "a\\b" },
    { "c\\\\\\", "c" },
#endif
  };
  guint n_dirname_checks = sizeof (dirname_checks) / sizeof (dirname_checks[0]);

  for (i = 0; i < n_dirname_checks; i++)
    {
      gchar *dirname;

      dirname = g_dirname (dirname_checks[i].filename);
      g_assert (strcmp (dirname, dirname_checks[i].dirname) == 0);
      g_free (dirname);
    }

  return 0;
}

