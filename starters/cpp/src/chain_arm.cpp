// ARMv8 crypto-extension back end for the /risk chain.
//
// This translation unit is the ONLY one compiled with `+crypto` (see
// CMakeLists.txt), and nothing in it is called until arm_crypto_backend() has
// confirmed HWCAP_SHA2 at runtime. That keeps the binary safe to start on an
// ARM core without the extension, and safe to build for any architecture.
//
// On a non-aarch64 build the whole file collapses to `return nullptr`.
#include "chain_backend.hpp"

#if defined(__aarch64__)

#include <arm_neon.h>

#include <cstdint>
#include <cstring>

#if defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

namespace obsidio {
namespace chain {
namespace {

alignas(16) const std::uint32_t KC[64] = {
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

// Every message in the steady state is exactly 64 bytes, so block 2 is always
// 0x80, zeros, and the bit length 512 -- a compile-time constant. Its whole
// message schedule, and therefore K[i] + W[i], is constant too, so block 2
// needs no sha256su0/su1 at all.
//
// Worth ~2% in isolation, not the 20% you would guess: block 2 is bound by the
// sha256h -> sha256h2 dependency chain, so the scheduling work this removes was
// filling issue slots that were idle anyway. It is free, so it stays.
alignas(16) std::uint32_t KW2[64];

constexpr std::uint32_t rotr(std::uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

struct Kw2Init {
  Kw2Init() {
    std::uint32_t w[64] = {};
    w[0] = 0x80000000u;
    w[15] = 512u;  // 64 message bytes * 8
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 =
          rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 =
          rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; ++i) KW2[i] = KC[i] + w[i];
  }
};
const Kw2Init g_kw2_init;

// One chain's live state. The whole 50,000-iteration loop keeps these six
// vectors in registers and never touches memory -- that, not the intrinsics
// themselves, is where the bulk of the speedup over a per-iteration library
// call comes from.
struct Lane {
  uint32x4_t s0, s1;          // abcd, efgh
  uint32x4_t m0, m1, m2, m3;  // W0..W15 of block 1
};

const uint32x4_t kInitAbcd = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au};
const uint32x4_t kInitEfgh = {0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

#define ROUND4_MSG(L, Ma, Mb, Mc, Md, KIDX)                        \
  do {                                                             \
    const uint32x4_t wk = vaddq_u32((L).Ma, vld1q_u32(&KC[KIDX])); \
    (L).Ma = vsha256su0q_u32((L).Ma, (L).Mb);                      \
    const uint32x4_t save = (L).s0;                                \
    (L).s0 = vsha256hq_u32((L).s0, (L).s1, wk);                    \
    (L).s1 = vsha256h2q_u32((L).s1, save, wk);                     \
    (L).Ma = vsha256su1q_u32((L).Ma, (L).Mc, (L).Md);              \
  } while (0)

#define ROUND4_LAST(L, Ma, KIDX)                                   \
  do {                                                             \
    const uint32x4_t wk = vaddq_u32((L).Ma, vld1q_u32(&KC[KIDX])); \
    const uint32x4_t save = (L).s0;                                \
    (L).s0 = vsha256hq_u32((L).s0, (L).s1, wk);                    \
    (L).s1 = vsha256h2q_u32((L).s1, save, wk);                     \
  } while (0)

#define ROUND4_CONST(L, KIDX)                       \
  do {                                              \
    const uint32x4_t wk = vld1q_u32(&KW2[KIDX]);    \
    const uint32x4_t save = (L).s0;                 \
    (L).s0 = vsha256hq_u32((L).s0, (L).s1, wk);     \
    (L).s1 = vsha256h2q_u32((L).s1, save, wk);      \
  } while (0)

const uint8x16_t kHexTable = {'0', '1', '2', '3', '4', '5', '6', '7',
                              '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

// Hex-encode the digest in (s0, s1) directly into the next iteration's message
// vectors: no store, no reload, no separate byte-swap pass.
inline void digest_to_next_message(Lane& L) {
  const uint8x16_t ba = vrev32q_u8(vreinterpretq_u8_u32(L.s0));
  const uint8x16_t bb = vrev32q_u8(vreinterpretq_u8_u32(L.s1));
  const uint8x16_t nib = vdupq_n_u8(0x0f);

  const uint8x16x2_t za = vzipq_u8(vshrq_n_u8(ba, 4), vandq_u8(ba, nib));
  const uint8x16x2_t zb = vzipq_u8(vshrq_n_u8(bb, 4), vandq_u8(bb, nib));

  // The hex characters are the next message; message words are big-endian
  // loads of them, hence the vrev32.
  L.m0 = vreinterpretq_u32_u8(vrev32q_u8(vqtbl1q_u8(kHexTable, za.val[0])));
  L.m1 = vreinterpretq_u32_u8(vrev32q_u8(vqtbl1q_u8(kHexTable, za.val[1])));
  L.m2 = vreinterpretq_u32_u8(vrev32q_u8(vqtbl1q_u8(kHexTable, zb.val[0])));
  L.m3 = vreinterpretq_u32_u8(vrev32q_u8(vqtbl1q_u8(kHexTable, zb.val[1])));

  L.s0 = kInitAbcd;
  L.s1 = kInitEfgh;
}

// SHA-256 of the 64 bytes currently in m0..m3: block 1 scheduled live, block 2
// from the constant table.
inline void hash64(Lane& L) {
  const uint32x4_t a0 = L.s0, b0 = L.s1;

  ROUND4_MSG(L, m0, m1, m2, m3, 0);
  ROUND4_MSG(L, m1, m2, m3, m0, 4);
  ROUND4_MSG(L, m2, m3, m0, m1, 8);
  ROUND4_MSG(L, m3, m0, m1, m2, 12);
  ROUND4_MSG(L, m0, m1, m2, m3, 16);
  ROUND4_MSG(L, m1, m2, m3, m0, 20);
  ROUND4_MSG(L, m2, m3, m0, m1, 24);
  ROUND4_MSG(L, m3, m0, m1, m2, 28);
  ROUND4_MSG(L, m0, m1, m2, m3, 32);
  ROUND4_MSG(L, m1, m2, m3, m0, 36);
  ROUND4_MSG(L, m2, m3, m0, m1, 40);
  ROUND4_MSG(L, m3, m0, m1, m2, 44);
  ROUND4_LAST(L, m0, 48);
  ROUND4_LAST(L, m1, 52);
  ROUND4_LAST(L, m2, 56);
  ROUND4_LAST(L, m3, 60);

  const uint32x4_t a1 = vaddq_u32(L.s0, a0);
  const uint32x4_t b1 = vaddq_u32(L.s1, b0);

  L.s0 = a1;
  L.s1 = b1;
  ROUND4_CONST(L, 0);  ROUND4_CONST(L, 4);  ROUND4_CONST(L, 8);
  ROUND4_CONST(L, 12); ROUND4_CONST(L, 16); ROUND4_CONST(L, 20);
  ROUND4_CONST(L, 24); ROUND4_CONST(L, 28); ROUND4_CONST(L, 32);
  ROUND4_CONST(L, 36); ROUND4_CONST(L, 40); ROUND4_CONST(L, 44);
  ROUND4_CONST(L, 48); ROUND4_CONST(L, 52); ROUND4_CONST(L, 56);
  ROUND4_CONST(L, 60);

  L.s0 = vaddq_u32(L.s0, a1);
  L.s1 = vaddq_u32(L.s1, b1);
}

inline void lane_load(Lane& L, const char in[64]) {
  const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(in);
  L.s0 = kInitAbcd;
  L.s1 = kInitEfgh;
  L.m0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p)));
  L.m1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16)));
  L.m2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 32)));
  L.m3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 48)));
}

