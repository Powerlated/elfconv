#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static void emit(int fd, const char *text) {
  size_t remaining = strlen(text);
  while (remaining) {
    ssize_t written = write(fd, text, remaining);
    if (written <= 0) _exit(1);
    text += written;
    remaining -= (size_t)written;
  }
}

static void require(int condition, const char *message) {
  if (!condition) {
    emit(STDERR_FILENO, "failed: ");
    emit(STDERR_FILENO, message);
    emit(STDERR_FILENO, "\n");
    exit(1);
  }
}

int main(void) {
  char text[64] = "Alpha Beta";
  require(strchr(text, 'B') == text + 6 && strchr(text, 0) == text + 10,
          "strchr returns guest pointers including terminator");
  require(strstr(text, "Beta") == text + 6 && strstr(text, "") == text && !strstr(text, "missing"), "strstr");
  require(strcasestr(text, "bETA") == text + 6 && strpbrk(text, "xyB") == text + 6, "string searches");
  require(!strcasecmp("aBc", "AbC") && strcasecmp("a", "B") < 0, "case comparison");
  const char prefix[] = {'A', 'b'};
  require(!strncasecmp(prefix, "aBextra", sizeof(prefix)), "bounded unterminated prefix");
  const unsigned char binary[] = {0, 0xfe, 0x7f, 0};
  require(memchr(binary, 0xfe, sizeof(binary)) == binary + 1 && !memchr(binary, 5, sizeof(binary)), "binary search");
  char *copy = strdup(text);
  require(copy && copy != text && !strcmp(copy, text), "owned strdup");
  copy[0] = 'X';
  require(text[0] == 'A', "strdup is independent");
  free(copy);
  require(stpcpy(text, "one") == text + 3 && strcat(text, " two") == text, "copy and append pointers");
  const char suffix[] = {'!', '?', 'x'};
  require(strncat(text, suffix, 2) == text && !strcmp(text, "one two!?"), "bounded nonterminated append");
  require((tolower)('Q') == 'q' && (toupper)('q') == 'Q' && (tolower)(-1) == -1, "character mapping");
  const int32_t *upper = *__ctype_toupper_loc();
  require(upper['q'] == 'Q' && upper[-1] == -1 && upper[-2] == 254, "glibc upper table and signed indices");
  require((isspace)('\t') && !(isspace)('x') && (isalnum)('7') && !(isalnum)('!'), "character classification");
  emit(STDOUT_FILENO, "libc:strings-ctype\n");

  char directory[] = "elfconv-adapters-XXXXXX";
  char original[PATH_MAX], resolved[PATH_MAX];
  require(getcwd(original, sizeof(original)) == original, "caller-owned cwd");
  char *owned_cwd = getcwd(NULL, PATH_MAX);
  require(owned_cwd && !strcmp(owned_cwd, original), "allocated cwd");
  owned_cwd[PATH_MAX - 1] = 0;  /* getcwd(NULL, size) owns at least size bytes. */
  free(owned_cwd);
  require(mkdtemp(directory) == directory && !chdir(directory), "create and enter directory");
  errno = 0;
  require(open("missing", O_RDONLY) == -1 && errno == ENOENT && errno == 2, "Linux ENOENT translation");
  errno = 777;
  require(strlen(text) == 9 && errno == 777, "successful calls preserve guest errno");
  int fd = open("data", O_CREAT | O_EXCL | O_RDWR, 0600);
  require(fd >= 0 && write(fd, "hello\nworld", 11) == 11 && !close(fd), "descriptor write");
  errno = 0;
  require(open("data", O_CREAT | O_EXCL | O_WRONLY, 0600) == -1 && errno == EEXIST && errno == 17, "Linux EEXIST translation");
  fd = open("data", O_RDONLY);
  char content[32] = {};
  require(fd >= 0 && read(fd, content, sizeof(content)) == 11 && !strcmp(content, "hello\nworld"), "descriptor read");
  require(!close(fd), "descriptor close");
  require(!chmod("data", 0640) && !access("data", R_OK) && !rename("data", "renamed"), "file metadata and rename");
  require(!symlink("renamed", "alias"), "symlink");
  memset(content, '!', sizeof(content));
  require(readlink("alias", content, sizeof(content)) == 7 && content[7] == '!' && !memcmp(content, "renamed", 7), "readlink does not append NUL");
  require(realpath("alias", resolved) == resolved, "caller-owned realpath");
  char *owned_path = realpath("renamed", NULL);
  require(owned_path && !strcmp(owned_path, resolved), "allocated canonical path");
  free(owned_path);
  require(!unlink("alias") && !unlink("renamed"), "unlink files");
  require(!chdir(original) && !rmdir(directory), "restore cwd and remove directory");
  emit(STDOUT_FILENO, "libc:filesystem-errno\n");
  return 0;
}
