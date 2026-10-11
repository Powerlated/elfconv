#include <stdint.h>
#include <stdio.h>
#include <string.h>

__attribute__((target("sse4.1")))
static int check(void) {
  uint32_t value[4] = {1, 2, 3, 4};
  const uint32_t scalar = 0xaabbccdd;
  __asm__ volatile("movdqu %0, %%xmm0; pinsrd $6, %1, %%xmm0; movdqu %%xmm0, %0"
                   : "+m"(value) : "r"(scalar) : "xmm0");
  if (value[0] != 1 || value[1] != 2 || value[2] != scalar || value[3] != 4) return 1;
  __asm__ volatile("movdqu %0, %%xmm0; pinsrd $255, %1, %%xmm0; movdqu %%xmm0, %0"
                   : "+m"(value) : "m"(scalar) : "xmm0");
  if (value[0] != 1 || value[1] != 2 || value[2] != scalar || value[3] != scalar) return 2;
  const float input[4] __attribute__((aligned(16))) = {1.0f, 4.0f, 9.0f, 16.0f};
  const float expected[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  __asm__ volatile("movups %1, %%xmm1; sqrtps %%xmm1, %%xmm0; movups %%xmm0, %0"
                   : "=m"(value) : "m"(input) : "xmm0", "xmm1");
  if (memcmp(value, expected, 16)) return 3;
  __asm__ volatile("sqrtps %1, %%xmm0; movups %%xmm0, %0"
                   : "=m"(value) : "m"(input) : "xmm0");
  if (memcmp(value, expected, 16)) return 4;
  return 0;
}
int main(void) {
  if (check()) return 1;
  puts("packed insertion index masking and four-lane square roots passed");
  return 0;
}