inline void lane_store_hex(const Lane& L, char out[64]) {
  const uint8x16_t ba = vrev32q_u8(vreinterpretq_u8_u32(L.s0));
  const uint8x16_t bb = vrev32q_u8(vreinterpretq_u8_u32(L.s1));
  const uint8x16_t nib = vdupq_n_u8(0x0f);
  const uint8x16x2_t za = vzipq_u8(vshrq_n_u8(ba, 4), vandq_u8(ba, nib));
  const uint8x16x2_t zb = vzipq_u8(vshrq_n_u8(bb, 4), vandq_u8(bb, nib));
  std::uint8_t* o = reinterpret_cast<std::uint8_t*>(out);
  vst1q_u8(o,      vqtbl1q_u8(kHexTable, za.val[0]));
  vst1q_u8(o + 16, vqtbl1q_u8(kHexTable, za.val[1]));
  vst1q_u8(o + 32, vqtbl1q_u8(kHexTable, zb.val[0]));
  vst1q_u8(o + 48, vqtbl1q_u8(kHexTable, zb.val[1]));
}

void chain1_impl(const char in[64], int rounds, char out[64]) {
  if (rounds <= 0) {
    std::memcpy(out, in, 64);
    return;
  }
  Lane L;
  lane_load(L, in);
  for (int r = 0; r < rounds; ++r) {
    hash64(L);
    if (r + 1 < rounds) digest_to_next_message(L);
  }
  lane_store_hex(L, out);
}

void chain2_impl(const char in_a[64], const char in_b[64], int rounds,
                 char out_a[64], char out_b[64]) {
  if (rounds <= 0) {
    std::memcpy(out_a, in_a, 64);
    std::memcpy(out_b, in_b, 64);
    return;
  }
  Lane A, B;
  lane_load(A, in_a);
  lane_load(B, in_b);
  // Interleaved on purpose: the two chains are independent, so the second
  // fills the pipeline bubbles the first leaves behind sha256h's latency.
  for (int r = 0; r < rounds; ++r) {
    hash64(A);
    hash64(B);
    if (r + 1 < rounds) {
      digest_to_next_message(A);
      digest_to_next_message(B);
    }
  }
  lane_store_hex(A, out_a);
  lane_store_hex(B, out_b);
}

const Backend kArmBackend = {"armv8-crypto (x2 interleaved)", chain1_impl,
                             chain2_impl};

bool cpu_has_sha2() {
#if defined(__linux__)
  return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#else
  // Every Apple Silicon core has the crypto extensions; this path only exists
  // so the chain can be benchmarked natively on macOS.
  return true;
#endif
}

}  // namespace

const Backend* arm_crypto_backend() {
  return cpu_has_sha2() ? &kArmBackend : nullptr;
}

}  // namespace chain
}  // namespace obsidio

#else  // !__aarch64__

namespace obsidio {
namespace chain {

// No accelerated back end for this architecture yet. risk.cpp falls back to the
// portable per-iteration path, which is correct, just ~7x slower.
//
// An x86 SHA-NI back end belongs here: implement chain1/chain2 with
// _mm_sha256rnds2_epu32 / _mm_sha256msg1_epu32 / _mm_sha256msg2_epu32, gate it
// on __builtin_cpu_supports("sha"), and return it below. risk.cpp will refuse
// to use it unless it reproduces the reference digests, so a wrong
// implementation costs speed, never correctness.
const Backend* arm_crypto_backend() { return nullptr; }

}  // namespace chain
}  // namespace obsidio

#endif  // __aarch64__
