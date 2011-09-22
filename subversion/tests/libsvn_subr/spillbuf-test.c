/*
 * spillbuf-test.c : test the spill buffer code
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include "svn_types.h"

#include "private/svn_subr_private.h"

#include "../svn_test.h"


static const char basic_data[] = ("abcdefghijklmnopqrstuvwxyz"
                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                  "0123456789");


static svn_error_t *
test_spillbuf_basic(apr_pool_t *pool)
{
  svn_spillbuf_t *buf = svn_spillbuf__create(
                          sizeof(basic_data) /* blocksize */,
                          10 * sizeof(basic_data) /* maxsize */,
                          pool);
  int i;

  /* It starts empty.  */
  SVN_TEST_ASSERT(svn_spillbuf__get_size(buf) == 0);

  /* Place enough data into the buffer to cause a spill to disk.  */
  for (i = 20; i--; )
    SVN_ERR(svn_spillbuf__write(buf, basic_data, sizeof(basic_data), pool));

  /* And now has content.  */
  SVN_TEST_ASSERT(svn_spillbuf__get_size(buf) > 0);

  while (TRUE)
    {
      const char *readptr;
      apr_size_t readlen;

      SVN_ERR(svn_spillbuf__read(&readptr, &readlen, buf, pool));
      if (readptr == NULL)
        break;

      /* We happen to know that the spill buffer reads data in lengths
         of BLOCKSIZE.  */
      SVN_TEST_ASSERT(readlen == sizeof(basic_data));

      /* And it should match each block of data we put in.  */
      SVN_TEST_ASSERT(memcmp(readptr, basic_data, readlen) == 0);
    }

  SVN_TEST_ASSERT(svn_spillbuf__get_size(buf) == 0);

  return SVN_NO_ERROR;
}


static svn_error_t *
read_callback(svn_boolean_t *stop,
              void *baton,
              const char *data,
              apr_size_t len,
              apr_pool_t *scratch_pool)
{
  int *counter = baton;

  SVN_TEST_ASSERT(len == sizeof(basic_data));
  SVN_TEST_ASSERT(memcmp(data, basic_data, len) == 0);

  *stop = (++*counter == 10);

  return SVN_NO_ERROR;
}


static svn_error_t *
test_spillbuf_callback(apr_pool_t *pool)
{
  svn_spillbuf_t *buf = svn_spillbuf__create(
                          sizeof(basic_data) /* blocksize */,
                          10 * sizeof(basic_data) /* maxsize */,
                          pool);
  int i;
  int counter;
  svn_boolean_t exhausted;

  /* Place enough data into the buffer to cause a spill to disk.  */
  for (i = 20; i--; )
    SVN_ERR(svn_spillbuf__write(buf, basic_data, sizeof(basic_data), pool));

  counter = 0;
  SVN_ERR(svn_spillbuf__process(&exhausted, buf, read_callback, &counter,
                                pool));
  SVN_TEST_ASSERT(!exhausted);
  
  SVN_ERR(svn_spillbuf__process(&exhausted, buf, read_callback, &counter,
                                pool));
  SVN_TEST_ASSERT(exhausted);

  return SVN_NO_ERROR;
}


static svn_error_t *
test_spillbuf_file(apr_pool_t *pool)
{
  apr_size_t altsize = sizeof(basic_data) + 2;
  svn_spillbuf_t *buf = svn_spillbuf__create(
                          altsize /* blocksize */,
                          2 * sizeof(basic_data) /* maxsize */,
                          pool);
  int i;
  const char *readptr;
  apr_size_t readlen;
  int cur_index;

  /* Place enough data into the buffer to cause a spill to disk. Note that
     we are writing data that is *smaller* than the blocksize.  */
  for (i = 7; i--; )
    SVN_ERR(svn_spillbuf__write(buf, basic_data, sizeof(basic_data), pool));

  /* The first two reads will be in-memory blocks (the third write causes
     the spill to disk). The spillbuf will pack the content into BLOCKSIZE
     blocks. The second/last memory block will (thus) be a bit smaller.  */
  SVN_ERR(svn_spillbuf__read(&readptr, &readlen, buf, pool));
  SVN_TEST_ASSERT(readptr != NULL);
  SVN_TEST_ASSERT(readlen == altsize);
  SVN_ERR(svn_spillbuf__read(&readptr, &readlen, buf, pool));
  SVN_TEST_ASSERT(readptr != NULL);
  /* The second write put sizeof(basic_data) into the buffer. A small
     portion was stored at the end of the memblock holding the first write.
     Thus, the size of this read will be the written data, minus that
     slice written to the first block.  */
  SVN_TEST_ASSERT(readlen
                  == sizeof(basic_data) - (altsize - sizeof(basic_data)));

  /* Current index into basic_data[] that we compare against.  */
  cur_index = 0;

  while (TRUE)
    {
      /* This will read more bytes (from the spill file into a temporary
         in-memory block) than the blocks of data that we wrote. This makes
         it trickier to verify that the right data is being returned.  */
      SVN_ERR(svn_spillbuf__read(&readptr, &readlen, buf, pool));
      if (readptr == NULL)
        break;

      while (TRUE)
        {
          apr_size_t amt;

          /* Compute the slice of basic_data that we will compare against,
             given the readlen and cur_index.  */
          if (cur_index + readlen >= sizeof(basic_data))
            amt = sizeof(basic_data) - cur_index;
          else
            amt = readlen;
          SVN_TEST_ASSERT(memcmp(readptr, &basic_data[cur_index], amt) == 0);
          if ((cur_index += amt) == sizeof(basic_data))
            cur_index = 0;
          if ((readlen -= amt) == 0)
            break;
          readptr += amt;
        }
    }

  SVN_TEST_ASSERT(svn_spillbuf__get_size(buf) == 0);

  return SVN_NO_ERROR;
}


