#include "linkobs/classify/classifier.hpp"

#include <algorithm>
#include <map>

#include "linkobs/core/text.hpp"

namespace linkobs {
namespace {

constexpr MetricKey kOperKey{MetricFamily::OperState, Direction::Unknown, ErrorClass::None};
constexpr MetricKey kAdminKey{MetricFamily::AdminState, Direction::Unknown, ErrorClass::None};
constexpr MetricKey kOctetsKey{MetricFamily::Octets, Direction::In, ErrorClass::None};

[[nodiscard]] bool higher_priority(const EvaluatedEvidence& lhs, const EvaluatedEvidence& rhs) {
  if (lhs.evidence.authority != rhs.evidence.authority) {
    return lhs.evidence.authority > rhs.evidence.authority;
  }
  if (!(lhs.evidence.observation.observed_at == rhs.evidence.observation.observed_at)) {
    return lhs.evidence.observation.observed_at > rhs.evidence.observation.observed_at;
  }
  if (!(lhs.evidence.observation.received_at == rhs.evidence.observation.received_at)) {
    return lhs.evidence.observation.received_at > rhs.evidence.observation.received_at;
  }
  return lhs.evidence.observation.provenance.source < rhs.evidence.observation.provenance.source;
}

/// Highest authority authority among eligible contenders, or Unknown.
[[nodiscard]] SourceAuthority top_authority(const std::vector<EvaluatedEvidence>& contenders) {
  SourceAuthority best = SourceAuthority::Unknown;
  for (const auto& candidate : contenders) {
    if (candidate.eligible && candidate.evidence.authority > best) {
      best = candidate.evidence.authority;
    }
  }
  return best;
}

[[nodiscard]] const EvaluatedEvidence* select_winner(
    const std::vector<EvaluatedEvidence>& contenders) {
  const EvaluatedEvidence* best = nullptr;
  for (const auto& candidate : contenders) {
    if (!candidate.eligible) {
      continue;
    }
    if (best == nullptr || higher_priority(candidate, *best)) {
      best = &candidate;
    }
  }
  return best;
}

[[nodiscard]] std::uint64_t relative_difference_ppb(std::uint64_t lhs, std::uint64_t rhs) {
  const std::uint64_t difference = lhs > rhs ? lhs - rhs : rhs - lhs;
  const std::uint64_t scale = lhs > rhs ? lhs : rhs;
  if (scale == 0U) {
    return 0U;
  }
  std::uint64_t relative = 0;
  if (!ratio_ppb(difference, scale, relative)) {
    return 1000000000ULL;
  }
  return relative;
}

/// True when two readings of the same key contradict each other.
[[nodiscard]] bool readings_disagree(const MetricPayload& lhs,
                                     const MetricPayload& rhs,
                                     std::uint64_t tolerance_ppb) {
  if (const auto* left = std::get_if<AdminStateReading>(&lhs)) {
    const auto* right = std::get_if<AdminStateReading>(&rhs);
    return right != nullptr && left->value != right->value &&
           left->value != AdminStateValue::Unknown && right->value != AdminStateValue::Unknown;
  }
  if (const auto* left = std::get_if<OperStateReading>(&lhs)) {
    const auto* right = std::get_if<OperStateReading>(&rhs);
    return right != nullptr && left->value != right->value &&
           left->value != OperStateValue::Unknown && right->value != OperStateValue::Unknown;
  }
  std::uint64_t difference = 0;
  if (!payload_difference_ppb(lhs, rhs, difference)) {
    return false;
  }
  std::uint64_t left_value = 0;
  std::uint64_t right_value = 0;
  if (const auto* ratio = std::get_if<RatioReading>(&lhs)) {
    left_value = ratio->ppb;
  } else if (const auto* quality = std::get_if<QualityReading>(&lhs)) {
    left_value = quality->value_ppb;
  }
  if (const auto* ratio = std::get_if<RatioReading>(&rhs)) {
    right_value = ratio->ppb;
  } else if (const auto* quality = std::get_if<QualityReading>(&rhs)) {
    right_value = quality->value_ppb;
  }
  return relative_difference_ppb(left_value, right_value) > tolerance_ppb;
}

struct Derived {
  bool has_delta{false};
  std::uint64_t delta{0};
  Duration window{};
  DerivationFault fault{DerivationFault::MissingPreviousSample};
  bool has_utilization{false};
  std::uint64_t utilization_ppb{0};
};

[[nodiscard]] bool sample_usable(const CounterSample& sample,
                                 TimePoint now,
                                 const FreshnessPolicy& freshness,
                                 const StreamState* stream,
                                 GenerationId current_generation,
                                 bool has_generation) {
  if (stream == nullptr || !stream->has_scope || !(sample.scope == stream->scope)) {
    return false;
  }
  if (has_generation && !(sample.generation == current_generation)) {
    return false;
  }
  return classify_sample_freshness(sample, now, freshness) != Freshness::Expired;
}

[[nodiscard]] Derived derive_for(const EvidenceStore& store,
                                 const LinkFacts& facts,
                                 LinkId link,
                                 const MetricKey& key,
                                 SourceId source,
                                 Direction direction,
                                 const Policy& policy,
                                 TimePoint now,
                                 GenerationId current_generation,
                                 bool has_generation) {
  Derived derived{};
  const CounterContinuity* continuity = store.continuity(link, key, source);
  if (continuity == nullptr || !continuity->has_latest || !continuity->has_previous) {
    derived.fault = DerivationFault::MissingPreviousSample;
    return derived;
  }
  const StreamState* stream = store.stream_state(source);
  const FreshnessPolicy freshness = freshness_policy_for(policy, key.family);

  DerivationRequest request{};
  request.previous = continuity->previous;
  request.current = continuity->latest;
  request.has_previous = true;
  request.previous_usable = sample_usable(continuity->previous, now, freshness, stream,
                                          current_generation, has_generation);
  request.current_usable = sample_usable(continuity->latest, now, freshness, stream,
                                         current_generation, has_generation);
  request.max_window = policy.thresholds.derivation_window;

  const DeltaResult delta = derive_delta(request);
  if (!delta.valid) {
    derived.fault = delta.fault;
    return derived;
  }
  derived.has_delta = true;
  derived.delta = delta.delta;
  derived.window = delta.window;
  derived.fault = DerivationFault::None;

  std::uint64_t capacity = 0;
  if (!capacity_for(facts, direction, capacity)) {
    derived.fault = DerivationFault::MissingCapacity;
    return derived;
  }
  request.capacity_bps = capacity;
  const DerivationResult utilization = derive_utilization(request);
  if (utilization.valid) {
    derived.has_utilization = true;
    derived.utilization_ppb = utilization.utilization_ppb;
  } else {
    derived.fault = utilization.fault;
  }
  return derived;
}

[[nodiscard]] bool is_decisive_family(MetricFamily family) noexcept {
  return family != MetricFamily::FlapReport;
}

[[nodiscard]] Confidence weakest(Confidence lhs, Confidence rhs) noexcept {
  return static_cast<std::uint8_t>(lhs) <= static_cast<std::uint8_t>(rhs) ? lhs : rhs;
}

[[nodiscard]] Freshness strongest(Freshness lhs, Freshness rhs) noexcept {
  const auto rank = [](Freshness value) -> int {
    switch (value) {
      case Freshness::Fresh:
        return 3;
      case Freshness::Stale:
        return 2;
      case Freshness::Expired:
        return 1;
      case Freshness::Unknown:
        return 0;
    }
    return 0;
  };
  return rank(lhs) >= rank(rhs) ? lhs : rhs;
}

}  // namespace

std::string_view rule_name(RuleId rule) noexcept {
  switch (rule) {
    case RuleId::NoEvidence:
      return "no-evidence";
    case RuleId::SourceConflict:
      return "source-conflict";
    case RuleId::OperDown:
      return "oper-down";
    case RuleId::AdminDisabled:
      return "admin-disabled";
    case RuleId::Flapping:
      return "flapping";
    case RuleId::SourceFlapReport:
      return "source-flap-report";
    case RuleId::Erroring:
      return "erroring";
    case RuleId::SaturatedDerived:
      return "saturated-derived";
    case RuleId::SaturatedReported:
      return "saturated-reported";
    case RuleId::DegradedQuality:
      return "degraded-quality";
    case RuleId::DegradedDiscards:
      return "degraded-discards";
    case RuleId::DegradedOperState:
      return "degraded-oper-state";
    case RuleId::UnsupportedDecisive:
      return "unsupported-decisive";
    case RuleId::IncompleteDecisive:
      return "incomplete-decisive";
    case RuleId::StaleEvidence:
      return "stale-evidence";
    case RuleId::Healthy:
      return "healthy";
  }
  return "unknown-rule";
}

const FamilyReport* Classification::find(MetricFamily family, Direction direction) const noexcept {
  for (const auto& report : families) {
    if (report.key.family == family && report.key.direction == direction) {
      return &report;
    }
  }
  return nullptr;
}

Classification classify(const ClassificationInput& input) {
  Classification out{};
  out.link = input.link;
  out.decided_at = input.now;
  out.generation = input.current_generation;
  out.generation_changes = input.generation_changes;
  out.retired = input.entry != nullptr && input.entry->retired;
  if (input.policy == nullptr || input.store == nullptr) {
    return out;
  }
  const Policy& policy = *input.policy;
  const EvidenceStore& store = *input.store;

  LinkFacts facts{};
  if (input.entry != nullptr) {
    facts = input.entry->facts;
    out.accepted_total = input.entry->accepted_observations;
    out.fenced_total = input.entry->fenced_observations;
  }

  std::vector<MetricKey> keys = store.keys_for(input.link);
  const auto ensure_key = [&keys](const MetricKey& key) {
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
      keys.push_back(key);
    }
  };
  ensure_key(kOperKey);
  ensure_key(kAdminKey);
  // The primary traffic family is always reported, so that "no capacity" and
  // "no traffic evidence" are visible as Unsupported rather than simply absent.
  ensure_key(kOctetsKey);
  std::sort(keys.begin(), keys.end());

