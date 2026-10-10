#include "Runtime.h"
#include "utils/elfconv.h"
#include "I386DynamicLibraries.h"
#include "I386Tls.h"
#include "I386Errno.h"

#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengl.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>
#include <fcntl.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <map>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#ifndef ECV_LEGACY_GL
#define ECV_LEGACY_GL 1
#endif

extern "C" const uint32_t _ecv_i386_initializers[];
extern "C" const uint32_t _ecv_i386_finalizers[];

namespace {
// Low guest memory is reserved for returned host strings. Host pointers never
// escape into guest registers or memory; SDL objects use typed opaque handles.
constexpr uint32_t string_base = 0x20000;
constexpr size_t string_capacity = 4096;
SDL_Window *windows[16] = {};
SDL_GLContext contexts[16] = {};
#ifdef __EMSCRIPTEN__
GLint pack_alignment = 4, pack_row_length = 0, pack_skip_rows = 0, pack_skip_pixels = 0;
GLenum guest_gl_error = GL_NO_ERROR;
#endif

void *guest(uint8_t *arena, uint32_t address, size_t length) {
  if (address < NULL_GUARD_SIZE || address >= MEMORY_ARENA_SIZE ||
      length > MEMORY_ARENA_SIZE - address)
    elfconv_runtime_error("Invalid i386 host-import buffer: 0x%x, %zu bytes.\n", address, length);
  return arena + address;
}
const char *text(uint8_t *arena, uint32_t address) {
  auto *p = static_cast<const char *>(guest(arena, address, 1));
  if (!memchr(p, 0, MEMORY_ARENA_SIZE - address))
    elfconv_runtime_error("Unterminated i386 host-import string.\n");
  return p;
}
const char *bounded_text(uint8_t *arena, uint32_t address, uint32_t limit) {
  auto *p = static_cast<const char *>(guest(arena, address, 1));
  const size_t available = MEMORY_ARENA_SIZE - address;
  if (limit > available && !memchr(p, 0, available))
    elfconv_runtime_error("Unmapped i386 bounded string.\n");
  return p;
}
uint32_t copy_string(uint8_t *arena, const char *value, unsigned slot) {
  if (!value) return 0;
  size_t length = strlen(value) + 1;
  if (length > string_capacity) elfconv_runtime_error("Host-import string is too long.\n");
  uint32_t address = string_base + slot * string_capacity;
  memcpy(guest(arena, address, length), value, length);
  return address;
}
constexpr uint32_t guest_errno_offset = 32;  // Adapter-private word in the reserved TCB.
struct Call {
  uint8_t *arena;
  State *state;
  uint32_t stack;
  int initial_guest_errno = 0, initial_host_errno = 0;
  Call(uint8_t *a, State *s) : arena(a), state(s), stack(s->gpr.rsp.dword) {
    if (state->addr.gs_base.dword) {
      memcpy(&initial_guest_errno, guest(arena, state->addr.gs_base.dword + guest_errno_offset, 4), 4);
      errno = i386_errno_to_host(initial_guest_errno);
    }
    initial_host_errno = errno;
  }
  uint32_t word(unsigned index) const {
    uint32_t value;
    uint64_t address = uint64_t(stack) + 4 * uint64_t(index + 1);
    if (address > UINT32_MAX) elfconv_runtime_error("i386 import stack overflow.\n");
    memcpy(&value, guest(arena, address, 4), 4);
    return value;
  }
  float real(unsigned index) const {
    uint32_t bits = word(index);
    float value;
    memcpy(&value, &bits, 4);
    return value;
  }
  uint64_t wide(unsigned index) const {
    return uint64_t(word(index)) | (uint64_t(word(index + 1)) << 32);
  }
  double real64(unsigned index) const {
    const uint64_t bits = wide(index);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
  }
  void result64(uint64_t value) {
    state->gpr.rax.dword = uint32_t(value);
    state->gpr.rdx.dword = uint32_t(value >> 32);
  }
  void result(uint32_t value) { state->gpr.rax.dword = value; }
  ~Call() {
    if (state->addr.gs_base.dword) {
      const int value = errno == initial_host_errno ? initial_guest_errno : host_errno_to_i386(errno);
      memcpy(guest(arena, state->addr.gs_base.dword + guest_errno_offset, 4), &value, 4);
    }
    uint32_t return_pc;
    memcpy(&return_pc, guest(arena, stack, 4), 4);
    state->gpr.rip.dword = return_pc;
    state->gpr.rsp.dword = stack + 4;
  }
};
SDL_Window *window(uint32_t handle) {
  if (handle < 0x100 || handle >= 0x110 || !windows[handle - 0x100])
    elfconv_runtime_error("Invalid guest SDL window handle.\n");
  return windows[handle - 0x100];
}
SDL_GLContext context(uint32_t handle) {
  if (handle < 0x200 || handle >= 0x210 || !contexts[handle - 0x200])
    elfconv_runtime_error("Invalid guest SDL GL context handle.\n");
  return contexts[handle - 0x200];
}
void invoke(uint8_t *arena, State *state, RuntimeManager *runtime, uint32_t target,
            const uint32_t *args, unsigned count) {
  auto it = std::lower_bound(runtime->addr_funptr_srt_list.begin(),
      runtime->addr_funptr_srt_list.end(), target,
      [](const auto &item, uint32_t value) { return item.first < value; });
  if (it == runtime->addr_funptr_srt_list.end() || it->first != target)
    elfconv_runtime_error("Unknown guest callback 0x%x.\n", target);
  uint32_t saved_stack = state->gpr.rsp.dword;
  uint32_t saved_pc = state->gpr.rip.dword;
  // At function entry ESP+4 must be 16-byte aligned for the i386 SysV ABI.
  uint32_t stack = ((saved_stack - 4 * count - 16) & ~15u) - 4;
  uint32_t return_pc = 0;
  memcpy(guest(arena, stack, 4), &return_pc, 4);
  if (count) memcpy(guest(arena, stack + 4, count * 4), args, count * 4);
  state->gpr.rsp.dword = stack;
  state->gpr.rip.dword = target;
  it->second(arena, state, target, runtime);
  state->gpr.rsp.dword = saved_stack;
  state->gpr.rip.dword = saved_pc;
}

constexpr uint32_t library_handle_base = 0x300;

void dynamic_error(RuntimeManager *runtime, const char *kind, const char *name) {
  snprintf(runtime->dynamic_error, sizeof(runtime->dynamic_error), "%s: %s", kind, name);
  runtime->dynamic_error_pending = true;
}

void prepare_libraries(RuntimeManager *runtime) {
  if (!runtime->dynamic_libraries.empty()) return;
  runtime->dynamic_libraries.resize(_ecv_i386_library_count);
  for (uint32_t i = 0; i < _ecv_i386_library_count; ++i) {
    auto &scope = runtime->dynamic_libraries[i].scope;
    scope.reserve(_ecv_i386_library_count);
    runtime->dynamic_libraries[i].lookup_dependencies.reserve(_ecv_i386_library_count);
    scope.push_back(i);
    for (size_t j = 0; j < scope.size(); ++j) {
      for (auto *p = _ecv_i386_libraries[scope[j]].dependencies; *p != UINT32_MAX; ++p) {
        if (*p >= _ecv_i386_library_count)
          elfconv_runtime_error("Invalid bundled dependency index.\n");
        if (std::find(scope.begin(), scope.end(), *p) == scope.end()) scope.push_back(*p);
      }
    }
  }
  if (_ecv_i386_library_count) {
    auto &main = runtime->dynamic_libraries[0];
    main.references = 1;
    main.live = main.global = true;
  }
}

void reset_tls_module(uint8_t *arena, const RuntimeManager::TlsThread &thread, uint32_t index) {
  const auto &library = _ecv_i386_libraries[index];
  const uint32_t address = library.tls_size ? thread.pointer - library.tls_distance : 0;
  if (library.tls_size) {
    auto *block = guest(arena, address, library.tls_size);
    memset(block, 0, library.tls_size);
    if (library.tls_file_size) memcpy(block, library.tls_template, library.tls_file_size);
  }
  memcpy(guest(arena, thread.dtv + 8 * (index + 1), 4), &address, 4);
}

RuntimeManager::TlsThread &tls_thread(uint8_t *arena, State *state, RuntimeManager *runtime) {
  // Caller holds tls_mutex. CPU-state identity, not the host's TLS pointer, owns guest TLS.
  for (const auto &thread : runtime->tls_threads)
    if (thread->state == state) return *thread;
  RuntimeManager::TlsThread *thread = nullptr;
  for (const auto &slot : runtime->tls_threads)
    if (!slot->state) { thread = slot.get(); break; }
  if (!thread) {
    const uint64_t alignment = _ecv_i386_tls_static_alignment;
    if (alignment < 16 || alignment > 0x100000 || (alignment & (alignment - 1)))
      elfconv_runtime_error("Invalid i386 TLS alignment.\n");
    const uint64_t base = (runtime->tls_next_address + alignment - 1) & ~(alignment - 1);
    const uint64_t size = (_ecv_i386_tls_static_size + 64ull +
        8ull * (_ecv_i386_library_count + 2) + alignment - 1) & ~(alignment - 1);
    if (base + size > 0x01000000)
      elfconv_runtime_error("Guest thread TLS exceeds the reserved TLS region.\n");
    if (runtime->tls_threads.empty()) {
      if (getentropy(runtime->tls_guards, sizeof(runtime->tls_guards)))
        elfconv_runtime_error("Cannot initialize guest TLS guards.\n");
      runtime->tls_guards[0] &= ~0xffu;
    }
    auto slot = std::make_unique<RuntimeManager::TlsThread>();
    slot->base = base;
    slot->size = size;
    slot->pointer = base + _ecv_i386_tls_static_size;
    slot->dtv = slot->pointer + 64 + 8;
    thread = slot.get();
    runtime->tls_threads.push_back(std::move(slot));
    runtime->tls_next_address = base + size;
  }
  thread->state = state;
  memset(guest(arena, thread->base, thread->size), 0, thread->size);
  const uint32_t header[] = {thread->pointer, thread->dtv, thread->pointer,
      0, 0, runtime->tls_guards[0], runtime->tls_guards[1]};
  memcpy(guest(arena, thread->pointer, sizeof(header)), header, sizeof(header));
  const uint32_t count = _ecv_i386_library_count, generation = 1;
  memcpy(guest(arena, thread->dtv - 8, 4), &count, 4);
  memcpy(guest(arena, thread->dtv, 4), &generation, 4);
  for (uint32_t i = 0; i < count; ++i)
    if (runtime->dynamic_libraries[i].live) reset_tls_module(arena, *thread, i);
  state->addr.gs_base.dword = thread->pointer;
  if (std::count_if(runtime->tls_threads.begin(), runtime->tls_threads.end(),
      [](const auto &slot) { return slot->state != nullptr; }) > 1) {
    const uint32_t multiple = 1;
    for (const auto &slot : runtime->tls_threads)
      if (slot->state) memcpy(guest(arena, slot->pointer + 12, 4), &multiple, 4);
  }
  return *thread;
}

void reset_loaded_tls(uint8_t *arena, RuntimeManager *runtime, uint32_t index) {
  std::lock_guard<std::mutex> lock(runtime->tls_mutex);
  for (const auto &thread : runtime->tls_threads)
    if (thread->state) reset_tls_module(arena, *thread, index);
}

void drain_tls_destructors(uint8_t *arena, State *state, RuntimeManager *runtime);

void restore_library(uint8_t *arena, uint32_t index) {
  char prefix[32];
  const int length = snprintf(prefix, sizeof(prefix), "library%u:", index);
  for (size_t i = 0; i < _ecv_data_sec_num; ++i) {
    if (strncmp(reinterpret_cast<const char *>(_ecv_data_sec_name_ptr_array[i]), prefix, length))
      continue;
    memcpy(guest(arena, _ecv_data_sec_vma_array[i], _ecv_data_sec_size_array[i]),
           _ecv_data_sec_bytes_ptr_array[i], _ecv_data_sec_size_array[i]);
  }
}

void open_library(uint8_t *arena, State *state, RuntimeManager *runtime, uint32_t index) {
  auto &loaded = runtime->dynamic_libraries[index];
  if (loaded.references == UINT32_MAX) elfconv_runtime_error("Library reference count overflow.\n");
  if (loaded.references++ || loaded.live) return;
  loaded.live = true;  // Cyclic dependency constructors can look up each other.
  restore_library(arena, index);
  reset_loaded_tls(arena, runtime, index);
  for (auto *p = _ecv_i386_libraries[index].dependencies; *p != UINT32_MAX; ++p)
    open_library(arena, state, runtime, *p);
  for (auto *p = _ecv_i386_libraries[index].initializers; *p; ++p)
    invoke(arena, state, runtime, *p, runtime->dynamic_arguments, 3);
  runtime->dynamic_initialization_order.push_back(index);
}

void close_library(uint8_t *arena, State *state, RuntimeManager *runtime, uint32_t index) {
  auto &loaded = runtime->dynamic_libraries[index];
  if (!loaded.references || --loaded.references || loaded.tls_destructor_count) return;
  for (auto *p = _ecv_i386_libraries[index].finalizers; *p; ++p)
    invoke(arena, state, runtime, *p, nullptr, 0);
  loaded.live = loaded.global = false;
  auto &order = runtime->dynamic_initialization_order;
  order.erase(std::remove(order.begin(), order.end(), index), order.end());
  for (auto *p = _ecv_i386_libraries[index].dependencies; *p != UINT32_MAX; ++p)
    close_library(arena, state, runtime, *p);
  for (uint32_t dependency : loaded.lookup_dependencies)
    close_library(arena, state, runtime, dependency);
  loaded.lookup_dependencies.clear();
}

void drain_tls_destructors(uint8_t *arena, State *state, RuntimeManager *runtime) {
  for (;;) {
    RuntimeManager::TlsDestructor destructor;
    {
      std::lock_guard<std::mutex> lock(runtime->tls_mutex);
      auto &thread = tls_thread(arena, state, runtime);
      if (thread.destructors.empty()) return;
      destructor = thread.destructors.back();
      thread.destructors.pop_back();
    }
    invoke(arena, state, runtime, destructor.function, &destructor.argument, 1);
    // glibc releases the TLS pin without running global finalizers at thread exit.
    std::lock_guard<std::mutex> lock(runtime->tls_mutex);
    --runtime->dynamic_libraries[destructor.module].tls_destructor_count;
  }
}

void start_library_dependencies(uint8_t *arena, State *state, RuntimeManager *runtime,
                                const uint32_t *arguments) {
  memcpy(runtime->dynamic_arguments, arguments, sizeof(runtime->dynamic_arguments));
  prepare_libraries(runtime);
  __ecv_i386_initialize_thread_tls(arena, state, runtime);
  if (!_ecv_i386_library_count) return;
  for (auto *p = _ecv_i386_libraries[0].dependencies; *p != UINT32_MAX; ++p) {
    open_library(arena, state, runtime, *p);
    for (uint32_t index : runtime->dynamic_libraries[*p].scope)
      runtime->dynamic_libraries[index].global = true;
  }
}

void finish_libraries(uint8_t *arena, State *state, RuntimeManager *runtime) {
  // Finalize each still-loaded object once, even when dependency cycles retain references.
  while (!runtime->dynamic_initialization_order.empty()) {
    const uint32_t index = runtime->dynamic_initialization_order.back();
    runtime->dynamic_initialization_order.pop_back();
    auto &loaded = runtime->dynamic_libraries[index];
    if (!loaded.live) continue;
    for (auto *p = _ecv_i386_libraries[index].finalizers; *p; ++p)
      invoke(arena, state, runtime, *p, nullptr, 0);
    loaded.live = loaded.global = false;
    loaded.references = 0;
  }
}

const I386DynamicSymbol *lookup_library(uint32_t index, const char *name) {
  const auto &library = _ecv_i386_libraries[index];
  auto *end = library.symbols + library.symbol_count;
  auto *symbol = std::lower_bound(library.symbols, end, name,
      [](const I386DynamicSymbol &symbol, const char *name) { return strcmp(symbol.name, name) < 0; });
  return symbol != end && !strcmp(symbol->name, name) ? symbol : nullptr;
}

struct Allocation { uint32_t size; bool free; };
std::map<uint32_t, Allocation> allocations;
std::recursive_mutex allocation_mutex;
uint32_t allocate(uint32_t size) {
  std::lock_guard<std::recursive_mutex> lock(allocation_mutex);
  if (size > UINT32_MAX - 15) return 0;
  size = (std::max(size, 1u) + 15) & ~15u;
  if (allocations.empty()) {
    uint32_t end = BRK_END_VMA;
    for (uint64_t i = 0; i < _ecv_data_sec_num; ++i) {
      uint64_t start = _ecv_data_sec_vma_array[i], length = _ecv_data_sec_size_array[i];
      if (start <= BRK_START_VMA && length > BRK_START_VMA - start)
        elfconv_runtime_error("ELF overlaps guest import heap.\n");
      if (start >= BRK_START_VMA && start < end) end = start & ~15u;
    }
    allocations.emplace(BRK_START_VMA, Allocation{end - uint32_t(BRK_START_VMA), true});
  }
  for (auto it = allocations.begin(); it != allocations.end(); ++it) {
    if (!it->second.free || it->second.size < size) continue;
    uint32_t address = it->first, remaining = it->second.size - size;
    it->second = {size, false};
    if (remaining) allocations.emplace(address + size, Allocation{remaining, true});
    return address;
  }
  errno = ENOMEM;
  return 0;
}
void release(uint32_t address) {
  std::lock_guard<std::recursive_mutex> lock(allocation_mutex);
  if (!address) return;
  auto it = allocations.find(address);
  if (it == allocations.end() || it->second.free)
    elfconv_runtime_error("Invalid guest free: 0x%x.\n", address);
  it->second.free = true;
  auto next = std::next(it);
  if (next != allocations.end() && next->second.free) {
    it->second.size += next->second.size;
    allocations.erase(next);
  }
  if (it != allocations.begin()) {
    auto prev = std::prev(it);
    if (prev->second.free) {
      prev->second.size += it->second.size;
      allocations.erase(it);
    }
  }
}
uint32_t owned_string(uint8_t *arena, const char *value) {
  if (!value) return 0;
  size_t length = strlen(value) + 1;
  if (length > UINT32_MAX) return 0;
  uint32_t address = allocate(length);
  if (address) memcpy(guest(arena, address, length), value, length);
  return address;
}
std::map<std::string, uint32_t, std::less<>> environment_values;
std::map<uint32_t, FILE *> files;
uint32_t next_file = 0x1000;
FILE *stream(uint32_t handle) {
  if (handle == 1) return stdout;
  if (handle == 2) return stderr;
  if (handle == 3) return stdin;
  auto it = files.find(handle);
  if (it != files.end()) return it->second;
  elfconv_runtime_error("Unsupported guest FILE handle.\n");
}

struct Arguments {
  uint8_t *arena;
  uint32_t address;
  uint32_t word() {
    uint32_t value;
    memcpy(&value, guest(arena, address, 4), 4);
    address += 4;
    return value;
  }
  uint64_t wide() { uint64_t lo = word(); return lo | (uint64_t(word()) << 32); }
};
template<typename T>
void append_format(std::string &out, const std::string &spec, T value) {
  char buffer[256];
  int size = snprintf(buffer, sizeof(buffer), spec.c_str(), value);
  if (size < 0) elfconv_runtime_error("Invalid guest printf format.\n");
  if (size < int(sizeof(buffer))) out.append(buffer, size);
  else {
    std::vector<char> large(size_t(size) + 1);
    snprintf(large.data(), large.size(), spec.c_str(), value);
    out.append(large.data(), size);
  }
}
std::string format(uint8_t *arena, const char *input, Arguments args) {
  std::string out;
  for (const char *p = input; *p;) {
    if (*p != '%') { out += *p++; continue; }
    ++p;
    if (*p == '%') { out += *p++; continue; }
    std::string spec = "%";
    while (*p && strchr("-+ #0", *p)) spec += *p++;
    if (*p == '*') {
      int32_t width = args.word();
      if (width == INT32_MIN) elfconv_runtime_error("Guest printf width overflow.\n");
      if (width < 0) { spec += '-'; width = -width; }
      spec += std::to_string(width);
      ++p;
    } else while (isdigit(static_cast<unsigned char>(*p))) spec += *p++;
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        int32_t precision = args.word();
        if (precision >= 0) spec += "." + std::to_string(precision);
        ++p;
      } else {
        spec += '.';
        while (isdigit(static_cast<unsigned char>(*p))) spec += *p++;
      }
    }
    char length = 0;
    if (*p && strchr("hljztL", *p)) {
      length = *p++;
      if (*p == length && (length == 'h' || length == 'l')) {
        ++p;
        length = length == 'l' ? 'q' : 'H';
      }
    }
    char conversion = *p++;
    if (!conversion) elfconv_runtime_error("Incomplete guest printf format.\n");
    if (strchr("diuoxX", conversion)) {
      bool wide = length == 'q' || length == 'j';
      uint64_t value = wide ? args.wide() : args.word();
      if (length == 'h') value = uint16_t(value);
      if (length == 'H') value = uint8_t(value);
      spec += "ll";
      spec += conversion;
      if (conversion == 'd' || conversion == 'i') {
        int64_t signed_value = wide ? int64_t(value) : int32_t(value);
        if (length == 'h') signed_value = int16_t(value);
        if (length == 'H') signed_value = int8_t(value);
        append_format(out, spec, static_cast<long long>(signed_value));
      } else append_format(out, spec, static_cast<unsigned long long>(value));
    } else if (strchr("aAeEfFgG", conversion) && length != 'L') {
      uint64_t bits = args.wide();
      double value;
      memcpy(&value, &bits, 8);
      spec += conversion;
      append_format(out, spec, value);
    } else if (conversion == 's' && !length) {
      uint32_t address = args.word();
      spec += 's';
      append_format(out, spec, address ? text(arena, address) : "(null)");
    } else if (conversion == 'c' && !length) {
      spec += 'c';
      append_format(out, spec, int(args.word()));
    } else if (conversion == 'p' && !length) {
      spec += "llx";
      append_format(out, spec, static_cast<unsigned long long>(args.word()));
    } else if (conversion == 'n') {
      uint32_t address = args.word();
      uint64_t count = out.size();
      size_t size = length == 'q' || length == 'j' ? 8 : length == 'h' ? 2 : length == 'H' ? 1 : 4;
      memcpy(guest(arena, address, size), &count, size);
    } else elfconv_runtime_error("Unsupported guest printf conversion: %c.\n", conversion);
    if (out.size() > INT_MAX) elfconv_runtime_error("Guest printf output overflow.\n");
  }
  return out;
}
int print(Call &call, FILE *stream, unsigned format_arg) {
  auto out = format(call.arena, text(call.arena, call.word(format_arg)),
                    {call.arena, call.stack + 4 * (format_arg + 2)});
  return fwrite(out.data(), 1, out.size(), stream) == out.size() ? int(out.size()) : -1;
}
void parse_end(Call &call, const char *input, const char *end) {
  if (!call.word(1)) return;
  const uint32_t address = call.word(0) + (end - input);
  memcpy(guest(call.arena, call.word(1), 4), &address, 4);
}
struct ParsedInteger { uint64_t magnitude; bool negative; const char *end; };
ParsedInteger parse_integer(Call &call, bool binary_prefix) {
  const char *input = text(call.arena, call.word(0));
  const char *digits = input;
  while (isspace(static_cast<unsigned char>(*digits))) ++digits;
  const bool negative = *digits == '-';
  const bool sign = negative || *digits == '+';
  if (sign) ++digits;
  int base = int32_t(call.word(2));
  if (base != 0 && (base < 2 || base > 36)) {
    errno = EINVAL; return {0, negative, input};
  }
  if (sign && (isspace(static_cast<unsigned char>(*digits)) || *digits == '+' || *digits == '-'))
    return {0, negative, input};
  if (binary_prefix && (base == 0 || base == 2) && digits[0] == '0' &&
      (digits[1] == 'b' || digits[1] == 'B') && (digits[2] == '0' || digits[2] == '1')) {
    digits += 2; base = 2;
  }
  char *end = const_cast<char *>(digits);
  const uint64_t magnitude = strtoull(digits, &end, base);
  return {magnitude, negative, end == digits ? input : end};
}
void parse_unsigned(Call &call, bool binary_prefix = false) {
  const char *input = text(call.arena, call.word(0));
  const auto parsed = parse_integer(call, binary_prefix);
  uint64_t value = parsed.magnitude;
  if (value > UINT32_MAX) { value = UINT32_MAX; errno = ERANGE; }
  else if (parsed.negative) value = uint32_t(0) - uint32_t(value);
  parse_end(call, input, parsed.end);
  call.result(uint32_t(value));
}
void parse_signed(Call &call, unsigned bits, bool binary_prefix = false) {
  const char *input = text(call.arena, call.word(0));
  const auto parsed = parse_integer(call, binary_prefix);
  const uint64_t maximum = (uint64_t(1) << (bits - 1)) - 1;
  const uint64_t limit = maximum + parsed.negative;
  uint64_t magnitude = parsed.magnitude;
  if (magnitude > limit) { magnitude = limit; errno = ERANGE; }
  const uint64_t value = parsed.negative ? uint64_t(0) - magnitude : magnitude;
  parse_end(call, input, parsed.end);
  if (bits == 64) call.result64(value);
  else call.result(uint32_t(value));
}
}  // namespace

