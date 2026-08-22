// Probes B and C: the two cheap assumptions the phase-split kernel rests on.
//
// Stage 2 (see docs/phase-split-kernel.md) wants to expand each lane's message
// schedule into an L1-resident W+K buffer first, then run a round phase where a
// lane costs only 2 registers because its round constants come from that buffer
// instead of from four live schedule registers. Two things about that were
// asserted and never measured:
//
//   B. The round phase adds one memory operand per round constant. Loads issue
//      on different ports from the SHA unit, so this SHOULD be free -- but
//      rnds2_ports.cpp measured the bare instruction with the constant in a
//      register, which is not what will ship.
//
//   C. The schedule work has to happen somewhere. The claim is that ~1.5
//      128-bit vector ops per rnds2 fit in the spare issue capacity that
//      avx_transition.cpp found (four 128-bit ops alongside rnds2 cost 0-11%).
//
// Everything here is 128-bit. sha256rnds2 has no VEX encoding, so anything
// VEX-256 anywhere near it pays an AVX/SSE transition penalty of ~98% -- see
// avx_transition.cpp. That is a property of the instruction, not of this box.
//
// Method, unchanged from rnds2_ports.cpp: N independent dependency chains, the
// headline is a ratio taken inside one run so a sagging laptop clock scales
// both halves equally, and the WORST rep is what gets quoted. An argmax over a
// noisy plateau is not a measurement.
//
// Build: not part of the server. See bench/ryzen/README.md.
#include <cstdint>
#include <cstdio>
#include <cstring>

#if !defined(__x86_64__)
int main() {
  std::printf("stage2_probes: x86-64 only; this machine is a different ISA.\n");
  return 0;
}
#else

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace {

// 16 pairs = 32 rnds2 = one SHA-256 block, which is the unit the real kernel
// works in. Iterations are counted in blocks so every arm does the same rnds2
// count regardless of how its inner loop is shaped.
// Sized so one rep is ~50-100 ms at every lane count. Short reps are the
// classic way to measure the timer instead of the CPU: at 12,500 blocks a
// 4-lane rep finished in 0.4 ms, which is noise with a benchmark wrapped
// around it.
constexpr int kBlocks = 2000000;
constexpr int kReps = 7;  // odd, so the median is a real sample

using Clock = std::chrono::steady_clock;

std::uint32_t g_sink = 0;
inline void consume(__m128i v) {
  alignas(16) std::uint32_t tmp[4];
  _mm_store_si128(reinterpret_cast<__m128i*>(tmp), v);
  g_sink ^= tmp[0] ^ tmp[1] ^ tmp[2] ^ tmp[3];
}

constexpr int kMaxLanes = 6;

// Per-lane W+K buffer: 16 vectors = 64 words = one block's round constants.
// N lanes at 256 bytes each is 1.5 KB at N=6, comfortably L1-resident, which
// is the point -- this probe is about the load PORT, not about cache misses.
alignas(64) __m128i g_wk[kMaxLanes][16];

void init_buffers() {
  // Values are irrelevant to timing; what matters is that the compiler cannot
  // constant-fold them. Filled from a runtime-visible counter.
  for (int lane = 0; lane < kMaxLanes; ++lane) {
    for (int i = 0; i < 16; ++i) {
      const int b = lane * 16 + i;
      g_wk[lane][i] = _mm_setr_epi32(0x428a2f98 + b, 0x71374491 + b,
                                     0xb5c0fbcf + b, 0xe9b5dba5 + b);
    }
  }
}

// Stops the optimiser proving anything about g_wk's contents, so the loads
// below cannot be hoisted out of the timed loop and folded into registers.
// Called once, outside the timing.
inline void launder_buffers() {
  __asm__ __volatile__("" : : "r"(&g_wk[0][0]) : "memory");
}

// --- the control ------------------------------------------------------------
// Round constant lives in a register. This is exactly rnds2_ports.cpp's
// run_chains, repeated here so every number on the table comes out of one
// binary, one clock and one thermal state.
template <int N>
double run_bare(int blocks) {
  __m128i s0[N], s1[N];
  const __m128i k = _mm_setr_epi32(0x428a2f98, 0x71374491, 0xb5c0fbcf,
                                   0xe9b5dba5);
  for (int i = 0; i < N; ++i) {
    s0[i] = _mm_setr_epi32(0x6a09e667 + i, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a);
    s1[i] = _mm_setr_epi32(0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 + i);
  }

  const auto t0 = Clock::now();
  for (int b = 0; b < blocks; ++b) {
    for (int p = 0; p < 16; ++p) {
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], k);
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], k);
    }
  }
  const auto t1 = Clock::now();

  for (int i = 0; i < N; ++i) consume(_mm_xor_si128(s0[i], s1[i]));
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  return static_cast<double>(blocks) * 32.0 * N / secs;  // rnds2 per second
}

