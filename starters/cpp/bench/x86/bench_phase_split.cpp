// Probe A: what does it cost to keep N chain states live across a phase
// boundary?
//
// This is the quantity that killed Stage 1 and that nobody measured before
// building it. Stage 1 widened block 2 to four lanes, projected +22.2% from
// two correctly-measured halves, and delivered -23% -- because the projection
// treated the two blocks as separable when four chain states had to cross the
// boundary between them, spilling inside block 1's 64-round schedule loop.
//
// Stage 2's claim is that a different split does not have that problem:
//
//   Schedule phase   Expand each lane's 64-byte message into an L1-resident
//                    W+K buffer. The schedule depends only on the message --
//                    the previous round's hex output -- so NO chain state is
//                    live here. 6 registers, one lane (or one pair) at a time.
//   Round phase      All N lanes interleaved at 2 registers each. Block 1
//                    reads W+K from the buffers instead of from four live
//                    schedule registers; block 2 reads KW2V exactly as today.
//
// The plan's guardrail is that no projection built from separately-measured
// halves may cross a gate. So this bench does not measure the halves. It
// builds the whole structure -- real register pressure, real buffers, real
// hex round trip -- and times it end to end against the shipped kernel running
// in the same process, on the same clock, in the same thermal state.
//
// Correctness first, always: every configuration is verified against
// obsidio::risk_hash() before anything is timed. A wrong digest scores zero
// and looks exactly like a right one.
//
// Build: not part of the server. See bench/ryzen/README.md.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if !defined(__x86_64__)
int main() {
  std::printf("bench_phase_split: x86-64 only.\n");
  return 0;
}
#else

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "risk.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaxLanes = 6;
constexpr int kVerifyRounds = 512;
constexpr int kTimeRounds = 400000;
constexpr int kReps = 7;

// --- constants, transcribed from src/chain_x86.cpp --------------------------
// Reproduced rather than shared because that file keeps them in an anonymous
// namespace. Any transcription error is caught by the digest check below,
// which compares against the shipped kernel through the public API.

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

__m128i KV[16];
__m128i KW2V[16];

const __m128i kBswap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15,
                                     14, 13, 12);
const __m128i kInitABEF =
    _mm_setr_epi32(0x9b05688c, 0x510e527f, 0xbb67ae85, 0x6a09e667);
const __m128i kInitCDGH =
    _mm_setr_epi32(0x5be0cd19, 0x1f83d9ab, 0xa54ff53a, 0x3c6ef372);

void init_constants() {
  for (int i = 0; i < 16; ++i) {
    KV[i] = _mm_setr_epi32(
        static_cast<int>(K[4 * i]), static_cast<int>(K[4 * i + 1]),
        static_cast<int>(K[4 * i + 2]), static_cast<int>(K[4 * i + 3]));
  }
  std::uint32_t w[64] = {};
  w[0] = 0x80000000u;
  w[15] = 512u;
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

struct Hex {
  __m128i h0, h1, h2, h3;
};

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

inline Hex state_to_hex(__m128i abef, __m128i cdgh) {
  const __m128i tmp = _mm_shuffle_epi32(abef, 0x1B);
  const __m128i s1 = _mm_shuffle_epi32(cdgh, 0xB1);
  const __m128i abcd = _mm_blend_epi16(tmp, s1, 0xF0);
  const __m128i efgh = _mm_alignr_epi8(s1, tmp, 8);

  const __m128i lut = _mm_setr_epi8('0', '1', '2', '3', '4', '5', '6', '7', '8',
                                    '9', 'a', 'b', 'c', 'd', 'e', 'f');
  const __m128i nib = _mm_set1_epi8(0x0f);

  const __m128i d0 = _mm_shuffle_epi8(abcd, kBswap);
  const __m128i d1 = _mm_shuffle_epi8(efgh, kBswap);

  const __m128i hi0 =
      _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(d0, 4), nib));
  const __m128i lo0 = _mm_shuffle_epi8(lut, _mm_and_si128(d0, nib));
  const __m128i hi1 =
      _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(d1, 4), nib));
  const __m128i lo1 = _mm_shuffle_epi8(lut, _mm_and_si128(d1, nib));

  Hex h;
  h.h0 = _mm_unpacklo_epi8(hi0, lo0);
  h.h1 = _mm_unpackhi_epi8(hi0, lo0);
  h.h2 = _mm_unpacklo_epi8(hi1, lo1);
  h.h3 = _mm_unpackhi_epi8(hi1, lo1);
  return h;
}