extern "C" void __ecv_i386_initialize_thread_tls(uint8_t *arena, State *state,
                                               RuntimeManager *runtime) {
  std::lock_guard<std::mutex> lock(runtime->tls_mutex);
  prepare_libraries(runtime);
  tls_thread(arena, state, runtime);
}

extern "C" uint32_t __ecv_i386_tls_address(uint8_t *arena, State *state,
    RuntimeManager *runtime, uint32_t module, uint32_t offset) {
  std::lock_guard<std::mutex> lock(runtime->tls_mutex);
  prepare_libraries(runtime);
  if (!module || module > _ecv_i386_library_count ||
      !runtime->dynamic_libraries[module - 1].live ||
      offset >= _ecv_i386_libraries[module - 1].tls_size)
    elfconv_runtime_error("Invalid or unloaded guest TLS module/offset: %u/%u.\n", module, offset);
  return tls_thread(arena, state, runtime).pointer -
      _ecv_i386_libraries[module - 1].tls_distance + offset;
}

extern "C" void __ecv_i386_finalize_thread_tls(uint8_t *arena, State *state,
                                             RuntimeManager *runtime) {
  drain_tls_destructors(arena, state, runtime);
  std::lock_guard<std::mutex> lock(runtime->tls_mutex);
  auto &thread = tls_thread(arena, state, runtime);
  memset(guest(arena, thread.base, thread.size), 0, thread.size);
  thread.state = nullptr;
  state->addr.gs_base.dword = 0;
}