// --- Probe B, gate arm ------------------------------------------------------
// One 16-byte L1 load per rnds2. This is the literal form Gate B is written
// against, and it is PESSIMISTIC by a factor of two against what will ship --
// see run_load_per_pair below.
template <int N>
double run_load_per_rnds2(int blocks) {
  __m128i s0[N], s1[N];
  for (int i = 0; i < N; ++i) {
    s0[i] = _mm_setr_epi32(0x6a09e667 + i, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a);
    s1[i] = _mm_setr_epi32(0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 + i);
  }

  const auto t0 = Clock::now();
  for (int b = 0; b < blocks; ++b) {
    for (int p = 0; p < 16; ++p) {
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) {
        s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], g_wk[i][p]);
      }
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) {
        s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], g_wk[i][15 - p]);
      }
    }
  }
  const auto t1 = Clock::now();

  for (int i = 0; i < N; ++i) consume(_mm_xor_si128(s0[i], s1[i]));
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  return static_cast<double>(blocks) * 32.0 * N / secs;
}

// --- Probe B, shipped-shape arm --------------------------------------------
// What the kernel will actually do. A 128-bit W+K vector carries FOUR words,
// which is two rnds2 worth: load once, feed the low half, shuffle down, feed
// the high half. So the real cost is one load and one shuffle per TWO rnds2,
// exactly as compress_generic's RNDS2_PAIR does today with a register operand.
template <int N>
double run_load_per_pair(int blocks) {
  __m128i s0[N], s1[N], m[N];
  for (int i = 0; i < N; ++i) {
    s0[i] = _mm_setr_epi32(0x6a09e667 + i, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a);
    s1[i] = _mm_setr_epi32(0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 + i);
  }

  const auto t0 = Clock::now();
  for (int b = 0; b < blocks; ++b) {
    for (int p = 0; p < 16; ++p) {
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) m[i] = g_wk[i][p];
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], m[i]);
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) m[i] = _mm_shuffle_epi32(m[i], 0x0E);
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], m[i]);
    }
  }
  const auto t1 = Clock::now();

  for (int i = 0; i < N; ++i) consume(_mm_xor_si128(s0[i], s1[i]));
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  return static_cast<double>(blocks) * 32.0 * N / secs;
}

