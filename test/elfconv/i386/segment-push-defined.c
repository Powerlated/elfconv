#include <stdint.h>
#include <stdio.h>

#define CHECK_SEGMENT(segment, privilege) do { \
  uint16_t selector, narrow; uint32_t before, inside, after, value; \
  __asm__ volatile("movw %%" #segment ", %0" : "=r"(selector)); \
  if ((privilege) && (selector & 3) != 3) return 1; \
  __asm__ volatile("movl %%esp, %0; pushl %%" #segment "; movl %%esp, %1; popl %2; movl %%esp, %3" \
                   : "=&r"(before), "=&r"(inside), "=&r"(value), "=&r"(after) : : "memory"); \
  if (before - inside != 4 || after != before || (uint16_t)value != selector) { \
    printf(#segment " push32: before=%x inside=%x after=%x value=%x selector=%x\n", before, inside, after, value, selector); \
    return 2; \
  } \
  __asm__ volatile("movl %%esp, %0; pushw %%" #segment "; movl %%esp, %1; popw %2; movl %%esp, %3" \
                   : "=&r"(before), "=&r"(inside), "=&r"(narrow), "=&r"(after) : : "memory"); \
  if (before - inside != 2 || after != before || narrow != selector) { \
    printf(#segment " push16: before=%x inside=%x after=%x value=%x selector=%x\n", before, inside, after, narrow, selector); \
    return 3; \
  } \
} while (0)
int main(void) {
  CHECK_SEGMENT(cs, 1); CHECK_SEGMENT(ds, 1); CHECK_SEGMENT(es, 1);
  CHECK_SEGMENT(ss, 1); CHECK_SEGMENT(fs, 0); CHECK_SEGMENT(gs, 1);
  uint32_t flags;
  __asm__ volatile("pushfl; popl %0" : "=r"(flags) : : "memory");
  if ((flags & 0x202) != 0x202) return 4;
  puts("segment stack widths and user privilege selectors passed");
  return 0;
}
