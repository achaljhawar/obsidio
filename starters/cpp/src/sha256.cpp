#include "sha256.hpp"

#include <cstdint>
#include <cstring>

#if defined(OBSIDIO_USE_OPENSSL)
// OpenSSL dispatches to SHA-NI on x86-64 and the ARMv8 crypto extensions on
// aarch64 at runtime. This is the same hardware path Go's crypto/sha256 uses,
// so it is the fair baseline to measure against -- the portable path below is
// several times slower and would flatter any optimisation you make later.
//
// SHA256_Transform is deprecated in OpenSSL 3.0, and the suppression below is
// deliberate: it is the only supported way to reach the hardware block function
// without going back through the EVP layer. The one-shot SHA256() re-fetches
// its provider on EVERY call, which for a 64-byte input costs several times the
// compression itself -- and the risk chain makes 49,999 of those calls per
// request. Measured on aarch64, 50,000 rounds: SHA256() 16.78 ms,
// Init+Update+Final 3.27 ms, Init+Transform x2 2.92 ms.
//
// The Dockerfile pins libssl3=3.0.20-1~deb12u2, so this API is present and
// stable for the build we ship. Revisit if that pin ever moves to OpenSSL 4.
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>
#endif

namespace obsidio {
namespace {

// The second SHA-256 block of ANY 64-byte message is this exact constant: the
// 0x80 terminator, zero padding, then a big-endian 64-bit length of 512 bits.
// Every round of the risk chain after the first hashes exactly 64 hex chars, so
// every one of them ends with this block.
constexpr std::uint8_t kPad64[64] = {
    0x80, 0, 0, 0, 0, 0, 0,    0,
    0,    0, 0, 0, 0, 0, 0,    0,
    0,    0, 0, 0, 0, 0, 0,    0,
    0,    0, 0, 0, 0, 0, 0,    0,
    0,    0, 0, 0, 0, 0, 0,    0,
    0,    0, 0, 0, 0, 0, 0,    0,
    0,    0, 0, 0, 0, 0, 0,    0,
    0,    0, 0, 0, 0, 0, 0x02, 0,
};

}  // namespace

#if defined(OBSIDIO_USE_OPENSSL)

// Generic path. Used for the caller's arbitrary-length seed (round one), by the
// reference chain that verifies every accelerated back end, and by the
// self-test. Not hot.
void sha256(const std::uint8_t* data, std::size_t len, std::uint8_t out[32]) {
  ::SHA256(data, len, out);
}

void sha256_64(const std::uint8_t in[64], std::uint8_t out[32]) {
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Transform(&ctx, in);      // block 1: the 64 message bytes
  SHA256_Transform(&ctx, kPad64);  // block 2: the constant padding block
  for (int i = 0; i < 8; ++i) {
    const std::uint32_t v = ctx.h[i];
    out[i * 4]     = static_cast<std::uint8_t>(v >> 24);
    out[i * 4 + 1] = static_cast<std::uint8_t>(v >> 16);
    out[i * 4 + 2] = static_cast<std::uint8_t>(v >> 8);
    out[i * 4 + 3] = static_cast<std::uint8_t>(v);
  }
}

#else

namespace {

constexpr std::uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline std::uint32_t rotr(std::uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

void compress(std::uint32_t state[8], const std::uint8_t block[64]) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           (static_cast<std::uint32_t>(block[i * 4 + 3]));
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + S1 + ch + K[i] + w[i];
    const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }

  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

}  // namespace

void sha256(const std::uint8_t* data, std::size_t len, std::uint8_t out[32]) {
  std::uint32_t state[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };

  std::size_t off = 0;
  for (; off + 64 <= len; off += 64) compress(state, data + off);

  // Final block(s): 0x80, zero padding, then the 64-bit big-endian bit length.
  std::uint8_t tail[128];
  const std::size_t rem = len - off;
  std::memcpy(tail, data + off, rem);
  tail[rem] = 0x80;
  const std::size_t tail_len = (rem + 1 + 8 <= 64) ? 64 : 128;
  std::memset(tail + rem + 1, 0, tail_len - rem - 1 - 8);

  const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
  for (int i = 0; i < 8; ++i) {
    tail[tail_len - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
  }
  for (std::size_t i = 0; i < tail_len; i += 64) compress(state, tail + i);

  for (int i = 0; i < 8; ++i) {
    out[i * 4]     = static_cast<std::uint8_t>(state[i] >> 24);
    out[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
    out[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
  }
}

void sha256_64(const std::uint8_t in[64], std::uint8_t out[32]) {
  std::uint32_t state[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  compress(state, in);
  compress(state, kPad64);
  for (int i = 0; i < 8; ++i) {
    out[i * 4]     = static_cast<std::uint8_t>(state[i] >> 24);
    out[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
    out[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
  }
}

#endif  // OBSIDIO_USE_OPENSSL

void hex_encode(const std::uint8_t* in, std::size_t in_len, char* out) {
  static constexpr char kDigits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < in_len; ++i) {
    out[i * 2]     = kDigits[in[i] >> 4];
    out[i * 2 + 1] = kDigits[in[i] & 0x0f];
  }
}

}  // namespace obsidio
