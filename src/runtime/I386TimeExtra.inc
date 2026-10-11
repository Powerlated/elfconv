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
  struct timeval values[2];
  struct timeval *pointer = nullptr;
  if (call.word(1)) {
    int32_t fields[4];
    memcpy(fields, guest(arena, call.word(1), sizeof(fields)), sizeof(fields));
    values[0] = {fields[0], fields[1]}; values[1] = {fields[2], fields[3]}; pointer = values;
  }
  call.result(futimes(int32_t(call.word(0)), pointer));
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
