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

// The widest group any back end offers. Sizes the per-worker job array; the
// batcher never asks for more than the selected back end's lane width.
constexpr int kMaxBatch = 8;

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
    // Take a whole lane group when one is queued, otherwise the largest batch
    // that composes without stranding a chain on its own. A single chain is
    // latency-bound, so extra independent chains interleaved in the same loop
    // ride along in the pipeline bubbles -- that is instruction-level
    // parallelism inside one thread, NOT extra threads: the worker count is
    // unchanged and the 2-CPU cap is untouched.
    //
    // COMPOSITION RULES, because this is where getting it wrong is invisible.
    //
    // The grouping follows the back end's real lane width, not the widest
    // chainN that happens to exist. Every chainN is callable and correct, but
    // a back end may implement some of them by composition: on x86 chain3 is
    // chain2 + chain1, so the odd chain runs alone at the 1-lane rate and the
    // group measures 39% WORSE than a two-job group (390.84 vs 640.46
    // chains/s, Ryzen 7 170). That is the shape of bug to avoid -- never put a
    // chain somewhere it runs alone inside a wider group.
    //
    // x86's width is 8 now, via a pipelined phase-split kernel that is a
    // different kernel rather than a wider one (chain_x86.cpp). Below eight it
    // has nothing new: its sub-eight compositions are still pairs. So the
    // policy is deliberately "eight when eight are queued, otherwise exactly
    // what this code did before chain8 existed":
    //
    //   queued >= 8, width 8   -> 8   one chain8 group
    //   queued >= 4            -> 4   two fused pairs, as before
    //   queued >= sub          -> largest multiple of sub, as before
    //   otherwise              -> whatever is left (only 1 can reach here)
    //
    // where `sub` is the width the composed paths actually run at: 2 on a
    // width-8 back end, and the width itself everywhere else. On ARM (width 4,
    // a genuine four-lane interleave with genuine chain3/chain2 below it) and
    // on the reference path (width 1) this is byte-for-byte the old behaviour,
    // which is the point -- the only new case is the eight-wide one.
    //
    // Leaving a remainder queued rather than running it short is strictly
    // better: under the graded load the queue sits deep at peak, so a partner
    // is essentially always about to arrive. The shorter paths are what run
    // during ramp-up and cool-down.
    RiskJob jobs[kMaxBatch];
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
      const int sub = (width >= 8) ? 2 : width;
      int want;
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

    // Consume the batch widest-group-first. `taken` is only ever 8, 4, 3, 2 or
    // 1 by construction above, so the single-pass shapes below are the normal
    // case; the loop exists because deadline shedding can remove jobs from the
    // middle of a batch and leave a count the batcher would never have chosen
    // (5, 6, 7). That path is unreachable unless RISK_DEADLINE_MS is set, and
    // it degrades to a narrower composition rather than to anything wrong.
    int i = 0;
    while (i < live) {
      const int rem = live - i;
      if (rem >= 8) {
        std::string seeds[8], digests[8];
        for (int j = 0; j < 8; ++j) seeds[j] = jobs[i + j].seed;
        risk_hash_x8(seeds, digests);
        for (int j = 0; j < 8; ++j) on_done_(jobs[i + j], digests[j]);
        i += 8;
      } else if (rem >= 4) {
        std::string digest_a, digest_b, digest_c, digest_d;
        risk_hash_x4(jobs[i].seed, jobs[i + 1].seed, jobs[i + 2].seed,
                     jobs[i + 3].seed, digest_a, digest_b, digest_c, digest_d);
        on_done_(jobs[i], digest_a);
        on_done_(jobs[i + 1], digest_b);
        on_done_(jobs[i + 2], digest_c);
        on_done_(jobs[i + 3], digest_d);
        i += 4;
      } else if (rem == 3) {
        std::string digest_a, digest_b, digest_c;
        risk_hash_x3(jobs[i].seed, jobs[i + 1].seed, jobs[i + 2].seed, digest_a,
                     digest_b, digest_c);
        on_done_(jobs[i], digest_a);
        on_done_(jobs[i + 1], digest_b);
        on_done_(jobs[i + 2], digest_c);
        i += 3;
      } else if (rem == 2) {
        std::string digest_a, digest_b;
        risk_hash_x2(jobs[i].seed, jobs[i + 1].seed, digest_a, digest_b);
        on_done_(jobs[i], digest_a);
        on_done_(jobs[i + 1], digest_b);
        i += 2;
      } else {
        on_done_(jobs[i], risk_hash(jobs[i].seed));
        i += 1;
      }
    }
  }
}

}  // namespace obsidio
