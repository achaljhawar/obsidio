// Does cgroup CFS quota throttling stall the IO thread, even when the hash
// workers run at SCHED_IDLE?
//
// SCHED_IDLE is a *scheduler class* -- it decides who runs when the cgroup is
// allowed to run at all. CFS bandwidth control sits above it: when the cgroup
// burns its quota for the 100 ms period, EVERY thread in the cgroup is stopped
// until the period rolls over, regardless of priority. This measures that.
//
//   argv[1] = number of saturating "hash worker" threads
//   argv[2] = 1 to put them at SCHED_IDLE, 0 to leave them at SCHED_OTHER
//   argv[3] = duty cycle percent (100 = never yield; 85 = leave headroom)

#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};

long long read_cpu_stat(const char* key) {
  FILE* f = std::fopen("/sys/fs/cgroup/cpu.stat", "r");
  if (f == nullptr) return -1;
  char name[64];
  long long val = 0;
  long long found = -1;
  while (std::fscanf(f, "%63s %lld", name, &val) == 2) {
    if (std::strcmp(name, key) == 0) { found = val; break; }
  }
  std::fclose(f);
  return found;
}

// Stand-in for the hash chain: pure CPU, no syscalls, no memory traffic.
void burn(int duty_pct) {
  volatile std::uint64_t x = 1;
  if (duty_pct >= 100) {
    while (!g_stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 100000; ++i) x = x * 6364136223846793005ULL + 1;
    }
    return;
  }
  // Work for duty_pct% of each 10 ms slice, then sleep the remainder, so the
  // cgroup never reaches its quota and the kernel never has to throttle us.
  const auto slice = std::chrono::microseconds(10000);
  while (!g_stop.load(std::memory_order_relaxed)) {
    const auto start = Clock::now();
    const auto work_until = start + (slice * duty_pct) / 100;
    while (Clock::now() < work_until) {
      for (int i = 0; i < 2000; ++i) x = x * 6364136223846793005ULL + 1;
    }
    std::this_thread::sleep_until(start + slice);
  }
}

int main(int argc, char** argv) {
  const int workers  = argc > 1 ? std::atoi(argv[1]) : 2;
  const int idle     = argc > 2 ? std::atoi(argv[2]) : 1;
  const int duty     = argc > 3 ? std::atoi(argv[3]) : 100;

  const long long thr0 = read_cpu_stat("nr_throttled");
  const long long tus0 = read_cpu_stat("throttled_usec");

  std::vector<std::thread> pool;
  for (int i = 0; i < workers; ++i) {
    pool.emplace_back([idle, duty] {
      if (idle) {
        sched_param p{};
        p.sched_priority = 0;
        if (sched_setscheduler(0, SCHED_IDLE, &p) != 0) {
          setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
        }
      }
      burn(duty);
    });
  }

  // The "IO thread": wants to wake every 2 ms and do a trivial amount of work,
  // exactly like an epoll loop answering /price. We measure how late it wakes.
  std::vector<double> late_ms;
  late_ms.reserve(6000);
  const auto probe_end = Clock::now() + std::chrono::seconds(8);
  while (Clock::now() < probe_end) {
    const auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const double actual =
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    late_ms.push_back(actual - 2.0);
  }

  g_stop.store(true);
  for (auto& t : pool) t.join();

  std::sort(late_ms.begin(), late_ms.end());
  auto pct = [&](double p) {
    return late_ms[(std::size_t)(p * (late_ms.size() - 1))];
  };

  const long long thr1 = read_cpu_stat("nr_throttled");
  const long long tus1 = read_cpu_stat("throttled_usec");

  std::printf(
      "workers=%d sched=%-11s duty=%3d%% | wake delay ms  p50=%6.2f p95=%6.2f "
      "p99=%6.2f max=%7.2f | throttled %lld times, %.0f ms\n",
      workers, idle ? "SCHED_IDLE" : "SCHED_OTHER", duty, pct(0.50), pct(0.95),
      pct(0.99), late_ms.back(), thr1 - thr0, (tus1 - tus0) / 1000.0);
  return 0;
}
