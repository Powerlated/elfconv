#pragma once

#include <cstdint>

struct State;
class RuntimeManager;

// Call before executing a new guest CPU state, and after its final guest callback.
extern "C" void __ecv_i386_initialize_thread_tls(uint8_t *arena, State *state, RuntimeManager *runtime);
extern "C" void __ecv_i386_finalize_thread_tls(uint8_t *arena, State *state, RuntimeManager *runtime);
extern "C" uint32_t __ecv_i386_tls_address(uint8_t *arena, State *state,
                                         RuntimeManager *runtime, uint32_t module, uint32_t offset);
