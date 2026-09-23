#include "linkobs/time/clock.hpp"

#include <chrono>

#include "linkobs/core/text.hpp"

namespace linkobs {

Clock::~Clock() = default;

std::string TimePoint::to_string() const { return to_dec(nanos_); }

std::string Duration::to_string() const { return to_dec(nanos_); }

Checked<Duration> difference(TimePoint later, TimePoint earlier) noexcept {
  const Checked<std::int64_t> delta = checked_sub(later.nanos(), earlier.nanos());
  Checked<Duration> result{};
  if (!delta.ok) {
    return result;
  }
  result.ok = true;
  result.value = Duration::from_nanos(delta.value);
  return result;
}

Checked<TimePoint> advance(TimePoint origin, Duration delta) noexcept {
  const Checked<std::int64_t> sum = checked_add(origin.nanos(), delta.nanos());
  Checked<TimePoint> result{};
  if (!sum.ok) {
    return result;
  }
  result.ok = true;
  result.value = TimePoint::from_nanos(sum.value);
  return result;
}

TimePoint SystemClock::now() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch);
  return TimePoint::from_nanos(static_cast<Nanos>(nanos.count()));
}

}  // namespace linkobs
