#include <stdint.h>
#include <stdio.h>

int main(void) {
  const uint64_t input = UINT64_C(0x0123456789abcdef);
  uint64_t output;
  double sum;
  __asm__ volatile (
      "fninit\n\t"
      "movq %2, %%mm0\n\t"
      "emms\n\t"
      "movq %%mm0, %1\n\t"
      "emms\n\t"
      "fld1\n\t"
      "fstpl %0"
      : "=m"(sum), "=m"(output)
      : "m"(input)
      : "mm0", "st", "memory");
  if (sum != 1.0 || output != input) {
    fprintf(stderr, "sum=%f MMX=%016llx expected=%016llx\n", sum,
            (unsigned long long)output, (unsigned long long)input);
    return 1;
  }
  puts("EMMS preserved MMX data and enabled x87 arithmetic");
  return 0;
}
