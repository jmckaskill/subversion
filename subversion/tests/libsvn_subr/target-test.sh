#!/bin/sh

FAILED="no"

# PWD is a read only variable in some /bin/sh's, so we would get an error here, 
# fortunately, those /bin/sh's also allow you to just read $PWD to get the 
# current working directory, so we can safely ignore the problem and continue.
PWD=`pwd` 2>/dev/null

if test -d "z"; then
    :
else
    mkdir z
    mkdir z/A
    mkdir z/A/B
    mkdir z/A/C
    mkdir z/D
    mkdir z/D/E
    mkdir z/D/F
    mkdir z/G
    mkdir z/G/H
    mkdir z/G/I
    touch z/A/file
fi

EXPECTED="${PWD}/z: A, D, G, "
GOT=`./target-test z/A/B z/A z/A/C z/D/E z/D/F z/D z/G z/G/H z/G/I`
if [ $? != 0 ]; then
  echo "FAIL: target-test  1: normal use (non-null return)"
  FAILED="yes"
else
  if [ "$GOT" != "$EXPECTED" ]; then
    echo "FAIL: target-test  1: normal use"
    FAILED="yes"
  else
    echo "PASS: target-test  1: normal use"
  fi
fi

EXPECTED="${PWD}/z/A: "
GOT=`./target-test z/A z/A z/A z/A`
if [ $? != 0 ]; then
  echo "FAIL: target-test  2: identical dirs (non-null return)"
  FAILED="yes"
else
  if [ "$GOT" != "$EXPECTED" ]; then
    echo "FAIL: target-test  2: identical dirs"
    FAILED="yes"
  else
    echo "PASS: target-test  2: identical dirs"
  fi
fi

EXPECTED="${PWD}/z/A: file, "
GOT=`./target-test z/A/file z/A/file z/A/file z/A/file`
if [ $? != 0 ]; then
  echo "FAIL: target-test  3: identical files (non-null return)"
  FAILED="yes"
else
  if [ "$GOT" != "$EXPECTED" ]; then
    echo "FAIL: target-test  3: identical files"
    FAILED="yes"
  else
    echo "PASS: target-test  3: identical files"
  fi
fi

EXPECTED="${PWD}/z/A: "
GOT=`./target-test z/A`
if [ $? != 0 ]; then
  echo "FAIL: target-test  4: single dir (non-null return)"
  FAILED="yes"
else
  if [ "$GOT" != "$EXPECTED" ]; then
    echo "FAIL: target-test  4: single dir"
    FAILED="yes"
  else
    echo "PASS: target-test  4: single dir"
  fi
fi

EXPECTED="${PWD}/z/A: file, "
GOT=`./target-test z/A/file`
if [ $? != 0 ]; then
  echo "FAIL: target-test  5: single file (non-null return)"
  FAILED="yes"
else
  if [ "$GOT" != "$EXPECTED" ]; then
    echo "FAIL: target-test  5: single file"
    FAILED="yes"
  else
    echo "PASS: target-test  5: single file"
  fi
fi

if [ "$FAILED" != "no" ]; then
  exit 1
else
  exit 0
fi