// --- Probe C, the real mix co-issued ---------------------------------------
// The genuine schedule group from compress_generic: alignr, add, sha256msg2,
// sha256msg1 on a rotating four-register message set. One group per rnds2 PAIR
// per lane is 4 ops per 2 rnds2 in the steady state; across a whole block the
// real sequence averages 48 ops per 32 rnds2 = the 1.5 per rnds2 the plan
// quotes, because the first four pairs and the last two are lighter.
//
// This runs N schedule streams alongside N round lanes, which is what "hide
// the schedule behind the rounds" would literally require. It is also 5
// registers per stream on top of 2 per lane, so at N=4 it wants 28 registers
// in a file of 16. Whether that spills is exactly the question.
template <int N>
double run_sched_coissue(int blocks) {
  __m128i s0[N], s1[N];
  __m128i m0[N], m1[N], m2[N], m3[N];
  const __m128i k = _mm_setr_epi32(0x428a2f98, 0x71374491, 0xb5c0fbcf,
                                   0xe9b5dba5);
  for (int i = 0; i < N; ++i) {
    s0[i] = _mm_setr_epi32(0x6a09e667 + i, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a);
    s1[i] = _mm_setr_epi32(0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 + i);
    m0[i] = g_wk[i][0];
    m1[i] = g_wk[i][1];
    m2[i] = g_wk[i][2];
    m3[i] = g_wk[i][3];
  }

  const auto t0 = Clock::now();
  for (int b = 0; b < blocks; ++b) {
    for (int p = 0; p < 16; ++p) {
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], k);
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], k);
      // Four real schedule ops per lane per pair, with the real dependency
      // shape: the msg1 feeds the msg2 that produces the next round's words.
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) {
        const __m128i t = _mm_alignr_epi8(m3[i], m2[i], 4);
        m0[i] = _mm_sha256msg1_epu32(m0[i], m1[i]);
        m0[i] = _mm_add_epi32(m0[i], t);
        m0[i] = _mm_sha256msg2_epu32(m0[i], m3[i]);
        const __m128i rot = m0[i];
        m0[i] = m1[i];
        m1[i] = m2[i];
        m2[i] = m3[i];
        m3[i] = rot;
      }
    }
  }
  const auto t1 = Clock::now();

  for (int i = 0; i < N; ++i) {
    consume(_mm_xor_si128(s0[i], s1[i]));
    consume(_mm_xor_si128(m0[i], m3[i]));
  }
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  return static_cast<double>(blocks) * 32.0 * N / secs;
}

// --- Probe C, the port question isolated ------------------------------------
// run_sched_coissue above gives every lane its own schedule stream, which is
// what Stage 2 would need but costs 5 registers per lane on top of 2 -- 28 at
// four lanes, in a file of 16. So it measures register pressure and issue
// pressure fused together, and a failure there does not say which.
//
// This separates them. N round lanes, but only STREAMS schedule streams shared
// across the whole loop, so the register bill is 2N + 5*STREAMS and stays
// inside the file. Sweeping STREAMS at fixed N moves the schedule op RATE
// without moving the register pressure much, which is exactly what is needed
// to answer: do sha256msg1/msg2 contend with sha256rnds2 for the same issue
// port, or do they slot in beside it the way avx_transition.cpp's 128-bit adds
// did?
//
// Writes the schedule-op rate through sched_ops_out and returns the rnds2
// rate, so the caller can add them: if the SUM is invariant as STREAMS rises,
// the two instruction families are sharing one unit and the "schedule hides
// behind the rounds" premise is dead on ports alone.
template <int N, int STREAMS>
double run_sched_shared(int blocks, double* sched_ops_out) {
  __m128i s0[N], s1[N];
  __m128i m0[STREAMS > 0 ? STREAMS : 1], m1[STREAMS > 0 ? STREAMS : 1];
  __m128i m2[STREAMS > 0 ? STREAMS : 1], m3[STREAMS > 0 ? STREAMS : 1];
  const __m128i k = _mm_setr_epi32(0x428a2f98, 0x71374491, 0xb5c0fbcf,
                                   0xe9b5dba5);
  for (int i = 0; i < N; ++i) {
    s0[i] = _mm_setr_epi32(0x6a09e667 + i, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a);
    s1[i] = _mm_setr_epi32(0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 + i);
  }
  for (int i = 0; i < STREAMS; ++i) {
    m0[i] = g_wk[i][0];
    m1[i] = g_wk[i][1];
    m2[i] = g_wk[i][2];
    m3[i] = g_wk[i][3];
  }

  const auto t0 = Clock::now();
  for (int b = 0; b < blocks; ++b) {
    for (int p = 0; p < 16; ++p) {
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], k);
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], k);
#pragma GCC unroll 8
      for (int i = 0; i < STREAMS; ++i) {
        const __m128i t = _mm_alignr_epi8(m3[i], m2[i], 4);
        m0[i] = _mm_sha256msg1_epu32(m0[i], m1[i]);
        m0[i] = _mm_add_epi32(m0[i], t);
        m0[i] = _mm_sha256msg2_epu32(m0[i], m3[i]);
        const __m128i rot = m0[i];
        m0[i] = m1[i];
        m1[i] = m2[i];
        m2[i] = m3[i];
        m3[i] = rot;
      }
    }
  }
  const auto t1 = Clock::now();

  for (int i = 0; i < N; ++i) consume(_mm_xor_si128(s0[i], s1[i]));
  for (int i = 0; i < STREAMS; ++i) consume(_mm_xor_si128(m0[i], m3[i]));
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  // Only sha256msg1 and sha256msg2 are SHA-unit ops; alignr and add are
  // ordinary vector ops on other ports. Count the two that matter.
  *sched_ops_out = static_cast<double>(blocks) * 16.0 * 2.0 * STREAMS / secs;
  return static_cast<double>(blocks) * 32.0 * N / secs;
}

