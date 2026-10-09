/* Differential checks for defined 8-, 16-, and 32-bit shift behavior. */
#include <stdint.h>
#include <stdio.h>

#define RUN_SHIFT(name, instruction, initial_value, shift_amount) do {          \
    uint32_t result, flags;                                                      \
    uint8_t shift_count = (uint8_t)(shift_amount);                               \
    __asm__ volatile (                                                          \
        "movl %[value], %%eax\n\t"                                             \
        "cmpl $0x8000000a, %%eax\n\t"                                           \
        "movb %[count], %%cl\n\t"                                              \
        instruction " %%cl, %%eax\n\t"                                         \
        "pushfl\n\t"                                                           \
        "popl %[flags]\n\t"                                                    \
        : "=a" (result), [flags] "=m" (flags)                                 \
        : [value] "r" (initial_value), [count] "m" (shift_count)                 \
        : "ecx", "cc", "memory");                                             \
    unsigned masked = shift_count & 31u;                                        \
    unsigned flag_mask = masked == 0 ? 0x8d5u : 0x0c4u;                        \
    if (masked != 0) flag_mask |= 0x001u;                                      \
    if (masked == 1) flag_mask |= 0x800u;                                       \
    printf(name " %08x %03x\n", result, flags & flag_mask);                    \
} while (0)

#define CHECK_OP(name, instruction) do {                                         \
    RUN_SHIFT(name, instruction, 0x80000005u, 0);                               \
    RUN_SHIFT(name, instruction, 0x80000005u, 1);                               \
    RUN_SHIFT(name, instruction, 0x80000005u, 7);                               \
    RUN_SHIFT(name, instruction, 0x80000005u, 31);                              \
    RUN_SHIFT(name, instruction, 0x80000005u, 32);                              \
    RUN_SHIFT(name, instruction, 0x80000005u, 33);                              \
    RUN_SHIFT(name, instruction, 0x7fffffffu, 1);                               \
} while (0)

#define RUN_NARROW(name, instruction, reg, width, shift_amount, arithmetic) do { \
    uint32_t result, flags;                                                      \
    uint8_t shift_count = (uint8_t)(shift_amount);                               \
    __asm__ volatile (                                                          \
        "movl %[value], %%eax\n\t"                                             \
        "cmpl $0x8000000a, %%eax\n\t"                                           \
        "movb %[count], %%cl\n\t"                                              \
        instruction " %%cl, " reg "\n\t"                                      \
        "pushfl\n\t"                                                           \
        "popl %[flags]\n\t"                                                    \
        : "=a" (result), [flags] "=m" (flags)                                 \
        : [value] "r" (0x12345685u), [count] "m" (shift_count)              \
        : "ecx", "cc", "memory");                                             \
    unsigned masked = shift_count & 31u;                                        \
    unsigned flag_mask = masked == 0 ? 0x8d5u : 0x0c4u;                        \
    if (masked != 0 && ((arithmetic) || masked < (width)))                    \
        flag_mask |= 0x001u;                                                    \
    if (masked == 1) flag_mask |= 0x800u;                                       \
    printf(name " %u %u %08x %03x\n", (unsigned)(width), shift_count,          \
           result, flags & flag_mask);                                         \
} while (0)

#define CHECK_NARROW(name, instruction, reg, width, arithmetic) do {            \
    RUN_NARROW(name, instruction, reg, width, 0, arithmetic);                   \
    RUN_NARROW(name, instruction, reg, width, 1, arithmetic);                   \
    RUN_NARROW(name, instruction, reg, width, (width) - 1, arithmetic);         \
    RUN_NARROW(name, instruction, reg, width, width, arithmetic);              \
    RUN_NARROW(name, instruction, reg, width, (width) + 1, arithmetic);         \
    RUN_NARROW(name, instruction, reg, width, 31, arithmetic);                  \
} while (0)


int main(void) {
    CHECK_OP("shl", "shll");
    CHECK_OP("shr", "shrl");
    CHECK_OP("sar", "sarl");
    CHECK_NARROW("shlb", "shlb", "%%al", 8, 0);
    CHECK_NARROW("shrb", "shrb", "%%al", 8, 0);
    CHECK_NARROW("sarb", "sarb", "%%al", 8, 1);
    CHECK_NARROW("shlw", "shlw", "%%ax", 16, 0);
    CHECK_NARROW("shrw", "shrw", "%%ax", 16, 0);
    CHECK_NARROW("sarw", "sarw", "%%ax", 16, 1);
    return 0;
}
