// Per-chain cost of the SHIPPED x86 kernel at one, two, three and four lanes.
//
// rnds2_ports.cpp measures the instruction in isolation and says how many
// independent chains the hardware *could* absorb. This measures what the real
// kernel achieves, including the message schedule, the hex conversion, and
// whatever register pressure the compiler actually ends up with. The gap
// between the two is the headroom a redesign could recover.
//
// Read them together:
//   rnds2_ports says 2 lanes saturate, and x2 here is ~2x x1  -> kernel done.
//   rnds2_ports says 4 lanes saturate, but x4 here is ~= x2   -> the kernel is
//     leaving throughput on the table for a structural reason (register
//     pressure / spills), which is exactly what the phase-split redesign
//     addresses.
//
// Single-threaded and isolated from the HTTP server on purpose: no queue, no
// sockets, no scheduler interaction. Compare ratios between rows, not absolute
// numbers against the graded run.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if !defined(__x86_64__)
int main() {
  std::printf("bench_chain_x86: x86-64 only; this machine is a different ISA.\n");
  return 0;
}
#else

#include <algorithm>
#include <chrono>
#include <vector>

#include "chain_backend.hpp"
#include "sha256.hpp"

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kRounds = 50000;  // the graded chain length
constexpr int kReps = 5;

void first_round(const std::string& seed, char out[64]) {
  std::uint8_t d[obsidio::kSha256DigestBytes];
  obsidio::sha256(reinterpret_cast<const std::uint8_t*>(seed.data()),
                  seed.size(), d);
  obsidio::hex_encode(d, obsidio::kSha256DigestBytes, out);
}

double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

const char* kGolden05 =
    "8dc4014994d6d0df04656cb1d5988562af06015babd9592bf37451173c451148";

}  // namespace

int main() {
  __builtin_cpu_init();
  const obsidio::chain::Backend* b = obsidio::chain::x86_sha_backend();
  if (b == nullptr) {
    std::printf(
        "bench_chain_x86: the x86 SHA-NI back end declined to select on this\n"
        "CPU, so there is no kernel here to benchmark. On a machine that is\n"
        "meant to have SHA-NI, that is a bug worth chasing.\n");
    return 0;
  }
  std::printf("kernel: %s\n", b->name);
  std::printf("%d rounds per chain, median of %d reps\n\n", kRounds, kReps);

  char h[8][64], o[8][64];
  const char* seeds[8] = {"0.5",     "0.9999",  "0.31415", "0.271828",
                          "1.61803", "2.71828", "3.14159", "0.000001"};
  for (int i = 0; i < 8; ++i) first_round(seeds[i], h[i]);

  // Correctness before speed: a kernel that is fast and wrong scores zero, and
  // a benchmark that does not check is how that ships.
  b->chain1(h[0], kRounds - 1, o[0]);
  if (std::string(o[0], 64) != kGolden05) {
    std::printf("FAIL: chain1 digest does not match the known golden.\n");
    return 1;
  }
  std::printf("digest check: chain1(seed=0.5) matches the golden\n\n");

  struct Row {
    const char* name;
    int lanes;
    double chains_per_sec;
  };
  std::vector<Row> rows;

  {  // x1
    std::vector<double> s;
    for (int r = 0; r < kReps; ++r) {
      const auto t0 = Clock::now();
      b->chain1(h[0], kRounds - 1, o[0]);
      const double dt = std::chrono::duration<double>(Clock::now() - t0).count();
      s.push_back(1.0 / dt);
    }
    rows.push_back({"x1", 1, median_of(s)});
  }
  {  // x2
    std::vector<double> s;
    for (int r = 0; r < kReps; ++r) {
      const auto t0 = Clock::now();
      b->chain2(h[0], h[1], kRounds - 1, o[0], o[1]);
      const double dt = std::chrono::duration<double>(Clock::now() - t0).count();
      s.push_back(2.0 / dt);
    }
    rows.push_back({"x2", 2, median_of(s)});
  }
  if (b->chain3 != nullptr) {
    std::vector<double> s;
    for (int r = 0; r < kReps; ++r) {
      const auto t0 = Clock::now();
      b->chain3(h[0], h[1], h[2], kRounds - 1, o[0], o[1], o[2]);
      const double dt = std::chrono::duration<double>(Clock::now() - t0).count();
      s.push_back(3.0 / dt);
    }
    rows.push_back({"x3", 3, median_of(s)});
  }
  if (b->chain4 != nullptr) {
    std::vector<double> s;
    for (int r = 0; r < kReps; ++r) {
      const auto t0 = Clock::now();
      b->chain4(h[0], h[1], h[2], h[3], kRounds - 1, o[0], o[1], o[2], o[3]);
      const double dt = std::chrono::duration<double>(Clock::now() - t0).count();
      s.push_back(4.0 / dt);
    }
    rows.push_back({"x4", 4, median_of(s)});
  }
  if (b->chain8 != nullptr) {
    // Same rule as above: verified before timed. chain8 is a structurally
    // different kernel (pipelined phase-split), so check it against chain2 on
    // the same seeds rather than trusting the chain1 golden to cover it.
    char c2[2][64];
    b->chain2(h[0], h[1], kRounds - 1, c2[0], c2[1]);
    b->chain8(h, kRounds - 1, o);
    if (std::memcmp(o[0], c2[0], 64) != 0 || std::memcmp(o[1], c2[1], 64) != 0) {
      std::printf("FAIL: chain8 lanes 0-1 disagree with chain2 on the same seeds.\n");
      return 1;
    }
    std::vector<double> s;
    for (int r = 0; r < kReps; ++r) {
      const auto t0 = Clock::now();
      b->chain8(h, kRounds - 1, o);
      const double dt = std::chrono::duration<double>(Clock::now() - t0).count();
      s.push_back(8.0 / dt);
    }
    rows.push_back({"x8", 8, median_of(s)});
  }

  std::printf("%6s  %14s  %14s  %s\n", "lanes", "chains/s", "ms/chain",
              "vs x1");
  std::printf("------------------------------------------------------------\n");
  const double base = rows.front().chains_per_sec;
  for (const Row& r : rows) {
    std::printf("%6s  %14.2f  %14.3f  %8.2fx\n", r.name, r.chains_per_sec,
                1000.0 / r.chains_per_sec, r.chains_per_sec / base);
  }

  double best = 0.0;
  const char* best_name = "";
  for (const Row& r : rows) {
    if (r.chains_per_sec > best) {
      best = r.chains_per_sec;
      best_name = r.name;
    }
  }
  std::printf("\nbest lane count on this CPU: %s (%.2f chains/s single-thread)\n",
              best_name, best);
  std::printf(
      "\nThe pool batches up to four jobs, so if a wider row wins here it is\n"
      "worth wiring; if x4 is not better than x2, the pool's widest path is\n"
      "costing register pressure for nothing.\n");
  return 0;
}
#endif
