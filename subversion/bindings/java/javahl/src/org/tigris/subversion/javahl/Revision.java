/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2003-2004 CollabNet.  All rights reserved.
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
 */
package org.tigris.subversion.javahl;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Class to specify a revision in a svn command.
 */
public class Revision
{
    protected int revKind;

    public Revision(int kind)
    {
        revKind = kind;
    }

    public int getKind()
    {
        return revKind;
    }

    public String toString()
    {
        switch(revKind) {
            case Kind.base : return "BASE";
            case Kind.committed : return "COMMITTED";
            case Kind.head : return "HEAD";
            case Kind.previous : return "PREV";
            case Kind.working : return "WORKING";
        }
        return super.toString();
    }

    public boolean equals(Object target) {
        if (this == target)
            return true;
        if (!(target instanceof Revision))
            return false;

        return ((Revision)target).revKind == revKind;        
    }

    public static final Revision HEAD = new Revision(Kind.head);
    public static final Revision START = new Revision(Kind.unspecified);
    public static final Revision COMMITTED = new Revision(Kind.committed);
    public static final Revision PREVIOUS = new Revision(Kind.previous);
    public static final Revision BASE = new Revision(Kind.base);
    public static final Revision WORKING = new Revision(Kind.working);
    public static final int SVN_INVALID_REVNUM = -1;

    public static class Number extends Revision
    {
        protected long revNumber;

        public Number(long number)
        {
            super(Kind.number);
            revNumber = number;
        }

        public long getNumber()
        {
            return revNumber;
        }

        public String toString() {
            return Long.toString(revNumber);
        }
        
        public boolean equals(Object target) {
            if (!super.equals(target))
                return false;

            return ((Revision.Number)target).revNumber == revNumber;        
        }
    }

    public static class DateSpec extends Revision
    {
        protected Date revDate;
        public DateSpec(Date date)
        {
            super(Kind.date);
            revDate = date;
        }
        public Date getDate()
        {
            return revDate;
        }

        public String toString() {
            
            SimpleDateFormat dateFormat = new SimpleDateFormat("EEE, d MMM yyyy HH:mm:ss Z", Locale.US); 
            return '{'+dateFormat.format(revDate)+'}';
        }

        public boolean equals(Object target) {
            if (!super.equals(target))
                return false;

            return ((Revision.DateSpec)target).revDate.equals(revDate);        
        }
        
    }

    /** Various ways of specifying revisions.
     *
     * Various ways of specifying revisions.
     *
     * Note:
     * In contexts where local mods are relevant, the `working' kind
     * refers to the uncommitted "working" revision, which may be modified
     * with respect to its base revision.  In other contexts, `working'
     * should behave the same as `committed' or `current'.
     */
    public static final class Kind
    {
        /** No revision information given. */
        public static final int unspecified = 0;

        /** revision given as number */
        public static final int number = 1;

        /** revision given as date */
        public static final int date = 2;

        /** rev of most recent change */
        public static final int committed = 3;

        /** (rev of most recent change) - 1 */
        public static final int previous = 4;

        /** .svn/entries current revision */
        public static final int base = 5;

        /** current, plus local mods */
        public static final int working = 6;

        /** repository youngest */
        public static final int head = 7;

    }
}
