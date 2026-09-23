// Link Observatory - domain enumerations.
//
// Every enumerated value has a stable lowercase textual spelling. Those
// spellings are part of the ingest record grammar, the snapshot encoding and
// every report, so they are frozen: a spelling is never reused for a different
// meaning, and unknown spellings are rejected instead of defaulted.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace linkobs {

// ---------------------------------------------------------------------------
// Metric identity
// ---------------------------------------------------------------------------

/// A family of link telemetry. Freshness, confidence, thresholds and conflict
/// tolerance are all evaluated per family, never globally.
enum class MetricFamily : std::uint8_t {
  AdminState = 0,
  OperState = 1,
  Octets = 2,
  Errors = 3,
  Discards = 4,
  UtilReported = 5,
  SignalQuality = 6,
  FlapReport = 7,
};

inline constexpr std::size_t kMetricFamilyCount = 8U;

/// Direction of a metric. Not every family is directional; a direction that a
/// family does not define is rejected rather than ignored.
enum class Direction : std::uint8_t {
  Unknown = 0,
  In = 1,
  Out = 2,
  Both = 3,
};

inline constexpr std::size_t kDirectionCount = 4U;

/// Vendor neutral error classification. The runtime never maps a vendor counter
/// name onto these classes by guessing; the mapping is supplied by the source
/// registration or the record itself.
enum class ErrorClass : std::uint8_t {
  None = 0,
  Total = 1,
  Crc = 2,
  Symbol = 3,
  Framing = 4,
  Encoding = 5,
  Congestion = 6,
  Other = 7,
};

inline constexpr std::size_t kErrorClassCount = 8U;

/// Declared width of a source counter, needed to tell a wrap from a reset.
enum class CounterWidth : std::uint8_t {
  Bits32 = 0,
  Bits64 = 1,
};

// ---------------------------------------------------------------------------
// Link state
// ---------------------------------------------------------------------------

/// The decisive, deterministic link health state.
///
/// Precedence (highest first) is fixed and documented in docs/states.md:
///   Down, Conflicting, Flapping, Erroring, Saturated, Degraded, Stale,
///   Unknown, Healthy.
/// Stale and Unknown are not failures: they are explicit admissions that the
/// runtime cannot currently justify a positive claim.
enum class LinkState : std::uint8_t {
  Unknown = 0,
  Healthy = 1,
  Degraded = 2,
  Down = 3,
  Flapping = 4,
  Saturated = 5,
  Erroring = 6,
  Stale = 7,
  Conflicting = 8,
};

/// Age classification of a single piece of evidence, relative to an explicit
/// "now".
enum class Freshness : std::uint8_t {
  Unknown = 0,
  Fresh = 1,
  Stale = 2,
  Expired = 3,
};

/// How much the runtime trusts the value it is reporting.
enum class Confidence : std::uint8_t {
  Unknown = 0,
  Low = 1,
  Medium = 2,
  High = 3,
};

/// Whether a metric family can be supplied at all by the configured sources.
/// "Unsupported" is a statement about configuration, never about a link.
enum class SupportState : std::uint8_t {
  Unsupported = 0,
  Supported = 1,
};

/// Whether the accepted evidence for a family forms an unbroken revision
/// sequence. A gap makes derived quantities unavailable, not zero.
enum class Completeness : std::uint8_t {
  Unknown = 0,
  Complete = 1,
  Incomplete = 2,
};

/// Result of comparing evidence for the same key from several sources.
enum class ConflictState : std::uint8_t {
  None = 0,
  Corroborated = 1,
  Conflicting = 2,
  Insufficient = 3,
};

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

/// Ordered source authority. Higher wins when two sources disagree. The value
/// comes from local source registration, never from the payload, so a source
/// cannot promote itself.
enum class SourceAuthority : std::uint8_t {
  Unknown = 0,
  Synthetic = 1,
  Derived = 2,
  Secondary = 3,
  Primary = 4,
};

/// What kind of evidence a source supplies. This is a label on the proof
/// surface, not a quality ranking: REAL and SYNTHETIC evidence are both valid
/// input, and the runtime never presents SYNTHETIC evidence as REAL.
enum class ProvenanceClass : std::uint8_t {
  Unknown = 0,
  Real = 1,
  Synthetic = 2,
  Replayed = 3,
};

// ---------------------------------------------------------------------------
// Transitions and fencing
// ---------------------------------------------------------------------------

enum class TransitionKind : std::uint8_t {
  LinkRegistered = 0,
  LinkRetired = 1,
  GenerationChanged = 2,
  AdminStateChanged = 3,
  OperStateChanged = 4,
  ClassificationChanged = 5,
  CounterReset = 6,
  CounterWrap = 7,
  SourceEpochChanged = 8,
  SourceIncarnationChanged = 9,
  RevisionGap = 10,
  StaleEvidence = 11,
};

