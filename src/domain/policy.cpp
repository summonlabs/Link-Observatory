#include "linkobs/domain/policy.hpp"

namespace linkobs {
namespace {

constexpr std::uint64_t kFullScalePpb = 1000000000ULL;

[[nodiscard]] FreshnessPolicy make_freshness(std::int64_t fresh_seconds,
                                             std::int64_t usable_seconds) noexcept {
  FreshnessPolicy policy{};
  policy.fresh_within = Duration::from_seconds(fresh_seconds);
  policy.usable_within = Duration::from_seconds(usable_seconds);
  return policy;
}

[[nodiscard]] Status require(bool condition, std::string_view message) {
  if (condition) {
    return Status::success();
  }
  return Status::of(StatusCode::InvalidArgument, message);
}

}  // namespace

Policy make_default_policy() {
  Policy policy{};
  policy.freshness[static_cast<std::size_t>(MetricFamily::AdminState)] = make_freshness(30, 300);
  policy.freshness[static_cast<std::size_t>(MetricFamily::OperState)] = make_freshness(30, 300);
  policy.freshness[static_cast<std::size_t>(MetricFamily::Octets)] = make_freshness(30, 300);
  policy.freshness[static_cast<std::size_t>(MetricFamily::Errors)] = make_freshness(60, 600);
  policy.freshness[static_cast<std::size_t>(MetricFamily::Discards)] = make_freshness(60, 600);
  policy.freshness[static_cast<std::size_t>(MetricFamily::UtilReported)] = make_freshness(30, 300);
  policy.freshness[static_cast<std::size_t>(MetricFamily::SignalQuality)] = make_freshness(60, 600);
  policy.freshness[static_cast<std::size_t>(MetricFamily::FlapReport)] = make_freshness(120, 1200);

  policy.thresholds.saturation_ppb = 900000000ULL;
  policy.thresholds.degraded_quality_ppb = 500000000ULL;
  policy.thresholds.error_ratio_ppb = 1000000ULL;
  policy.thresholds.discard_ratio_ppb = 10000000ULL;
  policy.thresholds.conflict_tolerance_ppb = 50000000ULL;
  policy.thresholds.flap_threshold = 4U;
  policy.thresholds.flap_window = Duration::from_seconds(60);
  policy.thresholds.derivation_window = Duration::from_seconds(300);
  policy.thresholds.max_history_window = Duration::from_seconds(3600);
  return policy;
}

Status validate(const Policy& policy) {
  for (std::size_t index = 0; index < kMetricFamilyCount; ++index) {
    const FreshnessPolicy& entry = policy.freshness[index];
    if (entry.fresh_within.nanos() <= 0) {
      return Status::of(StatusCode::InvalidArgument, "freshness fresh window must be positive");
    }
    if (entry.usable_within.nanos() <= entry.fresh_within.nanos()) {
      return Status::of(StatusCode::InvalidArgument,
                        "freshness usable window must exceed the fresh window");
    }
  }

  const StateThresholds& thresholds = policy.thresholds;
  if (thresholds.saturation_ppb == 0U || thresholds.saturation_ppb > kFullScalePpb) {
    return Status::of(StatusCode::InvalidArgument, "saturation threshold must be in (0, 100%]");
  }
  if (thresholds.degraded_quality_ppb > kFullScalePpb) {
    return Status::of(StatusCode::InvalidArgument, "degraded quality threshold must be <= 100%");
  }
  if (thresholds.conflict_tolerance_ppb > kFullScalePpb) {
    return Status::of(StatusCode::InvalidArgument, "conflict tolerance must be <= 100%");
  }
  if (thresholds.flap_threshold == 0U) {
    return Status::of(StatusCode::InvalidArgument, "flap threshold must be at least one");
  }
  if (thresholds.flap_window.nanos() <= 0) {
    return Status::of(StatusCode::InvalidArgument, "flap window must be positive");
  }
  if (thresholds.derivation_window.nanos() <= 0) {
    return Status::of(StatusCode::InvalidArgument, "derivation window must be positive");
  }
  if (thresholds.max_history_window.nanos() < thresholds.derivation_window.nanos()) {
    return Status::of(StatusCode::InvalidArgument,
                      "history window must not be shorter than the derivation window");
  }

  const Limits& limits = policy.limits;
  const Status links = require(limits.max_links > 0U, "max_links must be positive");
  if (!links.ok()) {
    return links;
  }
  const Status sources = require(limits.max_sources > 0U, "max_sources must be positive");
  if (!sources.ok()) {
    return sources;
  }
  const Status evidence = require(limits.max_evidence_per_key > 0U,
                                  "max_evidence_per_key must be positive");
  if (!evidence.ok()) {
    return evidence;
  }
  const Status contenders = require(limits.max_contenders_per_key > 0U,
                                    "max_contenders_per_key must be positive");
  if (!contenders.ok()) {
    return contenders;
  }
  const Status transitions = require(limits.max_transitions_per_link > 0U,
                                     "max_transitions_per_link must be positive");
  if (!transitions.ok()) {
    return transitions;
  }
  const Status rows = require(limits.max_result_rows > 0U, "max_result_rows must be positive");
  if (!rows.ok()) {
    return rows;
  }
  const Status record = require(limits.max_record_bytes > 0U, "max_record_bytes must be positive");
  if (!record.ok()) {
    return record;
  }
  const Status batch = require(limits.max_records_per_batch > 0U,
                               "max_records_per_batch must be positive");
  if (!batch.ok()) {
    return batch;
  }
  const Status queue = require(limits.max_ingest_queue_depth > 0U,
                               "max_ingest_queue_depth must be positive");
  if (!queue.ok()) {
    return queue;
  }
  const Status workers = require(limits.max_workers > 0U, "max_workers must be positive");
  if (!workers.ok()) {
    return workers;
  }
  const Status snapshot = require(limits.max_snapshot_bytes > 0U,
                                  "max_snapshot_bytes must be positive");
  if (!snapshot.ok()) {
    return snapshot;
  }
  const Status payload = require(limits.max_transport_payload_bytes > 0U,
                                 "max_transport_payload_bytes must be positive");
  if (!payload.ok()) {
    return payload;
  }
  if (limits.max_total_transitions < limits.max_transitions_per_link) {
    return Status::of(StatusCode::InvalidArgument,
                      "max_total_transitions must not be smaller than the per-link bound");
  }
  return Status::success();
}

const FreshnessPolicy& freshness_policy_for(const Policy& policy, MetricFamily family) noexcept {
  const std::size_t index = static_cast<std::size_t>(family);
  if (index >= kMetricFamilyCount) {
    return policy.freshness[0];
  }
  return policy.freshness[index];
}

}  // namespace linkobs
