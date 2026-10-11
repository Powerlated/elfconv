#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

__thread int root_tls = 11;
static __thread unsigned char root_zero[32] __attribute__((aligned(64)));
#ifdef STARTUP_LINKED
extern int plugin_tls_check(void);
#endif

static void require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "failed: %s\n", message); exit(1); }
}
__attribute__((constructor)) static void initialize(void) {
  require(root_tls == 11 && root_zero[0] == 0, "TLS before main constructors");
  root_tls = 19;
  puts("tls-main:init");
}
__attribute__((destructor)) static void finalize(void) {
  printf("tls-main:final=%d\n", root_tls);
}

int main(void) {
  require(root_tls == 19 && ((uintptr_t)root_zero & 63) == 0, "local-exec TLS and alignment");
#ifdef STARTUP_LINKED
  require(plugin_tls_check() == 40, "startup-linked TLS constructors");
#endif
  void *library = dlopen("libplugin.so", RTLD_NOW);
  require(library != NULL, "TLS library open");
  int *value = dlsym(library, "plugin_tls");
  int (*check)(void) = dlsym(library, "plugin_tls_check");
  int (*dependency)(void) = dlsym(library, "plugin_tls_dependency");
  require(value && check && dependency && *value == 23 && check() == 40,
          "general-dynamic TLS and dlsym");
  require(dependency() == 17, "cross-module TLS relocation");
  *value = 101;
  require(check() == 118, "TLS dlsym points to the current block");
  require(root_tls == 19, "library TLS does not alias main TLS");
  require(!dlclose(library), "TLS library close");
  library = dlopen("libplugin.so", RTLD_NOW);
  require(library != NULL, "TLS library reopen");
  value = dlsym(library, "plugin_tls");
#ifndef STARTUP_LINKED
  require(value && *value == 23, "reload resets TLS template");
#endif
  int (*register_destructors)(void) = dlsym(library, "plugin_tls_register");
  require(register_destructors && !register_destructors(), "thread destructor registration");
  require(!dlclose(library), "destructors retain the library");
  void *main_handle = dlopen(NULL, RTLD_NOW);
  require(dlsym(main_handle, "root_tls") == &root_tls, "main TLS dlsym");
  require(!dlclose(main_handle), "main handle close");
  puts("tls:ok");
  return 0;
}
