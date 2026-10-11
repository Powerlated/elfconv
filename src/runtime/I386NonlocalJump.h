#pragma once

// Unwind host lifted frames without treating guest jump buffers as host state.
// The invocation boundary resumes the saved guest basic block and return chain.
struct I386NonlocalJump {};

struct State;
class RuntimeManager;
extern "C" void __ecv_i386_dispatch_pending_signals(unsigned char *arena, State *state,
                                                   RuntimeManager *runtime);