  EvaluationContext context{};
  context.now = input.now;
  context.current_generation = input.current_generation;
  context.has_generation = input.current_generation.valid();

  std::map<MetricKey, std::vector<EvaluatedEvidence>> contenders_by_key;
  std::map<MetricKey, Derived> derived_by_key;
  std::map<MetricKey, MetricPayload> payload_by_key;

  const auto derived_for_key = [&](const MetricKey& key, SourceId source) {
    return derive_for(store, facts, input.link, key, source, key.direction, policy, input.now,
                      input.current_generation, input.current_generation.valid());
  };

  for (const MetricKey& key : keys) {
    FamilyReport report{};
    report.key = key;
    const auto view = store.slot_view(input.link, key, input.now);
    if (view.has_value()) {
      report.accepted = view->accepted;
      report.fenced = view->fenced;
      report.completeness = view->completeness;
      report.support = view->accepted > 0U ? SupportState::Supported : SupportState::Unsupported;
    }

    std::vector<EvaluatedEvidence> contenders = store.contenders(input.link, key, context);
    report.contenders = contenders.size();

    std::vector<SourceId> sources;
    for (const auto& candidate : contenders) {
      const SourceId source = candidate.evidence.observation.provenance.source;
      if (std::find(sources.begin(), sources.end(), source) == sources.end()) {
        sources.push_back(source);
      }
      report.freshness = strongest(report.freshness, candidate.freshness);
      if (candidate.eligible) {
        out.any_eligible = true;
      }
    }
    report.sources = sources.size();
    if (!contenders.empty()) {
      out.any_evidence = true;
    }

    const EvaluatedEvidence* winner = select_winner(contenders);
    if (winner != nullptr) {
      const Evidence& evidence = winner->evidence;
      report.has_value = true;
      report.eligible = true;
      report.winner_source = evidence.observation.provenance.source;
      report.winner_provenance = evidence.provenance;
      report.winner_observed_at = evidence.observation.observed_at;
      report.winner_received_at = evidence.observation.received_at;
      report.value_text = format_payload(evidence.observation.payload);
      if (const auto* ratio = std::get_if<RatioReading>(&evidence.observation.payload)) {
        report.value_ppb = ratio->ppb;
      } else if (const auto* quality =
                     std::get_if<QualityReading>(&evidence.observation.payload)) {
        report.value_ppb = quality->value_ppb;
      }
      payload_by_key.emplace(key, evidence.observation.payload);
      if (is_counter_family(key.family)) {
        const Derived derived = derived_for_key(key, report.winner_source);
        report.has_delta = derived.has_delta;
        report.has_derived = derived.has_utilization;
        report.derived_ppb = derived.has_utilization ? derived.utilization_ppb : 0U;
        report.derivation_fault = derived.fault;
        derived_by_key.emplace(key, derived);
        if (const CounterContinuity* continuity =
                store.continuity(input.link, key, report.winner_source)) {
          report.reset_count = continuity->reset_count;
          report.wrap_count = continuity->wrap_count;
        }
      }
    }

    const SourceAuthority authority = top_authority(contenders);
    if (report.has_value && authority != SourceAuthority::Unknown) {
      std::vector<const EvaluatedEvidence*> top;
      for (const auto& candidate : contenders) {
        if (candidate.eligible && candidate.evidence.authority == authority) {
          top.push_back(&candidate);
        }
      }
      if (top.size() < 2U) {
        report.conflict = ConflictState::Insufficient;
      } else {
        bool conflicting = false;
        bool compared = false;
        for (std::size_t lhs = 0; lhs < top.size() && !conflicting; ++lhs) {
          for (std::size_t rhs = lhs + 1U; rhs < top.size(); ++rhs) {
            const SourceId lhs_source = top[lhs]->evidence.observation.provenance.source;
            const SourceId rhs_source = top[rhs]->evidence.observation.provenance.source;
            if (lhs_source == rhs_source) {
              continue;
            }
            compared = true;
            if (is_counter_family(key.family)) {
              const auto left = derived_by_key.find(key);
              const Derived right = derived_for_key(key, rhs_source);
              if (left == derived_by_key.end() || !left->second.has_utilization ||
                  !right.has_utilization) {
                continue;
              }
              if (relative_difference_ppb(left->second.utilization_ppb, right.utilization_ppb) >
                  policy.thresholds.conflict_tolerance_ppb) {
                conflicting = true;
              }
            } else if (readings_disagree(top[lhs]->evidence.observation.payload,
                                         top[rhs]->evidence.observation.payload,
                                         policy.thresholds.conflict_tolerance_ppb)) {
              conflicting = true;
            }
          }
        }
        report.conflict = conflicting ? ConflictState::Conflicting
                                      : (compared ? ConflictState::Corroborated
                                                  : ConflictState::None);
      }
    }

    if (report.has_value && winner != nullptr) {
      report.confidence = derive_confidence(winner->evidence.authority, winner->freshness,
                                            report.conflict, report.completeness);
    }

    contenders_by_key.emplace(key, std::move(contenders));
    out.families.push_back(std::move(report));
  }

