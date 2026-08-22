// x86-64 SHA extensions (SHA-NI) back end for the /risk chain.
//
// Mirrors chain_arm.cpp: block 1 of every steady-state hash gets the real
// message schedule, block 2 is always the fixed padding (0x80, zeros, bit
// length 512) so its whole message schedule is a compile-time constant and
// needs no sha256msg1/msg2 at all.
//
// Written without access to any x86 machine with a C++ toolchain -- every
// instruction sequence below was derived by hand from the documented
// semantics of each intrinsic, not copy-pasted on faith. It was then
// verified on real x86-64 hardware (AMD Ryzen 7 6800HS, WSL2 Ubuntu 24.04,
// 2026-08-22): all selftest digest checks pass (FIPS vectors, short chains,
// both full 50,000-round chains, every interleaved variant including swapped
// lanes), and it measured ~4.0x the OpenSSL-per-call reference path
// (16.7ms -> 4.2ms per chain, three repeated trials, same binary, same run --
// see STRATEGY.md section 12). chain1/chain2 are that verified code,
// byte-identical. chain3/chain4 reuse the same hash64() sequences and change
// only scheduling order -- lanes are pure functions of their own state, so
// order cannot change results, and risk.cpp verifies each entry point
// independently at boot anyway, degrading to fewer lanes rather than
// rejecting the whole back end if one ever disagrees with the oracle.
//
// verify_backend() in risk.cpp still gates it on every boot, on every
// architecture -- that is defense in depth against a future regression in
// this file, not a reason to skip re-testing after changing it:
//   RISK_BACKEND=x86-sha-ni ./build/obsidio-selftest
// on any x86-64 host with a C++ toolchain (no epoll/Linux-only APIs needed
// for the selftest binary itself). A wrong digest here gets rejected at
// runtime and the chain falls back to the fast scalar path; the Dockerfile's
// forced pass turns a rejection on a CPU that advertises SHA-NI into a build
// failure, so re-verify before merging any change to this file.
#include "chain_backend.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#include <immintrin.h>

#include <cstdint>
#include <cstring>

#include "sha256.hpp"

namespace obsidio {
namespace chain {
namespace {

// Identical table to sha256.cpp's K[] and chain_arm.cpp's KC[] -- copied
// verbatim rather than shared across translation units so this file has no
// dependency beyond the standard library and immintrin.h.
alignas(16) const std::uint32_t K[64] = {
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

// K[4i..4i+3] packed as a vector, element order matching a message vector
// loaded from memory (element 0 = first word), so MSG + KV[i] lines the right
// constant up with the right word without any extra shuffling.
__m128i KV[16];

// Block 2 is always 0x80, 47 zero bytes, then the 64-bit length 512 -- so its
// entire 64-word message schedule is known at startup. Precomputing K[i]+W[i]
// means block 2 needs zero sha256msg1/msg2 calls, only rnds2.
__m128i KW2V[16];

void init_constants() {
  for (int i = 0; i < 16; ++i) {
    KV[i] = _mm_setr_epi32(static_cast<int>(K[4 * i]),
                           static_cast<int>(K[4 * i + 1]),
                           static_cast<int>(K[4 * i + 2]),
                           static_cast<int>(K[4 * i + 3]));
  }

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
  for (int i = 0; i < 16; ++i) {
    KW2V[i] = _mm_setr_epi32(static_cast<int>(K[4 * i] + w[4 * i]),
                             static_cast<int>(K[4 * i + 1] + w[4 * i + 1]),
                             static_cast<int>(K[4 * i + 2] + w[4 * i + 2]),
                             static_cast<int>(K[4 * i + 3] + w[4 * i + 3]));
  }
}

// Reverses bytes within each 32-bit lane: dest byte i = src byte (i with its
// low 2 bits complemented against 3), i.e. a big-endian<->little-endian swap
// applied independently to each of the four 32-bit words in the vector.
const __m128i kBswap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15,
                                     14, 13, 12);

// SHA-256 IV, pre-permuted into the (C,D,G,H)/(A,B,E,F) lane arrangement that
// sha256rnds2 expects, so the permute dance in the reference algorithm only
// has to run once at the very end of a hash instead of at both ends of every
// one of the 100,000 blocks in a request. Derivation: the standard permute-in
// is TMP=shuffle(ABCD,0xB1); S1=shuffle(EFGH,0x1B);
// ABEF=alignr(TMP,S1,8); CDGH=blend(S1,TMP,0xF0) -- applied here once, by
// hand, to the fixed IV word values.
const __m128i kInitABEF =
    _mm_setr_epi32(0x9b05688c, 0x510e527f, 0xbb67ae85, 0x6a09e667);
const __m128i kInitCDGH =
    _mm_setr_epi32(0x5be0cd19, 0x1f83d9ab, 0xa54ff53a, 0x3c6ef372);

struct State {
  __m128i abef;
  __m128i cdgh;
};

// Block 1: the real 64-byte message, full live schedule.
inline void compress_generic(State& S, const std::uint8_t block[64]) {
  __m128i msg0 = _mm_shuffle_epi8(
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 0)), kBswap);
  __m128i msg1 = _mm_shuffle_epi8(
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16)), kBswap);
  __m128i msg2 = _mm_shuffle_epi8(
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 32)), kBswap);
  __m128i msg3 = _mm_shuffle_epi8(
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 48)), kBswap);

  __m128i state0 = S.abef, state1 = S.cdgh;
  __m128i msg, tmp;

