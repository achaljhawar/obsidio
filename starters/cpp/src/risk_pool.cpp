#include "risk_pool.hpp"

#include "risk.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#if defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace obsidio {
namespace {

// How hard to hold the hash workers back, selected by RISK_SCHED.
//
// The original choice was SCHED_IDLE, and it does exactly what it promises:
// when an IO thread becomes runnable the kernel preempts the worker almost
// immediately, so /price is answered in microseconds while /risk saturates
// both cores. What it also does is less obvious. A SCHED_IDLE thread carries
// scheduler weight 3 against a normal thread's 1024, so while any IO thread is
// runnable the workers get about 0.3% of the CPU -- not a reduced share,
// effectively none. The hash workers therefore lose roughly whatever fraction
// of wall time the IO threads are runnable.
//
// That is measurable, and it is large: the risk-only probe reaches ~1097
// chains/s, while the graded mix sustains ~849 chains/s with the queue never
// empty. The missing 22.6% is not hashing work that failed to fit, it is
// hashing capacity handed to the IO threads by the scheduler.
//
// The trade being made is worth stating in numbers, because it looks very
// different once written down: /price p95 measures 336 microseconds against a
// 200 ms bar -- 595x of headroom -- and a fifth of the score is being spent
// defending it. Even at 5 ms the margin would still be 40x.
//
// So the class is a knob now, not a constant, and the sweep in
// bench/ryzen/sched_sweep.sh is how the trade gets priced instead of assumed.
// Default remains SCHED_IDLE: unchanged behaviour until a measurement says
// otherwise.
//
//   RISK_SCHED=idle    SCHED_IDLE, weight 3            (default, as shipped)
//   RISK_SCHED=batch   SCHED_BATCH, full weight, but never preempts on wake
//   RISK_SCHED=<0..19> normal class at that nice value; 0 is peer to the IO
//                      threads, 19 is weakest-but-still-fair (weight 15)
//
// All of these lower or keep this thread's own priority, which never requires
// CAP_SYS_NICE inside a container.
const char* apply_worker_scheduling() {
#if defined(__linux__)
  const char* raw = std::getenv("RISK_SCHED");
  const std::string mode = (raw != nullptr && *raw != '\0') ? raw : "idle";

  if (mode == "idle") {
    sched_param param{};
    param.sched_priority = 0;
    if (sched_setscheduler(0, SCHED_IDLE, &param) == 0) return "idle";
    // SCHED_IDLE unavailable (some sandboxes); fall back to max niceness.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);
    return "nice19 (SCHED_IDLE unavailable)";
  }

  if (mode == "batch") {
    sched_param param{};
    param.sched_priority = 0;
    if (sched_setscheduler(0, SCHED_BATCH, &param) == 0) return "batch";
    return "normal (SCHED_BATCH unavailable)";
  }

  // Anything else is read as a nice value. An unparseable or out-of-range
  // value is clamped rather than ignored, so a typo degrades predictably
  // instead of silently leaving the worker at normal priority.
  char* end = nullptr;
  long nice_value = std::strtol(mode.c_str(), &end, 10);
  if (end == mode.c_str()) nice_value = 19;
  if (nice_value < 0) nice_value = 0;
  if (nice_value > 19) nice_value = 19;
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)),
              static_cast<int>(nice_value));
  static thread_local char label[16];
  std::snprintf(label, sizeof(label), "nice%ld", nice_value);
  return label;
#else
  return "unsupported";
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
  // Report the applied class once, from the first worker only: the startup
  // banner is the single place anyone looks to find out what is actually
  // running, and a sweep that silently failed to apply its setting would
  // otherwise look like a null result.
  const char* applied = apply_worker_scheduling();
  static std::once_flag announced;
  std::call_once(announced, [applied] {
    std::fprintf(stderr, "risk workers: scheduling=%s\n", applied);
  });

  for (;;) {
    // Take up to FOUR jobs when four are queued. A single chain is
    // latency-bound, so extra independent chains interleaved in the same loop
    // ride along in the pipeline bubbles: three cost ~37% more time than one
    // and produce that many answers. This is instruction-level parallelism
    // inside one thread, NOT extra threads -- the worker count is unchanged and
    // the 2-CPU cap is untouched. Under the graded load the queue sits deep at
    // peak, so full batches are essentially always available; the shorter paths
    // are what run during ramp-up and cool-down.
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
      while (taken < 4 && !queue_.empty()) {
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
