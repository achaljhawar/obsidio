// x86-64 SHA extensions (SHA-NI) back end for the /risk chain.
//
// Mirrors chain_arm.cpp: block 1 of every steady-state hash gets the real
// message schedule, block 2 is always the fixed padding (0x80, zeros, bit
// length 512) so its whole message schedule is a compile-time constant and
// needs no sha256msg1/msg2 at all.
//
// History, because the provenance of crypto code matters:
//   1. The single-chain instruction sequences (compress_generic,
//      compress_const_block2) were derived by hand from the documented
//      semantics of each intrinsic and verified on real x86-64 hardware
//      (AMD Ryzen 7 6800HS, WSL2, 2026-08-22): FIPS vectors, short chains,
//      both full 50,000-round chains, all interleaved variants.
//   2. A coarse chain2/3/4 (back-to-back hash64 calls per round) measured
//      +28.5% over the OpenSSL fallback on a Ryzen 7 170 (alternating-probe
//      A/B, ~1% noise floor -- see obsidio-findings.md at the repo root).
//   3. The register-resident two-lane interleave below replaced the coarse
//      version. Its digests were verified bit-identical to an independent
//      reference (portable sha256.cpp) and to the Python goldens by running
//      this exact file under a scalar emulation of the SHA-NI/SSE intrinsics,
//      since no arm64 dev machine can execute them natively.
//
// verify_backend() in risk.cpp still gates this back end on every boot, and
// the Dockerfile's forced pass turns a rejection on a CPU that advertises
// SHA-NI into a build failure. Neither is a reason to skip re-testing after
// changing this file:
//   RISK_BACKEND=x86-sha-ni ./build/obsidio-selftest
// on any x86-64 host with SHA-NI (no epoll/Linux-only APIs needed for the
// selftest binary itself) -- it must print "all checks passed", not SKIP.
#include "chain_backend.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#include <immintrin.h>

#include <cstdint>
#include <cstdlib>
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

// The upper half of each KW2V entry, pre-shuffled. sha256rnds2 consumes two
// rounds per instruction from the low quadword, so the second call of each
// pair needs _mm_shuffle_epi32(k, 0x0E). For block 2 that shuffle is a pure
// function of a constant, yet the baseline kernel recomputes it 16 times per
// hash. Hoisting it is exact -- same value, fewer instructions.
//
// Whether fewer instructions means faster is a measurement, not a deduction:
// the shuffle sits off the rnds2 dependency chain, so a latency-bound kernel
// may not notice it at all, while a port-bound one should. That is precisely
// why this is a selectable variant rather than an assumed win.
__m128i KW2V_HI[16];

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
    KW2V_HI[i] = _mm_shuffle_epi32(KW2V[i], 0x0E);
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
template <bool kHoistHi>
inline void compress_const_block2(State& S) {
  __m128i state0 = S.abef, state1 = S.cdgh;
  for (int i = 0; i < 16; ++i) {
    const __m128i lo = KW2V[i];
    state1 = _mm_sha256rnds2_epu32(state1, state0, lo);
    const __m128i hi = kHoistHi ? KW2V_HI[i] : _mm_shuffle_epi32(lo, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, hi);
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
template <bool kHoistHi>
inline void hash64(const char in[64], char out[64]) {
  State s{kInitABEF, kInitCDGH};
  compress_generic(s, reinterpret_cast<const std::uint8_t*>(in));
  compress_const_block2<kHoistHi>(s);
  std::uint8_t digest[32];
  finish_digest(s, digest);
  hex_encode(digest, kSha256DigestBytes, out);
}

template <bool kHoistHi>
void chain1_impl(const char in[64], int rounds, char out[64]) {
  if (rounds <= 0) {
    std::memcpy(out, in, 64);
    return;
  }
  char state[64];
  std::memcpy(state, in, 64);
  for (int r = 0; r < rounds; ++r) {
    hash64<kHoistHi>(state, state);
  }
  std::memcpy(out, state, 64);
}

// ---------------------------------------------------------------------------
// Register-resident two-lane interleave.
//
// The coarse chain2 this replaces called hash64() twice per round, so each
// lane's state round-tripped through a char[64] in memory 50,000 times and the
// only cross-lane overlap was whatever the out-of-order window caught across a
// whole two-block hash. Here both lanes live entirely in XMM registers for the
// whole chain: the two sha256rnds2 dependency chains are interleaved statement
// by statement so lane B's rounds fill lane A's multi-cycle rnds2 latency, and
// the digest -> 64-hex-char -> next-message conversion happens in registers
// too (PSHUFB nibble lookup), so nothing touches memory between round 1 and
// round 50,000.
//
// Two lanes is the x86 ceiling: a lane is ~6 vectors (2 state + 4 schedule),
// sha256rnds2 reserves XMM0 as its implicit operand, and the 16-entry XMM file
// fits two lanes with a handful of registers left for round temporaries. A
// third lane spills the schedule and a fourth cannot fit at all -- the ARM
// back end's x4 needs a 32-entry vector file. chain3/chain4 therefore run as
// a pair plus a single / two pairs, which keeps this function's per-chain
// rate at the deeper queue batches the pool prefers under load.

// 64 hex characters as four vectors, in memory order.
struct Hex {
  __m128i h0, h1, h2, h3;
};

// Block-1 message words, already byte-swapped into schedule order.
struct Msg {
  __m128i m0, m1, m2, m3;
};

inline Msg load_msg(const char in[64]) {
  Msg m;
  m.m0 = _mm_shuffle_epi8(
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 0)), kBswap);
  m.m1 = _mm_shuffle_epi8(
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16)), kBswap);
  m.m2 = _mm_shuffle_epi8(
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 32)), kBswap);
  m.m3 = _mm_shuffle_epi8(
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 48)), kBswap);
  return m;
}

