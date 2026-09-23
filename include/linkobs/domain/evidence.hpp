// Link Observatory - accepted evidence and how it is aged and trusted.
//
// Evidence is what the runtime is willing to reason about: an observation that
// passed validation and fencing. Freshness is always computed against an
// explicit "now" and never stored as a durable property, which is precisely why
// a restart cannot make old evidence fresh again.

#pragma once

#include <cstdint>
#include <string>

#include "linkobs/domain/enums.hpp"
#include "linkobs/domain/observation.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/time/clock.hpp"

namespace linkobs {

/// An accepted observation plus the local acceptance metadata.
///
/// Authority and provenance class are captured at acceptance time, not looked up
/// later: evidence keeps the standing it was admitted under, so re-registering a
/// source cannot retroactively upgrade history that was already recorded.
struct Evidence {
  Observation observation{};
  TimePoint accepted_at{};
  SourceAuthority authority{SourceAuthority::Unknown};
  ProvenanceClass provenance{ProvenanceClass::Unknown};

  friend bool operator==(const Evidence&, const Evidence&) noexcept = default;
};

struct AgeReport {
  Duration observed_age{};
  Duration received_age{};
  /// True when the observation is dated ahead of the evaluation instant, which
  /// is treated as suspect clock behaviour rather than as extra freshness.
  bool backwards_clock{false};
};

/// Ages a piece of evidence against an explicit instant.
[[nodiscard]] AgeReport age_evidence(const Evidence& evidence, TimePoint now) noexcept;

/// Deterministic freshness classification.
///
/// Received age dominates: evidence that is old by observation time but was just
/// received is still not current, because the runtime cannot know what happened
/// between the two instants.
[[nodiscard]] Freshness classify_freshness(const AgeReport& age,
                                           const FreshnessPolicy& policy) noexcept;

/// Deterministic confidence derivation from authority, freshness, conflict and
/// completeness. Documented in docs/confidence.md.
[[nodiscard]] Confidence derive_confidence(SourceAuthority authority,
                                           Freshness freshness,
                                           ConflictState conflict,
                                           Completeness completeness) noexcept;

/// Identity of a stored evidence record: (link, key, source, incarnation, epoch,
/// revision). Two records with the same identity are the same evidence.
[[nodiscard]] EvidenceId compute_evidence_id(const Observation& observation);

/// Deterministic rendering of one evidence record.
[[nodiscard]] std::string format_evidence(const Evidence& evidence,
                                          Freshness freshness,
                                          Confidence confidence);

}  // namespace linkobs
