#include <stdint.h>
#include <stdio.h>
#include <string.h>

__attribute__((target("sse2")))
static int check(uint32_t input, int memory) {
  uint32_t source[4] = {input, 1, 2, 3};
  uint32_t result[4] = {0, 0x12345678, 0x87654321, 0xabcdef01};
  uint32_t saved, control = 0x1f80, after;
  __asm__ volatile("stmxcsr %0; ldmxcsr %1" : "=m"(saved) : "m"(control));
  if (memory) {
    __asm__ volatile("movups %0, %%xmm0; rsqrtss %1, %%xmm0; movups %%xmm0, %0"
                     : "+m"(result) : "m"(source[0]) : "xmm0");
  } else {
    __asm__ volatile("movups %0, %%xmm0; movups %1, %%xmm1; rsqrtss %%xmm1, %%xmm0; movups %%xmm0, %0"
                     : "+m"(result) : "m"(source) : "xmm0", "xmm1");
  }
  __asm__ volatile("stmxcsr %0; ldmxcsr %1" : "=m"(after) : "m"(saved));
  if (after != control || result[1] != 0x12345678 || result[2] != 0x87654321 || result[3] != 0xabcdef01) return 1;
  float value;
  memcpy(&value, result, 4);
  if (input == 0x40800000) return !(value > 0.4998f && value < 0.5002f);
  if (input == 0x41100000) return !(value > 0.3332f && value < 0.3335f);
  if ((input & 0x7f800000) == 0) return result[0] != ((input & 0x80000000) | 0x7f800000);
  if (input == 0x7f800000) return result[0] != 0;
  if (input == 0xbf800000) return result[0] != 0xffc00000;
  return result[0] != (input | 0x00400000);
}

int main(void) {
  const uint32_t inputs[] = {0x40800000, 0x41100000, 0, 0x80000000, 1, 0x80000001,
                             0x7f800000, 0xbf800000, 0x7fc12345, 0x7f812345};
  for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i)
    if (check(inputs[i], 0) || check(inputs[i], 1)) return 1;
  puts("rsqrt scalar operands, upper lanes and special values passed");
  return 0;
}
