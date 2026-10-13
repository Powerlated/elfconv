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
IMPORT(iscntrl) { CALL; call.result(iscntrl(int32_t(call.word(0))) ? 0x0002 : 0); }
IMPORT(ispunct) { CALL; call.result(ispunct(int32_t(call.word(0))) ? 0x0004 : 0); }
IMPORT(isupper) { CALL; call.result(isupper(int32_t(call.word(0))) ? 0x0100 : 0); }
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
