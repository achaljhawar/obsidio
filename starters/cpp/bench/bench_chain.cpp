// Microbenchmark for the /risk hash chain. Not part of the shipped image.
//
// Compares:
//   A. baseline      -- risk_hash() as shipped (OpenSSL SHA256() per iteration)
//   B. armv8 x1      -- hand-rolled crypto-extension chain, specialised 2-block,
//                       block 2's message schedule precomputed as constants
//   C. armv8 xN      -- N independent chains interleaved in one thread, to fill
//                       the crypto pipeline that a single serial chain leaves idle
//
// Every variant is checked against the baseline digest before it is timed.

#include <arm_neon.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "risk.hpp"

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

// ---------------------------------------------------------------------------
// Block 2 of every iteration after the first is a compile-time constant: the
// message is exactly 64 bytes, so block 2 is 0x80, zeros, and the bit length
// 512. Its whole 64-word message schedule is therefore constant, and so is
// K[i] + W[i]. Precompute that once; block 2 then needs no su0/su1 at all.
alignas(16) std::uint32_t KW2[64];

inline std::uint32_t rotr(std::uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

void init_kw2() {
  std::uint32_t w[64] = {};
  w[0] = 0x80000000u;
  w[15] = 512u;  // 64 bytes * 8 bits
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 =
        rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  for (int i = 0; i < 64; ++i) KW2[i] = KC[i] + w[i];
}

// ---------------------------------------------------------------------------
// One chain's live state: the two SHA-256 state vectors plus the four message
// vectors holding the current 64-byte hex message.
struct Lane {
  uint32x4_t s0, s1;                    // abcd, efgh
  uint32x4_t m0, m1, m2, m3;            // W0..W15 of block 1
};

const uint32x4_t kInitAbcd = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au};
const uint32x4_t kInitEfgh = {0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

#define ROUND4_MSG(L, Ma, Mb, Mc, Md, KIDX)                                  \
  do {                                                                       \
    const uint32x4_t wk = vaddq_u32((L).Ma, vld1q_u32(&KC[KIDX]));           \
    (L).Ma = vsha256su0q_u32((L).Ma, (L).Mb);                                \
    const uint32x4_t save = (L).s0;                                          \
    (L).s0 = vsha256hq_u32((L).s0, (L).s1, wk);                              \
    (L).s1 = vsha256h2q_u32((L).s1, save, wk);                               \
    (L).Ma = vsha256su1q_u32((L).Ma, (L).Mc, (L).Md);                        \
  } while (0)

#define ROUND4_LAST(L, Ma, KIDX)                                             \
  do {                                                                       \
    const uint32x4_t wk = vaddq_u32((L).Ma, vld1q_u32(&KC[KIDX]));           \
    const uint32x4_t save = (L).s0;                                          \
    (L).s0 = vsha256hq_u32((L).s0, (L).s1, wk);                              \
    (L).s1 = vsha256h2q_u32((L).s1, save, wk);                               \
  } while (0)

#define ROUND4_CONST(L, KIDX)                                                \
  do {                                                                       \
    const uint32x4_t wk = vld1q_u32(&KW2[KIDX]);                             \
    const uint32x4_t save = (L).s0;                                          \
    (L).s0 = vsha256hq_u32((L).s0, (L).s1, wk);                              \
    (L).s1 = vsha256h2q_u32((L).s1, save, wk);                               \
  } while (0)

// Hex-encode the 32-byte digest held in (a, b) straight into four big-endian
// message vectors -- no store/reload, no byte-swap round trip.
inline void digest_to_hex_msg(uint32x4_t a, uint32x4_t b, uint32x4_t& m0,
                              uint32x4_t& m1, uint32x4_t& m2, uint32x4_t& m3) {
  const uint8x16_t tbl = {'0', '1', '2', '3', '4', '5', '6', '7',
                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  // Digest bytes, big-endian.
  const uint8x16_t ba = vrev32q_u8(vreinterpretq_u8_u32(a));
  const uint8x16_t bb = vrev32q_u8(vreinterpretq_u8_u32(b));

  const uint8x16_t nib = vdupq_n_u8(0x0f);
  const uint8x16x2_t za = vzipq_u8(vshrq_n_u8(ba, 4), vandq_u8(ba, nib));
  const uint8x16x2_t zb = vzipq_u8(vshrq_n_u8(bb, 4), vandq_u8(bb, nib));

  // Hex chars are ASCII; message words are big-endian loads of those chars.
  m0 = vreinterpretq_u32_u8(vrev32q_u8(vqtbl1q_u8(tbl, za.val[0])));
  m1 = vreinterpretq_u32_u8(vrev32q_u8(vqtbl1q_u8(tbl, za.val[1])));
  m2 = vreinterpretq_u32_u8(vrev32q_u8(vqtbl1q_u8(tbl, zb.val[0])));
  m3 = vreinterpretq_u32_u8(vrev32q_u8(vqtbl1q_u8(tbl, zb.val[1])));
}

// 64 rounds over the message currently in L.m0..m3, with live scheduling.
inline void compress_scheduled(Lane& L) {
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
}

// One full 64-byte-message hash = exactly two blocks.
// ConstBlock2 = true uses the precomputed constant schedule for block 2.
template <bool ConstBlock2>
inline void hash64(Lane& L) {
  const uint32x4_t a0 = L.s0, b0 = L.s1;
  compress_scheduled(L);
  const uint32x4_t a1 = vaddq_u32(L.s0, a0);
  const uint32x4_t b1 = vaddq_u32(L.s1, b0);

  L.s0 = a1;
  L.s1 = b1;
  if (ConstBlock2) {
    // Fully unrolled so the constant path is not penalised by loop overhead.
    ROUND4_CONST(L, 0);  ROUND4_CONST(L, 4);  ROUND4_CONST(L, 8);
    ROUND4_CONST(L, 12); ROUND4_CONST(L, 16); ROUND4_CONST(L, 20);
    ROUND4_CONST(L, 24); ROUND4_CONST(L, 28); ROUND4_CONST(L, 32);
    ROUND4_CONST(L, 36); ROUND4_CONST(L, 40); ROUND4_CONST(L, 44);
    ROUND4_CONST(L, 48); ROUND4_CONST(L, 52); ROUND4_CONST(L, 56);
    ROUND4_CONST(L, 60);
  } else {
    // The generic path: build the padding block and schedule it like any other.
    L.m0 = uint32x4_t{0x80000000u, 0, 0, 0};
    L.m1 = uint32x4_t{0, 0, 0, 0};
    L.m2 = uint32x4_t{0, 0, 0, 0};
    L.m3 = uint32x4_t{0, 0, 0, 512u};
    compress_scheduled(L);
  }
  L.s0 = vaddq_u32(L.s0, a1);
  L.s1 = vaddq_u32(L.s1, b1);
}

std::string digest_hex(uint32x4_t a, uint32x4_t b) {
  std::uint8_t d[32];
  vst1q_u8(d, vrev32q_u8(vreinterpretq_u8_u32(a)));
  vst1q_u8(d + 16, vrev32q_u8(vreinterpretq_u8_u32(b)));
  static const char* hx = "0123456789abcdef";
  std::string out(64, '0');
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = hx[d[i] >> 4];
    out[i * 2 + 1] = hx[d[i] & 0xf];
  }
  return out;
}

// N chains advanced in lockstep. N == 1 is the plain specialised chain.
template <int N, bool ConstBlock2 = true>
void risk_hash_neon(const std::string* seeds, std::string* out, int iterations) {
  Lane L[N];

  // Round 1 works on the caller's seed (arbitrary length) -- use the shipped
  // scalar/OpenSSL path for it, then enter the fixed-64-byte loop.
  for (int k = 0; k < N; ++k) {
    const std::string h1 = obsidio::risk_hash(seeds[k], 1);
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(h1.data());
    L[k].s0 = kInitAbcd;
    L[k].s1 = kInitEfgh;
    L[k].m0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p)));
    L[k].m1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16)));
    L[k].m2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 32)));
    L[k].m3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 48)));
  }

  for (int i = 1; i < iterations; ++i) {
    for (int k = 0; k < N; ++k) hash64<ConstBlock2>(L[k]);
    if (i + 1 < iterations) {
      for (int k = 0; k < N; ++k) {
        digest_to_hex_msg(L[k].s0, L[k].s1, L[k].m0, L[k].m1, L[k].m2, L[k].m3);
        L[k].s0 = kInitAbcd;
        L[k].s1 = kInitEfgh;
      }
    }
  }
  for (int k = 0; k < N; ++k) out[k] = digest_hex(L[k].s0, L[k].s1);
}