// --- the schedule phase -----------------------------------------------------
// compress_generic's exact schedule sequence with every RNDS2_PAIR replaced by
// a store of (W + K). Six registers live -- m0..m3, a temporary, and the KV
// constant -- and, the whole point, NO chain state.
// always_inline is load-bearing, not decoration. GCC's size heuristic leaves
// this out of line -- it is ~135 instructions and called N times per round --
// and an out-of-line call wrecks the measurement three ways: the call itself,
// the SysV ABI making every XMM caller-saved so the round phase's state spills
// around it, and the two-lanes-interleaved variant silently becoming two
// sequential calls that no scheduler can overlap. Measured before this
// attribute was added: N=4 read 0.4885 ns and ilv=2 was indistinguishable
// from ilv=1, which is exactly what a call boundary looks like.
__attribute__((always_inline)) inline void schedule_lane(const Hex& h,
                                                         __m128i wk[16]) {
  __m128i m0 = _mm_shuffle_epi8(h.h0, kBswap);
  __m128i m1 = _mm_shuffle_epi8(h.h1, kBswap);
  __m128i m2 = _mm_shuffle_epi8(h.h2, kBswap);
  __m128i m3 = _mm_shuffle_epi8(h.h3, kBswap);
  __m128i t;

  wk[0] = _mm_add_epi32(m0, KV[0]);
  wk[1] = _mm_add_epi32(m1, KV[1]);
  m0 = _mm_sha256msg1_epu32(m0, m1);
  wk[2] = _mm_add_epi32(m2, KV[2]);
  m1 = _mm_sha256msg1_epu32(m1, m2);
  wk[3] = _mm_add_epi32(m3, KV[3]);

#define SCHED_STEP(DST, A, B, C, D, IDX)               \
  do {                                                 \
    t = _mm_alignr_epi8((C), (D), 4);                  \
    (A) = _mm_add_epi32((A), t);                       \
    (A) = _mm_sha256msg2_epu32((A), (C));              \
    (B) = _mm_sha256msg1_epu32((B), (C));              \
    (DST) = _mm_add_epi32((A), KV[IDX]);               \
  } while (0)

  // A gets extended and stored; B gets its msg1 half done early; C and D are
  // the two newest vectors feeding alignr. The rotation is m0->m1->m2->m3.
  SCHED_STEP(wk[4], m0, m2, m3, m2, 4);
  SCHED_STEP(wk[5], m1, m3, m0, m3, 5);
  SCHED_STEP(wk[6], m2, m0, m1, m0, 6);
  SCHED_STEP(wk[7], m3, m1, m2, m1, 7);
  SCHED_STEP(wk[8], m0, m2, m3, m2, 8);
  SCHED_STEP(wk[9], m1, m3, m0, m3, 9);
  SCHED_STEP(wk[10], m2, m0, m1, m0, 10);
  SCHED_STEP(wk[11], m3, m1, m2, m1, 11);
  SCHED_STEP(wk[12], m0, m2, m3, m2, 12);
  SCHED_STEP(wk[13], m1, m3, m0, m3, 13);
#undef SCHED_STEP

  // The last two groups need no further msg1: only 64 words exist.
  t = _mm_alignr_epi8(m1, m0, 4);
  m2 = _mm_add_epi32(m2, t);
  m2 = _mm_sha256msg2_epu32(m2, m1);
  wk[14] = _mm_add_epi32(m2, KV[14]);

  t = _mm_alignr_epi8(m2, m1, 4);
  m3 = _mm_add_epi32(m3, t);
  m3 = _mm_sha256msg2_epu32(m3, m2);
  wk[15] = _mm_add_epi32(m3, KV[15]);
}