static svn_error_t *
test_spillbuf_interleaving(apr_pool_t *pool)
{
  svn_spillbuf_t *buf = svn_spillbuf__create(8 /* blocksize */,
                                             15 /* maxsize */,
                                             pool);
  const char *readptr;
  apr_size_t readlen;

  SVN_ERR(svn_spillbuf__write(buf, "abcdef", 6, pool));
  SVN_ERR(svn_spillbuf__write(buf, "ghijkl", 6, pool));
  /* now: two blocks of 8 and 4 bytes  */

  SVN_ERR(svn_spillbuf__read(&readptr, &readlen, buf, pool));
  SVN_TEST_ASSERT(readptr != NULL
                  && readlen == 8
                  && memcmp(readptr, "abcdefgh", 8) == 0);
  /* now: one block of 4 bytes  */

  SVN_ERR(svn_spillbuf__write(buf, "mnopqr", 6, pool));
  /* now: two blocks of 8 and 2 bytes  */

  SVN_ERR(svn_spillbuf__read(&readptr, &readlen, buf, pool));
  SVN_TEST_ASSERT(readptr != NULL
                  && readlen == 8
                  && memcmp(readptr, "ijklmnop", 8) == 0);
  /* now: one block of 2 bytes  */

  SVN_ERR(svn_spillbuf__write(buf, "stuvwx", 6, pool));
  SVN_ERR(svn_spillbuf__write(buf, "ABCDEF", 6, pool));
  SVN_ERR(svn_spillbuf__write(buf, "GHIJKL", 6, pool));
  /* now: two blocks of 8 and 6 bytes, and 6 bytes spilled to a file  */

  SVN_ERR(svn_spillbuf__read(&readptr, &readlen, buf, pool));
  SVN_TEST_ASSERT(readptr != NULL
                  && readlen == 8
                  && memcmp(readptr, "qrstuvwx", 8) == 0);
  SVN_ERR(svn_spillbuf__read(&readptr, &readlen, buf, pool));
  SVN_TEST_ASSERT(readptr != NULL
                  && readlen == 6
                  && memcmp(readptr, "ABCDEF", 6) == 0);
  SVN_ERR(svn_spillbuf__read(&readptr, &readlen, buf, pool));
  SVN_TEST_ASSERT(readptr != NULL
                  && readlen == 6
                  && memcmp(readptr, "GHIJKL", 6) == 0);

  return SVN_NO_ERROR;
}


static svn_error_t *
test_spillbuf_reader(apr_pool_t *pool)
{
  svn_spillbuf_reader_t *sbr;
  apr_size_t amt;
  char buf[10];

  sbr = svn_spillbuf__reader_create(4 /* blocksize */,
                                    100 /* maxsize */,
                                    pool);

  SVN_ERR(svn_spillbuf__reader_write(sbr, "abcdef", 6, pool));

  /* Get a buffer from the underlying reader, and grab a couple bytes.  */
  SVN_ERR(svn_spillbuf__reader_read(&amt, sbr, buf, 2, pool));
  SVN_TEST_ASSERT(amt == 2 && memcmp(buf, "ab", 2) == 0);

  /* Trigger the internal "save" feature of the SBR.  */
  SVN_ERR(svn_spillbuf__reader_write(sbr, "ghijkl", 6, pool));

  /* Read from the save buffer, and from the internal blocks.  */
  SVN_ERR(svn_spillbuf__reader_read(&amt, sbr, buf, 10, pool));
  SVN_TEST_ASSERT(amt == 10 && memcmp(buf, "cdefghijkl", 10) == 0);

  /* Should be done.  */
  SVN_ERR(svn_spillbuf__reader_read(&amt, sbr, buf, 10, pool));
  SVN_TEST_ASSERT(amt == 0);

  return SVN_NO_ERROR;
}


/* The test table.  */
struct svn_test_descriptor_t test_funcs[] =
  {
    SVN_TEST_NULL,
    SVN_TEST_PASS2(test_spillbuf_basic, "basic spill buffer test"),
    SVN_TEST_PASS2(test_spillbuf_callback, "spill buffer read callback"),
    SVN_TEST_PASS2(test_spillbuf_file, "spill buffer file test"),
    SVN_TEST_PASS2(test_spillbuf_interleaving,
                   "interleaving reads and writes"),
    SVN_TEST_PASS2(test_spillbuf_reader, "spill buffer reader test"),
    SVN_TEST_NULL
  };
