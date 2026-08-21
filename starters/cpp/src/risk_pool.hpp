// Bounded work queue for /risk.
//
// This is the piece that keeps the fast path fast. /price and /stats are
// handled inline on the IO threads in microseconds; /risk is handed off here so
// it can never occupy an IO thread. Worker threads additionally drop to the
// lowest scheduler priority the kernel offers, so the moment an IO thread has
// work the kernel preempts a hashing worker.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace obsidio {

struct RiskJob {
  int fd = -1;
  int loop_index = 0;  // which IO thread owns this connection
  std::string seed;
  bool keep_alive = true;
  std::chrono::steady_clock::time_point queued_at;
};

class RiskPool {
 public:
  // `on_done` is invoked on a worker thread with the job and its digest.
  // `on_dropped` is invoked when a job is shed past its deadline.
  RiskPool(std::size_t workers, std::size_t max_queue,
           std::chrono::milliseconds deadline,
           std::function<void(const RiskJob&, const std::string&)> on_done,
           std::function<void(const RiskJob&)> on_dropped);
  ~RiskPool();

  RiskPool(const RiskPool&) = delete;
  RiskPool& operator=(const RiskPool&) = delete;

  // Returns false if the queue is full (caller should shed with 503).
  bool submit(RiskJob job);

  void stop();

  std::size_t queue_depth() const;

 private:
  void worker_loop();
  bool expired(const RiskJob& job) const;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<RiskJob> queue_;
  std::vector<std::thread> workers_;
  std::size_t max_queue_;
  std::chrono::milliseconds deadline_;
  std::function<void(const RiskJob&, const std::string&)> on_done_;
  std::function<void(const RiskJob&)> on_dropped_;
  std::atomic<bool> stopping_{false};
};

}  // namespace obsidio
