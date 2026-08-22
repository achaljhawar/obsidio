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

// The inverse of store_hex: 64 hex characters straight into registers, no
// byte-swap. Only the eight-lane path needs it, because that one carries its
// per-lane state as Hex between rounds so the schedule phase can byte-swap it
// on its own schedule rather than at load time.
inline Hex load_hex(const char in[64]) {
  Hex h;
  h.h0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 0));
  h.h1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16));
  h.h2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 32));
  h.h3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 48));
  return h;
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
inline void compress2_const(State& SA, State& SB) {
  __m128i a0 = SA.abef, a1 = SA.cdgh;
  __m128i b0 = SB.abef, b1 = SB.cdgh;
  for (int i = 0; i < 16; ++i) {
    const __m128i k = KW2V[i];
    a1 = _mm_sha256rnds2_epu32(a1, a0, k);
    b1 = _mm_sha256rnds2_epu32(b1, b0, k);
    const __m128i ks = _mm_shuffle_epi32(k, 0x0E);
    a0 = _mm_sha256rnds2_epu32(a0, a1, ks);
    b0 = _mm_sha256rnds2_epu32(b0, b1, ks);
  }
  SA.abef = _mm_add_epi32(a0, SA.abef);
  SA.cdgh = _mm_add_epi32(a1, SA.cdgh);
  SB.abef = _mm_add_epi32(b0, SB.abef);
  SB.cdgh = _mm_add_epi32(b1, SB.cdgh);
}

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
    compress2_const(sa, sb);
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
void chain3_impl(const char in_a[64], const char in_b[64], const char in_c[64],
                 int rounds, char out_a[64], char out_b[64], char out_c[64]) {
  chain2_impl(in_a, in_b, rounds, out_a, out_b);
  chain1_impl(in_c, rounds, out_c);
}

// Two pairs: per-chain rate identical to chain2's, so the four-deep batches
// the pool prefers under load lose nothing to register pressure.
void chain4_impl(const char in_a[64], const char in_b[64], const char in_c[64],
                 const char in_d[64], int rounds, char out_a[64],
                 char out_b[64], char out_c[64], char out_d[64]) {
  chain2_impl(in_a, in_b, rounds, out_a, out_b);
  chain2_impl(in_c, in_d, rounds, out_c, out_d);
}

// ---------------------------------------------------------------------------
// Eight lanes, pipelined phase-split.
//
// The two-lane kernel above is capped by a REGISTER argument: block 1 wants ~6
// XMM per lane (2 state + 4 schedule window), so two lanes plus temporaries is
// all the 16-entry file holds once sha256rnds2 has reserved XMM0. That cap is
// real, but it only binds while the message schedule is live in registers.
//
// Phase-splitting moves the schedule out: expand a lane's message into an
// L1-resident W+K buffer first, then run the rounds reading that buffer at 2
// registers per lane. Measured, the round phase alone reaches 0.3058 ns/rnds2
// against this kernel's 0.5456 -- 1.78x. But run as its own sequential phase
// the schedule stops being free (it used to hide inside sha256rnds2's 4-cycle
// latency) and costs 46% of the group-round, which ate the whole gain: the
// straight phase-split measured +6.2% against a +15% floor and was rejected.
// docs/phase-split-negative-result.md has that result in full.
//
// What works is putting the schedule back under the rounds without ever
// holding both in registers at once. The obstacle is that a lane's own next
// schedule depends on the hex its current round phase has not produced yet --
// strictly serial, no way around it. So the pipeline runs across lane GROUPS:
//
//     prime:  schedule(A)
//     step 1: rounds(A)  co-issued with  schedule(B)
//     step 2: rounds(B)  co-issued with  schedule(A')
//
// Two steps advance all eight chains by one round, and each step's two halves
// are independent. At most ONE schedule stream is live at a time -- lane 0's
// rides block 1, lane 1's rides block 2, dealt out a step per rnds2 pair --
// which is what keeps this inside the register file. Co-issuing four schedule
// streams at once, the obvious thing to try, wants 28 registers and measures
// -69%.
//
// Measured on a Ryzen 7 170, cool box, worst rep of 7, against this file's own
// chain2 in the same process: 0.3679 ns/rnds2 against 0.5200, a 1.413x risk
// path. Holding the round-phase width fixed, the co-issue alone is worth 27%.
//
// Originally only lanes 0 and 1 got their schedules co-issued and lanes 2-3
// fell through to schedule_all(), paying roughly half the schedule in the
// open. That was then probed (bench_phase_split modes A and B): dealing lanes
// 0+2 into block 1 and 1+3 into block 2 -- two schedule streams live at once,
// against this real round phase rather than the toy one that condemned it --
// measures 0.3294 vs 0.3752 ns/rnds2 worst, so the two-stream shape below is
// what ships. The denser one-stream alternative loses to its own serial
// dependence (0.4319) and stays unshipped.

