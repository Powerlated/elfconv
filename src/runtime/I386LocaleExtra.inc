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
}

IMPORT(setlocale) {
  CALL;
  const int category = host_locale_category(int32_t(call.word(0)));
  const char *locale = call.word(1) ? text(arena, call.word(1)) : nullptr;
  call.result(copy_string(arena, category < 0 ? nullptr : setlocale(category, locale), 10));
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
