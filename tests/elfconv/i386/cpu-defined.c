#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
  __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
static uint64_t timestamp(void) {
  uint32_t low, high, preserved = 0x12345678;
  unsigned char equal;
  __asm__ volatile("cmpl %%ecx, %%ecx; rdtsc; setz %3"
                   : "=a"(low), "=d"(high), "+c"(preserved), "=qm"(equal) : : "cc");
  if (!equal || preserved != 0x12345678) return 0;
  return ((uint64_t)high << 32) | low;
}
__attribute__((target("sse2")))
int main(void) {
  uint32_t a, b, c, d;
  cpuid(1, &a, &b, &c, &d);
  if (!(d & (1u << 4)) || !(d & (1u << 26))) return 1;
  // CPU detection selects the SSE2 path, then executes the advertised ISA.
  double input[2] = {2.0, 3.0}, output[2];
  __asm__ volatile("movupd %1, %%xmm0; addpd %%xmm0, %%xmm0; movupd %%xmm0, %0"
                   : "=m"(output) : "m"(input) : "xmm0");
  if (output[0] != 4.0 || output[1] != 6.0) return 2;
  const uint64_t before = timestamp();
  if (!before || usleep(1000)) return 3;
  const uint64_t after = timestamp();
  if (after <= before || after - before < 100000) return 4;
  cpuid(0x80000001, &a, &b, &c, &d);
  if (d & (1u << 27)) {
    uint32_t low, high, auxiliary;
    __asm__ volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(auxiliary));
    if ((((uint64_t)high << 32) | low) < after) return 5;
  }
  puts("CPUID-selected SSE2 and monotonic timestamp execution passed");
  return 0;
}
