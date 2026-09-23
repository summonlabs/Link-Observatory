// Link Observatory - bounded work queues.
//
// Every queue has a hard depth. A full queue is reported to the producer, which
// must decide what to do; the runtime never grows a queue without limit and
// never drops silently. Closure wakes every waiter, so shutdown cannot hang on a
// queue that will never be filled again.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "linkobs/core/lock_tracker.hpp"
#include "linkobs/ingest/codec.hpp"

namespace linkobs {

/// A one-shot latch that reports when a batch has actually been applied.
///
/// The transport acknowledges a push only once the records are in the store.
/// Acknowledging on enqueue would let a client observe a state that predates its
/// own write. There is no timeout: the wait ends when the work ends.
class BatchCompletion {
 public:
  void signal();
  void wait();
  [[nodiscard]] bool done() const;

 private:
  mutable TrackedLock lock_{LockClass::Diagnostics};
  std::condition_variable_any condition_{};
  bool done_{false};
};

class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0U ? 1U : capacity) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  /// Blocks until space is available or the queue is closed. Returns false when
  /// the queue was closed before the item could be enqueued.
  [[nodiscard]] bool push(Batch batch);

  /// Non-blocking. Returns false when the queue is full or closed.
  [[nodiscard]] bool try_push(Batch batch);

  /// Blocks until an item is available or the queue is closed and empty.
  [[nodiscard]] bool pop(Batch& out);

  /// Non-blocking. Returns false when nothing is available.
  [[nodiscard]] bool try_pop(Batch& out);

  void close();
  [[nodiscard]] bool closed() const;

  /// Removes and counts everything still pending. Used by cancellation, so that
  /// abandoned work is reported rather than silently forgotten.
  std::size_t discard_pending();

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t high_water() const;
  [[nodiscard]] std::uint64_t pushed() const;
  [[nodiscard]] std::uint64_t popped() const;
  [[nodiscard]] std::uint64_t rejected_full() const;
  [[nodiscard]] std::uint64_t rejected_closed() const;
  /// Records abandoned when the queue was closed with items still pending.
  [[nodiscard]] std::uint64_t abandoned() const;

 private:
  mutable TrackedLock lock_{LockClass::IngestQueue};
  std::condition_variable_any not_empty_{};
  std::condition_variable_any not_full_{};
  std::deque<Batch> items_{};
  std::size_t capacity_{1};
  bool closed_{false};
  std::size_t high_water_{0};
  std::uint64_t pushed_{0};
  std::uint64_t popped_{0};
  std::uint64_t rejected_full_{0};
  std::uint64_t rejected_closed_{0};
  std::uint64_t abandoned_{0};
};

}  // namespace linkobs
