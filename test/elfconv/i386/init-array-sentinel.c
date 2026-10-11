#include <stdio.h>

static int initialized;

static void __attribute__((constructor)) record_constructor(void) {
  initialized = 1;
  puts("constructor");
}

int main(void) {
  printf("main:%d\n", initialized);
  return initialized ? 0 : 1;
}
