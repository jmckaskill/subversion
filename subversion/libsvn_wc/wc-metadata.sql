/* wc-metadata.sql -- schema used in the wc-metadata SQLite database
 *     This is intended for use with SQLite 3
 *
 * ====================================================================
 * Copyright (c) 2008 CollabNet.  All rights reserved.
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

/* ### the following tables define the BASE tree */

CREATE TABLE REPOSITORY (
  id  INTEGER PRIMARY KEY AUTOINCREMENT,

  /* the UUID of the repository */
  uuid  TEXT NOT NULL,

  /* URL of the root of the repository */
  url  TEXT NOT NULL
  );

CREATE INDEX I_UUID ON REPOSITORY (uuid);


CREATE TABLE WORKING_COPY (
  id  INTEGER PRIMARY KEY AUTOINCREMENT,

  /* foreign key to REPOSITORY.id */
  repos_id  INTEGER NOT NULL,

  /* absolute path in the local filesystem */
  /* ### NULL if the metadata is stored in the wc root */
  local_path  TEXT,

  /* repository path corresponding to root of the working copy */
  repos_path  TEXT NOT NULL
  );

CREATE UNIQUE INDEX I_LOCAL_PATH ON WORKING_COPY (local_path);


CREATE TABLE DIRECTORY (
  id  INTEGER PRIMARY KEY AUTOINCREMENT,

  wc_id  INTEGER NOT NULL,

  /* relative path from wcroot. "" is used for the wcroot. */
  local_relpath  TEXT NOT NULL,

  /* path in repository */
  /* ### for switched subdirs, this could point to something other than
     ### local_relpath would imply. anything else for switching? */
  /* ### NULL if local_relpath can imply the repos path */
  repos_path  TEXT
  );

CREATE UNIQUE INDEX I_DIR_PATH ON DIRECTORY (wc_id, local_relpath);


CREATE TABLE NODE (
  id  INTEGER PRIMARY KEY AUTOINCREMENT,

  dir_id  INTEGER NOT NULL,

  /* ### use "" for "this directory" */
  filename  TEXT NOT NULL,

  revnum  INTEGER NOT NULL,

  kind  INTEGER NOT NULL,

  /* NULL for a directory */
  checksum  TEXT,

  /* ### do we need to deal with repos-size vs. eol-style-size?
     ### this value is the size of WORKING (which is BASE plus the
     ### transforms as defined for this node), so we can quickly detect
     ### differences. */
  working_size  INTEGER NOT NULL,

  /* Information about the last change to this node */
  changed_rev  INTEGER NOT NULL,
  changed_date  INTEGER NOT NULL,  /* an APR date/time (usec since 1970) */
  changed_author  TEXT NOT NULL,

  /* NULL depth means "default" (typically svn_depth_infinity) */
  depth  INTEGER,

  /* ### note: these values are caches from the server! */
  lock_token  TEXT,
  lock_owner  TEXT,
  lock_comment  TEXT,
  lock_date  INTEGER,  /* an APR date/time (usec since 1970) */

  /* ### Do we need this?  We've currently got various mod time APIs
     ### internal to libsvn_wc, but those might be used in answering some
     ### question which is better answered some other way. */
  last_mod_time  INTEGER  /* an APR date/time (usec since 1970) */
  );

CREATE UNIQUE INDEX I_PATH ON NODE (dir_id, filename);
CREATE INDEX I_NODELIST ON NODE (dir_id);
CREATE INDEX I_LOCKS ON NODE (lock_token);


CREATE TABLE PROPERTIES (
  node_id  INTEGER NOT NULL,

  name  TEXT NOT NULL,

  value  BLOB NOT NULL,

  PRIMARY KEY (node_id, name)
  );

CREATE UNIQUE INDEX I_NODE_PROPS ON PROPERTIES (node_id);


CREATE TABLE PRISTINE (
  /* ### the hash algorithm (MD5 or SHA-1) is encoded in this value */
  checksum  TEXT NOT NULL PRIMARY KEY,

  /* ### enumerated values specifying type of compression. NULL implies
     ### that no compression has been applied. */
  compression  INTEGER,

  /* ### used to verify the pristine file is "proper". NULL if unknown,
     ### and (thus) the pristine copy is incomplete/unusable. */
  size  INTEGER,

  refcount  INTEGER NOT NULL
  );


/* ------------------------------------------------------------------------- */

/* ### the following tables define the WORKING tree */

/* ### add/delete nodes */
CREATE TABLE NODE_CHANGES (
  id  INTEGER PRIMARY KEY AUTOINCREMENT, 

  /* Basic information about the node.  filename=="" for "this directory". */
  dir_id  INTEGER NOT NULL,
  filename  TEXT NOT NULL,
  kind  INTEGER NOT NULL,

  /* Enumerated type specifying what kind of change is at this location. */
  status  INTEGER NOT NULL,

  /* Where this node was copied/moved from. */
  original_repos_path  TEXT,
  original_revnum  INTEGER,
  checksum  TEXT,
  working_size  INTEGER,
  changed_rev  INTEGER,
  changed_date  INTEGER,  /* an APR date/time (usec since 1970) */
  changed_author  TEXT,

  /* ### do we want to record the revnums which caused this? */
  conflict_old  TEXT,
  conflict_new  TEXT,
  conflict_working  TEXT,
  prop_reject  TEXT,  /* ### is this right? */

  changelist_id  INTEGER
  );

CREATE UNIQUE INDEX I_PATH_CHANGES ON NODE_CHANGES (dir_id, filename);
CREATE INDEX I_NODELIST_CHANGES ON NODE_CHANGES (dir_id);


CREATE TABLE PROPERTIES_CHANGES (
  dir_id  INTEGER NOT NULL,

  filename  TEXT NOT NULL,

  name  TEXT NOT NULL,

  /* ### NULL implies deletion. */
  value  BLOB,

  PRIMARY KEY (dir_id, filename, name)
  );

CREATE INDEX I_PROPS_CHANGES ON PROPERTIES_CHANGES (dir_id, filename);


CREATE TABLE CHANGELIST (
  id  INTEGER PRIMARY KEY AUTOINCREMENT,

  wc_id  INTEGER NOT NULL,

  name  TEXT NOT NULL
  );

CREATE UNIQUE INDEX I_CHANGELIST ON CHANGELIST (wc_id, name);
CREATE UNIQUE INDEX I_CL_LIST ON CHANGELIST (wc_id);
