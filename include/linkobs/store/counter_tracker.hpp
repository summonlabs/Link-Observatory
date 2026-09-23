// Link Observatory - counter continuity and utilization derivation.
//
// Cumulative counters are the only source of traffic volume, and they are also
// the easiest place to fabricate data: a reset read as a delta produces a huge
// phantom burst, and a stale pair of samples produces a confident but false
// utilization. Both are refused here.
//
// Rules, in order:
//   * a sample whose declared reset flag is set starts a new baseline,
//   * a forward step (current >= previous) is the only unconditional delta,
//   * a backward step is classified as wrap only when the previous value sits in
//     the top quarter of the declared counter width, otherwise as reset,
//   * a reset never yields a delta: it yields "no derivable value",
//   * a wrap yields the exact modular delta,
//   * a baseline that is no longer usable never yields a delta,
//   * a derived rate requires a positive window no longer than the configured
//     derivation window, matching scopes and generations, and a contiguous
//     revision sequence.

#pragma once

#include <cstdint>

#include "linkobs/domain/enums.hpp"
#include "linkobs/domain/metric.hpp"
#include "linkobs/domain/observation.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/time/clock.hpp"

namespace linkobs {

/// One counter sample together with everything needed to interpret it.
struct CounterSample {
  CounterReading reading{};
  TimePoint observed_at{};
  TimePoint received_at{};
  Sequence sequence{};
  SequenceScope scope{};
  GenerationId generation{};
  /// Number of revision-stream discontinuities observed at the moment this
  /// sample was accepted. Two samples carrying the same epoch had no missing
  /// revision between them, whatever else the source interleaved. This is what
  /// makes a rate derivable from a stream that reports several metric families
  /// under one revision counter.
  std::uint64_t gap_epoch{0};
};

/// Per (source, metric key) continuity state.
///
/// Two samples are retained: the newest, and the one before it. That is the
/// minimum needed to derive a rate, and it is also the maximum, so continuity
/// state cannot grow with the length of the run.
struct CounterContinuity {
  CounterSample latest{};
  CounterSample previous{};
  bool has_latest{false};
  bool has_previous{false};
  CounterDiscontinuity last_discontinuity{CounterDiscontinuity::None};
  std::uint64_t reset_count{0};
  std::uint64_t wrap_count{0};
  std::uint64_t gap_count{0};
};

struct ContinuityOutcome {
  CounterDiscontinuity discontinuity{CounterDiscontinuity::None};
  bool delta_valid{false};
  std::uint64_t delta{0};
  bool sequence_gap{false};
  bool scope_changed{false};
  bool generation_changed{false};
  bool first_sample{false};
};

/// Highest value that still counts as "near the top" of a counter width.
///
/// A backward step from below this value is treated as a reset, which loses a
/// delta rather than inventing one. Ambiguity always resolves against the
/// fabrication of traffic.
[[nodiscard]] std::uint64_t wrap_detection_threshold(CounterWidth width) noexcept;

/// Folds one accepted sample into the continuity state and reports what happened
/// across the boundary. Must be called exactly once per accepted sample, in
/// acceptance order.
[[nodiscard]] ContinuityOutcome observe_counter(CounterContinuity& state,
                                                const CounterSample& sample) noexcept;

/// Freshness of a retained sample against an explicit instant.
[[nodiscard]] Freshness classify_sample_freshness(const CounterSample& sample,
                                                  TimePoint now,
                                                  const FreshnessPolicy& policy) noexcept;

struct DerivationRequest {
  CounterSample previous{};
  CounterSample current{};
  bool has_previous{false};
  /// False when the baseline may no longer be used (stale, superseded scope or
  /// superseded topology generation).
  bool previous_usable{true};
  bool current_usable{true};
  std::uint64_t capacity_bps{0};
  Duration max_window{};
};

/// The delta between two samples, without any capacity scaling.
struct DeltaResult {
  DerivationFault fault{DerivationFault::None};
  bool valid{false};
  std::uint64_t delta{0};
  Duration window{};
  CounterDiscontinuity discontinuity{CounterDiscontinuity::None};

  [[nodiscard]] bool usable() const noexcept { return valid; }
};

struct DerivationResult {
  DerivationFault fault{DerivationFault::None};
  bool valid{false};
  std::uint64_t delta_bytes{0};
  std::uint64_t bits_per_second{0};
  std::uint64_t utilization_ppb{0};
  Duration window{};
  CounterDiscontinuity discontinuity{CounterDiscontinuity::None};

  [[nodiscard]] bool usable() const noexcept { return valid; }
};

/// Derives a rate and a utilization from two samples of a cumulative counter.
///
/// Every failure mode produces fault != None and valid == false. There is no
/// code path that returns a zero utilization to mean "unknown".
[[nodiscard]] DeltaResult derive_delta(const DerivationRequest& request) noexcept;

[[nodiscard]] DerivationResult derive_utilization(const DerivationRequest& request) noexcept;

/// Ratio helper used by the classifier for error and discard rates, in parts per
/// billion. Returns false when the ratio is not defined.
[[nodiscard]] bool ratio_ppb(std::uint64_t numerator,
                             std::uint64_t denominator,
                             std::uint64_t& out) noexcept;

}  // namespace linkobs
