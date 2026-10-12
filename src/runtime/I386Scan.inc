// Included after the core guest-memory and varargs helpers.
namespace {
template<typename Char>
struct GuestScanInput {
  const Char *text = nullptr;
  FILE *file = nullptr;
  size_t position = 0;

  template<typename T>
  int scan(std::basic_string<Char> spec, T *output, bool suppressed, int &consumed) {
    spec += '%'; spec += 'n';
    consumed = -1;
    int result;
    if constexpr (std::is_same_v<Char, char>) {
      if (file)
        result = suppressed ? fscanf(file, spec.c_str(), &consumed)
                            : fscanf(file, spec.c_str(), output, &consumed);
      else
        result = suppressed ? sscanf(text + position, spec.c_str(), &consumed)
                            : sscanf(text + position, spec.c_str(), output, &consumed);
    } else {
      if (file)
        result = suppressed ? fwscanf(file, spec.c_str(), &consumed)
                            : fwscanf(file, spec.c_str(), output, &consumed);
      else
        result = suppressed ? swscanf(text + position, spec.c_str(), &consumed)
                            : swscanf(text + position, spec.c_str(), output, &consumed);
    }
    if (consumed >= 0) position += size_t(consumed);
    return result;
  }
  template<typename T>
  int scan_international(std::basic_string<Char> spec, T *output,
                         bool suppressed, int &consumed) {
    if constexpr (!std::is_same_v<Char, char>) {
      return scan(std::move(spec), output, suppressed, consumed);
    } else {
      if (file) return scan(std::move(spec), output, suppressed, consumed);
      const char *locale = setlocale(LC_NUMERIC, nullptr);
      if (!locale || strncmp(locale, "fa", 2) != 0 ||
          (locale[2] != '_' && locale[2] != '.' && locale[2] != '@' && locale[2] != '\0'))
        return scan(std::move(spec), output, suppressed, consumed);
      const char *source = text + position;
      std::string translated;
      for (size_t i = 0; source[i];) {
        if (uint8_t(source[i]) == 0xdb && uint8_t(source[i + 1]) >= 0xb0 &&
            uint8_t(source[i + 1]) <= 0xb9) {
          translated += char('0' + uint8_t(source[i + 1]) - 0xb0);
          i += 2;
        } else {
          translated += source[i++];
        }
      }
      GuestScanInput<char> normalized{translated.c_str()};
      int normalized_consumed = -1;
      const int result = normalized.scan(std::move(spec), output, suppressed, normalized_consumed);
      consumed = normalized_consumed;
      if (consumed >= 0) {
        size_t source_bytes = 0;
        int normalized_bytes = 0;
        while (normalized_bytes < consumed) {
          if (uint8_t(source[source_bytes]) == 0xdb &&
              uint8_t(source[source_bytes + 1]) >= 0xb0 &&
              uint8_t(source[source_bytes + 1]) <= 0xb9) {
            source_bytes += 2;
          } else {
            ++source_bytes;
          }
          ++normalized_bytes;
        }
        consumed = int(source_bytes);
        position += source_bytes;
      }
      return result;
    }
  }
  // Match glibc's signed-overflow saturation; %i also needs base autodetection.
  int scan_signed_integer(std::basic_string<Char> spec, uint32_t width,
                         size_t output_size, bool suppressed,
                         bool automatic_base, long long &value, int &consumed) {
    if constexpr (!std::is_same_v<Char, char>) {
      return scan(std::move(spec), &value, suppressed, consumed);
    } else {
      size_t offset = 0;
      int read_count = 0;
      auto get = [&]() {
        if (file) {
          const int c = fgetc(file);
          if (c != EOF) ++read_count;
          return c;
        }
        const unsigned char c = static_cast<unsigned char>(text[position + offset]);
        if (!c) return EOF;
        ++offset;
        ++read_count;
        return int(c);
      };
      auto unget = [&](int c) {
        if (c == EOF) return;
        if (file) ungetc(c, file);
        else --offset;
        --read_count;
      };
      int c = get();
      while (c != EOF && isspace(static_cast<unsigned char>(c))) c = get();
      if (c == EOF) { consumed = -1; return EOF; }
      bool negative = false;
      uint32_t item_size = 0;
      if (c == '+' || c == '-') {
        negative = c == '-';
        ++item_size;
        if (width && item_size == width) { consumed = -1; return 0; }
        c = get();
      }
      if (c < '0' || c > '9') {
        unget(c);
        consumed = -1;
        return 0;
      }
      const uint64_t positive_limit = uint64_t(LLONG_MAX);
      const uint64_t limit = negative ? positive_limit + 1 : positive_limit;
      unsigned base = automatic_base ? (c == '0' ? 8 : 10) : 10;
      uint64_t magnitude = uint64_t(c - '0');
      bool overflow = false;
      ++item_size;
      if (automatic_base && c == '0' && (!width || item_size < width)) {
        const int prefix = get();
        unsigned prefix_base = prefix == 'x' || prefix == 'X' ? 16
            : prefix == 'b' || prefix == 'B' ? 2 : 0;
        if (prefix_base) {
          ++item_size;
          base = prefix_base;
          if (width && item_size == width) { consumed = -1; return 0; }
          c = get();
          const int digit = c >= '0' && c <= '9' ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
          if (digit < 0 || unsigned(digit) >= base) {
            unget(c);
            consumed = -1;
            return 0;
          }
          magnitude = uint64_t(digit);
          ++item_size;
        } else {
          unget(prefix);
        }
      }
      while (!width || item_size < width) {
        c = get();
        const int digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0 || unsigned(digit) >= base) { unget(c); break; }
        if (magnitude > (limit - uint64_t(digit)) / base) overflow = true;
        else if (!overflow) magnitude = magnitude * base + uint64_t(digit);
        ++item_size;
      }
      if (overflow) { magnitude = limit; errno = ERANGE; }
      value = negative
          ? magnitude == uint64_t(LLONG_MAX) + 1 ? LLONG_MIN : -static_cast<long long>(magnitude)
          : static_cast<long long>(magnitude);
      consumed = read_count;
      position += size_t(consumed);
      return suppressed ? 0 : 1;
    }
  }
  int scan_characters(uint32_t width, char *output, bool suppressed, int &consumed) {
    const uint32_t required = width ? width : 1;
    uint32_t count = 0;
    while (count < required) {
      int c;
      if (file) c = fgetc(file);
      else {
        const unsigned char value = static_cast<unsigned char>(text[position + count]);
        c = value ? int(value) : EOF;
      }
      if (c == EOF) { consumed = -1; return EOF; }
      if (!suppressed) output[count] = char(c);
      ++count;
    }
    consumed = int(count);
    position += count;
    return suppressed ? 0 : 1;
  }
  int scan_binary(uint32_t width, bool suppressed, uint64_t &value, int &consumed) {
    size_t offset = 0;
    int read_count = 0;
    auto get = [&]() {
      if (file) {
        if constexpr (std::is_same_v<Char, char>) {
          const int c = fgetc(file);
          if (c != EOF) ++read_count;
          return c;
        } else {
          const wint_t c = fgetwc(file);
          if (c == WEOF) return EOF;
          ++read_count;
          return int(c);
        }
      }
      const Char c = text[position + offset];
      if (!c) return EOF;
      ++offset;
      ++read_count;
      if constexpr (std::is_same_v<Char, char>)
        return int(static_cast<unsigned char>(c));
      else
        return int(c);
    };
    auto unget = [&](int c) {
      if (c == EOF) return;
      if (file) {
        if constexpr (std::is_same_v<Char, char>)
          ungetc(c, file);
        else
          ungetwc(wchar_t(c), file);
      } else {
        --offset;
      }
      --read_count;
    };
    auto space = [](int c) {
      if constexpr (std::is_same_v<Char, char>)
        return c != EOF && isspace(static_cast<unsigned char>(c));
      else
        return c != EOF && iswspace(wint_t(c));
    };
    auto digit = [](int c) { return c == '0' || c == '1'; };
    int c;
    do { c = get(); } while (space(c));
    if (c == EOF) { consumed = -1; return EOF; }
    unget(c);

    uint32_t item_size = 0;
    bool negative = false;
    c = get();
    if (c == '+' || c == '-') {
      negative = c == '-';
      ++item_size;
      if (width && item_size == width) { consumed = -1; return 0; }
      c = get();
    }
    if (!digit(c)) {
      unget(c);
      consumed = -1;
      return 0;
    }

    uint64_t parsed = uint64_t(c - '0');
    bool overflow = false;
    ++item_size;
    if (c == '0' && (!width || item_size < width)) {
      const int prefix = get();
      if (prefix == 'b' || prefix == 'B') {
        ++item_size;
        if (width && item_size == width) {
          consumed = -1;
          return 0;
        }
        c = get();
        if (!digit(c)) {
          unget(c);
          consumed = -1;
          return 0;
        }
        parsed = uint64_t(c - '0');
        ++item_size;
      } else if (digit(prefix)) {
        parsed = uint64_t(prefix - '0');
        ++item_size;
      } else {
        unget(prefix);
      }
    }
    while (!width || item_size < width) {
      c = get();
      if (!digit(c)) { unget(c); break; }
      const uint64_t bit = uint64_t(c - '0');
      if (parsed > (UINT64_MAX - bit) / 2) {
        parsed = UINT64_MAX;
        overflow = true;
      } else if (!overflow) {
        parsed = parsed * 2 + bit;
      }
      ++item_size;
    }
    if (overflow) { parsed = UINT64_MAX; errno = ERANGE; }
    value = overflow ? UINT64_MAX : negative ? uint64_t(0) - parsed : parsed;
    consumed = read_count;
    position += size_t(consumed);
    return suppressed ? 0 : 1;
  }
};