// One resumable step of compress_generic's schedule: step P produces the
// (W+K) vector the round phase consumes at pair P. Splitting it this way is
// what lets the schedule be interleaved a step at a time between rnds2 pairs
// instead of running as a block. The sequence is compress_generic's, unchanged
// -- every RNDS2_PAIR there becomes a store here.
template <int P>
inline void sched_step(Msg& m, __m128i wk[16]) {
  if constexpr (P == 0) {
    wk[0] = _mm_add_epi32(m.m0, KV[0]);
  } else if constexpr (P == 1) {
    wk[1] = _mm_add_epi32(m.m1, KV[1]);
    m.m0 = _mm_sha256msg1_epu32(m.m0, m.m1);
  } else if constexpr (P == 2) {
    wk[2] = _mm_add_epi32(m.m2, KV[2]);
    m.m1 = _mm_sha256msg1_epu32(m.m1, m.m2);
  } else if constexpr (P == 3) {
    wk[3] = _mm_add_epi32(m.m3, KV[3]);
  } else if constexpr (P >= 4 && P <= 13) {
    // The steady-state group, rotating m0->m1->m2->m3 with period four:
    // extend one vector with alignr+msg2, start the next one's msg1, store.
    constexpr int r = (P - 4) % 4;
    __m128i t;
    if constexpr (r == 0) {
      t = _mm_alignr_epi8(m.m3, m.m2, 4);
      m.m0 = _mm_add_epi32(m.m0, t);
      m.m0 = _mm_sha256msg2_epu32(m.m0, m.m3);
      m.m2 = _mm_sha256msg1_epu32(m.m2, m.m3);
      wk[P] = _mm_add_epi32(m.m0, KV[P]);
    } else if constexpr (r == 1) {
      t = _mm_alignr_epi8(m.m0, m.m3, 4);
      m.m1 = _mm_add_epi32(m.m1, t);
      m.m1 = _mm_sha256msg2_epu32(m.m1, m.m0);
      m.m3 = _mm_sha256msg1_epu32(m.m3, m.m0);
      wk[P] = _mm_add_epi32(m.m1, KV[P]);
    } else if constexpr (r == 2) {
      t = _mm_alignr_epi8(m.m1, m.m0, 4);
      m.m2 = _mm_add_epi32(m.m2, t);
      m.m2 = _mm_sha256msg2_epu32(m.m2, m.m1);
      m.m0 = _mm_sha256msg1_epu32(m.m0, m.m1);
      wk[P] = _mm_add_epi32(m.m2, KV[P]);
    } else {
      t = _mm_alignr_epi8(m.m2, m.m1, 4);
      m.m3 = _mm_add_epi32(m.m3, t);
      m.m3 = _mm_sha256msg2_epu32(m.m3, m.m2);
      m.m1 = _mm_sha256msg1_epu32(m.m1, m.m2);
      wk[P] = _mm_add_epi32(m.m3, KV[P]);
    }
  } else if constexpr (P == 14) {
    // Only 64 words exist, so the last two groups need no further msg1.
    const __m128i t = _mm_alignr_epi8(m.m1, m.m0, 4);
    m.m2 = _mm_add_epi32(m.m2, t);
    m.m2 = _mm_sha256msg2_epu32(m.m2, m.m1);
    wk[14] = _mm_add_epi32(m.m2, KV[14]);
  } else {
    const __m128i t = _mm_alignr_epi8(m.m2, m.m1, 4);
    m.m3 = _mm_add_epi32(m.m3, t);
    m.m3 = _mm_sha256msg2_epu32(m.m3, m.m2);
    wk[15] = _mm_add_epi32(m.m3, KV[15]);
  }
}

