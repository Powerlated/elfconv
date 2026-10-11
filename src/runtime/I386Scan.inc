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
      result = suppressed ? swscanf(text + position, spec.c_str(), &consumed)
                          : swscanf(text + position, spec.c_str(), output, &consumed);
    }
    if (consumed >= 0) position += size_t(consumed);
    return result;
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
    if (strchr("diuoxX", conversion)) {
      append_width(width); spec += 'l'; spec += 'l'; spec += conversion;
      uint64_t value = 0;
      if (conversion == 'd' || conversion == 'i') {
        long long signed_value = 0;
        result = input.scan(spec, &signed_value, suppressed, used);
        value = uint64_t(signed_value);
      } else {
        unsigned long long unsigned_value = 0;
        result = input.scan(spec, &unsigned_value, suppressed, used);
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
        result = input.scan(spec, output, suppressed, used);
      }
    } else elfconv_runtime_error("Unsupported guest scanf conversion: %c.\n", conversion);
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
int scan_guest_file(uint8_t *arena, FILE *file, const char *fmt, Arguments arguments) {
  GuestScanInput<char> source{nullptr, file};
  return scan_guest_input(arena, source, fmt, arguments);
}
}  // namespace
