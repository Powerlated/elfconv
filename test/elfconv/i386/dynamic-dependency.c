#include <stdio.h>

int dependency_value = 7;
int dependency_add(int value) { return dependency_value + value; }

__attribute__((constructor)) static void initialize(void) {
  dependency_value = 17;
  puts("dependency:init");
}
__attribute__((destructor)) static void finalize(void) {
  printf("dependency:final=%d\n", dependency_value);
}