  const auto payload_of = [&payload_by_key](const MetricKey& key) -> const MetricPayload* {
    const auto found = payload_by_key.find(key);
    return found == payload_by_key.end() ? nullptr : &found->second;
  };
  const auto report_of = [&out](const MetricKey& key) -> const FamilyReport* {
    for (const auto& report : out.families) {
      if (report.key == key) {
        return &report;
      }
    }
    return nullptr;
  };

  const FamilyReport* oper = report_of(kOperKey);
  const FamilyReport* admin = report_of(kAdminKey);
  const bool oper_supported = oper != nullptr && oper->support == SupportState::Supported;
  const bool oper_eligible = oper != nullptr && oper->eligible;
  const bool admin_eligible = admin != nullptr && admin->eligible;

  OperStateValue oper_value = OperStateValue::Unknown;
  if (const MetricPayload* payload = oper_eligible ? payload_of(kOperKey) : nullptr) {
    if (const auto* reading = std::get_if<OperStateReading>(payload)) {
      oper_value = reading->value;
    }
  }
  AdminStateValue admin_value = AdminStateValue::Unknown;
  if (const MetricPayload* payload = admin_eligible ? payload_of(kAdminKey) : nullptr) {
    if (const auto* reading = std::get_if<AdminStateReading>(payload)) {
      admin_value = reading->value;
    }
  }

