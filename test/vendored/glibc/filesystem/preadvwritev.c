/* Tests for preadv and pwritev.
   Copyright (C) 2016-2024 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.

   Adapted from glibc misc/tst-preadvwritev-common.c and
   misc/tst-preadvwritev.c at 3d1aed874918c466a4477af1da35983ab036690e. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#define FAIL_RET(msg) do { fprintf(stderr, "%s: %s\n", msg, strerror(errno)); return 1; } while (0)

int
main(void)
{
  char path[] = "preadvwritev.XXXXXX";
  int fd = mkstemp(path);
  if (fd == -1)
    FAIL_RET("cannot create temporary file");

  struct iovec iov[2];
  char buf1[32], buf2[64];
  memset(buf1, 0xf0, sizeof buf1);
  memset(buf2, 0x0f, sizeof buf2);
  iov[0] = (struct iovec) { buf1, sizeof buf1 };
  iov[1] = (struct iovec) { buf2, sizeof buf2 };

  ssize_t ret = pwritev(fd, iov, 2, 0);
  if (ret == -1) FAIL_RET("first pwritev returned -1");
  if (ret != sizeof buf1 + sizeof buf2) FAIL_RET("first pwritev returned an unexpected value");
  ret = pwritev(fd, iov, 2, sizeof buf1 + sizeof buf2);
  if (ret == -1) FAIL_RET("second pwritev returned -1");
  if (ret != sizeof buf1 + sizeof buf2) FAIL_RET("second pwritev returned an unexpected value");

  char buf3[32], buf4[64];
  memset(buf3, 0x0f, sizeof buf3);
  memset(buf4, 0xf0, sizeof buf4);
  iov[0] = (struct iovec) { buf3, sizeof buf3 };
  iov[1] = (struct iovec) { buf4, sizeof buf4 };
  ret = preadv(fd, iov, 2, 0);
  if (ret == -1) FAIL_RET("first preadv returned -1");
  if (ret != sizeof buf3 + sizeof buf4) FAIL_RET("first preadv returned an unexpected value");
  if (memcmp(buf1, buf3, sizeof buf1) != 0) FAIL_RET("first buffer from first preadv different than expected");
  if (memcmp(buf2, buf4, sizeof buf2) != 0) FAIL_RET("second buffer from first preadv different than expected");
  ret = preadv(fd, iov, 2, sizeof buf3 + sizeof buf4);
  if (ret == -1) FAIL_RET("second preadv returned -1");
  if (ret != sizeof buf3 + sizeof buf4) FAIL_RET("second preadv returned an unexpected value");
  if (memcmp(buf1, buf3, sizeof buf1) != 0) FAIL_RET("first buffer from second preadv different than expected");
  if (memcmp(buf2, buf4, sizeof buf2) != 0) FAIL_RET("second buffer from second preadv different than expected");

  if (close(fd) != 0 || unlink(path) != 0) FAIL_RET("cleanup failed");
  puts("glibc preadv/pwritev offsets and contents passed");
  return 0;
}
