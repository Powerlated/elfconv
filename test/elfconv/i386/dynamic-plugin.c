#include <stdio.h>

extern int dependency_add(int);
int plugin_value = 3;
int plugin_call(int value) { return plugin_value + dependency_add(value); }
__asm__(".globl zero_symbol\n.set zero_symbol, 0");

__attribute__((constructor)) static void initialize(void) {
  plugin_value = dependency_add(5);
  printf("plugin:init=%d\n", plugin_value);
}
__attribute__((destructor)) static void finalize(void) {
  printf("plugin:final=%d\n", plugin_value);
}
