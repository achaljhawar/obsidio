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

// ---------------------------------------------------------------------------
// True round-by-round two-lane interleave.
//
// The coarse path below (hash64 called once per lane per round) is limited by
// two things that a fused kernel removes:
//
//   1. sha256rnds2 has ~4-cycle latency against ~1-per-2-cycle throughput, so
//      a single serial chain leaves the SHA port mostly idle waiting on its
//      own dependency. Exactly two lanes saturate that port; a third does not
//      fit (see the register budget below), which is why this stops at two
//      rather than at four like the ARM back end.
//   2. Every coarse round round-trips the 64-byte state through memory AND
//      through hex_encode()'s scalar byte loop -- 64 table lookups and 64
//      stores per hash, 50,000 times per request. The fused kernel keeps the
//      chain state in registers end to end and does the hex conversion with
//      two pshufb instructions per 16 bytes.
//
// Register budget, the reason this is x2 and not x3:
//   per lane: 4 message-schedule vectors + 2 state vectors      = 6
//   two lanes:                                                  = 12
//   two scratch temporaries                                     = 14
//   xmm0, reserved by sha256rnds2's implicit third operand      = 15 of 16
// A third lane would want 21 and spills before it pays.
//
// Correctness: each lane is a pure function of its own state, so interleaving
// cannot change results -- but that is an argument, not evidence, and this is
// exactly the kind of change that silently corrupts one lane. selftest.cpp
// cross-checks every lane against the reference oracle and against the other
// lane counts, and risk.cpp's verify_backend() re-checks at every boot.

#define X2_PAIR(S0, S1, M, KIDX, T)            \
  T = _mm_add_epi32((M), KV[KIDX]);            \
  S1 = _mm_sha256rnds2_epu32(S1, S0, T);       \
  T = _mm_shuffle_epi32(T, 0x0E);              \
  S0 = _mm_sha256rnds2_epu32(S0, S1, T)

#define X2_MSG1(MA, MB) MA = _mm_sha256msg1_epu32(MA, MB)

#define X2_EXT(MB, MD, MA, T)                  \
  T = _mm_alignr_epi8(MA, MD, 4);              \
  MB = _mm_add_epi32(MB, T);                   \
  MB = _mm_sha256msg2_epu32(MB, MA)

