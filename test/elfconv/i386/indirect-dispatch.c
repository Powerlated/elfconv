#include <stdio.h>

extern int local_dispatch(int selector, int value);
extern int tail_dispatch(int value, int (*target)(int));
extern int unwind_dispatch(int value);

__attribute__((noinline)) static int add_seven(int value) {
  return value + 7;
}

__attribute__((noinline)) static int triple(int value) {
  return value * 3;
}

int main(void) {
  for (int selector = 0; selector < 2; ++selector) {
    for (int value = -3; value <= 3; ++value) {
      int local = local_dispatch(selector, value);
      int tail = tail_dispatch(value, selector ? triple : add_seven);
      int unwind = unwind_dispatch(value);
      if (local != (selector ? value * 5 : value - 11) ||
          tail != (selector ? value * 3 : value + 7) ||
          unwind != value + 7)
        return 1;
      printf("%d %d: %d %d %d\n", selector, value, local, tail, unwind);
    }
  }
  return 0;
}