#define OB_UNROLL16(X)                                                        \
  X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13)   \
  X(14) X(15)

// All sixteen steps back to back, for the prime and for the lanes that have no
// round phase left to hide under.
inline void schedule_all(Msg m, __m128i wk[16]) {
#define OB_SCHED_ALL(P) sched_step<P>(m, wk);
  OB_UNROLL16(OB_SCHED_ALL)
#undef OB_SCHED_ALL
}

constexpr int kPipeHalf = 4;

// Advance four chains one full round while computing four other chains'
// schedules for THEIR next round. hexR is read and overwritten; hexS is read
// only, and its results land in wkS.
inline void pipe_step(Hex hexR[kPipeHalf], __m128i wkR[kPipeHalf][16],
                      const Hex hexS[kPipeHalf], __m128i wkS[kPipeHalf][16]) {
  __m128i s0[kPipeHalf], s1[kPipeHalf];
  for (int i = 0; i < kPipeHalf; ++i) {
    s0[i] = kInitABEF;
    s1[i] = kInitCDGH;
  }

  // Block 1: four lanes at two registers each, round constants from L1, with
  // scheduled lanes 0 AND 2 dealt one step each per pair. Two schedule streams
  // live at once was the shape the naive co-issue probe said would blow the
  // register file; against the real round phase it holds, and it is what
  // stops lanes 2-3's schedules being paid for in the open. Measured in
  // bench_phase_split (mode A): 0.3294 vs 0.3752 ns/rnds2 worst -- 14% over
  // the single-stream shape at N=8.
  Msg sa = hex_to_msg(hexS[0]);
  Msg sc = hex_to_msg(hexS[2]);
#define OB_PIPE_B1(P)                                             \
  {                                                               \
    __m128i k0 = wkR[0][P], k1 = wkR[1][P];                       \
    __m128i k2 = wkR[2][P], k3 = wkR[3][P];                       \
    s1[0] = _mm_sha256rnds2_epu32(s1[0], s0[0], k0);              \
    s1[1] = _mm_sha256rnds2_epu32(s1[1], s0[1], k1);              \
    s1[2] = _mm_sha256rnds2_epu32(s1[2], s0[2], k2);              \
    s1[3] = _mm_sha256rnds2_epu32(s1[3], s0[3], k3);              \
    k0 = _mm_shuffle_epi32(k0, 0x0E);                             \
    k1 = _mm_shuffle_epi32(k1, 0x0E);                             \
    k2 = _mm_shuffle_epi32(k2, 0x0E);                             \
    k3 = _mm_shuffle_epi32(k3, 0x0E);                             \
    s0[0] = _mm_sha256rnds2_epu32(s0[0], s1[0], k0);              \
    s0[1] = _mm_sha256rnds2_epu32(s0[1], s1[1], k1);              \
    s0[2] = _mm_sha256rnds2_epu32(s0[2], s1[2], k2);              \
    s0[3] = _mm_sha256rnds2_epu32(s0[3], s1[3], k3);              \
    sched_step<P>(sa, wkS[0]);                                    \
    sched_step<P>(sc, wkS[2]);                                    \
  }
  OB_UNROLL16(OB_PIPE_B1)
#undef OB_PIPE_B1

  // Block 1's feed-forward. Its input state was the IV, a constant, so nothing
  // had to be kept live across the rounds to get here.
  __m128i f0[kPipeHalf], f1[kPipeHalf];
  for (int i = 0; i < kPipeHalf; ++i) {
    f0[i] = _mm_add_epi32(s0[i], kInitABEF);
    f1[i] = _mm_add_epi32(s1[i], kInitCDGH);
    s0[i] = f0[i];
    s1[i] = f1[i];
  }

  // Block 2: every lane consumes the same KW2V entry, so one register carries
  // it for all four. Scheduled lanes 1 and 3 ride here, so every lane's
  // schedule now hides under a round phase and nothing is paid for plainly.
  Msg sb = hex_to_msg(hexS[1]);
  Msg sd = hex_to_msg(hexS[3]);
#define OB_PIPE_B2(P)                                             \
  {                                                               \
    __m128i k = KW2V[P];                                          \
    s1[0] = _mm_sha256rnds2_epu32(s1[0], s0[0], k);               \
    s1[1] = _mm_sha256rnds2_epu32(s1[1], s0[1], k);               \
    s1[2] = _mm_sha256rnds2_epu32(s1[2], s0[2], k);               \
    s1[3] = _mm_sha256rnds2_epu32(s1[3], s0[3], k);               \
    k = _mm_shuffle_epi32(k, 0x0E);                               \
    s0[0] = _mm_sha256rnds2_epu32(s0[0], s1[0], k);               \
    s0[1] = _mm_sha256rnds2_epu32(s0[1], s1[1], k);               \
    s0[2] = _mm_sha256rnds2_epu32(s0[2], s1[2], k);               \
    s0[3] = _mm_sha256rnds2_epu32(s0[3], s1[3], k);               \
    sched_step<P>(sb, wkS[1]);                                    \
    sched_step<P>(sd, wkS[3]);                                    \
  }
  OB_UNROLL16(OB_PIPE_B2)
#undef OB_PIPE_B2

  for (int i = 0; i < kPipeHalf; ++i) {
    State st{_mm_add_epi32(s0[i], f0[i]), _mm_add_epi32(s1[i], f1[i])};
    hexR[i] = state_to_hex(st);
  }
}

