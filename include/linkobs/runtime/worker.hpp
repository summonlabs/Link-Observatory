// Link Observatory - bounded worker pool with per-source ordering.
//
// Work is routed to a worker by a deterministic function of the source name, so
// records from one source are always applied by the same worker in submission
// order. Increasing the worker count therefore changes throughput and nothing
// else: the resulting evidence set is the same as a single threaded run.
//
// Shutdown is real. stop() closes the queues and lets workers drain; cancel()
// discards pending work, counts it, and joins. Neither waits on a timeout.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "linkobs/core/lock_tracker.hpp"
#include "linkobs/core/status.hpp"
#include "linkobs/ingest/queue.hpp"

namespace linkobs {

class WorkerPool {
 public:
  using Handler = std::function<void(Batch&&)>;

  WorkerPool(std::size_t worker_count, std::size_t queue_capacity);
  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  [[nodiscard]] Status start(Handler handler);
  [[nodiscard]] bool running() const;

  /// Route for a source name. Deterministic across processes and runs.
  [[nodiscard]] std::size_t route_of(std::string_view source_name) const;

  /// Blocking submit. Fails when the pool is not running or was cancelled.
  [[nodiscard]] Status submit(std::size_t route, Batch batch);

  /// Non-blocking submit. Returns CapacityExceeded when the routed queue is full.
  [[nodiscard]] Status try_submit(std::size_t route, Batch batch);

  /// Closes every queue and joins after the pending work is processed.
  void stop();

  /// Discards pending work and joins. Abandoned batches are counted.
  void cancel();

  [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }
  [[nodiscard]] std::uint64_t processed() const noexcept { return processed_.load(); }
  [[nodiscard]] std::uint64_t abandoned() const;
  [[nodiscard]] std::vector<std::size_t> queue_high_water() const;
  [[nodiscard]] std::uint64_t rejected_full() const;

 private:
  struct Worker {
    std::unique_ptr<BoundedQueue> queue{};
    std::thread thread{};
  };

  mutable TrackedLock lifecycle_lock_{LockClass::RuntimeLifecycle};
  std::vector<Worker> workers_{};
  Handler handler_{};
  bool running_{false};
  bool cancelled_{false};
  std::atomic<std::uint64_t> processed_{0};
};

}  // namespace linkobs
