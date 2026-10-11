#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned char original[512] __attribute__((aligned(16)));
static unsigned char saved[512] __attribute__((aligned(16)));
static unsigned char popped[512] __attribute__((aligned(16)));
static const uint32_t vector0[4] = {0x12345678, 0x23456789, 0x3456789a, 0x456789ab};
static const uint32_t vector7[4] = {0xabcdef01, 0xbcdef012, 0xcdef0123, 0xdef01234};

__attribute__((target("sse2"))) int main(void) {
  const double first = 2.5, second = -7.75;
  const uint16_t control = 0x077f;
  const uint32_t mxcsr = 0x3f80, default_mxcsr = 0x1f80;
  uint32_t restored_mxcsr, restored0[4], restored7[4];
  uint16_t restored_control, restored_status;
  double a, b;
  memset(saved, 0xa5, sizeof(saved));
  __asm__ volatile("fxsave %0" : "=m"(original) : : "memory");
  __asm__ volatile("fninit; fldcw %0; ldmxcsr %1; fldl %2; fldl %3; "
                   "movdqu %4, %%xmm0; movdqu %5, %%xmm7; fxsave %6"
                   : : "m"(control), "m"(mxcsr), "m"(first), "m"(second),
                       "m"(vector0), "m"(vector7), "m"(saved)
                   : "memory", "st", "st(1)", "xmm0", "xmm7");
  __asm__ volatile("fninit; ldmxcsr %0; pxor %%xmm0, %%xmm0; pxor %%xmm7, %%xmm7; fxrstor %1"
                   : : "m"(default_mxcsr), "m"(saved) : "memory", "st", "st(1)", "xmm0", "xmm7");
  __asm__ volatile("fnstcw %0; fnstsw %1; stmxcsr %2; movdqu %%xmm0, %3; "
                   "movdqu %%xmm7, %4; fstpl %5; fstpl %6; fxsave %7; fxrstor %8"
                   : "=m"(restored_control), "=m"(restored_status), "=m"(restored_mxcsr),
                     "=m"(restored0), "=m"(restored7), "=m"(a), "=m"(b), "=m"(popped)
                   : "m"(original) : "memory", "st", "st(1)");
  int tail_preserved = 1;
  for (unsigned i = 464; i < 512; ++i) tail_preserved &= saved[i] == 0xa5;
  int vectors = !memcmp(restored0, vector0, sizeof(vector0)) &&
                !memcmp(restored7, vector7, sizeof(vector7));
  printf("fxsave:cw=%04x top=%u tags=%02x mxcsr=%04x values=%.2f/%.2f vectors=%d tail=%d empty=%02x\n",
         restored_control, (restored_status >> 11) & 7, saved[4], restored_mxcsr,
         a, b, vectors, tail_preserved, popped[4]);
  return !(restored_control == control && ((restored_status >> 11) & 7) == 6 &&
           saved[4] == 0xc0 && restored_mxcsr == mxcsr && a == second && b == first &&
           vectors && tail_preserved && popped[4] == 0);
}
