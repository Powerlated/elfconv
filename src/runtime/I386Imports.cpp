#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "Runtime.h"
#include "utils/elfconv.h"
#include "I386DynamicLibraries.h"
#include "I386Tls.h"
#include "I386Errno.h"
#include "I386NonlocalJump.h"

#include <SDL2/SDL.h>
#ifndef ECV_LEGACY_GL
#define ECV_LEGACY_GL 1
#endif
#ifndef ECV_USE_GL4ES
#define ECV_USE_GL4ES 0
#endif
#if defined(__EMSCRIPTEN__) && ECV_USE_GL4ES
#include <GL/gl.h>
#include <gl4esinit.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengl.h>
#endif
#if defined(__EMSCRIPTEN__) && ECV_USE_GL4ES
#undef glFenceSync
#undef glIsSync
#undef glDeleteSync
#undef glClientWaitSync
#undef glWaitSync
#undef glGetInteger64v
#undef glGetSynciv
extern "C" {
GLsync glFenceSync(GLenum condition, GLbitfield flags);
GLboolean glIsSync(GLsync sync);
void glDeleteSync(GLsync sync);
GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
void glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
void glGetInteger64v(GLenum pname, GLint64 *params);
void glGetSynciv(GLsync sync, GLenum pname, GLsizei buf_size, GLsizei *length,
                 GLint *values);
void gl4es_glDeleteObject(GLhandleARB object);
void gl4es_glGetInfoLog(GLhandleARB object, GLsizei max_length, GLsizei *length,
                        GLcharARB *info_log);
void gl4es_glGetObjectParameteriv(GLhandleARB object, GLenum pname, GLint *params);
}
#endif
#if defined(__EMSCRIPTEN__)
#include <emscripten/html5.h>
#endif
#include <algorithm>
#include <exception>
#include <type_traits>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>
#include <fenv.h>
#include <fcntl.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <unistd.h>
#include <getopt.h>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <memory>
#include <atomic>
#include <mutex>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif


extern "C" const uint32_t _ecv_i386_initializers[];
extern "C" const uint32_t _ecv_i386_finalizers[];
extern "C" const uint32_t _ecv_i386_libc_data[];
extern "C" void __ecv_i386_drain_guest_atexit(uint8_t *, State *, RuntimeManager *);
#if defined(__EMSCRIPTEN__) && ECV_LEGACY_GL && !ECV_USE_GL4ES
extern "C" void glBindProgramARB(GLenum target, GLuint program);
extern "C" void glDeleteObjectARB(GLuint object);
extern "C" void glGetInfoLogARB(GLuint object, GLsizei max_length, GLsizei *length,
                                 GLchar *info_log);
extern "C" void glGetObjectParameterivARB(GLuint object, GLenum pname, GLint *params);
#endif

namespace {
// Low guest memory is reserved for returned host strings. Host pointers never
// escape into guest registers or memory; SDL objects use typed opaque handles.
constexpr uint32_t string_base = 0x20000;
constexpr size_t string_capacity = 4096;
SDL_Window *windows[16] = {};
SDL_GLContext contexts[16] = {};
#if defined(__EMSCRIPTEN__) && ECV_USE_GL4ES
std::atomic<SDL_Window *> gl4es_main_window{nullptr};
void *gl4es_proc_address(const char *name) {
  return SDL_GL_GetProcAddress(name);
}
void gl4es_main_framebuffer_size(int *width, int *height) {
  SDL_Window *window = gl4es_main_window.load(std::memory_order_acquire);
  if (window) {
    SDL_GL_GetDrawableSize(window, width, height);
  } else {
    *width = 0;
    *height = 0;
  }
}
void initialize_gl4es_for_window(SDL_Window *window) {
  gl4es_main_window.store(window, std::memory_order_release);
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    set_getprocaddress(gl4es_proc_address);
    set_getmainfbsize(gl4es_main_framebuffer_size);
    initialize_gl4es();
  });
}
#endif
constexpr uint32_t guest_gl_sync_base = 0x600;
constexpr size_t guest_gl_sync_limit = 64;
GLsync guest_gl_syncs[guest_gl_sync_limit] = {};
int guest_gl_sync_slot(uint32_t handle) {
  if (handle < guest_gl_sync_base ||
      handle >= guest_gl_sync_base + guest_gl_sync_limit)
    return -1;
  return int(handle - guest_gl_sync_base);
}
void dispatch_guest_event_watches(uint8_t *arena, State *state, RuntimeManager *runtime,
                                  uint32_t event_address);
void reset_guest_renderers();
void destroy_guest_renderers_for_window(uint32_t handle);
#ifdef __EMSCRIPTEN__
GLint pack_alignment = 4, pack_row_length = 0, pack_skip_rows = 0, pack_skip_pixels = 0;
GLenum guest_gl_error = GL_NO_ERROR;
struct GuestGLBufferMapping {
  GLenum target = 0;
  GLuint buffer = 0;
  uint32_t address = 0;
  GLsizei size = 0;
  GLenum access = 0;
  bool active = false;
};
thread_local GuestGLBufferMapping guest_gl_buffer_mappings[2] = {};
GLenum guest_gl_buffer_binding_enum(GLenum target) {
  switch (target) {
    case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    default: return 0;
  }
}
thread_local std::vector<uint8_t> guest_gl_bgr_staging;
const void *guest_gl_bgr_to_rgb(const void *pixels, uint64_t length,
                                int32_t width, int32_t height,
                                GLint alignment, GLint row_length,
                                GLint skip_rows, GLint skip_pixels,
                                uint32_t bytes_per_pixel) {
  if (!pixels || !length) return pixels;
  guest_gl_bgr_staging.resize(size_t(length));
  memcpy(guest_gl_bgr_staging.data(), pixels, size_t(length));
  const uint64_t row_pixels = row_length ? uint64_t(row_length) : uint64_t(width);
  const uint64_t row_bytes = row_pixels * bytes_per_pixel;
  const uint64_t stride = (row_bytes + alignment - 1) & ~uint64_t(alignment - 1);
  for (int32_t y = 0; y < height; ++y) {
    const uint64_t row = (uint64_t(skip_rows) + uint64_t(y)) * stride +
                         uint64_t(skip_pixels) * bytes_per_pixel;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t *pixel = guest_gl_bgr_staging.data() + row +
                       uint64_t(x) * bytes_per_pixel;
      std::swap(pixel[0], pixel[2]);
    }
  }
  return guest_gl_bgr_staging.data();
}
thread_local std::vector<uint8_t> guest_gl_luminance_staging;
bool guest_gl_luminance_internal_format(GLenum format,
                                       GLint &host_internal_format) {
  switch (format) {
    case GL_LUMINANCE:
    case GL_LUMINANCE8:
    case GL_LUMINANCE_ALPHA:
    case GL_LUMINANCE8_ALPHA8:
      host_internal_format = GL_RGBA8;
      return true;
    case GL_SLUMINANCE:
    case GL_SLUMINANCE8:
    case GL_SLUMINANCE_ALPHA:
    case GL_SLUMINANCE8_ALPHA8:
      host_internal_format = GL_SRGB8_ALPHA8;
      return true;
    default:
      return false;
  }
}
const void *guest_gl_luminance_to_rgba(const void *pixels, uint64_t alignment,
                                       uint64_t row_length, uint64_t skip_rows,
                                       uint64_t skip_pixels, int32_t width,
                                       int32_t height, uint32_t input_bpp) {
  if (!pixels || !width || !height) return pixels;
  const uint64_t row_pixels = row_length ? row_length : uint64_t(width);
  const uint64_t input_row_bytes = row_pixels * input_bpp;
  const uint64_t input_stride =
      (input_row_bytes + alignment - 1) & ~(alignment - 1);
  const uint64_t output_row_bytes = row_pixels * 4;
  const uint64_t output_stride =
      (output_row_bytes + alignment - 1) & ~(alignment - 1);
  const uint64_t rows_before_last = skip_rows + uint64_t(height) - 1;
  const uint64_t last_row_bytes = (skip_pixels + uint64_t(width)) * 4;
  if (last_row_bytes > MEMORY_ARENA_SIZE ||
      (rows_before_last &&
       output_stride > (MEMORY_ARENA_SIZE - last_row_bytes) / rows_before_last))
    elfconv_runtime_error("Guest luminance texture conversion exceeds staging limit.\n");
  const uint64_t output_length =
      rows_before_last * output_stride + last_row_bytes;
  guest_gl_luminance_staging.resize(size_t(output_length));
  const auto *source = static_cast<const uint8_t *>(pixels);
  for (int32_t y = 0; y < height; ++y) {
    const uint64_t source_row =
        (skip_rows + uint64_t(y)) * input_stride + skip_pixels * input_bpp;
    const uint64_t output_row =
        (skip_rows + uint64_t(y)) * output_stride + skip_pixels * 4;
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t *in = source + source_row + uint64_t(x) * input_bpp;
      uint8_t *out = guest_gl_luminance_staging.data() + output_row +
                     uint64_t(x) * 4;
      out[0] = out[1] = out[2] = in[0];
      out[3] = input_bpp == 2 ? in[1] : 255;
    }
  }
  return guest_gl_luminance_staging.data();
}
struct GuestGLTextureLevel {
  GLint internal_format = 0;
  GLint width = 0, height = 0, depth = 1;
};
using GuestGLTextureLevels =
    std::map<std::pair<GLuint, GLint>, GuestGLTextureLevel>;
thread_local std::map<SDL_GLContext, GuestGLTextureLevels> guest_gl_texture_levels;
thread_local std::map<SDL_GLContext, GLuint> guest_gl_readback_framebuffers;
GLenum guest_gl_texture_binding_enum(GLenum target) {
  switch (target) {
    case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D;
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
      return GL_TEXTURE_BINDING_CUBE_MAP;
    case GL_TEXTURE_3D: return GL_TEXTURE_BINDING_3D;
    case GL_TEXTURE_2D_ARRAY: return GL_TEXTURE_BINDING_2D_ARRAY;
    default: return 0;
  }
}
void guest_gl_record_texture_level(GLenum target, GLint level,
                                   GLint internal_format, GLint width,
                                   GLint height, GLint depth) {
  const GLenum binding = guest_gl_texture_binding_enum(target);
  if (!binding) return;
  GLint texture = 0;
  glGetIntegerv(binding, &texture);
  if (!texture) return;
  guest_gl_texture_levels[SDL_GL_GetCurrentContext()][{GLuint(texture), level}] =
      {internal_format, width, height, depth};
}
constexpr size_t client_attrib_stack_depth = 16;
constexpr size_t client_vertex_attrib_limit = 64;
struct ClientVertexAttribState {
  GLint enabled, size, type, normalized, stride, buffer;
  void *pointer;
};
struct ClientAttribState {
  GLbitfield mask;
  GLenum active_texture;
  GLint vertex_array, array_buffer, vertex_attrib_count;
  GLint pack[4], unpack[6];
  ClientVertexAttribState vertex_attribs[client_vertex_attrib_limit];
};
ClientAttribState client_attrib_stack[client_attrib_stack_depth] = {};
size_t client_attrib_stack_size = 0;
GLenum client_active_texture = GL_TEXTURE0;
#endif

ssize_t zero_device_read(void *, char *buffer, size_t size) {
  memset(buffer, 0, size);
  return ssize_t(size);
}
ssize_t zero_device_write(void *, const char *, size_t size) { return ssize_t(size); }
int zero_device_close(void *) { return 0; }
ssize_t zero_length_memfile_read(void *, char *, size_t) { return 0; }
int zero_length_memfile_seek(void *, off64_t *offset, int whence) {
  if (*offset != 0 || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)) {
    errno = EINVAL;
    return -1;
  }
  *offset = 0;
  return 0;
}
ssize_t zero_length_memfile_write(void *, const char *, size_t) {
  errno = ENOSPC;
  return -1;
}
int zero_device_seek(void *, off_t *, int) { errno = ESPIPE; return -1; }
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
  RuntimeManager *runtime;
  uint32_t stack;
  uint32_t return_argument_bytes = 0;  // i386 struct-return callees pop the hidden pointer.
  int initial_guest_errno = 0, initial_host_errno = 0;
  Call(uint8_t *a, State *s, RuntimeManager *r = nullptr)
      : arena(a), state(s), runtime(r), stack(s->gpr.rsp.dword) {
    if (state->addr.gs_base.dword) {
      memcpy(&initial_guest_errno, guest(arena, state->addr.gs_base.dword + guest_errno_offset, 4), 4);
      errno = i386_errno_to_host(initial_guest_errno);
    }
    initial_host_errno = errno;
#if defined(__EMSCRIPTEN__)
    if (EM_ASM_INT({
          if (!Module.__elfconvTraceEvent) {
            Module.__elfconvTraceEvent = function(pc) {
              if (!Module.__elfconvTraceConfigured) {
                Module.__elfconvTraceConfigured = true;
                var trace = Module.elfconvTrace;
                if (!trace && typeof globalThis.location === 'object') {
                  var q = new URLSearchParams(globalThis.location.search);
                  if (q.get('trace') === '1') {
                    trace = Module.elfconvTrace = {};
                    if (q.has('traceWorker')) trace.worker = Number(q.get('traceWorker'));
                    if (q.has('tracePc')) trace.pc = Number(q.get('tracePc'));
                    if (q.has('traceLimit')) trace.limit = Number(q.get('traceLimit'));
                  }
                }
                if (trace) {
                  for (var key of ['worker', 'pc', 'limit'])
                    if (key in trace && (!Number.isSafeInteger(trace[key]) || trace[key] < 0))
                      delete trace[key];
                  if (!('limit' in trace)) trace.limit = 100;
                }
              }
              var trace = Module.elfconvTrace;
              var worker = typeof workerID === 'number' ? workerID : -1;
              if (!trace || (typeof trace.worker === 'number' && trace.worker !== worker) ||
                  (typeof trace.pc === 'number' && trace.pc !== pc)) return false;
              var count = Module.__elfconvTraceCount || 0;
              if (count >= trace.limit) return false;
              Module.__elfconvTraceCount = count + 1;
              Module.__elfconvTraceLastPc = pc;
              return true;
            };
          }
          if (!Module.__elfconvTraceEvent($0)) return 0;
          Module.__elfconvTraceLastSp = $1;
          return 1;
        }, state->gpr.rip.dword, stack)) {
      uint32_t return_pc = 0, frame_chain[8] = {};
      memcpy(&return_pc, guest(arena, stack, sizeof(return_pc)), sizeof(return_pc));
      uint32_t frame = state->gpr.rbp.dword;
      for (unsigned i = 0; i < 4 && frame >= NULL_GUARD_SIZE &&
                              frame <= MEMORY_ARENA_SIZE - sizeof(frame_chain[i * 2]); ++i) {
        memcpy(frame_chain + i * 2, guest(arena, frame, sizeof(uint32_t) * 2),
               sizeof(uint32_t) * 2);
        const uint32_t previous = frame_chain[i * 2];
        if (previous <= frame || previous > MEMORY_ARENA_SIZE - sizeof(frame_chain[i * 2]))
          break;
        frame = previous;
      }
      EM_ASM({ Module.__elfconvTraceLastReturnPc = $0; }, return_pc);
      EM_ASM({
        var frames = [];
        for (var i = 0; i < 8; i += 2) {
          var ret = HEAPU32[($3 >> 2) + i + 1];
          if (!ret) break;
          frames.push('0x' + ret.toString(16));
        }
        Module.printErr('guest-import worker=' +
          (typeof workerID === 'number' ? workerID : -1) + ' pc=0x' +
          $0.toString(16) + ' ret=0x' + $2.toString(16) + ' sp=0x' +
          $1.toString(16) + ' frames=' + frames.join(' <- '));
      }, state->gpr.rip.dword, stack, return_pc, frame_chain);
    }
#endif
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
  ~Call() noexcept(false) {
    if (state->addr.gs_base.dword) {
      const int value = errno == initial_host_errno ? initial_guest_errno : host_errno_to_i386(errno);
      memcpy(guest(arena, state->addr.gs_base.dword + guest_errno_offset, 4), &value, 4);
    }
    if (std::uncaught_exceptions()) return;
    uint32_t return_pc;
    memcpy(&return_pc, guest(arena, stack, 4), 4);
    state->gpr.rip.dword = return_pc;
    state->gpr.rsp.dword = stack + 4 + return_argument_bytes;
    if (runtime) __ecv_i386_dispatch_pending_signals(arena, state, runtime);
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
  LiftedFunc function = runtime->FindGuestAdapter(target);
  if (!function) {
    auto it = std::lower_bound(runtime->addr_funptr_srt_list.begin(),
        runtime->addr_funptr_srt_list.end(), target,
        [](const auto &item, uint32_t value) { return item.first < value; });
    if (it == runtime->addr_funptr_srt_list.end() || it->first != target) {
      const uint32_t previous = it == runtime->addr_funptr_srt_list.begin() ? 0 : uint32_t((it - 1)->first);
      const uint32_t next = it == runtime->addr_funptr_srt_list.end() ? 0 : uint32_t(it->first);
      elfconv_runtime_error("Unknown guest callback 0x%x (functions=%zu, previous=0x%x, next=0x%x).\n",
                            target, runtime->addr_funptr_srt_list.size(), previous, next);
    }
    function = it->second;
  }
  uint32_t saved_stack = state->gpr.rsp.dword;
  uint32_t saved_pc = state->gpr.rip.dword;
  // At function entry ESP+4 must be 16-byte aligned for the i386 SysV ABI.
  uint32_t stack = ((saved_stack - 4 * count - 16) & ~15u) - 4;
  uint32_t return_pc = 0;
  memcpy(guest(arena, stack, 4), &return_pc, 4);
  if (count) memcpy(guest(arena, stack + 4, count * 4), args, count * 4);
  state->gpr.rsp.dword = stack;
  state->gpr.rip.dword = target;
  try {
    function(arena, state, target, runtime);
  } catch (const I386NonlocalJump &) {
    // A callback may jump to an older invocation. Leave its restored state intact.
    if (state->gpr.rsp.dword > stack + 4) throw;
    while (state->gpr.rip.dword) {
      const uint32_t pc = state->gpr.rip.dword;
      auto owner = std::upper_bound(runtime->addr_funptr_srt_list.begin(),
          runtime->addr_funptr_srt_list.end(), pc,
          [](uint32_t value, const auto &item) { return value < item.first; });
      if (owner == runtime->addr_funptr_srt_list.begin())
        elfconv_runtime_error("Unknown guest longjmp target 0x%x.\n", pc);
      --owner;
      auto blocks = runtime->fun_bb_addr_map.find(owner->first);
      if (blocks == runtime->fun_bb_addr_map.end() || !blocks->second.count(pc))
        elfconv_runtime_error("Guest longjmp target is not a lifted continuation: 0x%x.\n", pc);
      try {
        owner->second(arena, state, pc, runtime);
      } catch (const I386NonlocalJump &) {
        if (state->gpr.rsp.dword > stack + 4) throw;
      }
    }
  }
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
uint32_t allocate_aligned(uint32_t size, uint32_t alignment) {
  std::lock_guard<std::recursive_mutex> lock(allocation_mutex);
  if (!alignment || (alignment & (alignment - 1))) { errno = EINVAL; return 0; }
  alignment = std::max(alignment, 16u);
  if (size > UINT32_MAX - 15) { errno = ENOMEM; return 0; }
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
    if (!it->second.free) continue;
    const uint64_t aligned = (uint64_t(it->first) + alignment - 1) & ~uint64_t(alignment - 1);
    const uint64_t padding = aligned - it->first;
    if (padding + size > it->second.size) continue;
    const uint32_t address = uint32_t(aligned);
    const uint32_t remaining = it->second.size - uint32_t(padding) - size;
    if (padding) {
      it->second.size = uint32_t(padding);
      allocations.emplace_hint(std::next(it), address, Allocation{size, false});
    } else {
      it->second = {size, false};
    }
    if (remaining) allocations.emplace(address + size, Allocation{remaining, true});
    return address;
  }
  errno = ENOMEM;
  return 0;
}
uint32_t allocate(uint32_t size) { return allocate_aligned(size, 16); }
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

uint32_t register_guest_adapter(uint8_t *arena, RuntimeManager *runtime, LiftedFunc adapter) {
  if (!runtime || !adapter) { errno = EINVAL; return 0; }
  std::lock_guard<std::mutex> lock(runtime->adapter_mutex);
  for (const auto &[address, function] : runtime->guest_adapters)
    if (function == adapter) return address;
  const uint32_t address = allocate(1);
  if (!address) return 0;
  *static_cast<uint8_t *>(guest(arena, address, 1)) = 0xc3;
  runtime->guest_adapters.emplace(address, adapter);
  return address;
}
std::map<std::string, uint32_t, std::less<>> environment_values;
std::map<uint32_t, FILE *> files;
uint32_t next_file = 0x1000;
std::map<int, off_t> closed_stream_offsets;
struct VirtualDatagramPair {
  std::deque<std::vector<char>> incoming[2];
  bool closed[2] = {false, false};
};
struct VirtualDatagramEnd {
  std::shared_ptr<VirtualDatagramPair> pair;
  unsigned side;
};
std::map<int, VirtualDatagramEnd> virtual_datagram_sockets;
int next_virtual_socket = 0x40000000;
bool virtual_socket_write(int fd, const void *buffer, size_t size, ssize_t &result) {
  auto it = virtual_datagram_sockets.find(fd);
  if (it == virtual_datagram_sockets.end()) return false;
  const unsigned peer = 1 - it->second.side;
  if (it->second.pair->closed[peer]) { errno = EPIPE; result = -1; return true; }
  std::vector<char> packet;
  if (size) packet.assign(static_cast<const char *>(buffer),
                           static_cast<const char *>(buffer) + size);
  it->second.pair->incoming[peer].push_back(std::move(packet));
  result = ssize_t(size);
  return true;
}
bool virtual_socket_read(int fd, void *buffer, size_t size, ssize_t &result) {
  auto it = virtual_datagram_sockets.find(fd);
  if (it == virtual_datagram_sockets.end()) return false;
  if (!size) { result = 0; return true; }
  auto &queue = it->second.pair->incoming[it->second.side];
  if (queue.empty()) {
    if (it->second.pair->closed[1 - it->second.side]) result = 0;
    else { errno = EAGAIN; result = -1; }
    return true;
  }
  auto packet = std::move(queue.front());
  queue.pop_front();
  const size_t count = std::min(size, packet.size());
  if (count) memcpy(buffer, packet.data(), count);
  result = ssize_t(count);
  return true;
}
bool virtual_socket_close(int fd) {
  auto it = virtual_datagram_sockets.find(fd);
  if (it == virtual_datagram_sockets.end()) return false;
  it->second.pair->closed[it->second.side] = true;
  virtual_datagram_sockets.erase(it);
  return true;
}
FILE *stream(uint32_t handle) {
  if (handle == 1) return stdout;
  if (handle == 2) return stderr;
  if (handle == 3) return stdin;
  auto it = files.find(handle);
  if (it != files.end()) return it->second;
  elfconv_runtime_error("Unsupported guest FILE handle.\n");
}
struct GuestCookie {
  uint8_t *arena;
  State *state;
  RuntimeManager *runtime;
  uint32_t cookie, read, write, seek, close;
  std::vector<char> pending_write;
};
int flush_guest_cookie_write(GuestCookie *cookie) {
  if (cookie->pending_write.empty()) return 0;
  if (!cookie->write) { errno = EBADF; return -1; }
  if (cookie->pending_write.size() > UINT32_MAX) { errno = EOVERFLOW; return -1; }
  const uint32_t size = uint32_t(cookie->pending_write.size());
  const uint32_t address = allocate(size);
  if (!address) return -1;
  memcpy(guest(cookie->arena, address, size), cookie->pending_write.data(), size);
  uint32_t args[] = {cookie->cookie, address, size};
  invoke(cookie->arena, cookie->state, cookie->runtime, cookie->write, args, 3);
  const int32_t result = int32_t(cookie->state->gpr.rax.dword);
  release(address);
  if (result < 0) return -1;
  const size_t written = std::min(size_t(result), cookie->pending_write.size());
  cookie->pending_write.erase(cookie->pending_write.begin(),
                              cookie->pending_write.begin() + written);
  if (written != size) { errno = EIO; return -1; }
  return 0;
}
ssize_t guest_cookie_read(void *opaque, char *buffer, size_t size) {
  auto *cookie = static_cast<GuestCookie *>(opaque);
  if (!cookie->read) { errno = EBADF; return -1; }
  if (size > UINT32_MAX) { errno = EOVERFLOW; return -1; }
  const uint32_t address = size ? allocate(uint32_t(size)) : 0;
  if (size && !address) return -1;
  uint32_t args[] = {cookie->cookie, address, uint32_t(size)};
  invoke(cookie->arena, cookie->state, cookie->runtime, cookie->read, args, 3);
  int32_t result = int32_t(cookie->state->gpr.rax.dword);
  if (result > 0) {
    if (size_t(result) > size) result = int32_t(size);
    memcpy(buffer, guest(cookie->arena, address, uint32_t(result)), size_t(result));
  } else if (result < 0 && cookie->state->addr.gs_base.dword) {
    int32_t guest_errno;
    memcpy(&guest_errno,
           guest(cookie->arena, cookie->state->addr.gs_base.dword + guest_errno_offset, 4), 4);
    errno = i386_errno_to_host(guest_errno);
  }
  if (address) release(address);
  return result;
}
ssize_t guest_cookie_write(void *opaque, const char *buffer, size_t size) {
  auto *cookie = static_cast<GuestCookie *>(opaque);
  if (!cookie->write) { errno = EBADF; return -1; }
  if (size) cookie->pending_write.insert(cookie->pending_write.end(), buffer, buffer + size);
  return ssize_t(size);
}
int guest_cookie_seek(void *opaque, off64_t *offset, int whence) {
  auto *cookie = static_cast<GuestCookie *>(opaque);
  if (!cookie->seek) { errno = ESPIPE; return -1; }
  const uint32_t address = allocate(sizeof(*offset));
  if (!address) return -1;
  memcpy(guest(cookie->arena, address, sizeof(*offset)), offset, sizeof(*offset));
  uint32_t args[] = {cookie->cookie, address, uint32_t(whence)};
  invoke(cookie->arena, cookie->state, cookie->runtime, cookie->seek, args, 3);
  const int32_t result = int32_t(cookie->state->gpr.rax.dword);
  memcpy(offset, guest(cookie->arena, address, sizeof(*offset)), sizeof(*offset));
  release(address);
  const int flush_result = flush_guest_cookie_write(cookie);
  return result != 0 ? result : flush_result;
}
int guest_cookie_close(void *opaque) {
  auto *cookie = static_cast<GuestCookie *>(opaque);
  const int32_t function = int32_t(cookie->close);
  int result = flush_guest_cookie_write(cookie);
  if (function) {
    uint32_t args[] = {cookie->cookie};
    invoke(cookie->arena, cookie->state, cookie->runtime, uint32_t(function), args, 1);
    const int close_result = int32_t(cookie->state->gpr.rax.dword);
    if (result == 0) result = close_result;
  }
  delete cookie;
  return result;
}
struct GuestMemStream {
  char *host_buffer = nullptr;
  size_t size = 0;
  uint32_t buffer_output = 0, size_output = 0, guest_buffer = 0;
};
std::map<uint32_t, GuestMemStream> memstreams;
int sync_memstream(uint8_t *arena, uint32_t handle) {
  auto it = memstreams.find(handle);
  if (it == memstreams.end()) return 0;
  GuestMemStream &mem = it->second;
  if (mem.size >= UINT32_MAX) { errno = EOVERFLOW; return -1; }
  if (mem.size && !mem.host_buffer) { errno = EIO; return -1; }
  const uint32_t bytes = uint32_t(mem.size + 1);
  const uint32_t next = allocate(bytes);
  if (!next) return -1;
  auto *output = static_cast<char *>(guest(arena, next, bytes));
  if (mem.size) memcpy(output, mem.host_buffer, mem.size);
  output[mem.size] = 0;
  const uint32_t size = uint32_t(mem.size);
  memcpy(guest(arena, mem.buffer_output, 4), &next, 4);
  memcpy(guest(arena, mem.size_output, 4), &size, 4);
  if (mem.guest_buffer) release(mem.guest_buffer);
  mem.guest_buffer = next;
  return 0;
}
int sync_memstreams(uint8_t *arena) {
  for (const auto &entry : memstreams)
    if (sync_memstream(arena, entry.first) < 0) return -1;
  return 0;
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
const wchar_t *wide_text(uint8_t *arena, uint32_t address);
int scan_guest_string(uint8_t *arena, const char *input, const char *fmt, Arguments args,
                      bool deprecated_scanf = false);
bool write_event(uint8_t *arena, uint32_t address, SDL_Event &event);

template<typename Char>
struct FormatOutput {
  FILE *stream = nullptr;
  std::basic_string<Char> buffer;
  uint64_t count = 0;
  int error = 0;

  void fail(int value) { if (!error) error = value ? value : EIO; }
  void clear() {
    if (stream) fail(EOVERFLOW);
    else { buffer.clear(); count = 0; }
  }
  void append(const Char *data, size_t size) {
    if (error) return;
    if (size > uint64_t(INT_MAX) - count) { fail(EOVERFLOW); return; }
    if (stream) {
      if constexpr (std::is_same_v<Char, char>) {
        const size_t written = fwrite(data, 1, size, stream);
        count += written;
        if (written != size) fail(errno);
      } else {
        fail(EINVAL);
      }
      return;
    }
    buffer.append(data, size);
    count += size;
  }
  void append(Char value) { append(&value, 1); }
};
template<typename Char, typename T>
void append_format(FormatOutput<Char> &out, const std::basic_string<Char> &spec, T value) {
  if (out.error) return;
  Char buffer[256];
  int size;
  if constexpr (std::is_same_v<Char, char>)
    size = snprintf(buffer, 256, spec.c_str(), value);
  else
    size = swprintf(buffer, 256, spec.c_str(), value);
  if (size >= 0 && size < 256) { out.append(buffer, size); return; }
  if constexpr (std::is_same_v<Char, char>) {
    if (size < 0) {
      if (errno == EOVERFLOW) out.fail(EOVERFLOW);
      else elfconv_runtime_error("Invalid guest printf format.\n");
      return;
    }
    if (uint64_t(size) > uint64_t(INT_MAX) - out.count) { out.fail(EOVERFLOW); return; }
    if (out.stream) {
      const int written = fprintf(out.stream, spec.c_str(), value);
      if (written < 0) { out.fail(errno); return; }
      out.count += uint32_t(written);
      return;
    }
    std::vector<Char> large(size_t(size) + 1);
    snprintf(large.data(), large.size(), spec.c_str(), value);
    out.append(large.data(), size);
  } else {
    for (size_t capacity = 512; capacity <= INT_MAX; capacity *= 2) {
      std::vector<Char> large(capacity);
      size = swprintf(large.data(), large.size(), spec.c_str(), value);
      if (size >= 0) { out.append(large.data(), size); return; }
      if (errno == EILSEQ) break;
    }
    elfconv_runtime_error("Invalid guest wide printf format.\n");
  }
}
template<typename T>
void append_wide_numeric_format(FormatOutput<wchar_t> &out, const std::wstring &spec, T value) {
  const std::string narrow_spec(spec.begin(), spec.end());
  char small[256];
  int size = snprintf(small, sizeof(small), narrow_spec.c_str(), value);
  if (size < 0) elfconv_runtime_error("Invalid guest wide numeric format.\n");
  std::vector<char> narrow;
  const char *formatted = small;
  if (size >= int(sizeof(small))) {
    narrow.resize(size_t(size) + 1);
    snprintf(narrow.data(), narrow.size(), narrow_spec.c_str(), value);
    formatted = narrow.data();
  }
  std::mbstate_t state{};
  const char *source = formatted;
  const size_t wide_size = mbsrtowcs(nullptr, &source, 0, &state);
  if (wide_size == size_t(-1))
    elfconv_runtime_error("Invalid multibyte guest wide numeric output.\n");
  std::vector<wchar_t> wide(wide_size + 1);
  state = {};
  source = formatted;
  if (mbsrtowcs(wide.data(), &source, wide.size(), &state) == size_t(-1))
    elfconv_runtime_error("Invalid multibyte guest wide numeric output.\n");
  out.append(wide.data(), wide_size);
}
template<typename Char>
void append_binary_format(FormatOutput<Char> &out, uint64_t value, char conversion,
                          bool alternate, bool left, bool zero, int width, int precision) {
  Char digits[64];
  size_t count = 0;
  if (value || precision != 0) {
    do {
      digits[count++] = Char('0' + (value & 1));
      value >>= 1;
    } while (value);
  }
  while (precision > 0 && count < size_t(precision)) digits[count++] = '0';
  const bool prefix = alternate && count && digits[0] != '0' ? true
      : alternate && count && (count > 1 || digits[0] != '0');
  const int prefix_size = prefix ? 2 : 0;
  int padding = width > int(count) + prefix_size ? width - int(count) - prefix_size : 0;
  const bool zero_padding = zero && !left && precision < 0;
  if (!left && !zero_padding)
    while (padding-- > 0) out.append(Char(' '));
  if (prefix) {
    out.append(Char('0'));
    out.append(Char(conversion == 'B' ? 'B' : 'b'));
  }
  if (zero_padding)
    while (padding-- > 0) out.append(Char('0'));
  while (count) out.append(digits[--count]);
  if (left)
    while (padding-- > 0) out.append(Char(' '));
}
std::map<char, bool> registered_printf_sizes;
template<typename Char>
void append_size_format(FormatOutput<Char> &out, double value, char conversion,
                        bool alternate, bool left, bool zero, bool plus, bool space,
                        int width, int precision) {
  const bool decimal = conversion == 'B';
  const double divisor = decimal ? 1000.0 : 1024.0;
  const char *units = decimal ? " KMGTPEZY" : " kmgtpezy";
  char unit = *units;
  while (value >= divisor && units[1]) {
    value /= divisor;
    ++units;
    unit = *units;
  }
  if (precision < 0) precision = 3;
  std::string spec("%");
  if (plus) spec += '+';
  else if (space) spec += ' ';
  if (alternate) spec += '#';
  spec += '.';
  spec += std::to_string(precision);
  spec += 'f';
  char small[256];
  int size = snprintf(small, sizeof(small), spec.c_str(), value);
  if (size < 0) { out.fail(errno); return; }
  std::vector<char> large;
  const char *number = small;
  if (size >= int(sizeof(small))) {
    large.resize(size_t(size) + 1);
    snprintf(large.data(), large.size(), spec.c_str(), value);
    number = large.data();
  }
  const int total = size + 1;
  int padding = width > total ? width - total : 0;
  const bool zero_padding = zero && !left;
  if (!left && !zero_padding)
    while (padding-- > 0) out.append(Char(' '));
  int offset = 0;
  if (zero_padding && size && (number[0] == '+' || number[0] == '-' || number[0] == ' ')) {
    out.append(Char(number[0]));
    offset = 1;
  }
  if (zero_padding)
    while (padding-- > 0) out.append(Char('0'));
  for (int i = offset; i < size; ++i) out.append(Char(number[i]));
  out.append(Char(unit));
  if (left)
    while (padding-- > 0) out.append(Char(' '));
}
template<typename Char>
void format_impl(uint8_t *arena, const Char *input, Arguments args, FormatOutput<Char> &out) {
  const int error_number = errno;
  auto ascii = [](Char value, const char *set) {
    return value > 0 && value < 128 && strchr(set, int(value));
  };
  auto take_position = [&](const Char *&cursor, uint64_t &position) {
    const Char *digits = cursor;
    uint64_t value = 0;
    bool overflow = false;
    while (*cursor >= '0' && *cursor <= '9') {
      const uint32_t digit = uint32_t(*cursor++ - '0');
      if (value > (UINT64_MAX - digit) / 10) overflow = true;
      else value = value * 10 + digit;
    }
    if (cursor == digits || *cursor != '$') { cursor = digits; return false; }
    ++cursor;
    position = overflow ? UINT64_MAX : value;
    return true;
  };
  const uint32_t argument_base = args.address;
  bool invalid_position = false;
  auto argument_at = [&](uint64_t position) {
    if (!position || position - 1 > (UINT32_MAX - argument_base) / 4) {
      invalid_position = true;
      errno = EOVERFLOW;
      return Arguments{arena, 0};
    }
    const uint64_t address = uint64_t(argument_base) + (position - 1) * 4;
    if (address > MEMORY_ARENA_SIZE - 4) {
      invalid_position = true;
      errno = EOVERFLOW;
      return Arguments{arena, 0};
    }
    return Arguments{arena, uint32_t(address)};
  };
  for (const Char *p = input; *p;) {
    if (*p != '%') { out.append(p++, 1); continue; }
    ++p;
    if (*p == '%') { out.append(p++, 1); continue; }
    std::basic_string<Char> spec(1, '%');
    uint64_t value_position;
    Arguments positional_value{arena, argument_base};
    Arguments *value_arguments = &args;
    if (take_position(p, value_position)) {
      positional_value = argument_at(value_position);
      if (invalid_position) { out.clear(); out.fail(EOVERFLOW); return; }
      value_arguments = &positional_value;
    }
    bool flag_alternate = false, flag_left = false, flag_zero = false;
    bool flag_plus = false, flag_space = false;
    while (ascii(*p, "-+ #0I'")) {
      if (*p != 'I') spec += *p;
      flag_alternate |= *p == '#';
      flag_left |= *p == '-';
      flag_zero |= *p == '0';
      flag_plus |= *p == '+';
      flag_space |= *p == ' ';
      ++p;
    }
    int field_width = -1;
    if (*p == '*') {
      ++p;
      uint64_t width_position;
      int32_t width;
      if (take_position(p, width_position)) {
        Arguments positional_width = argument_at(width_position);
        if (invalid_position) { out.clear(); out.fail(EOVERFLOW); return; }
        width = int32_t(positional_width.word());
      } else width = int32_t(args.word());
      if (width == INT32_MIN) elfconv_runtime_error("Guest printf width overflow.\n");
      if (width < 0) { spec += '-'; flag_left = true; width = -width; }
      field_width = width;
      const auto digits = std::to_string(width);
      spec.append(digits.begin(), digits.end());
    } else {
      while (*p >= '0' && *p <= '9') {
        const int digit = int(*p - '0');
        field_width = field_width < 0 ? digit : std::min(INT_MAX, field_width * 10 + digit);
        spec += *p++;
      }
    }
    int field_precision = -1;
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        ++p;
        uint64_t precision_position;
        int32_t precision;
        if (take_position(p, precision_position)) {
          Arguments positional_precision = argument_at(precision_position);
          if (invalid_position) { out.clear(); out.fail(EOVERFLOW); return; }
          precision = int32_t(positional_precision.word());
        } else precision = int32_t(args.word());
        if (precision >= 0) {
          field_precision = precision;
          spec += '.';
          const auto digits = std::to_string(precision);
          spec.append(digits.begin(), digits.end());
        }
      } else {
        field_precision = 0;
        spec += '.';
        while (*p >= '0' && *p <= '9') {
          field_precision = std::min(INT_MAX, field_precision * 10 + int(*p - '0'));
          spec += *p++;
        }
      }
    }
    char length = 0;
    unsigned bit_width = 0;
    if (*p == 'w') {
      ++p;
      length = 'w';
      if (*p == 'f') { ++p; length = 'W'; }
      while (*p >= '0' && *p <= '9') {
        bit_width = std::min(1024u, bit_width * 10 + unsigned(*p++ - '0'));
      }
      if (!bit_width || (bit_width != 8 && bit_width != 16 && bit_width != 32 && bit_width != 64)) {
        if (out.stream) out.fail(EINVAL);
        else { out.clear(); out.fail(EINVAL); }
        errno = EINVAL;
        return;
      }
      if (length == 'W' && bit_width == 16) bit_width = 32;
    } else if (ascii(*p, "hljztLZ")) {
      length = char(*p++);
      if (*p == length && (length == 'h' || length == 'l')) {
        ++p;
        length = length == 'l' ? 'q' : 'H';
      }
    }
    const char conversion = char(*p++);
    if (!conversion) elfconv_runtime_error("Incomplete guest printf format.\n");
    if ((conversion == 'b' || conversion == 'B') && !length &&
        registered_printf_sizes.find(conversion) != registered_printf_sizes.end()) {
      const uint64_t bits = value_arguments->wide();
      double value;
      memcpy(&value, &bits, sizeof(value));
      append_size_format(out, value, conversion, flag_alternate, flag_left, flag_zero,
                         flag_plus, flag_space, field_width, field_precision);
    } else if (strchr("diuoxXbB", conversion)) {
      const bool wide = length == 'q' || length == 'j' || bit_width > 32;
      uint64_t value = wide ? value_arguments->wide() : value_arguments->word();
      if (bit_width) {
        if (bit_width > 64) elfconv_runtime_error("Unsupported guest printf bit width.\n");
        const uint64_t mask = bit_width == 64 ? UINT64_MAX : (UINT64_C(1) << bit_width) - 1;
        value &= mask;
      } else {
        if (length == 'h') value = uint16_t(value);
        if (length == 'H') value = uint8_t(value);
      }
      if (conversion == 'b' || conversion == 'B') {
        append_binary_format(out, value, conversion, flag_alternate, flag_left, flag_zero,
                             field_width, field_precision);
      } else {
        spec += 'l'; spec += 'l'; spec += conversion;
        if (conversion == 'd' || conversion == 'i') {
          int64_t signed_value = wide ? int64_t(value) : int32_t(value);
          if (bit_width && bit_width < 64) {
            const uint64_t sign = UINT64_C(1) << (bit_width - 1);
            signed_value = int64_t((value ^ sign) - sign);
          }
          if (!bit_width && length == 'h') signed_value = int16_t(value);
          if (!bit_width && length == 'H') signed_value = int8_t(value);
          append_format(out, spec, static_cast<long long>(signed_value));
        } else append_format(out, spec, static_cast<unsigned long long>(value));
      }
    } else if (strchr("aAeEfFgG", conversion)) {
      double value;
      if (length == 'L') {
        float80_t extended;
        memcpy(extended.data, guest(arena, value_arguments->address, 12), 10);
        value_arguments->address += 12;
        value = double(extended);
      } else {
        const uint64_t bits = value_arguments->wide();
        memcpy(&value, &bits, 8);
      }
      spec += conversion;
      if constexpr (std::is_same_v<Char, char>)
        append_format(out, spec, value);
      else
        append_wide_numeric_format(out, spec, value);
    } else if ((conversion == 's' && (length == 0 || length == 'l')) ||
               (std::is_same_v<Char, char> && conversion == 'S' && !length)) {
      const uint32_t address = value_arguments->word();
      if (length == 'l' || conversion == 'S') {
        spec += 'l'; spec += 's';
        append_format(out, spec, address ? wide_text(arena, address) : L"(null)");
      } else {
        spec += 's';
        append_format(out, spec, address ? text(arena, address) : "(null)");
      }
    } else if (conversion == 'c' && (length == 0 || length == 'l')) {
      if (length == 'l') spec += 'l';
      spec += 'c';
      append_format(out, spec, value_arguments->word());
    } else if (conversion == 'p' && !length) {
      const uint32_t pointer = value_arguments->word();
      if constexpr (std::is_same_v<Char, wchar_t>) {
        if (!pointer) {
          spec += 'l'; spec += 's';
          append_format(out, spec, L"(nil)");
        } else {
          spec += 'p';
          append_format(out, spec, reinterpret_cast<void *>(uintptr_t(pointer)));
        }
      } else if (!pointer) {
        spec += 's';
        append_format(out, spec, "(nil)");
      } else {
        spec += 'p';
        append_format(out, spec, reinterpret_cast<void *>(uintptr_t(pointer)));
      }
    } else if (conversion == 'n') {
      const uint32_t address = value_arguments->word();
      const uint64_t count = out.count;
      const size_t size = bit_width ? bit_width / 8
          : length == 'q' || length == 'j' ? 8 : length == 'h' ? 2 : length == 'H' ? 1 : 4;
      memcpy(guest(arena, address, size), &count, size);
    } else if (conversion == 'm' && !length) {
      const char *message = strerror(error_number);
      spec += 's';
      if constexpr (std::is_same_v<Char, char>) {
        append_format(out, spec, message);
      } else {
        std::vector<wchar_t> wide(strlen(message) + 1);
        if (mbstowcs(wide.data(), message, wide.size()) == size_t(-1))
          elfconv_runtime_error("Invalid multibyte error message in guest printf.\n");
        append_format(out, spec, wide.data());
      }
    } else elfconv_runtime_error("Unsupported guest printf conversion: %c.\n", conversion);
    if (out.error) { errno = out.error; return; }
  }
}
std::string format(uint8_t *arena, const char *input, Arguments args, int *error = nullptr) {
  FormatOutput<char> out;
  format_impl(arena, input, args, out);
  if (error) *error = out.error;
  return std::move(out.buffer);
}
std::wstring wide_format(uint8_t *arena, const wchar_t *input, Arguments args) {
  FormatOutput<wchar_t> out;
  format_impl(arena, input, args, out);
  return std::move(out.buffer);
}
int print(Call &call, FILE *stream, unsigned format_arg) {
  FormatOutput<char> out;
  out.stream = stream;
  format_impl(call.arena, text(call.arena, call.word(format_arg)),
              {call.arena, call.stack + 4 * (format_arg + 2)}, out);
  if (out.error) { errno = out.error; return -1; }
  return int(out.count);
}
int print_fd(Call &call, unsigned format_arg) {
  FormatOutput<char> out;
  format_impl(call.arena, text(call.arena, call.word(format_arg)),
              {call.arena, call.stack + 4 * (format_arg + 2)}, out);
  if (out.error) { errno = out.error; return -1; }
  size_t offset = 0;
  const int fd = int32_t(call.word(0));
  while (offset < out.buffer.size()) {
    ssize_t written;
    if (!virtual_socket_write(fd, out.buffer.data() + offset,
                              out.buffer.size() - offset, written))
      written = write(fd, out.buffer.data() + offset, out.buffer.size() - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (!written) { errno = EIO; return -1; }
    offset += size_t(written);
  }
  return int(offset);
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
#define CALL Call call(arena, state, runtime)
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

static uint32_t open_guest_library(uint8_t *arena, State *state,
                                   RuntimeManager *runtime, uint32_t name_address,
                                   uint32_t flags) {
  prepare_libraries(runtime);
  if ((flags & ~0x107u) || !(flags & 3) || (flags & 3) == 3) {
    dynamic_error(runtime, "Unsupported dlopen flags", "");
    return 0;
  }
  const char *name = name_address ? text(arena, name_address) : nullptr;
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
      !runtime->dynamic_libraries[index].live) return 0;
  if (index >= _ecv_i386_library_count) {
    dynamic_error(runtime, "Library not bundled", name ? name : "<main>");
    return 0;
  }
  open_library(arena, state, runtime, index);
  if (flags & 0x100)
    for (uint32_t member : runtime->dynamic_libraries[index].scope)
      runtime->dynamic_libraries[member].global = true;
  return library_handle_base + index;
}
IMPORT(dlopen) {
  CALL;
  call.result(open_guest_library(arena, state, runtime, call.word(0), call.word(1)));
}
IMPORT(dlmopen) {
  CALL;
  if (int32_t(call.word(0)) != 0) {
    dynamic_error(runtime, "Unsupported dlmopen namespace", "");
    call.result(0);
    return;
  }
  call.result(open_guest_library(arena, state, runtime, call.word(1), call.word(2)));
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

static void initialize_libc_data(uint8_t *arena, uint32_t argc, uint32_t argv,
                                 uint32_t envp) {
  uint32_t name = 0, basename = 0;
  if (argc) {
    memcpy(&name, guest(arena, argv, 4), 4);
    if (name) {
      const char *value = text(arena, name);
      const char *slash = strrchr(value, '/');
      basename = name + (slash ? uint32_t(slash + 1 - value) : 0);
    }
  }
  for (auto *entry = _ecv_i386_libc_data; *entry; entry += 2) {
    const uint32_t value = entry[1] == 1 ? name :
        entry[1] == 2 ? basename : entry[1] == 3 ? envp : 0;
    memcpy(guest(arena, entry[0], 4), &value, 4);
  }
}

extern "C" int __ecv_i386_run_function_entry(uint8_t *arena, State *state,
                                           uint32_t entry, RuntimeManager *runtime) {
  uint32_t argc;
  const uint32_t argv = state->gpr.rsp.dword + 4;
  memcpy(&argc, guest(arena, state->gpr.rsp.dword, 4), 4);
  const uint32_t args[] = {argc, argv, argv + 4 * (argc + 1)};
  initialize_libc_data(arena, argc, argv, args[2]);
  start_library_dependencies(arena, state, runtime, args);
  for (auto *p = _ecv_i386_initializers; *p; ++p)
    invoke(arena, state, runtime, *p, args, 3);
  invoke(arena, state, runtime, entry, args, 3);
  const int status = state->gpr.rax.dword;
  drain_tls_destructors(arena, state, runtime);
  __ecv_i386_drain_guest_atexit(arena, state, runtime);
  for (auto *p = _ecv_i386_finalizers; *p; ++p)
    invoke(arena, state, runtime, *p, nullptr, 0);
  finish_libraries(arena, state, runtime);
  return status;
}
IMPORT(__libc_start_main) {
  CALL;
  uint32_t main = call.word(0), argc = call.word(1), argv = call.word(2);
  uint32_t args[] = {argc, argv, argv + 4 * (argc + 1)};
  initialize_libc_data(arena, argc, argv, args[2]);
  start_library_dependencies(arena, state, runtime, args);
  if (call.word(3)) invoke(arena, state, runtime, call.word(3), args, 3);
  else for (auto *p = _ecv_i386_initializers; *p; ++p) invoke(arena, state, runtime, *p, args, 3);
  invoke(arena, state, runtime, main, args, 3);
  int status = state->gpr.rax.dword;
  drain_tls_destructors(arena, state, runtime);
  __ecv_i386_drain_guest_atexit(arena, state, runtime);
  for (auto *p = _ecv_i386_finalizers; *p; ++p) invoke(arena, state, runtime, *p, nullptr, 0);
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
IMPORT(unsetenv) { CALL; call.result(unsetenv(text(arena, call.word(0)))); }
IMPORT(getopt) {
  CALL;
  const int argc = int32_t(call.word(0));
  const uint32_t argv_address = call.word(1);
  if (argc < 0 || uint64_t(argc + 1) * 4 > MEMORY_ARENA_SIZE) {
    errno = EINVAL;
    call.result(-1);
    return;
  }
  auto *guest_argv = static_cast<uint8_t *>(guest(arena, argv_address, uint64_t(argc + 1) * 4));
  std::vector<char *> host_argv;
  host_argv.reserve(size_t(argc) + 1);
  for (int i = 0; i < argc; ++i) {
    uint32_t address;
    memcpy(&address, guest_argv + size_t(i) * 4, 4);
    host_argv.push_back(address ? const_cast<char *>(text(arena, address)) : nullptr);
  }
  host_argv.push_back(nullptr);
  static RuntimeManager *active_runtime = nullptr;
  static uint32_t active_argv = 0;
  if (active_runtime != runtime || active_argv != argv_address) {
    optind = 1;
    active_runtime = runtime;
    active_argv = argv_address;
  }
  call.result(::getopt(argc, host_argv.data(), text(arena, call.word(2))));
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
IMPORT(dprintf) { CALL; call.result(print_fd(call, 1)); }
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
  FILE *file = stream(call.word(3));
  const size_t result = fwrite(guest(arena, call.word(0), length), size, count, file);
  if (result < count && ferror(file) && !errno) errno = EBADF;
  call.result(result);
}
void obstack_newchunk(uint8_t *arena, uint32_t handle, uint32_t needed) {
  uint32_t fields[11]{};
  memcpy(fields, guest(arena, handle, sizeof(fields)), sizeof(fields));
  const uint32_t old_chunk = fields[1], object_base = fields[2], next_free = fields[3];
  const uint32_t chunk_size = fields[0];
  if (!old_chunk || next_free < object_base) elfconv_runtime_error("Invalid guest obstack state.\n");
  const uint32_t used = next_free - object_base;
  const uint64_t requested = uint64_t(used) + needed + fields[6] + 1;
  const uint32_t capacity = uint32_t(std::max<uint64_t>(chunk_size, requested));
  if (capacity > UINT32_MAX - 8) elfconv_runtime_error("Guest obstack chunk is too large.\n");
  const uint32_t chunk = allocate(capacity + 8);
  if (!chunk) elfconv_runtime_error("Guest obstack allocation failed.\n");
  uint8_t *header = static_cast<uint8_t *>(guest(arena, chunk, capacity + 8));
  const uint32_t limit = chunk + 8 + capacity;
  memcpy(header, &limit, 4);
  memcpy(header + 4, &old_chunk, 4);
  if (used) memcpy(header + 8, guest(arena, object_base, used), used);
  fields[1] = chunk;
  fields[2] = chunk + 8;
  fields[3] = chunk + 8 + used;
  fields[4] = limit;
  memcpy(guest(arena, handle, sizeof(fields)), fields, sizeof(fields));
}
IMPORT(_obstack_begin) {
  CALL;
  const uint32_t handle = call.word(0);
  const uint32_t capacity = call.word(1) ? call.word(1) : 4096;
  const uint32_t alignment = call.word(2) ? call.word(2) - 1 : 7;
  if (capacity > UINT32_MAX - 8) { errno = ENOMEM; call.result(0); return; }
  const uint32_t chunk = allocate(capacity + 8);
  if (!chunk) { call.result(0); return; }
  uint32_t fields[11]{};
  fields[0] = capacity;
  fields[1] = chunk;
  fields[2] = fields[3] = chunk + 8;
  fields[4] = chunk + 8 + capacity;
  fields[6] = alignment;
  memcpy(guest(arena, chunk, 8), &fields[4], 4);
  const uint32_t previous = 0;
  memcpy(guest(arena, chunk + 4, 4), &previous, 4);
  memcpy(guest(arena, handle, sizeof(fields)), fields, sizeof(fields));
  call.result(1);
}
IMPORT(_obstack_newchunk) {
  CALL;
  obstack_newchunk(arena, call.word(0), call.word(1));
}
IMPORT(obstack_printf) {
  CALL;
  int error = 0;
  const std::string output = format(arena, text(arena, call.word(1)),
                                    {arena, call.stack + 12}, &error);
  if (error) { errno = error; call.result(-1); return; }
  const uint32_t handle = call.word(0);
  uint32_t next_free, chunk_limit;
  memcpy(&next_free, guest(arena, handle + 12, 4), 4);
  memcpy(&chunk_limit, guest(arena, handle + 16, 4), 4);
  if (output.size() > chunk_limit - next_free)
    obstack_newchunk(arena, handle, uint32_t(output.size()));
  memcpy(&next_free, guest(arena, handle + 12, 4), 4);
  memcpy(guest(arena, next_free, output.size()), output.data(), output.size());
  next_free += uint32_t(output.size());
  memcpy(guest(arena, handle + 12, 4), &next_free, 4);
  call.result(output.size());
}
IMPORT(mcheck_check_all) {
  CALL;
  std::lock_guard<std::recursive_mutex> lock(allocation_mutex);
  uint64_t end = 0;
  for (const auto &entry : allocations) {
    if (entry.first < end || !entry.second.size ||
        uint64_t(entry.first) + entry.second.size > MEMORY_ARENA_SIZE)
      elfconv_runtime_error("Guest heap metadata is corrupt.\n");
    end = uint64_t(entry.first) + entry.second.size;
  }
}
IMPORT(mcheck_pedantic) {
  CALL;
  std::lock_guard<std::recursive_mutex> lock(allocation_mutex);
  uint64_t end = 0;
  for (const auto &entry : allocations) {
    if (entry.first < end || !entry.second.size ||
        uint64_t(entry.first) + entry.second.size > MEMORY_ARENA_SIZE)
      elfconv_runtime_error("Guest heap metadata is corrupt.\n");
    end = uint64_t(entry.first) + entry.second.size;
  }
  call.result(0);
}
IMPORT(register_printf_function) {
  CALL;
  const int spec = int32_t(call.word(0));
  if (spec <= 0 || spec > 127 || !call.word(1) || !call.word(2)) {
    errno = EINVAL;
    call.result(-1);
    return;
  }
  registered_printf_sizes[char(spec)] = true;
  call.result(0);
}
IMPORT(printf_size_info) {
  CALL;
  if (call.word(1)) {
    const int32_t type = 6;
    memcpy(guest(arena, call.word(2), 4), &type, 4);
  }
  call.result(1);
}
IMPORT(printf_size) {
  CALL;
  const uint32_t info = call.word(1), args_address = call.word(2);
  int32_t precision, width;
  uint32_t spec, flags;
  memcpy(&precision, guest(arena, info, 4), 4);
  memcpy(&width, guest(arena, info + 4, 4), 4);
  memcpy(&spec, guest(arena, info + 8, 4), 4);
  memcpy(&flags, guest(arena, info + 12, 4), 4);
  uint32_t value_address;
  memcpy(&value_address, guest(arena, args_address, 4), 4);
  double value;
  memcpy(&value, guest(arena, value_address, 8), 8);
  FormatOutput<char> output;
  output.stream = stream(call.word(0));
  append_size_format(output, value, char(spec), flags & (1u << 3), flags & (1u << 5),
                     false, flags & (1u << 6), flags & (1u << 4), width, precision);
  if (output.error) { errno = output.error; call.result(-1); }
  else call.result(output.count);
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
  const char *path = text(arena, call.word(0));
  const char *mode = text(arena, call.word(1));
  FILE *file;
  if (strcmp(path, "/dev/zero") == 0) {
    cookie_io_functions_t io{};
    io.seek = zero_device_seek;
    io.read = zero_device_read;
    io.write = zero_device_write;
    io.close = zero_device_close;
    file = fopencookie(nullptr, mode, io);
  } else {
    file = fopen(path, mode);
  }
  if (file && mode[0] == 'a' && !strchr(mode, '+')) (void) fseek(file, 0, SEEK_END);
  if (!file) { call.result(0); return; }
  uint32_t handle = next_file++;
  files.emplace(handle, file);
  call.result(handle);
}
IMPORT(fopencookie) {
  CALL;
  uint32_t functions[4] = {call.word(2), call.word(3), call.word(4), call.word(5)};
  auto *cookie = new GuestCookie{arena, state, runtime, call.word(0),
                                 functions[0], functions[1], functions[2], functions[3]};
  cookie_io_functions_t host_functions{};
  host_functions.read = functions[0] ? guest_cookie_read : nullptr;
  host_functions.write = functions[1] ? guest_cookie_write : nullptr;
  host_functions.seek = functions[2] ? guest_cookie_seek : nullptr;
  host_functions.close = guest_cookie_close;
  FILE *file = fopencookie(cookie, text(arena, call.word(1)), host_functions);
  if (!file) { delete cookie; call.result(0); return; }
  if (setvbuf(file, nullptr, _IONBF, 0) != 0) {
    fclose(file);
    call.result(0);
    return;
  }
  const uint32_t handle = next_file++;
  files.emplace(handle, file);
  call.result(handle);
}
IMPORT(tmpfile) {
  CALL;
  FILE *file = ::tmpfile();
  if (!file) { call.result(0); return; }
  const uint32_t handle = next_file++;
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
IMPORT(freopen64) { __ecv_i386_freopen(arena, state, 0, runtime); }
IMPORT(fclose) {
  CALL;
  const uint32_t handle = call.word(0);
  FILE *file = stream(handle);
  const int descriptor = fileno(file);
  const off_t position = ftello(file);
  if (descriptor >= 0 && position >= 0) closed_stream_offsets[descriptor] = position;
  int result = fclose(file);
  auto mem = memstreams.find(handle);
  if (mem != memstreams.end()) {
    if (sync_memstream(arena, handle) < 0) result = EOF;
    free(mem->second.host_buffer);
    memstreams.erase(mem);
  }
  files.erase(handle);
  call.result(result);
}
IMPORT(fread) {
  CALL;
  uint32_t size = call.word(1), count = call.word(2);
  if (!size || !count) { call.result(0); return; }
  uint64_t length = uint64_t(size) * count;
  FILE *file = stream(call.word(3));
  const size_t result = fread(guest(arena, call.word(0), length), size, count, file);
  if (result < count && ferror(file) && !errno) errno = EBADF;
  call.result(result);
}
IMPORT(fseek) { CALL; call.result(fseek(stream(call.word(0)), int32_t(call.word(1)), call.word(2))); }
IMPORT(fseeko) { CALL; call.result(fseeko(stream(call.word(0)), int32_t(call.word(1)), call.word(2))); }
IMPORT(ftell) {
  CALL;
  FILE *file = stream(call.word(0));
  const bool eof = feof(file);
  if (fseek(file, 0, SEEK_CUR) != 0) { call.result(-1); return; }
  if (eof) (void) fgetc(file);
  call.result(ftell(file));
}
IMPORT(ftello) { CALL; call.result(ftello(stream(call.word(0)))); }
IMPORT(fseeko64) {
  CALL;
  call.result(fseeko(stream(call.word(0)), int64_t(call.wide(1)), int32_t(call.word(3))));
}
IMPORT(ftello64) { CALL; call.result64(ftello(stream(call.word(0)))); }
IMPORT(fgetpos) {
  CALL;
  const off_t position = ftello(stream(call.word(0)));
  if (position < INT32_MIN || position > INT32_MAX) {
    errno = EOVERFLOW;
    call.result(-1);
    return;
  }
  const int32_t guest_position = int32_t(position);
  memcpy(guest(arena, call.word(1), 4), &guest_position, 4);
  call.result(0);
}
IMPORT(fsetpos) {
  CALL;
  int32_t position;
  memcpy(&position, guest(arena, call.word(1), 4), 4);
  call.result(fseeko(stream(call.word(0)), position, SEEK_SET));
}
IMPORT(fgetpos64) {
  CALL;
  const int64_t position = ftello(stream(call.word(0)));
  if (position == -1) { call.result(-1); return; }
  memcpy(guest(arena, call.word(1), 8), &position, 8);
  call.result(0);
}
IMPORT(fsetpos64) {
  CALL;
  int64_t position;
  memcpy(&position, guest(arena, call.word(1), 8), 8);
  call.result(fseeko(stream(call.word(0)), position, SEEK_SET));
}
IMPORT(fegetround) { CALL; call.result(fegetround()); }
#if defined(__EMSCRIPTEN__)
IMPORT(fesetround) {
  CALL;
  const int mode = int32_t(call.word(0));
  if (mode != FE_TONEAREST) { errno = ENOTSUP; call.result(-1); }
  else call.result(0);
}
#else
IMPORT(fesetround) { CALL; call.result(fesetround(int32_t(call.word(0)))); }
#endif
off_t guest_lseek(int fd, off_t offset, int whence) {
  if (!offset && whence == SEEK_CUR) {
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
      if (fileno(it->second) == fd) {
        const off_t position = ftello(it->second);
        if (position >= 0) return ::lseek(fd, position, SEEK_SET);
      }
    }
    auto it = closed_stream_offsets.find(fd);
    if (it != closed_stream_offsets.end()) {
      const off_t position = it->second;
      closed_stream_offsets.erase(it);
      return ::lseek(fd, position, SEEK_SET);
    }
  }
  closed_stream_offsets.erase(fd);
  return ::lseek(fd, offset, whence);
}
IMPORT(lseek) {
  CALL;
  const off_t result = guest_lseek(int32_t(call.word(0)), int32_t(call.word(1)),
                                   int32_t(call.word(2)));
  if (result > INT32_MAX || result < INT32_MIN) {
    errno = EOVERFLOW;
    call.result(-1);
  } else {
    call.result(uint32_t(int32_t(result)));
  }
}
IMPORT(lseek64) {
  CALL;
  call.result64(guest_lseek(int32_t(call.word(0)), int64_t(call.wide(1)),
                           int32_t(call.word(3))));
}
IMPORT(feof) { CALL; call.result(feof(stream(call.word(0)))); }
IMPORT(rewind) { CALL; rewind(stream(call.word(0))); }
IMPORT(fflush) {
  CALL;
  const uint32_t handle = call.word(0);
  int result = fflush(handle ? stream(handle) : nullptr);
  if (result == 0) {
    if (handle) {
      FILE *file = stream(handle);
      const int saved_errno = errno;
      if (fseek(file, 0, SEEK_CUR) != 0) { clearerr(file); errno = saved_errno; }
      result = sync_memstream(arena, handle);
    } else {
      for (const auto &entry : files) {
        const int saved_errno = errno;
        if (fseek(entry.second, 0, SEEK_CUR) != 0) clearerr(entry.second);
        errno = saved_errno;
      }
      result = sync_memstreams(arena);
    }
  }
  call.result(result);
}
IMPORT(fgetc) { CALL; call.result(fgetc(stream(call.word(0)))); }
IMPORT(getc) { CALL; call.result(getc(stream(call.word(0)))); }
IMPORT(ungetc) { CALL; call.result(ungetc(int32_t(call.word(0)), stream(call.word(1)))); }
IMPORT(ferror) { CALL; call.result(ferror(stream(call.word(0)))); }
IMPORT(clearerr) { CALL; clearerr(stream(call.word(0))); }
IMPORT(fileno) { CALL; call.result(fileno(stream(call.word(0)))); }
IMPORT(fmemopen) {
  CALL;
  const uint32_t buffer_address = call.word(0), size = call.word(1);
  const char *mode = text(arena, call.word(2));
  if (buffer_address && mode[0] == 'w' && strchr(mode, '+'))
    *static_cast<char *>(guest(arena, buffer_address, 1)) = 0;
  FILE *file;
  if (size == 0) {
    cookie_io_functions_t io{};
    if (mode[0] == 'r' || strchr(mode, '+')) io.read = zero_length_memfile_read;
    if (mode[0] != 'r' || strchr(mode, '+')) io.write = zero_length_memfile_write;
    io.seek = zero_length_memfile_seek;
    io.close = zero_device_close;
    file = fopencookie(nullptr, mode, io);
  } else {
    file = fmemopen(buffer_address ? guest(arena, buffer_address, size) : nullptr, size, mode);
  }
  if (!file) { call.result(0); return; }
  const uint32_t handle = next_file++;
  files.emplace(handle, file);
  call.result(handle);
}
IMPORT(open_memstream) {
  CALL;
  const uint32_t handle = next_file++;
  auto inserted = memstreams.emplace(handle, GuestMemStream{});
  GuestMemStream &mem = inserted.first->second;
  mem.buffer_output = call.word(0);
  mem.size_output = call.word(1);
  memcpy(guest(arena, mem.buffer_output, 4), &mem.guest_buffer, 4);
  const uint32_t size = 0;
  memcpy(guest(arena, mem.size_output, 4), &size, 4);
  FILE *file = open_memstream(&mem.host_buffer, &mem.size);
  if (!file) { memstreams.erase(inserted.first); call.result(0); return; }
  files.emplace(handle, file);
  call.result(handle);
}
IMPORT(gets) {
  CALL;
  const uint32_t destination = call.word(0);
  auto *buffer = static_cast<char *>(guest(arena, destination, 1));
  size_t length = 0;
  for (;;) {
    const int c = fgetc(stdin);
    if (c == EOF) {
      if (ferror(stdin) || !length) { call.result(0); return; }
      break;
    }
    if (c == '\n') break;
    if (uint64_t(destination) + length + 1 >= MEMORY_ARENA_SIZE)
      elfconv_runtime_error("Guest gets string exceeds the memory arena.\n");
    buffer[length++] = char(c);
  }
  buffer[length] = '\0';
  call.result(destination);
}
IMPORT(fgets) {
  CALL;
  const int size = int32_t(call.word(1));
  if (size <= 0) { call.result(0); return; }
  FILE *file = stream(call.word(2));
  const uint32_t destination = call.word(0);
  if (!destination) {
    const int character = fgetc(file);
    if (character == EOF) { call.result(0); return; }
    ungetc(character, file);
    errno = EFAULT;
    call.result(0);
    return;
  }
  auto *buffer = static_cast<char *>(guest(arena, destination, size));
  call.result(fgets(buffer, size, file) ? destination : 0);
}
IMPORT(getline) {
  CALL;
  const uint32_t line_slot = call.word(0), capacity_slot = call.word(1);
  FILE *file = stream(call.word(2));
  if (!line_slot || !capacity_slot) {
    errno = EINVAL;
    (void) fputc(0, file);
    errno = EINVAL;
    call.result(-1);
    return;
  }
  uint32_t buffer, capacity;
  memcpy(&buffer, guest(arena, line_slot, 4), 4);
  memcpy(&capacity, guest(arena, capacity_slot, 4), 4);
  if (!buffer) {
    capacity = 128;
    buffer = allocate(capacity);
    if (!buffer) { errno = ENOMEM; call.result(-1); return; }
    memcpy(guest(arena, line_slot, 4), &buffer, 4);
    memcpy(guest(arena, capacity_slot, 4), &capacity, 4);
  }
  if (ferror(file)) { call.result(-1); return; }
  size_t length = 0;
  for (;;) {
    const int character = fgetc(file);
    if (character == EOF) {
      if (!length) { call.result(-1); return; }
      break;
    }
    if (length > UINT32_MAX - 2) { errno = EOVERFLOW; call.result(-1); return; }
    const uint32_t required = uint32_t(length + 2);
    if (required > capacity) {
      uint32_t new_capacity = capacity ? capacity : 128;
      while (new_capacity < required) {
        if (new_capacity > UINT32_MAX / 2) { new_capacity = required; break; }
        new_capacity *= 2;
      }
      const uint32_t replacement = allocate(new_capacity);
      if (!replacement) { errno = ENOMEM; call.result(-1); return; }
      const uint32_t preserved = buffer ? std::min(capacity, new_capacity) : 0;
      if (preserved)
        memcpy(guest(arena, replacement, preserved), guest(arena, buffer, preserved), preserved);
      if (buffer) release(buffer);
      buffer = replacement;
      capacity = new_capacity;
    }
    static_cast<char *>(guest(arena, buffer + uint32_t(length), 1))[0] = char(character);
    ++length;
    if (character == '\n') break;
  }
  static_cast<char *>(guest(arena, buffer + uint32_t(length), 1))[0] = '\0';
  memcpy(guest(arena, line_slot, 4), &buffer, 4);
  memcpy(guest(arena, capacity_slot, 4), &capacity, 4);
  call.result(int32_t(length));
}
IMPORT(fgets_unlocked) {
  CALL;
  const uint32_t destination = call.word(0);
  const int size = int32_t(call.word(1));
  if (size <= 0) { call.result(0); return; }
  auto *buffer = static_cast<char *>(guest(arena, destination, size));
  call.result(fgets(buffer, size, stream(call.word(2))) ? destination : 0);
}
IMPORT(fputs) { CALL; call.result(fputs(text(arena, call.word(0)), stream(call.word(1)))); }
IMPORT(perror) {
  CALL;
  const int saved_errno = errno;
  std::string message;
  if (call.word(0)) {
    message = text(arena, call.word(0));
    message += ": ";
  }
  const char *description = saved_errno == EILSEQ
      ? "Invalid or incomplete multibyte or wide character" : strerror(saved_errno);
  message += description;
  message += '\n';
  FILE *target = stderr;
  for (auto it = files.rbegin(); it != files.rend(); ++it) {
    if (fileno(it->second) == STDERR_FILENO) {
      target = it->second;
      break;
    }
  }
  if (target) fflush(target);
  const int fd = target && fileno(target) >= 0 ? fileno(target) : STDERR_FILENO;
  size_t offset = 0;
  while (offset < message.size()) {
    const ssize_t written = ::write(fd, message.data() + offset, message.size() - offset);
    if (written < 0) break;
    offset += size_t(written);
  }
  errno = saved_errno;
}
IMPORT(error) {
  CALL;
  int format_error = 0;
  const auto message = format(arena, text(arena, call.word(2)),
                              {arena, call.stack + 16}, &format_error);
  if (format_error) errno = format_error;
  fprintf(stderr, "%s", message.c_str());
  if (call.word(1)) fprintf(stderr, ": %s", strerror(i386_errno_to_host(call.word(1))));
  fputc('\n', stderr);
  if (call.word(0)) exit(call.word(0));
}
IMPORT(fdopen) {
  CALL;
  const int fd = int32_t(call.word(0));
  const char *mode = text(arena, call.word(1));
  const int flags = fcntl(fd, F_GETFL);
  const int access = flags < 0 ? -1 : flags & O_ACCMODE;
  const bool update = strchr(mode, '+') != nullptr;
  const bool read_only_mode = mode[0] == 'r' && !update;
  if ((update && access != O_RDWR) ||
      (read_only_mode && access == O_WRONLY) ||
      (!read_only_mode && !update && mode[0] != 'r' && access == O_RDONLY)) {
    errno = flags < 0 ? errno : EINVAL;
    call.result(0);
    return;
  }
  FILE *file = fdopen(fd, mode);
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
IMPORT(setlinebuf) { CALL; call.result(setvbuf(stream(call.word(0)), nullptr, _IOLBF, 0)); }
IMPORT(fwide) { CALL; call.result(fwide(stream(call.word(0)), int32_t(call.word(1)))); }
IMPORT(_exit) { CALL; _Exit(int32_t(call.word(0))); }
IMPORT(fputc) { CALL; call.result(fputc(call.word(0), stream(call.word(1)))); }
IMPORT(putc) { CALL; call.result(putc(int32_t(call.word(0)), stream(call.word(1)))); }
IMPORT(putc_unlocked) { CALL; call.result(putc_unlocked(int32_t(call.word(0)), stream(call.word(1)))); }
IMPORT(putchar) { CALL; call.result(putchar(call.word(0))); }
IMPORT(exit) {
  CALL;
  const uint32_t status = call.word(0);
  drain_tls_destructors(arena, state, runtime);
  __ecv_i386_drain_guest_atexit(arena, state, runtime);
  for (auto *p = _ecv_i386_finalizers; *p; ++p)
    invoke(arena, state, runtime, *p, nullptr, 0);
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
  int format_error = 0;
  auto out = format(call.arena, text(call.arena, call.word(format_arg)), {call.arena, args}, &format_error);
  uint32_t capacity = bounded ? call.word(1) : uint32_t(out.size() + 1);
  if (fortified && !bounded && capacity > call.word(2))
    elfconv_runtime_error("Guest fortified formatting overflow.\n");
  if (capacity) {
    size_t length = std::min(out.size(), size_t(capacity - 1));
    auto *dst = static_cast<char *>(guest(call.arena, call.word(0), length + 1));
    memcpy(dst, out.data(), length);
    dst[length] = 0;
  }
  if (format_error) errno = format_error;
  call.result(format_error ? -1 : int32_t(out.size()));
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
IMPORT(__acosf_finite) { CALL; floating_result(state, acosf(call.real(0))); }
IMPORT(asinf) { CALL; floating_result(state, asinf(call.real(0))); }
IMPORT(atanf) { CALL; floating_result(state, atanf(call.real(0))); }
IMPORT(atan2f) { CALL; floating_result(state, atan2f(call.real(0), call.real(1))); }
IMPORT(__atan2f_finite) { CALL; floating_result(state, atan2f(call.real(0), call.real(1))); }
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
IMPORT(__pow_finite) { CALL; floating_result(state, pow(call.real64(0), call.real64(2))); }
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
IMPORT(__vsprintf_chk) { CALL; buffer_format(call, false, true, true); }
IMPORT(asprintf) {
  CALL;
  const uint32_t output = call.word(0);
  const uint32_t null_pointer = 0;
  memcpy(guest(arena, output, 4), &null_pointer, 4);
  int format_error = 0;
  auto result = format(arena, text(arena, call.word(1)), {arena, call.stack + 12}, &format_error);
  if (format_error) { errno = format_error; call.result(-1); return; }
  const uint32_t address = allocate(uint32_t(result.size() + 1));
  if (!address) { call.result(-1); return; }
  auto *buffer = static_cast<char *>(guest(arena, address, result.size() + 1));
  memcpy(buffer, result.data(), result.size());
  buffer[result.size()] = 0;
  memcpy(guest(arena, output, 4), &address, 4);
  call.result(uint32_t(result.size()));
}
IMPORT(vasprintf) {
  CALL;
  const uint32_t output = call.word(0);
  const uint32_t null_pointer = 0;
  memcpy(guest(arena, output, 4), &null_pointer, 4);
  int format_error = 0;
  auto result = format(arena, text(arena, call.word(1)), {arena, call.word(2)}, &format_error);
  if (format_error) { errno = format_error; call.result(-1); return; }
  const uint32_t address = allocate(uint32_t(result.size() + 1));
  if (!address) { call.result(-1); return; }
  auto *buffer = static_cast<char *>(guest(arena, address, result.size() + 1));
  memcpy(buffer, result.data(), result.size());
  buffer[result.size()] = 0;
  memcpy(guest(arena, output, 4), &address, 4);
  call.result(uint32_t(result.size()));
}
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
IMPORT(pipe) {
  CALL;
  const uint32_t address = call.word(0);
  if (!address) { errno = EFAULT; call.result(-1); return; }
  int descriptors[2];
  const int result = pipe(descriptors);
  if (!result) memcpy(guest(arena, address, sizeof(descriptors)), descriptors, sizeof(descriptors));
  call.result(result);
}
IMPORT(socketpair) {
  CALL;
  const uint32_t address = call.word(3);
  if (!address) { errno = EFAULT; call.result(-1); return; }
  const int domain = int32_t(call.word(0));
  const int type = int32_t(call.word(1));
  const int protocol = int32_t(call.word(2));
  if ((domain != AF_LOCAL && domain != AF_UNIX) ||
      (type & 0xf) != SOCK_DGRAM || protocol != 0) {
    errno = EPROTONOSUPPORT;
    call.result(-1);
    return;
  }
  auto pair = std::make_shared<VirtualDatagramPair>();
  int descriptors[2] = {next_virtual_socket++, next_virtual_socket++};
  virtual_datagram_sockets.emplace(descriptors[0], VirtualDatagramEnd{pair, 0});
  virtual_datagram_sockets.emplace(descriptors[1], VirtualDatagramEnd{pair, 1});
  memcpy(guest(arena, address, sizeof(descriptors)), descriptors, sizeof(descriptors));
  call.result(0);
}
IMPORT(close) {
  CALL;
  const int fd = int32_t(call.word(0));
  call.result(virtual_socket_close(fd) ? 0 : close(fd));
}
IMPORT(read) {
  CALL;
  const int fd = int32_t(call.word(0));
  const uint32_t size = call.word(2);
  void *buffer = size ? guest(arena, call.word(1), size) : nullptr;
  ssize_t result;
  if (!virtual_socket_read(fd, buffer, size, result))
    result = read(fd, buffer, size);
  call.result(result);
}
IMPORT(write) {
  CALL;
  const int fd = int32_t(call.word(0));
  const uint32_t size = call.word(2);
  const void *buffer = size ? guest(arena, call.word(1), size) : nullptr;
  ssize_t result;
  if (!virtual_socket_write(fd, buffer, size, result))
    result = write(fd, buffer, size);
  call.result(result);
}
IMPORT(access) { CALL; call.result(access(text(arena, call.word(0)), call.word(1))); }
IMPORT(pathconf) { CALL; call.result(pathconf(text(arena, call.word(0)), int32_t(call.word(1)))); }
IMPORT(chmod) { CALL; call.result(chmod(text(arena, call.word(0)), call.word(1))); }
IMPORT(chown) { CALL; call.result(chown(text(arena, call.word(0)), call.word(1), call.word(2))); }
IMPORT(lchown) { CALL; call.result(lchown(text(arena, call.word(0)), call.word(1), call.word(2))); }
IMPORT(chdir) { CALL; call.result(chdir(text(arena, call.word(0)))); }
IMPORT(rename) { CALL; call.result(rename(text(arena, call.word(0)), text(arena, call.word(1)))); }
IMPORT(remove) { CALL; call.result(remove(text(arena, call.word(0)))); }
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
IMPORT(fstat64) {
  CALL;
  struct stat host;
  int result = fstat(int32_t(call.word(0)), &host);
  if (!result) result = store_stat64(arena, call.word(1), host);
  call.result(result);
}
IMPORT(stat64) {
  CALL;
  struct stat host;
  int result = stat(text(arena, call.word(0)), &host);
  if (!result) result = store_stat64(arena, call.word(1), host);
  call.result(result);
}
IMPORT(lstat64) {
  CALL; struct stat host;
  int result = lstat(text(arena, call.word(0)), &host);
  if (!result) result = store_stat64(arena, call.word(1), host);
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
IMPORT(fdopendir) {
  CALL; DIR *dir = fdopendir(int32_t(call.word(0)));
  if (!dir) { call.result(0); return; }
  uint32_t entry = allocate(276);
  if (!entry) { closedir(dir); call.result(0); return; }
  uint32_t handle = next_directory++;
  directories.emplace(handle, Directory{dir, entry});
  call.result(handle);
}
IMPORT(dirfd) {
  CALL;
  auto found = directories.find(call.word(0));
  if (found == directories.end()) elfconv_runtime_error("Invalid guest DIR handle.\n");
  call.result(dirfd(found->second.host));
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
IMPORT(qsort_r) {
  CALL;
  const uint32_t base = call.word(0), count = call.word(1), size = call.word(2);
  const uint32_t comparator = call.word(3), context = call.word(4);
  if (count < 2 || !size) return;
  guest(arena, base, uint64_t(count) * size);
  auto less = [&](uint32_t a, uint32_t b) {
    uint32_t args[] = {base + a * size, base + b * size, context};
    invoke(arena, state, runtime, comparator, args, 3);
    return int32_t(state->gpr.rax.dword) < 0;
  };
  auto swap = [&](uint32_t a, uint32_t b) {
    for (uint32_t i = 0; i < size; ++i)
      std::swap(arena[base + a * size + i], arena[base + b * size + i]);
  };
  auto sift = [&](uint32_t root, uint32_t end) {
    while (uint64_t(root) * 2 + 1 < end) {
      uint32_t child = root * 2 + 1;
      if (child + 1 < end && less(child, child + 1)) ++child;
      if (!less(root, child)) break;
      swap(root, child); root = child;
    }
  };
  for (uint32_t i = count / 2; i; --i) sift(i - 1, count);
  for (uint32_t end = count - 1; end; --end) { swap(0, end); sift(0, end); }
}

IMPORT(__isoc99_sscanf) {
  CALL;
  call.result(scan_guest_string(arena, text(arena, call.word(0)),
      text(arena, call.word(1)), {arena, call.stack + 12}));
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

IMPORT(SDL_Init) {
  CALL;
#if defined(__EMSCRIPTEN__)
  EM_ASM({ if (Module.__elfconvTraceEvent($0))
    Module.printErr('SDL_Init worker='+(typeof workerID==='number'?workerID:-1)+
      ' pc=0x'+$0.toString(16)+' flags='+$1); },
    state->gpr.rip.dword, call.word(0));
#endif
  const int result = SDL_Init(call.word(0));
  call.result(result);
}
IMPORT(SDL_Quit) {
  CALL;
  reset_guest_renderers();
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
#if defined(__EMSCRIPTEN__)
  EM_ASM({ if (Module.__elfconvTraceEvent($0))
    Module.printErr('SDL_CreateWindow worker='+(typeof workerID==='number'?workerID:-1)+
      ' pc=0x'+$0.toString(16)+' size='+$1+'x'+$2+' flags='+$3); },
    state->gpr.rip.dword, call.word(3), call.word(4), call.word(5));
#endif
  windows[slot] = SDL_CreateWindow(text(arena, call.word(0)), call.word(1), call.word(2),
                                  call.word(3), call.word(4), call.word(5));
#if defined(__EMSCRIPTEN__)
  EM_ASM({ if (Module.__elfconvTraceEvent($0))
    Module.printErr('SDL_CreateWindow result worker='+(typeof workerID==='number'?workerID:-1)+
      ' pc=0x'+$0.toString(16)+' ok='+$1+' sdl='+(UTF8ToString($2)||'(none)')); },
    state->gpr.rip.dword, windows[slot] ? 1 : 0, SDL_GetError());
#endif
  call.result(windows[slot] ? 0x100 + slot : 0);
}
IMPORT(SDL_DestroyWindow) {
  CALL;
  if (call.word(0)) {
    destroy_guest_renderers_for_window(call.word(0));
    SDL_DestroyWindow(window(call.word(0)));
    windows[call.word(0) - 0x100] = nullptr;
  }
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
  const int makeCurrent = handle > 0
      ? emscripten_webgl_make_context_current(handle) : EMSCRIPTEN_RESULT_FAILED;
  if (handle > 0 && makeCurrent == EMSCRIPTEN_RESULT_SUCCESS) {
    contexts[slot] = reinterpret_cast<SDL_GLContext>(uintptr_t(handle));
  } else if (handle > 0) {
    emscripten_webgl_destroy_context(handle);
  }
  EM_ASM({ if (Module.elfconvTrace)
    Module.printErr('SDL_GL_CreateContext worker='+
      (typeof workerID==='number'?workerID:-1)+' pc=0x'+$0.toString(16)+
      ' handle='+$1+' makeCurrent='+$2+' current='+$3); },
    state->gpr.rip.dword, int(handle), makeCurrent,
    emscripten_webgl_get_current_context());
#else
  contexts[slot] = SDL_GL_CreateContext(window(call.word(0)));
#endif
#if defined(__EMSCRIPTEN__) && ECV_USE_GL4ES
  if (contexts[slot])
    initialize_gl4es_for_window(window(call.word(0)));
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
  await new Promise(resolve => setTimeout(resolve, 16));
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
static SDL_Joystick *guest_joysticks[32] = {};
struct GuestJoystickRefs {
  unsigned direct_opens = 0;
  unsigned controller_refs = 0;
};
static GuestJoystickRefs guest_joystick_refs[32] = {};
static SDL_RWops *guest_rwops[32] = {};
static uint32_t guest_keyboard_state;
static int guest_keyboard_count;
static uint32_t opaque_handle(unsigned slot, uint32_t base, unsigned count) {
  if (slot >= count) elfconv_runtime_error("Guest SDL handle table exhausted.\n");
  return base + slot;
}
static int joystick_slot(SDL_Joystick *host) {
  if (!host) return -1;
  for (unsigned i = 0; i < 32; ++i)
    if (guest_joysticks[i] == host) return int(i);
  return -1;
}
static int ensure_joystick_slot(SDL_Joystick *host) {
  if (!host) elfconv_runtime_error("SDL returned a null joystick handle.\n");
  int slot = joystick_slot(host);
  if (slot >= 0) return slot;
  for (unsigned i = 0; i < 32; ++i) {
    if (!guest_joysticks[i]) {
      guest_joysticks[i] = host;
      return int(i);
    }
  }
  elfconv_runtime_error("Guest SDL joystick handle table exhausted.\n");
}
static void release_joystick_slot(unsigned slot) {
  if (!guest_joystick_refs[slot].direct_opens &&
      !guest_joystick_refs[slot].controller_refs) {
    guest_joysticks[slot] = nullptr;
    guest_joystick_refs[slot] = {};
  }
}
static SDL_Joystick *joystick(uint32_t handle) {
  if (!handle) return nullptr;
  if (handle < 0x900 || handle >= 0x920 || !guest_joysticks[handle - 0x900])
    elfconv_runtime_error("Invalid guest SDL joystick handle.\n");
  return guest_joysticks[handle - 0x900];
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
    if (!write_event(arena, call.word(0), event)) found = 0;
    else dispatch_guest_event_watches(arena, state, runtime, call.word(0));
  }
  call.result(found);
}
IMPORT(SDL_GetRelativeMouseState) {
  CALL; int x, y; Uint32 buttons = SDL_GetRelativeMouseState(&x, &y);
  if (call.word(0)) memcpy(guest(arena, call.word(0), 4), &x, 4);
  if (call.word(1)) memcpy(guest(arena, call.word(1), 4), &y, 4);
  call.result(buttons);
}
IMPORT(SDL_GetMouseState) {
  CALL; int x, y; Uint32 buttons = SDL_GetMouseState(&x, &y);
  if (call.word(0)) memcpy(guest(arena, call.word(0), 4), &x, 4);
  if (call.word(1)) memcpy(guest(arena, call.word(1), 4), &y, 4);
  call.result(buttons);
}
IMPORT(SDL_NumJoysticks) { CALL; call.result(SDL_NumJoysticks()); }
IMPORT(SDL_IsGameController) { CALL; call.result(SDL_IsGameController(int(call.word(0)))); }
IMPORT(SDL_JoystickOpen) {
  CALL;
  SDL_Joystick *host = SDL_JoystickOpen(int(call.word(0)));
  if (!host) { call.result(0); return; }
  const unsigned slot = ensure_joystick_slot(host);
  ++guest_joystick_refs[slot].direct_opens;
  call.result(opaque_handle(slot, 0x900, 32));
}
IMPORT(SDL_JoystickClose) {
  CALL;
  const uint32_t handle = call.word(0);
  if (!handle) return;
  SDL_Joystick *host = joystick(handle);
  const unsigned slot = handle - 0x900;
  if (!guest_joystick_refs[slot].direct_opens)
    elfconv_runtime_error("SDL_JoystickClose called without a guest SDL_JoystickOpen.\n");
  SDL_JoystickClose(host);
  --guest_joystick_refs[slot].direct_opens;
  release_joystick_slot(slot);
}
IMPORT(SDL_JoystickInstanceID) {
  CALL; call.result(uint32_t(SDL_JoystickInstanceID(joystick(call.word(0)))));
}
IMPORT(SDL_JoystickGetDeviceGUID) {
  CALL;
  const uint32_t result = call.word(0);
  const SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(int(call.word(1)));
  memcpy(guest(arena, result, sizeof(guid)), &guid, sizeof(guid));
  call.return_argument_bytes = 4;
  call.result(result);
}
IMPORT(SDL_JoystickGetGUIDString) {
  CALL;
  SDL_JoystickGUID guid;
  memcpy(&guid, guest(arena, call.stack + 4, sizeof(guid)), sizeof(guid));
  const uint32_t address = call.word(4), length = call.word(5);
  SDL_JoystickGetGUIDString(guid, static_cast<char *>(guest(arena, address, length)),
                            int(length));
}
IMPORT(SDL_GameControllerOpen) {
  CALL;
  unsigned slot = 0; while (slot < 32 && guest_controllers[slot]) ++slot;
  if (slot == 32) elfconv_runtime_error("Guest SDL controller handle table exhausted.\n");
  SDL_GameController *p = SDL_GameControllerOpen(int(call.word(0)));
  if (!p) { call.result(0); return; }
  const int joystick_index = ensure_joystick_slot(SDL_GameControllerGetJoystick(p));
  ++guest_joystick_refs[joystick_index].controller_refs;
  guest_controllers[slot] = p; call.result(opaque_handle(slot, 0x300, 32));
}
IMPORT(SDL_GameControllerClose) {
  CALL;
  const uint32_t handle = call.word(0);
  if (handle) {
    SDL_GameController *controller = game_controller(handle);
    const int joystick_index = joystick_slot(SDL_GameControllerGetJoystick(controller));
    SDL_GameControllerClose(controller);
    guest_controllers[handle - 0x300] = nullptr;
    if (joystick_index >= 0) {
      --guest_joystick_refs[joystick_index].controller_refs;
      release_joystick_slot(unsigned(joystick_index));
    }
  }
}
IMPORT(SDL_GameControllerGetJoystick) {
  CALL;
  SDL_Joystick *host = SDL_GameControllerGetJoystick(game_controller(call.word(0)));
  if (!host) { call.result(0); return; }
  const unsigned slot = unsigned(ensure_joystick_slot(host));
  call.result(opaque_handle(slot, 0x900, 32));
}
IMPORT(SDL_HapticOpenFromJoystick) {
  CALL;
  unsigned slot = 0; while (slot < 32 && guest_haptics[slot]) ++slot;
  if (slot == 32) elfconv_runtime_error("Guest SDL haptic handle table exhausted.\n");
  SDL_Haptic *p = SDL_HapticOpenFromJoystick(joystick(call.word(0)));
  if (!p) { call.result(0); return; }
  guest_haptics[slot] = p; call.result(opaque_handle(slot, 0x400, 32));
}
IMPORT(SDL_SetHint) {
  CALL;
  call.result(SDL_SetHint(text(arena, call.word(0)), text(arena, call.word(1))));
}
IMPORT(SDL_setenv) {
  CALL;
  call.result(SDL_setenv(text(arena, call.word(0)), text(arena, call.word(1)),
                         int(call.word(2))));
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
IMPORT(SDL_GetClipboardText) {
  CALL;
  char *host = SDL_GetClipboardText();
  const uint32_t address = owned_string(arena, host);
  if (host) SDL_free(host);
  call.result(address);
}
IMPORT(SDL_SetClipboardText) {
  CALL;
  call.result(SDL_SetClipboardText(text(arena, call.word(0))));
}
IMPORT(SDL_HasClipboardText) { CALL; call.result(SDL_HasClipboardText()); }
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
  const auto *value = glGetString(call.word(0));
  const char *result = reinterpret_cast<const char *>(value);
#ifdef __EMSCRIPTEN__
  if (call.word(0) == GL_VERSION && result &&
      !strncmp(result, "OpenGL ES ", sizeof("OpenGL ES ") - 1))
    result += sizeof("OpenGL ES ") - 1;
  EM_ASM({ if (Module.elfconvTrace)
    Module.printErr('glGetString worker='+(typeof workerID==='number'?workerID:-1)+
      ' pc=0x'+$0.toString(16)+' name='+$1+' value='+($2?UTF8ToString($2):'null')); },
    state->gpr.rip.dword, call.word(0), uintptr_t(result));
#endif
  uint32_t address = copy_string(arena, result, slot);
#if defined(__EMSCRIPTEN__) && ECV_LEGACY_GL
  if (address && call.word(0) == GL_EXTENSIONS) {
    char *extensions = static_cast<char *>(
        guest(arena, address, string_capacity));
    const int webgl_extensions = EM_ASM_INT({
      var drawBuffers = Module.__ecvDrawBuffersIndexed;
      if (!drawBuffers) {
        drawBuffers = GLctx.getExtension('OES_draw_buffers_indexed');
        if (drawBuffers) Module.__ecvDrawBuffersIndexed = drawBuffers;
      }
      if (Module.__ecvWebGLSyncObjects === undefined) {
        Module.__ecvWebGLSyncObjects = !!(GLctx &&
          typeof GLctx.fenceSync === 'function' &&
          typeof GLctx.clientWaitSync === 'function' &&
          typeof GLctx.waitSync === 'function' &&
          typeof GLctx.deleteSync === 'function' &&
          typeof GLctx.getSyncParameter === 'function');
      }
      var srgbDecode = GLctx &&
        typeof GLctx.texParameteri === 'function' &&
        typeof GLctx.samplerParameteri === 'function';
      return (drawBuffers ? 1 : 0) | (Module.__ecvWebGLSyncObjects ? 2 : 0) |
             (srgbDecode ? 4 : 0);
    });
    if (webgl_extensions) {
      auto append_extension = [&](const char *suffix) {
        if (strstr(extensions, suffix + 1)) return;
        const size_t length = strlen(extensions), suffix_size = strlen(suffix) + 1;
        if (length > string_capacity - suffix_size)
          elfconv_runtime_error("OpenGL extension string exceeds guest capacity.\n");
        memcpy(extensions + length, suffix, suffix_size);
      };
      if (webgl_extensions & 1) append_extension(" GL_EXT_draw_buffers2");
      if (webgl_extensions & 2) append_extension(" GL_ARB_sync");
      if (webgl_extensions & 4) append_extension(" GL_EXT_texture_sRGB_decode");
    }
#if defined(__EMSCRIPTEN__)
    EM_ASM({ if (Module.elfconvTrace)
      Module.printErr('glGetString guest extensions='+UTF8ToString($0)); },
      uintptr_t(extensions));
#endif
  }
#endif
  call.result(address);
}
IMPORT(glGetBooleanv) {
  CALL;
  uint32_t address = call.word(1);
  if (address >= MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL boolean-query destination.\n");
  auto *params = static_cast<GLboolean *>(
      guest(arena, address, MEMORY_ARENA_SIZE - address));
  glGetBooleanv(call.word(0), params);
}
IMPORT(glGetDoublev) {
  CALL;
  uint32_t address = call.word(1);
  if (address >= MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL double-query destination.\n");
  auto *params = static_cast<GLdouble *>(
      guest(arena, address, MEMORY_ARENA_SIZE - address));
#ifdef __EMSCRIPTEN__
  GLfloat values[2];
  GLsizei count;
  switch (call.word(0)) {
    case GL_DEPTH_RANGE: count = 2; break;
    case GL_DEPTH_CLEAR_VALUE: count = 1; break;
    default: elfconv_runtime_error("Unsupported guest glGetDoublev query.\n");
  }
  glGetFloatv(call.word(0), values);
  for (GLsizei i = 0; i < count; ++i) params[i] = values[i];
#else
  glGetDoublev(call.word(0), params);
#endif
}
IMPORT(glGetFloatv) {
  CALL;
  uint32_t address = call.word(1);
  if (address >= MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL float-query destination.\n");
  auto *params = static_cast<GLfloat *>(
      guest(arena, address, MEMORY_ARENA_SIZE - address));
  glGetFloatv(call.word(0), params);
}
IMPORT(glGetCompressedTexImage) {
  CALL;
  elfconv_runtime_error("GL compressed-texture readback is unavailable in WebGL.\n");
}
IMPORT(glGetTexImage) {
  CALL;
#if defined(__EMSCRIPTEN__)
  const GLenum target = call.word(0), format = call.word(2), type = call.word(3);
  const GLint level = call.word(1);
  const GLenum binding = guest_gl_texture_binding_enum(target);
  if (!binding) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_ENUM;
    return;
  }
  GLint texture = 0;
  glGetIntegerv(binding, &texture);
  const SDL_GLContext context = SDL_GL_GetCurrentContext();
  auto context_levels = guest_gl_texture_levels.find(context);
  const GuestGLTextureLevel *image = nullptr;
  if (context_levels != guest_gl_texture_levels.end()) {
    auto found = context_levels->second.find({GLuint(texture), level});
    if (found != context_levels->second.end()) image = &found->second;
  }
  if (!texture || !image || target != GL_TEXTURE_2D) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_OPERATION;
    return;
  }
  const bool bgra = format == GL_BGRA;
  const uint32_t bytes_per_pixel =
      format == GL_RGB && type == GL_UNSIGNED_BYTE ? 3 :
      (format == GL_RGBA || bgra) &&
              (type == GL_UNSIGNED_BYTE ||
               type == GL_UNSIGNED_INT_8_8_8_8_REV) ? 4 : 0;
  if (!bytes_per_pixel) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_ENUM;
    return;
  }
  GLint alignment = pack_alignment, row_length = pack_row_length;
  GLint skip_rows = pack_skip_rows, skip_pixels = pack_skip_pixels;
  if ((alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8) ||
      row_length < 0 || skip_rows < 0 || skip_pixels < 0) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_VALUE;
    return;
  }
  const uint64_t width = uint64_t(image->width), height = uint64_t(image->height);
  const uint64_t row_pixels = row_length ? uint64_t(row_length) : width;
  const uint64_t row_bytes = row_pixels * bytes_per_pixel;
  const uint64_t stride = (row_bytes + alignment - 1) &
                          ~uint64_t(alignment - 1);
  const uint64_t rows_before_last = width && height
      ? uint64_t(skip_rows) + height - 1 : 0;
  const uint64_t last_row_bytes = width && height
      ? (uint64_t(skip_pixels) + width) * bytes_per_pixel : 0;
  if (rows_before_last && stride > MEMORY_ARENA_SIZE / rows_before_last) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_VALUE;
    return;
  }
  const uint64_t length = rows_before_last * stride + last_row_bytes;
  if (length > MEMORY_ARENA_SIZE || (length && !call.word(4))) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_VALUE;
    return;
  }
  auto *destination = static_cast<uint8_t *>(
      length ? guest(arena, call.word(4), length) : nullptr);
  GLuint &framebuffer = guest_gl_readback_framebuffers[context];
  if (!framebuffer) glGenFramebuffers(1, &framebuffer);
  if (!framebuffer) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_OUT_OF_MEMORY;
    return;
  }
  GLint previous_framebuffer = 0, previous_read_buffer = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_framebuffer);
  glGetIntegerv(GL_READ_BUFFER, &previous_read_buffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         target, GLuint(texture), level);
  if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) !=
      GL_FRAMEBUFFER_COMPLETE) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_framebuffer);
    glReadBuffer(previous_read_buffer);
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_OPERATION;
    return;
  }
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  uint8_t rgba[4096];
  for (uint32_t y = 0; y < image->height; ++y) {
    for (uint32_t x = 0; x < image->width;) {
      const uint32_t pixels = std::min(
          uint32_t(image->width) - x, uint32_t(sizeof(rgba) / 4));
      glReadPixels(x, y, pixels, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
      for (uint32_t i = 0; i < pixels; ++i) {
        uint8_t *out = destination +
            (uint64_t(skip_rows) + y) * stride +
            (uint64_t(skip_pixels) + x + i) * bytes_per_pixel;
        const uint8_t *in = rgba + i * 4;
        if (bgra) {
          out[0] = in[2]; out[1] = in[1]; out[2] = in[0]; out[3] = in[3];
        } else {
          memcpy(out, in, bytes_per_pixel);
        }
      }
      x += pixels;
    }
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_framebuffer);
  glReadBuffer(previous_read_buffer);
#else
  elfconv_runtime_error("GL texture readback is unsupported outside WebGL.\n");
#endif
}
IMPORT(glGetTexLevelParameteriv) {
  CALL;
#if defined(__EMSCRIPTEN__)
  if (!call.word(2))
    elfconv_runtime_error("Null guest GL texture-level query output.\n");
  auto *params = static_cast<GLint *>(guest(arena, call.word(2), sizeof(GLint)));
  const GLenum binding = guest_gl_texture_binding_enum(call.word(0));
  if (!binding) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_ENUM;
    *params = 0;
    return;
  }
  GLint texture = 0;
  glGetIntegerv(binding, &texture);
  auto context_levels = guest_gl_texture_levels.find(SDL_GL_GetCurrentContext());
  const GuestGLTextureLevel *image = nullptr;
  if (context_levels != guest_gl_texture_levels.end()) {
    auto found = context_levels->second.find({GLuint(texture), call.word(1)});
    if (found != context_levels->second.end()) image = &found->second;
  }
  if (!texture || !image) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_OPERATION;
    *params = 0;
    return;
  }
  switch (call.word(2)) {
    case GL_TEXTURE_WIDTH: *params = image->width; break;
    case GL_TEXTURE_HEIGHT: *params = image->height; break;
    case GL_TEXTURE_DEPTH: *params = image->depth; break;
    case GL_TEXTURE_INTERNAL_FORMAT: *params = image->internal_format; break;
    default:
      if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_ENUM;
      *params = 0;
      break;
  }
#else
  elfconv_runtime_error("GL texture-level queries are unsupported outside WebGL.\n");
#endif
}
IMPORT(glGetIntegerv) {
  CALL;
  auto *params = static_cast<GLint *>(guest(arena, call.word(1), sizeof(GLint)));
  glGetIntegerv(call.word(0), params);
}
IMPORT(glGetError) {
  CALL;
#ifdef __EMSCRIPTEN__
  GLenum result;
  if (guest_gl_error != GL_NO_ERROR) {
    result = guest_gl_error;
    guest_gl_error = GL_NO_ERROR;
  } else {
    result = glGetError();
  }
  EM_ASM({ if (Module.elfconvTrace)
    Module.printErr('glGetError worker='+(typeof workerID==='number'?workerID:-1)+
      ' pc=0x'+$0.toString(16)+' result=0x'+$1.toString(16)); },
    state->gpr.rip.dword, result);
  call.result(result);
#else
  call.result(glGetError());
#endif
}
#if ECV_LEGACY_GL
IMPORT(glAlphaFunc) { CALL; glAlphaFunc(call.word(0), call.real(1)); }
IMPORT(glBegin) { CALL; glBegin(call.word(0)); }
IMPORT(glEnd) { CALL; glEnd(); }
IMPORT(glBindProgramARB) { CALL; glBindProgramARB(call.word(0), call.word(1)); }
IMPORT(glDeleteObjectARB) {
  CALL;
#if defined(__EMSCRIPTEN__) && ECV_USE_GL4ES
  gl4es_glDeleteObject(call.word(0));
#else
  glDeleteObjectARB(call.word(0));
#endif
}
IMPORT(glGetInfoLogARB) {
  CALL;
  int32_t max_length = call.word(1);
  if (max_length < 0)
    elfconv_runtime_error("Negative guest GL info-log size.\n");
  auto *length = call.word(2)
      ? static_cast<GLsizei *>(guest(arena, call.word(2), sizeof(GLsizei)))
      : nullptr;
  auto *info_log = max_length && call.word(3)
      ? static_cast<GLchar *>(guest(arena, call.word(3), max_length))
      : nullptr;
#if defined(__EMSCRIPTEN__) && ECV_USE_GL4ES
  gl4es_glGetInfoLog(call.word(0), max_length, length,
                     reinterpret_cast<GLcharARB *>(info_log));
#else
  glGetInfoLogARB(call.word(0), max_length, length, info_log);
#endif
}
IMPORT(glGetObjectParameterivARB) {
  CALL;
  auto *params = static_cast<GLint *>(guest(arena, call.word(2), sizeof(GLint)));
#if defined(__EMSCRIPTEN__) && ECV_USE_GL4ES
  gl4es_glGetObjectParameteriv(call.word(0), call.word(1), params);
#else
  glGetObjectParameterivARB(call.word(0), call.word(1), params);
#endif
}
IMPORT(glMapBufferARB) {
  CALL;
#if defined(__EMSCRIPTEN__)
  const GLenum target = call.word(0), access = call.word(1);
  const GLenum binding_enum = guest_gl_buffer_binding_enum(target);
  if (!binding_enum) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_ENUM;
    call.result(0);
    return;
  }
  if (access != GL_READ_ONLY && access != GL_WRITE_ONLY &&
      access != GL_READ_WRITE) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_ENUM;
    call.result(0);
    return;
  }
  GLint buffer = 0, size = 0;
  glGetIntegerv(binding_enum, &buffer);
  if (!buffer) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_OPERATION;
    call.result(0);
    return;
  }
  glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
  EM_ASM({ if (Module.elfconvTrace)
    Module.printErr('glMapBufferARB pc=0x'+$0.toString(16)+
      ' target=0x'+$1.toString(16)+' access=0x'+$2.toString(16)+
      ' size='+$3); },
    state->gpr.rip.dword, target, access, size);
  if (size <= 0) {
    call.result(0);
    return;
  }
  GuestGLBufferMapping *available = nullptr;
  for (auto &mapping : guest_gl_buffer_mappings) {
    if (mapping.active && mapping.buffer == GLuint(buffer)) {
      if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_OPERATION;
      call.result(0);
      return;
    }
    if (!mapping.active) available = &mapping;
  }
  if (!available) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_OUT_OF_MEMORY;
    call.result(0);
    return;
  }
  const uint32_t address = allocate(uint32_t(size));
  if (!address) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_OUT_OF_MEMORY;
    call.result(0);
    return;
  }
  if (access != GL_WRITE_ONLY)
    glGetBufferSubData(target, 0, size, guest(arena, address, size));
  *available = {target, GLuint(buffer), address, size, access, true};
  call.result(address);
#else
  elfconv_runtime_error("GL buffer mapping is unsupported outside WebGL.\n");
  call.result(0);
#endif
}
IMPORT(glUnmapBuffer) {
  CALL;
#if defined(__EMSCRIPTEN__)
  const GLenum target = call.word(0);
  const GLenum binding_enum = guest_gl_buffer_binding_enum(target);
  if (!binding_enum) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_ENUM;
    call.result(GL_FALSE);
    return;
  }
  GLint buffer = 0;
  glGetIntegerv(binding_enum, &buffer);
  GuestGLBufferMapping *mapping = nullptr;
  for (auto &candidate : guest_gl_buffer_mappings)
    if (candidate.active && candidate.target == target) mapping = &candidate;
  if (!mapping || !buffer || mapping->buffer != GLuint(buffer)) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_OPERATION;
    call.result(GL_FALSE);
    return;
  }
  if (mapping->access != GL_READ_ONLY)
    glBufferSubData(target, 0, mapping->size,
                    guest(arena, mapping->address, mapping->size));
  release(mapping->address);
  *mapping = {};
  call.result(GL_TRUE);
#else
  elfconv_runtime_error("GL buffer mapping is unsupported outside WebGL.\n");
  call.result(GL_FALSE);
#endif
}
IMPORT(glOrtho) {
  CALL;
  glOrtho(call.real64(0), call.real64(1), call.real64(2), call.real64(3),
          call.real64(4), call.real64(5));
}
IMPORT(glPolygonMode) {
  CALL;
#ifdef __EMSCRIPTEN__
  if ((call.word(0) == GL_FRONT || call.word(0) == GL_BACK ||
       call.word(0) == GL_FRONT_AND_BACK) && call.word(1) == GL_FILL)
    return;
  elfconv_runtime_error("WebGL polygon mode supports filled polygons only.\n");
#else
  glPolygonMode(call.word(0), call.word(1));
#endif
}
IMPORT(glGenProgramsARB) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 ||
      uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL program count.\n");
  auto *programs = count
      ? static_cast<GLuint *>(
            guest(arena, call.word(1), size_t(count) * sizeof(GLuint)))
      : nullptr;
  glGenProgramsARB(count, programs);
}
IMPORT(glDeleteProgramsARB) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 ||
      uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL program count.\n");
  const auto *programs = count
      ? static_cast<const GLuint *>(
            guest(arena, call.word(1), size_t(count) * sizeof(GLuint)))
      : nullptr;
  glDeleteProgramsARB(count, programs);
}
IMPORT(glPushAttrib) {
  CALL;
  elfconv_runtime_error("GL attribute stacks are unsupported in WebGL.\n");
}
IMPORT(glPopAttrib) {
  CALL;
  elfconv_runtime_error("GL attribute stacks are unsupported in WebGL.\n");
}
IMPORT(glPushClientAttrib) {
  CALL;
#ifdef __EMSCRIPTEN__
  GLbitfield mask = call.word(0);
  constexpr GLbitfield supported_mask = GL_CLIENT_PIXEL_STORE_BIT |
                                        GL_CLIENT_VERTEX_ARRAY_BIT;
  if (mask == GL_CLIENT_ALL_ATTRIB_BITS) mask = supported_mask;
  if (mask & ~supported_mask) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_VALUE;
    return;
  }
  if (client_attrib_stack_size == client_attrib_stack_depth) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_STACK_OVERFLOW;
    return;
  }
  ClientAttribState &saved = client_attrib_stack[client_attrib_stack_size];
  saved.mask = mask;
  if (mask & GL_CLIENT_PIXEL_STORE_BIT) {
    saved.pack[0] = pack_alignment;
    saved.pack[1] = pack_row_length;
    saved.pack[2] = pack_skip_rows;
    saved.pack[3] = pack_skip_pixels;
    const GLenum pnames[] = {
        GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,
        GL_UNPACK_SKIP_PIXELS, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES};
    for (size_t i = 0; i < sizeof(pnames) / sizeof(pnames[0]); ++i)
      glGetIntegerv(pnames[i], &saved.unpack[i]);
  }
  if (mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
    saved.active_texture = client_active_texture;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved.vertex_array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved.array_buffer);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &saved.vertex_attrib_count);
    if (saved.vertex_attrib_count < 0 ||
        size_t(saved.vertex_attrib_count) > client_vertex_attrib_limit) {
      if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_OPERATION;
      return;
    }
    for (GLint i = 0; i < saved.vertex_attrib_count; ++i) {
      ClientVertexAttribState &attrib = saved.vertex_attribs[i];
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrib.enabled);
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attrib.size);
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attrib.type);
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attrib.normalized);
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attrib.stride);
      glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attrib.buffer);
      glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attrib.pointer);
    }
  }
  ++client_attrib_stack_size;
#else
  glPushClientAttrib(call.word(0));
#endif
}
IMPORT(glPopClientAttrib) {
  CALL;
#ifdef __EMSCRIPTEN__
  if (!client_attrib_stack_size) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_STACK_UNDERFLOW;
    return;
  }
  ClientAttribState &saved = client_attrib_stack[client_attrib_stack_size - 1];
  if (saved.mask & GL_CLIENT_PIXEL_STORE_BIT) {
    pack_alignment = saved.pack[0];
    pack_row_length = saved.pack[1];
    pack_skip_rows = saved.pack[2];
    pack_skip_pixels = saved.pack[3];
    const GLenum pnames[] = {
        GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,
        GL_UNPACK_SKIP_PIXELS, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES};
    for (size_t i = 0; i < sizeof(pnames) / sizeof(pnames[0]); ++i)
      glPixelStorei(pnames[i], saved.unpack[i]);
  }
  if (saved.mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
    glBindVertexArray(saved.vertex_array);
    glClientActiveTexture(saved.active_texture);
    client_active_texture = saved.active_texture;
    for (GLint i = 0; i < saved.vertex_attrib_count; ++i) {
      const ClientVertexAttribState &attrib = saved.vertex_attribs[i];
      glBindBuffer(GL_ARRAY_BUFFER, attrib.buffer);
      glVertexAttribPointer(i, attrib.size, attrib.type, attrib.normalized,
                            attrib.stride, attrib.pointer);
      if (attrib.enabled) glEnableVertexAttribArray(i);
      else glDisableVertexAttribArray(i);
    }
    glBindBuffer(GL_ARRAY_BUFFER, saved.array_buffer);
  }
  --client_attrib_stack_size;
#else
  glPopClientAttrib();
#endif
}
IMPORT(glProgramStringARB) {
  CALL;
  const int32_t length = call.word(2);
  if (length < 0)
    elfconv_runtime_error("Invalid guest GL program source length.\n");
  const auto *source = static_cast<const GLvoid *>(
      guest(arena, call.word(3), size_t(length)));
  glProgramStringARB(call.word(0), call.word(1), length, source);
}
IMPORT(glProgramEnvParameters4fvEXT) {
  CALL;
  const int32_t count = call.word(2);
  if (count < 0 ||
      uint64_t(count) * 4 * sizeof(GLfloat) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL environment parameter count.\n");
  const auto *params = count
      ? static_cast<const GLfloat *>(
            guest(arena, call.word(3), size_t(count) * 4 * sizeof(GLfloat)))
      : nullptr;
  glProgramEnvParameters4fvEXT(call.word(0), call.word(1), count, params);
}
#endif
IMPORT(glGenQueriesARB) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL query count.\n");
  auto *queries = count
      ? static_cast<GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glGenQueries(count, queries);
}
IMPORT(glDeleteQueriesARB) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL query count.\n");
  auto *queries = count
      ? static_cast<GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glDeleteQueries(count, queries);
}
IMPORT(glClear) { CALL; glClear(call.word(0)); }
IMPORT(glClearDepth) {
  CALL;
#ifdef __EMSCRIPTEN__
  glClearDepthf(GLfloat(call.real64(0)));
#else
  glClearDepth(call.real64(0));
#endif
}
IMPORT(glViewport) { CALL; glViewport(call.word(0), call.word(1), call.word(2), call.word(3)); }
IMPORT(glClearStencil) { CALL; glClearStencil(call.word(0)); }
IMPORT(glColorMask) {
  CALL;
  glColorMask(call.word(0), call.word(1), call.word(2), call.word(3));
}
IMPORT(glColorMaskIndexedEXT) {
  CALL;
#ifdef __EMSCRIPTEN__
  EM_ASM({
    var ext = Module.__ecvDrawBuffersIndexed;
    if (!ext) {
      ext = GLctx.getExtension('OES_draw_buffers_indexed');
      if (!ext) abort('OES_draw_buffers_indexed is unavailable');
      Module.__ecvDrawBuffersIndexed = ext;
    }
    ext.colorMaskiOES($0, !!$1, !!$2, !!$3, !!$4);
  }, call.word(0), call.word(1), call.word(2), call.word(3), call.word(4));
#else
  using Proc = void (*)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
  auto proc = reinterpret_cast<Proc>(SDL_GL_GetProcAddress("glColorMaskIndexedEXT"));
  if (!proc) elfconv_runtime_error("glColorMaskIndexedEXT is unavailable.\n");
  proc(call.word(0), call.word(1), call.word(2), call.word(3), call.word(4));
#endif
}
IMPORT(glEnableIndexedEXT) {
  CALL;
#ifdef __EMSCRIPTEN__
  EM_ASM({
    var ext = Module.__ecvDrawBuffersIndexed;
    if (!ext) {
      ext = GLctx.getExtension('OES_draw_buffers_indexed');
      if (!ext) abort('OES_draw_buffers_indexed is unavailable');
      Module.__ecvDrawBuffersIndexed = ext;
    }
    ext.enableiOES($0, $1);
  }, call.word(0), call.word(1));
#else
  using Proc = void (*)(GLenum, GLuint);
  auto proc = reinterpret_cast<Proc>(SDL_GL_GetProcAddress("glEnableIndexedEXT"));
  if (!proc) elfconv_runtime_error("glEnableIndexedEXT is unavailable.\n");
  proc(call.word(0), call.word(1));
#endif
}
IMPORT(glDisableIndexedEXT) {
  CALL;
#ifdef __EMSCRIPTEN__
  EM_ASM({
    var ext = Module.__ecvDrawBuffersIndexed;
    if (!ext) {
      ext = GLctx.getExtension('OES_draw_buffers_indexed');
      if (!ext) abort('OES_draw_buffers_indexed is unavailable');
      Module.__ecvDrawBuffersIndexed = ext;
    }
    ext.disableiOES($0, $1);
  }, call.word(0), call.word(1));
#else
  using Proc = void (*)(GLenum, GLuint);
  auto proc = reinterpret_cast<Proc>(SDL_GL_GetProcAddress("glDisableIndexedEXT"));
  if (!proc) elfconv_runtime_error("glDisableIndexedEXT is unavailable.\n");
  proc(call.word(0), call.word(1));
#endif
}
IMPORT(glGetBooleanIndexedvEXT) {
  CALL;
  const GLenum target = call.word(0);
  const GLuint index = call.word(1);
  const uint32_t address = call.word(2);
  if (!address) return;
  const size_t size = target == GL_COLOR_WRITEMASK ? 4 : 1;
  auto *params = static_cast<GLboolean *>(guest(arena, address, size));
#ifdef __EMSCRIPTEN__
  EM_ASM({
    var ext = Module.__ecvDrawBuffersIndexed;
    if (!ext) {
      ext = GLctx.getExtension('OES_draw_buffers_indexed');
      if (!ext) abort('OES_draw_buffers_indexed is unavailable');
      Module.__ecvDrawBuffersIndexed = ext;
    }
    if ($0 === 0x0be2) {
      HEAPU8[$2] = ext.isEnablediOES($0, $1) ? 1 : 0;
    } else if ($0 === 0x0c23) {
      var mask = GLctx.getIndexedParameter($0, $1);
      if (mask) {
        HEAPU8[$2] = mask[0] ? 1 : 0;
        HEAPU8[$2 + 1] = mask[1] ? 1 : 0;
        HEAPU8[$2 + 2] = mask[2] ? 1 : 0;
        HEAPU8[$2 + 3] = mask[3] ? 1 : 0;
      }
    } else {
      GLctx.getIndexedParameter($0, $1);
    }
  }, target, index, uintptr_t(params));
#else
  using Proc = void (*)(GLenum, GLuint, GLboolean *);
  auto proc = reinterpret_cast<Proc>(SDL_GL_GetProcAddress("glGetBooleanIndexedvEXT"));
  if (!proc) elfconv_runtime_error("glGetBooleanIndexedvEXT is unavailable.\n");
  proc(target, index, params);
#endif
}
IMPORT(glClipPlane) {
  CALL;
  const auto *equation = static_cast<const GLdouble *>(
      guest(arena, call.word(1), 4 * sizeof(GLdouble)));
  glClipPlane(call.word(0), equation);
}
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
IMPORT(glColor4f) { CALL; glColor4f(call.real(0), call.real(1), call.real(2), call.real(3)); }
IMPORT(glColor4sv) {
  CALL;
  const auto *color = static_cast<const GLshort *>(
      guest(arena, call.word(0), 4 * sizeof(GLshort)));
  glColor4f(std::max(-1.0f, GLfloat(color[0]) / 32767.0f),
            std::max(-1.0f, GLfloat(color[1]) / 32767.0f),
            std::max(-1.0f, GLfloat(color[2]) / 32767.0f),
            std::max(-1.0f, GLfloat(color[3]) / 32767.0f));
}
IMPORT(glTexCoord2f) { CALL; glTexCoord2f(call.real(0), call.real(1)); }
IMPORT(glEnableClientState) { CALL; glEnableClientState(call.word(0)); }
IMPORT(glDisableClientState) { CALL; glDisableClientState(call.word(0)); }
IMPORT(glClientActiveTexture) {
  CALL;
#ifdef __EMSCRIPTEN__
  client_active_texture = call.word(0);
#endif
  glClientActiveTexture(call.word(0));
}
IMPORT(glVertexPointer) {
  CALL;
  GLint buffer;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
  const void *pointer;
  if (buffer) {
    pointer = reinterpret_cast<const void *>(uintptr_t(call.word(3)));
  } else if (call.word(3)) {
    uint32_t address = call.word(3);
    if (address >= MEMORY_ARENA_SIZE)
      elfconv_runtime_error("Invalid guest GL vertex-array pointer.\n");
    pointer = guest(arena, address, MEMORY_ARENA_SIZE - address);
  } else {
    pointer = nullptr;
  }
  glVertexPointer(call.word(0), call.word(1), call.word(2), pointer);
}
IMPORT(glTexCoordPointer) {
  CALL;
  GLint buffer;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
  const void *pointer;
  if (buffer) {
    pointer = reinterpret_cast<const void *>(uintptr_t(call.word(3)));
  } else if (call.word(3)) {
    uint32_t address = call.word(3);
    if (address >= MEMORY_ARENA_SIZE)
      elfconv_runtime_error("Invalid guest GL texture-coordinate pointer.\n");
    pointer = guest(arena, address, MEMORY_ARENA_SIZE - address);
  } else {
    pointer = nullptr;
  }
  glTexCoordPointer(call.word(0), call.word(1), call.word(2), pointer);
}
IMPORT(glVertex3f) { CALL; glVertex3f(call.real(0), call.real(1), call.real(2)); }
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
IMPORT(glDetachShader) { CALL; glDetachShader(call.word(0), call.word(1)); }
IMPORT(glBindAttribLocation) { CALL; glBindAttribLocation(call.word(0), call.word(1), text(arena, call.word(2))); }
IMPORT(glLinkProgram) { CALL; glLinkProgram(call.word(0)); }
IMPORT(glUseProgram) { CALL; glUseProgram(call.word(0)); }
IMPORT(glDeleteProgram) { CALL; glDeleteProgram(call.word(0)); }
IMPORT(glGetUniformLocation) { CALL; call.result(glGetUniformLocation(call.word(0), text(arena, call.word(1)))); }
IMPORT(glEnable) { CALL; glEnable(call.word(0)); }
IMPORT(glIsEnabled) { CALL; call.result(glIsEnabled(call.word(0))); }
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
IMPORT(glBufferSubData) {
  CALL;
  int32_t offset = call.word(1);
  int32_t size = call.word(2);
  if (size < 0) elfconv_runtime_error("Negative guest GL buffer update size.\n");
  if (size && !call.word(3))
    elfconv_runtime_error("Null guest GL buffer update data.\n");
  const void *data = size ? guest(arena, call.word(3), size) : nullptr;
  glBufferSubData(call.word(0), offset, size, data);
}
IMPORT(glGenSamplers) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL sampler count.\n");
  auto *samplers = count
      ? static_cast<GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glGenSamplers(count, samplers);
}
IMPORT(glDeleteSamplers) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL sampler count.\n");
  auto *samplers = count
      ? static_cast<const GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glDeleteSamplers(count, samplers);
}
IMPORT(glBindSampler) {
  CALL;
  glBindSampler(call.word(0), call.word(1));
}
IMPORT(glSamplerParameteri) {
  CALL;
#ifdef __EMSCRIPTEN__
  if (call.word(1) == 0x8A48) {
    if (call.word(2) != 0x8A49 && call.word(2) != 0x8A4A) {
      if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_ENUM;
      return;
    }
    EM_ASM({
      var count = Module.__ecvSrgbDecodeTraceCount || 0;
      if (Module.elfconvTrace && count < 8) {
        Module.__ecvSrgbDecodeTraceCount = count + 1;
        Module.printErr('glSamplerParameteri sRGB decode sampler='+$0+' value=0x'+$1.toString(16));
      }
    }, call.word(0), call.word(2));
    return;
  }
#endif
  glSamplerParameteri(call.word(0), call.word(1), call.word(2));
}
IMPORT(glSamplerParameterf) {
  CALL;
  glSamplerParameterf(call.word(0), call.word(1), call.real(2));
}
IMPORT(glSamplerParameterfv) {
  CALL;
  const auto *params = static_cast<const GLfloat *>(
      guest(arena, call.word(2), sizeof(GLfloat)));
  glSamplerParameterfv(call.word(0), call.word(1), params);
}
IMPORT(glGenQueries) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL query count.\n");
  auto *queries = count
      ? static_cast<GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glGenQueries(count, queries);
}
IMPORT(glDeleteQueries) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL query count.\n");
  auto *queries = count
      ? static_cast<const GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glDeleteQueries(count, queries);
}
IMPORT(glBeginQuery) {
  CALL;
  glBeginQuery(call.word(0), call.word(1));
}
IMPORT(glEndQuery) {
  CALL;
  glEndQuery(call.word(0));
}
IMPORT(glFenceSync) {
  CALL;
  GLsync sync = glFenceSync(call.word(0), call.word(1));
  if (!sync) { call.result(0); return; }
  size_t slot = 0;
  while (slot < guest_gl_sync_limit && guest_gl_syncs[slot]) ++slot;
  if (slot == guest_gl_sync_limit) {
    glDeleteSync(sync);
#ifdef __EMSCRIPTEN__
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_OUT_OF_MEMORY;
#endif
    call.result(0);
    return;
  }
  guest_gl_syncs[slot] = sync;
  call.result(guest_gl_sync_base + uint32_t(slot));
}
IMPORT(glIsSync) {
  CALL;
  int slot = guest_gl_sync_slot(call.word(0));
  call.result(slot >= 0 && guest_gl_syncs[slot] &&
              glIsSync(guest_gl_syncs[slot]) ? GL_TRUE : GL_FALSE);
}
IMPORT(glDeleteSync) {
  CALL;
  int slot = guest_gl_sync_slot(call.word(0));
  if (slot < 0 || !guest_gl_syncs[slot]) {
#ifdef __EMSCRIPTEN__
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_VALUE;
#else
    glDeleteSync(nullptr);
#endif
    return;
  }
  glDeleteSync(guest_gl_syncs[slot]);
  guest_gl_syncs[slot] = nullptr;
}
IMPORT(glClientWaitSync) {
  CALL;
  int slot = guest_gl_sync_slot(call.word(0));
  if (slot < 0 || !guest_gl_syncs[slot]) {
#ifdef __EMSCRIPTEN__
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_VALUE;
#endif
    call.result(GL_WAIT_FAILED);
    return;
  }
  call.result(glClientWaitSync(guest_gl_syncs[slot], call.word(1), call.wide(2)));
}
IMPORT(glWaitSync) {
  CALL;
  int slot = guest_gl_sync_slot(call.word(0));
  if (slot < 0 || !guest_gl_syncs[slot]) {
#ifdef __EMSCRIPTEN__
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_VALUE;
#endif
    return;
  }
  glWaitSync(guest_gl_syncs[slot], call.word(1), call.wide(2));
}
IMPORT(glGetSynciv) {
  CALL;
  int slot = guest_gl_sync_slot(call.word(0));
  if (slot < 0 || !guest_gl_syncs[slot]) {
#ifdef __EMSCRIPTEN__
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_VALUE;
#endif
    return;
  }
  int32_t count = call.word(2);
  if (count < 0 || uint64_t(count) * sizeof(GLint) > MEMORY_ARENA_SIZE) {
#ifdef __EMSCRIPTEN__
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_VALUE;
#endif
    return;
  }
  auto *length = call.word(3)
      ? static_cast<GLsizei *>(guest(arena, call.word(3), sizeof(GLsizei)))
      : nullptr;
  auto *values = count && call.word(4)
      ? static_cast<GLint *>(guest(arena, call.word(4), count * sizeof(GLint)))
      : nullptr;
  glGetSynciv(guest_gl_syncs[slot], call.word(1), count, length, values);
}
IMPORT(glGetInteger64v) {
  CALL;
  GLint64 value = 0;
  glGetInteger64v(call.word(0), &value);
  auto *params = static_cast<uint8_t *>(guest(arena, call.word(1), sizeof(value)));
  std::memcpy(params, &value, sizeof(value));
}
IMPORT(glQueryCounter) {
  CALL;
#ifdef __EMSCRIPTEN__
  GLenum target = call.word(1);
  GLenum error = target == 0x8E28 ? GL_INVALID_OPERATION : GL_INVALID_ENUM;
  if (guest_gl_error == GL_NO_ERROR) guest_gl_error = error;
#else
  glQueryCounter(call.word(0), call.word(1));
#endif
}
IMPORT(glGetQueryObjectiv) {
  CALL;
  GLuint raw_value = 0;
  glGetQueryObjectuiv(call.word(0), call.word(1), &raw_value);
  GLint value = static_cast<GLint>(raw_value);
  auto *params = static_cast<uint8_t *>(guest(arena, call.word(2), sizeof(value)));
  std::memcpy(params, &value, sizeof(value));
}
IMPORT(glGetQueryObjectui64v) {
  CALL;
  GLuint value = 0;
  glGetQueryObjectuiv(call.word(0), call.word(1), &value);
  GLuint64 result = value;
  auto *params = static_cast<uint8_t *>(guest(arena, call.word(2), sizeof(result)));
  std::memcpy(params, &result, sizeof(result));
}
IMPORT(glCopyBufferSubData) {
  CALL;
  GLintptr read_offset = static_cast<int32_t>(call.word(2));
  GLintptr write_offset = static_cast<int32_t>(call.word(3));
  GLsizeiptr size = static_cast<int32_t>(call.word(4));
  glCopyBufferSubData(call.word(0), call.word(1), read_offset, write_offset, size);
}
IMPORT(glBlitFramebuffer) {
  CALL;
  glBlitFramebuffer(call.word(0), call.word(1), call.word(2), call.word(3), call.word(4),
                    call.word(5), call.word(6), call.word(7), call.word(8), call.word(9));
}
IMPORT(glBindFramebuffer) {
  CALL;
  glBindFramebuffer(call.word(0), call.word(1));
}
IMPORT(glGenFramebuffers) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL framebuffer count.\n");
  auto *framebuffers = count
      ? static_cast<GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glGenFramebuffers(count, framebuffers);
}
IMPORT(glDeleteFramebuffers) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL framebuffer count.\n");
  auto *framebuffers = count
      ? static_cast<const GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glDeleteFramebuffers(count, framebuffers);
}
IMPORT(glCheckFramebufferStatus) {
  CALL;
  const GLenum target = call.word(0);
  const GLenum status = glCheckFramebufferStatus(target);
#ifdef __EMSCRIPTEN__
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    uint32_t return_pc = 0;
    memcpy(&return_pc, guest(call.arena, call.stack, sizeof(return_pc)),
           sizeof(return_pc));
    EM_ASM({
      var count = Module.__ecvFramebufferIncompleteTraceCount || 0;
      if (Module.elfconvTrace && count < 16) {
        Module.__ecvFramebufferIncompleteTraceCount = count + 1;
        Module.printErr('glCheckFramebufferStatus target=0x' +
          $0.toString(16) + ' status=0x' + $1.toString(16) +
          ' ret=0x' + $2.toString(16));
      }
    }, target, status, return_pc);
  }
#endif
  call.result(status);
}
IMPORT(glFramebufferTexture2D) {
  CALL;
  glFramebufferTexture2D(call.word(0), call.word(1), call.word(2), call.word(3), call.word(4));
}
IMPORT(glFramebufferTextureLayer) {
  CALL;
  glFramebufferTextureLayer(call.word(0), call.word(1), call.word(2), call.word(3), call.word(4));
}
IMPORT(glFramebufferTexture3DEXT) {
  CALL;
  if (call.word(2) != GL_TEXTURE_3D) {
    if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_ENUM;
    return;
  }
  glFramebufferTextureLayer(call.word(0), call.word(1), call.word(3), call.word(4), call.word(5));
}
IMPORT(glFramebufferRenderbuffer) {
  CALL;
  glFramebufferRenderbuffer(call.word(0), call.word(1), call.word(2), call.word(3));
}
IMPORT(glBindRenderbuffer) {
  CALL;
  glBindRenderbuffer(call.word(0), call.word(1));
}
IMPORT(glGenRenderbuffers) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL renderbuffer count.\n");
  auto *renderbuffers = count
      ? static_cast<GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glGenRenderbuffers(count, renderbuffers);
}
IMPORT(glDeleteRenderbuffers) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL renderbuffer count.\n");
  auto *renderbuffers = count
      ? static_cast<const GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glDeleteRenderbuffers(count, renderbuffers);
}
IMPORT(glRenderbufferStorage) {
  CALL;
  glRenderbufferStorage(call.word(0), call.word(1), call.word(2), call.word(3));
}
IMPORT(glRenderbufferStorageMultisample) {
  CALL;
  glRenderbufferStorageMultisample(call.word(0), call.word(1), call.word(2), call.word(3),
                                   call.word(4));
}
IMPORT(glGetFramebufferAttachmentParameteriv) {
  CALL;
  auto *params = static_cast<GLint *>(guest(arena, call.word(3), sizeof(GLint)));
  glGetFramebufferAttachmentParameteriv(call.word(0), call.word(1), call.word(2), params);
}
IMPORT(glGetRenderbufferParameteriv) {
  CALL;
  auto *params = static_cast<GLint *>(guest(arena, call.word(2), sizeof(GLint)));
  glGetRenderbufferParameteriv(call.word(0), call.word(1), params);
}
IMPORT(glIsFramebuffer) {
  CALL;
  call.result(glIsFramebuffer(call.word(0)));
}
IMPORT(glIsRenderbuffer) {
  CALL;
  call.result(glIsRenderbuffer(call.word(0)));
}
IMPORT(glGenVertexArrays) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL vertex array count.\n");
  auto *arrays = count
      ? static_cast<GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glGenVertexArrays(count, arrays);
}
IMPORT(glDeleteVertexArrays) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL vertex array count.\n");
  auto *arrays = count
      ? static_cast<const GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glDeleteVertexArrays(count, arrays);
}
IMPORT(glBindVertexArray) {
  CALL;
  glBindVertexArray(call.word(0));
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
IMPORT(glUniform4fv) {
  CALL;
  int32_t count = call.word(1);
  if (count < 0 || uint64_t(count) * 4 * sizeof(GLfloat) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest vec4 uniform count.\n");
  auto *values = count
      ? static_cast<const GLfloat *>(guest(arena, call.word(2), count * 4 * sizeof(GLfloat)))
      : nullptr;
  glUniform4fv(call.word(0), count, values);
}
IMPORT(glDrawElements) {
  CALL;
  // The element-array buffer owns the indices; retain its byte offset.
  glDrawElements(call.word(0), call.word(1), call.word(2),
                 reinterpret_cast<const void *>(uintptr_t(call.word(3))));
}
IMPORT(glDrawArrays) { CALL; glDrawArrays(call.word(0), call.word(1), call.word(2)); }
IMPORT(glDrawBuffer) {
  CALL;
  GLenum buffer = call.word(0);
  glDrawBuffers(1, &buffer);
}
IMPORT(glDrawBuffers) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLenum) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL draw-buffer count.\n");
  auto *buffers = count
      ? static_cast<const GLenum *>(guest(arena, call.word(1), count * sizeof(GLenum)))
      : nullptr;
  glDrawBuffers(count, buffers);
}
IMPORT(glReadBuffer) { CALL; glReadBuffer(call.word(0)); }
IMPORT(glDrawRangeElements) {
  CALL;
  glDrawRangeElements(call.word(0), call.word(1), call.word(2), call.word(3),
                      call.word(4), reinterpret_cast<const void *>(uintptr_t(call.word(5))));
}
IMPORT(glDrawRangeElementsBaseVertex) {
  CALL;
  if (int32_t(call.word(6)) != 0)
    elfconv_runtime_error("Nonzero GL base-vertex draws are unsupported.\n");
  glDrawRangeElements(call.word(0), call.word(1), call.word(2), call.word(3),
                      call.word(4), reinterpret_cast<const void *>(uintptr_t(call.word(5))));
}
IMPORT(glFinish) { CALL; glFinish(); }
IMPORT(glFlush) { CALL; glFlush(); }
IMPORT(glFrontFace) { CALL; glFrontFace(call.word(0)); }
IMPORT(glGenTextures) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL texture count.\n");
  auto *textures = count ? static_cast<GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint))) : nullptr;
  glGenTextures(count, textures);
}
IMPORT(glDeleteTextures) {
  CALL;
  int32_t count = call.word(0);
  if (count < 0 || uint64_t(count) * sizeof(GLuint) > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL texture count.\n");
  auto *textures = count
      ? static_cast<const GLuint *>(guest(arena, call.word(1), count * sizeof(GLuint)))
      : nullptr;
  glDeleteTextures(count, textures);
  #if defined(__EMSCRIPTEN__)
  auto levels = guest_gl_texture_levels.find(SDL_GL_GetCurrentContext());
  if (levels != guest_gl_texture_levels.end()) {
    for (int32_t i = 0; i < count; ++i) {
      for (auto it = levels->second.begin(); it != levels->second.end();) {
        if (it->first.first == textures[i]) it = levels->second.erase(it);
        else ++it;
      }
    }
  }
  #endif
}
IMPORT(glBindTexture) { CALL; glBindTexture(call.word(0), call.word(1)); }
IMPORT(glIsTexture) { CALL; call.result(glIsTexture(call.word(0))); }
IMPORT(glTexImage2D) {
  CALL;
  int32_t width = call.word(3), height = call.word(4);
  if (width < 0 || height < 0)
    elfconv_runtime_error("Negative guest glTexImage2D extent.\n");
  uint32_t format = call.word(6), type = call.word(7);
  uint32_t host_format = format, host_type = type;
  GLint host_internal_format = static_cast<GLint>(call.word(2));
  bool convert_luminance = false, convert_bgra = false, convert_bgr = false;
  uint32_t bytes_per_pixel = 0;
#if defined(__EMSCRIPTEN__)
  if ((format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA) &&
      type == GL_UNSIGNED_BYTE &&
      guest_gl_luminance_internal_format(call.word(2), host_internal_format)) {
    bytes_per_pixel = format == GL_LUMINANCE ? 1 : 2;
    host_format = GL_RGBA;
    host_type = GL_UNSIGNED_BYTE;
    convert_luminance = true;
  } else if (format == GL_BGRA && type == GL_UNSIGNED_INT_8_8_8_8_REV) {
    bytes_per_pixel = 4;
    host_format = GL_RGBA;
    host_type = GL_UNSIGNED_BYTE;
    convert_bgra = true;
  } else if (format == GL_BGR && type == GL_UNSIGNED_BYTE) {
    bytes_per_pixel = 3;
    host_format = GL_RGB;
    host_type = GL_UNSIGNED_BYTE;
    convert_bgr = true;
  } else if (format == GL_RGBA && type == GL_UNSIGNED_INT_8_8_8_8_REV) {
    bytes_per_pixel = 4;
    host_type = GL_UNSIGNED_BYTE;
  } else if (format == GL_RGBA && type == GL_HALF_FLOAT) {
    bytes_per_pixel = 8;
  } else
#endif
  if (type == GL_UNSIGNED_BYTE && format == GL_RGBA) bytes_per_pixel = 4;
  else if (format == GL_DEPTH_STENCIL && type == GL_UNSIGNED_INT_24_8)
    bytes_per_pixel = 4;
  else if (type == GL_UNSIGNED_BYTE && format == GL_RGB) bytes_per_pixel = 3;
  if (!bytes_per_pixel) {
#if defined(__EMSCRIPTEN__)
    EM_ASM({ if (Module.elfconvTrace)
      Module.printErr('glTexImage2D unsupported pc=0x'+$0.toString(16)+
        ' target=0x'+$1.toString(16)+' level='+$2+' internal=0x'+
        $3.toString(16)+' size='+$4+'x'+$5+' format=0x'+$6.toString(16)+
        ' type=0x'+$7.toString(16)); },
      state->gpr.rip.dword, call.word(0), call.word(1), call.word(2),
      width, height, format, type);
#endif
    elfconv_runtime_error("Unsupported guest glTexImage2D format or type.\n");
  }
  GLint alignment, row_length = 0, skip_rows = 0, skip_pixels = 0;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
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
  if (convert_luminance)
    pixels = guest_gl_luminance_to_rgba(
        pixels, alignment, row_length, skip_rows, skip_pixels, width, height,
        bytes_per_pixel);
  else if (convert_bgra || convert_bgr)
    pixels = guest_gl_bgr_to_rgb(pixels, length, width, height, alignment,
                                 row_length, skip_rows, skip_pixels,
                                 bytes_per_pixel);
  glTexImage2D(call.word(0), call.word(1), host_internal_format, width, height,
               call.word(5), host_format, host_type, pixels);
  #if defined(__EMSCRIPTEN__)
  guest_gl_record_texture_level(call.word(0), call.word(1), call.word(2),
                                width, height, 1);
  #endif
}
IMPORT(glTexSubImage2D) {
  CALL;
  int32_t width = call.word(4), height = call.word(5);
  if (width < 0 || height < 0)
    elfconv_runtime_error("Negative guest glTexSubImage2D extent.\n");
  uint32_t format = call.word(6), type = call.word(7);
  uint32_t host_format = format, host_type = type;
  bool convert_luminance = false, convert_bgra = false, convert_bgr = false;
  uint32_t bytes_per_pixel = 0;
#if defined(__EMSCRIPTEN__)
  if ((format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA) &&
      type == GL_UNSIGNED_BYTE) {
    bytes_per_pixel = format == GL_LUMINANCE ? 1 : 2;
    host_format = GL_RGBA;
    host_type = GL_UNSIGNED_BYTE;
    convert_luminance = true;
  } else if (format == GL_BGRA && type == GL_UNSIGNED_INT_8_8_8_8_REV) {
    bytes_per_pixel = 4;
    host_format = GL_RGBA;
    host_type = GL_UNSIGNED_BYTE;
    convert_bgra = true;
  } else if (format == GL_BGR && type == GL_UNSIGNED_BYTE) {
    bytes_per_pixel = 3;
    host_format = GL_RGB;
    host_type = GL_UNSIGNED_BYTE;
    convert_bgr = true;
  } else if (format == GL_RGBA && type == GL_UNSIGNED_INT_8_8_8_8_REV) {
    bytes_per_pixel = 4;
    host_type = GL_UNSIGNED_BYTE;
  } else if (format == GL_RGBA && type == GL_HALF_FLOAT) {
    bytes_per_pixel = 8;
  } else
#endif
  if (type == GL_UNSIGNED_BYTE && format == GL_RGBA) bytes_per_pixel = 4;
  else if (format == GL_DEPTH_STENCIL && type == GL_UNSIGNED_INT_24_8)
    bytes_per_pixel = 4;
  else if (type == GL_UNSIGNED_BYTE && format == GL_RGB) bytes_per_pixel = 3;
  if (!bytes_per_pixel)
    elfconv_runtime_error("Unsupported guest glTexSubImage2D format or type.\n");
  GLint alignment, row_length = 0, skip_rows = 0, skip_pixels = 0;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
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
    elfconv_runtime_error("Guest glTexSubImage2D buffer overflow.\n");
  uint64_t length = rows_before_last * stride + last_row_bytes;
  if (length > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Guest glTexSubImage2D buffer overflow.\n");
  const void *pixels = call.word(8) ? guest(arena, call.word(8), length) : nullptr;
  if (convert_luminance)
    pixels = guest_gl_luminance_to_rgba(
        pixels, alignment, row_length, skip_rows, skip_pixels, width, height,
        bytes_per_pixel);
  else if (convert_bgra || convert_bgr)
    pixels = guest_gl_bgr_to_rgb(pixels, length, width, height, alignment,
                                 row_length, skip_rows, skip_pixels,
                                 bytes_per_pixel);
  glTexSubImage2D(call.word(0), call.word(1), call.word(2), call.word(3),
                  width, height, host_format, host_type, pixels);
}
IMPORT(glTexImage3D) {
  CALL;
  int32_t width = call.word(3), height = call.word(4), depth = call.word(5);
  if (width < 0 || height < 0 || depth < 0)
    elfconv_runtime_error("Negative guest glTexImage3D extent.\n");
  uint32_t components;
  switch (call.word(7)) {
    case GL_RED: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT:
      components = 1; break;
    case GL_RG: case GL_LUMINANCE_ALPHA: components = 2; break;
    case GL_RGB: components = 3; break;
    case GL_RGBA: components = 4; break;
    default: elfconv_runtime_error("Unsupported guest glTexImage3D format.\n");
  }
  uint32_t bytes_per_component;
  switch (call.word(8)) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: bytes_per_component = 1; break;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT:
      bytes_per_component = 2; break;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT:
      bytes_per_component = 4; break;
    default: elfconv_runtime_error("Unsupported guest glTexImage3D type.\n");
  }
  uint64_t bytes_per_pixel = uint64_t(components) * bytes_per_component;
  GLint alignment, row_length, image_height, skip_pixels, skip_rows, skip_images;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
  glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &image_height);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
  glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_images);
  if ((alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8) ||
      row_length < 0 || image_height < 0 || skip_pixels < 0 ||
      skip_rows < 0 || skip_images < 0)
    elfconv_runtime_error("Invalid guest GL unpack state.\n");
  uint64_t length = 0;
  if (width && height && depth) {
    uint64_t row_pixels = row_length ? uint64_t(row_length) : uint64_t(width);
    if (row_pixels > MEMORY_ARENA_SIZE / bytes_per_pixel)
      elfconv_runtime_error("Guest glTexImage3D row exceeds guest memory.\n");
    uint64_t row_bytes = row_pixels * bytes_per_pixel;
    if (row_bytes > MEMORY_ARENA_SIZE - uint64_t(alignment - 1))
      elfconv_runtime_error("Guest glTexImage3D row exceeds guest memory.\n");
    uint64_t stride = (row_bytes + alignment - 1) & ~uint64_t(alignment - 1);
    uint64_t rows_per_image = image_height ? uint64_t(image_height) : uint64_t(height);
    uint64_t last_row = (uint64_t(skip_images) + uint64_t(depth) - 1) *
                        rows_per_image + uint64_t(skip_rows) + uint64_t(height) - 1;
    uint64_t last_row_bytes = (uint64_t(skip_pixels) + uint64_t(width)) * bytes_per_pixel;
    if (last_row && stride > MEMORY_ARENA_SIZE / last_row)
      elfconv_runtime_error("Guest glTexImage3D buffer overflow.\n");
    uint64_t preceding_rows = last_row * stride;
    if (last_row_bytes > MEMORY_ARENA_SIZE - preceding_rows)
      elfconv_runtime_error("Guest glTexImage3D buffer overflow.\n");
    length = preceding_rows + last_row_bytes;
  }
  const void *pixels = call.word(9) && length
      ? guest(arena, call.word(9), length) : nullptr;
  glTexImage3D(call.word(0), call.word(1), call.word(2), width, height, depth,
               call.word(6), call.word(7), call.word(8), pixels);
}
IMPORT(glCompressedTexImage2D) {
  CALL;
  int32_t image_size = call.word(6);
  if (image_size < 0)
    elfconv_runtime_error("Negative guest compressed texture size.\n");
  const void *data = call.word(7) ? guest(arena, call.word(7), image_size) : nullptr;
  glCompressedTexImage2D(call.word(0), call.word(1), call.word(2), call.word(3),
                         call.word(4), call.word(5), image_size, data);
}
IMPORT(glCompressedTexImage3D) {
  CALL;
  int32_t image_size = call.word(7);
  if (image_size < 0)
    elfconv_runtime_error("Negative guest compressed texture size.\n");
  const void *data = call.word(8) ? guest(arena, call.word(8), image_size) : nullptr;
  glCompressedTexImage3D(call.word(0), call.word(1), call.word(2), call.word(3),
                         call.word(4), call.word(5), call.word(6), image_size, data);
}
IMPORT(glTexParameteri) {
  CALL;
#ifdef __EMSCRIPTEN__
  if (call.word(1) == 0x8A48) {
    if (call.word(2) != 0x8A49 && call.word(2) != 0x8A4A) {
      if (guest_gl_error == GL_NO_ERROR) guest_gl_error = GL_INVALID_ENUM;
      return;
    }
    EM_ASM({
      var count = Module.__ecvSrgbDecodeTraceCount || 0;
      if (Module.elfconvTrace && count < 8) {
        Module.__ecvSrgbDecodeTraceCount = count + 1;
        Module.printErr('glTexParameteri sRGB decode target=0x'+$0.toString(16)+
          ' value=0x'+$1.toString(16));
      }
    }, call.word(0), call.word(2));
    return;
  }
#endif
  glTexParameteri(call.word(0), call.word(1), call.word(2));
}
IMPORT(glTexParameterfv) {
  CALL;
  uint32_t address = call.word(2);
  if (address >= MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Invalid guest GL texture-parameter pointer.\n");
  auto *params = static_cast<const GLfloat *>(
      guest(arena, address, MEMORY_ARENA_SIZE - address));
  glTexParameterfv(call.word(0), call.word(1), params);
}
IMPORT(glTexParameterf) { CALL; glTexParameterf(call.word(0), call.word(1), call.real(2)); }
IMPORT(glDepthFunc) { CALL; glDepthFunc(call.word(0)); }
IMPORT(glDepthMask) { CALL; glDepthMask(call.word(0)); }
IMPORT(glDepthRange) {
  CALL;
#ifdef __EMSCRIPTEN__
  glDepthRangef(GLfloat(call.real64(0)), GLfloat(call.real64(1)));
#else
  glDepthRange(call.real64(0), call.real64(1));
#endif
}
IMPORT(glBlendFunc) { CALL; glBlendFunc(call.word(0), call.word(1)); }
IMPORT(glBlendColor) { CALL; glBlendColor(call.real(0), call.real(1), call.real(2), call.real(3)); }
IMPORT(glBlendEquation) { CALL; glBlendEquation(call.word(0)); }
IMPORT(glScissor) { CALL; glScissor(call.word(0), call.word(1), call.word(2), call.word(3)); }
IMPORT(glStencilFunc) { CALL; glStencilFunc(call.word(0), call.word(1), call.word(2)); }
IMPORT(glStencilMask) { CALL; glStencilMask(call.word(0)); }
IMPORT(glStencilOp) { CALL; glStencilOp(call.word(0), call.word(1), call.word(2)); }
IMPORT(glStencilFuncSeparate) {
  CALL;
  glStencilFuncSeparate(call.word(0), call.word(1), call.word(2), call.word(3));
}
IMPORT(glStencilOpSeparate) {
  CALL;
  glStencilOpSeparate(call.word(0), call.word(1), call.word(2), call.word(3));
}
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

// Locale, iconv, gettext, and wide-character imports. Included after the
// core adapter so guest-memory and CALL helpers are available.
#include <iconv.h>
#include <langinfo.h>
#include <libintl.h>
#include <locale.h>
#include <cwchar>
#include <cwctype>
#include <unordered_map>

namespace {
std::mutex locale_handles_mutex;
std::unordered_map<uint32_t, locale_t> locale_handles;
std::unordered_map<locale_t, uint32_t> locale_reverse_handles;
uint32_t next_locale_handle = 0x400;
std::mutex iconv_handles_mutex;
std::unordered_map<uint32_t, iconv_t> iconv_handles;
uint32_t next_iconv_handle = 0x500;
std::mutex wctype_handles_mutex;
std::unordered_map<uint32_t, wctype_t> wctype_handles;
std::unordered_map<wctype_t, uint32_t> wctype_reverse_handles;
uint32_t next_wctype_handle = 0x600;
std::mutex mbstate_mutex;
std::unordered_map<uint64_t, std::string> mbstate_values;
std::unordered_map<std::string, uint64_t> mbstate_reverse_values;
uint64_t next_mbstate_handle = 1;

uint32_t remember_locale(locale_t value) {
  if (!value || value == LC_GLOBAL_LOCALE) return UINT32_MAX;
  std::lock_guard<std::mutex> lock(locale_handles_mutex);
  auto found = locale_reverse_handles.find(value);
  if (found != locale_reverse_handles.end()) return found->second;
  if (next_locale_handle == UINT32_MAX) { errno = EMFILE; return 0; }
  const uint32_t handle = next_locale_handle++;
  locale_handles.emplace(handle, value);
  locale_reverse_handles.emplace(value, handle);
  return handle;
}
locale_t guest_locale(uint32_t handle) {
  if (handle == UINT32_MAX) return LC_GLOBAL_LOCALE;
  std::lock_guard<std::mutex> lock(locale_handles_mutex);
  auto found = locale_handles.find(handle);
  if (found == locale_handles.end()) { errno = EINVAL; return nullptr; }
  return found->second;
}
int host_locale_category(int guest_category) {
  switch (guest_category) {
    case 0: return LC_CTYPE;
    case 1: return LC_NUMERIC;
    case 2: return LC_TIME;
    case 3: return LC_COLLATE;
    case 4: return LC_MONETARY;
    case 5: return LC_MESSAGES;
    case 6: return LC_ALL;
    default: errno = EINVAL; return -1;
  }
}
int host_locale_mask(uint32_t guest_mask) {
  constexpr uint32_t guest_all_mask = 0x1fbf;  // glibc excludes the LC_ALL category bit.
  constexpr uint32_t guest_core_mask = 0x003f;
  // glibc/locale/newlocale.c accepts the LC_ALL category bit alone too.
  if (guest_mask == (1u << 6)) guest_mask = guest_all_mask;
  if (guest_mask & ~guest_core_mask) {
    if (guest_mask != guest_all_mask) { errno = EINVAL; return -1; }
    guest_mask = guest_core_mask;
  }
  constexpr int host_categories[] = {LC_CTYPE, LC_NUMERIC, LC_TIME, LC_COLLATE, LC_MONETARY, LC_MESSAGES};
  int mask = 0;
  for (unsigned i = 0; i < 6; ++i)
    if (guest_mask & (1u << i)) mask |= 1u << host_categories[i];
  return mask;
}
locale_t make_locale(uint32_t mask, const char *name, uint32_t base_handle) {
  const int host_mask = host_locale_mask(mask);
  if (host_mask < 0) return nullptr;
  locale_t base = nullptr;
  if (base_handle) {
    base = guest_locale(base_handle);
    if (!base) return nullptr;
  }
  locale_t result = newlocale(host_mask, name, base);
  if (result && base_handle) {
    std::lock_guard<std::mutex> lock(locale_handles_mutex);
    auto it = locale_handles.find(base_handle);
    if (it != locale_handles.end()) {
      locale_reverse_handles.erase(it->second);
      locale_handles.erase(it);
    }
  }
  return result;
}
uint32_t remember_wctype(wctype_t value) {
  if (!value) return 0;
  std::lock_guard<std::mutex> lock(wctype_handles_mutex);
  auto found = wctype_reverse_handles.find(value);
  if (found != wctype_reverse_handles.end()) return found->second;
  if (next_wctype_handle == UINT32_MAX) { errno = EMFILE; return 0; }
  const uint32_t handle = next_wctype_handle++;
  wctype_handles.emplace(handle, value);
  wctype_reverse_handles.emplace(value, handle);
  return handle;
}
wctype_t guest_wctype(uint32_t handle) {
  if (!handle) return wctype_t(0);
  std::lock_guard<std::mutex> lock(wctype_handles_mutex);
  auto found = wctype_handles.find(handle);
  if (found == wctype_handles.end()) { errno = EINVAL; return wctype_t(0); }
  return found->second;
}
uint32_t iconv_handle(iconv_t value) {
  std::lock_guard<std::mutex> lock(iconv_handles_mutex);
  if (next_iconv_handle == UINT32_MAX) { errno = EMFILE; return 0; }
  const uint32_t handle = next_iconv_handle++;
  iconv_handles.emplace(handle, value);
  return handle;
}
iconv_t guest_iconv(uint32_t handle) {
  std::lock_guard<std::mutex> lock(iconv_handles_mutex);
  auto found = iconv_handles.find(handle);
  if (found == iconv_handles.end()) { errno = EBADF; return (iconv_t)-1; }
  return found->second;
}
constexpr size_t guest_mbstate_size = 8;
mbstate_t load_mbstate(uint8_t *arena, uint32_t address) {
  mbstate_t state{};
  if (!address) return state;
  uint64_t handle = 0;
  const auto *bytes = static_cast<const uint8_t *>(guest(arena, address, guest_mbstate_size));
  for (unsigned i = 0; i < 8; ++i) handle |= uint64_t(bytes[i]) << (8 * i);
  if (!handle) return state;
  std::lock_guard<std::mutex> lock(mbstate_mutex);
  auto it = mbstate_values.find(handle);
  if (it == mbstate_values.end()) { errno = EINVAL; return state; }
  memcpy(&state, it->second.data(), sizeof(state));
  return state;
}
void save_mbstate(uint8_t *arena, uint32_t address, const mbstate_t &state) {
  if (!address) return;
  uint8_t bytes[guest_mbstate_size] = {};
  const auto *host_bytes = reinterpret_cast<const uint8_t *>(&state);
  bool initial = true;
  for (size_t i = 0; i < sizeof(state); ++i) initial &= host_bytes[i] == 0;
  uint64_t handle = 0;
  if (!initial) {
    std::string key(reinterpret_cast<const char *>(&state), sizeof(state));
    std::lock_guard<std::mutex> lock(mbstate_mutex);
    auto found = mbstate_reverse_values.find(key);
    if (found != mbstate_reverse_values.end()) handle = found->second;
    else {
      if (!next_mbstate_handle) { errno = EOVERFLOW; return; }
      handle = next_mbstate_handle++;
      mbstate_values.emplace(handle, key);
      mbstate_reverse_values.emplace(key, handle);
    }
  }
  for (unsigned i = 0; i < 8; ++i) bytes[i] = uint8_t(handle >> (8 * i));
  memcpy(guest(arena, address, guest_mbstate_size), bytes, guest_mbstate_size);
}
// The eight-byte guest mbstate_t is opaque. Guest storage carries an explicit
// little-endian token into an intern table of host-libc states, so musl/glibc
// internal layouts never cross the ABI boundary.
mbstate_t *implicit_mbstate(unsigned operation) {
  static thread_local mbstate_t states[5]{};
  return &states[operation];
}
bool host_langinfo_item(uint32_t guest_item, nl_item &host_item) {
  if (guest_item == 14) { host_item = CODESET; return true; }
  if (guest_item >= 0x20000 && guest_item <= 0x20031) {
    static const nl_item items[] = {
      ABDAY_1, ABDAY_2, ABDAY_3, ABDAY_4, ABDAY_5, ABDAY_6, ABDAY_7,
      DAY_1, DAY_2, DAY_3, DAY_4, DAY_5, DAY_6, DAY_7,
      ABMON_1, ABMON_2, ABMON_3, ABMON_4, ABMON_5, ABMON_6,
      ABMON_7, ABMON_8, ABMON_9, ABMON_10, ABMON_11, ABMON_12,
      MON_1, MON_2, MON_3, MON_4, MON_5, MON_6,
      MON_7, MON_8, MON_9, MON_10, MON_11, MON_12,
      AM_STR, PM_STR, D_T_FMT, D_FMT, T_FMT, T_FMT_AMPM,
      ERA, nl_item(0), ERA_D_FMT, ALT_DIGITS, ERA_D_T_FMT, ERA_T_FMT
    };
    const uint32_t index = guest_item - 0x20000;
    if (index == 45) return false;
    host_item = items[index];
    return true;
  }
  if ((guest_item & 0xffff) == 0xffff) {
    const int category = host_locale_category(int(guest_item >> 16));
    if (category < 0) return false;
#ifdef NL_LOCALE_NAME
    host_item = NL_LOCALE_NAME(category);
#else
    host_item = nl_item((uint32_t(category) << 16) | 0xffff);
#endif
    return true;
  }
  switch (guest_item) {
    case 0x10000: host_item = RADIXCHAR; return true;
    case 0x10001: host_item = THOUSEP; return true;
    case 0x4000f: host_item = CRNCYSTR; return true;
    case 0x50000: host_item = YESEXPR; return true;
    case 0x50001: host_item = NOEXPR; return true;
#ifdef YESSTR
    case 0x50002: host_item = YESSTR; return true;
    case 0x50003: host_item = NOSTR; return true;
#endif
    default: return false;
  }
}
uint32_t wide_guest_pointer(uint8_t *arena, uint32_t base, const wchar_t *p) {
  return p ? base + uint32_t(reinterpret_cast<const uint8_t *>(p) -
      static_cast<const uint8_t *>(guest(arena, base, 1))) : 0;
}
// i386 glibc lconv: ten 32-bit pointers, fourteen chars, then two pad bytes.
// Serialize guest pointers explicitly; host pointers must never escape.
constexpr uint32_t guest_lconv_size = 56;
constexpr uint32_t guest_lconv_pointer_count = 10;
constexpr uint32_t guest_lconv_pointer_bytes = guest_lconv_pointer_count * 4;

struct GuestLocaleconvStorage {
  uint32_t address = 0;
  uint32_t capacity = 0;
};
uint32_t serialize_localeconv(uint8_t *arena, const struct lconv *value) {
  if (!value) return 0;
  const char *strings[] = {
    value->decimal_point, value->thousands_sep, value->grouping,
    value->int_curr_symbol, value->currency_symbol,
    value->mon_decimal_point, value->mon_thousands_sep, value->mon_grouping,
    value->positive_sign, value->negative_sign,
  };
  for (auto &string : strings) if (!string) string = "";
  const char fields[] = {
    value->int_frac_digits, value->frac_digits, value->p_cs_precedes,
    value->p_sep_by_space, value->n_cs_precedes, value->n_sep_by_space,
    value->p_sign_posn, value->n_sign_posn, value->int_p_cs_precedes,
    value->int_p_sep_by_space, value->int_n_cs_precedes,
    value->int_n_sep_by_space, value->int_p_sign_posn,
    value->int_n_sign_posn,
  };
  size_t lengths[guest_lconv_pointer_count];
  size_t required = guest_lconv_size;
  for (size_t i = 0; i < guest_lconv_pointer_count; ++i) {
    lengths[i] = strlen(strings[i]) + 1;
    if (lengths[i] > UINT32_MAX - required) { errno = ENOMEM; return 0; }
    required += lengths[i];
  }
  static thread_local GuestLocaleconvStorage storage;
  if (required > storage.capacity) {
    const uint32_t address = allocate(uint32_t(required));
    if (!address) return 0;
    if (storage.address) release(storage.address);
    storage.address = address;
    storage.capacity = uint32_t(required);
  }
  auto *out = static_cast<uint8_t *>(guest(arena, storage.address, required));
  memset(out, 0, guest_lconv_size);
  size_t cursor = guest_lconv_size;
  for (size_t i = 0; i < guest_lconv_pointer_count; ++i) {
    const uint32_t address = storage.address + uint32_t(cursor);
    memcpy(out + i * sizeof(uint32_t), &address, sizeof(address));
    memcpy(out + cursor, strings[i], lengths[i]);
    cursor += lengths[i];
  }
  memcpy(out + guest_lconv_pointer_bytes, fields, sizeof(fields));
  return storage.address;
}
}

IMPORT(setlocale) {
  CALL;
  const int category = host_locale_category(int32_t(call.word(0)));
  const char *locale = call.word(1) ? text(arena, call.word(1)) : nullptr;
  call.result(copy_string(arena, category < 0 ? nullptr : setlocale(category, locale), 10));
}
IMPORT(localeconv) {
  CALL;
  call.result(serialize_localeconv(arena, ::localeconv()));
}
IMPORT(__newlocale) {
  CALL;
  locale_t value = make_locale(call.word(0), text(arena, call.word(1)), call.word(2));
  call.result(value ? remember_locale(value) : 0);
}
IMPORT(__duplocale) {
  CALL;
  locale_t value = guest_locale(call.word(0));
  if (!value) { call.result(0); return; }
  locale_t copy = duplocale(value);
  call.result(copy ? remember_locale(copy) : 0);
}
IMPORT(__freelocale) {
  CALL;
  const uint32_t handle = call.word(0);
  if (!handle) { errno = EINVAL; return; }
  locale_t value = guest_locale(handle);
  if (!value || value == LC_GLOBAL_LOCALE) { if (value == LC_GLOBAL_LOCALE) errno = EINVAL; return; }
  {
    std::lock_guard<std::mutex> lock(locale_handles_mutex);
    locale_handles.erase(handle);
    locale_reverse_handles.erase(value);
  }
  freelocale(value);
}
IMPORT(__uselocale) {
  CALL;
  const uint32_t handle = call.word(0);
  locale_t value = handle ? guest_locale(handle) : nullptr;
  if (handle && !value) { call.result(0); return; }
  locale_t previous = uselocale(value);
  call.result(remember_locale(previous));
}
IMPORT(__ctype_get_mb_cur_max) { CALL; call.result(MB_CUR_MAX); }
IMPORT(__nl_langinfo_l) {
  CALL;
  locale_t locale = guest_locale(call.word(1));
  nl_item item;
  if (!host_langinfo_item(call.word(0), item)) { errno = EINVAL; call.result(0); return; }
  call.result(locale ? copy_string(arena, nl_langinfo_l(item, locale), 11) : 0);
}
IMPORT(bindtextdomain) {
  CALL;
  char *result = bindtextdomain(text(arena, call.word(0)), call.word(1) ? const_cast<char *>(text(arena, call.word(1))) : nullptr);
  call.result(copy_string(arena, result, 12));
}
IMPORT(bind_textdomain_codeset) {
  CALL;
  char *result = bind_textdomain_codeset(text(arena, call.word(0)), call.word(1) ? const_cast<char *>(text(arena, call.word(1))) : nullptr);
  call.result(copy_string(arena, result, 13));
}
IMPORT(gettext) { CALL; call.result(copy_string(arena, gettext(text(arena, call.word(0))), 14)); }
IMPORT(textdomain) { CALL; call.result(copy_string(arena, textdomain(call.word(0) ? text(arena, call.word(0)) : nullptr), 16)); }
IMPORT(dgettext) { CALL; call.result(copy_string(arena, dgettext(text(arena, call.word(0)), text(arena, call.word(1))), 15)); }
IMPORT(iconv_open) {
  CALL;
  iconv_t cd = iconv_open(text(arena, call.word(0)), text(arena, call.word(1)));
  call.result(cd == (iconv_t)-1 ? UINT32_MAX : iconv_handle(cd));
}
IMPORT(iconv_close) {
  CALL;
  const uint32_t handle = call.word(0);
  iconv_t cd = guest_iconv(handle);
  if (cd == (iconv_t)-1) { call.result(-1); return; }
  call.result(iconv_close(cd));
  std::lock_guard<std::mutex> lock(iconv_handles_mutex);
  iconv_handles.erase(handle);
}
IMPORT(iconv) {
  CALL;
  iconv_t cd = guest_iconv(call.word(0));
  if (cd == (iconv_t)-1) { call.result(UINT32_MAX); return; }
  const uint32_t inp = call.word(1), inleft = call.word(2), outp = call.word(3), outleft = call.word(4);
  if ((inp && !inleft) || (outp && !outleft)) { call.result(UINT32_MAX); errno = EFAULT; return; }
  uint32_t input = 0, input_left = 0, output = 0, output_left = 0;
  if (inp) { memcpy(&input, guest(arena, inp, 4), 4); memcpy(&input_left, guest(arena, inleft, 4), 4); }
  if (outp) { memcpy(&output, guest(arena, outp, 4), 4); memcpy(&output_left, guest(arena, outleft, 4), 4); }
  if ((inp && !input) || (outp && !output) ||
      (input && (input >= MEMORY_ARENA_SIZE || input_left > MEMORY_ARENA_SIZE - input)) ||
      (output && (output >= MEMORY_ARENA_SIZE || output_left > MEMORY_ARENA_SIZE - output))) {
    call.result(UINT32_MAX); errno = EFAULT; return;
  }
  char *in = inp ? static_cast<char *>(guest(arena, input, input_left)) : nullptr;
  char *out = outp ? static_cast<char *>(guest(arena, output, output_left)) : nullptr;
  size_t il = input_left, ol = output_left;
  size_t result = iconv(cd, inp ? &in : nullptr, inp ? &il : nullptr, outp ? &out : nullptr, outp ? &ol : nullptr);
  if (inp && input) input = uint32_t(in - reinterpret_cast<char *>(arena));
  if (outp && output) output = uint32_t(out - reinterpret_cast<char *>(arena));
  input_left = uint32_t(il); output_left = uint32_t(ol);
  if (inp) { memcpy(guest(arena, inp, 4), &input, 4); memcpy(guest(arena, inleft, 4), &input_left, 4); }
  if (outp) { memcpy(guest(arena, outp, 4), &output, 4); memcpy(guest(arena, outleft, 4), &output_left, 4); }
  call.result(result == size_t(-1) ? UINT32_MAX : uint32_t(result));
}

IMPORT(__strcoll_l) { CALL; locale_t l = guest_locale(call.word(2)); call.result(l ? strcoll_l(text(arena, call.word(0)), text(arena, call.word(1)), l) : 0); }
IMPORT(__strxfrm_l) {
  CALL; locale_t l = guest_locale(call.word(3)); if (!l) { call.result(0); return; }
  uint32_t n = call.word(2); char *dst = n ? static_cast<char *>(guest(arena, call.word(0), n)) : nullptr;
  call.result(strxfrm_l(dst, text(arena, call.word(1)), n, l));
}
IMPORT(__strtod_l) {
  CALL; locale_t l = guest_locale(call.word(2)); if (!l) { floating_result(state, 0.0); return; }
  const char *in = text(arena, call.word(0)); char *end = const_cast<char *>(in); double v = strtod_l(in, &end, l);
  parse_end(call, in, end); floating_result(state, v);
}
IMPORT(__strtof_l) {
  CALL; locale_t l = guest_locale(call.word(2)); if (!l) { floating_result(state, 0.0f); return; }
  const char *in = text(arena, call.word(0)); char *end = const_cast<char *>(in); float v = strtof_l(in, &end, l);
  parse_end(call, in, end); floating_result(state, v);
}
IMPORT(strtold_l) {
  CALL; locale_t l = guest_locale(call.word(2)); if (!l) { floating_result(state, 0.0); return; }
  const char *in = text(arena, call.word(0)); char *end = const_cast<char *>(in); long double v = strtold_l(in, &end, l);
  parse_end(call, in, end); floating_result(state, double(v));
}

IMPORT(wcscat) { CALL; call.result(wide_guest_pointer(arena, call.word(0), wcscat(wide_buffer(arena, call.word(0), wide_length(arena, call.word(0)) + wide_length(arena, call.word(1)) + 1), wide_text(arena, call.word(1))))); }
IMPORT(wcscpy) { CALL; uint32_t dst = call.word(0); wcscpy(wide_buffer(arena, dst, wide_length(arena, call.word(1)) + 1), wide_text(arena, call.word(1))); call.result(dst); }
IMPORT(wcschr) { CALL; uint32_t a = call.word(0); const wchar_t *s = wide_text(arena, a); call.result(wide_guest_pointer(arena, a, wcschr(s, wchar_t(call.word(1))))); }
IMPORT(wcsncmp) {
  CALL; const uint32_t count = call.word(2);
  call.result(count ? wcsncmp(wide_buffer(arena, call.word(0), count),
      wide_buffer(arena, call.word(1), count), count) : 0);
}
IMPORT(wcsstr) { CALL; uint32_t a = call.word(0); const wchar_t *s = wide_text(arena, a); call.result(wide_guest_pointer(arena, a, wcsstr(s, wide_text(arena, call.word(1))))); }
IMPORT(wcstombs) {
  CALL; uint32_t dst = call.word(0), n = call.word(2); const wchar_t *src = wide_text(arena, call.word(1));
  call.result(wcstombs(dst ? static_cast<char *>(guest(arena, dst, n)) : nullptr, src, n));
}
IMPORT(wcstod) {
  CALL; const wchar_t *in = wide_text(arena, call.word(0)); wchar_t *end = const_cast<wchar_t *>(in); double v = wcstod(in, &end);
  if (call.word(1)) { uint32_t p = call.word(0) + uint32_t((end - in) * sizeof(wchar_t)); memcpy(guest(arena, call.word(1), 4), &p, 4); }
  floating_result(state, v);
}
IMPORT(wcstol) {
  CALL; const wchar_t *in = wide_text(arena, call.word(0)); wchar_t *end = const_cast<wchar_t *>(in);
  long long parsed = wcstoll(in, &end, int32_t(call.word(2)));
  if (parsed > INT32_MAX) { parsed = INT32_MAX; errno = ERANGE; }
  else if (parsed < INT32_MIN) { parsed = INT32_MIN; errno = ERANGE; }
  if (call.word(1)) { uint32_t p = call.word(0) + uint32_t((end - in) * sizeof(wchar_t)); memcpy(guest(arena, call.word(1), 4), &p, 4); }
  call.result(uint32_t(int32_t(parsed)));
}
IMPORT(wcstoll) {
  CALL; const wchar_t *in = wide_text(arena, call.word(0)); wchar_t *end = const_cast<wchar_t *>(in); long long v = wcstoll(in, &end, int32_t(call.word(2)));
  if (call.word(1)) { uint32_t p = call.word(0) + uint32_t((end - in) * sizeof(wchar_t)); memcpy(guest(arena, call.word(1), 4), &p, 4); } call.result64(v);
}
IMPORT(wcsnrtombs) {
  CALL; uint32_t srcp = call.word(1), statep = call.word(4); uint32_t src;
  memcpy(&src, guest(arena, srcp, 4), 4);
  const uint32_t count = call.word(2), len = call.word(3);
  if (!src && count) { errno = EFAULT; call.result(UINT32_MAX); return; }
  const wchar_t *in = src ? wide_buffer(arena, src, count) : nullptr;
  mbstate_t host_state = load_mbstate(arena, statep);
  mbstate_t *state_arg = statep ? &host_state : implicit_mbstate(0);
  uint32_t dst = call.word(0);
  size_t result = wcsnrtombs(dst ? static_cast<char *>(guest(arena, dst, len)) : nullptr,
      &in, count, len, state_arg);
  if (src && in) src = uint32_t(reinterpret_cast<const uint8_t *>(in) - arena); else if (src && !in) src = 0;
  memcpy(guest(arena, srcp, 4), &src, 4); if (statep) save_mbstate(arena, statep, *state_arg); call.result(result == size_t(-1) ? UINT32_MAX : uint32_t(result));
}
IMPORT(mbstowcs) {
  CALL; uint32_t dst = call.word(0), n = call.word(2); const char *src = text(arena, call.word(1));
  call.result(mbstowcs(dst ? wide_buffer(arena, dst, n) : nullptr, src, n));
}
IMPORT(mbrtowc) {
  CALL; uint32_t dst = call.word(0), src = call.word(1), statep = call.word(3), count = call.word(2);
  mbstate_t local_state = load_mbstate(arena, statep);
  mbstate_t *state_arg = statep ? &local_state : implicit_mbstate(1);
  const char *input = src ? (count ? static_cast<const char *>(guest(arena, src, count)) : "") : "";
  const size_t input_count = src ? count : 1;
  size_t result = mbrtowc(src && dst ? wide_buffer(arena, dst, 1) : nullptr, input, input_count, state_arg);
  if (statep) save_mbstate(arena, statep, *state_arg);
  call.result(result == size_t(-1) ? UINT32_MAX : result == size_t(-2) ? UINT32_MAX - 1 : uint32_t(result));
}
IMPORT(mbsrtowcs) {
  CALL; uint32_t srcp = call.word(1), statep = call.word(3), src; memcpy(&src, guest(arena, srcp, 4), 4);
  const char *in = src ? text(arena, src) : nullptr; mbstate_t host_state = load_mbstate(arena, statep);
  if (!src) { errno = EFAULT; call.result(UINT32_MAX); return; }
  mbstate_t *state_arg = statep ? &host_state : implicit_mbstate(2);
  uint32_t dst = call.word(0), n = call.word(2);
  size_t result = mbsrtowcs(dst ? wide_buffer(arena, dst, n) : nullptr, &in, n, state_arg);
  if (src && in) src = uint32_t(reinterpret_cast<const uint8_t *>(in) - arena); else if (src && !in) src = 0;
  memcpy(guest(arena, srcp, 4), &src, 4); if (statep) save_mbstate(arena, statep, *state_arg); call.result(result == size_t(-1) ? UINT32_MAX : uint32_t(result));
}
IMPORT(mbsnrtowcs) {
  CALL; uint32_t srcp = call.word(1), statep = call.word(4), src; memcpy(&src, guest(arena, srcp, 4), 4);
  uint32_t dst = call.word(0), n = call.word(3), len = call.word(2);
  if (!src && len) { errno = EFAULT; call.result(UINT32_MAX); return; }
  const char *in = src ? static_cast<const char *>(guest(arena, src, len ? len : 1)) : nullptr; mbstate_t host_state = load_mbstate(arena, statep);
  mbstate_t *state_arg = statep ? &host_state : implicit_mbstate(3);
  size_t result = mbsnrtowcs(dst ? wide_buffer(arena, dst, n) : nullptr, &in, len, n, state_arg);
  if (src && in) src = uint32_t(reinterpret_cast<const uint8_t *>(in) - arena); else if (src && !in) src = 0;
  memcpy(guest(arena, srcp, 4), &src, 4); if (statep) save_mbstate(arena, statep, *state_arg); call.result(result == size_t(-1) ? UINT32_MAX : uint32_t(result));
}
IMPORT(wcrtomb) {
  CALL; uint32_t dst = call.word(0), statep = call.word(2); mbstate_t host_state = load_mbstate(arena, statep);
  mbstate_t *state_arg = statep ? &host_state : implicit_mbstate(4);
  size_t result = wcrtomb(dst ? static_cast<char *>(guest(arena, dst, MB_CUR_MAX)) : nullptr, wchar_t(call.word(1)), state_arg);
  if (statep) save_mbstate(arena, statep, *state_arg); call.result(result == size_t(-1) ? UINT32_MAX : uint32_t(result));
}
IMPORT(btowc) { CALL; call.result(btowc(int32_t(call.word(0)))); }
IMPORT(wctob) { CALL; call.result(wctob(wint_t(call.word(0)))); }
IMPORT(iswalnum) { CALL; call.result(iswalnum(wint_t(call.word(0)))); }
IMPORT(iswcntrl) { CALL; call.result(iswcntrl(wint_t(call.word(0)))); }
IMPORT(iswdigit) { CALL; call.result(iswdigit(wint_t(call.word(0)))); }
IMPORT(__iswctype_l) {
  CALL; locale_t l = guest_locale(call.word(2));
  wctype_t descriptor = guest_wctype(call.word(1));
  call.result(l ? iswctype_l(wint_t(call.word(0)), descriptor, l) : 0);
}
IMPORT(__wctype_l) {
  CALL; locale_t l = guest_locale(call.word(1));
  call.result(l ? remember_wctype(wctype_l(text(arena, call.word(0)), l)) : 0);
}
IMPORT(__towlower_l) { CALL; locale_t l = guest_locale(call.word(1)); call.result(l ? towlower_l(wint_t(call.word(0)), l) : call.word(0)); }
IMPORT(__towupper_l) { CALL; locale_t l = guest_locale(call.word(1)); call.result(l ? towupper_l(wint_t(call.word(0)), l) : call.word(0)); }
IMPORT(__wcscoll_l) { CALL; locale_t l = guest_locale(call.word(2)); call.result(l ? wcscoll_l(wide_text(arena, call.word(0)), wide_text(arena, call.word(1)), l) : 0); }
IMPORT(__wcsxfrm_l) {
  CALL; locale_t l = guest_locale(call.word(3)); if (!l) { call.result(0); return; }
  uint32_t n = call.word(2); wchar_t *dst = n ? wide_buffer(arena, call.word(0), n) : nullptr;
  call.result(wcsxfrm_l(dst, wide_text(arena, call.word(1)), n, l));
}
#include <locale.h>
#include <sys/times.h>
#include <utime.h>

namespace {
constexpr uint32_t time_tm_slot = 10;
constexpr uint32_t time_text_slot = 11;

time_t host_time(uint8_t *arena, uint32_t address) {
  int32_t value;
  memcpy(&value, guest(arena, address, sizeof(value)), sizeof(value));
  return time_t(value);
}

struct tm decode_tm(uint8_t *arena, uint32_t address) {
  guest(arena, address, 44);
  int32_t fields[9];
  memcpy(fields, guest(arena, address, sizeof(fields)), sizeof(fields));
  struct tm value = {};
  value.tm_sec = fields[0]; value.tm_min = fields[1]; value.tm_hour = fields[2];
  value.tm_mday = fields[3]; value.tm_mon = fields[4]; value.tm_year = fields[5];
  value.tm_wday = fields[6]; value.tm_yday = fields[7]; value.tm_isdst = fields[8];
  int32_t offset;
  memcpy(&offset, guest(arena, address + 36, 4), 4);
  value.tm_gmtoff = offset;
  uint32_t zone;
  memcpy(&zone, guest(arena, address + 40, 4), 4);
  value.tm_zone = zone ? text(arena, zone) : nullptr;
  return value;
}

void encode_tm(uint8_t *arena, uint32_t address, const struct tm &value) {
  guest(arena, address, 44);
  const int32_t fields[9] = {value.tm_sec, value.tm_min, value.tm_hour,
      value.tm_mday, value.tm_mon, value.tm_year, value.tm_wday,
      value.tm_yday, value.tm_isdst};
  memcpy(guest(arena, address, sizeof(fields)), fields, sizeof(fields));
  const int32_t offset = int32_t(value.tm_gmtoff);
  memcpy(guest(arena, address + 36, 4), &offset, 4);
  const uint32_t zone = value.tm_zone ? copy_string(arena, value.tm_zone, 12) : 0;
  memcpy(guest(arena, address + 40, 4), &zone, 4);
}

uint32_t returned_tm(uint8_t *arena, const struct tm *value) {
  if (!value) return 0;
  const uint32_t address = string_base + time_tm_slot * string_capacity;
  encode_tm(arena, address, *value);
  return address;
}

char *guest_format(uint8_t *arena, uint32_t address, size_t capacity) {
  return capacity ? static_cast<char *>(guest(arena, address, capacity)) : nullptr;
}
bool enter_locale(locale_t locale, locale_t &previous) {
  if (!locale) return false;
  previous = uselocale(locale);
  return previous != nullptr;
}
void leave_locale(locale_t previous) {
  if (previous) uselocale(previous);
}
}

IMPORT(atan) { CALL; floating_result(state, atan(call.real64(0))); }
IMPORT(acos) { CALL; floating_result(state, acos(call.real64(0))); }
IMPORT(asin) { CALL; floating_result(state, asin(call.real64(0))); }
IMPORT(atan2) { CALL; floating_result(state, atan2(call.real64(0), call.real64(2))); }
IMPORT(__acos_finite) { CALL; floating_result(state, acos(call.real64(0))); }
IMPORT(__asin_finite) { CALL; floating_result(state, asin(call.real64(0))); }
IMPORT(__atan2_finite) { CALL; floating_result(state, atan2(call.real64(0), call.real64(2))); }
IMPORT(exp2) { CALL; floating_result(state, exp2(call.real64(0))); }
IMPORT(exp2f) { CALL; floating_result(state, exp2f(call.real(0))); }
IMPORT(expf) { CALL; floating_result(state, expf(call.real(0))); }
IMPORT(fmaxf) { CALL; floating_result(state, fmaxf(call.real(0), call.real(1))); }
IMPORT(fminf) { CALL; floating_result(state, fminf(call.real(0), call.real(1))); }
IMPORT(fmod) { CALL; floating_result(state, fmod(call.real64(0), call.real64(2))); }
IMPORT(frexp) {
  CALL;
  int exponent;
  const double fraction = frexp(call.real64(0), &exponent);
  memcpy(guest(arena, call.word(2), 4), &exponent, 4);
  floating_result(state, fraction);
}
IMPORT(log) { CALL; floating_result(state, log(call.real64(0))); }
IMPORT(__log_finite) { CALL; floating_result(state, log(call.real64(0))); }
IMPORT(log10) { CALL; floating_result(state, log10(call.real64(0))); }
IMPORT(log10f) { CALL; floating_result(state, log10f(call.real(0))); }
IMPORT(sincos) {
  CALL;
  const double value = call.real64(0), sine = sin(value), cosine = cos(value);
  memcpy(guest(arena, call.word(2), 8), &sine, 8);
  memcpy(guest(arena, call.word(3), 8), &cosine, 8);
}
IMPORT(trunc) { CALL; floating_result(state, trunc(call.real64(0))); }
IMPORT(rand) { CALL; call.result(rand()); }
IMPORT(srand) { CALL; srand(call.word(0)); }
IMPORT(random) { CALL; call.result(::random()); }

IMPORT(asctime) {
  CALL;
  struct tm value = decode_tm(arena, call.word(0));
  char buffer[64];
  char *result = asctime_r(&value, buffer);
  call.result(result ? copy_string(arena, result, time_text_slot) : 0);
}
IMPORT(asctime_r) {
  CALL;
  struct tm value = decode_tm(arena, call.word(0));
  char buffer[64];
  char *result = asctime_r(&value, buffer);
  if (result) memcpy(guest(arena, call.word(1), 26), result, 26);
  call.result(result ? call.word(1) : 0);
}
IMPORT(clock) {
  CALL;
  const clock_t value = clock();
  call.result(value < 0 || uint64_t(value) > INT32_MAX ? UINT32_MAX : uint32_t(value));
}
IMPORT(ctime) {
  CALL;
  const time_t value = host_time(arena, call.word(0));
  char buffer[64];
  char *result = ctime_r(&value, buffer);
  call.result(result ? copy_string(arena, result, time_text_slot) : 0);
}
IMPORT(ctime_r) {
  CALL;
  const time_t value = host_time(arena, call.word(0));
  char buffer[64];
  char *result = ctime_r(&value, buffer);
  if (result) memcpy(guest(arena, call.word(1), 26), result, 26);
  call.result(result ? call.word(1) : 0);
}
IMPORT(gmtime) {
  CALL;
  const time_t value = host_time(arena, call.word(0));
  call.result(returned_tm(arena, gmtime(&value)));
}
IMPORT(gmtime_r) {
  CALL;
  const time_t value = host_time(arena, call.word(0));
  struct tm result;
  const bool ok = gmtime_r(&value, &result) != nullptr;
  if (ok) encode_tm(arena, call.word(1), result);
  call.result(ok ? call.word(1) : 0);
}
IMPORT(localtime) {
  CALL;
  const time_t value = host_time(arena, call.word(0));
  call.result(returned_tm(arena, localtime(&value)));
}
IMPORT(localtime_r) {
  CALL;
  const time_t value = host_time(arena, call.word(0));
  struct tm result;
  const bool ok = localtime_r(&value, &result) != nullptr;
  if (ok) encode_tm(arena, call.word(1), result);
  call.result(ok ? call.word(1) : 0);
}
IMPORT(strftime) {
  CALL;
  const uint32_t capacity = call.word(1);
  struct tm value = decode_tm(arena, call.word(3));
  char *output = guest_format(arena, call.word(0), capacity);
  call.result(strftime(output, capacity, text(arena, call.word(2)), &value));
}
IMPORT(__strftime_l) {
  CALL;
  const uint32_t capacity = call.word(1);
  struct tm value = decode_tm(arena, call.word(3));
  locale_t locale = guest_locale(call.word(4)), previous;
  if (!enter_locale(locale, previous)) { call.result(0); return; }
  char *output = guest_format(arena, call.word(0), capacity);
  const size_t result = strftime(output, capacity, text(arena, call.word(2)), &value);
  leave_locale(previous);
  call.result(result);
}
IMPORT(__wcsftime_l) {
  CALL;
  const uint32_t capacity = call.word(1);
  struct tm value = decode_tm(arena, call.word(3));
  locale_t locale = guest_locale(call.word(4)), previous;
  if (!enter_locale(locale, previous)) { call.result(0); return; }
  const wchar_t *format = static_cast<const wchar_t *>(guest(arena, call.word(2), sizeof(wchar_t)));
  const size_t available = (MEMORY_ARENA_SIZE - call.word(2)) / sizeof(wchar_t);
  size_t length = 0;
  while (length < available && format[length]) ++length;
  if (length == available) elfconv_runtime_error("Unterminated i386 wide time format.\n");
  wchar_t *output = capacity ? static_cast<wchar_t *>(guest(arena, call.word(0), size_t(capacity) * sizeof(wchar_t))) : nullptr;
  const size_t result = wcsftime(output, capacity, format, &value);
  leave_locale(previous);
  call.result(result);
}
IMPORT(timegm) {
  CALL;
  struct tm value = decode_tm(arena, call.word(0));
  const time_t result = timegm(&value);
  encode_tm(arena, call.word(0), value);
  if (result < INT32_MIN || result > INT32_MAX) { errno = EOVERFLOW; call.result(UINT32_MAX); }
  else call.result(uint32_t(int32_t(result)));
}
IMPORT(times) {
  CALL;
  struct tms host = {};
  const clock_t result = times(&host);
  const int32_t fields[4] = {int32_t(host.tms_utime), int32_t(host.tms_stime),
      int32_t(host.tms_cutime), int32_t(host.tms_cstime)};
  if (call.word(0))
    memcpy(guest(arena, call.word(0), sizeof(fields)), fields, sizeof(fields));
  call.result(uint32_t(result));
}
IMPORT(tzset) { CALL; tzset(); }
IMPORT(utime) {
  CALL;
  struct utimbuf value;
  struct utimbuf *pointer = nullptr;
  if (call.word(1)) {
    int32_t fields[2];
    memcpy(fields, guest(arena, call.word(1), sizeof(fields)), sizeof(fields));
    value.actime = fields[0]; value.modtime = fields[1]; pointer = &value;
  }
  call.result(utime(text(arena, call.word(0)), pointer));
}
IMPORT(utimes) {
  CALL;
  struct timeval values[2];
  struct timeval *pointer = nullptr;
  if (call.word(1)) {
    int32_t fields[4];
    memcpy(fields, guest(arena, call.word(1), sizeof(fields)), sizeof(fields));
    values[0] = {fields[0], fields[1]}; values[1] = {fields[2], fields[3]}; pointer = values;
  }
  call.result(utimes(text(arena, call.word(0)), pointer));
}
IMPORT(futimes) {
  CALL;
  if (call.word(1)) {
    int32_t fields[4];
    memcpy(fields, guest(arena, call.word(1), sizeof(fields)), sizeof(fields));
#ifdef __EMSCRIPTEN__
    struct timespec times[2] = {
        {fields[0], long(fields[1]) * 1000}, {fields[2], long(fields[3]) * 1000}};
    call.result(futimens(int32_t(call.word(0)), times));
#else
    struct timeval times[2] = {{fields[0], fields[1]}, {fields[2], fields[3]}};
    call.result(futimes(int32_t(call.word(0)), times));
#endif
    return;
  }
#ifdef __EMSCRIPTEN__
  call.result(futimens(int32_t(call.word(0)), nullptr));
#else
  call.result(futimes(int32_t(call.word(0)), nullptr));
#endif
}
IMPORT(alarm) { CALL; call.result(alarm(call.word(0))); }
IMPORT(get_nprocs) {
  CALL;
  const long result = sysconf(_SC_NPROCESSORS_ONLN);
  call.result(uint32_t(result));
}
IMPORT(sysconf) { CALL; call.result(uint32_t(sysconf(int32_t(call.word(0))))); }
IMPORT(getloadavg) {
  CALL;
  const int count = int32_t(call.word(1));
  if (count < 0) { errno = EINVAL; call.result(-1); return; }
  double values[3];
  const int result = getloadavg(values, std::min(count, 3));
  for (int i = 0; i < result; ++i) memcpy(guest(arena, call.word(0) + uint32_t(i) * 8, 8), &values[i], 8);
  call.result(result);
}
// Additional libc imports; included after I386Imports.cpp adapter helpers.
#include "I386Scan.inc"
#include <locale.h>
extern "C" void __ecv_i386_fcntl(uint8_t *, State *, uint32_t, RuntimeManager *);
namespace {
struct GuestAtexitEntry { uint32_t function, argument, dso; };
std::mutex guest_atexit_mutex;
std::map<RuntimeManager *, std::vector<GuestAtexitEntry>> guest_atexit;
uint32_t guest_offset(uint8_t *arena, const void *pointer) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(arena), value = reinterpret_cast<uintptr_t>(pointer);
  if (value < base || value - base >= MEMORY_ARENA_SIZE)
    elfconv_runtime_error("Host libc returned a pointer outside guest memory.\n");
  return uint32_t(value - base);
}
void drain_guest_atexit(uint8_t *arena, State *state, RuntimeManager *runtime,
                        uint32_t dso = 0) {
  for (;;) {
    GuestAtexitEntry entry;
    {
      std::lock_guard<std::mutex> lock(guest_atexit_mutex);
      auto found = guest_atexit.find(runtime);
      if (found == guest_atexit.end()) return;
      auto &entries = found->second;
      auto match = entries.end();
      for (auto it = entries.end(); it != entries.begin();) {
        --it;
        if (!dso || it->dso == dso) { match = it; break; }
      }
      if (match == entries.end()) return;
      entry = *match;
      entries.erase(match);
      if (entries.empty()) guest_atexit.erase(found);
    }
    const uint32_t args[] = {entry.argument};
    invoke(arena, state, runtime, entry.function, args, 1);
  }
}
int print_va(Call &call, FILE *output, unsigned format_arg, unsigned va_arg) {
  auto out = format(call.arena, text(call.arena, call.word(format_arg)),
                    {call.arena, call.word(va_arg)});
  return fwrite(out.data(), 1, out.size(), output) == out.size() ? int(out.size()) : -1;
}
}
extern "C" void __ecv_i386_drain_guest_atexit(uint8_t *arena, State *state,
                                                RuntimeManager *runtime) {
  drain_guest_atexit(arena, state, runtime);
}
IMPORT(__cxa_atexit) {
  CALL;
  {
    std::lock_guard<std::mutex> lock(guest_atexit_mutex);
    guest_atexit[runtime].push_back({call.word(0), call.word(1), call.word(2)});
  }
  call.result(0);
}
IMPORT(__cxa_finalize) {
  CALL;
  drain_guest_atexit(arena, state, runtime, call.word(0));
}
IMPORT(__fdelt_chk) {
  CALL;
  const uint32_t fd = call.word(0);
  if (int32_t(fd) < 0 || fd >= 1024) elfconv_runtime_error("Guest fd_set index out of range.\n");
  call.result(fd / 32);
}
IMPORT(__fread_chk) {
  CALL;
  const uint32_t size = call.word(2), count = call.word(3);
  const uint64_t bytes = uint64_t(size) * count;
  if (bytes > call.word(1)) elfconv_runtime_error("Guest fortified fread overflow.\n");
  if (!bytes) { call.result(0); return; }
  call.result(fread(guest(arena, call.word(0), size_t(bytes)), size, count, stream(call.word(4))));
}
IMPORT(__memmove_chk) {
  CALL; const uint32_t size = call.word(2);
  if (size > call.word(3)) elfconv_runtime_error("Guest fortified memory move overflow.\n");
  if (size) memmove(guest(arena, call.word(0), size), guest(arena, call.word(1), size), size);
  call.result(call.word(0));
}
IMPORT(__memset_chk) {
  CALL; const uint32_t size = call.word(2);
  if (size > call.word(3)) elfconv_runtime_error("Guest fortified memory set overflow.\n");
  if (size) memset(guest(arena, call.word(0), size), call.word(1), size);
  call.result(call.word(0));
}
IMPORT(__rawmemchr) {
  CALL; const uint32_t address = call.word(0);
  const auto *begin = static_cast<const unsigned char *>(guest(arena, address, 1));
  const auto *found = static_cast<const unsigned char *>(memchr(begin, call.word(1), MEMORY_ARENA_SIZE - address));
  if (!found) elfconv_runtime_error("Unterminated guest rawmemchr input.\n");
  call.result(address + uint32_t(found - begin));
}
IMPORT(__strncpy_chk) {
  CALL; const uint32_t n = call.word(2);
  if (n > call.word(3)) elfconv_runtime_error("Guest fortified strncpy overflow.\n");
  if (n) strncpy(static_cast<char *>(guest(arena, call.word(0), n)), text(arena, call.word(1)), n);
  call.result(call.word(0));
}
IMPORT(__strtod_internal) {
  CALL;
  const char *input = text(arena, call.word(0)); char *end = nullptr;
  const double value = strtod(input, &end);
  if (call.word(1)) { const uint32_t ptr = call.word(0) + uint32_t(end - input); memcpy(guest(arena, call.word(1), 4), &ptr, 4); }
  floating_result(state, value);
}
IMPORT(__strtol_internal) { CALL; parse_signed(call, 32); }
IMPORT(__strtoul_internal) { CALL; parse_unsigned(call); }
IMPORT(__strtok_r) {
  CALL; const uint32_t slot = call.word(2); uint32_t current;
  if (call.word(0)) current = call.word(0); else memcpy(&current, guest(arena, slot, 4), 4);
  if (!current) { call.result(0); return; }
  char *s = static_cast<char *>(guest(arena, current, 1));
  char *token = strsep(&s, text(arena, call.word(1)));
  while (token && !*token) token = strsep(&s, text(arena, call.word(1)));
  const uint32_t next = s ? guest_offset(arena, s) : 0;
  memcpy(guest(arena, slot, 4), &next, 4);
  call.result(token ? guest_offset(arena, token) : 0);
}
IMPORT(wprintf) {
  CALL;
  const auto output = wide_format(arena, wide_text(arena, call.word(0)),
                                 {arena, call.stack + 8});
  for (wchar_t character : output) {
    if (fputwc(character, stdout) == WEOF) { call.result(-1); return; }
  }
  call.result(output.size());
}
IMPORT(swprintf) {
  CALL;
  const uint32_t capacity = call.word(1);
  const auto out = wide_format(arena, wide_text(arena, call.word(2)),
                               {arena, call.stack + 16});
  if (out.size() >= capacity) { errno = E2BIG; call.result(-1); return; }
  memcpy(wide_buffer(arena, call.word(0), out.size() + 1), out.c_str(),
         (out.size() + 1) * sizeof(wchar_t));
  call.result(out.size());
}
IMPORT(__swprintf_chk) {
  CALL; const uint32_t capacity = call.word(1);
  if (capacity > call.word(3)) elfconv_runtime_error("Guest fortified swprintf overflow.\n");
  const auto out = wide_format(arena, wide_text(arena, call.word(4)), {arena, call.stack + 24});
  if (out.size() >= capacity) { errno = E2BIG; call.result(-1); return; }
  memcpy(wide_buffer(arena, call.word(0), out.size() + 1), out.c_str(),
         (out.size() + 1) * sizeof(wchar_t));
  call.result(out.size());
}
IMPORT(__vfprintf_chk) { CALL; call.result(print_va(call, stream(call.word(0)), 2, 3)); }
IMPORT(__vswprintf_chk) {
  CALL; const uint32_t capacity = call.word(1);
  if (capacity > call.word(3)) elfconv_runtime_error("Guest fortified vswprintf overflow.\n");
  const auto out = wide_format(arena, wide_text(arena, call.word(4)), {arena, call.word(5)});
  if (out.size() >= capacity) { errno = E2BIG; call.result(-1); return; }
  memcpy(wide_buffer(arena, call.word(0), out.size() + 1), out.c_str(),
         (out.size() + 1) * sizeof(wchar_t));
  call.result(out.size());
}
IMPORT(__wcscat_chk) {
  CALL; const uint32_t dst = call.word(0), src = call.word(1);
  const uint32_t length = wide_length(arena, dst), add = wide_length(arena, src);
  if (uint64_t(length) + add + 1 > call.word(2)) elfconv_runtime_error("Guest fortified wcscat overflow.\n");
  wcscat(wide_buffer(arena, dst, length + add + 1), wide_text(arena, src)); call.result(dst);
}
IMPORT(__wcsncpy_chk) {
  CALL; const uint32_t count = call.word(2);
  if (count > call.word(3)) elfconv_runtime_error("Guest fortified wcsncpy overflow.\n");
  if (count) wcsncpy(wide_buffer(arena, call.word(0), count), wide_text(arena, call.word(1)), count);
  call.result(call.word(0));
}
IMPORT(__xpg_strerror_r) {
  CALL; const uint32_t output = call.word(1), size = call.word(2);
  const char *message = strerror(i386_errno_to_host(call.word(0)));
  const size_t length = strlen(message);
  if (size) { const size_t n = std::min(length, size_t(size - 1)); char *dst = static_cast<char *>(guest(arena, output, n + 1)); memcpy(dst, message, n); dst[n] = 0; }
  call.result(length < size ? 0 : ERANGE);
}
IMPORT(strerrorname_np) {
  CALL;
  const char *name = i386_strerrorname(call.word(0));
  call.result(name ? copy_string(arena, name, 20) : 0);
}
IMPORT(strerrordesc_np) {
  CALL;
  const int error = int32_t(call.word(0));
  if (!i386_strerrorname(error)) { call.result(0); return; }
  const char *description = i386_strerrordesc(error);
  if (!description) description = strerror(i386_errno_to_host(error));
  call.result(copy_string(arena, description, 64));
}
IMPORT(abort) { abort(); }
IMPORT(mtrace) {
  CALL;
  const char *path = getenv("MALLOC_TRACE");
  if (path && *path)
    elfconv_runtime_error("Guest mtrace requires unsupported allocator tracing.\n");
}
IMPORT(__isoc23_scanf) {
  CALL;
  call.result(scan_guest_file(arena, stdin, text(arena, call.word(0)),
                             {arena, call.stack + 8}));
}
IMPORT(__isoc23_vscanf) {
  CALL;
  call.result(scan_guest_file(arena, stdin, text(arena, call.word(0)),
                             {arena, call.word(1)}));
}
IMPORT(__isoc23_fscanf) {
  CALL;
  call.result(scan_guest_file(arena, stream(call.word(0)), text(arena, call.word(1)),
                             {arena, call.stack + 12}));
}
IMPORT(__isoc23_sscanf) {
  CALL;
  call.result(scan_guest_string(arena, text(arena, call.word(0)), text(arena, call.word(1)),
                               {arena, call.stack + 12}));
}
IMPORT(__isoc23_vsscanf) {
  CALL;
  call.result(scan_guest_string(arena, text(arena, call.word(0)), text(arena, call.word(1)),
                               {arena, call.word(2)}));
}
IMPORT(fscanf) {
  CALL;
  call.result(scan_guest_file(arena, stream(call.word(0)), text(arena, call.word(1)),
                             {arena, call.stack + 12}, true));
}
IMPORT(__isoc99_fscanf) {
  CALL;
  call.result(scan_guest_file(arena, stream(call.word(0)), text(arena, call.word(1)),
                             {arena, call.stack + 12}));
}
IMPORT(__isoc23_vfscanf) {
  CALL;
  call.result(scan_guest_file(arena, stream(call.word(0)), text(arena, call.word(1)),
                             {arena, call.word(2)}));
}
IMPORT(sscanf) {
  CALL;
  call.result(scan_guest_string(arena, text(arena, call.word(0)), text(arena, call.word(1)),
                               {arena, call.stack + 12}, true));
}
IMPORT(vsscanf) {
  CALL;
  call.result(scan_guest_string(arena, text(arena, call.word(0)), text(arena, call.word(1)),
                               {arena, call.word(2)}, true));
}
IMPORT(__isoc99_vsscanf) {
  CALL;
  call.result(scan_guest_string(arena, text(arena, call.word(0)), text(arena, call.word(1)),
                               {arena, call.word(2)}));
}
IMPORT(vscanf) {
  CALL;
  call.result(scan_guest_file(arena, stdin, text(arena, call.word(0)), {arena, call.word(1)}, true));
}
IMPORT(__isoc99_vscanf) {
  CALL;
  call.result(scan_guest_file(arena, stdin, text(arena, call.word(0)), {arena, call.word(1)}));
}
IMPORT(vfscanf) {
  CALL;
  call.result(scan_guest_file(arena, stream(call.word(0)), text(arena, call.word(1)),
                             {arena, call.word(2)}, true));
}
IMPORT(__isoc99_vfscanf) {
  CALL;
  call.result(scan_guest_file(arena, stream(call.word(0)), text(arena, call.word(1)),
                             {arena, call.word(2)}));
}
IMPORT(scanf) {
  CALL;
  call.result(scan_guest_file(arena, stdin, text(arena, call.word(0)),
                             {arena, call.stack + 8}, true));
}
IMPORT(__isoc99_scanf) {
  CALL;
  call.result(scan_guest_file(arena, stdin, text(arena, call.word(0)), {arena, call.stack + 8}));
}
IMPORT(getchar) { CALL; call.result(uint32_t(getchar())); }
IMPORT(getwc) { CALL; call.result(uint32_t(getwc(stream(call.word(0))))); }
IMPORT(fgetwc) { CALL; call.result(uint32_t(fgetwc(stream(call.word(0))))); }
IMPORT(isalpha) { CALL; call.result(isalpha(int32_t(call.word(0))) ? 0x0400 : 0); }
IMPORT(isgraph) { CALL; call.result(isgraph(int32_t(call.word(0))) ? 0x8000 : 0); }
IMPORT(iscntrl) { CALL; call.result(iscntrl(int32_t(call.word(0))) ? 0x0002 : 0); }
IMPORT(ispunct) { CALL; call.result(ispunct(int32_t(call.word(0))) ? 0x0004 : 0); }
IMPORT(isupper) { CALL; call.result(isupper(int32_t(call.word(0))) ? 0x0100 : 0); }
IMPORT(strspn) {
  CALL;
  call.result(uint32_t(strspn(text(arena, call.word(0)), text(arena, call.word(1)))));
}
IMPORT(putwc) { CALL; call.result(uint32_t(putwc(wchar_t(call.word(0)), stream(call.word(1))))); }
IMPORT(fputws) { CALL; call.result(fputws(wide_text(arena, call.word(0)), stream(call.word(1)))); }
IMPORT(strtok) {
  CALL;
  static std::map<RuntimeManager *, uint32_t> next;
  uint32_t current = call.word(0) ? call.word(0) : next[runtime];
  if (!current) { call.result(0); return; }
  char *s = static_cast<char *>(guest(arena, current, 1));
  char *token = strsep(&s, text(arena, call.word(1)));
  while (token && !*token) token = strsep(&s, text(arena, call.word(1)));
  next[runtime] = s ? guest_offset(arena, s) : 0;
  call.result(token ? guest_offset(arena, token) : 0);
}
IMPORT(swscanf) {
  CALL;
  call.result(scan_guest_wide_string(arena, wide_text(arena, call.word(0)),
                                    wide_text(arena, call.word(1)), {arena, call.stack + 12}));
}
IMPORT(__isoc23_swscanf) {
  CALL;
  call.result(scan_guest_wide_string(arena, wide_text(arena, call.word(0)),
                                    wide_text(arena, call.word(1)), {arena, call.stack + 12}));
}
IMPORT(__isoc23_fwscanf) {
  CALL;
  call.result(scan_guest_wide_file(arena, stream(call.word(0)), wide_text(arena, call.word(1)),
                                  {arena, call.stack + 12}));
}
IMPORT(ungetwc) { CALL; call.result(uint32_t(ungetwc(wint_t(call.word(0)), stream(call.word(1))))); }
IMPORT(vfprintf) {
  CALL; auto out = format(arena, text(arena, call.word(1)), {arena, call.word(2)});
  FILE *output = stream(call.word(0));
  call.result(fwrite(out.data(), 1, out.size(), output) == out.size() ? int(out.size()) : -1);
}
IMPORT(vprintf) {
  CALL;
  call.result(print_va(call, stdout, 0, 1));
}
IMPORT(fmax) { CALL; floating_result(state, fmax(call.real64(0), call.real64(2))); }
IMPORT(fmin) { CALL; floating_result(state, fmin(call.real64(0), call.real64(2))); }
IMPORT(fma) { CALL; floating_result(state, fma(call.real64(0), call.real64(2), call.real64(4))); }
IMPORT(fmaf) { CALL; floating_result(state, fmaf(call.real(0), call.real(1), call.real(2))); }
IMPORT(frexpf) {
  CALL; int exponent;
  const float fraction = frexpf(call.real(0), &exponent);
  memcpy(guest(arena, call.word(1), sizeof(exponent)), &exponent, sizeof(exponent));
  floating_result(state, fraction);
}
IMPORT(ldexp) { CALL; floating_result(state, ldexp(call.real64(0), int32_t(call.word(2)))); }
IMPORT(ldexpf) { CALL; floating_result(state, ldexpf(call.real(0), int32_t(call.word(1)))); }
IMPORT(log2) { CALL; floating_result(state, log2(call.real64(0))); }
IMPORT(log2f) { CALL; floating_result(state, log2f(call.real(0))); }
IMPORT(llround) { CALL; call.result64(uint64_t(llround(call.real64(0)))); }
IMPORT(llroundf) { CALL; call.result64(uint64_t(llroundf(call.real(0)))); }
IMPORT(newlocale) { __ecv_i386___newlocale(arena, state, 0, runtime); }
IMPORT(freelocale) { __ecv_i386___freelocale(arena, state, 0, runtime); }
IMPORT(strtod_l) {
  CALL; const char *input = text(arena, call.word(0)); char *end = nullptr;
  const double result = strtod_l(input, &end, guest_locale(call.word(2)));
  parse_end(call, input, end); floating_result(state, result);
}
IMPORT(strtof_l) {
  CALL; const char *input = text(arena, call.word(0)); char *end = nullptr;
  const float result = strtof_l(input, &end, guest_locale(call.word(2)));
  parse_end(call, input, end); floating_result(state, result);
}
IMPORT(strndup) {
  CALL; const uint32_t size = call.word(1);
  const char *input = bounded_text(arena, call.word(0), size);
  const size_t length = strnlen(input, size);
  const uint32_t result = allocate(uint32_t(length + 1));
  if (!result) { call.result(0); return; }
  auto *output = static_cast<char *>(guest(arena, result, length + 1));
  memcpy(output, input, length); output[length] = 0; call.result(result);
}
IMPORT(strnlen) {
  CALL; const uint32_t size = call.word(1);
  call.result(strnlen(text(arena, call.word(0)), size));
}
IMPORT(strtok_r) { __ecv_i386___strtok_r(arena, state, 0, runtime); }
IMPORT(SDL_SetError) {
  CALL;
  const auto message = format(arena, text(arena, call.word(0)), {arena, call.stack + 8});
  call.result(SDL_SetError("%s", message.c_str()));
}
IMPORT(arc4random) {
  CALL; uint32_t value;
  if (getentropy(&value, sizeof(value))) { call.result(0); return; }
  call.result(value);
}
IMPORT(getentropy) {
  CALL; const uint32_t size = call.word(1);
  if (size > 256) { errno = EIO; call.result(-1); return; }
  call.result(getentropy(guest(arena, call.word(0), size), size));
}
IMPORT(secure_getenv) {
  CALL; call.result(copy_string(arena, secure_getenv(text(arena, call.word(0))), 21));
}
IMPORT(sched_getcpu) {
  CALL;
#ifdef __EMSCRIPTEN__
  errno = ENOSYS; call.result(-1);
#else
  call.result(sched_getcpu());
#endif
}
IMPORT(sched_yield) { CALL; call.result(host_errno_to_i386(sched_yield())); }
IMPORT(sync) { CALL; sync(); }
IMPORT(setpriority) {
  CALL;
#ifdef __EMSCRIPTEN__
  errno = ENOSYS; call.result(-1);
#else
  call.result(setpriority(int32_t(call.word(0)), int32_t(call.word(1)),
                          int32_t(call.word(2))));
#endif
}
IMPORT(sigfillset) {
  CALL; sigset_t set; sigfillset(&set);
  memcpy(guest(arena, call.word(0), sizeof(set)), &set, sizeof(set)); call.result(0);
}
IMPORT(sigdelset) {
  CALL; sigset_t set;
  memcpy(&set, guest(arena, call.word(0), sizeof(set)), sizeof(set));
  const int result = sigdelset(&set, int32_t(call.word(1)));
  memcpy(guest(arena, call.word(0), sizeof(set)), &set, sizeof(set)); call.result(result);
}
namespace {
std::string guest_syslog_ident;
int guest_syslog_options = 0, guest_syslog_facility = 0;
void guest_syslog_message(int priority, std::string message) {
  static const char *names[] = {"emergency", "alert", "critical", "error",
                                "warning", "notice", "info", "debug"};
  const unsigned level = unsigned(priority) & 7;
  if ((guest_syslog_options & 1) != 0)
    fprintf(stderr, "%s[%u]: %s: %s\n", guest_syslog_ident.empty() ? "elfconv" :
            guest_syslog_ident.c_str(), unsigned(getpid()), names[level], message.c_str());
  else
    fprintf(stderr, "%s: %s: %s\n", guest_syslog_ident.empty() ? "elfconv" :
            guest_syslog_ident.c_str(), names[level], message.c_str());
}
}
IMPORT(posix_memalign) {
  CALL; const uint32_t output = call.word(0), alignment = call.word(1), size = call.word(2);
  if (!alignment || (alignment & (alignment - 1)) || alignment % sizeof(void *)) {
    call.result(EINVAL); return;
  }
  const uint32_t address = allocate_aligned(size, alignment);
  if (!address) { call.result(ENOMEM); return; }
  memcpy(guest(arena, output, 4), &address, 4); call.result(0);
}
IMPORT(fchmod) { CALL; call.result(fchmod(int32_t(call.word(0)), call.word(1))); }
IMPORT(fchmodat) {
  CALL; call.result(fchmodat(int32_t(call.word(0)), text(arena, call.word(1)),
                             call.word(2), int32_t(call.word(3))));
}
IMPORT(openat64) {
  CALL;
  const int flags = int32_t(call.word(2));
  const bool needs_mode = (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE;
  call.result(openat(int32_t(call.word(0)), text(arena, call.word(1)), flags,
                     needs_mode ? call.word(3) : 0));
}
IMPORT(unlinkat) {
  CALL; call.result(unlinkat(int32_t(call.word(0)), text(arena, call.word(1)),
                             int32_t(call.word(2))));
}
IMPORT(utimensat) {
  CALL; const uint32_t path = call.word(1), times = call.word(2);
  timespec host_times[2], *host_pointer = nullptr;
  if (times) {
    int32_t values[4];
    memcpy(values, guest(arena, times, sizeof(values)), sizeof(values));
    for (unsigned i = 0; i < 2; ++i) {
      host_times[i].tv_sec = values[i * 2];
      host_times[i].tv_nsec = values[i * 2 + 1];
    }
    host_pointer = host_times;
  }
  call.result(utimensat(int32_t(call.word(0)), text(arena, path), host_pointer,
                        int32_t(call.word(3))));
}
IMPORT(truncate64) {
  CALL; call.result(truncate(text(arena, call.word(0)), off_t(call.wide(1))));
}
IMPORT(sendfile64) {
  CALL;
  const int output_fd = int32_t(call.word(0)), input_fd = int32_t(call.word(1));
  const uint32_t offset_address = call.word(2), count = call.word(3);
  int64_t offset = 0;
  if (offset_address) memcpy(&offset, guest(arena, offset_address, sizeof(offset)), sizeof(offset));
  std::array<uint8_t, 16384> buffer;
  size_t transferred = 0;
  while (transferred < count) {
    const size_t chunk = std::min(buffer.size(), size_t(count) - transferred);
    const ssize_t read_count = offset_address
        ? pread(input_fd, buffer.data(), chunk, off_t(offset))
        : read(input_fd, buffer.data(), chunk);
    if (read_count < 0) {
      if (errno == EINTR) continue;
      if (transferred) break;
      call.result(-1); return;
    }
    if (!read_count) break;
    size_t written = 0;
    while (written < size_t(read_count)) {
      const ssize_t result = write(output_fd, buffer.data() + written,
                                   size_t(read_count) - written);
      if (result < 0) {
        if (errno == EINTR) continue;
        if (!transferred && !written) { call.result(-1); return; }
        break;
      }
      if (!result) break;
      written += size_t(result);
    }
    transferred += written;
    if (offset_address) offset += int64_t(written);
    if (written != size_t(read_count)) break;
  }
  if (offset_address) memcpy(guest(arena, offset_address, sizeof(offset)), &offset, sizeof(offset));
  call.result(uint32_t(transferred));
}
IMPORT(strerror_r) {
  CALL; const uint32_t error = call.word(0), output = call.word(1), size = call.word(2);
  const char *message = strerror(i386_errno_to_host(error));
  const size_t length = strlen(message);
  if (!size) { call.result(0); return; }
  const size_t copied = std::min(length, size_t(size - 1));
  auto *destination = static_cast<char *>(guest(arena, output, copied + 1));
  memcpy(destination, message, copied); destination[copied] = 0;
  call.result(output);
}
IMPORT(__wmemcpy_chk) {
  CALL; const uint32_t count = call.word(2);
  if (uint64_t(count) * sizeof(wchar_t) > call.word(3))
    elfconv_runtime_error("Guest fortified wide-memory copy overflow.\n");
  if (count) wmemcpy(wide_buffer(arena, call.word(0), count),
                     wide_text(arena, call.word(1)), count);
  call.result(call.word(0));
}
IMPORT(__sched_cpucount) {
  CALL; const uint32_t bytes = call.word(0);
  const auto *set = static_cast<const uint8_t *>(guest(arena, call.word(1), bytes));
  unsigned count = 0;
  for (uint32_t i = 0; i < bytes; ++i) count += unsigned(__builtin_popcount(unsigned(set[i])));
  call.result(count);
}
IMPORT(openlog) {
  CALL; guest_syslog_ident = call.word(0) ? text(arena, call.word(0)) : "";
  guest_syslog_options = int32_t(call.word(1));
  guest_syslog_facility = int32_t(call.word(2));
}
IMPORT(__syslog_chk) {
  CALL; const int priority = int32_t(call.word(0));
  const auto message = format(arena, text(arena, call.word(2)), {arena, call.stack + 16});
  guest_syslog_message(priority, message);
}
IMPORT(_dl_find_object) { CALL; call.result(uint32_t(-1)); }
IMPORT(__mbsrtowcs_chk) {
  CALL;
  const uint32_t destination = call.word(0), source_address = call.word(1);
  const uint32_t count = call.word(2), state_address = call.word(3), object_size = call.word(4);
  if (destination && uint64_t(count) * sizeof(wchar_t) > object_size)
    elfconv_runtime_error("Guest fortified multibyte conversion overflow.\n");
  uint32_t source;
  memcpy(&source, guest(arena, source_address, 4), 4);
  const char *host_source = source ? text(arena, source) : nullptr;
  mbstate_t host_state{};
  if (state_address) memcpy(&host_state, guest(arena, state_address, sizeof(host_state)), sizeof(host_state));
  wchar_t *host_destination = destination
      ? wide_buffer(arena, destination, count) : nullptr;
  const size_t result = mbsrtowcs(host_destination, &host_source, count, &host_state);
  if (source_address) {
    const uint32_t next = host_source ? guest_offset(arena, host_source) : 0;
    memcpy(guest(arena, source_address, 4), &next, 4);
  }
  if (state_address)
    memcpy(guest(arena, state_address, sizeof(host_state)), &host_state, sizeof(host_state));
  call.result(uint32_t(result));
}
IMPORT(__isoc23_strtoull) { CALL; parse_unsigned(call, true); }
IMPORT(__wmemset_chk) {
  CALL; const uint32_t count = call.word(2);
  if (uint64_t(count) * sizeof(wchar_t) > call.word(3))
    elfconv_runtime_error("Guest fortified wide-memory set overflow.\n");
  if (count) wmemset(wide_buffer(arena, call.word(0), count),
                     wchar_t(call.word(1)), count);
  call.result(call.word(0));
}
IMPORT(__asprintf_chk) {
  CALL; const uint32_t output = call.word(0);
  const uint32_t null_pointer = 0;
  memcpy(guest(arena, output, 4), &null_pointer, 4);
  int error = 0;
  const auto result = format(arena, text(arena, call.word(2)),
                             {arena, call.stack + 16}, &error);
  if (error) { errno = error; call.result(-1); return; }
  const uint32_t address = allocate(uint32_t(result.size() + 1));
  if (!address) { call.result(-1); return; }
  auto *buffer = static_cast<char *>(guest(arena, address, result.size() + 1));
  memcpy(buffer, result.data(), result.size()); buffer[result.size()] = 0;
  memcpy(guest(arena, output, 4), &address, 4); call.result(int32_t(result.size()));
}
IMPORT(__vasprintf_chk) {
  CALL; const uint32_t output = call.word(0);
  const uint32_t null_pointer = 0;
  memcpy(guest(arena, output, 4), &null_pointer, 4);
  int error = 0;
  const auto result = format(arena, text(arena, call.word(2)),
                             {arena, call.word(3)}, &error);
  if (error) { errno = error; call.result(-1); return; }
  const uint32_t address = allocate(uint32_t(result.size() + 1));
  if (!address) { call.result(-1); return; }
  auto *buffer = static_cast<char *>(guest(arena, address, result.size() + 1));
  memcpy(buffer, result.data(), result.size()); buffer[result.size()] = 0;
  memcpy(guest(arena, output, 4), &address, 4); call.result(int32_t(result.size()));
}
IMPORT(__openat64_2) { __ecv_i386_openat64(arena, state, 0, runtime); }
IMPORT(fcntl64) { __ecv_i386_fcntl(arena, state, 0, runtime); }
IMPORT(__read_chk) {
  CALL; const uint32_t count = call.word(2);
  if (count > call.word(3)) elfconv_runtime_error("Guest fortified read overflow.\n");
  __ecv_i386_read(arena, state, 0, runtime);
}
IMPORT(sched_getaffinity) {
  CALL; errno = ENOSYS; call.result(-1);
}
IMPORT(clock_nanosleep) {
  CALL;
  const clockid_t clock = clockid_t(int32_t(call.word(0)));
  const int flags = int32_t(call.word(1));
  int32_t values[2];
  memcpy(values, guest(arena, call.word(2), sizeof(values)), sizeof(values));
  if (values[1] < 0 || values[1] >= 1000000000) { call.result(EINVAL); return; }
  timespec requested{values[0], values[1]}, remaining{};
  if (flags & TIMER_ABSTIME) {
    timespec now;
    if (clock_gettime(clock, &now)) { call.result(errno); return; }
    requested.tv_sec -= now.tv_sec;
    requested.tv_nsec -= now.tv_nsec;
    if (requested.tv_nsec < 0) { --requested.tv_sec; requested.tv_nsec += 1000000000; }
    if (requested.tv_sec < 0) requested = {0, 0};
  } else if (flags) { call.result(EINVAL); return; }
  const int result = nanosleep(&requested, &remaining);
  if (result && call.word(3)) {
    const int32_t guest_remaining[2] = {
      int32_t(remaining.tv_sec), int32_t(remaining.tv_nsec)
    };
    memcpy(guest(arena, call.word(3), sizeof(guest_remaining)),
           guest_remaining, sizeof(guest_remaining));
  }
  call.result(result ? uint32_t(errno) : 0);
}
// Additional i386 POSIX imports used by Portal and its bundled libraries.
#include <pwd.h>
#include <sys/resource.h>
#include <sys/mman.h>
#ifndef __EMSCRIPTEN__
#include <sys/mount.h>
#endif
#include <sys/uio.h>
#include <termios.h>
#ifndef __EMSCRIPTEN__
#include <sys/file.h>
#endif
#include <link.h>
#include <elf.h>
#include <sys/statvfs.h>
#include <signal.h>
struct GuestStatfs32 {
  int32_t type, block_size;
  uint32_t blocks, blocks_free, blocks_available, files, files_free;
  uint32_t fsid[2], name_length, fragment_size, flags, spare[4];
};
constexpr size_t guest_statfs64_size = 84;
namespace {
int store_stat32_extra(uint8_t *arena, uint32_t address, const struct stat &host) {
  if (host.st_size < INT32_MIN || host.st_size > INT32_MAX ||
      uint64_t(host.st_ino) > UINT32_MAX || host.st_atim.tv_sec < INT32_MIN ||
      host.st_atim.tv_sec > INT32_MAX || host.st_mtim.tv_sec < INT32_MIN ||
      host.st_mtim.tv_sec > INT32_MAX || host.st_ctim.tv_sec < INT32_MIN ||
      host.st_ctim.tv_sec > INT32_MAX) { errno = EOVERFLOW; return -1; }
  uint8_t data[88] = {};
  auto put = [&](unsigned o, uint64_t v, unsigned n = 4) { memcpy(data + o, &v, n); };
  put(0, host.st_dev, 8); put(12, host.st_ino); put(16, host.st_mode);
  put(20, host.st_nlink); put(24, host.st_uid); put(28, host.st_gid);
  put(32, host.st_rdev, 8); put(44, host.st_size); put(48, host.st_blksize);
  put(52, host.st_blocks); put(56, host.st_atim.tv_sec); put(60, host.st_atim.tv_nsec);
  put(64, host.st_mtim.tv_sec); put(68, host.st_mtim.tv_nsec);
  put(72, host.st_ctim.tv_sec); put(76, host.st_ctim.tv_nsec);
  memcpy(guest(arena, address, sizeof(data)), data, sizeof(data));
  return 0;
}
uint32_t extra_next_file = 0x70000000;
std::map<uint32_t, FILE *> extra_pipes;
struct ExtraMapping { uint32_t length; int fd; uint64_t offset; int flags; uint32_t prot; uint32_t file_bytes; };
std::map<uint32_t, ExtraMapping> extra_mappings;
std::map<uint32_t, uint32_t> extra_free_mappings{{uint32_t(MMAP_START_VMA),uint32_t(MMAP_REGION_SIZE)}};
uint32_t extra_passwd_record=0;
uint32_t extra_passwd_strings[5]={};
std::mutex extra_mapping_mutex;
std::mutex extra_passwd_mutex;
uint32_t extra_string(uint8_t *arena,const char *value,unsigned slot){return copy_string(arena,value,slot);}
int extra_flush_mapping(uint8_t *arena,uint32_t address,uint32_t length,int fd,uint64_t offset) {
  uint32_t done=0;
  while(done<length) {
    ssize_t n=pwrite(fd,guest(arena,address+done,length-done),length-done,off_t(offset+done));
    if(n<0){if(errno==EINTR)continue;return -1;}
    if(!n){errno=EIO;return -1;}
    done+=uint32_t(n);
  }
  return 0;
}
void extra_iovec(uint8_t *arena, uint32_t address, uint32_t count,
                 std::vector<struct iovec> &host) {
  if (count > 1024) { errno = EINVAL; return; }
  const uint8_t *data = count ? static_cast<const uint8_t *>(guest(arena, address, uint64_t(count) * 8)) : nullptr;
  host.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t base, len;
    memcpy(&base, data + i * 8, 4); memcpy(&len, data + i * 8 + 4, 4);
    host.push_back({len ? guest(arena, base, len) : nullptr, len});
  }
}
struct ExtraScandirEntry { uint32_t address; };
}

IMPORT(__fxstat) {
  CALL; struct stat st;
  int r = fstat(int32_t(call.word(1)), &st);
  if (!r) r = store_stat32_extra(arena, call.word(2), st);
  call.result(r);
}
IMPORT(fstat) {
  CALL;
  struct stat st;
  int result = fstat(int32_t(call.word(0)), &st);
  if (!result) result = store_stat32_extra(arena, call.word(1), st);
  call.result(result);
}
IMPORT(faccessat) {
  CALL;
  call.result(faccessat(int32_t(call.word(0)), text(arena, call.word(1)),
                        int32_t(call.word(2)), int32_t(call.word(3))));
}
IMPORT(kill) {
  CALL;
  const int pid = int32_t(call.word(0)), signal = int32_t(call.word(1));
  if (signal < 0 || signal >= NSIG) { errno = EINVAL; call.result(-1); return; }
  if (signal == 0) {
    if (pid == 0 || pid == getpid()) { call.result(0); return; }
    errno = ESRCH;
    call.result(-1);
    return;
  }
  errno = ENOSYS;
  call.result(-1);
}
namespace {
int store_statvfs(uint8_t *arena, uint32_t address, const struct statvfs &s,
                  bool wide) {
  uint8_t data[96] = {};
  auto put = [&](unsigned offset, uint64_t value, unsigned bytes) {
    memcpy(data + offset, &value, bytes);
  };
  if (wide) {
    put(0, s.f_bsize, 4); put(4, s.f_frsize, 4);
    put(8, s.f_blocks, 8); put(16, s.f_bfree, 8); put(24, s.f_bavail, 8);
    put(32, s.f_files, 8); put(40, s.f_ffree, 8); put(48, s.f_favail, 8);
    put(56, s.f_fsid, 4); put(64, s.f_flag, 4); put(68, s.f_namemax, 4);
    put(72, s.f_type, 4);
    memcpy(guest(arena, address, sizeof(data)), data, sizeof(data));
    return 0;
  }
  const uint64_t fields[] = {s.f_bsize, s.f_frsize, s.f_blocks, s.f_bfree,
      s.f_bavail, s.f_files, s.f_ffree, s.f_favail, s.f_fsid, s.f_flag,
      s.f_namemax, s.f_type};
  for (uint64_t field : fields)
    if (field > UINT32_MAX) { errno = EOVERFLOW; return -1; }
  put(0, fields[0], 4); put(4, fields[1], 4); put(8, fields[2], 4);
  put(12, fields[3], 4); put(16, fields[4], 4); put(20, fields[5], 4);
  put(24, fields[6], 4); put(28, fields[7], 4); put(32, fields[8], 4);
  put(40, fields[9], 4); put(44, fields[10], 4); put(48, fields[11], 4);
  memcpy(guest(arena, address, 72), data, 72);
  return 0;
}
}
IMPORT(statvfs) {
  CALL; struct statvfs result;
  int r = ::statvfs(text(arena, call.word(0)), &result);
  if (!r) r = store_statvfs(arena, call.word(1), result, false);
  call.result(r);
}
IMPORT(statvfs64) {
  CALL; struct statvfs result;
  int r = ::statvfs(text(arena, call.word(0)), &result);
  if (!r) r = store_statvfs(arena, call.word(1), result, true);
  call.result(r);
}
int fill_statfs(uint8_t *arena, uint32_t address, const struct statvfs &s) {
  const uint64_t fields[] = {s.f_bsize, s.f_blocks, s.f_bfree, s.f_bavail,
      s.f_files, s.f_ffree, s.f_namemax, s.f_frsize, s.f_flag};
  for (uint64_t value : fields)
    if (value > UINT32_MAX) { errno = EOVERFLOW; return -1; }
  GuestStatfs32 guest_stat{};
  const uint64_t fsid = s.f_fsid;
  guest_stat.fsid[0] = uint32_t(fsid);
  guest_stat.fsid[1] = uint32_t(fsid >> 32);
  guest_stat.type = int32_t(s.f_type);
  guest_stat.blocks = uint32_t(s.f_blocks);
  guest_stat.blocks_free = uint32_t(s.f_bfree);
  guest_stat.blocks_available = uint32_t(s.f_bavail);
  guest_stat.files = uint32_t(s.f_files);
  guest_stat.files_free = uint32_t(s.f_ffree);
  guest_stat.name_length = uint32_t(s.f_namemax);
  guest_stat.fragment_size = uint32_t(s.f_frsize);
  guest_stat.flags = uint32_t(s.f_flag);
  memcpy(guest(arena, address, sizeof(guest_stat)), &guest_stat, sizeof(guest_stat));
  return 0;
}
IMPORT(statfs) {
  CALL; struct statvfs result;
  int r = ::statvfs(text(arena, call.word(0)), &result);
  if (!r) r = fill_statfs(arena, call.word(1), result);
  call.result(r);
}
IMPORT(statfs64) {
  CALL;
  struct statvfs result;
  int r = ::statvfs(text(arena, call.word(0)), &result);
  if (!r) {
    uint8_t data[guest_statfs64_size] = {};
    const int32_t type = int32_t(result.f_type);
    const int32_t block_size = int32_t(result.f_bsize);
    memcpy(data, &type, 4);
    memcpy(data + 4, &block_size, 4);
    const uint64_t counts[] = {result.f_blocks, result.f_bfree, result.f_bavail,
                               result.f_files, result.f_ffree};
    memcpy(data + 8, counts, sizeof(counts));
    const uint64_t fsid = result.f_fsid;
    const uint32_t fields[] = {uint32_t(fsid), uint32_t(fsid >> 32),
                               uint32_t(result.f_namemax), uint32_t(result.f_frsize),
                               uint32_t(result.f_flag)};
    memcpy(data + 48, fields, sizeof(fields));
    memcpy(guest(arena, call.word(1), sizeof(data)), data, sizeof(data));
  }
  call.result(r);
}
IMPORT(ftruncate) {
  CALL;
  call.result(ftruncate(int32_t(call.word(0)), int32_t(call.word(1))));
}
IMPORT(ftruncate64) {
  CALL;
  call.result(ftruncate(int32_t(call.word(0)), int64_t(call.wide(1))));
}
IMPORT(__lxstat) {
  CALL; struct stat st; int r = lstat(text(arena, call.word(1)), &st);
  if (!r) r = store_stat32_extra(arena, call.word(2), st);
  call.result(r);
}
IMPORT(__xstat) {
  CALL; struct stat st; int r = stat(text(arena, call.word(1)), &st);
  if (!r) r = store_stat32_extra(arena, call.word(2), st);
  call.result(r);
}
IMPORT(__xmknod) {
  CALL;dev_t dev=0;
  if(call.word(3))memcpy(&dev,guest(arena,call.word(3),sizeof(dev)),sizeof(dev));
  call.result(mknod(text(arena,call.word(1)),call.word(2),dev));
}
IMPORT(__xpg_basename) {
  CALL; uint32_t a=call.word(0);char *s=static_cast<char *>(guest(arena,a,1));
  if(!*s){call.result(copy_string(arena,".",18));return;}
  char *end=s+strlen(s);while(end>s+1&&end[-1]=='/')--end;
  char *begin=end;while(begin>s&&begin[-1]!='/')--begin;
  if(begin==end){s[0]='/';s[1]=0;call.result(a);return;}
  *end=0;call.result(a+uint32_t(begin-s));
}
IMPORT(alphasort64) {
  CALL; uint32_t a=call.word(0),b=call.word(1),first,second;
  memcpy(&first,guest(arena,a,4),4);memcpy(&second,guest(arena,b,4),4);
  call.result(strcoll(static_cast<const char *>(guest(arena,first+19,1)),
                      static_cast<const char *>(guest(arena,second+19,1))));
}
IMPORT(ctermid) {
  CALL;uint32_t address=call.word(0);
  if(address){const char *s="/dev/tty";memcpy(guest(arena,address,strlen(s)+1),s,strlen(s)+1);call.result(address);}
  else call.result(extra_string(arena,"/dev/tty",10));
}
IMPORT(dup) { CALL; call.result(dup(int32_t(call.word(0)))); }
IMPORT(dup2) { CALL; call.result(dup2(int32_t(call.word(0)), int32_t(call.word(1)))); }
IMPORT(renameat2) {
  CALL;
  const int flags = int32_t(call.word(4));
  if (flags) { errno = EINVAL; call.result(-1); return; }
  call.result(renameat(int32_t(call.word(0)), text(arena, call.word(1)),
                      int32_t(call.word(2)), text(arena, call.word(3))));
}
IMPORT(fcntl) {
  CALL;const int fd=int32_t(call.word(0)),cmd=int32_t(call.word(1));uint32_t arg=call.word(2);
  int r=-1;
  if(cmd==F_GETLK||cmd==F_SETLK||cmd==F_SETLKW) {
    auto *p=static_cast<uint8_t *>(guest(arena,arg,16));struct flock f{};
    int16_t type,whence;int32_t start,len,pid;
    memcpy(&type,p,2);memcpy(&whence,p+2,2);memcpy(&start,p+4,4);memcpy(&len,p+8,4);memcpy(&pid,p+12,4);
    f.l_type=type;f.l_whence=whence;f.l_start=start;f.l_len=len;f.l_pid=pid;
    r=fcntl(fd,cmd,&f);
    if(cmd==F_GETLK&&r!=-1) {
      if(f.l_start<INT32_MIN||f.l_start>INT32_MAX||f.l_len<INT32_MIN||f.l_len>INT32_MAX){errno=EOVERFLOW;r=-1;}
      else {type=f.l_type;whence=f.l_whence;start=int32_t(f.l_start);len=int32_t(f.l_len);pid=f.l_pid;
        memcpy(p,&type,2);memcpy(p+2,&whence,2);memcpy(p+4,&start,4);memcpy(p+8,&len,4);memcpy(p+12,&pid,4);}
    }
  }
  else if(cmd==12||cmd==13||cmd==14) {
    auto *p=static_cast<uint8_t *>(guest(arena,arg,24));struct flock f{};
    int16_t type,whence;int64_t start,len;int32_t pid;
    memcpy(&type,p,2);memcpy(&whence,p+2,2);memcpy(&start,p+4,8);memcpy(&len,p+12,8);memcpy(&pid,p+20,4);
    f.l_type=type;f.l_whence=whence;f.l_start=start;f.l_len=len;f.l_pid=pid;
    const int hostcmd=cmd==12?F_GETLK:cmd==13?F_SETLK:F_SETLKW;
    r=fcntl(fd,hostcmd,&f);
    if(cmd==12&&r!=-1){type=f.l_type;whence=f.l_whence;start=f.l_start;len=f.l_len;pid=f.l_pid;
      memcpy(p,&type,2);memcpy(p+2,&whence,2);memcpy(p+4,&start,8);memcpy(p+12,&len,8);memcpy(p+20,&pid,4);}
  }
#ifdef F_GETOWN_EX
  else if(cmd==F_GETOWN_EX||cmd==F_SETOWN_EX) {
    auto *p=static_cast<uint8_t *>(guest(arena,arg,8));struct f_owner_ex owner{};
    if(cmd==F_SETOWN_EX){memcpy(&owner.type,p,4);memcpy(&owner.pid,p+4,4);}
    r=fcntl(fd,cmd,&owner);
    if(cmd==F_GETOWN_EX){memcpy(p,&owner.type,4);memcpy(p+4,&owner.pid,4);}
  }
#endif
  else {
    switch(cmd) {
      case F_GETFD:case F_GETFL:case F_GETOWN:r=fcntl(fd,cmd);break;
      case F_DUPFD:case F_SETFD:case F_SETFL:case F_SETOWN:
#ifdef F_DUPFD_CLOEXEC
      case F_DUPFD_CLOEXEC:
#endif
#ifdef F_SETSIG
      case F_SETSIG:
#endif
#ifdef F_GETSIG
      case F_GETSIG:
#endif
#ifdef F_SETLEASE
      case F_SETLEASE:
#endif
#ifdef F_GETLEASE
      case F_GETLEASE:
#endif
#ifdef F_NOTIFY
      case F_NOTIFY:
#endif
#ifdef F_SETPIPE_SZ
      case F_SETPIPE_SZ:
#endif
#ifdef F_GETPIPE_SZ
      case F_GETPIPE_SZ:
#endif
        r=fcntl(fd,cmd,int32_t(arg));break;
      default:errno=EINVAL;r=-1;break;
    }
  }
  call.result(r);
}
IMPORT(fileno_unlocked) { CALL; call.result(fileno(stream(call.word(0)))); }
IMPORT(flock) {
  CALL;
#ifdef __EMSCRIPTEN__
  errno=ENOSYS;call.result(-1);
#else
  call.result(flock(int32_t(call.word(0)),int32_t(call.word(1))));
#endif
}
IMPORT(getegid) { CALL; call.result(getegid()); }
IMPORT(geteuid) { CALL; call.result(geteuid()); }
IMPORT(getgid) { CALL; call.result(getgid()); }
IMPORT(getuid) { CALL; call.result(getuid()); }
IMPORT(getpwuid) {
  CALL;std::lock_guard<std::mutex> lock(extra_passwd_mutex);struct passwd *p=getpwuid(call.word(0));
  if(!p){call.result(0);return;}
  uint32_t fresh[5]={};const char *src[5]={p->pw_name,p->pw_passwd,p->pw_gecos,p->pw_dir,p->pw_shell};
  for(unsigned i=0;i<5;++i)if(src[i]&&!(fresh[i]=owned_string(arena,src[i]))) {
    for(unsigned j=0;j<i;++j)if(fresh[j])release(fresh[j]);
    call.result(0);return;
  }
  if(!extra_passwd_record)extra_passwd_record=allocate(28);
  if(!extra_passwd_record){for(uint32_t a:fresh)if(a)release(a);call.result(0);return;}
  uint32_t data[7]={fresh[0],fresh[1],uint32_t(p->pw_uid),uint32_t(p->pw_gid),fresh[2],fresh[3],fresh[4]};
  memcpy(guest(arena,extra_passwd_record,sizeof(data)),data,sizeof(data));
  for(uint32_t a:extra_passwd_strings)if(a)release(a);
  memcpy(extra_passwd_strings,fresh,sizeof(fresh));call.result(extra_passwd_record);
}
IMPORT(getrusage) {
  CALL; struct rusage u; int r = getrusage(int32_t(call.word(0)), &u);
  if (!r) {
    int64_t raw[18]={int64_t(u.ru_utime.tv_sec),int64_t(u.ru_utime.tv_usec),
      int64_t(u.ru_stime.tv_sec),int64_t(u.ru_stime.tv_usec),int64_t(u.ru_maxrss),
      int64_t(u.ru_ixrss),int64_t(u.ru_idrss),int64_t(u.ru_isrss),int64_t(u.ru_minflt),
      int64_t(u.ru_majflt),int64_t(u.ru_nswap),int64_t(u.ru_inblock),int64_t(u.ru_oublock),
      int64_t(u.ru_msgsnd),int64_t(u.ru_msgrcv),int64_t(u.ru_nsignals),
      int64_t(u.ru_nvcsw),int64_t(u.ru_nivcsw)};
    int32_t vals[18];
    for(unsigned i=0;i<18;++i) {
      if(raw[i]<INT32_MIN||raw[i]>INT32_MAX){errno=EOVERFLOW;r=-1;break;}
      vals[i]=int32_t(raw[i]);
    }
    if(!r){uint8_t d[72]={};memcpy(d,vals,sizeof(vals));memcpy(guest(arena,call.word(1),sizeof(d)),d,sizeof(d));}
  }
  call.result(r);
}
IMPORT(getrlimit) {
  CALL;
  const uint32_t address = call.word(1);
  if (!address) { errno = EFAULT; call.result(-1); return; }
  struct rlimit limits;
  const int result = ::getrlimit(int32_t(call.word(0)), &limits);
  if (result == 0) {
    const auto guest_limit = [](rlim_t value) {
      return value == RLIM_INFINITY || value > UINT32_MAX
          ? UINT32_MAX : uint32_t(value);
    };
    uint32_t values[2] = {guest_limit(limits.rlim_cur), guest_limit(limits.rlim_max)};
    memcpy(guest(arena, address, sizeof(values)), values, sizeof(values));
  }
  call.result(result);
}
IMPORT(setrlimit) {
  CALL;
  const uint32_t address = call.word(1);
  if (!address) { errno = EFAULT; call.result(-1); return; }
  uint32_t values[2];
  memcpy(values, guest(arena, address, sizeof(values)), sizeof(values));
  struct rlimit limits = {
    values[0] == UINT32_MAX ? RLIM_INFINITY : rlim_t(values[0]),
    values[1] == UINT32_MAX ? RLIM_INFINITY : rlim_t(values[1])
  };
  call.result(::setrlimit(int32_t(call.word(0)), &limits));
}
IMPORT(isatty) { CALL; call.result(isatty(int32_t(call.word(0)))); }
IMPORT(mallinfo) {
  CALL;call.return_argument_bytes=4;uint32_t address=call.word(0);
  int32_t d[10]={};std::lock_guard<std::recursive_mutex> lock(allocation_mutex);
  uint64_t total=0,used=0,free=0;int32_t free_blocks=0;
  for(const auto &entry:allocations){total+=entry.second.size;if(entry.second.free){free+=entry.second.size;++free_blocks;}else used+=entry.second.size;}
  d[0]=int32_t(total);d[1]=free_blocks;d[7]=int32_t(used);d[8]=int32_t(free);
  if(!allocations.empty()&&allocations.rbegin()->second.free)d[9]=int32_t(allocations.rbegin()->second.size);
  memcpy(guest(arena,address,sizeof(d)),d,sizeof(d));call.result(address);
}
IMPORT(malloc_usable_size) {
  CALL; uint32_t a=call.word(0); std::lock_guard<std::recursive_mutex> lock(allocation_mutex);
  auto it=allocations.find(a); call.result(it != allocations.end() && !it->second.free ? it->second.size : 0);
}
IMPORT(memalign) {
  CALL; uint32_t align=call.word(0), size=call.word(1);
  if (!align || (align&(align-1))) { errno=EINVAL; call.result(0); return; }
  call.result(allocate_aligned(size,std::max(align,16u)));
}
IMPORT(mkfifo) { CALL; call.result(mkfifo(text(arena,call.word(0)),call.word(1))); }
namespace {
void extra_mmap(Call &call, bool wide_offset) {
  uint8_t *arena = call.arena;
  const uint32_t requested=call.word(0),length=call.word(1),prot=call.word(2);
  const int flags=int32_t(call.word(3)),fd=int32_t(call.word(4));
  const uint64_t offset=wide_offset?call.wide(5):call.word(5);
  std::lock_guard<std::mutex> mapping_lock(extra_mapping_mutex);
  constexpr uint32_t page=4096,map_fixed_noreplace=0x100000;
  constexpr uint32_t supported_map_flags=MAP_SHARED|MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS|map_fixed_noreplace;
  const uint32_t type=uint32_t(flags)&(MAP_SHARED|MAP_PRIVATE);
  if(!length||(offset&(page-1))||offset>uint64_t(INT64_MAX)||
     (prot&~uint32_t(PROT_READ|PROT_WRITE|PROT_EXEC))||
     (type!=MAP_SHARED&&type!=MAP_PRIVATE&&type!=3)||
     (uint32_t(flags)&~supported_map_flags)||
     ((flags&MAP_ANONYMOUS)&&offset)||((flags&MAP_FIXED)&&(uint32_t(flags)&map_fixed_noreplace))||
     (!(flags&MAP_ANONYMOUS)&&offset+uint64_t(length)>uint64_t(INT64_MAX))) {
    errno=EINVAL;call.result(UINT32_MAX);return;
  }
  const uint64_t rounded64=(uint64_t(length)+page-1)&~uint64_t(page-1);
  if(rounded64>MMAP_REGION_SIZE){errno=ENOMEM;call.result(UINT32_MAX);return;}
  const uint32_t rounded=uint32_t(rounded64);
  const bool noreplace=(uint32_t(flags)&map_fixed_noreplace)!=0;
  const bool fixed=(flags&MAP_FIXED)!=0||noreplace;
  int owned_fd=(flags&MAP_ANONYMOUS)?-1:dup(fd);uint32_t file_bytes=0;
  if(!(flags&MAP_ANONYMOUS)) {
    if(owned_fd<0){call.result(UINT32_MAX);return;}
    struct stat backing{};
    if(fstat(owned_fd,&backing)<0){int e=errno;close(owned_fd);errno=e;call.result(UINT32_MAX);return;}
    if(backing.st_size>0&&uint64_t(backing.st_size)>offset)
      file_bytes=uint32_t(std::min<uint64_t>(length,uint64_t(backing.st_size)-offset));
  }
  auto add_free=[&](uint32_t b,uint32_t n) {
    if(!n)return;
    auto p=extra_free_mappings.lower_bound(b);
    if(p!=extra_free_mappings.begin()){auto q=std::prev(p);if(uint64_t(q->first)+q->second==b){b=q->first;n+=q->second;extra_free_mappings.erase(q);}}
    p=extra_free_mappings.lower_bound(b);
    if(p!=extra_free_mappings.end()&&uint64_t(b)+n==p->first){n+=p->second;extra_free_mappings.erase(p);}
    extra_free_mappings.emplace(b,n);
  };
  uint32_t address=0;
  if(fixed) {
    address=requested;const uint64_t end=uint64_t(address)+rounded;
    if(address%page||address<MMAP_START_VMA||end>MMAP_END_VMA){if(owned_fd>=0)close(owned_fd);errno=EINVAL;call.result(UINT32_MAX);return;}
    auto f=extra_free_mappings.upper_bound(address);
    if(f!=extra_free_mappings.begin())--f;
    const bool entirely_free=f!=extra_free_mappings.end()&&address>=f->first&&end<=uint64_t(f->first)+f->second;
    if(noreplace&&!entirely_free){if(owned_fd>=0)close(owned_fd);errno=EEXIST;call.result(UINT32_MAX);return;}
    if(noreplace) {
      uint32_t b=f->first,n=f->second;extra_free_mappings.erase(f);
      if(address>b)extra_free_mappings.emplace(b,address-b);
      if(end<uint64_t(b)+n)extra_free_mappings.emplace(uint32_t(end),uint32_t(uint64_t(b)+n-end));
    } else {
      auto it=extra_mappings.lower_bound(address);if(it!=extra_mappings.begin())--it;
      while(it!=extra_mappings.end()) {
        uint32_t b=it->first;ExtraMapping m=it->second;uint64_t e=uint64_t(b)+m.length;
        if(e<=address){++it;continue;}if(b>=end)break;
        const uint32_t prefix=b<address?address-b:0,tail=uint32_t(e>end?e-end:0);
        int prefix_fd=prefix&&m.fd>=0?dup(m.fd):-1,tail_fd=tail&&m.fd>=0?dup(m.fd):-1;
        if((prefix&&m.fd>=0&&prefix_fd<0)||(tail&&m.fd>=0&&tail_fd<0)){
          if(prefix_fd>=0)close(prefix_fd);if(tail_fd>=0)close(tail_fd);if(owned_fd>=0)close(owned_fd);
          call.result(UINT32_MAX);return;
        }
        if((m.flags&MAP_SHARED)&&(m.prot&PROT_WRITE)&&m.fd>=0&&m.file_bytes&&
           extra_flush_mapping(arena,b,m.file_bytes,m.fd,m.offset)<0) {
          if(prefix_fd>=0)close(prefix_fd);if(tail_fd>=0)close(tail_fd);if(owned_fd>=0)close(owned_fd);
          call.result(UINT32_MAX);return;
        }
        it=extra_mappings.erase(it);
        if(prefix)extra_mappings.emplace(b,ExtraMapping{prefix,prefix_fd,m.offset,m.flags,m.prot,std::min(prefix,m.file_bytes)});
        if(tail)extra_mappings.emplace(uint32_t(end),ExtraMapping{tail,tail_fd,m.offset+end-b,m.flags,m.prot,
            m.file_bytes>end-b?std::min(tail,uint32_t(m.file_bytes-(end-b))):0});
        if(m.fd>=0)close(m.fd);
      }
      auto p=extra_free_mappings.lower_bound(address);if(p!=extra_free_mappings.begin())--p;
      while(p!=extra_free_mappings.end()) {
        uint32_t b=p->first,n=p->second;uint64_t e=uint64_t(b)+n;
        if(e<=address){++p;continue;}if(b>=end)break;p=extra_free_mappings.erase(p);
        if(b<address)extra_free_mappings.emplace(b,address-b);
        if(e>end)extra_free_mappings.emplace(uint32_t(end),uint32_t(e-end));
      }
    }
  } else {
    const uint32_t hint_address=requested&~(page-1);
    auto hint=extra_free_mappings.upper_bound(hint_address);
    if(requested&&hint_address>=MMAP_START_VMA&&hint!=extra_free_mappings.begin()) {
      --hint;
      if(hint_address>=hint->first&&uint64_t(hint_address)+rounded<=uint64_t(hint->first)+hint->second) {
        uint32_t b=hint->first,n=hint->second;extra_free_mappings.erase(hint);
        if(hint_address>b)extra_free_mappings.emplace(b,hint_address-b);
        uint32_t after=hint_address+rounded;
        if(uint64_t(after)<uint64_t(b)+n)extra_free_mappings.emplace(after,uint32_t(uint64_t(b)+n-after));
        address=hint_address;
      }
    }
    if(!address) {
      auto p=extra_free_mappings.begin();for(;p!=extra_free_mappings.end()&&p->second<rounded;++p){}
      if(p==extra_free_mappings.end()){if(owned_fd>=0)close(owned_fd);errno=ENOMEM;call.result(UINT32_MAX);return;}
      address=p->first;uint32_t left=p->second-rounded;extra_free_mappings.erase(p);
      if(left)extra_free_mappings.emplace(address+rounded,left);
    }
  }
  memset(guest(arena,address,rounded),0,rounded);
  if(owned_fd>=0&&file_bytes) {
    ssize_t n=pread(owned_fd,guest(arena,address,file_bytes),file_bytes,off_t(offset));
    if(n<0){int e=errno;close(owned_fd);add_free(address,rounded);memset(guest(arena,address,rounded),0,rounded);errno=e;call.result(UINT32_MAX);return;}
    file_bytes=std::min(file_bytes,uint32_t(n));
  }
  extra_mappings[address]={rounded,owned_fd,offset,flags,prot,file_bytes};
  call.result(address);
}
}
IMPORT(mmap) { CALL; extra_mmap(call, false); }
IMPORT(mmap64) { CALL; extra_mmap(call, true); }
IMPORT(mount) {
  CALL;
#ifdef __EMSCRIPTEN__
  errno=ENOSYS; call.result(-1);
#else
  call.result(mount(call.word(0)?text(arena,call.word(0)):nullptr,
      call.word(1)?text(arena,call.word(1)):nullptr,
      call.word(2)?text(arena,call.word(2)):nullptr,call.word(3),
      call.word(4)?guest(arena,call.word(4),1):nullptr));
#endif
}
IMPORT(munmap) {
  std::lock_guard<std::mutex> mapping_lock(extra_mapping_mutex);
  CALL;uint32_t start=call.word(0),length=call.word(1);constexpr uint32_t page=4096;
  if(!length||start%page){errno=EINVAL;call.result(-1);return;}
  uint64_t rounded=(uint64_t(length)+page-1)&~uint64_t(page-1);
  if(rounded>UINT32_MAX||start<MMAP_START_VMA||uint64_t(start)+rounded>MMAP_END_VMA){errno=EINVAL;call.result(-1);return;}
  length=uint32_t(rounded);
  uint64_t end=uint64_t(start)+length,cursor=start;auto it=extra_mappings.lower_bound(start);
  if(it!=extra_mappings.begin())--it;
  for(auto p=it;p!=extra_mappings.end()&&cursor<end;++p) {
    uint64_t b=p->first,e=b+p->second.length;if(e<=cursor)continue;
    if(b>cursor)break;cursor=std::min<uint64_t>(e,end);
  }
  if(cursor!=end){errno=EINVAL;call.result(-1);return;}
  auto add_free=[&](uint32_t b,uint32_t n) {
    auto p=extra_free_mappings.lower_bound(b);
    if(p!=extra_free_mappings.begin()){auto q=std::prev(p);if(uint64_t(q->first)+q->second==b){b=q->first;n+=q->second;extra_free_mappings.erase(q);}}
    p=extra_free_mappings.lower_bound(b);if(p!=extra_free_mappings.end()&&uint64_t(b)+n==p->first){n+=p->second;extra_free_mappings.erase(p);}
    extra_free_mappings.emplace(b,n);
  };
  it=extra_mappings.lower_bound(start);if(it!=extra_mappings.begin())--it;
  for(auto p=it;p!=extra_mappings.end();++p) {
    uint32_t b=p->first;const ExtraMapping &m=p->second;uint64_t e=uint64_t(b)+m.length;
    if(e<=start)continue;if(b>=end)break;
    uint32_t lo=std::max(b,start),hi=uint32_t(std::min(e,end));
    uint32_t backed=lo-b<m.file_bytes?std::min(hi-lo,uint32_t(m.file_bytes-(lo-b))):0;
    if((m.flags&MAP_SHARED)&&(m.prot&PROT_WRITE)&&m.fd>=0&&backed&&extra_flush_mapping(arena,lo,backed,m.fd,m.offset+lo-b)<0){call.result(-1);return;}
  }
  it=extra_mappings.lower_bound(start);if(it!=extra_mappings.begin())--it;
  while(it!=extra_mappings.end()) {
    uint32_t b=it->first;ExtraMapping m=it->second;uint64_t e=uint64_t(b)+m.length;
    if(e<=start){++it;continue;}if(b>=end)break;
    uint32_t lo=std::max(b,start),hi=uint32_t(std::min(e,end));
    uint32_t prefix=lo-b,tail=uint32_t(e-hi),skip=hi-b;
    int prefix_fd=prefix&&m.fd>=0?dup(m.fd):-1,tail_fd=tail&&m.fd>=0?dup(m.fd):-1;
    if((prefix&&m.fd>=0&&prefix_fd<0)||(tail&&m.fd>=0&&tail_fd<0)){
      if(prefix_fd>=0)close(prefix_fd);if(tail_fd>=0)close(tail_fd);call.result(-1);return;
    }
    it=extra_mappings.erase(it);
    if(prefix)extra_mappings.emplace(b,ExtraMapping{prefix,prefix_fd,m.offset,m.flags,m.prot,std::min(prefix,m.file_bytes)});
    if(tail)extra_mappings.emplace(hi,ExtraMapping{tail,tail_fd,m.offset+skip,m.flags,m.prot,m.file_bytes>skip?std::min(tail,uint32_t(m.file_bytes-skip)):0});
    if(m.fd>=0)close(m.fd);
  }
  add_free(start,length);memset(guest(arena,start,length),0,length);call.result(0);
}
// WebAssembly linear memory has no per-page protection; retain mapping metadata only.
IMPORT(mprotect) {
  std::lock_guard<std::mutex> mapping_lock(extra_mapping_mutex);
  CALL;
  constexpr uint32_t page = 4096;
  const uint32_t start = call.word(0), length = call.word(1), prot = call.word(2);
  const uint64_t raw_end = uint64_t(start) + length;
  const uint64_t end = (raw_end + page - 1) & ~uint64_t(page - 1);
  if (!length || start % page || (prot & ~uint32_t(PROT_READ | PROT_WRITE | PROT_EXEC)) ||
      start < MMAP_START_VMA || end > MMAP_END_VMA) {
    errno = EINVAL;
    call.result(-1);
    return;
  }

  uint64_t cursor = start;
  auto it = extra_mappings.upper_bound(start);
  if (it != extra_mappings.begin()) --it;
  for (; it != extra_mappings.end() && cursor < end; ++it) {
    const uint64_t base = it->first, limit = base + it->second.length;
    if (limit <= cursor) continue;
    if (base > cursor) break;
    cursor = std::min(end, limit);
  }
  if (cursor != end) {
    errno = ENOMEM;
    call.result(-1);
    return;
  }

  std::vector<uint32_t> replaced;
  std::vector<std::pair<uint32_t, ExtraMapping>> pieces;
  auto append = [&](uint32_t base, uint32_t size, const ExtraMapping &mapping,
                    uint32_t skip, uint32_t protection) {
    if (!size) return true;
    const int fd = mapping.fd >= 0 ? dup(mapping.fd) : -1;
    if (mapping.fd >= 0 && fd < 0) return false;
    const uint32_t file_bytes = mapping.file_bytes > skip
        ? std::min(size, uint32_t(mapping.file_bytes - skip)) : 0;
    pieces.emplace_back(base, ExtraMapping{size, fd, mapping.offset + skip,
                                            mapping.flags, protection, file_bytes});
    return true;
  };
  auto first = extra_mappings.upper_bound(start);
  if (first != extra_mappings.begin()) --first;
  for (it = first; it != extra_mappings.end(); ++it) {
    const uint32_t base = it->first;
    const ExtraMapping mapping = it->second;
    const uint64_t limit = uint64_t(base) + mapping.length;
    if (limit <= start) continue;
    if (base >= end) break;
    const uint32_t lo = std::max(base, start), hi = uint32_t(std::min(limit, end));
    const uint32_t prefix = lo - base, middle = hi - lo, tail = uint32_t(limit - hi);
    replaced.push_back(base);
    if (!append(base, prefix, mapping, 0, mapping.prot) ||
        !append(lo, middle, mapping, prefix, prot) ||
        !append(hi, tail, mapping, hi - base, mapping.prot)) {
      for (const auto &piece : pieces) if (piece.second.fd >= 0) close(piece.second.fd);
      call.result(-1);
      return;
    }
  }
  for (uint32_t base : replaced) {
    auto found = extra_mappings.find(base);
    if (found->second.fd >= 0) close(found->second.fd);
    extra_mappings.erase(found);
  }
  for (auto &piece : pieces) extra_mappings.emplace(piece.first, piece.second);
  call.result(0);
}
IMPORT(popen) {
  CALL;
#ifdef __EMSCRIPTEN__
  errno=ENOSYS; call.result(0);
#else
  FILE *f=popen(text(arena,call.word(0)),text(arena,call.word(1)));
  if (!f) { call.result(0); return; }
  uint32_t h=extra_next_file++;
  if (h < 0x70000000) { pclose(f); errno=EMFILE; call.result(0); return; }
  extra_pipes.emplace(h,f); files.emplace(h,f); call.result(h);
#endif
}
IMPORT(pclose) {
  CALL; uint32_t h=call.word(0); auto it=extra_pipes.find(h);
  if (it==extra_pipes.end()) { errno=EBADF; call.result(-1); return; }
#ifdef __EMSCRIPTEN__
  errno=ENOSYS; call.result(-1);
#else
  int r=pclose(it->second); extra_pipes.erase(it); files.erase(h); call.result(r);
#endif
}
IMPORT(scandir) {
  CALL;const char *path=text(arena,call.word(0));uint32_t out=call.word(1),filter=call.word(2),cmp=call.word(3);
  DIR *dir=opendir(path);if(!dir){call.result(-1);return;}
  std::vector<ExtraScandirEntry> entries;errno=0;
  auto discard=[&](){for(auto &e:entries)release(e.address);entries.clear();};
  dirent *e=nullptr;
  while((errno=0,(e=readdir(dir))!=nullptr)) {
    if(uint64_t(e->d_ino)>UINT32_MAX||e->d_off<INT32_MIN||e->d_off>INT32_MAX){
      closedir(dir);discard();errno=EOVERFLOW;call.result(-1);return;
    }
    uint32_t a=allocate(268);if(!a){closedir(dir);discard();call.result(-1);return;}
    uint8_t d[268]={};uint32_t ino=uint32_t(e->d_ino),off=uint32_t(e->d_off);
    uint16_t reclen=uint16_t(11+strlen(e->d_name)+1);
    memcpy(d,&ino,4);memcpy(d+4,&off,4);memcpy(d+8,&reclen,2);d[10]=e->d_type;
    memcpy(d+11,e->d_name,strlen(e->d_name)+1);memcpy(guest(arena,a,sizeof(d)),d,sizeof(d));
    if(filter){uint32_t args[]={a};invoke(arena,state,runtime,filter,args,1);if(!state->gpr.rax.dword){release(a);continue;}}
    entries.push_back({a});
  }
  int err=errno;closedir(dir);if(err){discard();errno=err;call.result(-1);return;}
  uint32_t sort_args=cmp?allocate(8):0;
  if(cmp&&!sort_args){discard();call.result(-1);return;}
  if(cmp)std::sort(entries.begin(),entries.end(),[&](const auto &a,const auto &b){
    uint32_t pair[2]={a.address,b.address};memcpy(guest(arena,sort_args,8),pair,8);
    uint32_t args[]={sort_args,sort_args+4};invoke(arena,state,runtime,cmp,args,2);
    return int32_t(state->gpr.rax.dword)<0;
  });
  if(sort_args)release(sort_args);
  uint32_t list=entries.empty()?0:allocate(uint32_t(entries.size()*4));
  if(entries.size()&&!list){discard();call.result(-1);return;}
  for(size_t i=0;i<entries.size();++i)memcpy(guest(arena,list+uint32_t(i*4),4),&entries[i].address,4);
  int n=int(entries.size());memcpy(guest(arena,out,4),&list,4);call.result(n);
}
IMPORT(scandir64) {
  CALL;const char *path=text(arena,call.word(0));uint32_t out=call.word(1),filter=call.word(2),cmp=call.word(3);
  DIR *dir=opendir(path);if(!dir){call.result(-1);return;}
  std::vector<ExtraScandirEntry> entries;errno=0;
  auto discard=[&](){for(auto &e:entries)release(e.address);entries.clear();};
  dirent *e=nullptr;
  while((errno=0,(e=readdir(dir))!=nullptr)) {
    uint32_t a=allocate(276);if(!a){closedir(dir);discard();call.result(-1);return;}
    uint8_t d[276]={};uint64_t ino=e->d_ino;int64_t off=e->d_off;
    uint16_t reclen=uint16_t((19+strlen(e->d_name)+1+7)&~size_t(7));
    memcpy(d,&ino,8);memcpy(d+8,&off,8);memcpy(d+16,&reclen,2);d[18]=e->d_type;
    memcpy(d+19,e->d_name,strlen(e->d_name)+1);memcpy(guest(arena,a,sizeof(d)),d,sizeof(d));
    if(filter){uint32_t args[]={a};invoke(arena,state,runtime,filter,args,1);if(!state->gpr.rax.dword){release(a);continue;}}
    entries.push_back({a});
  }
  int err=errno;closedir(dir);if(err){discard();errno=err;call.result(-1);return;}
  uint32_t sort_args=cmp?allocate(8):0;
  if(cmp&&!sort_args){discard();call.result(-1);return;}
  if(cmp)std::sort(entries.begin(),entries.end(),[&](const auto &a,const auto &b){
    uint32_t pair[2]={a.address,b.address};memcpy(guest(arena,sort_args,8),pair,8);
    uint32_t args[]={sort_args,sort_args+4};invoke(arena,state,runtime,cmp,args,2);
    return int32_t(state->gpr.rax.dword)<0;
  });
  if(sort_args)release(sort_args);
  uint32_t list=entries.empty()?0:allocate(uint32_t(entries.size()*4));
  if(entries.size()&&!list){discard();call.result(-1);return;}
  for(size_t i=0;i<entries.size();++i)memcpy(guest(arena,list+uint32_t(i*4),4),&entries[i].address,4);
  int n=int(entries.size());memcpy(guest(arena,out,4),&list,4);call.result(n);
}
IMPORT(setbuf) {
  CALL; FILE *f=stream(call.word(0)); uint32_t b=call.word(1);
  setbuf(f,b?static_cast<char *>(guest(arena,b,BUFSIZ)):nullptr);
}
IMPORT(setbuffer) {
  CALL;
  FILE *file = stream(call.word(0));
  const uint32_t address = call.word(1), size = call.word(2);
  setvbuf(file, address ? static_cast<char *>(guest(arena, address, size)) : nullptr,
          address ? _IOFBF : _IONBF, size);
}
IMPORT(shm_open) {
  CALL;
#ifdef __EMSCRIPTEN__
  errno=ENOSYS; call.result(-1);
#else
  call.result(shm_open(text(arena,call.word(0)),int32_t(call.word(1)),call.word(2)));
#endif
}
IMPORT(system) {
  CALL;
#ifdef __EMSCRIPTEN__
  if(!call.word(0)){call.result(0);return;}
  errno=ENOSYS; call.result(-1);
#else
  call.result(system(call.word(0)?text(arena,call.word(0)):nullptr));
#endif
}
IMPORT(tcgetattr) {
  CALL; struct termios t; int r=tcgetattr(int32_t(call.word(0)),&t);
  if(!r) {
    uint8_t d[60]={}; uint32_t v[4]={t.c_iflag,t.c_oflag,t.c_cflag,t.c_lflag};
    memcpy(d,v,16); memcpy(d+17,t.c_cc,std::min(sizeof(t.c_cc),size_t(32)));
    uint32_t is=cfgetispeed(&t),os=cfgetospeed(&t);
    memcpy(d+52,&is,4);memcpy(d+56,&os,4);memcpy(guest(arena,call.word(1),60),d,60);
  }
  call.result(r);
}
IMPORT(tcsetattr) {
  CALL; uint8_t d[60];memcpy(d,guest(arena,call.word(2),60),60);struct termios t{};
  memcpy(&t.c_iflag,d,4);memcpy(&t.c_oflag,d+4,4);memcpy(&t.c_cflag,d+8,4);
  memcpy(&t.c_lflag,d+12,4);memcpy(t.c_cc,d+17,std::min(sizeof(t.c_cc),size_t(32)));
  uint32_t is,os;memcpy(&is,d+52,4);memcpy(&os,d+56,4);
  if(cfsetispeed(&t,is)<0||cfsetospeed(&t,os)<0){call.result(-1);return;}
  call.result(tcsetattr(int32_t(call.word(0)),int32_t(call.word(1)),&t));
}
IMPORT(tmpnam) {
  CALL;char name[L_tmpnam];
  if(!::tmpnam(name)){call.result(0);return;}
  uint32_t address=call.word(0);
  if(address){memcpy(guest(arena,address,L_tmpnam),name,strlen(name)+1);call.result(address);}
  else call.result(owned_string(arena,name));
}
IMPORT(tempnam) {
  CALL;
  char *name = ::tempnam(call.word(0) ? text(arena, call.word(0)) : nullptr,
                         call.word(1) ? text(arena, call.word(1)) : nullptr);
  const uint32_t result = owned_string(arena, name);
  free(name);
  call.result(result);
}
int guest_mkstemp(uint8_t *arena, uint32_t address) {
  std::string pattern(text(arena, address));
  const int fd = ::mkstemp(pattern.data());
  if (fd >= 0) memcpy(guest(arena, address, pattern.size() + 1),
                       pattern.c_str(), pattern.size() + 1);
  return fd;
}
IMPORT(mkstemp) { CALL; call.result(guest_mkstemp(arena, call.word(0))); }
IMPORT(mkstemp64) { CALL; call.result(guest_mkstemp(arena, call.word(0))); }
IMPORT(mkdtemp) {
  CALL;
  const uint32_t address = call.word(0);
  char *pattern = static_cast<char *>(guest(arena, address, strlen(text(arena, address)) + 1));
  call.result(::mkdtemp(pattern) ? address : 0);
}
IMPORT(writev) {
  CALL; std::vector<struct iovec> v;
  if (call.word(2)>1024) { errno=EINVAL; call.result(-1); return; }
  extra_iovec(arena,call.word(1),call.word(2),v);
  ssize_t r=writev(int32_t(call.word(0)),v.data(),int(v.size()));call.result(r);
}
IMPORT(dl_iterate_phdr) {
  CALL;uint32_t callback=call.word(0),opaque=call.word(1);
  if(!callback){errno=EINVAL;call.result(-1);return;}
  if(_ecv_e_phent!=sizeof(Elf32_Phdr)||_ecv_e_phnum>UINT16_MAX){errno=ENOEXEC;call.result(-1);return;}
  uint32_t phdr_size=uint32_t(_ecv_e_phnum*sizeof(Elf32_Phdr));
  uint32_t phdrs=allocate(phdr_size),info=allocate(32),name=copy_string(arena,"",19);
  if((phdr_size&&!phdrs)||!info||!name){if(phdrs)release(phdrs);if(info)release(info);errno=ENOMEM;call.result(-1);return;}
  if(phdr_size)memcpy(guest(arena,phdrs,phdr_size),_ecv_e_ph,phdr_size);
  uint8_t d[32]={};memcpy(d+4,&name,4);memcpy(d+8,&phdrs,4);
  uint16_t count=uint16_t(_ecv_e_phnum);memcpy(d+12,&count,2);
  memcpy(guest(arena,info,sizeof(d)),d,sizeof(d));
  uint32_t args[]={info,32,opaque};invoke(arena,state,runtime,callback,args,3);
  int result=int32_t(state->gpr.rax.dword);release(info);if(phdrs)release(phdrs);call.result(result);
}
IMPORT(dladdr) {
  CALL;uint32_t addr=call.word(0),info=call.word(1);
  if(!info){errno=EFAULT;call.result(0);return;}
  uint32_t base=UINT32_MAX,sym=0,sym_addr=0;bool mapped=false;
  for(uint64_t i=0;i<_ecv_e_phnum;++i){
    Elf32_Phdr p;memcpy(&p,_ecv_e_ph+i*_ecv_e_phent,sizeof(p));
    if(p.p_type!=PT_LOAD)continue;
    if(p.p_vaddr<base)base=p.p_vaddr;
    if(uint64_t(addr)>=p.p_vaddr&&uint64_t(addr)<uint64_t(p.p_vaddr)+p.p_memsz)mapped=true;
  }
  if(!mapped){call.result(0);return;}
  if(base!=UINT32_MAX)base&=~0xfffu;else base=0;
  for(const auto &entry:runtime->addr_fun_symbol_map) {
    if(entry.first>addr||entry.first<sym_addr)continue;
    sym_addr=uint32_t(entry.first);sym=copy_string(arena,entry.second,16);
  }
  uint32_t d[4]={copy_string(arena,"",17),base,sym,sym?sym_addr:0};
  memcpy(guest(arena,info,sizeof(d)),d,sizeof(d));call.result(1);
}
#include <cstddef>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#ifdef __linux__
#include <linux/if_link.h>
#include <netpacket/packet.h>
#endif
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#include <sys/uio.h>
#include <termios.h>
#include <sys/utsname.h>

namespace {
std::mutex netextra_guest_alloc_mutex;
struct NetExtraOwned { void *host; std::vector<uint32_t> guest_blocks; };
std::map<uint32_t, NetExtraOwned> netextra_addrinfo_roots;
std::map<uint32_t, NetExtraOwned> netextra_ifaddrs_roots;
thread_local std::vector<uint32_t> netextra_hostent_blocks;
thread_local uint32_t netextra_hostent_root = 0;
thread_local std::vector<uint32_t> *netextra_active_blocks = nullptr;
struct NetExtraSignal {
  uint32_t arena_handler = 0;
  uint8_t *arena = nullptr;
  State *state = nullptr;
  RuntimeManager *runtime = nullptr;
  uint32_t guest_flags = 0;
  uint32_t guest_restorer = 0;
  sigset_t guest_mask{};
};
std::mutex netextra_signal_mutex;
std::map<std::pair<RuntimeManager *, int>, NetExtraSignal> netextra_signals;
thread_local volatile sig_atomic_t netextra_pending_signals[NSIG] = {};
thread_local bool netextra_dispatching_signals = false;
thread_local bool netextra_jump_saved_mask = false;
NetExtraSignal netextra_signal_get(RuntimeManager *runtime, int sig) {
  std::lock_guard<std::mutex> lock(netextra_signal_mutex);
  auto it = netextra_signals.find({runtime, sig});
  return it == netextra_signals.end() ? NetExtraSignal{} : it->second;
}
NetExtraSignal netextra_signal_set(RuntimeManager *runtime, int sig, const NetExtraSignal &entry) {
  std::lock_guard<std::mutex> lock(netextra_signal_mutex);
  auto key = std::make_pair(runtime, sig);
  auto it = netextra_signals.find(key);
  NetExtraSignal old = it == netextra_signals.end() ? NetExtraSignal{} : it->second;
  netextra_signals[key] = entry;
  return old;
}
void netextra_signal_trampoline(int number) {
  // Host async handlers only enqueue a coalesced standard signal; guest code runs at a safe point.
  if (number > 0 && number < NSIG) netextra_pending_signals[number] = 1;
}
void netextra_guest_sigset(uint8_t *arena, uint32_t address, sigset_t &result) {
  sigemptyset(&result);
  if (!address) return;
  uint32_t words[32]; memcpy(words, guest(arena, address, sizeof(words)), sizeof(words));
  for (int sig = 1; sig < NSIG && sig <= 1024; ++sig)
    if ((words[(sig - 1) >> 5] >> ((sig - 1) & 31)) & 1) sigaddset(&result, sig);
}
void netextra_store_sigset(uint8_t *arena, uint32_t address, const sigset_t &value) {
  if (!address) return;
  uint32_t words[32] = {};
  for (int sig = 1; sig < NSIG && sig <= 1024; ++sig)
    if (sigismember(&value, sig) == 1) words[(sig - 1) >> 5] |= 1u << ((sig - 1) & 31);
  memcpy(guest(arena, address, sizeof(words)), words, sizeof(words));
}
struct NetExtraGuestSigaction { uint32_t handler, mask[32], flags, restorer; };
static_assert(sizeof(NetExtraGuestSigaction) == 140 &&
              offsetof(NetExtraGuestSigaction, flags) == 132 &&
              offsetof(NetExtraGuestSigaction, restorer) == 136,
              "i386 glibc sigaction ABI mismatch");
uint32_t netextra_alloc(size_t, std::vector<uint32_t> *);
uint32_t netextra_copy_sockaddr(uint8_t *, const sockaddr *, socklen_t, std::vector<uint32_t> *);
uint32_t netextra_alloc(RuntimeManager *, size_t length, size_t = 4) {
  return netextra_alloc(length, netextra_active_blocks);
}
uint32_t netextra_copy_sockaddr(uint8_t *arena, RuntimeManager *runtime,
                                const sockaddr *address, socklen_t length) {
  return netextra_copy_sockaddr(arena, address, length, netextra_active_blocks);
}
uint32_t netextra_alloc(size_t length, std::vector<uint32_t> *blocks) {
  if (length > UINT32_MAX) { errno = ENOMEM; return 0; }
  uint32_t address = allocate(uint32_t(length));
  if (address && blocks) blocks->push_back(address);
  return address;
}



uint32_t netextra_copy_sockaddr(uint8_t *arena, const sockaddr *address, socklen_t length,
                               std::vector<uint32_t> *blocks) {
  if (!address || !length) return 0;
  uint32_t result = netextra_alloc(length, blocks);
  if (result) memcpy(guest(arena, result, length), address, length);
  return result;
}
ssize_t netextra_message(uint8_t *arena, uint32_t descriptor, uint32_t message,
                         int flags, bool receive) {
  uint32_t fields[7]; memcpy(fields, guest(arena, message, sizeof(fields)), sizeof(fields));
  const uint32_t name_address = fields[0], name_capacity = fields[1];
  const uint32_t iov_address = fields[2], iov_count = fields[3];
  const uint32_t control_address = fields[4], control_capacity = fields[5];
  // Ancillary cmsghdr has different i386/host alignment; do not pass or truncate it.
  if (iov_count > 1024 || uint64_t(iov_count) * 8 > MEMORY_ARENA_SIZE ||
      control_capacity) { errno = control_capacity ? EOPNOTSUPP : EINVAL; return -1; }
  sockaddr_storage name{};
  if (name_capacity > sizeof(name)) { errno = EINVAL; return -1; }
  if (name_address && name_capacity) memcpy(&name, guest(arena, name_address, name_capacity), name_capacity);
  std::vector<iovec> vectors(iov_count);
  for (uint32_t i = 0; i < iov_count; ++i) {
    uint32_t pair[2];
    memcpy(pair, guest(arena, iov_address + i * 8, 8), 8);
    vectors[i].iov_base = pair[1] ? guest(arena, pair[0], pair[1]) : nullptr;
    vectors[i].iov_len = pair[1];
  }
  msghdr host{};
  host.msg_name = name_address ? &name : nullptr;
  host.msg_namelen = name_address ? name_capacity : 0;
  host.msg_iov = vectors.empty() ? nullptr : vectors.data();
  host.msg_iovlen = vectors.size();
  host.msg_control = nullptr;
  host.msg_controllen = 0;
  ssize_t result = receive ? recvmsg(int32_t(descriptor), &host, flags)
                           : sendmsg(int32_t(descriptor), &host, flags);
  if (result >= 0 && receive) {
    if (name_address) {
      uint32_t copied = std::min<uint32_t>(name_capacity, host.msg_namelen);
      if (copied) memcpy(guest(arena, name_address, copied), &name, copied);
    }
    fields[1] = host.msg_namelen;
    fields[5] = uint32_t(host.msg_controllen);
    fields[6] = uint32_t(host.msg_flags);
    memcpy(guest(arena, message, sizeof(fields)), fields, sizeof(fields));
  }
  (void)control_address;
  return result;
}
bool netextra_timeval_option(int level, int option) {
  return level == SOL_SOCKET &&
      (option == SO_RCVTIMEO || option == SO_SNDTIMEO);
}
int netextra_setsockopt_guest(uint8_t *arena, int fd, int level, int option,
                              uint32_t value, int32_t length) {
  if (length < 0) { errno = EINVAL; return -1; }
  if (netextra_timeval_option(level, option)) {
    if (length != 8) { errno = EINVAL; return -1; }
    int32_t guest_time[2]; memcpy(guest_time, guest(arena, value, 8), 8);
    timeval host_time{guest_time[0], guest_time[1]};
    return setsockopt(fd, level, option, &host_time, sizeof(host_time));
  }
  return setsockopt(fd, level, option, length ? guest(arena, value, length) : nullptr, length);
}
int netextra_getsockopt_guest(uint8_t *arena, int fd, int level, int option,
                              uint32_t value, uint32_t length_address) {
  uint32_t capacity; memcpy(&capacity, guest(arena, length_address, 4), 4);
  if (netextra_timeval_option(level, option)) {
    timeval host_time{}; socklen_t host_length = sizeof(host_time);
    int result = getsockopt(fd, level, option, &host_time, &host_length);
    if (!result) {
      if (host_time.tv_sec < INT32_MIN || host_time.tv_sec > INT32_MAX) {
        errno = EOVERFLOW; return -1;
      }
      int32_t guest_time[2] = {int32_t(host_time.tv_sec), int32_t(host_time.tv_usec)};
      uint32_t copied = std::min<uint32_t>(capacity, sizeof(guest_time));
      if (copied) memcpy(guest(arena, value, copied), guest_time, copied);
      uint32_t output_length = sizeof(guest_time);
      memcpy(guest(arena, length_address, 4), &output_length, 4);
    }
    return result;
  }
  if (capacity > MEMORY_ARENA_SIZE) { errno = EINVAL; return -1; }
  std::vector<uint8_t> bytes(capacity);
  socklen_t host_length = capacity;
  int result = getsockopt(fd, level, option, capacity ? bytes.data() : nullptr, &host_length);
  if (!result) {
    uint32_t copied = std::min<uint32_t>(capacity, host_length);
    if (copied) memcpy(guest(arena, value, copied), bytes.data(), copied);
    memcpy(guest(arena, length_address, 4), &host_length, 4);
  }
  return result;
}
int netextra_accept4_guest(uint8_t *arena, int listener, uint32_t address,
                           uint32_t length_address, int flags) {
  uint32_t capacity = 0;
  if (address) {
    if (!length_address) { errno = EFAULT; return -1; }
    memcpy(&capacity, guest(arena, length_address, 4), 4);
  }
  sockaddr_storage host_address{}; socklen_t host_length = sizeof(host_address);
#ifdef __linux__
  int fd = accept4(listener, address ? reinterpret_cast<sockaddr *>(&host_address) : nullptr,
                   address ? &host_length : nullptr, flags);
#else
  int fd = accept(listener, address ? reinterpret_cast<sockaddr *>(&host_address) : nullptr,
                  address ? &host_length : nullptr);
  if (fd >= 0 && (flags & SOCK_CLOEXEC) && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    int error = errno; close(fd); errno = error; return -1;
  }
  if (fd >= 0 && (flags & SOCK_NONBLOCK) && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
    int error = errno; close(fd); errno = error; return -1;
  }
#endif
  if (fd >= 0 && address) {
    uint32_t copied = std::min<uint32_t>(capacity, host_length);
    if (copied) memcpy(guest(arena, address, copied), &host_address, copied);
    memcpy(guest(arena, length_address, 4), &host_length, 4);
  }
  return fd;
}
}

IMPORT(socket) { CALL; call.result(socket(int32_t(call.word(0)), int32_t(call.word(1)), int32_t(call.word(2)))); }
IMPORT(bind) {
  CALL;
  uint32_t length = call.word(2);
  if (length > sizeof(sockaddr_storage)) { errno = EINVAL; call.result(-1); return; }
  sockaddr_storage address{};
  if (length) memcpy(&address, guest(arena, call.word(1), length), length);
  call.result(bind(int32_t(call.word(0)), reinterpret_cast<sockaddr *>(&address), length));
}
IMPORT(connect) {
  CALL;
  uint32_t length = call.word(2);
  if (length > sizeof(sockaddr_storage)) { errno = EINVAL; call.result(-1); return; }
  sockaddr_storage address{};
  if (length) memcpy(&address, guest(arena, call.word(1), length), length);
  call.result(connect(int32_t(call.word(0)), reinterpret_cast<sockaddr *>(&address), length));
}
IMPORT(accept) {
  CALL;
  uint32_t address_pointer = call.word(1), length_pointer = call.word(2);
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  int fd = accept(int32_t(call.word(0)), address_pointer ? reinterpret_cast<sockaddr *>(&address) : nullptr,
                  address_pointer ? &length : nullptr);
  if (fd >= 0 && address_pointer) {
    uint32_t guest_length;
    memcpy(&guest_length, guest(arena, length_pointer, 4), 4);
    const uint32_t copied = std::min<uint32_t>(guest_length, length);
    if (copied) memcpy(guest(arena, address_pointer, copied), &address, copied);
    uint32_t output_length = length;
    memcpy(guest(arena, length_pointer, 4), &output_length, 4);
  }
  call.result(fd);
}
IMPORT(listen) { CALL; call.result(listen(int32_t(call.word(0)), int32_t(call.word(1)))); }
IMPORT(shutdown) { CALL; call.result(shutdown(int32_t(call.word(0)), int32_t(call.word(1)))); }
IMPORT(getsockname) {
  CALL;
  uint32_t length_pointer = call.word(2);
  uint32_t length; memcpy(&length, guest(arena, length_pointer, 4), 4);
  sockaddr_storage address{}; socklen_t host_length = sizeof(address);
  int result = getsockname(int32_t(call.word(0)), reinterpret_cast<sockaddr *>(&address), &host_length);
  if (!result) {
    uint32_t copied = std::min<uint32_t>(length, host_length);
    if (copied) memcpy(guest(arena, call.word(1), copied), &address, copied);
    uint32_t value = host_length; memcpy(guest(arena, length_pointer, 4), &value, 4);
  }
  call.result(result);
}
IMPORT(getpeername) {
  CALL;
  uint32_t length_pointer = call.word(2);
  uint32_t length; memcpy(&length, guest(arena, length_pointer, 4), 4);
  sockaddr_storage address{}; socklen_t host_length = sizeof(address);
  int result = getpeername(int32_t(call.word(0)), reinterpret_cast<sockaddr *>(&address), &host_length);
  if (!result) {
    uint32_t copied = std::min<uint32_t>(length, host_length);
    if (copied) memcpy(guest(arena, call.word(1), copied), &address, copied);
    uint32_t value = host_length; memcpy(guest(arena, length_pointer, 4), &value, 4);
  }
  call.result(result);
}
IMPORT(send) {
  CALL; int32_t length = int32_t(call.word(2));
  if (length < 0) { errno = EINVAL; call.result(-1); return; }
  call.result(send(int32_t(call.word(0)), length ? guest(arena, call.word(1), length) : nullptr,
                   length, call.word(3)));
}
IMPORT(recv) {
  CALL; int32_t length = int32_t(call.word(2));
  if (length < 0) { errno = EINVAL; call.result(-1); return; }
  call.result(recv(int32_t(call.word(0)), length ? guest(arena, call.word(1), length) : nullptr,
                   length, call.word(3)));
}
IMPORT(sendto) {
  CALL; int32_t length = int32_t(call.word(2)); uint32_t address_length = call.word(5);
  if (length < 0 || address_length > sizeof(sockaddr_storage)) { errno = EINVAL; call.result(-1); return; }
  sockaddr_storage address{};
  if (address_length) memcpy(&address, guest(arena, call.word(4), address_length), address_length);
  call.result(sendto(int32_t(call.word(0)), length ? guest(arena, call.word(1), length) : nullptr,
      length, call.word(3), address_length ? reinterpret_cast<sockaddr *>(&address) : nullptr, address_length));
}
IMPORT(recvfrom) {
  CALL; int32_t length = int32_t(call.word(2)); uint32_t address_pointer = call.word(4), length_pointer = call.word(5);
  if (length < 0) { errno = EINVAL; call.result(-1); return; }
  sockaddr_storage address{}; socklen_t host_length = sizeof(address);
  ssize_t result = recvfrom(int32_t(call.word(0)), length ? guest(arena, call.word(1), length) : nullptr,
      length, call.word(3), address_pointer ? reinterpret_cast<sockaddr *>(&address) : nullptr,
      address_pointer ? &host_length : nullptr);
  if (result >= 0 && address_pointer) {
    uint32_t capacity; memcpy(&capacity, guest(arena, length_pointer, 4), 4);
    uint32_t copied = std::min<uint32_t>(capacity, host_length);
    if (copied) memcpy(guest(arena, address_pointer, copied), &address, copied);
    uint32_t value = host_length; memcpy(guest(arena, length_pointer, 4), &value, 4);
  }
  call.result(result);
}
IMPORT(getsockopt) {
  CALL;
  call.result(netextra_getsockopt_guest(arena, int32_t(call.word(0)), int32_t(call.word(1)),
      int32_t(call.word(2)), call.word(3), call.word(4)));
}
IMPORT(setsockopt) {
  CALL;
  call.result(netextra_setsockopt_guest(arena, int32_t(call.word(0)), int32_t(call.word(1)),
      int32_t(call.word(2)), call.word(3), int32_t(call.word(4))));
}
IMPORT(poll) {
  CALL; int32_t count = int32_t(call.word(1));
  if (count < 0 || uint64_t(count) * sizeof(pollfd) > MEMORY_ARENA_SIZE) { errno = EINVAL; call.result(-1); return; }
  std::vector<pollfd> fds(count);
  for (int32_t i = 0; i < count; ++i) {
    uint8_t *p = static_cast<uint8_t *>(guest(arena, call.word(0) + uint32_t(i) * 8, 8));
    memcpy(&fds[i].fd, p, 4); memcpy(&fds[i].events, p + 4, 2); memcpy(&fds[i].revents, p + 6, 2);
  }
  int result = poll(count ? fds.data() : nullptr, count, int32_t(call.word(2)));
  for (int32_t i = 0; i < count; ++i)
    memcpy(guest(arena, call.word(0) + uint32_t(i) * 8 + 6, 2), &fds[i].revents, 2);
  call.result(result);
}
IMPORT(select) {
  CALL;
  int32_t nfds = int32_t(call.word(0));
  if (nfds < 0 || nfds > FD_SETSIZE) { errno = EINVAL; call.result(-1); return; }
  fd_set read_set, write_set, except_set;
  fd_set *r = nullptr, *w = nullptr, *e = nullptr;
  auto load = [&](uint32_t address, fd_set &set, fd_set *&target) {
    if (!address) return;
    target = &set; FD_ZERO(&set);
    for (int fd = 0; fd < nfds; ++fd) {
      uint32_t word; memcpy(&word, guest(arena, address + (fd >> 5) * 4, 4), 4);
      if ((word >> (fd & 31)) & 1) FD_SET(fd, &set);
    }
  };
  load(call.word(1), read_set, r); load(call.word(2), write_set, w); load(call.word(3), except_set, e);
  timeval timeout, *timeout_pointer = nullptr;
  if (call.word(4)) {
    int32_t values[2]; memcpy(values, guest(arena, call.word(4), 8), 8);
    timeout.tv_sec = values[0]; timeout.tv_usec = values[1]; timeout_pointer = &timeout;
  }
  int result = select(nfds, r, w, e, timeout_pointer);
  auto store = [&](uint32_t address, fd_set *set) {
    if (!address) return;
    for (int word_index = 0; word_index < (nfds + 31) / 32; ++word_index) {
      uint32_t bits = 0;
      for (int bit = 0; bit < 32 && word_index * 32 + bit < nfds; ++bit)
        if (FD_ISSET(word_index * 32 + bit, set)) bits |= 1u << bit;
      memcpy(guest(arena, address + word_index * 4, 4), &bits, 4);
    }
  };
  if (result >= 0) { store(call.word(1), r); store(call.word(2), w); store(call.word(3), e); }
  if (timeout_pointer && call.word(4)) { int32_t values[2] = {int32_t(timeout.tv_sec), int32_t(timeout.tv_usec)}; memcpy(guest(arena, call.word(4), 8), values, 8); }
  call.result(result);
}
IMPORT(ioctl) {
  CALL;
  unsigned long request = call.word(1); uint32_t argument = call.word(2);
  unsigned size = (request >> 16) & 0x3fff;
  unsigned direction = (request >> 30) & 3;
  if (!size) {
#ifdef TIOCGWINSZ
    if (request == TIOCGWINSZ) { size = sizeof(struct winsize); direction = 2; }
#endif
#ifdef TIOCSWINSZ
    if (request == TIOCSWINSZ) { size = sizeof(struct winsize); direction = 1; }
#endif
#ifdef FIONREAD
    if (request == FIONREAD) { size = sizeof(int); direction = 2; }
#endif
#ifdef FIONBIO
    if (request == FIONBIO) { size = sizeof(int); direction = 1; }
#endif
#ifdef TCGETS
    if (request == TCGETS) { size = sizeof(struct termios); direction = 2; }
#endif
#ifdef TCSETS
    if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
      size = sizeof(struct termios); direction = 1;
    }
#endif
#ifdef TIOCGPGRP
    if (request == TIOCGPGRP) { size = sizeof(pid_t); direction = 2; }
#endif
#ifdef TIOCSPGRP
    if (request == TIOCSPGRP) { size = sizeof(pid_t); direction = 1; }
#endif
  }
  if (size > MEMORY_ARENA_SIZE) { errno = EINVAL; call.result(-1); return; }
  if (!size || !argument) { call.result(ioctl(int32_t(call.word(0)), request, uintptr_t(argument))); return; }
  std::vector<uint8_t> bytes(size);
  if (direction & 1) memcpy(bytes.data(), guest(arena, argument, size), size);
  int result = ioctl(int32_t(call.word(0)), request, bytes.data());
  if (result >= 0 && (direction & 2)) memcpy(guest(arena, argument, size), bytes.data(), size);
  call.result(result);
}
IMPORT(inet_addr) { CALL; call.result(inet_addr(text(arena, call.word(0)))); }
IMPORT(inet_pton) {
  CALL; int32_t family = call.word(0);
  size_t size = family == AF_INET ? 4 : family == AF_INET6 ? 16 : 0;
  if (!size) { errno = EAFNOSUPPORT; call.result(-1); return; }
  uint8_t result[16];
  int status = inet_pton(family, text(arena, call.word(1)), result);
  if (status == 1) memcpy(guest(arena, call.word(2), size), result, size);
  call.result(status);
}
IMPORT(inet_ntop) {
  CALL; int32_t family = call.word(0);
  size_t size = family == AF_INET ? 4 : family == AF_INET6 ? 16 : 0;
  if (!size) { errno = EAFNOSUPPORT; call.result(0); return; }
  char result[INET6_ADDRSTRLEN];
  if (!inet_ntop(family, guest(arena, call.word(1), size), result, sizeof(result))) { call.result(0); return; }
  uint32_t capacity = call.word(3);
  size_t length = strlen(result) + 1;
  if (length > capacity) { errno = ENOSPC; call.result(0); return; }
  memcpy(guest(arena, call.word(2), length), result, length);
  call.result(call.word(2));
}
IMPORT(gethostname) {
  CALL; uint32_t length = call.word(1);
  if (!length || length > MEMORY_ARENA_SIZE) { errno = EINVAL; call.result(-1); return; }
  std::vector<char> name(length);
  int result = gethostname(name.data(), length);
  if (!result) memcpy(guest(arena, call.word(0), length), name.data(), length);
  call.result(result);
}
IMPORT(gethostbyname) {
  CALL;
  for (uint32_t block : netextra_hostent_blocks) release(block);
  netextra_hostent_blocks.clear(); netextra_hostent_root = 0;
  hostent *host = gethostbyname(text(arena, call.word(0)));
  if (!host) { call.result(0); return; }
  auto *prior_blocks = netextra_active_blocks; netextra_active_blocks = &netextra_hostent_blocks;
  size_t alias_count = 0, address_count = 0;
  if (host->h_aliases) while (host->h_aliases[alias_count]) ++alias_count;
  if (host->h_addr_list) while (host->h_addr_list[address_count]) ++address_count;
  uint32_t root = netextra_alloc(runtime, 20);
  uint32_t name = netextra_alloc(runtime, strlen(host->h_name) + 1);
  uint32_t aliases = netextra_alloc(runtime, (alias_count + 1) * 4);
  uint32_t list = netextra_alloc(runtime, (address_count + 1) * 4);
  bool failed = !root || !name || !aliases || !list || !address_count || host->h_length <= 0;
  std::vector<uint32_t> alias_strings(alias_count), address_values(address_count);
  for (size_t i = 0; i < alias_count && !failed; ++i) {
    alias_strings[i] = netextra_alloc(runtime, strlen(host->h_aliases[i]) + 1);
    if (!alias_strings[i]) { failed = true; break; }
    memcpy(guest(arena, alias_strings[i], strlen(host->h_aliases[i]) + 1),
           host->h_aliases[i], strlen(host->h_aliases[i]) + 1);
  }
  for (size_t i = 0; i < address_count && !failed; ++i) {
    address_values[i] = netextra_alloc(runtime, host->h_length);
    if (!address_values[i]) { failed = true; break; }
    memcpy(guest(arena, address_values[i], host->h_length),
           host->h_addr_list[i], host->h_length);
  }
  if (!failed) {
    memcpy(guest(arena, name, strlen(host->h_name) + 1), host->h_name, strlen(host->h_name) + 1);
    for (size_t i = 0; i < alias_count; ++i) memcpy(guest(arena, aliases + i * 4, 4), &alias_strings[i], 4);
    for (size_t i = 0; i < address_count; ++i) memcpy(guest(arena, list + i * 4, 4), &address_values[i], 4);
    uint32_t null = 0;
    memcpy(guest(arena, aliases + alias_count * 4, 4), &null, 4);
    memcpy(guest(arena, list + address_count * 4, 4), &null, 4);
    uint32_t fields[5] = {name, aliases, uint32_t(host->h_addrtype), uint32_t(host->h_length), list};
    memcpy(guest(arena, root, sizeof(fields)), fields, sizeof(fields));
    netextra_hostent_root = root;
  }
  netextra_active_blocks = prior_blocks;
  if (failed) {
    for (uint32_t block : netextra_hostent_blocks) release(block);
    netextra_hostent_blocks.clear(); errno = ENOMEM; call.result(0); return;
  }
  call.result(netextra_hostent_root);
}
IMPORT(getaddrinfo) {
  CALL; addrinfo hints{}; addrinfo *hints_pointer = nullptr;
  if (!call.word(3)) { errno = EFAULT; call.result(EAI_FAIL); return; }
  if (call.word(2)) {
    uint32_t words[8]; memcpy(words, guest(arena, call.word(2), sizeof(words)), sizeof(words));
    hints.ai_flags = int32_t(words[0]); hints.ai_family = int32_t(words[1]);
    hints.ai_socktype = int32_t(words[2]); hints.ai_protocol = int32_t(words[3]);
    hints_pointer = &hints;
  }
  addrinfo *result = nullptr;
  int status = getaddrinfo(call.word(0) ? text(arena, call.word(0)) : nullptr,
      call.word(1) ? text(arena, call.word(1)) : nullptr, hints_pointer, &result);
  if (status) { call.result(status); return; }
  size_t count = 0; for (addrinfo *p = result; p; p = p->ai_next) ++count;
  if (!count) { freeaddrinfo(result); uint32_t empty = 0; memcpy(guest(arena, call.word(3), 4), &empty, 4); call.result(0); return; }
  std::vector<uint32_t> blocks; auto *prior_blocks = netextra_active_blocks; netextra_active_blocks = &blocks;
  uint32_t *addresses = count ? new uint32_t[count] : nullptr;
  bool failed = false;
  for (size_t i = 0; i < count; ++i) {
    addresses[i] = netextra_alloc(runtime, 32, 4);
    if (!addresses[i]) { failed = true; break; }
  }
  size_t node_index = 0;
  for (addrinfo *p = result; p && !failed; p = p->ai_next, ++node_index) {
    const size_t i = node_index;
    uint32_t address = netextra_copy_sockaddr(arena, p->ai_addr, p->ai_addrlen, &blocks);
    uint32_t canon = 0;
    if (p->ai_canonname) {
      canon = netextra_alloc(runtime, strlen(p->ai_canonname) + 1);
      if (canon) memcpy(guest(arena, canon, strlen(p->ai_canonname) + 1), p->ai_canonname, strlen(p->ai_canonname) + 1);
    }
    if (!address || (p->ai_canonname && !canon)) { failed = true; break; }
    uint32_t fields[8] = {uint32_t(p->ai_flags), uint32_t(p->ai_family), uint32_t(p->ai_socktype), uint32_t(p->ai_protocol), uint32_t(p->ai_addrlen), address, canon, i + 1 < count ? addresses[i + 1] : 0};
    memcpy(guest(arena, addresses[i], sizeof(fields)), fields, sizeof(fields));
  }
  netextra_active_blocks = prior_blocks;
  if (failed) {
    delete[] addresses; freeaddrinfo(result);
    for (uint32_t block : blocks) release(block);
    call.result(EAI_MEMORY); return;
  }
  if (call.word(3)) memcpy(guest(arena, call.word(3), 4), addresses, 4);
  { std::lock_guard<std::mutex> lock(netextra_guest_alloc_mutex);
    netextra_addrinfo_roots[addresses[0]] = {result, std::move(blocks)}; }
  delete[] addresses; call.result(0);
}
IMPORT(freeaddrinfo) {
  CALL; NetExtraOwned owned{};
  { std::lock_guard<std::mutex> lock(netextra_guest_alloc_mutex);
    auto found = netextra_addrinfo_roots.find(call.word(0));
    if (found != netextra_addrinfo_roots.end()) { owned = std::move(found->second); netextra_addrinfo_roots.erase(found); } }
  if (owned.host) freeaddrinfo(static_cast<addrinfo *>(owned.host));
  for (uint32_t block : owned.guest_blocks) release(block);
}
IMPORT(getifaddrs) {
  CALL; ifaddrs *host = nullptr;
  if (!call.word(0)) { errno = EFAULT; call.result(-1); return; }
  int result = getifaddrs(&host);
  if (result) { call.result(result); return; }
  size_t count = 0; for (ifaddrs *p = host; p; p = p->ifa_next) ++count;
  if (!count) { freeifaddrs(host); uint32_t empty = 0; memcpy(guest(arena, call.word(0), 4), &empty, 4); call.result(0); return; }
  std::vector<uint32_t> blocks; auto *prior_blocks = netextra_active_blocks; netextra_active_blocks = &blocks;
  std::vector<uint32_t> nodes(count); size_t i = 0;
  for (; i < count; ++i) { nodes[i] = netextra_alloc(runtime, 28, 4); if (!nodes[i]) break; }
  bool failed = i != count;
  i = 0;
  for (ifaddrs *p = host; p && !failed; p = p->ifa_next, ++i) {
    uint32_t name = netextra_alloc(runtime, strlen(p->ifa_name) + 1);
    if (!name) { failed = true; break; }
    memcpy(guest(arena, name, strlen(p->ifa_name) + 1), p->ifa_name, strlen(p->ifa_name) + 1);
    auto copy_address = [&](sockaddr *a) { return a ? netextra_copy_sockaddr(arena, runtime, a,
        a->sa_family == AF_INET ? sizeof(sockaddr_in) :
        a->sa_family == AF_INET6 ? sizeof(sockaddr_in6) :
#ifdef __linux__
        a->sa_family == AF_PACKET ? sizeof(sockaddr_ll) :
#endif
        sizeof(sockaddr)) : 0; };
    uint32_t address = copy_address(p->ifa_addr), netmask = copy_address(p->ifa_netmask), broad = copy_address(p->ifa_broadaddr);
    uint32_t data = 0;
#ifdef __linux__
    if (p->ifa_data) {
      data = netextra_alloc(runtime, sizeof(struct rtnl_link_stats));
      if (data) memcpy(guest(arena, data, sizeof(struct rtnl_link_stats)), p->ifa_data, sizeof(struct rtnl_link_stats));
    }
#endif
    if ((p->ifa_addr && !address) || (p->ifa_netmask && !netmask) ||
        (p->ifa_broadaddr && !broad)
#ifdef __linux__
        || (p->ifa_data && !data)
#endif
        ) { failed = true; break; }
    uint32_t fields[7] = {i + 1 < count ? nodes[i + 1] : 0, name, p->ifa_flags,
                          address, netmask, broad, data};
    memcpy(guest(arena, nodes[i], sizeof(fields)), fields, sizeof(fields));
  }
  netextra_active_blocks = prior_blocks;
  if (failed) {
    freeifaddrs(host);
    for (uint32_t block : blocks) release(block);
    errno = ENOMEM; call.result(-1); return;
  }
  if (call.word(0)) memcpy(guest(arena, call.word(0), 4), &nodes[0], 4);
  { std::lock_guard<std::mutex> lock(netextra_guest_alloc_mutex);
    netextra_ifaddrs_roots[nodes[0]] = {host, std::move(blocks)}; }
  call.result(0);
}
IMPORT(freeifaddrs) {
  CALL; NetExtraOwned owned{};
  { std::lock_guard<std::mutex> lock(netextra_guest_alloc_mutex);
    auto found = netextra_ifaddrs_roots.find(call.word(0));
    if (found != netextra_ifaddrs_roots.end()) { owned = std::move(found->second); netextra_ifaddrs_roots.erase(found); } }
  if (owned.host) freeifaddrs(static_cast<ifaddrs *>(owned.host));
  for (uint32_t block : owned.guest_blocks) release(block);
}
IMPORT(sigemptyset) {
  CALL; uint32_t words[32] = {};
  memcpy(guest(arena, call.word(0), sizeof(words)), words, sizeof(words)); call.result(0);
}
IMPORT(sigaddset) {
  CALL; int32_t sig = int32_t(call.word(1));
  if (sig <= 0 || sig >= NSIG) { errno = EINVAL; call.result(-1); return; }
  uint32_t address = call.word(0), word;
  memcpy(&word, guest(arena, address + uint32_t((sig - 1) >> 5) * 4, 4), 4);
  word |= 1u << ((sig - 1) & 31);
  memcpy(guest(arena, address + uint32_t((sig - 1) >> 5) * 4, 4), &word, 4);
  call.result(0);
}
IMPORT(sigprocmask) {
  CALL; int32_t how = int32_t(call.word(0));
  sigset_t set, old; sigset_t *set_pointer = nullptr;
  if (call.word(1)) { netextra_guest_sigset(arena, call.word(1), set); set_pointer = &set; }
  int result = sigprocmask(how, set_pointer, call.word(2) ? &old : nullptr);
  if (!result && call.word(2)) netextra_store_sigset(arena, call.word(2), old);
  call.result(result);
}
IMPORT(raise) { CALL; call.result(raise(int32_t(call.word(0)))); }
IMPORT(sigaction) {
  CALL; int32_t sig = int32_t(call.word(0));
  if (sig <= 0 || sig >= NSIG) { errno = EINVAL; call.result(-1); return; }
  NetExtraSignal previous = netextra_signal_get(runtime, sig);
  struct sigaction next{}, old{}; struct sigaction *next_pointer = nullptr;
  if (call.word(1)) {
    NetExtraGuestSigaction guest_action;
    memcpy(&guest_action, guest(arena, call.word(1), sizeof(guest_action)), sizeof(guest_action));
    if (guest_action.flags & (SA_SIGINFO | SA_ONSTACK)) { errno = ENOTSUP; call.result(-1); return; }
    sigset_t mask; netextra_guest_sigset(arena, call.word(1) + 4, mask);
    const uint32_t handler = guest_action.handler;
    NetExtraSignal entry{handler, arena, state, runtime, guest_action.flags,
                         guest_action.restorer, mask};
    netextra_signal_set(runtime, sig, entry);
    sigemptyset(&next.sa_mask); next.sa_mask = mask;
    int host_flags = int32_t(guest_action.flags);
#ifdef SA_RESTORER
    host_flags &= ~SA_RESTORER;
#endif
    next.sa_flags = host_flags;
    next.sa_handler = handler == 0 ? SIG_DFL : handler == 1 ? SIG_IGN : netextra_signal_trampoline;
    next_pointer = &next;
  }
  int result = sigaction(sig, next_pointer, call.word(2) ? &old : nullptr);
  if (result && call.word(1)) netextra_signal_set(runtime, sig, previous);
  if (!result && call.word(2)) {
    NetExtraGuestSigaction output{};
    const bool ours = old.sa_handler == netextra_signal_trampoline;
    uint32_t handler = old.sa_handler == SIG_IGN ? 1 : old.sa_handler == SIG_DFL ? 0 :
        ours ? previous.arena_handler : 0;
    output.handler = handler;
    output.flags = ours ? previous.guest_flags : uint32_t(old.sa_flags);
    output.restorer = ours ? previous.guest_restorer : 0;
    memcpy(guest(arena, call.word(2), sizeof(output)), &output, sizeof(output));
    netextra_store_sigset(arena, call.word(2) + 4, old.sa_mask);
  }
  call.result(result);
}
IMPORT(signal) {
  CALL; int32_t sig = int32_t(call.word(0)); uint32_t handler = call.word(1);
  if (sig <= 0 || sig >= NSIG) { errno = EINVAL; call.result(UINT32_MAX); return; }
  NetExtraSignal previous = netextra_signal_get(runtime, sig);
  struct sigaction next{}, old{}; sigemptyset(&next.sa_mask);
  NetExtraSignal entry{handler, arena, state, runtime, SA_RESTART, 0, next.sa_mask};
  netextra_signal_set(runtime, sig, entry);
  next.sa_handler = handler == 0 ? SIG_DFL : handler == 1 ? SIG_IGN : netextra_signal_trampoline;
  next.sa_flags = SA_RESTART;
  if (sigaction(sig, &next, &old)) { netextra_signal_set(runtime, sig, previous); call.result(UINT32_MAX); return; }
  uint32_t old_handler = old.sa_handler == SIG_IGN ? 1 : old.sa_handler == SIG_DFL ? 0 :
      old.sa_handler == netextra_signal_trampoline ? previous.arena_handler : 0;
  call.result(old_handler);
}
IMPORT(syscall) {
  CALL; int32_t number = int32_t(call.word(0)); long result = -1;
  if (number == 102) {
    uint32_t call_number = call.word(1);
    uint32_t args[6] = {};
    const uint32_t argument_count =
        (call_number == 11 || call_number == 12) ? 6 :
        (call_number == 8 || call_number == 18) ? 4 :
        (call_number == 4 || call_number == 13) ? 2 :
        (call_number == 14 || call_number == 15) ? 5 :
        (call_number == 9 || call_number == 10) ? 4 :
        (call_number >= 1 && call_number <= 7) ||
        (call_number >= 16 && call_number <= 17) ? 3 : 0;
    if (argument_count)
      memcpy(args, guest(arena, call.word(2), argument_count * 4), argument_count * 4);
    switch (call_number) {
      case 1: result = socket(int32_t(args[0]), int32_t(args[1]), int32_t(args[2])); break;
      case 2: result = bind(int32_t(args[0]), reinterpret_cast<sockaddr *>(guest(arena, args[1], args[2])), args[2]); break;
      case 3: result = connect(int32_t(args[0]), reinterpret_cast<sockaddr *>(guest(arena, args[1], args[2])), args[2]); break;
      case 4: result = listen(int32_t(args[0]), int32_t(args[1])); break;
      case 8: result = socketpair(int32_t(args[0]), int32_t(args[1]), int32_t(args[2]),
                                  reinterpret_cast<int *>(guest(arena, args[3], 8))); break;
      case 5: {
        uint32_t capacity = 0;
        if (args[2]) memcpy(&capacity, guest(arena, args[2], 4), 4);
        sockaddr_storage address{}; socklen_t length = sizeof(address);
        int fd = accept(int32_t(args[0]), args[1] ? reinterpret_cast<sockaddr *>(&address) : nullptr,
                        args[1] ? &length : nullptr);
        if (fd >= 0 && args[1]) {
          uint32_t copied = std::min<uint32_t>(capacity, length);
          if (copied) memcpy(guest(arena, args[1], copied), &address, copied);
          memcpy(guest(arena, args[2], 4), &length, 4);
        }
        result = fd; break;
      }
      case 6: case 7: {
        uint32_t capacity; memcpy(&capacity, guest(arena, args[2], 4), 4);
        sockaddr_storage address{}; socklen_t length = sizeof(address);
        int status = call_number == 6
            ? getsockname(int32_t(args[0]), reinterpret_cast<sockaddr *>(&address), &length)
            : getpeername(int32_t(args[0]), reinterpret_cast<sockaddr *>(&address), &length);
        if (!status) {
          uint32_t copied = std::min<uint32_t>(capacity, length);
          if (copied) memcpy(guest(arena, args[1], copied), &address, copied);
          memcpy(guest(arena, args[2], 4), &length, 4);
        }
        result = status; break;
      }
      case 9: result = send(int32_t(args[0]), guest(arena, args[1], args[2]), args[2], args[3]); break;
      case 10: result = recv(int32_t(args[0]), guest(arena, args[1], args[2]), args[2], args[3]); break;
      case 11: result = sendto(int32_t(args[0]), guest(arena, args[1], args[2]), args[2], args[3],
          args[4] ? reinterpret_cast<sockaddr *>(guest(arena, args[4], args[5])) : nullptr, args[5]); break;
      case 12: {
        uint32_t capacity = 0;
        if (args[5]) memcpy(&capacity, guest(arena, args[5], 4), 4);
        sockaddr_storage address{}; socklen_t length = sizeof(address);
        ssize_t received = recvfrom(int32_t(args[0]), guest(arena, args[1], args[2]), args[2], args[3],
            args[4] ? reinterpret_cast<sockaddr *>(&address) : nullptr, args[4] ? &length : nullptr);
        if (received >= 0 && args[4]) {
          uint32_t copied = std::min<uint32_t>(capacity, length);
          if (copied) memcpy(guest(arena, args[4], copied), &address, copied);
          memcpy(guest(arena, args[5], 4), &length, 4);
        }
        result = received; break;
      }
      case 13: result = shutdown(int32_t(args[0]), int32_t(args[1])); break;
      case 14: result = netextra_setsockopt_guest(arena, int32_t(args[0]), int32_t(args[1]),
          int32_t(args[2]), args[3], int32_t(args[4])); break;
      case 15: result = netextra_getsockopt_guest(arena, int32_t(args[0]), int32_t(args[1]),
          int32_t(args[2]), args[3], args[4]); break;
      case 16: result = netextra_message(arena, args[0], args[1], int32_t(args[2]), false); break;
      case 17: result = netextra_message(arena, args[0], args[1], int32_t(args[2]), true); break;
      case 18: result = netextra_accept4_guest(arena, int32_t(args[0]), args[1], args[2], int32_t(args[3])); break;
      default: errno = ENOSYS; break;
    }
  } else if (number >= 359 && number <= 373) {
    uint32_t a[6]; for (unsigned i = 0; i < 6; ++i) a[i] = call.word(i + 1);
    switch (number) {
      case 359: result = socket(int32_t(a[0]), int32_t(a[1]), int32_t(a[2])); break;
      case 360: result = socketpair(int32_t(a[0]), int32_t(a[1]), int32_t(a[2]), reinterpret_cast<int *>(guest(arena, a[3], 8))); break;
      case 361: result = bind(int32_t(a[0]), reinterpret_cast<sockaddr *>(guest(arena, a[1], a[2])), a[2]); break;
      case 362: result = connect(int32_t(a[0]), reinterpret_cast<sockaddr *>(guest(arena, a[1], a[2])), a[2]); break;
      case 363: result = listen(int32_t(a[0]), int32_t(a[1])); break;
      case 364: result = netextra_accept4_guest(arena, int32_t(a[0]), a[1], a[2], int32_t(a[3])); break;
      case 365: result = netextra_getsockopt_guest(arena, int32_t(a[0]), int32_t(a[1]),
          int32_t(a[2]), a[3], a[4]); break;
      case 366: result = netextra_setsockopt_guest(arena, int32_t(a[0]), int32_t(a[1]),
          int32_t(a[2]), a[3], int32_t(a[4])); break;
      case 367: case 368: {
        uint32_t capacity; memcpy(&capacity, guest(arena, a[2], 4), 4);
        sockaddr_storage address{}; socklen_t length = sizeof(address);
        int status = number == 367 ? getsockname(int32_t(a[0]), reinterpret_cast<sockaddr *>(&address), &length)
                                  : getpeername(int32_t(a[0]), reinterpret_cast<sockaddr *>(&address), &length);
        if (!status) { uint32_t copied = std::min<uint32_t>(capacity, length); if (copied) memcpy(guest(arena, a[1], copied), &address, copied); memcpy(guest(arena, a[2], 4), &length, 4); }
        result = status; break;
      }
      case 369: result = sendto(int32_t(a[0]), a[2] ? guest(arena, a[1], a[2]) : nullptr, a[2], a[3], a[4] ? reinterpret_cast<sockaddr *>(guest(arena, a[4], a[5])) : nullptr, a[5]); break;
      case 371: {
        uint32_t capacity = 0; if (a[5]) memcpy(&capacity, guest(arena, a[5], 4), 4);
        sockaddr_storage address{}; socklen_t length = sizeof(address);
        ssize_t received = recvfrom(int32_t(a[0]), a[2] ? guest(arena, a[1], a[2]) : nullptr, a[2], a[3], a[4] ? reinterpret_cast<sockaddr *>(&address) : nullptr, a[4] ? &length : nullptr);
        if (received >= 0 && a[4]) { uint32_t copied = std::min<uint32_t>(capacity, length); if (copied) memcpy(guest(arena, a[4], copied), &address, copied); memcpy(guest(arena, a[5], 4), &length, 4); }
        result = received; break;
      }
      case 370: result = netextra_message(arena, a[0], a[1], int32_t(a[2]), false); break;
      case 372: result = netextra_message(arena, a[0], a[1], int32_t(a[2]), true); break;
      case 373: result = shutdown(int32_t(a[0]), int32_t(a[1])); break;
      default: errno = ENOSYS; break;
    }
  } else {
    switch (number) {
      case 1: exit(int32_t(call.word(1))); result = 0; break;
      case 2: errno = ENOSYS; break;
      case 3: result = read(int32_t(call.word(1)), call.word(3) ? guest(arena, call.word(2), call.word(3)) : nullptr, call.word(3)); break;
      case 4: result = write(int32_t(call.word(1)), call.word(3) ? guest(arena, call.word(2), call.word(3)) : nullptr, call.word(3)); break;
      case 6: result = close(int32_t(call.word(1))); break;
      case 20: result = getpid(); break;
      case 24: result = getuid(); break;
      case 47: result = getgid(); break;
      case 49: result = geteuid(); break;
      case 50: result = getegid(); break;
      case 122: result = uname(reinterpret_cast<struct utsname *>(guest(arena, call.word(1), sizeof(struct utsname)))); break;
      case 162: {
        int32_t guest_time[2]; memcpy(guest_time, guest(arena, call.word(1), 8), 8);
        timespec requested{guest_time[0], guest_time[1]}, remaining{};
        int status = nanosleep(&requested, call.word(2) ? &remaining : nullptr);
        if (status && call.word(2)) {
          int32_t output[2] = {int32_t(remaining.tv_sec), int32_t(remaining.tv_nsec)};
          memcpy(guest(arena, call.word(2), 8), output, 8);
        }
        result = status; break;
      }
#ifdef __linux__
      case 355: result = getrandom(call.word(2) ? guest(arena, call.word(1), call.word(2)) : nullptr, call.word(2), call.word(3)); break;
#else
      case 355: errno = ENOSYS; break;
#endif
      default: errno = ENOSYS; break;
    }
  }
  call.result(uint32_t(result));
}
namespace {
uint32_t netextra_mangle_pointer(uint32_t value, uint32_t guard) {
  uint32_t encoded = value ^ guard;
  return (encoded << 9) | (encoded >> 23);
}
uint32_t netextra_unmangle_pointer(uint32_t value, uint32_t guard) {
  uint32_t decoded = (value >> 9) | (value << 23);
  return decoded ^ guard;
}
void netextra_save_jump(uint8_t *arena, State *state, uint32_t env, bool save_mask) {
  uint32_t words[6] = {
      state->gpr.rbx.dword, state->gpr.rsi.dword, state->gpr.rdi.dword,
      state->gpr.rbp.dword, 0, 0};
  uint32_t return_pc;
  memcpy(&return_pc, guest(arena, state->gpr.rsp.dword, 4), 4);
  words[4] = state->gpr.rsp.dword + 4;
  words[5] = return_pc;
  uint32_t pointer_guard = 0;
  if (state->addr.gs_base.dword)
    memcpy(&pointer_guard, guest(arena, state->addr.gs_base.dword + 24, 4), 4);
  words[4] = netextra_mangle_pointer(words[4], pointer_guard);
  words[5] = netextra_mangle_pointer(words[5], pointer_guard);
  memcpy(guest(arena, env, sizeof(words)), words, sizeof(words));
  uint32_t was_saved = save_mask;
  memcpy(guest(arena, env + 24, 4), &was_saved, 4);
  if (save_mask) {
    sigset_t mask;
    if (sigprocmask(SIG_SETMASK, nullptr, &mask)) elfconv_runtime_error("Cannot save i386 signal mask.\n");
    netextra_store_sigset(arena, env + 28, mask);
  }
}
[[noreturn]] void netextra_longjmp(uint8_t *arena, State *state, uint32_t env,
                                  uint32_t value, bool checked) {
  uint32_t words[6]; memcpy(words, guest(arena, env, sizeof(words)), sizeof(words));
  uint32_t pointer_guard = 0;
  if (state->addr.gs_base.dword)
    memcpy(&pointer_guard, guest(arena, state->addr.gs_base.dword + 24, 4), 4);
  const uint32_t stack = netextra_unmangle_pointer(words[4], pointer_guard);
  if (checked && stack < state->gpr.rsp.dword)
    elfconv_runtime_error("invalid i386 longjmp target stack.\n");
  const uint32_t pc = netextra_unmangle_pointer(words[5], pointer_guard);
  // glibc sysdeps/unix/sysv/linux/i386/pointer_guard.h: SAVE uses rol32(ptr ^ guard,9);
  // RESTORE uses ror32(encoded,9)^guard, for both saved ESP and EIP.
  if (stack < NULL_GUARD_SIZE || stack > MEMORY_ARENA_SIZE)
    elfconv_runtime_error("invalid i386 longjmp target stack.\n");
  if (pc < NULL_GUARD_SIZE || pc >= MEMORY_ARENA_SIZE)
    elfconv_runtime_error("invalid i386 longjmp target pc.\n");
  uint32_t mask_saved; memcpy(&mask_saved, guest(arena, env + 24, 4), 4);
  netextra_jump_saved_mask = mask_saved != 0;
  if (mask_saved) {
    sigset_t mask; netextra_guest_sigset(arena, env + 28, mask);
    if (sigprocmask(SIG_SETMASK, &mask, nullptr))
      elfconv_runtime_error("Cannot restore i386 signal mask.\n");
  }
  state->gpr.rbx.dword = words[0];
  state->gpr.rsi.dword = words[1];
  state->gpr.rdi.dword = words[2];
  state->gpr.rbp.dword = words[3];
  state->gpr.rsp.dword = stack;
  state->gpr.rip.dword = pc;
  state->gpr.rax.dword = value ? value : 1;
  throw I386NonlocalJump{};
}
}
IMPORT(_setjmp) { CALL; netextra_save_jump(arena, state, call.word(0), false); call.result(0); }
IMPORT(__sigsetjmp) { CALL; netextra_save_jump(arena, state, call.word(0), call.word(1) != 0); call.result(0); }
IMPORT(longjmp) { CALL; netextra_longjmp(arena, state, call.word(0), call.word(1), false); }
IMPORT(__longjmp_chk) { CALL; netextra_longjmp(arena, state, call.word(0), call.word(1), true); }
// Delivery is deferred from the host trampoline until an import/dispatch safe point.
extern "C" void __ecv_i386_dispatch_pending_signals(uint8_t *arena, State *state,
                                                      RuntimeManager *runtime) {
  if (netextra_dispatching_signals || !state || !runtime) return;
  netextra_dispatching_signals = true;
  struct DispatchGuard {
    ~DispatchGuard() { netextra_dispatching_signals = false; }
  } dispatch_guard;
  for (int sig = 1; sig < NSIG; ++sig) {
    if (!netextra_pending_signals[sig]) continue;
    netextra_pending_signals[sig] = 0;
    NetExtraSignal action = netextra_signal_get(runtime, sig);
    if (!action.arena_handler || action.arena_handler == 1) continue;
    if (action.guest_flags & SA_RESETHAND) {
      NetExtraSignal reset{};
      reset.runtime = runtime;
      netextra_signal_set(runtime, sig, reset);
    }
    sigset_t old_mask;
    if (sigprocmask(SIG_SETMASK, nullptr, &old_mask))
      elfconv_runtime_error("Cannot query signal mask at guest safe point.\n");
    sigset_t callback_mask = action.guest_mask;
    if (!(action.guest_flags & SA_NODEFER)) sigaddset(&callback_mask, sig);
    if (sigprocmask(SIG_BLOCK, &callback_mask, nullptr))
      elfconv_runtime_error("Cannot apply guest signal mask at safe point.\n");
    const State interrupted = *state;
    const uint32_t argument = uint32_t(sig);
    const int saved_errno = errno;
    try {
      invoke(arena, state, runtime, action.arena_handler, &argument, 1);
    } catch (const I386NonlocalJump &) {
      if (!netextra_jump_saved_mask) sigprocmask(SIG_SETMASK, &old_mask, nullptr);
      netextra_jump_saved_mask = false;
      throw;
    } catch (...) {
      *state = interrupted;
      sigprocmask(SIG_SETMASK, &old_mask, nullptr);
      errno = saved_errno;
      throw;
    }
    *state = interrupted;
    if (sigprocmask(SIG_SETMASK, &old_mask, nullptr))
      elfconv_runtime_error("Cannot restore signal mask after guest callback.\n");
    netextra_jump_saved_mask = false;
    errno = saved_errno;
  }
}
// SDL2 imports whose ABI needs explicit guest/host translation.
#include <SDL2/SDL_syswm.h>

namespace {
struct GuestRenderer { SDL_Renderer *host; uint32_t window; };
struct GuestTexture { SDL_Texture *host; uint32_t renderer; int width, height; };
GuestRenderer guest_renderers[16] = {};
GuestTexture guest_textures[64] = {};
struct GuestSurface { SDL_Surface *host; uint32_t surface, format, pixels, palette, colors; };
struct GuestCursor { SDL_Cursor *host; bool owned; };
GuestSurface guest_surfaces[32] = {};
GuestCursor guest_cursors[32] = {};
std::map<uint32_t, unsigned> guest_event_pointers;
std::mutex guest_event_pointer_mutex;
void track_guest_event_pointer(uint32_t address, int delta) {
  if (!address) return;
  std::lock_guard<std::mutex> lock(guest_event_pointer_mutex);
  auto it = guest_event_pointers.find(address);
  if (delta > 0) {
    if (it == guest_event_pointers.end()) guest_event_pointers.emplace(address, unsigned(delta));
    else it->second += unsigned(delta);
  } else if (it != guest_event_pointers.end()) {
    if (it->second <= unsigned(-delta)) guest_event_pointers.erase(it);
    else it->second -= unsigned(-delta);
  }
}
bool guest_event_pointer(void *pointer, uint32_t &address, bool consume = false) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
  if (!value) { address = 0; return true; }
  if (value > UINT32_MAX) return false;
  address = uint32_t(value);
  std::lock_guard<std::mutex> lock(guest_event_pointer_mutex);
  auto it = guest_event_pointers.find(address);
  if (it == guest_event_pointers.end()) return false;
  if (consume && --it->second == 0) guest_event_pointers.erase(it);
  return true;
}
GuestSurface *surface_for(uint32_t address) {
  for (auto &s : guest_surfaces) if (s.host && s.surface == address) return &s;
  return nullptr;
}
uint32_t marshal_surface(uint8_t *arena, SDL_Surface *host) {
  if (!host) return 0;
  unsigned slot = 0; while (slot < 32 && guest_surfaces[slot].host) ++slot;
  if (slot == 32) { SDL_FreeSurface(host); errno = ENOMEM; return 0; }
  const uint64_t bytes64 = host->pitch > 0 && host->h > 0 ? uint64_t(uint32_t(host->pitch)) * uint32_t(host->h) : 0;
  if (bytes64 > UINT32_MAX || (bytes64 && !host->pixels)) { SDL_FreeSurface(host); errno = ENOMEM; return 0; }
  const uint32_t bytes = uint32_t(bytes64);
  const int color_count = host->format && host->format->palette ? host->format->palette->ncolors : 0;
  if (color_count < 0 || uint64_t(color_count) * 4 > UINT32_MAX ||
      (color_count && !host->format->palette->colors)) { SDL_FreeSurface(host); errno = ENOMEM; return 0; }
  const uint32_t surface = allocate(60), format = allocate(44);
  const uint32_t pixels = bytes ? allocate(bytes) : 0;
  const uint32_t palette = color_count > 0 ? allocate(16) : 0;
  const uint32_t colors = color_count > 0 ? allocate(uint32_t(color_count) * 4) : 0;
  if (!surface || !format || (bytes && !pixels) || (color_count > 0 && (!palette || !colors))) {
    if (surface) release(surface); if (format) release(format); if (pixels) release(pixels);
    if (palette) release(palette); if (colors) release(colors);
    SDL_FreeSurface(host); errno = ENOMEM; return 0;
  }
  memset(guest(arena, surface, 60), 0, 60); memset(guest(arena, format, 44), 0, 44);
  if (pixels) memcpy(guest(arena, pixels, bytes), host->pixels, bytes);
  auto *s = static_cast<uint8_t *>(guest(arena, surface, 60));
  auto *f = static_cast<uint8_t *>(guest(arena, format, 44));
  memcpy(s, &host->flags, 4); memcpy(s + 4, &format, 4); memcpy(s + 8, &host->w, 4);
  memcpy(s + 12, &host->h, 4); memcpy(s + 16, &host->pitch, 4); memcpy(s + 20, &pixels, 4);
  memcpy(s + 28, &host->locked, 4); memcpy(s + 36, &host->clip_rect, 16); memcpy(s + 56, &host->refcount, 4);
  memcpy(f, &host->format->format, 4); memcpy(f + 4, &palette, 4);
  const uint8_t bits[4] = {host->format->BitsPerPixel, host->format->BytesPerPixel, 0, 0}; memcpy(f + 8, bits, 4);
  memcpy(f + 12, &host->format->Rmask, 4); memcpy(f + 16, &host->format->Gmask, 4);
  memcpy(f + 20, &host->format->Bmask, 4); memcpy(f + 24, &host->format->Amask, 4);
  const uint8_t losses[4] = {host->format->Rloss, host->format->Gloss, host->format->Bloss, host->format->Aloss}; memcpy(f + 28, losses, 4);
  const uint8_t shifts[4] = {host->format->Rshift, host->format->Gshift, host->format->Bshift, host->format->Ashift}; memcpy(f + 32, shifts, 4);
  memcpy(f + 36, &host->format->refcount, 4);
  if (palette) {
    auto *p = static_cast<uint8_t *>(guest(arena, palette, 16));
    memcpy(p, &color_count, 4); memcpy(p + 4, &colors, 4);
    memcpy(p + 8, &host->format->palette->version, 4); memcpy(p + 12, &host->format->palette->refcount, 4);
    auto *dst = static_cast<uint8_t *>(guest(arena, colors, uint32_t(color_count) * 4));
    for (int i = 0; i < color_count; ++i) { dst[4*i] = host->format->palette->colors[i].r; dst[4*i+1] = host->format->palette->colors[i].g; dst[4*i+2] = host->format->palette->colors[i].b; dst[4*i+3] = host->format->palette->colors[i].a; }
  }
  guest_surfaces[slot] = {host, surface, format, pixels, palette, colors}; return surface;
}
bool write_event(uint8_t *arena, uint32_t address, SDL_Event &event) {
  if (!address) return true;
  auto *out = static_cast<uint8_t *>(guest(arena, address, 56));
  memset(out, 0, 56);
  if (event.type == SDL_SYSWMEVENT) { SDL_SetError("SDL_SYSWMEVENT contains native window-manager pointers"); return false; }
  if (event.type >= SDL_USEREVENT) {
    memcpy(out, &event.user.type, 4); memcpy(out + 4, &event.user.timestamp, 4);
    memcpy(out + 8, &event.user.windowID, 4); memcpy(out + 12, &event.user.code, 4);
    uint32_t a, b;
    if (!guest_event_pointer(event.user.data1, a) || !guest_event_pointer(event.user.data2, b)) {
      SDL_SetError("SDL user event contains an unregistered native pointer"); return false;
    }
    if (!guest_event_pointer(event.user.data1, a, true) || !guest_event_pointer(event.user.data2, b, true)) return false;
    memcpy(out + 16, &a, 4); memcpy(out + 20, &b, 4); return true;
  }
  if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT ||
      event.type == SDL_DROPBEGIN || event.type == SDL_DROPCOMPLETE) {
    memcpy(out, &event.drop.type, 4); memcpy(out + 4, &event.drop.timestamp, 4);
    const bool has_file = event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT;
    uint32_t p = has_file ? owned_string(arena, event.drop.file) : 0, id = event.drop.windowID;
    memcpy(out + 8, &p, 4); memcpy(out + 12, &id, 4);
    if (has_file && event.drop.file) SDL_free(event.drop.file); return true;
  }
  if (event.type == SDL_TEXTEDITING_EXT) {
    memcpy(out, &event.editExt.type, 12);
    uint32_t p = owned_string(arena, event.editExt.text);
    memcpy(out + 12, &p, 4); memcpy(out + 16, &event.editExt.start, 4); memcpy(out + 20, &event.editExt.length, 4);
    if (event.editExt.text) SDL_free(event.editExt.text); return true;
  }
  memcpy(out, &event, 56); return true;
}
struct GuestEventWatch {
  uint32_t callback;
  uint32_t userdata;
};
std::mutex guest_event_watches_mutex;
std::shared_ptr<const std::vector<GuestEventWatch>> guest_event_watches =
    std::make_shared<const std::vector<GuestEventWatch>>();
void dispatch_guest_event_watches(uint8_t *arena, State *state, RuntimeManager *runtime,
                                  uint32_t event_address) {
  std::shared_ptr<const std::vector<GuestEventWatch>> snapshot;
  {
    std::lock_guard<std::mutex> lock(guest_event_watches_mutex);
    snapshot = guest_event_watches;
  }
  for (const auto &watch : *snapshot) {
    const uint32_t args[2] = {watch.userdata, event_address};
    invoke(arena, state, runtime, watch.callback, args, 2);
  }
}
IMPORT(SDL_AddEventWatch) {
  CALL;
  const uint32_t callback = call.word(0), userdata = call.word(1);
  if (!callback) { SDL_SetError("SDL_AddEventWatch requires a callback"); return; }
  std::lock_guard<std::mutex> lock(guest_event_watches_mutex);
  auto next = std::make_shared<std::vector<GuestEventWatch>>(*guest_event_watches);
  next->push_back({callback, userdata});
  guest_event_watches = std::move(next);
}
IMPORT(SDL_DelEventWatch) {
  CALL;
  const uint32_t callback = call.word(0), userdata = call.word(1);
  std::lock_guard<std::mutex> lock(guest_event_watches_mutex);
  auto next = std::make_shared<std::vector<GuestEventWatch>>(*guest_event_watches);
  auto it = std::find_if(next->begin(), next->end(), [&](const GuestEventWatch &watch) {
    return watch.callback == callback && watch.userdata == userdata;
  });
  if (it != next->end()) {
    next->erase(it);
    guest_event_watches = std::move(next);
  }
}
bool read_event(uint8_t *arena, uint32_t address, SDL_Event &event) {
  auto *in = static_cast<uint8_t *>(guest(arena, address, 56));
  memset(&event, 0, sizeof(event));
  memcpy(&event.type, in, 4);
  if (event.type == SDL_SYSWMEVENT) { SDL_SetError("Cannot push SDL_SYSWMEVENT across the guest ABI"); return false; }
  if (event.type >= SDL_USEREVENT) {
    memcpy(&event.user.type, in, 4); memcpy(&event.user.timestamp, in + 4, 4);
    memcpy(&event.user.windowID, in + 8, 4); memcpy(&event.user.code, in + 12, 4);
    uint32_t a, b; memcpy(&a, in + 16, 4); memcpy(&b, in + 20, 4);
    event.user.data1 = reinterpret_cast<void *>(uintptr_t(a)); event.user.data2 = reinterpret_cast<void *>(uintptr_t(b)); return true;
  }
  if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT ||
      event.type == SDL_DROPBEGIN || event.type == SDL_DROPCOMPLETE) {
    memcpy(&event.drop.type, in, 4); memcpy(&event.drop.timestamp, in + 4, 4);
    uint32_t p = 0, id; memcpy(&id, in + 12, 4); event.drop.windowID = id;
    if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT) {
      memcpy(&p, in + 8, 4); event.drop.file = p ? SDL_strdup(text(arena, p)) : nullptr;
      return !p || event.drop.file;
    }
    event.drop.file = nullptr; return true;
  }
  if (event.type == SDL_TEXTEDITING_EXT) {
    memcpy(&event.editExt.type, in, 12); uint32_t p; memcpy(&p, in + 12, 4);
    memcpy(&event.editExt.start, in + 16, 4); memcpy(&event.editExt.length, in + 20, 4);
    event.editExt.text = p ? SDL_strdup(text(arena, p)) : nullptr; return !p || event.editExt.text;
  }
  memcpy(&event, in, 56); return true;
}
uint32_t cursor_handle(SDL_Cursor *host, bool owned) {
  if (!host) return 0;
  for (unsigned i = 0; i < 32; ++i) {
    if (guest_cursors[i].host == host) {
      guest_cursors[i].owned = guest_cursors[i].owned || owned;
      return 0x600 + i;
    }
  }
  for (unsigned i = 0; i < 32; ++i) {
    if (!guest_cursors[i].host) { guest_cursors[i] = {host, owned}; return 0x600 + i; }
  }
  if (owned) SDL_FreeCursor(host);
  SDL_SetError("Guest SDL cursor handle table exhausted"); return 0;
}
SDL_Cursor *cursor(uint32_t handle) {
  if (!handle) return nullptr;
  if (handle < 0x600 || handle >= 0x620 || !guest_cursors[handle - 0x600].host)
    elfconv_runtime_error("Invalid guest SDL cursor handle 0x%x.\n", handle);
  return guest_cursors[handle - 0x600].host;
}
void sync_surface(uint8_t *arena, GuestSurface &s) {
  SDL_Surface *host = s.host;
  const uint64_t bytes = host->pitch > 0 && host->h > 0 ? uint64_t(uint32_t(host->pitch)) * uint32_t(host->h) : 0;
  if (s.pixels && bytes <= UINT32_MAX) memcpy(host->pixels, guest(arena, s.pixels, size_t(bytes)), size_t(bytes));
  auto *f = static_cast<uint8_t *>(guest(arena, s.format, 44));
  memcpy(&host->format->format, f, 4);
  host->format->BitsPerPixel = f[8]; host->format->BytesPerPixel = f[9];
  memcpy(&host->format->Rmask, f + 12, 4); memcpy(&host->format->Gmask, f + 16, 4);
  memcpy(&host->format->Bmask, f + 20, 4); memcpy(&host->format->Amask, f + 24, 4);
  host->format->Rloss = f[28]; host->format->Gloss = f[29]; host->format->Bloss = f[30]; host->format->Aloss = f[31];
  host->format->Rshift = f[32]; host->format->Gshift = f[33]; host->format->Bshift = f[34]; host->format->Ashift = f[35];
  if (s.colors && host->format->palette) {
    auto *colors = static_cast<uint8_t *>(guest(arena, s.colors, uint32_t(host->format->palette->ncolors) * 4));
    for (int i = 0; i < host->format->palette->ncolors; ++i) {
      host->format->palette->colors[i].r = colors[4*i]; host->format->palette->colors[i].g = colors[4*i+1];
      host->format->palette->colors[i].b = colors[4*i+2]; host->format->palette->colors[i].a = colors[4*i+3];
    }
    SDL_SetPaletteColors(host->format->palette, host->format->palette->colors, 0, host->format->palette->ncolors);
  }
}
SDL_Renderer *guest_renderer(uint32_t handle) {
  if (handle < 0x700 || handle >= 0x710 || !guest_renderers[handle - 0x700].host)
    elfconv_runtime_error("Invalid guest SDL renderer handle 0x%x.\n", handle);
  return guest_renderers[handle - 0x700].host;
}
GuestTexture *guest_texture(uint32_t handle) {
  if (handle < 0x800 || handle >= 0x840 || !guest_textures[handle - 0x800].host)
    elfconv_runtime_error("Invalid guest SDL texture handle 0x%x.\n", handle);
  return &guest_textures[handle - 0x800];
}
bool guest_rect(uint8_t *arena, uint32_t address, SDL_Rect &rect) {
  static_assert(sizeof(SDL_Rect) == 16);
  if (!address) return true;
  memcpy(&rect, guest(arena, address, sizeof(rect)), sizeof(rect));
  return rect.w >= 0 && rect.h >= 0;
}
void destroy_renderer_slot(unsigned slot) {
  const uint32_t handle = 0x700 + slot;
  for (auto &entry : guest_textures)
    if (entry.host && entry.renderer == handle) entry = {};
  if (guest_renderers[slot].host) SDL_DestroyRenderer(guest_renderers[slot].host);
  guest_renderers[slot] = {};
}
void reset_guest_renderers() {
  for (unsigned i = 0; i < 16; ++i) destroy_renderer_slot(i);
  for (auto &entry : guest_textures) entry = {};
}
void destroy_guest_renderers_for_window(uint32_t handle) {
  for (unsigned i = 0; i < 16; ++i)
    if (guest_renderers[i].host && guest_renderers[i].window == handle)
      destroy_renderer_slot(i);
}
}
IMPORT(SDL_FreeSurface) {
  CALL; const uint32_t handle = call.word(0); GuestSurface *s = surface_for(handle);
  if (handle && !s) elfconv_runtime_error("Invalid guest SDL surface handle 0x%x.\n", handle);
  if (s) {
    SDL_FreeSurface(s->host); release(s->surface); release(s->format);
    if (s->pixels) release(s->pixels); if (s->palette) release(s->palette); if (s->colors) release(s->colors); *s = {};
  }
}
IMPORT(SDL_GL_LoadLibrary) { CALL; call.result(SDL_GL_LoadLibrary(call.word(0) ? text(arena, call.word(0)) : nullptr)); }
IMPORT(SDL_GL_UnloadLibrary) { CALL; SDL_GL_UnloadLibrary(); }
IMPORT(SDL_GL_MakeCurrent) {
  CALL; SDL_Window *w = call.word(0) ? window(call.word(0)) : nullptr;
#ifdef __EMSCRIPTEN_PTHREADS__
  if (!call.word(1)) call.result(emscripten_webgl_make_context_current(0));
  else call.result(emscripten_webgl_make_context_current(uintptr_t(context(call.word(1)))) == EMSCRIPTEN_RESULT_SUCCESS ? 0 : -1);
#else
  call.result(SDL_GL_MakeCurrent(w, call.word(1) ? context(call.word(1)) : nullptr));
#endif
}
IMPORT(glXGetCurrentDisplay) {
  CALL;
  // WebGL has no GLX display; expose a callable entry point that returns NULL.
  call.result(0);
}
IMPORT(glXQueryExtensionsString) {
  CALL;
  call.result(copy_string(arena, "", 31));
}
IMPORT(SDL_GL_GetProcAddress) {
  CALL; const char *name = text(arena, call.word(0));
  struct Entry { const char *name; LiftedFunc adapter; };
  static const Entry entries[] = {
    {"glXGetCurrentDisplay", __ecv_i386_glXGetCurrentDisplay},
    {"glXQueryExtensionsString", __ecv_i386_glXQueryExtensionsString},
    {"glGetString", __ecv_i386_glGetString}, {"glGetIntegerv", __ecv_i386_glGetIntegerv},
    {"glGetBooleanv", __ecv_i386_glGetBooleanv},
    {"glGetDoublev", __ecv_i386_glGetDoublev},
    {"glGetFloatv", __ecv_i386_glGetFloatv},
    {"glGetCompressedTexImage", __ecv_i386_glGetCompressedTexImage},
    {"glGetTexImage", __ecv_i386_glGetTexImage},
    {"glGetTexLevelParameteriv", __ecv_i386_glGetTexLevelParameteriv},
    {"glGetError", __ecv_i386_glGetError},
    {"glAttachObjectARB", __ecv_i386_glAttachShader},
    {"glDetachObjectARB", __ecv_i386_glDetachShader},
    {"glBindAttribLocationARB", __ecv_i386_glBindAttribLocation},
    {"glBindBufferARB", __ecv_i386_glBindBuffer},
    {"glBufferDataARB", __ecv_i386_glBufferData},
    {"glBufferSubDataARB", __ecv_i386_glBufferSubData},
    {"glCompileShaderARB", __ecv_i386_glCompileShader},
    {"glCreateProgramObjectARB", __ecv_i386_glCreateProgram},
    {"glCreateShaderObjectARB", __ecv_i386_glCreateShader},
    {"glDeleteBuffersARB", __ecv_i386_glDeleteBuffers},
    {"glGenBuffersARB", __ecv_i386_glGenBuffers},
    {"glGetProgramivARB", __ecv_i386_glGetProgramiv},
    {"glGetInfoLogARB", __ecv_i386_glGetInfoLogARB},
    {"glGetObjectParameterivARB", __ecv_i386_glGetObjectParameterivARB},
    {"glGetUniformLocationARB", __ecv_i386_glGetUniformLocation},
    {"glLinkProgramARB", __ecv_i386_glLinkProgram},
    {"glShaderSourceARB", __ecv_i386_glShaderSource},
    {"glUniform1iARB", __ecv_i386_glUniform1i},
    {"glClear", __ecv_i386_glClear}, {"glViewport", __ecv_i386_glViewport},
    {"glClearDepth", __ecv_i386_glClearDepth},
    {"glClearStencil", __ecv_i386_glClearStencil},
    {"glClipPlane", __ecv_i386_glClipPlane},
    {"glColorMask", __ecv_i386_glColorMask},
    {"glColorMaskIndexedEXT", __ecv_i386_glColorMaskIndexedEXT},
    {"glEnableIndexedEXT", __ecv_i386_glEnableIndexedEXT},
    {"glDisableIndexedEXT", __ecv_i386_glDisableIndexedEXT},
    {"glGetBooleanIndexedvEXT", __ecv_i386_glGetBooleanIndexedvEXT},
    {"glPixelStorei", __ecv_i386_glPixelStorei}, {"glClearColor", __ecv_i386_glClearColor},
    {"glReadPixels", __ecv_i386_glReadPixels}, {"glCreateShader", __ecv_i386_glCreateShader},
    {"glCompileShader", __ecv_i386_glCompileShader}, {"glDeleteShader", __ecv_i386_glDeleteShader},
    {"glCreateProgram", __ecv_i386_glCreateProgram}, {"glAttachShader", __ecv_i386_glAttachShader},
    {"glBindAttribLocation", __ecv_i386_glBindAttribLocation}, {"glLinkProgram", __ecv_i386_glLinkProgram},
    {"glUseProgram", __ecv_i386_glUseProgram}, {"glDeleteProgram", __ecv_i386_glDeleteProgram},
    {"glGetUniformLocation", __ecv_i386_glGetUniformLocation}, {"glEnable", __ecv_i386_glEnable},
    {"glIsEnabled", __ecv_i386_glIsEnabled},
    {"glEnableVertexAttribArray", __ecv_i386_glEnableVertexAttribArray}, {"glBindBuffer", __ecv_i386_glBindBuffer},
    {"glShaderSource", __ecv_i386_glShaderSource}, {"glGetShaderiv", __ecv_i386_glGetShaderiv},
    {"glGetProgramiv", __ecv_i386_glGetProgramiv}, {"glGetShaderInfoLog", __ecv_i386_glGetShaderInfoLog},
    {"glGetProgramInfoLog", __ecv_i386_glGetProgramInfoLog}, {"glGenBuffers", __ecv_i386_glGenBuffers},
    {"glDeleteBuffers", __ecv_i386_glDeleteBuffers}, {"glBufferData", __ecv_i386_glBufferData},
    {"glBufferSubData", __ecv_i386_glBufferSubData},
    {"glGenSamplers", __ecv_i386_glGenSamplers},
    {"glDeleteSamplers", __ecv_i386_glDeleteSamplers},
    {"glBindSampler", __ecv_i386_glBindSampler},
    {"glSamplerParameteri", __ecv_i386_glSamplerParameteri},
    {"glSamplerParameterf", __ecv_i386_glSamplerParameterf},
    {"glSamplerParameterfv", __ecv_i386_glSamplerParameterfv},
    {"glFenceSync", __ecv_i386_glFenceSync},
    {"glIsSync", __ecv_i386_glIsSync},
    {"glDeleteSync", __ecv_i386_glDeleteSync},
    {"glClientWaitSync", __ecv_i386_glClientWaitSync},
    {"glWaitSync", __ecv_i386_glWaitSync},
    {"glGetSynciv", __ecv_i386_glGetSynciv},
    {"glGetInteger64v", __ecv_i386_glGetInteger64v},
    {"glGenQueries", __ecv_i386_glGenQueries},
    {"glDeleteQueries", __ecv_i386_glDeleteQueries},
    {"glBeginQuery", __ecv_i386_glBeginQuery},
    {"glEndQuery", __ecv_i386_glEndQuery},
    {"glQueryCounter", __ecv_i386_glQueryCounter},
    {"glGetQueryObjectiv", __ecv_i386_glGetQueryObjectiv},
    {"glGetQueryObjectui64v", __ecv_i386_glGetQueryObjectui64v},
    {"glCopyBufferSubData", __ecv_i386_glCopyBufferSubData},
    {"glBlitFramebuffer", __ecv_i386_glBlitFramebuffer},
    {"glBindFramebuffer", __ecv_i386_glBindFramebuffer},
    {"glBindFramebufferEXT", __ecv_i386_glBindFramebuffer},
    {"glGenFramebuffers", __ecv_i386_glGenFramebuffers},
    {"glGenFramebuffersEXT", __ecv_i386_glGenFramebuffers},
    {"glDeleteFramebuffers", __ecv_i386_glDeleteFramebuffers},
    {"glDeleteFramebuffersEXT", __ecv_i386_glDeleteFramebuffers},
    {"glCheckFramebufferStatus", __ecv_i386_glCheckFramebufferStatus},
    {"glCheckFramebufferStatusEXT", __ecv_i386_glCheckFramebufferStatus},
    {"glFramebufferTexture2D", __ecv_i386_glFramebufferTexture2D},
    {"glFramebufferTexture2DEXT", __ecv_i386_glFramebufferTexture2D},
    {"glFramebufferTexture3DEXT", __ecv_i386_glFramebufferTexture3DEXT},
    {"glFramebufferTexture3D", __ecv_i386_glFramebufferTexture3DEXT},
    {"glFramebufferTextureLayer", __ecv_i386_glFramebufferTextureLayer},
    {"glFramebufferRenderbuffer", __ecv_i386_glFramebufferRenderbuffer},
    {"glFramebufferRenderbufferEXT", __ecv_i386_glFramebufferRenderbuffer},
    {"glBindRenderbuffer", __ecv_i386_glBindRenderbuffer},
    {"glBindRenderbufferEXT", __ecv_i386_glBindRenderbuffer},
    {"glGenRenderbuffers", __ecv_i386_glGenRenderbuffers},
    {"glGenRenderbuffersEXT", __ecv_i386_glGenRenderbuffers},
    {"glDeleteRenderbuffers", __ecv_i386_glDeleteRenderbuffers},
    {"glDeleteRenderbuffersEXT", __ecv_i386_glDeleteRenderbuffers},
    {"glRenderbufferStorage", __ecv_i386_glRenderbufferStorage},
    {"glRenderbufferStorageEXT", __ecv_i386_glRenderbufferStorage},
    {"glRenderbufferStorageMultisample", __ecv_i386_glRenderbufferStorageMultisample},
    {"glRenderbufferStorageMultisampleEXT", __ecv_i386_glRenderbufferStorageMultisample},
    {"glGetFramebufferAttachmentParameteriv", __ecv_i386_glGetFramebufferAttachmentParameteriv},
    {"glGetFramebufferAttachmentParameterivEXT", __ecv_i386_glGetFramebufferAttachmentParameteriv},
    {"glGetRenderbufferParameteriv", __ecv_i386_glGetRenderbufferParameteriv},
    {"glGetRenderbufferParameterivEXT", __ecv_i386_glGetRenderbufferParameteriv},
    {"glIsFramebuffer", __ecv_i386_glIsFramebuffer},
    {"glIsFramebufferEXT", __ecv_i386_glIsFramebuffer},
    {"glIsRenderbuffer", __ecv_i386_glIsRenderbuffer},
    {"glIsRenderbufferEXT", __ecv_i386_glIsRenderbuffer},
    {"glGenVertexArrays", __ecv_i386_glGenVertexArrays},
    {"glDeleteVertexArrays", __ecv_i386_glDeleteVertexArrays},
    {"glBindVertexArray", __ecv_i386_glBindVertexArray},
    {"glVertexAttribPointer", __ecv_i386_glVertexAttribPointer}, {"glUniformMatrix4fv", __ecv_i386_glUniformMatrix4fv},
    {"glDrawElements", __ecv_i386_glDrawElements}, {"glDrawArrays", __ecv_i386_glDrawArrays},
    {"glDrawBuffer", __ecv_i386_glDrawBuffer},
    {"glDrawBuffers", __ecv_i386_glDrawBuffers},
    {"glReadBuffer", __ecv_i386_glReadBuffer},
    {"glDrawRangeElements", __ecv_i386_glDrawRangeElements},
    {"glDrawRangeElementsBaseVertex", __ecv_i386_glDrawRangeElementsBaseVertex},
    {"glFinish", __ecv_i386_glFinish},
    {"glFlush", __ecv_i386_glFlush},
    {"glFrontFace", __ecv_i386_glFrontFace},
    {"glGenTextures", __ecv_i386_glGenTextures}, {"glBindTexture", __ecv_i386_glBindTexture},
    {"glIsTexture", __ecv_i386_glIsTexture},
    {"glDeleteTextures", __ecv_i386_glDeleteTextures},
    {"glTexImage2D", __ecv_i386_glTexImage2D}, {"glTexParameteri", __ecv_i386_glTexParameteri},
    {"glTexSubImage2D", __ecv_i386_glTexSubImage2D},
    {"glTexParameterfv", __ecv_i386_glTexParameterfv},
    {"glTexImage3D", __ecv_i386_glTexImage3D},
    {"glCompressedTexImage2D", __ecv_i386_glCompressedTexImage2D},
    {"glCompressedTexImage3D", __ecv_i386_glCompressedTexImage3D},
    {"glDepthFunc", __ecv_i386_glDepthFunc}, {"glDepthMask", __ecv_i386_glDepthMask},
    {"glDepthRange", __ecv_i386_glDepthRange},
    {"glBlendColor", __ecv_i386_glBlendColor},
    {"glBlendEquation", __ecv_i386_glBlendEquation},
    {"glBlendFunc", __ecv_i386_glBlendFunc}, {"glScissor", __ecv_i386_glScissor},
    {"glStencilFunc", __ecv_i386_glStencilFunc},
    {"glStencilMask", __ecv_i386_glStencilMask},
    {"glStencilOp", __ecv_i386_glStencilOp},
    {"glStencilFuncSeparate", __ecv_i386_glStencilFuncSeparate},
    {"glStencilOpSeparate", __ecv_i386_glStencilOpSeparate},
    {"glPolygonOffset", __ecv_i386_glPolygonOffset}, {"glUniform1f", __ecv_i386_glUniform1f},
    {"glUniform1i", __ecv_i386_glUniform1i}, {"glUniform2f", __ecv_i386_glUniform2f},
    {"glUniform4fv", __ecv_i386_glUniform4fv},
    {"glGetAttribLocation", __ecv_i386_glGetAttribLocation},
    {"glDisableVertexAttribArray", __ecv_i386_glDisableVertexAttribArray},
    {"glDisable", __ecv_i386_glDisable}, {"glActiveTexture", __ecv_i386_glActiveTexture},
#if ECV_LEGACY_GL
    {"glAlphaFunc", __ecv_i386_glAlphaFunc},
    {"glBindProgramARB", __ecv_i386_glBindProgramARB},
    {"glGenProgramsARB", __ecv_i386_glGenProgramsARB},
    {"glProgramStringARB", __ecv_i386_glProgramStringARB},
    {"glDeleteObjectARB", __ecv_i386_glDeleteObjectARB},
    {"glDeleteProgramsARB", __ecv_i386_glDeleteProgramsARB},
    {"glDeleteQueriesARB", __ecv_i386_glDeleteQueriesARB},
    {"glGenQueriesARB", __ecv_i386_glGenQueriesARB},
    {"glMapBufferARB", __ecv_i386_glMapBufferARB},
    {"glUnmapBuffer", __ecv_i386_glUnmapBuffer},
    {"glBegin", __ecv_i386_glBegin}, {"glEnd", __ecv_i386_glEnd},
    {"glOrtho", __ecv_i386_glOrtho},
    {"glPolygonMode", __ecv_i386_glPolygonMode},
    {"glPushAttrib", __ecv_i386_glPushAttrib},
    {"glPopAttrib", __ecv_i386_glPopAttrib},
    {"glPushClientAttrib", __ecv_i386_glPushClientAttrib},
    {"glPopClientAttrib", __ecv_i386_glPopClientAttrib},
    {"glMatrixMode", __ecv_i386_glMatrixMode}, {"glLoadIdentity", __ecv_i386_glLoadIdentity},
    {"glVertex2f", __ecv_i386_glVertex2f}, {"glColor3f", __ecv_i386_glColor3f},
    {"glTexCoord2f", __ecv_i386_glTexCoord2f},
    {"glVertex3f", __ecv_i386_glVertex3f},
    {"glColor4f", __ecv_i386_glColor4f},
    {"glColor4sv", __ecv_i386_glColor4sv},
    {"glProgramEnvParameters4fvEXT", __ecv_i386_glProgramEnvParameters4fvEXT},
    {"glEnableClientState", __ecv_i386_glEnableClientState},
    {"glDisableClientState", __ecv_i386_glDisableClientState},
    {"glClientActiveTexture", __ecv_i386_glClientActiveTexture},
    {"glVertexPointer", __ecv_i386_glVertexPointer},
    {"glTexCoordPointer", __ecv_i386_glTexCoordPointer},
#endif
  };
  for (const auto &e : entries) {
    if (strcmp(name, e.name)) continue;
    const auto address = register_guest_adapter(arena, runtime, e.adapter);
#ifdef __EMSCRIPTEN__
    if (!strcmp(name, "glGetError") || !strcmp(name, "glColorMaskIndexedEXT") ||
        !strcmp(name, "glGenTextures")) {
      EM_ASM({ if (Module.elfconvTrace)
        Module.printErr('SDL_GL_GetProcAddress worker='+
          (typeof workerID==='number'?workerID:-1)+' pc=0x'+$0.toString(16)+
          ' name='+UTF8ToString($1)+' address=0x'+$2.toString(16)); },
        state->gpr.rip.dword, uintptr_t(name), address);
    }
#endif
    call.result(address);
    return;
  }
#ifdef __EMSCRIPTEN__
  EM_ASM({
    if (Module.elfconvTrace && (Module.__elfconvUnknownGLCount || 0) < 32) {
      Module.__elfconvUnknownGLCount = (Module.__elfconvUnknownGLCount || 0) + 1;
      Module.printErr('SDL_GL_GetProcAddress unsupported worker=' +
        (typeof workerID === 'number' ? workerID : -1) + ' name=' + UTF8ToString($0));
    }
  }, uintptr_t(name));
#endif
  SDL_SetError("Unsupported GL function: %s", name);
  call.result(0);
}
IMPORT(SDL_GetCurrentAudioDriver) { CALL; call.result(copy_string(arena, SDL_GetCurrentAudioDriver(), 9)); }
IMPORT(SDL_GetCurrentVideoDriver) { CALL; call.result(copy_string(arena, SDL_GetCurrentVideoDriver(), 10)); }
IMPORT(SDL_GetCurrentDisplayMode) {
  CALL; SDL_DisplayMode m; int r = SDL_GetCurrentDisplayMode(int(call.word(0)), &m);
  if (!r && call.word(1)) { auto *p = static_cast<uint8_t *>(guest(arena, call.word(1), 20)); memset(p, 0, 20); memcpy(p, &m.format, 4); memcpy(p + 4, &m.w, 4); memcpy(p + 8, &m.h, 4); memcpy(p + 12, &m.refresh_rate, 4); }
  call.result(r);
}
IMPORT(SDL_GetDesktopDisplayMode) {
  CALL; SDL_DisplayMode m; int r = SDL_GetDesktopDisplayMode(int(call.word(0)), &m);
  if (!r && call.word(1)) { auto *p = static_cast<uint8_t *>(guest(arena, call.word(1), 20)); memset(p, 0, 20); memcpy(p, &m.format, 4); memcpy(p + 4, &m.w, 4); memcpy(p + 8, &m.h, 4); memcpy(p + 12, &m.refresh_rate, 4); }
  call.result(r);
}
IMPORT(SDL_GetDisplayBounds) { CALL; SDL_Rect r; int x = SDL_GetDisplayBounds(int(call.word(0)), &r); if (!x && call.word(1)) memcpy(guest(arena, call.word(1), 16), &r, 16); call.result(x); }
IMPORT(SDL_GetNumVideoDisplays) { CALL; call.result(SDL_GetNumVideoDisplays()); }
IMPORT(SDL_GetWindowGrab) { CALL; call.result(SDL_GetWindowGrab(window(call.word(0)))); }
IMPORT(SDL_GetWindowID) { CALL; call.result(SDL_GetWindowID(window(call.word(0)))); }
IMPORT(SDL_GetWindowWMInfo) {
  CALL; (void)window(call.word(0));
  SDL_SetError("SDL_GetWindowWMInfo cannot expose native window-manager pointers to the guest");
  call.result(0);
}
IMPORT(SDL_HideWindow) { CALL; SDL_HideWindow(window(call.word(0))); }
IMPORT(SDL_LoadBMP_RW) { CALL; uint32_t h = call.word(0); SDL_Surface *s = SDL_LoadBMP_RW(rwops(h), int(call.word(1))); if (call.word(1) && h) guest_rwops[h - 0x500] = nullptr; call.result(marshal_surface(arena, s)); }
IMPORT(SDL_PushEvent) {
  CALL; SDL_Event e;
  if (!read_event(arena, call.word(0), e)) { call.result(-1); return; }
  const bool user = e.type >= SDL_USEREVENT;
  const uint32_t data1 = user ? uint32_t(reinterpret_cast<uintptr_t>(e.user.data1)) : 0;
  const uint32_t data2 = user ? uint32_t(reinterpret_cast<uintptr_t>(e.user.data2)) : 0;
  track_guest_event_pointer(data1, 1); track_guest_event_pointer(data2, 1);
  int r = SDL_PushEvent(&e);
  if (r <= 0) { track_guest_event_pointer(data1, -1); track_guest_event_pointer(data2, -1); }
  if (r <= 0 && (e.type == SDL_DROPFILE || e.type == SDL_DROPTEXT ||
      e.type == SDL_DROPBEGIN || e.type == SDL_DROPCOMPLETE) && e.drop.file) SDL_free(e.drop.file);
  if (r <= 0 && e.type == SDL_TEXTEDITING_EXT && e.editExt.text) SDL_free(e.editExt.text);
  call.result(r);
}
IMPORT(SDL_RWFromFile) {
  CALL; unsigned slot = 0; while (slot < 32 && guest_rwops[slot]) ++slot;
  if (slot == 32) { errno = ENOMEM; call.result(0); return; }
  SDL_RWops *p = SDL_RWFromFile(text(arena, call.word(0)), text(arena, call.word(1)));
  if (!p) { call.result(0); return; } guest_rwops[slot] = p; call.result(opaque_handle(slot, 0x500, 32));
}
IMPORT(SDL_RaiseWindow) { CALL; SDL_RaiseWindow(window(call.word(0))); }
IMPORT(SDL_SetCursor) { CALL; SDL_SetCursor(cursor(call.word(0))); }
IMPORT(SDL_CreateSystemCursor) { CALL; call.result(cursor_handle(SDL_CreateSystemCursor(SDL_SystemCursor(call.word(0))), true)); }
IMPORT(SDL_CreateColorCursor) {
  CALL; GuestSurface *s = surface_for(call.word(0));
  if (!s) { SDL_SetError("Invalid guest SDL surface handle"); call.result(0); return; }
  sync_surface(arena, *s);
  call.result(cursor_handle(SDL_CreateColorCursor(s->host, int(call.word(1)), int(call.word(2))), true));
}
IMPORT(SDL_CreateCursor) {
  CALL;
  if (!call.word(0) || !call.word(1)) { SDL_SetError("SDL_CreateCursor requires guest bitmap and mask"); call.result(0); return; }
  const int width = int(call.word(2)), height = int(call.word(3));
  if (width <= 0 || height <= 0) { SDL_SetError("Invalid guest cursor dimensions"); call.result(0); return; }
  const uint64_t bytes = ((uint64_t(width) + 7) / 8) * uint64_t(height);
  if (bytes > MEMORY_ARENA_SIZE) { SDL_SetError("Guest cursor bitmap is too large"); call.result(0); return; }
  call.result(cursor_handle(SDL_CreateCursor(static_cast<Uint8 *>(guest(arena, call.word(0), size_t(bytes))),
      static_cast<Uint8 *>(guest(arena, call.word(1), size_t(bytes))), width, height,
      int(call.word(4)), int(call.word(5))), true));
}
IMPORT(SDL_GetCursor) { CALL; call.result(cursor_handle(SDL_GetCursor(), false)); }
IMPORT(SDL_GetDefaultCursor) { CALL; call.result(cursor_handle(SDL_GetDefaultCursor(), false)); }
IMPORT(SDL_FreeCursor) {
  CALL; uint32_t h = call.word(0); if (!h) return;
  SDL_Cursor *p = cursor(h); auto &entry = guest_cursors[h - 0x600];
  if (entry.owned) { SDL_FreeCursor(p); for (auto &alias : guest_cursors) if (alias.host == p) alias = {}; }
  else entry = {};
}
IMPORT(SDL_SetRelativeMouseMode) { CALL; call.result(SDL_SetRelativeMouseMode(SDL_bool(call.word(0)))); }
IMPORT(SDL_SetWindowBordered) { CALL; SDL_SetWindowBordered(window(call.word(0)), SDL_bool(call.word(1))); }
IMPORT(SDL_SetWindowDisplayMode) {
  CALL; SDL_DisplayMode m = {}; SDL_DisplayMode *mp = nullptr;
  if (call.word(1)) { auto *p = static_cast<uint8_t *>(guest(arena, call.word(1), 20)); memcpy(&m.format, p, 4); memcpy(&m.w, p + 4, 4); memcpy(&m.h, p + 8, 4); memcpy(&m.refresh_rate, p + 12, 4); mp = &m; }
  call.result(SDL_SetWindowDisplayMode(window(call.word(0)), mp));
}
IMPORT(SDL_SetWindowGammaRamp) {
  CALL; Uint16 r[256], g[256], b[256];
  for (unsigned c = 0; c < 3; ++c) { uint32_t addr = call.word(c + 1); Uint16 *dst = c == 0 ? r : c == 1 ? g : b; if (addr) memcpy(dst, guest(arena, addr, sizeof(r)), sizeof(r)); }
  call.result(SDL_SetWindowGammaRamp(window(call.word(0)), call.word(1) ? r : nullptr, call.word(2) ? g : nullptr, call.word(3) ? b : nullptr));
}
IMPORT(SDL_SetWindowGrab) { CALL; SDL_SetWindowGrab(window(call.word(0)), SDL_bool(call.word(1))); }
IMPORT(SDL_SetWindowIcon) {
  CALL; GuestSurface *s = surface_for(call.word(1));
  if (call.word(1) && !s) elfconv_runtime_error("Invalid guest SDL surface handle 0x%x.\n", call.word(1));
  if (s) { sync_surface(arena, *s); SDL_SetWindowIcon(window(call.word(0)), s->host); }
}
IMPORT(SDL_SetWindowTitle) { CALL; SDL_SetWindowTitle(window(call.word(0)), text(arena, call.word(1))); }
IMPORT(SDL_ShowMessageBox) {
  CALL; auto *p = static_cast<uint8_t *>(guest(arena, call.word(0), 28)); SDL_MessageBoxData d = {};
  uint32_t title, message, buttons_addr, window_addr, scheme_addr; int num;
  memcpy(&d.flags, p, 4); memcpy(&window_addr, p + 4, 4); memcpy(&title, p + 8, 4); memcpy(&message, p + 12, 4); memcpy(&num, p + 16, 4); memcpy(&buttons_addr, p + 20, 4); memcpy(&scheme_addr, p + 24, 4);
  d.title = text(arena, title); d.message = text(arena, message); d.window = window_addr ? window(window_addr) : nullptr; d.numbuttons = num;
  if (num < 0 || num > 256) { call.result(SDL_SetError("Invalid message box button count")); return; }
  std::vector<SDL_MessageBoxButtonData> buttons(size_t(num), SDL_MessageBoxButtonData{});
  for (int i = 0; i < num; ++i) { auto *b = static_cast<uint8_t *>(guest(arena, buttons_addr + uint32_t(i) * 12, 12)); memcpy(&buttons[i].flags, b, 4); memcpy(&buttons[i].buttonid, b + 4, 4); uint32_t t; memcpy(&t, b + 8, 4); buttons[i].text = text(arena, t); }
  SDL_MessageBoxColorScheme scheme = {};
  if (scheme_addr) { auto *colors = static_cast<uint8_t *>(guest(arena, scheme_addr, 15)); for (unsigned i = 0; i < 5; ++i) { scheme.colors[i].r = colors[3*i]; scheme.colors[i].g = colors[3*i+1]; scheme.colors[i].b = colors[3*i+2]; } d.colorScheme = &scheme; }
  d.buttons = buttons.data(); int selected = -1; int r = SDL_ShowMessageBox(&d, &selected);
  if (call.word(1) && !r) memcpy(guest(arena, call.word(1), 4), &selected, 4); call.result(r);
}
IMPORT(SDL_ShowWindow) { CALL; SDL_ShowWindow(window(call.word(0))); }
IMPORT(SDL_WaitEventTimeout) {
  CALL;
  SDL_Event event;
  int found = SDL_WaitEventTimeout(call.word(0) ? &event : nullptr, int(call.word(1)));
  if (found && call.word(0)) {
    if (!write_event(arena, call.word(0), event)) found = 0;
    else dispatch_guest_event_watches(arena, state, runtime, call.word(0));
  }
  call.result(found);
}
IMPORT(SDL_WarpMouseInWindow) { CALL; SDL_WarpMouseInWindow(call.word(0) ? window(call.word(0)) : nullptr, int(call.word(1)), int(call.word(2))); }
IMPORT(SDL_CreateRenderer) {
  CALL; const uint32_t window_handle = call.word(0);
  unsigned slot = 0; while (slot < 16 && guest_renderers[slot].host) ++slot;
  if (slot == 16) {
    SDL_SetError("Guest SDL renderer handle table exhausted");
    call.result(0); return;
  }
  SDL_Renderer *host = SDL_CreateRenderer(window(window_handle),
      int(call.word(1)), call.word(2));
  if (!host) { call.result(0); return; }
  guest_renderers[slot] = {host, window_handle};
  call.result(0x700 + slot);
}
IMPORT(SDL_DestroyRenderer) {
  CALL; const uint32_t handle = call.word(0);
  if (!handle) return;
  SDL_Renderer *host = guest_renderer(handle);
  for (auto &entry : guest_textures)
    if (entry.host && entry.renderer == handle) entry = {};
  SDL_DestroyRenderer(host);
  guest_renderers[handle - 0x700] = {};
}
IMPORT(SDL_CreateTexture) {
  CALL; const uint32_t renderer_handle = call.word(0);
  const int width = int(call.word(3)), height = int(call.word(4));
  if (call.word(1) != SDL_PIXELFORMAT_RGBA32 || width <= 0 ||
      width > INT_MAX / 4 || height <= 0) {
    SDL_SetError("SDL presentation supports positive RGBA32 textures only");
    call.result(0); return;
  }
  unsigned slot = 0; while (slot < 64 && guest_textures[slot].host) ++slot;
  if (slot == 64) {
    SDL_SetError("Guest SDL texture handle table exhausted");
    call.result(0); return;
  }
  SDL_Texture *host = SDL_CreateTexture(guest_renderer(renderer_handle),
      SDL_PIXELFORMAT_RGBA32, int(call.word(2)), width, height);
  if (!host) { call.result(0); return; }
  guest_textures[slot] = {host, renderer_handle, width, height};
  call.result(0x800 + slot);
}
IMPORT(SDL_DestroyTexture) {
  CALL; const uint32_t handle = call.word(0);
  if (!handle) return;
  GuestTexture *entry = guest_texture(handle);
  SDL_DestroyTexture(entry->host);
  *entry = {};
}
IMPORT(SDL_UpdateTexture) {
  CALL; GuestTexture *entry = guest_texture(call.word(0));
  SDL_Rect rect = {}; SDL_Rect *rect_ptr = nullptr;
  if (call.word(1)) {
    if (!guest_rect(arena, call.word(1), rect) || rect.x < 0 || rect.y < 0 ||
        int64_t(rect.x) + rect.w > entry->width ||
        int64_t(rect.y) + rect.h > entry->height) {
      call.result(SDL_SetError("Invalid guest SDL texture update rectangle")); return;
    }
    rect_ptr = &rect;
  }
  const int rows = rect_ptr ? rect.h : entry->height;
  const int row_bytes = (rect_ptr ? rect.w : entry->width) * 4;
  const int pitch = int(call.word(3));
  if (!call.word(2) || pitch < row_bytes || rows <= 0) {
    call.result(SDL_SetError("Invalid guest SDL texture pixels or pitch")); return;
  }
  const uint64_t bytes = uint64_t(rows - 1) * uint32_t(pitch) + uint32_t(row_bytes);
  if (bytes > UINT32_MAX) {
    call.result(SDL_SetError("Guest SDL texture update is too large")); return;
  }
  call.result(SDL_UpdateTexture(entry->host, rect_ptr,
      guest(arena, call.word(2), size_t(bytes)), pitch));
}
IMPORT(SDL_RenderClear) {
  CALL; call.result(SDL_RenderClear(guest_renderer(call.word(0))));
}
IMPORT(SDL_RenderCopy) {
  CALL; const uint32_t renderer_handle = call.word(0);
  GuestTexture *entry = guest_texture(call.word(1));
  if (entry->renderer != renderer_handle) {
    call.result(SDL_SetError("SDL texture belongs to a different renderer")); return;
  }
  SDL_Rect source = {}, destination = {};
  if (!guest_rect(arena, call.word(2), source) ||
      !guest_rect(arena, call.word(3), destination)) {
    call.result(SDL_SetError("Invalid guest SDL render rectangle")); return;
  }
  call.result(SDL_RenderCopy(guest_renderer(renderer_handle), entry->host,
      call.word(2) ? &source : nullptr, call.word(3) ? &destination : nullptr));
}
IMPORT(SDL_RenderPresent) {
  CALL; SDL_RenderPresent(guest_renderer(call.word(0)));
}
