#include <stdio.h>

/* Only CF/ZF are defined for LZCNT/TZCNT; POPCNT defines all arithmetic flags. */
#define COUNT_CASE(OP, SUFFIX, TYPE, VALUE, CONSTRAINT, MASK)                   \
  do {                                                                         \
    TYPE source = (VALUE);                                                     \
    TYPE result = (TYPE) 0x5678u;                                              \
    unsigned flags;                                                          \
    __asm__ volatile (                                                         \
        "pushl $0x8d7\n\t"                                                   \
        "popfl\n\t"                                                          \
        #OP SUFFIX " %2, %0\n\t"                                             \
        "pushfl\n\t"                                                         \
        "popl %1\n\t"                                                        \
        : "+&r" (result), "=r" (flags)                                       \
        : CONSTRAINT (source)                                                 \
        : "cc", "memory");                                                   \
    printf(#OP SUFFIX "/" CONSTRAINT "/%08x=%08x:%04x\n",                     \
           (unsigned) source, (unsigned) result, flags & (MASK));              \
  } while (0)

#define WIDTH_CASES(OP, SUFFIX, TYPE, MASK)                                    \
  do {                                                                         \
    const unsigned values[] = {0, 1, 2, 0x8000, 0xffff,                        \
                               0x80000000u, 0xffffffffu, 0x12340128};          \
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {        \
      COUNT_CASE(OP, SUFFIX, TYPE, values[i], "r", MASK);                    \
      COUNT_CASE(OP, SUFFIX, TYPE, values[i], "m", MASK);                    \
    }                                                                          \
  } while (0)

int main(void) {
  WIDTH_CASES(tzcnt, "w", unsigned short, 0x41u);
  WIDTH_CASES(tzcnt, "l", unsigned, 0x41u);
  WIDTH_CASES(lzcnt, "w", unsigned short, 0x41u);
  WIDTH_CASES(lzcnt, "l", unsigned, 0x41u);
  WIDTH_CASES(popcnt, "w", unsigned short, 0x8d5u);
  WIDTH_CASES(popcnt, "l", unsigned, 0x8d5u);
  return 0;
}
