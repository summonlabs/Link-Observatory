// Link Observatory - explicit, injectable time.
//
// The runtime never reads a clock implicitly. Every operation that needs "now"
// receives it from a Clock, which makes freshness, staleness, windows and
// restart behaviour fully deterministic under test with ManualClock.

#pragma once

#include <cstdint>
#include <string>

#include "linkobs/core/checked.hpp"

namespace linkobs {

using Nanos = std::int64_t;

/// An absolute instant, expressed as nanoseconds since the Unix epoch.
class TimePoint {
 public:
  constexpr TimePoint() noexcept = default;

  [[nodiscard]] static constexpr TimePoint from_nanos(Nanos value) noexcept {
    return TimePoint{value};
  }

  [[nodiscard]] constexpr Nanos nanos() const noexcept { return nanos_; }

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(TimePoint, TimePoint) noexcept = default;
  friend constexpr auto operator<=>(TimePoint, TimePoint) noexcept = default;

 private:
  explicit constexpr TimePoint(Nanos value) noexcept : nanos_(value) {}

  Nanos nanos_{0};
};

/// A signed span of time.
class Duration {
 public:
  constexpr Duration() noexcept = default;

  [[nodiscard]] static constexpr Duration from_nanos(Nanos value) noexcept {
    return Duration{value};
  }
  [[nodiscard]] static constexpr Duration from_micros(Nanos value) noexcept {
    return Duration{value * 1000};
  }
  [[nodiscard]] static constexpr Duration from_millis(Nanos value) noexcept {
    return Duration{value * 1000000};
  }
  [[nodiscard]] static constexpr Duration from_seconds(Nanos value) noexcept {
    return Duration{value * 1000000000};
  }

  [[nodiscard]] constexpr Nanos nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos_ < 0; }

  [[nodiscard]] constexpr Duration magnitude() const noexcept {
    return Duration{nanos_ < 0 ? -nanos_ : nanos_};
  }

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(Duration, Duration) noexcept = default;
  friend constexpr auto operator<=>(Duration, Duration) noexcept = default;

 private:
  explicit constexpr Duration(Nanos value) noexcept : nanos_(value) {}

  Nanos nanos_{0};
};

/// Signed difference between two instants, or an empty result on overflow.
/// The sign tells the caller which instant is later, which is how backwards
/// clock movement is detected instead of silently absorbed.
[[nodiscard]] Checked<Duration> difference(TimePoint later, TimePoint earlier) noexcept;

[[nodiscard]] Checked<TimePoint> advance(TimePoint origin, Duration delta) noexcept;

/// Source of the current instant.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock();

  [[nodiscard]] virtual TimePoint now() const = 0;
};

/// Wall clock, nanosecond resolution, Unix epoch based.
class SystemClock final : public Clock {
 public:
  SystemClock() = default;
  [[nodiscard]] TimePoint now() const override;
};

/// Fully controlled clock for deterministic tests and replays.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(TimePoint start = TimePoint::from_nanos(1700000000000000000LL)) noexcept
      : current_(start) {}

  [[nodiscard]] TimePoint now() const override { return current_; }

  void set(TimePoint instant) noexcept { current_ = instant; }
  void advance_by(Duration delta) noexcept { current_ = advance(current_, delta).value; }

 private:
  TimePoint current_;
};

}  // namespace linkobs
