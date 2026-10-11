#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef STARTUP_LINKED
extern int plugin_call(int);
#endif

int main_export(int value) { return value + 9; }

static void require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "failed: %s\n", message); exit(1); }
}

int main(void) {
#ifdef STARTUP_LINKED
  require(plugin_call(1) == 40, "startup dependency constructors");
#endif
  dlerror();
  require(!dlopen("libmissing-elfconv.so", RTLD_NOW), "missing library");
  require(dlerror() != NULL && dlerror() == NULL, "dlerror read clears error");
  void *library = dlopen("./libplugin.so", RTLD_NOW | RTLD_LOCAL);
  require(library != NULL, "plugin load");
  int (*call)(int) = (int (*)(int))dlsym(library, "plugin_call");
  int *value = (int *)dlsym(library, "plugin_value");
  require(call && value && *value == 22, "exported code and relocated data");
  int (*dependency)(int) = (int (*)(int))dlsym(library, "dependency_add");
  require(dependency && dependency(6) == 23, "dependency symbol scope");
  require(!dlsym(library, "zero_symbol") && !dlerror(), "zero-valued symbol is not an error");
  require(!dlsym(library, "missing_symbol") && dlerror(), "missing symbol");
#ifndef STARTUP_LINKED
  require(!dlsym(RTLD_DEFAULT, "plugin_call") && dlerror(), "local symbols stay local");
#endif
  void *again = dlopen("libplugin.so", RTLD_NOW);
  require(again != NULL, "second reference");
  *value = 40;
  require(call(2) == 59, "writable exported data and indirect call");
  require(!dlclose(library), "first close");
  require(call(3) == 60, "second reference keeps library alive");
  puts("references:alive");
  require(!dlclose(again), "last close");
#ifndef STARTUP_LINKED
  require(!dlopen("libplugin.so", RTLD_NOW | RTLD_NOLOAD), "NOLOAD after close");
  require(dlerror() == NULL, "NOLOAD absence is not a loader error");
#endif
  library = dlopen("libplugin.so", RTLD_NOW | RTLD_GLOBAL);
  require(library != NULL, "global reload");
  value = (int *)dlsym(library, "plugin_value");
#ifndef STARTUP_LINKED
  require(value && *value == 22, "reload restores initialized image");
#endif
  dependency = (int (*)(int))dlsym(RTLD_DEFAULT, "dependency_add");
  require(dependency && dependency(2) == 19, "global dependency scope");
  void *main_handle = dlopen(NULL, RTLD_NOW);
  int (*export)(int) = (int (*)(int))dlsym(main_handle, "main_export");
  require(export && export(4) == 13, "main program exports");
  require(!dlclose(main_handle), "main handle close");
  require(!dlclose(library), "reload close");
  puts("dynamic:ok");
  return 0;
}
