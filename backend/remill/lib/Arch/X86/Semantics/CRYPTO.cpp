// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace {
// AES substitution permutation from FIPS 197, section 5.1.1.
constexpr uint8_t aes_substitution[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
struct AESInverseTable { uint8_t bytes[256]; };
constexpr AESInverseTable MakeAESInverseTable() {
  AESInverseTable result = {};
  for (unsigned i = 0; i < 256; ++i) result.bytes[aes_substitution[i]] = uint8_t(i);
  return result;
}
constexpr auto aes_inverse_substitution = MakeAESInverseTable();
ALWAYS_INLINE uint8_t AESDouble(uint8_t value) {
  return uint8_t((value << 1) ^ ((value & 0x80) ? 0x1b : 0));
}
ALWAYS_INLINE void AESMixColumns(uint8v16_t &value, bool inverse) {
  _Pragma("unroll") for (unsigned column = 0; column < 16; column += 4) {
    uint8_t a = UExtractV8(value, column), b = UExtractV8(value, column + 1);
    uint8_t c = UExtractV8(value, column + 2), d = UExtractV8(value, column + 3);
    if (inverse) {
      const uint8_t ac = AESDouble(AESDouble(a ^ c));
      const uint8_t bd = AESDouble(AESDouble(b ^ d));
      a ^= ac; c ^= ac; b ^= bd; d ^= bd;
    }
    const uint8_t total = a ^ b ^ c ^ d;
    value = UInsertV8(value, column, uint8_t(a ^ total ^ AESDouble(a ^ b)));
    value = UInsertV8(value, column + 1, uint8_t(b ^ total ^ AESDouble(b ^ c)));
    value = UInsertV8(value, column + 2, uint8_t(c ^ total ^ AESDouble(c ^ d)));
    value = UInsertV8(value, column + 3, uint8_t(d ^ total ^ AESDouble(d ^ a)));
  }
}
template <bool inverse, bool last, typename D, typename S1, typename S2>
DEF_SEM(AESRound, D dst, S1 source, S2 round_key) {
  const auto input = UReadV8(source);
  const auto key = UReadV8(round_key);
  uint8v16_t result = {};
  _Pragma("unroll") for (unsigned column = 0; column < 4; ++column) {
    _Pragma("unroll") for (unsigned row = 0; row < 4; ++row) {
      const unsigned input_column = (column + (inverse ? 4 - row : row)) & 3;
      const uint8_t byte = UExtractV8(input, 4 * input_column + row);
      result = UInsertV8(result, 4 * column + row,
                        inverse ? aes_inverse_substitution.bytes[byte] : aes_substitution[byte]);
    }
  }
  if (!last) AESMixColumns(result, inverse);
  _Pragma("unroll") for (unsigned i = 0; i < 16; ++i)
    result = UInsertV8(result, i, uint8_t(UExtractV8(result, i) ^ UExtractV8(key, i)));
  UWriteV8(dst, result);
}
template <typename D, typename S>
DEF_SEM(AESInverseMix, D dst, S source) {
  auto value = UReadV8(source);
  AESMixColumns(value, true);
  UWriteV8(dst, value);
}
template <typename D, typename S>
DEF_SEM(AESKeyAssist, D dst, S source, I8 immediate) {
  const auto input = UReadV8(source);
  uint8v16_t result = {};
  _Pragma("unroll") for (unsigned half = 0; half < 2; ++half) {
    _Pragma("unroll") for (unsigned byte = 0; byte < 4; ++byte) {
      result = UInsertV8(result, half * 8 + byte,
                        aes_substitution[UExtractV8(input, half * 8 + 4 + byte)]);
      uint8_t rotated = aes_substitution[UExtractV8(input, half * 8 + 4 + ((byte + 1) & 3))];
      if (!byte) rotated ^= Read(immediate);
      result = UInsertV8(result, half * 8 + 4 + byte, rotated);
    }
  }
  UWriteV8(dst, result);
}
template <typename D, typename S1, typename S2>
DEF_SEM(CarrylessMultiply, D dst, S1 source, S2 multiplier, I8 immediate) {
  const uint8_t control = Read(immediate);
  const uint64_t a = UExtractV64(UReadV64(source), control & 1);
  uint64_t b = UExtractV64(UReadV64(multiplier), (control >> 4) & 1);
  uint64_t low = 0, high = 0;
  for (unsigned bit = 0; b; ++bit, b >>= 1) {
    if (!(b & 1)) continue;
    low ^= a << bit;
    if (bit) high ^= a >> (64 - bit);
  }
  uint64v2_t result = {};
  result = UInsertV64(result, 0, low);
  result = UInsertV64(result, 1, high);
  UWriteV64(dst, result);
}
}  // namespace

DEF_ISEL(AESENC_XMMdq_XMMdq) = AESRound<false, false, V128W, V128, V128>;
DEF_ISEL(AESENC_XMMdq_MEMdq) = AESRound<false, false, V128W, V128, MV128>;
DEF_ISEL(AESENCLAST_XMMdq_XMMdq) = AESRound<false, true, V128W, V128, V128>;
DEF_ISEL(AESENCLAST_XMMdq_MEMdq) = AESRound<false, true, V128W, V128, MV128>;
DEF_ISEL(AESDEC_XMMdq_XMMdq) = AESRound<true, false, V128W, V128, V128>;
DEF_ISEL(AESDEC_XMMdq_MEMdq) = AESRound<true, false, V128W, V128, MV128>;
DEF_ISEL(AESDECLAST_XMMdq_XMMdq) = AESRound<true, true, V128W, V128, V128>;
DEF_ISEL(AESDECLAST_XMMdq_MEMdq) = AESRound<true, true, V128W, V128, MV128>;
DEF_ISEL(AESIMC_XMMdq_XMMdq) = AESInverseMix<V128W, V128>;
DEF_ISEL(AESIMC_XMMdq_MEMdq) = AESInverseMix<V128W, MV128>;
DEF_ISEL(AESKEYGENASSIST_XMMdq_XMMdq_IMMb) = AESKeyAssist<V128W, V128>;
DEF_ISEL(AESKEYGENASSIST_XMMdq_MEMdq_IMMb) = AESKeyAssist<V128W, MV128>;
DEF_ISEL(PCLMULQDQ_XMMdq_XMMdq_IMMb) = CarrylessMultiply<V128W, V128, V128>;
DEF_ISEL(PCLMULQDQ_XMMdq_MEMdq_IMMb) = CarrylessMultiply<V128W, V128, MV128>;