#define IMPORT(name) extern "C" void __ecv_i386_##name(uint8_t *arena, State *state, uint32_t, RuntimeManager *runtime)
#include "I386Pthreads.inc"
#define CALL Call call(arena, state)
namespace {
static_assert(CLOCK_REALTIME == 0 && CLOCK_MONOTONIC == 1 &&
              CLOCK_PROCESS_CPUTIME_ID == 2 && CLOCK_THREAD_CPUTIME_ID == 3,
              "Linux i386 clock ID mismatch");
bool store_time32(uint8_t *arena, uint32_t address, int64_t seconds, int32_t fraction) {
  if (seconds < INT32_MIN || seconds > INT32_MAX) { errno = EOVERFLOW; return false; }
  const int32_t value[2] = {int32_t(seconds), fraction};
  memcpy(guest(arena, address, sizeof(value)), value, sizeof(value));
  return true;
}
}
IMPORT(time) {
  CALL;
  const time_t value = time(nullptr);
  if (value < INT32_MIN || value > INT32_MAX) {
    errno = EOVERFLOW; call.result(-1); return;
  }
  const int32_t result = value;
  if (call.word(0)) memcpy(guest(arena, call.word(0), 4), &result, 4);
  call.result(result);
}
IMPORT(clock_gettime) {
  CALL;
  struct timespec value;
  int result = clock_gettime(int32_t(call.word(0)), &value);
  if (!result && !store_time32(arena, call.word(1), value.tv_sec, value.tv_nsec)) result = -1;
  call.result(result);
}
IMPORT(gettimeofday) {
  CALL;
  struct timeval value;
  struct timezone zone;
  int result = gettimeofday(&value, call.word(1) ? &zone : nullptr);
  if (!result && call.word(0) &&
      !store_time32(arena, call.word(0), value.tv_sec, value.tv_usec)) result = -1;
  if (!result && call.word(1)) {
    const int32_t data[2] = {zone.tz_minuteswest, zone.tz_dsttime};
    memcpy(guest(arena, call.word(1), sizeof(data)), data, sizeof(data));
  }
  call.result(result);
}
IMPORT(nanosleep) {
  CALL;
  int32_t request[2];
  memcpy(request, guest(arena, call.word(0), sizeof(request)), sizeof(request));
  const struct timespec duration = {request[0], request[1]};
  struct timespec remaining = {};
  const int result = nanosleep(&duration, call.word(1) ? &remaining : nullptr);
  if (result && errno == EINTR && call.word(1))
    store_time32(arena, call.word(1), remaining.tv_sec, remaining.tv_nsec);
  call.result(result);
}
IMPORT(sleep) { CALL; call.result(sleep(call.word(0))); }

IMPORT(___tls_get_addr) {
  CALL;
  uint32_t index[2];
  memcpy(index, guest(arena, state->gpr.rax.dword, sizeof(index)), sizeof(index));
  call.result(__ecv_i386_tls_address(arena, state, runtime, index[0], index[1]));
}
IMPORT(__tls_get_addr) {
  CALL;
  uint32_t index[2];
  memcpy(index, guest(arena, call.word(0), sizeof(index)), sizeof(index));
  call.result(__ecv_i386_tls_address(arena, state, runtime, index[0], index[1]));
}
IMPORT(tlsdesc) {
  CALL;
  uint32_t offset;
  memcpy(&offset, guest(arena, state->gpr.rax.dword + 4, 4), 4);
  call.result(offset);
}

static void register_tls_destructor(uint8_t *arena, State *state, RuntimeManager *runtime) {
  Call call(arena, state);
  prepare_libraries(runtime);
  const uint32_t handle = call.word(2);
  uint32_t module = 0;
  for (uint32_t i = 1; i < _ecv_i386_library_count; ++i)
    if (handle >= _ecv_i386_libraries[i].image_begin &&
        handle < _ecv_i386_libraries[i].image_end) { module = i; break; }
  {
    std::lock_guard<std::mutex> lock(runtime->tls_mutex);
    tls_thread(arena, state, runtime).destructors.push_back(
        {call.word(0), call.word(1), module});
    auto &count = runtime->dynamic_libraries[module].tls_destructor_count;
    if (count == UINT32_MAX) elfconv_runtime_error("TLS destructor count overflow.\n");
    ++count;
  }
  call.result(0);
}
IMPORT(__cxa_thread_atexit_impl) { register_tls_destructor(arena, state, runtime); }
IMPORT(__cxa_thread_atexit) { register_tls_destructor(arena, state, runtime); }


IMPORT(dlopen) {
  CALL;
  prepare_libraries(runtime);
  const uint32_t flags = call.word(1);
  if ((flags & ~0x107u) || !(flags & 3) || (flags & 3) == 3) {
    dynamic_error(runtime, "Unsupported dlopen flags", "");
    call.result(0);
    return;
  }
  const char *name = call.word(0) ? text(arena, call.word(0)) : nullptr;
  uint32_t index = 0;
  if (name) {
    const char *basename = strrchr(name, '/');
    basename = basename ? basename + 1 : name;
    for (index = 1; index < _ecv_i386_library_count; ++index) {
      const auto &library = _ecv_i386_libraries[index];
      if (!strcmp(name, library.path) || !strcmp(name, library.name) ||
          !strcmp(basename, library.filename)) break;
    }
  }
  if ((flags & 4) && index < _ecv_i386_library_count &&
      !runtime->dynamic_libraries[index].live) {
    call.result(0);
    return;
  }
  if (index >= _ecv_i386_library_count) {
    dynamic_error(runtime, "Library not bundled", name ? name : "<main>");
    call.result(0);
    return;
  }
  open_library(arena, state, runtime, index);
  if (flags & 0x100)
    for (uint32_t member : runtime->dynamic_libraries[index].scope)
      runtime->dynamic_libraries[member].global = true;
  call.result(library_handle_base + index);
}

IMPORT(dlsym) {
  CALL;
  prepare_libraries(runtime);
  const uint32_t handle = call.word(0);
  const char *name = text(arena, call.word(1));
  const I386DynamicSymbol *symbol = nullptr;
  if (!handle) {
    for (uint32_t i = 0; i < _ecv_i386_library_count; ++i) {
      const auto &loaded = runtime->dynamic_libraries[i];
      if (!loaded.live || !loaded.global || !(symbol = lookup_library(i, name))) continue;
      uint32_t return_pc;
      memcpy(&return_pc, guest(arena, call.stack, 4), 4);
      uint32_t caller = 0;
      for (uint32_t j = 1; j < _ecv_i386_library_count; ++j) {
        const auto &library = _ecv_i386_libraries[j];
        if (return_pc >= library.image_begin && return_pc < library.image_end) { caller = j; break; }
      }
      auto &dependencies = runtime->dynamic_libraries[caller].lookup_dependencies;
      if (caller != i && std::find(dependencies.begin(), dependencies.end(), i) == dependencies.end()) {
        dependencies.push_back(i);
        open_library(arena, state, runtime, i);
      }
      break;
    }
  } else if (handle >= library_handle_base &&
             handle - library_handle_base < _ecv_i386_library_count &&
             runtime->dynamic_libraries[handle - library_handle_base].live) {
    for (uint32_t index : runtime->dynamic_libraries[handle - library_handle_base].scope)
      if ((symbol = lookup_library(index, name))) break;
  } else {
    dynamic_error(runtime, "Invalid or unsupported dlsym handle", name);
    call.result(0);
    return;
  }
  if (!symbol) dynamic_error(runtime, "Undefined dynamic symbol", name);
  call.result(symbol ? (symbol->tls_module
      ? __ecv_i386_tls_address(arena, state, runtime, symbol->tls_module, symbol->address)
      : symbol->address) : 0);
}

IMPORT(dlclose) {
  CALL;
  prepare_libraries(runtime);
  const uint32_t handle = call.word(0);
  if (handle < library_handle_base || handle - library_handle_base >= _ecv_i386_library_count ||
      !runtime->dynamic_libraries[handle - library_handle_base].live ||
      (handle == library_handle_base && runtime->dynamic_libraries[0].references == 1)) {
    dynamic_error(runtime, "Invalid dlclose handle", "");
    call.result(UINT32_MAX);
    return;
  }
  close_library(arena, state, runtime, handle - library_handle_base);
  call.result(0);
}

IMPORT(dlerror) {
  CALL;
  const uint32_t error = runtime->dynamic_error_pending
      ? copy_string(arena, runtime->dynamic_error, 8) : 0;
  runtime->dynamic_error_pending = false;
  call.result(error);
}