// Block 1 for two lanes, interleaved round by round. The per-lane instruction
// sequence is identical to compress_generic() above, step for step; only the
// scheduling is different.
inline void block1_x2(__m128i& s0a, __m128i& s1a, __m128i& m0a, __m128i& m1a,
                      __m128i& m2a, __m128i& m3a, __m128i& s0b, __m128i& s1b,
                      __m128i& m0b, __m128i& m1b, __m128i& m2b, __m128i& m3b) {
  __m128i ta, tb;

  X2_PAIR(s0a, s1a, m0a, 0, ta);  X2_PAIR(s0b, s1b, m0b, 0, tb);

  X2_PAIR(s0a, s1a, m1a, 1, ta);  X2_PAIR(s0b, s1b, m1b, 1, tb);
  X2_MSG1(m0a, m1a);              X2_MSG1(m0b, m1b);

  X2_PAIR(s0a, s1a, m2a, 2, ta);  X2_PAIR(s0b, s1b, m2b, 2, tb);
  X2_MSG1(m1a, m2a);              X2_MSG1(m1b, m2b);

  X2_PAIR(s0a, s1a, m3a, 3, ta);  X2_PAIR(s0b, s1b, m3b, 3, tb);
  X2_EXT(m0a, m2a, m3a, ta);      X2_EXT(m0b, m2b, m3b, tb);
  X2_MSG1(m2a, m3a);              X2_MSG1(m2b, m3b);

  X2_PAIR(s0a, s1a, m0a, 4, ta);  X2_PAIR(s0b, s1b, m0b, 4, tb);
  X2_EXT(m1a, m3a, m0a, ta);      X2_EXT(m1b, m3b, m0b, tb);
  X2_MSG1(m3a, m0a);              X2_MSG1(m3b, m0b);

  X2_PAIR(s0a, s1a, m1a, 5, ta);  X2_PAIR(s0b, s1b, m1b, 5, tb);
  X2_EXT(m2a, m0a, m1a, ta);      X2_EXT(m2b, m0b, m1b, tb);
  X2_MSG1(m0a, m1a);              X2_MSG1(m0b, m1b);

  X2_PAIR(s0a, s1a, m2a, 6, ta);  X2_PAIR(s0b, s1b, m2b, 6, tb);
  X2_EXT(m3a, m1a, m2a, ta);      X2_EXT(m3b, m1b, m2b, tb);
  X2_MSG1(m1a, m2a);              X2_MSG1(m1b, m2b);

  X2_PAIR(s0a, s1a, m3a, 7, ta);  X2_PAIR(s0b, s1b, m3b, 7, tb);
  X2_EXT(m0a, m2a, m3a, ta);      X2_EXT(m0b, m2b, m3b, tb);
  X2_MSG1(m2a, m3a);              X2_MSG1(m2b, m3b);

  X2_PAIR(s0a, s1a, m0a, 8, ta);  X2_PAIR(s0b, s1b, m0b, 8, tb);
  X2_EXT(m1a, m3a, m0a, ta);      X2_EXT(m1b, m3b, m0b, tb);
  X2_MSG1(m3a, m0a);              X2_MSG1(m3b, m0b);

  X2_PAIR(s0a, s1a, m1a, 9, ta);  X2_PAIR(s0b, s1b, m1b, 9, tb);
  X2_EXT(m2a, m0a, m1a, ta);      X2_EXT(m2b, m0b, m1b, tb);
  X2_MSG1(m0a, m1a);              X2_MSG1(m0b, m1b);

  X2_PAIR(s0a, s1a, m2a, 10, ta); X2_PAIR(s0b, s1b, m2b, 10, tb);
  X2_EXT(m3a, m1a, m2a, ta);      X2_EXT(m3b, m1b, m2b, tb);
  X2_MSG1(m1a, m2a);              X2_MSG1(m1b, m2b);

  X2_PAIR(s0a, s1a, m3a, 11, ta); X2_PAIR(s0b, s1b, m3b, 11, tb);
  X2_EXT(m0a, m2a, m3a, ta);      X2_EXT(m0b, m2b, m3b, tb);
  X2_MSG1(m2a, m3a);              X2_MSG1(m2b, m3b);

  X2_PAIR(s0a, s1a, m0a, 12, ta); X2_PAIR(s0b, s1b, m0b, 12, tb);
  X2_EXT(m1a, m3a, m0a, ta);      X2_EXT(m1b, m3b, m0b, tb);
  X2_MSG1(m3a, m0a);              X2_MSG1(m3b, m0b);

  // m2/m3 need no further extension: only 64 schedule words exist.
  X2_PAIR(s0a, s1a, m1a, 13, ta); X2_PAIR(s0b, s1b, m1b, 13, tb);
  X2_EXT(m2a, m0a, m1a, ta);      X2_EXT(m2b, m0b, m1b, tb);

  X2_PAIR(s0a, s1a, m2a, 14, ta); X2_PAIR(s0b, s1b, m2b, 14, tb);
  X2_EXT(m3a, m1a, m2a, ta);      X2_EXT(m3b, m1b, m2b, tb);

  X2_PAIR(s0a, s1a, m3a, 15, ta); X2_PAIR(s0b, s1b, m3b, 15, tb);
}

