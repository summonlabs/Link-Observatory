#include "linkobs/core/lock_tracker.hpp"

#include <atomic>
#include <utility>

namespace linkobs {
namespace {

constexpr std::size_t kMaxReports = 64U;

UntrackedLock& report_lock() {
  static UntrackedLock instance;
  return instance;
}

std::vector<std::string>& report_sink() {
  static std::vector<std::string> instance;
  return instance;
}

std::atomic<std::uint64_t>& violation_count() {
  static std::atomic<std::uint64_t> count{0};
  return count;
}

void record(std::string report) {
  violation_count().fetch_add(1U, std::memory_order_relaxed);
  std::lock_guard<UntrackedLock> guard(report_lock());
  auto& sink = report_sink();
  if (sink.size() >= kMaxReports) {
    sink.erase(sink.begin());
  }
  sink.push_back(std::move(report));
}

#if LINKOBS_LOCK_TRACKING

thread_local std::string g_thread_name{"unnamed"};
thread_local std::vector<LockClass> g_held{};

[[nodiscard]] std::string describe(LockClass value) { return std::string{to_string(value)}; }

void note_acquire(LockClass lock_class) {
  if (lock_class == LockClass::None) {
    return;
  }
  if (!g_held.empty()) {
    const LockClass top = g_held.back();
    if (top == lock_class) {
      record(std::string{"reentrant acquisition of lock class '"} + describe(lock_class) +
             "' on thread '" + g_thread_name + "'");
    } else if (static_cast<int>(lock_class) < static_cast<int>(top)) {
      record(std::string{"out-of-order acquisition: lock class '"} + describe(lock_class) +
             "' acquired while holding '" + describe(top) + "' on thread '" + g_thread_name + "'");
    }
  }
  g_held.push_back(lock_class);
}

void note_release(LockClass lock_class) {
  if (lock_class == LockClass::None) {
    return;
  }
  if (g_held.empty()) {
    record(std::string{"release of lock class '"} + describe(lock_class) +
           "' with no acquisition on thread '" + g_thread_name + "'");
    return;
  }
  if (g_held.back() != lock_class) {
    record(std::string{"unbalanced release of lock class '"} + describe(lock_class) +
           "' while holding '" + describe(g_held.back()) + "' on thread '" + g_thread_name + "'");
  }
  g_held.pop_back();
}

#else

void note_acquire(LockClass /*lock_class*/) {}
void note_release(LockClass /*lock_class*/) {}

#endif

}  // namespace

std::string_view to_string(LockClass lock_class) noexcept {
  switch (lock_class) {
    case LockClass::None:
      return "none";
    case LockClass::RuntimeLifecycle:
      return "runtime-lifecycle";
    case LockClass::ObservatoryState:
      return "observatory-state";
    case LockClass::Diagnostics:
      return "diagnostics";
    case LockClass::IngestQueue:
      return "ingest-queue";
    case LockClass::Transport:
      return "transport";
  }
  return "unknown";
}

void set_current_thread_name(std::string_view name) {
#if LINKOBS_LOCK_TRACKING
  g_thread_name.assign(name);
#else
  (void)name;
#endif
}

std::uint64_t lock_violation_count() noexcept {
  return violation_count().load(std::memory_order_relaxed);
}

std::vector<std::string> lock_violation_reports() {
  std::lock_guard<UntrackedLock> guard(report_lock());
  return report_sink();
}

void reset_lock_tracking() noexcept {
  violation_count().store(0U, std::memory_order_relaxed);
  {
    std::lock_guard<UntrackedLock> guard(report_lock());
    report_sink().clear();
  }
#if LINKOBS_LOCK_TRACKING
  g_held.clear();
#endif
}

LockScope::LockScope(LockClass lock_class) noexcept : lock_class_(lock_class) {
  note_acquire(lock_class_);
}

LockScope::~LockScope() { note_release(lock_class_); }

void TrackedLock::lock() {
  mutex_.lock();
  note_acquire(lock_class_);
}

void TrackedLock::unlock() {
  note_release(lock_class_);
  mutex_.unlock();
}

bool TrackedLock::try_lock() {
  if (!mutex_.try_lock()) {
    return false;
  }
  note_acquire(lock_class_);
  return true;
}

}  // namespace linkobs