extern "C" int __ecv_i386_run_function_entry(uint8_t *arena, State *state,
                                           uint32_t entry, RuntimeManager *runtime) {
  uint32_t argc;
  const uint32_t argv = state->gpr.rsp.dword + 4;
  memcpy(&argc, guest(arena, state->gpr.rsp.dword, 4), 4);
  const uint32_t args[] = {argc, argv, argv + 4 * (argc + 1)};
  start_library_dependencies(arena, state, runtime, args);
  for (auto *p = _ecv_i386_initializers; *p; ++p)
    invoke(arena, state, runtime, *p, args, 3);
  invoke(arena, state, runtime, entry, args, 3);
  const int status = state->gpr.rax.dword;
  drain_tls_destructors(arena, state, runtime);
  for (auto *p = _ecv_i386_finalizers; *p; ++p)
    invoke(arena, state, runtime, *p, nullptr, 0);
  finish_libraries(arena, state, runtime);
  return status;
}
IMPORT(__libc_start_main) {
  CALL;
  uint32_t main = call.word(0), argc = call.word(1), argv = call.word(2);
  uint32_t args[] = {argc, argv, argv + 4 * (argc + 1)};
  start_library_dependencies(arena, state, runtime, args);
  if (call.word(3)) invoke(arena, state, runtime, call.word(3), args, 3);
  else for (auto *p = _ecv_i386_initializers; *p; ++p) invoke(arena, state, runtime, *p, args, 3);
  invoke(arena, state, runtime, main, args, 3);
  int status = state->gpr.rax.dword;
  drain_tls_destructors(arena, state, runtime);
  for (auto *p = _ecv_i386_finalizers; *p; ++p) invoke(arena, state, runtime, *p, nullptr, 0);
  if (call.word(4)) invoke(arena, state, runtime, call.word(4), nullptr, 0);
  finish_libraries(arena, state, runtime);
  exit(status);
}
IMPORT(strcmp) { CALL; call.result(strcmp(text(arena, call.word(0)), text(arena, call.word(1)))); }
IMPORT(getenv) {
  CALL;
  const char *name = text(arena, call.word(0));
  const char *value = getenv(name);
  if (!value) { call.result(0); return; }
  auto found = environment_values.find(name);
  if (found != environment_values.end() &&
      !strcmp(text(arena, found->second), value)) {
    call.result(found->second);
    return;
  }
  const uint32_t result = owned_string(arena, value);
  if (!result) { call.result(0); return; }
  if (found == environment_values.end()) environment_values.emplace(name, result);
  else { release(found->second); found->second = result; }
  call.result(result);
}
IMPORT(setenv) {
  CALL;
  call.result(setenv(text(arena, call.word(0)), text(arena, call.word(1)), int32_t(call.word(2))));
}
namespace {
static_assert(sizeof(wchar_t) == 4, "Linux i386 wide-character ABI mismatch");
wchar_t *wide_buffer(uint8_t *arena, uint32_t address, uint32_t count) {
  return static_cast<wchar_t *>(guest(arena, address, uint64_t(count) * 4));
}
uint32_t wide_length(uint8_t *arena, uint32_t address) {
  const auto *begin = wide_buffer(arena, address, 1);
  const auto *end = begin + (MEMORY_ARENA_SIZE - address) / 4;
  const auto *terminator = std::find(begin, end, wchar_t(0));
  if (terminator == end) elfconv_runtime_error("Unterminated i386 wide string.\n");
  return terminator - begin;
}
const wchar_t *wide_text(uint8_t *arena, uint32_t address) {
  const uint32_t length = wide_length(arena, address);
  return wide_buffer(arena, address, length + 1);
}
}  // namespace
IMPORT(wcslen) { CALL; call.result(wide_length(arena, call.word(0))); }
IMPORT(wcscmp) {
  CALL;
  call.result(wcscmp(wide_text(arena, call.word(0)), wide_text(arena, call.word(1))));
}
IMPORT(wcsncpy) {
  CALL;
  const uint32_t count = call.word(2);
  if (count) {
    auto *destination = wide_buffer(arena, call.word(0), count);
    uint32_t copied = 0;
    while (copied < count) {
      const uint64_t address = uint64_t(call.word(1)) + uint64_t(copied) * 4;
      if (address > UINT32_MAX) elfconv_runtime_error("Guest wide string address overflow.\n");
      const wchar_t value = *wide_buffer(arena, uint32_t(address), 1);
      if (!value) break;
      destination[copied++] = value;
    }
    if (copied < count) wmemset(destination + copied, 0, count - copied);
  }
  call.result(call.word(0));
}
IMPORT(wcsncat) {
  CALL;
  const uint32_t destination = call.word(0), count = call.word(2);
  const uint32_t length = wide_length(arena, destination);
  uint32_t copied = 0;
  while (copied < count) {
    const uint64_t source = uint64_t(call.word(1)) + uint64_t(copied) * 4;
    if (source > UINT32_MAX) elfconv_runtime_error("Guest wide string address overflow.\n");
    const wchar_t value = *wide_buffer(arena, uint32_t(source), 1);
    if (!value) break;
    const uint64_t target = uint64_t(destination) + (uint64_t(length) + copied) * 4;
    if (target > UINT32_MAX) elfconv_runtime_error("Guest wide string address overflow.\n");
    *wide_buffer(arena, uint32_t(target), 1) = value;
    ++copied;
  }
  const uint64_t terminator = uint64_t(destination) + (uint64_t(length) + copied) * 4;
  if (terminator > UINT32_MAX) elfconv_runtime_error("Guest wide string address overflow.\n");
  *wide_buffer(arena, uint32_t(terminator), 1) = 0;
  call.result(destination);
}
IMPORT(wmemcpy) {
  CALL;
  const uint32_t count = call.word(2);
  if (count) wmemcpy(wide_buffer(arena, call.word(0), count), wide_buffer(arena, call.word(1), count), count);
  call.result(call.word(0));
}
IMPORT(wmemmove) {
  CALL;
  const uint32_t count = call.word(2);
  if (count) wmemmove(wide_buffer(arena, call.word(0), count), wide_buffer(arena, call.word(1), count), count);
  call.result(call.word(0));
}
IMPORT(wmemset) {
  CALL;
  const uint32_t count = call.word(2);
  if (count) wmemset(wide_buffer(arena, call.word(0), count), wchar_t(call.word(1)), count);
  call.result(call.word(0));
}
IMPORT(wmemcmp) {
  CALL;
  const uint32_t count = call.word(2);
  call.result(count ? wmemcmp(wide_buffer(arena, call.word(0), count),
                            wide_buffer(arena, call.word(1), count), count) : 0);
}
IMPORT(wmemchr) {
  CALL;
  const uint32_t count = call.word(2), address = call.word(0);
  if (!count) { call.result(0); return; }
  const auto *begin = wide_buffer(arena, address, count);
  const auto *found = wmemchr(begin, wchar_t(call.word(1)), count);
  call.result(found ? address + uint32_t(found - begin) * 4 : 0);
}
IMPORT(towlower) { CALL; call.result(towlower(call.word(0))); }
IMPORT(towupper) { CALL; call.result(towupper(call.word(0))); }
IMPORT(iswspace) { CALL; call.result(iswspace(call.word(0))); }
IMPORT(strtoul) { CALL; parse_unsigned(call); }
IMPORT(__isoc23_strtoul) { CALL; parse_unsigned(call, true); }
IMPORT(strtol) { CALL; parse_signed(call, 32); }
IMPORT(strtoll) { CALL; parse_signed(call, 64); }
IMPORT(__isoc23_strtol) { CALL; parse_signed(call, 32, true); }
IMPORT(__isoc23_strtoll) { CALL; parse_signed(call, 64, true); }
IMPORT(printf) { CALL; call.result(print(call, stdout, 0)); }
IMPORT(fprintf) {
  CALL;
  call.result(print(call, stream(call.word(0)), 1));
}
IMPORT(__printf_chk) { CALL; call.result(print(call, stdout, 1)); }
IMPORT(__fprintf_chk) { CALL; call.result(print(call, stream(call.word(0)), 2)); }
IMPORT(__stack_chk_fail) { elfconv_runtime_error("Guest stack protector triggered.\n"); }
IMPORT(puts) { CALL; call.result(puts(text(arena, call.word(0)))); }
IMPORT(fwrite) {
  CALL;
  uint32_t size = call.word(1), count = call.word(2);
  if (!size || !count) { call.result(0); return; }
  uint64_t length = uint64_t(size) * count;
  if (length > MEMORY_ARENA_SIZE) elfconv_runtime_error("Guest fwrite buffer is too large.\n");
  call.result(fwrite(guest(arena, call.word(0), length), size, count, stream(call.word(3))));
}
IMPORT(malloc) { CALL; call.result(allocate(call.word(0))); }
IMPORT(free) { CALL; release(call.word(0)); }
IMPORT(calloc) {
  CALL;
  uint64_t size = uint64_t(call.word(0)) * call.word(1);
  if (size > UINT32_MAX) { errno = ENOMEM; call.result(0); return; }
  uint32_t address = allocate(size);
  if (address) memset(guest(arena, address, size), 0, size);
  call.result(address);
}
IMPORT(realloc) {
  CALL;
  std::lock_guard<std::recursive_mutex> lock(allocation_mutex);
  uint32_t old = call.word(0), size = call.word(1);
  if (!old) { call.result(allocate(size)); return; }
  if (!size) { release(old); call.result(0); return; }
  auto it = allocations.find(old);
  if (it == allocations.end() || it->second.free) elfconv_runtime_error("Invalid guest realloc.\n");
  if (size <= it->second.size) { call.result(old); return; }
  uint32_t address = allocate(size);
  if (address) {
    memcpy(guest(arena, address, size), guest(arena, old, it->second.size), it->second.size);
    release(old);
  }
  call.result(address);
}
IMPORT(memcpy) {
  CALL;
  uint32_t length = call.word(2);
  if (length) memcpy(guest(arena, call.word(0), length), guest(arena, call.word(1), length), length);
  call.result(call.word(0));
}
IMPORT(memmove) {
  CALL;
  uint32_t length = call.word(2);
  if (length) memmove(guest(arena, call.word(0), length), guest(arena, call.word(1), length), length);
  call.result(call.word(0));
}
IMPORT(memset) {
  CALL;
  if (call.word(2)) memset(guest(arena, call.word(0), call.word(2)), call.word(1), call.word(2));
  call.result(call.word(0));
}
IMPORT(memcmp) {
  CALL;
  const uint32_t size = call.word(2);
  call.result(size ? memcmp(guest(arena, call.word(0), size),
                           guest(arena, call.word(1), size), size) : 0);
}
IMPORT(getpid) { CALL; call.result(runtime->main_ecv_pr->ecv_pid); }
IMPORT(strlen) { CALL; call.result(strlen(text(arena, call.word(0)))); }
IMPORT(strcpy) {
  CALL;
  const char *src = text(arena, call.word(1));
  memcpy(guest(arena, call.word(0), strlen(src) + 1), src, strlen(src) + 1);
  call.result(call.word(0));
}
IMPORT(strncpy) {
  CALL;
  uint32_t size = call.word(2);
  if (size) strncpy(static_cast<char *>(guest(arena, call.word(0), size)),
                    static_cast<const char *>(guest(arena, call.word(1), size)), size);
  call.result(call.word(0));
}
IMPORT(strncmp) {
  CALL;
  uint32_t size = call.word(2);
  // Either input may terminate before n; a terminated string need not own n bytes.
  if (!size) { call.result(0); return; }
  const char *a = bounded_text(arena, call.word(0), size), *b = bounded_text(arena, call.word(1), size);
  call.result(strncmp(a, b, size));
}
IMPORT(strrchr) {
  CALL;
  const char *start = text(arena, call.word(0)), *found = strrchr(start, call.word(1));
  call.result(found ? call.word(0) + (found - start) : 0);
}
IMPORT(strchr) {
  CALL;
  const char *start = text(arena, call.word(0)), *found = strchr(start, call.word(1));
  call.result(found ? call.word(0) + (found - start) : 0);
}
IMPORT(strstr) {
  CALL;
  const char *start = text(arena, call.word(0)), *found = strstr(start, text(arena, call.word(1)));
  call.result(found ? call.word(0) + (found - start) : 0);
}
IMPORT(strcasestr) {
  CALL;
  const char *start = text(arena, call.word(0)), *found = strcasestr(start, text(arena, call.word(1)));
  call.result(found ? call.word(0) + (found - start) : 0);
}
IMPORT(strpbrk) {
  CALL;
  const char *start = text(arena, call.word(0)), *found = strpbrk(start, text(arena, call.word(1)));
  call.result(found ? call.word(0) + (found - start) : 0);
}
IMPORT(strcasecmp) { CALL; call.result(strcasecmp(text(arena, call.word(0)), text(arena, call.word(1)))); }
IMPORT(strncasecmp) {
  CALL;
  const uint32_t size = call.word(2);
  call.result(size ? strncasecmp(bounded_text(arena, call.word(0), size),
                               bounded_text(arena, call.word(1), size), size) : 0);
}
IMPORT(strdup) { CALL; call.result(owned_string(arena, text(arena, call.word(0)))); }
IMPORT(memchr) {
  CALL;
  const uint32_t size = call.word(2);
  if (!size) { call.result(0); return; }
  auto *start = static_cast<uint8_t *>(guest(arena, call.word(0), size));
  auto *found = static_cast<uint8_t *>(memchr(start, call.word(1), size));
  call.result(found ? call.word(0) + (found - start) : 0);
}
namespace {
void concatenate(Call &call, bool bounded, bool fortified) {
  const uint32_t destination = call.word(0);
  const char *prefix = text(call.arena, destination);
  const size_t prefix_size = strlen(prefix);
  const uint32_t limit = bounded ? call.word(2) : 0;
  const char *source = bounded && !limit ? "" : bounded ?
      bounded_text(call.arena, call.word(1), limit) : text(call.arena, call.word(1));
  const size_t source_size = bounded ? strnlen(source, limit) : strlen(source);
  const size_t size = prefix_size + source_size + 1;
  if (fortified && size > call.word(bounded ? 3 : 2))
    elfconv_runtime_error("Guest fortified concatenation overflow.\n");
  auto *output = static_cast<char *>(guest(call.arena, destination, size));
  memcpy(output + prefix_size, source, source_size);
  output[size - 1] = 0;
  call.result(destination);
}
void copy_checked(Call &call, bool fortified, bool end_pointer) {
  const char *source = text(call.arena, call.word(1));
  const size_t size = strlen(source) + 1;
  if (fortified && size > call.word(2))
    elfconv_runtime_error("Guest fortified string copy overflow.\n");
  memcpy(guest(call.arena, call.word(0), size), source, size);
  call.result(call.word(0) + (end_pointer ? size - 1 : 0));
}
}
IMPORT(strcat) { CALL; concatenate(call, false, false); }
IMPORT(strncat) { CALL; concatenate(call, true, false); }
IMPORT(stpcpy) { CALL; copy_checked(call, false, true); }
IMPORT(__strcpy_chk) { CALL; copy_checked(call, true, false); }
IMPORT(__stpcpy_chk) { CALL; copy_checked(call, true, true); }
IMPORT(__strcat_chk) { CALL; concatenate(call, false, true); }
IMPORT(__strncat_chk) { CALL; concatenate(call, true, true); }
IMPORT(__memcpy_chk) {
  CALL;
  const uint32_t size = call.word(2);
  if (size > call.word(3)) elfconv_runtime_error("Guest fortified memory copy overflow.\n");
  if (size) memcpy(guest(arena, call.word(0), size), guest(arena, call.word(1), size), size);
  call.result(call.word(0));
}
IMPORT(__errno_location) {
  CALL;
  __ecv_i386_initialize_thread_tls(arena, state, runtime);
  call.result(state->addr.gs_base.dword + guest_errno_offset);
}
IMPORT(strerror) { CALL; call.result(copy_string(arena, strerror(i386_errno_to_host(call.word(0))), 9)); }
IMPORT(strcspn) { CALL; call.result(strcspn(text(arena, call.word(0)), text(arena, call.word(1)))); }
IMPORT(fopen) {
  CALL;
  FILE *file = fopen(text(arena, call.word(0)), text(arena, call.word(1)));
  if (!file) { call.result(0); return; }
  uint32_t handle = next_file++;
  files.emplace(handle, file);
  call.result(handle);
}
IMPORT(fopen64) { __ecv_i386_fopen(arena, state, 0, runtime); }
IMPORT(freopen) {
  CALL;
  const uint32_t handle = call.word(2);
  FILE *file = freopen(call.word(0) ? text(arena, call.word(0)) : nullptr,
                       text(arena, call.word(1)), stream(handle));
  if (!file) { files.erase(handle); call.result(0); return; }
  files[handle] = file;
  call.result(handle);
}
IMPORT(fclose) {
  CALL;
  uint32_t handle = call.word(0);
  FILE *file = stream(handle);
  call.result(fclose(file));
  files.erase(handle);
}
IMPORT(fread) {
  CALL;
  uint32_t size = call.word(1), count = call.word(2);
  if (!size || !count) { call.result(0); return; }
  uint64_t length = uint64_t(size) * count;
  call.result(fread(guest(arena, call.word(0), length), size, count, stream(call.word(3))));
}
IMPORT(fseek) { CALL; call.result(fseek(stream(call.word(0)), int32_t(call.word(1)), call.word(2))); }
IMPORT(ftell) { CALL; call.result(ftell(stream(call.word(0)))); }
IMPORT(fseeko64) {
  CALL;
  call.result(fseeko(stream(call.word(0)), int64_t(call.wide(1)), int32_t(call.word(3))));
}
IMPORT(ftello64) { CALL; call.result64(ftello(stream(call.word(0)))); }
IMPORT(lseek64) {
  CALL;
  call.result64(lseek(int32_t(call.word(0)), int64_t(call.wide(1)), int32_t(call.word(3))));
}
IMPORT(feof) { CALL; call.result(feof(stream(call.word(0)))); }
IMPORT(rewind) { CALL; rewind(stream(call.word(0))); }
IMPORT(fflush) { CALL; call.result(fflush(call.word(0) ? stream(call.word(0)) : nullptr)); }
IMPORT(fgetc) { CALL; call.result(fgetc(stream(call.word(0)))); }
IMPORT(getc) { CALL; call.result(getc(stream(call.word(0)))); }
IMPORT(ungetc) { CALL; call.result(ungetc(int32_t(call.word(0)), stream(call.word(1)))); }
IMPORT(ferror) { CALL; call.result(ferror(stream(call.word(0)))); }
IMPORT(fileno) { CALL; call.result(fileno(stream(call.word(0)))); }
IMPORT(fgets) {
  CALL;
  const int size = int32_t(call.word(1));
  if (size <= 0) { call.result(0); return; }
  auto *buffer = static_cast<char *>(guest(arena, call.word(0), size));
  call.result(fgets(buffer, size, stream(call.word(2))) ? call.word(0) : 0);
}
IMPORT(fputs) { CALL; call.result(fputs(text(arena, call.word(0)), stream(call.word(1)))); }
IMPORT(fdopen) {
  CALL;
  FILE *file = fdopen(int32_t(call.word(0)), text(arena, call.word(1)));
  if (!file) { call.result(0); return; }
  const uint32_t handle = next_file++;
  files.emplace(handle, file);
  call.result(handle);
}
IMPORT(setvbuf) {
  CALL;
  const uint32_t address = call.word(1), size = call.word(3);
  call.result(setvbuf(stream(call.word(0)), address ? static_cast<char *>(guest(arena, address, size)) : nullptr,
                     call.word(2), size));
}
IMPORT(_exit) { CALL; _Exit(int32_t(call.word(0))); }
IMPORT(fputc) { CALL; call.result(fputc(call.word(0), stream(call.word(1)))); }
IMPORT(putc) { CALL; call.result(putc(int32_t(call.word(0)), stream(call.word(1)))); }
IMPORT(putchar) { CALL; call.result(putchar(call.word(0))); }
IMPORT(exit) {
  CALL;
  const uint32_t status = call.word(0);
  drain_tls_destructors(arena, state, runtime);
  finish_libraries(arena, state, runtime);
  exit(status);
}
IMPORT(usleep) {
  CALL;
#ifdef __EMSCRIPTEN__
  emscripten_sleep((uint64_t(call.word(0)) + 999) / 1000);
  call.result(0);
#else
  call.result(usleep(call.word(0)));
#endif
}
IMPORT(__assert_fail) {
  CALL;
  elfconv_runtime_error("Guest assertion %s failed at %s:%u (%s).\n",
      text(arena, call.word(0)), text(arena, call.word(1)), call.word(2), text(arena, call.word(3)));
}
namespace {
void floating_result(State *state, double value) {
  for (unsigned i = 7; i; --i) state->st.elems[i].val = state->st.elems[i - 1].val;
  state->st.elems[0].val = float80_t(double(value));
  state->x87.fxsave.swd.top = (state->x87.fxsave.swd.top + 7) % 8;
  state->x87.fxsave.ftw.flat |= uint8_t(1u << state->x87.fxsave.swd.top);
}
void buffer_format(Call &call, bool bounded, bool va_list, bool fortified = false) {
  unsigned format_arg = (bounded ? 2 : 1) + (fortified ? 2 : 0);
  if (fortified && bounded && call.word(1) > call.word(3))
    elfconv_runtime_error("Guest fortified formatting overflow.\n");
  uint32_t args = va_list ? call.word(format_arg + 1) : call.stack + 4 * (format_arg + 2);
  auto out = format(call.arena, text(call.arena, call.word(format_arg)), {call.arena, args});
  uint32_t capacity = bounded ? call.word(1) : uint32_t(out.size() + 1);
  if (fortified && !bounded && capacity > call.word(2))
    elfconv_runtime_error("Guest fortified formatting overflow.\n");
  if (capacity) {
    size_t length = std::min(out.size(), size_t(capacity - 1));
    auto *dst = static_cast<char *>(guest(call.arena, call.word(0), length + 1));
    memcpy(dst, out.data(), length);
    dst[length] = 0;
  }
  call.result(out.size());
}
}
IMPORT(floorf) { CALL; floating_result(state, floorf(call.real(0))); }
IMPORT(ceilf) { CALL; floating_result(state, ceilf(call.real(0))); }
IMPORT(sqrtf) { CALL; floating_result(state, sqrtf(call.real(0))); }
IMPORT(sinf) { CALL; floating_result(state, sinf(call.real(0))); }
IMPORT(cosf) { CALL; floating_result(state, cosf(call.real(0))); }
IMPORT(strtod) {
  CALL;
  const char *input = text(arena, call.word(0));
  char *end = const_cast<char *>(input);
  const double value = strtod(input, &end);
  parse_end(call, input, end);
  floating_result(state, value);
}
IMPORT(acosf) { CALL; floating_result(state, acosf(call.real(0))); }
IMPORT(asinf) { CALL; floating_result(state, asinf(call.real(0))); }
IMPORT(atanf) { CALL; floating_result(state, atanf(call.real(0))); }
IMPORT(atan2f) { CALL; floating_result(state, atan2f(call.real(0), call.real(1))); }
IMPORT(cbrtf) { CALL; floating_result(state, cbrtf(call.real(0))); }
IMPORT(fmodf) { CALL; floating_result(state, fmodf(call.real(0), call.real(1))); }
IMPORT(logf) { CALL; floating_result(state, logf(call.real(0))); }
IMPORT(powf) { CALL; floating_result(state, powf(call.real(0), call.real(1))); }
IMPORT(tanf) { CALL; floating_result(state, tanf(call.real(0))); }
IMPORT(ceil) { CALL; floating_result(state, ceil(call.real64(0))); }
IMPORT(cos) { CALL; floating_result(state, cos(call.real64(0))); }
IMPORT(exp) { CALL; floating_result(state, exp(call.real64(0))); }
IMPORT(floor) { CALL; floating_result(state, floor(call.real64(0))); }
IMPORT(pow) { CALL; floating_result(state, pow(call.real64(0), call.real64(2))); }
IMPORT(sin) { CALL; floating_result(state, sin(call.real64(0))); }
IMPORT(tan) { CALL; floating_result(state, tan(call.real64(0))); }
IMPORT(modf) {
  CALL;
  double integral;
  const double fractional = modf(call.real64(0), &integral);
  memcpy(guest(arena, call.word(2), sizeof(integral)), &integral, sizeof(integral));
  floating_result(state, fractional);
}
IMPORT(sincosf) {
  CALL;
  const float value = call.real(0), sine = sinf(value), cosine = cosf(value);
  memcpy(guest(arena, call.word(1), sizeof(sine)), &sine, sizeof(sine));
  memcpy(guest(arena, call.word(2), sizeof(cosine)), &cosine, sizeof(cosine));
}
IMPORT(frexpl) {
  CALL;
  float80_t value;
  memcpy(value.data, guest(arena, call.stack + 4, 12), 10);
  int exponent;
  const double fraction = frexp(double(value), &exponent);
  memcpy(guest(arena, call.word(3), sizeof(exponent)), &exponent, sizeof(exponent));
  floating_result(state, fraction);
}
IMPORT(sprintf) { CALL; buffer_format(call, false, false); }
IMPORT(snprintf) { CALL; buffer_format(call, true, false); }
IMPORT(vsnprintf) { CALL; buffer_format(call, true, true); }
IMPORT(__snprintf_chk) { CALL; buffer_format(call, true, false, true); }
IMPORT(__sprintf_chk) { CALL; buffer_format(call, false, false, true); }
IMPORT(__vsnprintf_chk) { CALL; buffer_format(call, true, true, true); }
IMPORT(mkdir) { CALL; call.result(mkdir(text(arena, call.word(0)), call.word(1))); }
IMPORT(open) {
  CALL;
  const int flags = call.word(1);
  // i386 Linux and Emscripten use the same open flag bits.
  static_assert(O_CREAT == 0100 && O_TRUNC == 01000 && O_DIRECTORY == 0200000, "Open flag ABI mismatch");
  const bool needs_mode = (flags & 0100) || (flags & 020000000) == 020000000;
  call.result(open(text(arena, call.word(0)), flags, needs_mode ? call.word(2) : 0));
}
IMPORT(open64) { __ecv_i386_open(arena, state, 0, runtime); }
IMPORT(close) { CALL; call.result(close(int32_t(call.word(0)))); }
IMPORT(read) {
  CALL;
  const uint32_t size = call.word(2);
  call.result(read(int32_t(call.word(0)), size ? guest(arena, call.word(1), size) : nullptr, size));
}
IMPORT(write) {
  CALL;
  const uint32_t size = call.word(2);
  call.result(write(int32_t(call.word(0)), size ? guest(arena, call.word(1), size) : nullptr, size));
}
IMPORT(access) { CALL; call.result(access(text(arena, call.word(0)), call.word(1))); }
IMPORT(chmod) { CALL; call.result(chmod(text(arena, call.word(0)), call.word(1))); }
IMPORT(chown) { CALL; call.result(chown(text(arena, call.word(0)), call.word(1), call.word(2))); }
IMPORT(lchown) { CALL; call.result(lchown(text(arena, call.word(0)), call.word(1), call.word(2))); }
IMPORT(chdir) { CALL; call.result(chdir(text(arena, call.word(0)))); }
IMPORT(rename) { CALL; call.result(rename(text(arena, call.word(0)), text(arena, call.word(1)))); }
IMPORT(unlink) { CALL; call.result(unlink(text(arena, call.word(0)))); }
IMPORT(rmdir) { CALL; call.result(rmdir(text(arena, call.word(0)))); }
IMPORT(link) { CALL; call.result(link(text(arena, call.word(0)), text(arena, call.word(1)))); }
IMPORT(symlink) { CALL; call.result(symlink(text(arena, call.word(0)), text(arena, call.word(1)))); }
IMPORT(readlink) {
  CALL;
  const uint32_t size = call.word(2);
  call.result(readlink(text(arena, call.word(0)), size ? static_cast<char *>(guest(arena, call.word(1), size)) : nullptr, size));
}
IMPORT(getcwd) {
  CALL;
  const uint32_t address = call.word(0), size = call.word(1);
  if (address) {
    call.result(getcwd(static_cast<char *>(guest(arena, address, size)), size) ? address : 0);
    return;
  }
  char *host = getcwd(nullptr, size);
  if (!host) { call.result(0); return; }
  const uint32_t length = strlen(host) + 1;
  const uint32_t result = allocate(size ? size : length);
  if (result) memcpy(guest(arena, result, length), host, length);
  free(host);
  call.result(result);
}
static void resolve_path(Call &call) {
  const char *path = text(call.arena, call.word(0));
  const uint32_t address = call.word(1);
  if (address) {
    call.result(realpath(path, static_cast<char *>(guest(call.arena, address, PATH_MAX))) ? address : 0);
    return;
  }
  char *host = realpath(path, nullptr);
  const uint32_t result = owned_string(call.arena, host);
  free(host);
  call.result(result);
}
IMPORT(realpath) { CALL; resolve_path(call); }
IMPORT(__realpath_chk) {
  CALL;
  if (call.word(1) && call.word(2) < PATH_MAX)
    elfconv_runtime_error("Guest fortified realpath overflow.\n");
  resolve_path(call);
}
IMPORT(stat) {
  CALL;
  struct stat host;
  int result = stat(text(arena, call.word(0)), &host);
  if (!result) {
    if (host.st_size > INT32_MAX || host.st_size < INT32_MIN ||
        uint64_t(host.st_ino) > UINT32_MAX) { errno = EOVERFLOW; call.result(-1); return; }
    // Linux i386 stat (time32, off32): 88 bytes, four-byte member alignment.
    uint8_t data[88] = {};
    auto put = [&](unsigned offset, uint64_t value, unsigned size = 4) { memcpy(data + offset, &value, size); };
    put(0, host.st_dev, 8); put(12, host.st_ino); put(16, host.st_mode);
    put(20, host.st_nlink); put(24, host.st_uid); put(28, host.st_gid);
    put(32, host.st_rdev, 8); put(44, host.st_size); put(48, host.st_blksize);
    put(52, host.st_blocks); put(56, host.st_atim.tv_sec); put(60, host.st_atim.tv_nsec);
    put(64, host.st_mtim.tv_sec); put(68, host.st_mtim.tv_nsec);
    put(72, host.st_ctim.tv_sec); put(76, host.st_ctim.tv_nsec);
    memcpy(guest(arena, call.word(1), sizeof(data)), data, sizeof(data));
  }
  call.result(result);
}
namespace {
int store_stat64(uint8_t *arena, uint32_t address, const struct stat &host) {
  if (host.st_atim.tv_sec < INT32_MIN || host.st_atim.tv_sec > INT32_MAX ||
      host.st_mtim.tv_sec < INT32_MIN || host.st_mtim.tv_sec > INT32_MAX ||
      host.st_ctim.tv_sec < INT32_MIN || host.st_ctim.tv_sec > INT32_MAX) {
    errno = EOVERFLOW; return -1;
  }
  // Linux i386 stat64 (time32): verified against native sizeof/offsetof.
  uint8_t data[96] = {};
  auto put = [&](unsigned offset, uint64_t value, unsigned size = 4) {
    memcpy(data + offset, &value, size);
  };
  put(0, host.st_dev, 8); put(12, host.st_ino); put(16, host.st_mode);
  put(20, host.st_nlink); put(24, host.st_uid); put(28, host.st_gid);
  put(32, host.st_rdev, 8); put(44, host.st_size, 8); put(52, host.st_blksize);
  put(56, host.st_blocks, 8); put(64, host.st_atim.tv_sec); put(68, host.st_atim.tv_nsec);
  put(72, host.st_mtim.tv_sec); put(76, host.st_mtim.tv_nsec);
  put(80, host.st_ctim.tv_sec); put(84, host.st_ctim.tv_nsec); put(88, host.st_ino, 8);
  memcpy(guest(arena, address, sizeof(data)), data, sizeof(data));
  return 0;
}
}
IMPORT(__xstat64) {
  CALL;
  struct stat host;
  int result = stat(text(arena, call.word(1)), &host);
  if (!result) result = store_stat64(arena, call.word(2), host);
  call.result(result);
}
IMPORT(__lxstat64) {
  CALL;
  struct stat host;
  int result = lstat(text(arena, call.word(1)), &host);
  if (!result) result = store_stat64(arena, call.word(2), host);
  call.result(result);
}
IMPORT(__fxstat64) {
  CALL;
  struct stat host;
  int result = fstat(int32_t(call.word(1)), &host);
  if (!result) result = store_stat64(arena, call.word(2), host);
  call.result(result);
}
namespace {
struct Directory { DIR *host; uint32_t entry; };
std::map<uint32_t, Directory> directories;
uint32_t next_directory = 0x2000;
}
IMPORT(opendir) {
  CALL;
  DIR *dir = opendir(text(arena, call.word(0)));
  if (!dir) { call.result(0); return; }
  uint32_t entry = allocate(276);
  if (!entry) { closedir(dir); call.result(0); return; }
  uint32_t handle = next_directory++;
  directories.emplace(handle, Directory{dir, entry});
  call.result(handle);
}
IMPORT(readdir) {
  CALL;
  auto it = directories.find(call.word(0));
  if (it == directories.end()) elfconv_runtime_error("Invalid guest DIR handle.\n");
  auto *entry = readdir(it->second.host);
  if (!entry) { call.result(0); return; }
  if (uint64_t(entry->d_ino) > UINT32_MAX) { errno = EOVERFLOW; call.result(0); return; }
  uint8_t data[268] = {};
  uint32_t ino = entry->d_ino, offset = entry->d_off;
  uint16_t length = 11 + strlen(entry->d_name) + 1;
  memcpy(data, &ino, 4); memcpy(data + 4, &offset, 4); memcpy(data + 8, &length, 2);
  data[10] = entry->d_type;
  memcpy(data + 11, entry->d_name, strlen(entry->d_name) + 1);
  memcpy(guest(arena, it->second.entry, sizeof(data)), data, sizeof(data));
  call.result(it->second.entry);
}
IMPORT(readdir64) {
  CALL;
  auto it = directories.find(call.word(0));
  if (it == directories.end()) elfconv_runtime_error("Invalid guest DIR handle.\n");
  auto *entry = readdir(it->second.host);
  if (!entry) { call.result(0); return; }
  // Linux i386 dirent64: ino64, off64, reclen16, type8, name at byte 19.
  uint8_t data[276] = {};
  const uint64_t ino = entry->d_ino;
  const int64_t offset = entry->d_off;
  const size_t name_size = strlen(entry->d_name) + 1;
  const uint16_t length = (19 + name_size + 7) & ~7u;
  memcpy(data, &ino, 8); memcpy(data + 8, &offset, 8);
  memcpy(data + 16, &length, 2);
  data[18] = entry->d_type;
  memcpy(data + 19, entry->d_name, name_size);
  memcpy(guest(arena, it->second.entry, sizeof(data)), data, sizeof(data));
  call.result(it->second.entry);
}
IMPORT(closedir) {
  CALL;
  auto it = directories.find(call.word(0));
  if (it == directories.end()) elfconv_runtime_error("Invalid guest DIR handle.\n");
  call.result(closedir(it->second.host));
  release(it->second.entry);
  directories.erase(it);
}
IMPORT(qsort) {
  CALL;
  uint32_t base = call.word(0), count = call.word(1), size = call.word(2), comparator = call.word(3);
  if (count < 2 || !size) return;
  guest(arena, base, uint64_t(count) * size);
  // Heapsort keeps callback pointers inside the original guest array and avoids
  // host qsort's temporary storage escaping through the guest comparator.
  auto less = [&](uint32_t a, uint32_t b) {
    uint32_t args[] = {base + a * size, base + b * size};
    invoke(arena, state, runtime, comparator, args, 2);
    return int32_t(state->gpr.rax.dword) < 0;
  };
  auto swap = [&](uint32_t a, uint32_t b) {
    for (uint32_t i = 0; i < size; ++i) std::swap(arena[base + a * size + i], arena[base + b * size + i]);
  };
  auto sift = [&](uint32_t root, uint32_t end) {
    while (uint64_t(root) * 2 + 1 < end) {
      uint32_t child = root * 2 + 1;
      if (child + 1 < end && less(child, child + 1)) ++child;
      if (!less(root, child)) break;
      swap(root, child);
      root = child;
    }
  };
  for (uint32_t i = count / 2; i; --i) sift(i - 1, count);
  for (uint32_t end = count - 1; end; --end) { swap(0, end); sift(0, end); }
}

