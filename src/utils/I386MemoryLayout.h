#pragma once

#include <cstdint>

namespace i386_memory {
// Libraries remain below the conventional 0x08000000 executable/PIE base.
constexpr uint32_t kLibraryStart = 0x01000000;
constexpr uint32_t kLibraryEnd = 0x08000000;
constexpr uint32_t kMainImageStart = 0x08000000;
constexpr uint32_t kArenaSize = 512u * 1024 * 1024;
constexpr uint32_t kBrkStart = 160u * 1024 * 1024;
constexpr uint32_t kBrkSize = 192u * 1024 * 1024;
constexpr uint32_t kMmapSize = 144u * 1024 * 1024;
static_assert(kLibraryEnd <= kBrkStart, "Library images must not overlap the heap");
static_assert(kBrkStart + kBrkSize + kMmapSize + 16u * 1024 * 1024 == kArenaSize,
              "Guest memory regions must end at the stack top");
}  // namespace i386_memory
