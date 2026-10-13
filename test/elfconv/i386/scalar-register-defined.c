#include <stdint.h>
#include <stdio.h>

__attribute__((target("sse2")))
static int check_scalar_sqrt(void) {
  uint32_t single[4] = {0x41200000, 0x12345678, 0x87654321, 0xabcdef01};
  const uint32_t single_source[4] = {0x40800000, 0, 0, 0};
  __asm__ volatile("movups %0, %%xmm0; movups %1, %%xmm1; sqrtss %%xmm1, %%xmm0; movups %%xmm0, %0"
                   : "+m"(single) : "m"(single_source) : "xmm0", "xmm1");
  if (single[0] != 0x40000000 || single[1] != 0x12345678 ||
      single[2] != 0x87654321 || single[3] != 0xabcdef01) return 1;

  uint64_t dual[2] = {0x4022000000000000ULL, 0x123456789abcdef0ULL};
  const uint64_t dual_source[2] = {0x4022000000000000ULL, 0};
  __asm__ volatile("movupd %0, %%xmm0; movupd %1, %%xmm1; sqrtsd %%xmm1, %%xmm0; movupd %%xmm0, %0"
                   : "+m"(dual) : "m"(dual_source) : "xmm0", "xmm1");
  if (dual[0] != 0x4008000000000000ULL || dual[1] != 0x123456789abcdef0ULL) return 2;
  return 0;
}

int main(void) {
  int result = check_scalar_sqrt();
  if (result) return result;
  puts("scalar XMM low-lane extraction and upper-lane preservation passed");
  return 0;
}
