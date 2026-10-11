#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  time_t stored = 0, now = time(&stored);
  if (now == (time_t)-1 || stored != now) return 1;
  struct timeval wall;
  if (gettimeofday(&wall, 0) || wall.tv_usec < 0 || wall.tv_usec >= 1000000 ||
      wall.tv_sec < now || wall.tv_sec > now + 2) return 2;
  struct timespec before, after, duration = {0, 1000000};
  if (clock_gettime(CLOCK_MONOTONIC, &before) || nanosleep(&duration, 0) ||
      clock_gettime(CLOCK_MONOTONIC, &after)) return 3;
  int64_t elapsed = ((int64_t)after.tv_sec - before.tv_sec) * 1000000000 +
                    after.tv_nsec - before.tv_nsec;
  if (elapsed < 1000000 || after.tv_nsec < 0 || after.tv_nsec >= 1000000000) return 4;
  duration.tv_nsec = 1000000000;
  struct timespec remaining = {123, 456};
  errno = 0;
  if (nanosleep(&duration, &remaining) != -1 || errno != EINVAL ||
      remaining.tv_sec != 123 || remaining.tv_nsec != 456) return 5;
  errno = 0;
  if (clock_gettime(-123, &after) != -1 || errno != EINVAL) return 6;
  if (sleep(0)) return 7;
  puts("time32 outputs, monotonic sleep and invalid arguments passed");
  return 0;
}
