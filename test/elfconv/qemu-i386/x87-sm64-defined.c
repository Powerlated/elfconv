#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Exercise SM64's x87 path without depending on guest long-double printf. */
static void print_double(const char *name, double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  printf("%s=%016llx\n", name, (unsigned long long) bits);
}

__attribute__((noinline)) static void arithmetic(double a, double b) {
  print_double("add", a + b);
  print_double("sub", a - b);
  print_double("mul", a * b);
  print_double("div", a / b);
}

static void compare(double a, double b) {
  unsigned flags;
  __asm__ volatile (
      "fldl %2\n\tfldl %1\n\tfucomip %%st(1), %%st\n\t"
      "fstp %%st(0)\n\tpushfl\n\tpopl %0"
      : "=r" (flags) : "m" (a), "m" (b) : "st", "cc");
  printf("compare=%02x\n", flags & 0x45);
}

int main(void) {
  const uint64_t nan_bits = UINT64_C(0x7ff8000000000000);
  double nan;
  memcpy(&nan, &nan_bits, sizeof(nan));
  arithmetic(2, 3);
  arithmetic(1.4, -5);
  compare(2, -1);
  compare(2, 2);
  compare(2, 3);
  compare(nan, 1);

  /* A masked invalid operation must not corrupt the next finite FLD. */
  double zero = 0, loaded, finite = 2;
  __asm__ volatile (
      "fldl %1\n\tfldl %1\n\tfdivp\n\tfstp %%st(0)\n\t"
      "fldl %2\n\tfstpl %0"
      : "=m" (loaded) : "m" (zero), "m" (finite) : "st");
  print_double("load-after-invalid", loaded);

  uint16_t original;
  __asm__ volatile ("fnstcw %0" : "=m" (original));
  const double values[] = {0.5, -0.5, 1.5, -1.5, 2.5, -2.5,
                           1.0 / 7.0, -1.0 / 9.0, 32768,
                           2147483648.0, -2147483648.0,
                           9223372036854775808.0, nan};
  for (unsigned mode = 0; mode < 4; ++mode) {
    uint16_t control = (original & ~0x0c00u) | (mode << 10);
    __asm__ volatile ("fldcw %0" : : "m" (control));
    uint16_t observed;
    __asm__ volatile ("fnstcw %0" : "=m" (observed));
    printf("rounding=%u:%u\n", mode, (observed >> 10) & 3);
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
      int64_t integer;
      double rounded;
      __asm__ volatile ("fldl %1\n\tfistpll %0"
                        : "=m" (integer) : "m" (values[i]) : "st");
      __asm__ volatile ("fldl %1\n\tfrndint\n\tfstpl %0"
                        : "=m" (rounded) : "m" (values[i]) : "st");
      printf("integer/%u/%u=%016llx\n", mode, i,
             (unsigned long long) integer);
      print_double("rounded", rounded);
    }
  }
  __asm__ volatile ("fldcw %0" : : "m" (original));

  /* Game/libc code also spills x87 values through 80-bit memory slots. */
  long double extended;
  double restored;
  float single = 1.25f, product;
  __asm__ volatile ("flds %2\n\tfmuls %2\n\tfstps %0\n\t"
                    "fldl %3\n\tfstpt %1"
                    : "=m" (product), "=m" (extended)
                    : "m" (single), "m" (finite) : "st");
  __asm__ volatile ("fldt %1\n\tfstpl %0"
                    : "=m" (restored) : "m" (extended) : "st");
  print_double("single-product", product);
  print_double("extended-roundtrip", restored);
  return 0;
}
