/*
 * Architecturally defined SHLD/SHRD differential cases for the i386 runner.
 * Compile with: gcc -m32 -O0 -fno-pie -no-pie -o double-shift-defined \
 *   test/elfconv/qemu-i386/double-shift-defined.c
 */
#include <stdint.h>
#include <stdio.h>

static void run16(const char *name, int left, uint16_t dst, uint16_t src,
                  uint8_t count, unsigned flag_mask) {
  unsigned flags;
  if (left) {
    __asm__ volatile(
        "pushl $0x8d7\n\t"
        "popfl\n\t"
        "shldw %%cl, %%bx, %%ax\n\t"
        "pushfl\n\t"
        "popl %%edx"
        : "+a"(dst), "=&d"(flags)
        : "b"(src), "c"(count)
        : "cc");
  } else {
    __asm__ volatile(
        "pushl $0x8d7\n\t"
        "popfl\n\t"
        "shrdw %%cl, %%bx, %%ax\n\t"
        "pushfl\n\t"
        "popl %%edx"
        : "+a"(dst), "=&d"(flags)
        : "b"(src), "c"(count)
        : "cc");
  }
  printf("%s %04x %02x %03x\n", name, dst, count, flags & flag_mask);
}

static void run32(const char *name, int left, uint32_t dst, uint32_t src,
                  uint8_t count, unsigned flag_mask) {
  unsigned flags;
  if (left) {
    __asm__ volatile(
        "pushl $0x8d7\n\t"
        "popfl\n\t"
        "shldl %%cl, %%ebx, %%eax\n\t"
        "pushfl\n\t"
        "popl %%edx"
        : "+a"(dst), "=&d"(flags)
        : "b"(src), "c"(count)
        : "cc");
  } else {
    __asm__ volatile(
        "pushl $0x8d7\n\t"
        "popfl\n\t"
        "shrdl %%cl, %%ebx, %%eax\n\t"
        "pushfl\n\t"
        "popl %%edx"
        : "+a"(dst), "=&d"(flags)
        : "b"(src), "c"(count)
        : "cc");
  }
  printf("%s %08x %02x %03x\n", name, dst, count, flags & flag_mask);
}

int main(void) {
  static const uint8_t defined16[] = {0, 1, 15, 16, 32, 33, 48};
  static const uint8_t defined32[] = {0, 1, 15, 16, 17, 31, 32, 33, 48};
  for (unsigned i = 0; i < sizeof(defined16) / sizeof(defined16[0]); ++i) {
    uint8_t count = defined16[i];
    unsigned effective = count & 31;
    unsigned mask = effective == 0 ? 0x8d5 : effective == 1 ? 0x8c5 : 0x0c5;
    run16("shld16", 1, 0x9234, 0xa5c3, count, mask);
    run16("shrd16", 0, 0x9234, 0xa5c3, count, mask);
  }
  for (unsigned i = 0; i < sizeof(defined32) / sizeof(defined32[0]); ++i) {
    uint8_t count = defined32[i];
    unsigned effective = count & 31;
    unsigned mask = effective == 0 ? 0x8d5 : effective == 1 ? 0x8c5 : 0x0c5;
    run32("shld32", 1, 0x92345678, 0xa5c3f00d, count, mask);
    run32("shrd32", 0, 0x92345678, 0xa5c3f00d, count, mask);
  }
  return 0;
}
