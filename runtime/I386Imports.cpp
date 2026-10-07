#include "Runtime.h"
#include "utils/elfconv.h"

#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengl.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
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
uint32_t copy_string(uint8_t *arena, const char *value, unsigned slot) {
  if (!value) return 0;
  size_t length = strlen(value) + 1;
  if (length > string_capacity) elfconv_runtime_error("Host-import string is too long.\n");
  uint32_t address = string_base + slot * string_capacity;
  memcpy(guest(arena, address, length), value, length);
  return address;
}
struct Call {
  uint8_t *arena;
  State *state;
  uint32_t stack;
  Call(uint8_t *a, State *s) : arena(a), state(s), stack(s->gpr.rsp.dword) {}
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
  void result(uint32_t value) { state->gpr.rax.dword = value; }
  ~Call() {
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

FILE *stream(uint32_t handle) {
  if (handle == 1) return stdout;
  if (handle == 2) return stderr;
  elfconv_runtime_error("Unsupported guest FILE handle.\n");
}

int print(Call &call, FILE *stream, unsigned format_arg) {
  const char *format = text(call.arena, call.word(format_arg));
  unsigned argument = format_arg + 1;
  int total = 0;
  for (const char *p = format; *p;) {
    if (*p != '%') {
      const char *start = p;
      while (*p && *p != '%') ++p;
      size_t length = p - start;
      if (fwrite(start, 1, length, stream) != length) return -1;
      total += length;
      continue;
    }
    const char *start = p++;
    if (*p == '%') { if (fputc('%', stream) == EOF) return -1; ++p; ++total; continue; }
    while (*p && strchr("-+ #0.0123456789", *p)) ++p;
    char conversion = *p;
    if (!conversion || !strchr("sducxX", conversion) || p - start >= 30)
      elfconv_runtime_error("Unsupported i386 printf conversion near %s.\n", start);
    char spec[32];
    size_t size = ++p - start;
    memcpy(spec, start, size);
    spec[size] = 0;
    uint32_t value = call.word(argument++);
    int written;
    if (conversion == 's') written = fprintf(stream, spec, text(call.arena, value));
    else if (conversion == 'd' || conversion == 'c') written = fprintf(stream, spec, int32_t(value));
    else written = fprintf(stream, spec, value);
    if (written < 0 || written > INT_MAX - total) return -1;
    total += written;
  }
  return total;
}
}  // namespace

#define IMPORT(name) extern "C" void __ecv_i386_##name(uint8_t *arena, State *state, uint32_t, RuntimeManager *runtime)
#define CALL Call call(arena, state)

IMPORT(__libc_start_main) {
  CALL;
  uint32_t main = call.word(0), argc = call.word(1), argv = call.word(2);
  uint32_t args[] = {argc, argv, argv + 4 * (argc + 1)};
  if (call.word(3)) invoke(arena, state, runtime, call.word(3), args, 3);
  else for (auto *p = _ecv_i386_initializers; *p; ++p) invoke(arena, state, runtime, *p, args, 3);
  invoke(arena, state, runtime, main, args, 3);
  int status = state->gpr.rax.dword;
  for (auto *p = _ecv_i386_finalizers; *p; ++p) invoke(arena, state, runtime, *p, nullptr, 0);
  if (call.word(4)) invoke(arena, state, runtime, call.word(4), nullptr, 0);
  exit(status);
}
IMPORT(strcmp) { CALL; call.result(strcmp(text(arena, call.word(0)), text(arena, call.word(1)))); }
IMPORT(strtoul) {
  CALL;
  const char *input = text(arena, call.word(0));
  char *end;
  errno = 0;
  unsigned long value = strtoul(input, &end, call.word(2));
  if (value > UINT32_MAX) { value = UINT32_MAX; errno = ERANGE; }
  if (call.word(1)) {
    uint32_t address = call.word(0) + (end - input);
    memcpy(guest(arena, call.word(1), 4), &address, 4);
  }
  call.result(value);
}
IMPORT(printf) { CALL; call.result(print(call, stdout, 0)); }
IMPORT(fprintf) {
  CALL;
  call.result(print(call, stream(call.word(0)), 1));
}
IMPORT(puts) { CALL; call.result(puts(text(arena, call.word(0)))); }
IMPORT(fwrite) {
  CALL;
  uint32_t size = call.word(1), count = call.word(2);
  if (!size || !count) { call.result(0); return; }
  uint64_t length = uint64_t(size) * count;
  if (length > MEMORY_ARENA_SIZE) elfconv_runtime_error("Guest fwrite buffer is too large.\n");
  call.result(fwrite(guest(arena, call.word(0), length), size, count, stream(call.word(3))));
}
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
  contexts[slot] = SDL_GL_CreateContext(window(call.word(0)));
  call.result(contexts[slot] ? 0x200 + slot : 0);
}
IMPORT(SDL_GL_DeleteContext) {
  CALL;
  if (call.word(0)) { SDL_GL_DeleteContext(context(call.word(0))); contexts[call.word(0) - 0x200] = nullptr; }
}
IMPORT(SDL_GL_SetSwapInterval) { CALL; call.result(SDL_GL_SetSwapInterval(call.word(0))); }
IMPORT(SDL_GL_GetDrawableSize) {
  CALL;
  int width, height;
  SDL_GL_GetDrawableSize(window(call.word(0)), &width, &height);
  if (call.word(1)) memcpy(guest(arena, call.word(1), 4), &width, 4);
  if (call.word(2)) memcpy(guest(arena, call.word(2), 4), &height, 4);
}
IMPORT(SDL_PollEvent) {
  CALL;
  SDL_Event event;
  int found = SDL_PollEvent(call.word(0) ? &event : nullptr);
  if (found && call.word(0)) {
    if (event.type == SDL_SYSWMEVENT || event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT ||
        event.type == SDL_TEXTEDITING_EXT || event.type >= SDL_USEREVENT)
      elfconv_runtime_error("Unsupported pointer-bearing guest SDL event %u.\n", event.type);
    static_assert(sizeof(SDL_Event) == 56, "SDL2 guest event layout requires 56 bytes");
    memcpy(guest(arena, call.word(0), 56), &event, 56);
  }
  call.result(found);
}
IMPORT(SDL_GL_SwapWindow) {
  CALL;
  SDL_GL_SwapWindow(window(call.word(0)));
#ifdef __EMSCRIPTEN__
  // Yield lifted synchronous guest code so the browser can present and dispatch input.
  emscripten_sleep(16);
#endif
}
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