// --- Probe C, diagnostic: the schedule phase on its own ---------------------
// Stage 2 does NOT co-issue: it runs the schedule as a separate phase with no
// chain state live. This measures that phase alone, N lanes interleaved, so
// the cost has a number even though it must not be added to a round-phase
// number to make a projection. (Joining separately-measured halves across a
// boundary is precisely what killed Stage 1.) Reported as ops/s, and as the
// wall time a block's worth of schedule costs per lane.
template <int N>
double run_sched_only(int blocks) {
  __m128i m0[N], m1[N], m2[N], m3[N];
  for (int i = 0; i < N; ++i) {
    m0[i] = g_wk[i][0];
    m1[i] = g_wk[i][1];
    m2[i] = g_wk[i][2];
    m3[i] = g_wk[i][3];
  }

  const auto t0 = Clock::now();
  for (int b = 0; b < blocks; ++b) {
    for (int p = 0; p < 16; ++p) {
#pragma GCC unroll 8
      for (int i = 0; i < N; ++i) {
        const __m128i t = _mm_alignr_epi8(m3[i], m2[i], 4);
        m0[i] = _mm_sha256msg1_epu32(m0[i], m1[i]);
        m0[i] = _mm_add_epi32(m0[i], t);
        m0[i] = _mm_sha256msg2_epu32(m0[i], m3[i]);
        const __m128i rot = m0[i];
        m0[i] = m1[i];
        m1[i] = m2[i];
        m2[i] = m3[i];
        m3[i] = rot;
      }
    }
  }
  const auto t1 = Clock::now();

  for (int i = 0; i < N; ++i) consume(_mm_xor_si128(m0[i], m3[i]));
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  // 4 ops per pair per lane, 16 pairs per block.
  return static_cast<double>(blocks) * 64.0 * N / secs;  // schedule ops/s
}

// --- rep machinery ----------------------------------------------------------

struct Result {
  double best;   // fastest rep
  double worst;  // slowest rep -- this is what gets quoted
};

template <typename F>
Result reps(F fn) {
  fn(kBlocks / 20);  // warm caches and branch predictors, discard
  std::vector<double> v;
  v.reserve(kReps);
  for (int r = 0; r < kReps; ++r) v.push_back(fn(kBlocks));
  std::sort(v.begin(), v.end());
  return Result{v.back(), v.front()};
}

double ns_per(double rate) { return rate > 0.0 ? 1e9 / rate : 0.0; }

const int kLaneSet[3] = {2, 4, 6};

template <int N>
void row(const char* label, Result r) {
  std::printf("  %-26s %d   %8.2f   %8.2f   %8.4f   %8.4f\n", label, N,
              r.best / 1e6, r.worst / 1e6, ns_per(r.best), ns_per(r.worst));
}

}  // namespace