  bool any_conflict = false;
  std::string conflict_detail;
  for (const auto& report : out.families) {
    if (report.conflict == ConflictState::Conflicting) {
      any_conflict = true;
      if (conflict_detail.empty()) {
        conflict_detail = report.key.to_string();
      }
    }
  }

  // Flap counting uses observed link transitions only, never classification
  // churn, so a changing opinion cannot make a link look like it is flapping.
  std::size_t flap_events = 0;
  if (input.transitions != nullptr) {
    const Checked<TimePoint> start =
        advance(input.now, Duration::from_nanos(-policy.thresholds.flap_window.nanos()));
    const TimePoint window_start = start.ok ? start.value : TimePoint{};
    for (const auto& event :
         input.transitions->for_link(input.link, policy.limits.max_result_rows)) {
      if (event.at < window_start) {
        continue;
      }
      if (event.kind == TransitionKind::AdminStateChanged ||
          event.kind == TransitionKind::OperStateChanged) {
        ++flap_events;
      }
    }
    out.transitions = input.transitions->events_for(input.link);
    out.dropped_transitions = input.transitions->dropped_events();
  }
  out.flap_events = flap_events;

  std::uint64_t reported_flap_events = 0;
  bool reported_flaps_eligible = false;
  for (const auto& report : out.families) {
    if (report.key.family != MetricFamily::FlapReport || !report.has_value) {
      continue;
    }
    const MetricPayload* payload = payload_of(report.key);
    if (payload == nullptr) {
      continue;
    }
    if (const auto* reading = std::get_if<FlapReading>(payload)) {
      reported_flaps_eligible = true;
      if (reading->events > reported_flap_events) {
        reported_flap_events = reading->events;
      }
    }
  }