IMPORT(__isoc99_sscanf) {
  CALL;
  const char *input = text(arena, call.word(0)), *start = input;
  const char *fmt = text(arena, call.word(1));
  unsigned argument = 2;
  int assigned = 0;
  for (const char *p = fmt; *p;) {
    if (isspace(static_cast<unsigned char>(*p))) {
      while (isspace(static_cast<unsigned char>(*p))) ++p;
      while (isspace(static_cast<unsigned char>(*input))) ++input;
      continue;
    }
    if (*p != '%') { if (*p++ != *input) break; ++input; continue; }
    ++p;
    if (*p == '%') { ++p; if (*input != '%') break; ++input; continue; }
    bool suppress = *p == '*';
    if (suppress) ++p;
    uint32_t width = 0;
    while (isdigit(static_cast<unsigned char>(*p))) {
      if (width > (UINT32_MAX - 9) / 10) elfconv_runtime_error("Guest scanf width overflow.\n");
      width = width * 10 + (*p++ - '0');
    }
    char length = 0;
    if (*p && strchr("hljztL", *p)) {
      length = *p++;
      if (*p == length && (length == 'h' || length == 'l')) { ++p; length = length == 'l' ? 'q' : 'H'; }
    }
    char conversion = *p++;
    if (!conversion) elfconv_runtime_error("Incomplete guest scanf format.\n");
    uint32_t address = suppress ? 0 : call.word(argument++);
    size_t size = length == 'q' || length == 'j' ? 8 : length == 'h' ? 2 : length == 'H' ? 1 : 4;
    if (conversion == 'n') {
      uint64_t count = input - start;
      if (!suppress) memcpy(guest(arena, address, size), &count, size);
      continue;
    }
    if (conversion != 'c' && conversion != '[')
      while (isspace(static_cast<unsigned char>(*input))) ++input;
    if (!*input) { if (!assigned) assigned = EOF; break; }
    if (conversion == 's' || conversion == 'c') {
      size_t count = 0, limit = width ? width : conversion == 'c' ? 1 : UINT32_MAX;
      while (count < limit && input[count] &&
             (conversion == 'c' || !isspace(static_cast<unsigned char>(input[count])))) ++count;
      if (!count || (conversion == 'c' && count < limit)) break;
      if (!suppress) {
        auto *dst = static_cast<char *>(guest(arena, address, count + (conversion == 's')));
        memcpy(dst, input, count);
        if (conversion == 's') dst[count] = 0;
        ++assigned;
      }
      input += count;
      continue;
    }
    std::string bounded;
    const char *number = input;
    if (width && strlen(input) > width) { bounded.assign(input, width); number = bounded.c_str(); }
    char *end = nullptr;
    if (strchr("diuoxXp", conversion)) {
      int base = conversion == 'i' ? 0 : conversion == 'o' ? 8 :
                 conversion == 'x' || conversion == 'X' || conversion == 'p' ? 16 : 10;
      uint64_t value = conversion == 'd' || conversion == 'i' ?
          uint64_t(strtoll(number, &end, base)) : strtoull(number, &end, base);
      if (end == number) break;
      if (!suppress) { memcpy(guest(arena, address, size), &value, size); ++assigned; }
    } else if (strchr("aAeEfFgG", conversion) && length != 'L') {
      double value = strtod(number, &end);
      if (end == number) break;
      if (!suppress) {
        if (length == 'l') memcpy(guest(arena, address, 8), &value, 8);
        else { float single = value; memcpy(guest(arena, address, 4), &single, 4); }
        ++assigned;
      }
    } else elfconv_runtime_error("Unsupported guest scanf conversion: %c.\n", conversion);
    input += end - number;
  }
  call.result(assigned);
}
namespace {
uint32_t ctype_table(uint8_t *arena, unsigned kind) {
  // glibc exposes a pointer-to-table biased by 128 for signed char indexing.
  const bool mapping = kind != 0;
  uint32_t slot = 0x40000 + kind * 0x1000;
  uint32_t table = slot + 16, pointer = table + 128 * (mapping ? 4 : 2);
  memcpy(guest(arena, slot, 4), &pointer, 4);
  for (int c = -128; c < 256; ++c) {
    int value = c < -1 ? c + 256 : c;
    if (mapping) {
      int32_t mapped = value == -1 ? -1 : kind == 1 ? tolower(value) : toupper(value);
      memcpy(guest(arena, table + (c + 128) * 4, 4), &mapped, 4);
    } else {
      uint16_t bits = 0;
      if (value != -1) {
        // glibc stores its classification flags in network byte order.
        if (isupper(value)) bits |= 0x0100;
        if (islower(value)) bits |= 0x0200;
        if (isalpha(value)) bits |= 0x0400;
        if (isdigit(value)) bits |= 0x0800;
        if (isxdigit(value)) bits |= 0x1000;
        if (isspace(value)) bits |= 0x2000;
        if (isprint(value)) bits |= 0x4000;
        if (isgraph(value)) bits |= 0x8000;
        if (isblank(value)) bits |= 0x0001;
        if (iscntrl(value)) bits |= 0x0002;
        if (ispunct(value)) bits |= 0x0004;
        if (isalnum(value)) bits |= 0x0008;
      }
      memcpy(guest(arena, table + (c + 128) * 2, 2), &bits, 2);
    }
  }
  return slot;
}
}
IMPORT(__ctype_b_loc) { CALL; call.result(ctype_table(arena, 0)); }
IMPORT(__ctype_tolower_loc) { CALL; call.result(ctype_table(arena, 1)); }
IMPORT(__ctype_toupper_loc) { CALL; call.result(ctype_table(arena, 2)); }
IMPORT(tolower) { CALL; call.result(tolower(int32_t(call.word(0)))); }
IMPORT(toupper) { CALL; call.result(toupper(int32_t(call.word(0)))); }
IMPORT(isspace) { CALL; call.result(isspace(int32_t(call.word(0))) ? 0x2000 : 0); }
IMPORT(isalnum) { CALL; call.result(isalnum(int32_t(call.word(0))) ? 8 : 0); }

