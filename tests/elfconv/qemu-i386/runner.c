/* Keep upstream sources unchanged; discard unused platform tests at link time. */
#define main qemu_upstream_main
#include "vendor/test-i386.c"
#undef main

int main(int argc, char **argv)
{
#ifdef QEMU_I386_FULL
    return qemu_upstream_main(argc, argv);
#else
    (void)argc;
    (void)argv;
    for (void **entry = &__start_initcall; entry != &__stop_initcall; ++entry) {
        ((void (*)(void))*entry)();
    }
    test_bsx();
    test_xcnt();
    test_mul();
    test_jcc();
    test_loop();
    test_bcd();
    test_xchg();
    test_string();
    test_lea();
    test_enter();
    test_conv();
    return 0;
#endif
}
