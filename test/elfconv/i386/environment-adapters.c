#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const char *first_name = "ELFCONV_ENV_FIRST", *second_name = "ELFCONV_ENV_SECOND";
  if (setenv(first_name, "portal", 1) || setenv(second_name, "companion", 1)) return 1;
  char *first = getenv(first_name), *second = getenv(second_name);
  if (!first || !second || strcmp(first, "portal") || strcmp(second, "companion")) return 2;
  if (setenv(first_name, "ignored", 0) || strcmp(getenv(first_name), "portal") ||
      strcmp(second, "companion")) return 3;
  if (setenv(first_name, "lifted", 1) || strcmp(getenv(first_name), "lifted") ||
      strcmp(second, "companion")) return 4;
  char value[5001];
  memset(value, 'x', sizeof(value) - 1); value[5000] = 0;
  if (setenv(first_name, value, 1) || strcmp(getenv(first_name), value) ||
      strcmp(second, "companion")) return 5;
  errno = 0;
  if (setenv("INVALID=NAME", "value", 1) != -1 || errno != EINVAL) return 6;
  if (getenv("ELFCONV_ENV_ABSENT_943276")) return 7;
  puts("environment overwrite and independent guest string ownership passed");
  return 0;
}