#define RNDS2_PAIR(M, KIDX)                                       \
  do {                                                             \
    msg = _mm_add_epi32((M), KV[KIDX]);                            \
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);           \
    msg = _mm_shuffle_epi32(msg, 0x0E);                            \
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);           \
  } while (0)

  // Rounds 0-3: no schedule dependency yet, just consume msg0.
  RNDS2_PAIR(msg0, 0);

  // Rounds 4-7, then extend msg0 using msg1 (sha256msg1 only needs the two
  // oldest words; msg2 for msg0's msg2 op arrives later).
  RNDS2_PAIR(msg1, 1);
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);

  // Rounds 8-11.
  RNDS2_PAIR(msg2, 2);
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);

  // Rounds 12-15: msg0 can now be fully extended (needs msg2, msg3).
  RNDS2_PAIR(msg3, 3);
  tmp = _mm_alignr_epi8(msg3, msg2, 4);
  msg0 = _mm_add_epi32(msg0, tmp);
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);

  // Rounds 16-19.
  RNDS2_PAIR(msg0, 4);
  tmp = _mm_alignr_epi8(msg0, msg3, 4);
  msg1 = _mm_add_epi32(msg1, tmp);
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);

  // Rounds 20-23.
  RNDS2_PAIR(msg1, 5);
  tmp = _mm_alignr_epi8(msg1, msg0, 4);
  msg2 = _mm_add_epi32(msg2, tmp);
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);

  // Rounds 24-27.
  RNDS2_PAIR(msg2, 6);
  tmp = _mm_alignr_epi8(msg2, msg1, 4);
  msg3 = _mm_add_epi32(msg3, tmp);
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);

  // Rounds 28-31.
  RNDS2_PAIR(msg3, 7);
  tmp = _mm_alignr_epi8(msg3, msg2, 4);
  msg0 = _mm_add_epi32(msg0, tmp);
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);

  // Rounds 32-35.
  RNDS2_PAIR(msg0, 8);
  tmp = _mm_alignr_epi8(msg0, msg3, 4);
  msg1 = _mm_add_epi32(msg1, tmp);
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);

  // Rounds 36-39.
  RNDS2_PAIR(msg1, 9);
  tmp = _mm_alignr_epi8(msg1, msg0, 4);
  msg2 = _mm_add_epi32(msg2, tmp);
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);
  msg0 = _mm_sha256msg1_epu32(msg0, msg1);

  // Rounds 40-43.
  RNDS2_PAIR(msg2, 10);
  tmp = _mm_alignr_epi8(msg2, msg1, 4);
  msg3 = _mm_add_epi32(msg3, tmp);
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);
  msg1 = _mm_sha256msg1_epu32(msg1, msg2);

  // Rounds 44-47.
  RNDS2_PAIR(msg3, 11);
  tmp = _mm_alignr_epi8(msg3, msg2, 4);
  msg0 = _mm_add_epi32(msg0, tmp);
  msg0 = _mm_sha256msg2_epu32(msg0, msg3);
  msg2 = _mm_sha256msg1_epu32(msg2, msg3);

  // Rounds 48-51.
  RNDS2_PAIR(msg0, 12);
  tmp = _mm_alignr_epi8(msg0, msg3, 4);
  msg1 = _mm_add_epi32(msg1, tmp);
  msg1 = _mm_sha256msg2_epu32(msg1, msg0);
  msg3 = _mm_sha256msg1_epu32(msg3, msg0);

  // Rounds 52-55. msg2/msg3 need no further extension: only 64 words total.
  RNDS2_PAIR(msg1, 13);
  tmp = _mm_alignr_epi8(msg1, msg0, 4);
  msg2 = _mm_add_epi32(msg2, tmp);
  msg2 = _mm_sha256msg2_epu32(msg2, msg1);

  // Rounds 56-59.
  RNDS2_PAIR(msg2, 14);
  tmp = _mm_alignr_epi8(msg2, msg1, 4);
  msg3 = _mm_add_epi32(msg3, tmp);
  msg3 = _mm_sha256msg2_epu32(msg3, msg2);

  // Rounds 60-63.
  RNDS2_PAIR(msg3, 15);

