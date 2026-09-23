// Link Observatory - metric identity and typed readings.
//
// A reading is only meaningful together with its metric key. The key is
// (family, direction, error class); freshness, confidence, conflict and
// completeness are all tracked per key, never per link as a whole.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "linkobs/core/status.hpp"
#include "linkobs/domain/enums.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/time/clock.hpp"

namespace linkobs {

/// Administrative state as declared by the source.
enum class AdminStateValue : std::uint8_t {
  Unknown = 0,
  Enabled = 1,
  Disabled = 2,
};

/// Operational state as declared by the source. "Degraded" here means the
/// source itself reports a degraded operational state; it is not the runtime's
/// inference.
enum class OperStateValue : std::uint8_t {
  Unknown = 0,
  Up = 1,
  Down = 2,
  Degraded = 3,
};

[[nodiscard]] std::string_view to_string(AdminStateValue value) noexcept;
[[nodiscard]] std::string_view to_string(OperStateValue value) noexcept;
[[nodiscard]] bool parse_admin_state(std::string_view text, AdminStateValue& out) noexcept;
[[nodiscard]] bool parse_oper_state(std::string_view text, OperStateValue& out) noexcept;

/// Full scale for ratio readings, in parts per billion.
inline constexpr std::uint64_t kPpbFullScale = 1000000000ULL;
/// Largest ratio the runtime accepts, allowing sources to report oversubscription.
inline constexpr std::uint64_t kPpbAcceptedMax = 2000000000ULL;

struct AdminStateReading {
  AdminStateValue value{AdminStateValue::Unknown};
  friend bool operator==(const AdminStateReading&, const AdminStateReading&) noexcept = default;
};

struct OperStateReading {
  OperStateValue value{OperStateValue::Unknown};
  friend bool operator==(const OperStateReading&, const OperStateReading&) noexcept = default;
};

/// A cumulative counter sample. The width and the source reset flag are part of
/// the reading because they change how a discontinuity must be interpreted.
struct CounterReading {
  std::uint64_t raw{0};
  CounterWidth width{CounterWidth::Bits64};
  bool reset_declared{false};
  friend bool operator==(const CounterReading&, const CounterReading&) noexcept = default;
};

/// A ratio reported directly by a source, in parts per billion.
struct RatioReading {
  std::uint64_t ppb{0};
  friend bool operator==(const RatioReading&, const RatioReading&) noexcept = default;
};

/// A normalized quality indicator (higher is better) in parts per billion.
struct QualityReading {
  std::uint64_t value_ppb{0};
  bool degraded_declared{false};
  friend bool operator==(const QualityReading&, const QualityReading&) noexcept = default;
};

/// A source reported flap count over a source declared window.
struct FlapReading {
  std::uint64_t events{0};
  Duration window{};
  friend bool operator==(const FlapReading&, const FlapReading&) noexcept = default;
};

using MetricPayload =
    std::variant<AdminStateReading, OperStateReading, CounterReading, RatioReading, QualityReading,
                 FlapReading>;

struct MetricKey {
  MetricFamily family{MetricFamily::AdminState};
  Direction direction{Direction::Unknown};
  ErrorClass error_class{ErrorClass::None};

  friend bool operator==(const MetricKey&, const MetricKey&) noexcept = default;
  friend auto operator<=>(const MetricKey&, const MetricKey&) noexcept = default;

  /// Canonical spelling: "<family>[/<direction>][/<error-class>]". The optional
  /// parts are omitted exactly when they carry no information, so the spelling
  /// is a function of the key.
  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] static Result<MetricKey> parse(std::string_view text);
};

/// True when the payload alternative belongs to the family.
[[nodiscard]] bool payload_matches_family(MetricFamily family,
                                          const MetricPayload& payload) noexcept;

/// Validates a reading against its key, the policy bounds and the accepted
/// value ranges. Rejects rather than clamps: a clamped value would be a
/// fabricated value.
[[nodiscard]] Status validate_reading(const MetricKey& key,
                                      const MetricPayload& payload,
                                      const Policy& policy);

/// Deterministic textual rendering of a reading, used by every report.
[[nodiscard]] std::string format_payload(const MetricPayload& payload);

/// Numeric comparison used for conflict detection. Returns false when the two
/// readings are of different alternatives, which is itself a conflict.
[[nodiscard]] bool payloads_are_comparable(const MetricPayload& lhs,
                                           const MetricPayload& rhs) noexcept;

/// Absolute difference of two readings that are known to be comparable.
/// Returns false when no numeric difference is defined for the alternative.
[[nodiscard]] bool payload_difference_ppb(const MetricPayload& lhs,
                                          const MetricPayload& rhs,
                                          std::uint64_t& out) noexcept;

}  // namespace linkobs
