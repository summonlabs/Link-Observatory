// Link Observatory - lock order and re-entrancy auditing.
//
// The runtime is deliberately built from a small number of coarse locks. To make
// the locking discipline checkable rather than aspirational, every lock class is
// declared here and every acquisition is tracked (in tracking builds) with:
//
//   * re-entrancy detection: the same lock class acquired twice on one thread,
//   * ordering detection: a lower ranked lock class acquired while a higher
//     ranked one is held.
//
// The order below is a strict total order, so an out-of-order acquisition is a
// potential deadlock and is reported. Reports are collected, never thrown, so a
// test can assert that the count is zero after running the whole concurrency
// suite. That is how the deadlock audit is proved rather than asserted.
//
// Define LINKOBS_LOCK_TRACKING=1 to enable. The disabled form is a no-op wrapper
// with identical semantics, so concurrency behaviour never depends on the flag.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#ifndef LINKOBS_LOCK_TRACKING
#define LINKOBS_LOCK_TRACKING 0
#endif

namespace linkobs {

/// Total order of lock acquisition. A thread may only acquire a lock class with
/// a strictly higher rank than every class it already holds, and never the same
/// class twice on the same thread.
enum class LockClass : std::uint8_t {
  None = 0,
  RuntimeLifecycle = 1,
  ObservatoryState = 2,
  IngestQueue = 3,
  Transport = 4,
  Diagnostics = 5,
};

[[nodiscard]] std::string_view to_string(LockClass lock_class) noexcept;

/// Names the calling thread for violation reports. Diagnostic only, never part
/// of a deterministic report.
void set_current_thread_name(std::string_view name);

/// Number of recorded lock order / re-entrancy violations.
[[nodiscard]] std::uint64_t lock_violation_count() noexcept;

/// Human readable reports, oldest first. Bounded.
[[nodiscard]] std::vector<std::string> lock_violation_reports();

/// Clears recorded violations and the calling thread's lock stack.
void reset_lock_tracking() noexcept;

/// RAII marker meaning "this thread currently holds a lock of class X".
///
/// Use it when the underlying synchronisation object cannot itself be a
/// TrackedLock (for example a condition variable's mutex owned elsewhere).
class LockScope {
 public:
  explicit LockScope(LockClass lock_class) noexcept;
  ~LockScope();

  LockScope(const LockScope&) = delete;
  LockScope& operator=(const LockScope&) = delete;

 private:
  LockClass lock_class_;
};

/// A mutex that participates in the lock order audit.
class TrackedLock {
 public:
  explicit TrackedLock(LockClass lock_class) noexcept : lock_class_(lock_class) {}

  void lock();
  void unlock();
  [[nodiscard]] bool try_lock();

 private:
  LockClass lock_class_;
  std::mutex mutex_{};
};

/// A mutex that is deliberately outside the audit (used by the audit itself).
class UntrackedLock {
 public:
  void lock() { mutex_.lock(); }
  void unlock() { mutex_.unlock(); }
  [[nodiscard]] bool try_lock() { return mutex_.try_lock(); }

 private:
  std::mutex mutex_{};
};

}  // namespace linkobs