#undef RNDS2_PAIR

  S.abef = _mm_add_epi32(state0, S.abef);
  S.cdgh = _mm_add_epi32(state1, S.cdgh);
}

// Block 2: every message word is a compile-time constant, so this is 16
// rnds2 pairs against the precomputed KW2V table and nothing else -- no
// message load, no byte swap, no msg1/msg2.
inline void compress_const_block2(State& S) {
  __m128i state0 = S.abef, state1 = S.cdgh;
  __m128i msg;
  for (int i = 0; i < 16; ++i) {
    msg = KW2V[i];
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
  }
  S.abef = _mm_add_epi32(state0, S.abef);
  S.cdgh = _mm_add_epi32(state1, S.cdgh);
}

// Permute the final (ABEF, CDGH) pair back to standard word order
// (A,B,C,D) / (E,F,G,H) and write the 32-byte big-endian digest.
inline void finish_digest(const State& S, std::uint8_t digest[32]) {
  const __m128i tmp = _mm_shuffle_epi32(S.abef, 0x1B);
  const __m128i s1 = _mm_shuffle_epi32(S.cdgh, 0xB1);
  const __m128i abcd = _mm_blend_epi16(tmp, s1, 0xF0);
  const __m128i efgh = _mm_alignr_epi8(s1, tmp, 8);

  _mm_storeu_si128(reinterpret_cast<__m128i*>(digest),
                   _mm_shuffle_epi8(abcd, kBswap));
  _mm_storeu_si128(reinterpret_cast<__m128i*>(digest + 16),
                   _mm_shuffle_epi8(efgh, kBswap));
}

// One full 64-byte-message hash: block 1 (real) + block 2 (constant). Writes
// straight into `out` -- no std::string, no heap traffic, since this runs
// 50,000 times per request.
inline void hash64(const char in[64], char out[64]) {
  State s{kInitABEF, kInitCDGH};
  compress_generic(s, reinterpret_cast<const std::uint8_t*>(in));
  compress_const_block2(s);
  std::uint8_t digest[32];
  finish_digest(s, digest);
  hex_encode(digest, kSha256DigestBytes, out);
}

void chain1_impl(const char in[64], int rounds, char out[64]) {
  if (rounds <= 0) {
    std::memcpy(out, in, 64);
    return;
  }
  char state[64];
  std::memcpy(state, in, 64);
  for (int r = 0; r < rounds; ++r) {
    hash64(state, state);
  }
  std::memcpy(out, state, 64);
}

