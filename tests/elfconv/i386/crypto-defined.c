#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t Block[16];
#define ROUND_REGISTER(op) __asm__ volatile("movdqu %0, %%xmm0; movdqu %1, %%xmm1; " op " %%xmm1, %%xmm0; movdqu %%xmm0, %0" : "+m"(*(Block *)value) : "m"(*(const Block *)key) : "xmm0", "xmm1")
#define ROUND_MEMORY(op) __asm__ volatile("movdqu %0, %%xmm0; " op " %1, %%xmm0; movdqu %%xmm0, %0" : "+m"(*(Block *)value) : "m"(*(const Block *)key) : "xmm0")
__attribute__((target("aes,sse2")))
static void round_aes(uint8_t *value, const uint8_t *key, int inverse, int last, int memory) {
  if (memory) {
    if (inverse) { if (last) { ROUND_MEMORY("aesdeclast"); } else { ROUND_MEMORY("aesdec"); } }
    else { if (last) { ROUND_MEMORY("aesenclast"); } else { ROUND_MEMORY("aesenc"); } }
  } else {
    if (inverse) { if (last) { ROUND_REGISTER("aesdeclast"); } else { ROUND_REGISTER("aesdec"); } }
    else { if (last) { ROUND_REGISTER("aesenclast"); } else { ROUND_REGISTER("aesenc"); } }
  }
}
__attribute__((target("aes,sse2")))
static void inverse_key(uint8_t *out, const uint8_t *key, int memory) {
  if (memory) __asm__ volatile("aesimc %1, %%xmm0; movdqu %%xmm0, %0" : "=m"(*(Block *)out) : "m"(*(const Block *)key) : "xmm0");
  else __asm__ volatile("movdqu %1, %%xmm1; aesimc %%xmm1, %%xmm0; movdqu %%xmm0, %0" : "=m"(*(Block *)out) : "m"(*(const Block *)key) : "xmm0", "xmm1");
}
__attribute__((target("aes,sse2")))
static void assist_key(uint8_t *out, const uint8_t *key, int memory) {
  if (memory) __asm__ volatile("aeskeygenassist $1, %1, %%xmm0; movdqu %%xmm0, %0" : "=m"(*(Block *)out) : "m"(*(const Block *)key) : "xmm0");
  else __asm__ volatile("movdqu %1, %%xmm1; aeskeygenassist $1, %%xmm1, %%xmm0; movdqu %%xmm0, %0" : "=m"(*(Block *)out) : "m"(*(const Block *)key) : "xmm0", "xmm1");
}
static unsigned nibble(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
static void decode(Block out, const char *hex) {
  for (unsigned i = 0; i < 16; ++i) out[i] = (nibble(hex[2*i]) << 4) | nibble(hex[2*i+1]);
}
static void xor_key(Block value, const Block key) {
  for (unsigned i = 0; i < 16; ++i) value[i] ^= key[i];
}
__attribute__((target("pclmul,sse2")))
static void polynomial(void) {
  const uint64_t source[2] __attribute__((aligned(16))) = {0x8000000000000003ULL, 0xfedcba9876543210ULL};
  const uint64_t factor[2] __attribute__((aligned(16))) = {0x0123456789abcdefULL, 0x8000000000000001ULL};
  uint32_t output[4];
#define PRODUCT(control) do { \
  __asm__ volatile("movdqu %1, %%xmm0; movdqu %2, %%xmm1; pclmulqdq $" control ", %%xmm1, %%xmm0; movdqu %%xmm0, %0" : "=m"(output) : "m"(source), "m"(factor) : "xmm0", "xmm1"); \
  printf("polynomial " control "=%08x%08x%08x%08x\n", output[3], output[2], output[1], output[0]); \
} while (0)
  PRODUCT("0x00"); PRODUCT("0x01"); PRODUCT("0x10"); PRODUCT("0x11"); PRODUCT("0xee");
#undef PRODUCT
}
int main(void) {
  // FIPS 197 AES-128 example: published round keys and ciphertext.
  const char *round_keys[] = {
    "000102030405060708090a0b0c0d0e0f", "d6aa74fdd2af72fadaa678f1d6ab76fe",
    "b692cf0b643dbdf1be9bc5006830b3fe", "b6ff744ed2c2c9bf6c590cbf0469bf41",
    "47f7f7bc95353e03f96c32bcfd058dfd", "3caaa3e8a99f9deb50f3af57adf622aa",
    "5e390f7df7a69296a7553dc10aa31f6b", "14f9701ae35fe28c440adf4d4ea9c026",
    "47438735a41c65b9e016baf4aebf7ad2", "549932d1f08557681093ed9cbe2c974e",
    "13111d7fe3944a17f307a78b4d2b30c5"};
  Block keys[11] __attribute__((aligned(16))), plaintext, ciphertext, value;
  Block transformed __attribute__((aligned(16))), expected_assist;
  for (unsigned i = 0; i < 11; ++i) decode(keys[i], round_keys[i]);
  decode(plaintext, "00112233445566778899aabbccddeeff");
  decode(ciphertext, "69c4e0d86a7b0430d8cdb78070b4c55a");
  decode(expected_assist, "f26b6fc56a6fc5f2fed7ab76d6ab76fe");
  for (int memory = 0; memory < 2; ++memory) {
    memcpy(value, plaintext, 16); xor_key(value, keys[0]);
    for (unsigned i = 1; i <= 10; ++i) round_aes(value, keys[i], 0, i == 10, memory);
    if (memcmp(value, ciphertext, 16)) return 1;
    xor_key(value, keys[10]);
    for (int i = 9; i > 0; --i) {
      inverse_key(transformed, keys[i], memory);
      round_aes(value, transformed, 1, 0, memory);
    }
    round_aes(value, keys[0], 1, 1, memory);
    if (memcmp(value, plaintext, 16)) return 2;
    assist_key(transformed, keys[0], memory);
    if (memcmp(transformed, expected_assist, 16)) return 3;
  }
  polynomial();
  puts("AES encryption, decryption and key assistance passed");
  return 0;
}
