// Additional libc imports; included after I386Imports.cpp adapter helpers.
#include "I386Scan.inc"
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
IMPORT(abort) { abort(); }
IMPORT(fscanf) {
  CALL;
  call.result(scan_guest_file(arena, stream(call.word(0)), text(arena, call.word(1)),
                             {arena, call.stack + 12}));
}
IMPORT(sscanf) {
  CALL;
  call.result(scan_guest_string(arena, text(arena, call.word(0)), text(arena, call.word(1)),
                               {arena, call.stack + 12}));
}
IMPORT(getwc) { CALL; call.result(uint32_t(getwc(stream(call.word(0))))); }
IMPORT(isalpha) { CALL; call.result(isalpha(int32_t(call.word(0))) ? 0x0400 : 0); }
IMPORT(iscntrl) { CALL; call.result(iscntrl(int32_t(call.word(0))) ? 0x0002 : 0); }
IMPORT(ispunct) { CALL; call.result(ispunct(int32_t(call.word(0))) ? 0x0004 : 0); }
IMPORT(isupper) { CALL; call.result(isupper(int32_t(call.word(0))) ? 0x0100 : 0); }
IMPORT(putwc) { CALL; call.result(uint32_t(putwc(wchar_t(call.word(0)), stream(call.word(1))))); }
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
IMPORT(ungetwc) { CALL; call.result(uint32_t(ungetwc(wint_t(call.word(0)), stream(call.word(1))))); }
IMPORT(vfprintf) {
  CALL; auto out = format(arena, text(arena, call.word(1)), {arena, call.word(2)});
  FILE *output = stream(call.word(0));
  call.result(fwrite(out.data(), 1, out.size(), output) == out.size() ? int(out.size()) : -1);
}
