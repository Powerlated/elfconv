/* Copyright (C) 2000-2026 Free Software Foundation, Inc.
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
   <https://www.gnu.org/licenses/>.  */

/* We always want assert to be fully defined.  */
#undef NDEBUG
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>



/* UTF-8 single byte feeding test for mbrtowc().  */
static int
utf8_test_1 (void)
{
  wchar_t wc;
  mbstate_t s;

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (&wc, "\xE2", 1, &s) == (size_t) -2);	/* 1st byte processed */
  assert (mbrtowc (&wc, "\x89", 1, &s) == (size_t) -2);	/* 2nd byte processed */
  assert (wc == 42);		/* no value has not been stored into &wc yet */
  assert (mbrtowc (&wc, "\xA0", 1, &s) == 1);	/* 3rd byte processed */
  assert (wc == 0x2260);	/* E2 89 A0 = U+2260 (not equal) decoded correctly */
  assert (mbrtowc (&wc, "", 1, &s) == 0);	/* test final byte processing */
  assert (wc == 0);		/* test final byte decoding */

  /* The following test is by Al Viro <aviro@redhat.com>.  */
  const char str[] = "\xe0\xa0\x80";

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (&wc, str, 1, &s) == -2);
  assert (mbrtowc (&wc, str + 1, 2, &s) == 2);
  assert (wc == 0x800);

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (&wc, str, 3, &s) == 3);
  assert (wc == 0x800);

  return 0;
}

/* Test for NUL byte processing via empty string.  */
static int
utf8_test_2 (void)
{
  wchar_t wc;
  mbstate_t s;

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (NULL, "", 1, &s) == 0); /* valid terminator */
  assert (mbsinit (&s));

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (&wc, "\xE2", 1, &s) == (size_t) -2);	/* 1st byte processed */
  assert (mbrtowc (NULL, "", 1, &s) == (size_t) -1); /* invalid terminator */

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (&wc, "\xE2", 1, &s) == (size_t) -2);	/* 1st byte processed */
  assert (mbrtowc (&wc, "\x89", 1, &s) == (size_t) -2);	/* 2nd byte processed */
  assert (mbrtowc (NULL, "", 1, &s) == (size_t) -1); /* invalid terminator */

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (&wc, "\xE2", 1, &s) == (size_t) -2);	/* 1st byte processed */
  assert (mbrtowc (&wc, "\x89", 1, &s) == (size_t) -2);	/* 2nd byte processed */
  assert (mbrtowc (&wc, "\xA0", 1, &s) == 1);	/* 3rd byte processed */
  assert (mbrtowc (NULL, "", 1, &s) == 0); /* valid terminator */
  assert (mbsinit (&s));

  return 0;
}

/* Test for NUL byte processing via NULL string.  */
static int
utf8_test_3 (void)
{
  wchar_t wc;
  mbstate_t s;

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (NULL, NULL, 0, &s) == 0); /* valid terminator */
  assert (mbsinit (&s));

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (&wc, "\xE2", 1, &s) == (size_t) -2);	/* 1st byte processed */
  assert (mbrtowc (NULL, NULL, 0, &s) == (size_t) -1); /* invalid terminator */

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (&wc, "\xE2", 1, &s) == (size_t) -2);	/* 1st byte processed */
  assert (mbrtowc (&wc, "\x89", 1, &s) == (size_t) -2);	/* 2nd byte processed */
  assert (mbrtowc (NULL, NULL, 0, &s) == (size_t) -1); /* invalid terminator */

  wc = 42;			/* arbitrary number */
  memset (&s, 0, sizeof (s));	/* get s into initial state */
  assert (mbrtowc (&wc, "\xE2", 1, &s) == (size_t) -2);	/* 1st byte processed */
  assert (mbrtowc (&wc, "\x89", 1, &s) == (size_t) -2);	/* 2nd byte processed */
  assert (mbrtowc (&wc, "\xA0", 1, &s) == 1);	/* 3rd byte processed */
  assert (mbrtowc (NULL, NULL, 0, &s) == 0); /* valid terminator */
  assert (mbsinit (&s));

  return 0;
}

static int
utf8_test (void)
{
  if (setlocale (LC_CTYPE, "C.UTF-8") == NULL
      && setlocale (LC_CTYPE, "C.utf8") == NULL)
    {
      fprintf (stderr, "UTF-8 locale unavailable\n");
      return 1;
    }

  error |= utf8_test_1 ();
  error |= utf8_test_2 ();
  error |= utf8_test_3 ();

  return error;
}


static int
do_test (void)
{
  return utf8_test ();
}



int main (void) { return do_test (); }
