// The measurement the whole x86 kernel design rests on, and which has never
// been made.
//
// The current kernel keeps TWO chains resident and treats that as the ceiling,
// on a register argument: a lane needs ~6 XMM registers during block 1, and 16
// registers minus SHA256RNDS2's implicit XMM0 leaves room for two. That
// argument is about *registers*. Whether more lanes would help at all is a
// different question, about the instruction's latency and throughput, and the
// answer decides whether the kernel is finished or has a 2x left in it:
//
//   Let L = SHA256RNDS2 latency, T = its reciprocal throughput (cycles between
//   independent issues). One lane runs 32 dependent rnds2 per block, so it
//   takes 32*L cycles no matter how idle the machine is. Lanes run
//   concurrently, so N lanes take max(32*L, 32*N*T) cycles for N blocks.
//
//   Below N = L/T the kernel is LATENCY bound and every extra lane is free
//   throughput. At and above it the kernel is PORT bound and extra lanes buy
//   nothing but register pressure.
//
//   The measured 73 cycles/block at two lanes is consistent with BOTH
//   (L=4, T=2) and (L=4, T=1) -- the two cases differ only in whether four
//   lanes would halve it. That is the ambiguity this resolves.
//
// The headline number is frequency independent, which matters on a laptop that
// throttles: it is a RATIO of throughputs measured inside one run, so a clock
// that sags mid-benchmark scales both halves equally.
//
//   lanes_to_saturate = throughput(plateau) / throughput(N=1) = L/T
//
// If that comes out ~2, the two-lane kernel is optimal and the SHA work is
// done. If it comes out ~4, a four-lane kernel is worth roughly 2x on the risk
// path, and the register problem becomes worth solving (by splitting the
// message schedule out of the round phase, so a lane costs 2 registers during
// the rounds instead of 6).
//
// Test 3 answers a second dead question: whether an AVX2 multi-buffer hash
// could run *alongside* SHA-NI rather than instead of it. That idea was killed
// on the assumption that SHA-NI saturates its ports; if it is latency bound
// there are spare ports, and the hybrid reopens.
//
// Build: this is not part of the server. See bench/x86/README.md.
#include <cstdint>
#include <cstdio>
#include <cstring>

#if !defined(__x86_64__)
int main() {
  std::printf("rnds2_ports: x86-64 only; this machine is a different ISA.\n");
  return 0;
}
#else

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace {

constexpr int kIters = 200000;   // dependent rnds2 per chain, per repetition
constexpr int kReps = 7;         // odd, so the median is a real sample

using Clock = std::chrono::steady_clock;

// Sink that the optimiser cannot see through, so nothing below is dead code.
std::uint32_t g_sink = 0;
inline void consume(__m128i v) {
  alignas(16) std::uint32_t tmp[4];
  _mm_store_si128(reinterpret_cast<__m128i*>(tmp), v);
  g_sink ^= tmp[0] ^ tmp[1] ^ tmp[2] ^ tmp[3];
}

// N independent rnds2 dependency chains, fully unrolled by the template so the
// states stay in registers. Each chain is strictly serial: state[i] feeds
// itself, so one chain can never exceed 1/L. Chains are independent of each
// other, so together they can only be limited by the port.
template <int N>
double run_chains(int iters) {
  __m128i s0[N], s1[N];
  const __m128i k = _mm_setr_epi32(0x428a2f98, 0x71374491, 0xb5c0fbcf,
                                   0xe9b5dba5);
  for (int i = 0; i < N; ++i) {
    s0[i] = _mm_setr_epi32(0x6a09e667 + i, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a);
    s1[i] = _mm_setr_epi32(0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 + i);
  }

  const auto t0 = Clock::now();
  for (int r = 0; r < iters; ++r) {
#pragma GCC unroll 8
    for (int i = 0; i < N; ++i) {
      s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], k);
    }
  }
  const auto t1 = Clock::now();

  for (int i = 0; i < N; ++i) consume(_mm_xor_si128(s0[i], s1[i]));
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  return static_cast<double>(iters) * N / secs;  // rnds2 per second
}

// Same, with an independent AVX2 stream co-issued. If SHA-NI is port bound the
// two fight and rnds2 throughput drops; if it is latency bound the AVX2 work
// slots into the gaps almost for free. That difference is exactly the
// feasibility of a SHA-NI + AVX2 hybrid.
template <int N>
double run_chains_with_avx2(int iters) {
  __m128i s0[N], s1[N];
  const __m128i k = _mm_setr_epi32(0x428a2f98, 0x71374491, 0xb5c0fbcf,
                                   0xe9b5dba5);
  for (int i = 0; i < N; ++i) {
    s0[i] = _mm_setr_epi32(0x6a09e667 + i, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a);
    s1[i] = _mm_setr_epi32(0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 + i);
  }
  __m256i a0 = _mm256_set1_epi32(1), a1 = _mm256_set1_epi32(2);
  __m256i a2 = _mm256_set1_epi32(3), a3 = _mm256_set1_epi32(4);
  const __m256i one = _mm256_set1_epi32(0x9e3779b9);

  const auto t0 = Clock::now();
  for (int r = 0; r < iters; ++r) {
#pragma GCC unroll 8
    for (int i = 0; i < N; ++i) {
      s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], k);
    }
    // Four independent AVX2 adds per rnds2 round: enough to compete for vector
    // issue slots without creating a dependency of its own.
    a0 = _mm256_add_epi32(a0, one);
    a1 = _mm256_add_epi32(a1, one);
    a2 = _mm256_add_epi32(a2, one);
    a3 = _mm256_add_epi32(a3, one);
  }
  const auto t1 = Clock::now();

  for (int i = 0; i < N; ++i) consume(_mm_xor_si128(s0[i], s1[i]));
  consume(_mm256_castsi256_si128(
      _mm256_xor_si256(_mm256_xor_si256(a0, a1), _mm256_xor_si256(a2, a3))));
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  return static_cast<double>(iters) * N / secs;
}