IMPORT(SDL_Init) { CALL; call.result(SDL_Init(call.word(0))); }
IMPORT(SDL_Quit) {
  CALL;
  SDL_Quit();
  std::fill(std::begin(windows), std::end(windows), nullptr);
  std::fill(std::begin(contexts), std::end(contexts), nullptr);
}
IMPORT(SDL_GetError) { CALL; call.result(copy_string(arena, SDL_GetError(), 0)); }
IMPORT(SDL_GL_SetAttribute) { CALL; call.result(SDL_GL_SetAttribute(SDL_GLattr(call.word(0)), call.word(1))); }
IMPORT(SDL_CreateWindow) {
  CALL;
  unsigned slot = 0;
  while (slot < 16 && windows[slot]) ++slot;
  if (slot == 16) elfconv_runtime_error("Guest SDL window handle table exhausted.\n");
  fprintf(stderr, "Window diagnostic: guest size %u x %u\n", call.word(3), call.word(4));
  windows[slot] = SDL_CreateWindow(text(arena, call.word(0)), call.word(1), call.word(2),
                                  call.word(3), call.word(4), call.word(5));
  call.result(windows[slot] ? 0x100 + slot : 0);
}
IMPORT(SDL_DestroyWindow) {
  CALL;
  if (call.word(0)) { SDL_DestroyWindow(window(call.word(0))); windows[call.word(0) - 0x100] = nullptr; }
}
IMPORT(SDL_GL_CreateContext) {
  CALL;
  unsigned slot = 0;
  while (slot < 16 && contexts[slot]) ++slot;
  if (slot == 16) elfconv_runtime_error("Guest GL context handle table exhausted.\n");
#ifdef __EMSCRIPTEN_PTHREADS__
  // SDL's EGL backend proxies context creation to the browser thread. The
  // transferred canvas belongs to this worker; create and bind WebGL here.
  window(call.word(0));
  EmscriptenWebGLContextAttributes attributes;
  emscripten_webgl_init_context_attributes(&attributes);
  attributes.majorVersion = 2;
  attributes.depth = true;
  const auto handle = emscripten_webgl_create_context("#canvas", &attributes);
  if (handle > 0 && emscripten_webgl_make_context_current(handle) == EMSCRIPTEN_RESULT_SUCCESS)
    contexts[slot] = reinterpret_cast<SDL_GLContext>(uintptr_t(handle));
  else if (handle > 0) emscripten_webgl_destroy_context(handle);
#else
  contexts[slot] = SDL_GL_CreateContext(window(call.word(0)));
#endif
  call.result(contexts[slot] ? 0x200 + slot : 0);
}
IMPORT(SDL_GL_DeleteContext) {
  CALL;
  if (call.word(0)) {
#ifdef __EMSCRIPTEN_PTHREADS__
    emscripten_webgl_destroy_context(uintptr_t(context(call.word(0))));
#else
    SDL_GL_DeleteContext(context(call.word(0)));
#endif
    contexts[call.word(0) - 0x200] = nullptr;
  }
}
#ifdef __EMSCRIPTEN__
static int browser_swap_interval = 1;
static double browser_frame_start = -1.0;
static double browser_frame_wait = 0.0;
static bool browser_frame_submitted = false;
EM_ASYNC_JS(void, wait_browser_frame, (), {
  await new Promise(requestAnimationFrame);
});
#endif
IMPORT(SDL_GL_SetSwapInterval) {
  CALL;
#ifdef __EMSCRIPTEN__
  int32_t interval = call.word(0);
  if (interval < 0) { call.result(SDL_SetError("Adaptive swap intervals are unsupported in WebGL")); return; }
  browser_swap_interval = interval;
  call.result(0);
#else
  call.result(SDL_GL_SetSwapInterval(call.word(0)));
#endif
}
IMPORT(SDL_GL_GetDrawableSize) {
  CALL;
  int width, height;
  SDL_GL_GetDrawableSize(window(call.word(0)), &width, &height);
  if (call.word(1)) memcpy(guest(arena, call.word(1), 4), &width, 4);
  if (call.word(2)) memcpy(guest(arena, call.word(2), 4), &height, 4);
}
IMPORT(SDL_GL_SwapWindow) {
  CALL;
#ifndef __EMSCRIPTEN_PTHREADS__
  SDL_GL_SwapWindow(window(call.word(0)));
#else
  window(call.word(0));
#endif
#ifdef __EMSCRIPTEN__
  // Present at the requested browser frame boundary without requiring SDL's
  // Emscripten main-loop API; the lifted guest owns its synchronous loop.
  browser_frame_submitted = true;
  const double wait_start = emscripten_get_now();
  if (browser_swap_interval) {
    for (int i = 0; i < browser_swap_interval; ++i) wait_browser_frame();
  } else emscripten_sleep(0);
  browser_frame_wait += emscripten_get_now() - wait_start;
#endif
}
static SDL_GameController *guest_controllers[32] = {};
static SDL_Haptic *guest_haptics[32] = {};
static SDL_RWops *guest_rwops[32] = {};
static uint32_t guest_keyboard_state;
static int guest_keyboard_count;
static uint32_t opaque_handle(unsigned slot, uint32_t base, unsigned count) {
  if (slot >= count) elfconv_runtime_error("Guest SDL handle table exhausted.\n");
  return base + slot;
}
static SDL_GameController *game_controller(uint32_t handle) {
  if (!handle) return nullptr;
  if (handle < 0x300 || handle >= 0x320 || !guest_controllers[handle - 0x300])
    elfconv_runtime_error("Invalid guest SDL controller handle.\n");
  return guest_controllers[handle - 0x300];
}
static SDL_Haptic *haptic(uint32_t handle) {
  if (!handle) return nullptr;
  if (handle < 0x400 || handle >= 0x420 || !guest_haptics[handle - 0x400])
    elfconv_runtime_error("Invalid guest SDL haptic handle.\n");
  return guest_haptics[handle - 0x400];
}
static SDL_RWops *rwops(uint32_t handle) {
  if (!handle) return nullptr;
  if (handle < 0x500 || handle >= 0x520 || !guest_rwops[handle - 0x500])
    elfconv_runtime_error("Invalid guest SDL RWops handle.\n");
  return guest_rwops[handle - 0x500];
}
IMPORT(SDL_SetWindowPosition) { CALL; SDL_SetWindowPosition(window(call.word(0)), int(call.word(1)), int(call.word(2))); }
IMPORT(SDL_GetWindowSize) {
  CALL; int w, h; SDL_GetWindowSize(window(call.word(0)), &w, &h);
  if (call.word(1)) memcpy(guest(arena, call.word(1), 4), &w, 4);
  if (call.word(2)) memcpy(guest(arena, call.word(2), 4), &h, 4);
}
IMPORT(SDL_SetWindowSize) { CALL; SDL_SetWindowSize(window(call.word(0)), int(call.word(1)), int(call.word(2))); }
IMPORT(SDL_SetWindowFullscreen) { CALL; call.result(SDL_SetWindowFullscreen(window(call.word(0)), call.word(1))); }
IMPORT(SDL_GetWindowFlags) { CALL; call.result(SDL_GetWindowFlags(window(call.word(0)))); }
IMPORT(SDL_ShowCursor) { CALL; call.result(SDL_ShowCursor(call.word(0))); }
IMPORT(SDL_ShowSimpleMessageBox) {
  CALL; call.result(SDL_ShowSimpleMessageBox(call.word(0), text(arena, call.word(1)),
      text(arena, call.word(2)), call.word(3) ? window(call.word(3)) : nullptr));
}
IMPORT(SDL_memset) {
  CALL; uint32_t address = call.word(0), value = call.word(1), length = call.word(2);
  memset(guest(arena, address, length), int(value), length); call.result(address);
}
IMPORT(SDL_GetPerformanceFrequency) {
  CALL; uint64_t value = SDL_GetPerformanceFrequency();
  state->gpr.rax.dword = uint32_t(value); state->gpr.rdx.dword = uint32_t(value >> 32);
}
IMPORT(SDL_GetPerformanceCounter) {
#ifdef __EMSCRIPTEN__
  // SM64 samples its pacing timer after rendering and audio, immediately
  // before its end-of-frame sleep. Finish timing here, not at the earlier swap.
  if (browser_frame_start >= 0.0 && browser_frame_submitted) {
    const double elapsed = emscripten_get_now() - browser_frame_start - browser_frame_wait;
    EM_ASM({
      if (Module['onFrame']) Module['onFrame']($0);
    }, elapsed);
    browser_frame_start = -1.0;
    browser_frame_wait = 0.0;
    browser_frame_submitted = false;
  }
#endif
  CALL; uint64_t value = SDL_GetPerformanceCounter();
  state->gpr.rax.dword = uint32_t(value); state->gpr.rdx.dword = uint32_t(value >> 32);
}
IMPORT(SDL_GetKeyboardState) {
  CALL; int count = 0; const Uint8 *keys = SDL_GetKeyboardState(&count);
  if (!guest_keyboard_state || guest_keyboard_count != count) {
    if (guest_keyboard_state) release(guest_keyboard_state);
    guest_keyboard_state = allocate(count);
    guest_keyboard_count = guest_keyboard_state ? count : 0;
  }
  if (guest_keyboard_state && count) memcpy(guest(arena, guest_keyboard_state, count), keys, count);
  if (call.word(0)) memcpy(guest(arena, call.word(0), 4), &count, 4);
  call.result(guest_keyboard_state);
}
IMPORT(SDL_PollEvent) {
  CALL;
#ifdef __EMSCRIPTEN__
  // SM64 begins frame work by draining input events.
  if (browser_frame_start < 0.0) browser_frame_start = emscripten_get_now();
#endif
  SDL_Event event;
  int found = SDL_PollEvent(call.word(0) ? &event : nullptr);
  if (guest_keyboard_state) {
    int count = 0; const Uint8 *keys = SDL_GetKeyboardState(&count);
    if (count == guest_keyboard_count && count)
      memcpy(guest(arena, guest_keyboard_state, count), keys, count);
  }
  if (found && call.word(0)) {
    if (event.type == SDL_SYSWMEVENT || event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT ||
        event.type == SDL_TEXTEDITING_EXT || event.type >= SDL_USEREVENT)
      elfconv_runtime_error("Unsupported pointer-bearing guest SDL event %u.\n", event.type);
    static_assert(sizeof(SDL_Event) == 56, "SDL2 guest event layout requires 56 bytes");
    memcpy(guest(arena, call.word(0), 56), &event, 56);
  }
  call.result(found);
}
IMPORT(SDL_GetRelativeMouseState) {
  CALL; int x, y; Uint32 buttons = SDL_GetRelativeMouseState(&x, &y);
  if (call.word(0)) memcpy(guest(arena, call.word(0), 4), &x, 4);
  if (call.word(1)) memcpy(guest(arena, call.word(1), 4), &y, 4);
  call.result(buttons);
}
IMPORT(SDL_NumJoysticks) { CALL; call.result(SDL_NumJoysticks()); }
IMPORT(SDL_IsGameController) { CALL; call.result(SDL_IsGameController(int(call.word(0)))); }
IMPORT(SDL_GameControllerOpen) {
  CALL; unsigned slot = 0; while (slot < 32 && guest_controllers[slot]) ++slot;
  if (slot == 32) elfconv_runtime_error("Guest SDL controller handle table exhausted.\n");
  SDL_GameController *p = SDL_GameControllerOpen(int(call.word(0)));
  if (!p) { call.result(0); return; }
  guest_controllers[slot] = p; call.result(opaque_handle(slot, 0x300, 32));
}
IMPORT(SDL_GameControllerClose) {
  CALL; uint32_t h = call.word(0); if (h) { SDL_GameControllerClose(game_controller(h)); guest_controllers[h - 0x300] = nullptr; }
}
IMPORT(SDL_GameControllerGetAttached) { CALL; call.result(SDL_GameControllerGetAttached(game_controller(call.word(0)))); }
IMPORT(SDL_GameControllerUpdate) { CALL; SDL_GameControllerUpdate(); }
IMPORT(SDL_GameControllerGetAxis) {
  CALL; Sint16 axis = SDL_GameControllerGetAxis(game_controller(call.word(0)), SDL_GameControllerAxis(call.word(1)));
  call.result(uint32_t(int32_t(axis)));
}
IMPORT(SDL_GameControllerGetButton) {
  CALL; call.result(SDL_GameControllerGetButton(game_controller(call.word(0)), SDL_GameControllerButton(call.word(1))));
}
IMPORT(SDL_GameControllerHasRumble) { CALL; call.result(SDL_GameControllerHasRumble(game_controller(call.word(0)))); }
IMPORT(SDL_GameControllerRumble) {
  CALL; call.result(SDL_GameControllerRumble(game_controller(call.word(0)), call.word(1), call.word(2), call.word(3)));
}
IMPORT(SDL_HapticOpen) {
  CALL; unsigned slot = 0; while (slot < 32 && guest_haptics[slot]) ++slot;
  if (slot == 32) elfconv_runtime_error("Guest SDL haptic handle table exhausted.\n");
  SDL_Haptic *p = SDL_HapticOpen(int(call.word(0)));
  if (!p) { call.result(0); return; }
  guest_haptics[slot] = p; call.result(opaque_handle(slot, 0x400, 32));
}
IMPORT(SDL_HapticClose) {
  CALL; uint32_t h = call.word(0); if (h) { SDL_HapticClose(haptic(h)); guest_haptics[h - 0x400] = nullptr; }
}
IMPORT(SDL_HapticRumbleSupported) { CALL; call.result(SDL_HapticRumbleSupported(haptic(call.word(0)))); }
IMPORT(SDL_HapticRumbleInit) { CALL; call.result(SDL_HapticRumbleInit(haptic(call.word(0)))); }
IMPORT(SDL_HapticRumblePlay) { CALL; call.result(SDL_HapticRumblePlay(haptic(call.word(0)), call.real(1), call.word(2))); }
IMPORT(SDL_HapticRumbleStop) { CALL; call.result(SDL_HapticRumbleStop(haptic(call.word(0)))); }
IMPORT(SDL_JoystickNameForIndex) { CALL; call.result(copy_string(arena, SDL_JoystickNameForIndex(int(call.word(0))), 8)); }
IMPORT(SDL_RWFromConstMem) {
  CALL; unsigned slot = 0; while (slot < 32 && guest_rwops[slot]) ++slot;
  if (slot == 32) elfconv_runtime_error("Guest SDL RWops handle table exhausted.\n");
  uint32_t length = call.word(1);
  if (length > INT_MAX) elfconv_runtime_error("SDL_RWFromConstMem data is too large.\n");
  SDL_RWops *p = SDL_RWFromConstMem(guest(arena, call.word(0), length), int(length));
  if (!p) { call.result(0); return; }
  guest_rwops[slot] = p; call.result(opaque_handle(slot, 0x500, 32));
}
IMPORT(SDL_GameControllerAddMappingsFromRW) {
  CALL; uint32_t h = call.word(0);
  int result = SDL_GameControllerAddMappingsFromRW(rwops(h), call.word(1));
  if (call.word(1)) guest_rwops[h - 0x500] = nullptr;
  call.result(result);
}
IMPORT(SDL_OpenAudioDevice) {
  CALL;
  uint32_t name_addr = call.word(0), want_addr = call.word(2), have_addr = call.word(3);
  SDL_AudioSpec want = {}, have = {};
  SDL_AudioSpec *wp = nullptr;
  if (want_addr) {
    auto *p = static_cast<uint8_t *>(guest(arena, want_addr, 24));
    uint32_t callback, userdata;
    memcpy(&want.freq, p, 4); memcpy(&want.format, p + 4, 2);
    want.channels = p[6]; want.silence = p[7]; memcpy(&want.samples, p + 8, 2);
    memcpy(&want.size, p + 12, 4);
    memcpy(&callback, p + 16, 4); memcpy(&userdata, p + 20, 4);
    if (callback) elfconv_runtime_error("SDL audio callbacks are unsupported (SM64 requires queued audio).\n");
    if (userdata) elfconv_runtime_error("SDL audio userdata requires callback mode, which SM64 does not use.\n");
    wp = &want;
  }
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(name_addr ? text(arena, name_addr) : nullptr,
      call.word(1), wp, have_addr ? &have : nullptr, call.word(4));
  if (dev && have_addr) {
    auto *p = static_cast<uint8_t *>(guest(arena, have_addr, 24));
    memset(p, 0, 24); memcpy(p, &have.freq, 4); memcpy(p + 4, &have.format, 2);
    p[6] = have.channels; p[7] = have.silence; memcpy(p + 8, &have.samples, 2);
    memcpy(p + 12, &have.size, 4);
    if (have.callback || have.userdata) elfconv_runtime_error("SDL returned unsupported guest audio callback state.\n");
  }
  call.result(dev);
}
IMPORT(SDL_CloseAudioDevice) { CALL; SDL_CloseAudioDevice(call.word(0)); }
IMPORT(SDL_PauseAudioDevice) { CALL; SDL_PauseAudioDevice(call.word(0), call.word(1)); }
IMPORT(SDL_QueueAudio) {
  CALL; uint32_t length = call.word(2);
  call.result(SDL_QueueAudio(call.word(0), guest(arena, call.word(1), length), length));
}
IMPORT(SDL_GetQueuedAudioSize) { CALL; call.result(SDL_GetQueuedAudioSize(call.word(0))); }
IMPORT(SDL_GetBasePath) {
  CALL; char *p = SDL_GetBasePath(); uint32_t address = owned_string(arena, p);
  if (p) SDL_free(p); call.result(address);
}
IMPORT(SDL_GetPrefPath) {
  CALL; char *p = SDL_GetPrefPath(text(arena, call.word(0)), text(arena, call.word(1)));
  uint32_t address = owned_string(arena, p); if (p) SDL_free(p); call.result(address);
}
IMPORT(SDL_free) { CALL; release(call.word(0)); }
IMPORT(SDL_WasInit) { CALL; call.result(SDL_WasInit(call.word(0))); }
IMPORT(SDL_InitSubSystem) { CALL; call.result(SDL_InitSubSystem(call.word(0))); }
IMPORT(SDL_QuitSubSystem) { CALL; SDL_QuitSubSystem(call.word(0)); }
IMPORT(glGetString) {
  CALL;
  unsigned slot;
  switch (call.word(0)) {
    case GL_VENDOR: slot = 1; break;
    case GL_RENDERER: slot = 2; break;
    case GL_VERSION: slot = 3; break;
    case GL_EXTENSIONS: slot = 4; break;
    default: call.result(0); return;
  }
  call.result(copy_string(arena, reinterpret_cast<const char *>(glGetString(call.word(0))), slot));
}
IMPORT(glGetError) {
  CALL;
#ifdef __EMSCRIPTEN__
  if (guest_gl_error != GL_NO_ERROR) {
    call.result(guest_gl_error);
    guest_gl_error = GL_NO_ERROR;
    return;
  }
#endif
  call.result(glGetError());
}
#if ECV_LEGACY_GL
IMPORT(glBegin) { CALL; glBegin(call.word(0)); }
IMPORT(glEnd) { CALL; glEnd(); }
#endif
IMPORT(glClear) { CALL; glClear(call.word(0)); }
IMPORT(glViewport) { CALL; glViewport(call.word(0), call.word(1), call.word(2), call.word(3)); }
IMPORT(glPixelStorei) {
  CALL;
#ifdef __EMSCRIPTEN__
  GLint *setting = nullptr;
  switch (call.word(0)) {
    case GL_PACK_ALIGNMENT: setting = &pack_alignment; break;
    case GL_PACK_ROW_LENGTH: setting = &pack_row_length; break;
    case GL_PACK_SKIP_ROWS: setting = &pack_skip_rows; break;
    case GL_PACK_SKIP_PIXELS: setting = &pack_skip_pixels; break;
  }
  if (setting) {
    int32_t value = call.word(1);
    if (value < 0 || (setting == &pack_alignment &&
                     value != 1 && value != 2 && value != 4 && value != 8)) {
      if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_VALUE;
    } else {
      *setting = value;
    }
    return;
  }
#endif
  glPixelStorei(call.word(0), call.word(1));
}
#if ECV_LEGACY_GL
IMPORT(glMatrixMode) { CALL; glMatrixMode(call.word(0)); }
IMPORT(glLoadIdentity) { CALL; glLoadIdentity(); }
IMPORT(glVertex2f) { CALL; glVertex2f(call.real(0), call.real(1)); }
IMPORT(glColor3f) { CALL; glColor3f(call.real(0), call.real(1), call.real(2)); }
#endif
IMPORT(glClearColor) { CALL; glClearColor(call.real(0), call.real(1), call.real(2), call.real(3)); }
IMPORT(glReadPixels) {
  CALL;
  uint32_t width = call.word(2), height = call.word(3), format = call.word(4), type = call.word(5);
  if (int32_t(width) < 0 || int32_t(height) < 0 ||
      (format != GL_RGB && format != GL_RGBA) || type != GL_UNSIGNED_BYTE)
    elfconv_runtime_error("Unsupported guest glReadPixels format or extent.\n");
  GLint alignment, row_length, skip_rows, skip_pixels;
#ifdef __EMSCRIPTEN__
  alignment = pack_alignment;
  row_length = pack_row_length;
  skip_rows = pack_skip_rows;
  skip_pixels = pack_skip_pixels;
#else
  glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
  glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
  glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
  glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
#endif
  uint64_t channels = format == GL_RGB ? 3 : 4;
  uint64_t row = uint64_t(row_length ? row_length : width) * channels;
  uint64_t stride = (row + alignment - 1) & ~uint64_t(alignment - 1);
  uint64_t length = height && width ? (uint64_t(skip_rows) + height - 1) * stride +
                                    (uint64_t(skip_pixels) + width) * channels : 0;
  if (length > MEMORY_ARENA_SIZE) elfconv_runtime_error("Guest glReadPixels buffer overflow.\n");
  auto *destination = static_cast<uint8_t *>(guest(arena, call.word(6), length));
#ifdef __EMSCRIPTEN__
  // WebGL 1 guarantees RGBA/UNSIGNED_BYTE readback, not desktop GL_RGB or
  // desktop pack row/skip state. Convert into the guest's requested layout.
  uint8_t rgba[4096];
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width;) {
      uint32_t pixels = std::min(width - x, uint32_t(sizeof(rgba) / 4));
      glReadPixels(int32_t(call.word(0)) + x, int32_t(call.word(1)) + y,
                   pixels, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
      auto *row_out = destination + (uint64_t(skip_rows) + y) * stride +
                      (uint64_t(skip_pixels) + x) * channels;
      if (channels == 4) memcpy(row_out, rgba, pixels * 4);
      else for (uint32_t i = 0; i < pixels; ++i) memcpy(row_out + i * 3, rgba + i * 4, 3);
      x += pixels;
    }
  }
