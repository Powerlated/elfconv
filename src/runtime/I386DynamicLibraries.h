#pragma once

#include <cstdint>

struct I386DynamicSymbol {
  const char *name;
  uint32_t address;
  uint32_t tls_module;  // Zero for ordinary symbols; otherwise address is a TLS offset.
};

struct I386DynamicLibrary {
  const char *path;
  const char *name;
  const char *filename;
  const uint32_t *dependencies;  // UINT32_MAX terminated; host adapters are excluded.
  const I386DynamicSymbol *symbols;  // Sorted by name.
  uint32_t symbol_count;
  const uint32_t *initializers;  // Zero terminated.
  const uint32_t *finalizers;
  uint32_t image_begin, image_end;
  const uint8_t *tls_template;
  uint32_t tls_file_size, tls_size, tls_alignment, tls_distance;
};

extern "C" const I386DynamicLibrary _ecv_i386_libraries[];
extern "C" const uint32_t _ecv_i386_library_count;
extern "C" const uint32_t _ecv_i386_tls_static_size;
extern "C" const uint32_t _ecv_i386_tls_static_alignment;
