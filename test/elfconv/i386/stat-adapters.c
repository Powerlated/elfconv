#define _LARGEFILE64_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
extern int __xstat64(int, const char *, struct stat64 *);
extern int __lxstat64(int, const char *, struct stat64 *);
extern int __fxstat64(int, int, struct stat64 *);

int main(void) {
  const char *path = "stat-adapters.tmp", *link = "stat-adapters.link";
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0 || write(fd, "portal", 6) != 6 || chmod(path, 0640)) return 1;
  struct stat64 named, descriptor, target, symbolic;
  if (__xstat64(3, path, &named) || __fxstat64(3, fd, &descriptor)) return 2;
  if (!S_ISREG(named.st_mode) || (named.st_mode & 0777) != 0640 || named.st_size != 6 ||
      !named.st_ino || named.st_nlink != 1 || named.st_ino != descriptor.st_ino ||
      named.st_dev != descriptor.st_dev || named.st_size != descriptor.st_size ||
      named.st_mtim.tv_sec <= 0 || named.st_mtim.tv_nsec < 0 ||
      named.st_mtim.tv_nsec >= 1000000000) return 3;
  if (symlink(path, link) || __xstat64(3, link, &target) || __lxstat64(3, link, &symbolic)) return 4;
  if (target.st_ino != named.st_ino || !S_ISLNK(symbolic.st_mode) ||
      symbolic.st_size != 17 || symbolic.st_ino == target.st_ino) return 5;
  errno = 0;
  if (__fxstat64(3, -1, &descriptor) != -1 || errno != EBADF) return 6;
  if (close(fd) || unlink(link) || unlink(path)) return 7;
  const char marker[] = "stat64 file, descriptor, symlink and error layouts passed\n";
  if (write(STDOUT_FILENO, marker, sizeof(marker) - 1) != (ssize_t)(sizeof(marker) - 1)) return 8;
  return 0;
}