// Block 2 for two lanes. Every message word is a compile-time constant, so
// this is rnds2 against KW2V and nothing else. The schedule registers are dead
// by now, so register pressure here is trivial.
inline void block2_x2(__m128i& s0a, __m128i& s1a, __m128i& s0b,
                      __m128i& s1b) {
  for (int i = 0; i < 16; ++i) {
    __m128i ta = KW2V[i];
    __m128i tb = ta;
    s1a = _mm_sha256rnds2_epu32(s1a, s0a, ta);
    s1b = _mm_sha256rnds2_epu32(s1b, s0b, tb);
    ta = _mm_shuffle_epi32(ta, 0x0E);
    tb = ta;
    s0a = _mm_sha256rnds2_epu32(s0a, s1a, ta);
    s0b = _mm_sha256rnds2_epu32(s0b, s1b, tb);
  }
}

// Permute (ABEF, CDGH) back to standard order and byte-swap, leaving the
// 32-byte digest in two registers instead of storing it.
inline void finish_regs(__m128i abef, __m128i cdgh, __m128i& abcd,
                        __m128i& efgh) {
  const __m128i tmp = _mm_shuffle_epi32(abef, 0x1B);
  const __m128i s1 = _mm_shuffle_epi32(cdgh, 0xB1);
  abcd = _mm_shuffle_epi8(_mm_blend_epi16(tmp, s1, 0xF0), kBswap);
  efgh = _mm_shuffle_epi8(_mm_alignr_epi8(s1, tmp, 8), kBswap);
}

