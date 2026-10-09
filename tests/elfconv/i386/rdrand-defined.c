#include <stdint.h>
#include <stdio.h>

int main(void) {
  uint32_t value, flags, sentinel;
  __asm__ volatile("movl $0x12345678, %%edx; pushl $0x8d7; popfl; "
                   "rdrand %%eax; pushfl; popl %%ecx"
                   : "=a"(value), "=c"(flags), "=d"(sentinel) : : "cc");
  const int success = flags & 1;
  const int clear_flags = !(flags & 0x8d4);
  const int preserved = sentinel == 0x12345678;
  const int failed_zero = success || value == 0;
  printf("rdrand:flags=%d preserved=%d failure=%d\n", clear_flags, preserved, failed_zero);
  return !(clear_flags && preserved && failed_zero);
}
