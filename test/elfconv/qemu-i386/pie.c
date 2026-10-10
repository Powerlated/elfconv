#include <stdio.h>

static int value = 37;
static int *volatile pointer = &value;
static int add(int x) { return x + *pointer; }
static int (*volatile callback)(int) = add;
static int initialized;
__attribute__((constructor)) static void initialize(void) { initialized = callback(5); }
__attribute__((destructor)) static void finalize(void) { printf("final=%d\n", *pointer); }

int main(void) {
    fprintf(stdout, "init=%d callback=%d data=%d\n", initialized, callback(9), *pointer);
    fprintf(stderr, "stdio=%d\n", initialized);
    *pointer = 11;
    printf("changed=%d\n", callback(7));
    return initialized != 42 || callback(7) != 18;
}