#else
  glReadPixels(call.word(0), call.word(1), width, height, format, type, destination);
#endif
}

IMPORT(glCreateShader) { CALL; call.result(glCreateShader(call.word(0))); }
IMPORT(glCompileShader) { CALL; glCompileShader(call.word(0)); }
IMPORT(glDeleteShader) { CALL; glDeleteShader(call.word(0)); }
IMPORT(glCreateProgram) { CALL; call.result(glCreateProgram()); }
IMPORT(glAttachShader) { CALL; glAttachShader(call.word(0), call.word(1)); }
IMPORT(glBindAttribLocation) { CALL; glBindAttribLocation(call.word(0), call.word(1), text(arena, call.word(2))); }
IMPORT(glLinkProgram) { CALL; glLinkProgram(call.word(0)); }
IMPORT(glUseProgram) { CALL; glUseProgram(call.word(0)); }
IMPORT(glDeleteProgram) { CALL; glDeleteProgram(call.word(0)); }
IMPORT(glGetUniformLocation) { CALL; call.result(glGetUniformLocation(call.word(0), text(arena, call.word(1)))); }
IMPORT(glEnable) { CALL; glEnable(call.word(0)); }
IMPORT(glEnableVertexAttribArray) { CALL; glEnableVertexAttribArray(call.word(0)); }
IMPORT(glBindBuffer) { CALL; glBindBuffer(call.word(0), call.word(1)); }

