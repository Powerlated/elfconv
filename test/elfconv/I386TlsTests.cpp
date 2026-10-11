#include <gtest/gtest.h>
#include <sys/mman.h>
#include <atomic>
#include <thread>

#define ELF_IS_I386 1
#define ADDRESS_SIZE_BITS 32
#include "../../src/runtime/I386Imports.cpp"

namespace {
const uint8_t tls_template[] = {7, 0, 0, 0};
const uint32_t no_dependencies[] = {UINT32_MAX};
const uint32_t no_callbacks[] = {0};
}
extern "C" const uint32_t _ecv_i386_initializers[] = {0};
extern "C" const uint32_t _ecv_i386_finalizers[] = {0};
extern "C" const I386DynamicLibrary _ecv_i386_libraries[] = {
    {"main", "main", "main", no_dependencies, nullptr, 0, no_callbacks, no_callbacks,
     0x08000000, 0x08001000, tls_template, sizeof(tls_template), 128, 64, 128}};
extern "C" const uint32_t _ecv_i386_library_count = 1;
extern "C" const uint32_t _ecv_i386_tls_static_size = 128;
extern "C" const uint32_t _ecv_i386_tls_static_alignment = 64;
extern "C" const uint64_t _ecv_data_sec_num = 0;
extern "C" const uint64_t _ecv_data_sec_vma_array[] = {};
extern "C" const uint64_t _ecv_data_sec_size_array[] = {};
extern "C" {
const uint8_t *_ecv_data_sec_name_ptr_array[] = {};
const uint8_t *_ecv_data_sec_bytes_ptr_array[] = {};
}
void elfconv_runtime_error(const char *, ...) { std::abort(); }

namespace {
class TlsHarness {
 public:
  TlsHarness() : process(1, 0, 1, nullptr, &main_state, {}), runtime(&process) {
    arena = static_cast<uint8_t *>(mmap(nullptr, MEMORY_ARENA_SIZE,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (arena == MAP_FAILED) throw std::bad_alloc();
    __ecv_i386_initialize_thread_tls(arena, &main_state, &runtime);
  }
  ~TlsHarness() { munmap(arena, MEMORY_ARENA_SIZE); }
  uint32_t address(State &state, uint32_t offset = 0) {
    return __ecv_i386_tls_address(arena, &state, &runtime, 1, offset);
  }
  uint32_t word(uint32_t address) {
    uint32_t value;
    memcpy(&value, arena + address, sizeof(value));
    return value;
  }
  uint8_t *arena;
  State main_state{};
  EcvProcess process;
  RuntimeManager runtime;
};

TEST(I386Tls, ConcurrentContextsKeepInitializedAndZeroStorageSeparate) {
  TlsHarness harness;
  State states[2]{};
  uint32_t addresses[2]{};
  uint32_t observed[2]{};
  std::atomic<unsigned> ready{0};
  std::atomic<bool> release{false};
  std::thread workers[2];
  for (unsigned i = 0; i < 2; ++i) {
    workers[i] = std::thread([&, i] {
      __ecv_i386_initialize_thread_tls(harness.arena, &states[i], &harness.runtime);
      addresses[i] = harness.address(states[i]);
      observed[i] = harness.word(addresses[i]);
      const uint32_t value = 31 + 16 * i;
      memcpy(harness.arena + addresses[i], &value, sizeof(value));
      harness.arena[addresses[i] + 127] = 23 + i;
      ready.fetch_add(1, std::memory_order_release);
      while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
      __ecv_i386_finalize_thread_tls(harness.arena, &states[i], &harness.runtime);
    });
  }
  while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
  EXPECT_EQ(observed[0], 7u);
  EXPECT_EQ(observed[1], 7u);
  EXPECT_EQ(harness.word(addresses[0]), 31u);
  EXPECT_EQ(harness.word(addresses[1]), 47u);
  EXPECT_EQ(harness.arena[addresses[0] + 127], 23);
  EXPECT_EQ(harness.arena[addresses[1] + 127], 24);
  EXPECT_EQ(harness.word(harness.address(harness.main_state)), 7u);
  EXPECT_EQ(harness.arena[harness.address(harness.main_state, 127)], 0);
  for (unsigned i = 0; i < 2; ++i) {
    EXPECT_EQ(addresses[i] % 64, 0u);
    const uint32_t tp = states[i].addr.gs_base.dword;
    EXPECT_EQ(harness.word(tp), tp);
    EXPECT_EQ(harness.word(harness.word(tp + 4) + 8), addresses[i]);
    EXPECT_EQ(harness.word(tp + 12), 1u);
  }
  release.store(true, std::memory_order_release);
  for (auto &worker : workers) worker.join();
  for (auto &state : states) EXPECT_EQ(state.addr.gs_base.dword, 0u);
}

TEST(I386Tls, NewContextRestoresTemplateAfterPreviousContextExits) {
  TlsHarness harness;
  memset(harness.arena + harness.address(harness.main_state), 0xa5, 128);
  __ecv_i386_finalize_thread_tls(harness.arena, &harness.main_state, &harness.runtime);
  State next{};
  __ecv_i386_initialize_thread_tls(harness.arena, &next, &harness.runtime);
  EXPECT_EQ(harness.word(harness.address(next)), 7u);
  EXPECT_EQ(harness.arena[harness.address(next, 127)], 0);
  __ecv_i386_finalize_thread_tls(harness.arena, &next, &harness.runtime);
}

TEST(I386Tls, ResolverRejectsOffsetPastModuleStorage) {
  TlsHarness harness;
  EXPECT_DEATH(harness.address(harness.main_state, 128), "");
}
}  // namespace