inline Msg hex_to_msg(const Hex& h) {
  Msg m;
  m.m0 = _mm_shuffle_epi8(h.h0, kBswap);
  m.m1 = _mm_shuffle_epi8(h.h1, kBswap);
  m.m2 = _mm_shuffle_epi8(h.h2, kBswap);
  m.m3 = _mm_shuffle_epi8(h.h3, kBswap);
  return m;
}

inline void store_hex(const Hex& h, char out[64]) {
  _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 0), h.h0);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16), h.h1);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 32), h.h2);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 48), h.h3);
}

// Final (ABEF, CDGH) -> 64 lowercase hex chars, entirely in registers: permute
// back to (A..D)/(E..H) word order, byte-swap to digest byte order, split each
// byte into nibbles, look the ASCII digits up with PSHUFB, and interleave
// hi/lo. Same output as finish_digest + hex_encode, minus the memory trip.
inline Hex state_to_hex(const State& S) {
  const __m128i tmp = _mm_shuffle_epi32(S.abef, 0x1B);
  const __m128i s1 = _mm_shuffle_epi32(S.cdgh, 0xB1);
  const __m128i abcd = _mm_blend_epi16(tmp, s1, 0xF0);
  const __m128i efgh = _mm_alignr_epi8(s1, tmp, 8);

  const __m128i lut = _mm_setr_epi8('0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
  const __m128i nib = _mm_set1_epi8(0x0f);

  const __m128i d0 = _mm_shuffle_epi8(abcd, kBswap);  // digest bytes 0..15
  const __m128i d1 = _mm_shuffle_epi8(efgh, kBswap);  // digest bytes 16..31

  const __m128i hi0 = _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(d0, 4), nib));
  const __m128i lo0 = _mm_shuffle_epi8(lut, _mm_and_si128(d0, nib));
  const __m128i hi1 = _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(d1, 4), nib));
  const __m128i lo1 = _mm_shuffle_epi8(lut, _mm_and_si128(d1, nib));

  Hex h;
  h.h0 = _mm_unpacklo_epi8(hi0, lo0);
  h.h1 = _mm_unpackhi_epi8(hi0, lo0);
  h.h2 = _mm_unpacklo_epi8(hi1, lo1);
  h.h3 = _mm_unpackhi_epi8(hi1, lo1);
  return h;
}

