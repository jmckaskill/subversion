dnl
dnl java.m4: Locates the JDK and its include files and libraries.
dnl

AC_DEFUN(SVN_CHECK_JDK,
[
  AC_ARG_WITH(jdk,
              AC_HELP_STRING([--with-jdk=PATH],
                             [Try to use 'PATH/include' to find the JNI
                              headers.  If PATH is not specified, look 
                              for a Java Development Kit at JAVA_HOME.]),
  [
    case "$withval" in
      "no")
        JDK_SUITABLE=no
      ;;
      "yes")
        SVN_FIND_JDK(check)
      ;;
      *)
        SVN_FIND_JDK($withval)
      ;;
    esac
  ],
  [
    SVN_FIND_JDK(check)
  ])
])

AC_DEFUN(SVN_FIND_JDK,
[
  where=$1

  AC_MSG_CHECKING([for JDK])
  if test $where = check; then
    if test -d "$JAVA_HOME/include"; then
      JDK="$JAVA_HOME"
      JDK_SUITABLE=yes
    else
      JDK=none
      JDK_SUITABLE=no
    fi
  else
    JDK=$where
    if test -d "$JDK/include"; then
      JDK_SUITABLE=yes
    else
      AC_MSG_WARN([no JNI header files found.])
    fi
  fi
  AC_MSG_RESULT([$JDK_SUITABLE])

  JAVA_BIN='$(JDK)/bin'
  if test -f "$JDK/include/jni.h"; then
    JNI_INCLUDES="$JDK/include"
  fi

  dnl Correct for Darwin's odd JVM layout.  Ideally, we should use realpath,
  dnl but Darwin doesn't have that utility.  /usr/bin/java is a symlink into
  dnl /System/Library/Frameworks/JavaVM.framework/Versions/CurrentJDK/Commands
  os_arch=`uname`
  if test "$JDK_SUITABLE" = "yes" -a "$JDK" = "/usr" -a "$os_arch" = "Darwin" -a -d "/System/Library/Frameworks/JavaVM.framework/Versions/CurrentJDK"; then
      JDK="/System/Library/Frameworks/JavaVM.framework/Versions/CurrentJDK"
      JAVA_BIN='$(JDK)/Commands'
      JNI_INCLUDES="$JDK/Headers"
  fi

  JAVAC="$JAVA_BIN/javac"
  # TODO: Test for Jikes, which should be preferred (for speed) if available
  JAVAH="$JAVA_BIN/javah"
  JAR="$JAVA_BIN/jar"

  dnl We use JDK in both the swig.m4 macros and the Makefile
  AC_SUBST(JDK)
  AC_SUBST(JAVAC)
  AC_SUBST(JAVAH)
  AC_SUBST(JAR)
  AC_SUBST(JNI_INCLUDES)
])
