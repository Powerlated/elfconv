#include "rtti-types.h"
#include <cstring>

static Mixed mixed;
static Diamond diamond;
static Single single;
extern "C" Left *rtti_mixed(void) { return &mixed; }
extern "C" BranchA *rtti_diamond(void) { return &diamond; }
extern "C" int rtti_is_mixed(Left *object) { return dynamic_cast<Mixed *>(object) != nullptr; }
extern "C" Left *rtti_single(void) { return &single; }

static size_t (*volatile imported_strlen)(const char *) = std::strlen;
extern "C" unsigned rtti_direct_import(void) {
  const char *word = "guest";
  unsigned length;
  __asm__ volatile("push %[word]; call strlen; add $4, %%esp"
                   : "=a"(length) : [word] "r"(word) : "ecx", "edx", "memory", "cc");
  return length + imported_strlen(word);
}