IMPORT(glShaderSource) {
  CALL;
  int32_t count = call.word(1);
  if (count < 0 || count > 64) elfconv_runtime_error("Guest shader source count must be 0..64.\n");
  if (!count) { glShaderSource(call.word(0), 0, nullptr, nullptr); return; }
  const GLchar *sources[64];
  auto *addresses = static_cast<const uint8_t *>(guest(arena, call.word(2), count * 4));
  auto *lengths = call.word(3) ? static_cast<const GLint *>(guest(arena, call.word(3), count * 4)) : nullptr;
  for (int32_t i = 0; i < count; ++i) {
    uint32_t address;
    memcpy(&address, addresses + i * 4, 4);
    sources[i] = lengths && lengths[i] >= 0
        ? static_cast<const GLchar *>(guest(arena, address, lengths[i])) : text(arena, address);
  }
#ifdef __EMSCRIPTEN__
  // GLSL 1.20 and GLSL ES 1.00 share the attribute/varying shader model.
  // Translate the version/precision declarations, leaving unsupported desktop
  // features to the shader compiler rather than silently changing semantics.
  std::string combined;
  for (int32_t i = 0; i < count; ++i)
    combined.append(sources[i], lengths && lengths[i] >= 0 ? size_t(lengths[i]) : strlen(sources[i]));
  size_t version = combined.find("#version 120");
  if (version != std::string::npos) {
    size_t end = combined.find('\n', version);
    if (end == std::string::npos) end = combined.size();
    GLint kind;
    glGetShaderiv(call.word(0), GL_SHADER_TYPE, &kind);
    GLint range[2], precision = 0;
    glGetShaderPrecisionFormat(kind, GL_HIGH_FLOAT, range, &precision);
    std::string prelude = precision ? "#version 100\nprecision highp float;\n" :
                                    "#version 100\nprecision mediump float;\n";
    combined.replace(version, end - version, prelude);
    const GLchar *translated = combined.c_str();
    glShaderSource(call.word(0), 1, &translated, nullptr);
    return;
  }
#endif
  glShaderSource(call.word(0), count, sources, lengths);
}
IMPORT(glGetShaderiv) {
  CALL;
  glGetShaderiv(call.word(0), call.word(1), static_cast<GLint *>(guest(arena, call.word(2), 4)));
}
IMPORT(glGetProgramiv) {
  CALL;
  glGetProgramiv(call.word(0), call.word(1), static_cast<GLint *>(guest(arena, call.word(2), 4)));
}
IMPORT(glGetShaderInfoLog) {
  CALL;
  int32_t size = call.word(1);
  if (size < 0) elfconv_runtime_error("Negative guest shader log buffer size.\n");
  auto *length = call.word(2) ? static_cast<GLsizei *>(guest(arena, call.word(2), 4)) : nullptr;
  auto *log = size ? static_cast<GLchar *>(guest(arena, call.word(3), size)) : nullptr;
  glGetShaderInfoLog(call.word(0), size, length, log);
}
IMPORT(glGetProgramInfoLog) {
  CALL;
  int32_t size = call.word(1);
  if (size < 0) elfconv_runtime_error("Negative guest program log buffer size.\n");
  auto *length = call.word(2) ? static_cast<GLsizei *>(guest(arena, call.word(2), 4)) : nullptr;
  auto *log = size ? static_cast<GLchar *>(guest(arena, call.word(3), size)) : nullptr;
  glGetProgramInfoLog(call.word(0), size, length, log);
}
IMPORT(glGenBuffers) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * 4 > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL buffer count.\n");
  auto *buffers = count ? static_cast<GLuint *>(guest(arena, call.word(1), count * 4)) : nullptr;
  glGenBuffers(count, buffers);
}
IMPORT(glDeleteBuffers) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * 4 > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL buffer count.\n");
  auto *buffers = count ? static_cast<const GLuint *>(guest(arena, call.word(1), count * 4)) : nullptr;
  glDeleteBuffers(count, buffers);
}
IMPORT(glBufferData) {
  CALL;
  int32_t size = call.word(1);
  if (size < 0) elfconv_runtime_error("Negative guest GL buffer size.\n");
  const void *data = call.word(2) ? guest(arena, call.word(2), size) : nullptr;
  glBufferData(call.word(0), size, data, call.word(3));
}
IMPORT(glVertexAttribPointer) {
  CALL;
  // With a VBO bound, this argument is a byte offset, not a guest address.
  glVertexAttribPointer(call.word(0), call.word(1), call.word(2), call.word(3), call.word(4),
                        reinterpret_cast<const void *>(uintptr_t(call.word(5))));
}
IMPORT(glUniformMatrix4fv) {
  CALL;
  int32_t count = call.word(1);
  if (count < 0 || uint64_t(count) * 64 > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest uniform matrix count.\n");
  auto *values = count ? static_cast<const GLfloat *>(guest(arena, call.word(3), count * 64)) : nullptr;
  glUniformMatrix4fv(call.word(0), count, call.word(2), values);
}
IMPORT(glDrawElements) {
  CALL;
  // The element-array buffer owns the indices; retain its byte offset.
  glDrawElements(call.word(0), call.word(1), call.word(2),
                 reinterpret_cast<const void *>(uintptr_t(call.word(3))));
}
IMPORT(glDrawArrays) { CALL; glDrawArrays(call.word(0), call.word(1), call.word(2)); }
IMPORT(glGenTextures) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL texture count.\n");
  auto *textures = count ? static_cast<GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint))) : nullptr;
  glGenTextures(count, textures);
}
IMPORT(glBindTexture) { CALL; glBindTexture(call.word(0), call.word(1)); }
IMPORT(glTexImage2D) {
  CALL;
  int32_t width = call.word(3), height = call.word(4);
  if (width < 0 || height < 0)
    elfconv_runtime_error("Negative guest glTexImage2D extent.\n");
  uint32_t format = call.word(6), type = call.word(7);
  uint32_t bytes_per_pixel;
  if (type == GL_UNSIGNED_BYTE && format == GL_RGBA) bytes_per_pixel = 4;
  else if (type == GL_UNSIGNED_BYTE && format == GL_RGB) bytes_per_pixel = 3;
  else elfconv_runtime_error("Unsupported guest glTexImage2D format or type.\n");
  GLint alignment, row_length = 0, skip_rows = 0, skip_pixels = 0;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
#ifndef __EMSCRIPTEN__
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
#endif
  if (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)
    elfconv_runtime_error("Invalid GL_UNPACK_ALIGNMENT state.\n");
  if (row_length < 0 || skip_rows < 0 || skip_pixels < 0)
    elfconv_runtime_error("Invalid guest GL unpack state.\n");
  uint64_t row_pixels = row_length ? uint64_t(row_length) : uint64_t(width);
  uint64_t row_bytes = row_pixels * bytes_per_pixel;
  uint64_t stride = (row_bytes + alignment - 1) & ~uint64_t(alignment - 1);
  uint64_t rows_before_last = width && height
      ? uint64_t(skip_rows) + uint64_t(height) - 1 : 0;
  uint64_t last_row_bytes = width && height
      ? (uint64_t(skip_pixels) + uint64_t(width)) * bytes_per_pixel : 0;
  if (rows_before_last && stride > MEMORY_ARENA_SIZE / rows_before_last)
    elfconv_runtime_error("Guest glTexImage2D buffer overflow.\n");
  uint64_t length = rows_before_last * stride + last_row_bytes;
  if (length > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Guest glTexImage2D buffer overflow.\n");
  const void *pixels = call.word(8) ? guest(arena, call.word(8), length) : nullptr;
  glTexImage2D(call.word(0), call.word(1), call.word(2), width, height,
               call.word(5), format, type, pixels);
}
IMPORT(glTexParameteri) { CALL; glTexParameteri(call.word(0), call.word(1), call.word(2)); }
IMPORT(glTexParameterf) { CALL; glTexParameterf(call.word(0), call.word(1), call.real(2)); }
IMPORT(glDepthFunc) { CALL; glDepthFunc(call.word(0)); }
IMPORT(glDepthMask) { CALL; glDepthMask(call.word(0)); }
IMPORT(glBlendFunc) { CALL; glBlendFunc(call.word(0), call.word(1)); }
IMPORT(glScissor) { CALL; glScissor(call.word(0), call.word(1), call.word(2), call.word(3)); }
IMPORT(glPolygonOffset) { CALL; glPolygonOffset(call.real(0), call.real(1)); }
IMPORT(glUniform1f) { CALL; glUniform1f(call.word(0), call.real(1)); }
IMPORT(glUniform1i) { CALL; glUniform1i(call.word(0), call.word(1)); }
IMPORT(glUniform2f) { CALL; glUniform2f(call.word(0), call.real(1), call.real(2)); }
IMPORT(glGetAttribLocation) {
  CALL;
  call.result(glGetAttribLocation(call.word(0), text(arena, call.word(1))));
}
IMPORT(glDisableVertexAttribArray) { CALL; glDisableVertexAttribArray(call.word(0)); }
IMPORT(glDisable) { CALL; glDisable(call.word(0)); }
IMPORT(glActiveTexture) { CALL; glActiveTexture(call.word(0)); }
