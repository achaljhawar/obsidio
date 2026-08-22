#include "risk_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

#include "risk.hpp"

#if defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace obsidio {
namespace {

// The widest group any back end offers; sizes the per-worker job array.
constexpr int kMaxBatch{8};

// Groups executed at each width over the process lifetime. One relaxed
// increment against a group that costs milliseconds, and it answers what the
// score cannot: how much of a run executed below the kernel's best width.
std::atomic<unsigned long long> g_group_counts[kMaxBatch + 1];

// Drop to the weakest scheduling class available -- lowering your own priority
// never needs CAP_SYS_NICE. This is what stops a hashing worker from delaying
// a /price response: the kernel preempts it the moment IO becomes runnable.
void deprioritise_current_thread() {
#if defined(__linux__)
  sched_param param{};
  param.sched_priority = 0;
  if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
    // SCHED_IDLE unavailable (some sandboxes); fall back to max niceness.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);
  }
#endif
}

}  // namespace

RiskPool::RiskPool(std::size_t workers, std::size_t max_queue,
                   std::chrono::milliseconds deadline,
                   std::function<void(const RiskJob&, const std::string&)> on_done,
                   std::function<void(const RiskJob&)> on_dropped)
    : max_queue_{max_queue},
      deadline_{deadline},
      on_done_{std::move(on_done)},
      on_dropped_{std::move(on_dropped)} {
  workers_.reserve(workers);
  for (std::size_t i{}; i < workers; ++i) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

RiskPool::~RiskPool() { stop(); }

bool RiskPool::submit(RiskJob job) {
  {
    std::lock_guard<std::mutex> lock{mutex_};
    if (stopping_.load(std::memory_order_relaxed)) return false;
    if (queue_.size() >= max_queue_) return false;
    job.queued_at = std::chrono::steady_clock::now();
    queue_.push_back(std::move(job));
  }
  cv_.notify_one();
  return true;
}

void RiskPool::request_stop() { stopping_.store(true, std::memory_order_relaxed); }

void RiskPool::stop() {
  request_stop();
  cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();

  // Diagnostics, not telemetry: one stderr line at shutdown.
  unsigned long long total{};
  for (int w{1}; w <= kMaxBatch; ++w)
    total += g_group_counts[w].load(std::memory_order_relaxed);
  if (total != 0) {
    std::fprintf(stderr, "risk_pool group widths:");
    for (int w{1}; w <= kMaxBatch; ++w) {
      const unsigned long long c{g_group_counts[w].load(std::memory_order_relaxed)};
      if (c != 0) std::fprintf(stderr, "  x%d=%llu", w, c);
    }
    std::fprintf(stderr, "  (groups=%llu)\n", total);
  }
}

std::size_t RiskPool::queue_depth() const {
  std::lock_guard<std::mutex> lock{mutex_};
  return queue_.size();
}

bool RiskPool::expired(const RiskJob& job) const {
  if (deadline_.count() <= 0) return false;
  return (std::chrono::steady_clock::now() - job.queued_at) > deadline_;
}

void RiskPool::worker_loop() {
  deprioritise_current_thread();

  // Fixed for the process: the back end is selected under a call_once before
  // any worker takes a job.
  const int width{risk_lane_width()};

  for (;;) {
    // Independent chains interleaved in one kernel ride each other's pipeline
    // bubbles -- instruction-level parallelism inside one thread, not extra
    // threads, so the 2-CPU cap is untouched.
    //
    // Grouping follows the back end's real lane width, never the widest chainN
    // that exists: composing past it strands a chain at the 1-lane rate, which
    // measured 39% worse than not grouping it (x3 vs x2, Ryzen 7 170). Policy
    // is "eight when eight are queued, otherwise exactly the pre-chain8
    // behaviour", where `sub` is the width composed paths really run at -- 2 on
    // a width-8 back end, the width itself elsewhere. ARM (4) and the reference
    // path (1) are therefore unchanged.
    //
    // Leaving a remainder queued beats running it short: the queue sits deep at
    // peak, so a partner is essentially always about to arrive.
    RiskJob jobs[kMaxBatch];
    int taken{};
    {
      std::unique_lock<std::mutex> lock{mutex_};
      // Timed wait, not cv_.wait: request_stop() is a bare atomic store (it
      // must be async-signal-safe) so it cannot notify.
      while (!stopping_.load(std::memory_order_relaxed) && queue_.empty()) {
        cv_.wait_for(lock, std::chrono::milliseconds(100));
      }
      if (queue_.empty()) return;  // stopping and drained

      const int queued{static_cast<int>(queue_.size())};
      const int sub{(width >= 8) ? 2 : width};
      int want{};
      if (width >= 8 && queued >= 8) {
        want = 8;
      } else if (queued >= 4) {
        want = 4;
      } else if (queued >= sub) {
        want = (queued / sub) * sub;
      } else {
        want = queued;
      }

      while (taken < want) {
        jobs[taken++] = std::move(queue_.front());
        queue_.pop_front();
      }
    }

    // A request past its latency budget scores nothing even if finished, and
    // finishing it steals CPU from ones that could still land. Only worth
    // shedding if you can afford the error -- hence disabled by default.
    int live{};
    for (int i{}; i < taken; ++i) {
      if (expired(jobs[i])) {
        on_dropped_(jobs[i]);
        continue;
      }
      if (live != i) jobs[live] = std::move(jobs[i]);
      ++live;
    }

    // Widest-group-first. `taken` is only ever 8, 4, 3, 2 or 1 by construction,
    // so the loop exists only because deadline shedding can leave counts the
    // batcher would never choose (5, 6, 7); those degrade to narrower
    // compositions rather than to anything wrong.
    int i{};
    while (i < live) {
      const int rem{live - i};
      if (rem >= 8) {
        g_group_counts[8].fetch_add(1, std::memory_order_relaxed);
        std::string seeds[8], digests[8];
        for (int j{}; j < 8; ++j) seeds[j] = jobs[i + j].seed;
        risk_hash_x8(seeds, digests);
        for (int j{}; j < 8; ++j) on_done_(jobs[i + j], digests[j]);
        i += 8;
      } else if (rem >= 4) {
        g_group_counts[4].fetch_add(1, std::memory_order_relaxed);
        std::string digest_a, digest_b, digest_c, digest_d;
        risk_hash_x4(jobs[i].seed, jobs[i + 1].seed, jobs[i + 2].seed,
                     jobs[i + 3].seed, digest_a, digest_b, digest_c, digest_d);
        on_done_(jobs[i], digest_a);
        on_done_(jobs[i + 1], digest_b);
        on_done_(jobs[i + 2], digest_c);
        on_done_(jobs[i + 3], digest_d);
        i += 4;
      } else if (rem == 3) {
        g_group_counts[3].fetch_add(1, std::memory_order_relaxed);
        std::string digest_a, digest_b, digest_c;
        risk_hash_x3(jobs[i].seed, jobs[i + 1].seed, jobs[i + 2].seed, digest_a,
                     digest_b, digest_c);
        on_done_(jobs[i], digest_a);
        on_done_(jobs[i + 1], digest_b);
        on_done_(jobs[i + 2], digest_c);
        i += 3;
      } else if (rem == 2) {
        g_group_counts[2].fetch_add(1, std::memory_order_relaxed);
        std::string digest_a, digest_b;
        risk_hash_x2(jobs[i].seed, jobs[i + 1].seed, digest_a, digest_b);
        on_done_(jobs[i], digest_a);
        on_done_(jobs[i + 1], digest_b);
        i += 2;
      } else {
        g_group_counts[1].fetch_add(1, std::memory_order_relaxed);
        on_done_(jobs[i], risk_hash(jobs[i].seed));
        i += 1;
      }
    }
  }
}

}  // namespace obsidio