// Coarse-grained interleaving: independent, data-dependency-free calls back to
// back let an out-of-order core overlap some of sha256rnds2's multi-cycle
// latency between the chains. Not as tight a weave as the ARM back end's
// round-by-round interleave -- on x86 that would mean carrying three or four
// lane states through one hand-unrolled loop against a 16-entry XMM register
// file (six registers per lane minimum), which spills before it pays. The
// instruction sequences here are byte-identical to the hardware-verified
// single-chain path; only their scheduling order differs, and scheduling order
// cannot change results because the lanes are pure functions of their own
// state.
void chain2_impl(const char in_a[64], const char in_b[64], int rounds,
                 char out_a[64], char out_b[64]) {
  if (rounds <= 0) {
    std::memcpy(out_a, in_a, 64);
    std::memcpy(out_b, in_b, 64);
    return;
  }
  char sa[64], sb[64];
  std::memcpy(sa, in_a, 64);
  std::memcpy(sb, in_b, 64);
  for (int r = 0; r < rounds; ++r) {
    hash64(sa, sa);
    hash64(sb, sb);
  }
  std::memcpy(out_a, sa, 64);
  std::memcpy(out_b, sb, 64);
}

void chain3_impl(const char in_a[64], const char in_b[64], const char in_c[64],
                 int rounds, char out_a[64], char out_b[64], char out_c[64]) {
  if (rounds <= 0) {
    std::memcpy(out_a, in_a, 64);
    std::memcpy(out_b, in_b, 64);
    std::memcpy(out_c, in_c, 64);
    return;
  }
  char sa[64], sb[64], sc[64];
  std::memcpy(sa, in_a, 64);
  std::memcpy(sb, in_b, 64);
  std::memcpy(sc, in_c, 64);
  for (int r = 0; r < rounds; ++r) {
    hash64(sa, sa);
    hash64(sb, sb);
    hash64(sc, sc);
  }
  std::memcpy(out_a, sa, 64);
  std::memcpy(out_b, sb, 64);
  std::memcpy(out_c, sc, 64);
}

void chain4_impl(const char in_a[64], const char in_b[64], const char in_c[64],
                 const char in_d[64], int rounds, char out_a[64],
                 char out_b[64], char out_c[64], char out_d[64]) {
  if (rounds <= 0) {
    std::memcpy(out_a, in_a, 64);
    std::memcpy(out_b, in_b, 64);
    std::memcpy(out_c, in_c, 64);
    std::memcpy(out_d, in_d, 64);
    return;
  }
  char sa[64], sb[64], sc[64], sd[64];
  std::memcpy(sa, in_a, 64);
  std::memcpy(sb, in_b, 64);
  std::memcpy(sc, in_c, 64);
  std::memcpy(sd, in_d, 64);
  for (int r = 0; r < rounds; ++r) {
    hash64(sa, sa);
    hash64(sb, sb);
    hash64(sc, sc);
    hash64(sd, sd);
  }
  std::memcpy(out_a, sa, 64);
  std::memcpy(out_b, sb, 64);
  std::memcpy(out_c, sc, 64);
  std::memcpy(out_d, sd, 64);
}

const Backend kX86Backend = {"x86-sha-ni (x1..x4 coarse interleave)",
                             chain1_impl, chain2_impl, chain3_impl,
                             chain4_impl};

bool cpu_has_sha_ni() {
  __builtin_cpu_init();
  return __builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1") &&
         __builtin_cpu_supports("ssse3");
}

struct Init {
  Init() { init_constants(); }
};
const Init g_init;

}  // namespace

const Backend* x86_sha_backend() {
  return cpu_has_sha_ni() ? &kX86Backend : nullptr;
}

}  // namespace chain
}  // namespace obsidio

#else  // !(__x86_64__ || _M_X64)

namespace obsidio {
namespace chain {

const Backend* x86_sha_backend() { return nullptr; }

}  // namespace chain
}  // namespace obsidio

#endif
