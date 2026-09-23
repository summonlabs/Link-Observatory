// Link Observatory - observations and their provenance.
//
// An observation is never a bare number. It always answers: what was observed,
// by which source, for which topology generation, at what observation time, at
// what receive time, under which source incarnation and epoch, at which revision
// of that source stream, and with what declared provenance class.

#pragma once

#include <string>
#include <string_view>

#include "linkobs/core/strong_id.hpp"
#include "linkobs/domain/metric.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/time/clock.hpp"

namespace linkobs {

/// The full provenance of one observation.
struct Provenance {
  SourceId source{};
  IncarnationId incarnation{};
  EpochId epoch{};
  GenerationId generation{};
  Sequence sequence{};

  friend bool operator==(const Provenance&, const Provenance&) noexcept = default;
};

/// The scope in which a source sequence number is meaningful.
///
/// Sequence numbers from different scopes are never compared: a new incarnation
/// or a new epoch restarts the stream, and treating those numbers as continuing
/// would fabricate ordering that does not exist.
struct SequenceScope {
  SourceId source{};
  IncarnationId incarnation{};
  EpochId epoch{};

  friend bool operator==(const SequenceScope&, const SequenceScope&) noexcept = default;
  friend auto operator<=>(const SequenceScope&, const SequenceScope&) noexcept = default;
};

[[nodiscard]] SequenceScope scope_of(const Provenance& provenance) noexcept;

/// Locally configured properties of a telemetry source.
///
/// Authority and provenance class come from registration, never from the
/// payload, so an untrusted source cannot promote itself by asserting a value.
struct SourceRecord {
  SourceId id{};
  std::string name{};
  SourceAuthority authority{SourceAuthority::Unknown};
  ProvenanceClass provenance{ProvenanceClass::Unknown};
  bool registered{false};
  bool retired{false};
};

struct Observation {
  ObservationId id{};
  Provenance provenance{};
  LinkId link{};
  MetricKey key{};
  MetricPayload payload{};
  TimePoint observed_at{};
  TimePoint received_at{};

  friend bool operator==(const Observation&, const Observation&) noexcept = default;
};

/// Content address of an observation.
///
/// The identifier covers every field except the identifier itself, so two
/// observations that differ in any way - including timestamps, revision or
/// generation - have different identities.
[[nodiscard]] ObservationId compute_observation_id(const Observation& observation);

/// Returns a copy of the observation with its identity filled in.
[[nodiscard]] Observation with_identity(Observation observation);

/// Structural validation that does not depend on stored state.
[[nodiscard]] Status validate_observation(const Observation& observation, const Policy& policy);

/// Deterministic one-line rendering used by history and export.
[[nodiscard]] std::string format_observation(const Observation& observation);

}  // namespace linkobs
