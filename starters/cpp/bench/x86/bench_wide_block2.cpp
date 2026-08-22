// What did widening block 2 to four lanes actually buy?
//
// The old chain4 was literally two sequential chain2 calls, so both the before
// and the after can run in ONE binary through the public API: risk_hash_x2
// twice is the old behaviour exactly, risk_hash_x4 is the new one. Same
// process, same clock, same thermal state, alternated -- which is worth far
// more than an end-to-end A/B here, because the graded harness has a ~2.7%
// noise floor on this laptop and no way to swap kernels between arms.
//
// Digests are compared before anything is timed: four chains regrouped across
// block 2 must produce byte-identical output, and a benchmark that does not
// check that is measuring the speed of a wrong answer.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "risk.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kIterations = 5000;  // shorter than the real 50,000; same shape
constexpr int kReps = 7;

const std::string kSeeds[4] = {"0.11", "0.22", "0.33", "0.44"};

// Old chain4: two independent two-lane groups.
double run_pairs(int iters, std::string out[4]) {
  const auto t0 = Clock::now();
  obsidio::risk_hash_x2(kSeeds[0], kSeeds[1], out[0], out[1], iters);
  obsidio::risk_hash_x2(kSeeds[2], kSeeds[3], out[2], out[3], iters);
  const auto t1 = Clock::now();
  return std::chrono::duration<double>(t1 - t0).count();
}

// New chain4: block 1 as two pairs, then one four-lane block 2.
double run_quad(int iters, std::string out[4]) {
  const auto t0 = Clock::now();
  obsidio::risk_hash_x4(kSeeds[0], kSeeds[1], kSeeds[2], kSeeds[3], out[0],
                        out[1], out[2], out[3], iters);
  const auto t1 = Clock::now();
  return std::chrono::duration<double>(t1 - t0).count();
}

double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

int main() {
  obsidio::init_risk_backend();
  std::printf("back end: %s\n", obsidio::risk_backend_name());
  std::printf("lane width %d\n", obsidio::risk_lane_width());
  std::printf("%d rounds per chain, 4 chains, median of %d reps\n\n",
              kIterations, kReps);

  std::string a[4], b[4];
  run_pairs(kIterations, a);
  run_quad(kIterations, b);
  for (int i = 0; i < 4; ++i) {
    if (a[i] != b[i]) {
      std::printf("DIGEST MISMATCH on chain %d\n  pairs %s\n  quad  %s\n", i,
                  a[i].c_str(), b[i].c_str());
      return 1;
    }
  }
  std::printf("digest check: all four chains identical between paths\n\n");

  // Alternate the arms so any monotonic clock drift lands on both equally.
  std::vector<double> pairs, quad;
  for (int r = 0; r < kReps; ++r) {
    pairs.push_back(run_pairs(kIterations, a));
    quad.push_back(run_quad(kIterations, b));
  }

  const double tp = median_of(pairs);
  const double tq = median_of(quad);
  const double chains = 4.0;
  std::printf("  %-34s %8.2f ms   %8.2f chains/s\n",
              "two chain2 groups (old chain4)", tp * 1e3, chains / tp);
  std::printf("  %-34s %8.2f ms   %8.2f chains/s\n",
              "one chain4 call (wide when applied)", tq * 1e3, chains / tq);

  const double speedup = tp / tq;
  // The risk chain is ~91% of server CPU; the cheap path is the other ~9%.
  const double score = 100.0 / (91.0 / speedup + 9.0);
  std::printf("\n  risk path %.3fx  ->  projected work_score %+.1f%%\n",
              speedup, (score - 1.0) * 100.0);
  return 0;
}
