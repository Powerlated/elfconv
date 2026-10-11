#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

extern unsigned long legacy_strtoul(const char *, char **, int) __asm__("strtoul");
extern long legacy_strtol(const char *, char **, int) __asm__("strtol");
extern long __isoc23_strtol(const char *, char **, int);
int main(void) {
  char *end;
  const char *input = " -1tail";
  errno = EDOM;
  if (strtoul(input, &end, 10) != ULONG_MAX || errno != EDOM || end != input + 3) return 1;
  input = "-4294967296!";
  errno = 0;
  if (strtoul(input, &end, 10) != ULONG_MAX || errno != ERANGE || *end != '!') return 2;
  input = " - 1";
  errno = EDOM;
  if (strtoul(input, &end, 10) || end != input || errno != EDOM) return 3;
  errno = 0;
  if (strtol("2147483648", &end, 10) != LONG_MAX || errno != ERANGE || *end) return 4;
  errno = 0;
  if (strtol("-2147483649", &end, 10) != LONG_MIN || errno != ERANGE || *end) return 5;
  errno = EDOM;
  if (strtoll("-9223372036854775808x", &end, 10) != LLONG_MIN || errno != EDOM || *end != 'x') return 6;
  errno = 0;
  if (strtoll("9223372036854775808", &end, 10) != LLONG_MAX || errno != ERANGE) return 7;
  errno = EDOM;
  if (strtod("0x1.8p+2tail", &end) != 6.0 || *end != 't' || errno != EDOM) return 8;
  input = " -0b101tail";
  errno = EDOM;
  if (__isoc23_strtol(input, &end, 0) != -5 || end != input + 7 || errno != EDOM) return 9;
  if (legacy_strtol(input, &end, 0) != 0 || end != input + 3) return 10;
  if (legacy_strtoul("-1", &end, 10) != ULONG_MAX || errno != EDOM || *end) return 11;
  puts("numeric widths, signs, end pointers and overflow passed");
  return 0;
}
