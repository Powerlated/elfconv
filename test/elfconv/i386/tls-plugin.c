#include <stdio.h>

extern __thread int dependency_tls;
extern int dependency_tls_get(void);
extern void *__dso_handle;
extern int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
__thread int plugin_tls = 9;
static __thread unsigned char local_tls[16] __attribute__((aligned(256)));

int plugin_tls_check(void) { return plugin_tls + dependency_tls_get() + local_tls[0]; }
int plugin_tls_dependency(void) { return dependency_tls; }

static void first_destructor(void *pointer) {
  int *value = pointer;
  printf("tls-destructor:first=%d\n", *value);
  ++*value;
}
static void second_destructor(void *pointer) {
  int *value = pointer;
  printf("tls-destructor:second=%d\n", *value);
  ++*value;
}
int plugin_tls_register(void) {
  int first = __cxa_thread_atexit_impl(first_destructor, &plugin_tls, __dso_handle);
  int second = __cxa_thread_atexit_impl(second_destructor, &plugin_tls, __dso_handle);
  return first || second;
}

__attribute__((constructor)) static void initialize(void) {
  printf("tls-plugin:init=%d/%u/%d\n", plugin_tls, local_tls[0], dependency_tls);
  plugin_tls = 23;
}
__attribute__((destructor)) static void finalize(void) {
  printf("tls-plugin:final=%d\n", plugin_tls);
}