// --- the phase-split group round --------------------------------------------
// SCHED_ILV picks whether the schedule phase runs one lane at a time or two
// interleaved. The plan leaves that as a measured choice; both are here.
// SKIP_SCHED=1 runs the round phase against whatever is already in the buffer.
// The digests it produces are garbage and it is never verified or reported as
// a result -- it exists only to decompose a joint measurement that has already
// been taken, so the write-up can say WHERE the time goes rather than guessing.
// This is not a projection input: nothing below adds it to anything.
template <int N, int SCHED_ILV, int SKIP_SCHED = 0>
inline void group_round(Hex hex[N], __m128i wk[N][16]) {
  // Phase 1: schedule. No chain state live anywhere in this loop.
  if (SKIP_SCHED) {
    // deliberately nothing
  } else if (SCHED_ILV == 1) {
    for (int i = 0; i < N; ++i) schedule_lane(hex[i], wk[i]);
  } else {
    int i = 0;
    for (; i + 1 < N; i += 2) {
      // Two lanes' schedules interleaved by the compiler: 12 registers, still
      // no chain state.
      schedule_lane(hex[i], wk[i]);
      schedule_lane(hex[i + 1], wk[i + 1]);
    }
    for (; i < N; ++i) schedule_lane(hex[i], wk[i]);
  }

  // Phase 2a: block 1. N lanes interleaved, 2 state registers each, round
  // constants from L1 instead of from live schedule registers.
  __m128i st0[N], st1[N], msg[N];
#pragma GCC unroll 8
  for (int i = 0; i < N; ++i) {
    st0[i] = kInitABEF;
    st1[i] = kInitCDGH;
  }
  for (int p = 0; p < 16; ++p) {
#pragma GCC unroll 8
    for (int i = 0; i < N; ++i) msg[i] = wk[i][p];
#pragma GCC unroll 8
    for (int i = 0; i < N; ++i)
      st1[i] = _mm_sha256rnds2_epu32(st1[i], st0[i], msg[i]);
#pragma GCC unroll 8
    for (int i = 0; i < N; ++i) msg[i] = _mm_shuffle_epi32(msg[i], 0x0E);
#pragma GCC unroll 8
    for (int i = 0; i < N; ++i)
      st0[i] = _mm_sha256rnds2_epu32(st0[i], st1[i], msg[i]);
  }

  // Block 1 feed-forward. Its input state was the IV, which is a constant, so
  // nothing had to be kept live for this.
  __m128i sv0[N], sv1[N];
#pragma GCC unroll 8
  for (int i = 0; i < N; ++i) {
    sv0[i] = _mm_add_epi32(st0[i], kInitABEF);
    sv1[i] = _mm_add_epi32(st1[i], kInitCDGH);
    st0[i] = sv0[i];
    st1[i] = sv1[i];
  }

  // Phase 2b: block 2, from KW2V exactly as the shipped kernel does. Every
  // lane consumes the SAME constant here, so one register holds it for all of
  // them -- block 2 really is 2 registers per lane and nothing else.
  for (int p = 0; p < 16; ++p) {
    __m128i c = KW2V[p];
#pragma GCC unroll 8
    for (int i = 0; i < N; ++i)
      st1[i] = _mm_sha256rnds2_epu32(st1[i], st0[i], c);
    c = _mm_shuffle_epi32(c, 0x0E);
#pragma GCC unroll 8
    for (int i = 0; i < N; ++i)
      st0[i] = _mm_sha256rnds2_epu32(st0[i], st1[i], c);
  }

#pragma GCC unroll 8
  for (int i = 0; i < N; ++i) {
    hex[i] = state_to_hex(_mm_add_epi32(st0[i], sv0[i]),
                          _mm_add_epi32(st1[i], sv1[i]));
  }
}

template <int N, int SCHED_ILV, int SKIP_SCHED = 0>
double run_phase_split(int rounds, const char in[N][64], char out[N][64]) {
  Hex hex[N];
  alignas(64) __m128i wk[N][16];
  for (int i = 0; i < N; ++i) hex[i] = load_hex(in[i]);
  // Give the round-phase-only variant something valid-shaped to read.
  for (int i = 0; i < N; ++i) schedule_lane(hex[i], wk[i]);

  const auto t0 = Clock::now();
  for (int r = 0; r < rounds; ++r) group_round<N, SCHED_ILV, SKIP_SCHED>(hex, wk);
  const auto t1 = Clock::now();

  for (int i = 0; i < N; ++i) store_hex(hex[i], out[i]);
  return std::chrono::duration<double>(t1 - t0).count();
}

// --- rep machinery ----------------------------------------------------------

