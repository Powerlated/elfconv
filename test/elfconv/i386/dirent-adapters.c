#define _LARGEFILE64_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  const char *directory = "dirent-adapters.tmp";
  const char *path = "dirent-adapters.tmp/portal";
  if (mkdir(directory, 0700)) return 1;
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd < 0 || close(fd)) return 2;
  DIR *dir = opendir(directory);
  if (!dir) return 3;
  int found = 0;
  struct dirent64 *entry;
  errno = 0;
  while ((entry = readdir64(dir))) {
    if (strcmp(entry->d_name, "portal")) continue;
    if (!entry->d_ino || entry->d_type != DT_REG || entry->d_reclen != 32) return 4;
    ++found;
  }
  if (errno || found != 1 || closedir(dir)) return 5;
  if (unlink(path) || rmdir(directory)) return 6;
  const char marker[] = "directory name, type, inode and record layout passed\n";
  if (write(STDOUT_FILENO, marker, sizeof(marker) - 1) != (ssize_t)(sizeof(marker) - 1)) return 7;
  return 0;
}
