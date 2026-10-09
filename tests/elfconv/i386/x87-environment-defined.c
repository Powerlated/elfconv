#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct __attribute__((packed)) environment {
  uint16_t control, reserved0, status, reserved1, tags, reserved2;
  uint32_t ip;
  uint16_t cs, opcode;
  uint32_t dp;
  uint16_t ds, reserved3;
};

int main(void) {
  struct environment saved, moved;
  const uint16_t control = 0x037e;
  uint16_t masked, restored;
  const double zero = 0.0, negative = -7.75;
  double shifted, a, b;
  __asm__ volatile("fninit; fldcw %2; fldl %3; fldl %4; fnstenv %0; fnstcw %1"
                   : "=m"(saved), "=m"(masked)
                   : "m"(control), "m"(zero), "m"(negative)
                   : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
  moved = saved;
  moved.status = (moved.status & ~0x3800u) | 0x3800u;
  __asm__ volatile("fldenv %1; fstpl %0" : "=m"(shifted) : "m"(moved)
                   : "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
  __asm__ volatile("fldenv %3; fnstcw %0; fstpl %1; fstpl %2; fninit"
                   : "=m"(restored), "=m"(a), "=m"(b)
                   : "m"(saved)
                   : "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
  int ok = saved.control == control && masked == 0x037f && restored == control &&
           ((saved.status >> 11) & 7) == 6 && saved.tags == 0x4fff &&
           shifted == zero && a == negative && b == zero && saved.ip != 0;
  printf("x87-environment=%d cw=%04x/%04x/%04x top=%u tags=%04x values=%d/%d/%d ip=%d\n",
         ok, saved.control, masked, restored, (saved.status >> 11) & 7, saved.tags,
         shifted == zero, a == negative, b == zero, saved.ip != 0);
  return !ok;
}