template<typename Char>
int scan_guest_input(uint8_t *arena, GuestScanInput<Char> &input,
                     const Char *fmt, Arguments arguments) {
  int assigned = 0, conversions = 0;
  auto whitespace = [](Char c) {
    if constexpr (std::is_same_v<Char, char>) return !!isspace(static_cast<unsigned char>(c));
    else return !!iswspace(c);
  };
  for (const Char *p = fmt; *p;) {
    int used = -1, result;
    if (*p != '%' || p[1] == '%') {
      std::basic_string<Char> literal;
      if (whitespace(*p)) {
        while (whitespace(*p)) literal += *p++;
      } else {
        literal += *p++;
        if (literal[0] == '%') literal += *p++;
      }
      result = input.scan(literal, static_cast<int *>(nullptr), true, used);
      if (used < 0) return result == EOF && !conversions ? EOF : assigned;
      continue;
    }
    ++p;
    bool suppressed = *p == '*';
    if (suppressed) ++p;
    uint32_t width = 0;
    while (*p >= '0' && *p <= '9') {
      if (width > (UINT32_MAX - 9) / 10) elfconv_runtime_error("Guest scanf width overflow.\n");
      width = width * 10 + uint32_t(*p++ - '0');
    }
    const bool international_digits = *p == 'I';
    if (international_digits) ++p;
    char length = 0;
    if (*p > 0 && *p < 128 && strchr("hljztL", int(*p))) {
      length = char(*p++);
      if (*p == length && (length == 'h' || length == 'l')) {
        ++p; length = length == 'l' ? 'q' : 'H';
      }
    }
    const char conversion = char(*p++);
    if (!conversion) elfconv_runtime_error("Incomplete guest scanf format.\n");
    const uint32_t address = suppressed ? 0 : arguments.word();
    const size_t integer_size = length == 'q' || length == 'j' ? 8 : length == 'h' ? 2 : length == 'H' ? 1 : 4;
    if (conversion == 'n') {
      const uint64_t count = input.position;
      if (!suppressed) memcpy(guest(arena, address, integer_size), &count, integer_size);
      continue;
    }
    std::basic_string<Char> spec(1, '%');
    if (suppressed) spec += '*';
    auto append_width = [&](uint32_t value) {
      if (value) { const auto digits = std::to_string(value); spec.append(digits.begin(), digits.end()); }
    };
    if (conversion == 'b') {
      uint64_t value = 0;
      result = input.scan_binary(width, suppressed, value, used);
      if (used >= 0 && !suppressed) memcpy(guest(arena, address, integer_size), &value, integer_size);
    } else if (strchr("diuoxX", conversion)) {
      append_width(width); spec += 'l'; spec += 'l'; spec += conversion;
      uint64_t value = 0;
      if (conversion == 'd' || conversion == 'i') {
        long long signed_value = 0;
        if (!international_digits && (conversion == 'd' || conversion == 'i'))
          result = input.scan_signed_integer(spec, width, integer_size, suppressed,
                                             conversion == 'i', signed_value, used);
        else
          result = international_digits
              ? input.scan_international(spec, &signed_value, suppressed, used)
              : input.scan(spec, &signed_value, suppressed, used);
        value = uint64_t(signed_value);
      } else {
        unsigned long long unsigned_value = 0;
        result = international_digits
            ? input.scan_international(spec, &unsigned_value, suppressed, used)
            : input.scan(spec, &unsigned_value, suppressed, used);
        value = unsigned_value;
      }
      if (used >= 0 && !suppressed) memcpy(guest(arena, address, integer_size), &value, integer_size);
    } else if (conversion == 'p') {
      append_width(width); spec += 'p';
      void *pointer = nullptr;
      result = input.scan(spec, &pointer, suppressed, used);
      const uint32_t value = uint32_t(reinterpret_cast<uintptr_t>(pointer));
      if (used >= 0 && !suppressed) memcpy(guest(arena, address, 4), &value, 4);
    } else if (strchr("aAeEfFgG", conversion)) {
      append_width(width); spec += 'L'; spec += conversion;
      long double value = 0;
      result = input.scan(spec, &value, suppressed, used);
      if (used >= 0 && !suppressed) {
        if (length == 'L') {
          const float80_t extended{double(value)};
          auto *destination = static_cast<uint8_t *>(guest(arena, address, 12));
          memcpy(destination, extended.data, 10); memset(destination + 10, 0, 2);
        } else if (length == 'l') {
          const double number = double(value); memcpy(guest(arena, address, 8), &number, 8);
        } else {
          const float number = float(value); memcpy(guest(arena, address, 4), &number, 4);
        }
      }
    } else if (conversion == 's' || conversion == 'c' || conversion == '[') {
      const bool wide_output = length == 'l';
      const uint32_t terminator = conversion != 'c';
      if (!suppressed) {
        guest(arena, address, 1);
        uint32_t available = uint32_t(MEMORY_ARENA_SIZE - address) / (wide_output ? 4 : 1);
        if constexpr (std::is_same_v<Char, wchar_t>)
          if (!wide_output) available /= std::max(size_t(1), size_t(MB_CUR_MAX));
        if (available <= terminator) elfconv_runtime_error("Guest scanf output overflow.\n");
        available -= terminator;
        width = width ? std::min(width, available) : conversion == 'c' ? 1 : available;
      }
      append_width(width);
      if (wide_output) spec += 'l';
      spec += conversion;
      if (conversion == '[') {
        if (*p == '^') spec += *p++;
        if (*p == ']') spec += *p++;
        while (*p && *p != ']') spec += *p++;
        if (*p != ']') elfconv_runtime_error("Invalid guest scanf scanset.\n");
        spec += *p++;
      }
      if (wide_output) {
        auto *output = suppressed ? nullptr : wide_buffer(arena, address, uint64_t(width) + terminator);
        result = input.scan(spec, output, suppressed, used);
      } else {
        const uint64_t bytes = (uint64_t(width) + terminator) * (std::is_same_v<Char, wchar_t> ? std::max(size_t(1), size_t(MB_CUR_MAX)) : 1);
        auto *output = suppressed ? nullptr : static_cast<char *>(guest(arena, address, bytes));
        if constexpr (std::is_same_v<Char, char>) {
          if (conversion == 'c')
            result = input.scan_characters(width, output, suppressed, used);
          else
            result = input.scan(spec, output, suppressed, used);
        } else {
          result = input.scan(spec, output, suppressed, used);
        }
      }
    } else { result = 0; used = -1; }
    if (used < 0) return result == EOF && !conversions ? EOF : assigned;
    ++conversions;
    if (!suppressed) ++assigned;
  }
  return assigned;
}
int scan_guest_string(uint8_t *arena, const char *input, const char *fmt, Arguments arguments) {
  GuestScanInput<char> source{input};
  return scan_guest_input(arena, source, fmt, arguments);
}
int scan_guest_wide_string(uint8_t *arena, const wchar_t *input, const wchar_t *fmt, Arguments arguments) {
  GuestScanInput<wchar_t> source{input};
  return scan_guest_input(arena, source, fmt, arguments);
}
int scan_guest_wide_file(uint8_t *arena, FILE *file, const wchar_t *fmt, Arguments arguments) {
  GuestScanInput<wchar_t> source{nullptr, file};
  return scan_guest_input(arena, source, fmt, arguments);
}
int scan_guest_file(uint8_t *arena, FILE *file, const char *fmt, Arguments arguments) {
  GuestScanInput<char> source{nullptr, file};
  return scan_guest_input(arena, source, fmt, arguments);
}
}  // namespace
