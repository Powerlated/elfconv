#pragma once

#include "Memory.h"
#include "utils/Util.h"

#include <pthread.h>
#if defined(ELF_IS_AARCH64)
#  include "remill/Arch/Runtime/Types.h"
#else
#  include "remill/Arch/Runtime/RemillTypes.h"
#endif

#include <cassert>
#include <map>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>

#if defined(ELF_IS_I386)
extern thread_local State *CPUState;
#else
extern State *CPUState;
#endif
// for debug
extern bool INVALID_ADDR_ACCESS;

#if defined(ELF_IS_AMD64) || defined(ELF_IS_I386)
extern "C" uint8_t *MemoryArenaPtr;
#endif

extern void *ManageNewForkPthread(void *arg);

class RuntimeManager {
 public:
  RuntimeManager(EcvProcess *__ecv_pr)
      : main_ecv_pr(__ecv_pr),
        main_memory_arena(__ecv_pr->memory_arena) {}

  // Linux system calls emulation
  void SVCBrowserCall(uint8_t *arena_ptr);  // for browser
  void SVCWasiCall(uint8_t *arena_ptr);  // for wasi
  void SVCNativeCall(uint8_t *arena_ptr);  // for native
  // unimplemented syscall
  void UnImplementedBrowserSyscall();
  void UnImplementedWasiSyscall();
  void UnImplementedNativeSyscall();

  // elfconv psuedo-process
  EcvProcess *main_ecv_pr;
  MemoryArena *main_memory_arena;

  std::vector<std::pair<addr_t, LiftedFunc>> addr_funptr_srt_list;
  std::map<addr_t, std::map<uint64_t, uint64_t *>> fun_bb_addr_map;
  std::vector<addr_t> call_stacks;

  // debug
  std::unordered_map<addr_t, const char *> addr_fun_symbol_map;
  std::unordered_map<uint64_t, int> func_cnt_map;
  uint64_t func_calling_cnt = 0;
  #if defined(ELF_IS_I386)
  struct DynamicLibraryState {
    uint32_t references = 0;
    uint32_t tls_destructor_count = 0;
    bool live = false;
    bool global = false;
    std::vector<uint32_t> scope;
    std::vector<uint32_t> lookup_dependencies;
  };
  std::vector<DynamicLibraryState> dynamic_libraries;
  std::vector<uint32_t> dynamic_initialization_order;
  char dynamic_error[512] = {};
  bool dynamic_error_pending = false;
  uint32_t dynamic_arguments[3] = {};
  struct TlsDestructor { uint32_t function, argument, module; };
  struct TlsThread {
    State *state = nullptr;
    uint32_t base = 0, size = 0, pointer = 0, dtv = 0;
    std::vector<TlsDestructor> destructors;
  };
  std::mutex tls_mutex;
  std::recursive_mutex guest_atomic_mutex;
  std::vector<std::unique_ptr<TlsThread>> tls_threads;
  uint32_t tls_next_address = THREAD_PTR;
  uint32_t tls_guards[2] = {};
  #endif
};
