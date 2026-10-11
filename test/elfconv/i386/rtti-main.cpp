#include "rtti-types.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <typeinfo>

static void require(bool value, const char *message) {
  if (!value) { std::fprintf(stderr, "failed: %s\n", message); std::exit(1); }
}
int main() {
  void *library = dlopen("libplugin.so", RTLD_NOW);
  require(library != nullptr, "C++ library open");
  auto mixed_factory = reinterpret_cast<Left *(*)()>(dlsym(library, "rtti_mixed"));
  auto diamond_factory = reinterpret_cast<BranchA *(*)()>(dlsym(library, "rtti_diamond"));
  auto plugin_cast = reinterpret_cast<int (*)(Left *)>(dlsym(library, "rtti_is_mixed"));
  auto single_factory = reinterpret_cast<Left *(*)()>(dlsym(library, "rtti_single"));
  auto direct_import = reinterpret_cast<unsigned (*)()>(dlsym(library, "rtti_direct_import"));
  require(mixed_factory && diamond_factory && plugin_cast && single_factory && direct_import, "C++ exports");
  require(direct_import() == 10, "direct and address-taken libc imports");
  Single *single = dynamic_cast<Single *>(single_factory());
  require(single && single->value == 505, "single-inheritance RTTI");
  Left *left = mixed_factory();
  require(left != nullptr, "polymorphic object");
  Mixed *mixed = dynamic_cast<Mixed *>(left);
  Right *right = dynamic_cast<Right *>(left);
  require(mixed && right && mixed->value == 303 && right->right == 202, "downcast and adjusted cross-cast");
  require(dynamic_cast<Unrelated *>(left) == nullptr, "failed cast");
  require(dynamic_cast<void *>(right) == mixed, "complete-object address");
  require(typeid(*left) == typeid(Mixed) && plugin_cast(left), "cross-library type identity");
  BranchA *branch = diamond_factory();
  VirtualBase *base = branch;
  BranchB *sibling = dynamic_cast<BranchB *>(base);
  require(sibling && sibling->value == 404 && dynamic_cast<Diamond *>(base), "virtual diamond cast");
  std::printf("rtti:mixed=%d/%d diamond=%d rejected=1 identity=1\n", mixed->value, right->right, sibling->value);
  require(dlclose(library) == 0, "C++ library close");
  return 0;
}