// ---------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

std::string seed_at(int i) { return "0." + std::to_string(480000 + i * 7919); }

template <int N, bool ConstBlock2 = true>
void run(const char* label, const std::string& golden, double baseline_ms) {
  std::string seeds[N], out[N];
  for (int k = 0; k < N; ++k) seeds[k] = "0.5";

  risk_hash_neon<N, ConstBlock2>(seeds, out, 50000);
  for (int k = 0; k < N; ++k) {
    if (out[k] != golden) {
      std::printf("  %-16s  WRONG DIGEST -- variant is broken\n", label);
      return;
    }
  }

  // Time: enough batches that each variant hashes the same total number of
  // chains, so ms/chain is directly comparable.
  const int batches = 24 / N;
  std::string s[N], o[N];
  const auto t0 = Clock::now();
  for (int b = 0; b < batches; ++b) {
    for (int k = 0; k < N; ++k) s[k] = seed_at(b * N + k);
    risk_hash_neon<N, ConstBlock2>(s, o, 50000);
  }
  const double total = ms_since(t0);
  const double per = total / (batches * N);
  std::printf("  %-16s  %7.2f ms/chain   %5.2fx   %6.0f chains/s/core\n", label,
              per, baseline_ms / per, 1000.0 / per);
}

}  // namespace

int main() {
  init_kw2();

  const std::string golden = obsidio::risk_hash("0.5", 50000);
  std::printf("golden(0.5) = %s\n\n", golden.c_str());

  // Baseline: exactly what ships today.
  const auto t0 = Clock::now();
  for (int i = 0; i < 12; ++i) {
    volatile auto x = obsidio::risk_hash(seed_at(i), 50000);
    (void)x;
  }
  const double base = ms_since(t0) / 12.0;
  std::printf("  %-16s  %7.2f ms/chain   %5.2fx   %6.0f chains/s/core\n",
              "baseline(ossl)", base, 1.0, 1000.0 / base);

  run<1, false>("armv8 x1 genblk2", golden, base);
  run<1>("armv8 x1", golden, base);
  run<2, false>("armv8 x2 genblk2", golden, base);
  run<2>("armv8 x2", golden, base);
  run<3>("armv8 x3", golden, base);
  run<4>("armv8 x4", golden, base);
  run<6>("armv8 x6", golden, base);
  return 0;
}
