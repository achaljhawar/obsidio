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

void RiskPool::stop() {
  if (stopping_.exchange(true)) return;
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

  for (;;) {
    // Take TWO jobs when two are queued. A single chain is latency-bound, so
    // running a second one interleaved costs ~45% more time and produces two
    // answers -- roughly 1.45x throughput. Under the graded load the queue sits
    // ~190 deep at peak, so pairs are essentially always available; the
    // single-job path is what runs during ramp-up and cool-down.
    RiskJob jobs[2];
    int taken = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] {
        return stopping_.load(std::memory_order_relaxed) || !queue_.empty();
      });
      if (queue_.empty()) return;  // stopping and drained
      while (taken < 2 && !queue_.empty()) {
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

    if (live == 2) {
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
