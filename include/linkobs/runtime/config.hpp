// Link Observatory - runtime configuration.
//
// Configuration is explicit, validated and bounded. There is no configuration
// file format with implicit defaults hidden in code: a configuration that is not
// valid is rejected with a reason.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "linkobs/core/status.hpp"
#include "linkobs/domain/enums.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/ingest/record.hpp"

namespace linkobs {

/// Which metric families the runtime accepts. Disabled families are fenced at
/// ingest with an explicit reason rather than silently ignored.
struct FamilyMask {
  std::uint32_t enabled{0xFFFFFFFFU};

  void disable(MetricFamily family) noexcept {
    enabled &= ~(1U << static_cast<std::uint32_t>(family));
  }
  void enable(MetricFamily family) noexcept {
    enabled |= (1U << static_cast<std::uint32_t>(family));
  }
  [[nodiscard]] bool is_enabled(MetricFamily family) const noexcept {
    return (enabled & (1U << static_cast<std::uint32_t>(family))) != 0U;
  }
};

struct RuntimeConfig {
  /// Path of the snapshot file used for persistence. Empty disables persistence.
  std::string snapshot_path{};
  /// When true the runtime loads an existing snapshot at start.
  bool restore_on_start{true};
  /// When true the runtime writes a snapshot on clean shutdown.
  bool persist_on_stop{true};
  /// Number of ingest workers. Zero selects a single inline (synchronous) worker.
  std::size_t workers{1U};
  /// Depth of each ingest queue.
  std::size_t queue_depth{1024U};
  /// Identity of this runtime instance. Deterministic when supplied.
  std::string runtime_name{"link-observatory"};
  /// Identity of this process incarnation. Supplied by the caller so that
  /// restarts are distinguishable without reading a clock.
  std::string incarnation_name{"incarnation-0"};
  FamilyMask families{};
  Policy policy{};
};

[[nodiscard]] Status validate(const RuntimeConfig& config);

/// Deterministic identifiers derived from the configuration.
[[nodiscard]] RuntimeId runtime_id_of(const RuntimeConfig& config);
[[nodiscard]] IncarnationId incarnation_id_of(const RuntimeConfig& config);

/// Counters that describe what the runtime did, for reports and benchmarks.
struct RuntimeStatistics {
  std::uint64_t records_submitted{0};
  std::uint64_t records_accepted{0};
  std::uint64_t records_fenced{0};
  std::uint64_t declarations_applied{0};
  std::uint64_t declarations_rejected{0};
  std::uint64_t batches_processed{0};
  std::uint64_t transitions_recorded{0};
  std::uint64_t classifications{0};
  std::uint64_t state_changes{0};
  std::uint64_t snapshot_writes{0};
  std::uint64_t snapshot_reads{0};
  std::uint64_t recoveries{0};
  std::uint64_t cancellations{0};
  std::uint64_t abandoned_batches{0};
};

}  // namespace linkobs
