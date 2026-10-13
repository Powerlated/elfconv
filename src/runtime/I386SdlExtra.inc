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
IMPORT(SDL_GL_GetProcAddress) {
  CALL; const char *name = text(arena, call.word(0));
  struct Entry { const char *name; LiftedFunc adapter; };
  static const Entry entries[] = {
    {"glGetString", __ecv_i386_glGetString}, {"glGetError", __ecv_i386_glGetError},
    {"glClear", __ecv_i386_glClear}, {"glViewport", __ecv_i386_glViewport},
    {"glPixelStorei", __ecv_i386_glPixelStorei}, {"glClearColor", __ecv_i386_glClearColor},
    {"glReadPixels", __ecv_i386_glReadPixels}, {"glCreateShader", __ecv_i386_glCreateShader},
    {"glCompileShader", __ecv_i386_glCompileShader}, {"glDeleteShader", __ecv_i386_glDeleteShader},
    {"glCreateProgram", __ecv_i386_glCreateProgram}, {"glAttachShader", __ecv_i386_glAttachShader},
    {"glBindAttribLocation", __ecv_i386_glBindAttribLocation}, {"glLinkProgram", __ecv_i386_glLinkProgram},
    {"glUseProgram", __ecv_i386_glUseProgram}, {"glDeleteProgram", __ecv_i386_glDeleteProgram},
    {"glGetUniformLocation", __ecv_i386_glGetUniformLocation}, {"glEnable", __ecv_i386_glEnable},
    {"glEnableVertexAttribArray", __ecv_i386_glEnableVertexAttribArray}, {"glBindBuffer", __ecv_i386_glBindBuffer},
    {"glShaderSource", __ecv_i386_glShaderSource}, {"glGetShaderiv", __ecv_i386_glGetShaderiv},
    {"glGetProgramiv", __ecv_i386_glGetProgramiv}, {"glGetShaderInfoLog", __ecv_i386_glGetShaderInfoLog},
    {"glGetProgramInfoLog", __ecv_i386_glGetProgramInfoLog}, {"glGenBuffers", __ecv_i386_glGenBuffers},
    {"glDeleteBuffers", __ecv_i386_glDeleteBuffers}, {"glBufferData", __ecv_i386_glBufferData},
    {"glVertexAttribPointer", __ecv_i386_glVertexAttribPointer}, {"glUniformMatrix4fv", __ecv_i386_glUniformMatrix4fv},
    {"glDrawElements", __ecv_i386_glDrawElements}, {"glDrawArrays", __ecv_i386_glDrawArrays},
    {"glGenTextures", __ecv_i386_glGenTextures}, {"glBindTexture", __ecv_i386_glBindTexture},
    {"glTexImage2D", __ecv_i386_glTexImage2D}, {"glTexParameteri", __ecv_i386_glTexParameteri},
    {"glDepthFunc", __ecv_i386_glDepthFunc}, {"glDepthMask", __ecv_i386_glDepthMask},
    {"glBlendFunc", __ecv_i386_glBlendFunc}, {"glScissor", __ecv_i386_glScissor},
    {"glPolygonOffset", __ecv_i386_glPolygonOffset}, {"glUniform1f", __ecv_i386_glUniform1f},
    {"glUniform1i", __ecv_i386_glUniform1i}, {"glUniform2f", __ecv_i386_glUniform2f},
    {"glGetAttribLocation", __ecv_i386_glGetAttribLocation},
    {"glDisableVertexAttribArray", __ecv_i386_glDisableVertexAttribArray},
    {"glDisable", __ecv_i386_glDisable}, {"glActiveTexture", __ecv_i386_glActiveTexture},
#if ECV_LEGACY_GL
    {"glBegin", __ecv_i386_glBegin}, {"glEnd", __ecv_i386_glEnd},
    {"glMatrixMode", __ecv_i386_glMatrixMode}, {"glLoadIdentity", __ecv_i386_glLoadIdentity},
    {"glVertex2f", __ecv_i386_glVertex2f}, {"glColor3f", __ecv_i386_glColor3f},
#endif
  };
  for (const auto &e : entries) if (!strcmp(name, e.name)) { call.result(register_guest_adapter(arena, runtime, e.adapter)); return; }
  SDL_SetError("Unsupported GL function: %s", name); call.result(0);
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
IMPORT(SDL_WaitEventTimeout) { CALL; SDL_Event e; int found = SDL_WaitEventTimeout(call.word(0) ? &e : nullptr, int(call.word(1))); if (found && call.word(0) && !write_event(arena, call.word(0), e)) found = 0; call.result(found); }
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