// Both lanes' block-1 compressions, interleaved statement by statement. The
// schedule is the exact sequence of compress_generic above, duplicated per
// lane; interleaving cannot change results because each lane is a pure
// function of its own state. Consumes the Msg structs.
inline void compress2(State& SA, Msg& A, State& SB, Msg& B) {
  __m128i a0 = SA.abef, a1 = SA.cdgh;
  __m128i b0 = SB.abef, b1 = SB.cdgh;
  __m128i ka, kb, ta, tb;

#define OB_RNDS2_PAIR2(MA, MB, KIDX)                    \
  do {                                                  \
    ka = _mm_add_epi32((MA), KV[KIDX]);                 \
    kb = _mm_add_epi32((MB), KV[KIDX]);                 \
    a1 = _mm_sha256rnds2_epu32(a1, a0, ka);             \
    b1 = _mm_sha256rnds2_epu32(b1, b0, kb);             \
    ka = _mm_shuffle_epi32(ka, 0x0E);                   \
    kb = _mm_shuffle_epi32(kb, 0x0E);                   \
    a0 = _mm_sha256rnds2_epu32(a0, a1, ka);             \
    b0 = _mm_sha256rnds2_epu32(b0, b1, kb);             \
  } while (0)

// Schedule extension, both lanes: MNEXT += alignr(MCUR, MPREV, 4), then msg2.
#define OB_EXT2(MCUR, MPREV, MNEXT)                     \
  do {                                                  \
    ta = _mm_alignr_epi8(A.MCUR, A.MPREV, 4);           \
    tb = _mm_alignr_epi8(B.MCUR, B.MPREV, 4);           \
    A.MNEXT = _mm_add_epi32(A.MNEXT, ta);               \
    B.MNEXT = _mm_add_epi32(B.MNEXT, tb);               \
    A.MNEXT = _mm_sha256msg2_epu32(A.MNEXT, A.MCUR);    \
    B.MNEXT = _mm_sha256msg2_epu32(B.MNEXT, B.MCUR);    \
  } while (0)

#define OB_MSG1_2(MPREV, MCUR)                          \
  do {                                                  \
    A.MPREV = _mm_sha256msg1_epu32(A.MPREV, A.MCUR);    \
    B.MPREV = _mm_sha256msg1_epu32(B.MPREV, B.MCUR);    \
  } while (0)

  OB_RNDS2_PAIR2(A.m0, B.m0, 0);

  OB_RNDS2_PAIR2(A.m1, B.m1, 1);
  OB_MSG1_2(m0, m1);

  OB_RNDS2_PAIR2(A.m2, B.m2, 2);
  OB_MSG1_2(m1, m2);

  OB_RNDS2_PAIR2(A.m3, B.m3, 3);
  OB_EXT2(m3, m2, m0);
  OB_MSG1_2(m2, m3);

  OB_RNDS2_PAIR2(A.m0, B.m0, 4);
  OB_EXT2(m0, m3, m1);
  OB_MSG1_2(m3, m0);

  OB_RNDS2_PAIR2(A.m1, B.m1, 5);
  OB_EXT2(m1, m0, m2);
  OB_MSG1_2(m0, m1);

  OB_RNDS2_PAIR2(A.m2, B.m2, 6);
  OB_EXT2(m2, m1, m3);
  OB_MSG1_2(m1, m2);

  OB_RNDS2_PAIR2(A.m3, B.m3, 7);
  OB_EXT2(m3, m2, m0);
  OB_MSG1_2(m2, m3);

  OB_RNDS2_PAIR2(A.m0, B.m0, 8);
  OB_EXT2(m0, m3, m1);
  OB_MSG1_2(m3, m0);

  OB_RNDS2_PAIR2(A.m1, B.m1, 9);
  OB_EXT2(m1, m0, m2);
  OB_MSG1_2(m0, m1);

  OB_RNDS2_PAIR2(A.m2, B.m2, 10);
  OB_EXT2(m2, m1, m3);
  OB_MSG1_2(m1, m2);

  OB_RNDS2_PAIR2(A.m3, B.m3, 11);
  OB_EXT2(m3, m2, m0);
  OB_MSG1_2(m2, m3);

  OB_RNDS2_PAIR2(A.m0, B.m0, 12);
  OB_EXT2(m0, m3, m1);
  OB_MSG1_2(m3, m0);

  OB_RNDS2_PAIR2(A.m1, B.m1, 13);
  OB_EXT2(m1, m0, m2);

  OB_RNDS2_PAIR2(A.m2, B.m2, 14);
  OB_EXT2(m2, m1, m3);

  OB_RNDS2_PAIR2(A.m3, B.m3, 15);

#undef OB_RNDS2_PAIR2
#undef OB_EXT2
#undef OB_MSG1_2

  SA.abef = _mm_add_epi32(a0, SA.abef);
  SA.cdgh = _mm_add_epi32(a1, SA.cdgh);
  SB.abef = _mm_add_epi32(b0, SB.abef);
  SB.cdgh = _mm_add_epi32(b1, SB.cdgh);
}