struct Row {
  const char* label;
  int lanes;
  double ns_best;
  double ns_worst;
};

std::vector<Row> g_rows;

double ns_per_rnds2(double secs, int rounds, int lanes) {
  // 64 rnds2 per chain round: 32 in block 1, 32 in block 2.
  return secs * 1e9 / (static_cast<double>(rounds) * 64.0 * lanes);
}

}  // namespace

int main() {
  if (!__builtin_cpu_supports("sha")) {
    std::printf("bench_phase_split: no SHA-NI on this CPU.\n");
    return 0;
  }
  init_constants();
  obsidio::init_risk_backend();
  std::printf("back end: %s (lane width %d)\n", obsidio::risk_backend_name(),
              obsidio::risk_lane_width());

  // --- correctness, before any timing --------------------------------------
  // Round 1 of risk_hash consumes the raw seed; every round after that
  // consumes the previous 64-char hex output, which is what the skeleton does.
  // So risk_hash(seed, 1) is the skeleton's starting state and
  // risk_hash(seed, 1 + R) is what it must produce after R rounds.
  const char* seeds[kMaxLanes] = {"0.11", "0.22", "0.33", "0.44", "0.55", "0.66"};
  char start[kMaxLanes][64];
  std::string expect[kMaxLanes];
  for (int i = 0; i < kMaxLanes; ++i) {
    const std::string s1 = obsidio::risk_hash(seeds[i], 1);
    std::memcpy(start[i], s1.data(), 64);
    expect[i] = obsidio::risk_hash(seeds[i], 1 + kVerifyRounds);
  }

  int failures = 0;
  {
    char got[kMaxLanes][64];
#define VERIFY(N, ILV)                                                        \
  do {                                                                        \
    char in[N][64], outb[N][64];                                              \
    for (int i = 0; i < N; ++i) std::memcpy(in[i], start[i], 64);             \
    run_phase_split<N, ILV>(kVerifyRounds, in, outb);                         \
    for (int i = 0; i < N; ++i) {                                             \
      if (std::memcmp(outb[i], expect[i].data(), 64) != 0) {                  \
        std::printf("DIGEST MISMATCH N=%d ilv=%d lane %d\n", N, ILV, i);      \
        std::printf("  want %.64s\n  got  %.64s\n", expect[i].data(),         \
                    outb[i]);                                                 \
        ++failures;                                                           \
      }                                                                       \
    }                                                                         \
  } while (0)
    (void)got;
    VERIFY(2, 1); VERIFY(3, 1); VERIFY(4, 1); VERIFY(6, 1);
    VERIFY(2, 2); VERIFY(4, 2); VERIFY(6, 2);
#undef VERIFY
  }
  if (failures != 0) {
    std::printf("\n%d digest failures -- refusing to time a wrong answer.\n",
                failures);
    return 1;
  }
  std::printf("digest check: every configuration matches risk_hash() over %d rounds\n\n",
              kVerifyRounds);

  // --- the shipped kernel, same process, same clock -------------------------
  std::vector<double> ship;
  for (int r = 0; r < kReps; ++r) {
    std::string oa, ob;
    const auto t0 = Clock::now();
    obsidio::risk_hash_x2(seeds[0], seeds[1], oa, ob, kTimeRounds);
    const auto t1 = Clock::now();
    ship.push_back(
        ns_per_rnds2(std::chrono::duration<double>(t1 - t0).count(), kTimeRounds, 2));
  }
  std::sort(ship.begin(), ship.end());
  const double ship_best = ship.front(), ship_worst = ship.back();

  std::printf("shipped x2 (in this process): best %.4f  worst %.4f ns/rnds2\n",
              ship_best, ship_worst);
  std::printf("reference from the last session: 0.4878 ns/rnds2\n\n");

  std::printf("  %-28s %5s %10s %10s %10s\n", "phase-split skeleton", "N",
              "ns best", "ns worst", "vs ship");
  std::printf("  ------------------------------------------------------------------\n");

#define TIME(N, ILV, LABEL)                                                   \
  do {                                                                        \
    char in[N][64], outb[N][64];                                              \
    std::vector<double> v;                                                    \
    for (int r = 0; r < kReps; ++r) {                                         \
      for (int i = 0; i < N; ++i) std::memcpy(in[i], start[i], 64);           \
      v.push_back(ns_per_rnds2(run_phase_split<N, ILV>(kTimeRounds, in, outb), \
                               kTimeRounds, N));                              \
    }                                                                         \
    std::sort(v.begin(), v.end());                                            \
    g_rows.push_back(Row{LABEL, N, v.front(), v.back()});                     \
    std::printf("  %-28s %5d %10.4f %10.4f %9.3fx\n", LABEL, N, v.front(),    \
                v.back(), ship_worst / v.back());                             \
  } while (0)

  TIME(2, 1, "schedule 1 lane at a time");
  TIME(3, 1, "schedule 1 lane at a time");
  TIME(4, 1, "schedule 1 lane at a time");
  TIME(6, 1, "schedule 1 lane at a time");
  TIME(2, 2, "schedule 2 lanes interleaved");
  TIME(4, 2, "schedule 2 lanes interleaved");
  TIME(6, 2, "schedule 2 lanes interleaved");
#undef TIME

  // --- gate ------------------------------------------------------------------
  double best_gate = 1e9;
  int best_n = 0;
  const char* best_label = "";
  for (const Row& r : g_rows) {
    if ((r.lanes == 4 || r.lanes == 6) && r.ns_worst < best_gate) {
      best_gate = r.ns_worst;
      best_n = r.lanes;
      best_label = r.label;
    }
  }

  std::printf("\nControl: N=2 must land near the shipped 0.4878, or the\n");
  std::printf("skeleton itself is wrong rather than the design being bad.\n");
  for (const Row& r : g_rows) {
    if (r.lanes == 2) {
      std::printf("  N=2 %-28s worst %.4f  (%.1f%% of shipped worst)\n", r.label,
                  r.ns_worst, 100.0 * r.ns_worst / ship_worst);
    }
  }

  std::printf("\nGate A  (worst-rep N=4 or N=6 <= 0.39 ns/rnds2):\n");
  std::printf("  best qualifying: N=%d %s, worst rep %.4f ns  ->  %s\n", best_n,
              best_label, best_gate, best_gate <= 0.39 ? "PASS" : "FAIL");

  const double r_speed = ship_worst / best_gate;
  const double score = 100.0 / (91.0 / r_speed + 9.0);
  std::printf("\n  Against the shipped kernel measured in THIS process\n");
  std::printf("  (%.4f ns worst), that is a risk-path speedup of %.3fx.\n",
              ship_worst, r_speed);
  std::printf("  91/9 CPU split -> projected work_score %+.1f%%\n",
              (score - 1.0) * 100.0);

  // --- decomposition of the joint, for the write-up -------------------------
  std::printf("\nWhere the time goes. The round phase re-run against a stale\n");
  std::printf("buffer, so the schedule is the difference. These digests are\n");
  std::printf("garbage on purpose and no gate is computed from them -- this\n");
  std::printf("explains a measurement already taken, it does not predict one.\n\n");
  std::printf("  %5s %12s %12s %12s %8s\n", "N", "full ns", "rounds-only",
              "schedule ns", "sched %");
#define DECOMP(N)                                                             \
  do {                                                                        \
    char in[N][64], outb[N][64];                                              \
    std::vector<double> full, ronly;                                          \
    for (int r = 0; r < kReps; ++r) {                                         \
      for (int i = 0; i < N; ++i) std::memcpy(in[i], start[i], 64);           \
      full.push_back(ns_per_rnds2(run_phase_split<N, 1, 0>(kTimeRounds, in, outb), \
                                  kTimeRounds, N));                           \
      for (int i = 0; i < N; ++i) std::memcpy(in[i], start[i], 64);           \
      ronly.push_back(ns_per_rnds2(run_phase_split<N, 1, 1>(kTimeRounds, in, outb), \
                                   kTimeRounds, N));                          \
    }                                                                         \
    std::sort(full.begin(), full.end());                                      \
    std::sort(ronly.begin(), ronly.end());                                    \
    const double f = full.back(), o = ronly.back();                           \
    std::printf("  %5d %12.4f %12.4f %12.4f %7.1f%%\n", N, f, o, f - o,       \
                100.0 * (f - o) / f);                                         \
  } while (0)
  DECOMP(2); DECOMP(4); DECOMP(6);
#undef DECOMP
  return 0;
}
#endif
