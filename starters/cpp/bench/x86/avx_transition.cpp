// Is the -98% "port contention" in bench/x86/rnds2_ports.cpp real port
// contention, or an AVX/SSE transition artifact?
//
// SHA256RNDS2 has no VEX encoding: it is legacy-SSE only, and a legacy-SSE
// write leaves the upper 128 bits of the YMM register in a "dirty upper"
// state. Interleaving it with VEX-256 AVX2 in the same loop is the textbook
// way to pay a transition penalty every iteration. If that is what is being
// measured, the hybrid was closed on an artifact.
//
// Four arms, same rnds2 work in each:
//   solo    -- rnds2 alone
//   sse128  -- rnds2 + four 128-bit SSE adds   (no 256-bit state, no penalty)
//   avx256  -- rnds2 + four 256-bit AVX2 adds  (what the harness does)
//   avx_vz  -- same, with an explicit vzeroupper before re-entering rnds2
//
// If sse128 is ~free and avx256 collapses, the collapse is the transition,
// not the issue ports.
#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kIters = 200000;
constexpr int kReps = 7;
constexpr int N = 2;  // the lane count the harness reports at

using Clock = std::chrono::steady_clock;

std::uint32_t g_sink = 0;
inline void consume(__m128i v) {
  alignas(16) std::uint32_t tmp[4];
  _mm_store_si128(reinterpret_cast<__m128i*>(tmp), v);
  g_sink ^= tmp[0] ^ tmp[1] ^ tmp[2] ^ tmp[3];
}

enum class Mode { kSolo, kSse128, kAvx256, kAvxVzero };

template <Mode M>
double run(int iters) {
  __m128i s0[N], s1[N];
  const __m128i k =
      _mm_setr_epi32(0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5);
  for (int i = 0; i < N; ++i) {
    s0[i] = _mm_setr_epi32(0x6a09e667 + i, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a);
    s1[i] = _mm_setr_epi32(0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 + i);
  }
  __m128i b0 = _mm_set1_epi32(1), b1 = _mm_set1_epi32(2);
  __m128i b2 = _mm_set1_epi32(3), b3 = _mm_set1_epi32(4);
  const __m128i bone = _mm_set1_epi32(0x9e3779b9);
  __m256i a0 = _mm256_set1_epi32(1), a1 = _mm256_set1_epi32(2);
  __m256i a2 = _mm256_set1_epi32(3), a3 = _mm256_set1_epi32(4);
  const __m256i aone = _mm256_set1_epi32(0x9e3779b9);

  const auto t0 = Clock::now();
  for (int r = 0; r < iters; ++r) {
    for (int i = 0; i < N; ++i) s0[i] = _mm_sha256rnds2_epu32(s0[i], s1[i], k);
    if constexpr (M == Mode::kSse128) {
      b0 = _mm_add_epi32(b0, bone);
      b1 = _mm_add_epi32(b1, bone);
      b2 = _mm_add_epi32(b2, bone);
      b3 = _mm_add_epi32(b3, bone);
    } else if constexpr (M == Mode::kAvx256 || M == Mode::kAvxVzero) {
      a0 = _mm256_add_epi32(a0, aone);
      a1 = _mm256_add_epi32(a1, aone);
      a2 = _mm256_add_epi32(a2, aone);
      a3 = _mm256_add_epi32(a3, aone);
      if constexpr (M == Mode::kAvxVzero) _mm256_zeroupper();
    }
  }
  const auto t1 = Clock::now();

  for (int i = 0; i < N; ++i) consume(_mm_xor_si128(s0[i], s1[i]));
  consume(_mm_xor_si128(_mm_xor_si128(b0, b1), _mm_xor_si128(b2, b3)));
  consume(_mm256_castsi256_si128(
      _mm256_xor_si256(_mm256_xor_si256(a0, a1), _mm256_xor_si256(a2, a3))));
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  return static_cast<double>(iters) * N / secs;
}

template <Mode M>
double best() {
  std::vector<double> v;
  run<M>(kIters / 10);
  for (int r = 0; r < kReps; ++r) v.push_back(run<M>(kIters));
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

int main() {
  const double solo = best<Mode::kSolo>() / 1e6;
  const double sse = best<Mode::kSse128>() / 1e6;
  const double avx = best<Mode::kAvx256>() / 1e6;
  const double vz = best<Mode::kAvxVzero>() / 1e6;

  std::printf("rnds2 at %d lanes, co-issued work varied (Mrnds2/s)\n\n", N);
  std::printf("  %-28s %10.2f   %6.1f%%\n", "solo (no co-issue)", solo, 0.0);
  std::printf("  %-28s %10.2f   %6.1f%%\n", "+ 4x SSE 128-bit adds", sse,
              100.0 * (sse - solo) / solo);
  std::printf("  %-28s %10.2f   %6.1f%%\n", "+ 4x AVX2 256-bit adds", avx,
              100.0 * (avx - solo) / solo);
  std::printf("  %-28s %10.2f   %6.1f%%\n", "+ 4x AVX2 + vzeroupper", vz,
              100.0 * (vz - solo) / solo);

  std::printf("\nverdict: ");
  if (sse > solo * 0.9 && avx < solo * 0.5) {
    std::printf(
        "AVX/SSE TRANSITION ARTIFACT.\n"
        "  128-bit co-issue is nearly free, so the issue ports are NOT\n"
        "  saturated. The 256-bit collapse is the dirty-upper penalty on\n"
        "  legacy-SSE sha256rnds2, which a real hybrid would avoid by\n"
        "  keeping its multi-buffer lanes 128-bit (or by vzeroupper).\n"
        "  The hybrid was closed on a measurement error.\n");
  } else if (sse < solo * 0.9) {
    std::printf(
        "GENUINE PORT CONTENTION.\n"
        "  Even 128-bit co-issue costs rnds2 throughput, so the vector\n"
        "  issue slots really are the constraint. The hybrid stays dead.\n");
  } else {
    std::printf("INCONCLUSIVE -- see the numbers above.\n");
  }
  std::printf("\n(sink %u -- ignore)\n", g_sink);
  return 0;
}