  bool reported_util_eligible = false;
  std::uint64_t reported_util_ppb = 0;
  bool reported_util_present = false;
  for (const auto& report : out.families) {
    if (report.key.family != MetricFamily::UtilReported || !report.has_value) {
      continue;
    }
    reported_util_present = true;
    if (report.eligible) {
      reported_util_eligible = true;
      if (report.value_ppb > reported_util_ppb) {
        reported_util_ppb = report.value_ppb;
      }
    }
  }

  bool derived_util_present = false;
  bool derived_util_eligible = false;
  std::uint64_t derived_util_ppb = 0;
  DerivationFault derived_fault = DerivationFault::MissingPreviousSample;
  for (const auto& report : out.families) {
    if (report.key.family != MetricFamily::Octets || !report.has_value) {
      continue;
    }
    const auto found = derived_by_key.find(report.key);
    if (found == derived_by_key.end()) {
      continue;
    }
    if (found->second.has_utilization) {
      derived_util_present = true;
      derived_util_eligible = true;
      if (found->second.utilization_ppb > derived_util_ppb) {
        derived_util_ppb = found->second.utilization_ppb;
      }
      derived_fault = DerivationFault::None;
    } else if (!derived_util_present) {
      derived_fault = found->second.fault;
    }
  }

  bool error_ratio_present = false;
  std::uint64_t error_ratio = 0;
  bool discard_ratio_present = false;
  std::uint64_t discard_ratio = 0;
  {
    std::uint64_t octet_delta = 0;
    Duration octet_window{};
    bool octet_ok = false;
    for (const auto& report : out.families) {
      if (report.key.family != MetricFamily::Octets || report.key.direction != Direction::In ||
          !report.has_value) {
        continue;
      }
      const auto found = derived_by_key.find(report.key);
      if (found != derived_by_key.end() && found->second.has_delta) {
        octet_delta = found->second.delta;
        octet_window = found->second.window;
        octet_ok = true;
        break;
      }
    }
    for (auto& report : out.families) {
      const bool is_error = report.key.family == MetricFamily::Errors;
      const bool is_discard = report.key.family == MetricFamily::Discards;
      if (!is_error && !is_discard) {
        continue;
      }
      const auto found = derived_by_key.find(report.key);
      if (found == derived_by_key.end() || !found->second.has_delta) {
        continue;
      }
      std::uint64_t ratio = 0;
      const bool ok = octet_ok && octet_window == found->second.window &&
                      ratio_ppb(found->second.delta, octet_delta, ratio);
      if (!ok) {
        continue;
      }
      report.has_ratio = true;
      report.ratio_ppb = ratio;
      if (is_error) {
        error_ratio_present = true;
        error_ratio = ratio;
      } else {
        discard_ratio_present = true;
        discard_ratio = ratio;
      }
    }
  }

  bool quality_degraded = false;
  std::uint64_t worst_quality = 0;
  bool quality_present = false;
  for (const auto& report : out.families) {
    if (report.key.family != MetricFamily::SignalQuality || !report.has_value || !report.eligible) {
      continue;
    }
    const MetricPayload* payload = payload_of(report.key);
    if (payload == nullptr) {
      continue;
    }
    if (const auto* reading = std::get_if<QualityReading>(payload)) {
      quality_present = true;
      if (worst_quality == 0U || reading->value_ppb < worst_quality) {
        worst_quality = reading->value_ppb;
      }
      if (reading->degraded_declared) {
        quality_degraded = true;
      }
    }
  }
  if (quality_present && worst_quality <= policy.thresholds.degraded_quality_ppb) {
    quality_degraded = true;
  }

