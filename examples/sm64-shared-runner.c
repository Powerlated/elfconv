#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s library.so [game arguments]\n", argv[0]);
        return 2;
    }
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    int (*entry)(int, char **) = (int (*)(int, char **))dlsym(library, "main");
    const char *error = dlerror();
    if (error) {
        fprintf(stderr, "%s\n", error);
        dlclose(library);
        return 1;
    }
    int status = entry(argc - 1, argv + 1);
    dlclose(library);
    return status;
}