int main() {
  if (!__builtin_cpu_supports("sha")) {
    std::printf(
        "stage2_probes: this CPU does not advertise SHA-NI, so there is\n"
        "nothing to measure here.\n");
    return 0;
  }

  init_buffers();
  launder_buffers();

  std::printf("Stage 2 probes B and C -- %d blocks x 32 rnds2 per lane, %d reps\n",
              kBlocks, kReps);
  std::printf("All arms 128-bit. Worst rep is the one to quote.\n\n");

  std::printf("  %-26s %s   %8s   %8s   %8s   %8s\n", "arm", "N", "best M/s",
              "worst", "ns best", "ns worst");
  std::printf("  ---------------------------------------------------------------------------------\n");

  // Probe B ------------------------------------------------------------------
  Result bare2 = reps([](int b) { return run_bare<2>(b); });
  Result bare4 = reps([](int b) { return run_bare<4>(b); });
  Result bare6 = reps([](int b) { return run_bare<6>(b); });
  row<2>("bare (register W+K)", bare2);
  row<4>("bare (register W+K)", bare4);
  row<6>("bare (register W+K)", bare6);

  Result l1_2 = reps([](int b) { return run_load_per_rnds2<2>(b); });
  Result l1_4 = reps([](int b) { return run_load_per_rnds2<4>(b); });
  Result l1_6 = reps([](int b) { return run_load_per_rnds2<6>(b); });
  row<2>("B: load per rnds2", l1_2);
  row<4>("B: load per rnds2", l1_4);
  row<6>("B: load per rnds2", l1_6);

  Result lp_2 = reps([](int b) { return run_load_per_pair<2>(b); });
  Result lp_4 = reps([](int b) { return run_load_per_pair<4>(b); });
  Result lp_6 = reps([](int b) { return run_load_per_pair<6>(b); });
  row<2>("B: load per pair (real)", lp_2);
  row<4>("B: load per pair (real)", lp_4);
  row<6>("B: load per pair (real)", lp_6);

  // Probe C ------------------------------------------------------------------
  Result c2 = reps([](int b) { return run_sched_coissue<2>(b); });
  Result c4 = reps([](int b) { return run_sched_coissue<4>(b); });
  Result c6 = reps([](int b) { return run_sched_coissue<6>(b); });
  row<2>("C: schedule co-issued", c2);
  row<4>("C: schedule co-issued", c4);
  row<6>("C: schedule co-issued", c6);

  std::printf("\n");

  // Gate B -------------------------------------------------------------------
  const double gate_b = ns_per(l1_4.worst);
  std::printf("Gate B  (worst-rep 4-lane, one load per rnds2, <= 0.30 ns):\n");
  std::printf("  measured %.4f ns/rnds2   ->   %s\n", gate_b,
              gate_b <= 0.30 ? "PASS" : "FAIL");
  std::printf("  bare 4-lane worst is %.4f ns; the load costs %+.1f%%\n",
              ns_per(bare4.worst), 100.0 * (bare4.worst / l1_4.worst - 1.0));
  std::printf("  shipped shape (one load per PAIR) is %.4f ns, %+.1f%% vs bare\n",
              ns_per(lp_4.worst),
              100.0 * (bare4.worst / lp_4.worst - 1.0));

  // Gate C -------------------------------------------------------------------
  const double cost_c = 100.0 * (bare4.worst / c4.worst - 1.0);
  std::printf("\nGate C  (schedule co-issue cost at 4 lanes, worst rep, <= 15%%):\n");
  std::printf("  bare %.4f ns  ->  co-issued %.4f ns   cost %+.1f%%   ->   %s\n",
              ns_per(bare4.worst), ns_per(c4.worst), cost_c,
              cost_c <= 15.0 ? "PASS" : "FAIL");

  // Probe C, port question isolated -----------------------------------------
  std::printf("\nIs the cost above register pressure or issue contention?\n");
  std::printf("Four round lanes, schedule streams added one at a time. Only\n");
  std::printf("sha256msg1/msg2 are counted as SHA-unit ops (alignr and add are\n");
  std::printf("ordinary vector ops). If the TOTAL column is flat, msg1/msg2 and\n");
  std::printf("rnds2 are contending for one unit and no amount of restructuring\n");
  std::printf("buys the schedule a free ride.\n\n");
  std::printf("  %-8s %10s %10s %10s   %s\n", "streams", "rnds2 M/s", "sched M/s",
              "TOTAL M/s", "regs");
  {
    double sched = 0.0;
    Result r0 = reps([&sched](int b) { return run_sched_shared<4, 0>(b, &sched); });
    std::printf("  %-8d %10.1f %10.1f %10.1f   %d\n", 0, r0.worst / 1e6, 0.0,
                r0.worst / 1e6, 8);

    double s1v = 0.0, s2v = 0.0, s3v = 0.0;
    Result r1 = reps([&s1v](int b) { return run_sched_shared<4, 1>(b, &s1v); });
    std::printf("  %-8d %10.1f %10.1f %10.1f   %d\n", 1, r1.worst / 1e6,
                s1v / 1e6, (r1.worst + s1v) / 1e6, 13);
    Result r2 = reps([&s2v](int b) { return run_sched_shared<4, 2>(b, &s2v); });
    std::printf("  %-8d %10.1f %10.1f %10.1f   %d\n", 2, r2.worst / 1e6,
                s2v / 1e6, (r2.worst + s2v) / 1e6, 18);
    Result r3 = reps([&s3v](int b) { return run_sched_shared<4, 3>(b, &s3v); });
    std::printf("  %-8d %10.1f %10.1f %10.1f   %d\n", 3, r3.worst / 1e6,
                s3v / 1e6, (r3.worst + s3v) / 1e6, 23);

    // Stage 2 needs 1.5 schedule ops per block-1 rnds2, of which 1.0 are
    // msg1/msg2 -- so per rnds2 across a whole round (block 2 has no schedule
    // at all) it needs 0.5 SHA-unit schedule ops per rnds2.
    // Count the real sequence rather than the synthetic one this arm runs.
    // compress_generic's schedule is 12 sha256msg1 + 12 sha256msg2 per block 1
    // -- confirmed by objdump on bench_phase_split's inlined schedule_lane --
    // so 24 SHA-unit ops against block 1's 32 rnds2, and block 2 needs none at
    // all. Over a whole 64-rnds2 round that is 24/64 = 0.375 per rnds2.
    //
    // This line previously said 0.5, from assuming one msg op per block-1
    // rnds2 instead of the actual 0.75. It overstated the requirement by 4/3
    // and the error reached a design note before review caught it.
    constexpr double kMsgOpsPerRnds2 = 24.0 / 64.0;
    std::printf("\n  Stage 2's requirement: 12 sha256msg1 + 12 sha256msg2 per\n");
    std::printf("  block 1 (32 rnds2), none in block 2 -- so %.3f SHA-unit\n",
                kMsgOpsPerRnds2);
    std::printf("  schedule ops per rnds2 over a whole round.\n");
    std::printf("  At the 4-lane bare rate of %.0f M rnds2/s that is %.0f M sched/s.\n",
                bare4.worst / 1e6, kMsgOpsPerRnds2 * bare4.worst / 1e6);
    std::printf("  NOTE: the streams above run a synthetic 4-ops-per-pair group,\n");
    std::printf("  which is 4/3 denser in msg ops than the real sequence.\n");
  }

  // Diagnostic ---------------------------------------------------------------
  std::printf("\nDiagnostic -- the schedule phase measured ALONE (not co-issued).\n");
  std::printf("Stage 2 runs this as a separate phase with no chain state live.\n");
  std::printf("Do NOT add this to a round-phase number to build a projection:\n");
  std::printf("joining separately-measured halves across a phase boundary is\n");
  std::printf("what killed Stage 1. Probe A measures the joint.\n");
  for (int idx = 0; idx < 3; ++idx) {
    const int n = kLaneSet[idx];
    Result s;
    if (n == 2) s = reps([](int b) { return run_sched_only<2>(b); });
    else if (n == 4) s = reps([](int b) { return run_sched_only<4>(b); });
    else s = reps([](int b) { return run_sched_only<6>(b); });
    std::printf("  schedule only, N=%d: %8.2f M ops/s best, %8.2f worst\n", n,
                s.best / 1e6, s.worst / 1e6);
  }

  std::printf("\n(sink %u -- ignore, it exists to defeat dead-code removal)\n",
              g_sink);
  return 0;
}
#endif
