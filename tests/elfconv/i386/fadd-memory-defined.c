#include <stdio.h>

int main(void) {
  const double initial = -7.5, wide = 2.25;
  const float narrow = 8.0f;
  double result;
  __asm__ volatile("fldl %1; fadds %2; faddl %3; fstpl %0"
                   : "=m"(result)
                   : "m"(initial), "m"(narrow), "m"(wide)
                   : "st");
  printf("fadd-memory=%d\n", result == 2.75);
  return result != 2.75;
}