  bool decisive_incomplete = false;
  bool decisive_unsupported = false;
  for (const auto& report : out.families) {
    if (!is_decisive_family(report.key.family) || report.key.direction == Direction::Both) {
      continue;
    }
    // Only the operational state is required for a positive claim. Traffic and
    // error counters may legitimately not be configured for a link, so their
    // absence is reported as Unsupported rather than treated as a fault.
    const bool required = report.key.family == MetricFamily::OperState;
    if (report.support == SupportState::Unsupported) {
      if (required) {
        decisive_unsupported = true;
      }
      continue;
    }
    if (report.completeness == Completeness::Incomplete) {
      decisive_incomplete = true;
    }
  }

  const bool no_evidence = !out.any_evidence;
  const bool oper_down = oper_eligible && oper_value == OperStateValue::Down;
  const bool admin_disabled = admin_eligible && admin_value == AdminStateValue::Disabled;
  const bool flapping = flap_events >= policy.thresholds.flap_threshold;
  const bool source_flapping =
      reported_flaps_eligible && reported_flap_events >= policy.thresholds.flap_threshold;
  const bool erroring = error_ratio_present && error_ratio >= policy.thresholds.error_ratio_ppb;
  const bool saturated_reported =
      reported_util_eligible && reported_util_ppb >= policy.thresholds.saturation_ppb;
  const bool saturated_derived =
      derived_util_eligible && derived_util_ppb >= policy.thresholds.saturation_ppb;
  const bool degraded_discards =
      discard_ratio_present && discard_ratio >= policy.thresholds.discard_ratio_ppb;
  const bool degraded_oper = oper_eligible && oper_value == OperStateValue::Degraded;
  const bool stale_evidence = out.any_evidence && !out.any_eligible;
  const bool healthy = oper_eligible && oper_value == OperStateValue::Up && !any_conflict &&
                       !erroring && !saturated_reported && !saturated_derived &&
                       !quality_degraded && !degraded_discards && !degraded_oper && !flapping &&
                       !source_flapping && !decisive_incomplete && !decisive_unsupported;

  const auto add_rule = [&out](RuleId rule, bool fired, std::string detail) {
    RuleEvaluation evaluation{};
    evaluation.rule = rule;
    evaluation.fired = fired;
    evaluation.detail = std::move(detail);
    out.rules.push_back(std::move(evaluation));
  };

  add_rule(RuleId::NoEvidence, no_evidence,
           no_evidence ? std::string{"no accepted evidence"} : std::string{});
  add_rule(RuleId::SourceConflict, any_conflict, conflict_detail);
  add_rule(RuleId::OperDown, oper_down, oper_down ? std::string{"operational state down"} : "");
  add_rule(RuleId::AdminDisabled, admin_disabled,
           admin_disabled ? std::string{"administrative state disabled"} : std::string{});
  add_rule(RuleId::Flapping, flapping,
           flapping ? std::string{"observed transitions="} + to_dec(flap_events) : std::string{});
  add_rule(RuleId::SourceFlapReport, source_flapping,
           source_flapping ? std::string{"reported flap events="} + to_dec(reported_flap_events)
                           : std::string{});
  add_rule(RuleId::Erroring, erroring,
           erroring ? std::string{"error ratio ppb="} + to_dec(error_ratio) : std::string{});
  add_rule(RuleId::SaturatedDerived, saturated_derived,
           saturated_derived ? std::string{"derived utilization ppb="} + to_dec(derived_util_ppb)
                             : std::string{});
  add_rule(RuleId::SaturatedReported, saturated_reported,
           saturated_reported
               ? std::string{"reported utilization ppb="} + to_dec(reported_util_ppb)
               : std::string{});
  add_rule(RuleId::DegradedQuality, quality_degraded,
           quality_degraded ? std::string{"quality ppb="} + to_dec(worst_quality) +
                                  " threshold ppb=" +
                                  to_dec(policy.thresholds.degraded_quality_ppb)
                            : std::string{});
  add_rule(RuleId::DegradedDiscards, degraded_discards,
           degraded_discards ? std::string{"discard ratio ppb="} + to_dec(discard_ratio)
                             : std::string{});
  add_rule(RuleId::DegradedOperState, degraded_oper,
           degraded_oper ? std::string{"operational state degraded"} : std::string{});
  add_rule(RuleId::UnsupportedDecisive, decisive_unsupported,
           decisive_unsupported ? std::string{"a required metric family has no source"}
                                : std::string{});
  add_rule(RuleId::IncompleteDecisive, decisive_incomplete,
           decisive_incomplete ? std::string{"revision gap within the derivation window; "} +
                                     "derivation-fault=" + std::string{to_string(derived_fault)}
                               : std::string{});
  add_rule(RuleId::StaleEvidence, stale_evidence,
           stale_evidence ? std::string{"accepted evidence is no longer usable"} : std::string{});
  add_rule(RuleId::Healthy, healthy,
           healthy ? std::string{"operational state up, no adverse indicator"} : std::string{});

