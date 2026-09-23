// Link Observatory - deterministic policy and resource bounds.
//
// Policy is data, not behaviour hidden in code: freshness windows, thresholds,
// conflict tolerance and every resource bound live here and are supplied
// explicitly. Two runs with the same policy and the same evidence produce the
// same classification and the same explanation bytes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "linkobs/core/status.hpp"
#include "linkobs/domain/enums.hpp"
#include "linkobs/time/clock.hpp"

namespace linkobs {

/// Freshness windows for one metric family.
///
/// "fresh" means the value may be reported as current. "usable" is the outer
/// bound: beyond it the value is Expired and may only be read as history. The
/// gap between the two is the Stale band, which never yields a positive claim.
struct FreshnessPolicy {
  Duration fresh_within{};
  Duration usable_within{};
};

/// Hard bounds on everything that can grow.
///
/// These are enforced at ingest, at derivation, at query and at persistence.
/// Exceeding a bound is an explicit, reported failure; nothing is silently
/// dropped, truncated or grown without limit.
struct Limits {
  std::size_t max_links{4096U};
  std::size_t max_sources{256U};
  std::size_t max_evidence_per_key{16U};
  std::size_t max_contenders_per_key{8U};
  std::size_t max_transitions_per_link{256U};
  std::size_t max_total_transitions{65536U};
  std::size_t max_result_rows{4096U};
  std::size_t max_record_bytes{4096U};
  std::size_t max_records_per_batch{4096U};
  std::size_t max_ingest_queue_depth{8192U};
  std::size_t max_workers{16U};
  std::size_t max_snapshot_bytes{64U * 1024U * 1024U};
  std::size_t max_transport_payload_bytes{1024U * 1024U};
  std::size_t max_transport_connections{16U};
  std::size_t max_explanation_steps{64U};
};

/// Decision thresholds. All ratios are integer parts-per-billion so that
/// classification never depends on floating point rounding.
struct StateThresholds {
  /// Reported utilization at or above this is Saturated.
  std::uint64_t saturation_ppb{900000000ULL};
  /// Reported signal quality at or below this is a degradation indicator.
  std::uint64_t degraded_quality_ppb{500000000ULL};
  /// Errors per octet at or above this is Erroring.
  std::uint64_t error_ratio_ppb{1000000ULL};
  /// Relative disagreement tolerated between sources before Conflicting.
  std::uint64_t conflict_tolerance_ppb{50000000ULL};
  /// Discards per octet at or above this is Degraded (not Erroring).
  std::uint64_t discard_ratio_ppb{10000000ULL};
  /// Number of observed state transitions within flap_window that is Flapping.
  std::uint32_t flap_threshold{4U};
  /// Window in which transitions are counted.
  Duration flap_window{};
  /// Longest span two samples may cover and still yield a derived rate.
  Duration derivation_window{};
  /// Longest span a history query may cover.
  Duration max_history_window{};
};

struct Policy {
  std::array<FreshnessPolicy, kMetricFamilyCount> freshness{};
  StateThresholds thresholds{};
  Limits limits{};
  /// When false (the default) an observation for an unregistered link is
  /// refused. When true the runtime creates a placeholder link with no declared
  /// capacity, so such a link can never produce a derived utilization.
  bool allow_implicit_links{false};
};

/// Explicit, documented defaults. Every value is a deliberate choice, not a
/// hidden constant: see docs/policy.md for the rationale of each number.
[[nodiscard]] Policy make_default_policy();

/// Rejects nonsensical policy: zero windows, inverted freshness bounds,
/// thresholds above 100%, unusable limits.
[[nodiscard]] Status validate(const Policy& policy);

[[nodiscard]] const FreshnessPolicy& freshness_policy_for(const Policy& policy,
                                                          MetricFamily family) noexcept;

}  // namespace linkobs