void chain8_impl(const char in[8][64], int rounds, char out[8][64]) {
  if (rounds <= 0) {
    for (int i = 0; i < 8; ++i) std::memcpy(out[i], in[i], 64);
    return;
  }

  Hex hex_a[kPipeHalf], hex_b[kPipeHalf];
  alignas(64) __m128i wk_a[kPipeHalf][16];
  alignas(64) __m128i wk_b[kPipeHalf][16];
  for (int i = 0; i < kPipeHalf; ++i) {
    hex_a[i] = load_hex(in[i]);
    hex_b[i] = load_hex(in[kPipeHalf + i]);
  }

  // Group A's first schedule has no round phase to hide inside; one schedule
  // out of `rounds`, and one more computed at the tail that is never consumed.
  // Both are O(1) against 50,000 rounds.
  for (int i = 0; i < kPipeHalf; ++i) schedule_all(hex_to_msg(hex_a[i]), wk_a[i]);

  for (int r = 0; r < rounds; ++r) {
    pipe_step(hex_a, wk_a, hex_b, wk_b);  // rounds(A) || schedule(B)
    pipe_step(hex_b, wk_b, hex_a, wk_a);  // rounds(B) || schedule(A')
  }

  for (int i = 0; i < kPipeHalf; ++i) {
    store_hex(hex_a[i], out[i]);
    store_hex(hex_b[i], out[kPipeHalf + i]);
  }
}

#undef OB_UNROLL16

const Backend kX86Backend = {
    "x86-sha-ni (pipelined phase-split x8; x2 register-resident)",
    /*lanes=*/8, chain1_impl, chain2_impl, chain3_impl, chain4_impl,
    chain8_impl};

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
