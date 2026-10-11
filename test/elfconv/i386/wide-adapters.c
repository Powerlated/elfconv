#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

int main(void) {
  wchar_t data[8] = {L'A', 0x1f600, L'Z', 0};
  if (wcslen(data) != 3 || wcscmp(data, L"A") <= 0 || wcscmp(L"B", data) <= 0) return 1;
  if (wmemchr(data, 0x1f600, 3) != data + 1 || wmemchr(data, L'Q', 3)) return 2;
  if (wmemmove(data + 1, data, 3) != data + 1 ||
      data[0] != L'A' || data[1] != L'A' || data[2] != 0x1f600 || data[3] != L'Z') return 3;
  wchar_t copy[4];
  if (wmemcpy(copy, data, 4) != copy || wmemcmp(copy, data, 4)) return 4;
  if (wmemset(copy + 1, 0x1234, 2) != copy + 1 ||
      copy[0] != L'A' || copy[1] != 0x1234 || copy[2] != 0x1234 || copy[3] != L'Z') return 5;
  if (wmemcmp(copy, data, 4) <= 0 || wmemchr(0, L'A', 0) || wmemcmp(0, 0, 0)) return 6;
  if (towlower(L'Z') != L'z' || towupper(L'a') != L'A' || !iswspace(L'\t') || iswspace(L'A')) return 7;
  wchar_t padded[5] = {1, 1, 1, 1, 0x1234};
  if (wcsncpy(padded, L"A", 4) != padded || padded[0] != L'A' ||
      padded[1] || padded[2] || padded[3] || padded[4] != 0x1234) return 8;
  const wchar_t unterminated[2] = {L'X', 0x1f600};
  if (wcsncpy(padded, unterminated, 2) != padded || padded[0] != L'X' ||
      padded[1] != 0x1f600 || padded[2]) return 9;
  if (wcsncpy(padded, unterminated, 0) != padded || padded[0] != L'X') return 10;
  wchar_t joined[6] = {L'A', 0, 1, 1, 1, 0x1234};
  wchar_t *(*volatile concatenate)(wchar_t *, const wchar_t *, size_t) = wcsncat;
  if (concatenate(joined, unterminated, 2) != joined ||
      joined[0] != L'A' || joined[1] != L'X' || joined[2] != 0x1f600 ||
      joined[3] || joined[4] != 1 || joined[5] != 0x1234) return 11;
  if (concatenate(joined, L"", 1) != joined || wcslen(joined) != 3) return 12;
  if (concatenate(joined, unterminated, 0) != joined || wcslen(joined) != 3) return 13;
  puts("wide-character widths, overlap, pointers and ordering passed");
  return 0;
}
