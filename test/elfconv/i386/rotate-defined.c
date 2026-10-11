#include <stdio.h>

#define ROTATE_CASE(OP, SUFFIX, TYPE, VALUE, COUNT)                            \
  do {                                                                        \
    TYPE value = (VALUE);                                                     \
    unsigned char count = (COUNT);                                            \
    unsigned flags;                                                           \
    unsigned masked_count = count & 31u;                                      \
    __asm__ volatile (                                                        \
        "pushl $0x802\n\t"                                                    \
        "popfl\n\t"                                                           \
        #OP #SUFFIX " %%cl, %0\n\t"                                          \
        "pushfl\n\t"                                                         \
        "popl %1\n\t"                                                        \
        : "+&q" (value), "=r" (flags)                                        \
        : "c" (count)                                                        \
        : "cc", "memory");                                                    \
    printf(#OP #SUFFIX "/%u=%0*x:%u:%u\n", (unsigned) count,                 \
           (int) sizeof(value) * 2, (unsigned) value, flags & 1u,             \
           masked_count <= 1 ? (flags >> 11) & 1u : 0u);                      \
  } while (0)

int main(void) {
  ROTATE_CASE(rol, b, unsigned char, 0x81, 0);
  ROTATE_CASE(rol, b, unsigned char, 0x81, 32); /* masked count zero */
  ROTATE_CASE(rol, b, unsigned char, 0x81, 8);  /* full-width rotation */
  ROTATE_CASE(rol, b, unsigned char, 0x81, 1);
  ROTATE_CASE(rol, w, unsigned short, 0x8001, 0);
  ROTATE_CASE(rol, w, unsigned short, 0x8001, 32); /* masked count zero */
  ROTATE_CASE(rol, w, unsigned short, 0x8001, 16); /* full-width rotation */
  ROTATE_CASE(rol, w, unsigned short, 0x8001, 1);
  ROTATE_CASE(rol, l, unsigned int, 0x80000001u, 0);
  ROTATE_CASE(rol, l, unsigned int, 0x80000001u, 32); /* masked count zero */
  ROTATE_CASE(rol, l, unsigned int, 0x80000001u, 1);
  ROTATE_CASE(ror, b, unsigned char, 0x81, 0);
  ROTATE_CASE(ror, b, unsigned char, 0x81, 32); /* masked count zero */
  ROTATE_CASE(ror, b, unsigned char, 0x81, 8);  /* full-width rotation */
  ROTATE_CASE(ror, b, unsigned char, 0x81, 1);
  ROTATE_CASE(ror, w, unsigned short, 0x8001, 0);
  ROTATE_CASE(ror, w, unsigned short, 0x8001, 32); /* masked count zero */
  ROTATE_CASE(ror, w, unsigned short, 0x8001, 16); /* full-width rotation */
  ROTATE_CASE(ror, w, unsigned short, 0x8001, 1);
  ROTATE_CASE(ror, l, unsigned int, 0x80000001u, 0);
  ROTATE_CASE(ror, l, unsigned int, 0x80000001u, 32); /* masked count zero */
  ROTATE_CASE(ror, l, unsigned int, 0x80000001u, 1);
  return 0;
}
