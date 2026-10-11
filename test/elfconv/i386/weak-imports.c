#include <stdio.h>

extern int absent_optional_hook(int) __attribute__((weak));

__attribute__((noinline)) static int optional_call(void) {
  return absent_optional_hook ? absent_optional_hook(13) : 17;
}

int main(void) {
  int missing = absent_optional_hook == 0;
  int fallback = optional_call() == 17;
  printf("weak-import:missing=%d fallback=%d\n", missing, fallback);
  return !(missing && fallback);
}
