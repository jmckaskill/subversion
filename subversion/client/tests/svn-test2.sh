#!/bin/sh

# Testing merging and conflict resolution.

SVN_PROG=../svn
TEST_DIR_1=t1
TEST_DIR_2=t2
COMMIT_RESULTFILE_NAME=commit2
ANCESTOR_PATH=anni       # See if Greg Stein notices. :-) 

check_status()
{
    res=$?
    if [ $res -ne 0 ]; then
      echo Oops, problem: ${@-"(no further details)"}
      exit $res
    fi
    # REMOVE ME;  for easy debugging step-throughs:
    if test "`whoami 2> /dev/null`" = "sussman"; then
      read foo
    fi
}

# Remove the testing tree
rm -rf ${TEST_DIR_1} ${TEST_DIR_2} ${COMMIT_RESULTFILE_NAME}*

echo

##### Run tests: #####

### Checking out.
echo "Checking out ${TEST_DIR_1}."
${SVN_PROG} checkout                                      \
      --target-dir ${TEST_DIR_1}                          \
      --xml-file ../../tests-common/xml/co1-inline.xml    \
      --revision 1                                        \
      --ancestor-path ${ANCESTOR_PATH}

check_status 1

### Give t1/iota some file-properties via update.
echo "Updating t1/iota with properties.  (up2.xml)"
${SVN_PROG} update --xml-file ../../tests-common/xml/up2.xml \
                   --revision 17 ${TEST_DIR_1}
check_status 2

### Give t1/A some dir-properties via update.
echo "Updating t1/A/ with properties.  (up5.xml)"
${SVN_PROG} update --xml-file ../../tests-common/xml/up5.xml \
                   --revision 18 ${TEST_DIR_1}
check_status 3

### Examine the all the properties using "proplist":
echo "Properties on t1/A:"
${SVN_PROG} proplist ${TEST_DIR_1}/A
check_status 4

echo "Properties on t1/iota:"
${SVN_PROG} proplist ${TEST_DIR_1}/iota
check_status 5

### Locally change properties
echo "Making local changes to these properties."
${SVN_PROG} propset ninja moo ${TEST_DIR_1}/A
${SVN_PROG} propset wings moo2 ${TEST_DIR_1}/A
${SVN_PROG} propset window moo3 ${TEST_DIR_1}/A
${SVN_PROG} propset door moo4 ${TEST_DIR_1}/A
${SVN_PROG} propset bat bandersnatch ${TEST_DIR_1}/iota
${SVN_PROG} propset lexicon cryptonalysis ${TEST_DIR_1}/iota
check_status 6

### Make local changes to pi's and rho's text, too.
echo "Making local text changes on pi and rho."
echo "new text for pi" >> ${TEST_DIR_1}/A/D/G/pi
echo "z" > ${TEST_DIR_1}/A/D/G/rho
check_status 7

### Examine status; we should see local mods present in both text and
### property columns.
echo "Status of directory:"
${SVN_PROG} status ${TEST_DIR_1}
check_status 8

### Update again.  This update should create conflicting properties.
echo "Updating with (conflicting) properties.  (up-props.xml)"
${SVN_PROG} update --xml-file ../../tests-common/xml/up-props.xml \
                   --revision 20 ${TEST_DIR_1}
check_status 9

### Update again.  This update should create conflicting text.
echo "Updating with (conflicting) text.  (pipatch.xml)"
${SVN_PROG} update --xml-file ../../tests-common/xml/pipatch.xml \
                   --revision 21 ${TEST_DIR_1}
check_status 10

### Examine status; we should see CONFLICTS present.
echo "Status of directory:"
${SVN_PROG} status ${TEST_DIR_1}
check_status 11

### Try to commit;  the conflict should abort due to conflicts.
echo "Attempting to commit while conflicts are present:"
${SVN_PROG} commit  --xml-file ${COMMIT_RESULTFILE_NAME}-1.xml \
                   --revision 22 ${TEST_DIR_1}
# (no check_status here, because we *expect* the commit to fail!)


### Clean up property-reject files.
echo "Removing all .prej files..."
rm -f ${TEST_DIR_1}/*.prej
rm -f ${TEST_DIR_1}/A/*.prej
check_status 12


### Try to commit again;  the conflict should abort due to text conflict.
echo "Attempting to commit while conflicts are present:"
${SVN_PROG} commit --xml-file ${COMMIT_RESULTFILE_NAME}-1.xml \
                   --revision 23 ${TEST_DIR_1}
# (no check_status here, because we *expect* the commit to fail!)


### Clean up the standard textual reject files.
echo "Remove all .rej files..."
rm -f ${TEST_DIR_1}/A/D/G/*.rej
check_status 13

### Examine status; there should only be local mods now, not conflicts.
echo "Status of directory:"
${SVN_PROG} status ${TEST_DIR_1}
check_status 14

### Try to commit;  the conflict should now succeed.
echo "Attempting to commit again, with conflicts removed."
${SVN_PROG} commit --xml-file ${COMMIT_RESULTFILE_NAME}-1.xml \
                   --revision 24 ${TEST_DIR_1}
check_status 15

### Examine status; everything should be up-to-date.
echo "Status of directory:"
${SVN_PROG} status ${TEST_DIR_1}
check_status 16


exit 0
