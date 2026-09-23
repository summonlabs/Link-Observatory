#include "linkobs/domain/enums.hpp"

#include <array>
#include <utility>

namespace linkobs {
namespace {

template <class Enum, std::size_t N>
[[nodiscard]] bool parse_from_table(
    std::string_view text,
    const std::array<std::pair<std::string_view, Enum>, N>& table,
    Enum& out) noexcept {
  for (const auto& entry : table) {
    if (entry.first == text) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

constexpr std::array<std::pair<std::string_view, MetricFamily>, kMetricFamilyCount>
    kMetricFamilyNames{{
        {"admin-state", MetricFamily::AdminState},
        {"oper-state", MetricFamily::OperState},
        {"octets", MetricFamily::Octets},
        {"errors", MetricFamily::Errors},
        {"discards", MetricFamily::Discards},
        {"util-reported", MetricFamily::UtilReported},
        {"signal-quality", MetricFamily::SignalQuality},
        {"flap-report", MetricFamily::FlapReport},
    }};

constexpr std::array<std::pair<std::string_view, Direction>, kDirectionCount> kDirectionNames{{
    {"unknown", Direction::Unknown},
    {"in", Direction::In},
    {"out", Direction::Out},
    {"both", Direction::Both},
}};

constexpr std::array<std::pair<std::string_view, ErrorClass>, kErrorClassCount> kErrorClassNames{{
    {"none", ErrorClass::None},         {"total", ErrorClass::Total},
    {"crc", ErrorClass::Crc},           {"symbol", ErrorClass::Symbol},
    {"framing", ErrorClass::Framing},   {"encoding", ErrorClass::Encoding},
    {"congestion", ErrorClass::Congestion}, {"other", ErrorClass::Other},
}};

}  // namespace

std::string_view to_string(MetricFamily value) noexcept {
  switch (value) {
    case MetricFamily::AdminState:
      return "admin-state";
    case MetricFamily::OperState:
      return "oper-state";
    case MetricFamily::Octets:
      return "octets";
    case MetricFamily::Errors:
      return "errors";
    case MetricFamily::Discards:
      return "discards";
    case MetricFamily::UtilReported:
      return "util-reported";
    case MetricFamily::SignalQuality:
      return "signal-quality";
    case MetricFamily::FlapReport:
      return "flap-report";
  }
  return "invalid";
}

std::string_view to_string(Direction value) noexcept {
  switch (value) {
    case Direction::Unknown:
      return "unknown";
    case Direction::In:
      return "in";
    case Direction::Out:
      return "out";
    case Direction::Both:
      return "both";
  }
  return "invalid";
}

std::string_view to_string(ErrorClass value) noexcept {
  switch (value) {
    case ErrorClass::None:
      return "none";
    case ErrorClass::Total:
      return "total";
    case ErrorClass::Crc:
      return "crc";
    case ErrorClass::Symbol:
      return "symbol";
    case ErrorClass::Framing:
      return "framing";
    case ErrorClass::Encoding:
      return "encoding";
    case ErrorClass::Congestion:
      return "congestion";
    case ErrorClass::Other:
      return "other";
  }
  return "invalid";
}

std::string_view to_string(CounterWidth value) noexcept {
  switch (value) {
    case CounterWidth::Bits32:
      return "bits32";
    case CounterWidth::Bits64:
      return "bits64";
  }
  return "invalid";
}

std::string_view to_string(LinkState value) noexcept {
  switch (value) {
    case LinkState::Unknown:
      return "unknown";
    case LinkState::Healthy:
      return "healthy";
    case LinkState::Degraded:
      return "degraded";
    case LinkState::Down:
      return "down";
    case LinkState::Flapping:
      return "flapping";
    case LinkState::Saturated:
      return "saturated";
    case LinkState::Erroring:
      return "erroring";
    case LinkState::Stale:
      return "stale";
    case LinkState::Conflicting:
      return "conflicting";
  }
  return "invalid";
}

std::string_view to_string(Freshness value) noexcept {
  switch (value) {
    case Freshness::Unknown:
      return "unknown";
    case Freshness::Fresh:
      return "fresh";
    case Freshness::Stale:
      return "stale";
    case Freshness::Expired:
      return "expired";
  }
  return "invalid";
}

std::string_view to_string(Confidence value) noexcept {
  switch (value) {
    case Confidence::Unknown:
      return "unknown";
    case Confidence::Low:
      return "low";
    case Confidence::Medium:
      return "medium";
    case Confidence::High:
      return "high";
  }
  return "invalid";
}

std::string_view to_string(SupportState value) noexcept {
  switch (value) {
    case SupportState::Unsupported:
      return "unsupported";
    case SupportState::Supported:
      return "supported";
  }
  return "invalid";
}

std::string_view to_string(Completeness value) noexcept {
  switch (value) {
    case Completeness::Unknown:
      return "unknown";
    case Completeness::Complete:
      return "complete";
    case Completeness::Incomplete:
      return "incomplete";
  }
  return "invalid";
}

std::string_view to_string(ConflictState value) noexcept {
  switch (value) {
    case ConflictState::None:
      return "none";
    case ConflictState::Corroborated:
      return "corroborated";
    case ConflictState::Conflicting:
      return "conflicting";
    case ConflictState::Insufficient:
      return "insufficient";
  }
  return "invalid";
}

std::string_view to_string(SourceAuthority value) noexcept {
  switch (value) {
    case SourceAuthority::Unknown:
      return "unknown";
    case SourceAuthority::Synthetic:
      return "synthetic";
    case SourceAuthority::Derived:
      return "derived";
    case SourceAuthority::Secondary:
      return "secondary";
    case SourceAuthority::Primary:
      return "primary";
  }
  return "invalid";
}

std::string_view to_string(ProvenanceClass value) noexcept {
  switch (value) {
    case ProvenanceClass::Unknown:
      return "unknown";
    case ProvenanceClass::Real:
      return "real";
    case ProvenanceClass::Synthetic:
      return "synthetic";
    case ProvenanceClass::Replayed:
      return "replayed";
  }
  return "invalid";
}

std::string_view to_string(TransitionKind value) noexcept {
  switch (value) {
    case TransitionKind::LinkRegistered:
      return "link-registered";
    case TransitionKind::LinkRetired:
      return "link-retired";
    case TransitionKind::GenerationChanged:
      return "generation-changed";
    case TransitionKind::AdminStateChanged:
      return "admin-state-changed";
    case TransitionKind::OperStateChanged:
      return "oper-state-changed";
    case TransitionKind::ClassificationChanged:
      return "classification-changed";
    case TransitionKind::CounterReset:
      return "counter-reset";
    case TransitionKind::CounterWrap:
      return "counter-wrap";
    case TransitionKind::SourceEpochChanged:
      return "source-epoch-changed";
    case TransitionKind::SourceIncarnationChanged:
      return "source-incarnation-changed";
    case TransitionKind::RevisionGap:
      return "revision-gap";
    case TransitionKind::StaleEvidence:
      return "stale-evidence";
  }
  return "invalid";
}

std::string_view to_string(FenceReason value) noexcept {
  switch (value) {
    case FenceReason::None:
      return "none";
    case FenceReason::UnknownSource:
      return "unknown-source";
    case FenceReason::UnknownLink:
      return "unknown-link";
    case FenceReason::UnsupportedFamily:
      return "unsupported-family";
    case FenceReason::InvalidValue:
      return "invalid-value";
    case FenceReason::DuplicateObservation:
      return "duplicate-observation";
    case FenceReason::RevisionReplay:
      return "revision-replay";
    case FenceReason::RevisionGap:
      return "revision-gap";
    case FenceReason::EpochBehind:
      return "epoch-behind";
    case FenceReason::IncarnationBehind:
      return "incarnation-behind";
    case FenceReason::GenerationBehind:
      return "generation-behind";
    case FenceReason::AuthorityDowngrade:
      return "authority-downgrade";
    case FenceReason::PayloadTooLarge:
      return "payload-too-large";
    case FenceReason::UnsupportedProvenance:
      return "unsupported-provenance";
    case FenceReason::StoreFull:
      return "store-full";
  }
  return "invalid";
}

std::string_view to_string(LinkKind value) noexcept {
  switch (value) {
    case LinkKind::Unknown:
      return "unknown";
    case LinkKind::Physical:
      return "physical";
    case LinkKind::Logical:
      return "logical";
    case LinkKind::Aggregate:
      return "aggregate";
  }
  return "invalid";
}

std::string_view to_string(CounterDiscontinuity value) noexcept {
  switch (value) {
    case CounterDiscontinuity::None:
      return "none";
    case CounterDiscontinuity::Reset:
      return "reset";
    case CounterDiscontinuity::Wrap:
      return "wrap";
  }
  return "invalid";
}

std::string_view to_string(DerivationFault value) noexcept {
  switch (value) {
    case DerivationFault::None:
      return "none";
    case DerivationFault::MissingCapacity:
      return "missing-capacity";
    case DerivationFault::MissingPreviousSample:
      return "missing-previous-sample";
    case DerivationFault::CounterReset:
      return "counter-reset";
    case DerivationFault::NonMonotonicTime:
      return "non-monotonic-time";
    case DerivationFault::WindowTooLarge:
      return "window-too-large";
    case DerivationFault::StaleSample:
      return "stale-sample";
    case DerivationFault::FencedSample:
      return "fenced-sample";
    case DerivationFault::RevisionGap:
      return "revision-gap";
    case DerivationFault::Overflow:
      return "overflow";
    case DerivationFault::DirectionNotDerivable:
      return "direction-not-derivable";
  }
  return "invalid";
}

bool parse_metric_family(std::string_view text, MetricFamily& out) noexcept {
  return parse_from_table(text, kMetricFamilyNames, out);
}

bool parse_direction(std::string_view text, Direction& out) noexcept {
  return parse_from_table(text, kDirectionNames, out);
}

bool parse_error_class(std::string_view text, ErrorClass& out) noexcept {
  return parse_from_table(text, kErrorClassNames, out);
}

bool parse_counter_width(std::string_view text, CounterWidth& out) noexcept {
  if (text == "bits32") {
    out = CounterWidth::Bits32;
    return true;
  }
  if (text == "bits64") {
    out = CounterWidth::Bits64;
    return true;
  }
  return false;
}

bool parse_link_state(std::string_view text, LinkState& out) noexcept {
  static constexpr std::array<std::pair<std::string_view, LinkState>, 9> kNames{{
      {"unknown", LinkState::Unknown},         {"healthy", LinkState::Healthy},
      {"degraded", LinkState::Degraded},       {"down", LinkState::Down},
      {"flapping", LinkState::Flapping},       {"saturated", LinkState::Saturated},
      {"erroring", LinkState::Erroring},       {"stale", LinkState::Stale},
      {"conflicting", LinkState::Conflicting},
  }};
  return parse_from_table(text, kNames, out);
}

bool parse_freshness(std::string_view text, Freshness& out) noexcept {
  static constexpr std::array<std::pair<std::string_view, Freshness>, 4> kNames{{
      {"unknown", Freshness::Unknown},
      {"fresh", Freshness::Fresh},
      {"stale", Freshness::Stale},
      {"expired", Freshness::Expired},
  }};
  return parse_from_table(text, kNames, out);
}

bool parse_confidence(std::string_view text, Confidence& out) noexcept {
  static constexpr std::array<std::pair<std::string_view, Confidence>, 4> kNames{{
      {"unknown", Confidence::Unknown},
      {"low", Confidence::Low},
      {"medium", Confidence::Medium},
      {"high", Confidence::High},
  }};
  return parse_from_table(text, kNames, out);
}

bool parse_support_state(std::string_view text, SupportState& out) noexcept {
  if (text == "unsupported") {
    out = SupportState::Unsupported;
    return true;
  }
  if (text == "supported") {
    out = SupportState::Supported;
    return true;
  }
  return false;
}

bool parse_completeness(std::string_view text, Completeness& out) noexcept {
  if (text == "unknown") {
    out = Completeness::Unknown;
    return true;
  }
  if (text == "complete") {
    out = Completeness::Complete;
    return true;
  }
  if (text == "incomplete") {
    out = Completeness::Incomplete;
    return true;
  }
  return false;
}

bool parse_conflict_state(std::string_view text, ConflictState& out) noexcept {
  if (text == "none") {
    out = ConflictState::None;
    return true;
  }
  if (text == "corroborated") {
    out = ConflictState::Corroborated;
    return true;
  }
  if (text == "conflicting") {
    out = ConflictState::Conflicting;
    return true;
  }
  if (text == "insufficient") {
    out = ConflictState::Insufficient;
    return true;
  }
  return false;
}

bool parse_source_authority(std::string_view text, SourceAuthority& out) noexcept {
  static constexpr std::array<std::pair<std::string_view, SourceAuthority>, 5> kNames{{
      {"unknown", SourceAuthority::Unknown},   {"synthetic", SourceAuthority::Synthetic},
      {"derived", SourceAuthority::Derived},   {"secondary", SourceAuthority::Secondary},
      {"primary", SourceAuthority::Primary},
  }};
  return parse_from_table(text, kNames, out);
}

bool parse_provenance_class(std::string_view text, ProvenanceClass& out) noexcept {
  static constexpr std::array<std::pair<std::string_view, ProvenanceClass>, 4> kNames{{
      {"unknown", ProvenanceClass::Unknown},
      {"real", ProvenanceClass::Real},
      {"synthetic", ProvenanceClass::Synthetic},
      {"replayed", ProvenanceClass::Replayed},
  }};
  return parse_from_table(text, kNames, out);
}

bool parse_transition_kind(std::string_view text, TransitionKind& out) noexcept {
  static constexpr std::array<std::pair<std::string_view, TransitionKind>, 12> kNames{{
      {"link-registered", TransitionKind::LinkRegistered},
      {"link-retired", TransitionKind::LinkRetired},
      {"generation-changed", TransitionKind::GenerationChanged},
      {"admin-state-changed", TransitionKind::AdminStateChanged},
      {"oper-state-changed", TransitionKind::OperStateChanged},
      {"classification-changed", TransitionKind::ClassificationChanged},
      {"counter-reset", TransitionKind::CounterReset},
      {"counter-wrap", TransitionKind::CounterWrap},
      {"source-epoch-changed", TransitionKind::SourceEpochChanged},
      {"source-incarnation-changed", TransitionKind::SourceIncarnationChanged},
      {"revision-gap", TransitionKind::RevisionGap},
      {"stale-evidence", TransitionKind::StaleEvidence},
  }};
  return parse_from_table(text, kNames, out);
}

bool parse_fence_reason(std::string_view text, FenceReason& out) noexcept {
  static constexpr std::array<std::pair<std::string_view, FenceReason>, 15> kNames{{
      {"none", FenceReason::None},
      {"unknown-source", FenceReason::UnknownSource},
      {"unknown-link", FenceReason::UnknownLink},
      {"unsupported-family", FenceReason::UnsupportedFamily},
      {"invalid-value", FenceReason::InvalidValue},
      {"duplicate-observation", FenceReason::DuplicateObservation},
      {"revision-replay", FenceReason::RevisionReplay},
      {"revision-gap", FenceReason::RevisionGap},
      {"epoch-behind", FenceReason::EpochBehind},
      {"incarnation-behind", FenceReason::IncarnationBehind},
      {"generation-behind", FenceReason::GenerationBehind},
      {"authority-downgrade", FenceReason::AuthorityDowngrade},
      {"payload-too-large", FenceReason::PayloadTooLarge},
      {"unsupported-provenance", FenceReason::UnsupportedProvenance},
      {"store-full", FenceReason::StoreFull},
  }};
  return parse_from_table(text, kNames, out);
}

bool parse_link_kind(std::string_view text, LinkKind& out) noexcept {
  static constexpr std::array<std::pair<std::string_view, LinkKind>, 4> kNames{{
      {"unknown", LinkKind::Unknown},
      {"physical", LinkKind::Physical},
      {"logical", LinkKind::Logical},
      {"aggregate", LinkKind::Aggregate},
  }};
  return parse_from_table(text, kNames, out);
}

bool parse_counter_discontinuity(std::string_view text, CounterDiscontinuity& out) noexcept {
  if (text == "none") {
    out = CounterDiscontinuity::None;
    return true;
  }
  if (text == "reset") {
    out = CounterDiscontinuity::Reset;
    return true;
  }
  if (text == "wrap") {
    out = CounterDiscontinuity::Wrap;
    return true;
  }
  return false;
}

bool parse_derivation_fault(std::string_view text, DerivationFault& out) noexcept {
  static constexpr std::array<std::pair<std::string_view, DerivationFault>, 11> kNames{{
      {"none", DerivationFault::None},
      {"missing-capacity", DerivationFault::MissingCapacity},
      {"missing-previous-sample", DerivationFault::MissingPreviousSample},
      {"counter-reset", DerivationFault::CounterReset},
      {"non-monotonic-time", DerivationFault::NonMonotonicTime},
      {"window-too-large", DerivationFault::WindowTooLarge},
      {"stale-sample", DerivationFault::StaleSample},
      {"fenced-sample", DerivationFault::FencedSample},
      {"revision-gap", DerivationFault::RevisionGap},
      {"overflow", DerivationFault::Overflow},
      {"direction-not-derivable", DerivationFault::DirectionNotDerivable},
  }};
  return parse_from_table(text, kNames, out);
}

}  // namespace linkobs
