// The x86 SHA-NI chain, written ONCE against an abstract 128-bit op set.
//
// Why the indirection: this code cannot be executed on the machine it was
// written on (Docker on Apple Silicon runs amd64 under Rosetta, which raises
// SIGILL on sha256rnds2). So the algorithm is parameterised over an Ops type
// with two implementations:
//
//   X86Ops    (chain_x86_ops.hpp)   real intrinsics, compiled only on x86
//   ModelOps  (chain_model_ops.hpp) portable C++ modelling each instruction
//                                   straight from the Intel SDM pseudocode
//
// The self-test runs this core under ModelOps on EVERY architecture and checks
// it against the golden digests. SHA-256 is unforgiving: if the round or
// message-schedule semantics were misunderstood, the digest would be garbage.
// So a passing model test says the algorithm structure is right.
//
// The residual risk it does NOT cover: if the model and this code shared the
// same misreading -- e.g. both swapping rnds2's two state operands -- the model
// test would pass and real hardware would differ. That is why risk.cpp still
// gates selection on verify_backend() at runtime: on a real x86 box a wrong
// back end is rejected and the fallback runs. Wrong costs speed, never a digest.
#pragma once

#include <cstdint>

namespace obsidio {
namespace chain {

// SHA-256 round constants.
inline constexpr std::uint32_t kK[64] = {
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

// Block 2 of every steady-state iteration is the constant padding block, so its
// message schedule -- and therefore K[i]+W[i] -- is a compile-time constant.
// Computed at compile time so block 2 needs no scheduling instructions at all.
struct Kw2Table {
  std::uint32_t v[64];
};

inline constexpr std::uint32_t crotr(std::uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

inline constexpr Kw2Table make_kw2() {
  std::uint32_t w[64] = {};
  w[0] = 0x80000000u;
  w[15] = 512u;  // 64 message bytes * 8 bits
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        crotr(w[i - 15], 7) ^ crotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 =
        crotr(w[i - 2], 17) ^ crotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  Kw2Table t{};
  for (int i = 0; i < 64; ++i) t.v[i] = kK[i] + w[i];
  return t;
}

inline constexpr Kw2Table kKw2 = make_kw2();

// The SHA-256 initial state, pre-arranged into the register layout SHA-NI uses:
//   ABEF lane order (low->high) = F, E, B, A
//   CDGH lane order (low->high) = H, G, D, C
// IV = a:6a09e667 b:bb67ae85 c:3c6ef372 d:a54ff53a
//      e:510e527f f:9b05688c g:1f83d9ab h:5be0cd19
// ABEF holds {F, E, B, A} low->high; CDGH holds {H, G, D, C} low->high.
inline constexpr std::uint32_t kInitAbef[4] = {0x9b05688cu, 0x510e527fu,
                                               0xbb67ae85u, 0x6a09e667u};
inline constexpr std::uint32_t kInitCdgh[4] = {0x5be0cd19u, 0x1f83d9abu,
                                               0xa54ff53au, 0x3c6ef372u};

// One chain's live state.
template <class O>
struct LaneT {
  typename O::T abef, cdgh;
  typename O::T m0, m1, m2, m3;
};

// Four rounds. rnds2 consumes two (W+K) values from the low half of wk, so the
// second pair is brought down with a 0x0E shuffle.
//
// The ping-pong is the subtle part: after two rounds the register that held
// A,B,E,F holds what is now C,D,G,H, so the two state registers swap roles on
// every call and are back in place after four.
template <class O>
inline void rounds4(typename O::T& abef, typename O::T& cdgh,
                    typename O::T wk) {
  // After call 1, `cdgh` holds the new ABEF, and `abef` still holds the old
  // ABEF -- which, two rounds on, is exactly what C,D,G,H now are. So call 2
  // passes them in that swapped sense on purpose, and both names are correct
  // again once four rounds are done.
  cdgh = O::rnds2(cdgh, abef, wk);
  abef = O::rnds2(abef, cdgh, O::shuf_0e(wk));
}

// Four rounds plus the message-schedule step that produces the next four W.
#define OBSIDIO_X86_SCHED(Ma, Mb, Mc, Md)                     \
  do {                                                        \
    L.Ma = O::msg1(L.Ma, L.Mb);                               \
    L.Ma = O::add(L.Ma, O::alignr4(L.Md, L.Mc));              \
    L.Ma = O::msg2(L.Ma, L.Md);                               \
  } while (0)

#define OBSIDIO_X86_GROUP(Ma, Mb, Mc, Md, KIDX, DO_SCHED)     \
  do {                                                        \
    const typename O::T wk =                                  \
        O::add(L.Ma, O::loadu(&kK[KIDX]));                    \
    if (DO_SCHED) OBSIDIO_X86_SCHED(Ma, Mb, Mc, Md);          \
    rounds4<O>(a, c, wk);                                     \
  } while (0)

// SHA-256 of the 64 bytes held in L.m0..m3, leaving the digest in L.abef/cdgh.
template <class O>
inline void hash64(LaneT<O>& L) {
  typename O::T a = L.abef, c = L.cdgh;
  const typename O::T a0 = a, c0 = c;

  // Block 1: the message, scheduled live.
  OBSIDIO_X86_GROUP(m0, m1, m2, m3, 0,  true);
  OBSIDIO_X86_GROUP(m1, m2, m3, m0, 4,  true);
  OBSIDIO_X86_GROUP(m2, m3, m0, m1, 8,  true);
  OBSIDIO_X86_GROUP(m3, m0, m1, m2, 12, true);
  OBSIDIO_X86_GROUP(m0, m1, m2, m3, 16, true);
  OBSIDIO_X86_GROUP(m1, m2, m3, m0, 20, true);
  OBSIDIO_X86_GROUP(m2, m3, m0, m1, 24, true);
  OBSIDIO_X86_GROUP(m3, m0, m1, m2, 28, true);
  OBSIDIO_X86_GROUP(m0, m1, m2, m3, 32, true);
  OBSIDIO_X86_GROUP(m1, m2, m3, m0, 36, true);
  OBSIDIO_X86_GROUP(m2, m3, m0, m1, 40, true);
  OBSIDIO_X86_GROUP(m3, m0, m1, m2, 44, true);
  OBSIDIO_X86_GROUP(m0, m1, m2, m3, 48, false);
  OBSIDIO_X86_GROUP(m1, m2, m3, m0, 52, false);
  OBSIDIO_X86_GROUP(m2, m3, m0, m1, 56, false);
  OBSIDIO_X86_GROUP(m3, m0, m1, m2, 60, false);

  a = O::add(a, a0);
  c = O::add(c, c0);

  // Block 2: constant padding, constant schedule, no msg1/msg2 at all.
  const typename O::T a1 = a, c1 = c;
  for (int i = 0; i < 64; i += 4) {
    rounds4<O>(a, c, O::loadu(&kKw2.v[i]));
  }
  L.abef = O::add(a, a1);
  L.cdgh = O::add(c, c1);
}

// Digest (abef/cdgh) -> the 32 raw digest bytes, in A..H order, as two vectors.
template <class O>
inline void digest_vectors(const LaneT<O>& L, typename O::T& d0,
                           typename O::T& d1) {
  const typename O::T t0 = O::shuf_1b(L.abef);  // -> A, B, E, F
  const typename O::T t1 = O::shuf_1b(L.cdgh);  // -> C, D, G, H
  d0 = O::bswap32(O::unpacklo64(t0, t1));       // A, B, C, D
  d1 = O::bswap32(O::unpackhi64(t0, t1));       // E, F, G, H
}

// 16 digest bytes -> 32 ASCII hex characters, as two vectors.
template <class O>
inline void hex16(typename O::T d, typename O::T& h0, typename O::T& h1) {
  const typename O::T mask = O::set8(0x0f);
  const typename O::T hi = O::and_(O::srli16_4(d), mask);
  const typename O::T lo = O::and_(d, mask);
  const typename O::T tbl = O::hex_table();
  h0 = O::shuffle_bytes(tbl, O::unpacklo8(hi, lo));
  h1 = O::shuffle_bytes(tbl, O::unpackhi8(hi, lo));
}

// Digest -> the next iteration's message, and reset the state to the IV.
template <class O>
inline void digest_to_next_message(LaneT<O>& L) {
  typename O::T d0, d1;
  digest_vectors<O>(L, d0, d1);
  typename O::T h0, h1, h2, h3;
  hex16<O>(d0, h0, h1);
  hex16<O>(d1, h2, h3);
  // Message words are big-endian loads of those ASCII bytes.
  L.m0 = O::bswap32(h0);
  L.m1 = O::bswap32(h1);
  L.m2 = O::bswap32(h2);
  L.m3 = O::bswap32(h3);
  L.abef = O::loadu(kInitAbef);
  L.cdgh = O::loadu(kInitCdgh);
}

template <class O>
inline void lane_load(LaneT<O>& L, const char in[64]) {
  L.abef = O::loadu(kInitAbef);
  L.cdgh = O::loadu(kInitCdgh);
  L.m0 = O::bswap32(O::loadu_bytes(in));
  L.m1 = O::bswap32(O::loadu_bytes(in + 16));
  L.m2 = O::bswap32(O::loadu_bytes(in + 32));
  L.m3 = O::bswap32(O::loadu_bytes(in + 48));
}

template <class O>
inline void lane_store_hex(const LaneT<O>& L, char out[64]) {
  typename O::T d0, d1, h0, h1, h2, h3;
  digest_vectors<O>(L, d0, d1);
  hex16<O>(d0, h0, h1);
  hex16<O>(d1, h2, h3);
  O::storeu_bytes(out,      h0);
  O::storeu_bytes(out + 16, h1);
  O::storeu_bytes(out + 32, h2);
  O::storeu_bytes(out + 48, h3);
}

template <class O>
void chain1_core(const char in[64], int rounds, char out[64]) {
  if (rounds <= 0) {
    for (int i = 0; i < 64; ++i) out[i] = in[i];
    return;
  }
  LaneT<O> L;
  lane_load<O>(L, in);
  for (int r = 0; r < rounds; ++r) {
    hash64<O>(L);
    if (r + 1 < rounds) digest_to_next_message<O>(L);
  }
  lane_store_hex<O>(L, out);
}

// Two independent chains interleaved. Same reasoning as the ARM back end:
// sha256rnds2 is latency-bound (4 cycles latency, ~2 cycles throughput on
// Intel), so a single serial chain leaves roughly half the unit idle.
template <class O>
void chain2_core(const char in_a[64], const char in_b[64], int rounds,
                 char out_a[64], char out_b[64]) {
  if (rounds <= 0) {
    for (int i = 0; i < 64; ++i) { out_a[i] = in_a[i]; out_b[i] = in_b[i]; }
    return;
  }
  LaneT<O> A, B;
  lane_load<O>(A, in_a);
  lane_load<O>(B, in_b);
  for (int r = 0; r < rounds; ++r) {
    hash64<O>(A);
    hash64<O>(B);
    if (r + 1 < rounds) {
      digest_to_next_message<O>(A);
      digest_to_next_message<O>(B);
    }
  }
  lane_store_hex<O>(A, out_a);
  lane_store_hex<O>(B, out_b);
}

}  // namespace chain
}  // namespace obsidio