  std::sort(out.rules.begin(), out.rules.end(),
            [](const RuleEvaluation& lhs, const RuleEvaluation& rhs) {
              return static_cast<std::uint32_t>(lhs.rule) < static_cast<std::uint32_t>(rhs.rule);
            });

  if (no_evidence) {
    out.state = LinkState::Unknown;
  } else if (any_conflict) {
    out.state = LinkState::Conflicting;
  } else if (oper_down || admin_disabled) {
    out.state = LinkState::Down;
  } else if (flapping || source_flapping) {
    out.state = LinkState::Flapping;
  } else if (erroring) {
    out.state = LinkState::Erroring;
  } else if (saturated_reported || saturated_derived) {
    out.state = LinkState::Saturated;
  } else if (quality_degraded || degraded_discards || degraded_oper) {
    out.state = LinkState::Degraded;
  } else if (decisive_unsupported || decisive_incomplete) {
    out.state = LinkState::Unknown;
  } else if (stale_evidence) {
    out.state = LinkState::Stale;
  } else if (healthy) {
    out.state = LinkState::Healthy;
  } else {
    out.state = LinkState::Unknown;
  }

  out.decisive_incomplete = decisive_incomplete;
  out.decisive_unsupported = decisive_unsupported;
  out.support = oper_supported ? SupportState::Supported : SupportState::Unsupported;
  if (!out.any_evidence) {
    out.freshness = Freshness::Unknown;
    out.completeness = Completeness::Unknown;
  } else {
    // The link level freshness is the best freshness any family still has, so an
    // expired runtime says expired rather than reporting a vague "stale".
    bool any_fresh = false;
    bool any_stale = false;
    bool any_expired = false;
    // Every family that has ever held evidence contributes, whether or not it
    // still has an eligible winner: expired evidence is exactly the case where
    // there is no eligible winner but the age is still known.
    for (const auto& report : out.families) {
      any_fresh = any_fresh || report.freshness == Freshness::Fresh;
      any_stale = any_stale || report.freshness == Freshness::Stale;
      any_expired = any_expired || report.freshness == Freshness::Expired;
    }
    if (any_fresh) {
      out.freshness = Freshness::Fresh;
    } else if (any_stale) {
      out.freshness = Freshness::Stale;
    } else if (any_expired) {
      out.freshness = Freshness::Expired;
    } else {
      out.freshness = Freshness::Unknown;
    }
    out.completeness = decisive_incomplete ? Completeness::Incomplete : Completeness::Complete;
  }

  Confidence aggregate = Confidence::Unknown;
  bool have_confidence = false;
  for (const auto& report : out.families) {
    if (!is_decisive_family(report.key.family) || !report.has_value) {
      continue;
    }
    aggregate = have_confidence ? weakest(aggregate, report.confidence) : report.confidence;
    have_confidence = true;
  }
  out.confidence = have_confidence ? aggregate : Confidence::Unknown;

  if (input.transitions != nullptr) {
    const std::vector<TransitionEvent> history =
        input.transitions->for_link(input.link, policy.limits.max_result_rows);
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      if (it->kind != TransitionKind::ClassificationChanged) {
        continue;
      }
      LinkState parsed{};
      if (parse_link_state(it->to, parsed)) {
        out.last_known_state = parsed;
        out.has_last_known = true;
      }
      break;
    }
  }
  (void)reported_util_present;
  return out;
}

bool classification_changed(const Classification& previous,
                            const Classification& current) noexcept {
  return previous.state != current.state;
}

}  // namespace linkobs