// Both lanes' constant second block: the same KW2V entry feeds both rnds2
// chains, so this is 64 interleaved rounds with zero schedule work and only
// one constant load per four rounds.
template <bool kHoistHi>
inline void compress2_const(State& SA, State& SB) {
  __m128i a0 = SA.abef, a1 = SA.cdgh;
  __m128i b0 = SB.abef, b1 = SB.cdgh;
  for (int i = 0; i < 16; ++i) {
    const __m128i k = KW2V[i];
    a1 = _mm_sha256rnds2_epu32(a1, a0, k);
    b1 = _mm_sha256rnds2_epu32(b1, b0, k);
    const __m128i ks = kHoistHi ? KW2V_HI[i] : _mm_shuffle_epi32(k, 0x0E);
    a0 = _mm_sha256rnds2_epu32(a0, a1, ks);
    b0 = _mm_sha256rnds2_epu32(b0, b1, ks);
  }
  SA.abef = _mm_add_epi32(a0, SA.abef);
  SA.cdgh = _mm_add_epi32(a1, SA.cdgh);
  SB.abef = _mm_add_epi32(b0, SB.abef);
  SB.cdgh = _mm_add_epi32(b1, SB.cdgh);
}

template <bool kHoistHi>
void chain2_impl(const char in_a[64], const char in_b[64], int rounds,
                 char out_a[64], char out_b[64]) {
  if (rounds <= 0) {
    std::memcpy(out_a, in_a, 64);
    std::memcpy(out_b, in_b, 64);
    return;
  }
  Msg ma = load_msg(in_a);
  Msg mb = load_msg(in_b);
  for (int r = 0;;) {
    State sa{kInitABEF, kInitCDGH};
    State sb{kInitABEF, kInitCDGH};
    compress2(sa, ma, sb, mb);
    compress2_const<kHoistHi>(sa, sb);
    const Hex ha = state_to_hex(sa);
    const Hex hb = state_to_hex(sb);
    if (++r == rounds) {
      store_hex(ha, out_a);
      store_hex(hb, out_b);
      return;
    }
    ma = hex_to_msg(ha);
    mb = hex_to_msg(hb);
  }
}

// A pair plus a single: two register-resident lanes is the XMM ceiling, so
// the third chain runs after the pair rather than spilling all three.
template <bool kHoistHi>
void chain3_impl(const char in_a[64], const char in_b[64], const char in_c[64],
                 int rounds, char out_a[64], char out_b[64], char out_c[64]) {
  chain2_impl<kHoistHi>(in_a, in_b, rounds, out_a, out_b);
  chain1_impl<kHoistHi>(in_c, rounds, out_c);
}

// Two pairs: per-chain rate identical to chain2's, so the four-deep batches
// the pool prefers under load lose nothing to register pressure.
template <bool kHoistHi>
void chain4_impl(const char in_a[64], const char in_b[64], const char in_c[64],
                 const char in_d[64], int rounds, char out_a[64],
                 char out_b[64], char out_c[64], char out_d[64]) {
  chain2_impl<kHoistHi>(in_a, in_b, rounds, out_a, out_b);
  chain2_impl<kHoistHi>(in_c, in_d, rounds, out_c, out_d);
}

// Two instantiations of the identical kernel, differing only in whether the
// block-2 upper-half constants are recomputed or read from KW2V_HI. Both are
// verified independently at boot by risk.cpp, and the self-test pins both to
// the same goldens, so picking between them can never affect a digest -- only
// a cycle count.
const Backend kX86Baseline = {
    "x86-sha-ni (register-resident x2; x3/x4 as pairs)",
    chain1_impl<false>, chain2_impl<false>, chain3_impl<false>,
    chain4_impl<false>};

const Backend kX86Hoisted = {
    "x86-sha-ni (register-resident x2, hoisted block-2 constants)",
    chain1_impl<true>, chain2_impl<true>, chain3_impl<true>, chain4_impl<true>};

// Which kernel serves. Defaults to the variant that has actually been measured
// end to end on x86 hardware; the tuned one stays opt-in until it clears the
// win threshold on the grading CPU, because an unmeasured "optimisation" that
// ships by default is just an unreviewed change.
//   RISK_X86_KERNEL=baseline   (default)
//   RISK_X86_KERNEL=hoisted
const Backend* select_kernel() {
  const char* raw = std::getenv("RISK_X86_KERNEL");
  if (raw != nullptr && std::strcmp(raw, "hoisted") == 0) return &kX86Hoisted;
  return &kX86Baseline;
}

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
  return cpu_has_sha_ni() ? select_kernel() : nullptr;
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
