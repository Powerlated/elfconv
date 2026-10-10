#include <stdio.h>

#define CASE(NAME, CODE)                                                       \
  do {                                                                        \
    unsigned value = input, flags;                                            \
    __asm__ volatile(CODE "\n\tpushfl\n\tpopl %1"                            \
                     : "+&r"(value), "=r"(flags) : : "cc", "memory");       \
    printf(NAME "/%08x=%08x:%04x\n", input, value, flags & 0x8d5u);            \
  } while (0)

int main(void) {
  const unsigned inputs[] = {0, 1, 0x7fffffffu, 0x80000000u, 0xffffffffu};
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    unsigned input = inputs[i];
    CASE("add-sub", "addl $1, %0\n\tsubl $2, %0");
    CASE("sub-cmp", "subl $1, %0\n\tcmpl $0, %0");
    CASE("cmp-add", "cmpl $0, %0\n\taddl $1, %0");
    CASE("carry-read", "addl $1, %0\n\tadcl $0, %0");
    CASE("partial-write", "addl $1, %0\n\tincl %0");
    CASE("flag-read", "addl $1, %0\n\tpushfl\n\tpopl %0");
    CASE("across-mov-lea",
         "addl $1, %0\n\tmovl %0, %0\n\tleal 4(%0), %0\n\tsubl $2, %0");
    CASE("zf-cf-live", "addl $1, %0\n\tsetz %b0\n\tincl %0");
    CASE("carry-before-kill", "addl $1, %0\n\tadcl $0, %0\n\tcmpl $0, %0");
    CASE("logical-kill", "subl $1, %0\n\tmovl %0, %0\n\txorl $7, %0");
    CASE("zero-shift-boundary", "addl $1, %0\n\tshll $0, %0");
    CASE("branch-boundary", "addl $1, %0\n\tjnc 1f\n\tmovl $7, %0\n1:");
  }
  return 0;
}
