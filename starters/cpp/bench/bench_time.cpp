// Quick wall-clock A/B harness: run once per RISK_BACKEND value and compare.
// Not part of the shipped image -- compiled standalone, see README.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "risk.hpp"

int main(int argc, char** argv) {
  const int n = argc > 1 ? std::atoi(argv[1]) : 20;
  obsidio::init_risk_backend();

  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  for (int i = 0; i < n; ++i) {
    std::string seed = "0." + std::to_string(480000 + i * 7919);
    volatile auto x = obsidio::risk_hash(seed);
    (void)x;
  }
  const double ms =
      std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

  std::printf("backend=%-32s chains=%-4d ms/chain=%7.3f  chains/s/core=%6.0f\n",
              obsidio::risk_backend_name(), n, ms / n, 1000.0 * n / ms);
  return 0;
}
