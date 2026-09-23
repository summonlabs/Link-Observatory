// Link Observatory - deterministic link classification.
//
// Classification is a pure function of (policy, link facts, evidence, history,
// explicit instant). No clock is read directly, no iteration order is
// unspecified, and no floating point is used, so the same inputs always produce
// the same state, the same rule trace and the same explanation bytes.
//
// The runtime reports what the evidence supports. It never converts absent,
// stale, superseded or incomplete evidence into a positive claim, and it never
// infers a failure from a missing sample.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "linkobs/domain/link.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/store/evidence_store.hpp"
#include "linkobs/store/history.hpp"
#include "linkobs/store/link_registry.hpp"

namespace linkobs {

/// Stable rule identifiers. They are part of the explanation format and are
/// never renumbered.
enum class RuleId : std::uint32_t {
  NoEvidence = 800,
  SourceConflict = 200,
  OperDown = 100,
  AdminDisabled = 110,
  Flapping = 300,
  SourceFlapReport = 310,
  Erroring = 400,
  SaturatedDerived = 500,
  SaturatedReported = 510,
  DegradedQuality = 600,
  DegradedDiscards = 610,
  DegradedOperState = 620,
  UnsupportedDecisive = 810,
  IncompleteDecisive = 820,
  StaleEvidence = 700,
  Healthy = 900,
};

[[nodiscard]] std::string_view rule_name(RuleId rule) noexcept;

struct RuleEvaluation {
  RuleId rule{RuleId::NoEvidence};
  bool fired{false};
  /// Deterministic, bounded detail string. Empty when the rule did not fire or
  /// has nothing to add.
  std::string detail{};
};

/// What the runtime concluded about one metric family.
struct FamilyReport {
  MetricKey key{};
  SupportState support{SupportState::Unsupported};
  Freshness freshness{Freshness::Unknown};
  Confidence confidence{Confidence::Unknown};
  Completeness completeness{Completeness::Unknown};
  ConflictState conflict{ConflictState::None};
  std::size_t contenders{0};
  std::size_t sources{0};
  std::uint64_t accepted{0};
  std::uint64_t fenced{0};
  bool has_value{false};
  bool eligible{false};
  /// True when a delta between two consecutive counter samples was derived.
  bool has_delta{false};
  /// True when a utilization was derived. This is stricter than has_delta: it
  /// also requires a declared capacity and a policy-valid window.
  bool has_derived{false};
  std::uint64_t value_ppb{0};
  std::uint64_t derived_ppb{0};
  std::uint64_t derived_bps{0};
  DerivationFault derivation_fault{DerivationFault::None};
  SourceId winner_source{};
  ProvenanceClass winner_provenance{ProvenanceClass::Unknown};
  TimePoint winner_observed_at{};
  TimePoint winner_received_at{};
  std::string value_text{};
  std::uint64_t reset_count{0};
  std::uint64_t wrap_count{0};
  /// Error or discard volume over the derivation window, when derivable.
  bool has_ratio{false};
  std::uint64_t ratio_ppb{0};
};

struct Classification {
  LinkId link{};
  LinkState state{LinkState::Unknown};
  LinkState last_known_state{LinkState::Unknown};
  bool has_last_known{false};
  TimePoint decided_at{};
  GenerationId generation{};
  std::uint64_t generation_changes{0};
  bool retired{false};
  Freshness freshness{Freshness::Unknown};
  Confidence confidence{Confidence::Unknown};
  Completeness completeness{Completeness::Unknown};
  SupportState support{SupportState::Unsupported};
  bool any_evidence{false};
  bool any_eligible{false};
  bool decisive_unsupported{false};
  bool decisive_incomplete{false};
  std::vector<FamilyReport> families{};
  std::vector<RuleEvaluation> rules{};
  std::uint64_t accepted_total{0};
  std::uint64_t fenced_total{0};
  std::uint64_t flap_events{0};
  std::size_t transitions{0};
  std::uint64_t dropped_transitions{0};

  [[nodiscard]] const FamilyReport* find(MetricFamily family,
                                         Direction direction) const noexcept;
};

struct ClassificationInput {
  LinkId link{};
  const LinkEntry* entry{nullptr};
  GenerationId current_generation{};
  bool has_generation{false};
  std::uint64_t generation_changes{0};
  const Policy* policy{nullptr};
  const EvidenceStore* store{nullptr};
  const TransitionLog* transitions{nullptr};
  TimePoint now{};
};

/// Evaluates every rule, then selects the state by fixed precedence.
[[nodiscard]] Classification classify(const ClassificationInput& input);

/// Compares two classifications for the purpose of transition recording.
/// Returns true when the reported state or the decisive values changed.
[[nodiscard]] bool classification_changed(const Classification& previous,
                                          const Classification& current) noexcept;

}  // namespace linkobs
