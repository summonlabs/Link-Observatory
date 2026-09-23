#include "linkobs/runtime/worker.hpp"

#include <functional>
#include <utility>

#include "linkobs/core/text.hpp"

namespace linkobs {
namespace {

/// FNV-1a 64 over the source name. Deliberately independent of std::hash so the
/// routing is identical across standard libraries and processes.
[[nodiscard]] std::uint64_t route_hash(std::string_view name) noexcept {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char raw : name) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

}  // namespace

WorkerPool::WorkerPool(std::size_t worker_count, std::size_t queue_capacity) {
  const std::size_t count = worker_count == 0U ? 1U : worker_count;
  workers_.resize(count);
  for (auto& worker : workers_) {
    worker.queue = std::make_unique<BoundedQueue>(queue_capacity);
  }
}

WorkerPool::~WorkerPool() {
  cancel();
}

Status WorkerPool::start(Handler handler) {
  std::lock_guard<TrackedLock> guard(lifecycle_lock_);
  if (running_) {
    return Status::of(StatusCode::AlreadyExists, "worker pool is already running");
  }
  handler_ = std::move(handler);
  cancelled_ = false;
  running_ = true;
  for (std::size_t index = 0; index < workers_.size(); ++index) {
    Worker& worker = workers_[index];
    const std::string thread_name = std::string{"ingest-"} + to_dec(index);
    worker.thread = std::thread([this, &worker, thread_name]() {
      set_current_thread_name(thread_name);
      Batch batch{};
      while (worker.queue->pop(batch)) {
        // The completion callback is taken out of the batch before the handler
        // consumes it, so it is invoked exactly once after every record in the
        // batch has been applied.
        std::function<void()> on_applied = std::move(batch.on_applied);
        if (handler_) {
          handler_(std::move(batch));
        }
        if (on_applied) {
          on_applied();
        }
        batch = Batch{};
        processed_.fetch_add(1U, std::memory_order_relaxed);
      }
    });
  }
  return Status::success();
}

bool WorkerPool::running() const {
  std::lock_guard<TrackedLock> guard(lifecycle_lock_);
  return running_;
}

std::size_t WorkerPool::route_of(std::string_view source_name) const {
  if (workers_.empty()) {
    return 0U;
  }
  return static_cast<std::size_t>(route_hash(source_name) % workers_.size());
}

Status WorkerPool::submit(std::size_t route, Batch batch) {
  if (workers_.empty()) {
    return Status::of(StatusCode::Internal, "worker pool has no workers");
  }
  {
    std::lock_guard<TrackedLock> guard(lifecycle_lock_);
    if (!running_) {
      return Status::of(StatusCode::Cancelled, "worker pool is not running");
    }
  }
  const std::size_t index = route % workers_.size();
  if (!workers_[index].queue->push(std::move(batch))) {
    return Status::of(StatusCode::Cancelled, "worker pool queue is closed");
  }
  return Status::success();
}

Status WorkerPool::try_submit(std::size_t route, Batch batch) {
  if (workers_.empty()) {
    return Status::of(StatusCode::Internal, "worker pool has no workers");
  }
  {
    std::lock_guard<TrackedLock> guard(lifecycle_lock_);
    if (!running_) {
      return Status::of(StatusCode::Cancelled, "worker pool is not running");
    }
  }
  const std::size_t index = route % workers_.size();
  if (!workers_[index].queue->try_push(std::move(batch))) {
    return Status::of(StatusCode::CapacityExceeded, "worker pool queue is full or closed");
  }
  return Status::success();
}

void WorkerPool::stop() {
  std::lock_guard<TrackedLock> guard(lifecycle_lock_);
  if (!running_) {
    return;
  }
  for (auto& worker : workers_) {
    worker.queue->close();
  }
  for (auto& worker : workers_) {
    if (worker.thread.joinable()) {
      worker.thread.join();
    }
  }
  running_ = false;
  handler_ = nullptr;
}

void WorkerPool::cancel() {
  std::lock_guard<TrackedLock> guard(lifecycle_lock_);
  if (!running_) {
    return;
  }
  cancelled_ = true;
  for (auto& worker : workers_) {
    worker.queue->close();
    const std::size_t discarded = worker.queue->discard_pending();
    (void)discarded;
  }
  for (auto& worker : workers_) {
    if (worker.thread.joinable()) {
      worker.thread.join();
    }
  }
  running_ = false;
  handler_ = nullptr;
}

std::uint64_t WorkerPool::abandoned() const {
  std::uint64_t total = 0;
  for (const auto& worker : workers_) {
    total += worker.queue->abandoned();
  }
  return total;
}

std::vector<std::size_t> WorkerPool::queue_high_water() const {
  std::vector<std::size_t> out;
  out.reserve(workers_.size());
  for (const auto& worker : workers_) {
    out.push_back(worker.queue->high_water());
  }
  return out;
}

std::uint64_t WorkerPool::rejected_full() const {
  std::uint64_t total = 0;
  for (const auto& worker : workers_) {
    total += worker.queue->rejected_full();
  }
  return total;
}

}  // namespace linkobs