/// Why an observation was refused. Refusals are recorded and counted; they
/// never mutate current evidence.
enum class FenceReason : std::uint8_t {
  None = 0,
  UnknownSource = 1,
  UnknownLink = 2,
  UnsupportedFamily = 3,
  InvalidValue = 4,
  DuplicateObservation = 5,
  RevisionReplay = 6,
  RevisionGap = 7,
  EpochBehind = 8,
  IncarnationBehind = 9,
  GenerationBehind = 10,
  AuthorityDowngrade = 11,
  PayloadTooLarge = 12,
  UnsupportedProvenance = 13,
  StoreFull = 14,
};

/// Declared physical/logical nature of a link. Supplied by registration; never
/// inferred from the presence or absence of telemetry.
enum class LinkKind : std::uint8_t {
  Unknown = 0,
  Physical = 1,
  Logical = 2,
  Aggregate = 3,
};

/// How a counter discontinuity was classified, and therefore whether a delta
/// across it is usable.
enum class CounterDiscontinuity : std::uint8_t {
  None = 0,
  Reset = 1,
  Wrap = 2,
};

/// Reason a derived utilization value is unusable.
enum class DerivationFault : std::uint8_t {
  None = 0,
  MissingCapacity = 1,
  MissingPreviousSample = 2,
  CounterReset = 3,
  NonMonotonicTime = 4,
  WindowTooLarge = 5,
  StaleSample = 6,
  FencedSample = 7,
  RevisionGap = 8,
  Overflow = 9,
  DirectionNotDerivable = 10,
};

// ---------------------------------------------------------------------------
// Textual spellings
// ---------------------------------------------------------------------------

[[nodiscard]] std::string_view to_string(MetricFamily value) noexcept;
[[nodiscard]] std::string_view to_string(Direction value) noexcept;
[[nodiscard]] std::string_view to_string(ErrorClass value) noexcept;
[[nodiscard]] std::string_view to_string(CounterWidth value) noexcept;
[[nodiscard]] std::string_view to_string(LinkState value) noexcept;
[[nodiscard]] std::string_view to_string(Freshness value) noexcept;
[[nodiscard]] std::string_view to_string(Confidence value) noexcept;
[[nodiscard]] std::string_view to_string(SupportState value) noexcept;
[[nodiscard]] std::string_view to_string(Completeness value) noexcept;
[[nodiscard]] std::string_view to_string(ConflictState value) noexcept;
[[nodiscard]] std::string_view to_string(SourceAuthority value) noexcept;
[[nodiscard]] std::string_view to_string(ProvenanceClass value) noexcept;
[[nodiscard]] std::string_view to_string(TransitionKind value) noexcept;
[[nodiscard]] std::string_view to_string(FenceReason value) noexcept;
[[nodiscard]] std::string_view to_string(LinkKind value) noexcept;
[[nodiscard]] std::string_view to_string(CounterDiscontinuity value) noexcept;
[[nodiscard]] std::string_view to_string(DerivationFault value) noexcept;

[[nodiscard]] bool parse_metric_family(std::string_view text, MetricFamily& out) noexcept;
[[nodiscard]] bool parse_direction(std::string_view text, Direction& out) noexcept;
[[nodiscard]] bool parse_error_class(std::string_view text, ErrorClass& out) noexcept;
[[nodiscard]] bool parse_counter_width(std::string_view text, CounterWidth& out) noexcept;
[[nodiscard]] bool parse_link_state(std::string_view text, LinkState& out) noexcept;
[[nodiscard]] bool parse_freshness(std::string_view text, Freshness& out) noexcept;
[[nodiscard]] bool parse_confidence(std::string_view text, Confidence& out) noexcept;
[[nodiscard]] bool parse_support_state(std::string_view text, SupportState& out) noexcept;
[[nodiscard]] bool parse_completeness(std::string_view text, Completeness& out) noexcept;
[[nodiscard]] bool parse_conflict_state(std::string_view text, ConflictState& out) noexcept;
[[nodiscard]] bool parse_source_authority(std::string_view text, SourceAuthority& out) noexcept;
[[nodiscard]] bool parse_provenance_class(std::string_view text, ProvenanceClass& out) noexcept;
[[nodiscard]] bool parse_transition_kind(std::string_view text, TransitionKind& out) noexcept;
[[nodiscard]] bool parse_fence_reason(std::string_view text, FenceReason& out) noexcept;
[[nodiscard]] bool parse_link_kind(std::string_view text, LinkKind& out) noexcept;
[[nodiscard]] bool parse_counter_discontinuity(std::string_view text,
                                               CounterDiscontinuity& out) noexcept;
[[nodiscard]] bool parse_derivation_fault(std::string_view text, DerivationFault& out) noexcept;

/// True when the family carries a cumulative counter and can therefore be the
/// substrate of a derived rate.
[[nodiscard]] constexpr bool is_counter_family(MetricFamily family) noexcept {
  return family == MetricFamily::Octets || family == MetricFamily::Errors ||
         family == MetricFamily::Discards;
}

/// True when the family accepts a Direction other than Unknown.
[[nodiscard]] constexpr bool is_directional_family(MetricFamily family) noexcept {
  return family == MetricFamily::Octets || family == MetricFamily::Errors ||
         family == MetricFamily::Discards || family == MetricFamily::UtilReported ||
         family == MetricFamily::SignalQuality;
}

}  // namespace linkobs
