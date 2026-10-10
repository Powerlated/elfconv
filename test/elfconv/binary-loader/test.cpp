#include "lifter/Binary/Loader.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <elf-file>\n", argv[0]);
    return EXIT_FAILURE;
  }

  BinaryLoader::ELFObject elf_obj(argv[1]);
  elf_obj.LoadELF();
  elf_obj.DebugBinary();

  return 0;
}