const __m128i kHexLut = _mm_setr_epi8('0', '1', '2', '3', '4', '5', '6', '7',
                                      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
const __m128i kNibbleMask = _mm_set1_epi8(0x0F);

// 16 raw bytes -> 32 lowercase ASCII hex characters, in two vectors.
// Replaces 32 iterations of hex_encode()'s scalar table-lookup loop.
// The srli_epi16 shifts across byte boundaries, but the per-byte mask
// discards the bits that crossed, leaving each byte's high nibble.
inline void hex16(__m128i v, __m128i& lo_out, __m128i& hi_out) {
  const __m128i hi = _mm_and_si128(_mm_srli_epi16(v, 4), kNibbleMask);
  const __m128i lo = _mm_and_si128(v, kNibbleMask);
  lo_out = _mm_shuffle_epi8(kHexLut, _mm_unpacklo_epi8(hi, lo));
  hi_out = _mm_shuffle_epi8(kHexLut, _mm_unpackhi_epi8(hi, lo));
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

// Two chains advanced in lockstep, round by round, with both lanes' state and
// message schedules resident in XMM for the whole run. See the register-budget
// note above block1_x2() for why this stops at two lanes on x86 while the ARM
// back end goes to four.
//
// Nothing round-trips through memory between iterations: the digest is
// hex-encoded with pshufb straight back into the next round's message
// registers. kBswap is an involution, so the final ASCII state is recovered
// from the message vectors at the end with one more shuffle rather than being
// stored on every iteration.
void chain2_impl(const char in_a[64], const char in_b[64], int rounds,
                 char out_a[64], char out_b[64]) {
  if (rounds <= 0) {
    std::memcpy(out_a, in_a, 64);
    std::memcpy(out_b, in_b, 64);
    return;
  }

  const __m128i* pa = reinterpret_cast<const __m128i*>(in_a);
  const __m128i* pb = reinterpret_cast<const __m128i*>(in_b);
  __m128i m0a = _mm_shuffle_epi8(_mm_loadu_si128(pa + 0), kBswap);
  __m128i m1a = _mm_shuffle_epi8(_mm_loadu_si128(pa + 1), kBswap);
  __m128i m2a = _mm_shuffle_epi8(_mm_loadu_si128(pa + 2), kBswap);
  __m128i m3a = _mm_shuffle_epi8(_mm_loadu_si128(pa + 3), kBswap);
  __m128i m0b = _mm_shuffle_epi8(_mm_loadu_si128(pb + 0), kBswap);
  __m128i m1b = _mm_shuffle_epi8(_mm_loadu_si128(pb + 1), kBswap);
  __m128i m2b = _mm_shuffle_epi8(_mm_loadu_si128(pb + 2), kBswap);
  __m128i m3b = _mm_shuffle_epi8(_mm_loadu_si128(pb + 3), kBswap);

  for (int r = 0; r < rounds; ++r) {
    __m128i s0a = kInitABEF, s1a = kInitCDGH;
    __m128i s0b = kInitABEF, s1b = kInitCDGH;

    block1_x2(s0a, s1a, m0a, m1a, m2a, m3a, s0b, s1b, m0b, m1b, m2b, m3b);

    // Block 1 always starts from the IV, so the feed-forward add is against
    // the constant rather than a loaded previous state.
    s0a = _mm_add_epi32(s0a, kInitABEF);
    s1a = _mm_add_epi32(s1a, kInitCDGH);
    s0b = _mm_add_epi32(s0b, kInitABEF);
    s1b = _mm_add_epi32(s1b, kInitCDGH);

    const __m128i h0a = s0a, h1a = s1a;
    const __m128i h0b = s0b, h1b = s1b;

    block2_x2(s0a, s1a, s0b, s1b);

    __m128i abcd_a, efgh_a, abcd_b, efgh_b;
    finish_regs(_mm_add_epi32(s0a, h0a), _mm_add_epi32(s1a, h1a), abcd_a,
                efgh_a);
    finish_regs(_mm_add_epi32(s0b, h0b), _mm_add_epi32(s1b, h1b), abcd_b,
                efgh_b);

    // Hex straight into the next round's message vectors.
    __m128i x0, x1, x2, x3;
    hex16(abcd_a, x0, x1);
    hex16(efgh_a, x2, x3);
    m0a = _mm_shuffle_epi8(x0, kBswap);
    m1a = _mm_shuffle_epi8(x1, kBswap);
    m2a = _mm_shuffle_epi8(x2, kBswap);
    m3a = _mm_shuffle_epi8(x3, kBswap);

    hex16(abcd_b, x0, x1);
    hex16(efgh_b, x2, x3);
    m0b = _mm_shuffle_epi8(x0, kBswap);
    m1b = _mm_shuffle_epi8(x1, kBswap);
    m2b = _mm_shuffle_epi8(x2, kBswap);
    m3b = _mm_shuffle_epi8(x3, kBswap);
  }

  __m128i* qa = reinterpret_cast<__m128i*>(out_a);
  __m128i* qb = reinterpret_cast<__m128i*>(out_b);
  _mm_storeu_si128(qa + 0, _mm_shuffle_epi8(m0a, kBswap));
  _mm_storeu_si128(qa + 1, _mm_shuffle_epi8(m1a, kBswap));
  _mm_storeu_si128(qa + 2, _mm_shuffle_epi8(m2a, kBswap));
  _mm_storeu_si128(qa + 3, _mm_shuffle_epi8(m3a, kBswap));
  _mm_storeu_si128(qb + 0, _mm_shuffle_epi8(m0b, kBswap));
  _mm_storeu_si128(qb + 1, _mm_shuffle_epi8(m1b, kBswap));
  _mm_storeu_si128(qb + 2, _mm_shuffle_epi8(m2b, kBswap));
  _mm_storeu_si128(qb + 3, _mm_shuffle_epi8(m3b, kBswap));
}

void chain3_impl(const char in_a[64], const char in_b[64], const char in_c[64],
                 int rounds, char out_a[64], char out_b[64], char out_c[64]) {
  if (rounds <= 0) {
    std::memcpy(out_a, in_a, 64);
    std::memcpy(out_b, in_b, 64);
    std::memcpy(out_c, in_c, 64);
    return;
  }
  // Two lanes already saturate the SHA port, so a third lane is served by a
  // second pass rather than by widening the kernel past the register file.
  chain2_impl(in_a, in_b, rounds, out_a, out_b);
  chain1_impl(in_c, rounds, out_c);
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
  // Four lanes as two saturating pairs.
  chain2_impl(in_a, in_b, rounds, out_a, out_b);
  chain2_impl(in_c, in_d, rounds, out_c, out_d);
}

const Backend kX86Backend = {"x86-sha-ni (fused x2 round-by-round)",
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
