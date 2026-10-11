#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>

int main(void) {
  double integral;
  int exponent;
  float sine, cosine;
  int scalar = floor(1.75) == 1.0 && ceil(-1.75) == -1.0 &&
               pow(2.0, 5.0) == 32.0 && sqrtf(4.0f) == 2.0f &&
               fmodf(-5.5f, 2.0f) == -1.5f;
  int split = modf(-3.75, &integral) == -0.75 && integral == -3.0;
  int extended = frexpl(16.5L, &exponent) == 0.515625L && exponent == 5;
  sincosf(0.0f, &sine, &cosine);
  int pair = sine == 0.0f && cosine == 1.0f;
  printf("math:scalar=%d split=%d extended=%d pair=%d\n", scalar, split, extended, pair);
  return !(scalar && split && extended && pair);
}
