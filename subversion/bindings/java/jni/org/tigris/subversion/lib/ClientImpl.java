package org.tigris.subversion.lib;

/**
 * native interface to the client class
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
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
 */

import org.tigris.subversion.SubversionException;
import java.util.Hashtable;
import java.util.Date;

public class ClientImpl implements org.tigris.subversion.lib.Client {
  public native void checkout(TreeDeltaEditor beforeEditor,
    TreeDeltaEditor afterEditor, String url, String path,
    Revision revision, Date time, String xml_src) throws SubversionException;

  public native void update(TreeDeltaEditor beforeEditor,
    TreeDeltaEditor afterEditor, String path, String xml_src,
    String revision, Date time) throws SubversionException;

  public native void add(String path, boolean recursive) throws SubversionException;

  public native void delete(String path, boolean force) throws SubversionException;

  public native void performImport(TreeDeltaEditor beforeEditor,
      TreeDeltaEditor afterEditor, String path, String url,
      String new_entry, String log_msg,	String xml_dst,
      String Revision) throws SubversionException;

  public native void commit(TreeDeltaEditor beforeEditor,
    TreeDeltaEditor afterEditor, String targets[], String log_msg,
    String xml_dst, String revision) throws SubversionException;

  public native Hashtable status(String path, boolean descend)
    throws SubversionException;

  public native String fileDiff(String path) throws SubversionException;

  public native void cleanup(String dir) throws SubversionException;
  
}
