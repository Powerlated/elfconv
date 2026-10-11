#include <stdio.h>

__thread int dependency_tls = 17;
__thread unsigned char dependency_zero[8] __attribute__((aligned(128)));
int dependency_tls_get(void) { return dependency_tls + dependency_zero[0]; }

__attribute__((constructor)) static void initialize(void) {
  printf("tls-dependency:init=%d/%u\n", dependency_tls, dependency_zero[0]);
}
__attribute__((destructor)) static void finalize(void) {
  printf("tls-dependency:final=%d\n", dependency_tls);
}
