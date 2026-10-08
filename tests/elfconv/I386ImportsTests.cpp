#include <gtest/gtest.h>

#include <sys/mman.h>
#include <unistd.h>

#define ELF_IS_I386 1
#define ADDRESS_SIZE_BITS 32
#include "../../runtime/I386Imports.cpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" const uint32_t _ecv_i386_initializers[] = {0};
extern "C" const uint32_t _ecv_i386_finalizers[] = {0};
extern "C" const uint64_t _ecv_data_sec_num = 1;
extern "C" const uint64_t _ecv_data_sec_vma_array[] = {0x08000000};
extern "C" const uint64_t _ecv_data_sec_size_array[] = {0x01000000};
void elfconv_runtime_error(const char *, ...) { std::abort(); }

namespace {
constexpr uint32_t kStack = 0x0ff00000;
constexpr uint32_t kReturnPc = 0x12345678;
constexpr uint32_t kString = 0x10000;
constexpr uint32_t kFormat = 0x11000;
constexpr uint32_t kOutput = 0x12000;
constexpr uint32_t kData = 0x13000;

class ImportHarness {
 public:
  ImportHarness() {
    allocations.clear();
    arena = static_cast<uint8_t *>(mmap(nullptr, MEMORY_ARENA_SIZE,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (arena == MAP_FAILED) throw std::bad_alloc();
    state.gpr.rsp.dword = kStack;
  }
  ~ImportHarness() { if (arena != MAP_FAILED) munmap(arena, MEMORY_ARENA_SIZE); }

  template <typename... Args>
  void call(void (*adapter)(uint8_t *, State *, uint32_t, RuntimeManager *), Args... args) {
    std::array<uint32_t, sizeof...(Args) + 1> words{kReturnPc, static_cast<uint32_t>(args)...};
    memcpy(arena + kStack, words.data(), words.size() * sizeof(uint32_t));
    state.gpr.rsp.dword = kStack;
    state.gpr.rip.dword = 0;
    adapter(arena, &state, 0, nullptr);
    EXPECT_EQ(state.gpr.rsp.dword, kStack + 4);
    EXPECT_EQ(state.gpr.rip.dword, kReturnPc);
  }

  template <typename T> T at(uint32_t address) const {
    T value{};
    memcpy(&value, arena + address, sizeof(value));
    return value;
  }
  void string(uint32_t address, const char *value) {
    memcpy(arena + address, value, strlen(value) + 1);
  }

  uint8_t *arena = nullptr;
  State state{};
};

TEST(I386LibcImports, GuestAllocationFreeReuseReallocRetentionAndCallocOverflow) {
  ImportHarness h;
  h.call(__ecv_i386_malloc, 16u);
  uint32_t first = h.state.gpr.rax.dword;
  ASSERT_NE(first, 0u);
  EXPECT_GE(first, BRK_START_VMA);
  EXPECT_LT(first, BRK_END_VMA);
  memcpy(h.arena + first, "ownership", 10);

  h.call(__ecv_i386_realloc, first, 40u);
  uint32_t grown = h.state.gpr.rax.dword;
  ASSERT_NE(grown, 0u);
  EXPECT_STREQ(reinterpret_cast<char *>(h.arena + grown), "ownership");

  h.call(__ecv_i386_free, grown);
  h.call(__ecv_i386_malloc, 40u);
  uint32_t reused = h.state.gpr.rax.dword;
  ASSERT_NE(reused, 0u);
  h.call(__ecv_i386_free, reused);

  h.call(__ecv_i386_calloc, 8u, 4u);
  uint32_t zeroed = h.state.gpr.rax.dword;
  ASSERT_NE(zeroed, 0u);
  for (unsigned i = 0; i != 32; ++i) EXPECT_EQ(h.arena[zeroed + i], 0);
  h.call(__ecv_i386_calloc, UINT32_MAX, 2u);
  EXPECT_EQ(h.state.gpr.rax.dword, 0u);
  h.call(__ecv_i386_free, zeroed);
  // Reclaiming/coalescing freed blocks must make the entire guest heap usable.
  h.call(__ecv_i386_malloc, 0x04000000u);
  ASSERT_NE(h.state.gpr.rax.dword, 0u);
  h.call(__ecv_i386_free, h.state.gpr.rax.dword);
}

TEST(I386LibcImports, SnprintfFormatsGuestDoubleAndWideIntegerAndTruncates) {
  ImportHarness h;
  h.string(kFormat, "%0.2f:%lld");
  uint64_t double_bits;
  double value = 1.25;
  memcpy(&double_bits, &value, sizeof(value));
  const uint64_t wide = 0x1122334455667788ULL;
  uint32_t words[] = {kReturnPc, kOutput, 8, kFormat,
      static_cast<uint32_t>(double_bits), static_cast<uint32_t>(double_bits >> 32),
      static_cast<uint32_t>(wide), static_cast<uint32_t>(wide >> 32)};
  memcpy(h.arena + kStack, words, sizeof(words));
  h.state.gpr.rsp.dword = kStack;
  __ecv_i386_snprintf(h.arena, &h.state, 0, nullptr);
  EXPECT_EQ(h.state.gpr.rax.dword, 24u);
  EXPECT_STREQ(reinterpret_cast<char *>(h.arena + kOutput), "1.25:12");
  EXPECT_EQ(h.state.gpr.rsp.dword, kStack + 4);
  EXPECT_EQ(h.state.gpr.rip.dword, kReturnPc);
}

TEST(I386LibcImports, SscanfWritesSignedIntegerDoubleAndWideInteger) {
  ImportHarness h;
  h.string(kString, "-17 2.5 4294967298");
  h.string(kFormat, "%d %lf %lld");
  h.call(__ecv_i386___isoc99_sscanf, kString, kFormat, kData, kData + 8, kData + 16);
  EXPECT_EQ(h.state.gpr.rax.dword, 3u);
  EXPECT_EQ(h.at<int32_t>(kData), -17);
  EXPECT_EQ(h.at<double>(kData + 8), 2.5);
  EXPECT_EQ(h.at<int64_t>(kData + 16), 4294967298LL);
}

TEST(I386LibcImports, GlibcCtypeFlagsSupportEofAndSignedIndexing) {
  ImportHarness h;
  h.call(__ecv_i386___ctype_b_loc);
  uint32_t slot = h.state.gpr.rax.dword;
  uint32_t biased = h.at<uint32_t>(slot);
  auto flags = [&](int ch) { return h.at<uint16_t>(biased + ch * 2); };
  EXPECT_EQ(flags('A'), 0xD508u);
  EXPECT_EQ(flags(' '), 0x6001u);
  EXPECT_EQ(flags(-1), 0u);
  EXPECT_EQ(flags(-128), flags(128));

  h.call(__ecv_i386___ctype_tolower_loc);
  slot = h.state.gpr.rax.dword;
  biased = h.at<uint32_t>(slot);
  EXPECT_EQ(h.at<int32_t>(biased + 'A' * 4), 'a');
  EXPECT_EQ(h.at<int32_t>(biased + -1 * 4), -1);
  EXPECT_EQ(h.state.gpr.rsp.dword, kStack + 4);
}

TEST(I386LibcImports, FileHandlesAndI386StatLayoutAreGuestVisible) {
  ImportHarness h;
  char path[] = "/tmp/elfconv-i386-import-XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  const char payload[] = "adapter-file";
  ASSERT_EQ(write(fd, payload, sizeof(payload) - 1), ssize_t(sizeof(payload) - 1));
  close(fd);

  h.string(kString, path);
  h.string(kFormat, "r");
  h.call(__ecv_i386_fopen, kString, kFormat);
  uint32_t handle = h.state.gpr.rax.dword;
  ASSERT_NE(handle, 0u);
  h.call(__ecv_i386_fread, kData, 1u, uint32_t(sizeof(payload) - 1), handle);
  EXPECT_EQ(h.state.gpr.rax.dword, sizeof(payload) - 1);
  EXPECT_EQ(std::string(reinterpret_cast<char *>(h.arena + kData), sizeof(payload) - 1), payload);
  h.call(__ecv_i386_fclose, handle);
  EXPECT_EQ(h.state.gpr.rax.dword, 0u);

  h.call(__ecv_i386_stat, kString, kData + 0x100);
  EXPECT_EQ(h.state.gpr.rax.dword, 0u);
  struct stat host{};
  ASSERT_EQ(::stat(path, &host), 0);
  EXPECT_EQ(h.at<uint64_t>(kData + 0x100), uint64_t(host.st_dev));
  EXPECT_EQ(h.at<uint32_t>(kData + 0x10c), uint32_t(host.st_ino));
  EXPECT_EQ(h.at<uint32_t>(kData + 0x110), uint32_t(host.st_mode));
  EXPECT_EQ(h.at<uint32_t>(kData + 0x12c), uint32_t(host.st_size));
  EXPECT_EQ(h.at<uint32_t>(kData + 0x138), uint32_t(host.st_atim.tv_sec));
  EXPECT_EQ(h.at<uint32_t>(kData + 0x140), uint32_t(host.st_mtim.tv_sec));
  EXPECT_EQ(h.at<uint32_t>(kData + 0x148), uint32_t(host.st_ctim.tv_sec));
  unlink(path);
}
}  // namespace
