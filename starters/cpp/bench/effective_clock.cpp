// effective_clock -- portable sustained-clock probe.
//
// POWER-CORRECTION.md §1: neither /proc/cpuinfo MHz inside WSL2 nor any TSC-
// derived figure measures sustained core speed (the former is virtualized,
// the latter invariant by design). What scoring cares about is delivered
// compute per second, so this tool times a dependent operation chain whose
// per-link latency is known from microarchitecture tables and reports the
// implied clock:
//
//     effective GHz = iterations * latency_per_link / elapsed_seconds
//
// Validate once against a trusted Windows-side reading (HWiNFO64 / Ryzen
// Master / Task Manager -> Performance), record any correction factor, then
// treat this as the canonical clock metric everywhere -- container, WSL2,
// cloud VM, unknown grading hardware alike.
//
// Usage:
//   effective_clock [duration_seconds] [assumed_latency] [-w]
//
//   duration_seconds  measuring window length      (default 2.0)
//   assumed_latency   cycles per dependent link    (default 4; Zen addsd=3,
//                     Intel addsd=4 -- pass what your validation run says)
//   -w                watch mode: print one reading per window until killed,
//                     for clock-vs-time curves alongside a k6 run
//
// Build:  g++ -O2 -o effective_clock bench/effective_clock.cpp
// (no dependencies; plain C++17)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <sched.h>
#endif

using Clock = std::chrono::steady_clock;

#if defined(__x86_64__) || defined(__i386__)

// Serial addsd dependency chain. The inline asm guarantees the compiler
// cannot fold or vectorise it away -- the failure modes documented in
// findings.md §7.2 (v2's loop became a closed form) are impossible here
// because every iteration's input is the previous iteration's output as far
// as the register allocator can see.
static double dep_chain(std::uint64_t n) {
  double x = 0.5;
  const double one = 1.0;
  for (std::uint64_t i = 0; i < n; ++i) {
    __asm__ volatile("addsd %1, %0" : "+x"(x) : "x"(one));
  }
  __asm__ volatile("" : : "x"(x));  // sink: keep the chain alive
  return x;
}

#elif defined(__aarch64__) || defined(_M_ARM64)

// Same idea on ARM: serial fadd chain, ~3-4 cycle latency depending on core.
static double dep_chain(std::uint64_t n) {
  double x = 0.5;
  const double one = 1.0;
  for (std::uint64_t i = 0; i < n; ++i) {
    __asm__ volatile("fadd %d0, %d0, %d1" : "+w"(x) : "w"(one));
  }
  __asm__ volatile("" : : "w"(x));
  return x;
}

#else

// Portable fallback: volatile forces the round trip through memory each
// iteration. Slower and noisier than the asm paths but still monotone in
// clock speed, which is all the throttle question needs.
static double dep_chain(std::uint64_t n) {
  volatile double x = 0.5;
  const volatile double one = 1.0;
  for (std::uint64_t i = 0; i < n; ++i) {
    x = x + one;
  }
  return x;
}

#endif

// Iterations sized so one window is roughly `seconds` long at a guess of
// ~3 GHz. Overshoot is fine: we pick N from a short calibration pass so the
// reported window is close to the requested length without recompiling.
static std::uint64_t calibrate_n(double seconds) {
  const std::uint64_t probe = 20'000'000;
  const auto t0 = Clock::now();
  dep_chain(probe);
  const double dt =
      std::chrono::duration<double>(Clock::now() - t0).count();
  if (dt <= 0.0) return probe;
  const double per_iter = dt / static_cast<double>(probe);
  std::uint64_t n =
      static_cast<std::uint64_t>(static_cast<double>(seconds) / per_iter);
  if (n < 1'000'000) n = 1'000'000;
  return n;
}

int main(int argc, char** argv) {
  bool watch = false;
  double seconds = 2.0;
  double latency = 4.0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-w") {
      watch = true;
    } else if (i == 1) {
      seconds = std::atof(argv[i]);
    } else if (i == 2) {
      latency = std::atof(argv[i]);
    }
  }
  if (seconds <= 0.05) seconds = 0.05;
  if (latency <= 0.5) latency = 0.5;

#if defined(__linux__)
  // Pin to one CPU so migrations between cores (and heterogeneous clusters)
  // do not pollute the reading. Best effort only.
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  [[maybe_unused]] int pinned = sched_setaffinity(0, sizeof(set), &set);
#endif

  const std::uint64_t n = calibrate_n(seconds);

  // Warm-up window: caches, branch predictors, and any first-touch effects.
  dep_chain(n / 4);

  if (!watch) {
    // Best-of-three: throttling and scheduling noise only ever lower the
    // reading, so the max window is the cleanest estimate of sustained
    // clock during this interval.
    double best_ghz = 0.0;
    for (int rep = 0; rep < 3; ++rep) {
      const auto t0 = Clock::now();
      dep_chain(n);
      const double dt =
          std::chrono::duration<double>(Clock::now() - t0).count();
      if (dt > 0.0) {
        const double ghz =
            static_cast<double>(n) * latency / dt / 1e9;
        if (ghz > best_ghz) best_ghz = ghz;
      }
    }
    std::printf("effective-clock: %.3f GHz  (chain latency=%.1f cyc/link, "
                "window=%.1fs, best-of-3)\n",
                best_ghz, latency, seconds);
    return 0;
  }

  // Watch mode: one line per window until killed. Pair with k6 to produce
  // the clock-vs-time curve that locates thermal sag within a graded run.
  std::fprintf(stderr, "# t_s  effective_GHz\n");
  const auto start = Clock::now();
  for (;;) {
    const auto t0 = Clock::now();
    dep_chain(n);
    const double dt =
        std::chrono::duration<double>(Clock::now() - t0).count();
    if (dt <= 0.0) continue;
    const double ghz = static_cast<double>(n) * latency / dt / 1e9;
    std::printf("%.1f  %.3f\n", 
                std::chrono::duration<double>(Clock::now() - start).count(),
                ghz);
    std::fflush(stdout);
  }
}