double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

template <int N>
double best_chains() {
  std::vector<double> samples;
  run_chains<N>(kIters / 10);  // warm up: clocks, page faults, branch predictor
  for (int r = 0; r < kReps; ++r) samples.push_back(run_chains<N>(kIters));
  return median_of(samples);
}

template <int N>
double best_chains_avx2() {
  std::vector<double> samples;
  run_chains_with_avx2<N>(kIters / 10);
  for (int r = 0; r < kReps; ++r) {
    samples.push_back(run_chains_with_avx2<N>(kIters));
  }
  return median_of(samples);
}

}  // namespace

int main() {
  __builtin_cpu_init();
  if (!__builtin_cpu_supports("sha")) {
    std::printf(
        "rnds2_ports: this CPU does not advertise SHA-NI, so there is nothing\n"
        "to measure here. That is itself worth recording -- it means the\n"
        "accelerated x86 kernel would never be selected on this machine.\n");
    return 0;
  }

  std::printf("SHA256RNDS2 scaling: independent dependency chains\n");
  std::printf("%d dependent rnds2 per chain, median of %d reps\n\n", kIters,
              kReps);

  double t[9] = {0};
  t[1] = best_chains<1>();
  t[2] = best_chains<2>();
  t[3] = best_chains<3>();
  t[4] = best_chains<4>();
  t[5] = best_chains<5>();
  t[6] = best_chains<6>();
  t[8] = best_chains<8>();

  std::printf("%6s  %14s  %12s  %s\n", "lanes", "Mrnds2/s", "ns/rnds2",
              "vs 1 lane");
  std::printf("--------------------------------------------------------\n");
  const int shown[] = {1, 2, 3, 4, 5, 6, 8};
  for (const int n : shown) {
    std::printf("%6d  %14.2f  %12.4f  %8.2fx\n", n, t[n] / 1e6,
                1e9 / t[n] * n / n, t[n] / t[1]);
  }

  // The plateau is where an extra lane stops buying throughput. Report the
  // first lane count within 3% of the best observed.
  double best = 0.0;
  for (const int n : shown) best = std::max(best, t[n]);
  int plateau = 8;
  for (const int n : shown) {
    if (t[n] >= 0.97 * best) {
      plateau = n;
      break;
    }
  }
  const double ratio = best / t[1];

  std::printf("\nlanes to saturate: %d   (throughput ratio %.2fx over 1 lane)\n",
              plateau, ratio);
  std::printf("\nWhat this means for the kernel:\n");
  if (plateau <= 2) {
    std::printf(
        "  PORT BOUND at two lanes. The shipped two-lane kernel is already at\n"
        "  the instruction's throughput limit -- more lanes cannot help, and\n"
        "  the register-pressure ceiling was never the real constraint. Stop\n"
        "  optimising the SHA kernel; the remaining score is elsewhere.\n");
  } else {
    std::printf(
        "  LATENCY BOUND at two lanes. %d lanes reach %.2fx the single-lane\n"
        "  rate, so the shipped kernel is leaving roughly %.0f%% on the table.\n"
        "  Worth building the phase-split kernel: compute block 1's message\n"
        "  schedule into an L1 buffer first, then run the round phase with\n"
        "  only 2 registers per lane, which fits %d+ lanes in the XMM file.\n",
        plateau, ratio, 100.0 * (best / t[2] - 1.0), plateau);
  }

  if (__builtin_cpu_supports("avx2")) {
    std::printf("\nPort contention: SHA-NI with an AVX2 stream co-issued\n");
    const double solo2 = t[2];
    const double solo4 = t[4];
    const double mix2 = best_chains_avx2<2>();
    const double mix4 = best_chains_avx2<4>();
    std::printf("  2 lanes: %8.2f -> %8.2f Mrnds2/s  (%+.1f%%)\n", solo2 / 1e6,
                mix2 / 1e6, 100.0 * (mix2 / solo2 - 1.0));
    std::printf("  4 lanes: %8.2f -> %8.2f Mrnds2/s  (%+.1f%%)\n", solo4 / 1e6,
                mix4 / 1e6, 100.0 * (mix4 / solo4 - 1.0));
    const double worst = std::min(mix2 / solo2, mix4 / solo4);
    std::printf("\n  ");
    if (worst > 0.95) {
      std::printf(
          "AVX2 work is nearly free alongside SHA-NI: the vector ports have\n"
          "  slack. A SHA-NI + AVX2 multi-buffer hybrid is worth reopening.\n");
    } else {
      std::printf(
          "AVX2 steals from SHA-NI (worst case %.0f%% of solo). They compete\n"
          "  for issue slots; the hybrid stays dead.\n",
          100.0 * worst);
    }
  } else {
    std::printf("\nPort contention test skipped: no AVX2 on this CPU.\n");
  }

  std::printf("\n(sink %u -- ignore, it exists to defeat dead-code removal)\n",
              g_sink);
  return 0;
}
#endif
