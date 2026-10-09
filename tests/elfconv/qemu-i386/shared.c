#include <stdio.h>
#include <string.h>

int value = 37;
int *volatile pointer = &value;
int add(int x) { return x + *pointer; }
int (*volatile callback)(int) = add;
int initialized;
__attribute__((constructor)) static void initialize(void) { initialized = callback(5); }
__attribute__((destructor)) static void finalize(void) { printf("final=%d\n", *pointer); }

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "guest-argument")) return 2;
    fprintf(stdout, "init=%d callback=%d data=%d arg=%s\n",
            initialized, callback(9), *pointer, argv[1]);
    *pointer = 11;
    printf("changed=%d\n", add(7));
    return initialized != 42 || callback(7) != 18;
}
