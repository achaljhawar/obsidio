#include "risk_pool.hpp"

#include "risk.hpp"

#if defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace obsidio {
namespace {

// Drop this thread to the weakest scheduling class available. Both calls are
// permitted for unprivileged processes inside a container -- lowering your own
// priority never needs CAP_SYS_NICE.
//
// This is what stops a hashing worker from delaying a /price response: when an
// IO thread becomes runnable the kernel preempts the worker essentially
// immediately, instead of letting it run out a full CFS timeslice.
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
    : max_queue_(max_queue),
      deadline_(deadline),
      on_done_(std::move(on_done)),
      on_dropped_(std::move(on_dropped)) {
  workers_.reserve(workers);
  for (std::size_t i = 0; i < workers; ++i) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

RiskPool::~RiskPool() { stop(); }

bool RiskPool::submit(RiskJob job) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
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
}

std::size_t RiskPool::queue_depth() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

bool RiskPool::expired(const RiskJob& job) const {
  if (deadline_.count() <= 0) return false;
  return (std::chrono::steady_clock::now() - job.queued_at) > deadline_;
}

void RiskPool::worker_loop() {
  deprioritise_current_thread();

  // Fixed for the life of the process: the back end is selected once, under a
  // call_once, before any worker starts taking jobs.
  const int width = risk_lane_width();

  for (;;) {
    // Take up to FOUR jobs, but only in whole lane groups. A single chain is
    // latency-bound, so extra independent chains interleaved in the same loop
    // ride along in the pipeline bubbles -- that is instruction-level
    // parallelism inside one thread, NOT extra threads: the worker count is
    // unchanged and the 2-CPU cap is untouched.
    //
    // The grouping has to follow the back end's real lane width, not the widest
    // chainN that happens to exist. On x86 the width is 2, and a three-job
    // group there composes to chain2 + chain1: the odd chain runs alone at the
    // 1-lane rate, making the group 39% worse than a two-job group (390.84 vs
    // 640.46 chains/s, Ryzen 7 170). Leaving that third job queued to pair with
    // the next arrival is strictly better, and under the graded load the queue
    // sits deep at peak so a partner is essentially always about to arrive.
    // Four is still taken whole -- on a width-2 back end chain4 is two fused
    // pairs, which measures marginally better than two separate calls.
    RiskJob jobs[4];
    int taken = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // Timed wait rather than cv_.wait: request_stop() is a bare atomic store
      // (it must be async-signal-safe, so it cannot notify), and this bounds
      // shutdown latency to the wait interval without one.
      while (!stopping_.load(std::memory_order_relaxed) && queue_.empty()) {
        cv_.wait_for(lock, std::chrono::milliseconds(100));
      }
      if (queue_.empty()) return;  // stopping and drained

      // Whole lane groups while the queue can fill them; otherwise drain what
      // is left rather than stalling behind a partner that may never come.
      const int queued = static_cast<int>(queue_.size());
      int want;
      if (queued >= 4) {
        want = 4;
      } else if (queued >= width) {
        want = (queued / width) * width;
      } else {
        want = queued;
      }

      while (taken < want) {
        jobs[taken++] = std::move(queue_.front());
        queue_.pop_front();
      }
    }

    // A request that has already blown its latency budget scores nothing even
    // if we finish it, and finishing it steals CPU from requests that could
    // still land in time. Shedding it is only worth doing if you can afford the
    // error against the 1% ceiling -- hence disabled by default (deadline 0).
    int live = 0;
    for (int i = 0; i < taken; ++i) {
      if (expired(jobs[i])) {
        on_dropped_(jobs[i]);
        continue;
      }
      if (live != i) jobs[live] = std::move(jobs[i]);
      ++live;
    }

    if (live == 4) {
      std::string digest_a, digest_b, digest_c, digest_d;
      risk_hash_x4(jobs[0].seed, jobs[1].seed, jobs[2].seed, jobs[3].seed,
                   digest_a, digest_b, digest_c, digest_d);
      on_done_(jobs[0], digest_a);
      on_done_(jobs[1], digest_b);
      on_done_(jobs[2], digest_c);
      on_done_(jobs[3], digest_d);
    } else if (live == 3) {
      std::string digest_a, digest_b, digest_c;
      risk_hash_x3(jobs[0].seed, jobs[1].seed, jobs[2].seed, digest_a, digest_b,
                   digest_c);
      on_done_(jobs[0], digest_a);
      on_done_(jobs[1], digest_b);
      on_done_(jobs[2], digest_c);
    } else if (live == 2) {
      std::string digest_a, digest_b;
      risk_hash_x2(jobs[0].seed, jobs[1].seed, digest_a, digest_b);
      on_done_(jobs[0], digest_a);
      on_done_(jobs[1], digest_b);
    } else if (live == 1) {
      on_done_(jobs[0], risk_hash(jobs[0].seed));
    }
  }
}

}  // namespace obsidio
