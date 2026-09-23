#include "linkobs/ingest/queue.hpp"

#include <utility>

namespace linkobs {

void BatchCompletion::signal() {
  {
    std::lock_guard<TrackedLock> guard(lock_);
    if (done_) {
      return;
    }
    done_ = true;
  }
  condition_.notify_all();
}

void BatchCompletion::wait() {
  std::unique_lock<TrackedLock> guard(lock_);
  while (!done_) {
    condition_.wait(guard);
  }
}

bool BatchCompletion::done() const {
  std::lock_guard<TrackedLock> guard(lock_);
  return done_;
}

bool BoundedQueue::push(Batch batch) {
  std::unique_lock<TrackedLock> guard(lock_);
  while (!closed_ && items_.size() >= capacity_) {
    not_full_.wait(guard);
  }
  if (closed_) {
    ++rejected_closed_;
    return false;
  }
  items_.push_back(std::move(batch));
  ++pushed_;
  if (items_.size() > high_water_) {
    high_water_ = items_.size();
  }
  guard.unlock();
  not_empty_.notify_one();
  return true;
}

bool BoundedQueue::try_push(Batch batch) {
  std::lock_guard<TrackedLock> guard(lock_);
  if (closed_) {
    ++rejected_closed_;
    return false;
  }
  if (items_.size() >= capacity_) {
    ++rejected_full_;
    return false;
  }
  items_.push_back(std::move(batch));
  ++pushed_;
  if (items_.size() > high_water_) {
    high_water_ = items_.size();
  }
  not_empty_.notify_one();
  return true;
}

bool BoundedQueue::pop(Batch& out) {
  std::unique_lock<TrackedLock> guard(lock_);
  while (items_.empty() && !closed_) {
    not_empty_.wait(guard);
  }
  if (items_.empty()) {
    return false;
  }
  out = std::move(items_.front());
  items_.pop_front();
  ++popped_;
  guard.unlock();
  not_full_.notify_one();
  return true;
}

bool BoundedQueue::try_pop(Batch& out) {
  std::lock_guard<TrackedLock> guard(lock_);
  if (items_.empty()) {
    return false;
  }
  out = std::move(items_.front());
  items_.pop_front();
  ++popped_;
  not_full_.notify_one();
  return true;
}

void BoundedQueue::close() {
  std::lock_guard<TrackedLock> guard(lock_);
  closed_ = true;
  not_empty_.notify_all();
  not_full_.notify_all();
}

bool BoundedQueue::closed() const {
  std::lock_guard<TrackedLock> guard(lock_);
  return closed_;
}

std::size_t BoundedQueue::discard_pending() {
  std::lock_guard<TrackedLock> guard(lock_);
  const std::size_t count = items_.size();
  items_.clear();
  abandoned_ += static_cast<std::uint64_t>(count);
  not_full_.notify_all();
  return count;
}

std::size_t BoundedQueue::size() const {
  std::lock_guard<TrackedLock> guard(lock_);
  return items_.size();
}

std::size_t BoundedQueue::high_water() const {
  std::lock_guard<TrackedLock> guard(lock_);
  return high_water_;
}

std::uint64_t BoundedQueue::pushed() const {
  std::lock_guard<TrackedLock> guard(lock_);
  return pushed_;
}

std::uint64_t BoundedQueue::popped() const {
  std::lock_guard<TrackedLock> guard(lock_);
  return popped_;
}

std::uint64_t BoundedQueue::rejected_full() const {
  std::lock_guard<TrackedLock> guard(lock_);
  return rejected_full_;
}

std::uint64_t BoundedQueue::rejected_closed() const {
  std::lock_guard<TrackedLock> guard(lock_);
  return rejected_closed_;
}

std::uint64_t BoundedQueue::abandoned() const {
  std::lock_guard<TrackedLock> guard(lock_);
  return abandoned_;
}

}  // namespace linkobs
