#include "linkobs/store/evidence_store.hpp"

#include <algorithm>

#include "linkobs/core/text.hpp"

namespace linkobs {
namespace {

constexpr std::size_t kMaxKeysPerLink =
    kMetricFamilyCount * kDirectionCount * kErrorClassCount;
constexpr std::size_t kMaxRetiredScopes = 32U;

}  // namespace

EvidenceStore::Slot& EvidenceStore::slot_for(LinkId link, const MetricKey& key) {
  SlotKey slot_key{link, key};
  auto found = slots_.find(slot_key);
  if (found == slots_.end()) {
    Slot slot{};
    slot.key = key;
    found = slots_.emplace(slot_key, std::move(slot)).first;
  }
  return found->second;
}

StreamState& EvidenceStore::stream_for(SourceId source) {
  return streams_[source];
}

void EvidenceStore::count_fence(FenceReason reason) {
  total_fenced_ += 1U;
  fence_counts_[reason] += 1U;
}

FenceReason EvidenceStore::fence_stream(const StreamState& state,
                                        SequenceScope scope,
                                        SourceId source) const {
  (void)source;
  const auto matches = [&scope](const SequenceScope& candidate) {
    return candidate == scope;
  };
  if (!std::any_of(state.retired_scopes.begin(), state.retired_scopes.end(), matches)) {
    return FenceReason::None;
  }
  if (scope.incarnation != state.scope.incarnation) {
    return FenceReason::IncarnationBehind;
  }
  return FenceReason::EpochBehind;
}

AcceptOutcome EvidenceStore::accept(const Observation& observation,
                                    const AcceptContext& context,
                                    TimePoint now) {
  AcceptOutcome outcome{};

  const auto fence = [this, &outcome](FenceReason reason) {
    outcome.accepted = false;
    outcome.fence = reason;
    count_fence(reason);
  };

  if (context.source == nullptr || !context.source->registered || context.source->retired) {
    fence(FenceReason::UnknownSource);
    return outcome;
  }
  if (context.source->provenance == ProvenanceClass::Unknown) {
    fence(FenceReason::UnsupportedProvenance);
    return outcome;
  }
  if (!context.family_enabled) {
    fence(FenceReason::UnsupportedFamily);
    return outcome;
  }
  if (!observation.link.valid()) {
    fence(FenceReason::UnknownLink);
    return outcome;
  }

  const Status valid = validate_observation(observation, policy_);
  if (!valid.ok()) {
    fence(valid.code() == StatusCode::Unsupported ? FenceReason::UnsupportedFamily
                                                  : FenceReason::InvalidValue);
    return outcome;
  }

  if (context.current_generation.valid() &&
      observation.provenance.generation != context.current_generation) {
    fence(FenceReason::GenerationBehind);
    return outcome;
  }

  StreamState& stream = stream_for(observation.provenance.source);
  const SequenceScope scope = scope_of(observation.provenance);
  if (!stream.has_scope) {
    stream.has_scope = true;
    stream.scope = scope;
    stream.scope_changes += 1U;
    outcome.scope_changed = true;
  } else if (!(stream.scope == scope)) {
    const FenceReason stream_fence = fence_stream(stream, scope, observation.provenance.source);
    if (stream_fence != FenceReason::None) {
      stream.fenced += 1U;
      fence(stream_fence);
      return outcome;
    }
    stream.retired_scopes.push_back(stream.scope);
    while (stream.retired_scopes.size() > kMaxRetiredScopes) {
      stream.retired_scopes.erase(stream.retired_scopes.begin());
    }
    stream.scope = scope;
    stream.has_sequence = false;
    stream.gap_open = false;
    stream.scope_changes += 1U;
    outcome.scope_changed = true;
  }

  if (stream.has_sequence) {
    if (observation.provenance.sequence == stream.last_sequence) {
      if (observation.id == stream.last_observation) {
        duplicates_ += 1U;
        outcome.duplicate = true;
        stream.fenced += 1U;
        fence(FenceReason::DuplicateObservation);
        return outcome;
      }
      stream.fenced += 1U;
      fence(FenceReason::RevisionReplay);
      return outcome;
    }
    const Checked<Sequence> expected = stream.last_sequence.next();
    if (!expected.ok || observation.provenance.sequence.value() < stream.last_sequence.value()) {
      stream.fenced += 1U;
      fence(FenceReason::RevisionReplay);
      return outcome;
    }
    if (observation.provenance.sequence.value() > expected.value.value()) {
      outcome.revision_gap = true;
      stream.gap_open = true;
      stream.gap_count += 1U;
      revision_gaps_ += 1U;
    }
  }

  const SlotKey slot_key{observation.link, observation.key};
  const auto existing = slots_.find(slot_key);
  if (existing != slots_.end()) {
    for (auto it = existing->second.history.rbegin(); it != existing->second.history.rend();
         ++it) {
      if (it->observation.provenance.generation != observation.provenance.generation) {
        continue;
      }
      if (it->authority > context.source->authority &&
          it->observation.observed_at >= observation.observed_at) {
        stream.fenced += 1U;
        fence(FenceReason::AuthorityDowngrade);
        return outcome;
      }
      break;
    }
  }

  if (slots_.size() >= policy_.limits.max_links * kMaxKeysPerLink && existing == slots_.end()) {
    fence(FenceReason::StoreFull);
    return outcome;
  }

  Slot& slot = slot_for(observation.link, observation.key);
  Evidence evidence{};
  evidence.observation = observation;
  evidence.accepted_at = now;
  evidence.authority = context.source->authority;
  evidence.provenance = context.source->provenance;

  if (is_counter_family(observation.key.family)) {
    const auto* reading = std::get_if<CounterReading>(&observation.payload);
    if (reading != nullptr) {
      CounterSample sample{};
      sample.reading = *reading;
      sample.observed_at = observation.observed_at;
      sample.received_at = observation.received_at;
      sample.sequence = observation.provenance.sequence;
      sample.scope = scope;
      sample.generation = observation.provenance.generation;
      sample.gap_epoch = stream.gap_count;
      CounterContinuity& continuity = slot.continuity[observation.provenance.source];
      const ContinuityOutcome continuity_outcome = observe_counter(continuity, sample);
      outcome.discontinuity = continuity_outcome.discontinuity;
      outcome.delta_valid = continuity_outcome.delta_valid;
      outcome.delta = continuity_outcome.delta;
      outcome.counter_width = reading->width;
      if (continuity_outcome.discontinuity == CounterDiscontinuity::Reset) {
        slot.reset_count += 1U;
        counter_resets_ += 1U;
        slot.last_gap_at = now;
        slot.has_gap = true;
      } else if (continuity_outcome.discontinuity == CounterDiscontinuity::Wrap) {
        slot.wrap_count += 1U;
        counter_wraps_ += 1U;
      }
      if (continuity_outcome.sequence_gap) {
        slot.last_gap_at = now;
        slot.has_gap = true;
      }
    }
  }

  if (outcome.revision_gap) {
    slot.last_gap_at = now;
    slot.has_gap = true;
  }

  slot.history.push_back(std::move(evidence));
  while (slot.history.size() > policy_.limits.max_evidence_per_key) {
    slot.history.erase(slot.history.begin());
  }
  slot.accepted += 1U;
  total_accepted_ += 1U;
  stream.has_sequence = true;
  stream.last_sequence = observation.provenance.sequence;
  stream.last_observation = observation.id;
  stream.accepted += 1U;

  outcome.accepted = true;
  return outcome;
}

std::vector<EvaluatedEvidence> EvidenceStore::contenders(
    LinkId link,
    const MetricKey& key,
    const EvaluationContext& context) const {
  std::vector<EvaluatedEvidence> out;
  const auto found = slots_.find(SlotKey{link, key});
  if (found == slots_.end()) {
    return out;
  }
  const FreshnessPolicy& freshness_policy = freshness_policy_for(policy_, key.family);
  const Completeness completeness_value =
      completeness(link, key, context.now);

  std::vector<SourceId> seen;
  for (auto it = found->second.history.rbegin(); it != found->second.history.rend(); ++it) {
    const SourceId source = it->observation.provenance.source;
    if (std::find(seen.begin(), seen.end(), source) != seen.end()) {
      continue;
    }
    seen.push_back(source);

    EvaluatedEvidence evaluated{};
    evaluated.evidence = *it;
    evaluated.age = age_evidence(*it, context.now);
    evaluated.freshness = classify_freshness(evaluated.age, freshness_policy);

    const auto stream = streams_.find(source);
    evaluated.scope_current = stream != streams_.end() && stream->second.has_scope &&
                              stream->second.scope ==
                                  scope_of(it->observation.provenance);
    evaluated.generation_current =
        !context.has_generation ||
        it->observation.provenance.generation == context.current_generation;
    evaluated.historical = !evaluated.generation_current;
    evaluated.eligible = evaluated.freshness == Freshness::Fresh &&
                         evaluated.scope_current && evaluated.generation_current;
    evaluated.provisional_confidence = derive_confidence(
        it->authority, evaluated.freshness, ConflictState::None, completeness_value);
    out.push_back(std::move(evaluated));

    if (out.size() >= policy_.limits.max_contenders_per_key) {
      break;
    }
  }
  return out;
}

Completeness EvidenceStore::completeness(LinkId link,
                                              const MetricKey& key,
                                              TimePoint now) const {
  const auto found = slots_.find(SlotKey{link, key});
  if (found == slots_.end() || found->second.accepted == 0U) {
    return Completeness::Unknown;
  }
  if (!found->second.has_gap) {
    return Completeness::Complete;
  }
  const Checked<Duration> since = difference(now, found->second.last_gap_at);
  if (since.ok && !since.value.is_negative() &&
      since.value.nanos() <= policy_.thresholds.derivation_window.nanos()) {
    return Completeness::Incomplete;
  }
  return Completeness::Complete;
}

std::optional<SlotView> EvidenceStore::slot_view(LinkId link,
                                                 const MetricKey& key,
                                                 TimePoint now) const {
  const auto found = slots_.find(SlotKey{link, key});
  if (found == slots_.end()) {
    return std::nullopt;
  }
  SlotView view{};
  view.key = found->second.key;
  view.accepted = found->second.accepted;
  view.fenced = found->second.fenced;
  view.completeness = completeness(link, key, now);
  view.retained = found->second.history.size();
  view.last_gap_at = found->second.last_gap_at;
  view.has_gap = found->second.has_gap;
  return view;
}

const CounterContinuity* EvidenceStore::continuity(LinkId link,
                                                   const MetricKey& key,
                                                   SourceId source) const {
  const auto found = slots_.find(SlotKey{link, key});
  if (found == slots_.end()) {
    return nullptr;
  }
  const auto entry = found->second.continuity.find(source);
  return entry == found->second.continuity.end() ? nullptr : &entry->second;
}

const StreamState* EvidenceStore::stream_state(SourceId source) const {
  const auto found = streams_.find(source);
  return found == streams_.end() ? nullptr : &found->second;
}

std::vector<MetricKey> EvidenceStore::keys_for(LinkId link) const {
  std::vector<MetricKey> keys;
  for (const auto& entry : slots_) {
    if (entry.first.link == link) {
      keys.push_back(entry.first.key);
    }
  }
  return keys;
}

std::vector<LinkId> EvidenceStore::links_with_evidence() const {
  std::vector<LinkId> links;
  for (const auto& entry : slots_) {
    if (std::find(links.begin(), links.end(), entry.first.link) == links.end()) {
      links.push_back(entry.first.link);
    }
  }
  return links;
}

std::size_t EvidenceStore::evidence_count(LinkId link) const {
  std::size_t count = 0;
  for (const auto& entry : slots_) {
    if (entry.first.link == link) {
      count += entry.second.history.size();
    }
  }
  return count;
}

std::uint64_t EvidenceStore::fence_count(FenceReason reason) const {
  const auto found = fence_counts_.find(reason);
  return found == fence_counts_.end() ? 0U : found->second;
}

std::vector<PersistedSlot> EvidenceStore::persisted_slots() const {
  std::vector<PersistedSlot> out;
  out.reserve(slots_.size());
  for (const auto& entry : slots_) {
    PersistedSlot persisted{};
    persisted.slot = entry.first;
    persisted.history = entry.second.history;
    persisted.continuity = entry.second.continuity;
    persisted.accepted = entry.second.accepted;
    persisted.fenced = entry.second.fenced;
    persisted.last_gap_at = entry.second.last_gap_at;
    persisted.has_gap = entry.second.has_gap;
    persisted.reset_count = entry.second.reset_count;
    persisted.wrap_count = entry.second.wrap_count;
    out.push_back(std::move(persisted));
  }
  return out;
}

std::vector<std::pair<SourceId, StreamState>> EvidenceStore::persisted_streams() const {
  std::vector<std::pair<SourceId, StreamState>> out;
  out.reserve(streams_.size());
  for (const auto& entry : streams_) {
    out.emplace_back(entry.first, entry.second);
  }
  return out;
}

Status EvidenceStore::restore(const PersistedSlot& slot) {
  if (!slot.slot.link.valid()) {
    return Status::of(StatusCode::CorruptData, "persisted slot has no link identity");
  }
  if (slot.history.size() > policy_.limits.max_evidence_per_key) {
    return Status::of(StatusCode::CapacityExceeded, "persisted slot exceeds the evidence bound");
  }
  if (slot.continuity.size() > policy_.limits.max_sources) {
    return Status::of(StatusCode::CapacityExceeded, "persisted slot exceeds the source bound");
  }
  if (slots_.size() >= policy_.limits.max_links * kMaxKeysPerLink &&
      slots_.find(slot.slot) == slots_.end()) {
    return Status::of(StatusCode::CapacityExceeded, "restored store exceeds the slot bound");
  }
  Slot restored{};
  restored.key = slot.slot.key;
  restored.history = slot.history;
  restored.continuity = slot.continuity;
  restored.accepted = slot.accepted;
  restored.fenced = slot.fenced;
  restored.last_gap_at = slot.last_gap_at;
  restored.has_gap = slot.has_gap;
  restored.reset_count = slot.reset_count;
  restored.wrap_count = slot.wrap_count;
  slots_[slot.slot] = std::move(restored);
  return Status::success();
}

Status EvidenceStore::restore_stream(const std::pair<SourceId, StreamState>& stream) {
  if (!stream.first.valid()) {
    return Status::of(StatusCode::CorruptData, "persisted stream has no source identity");
  }
  if (stream.second.retired_scopes.size() > kMaxRetiredScopes) {
    return Status::of(StatusCode::CapacityExceeded, "persisted stream exceeds the retired scope bound");
  }
  if (streams_.size() >= policy_.limits.max_sources && streams_.find(stream.first) == streams_.end()) {
    return Status::of(StatusCode::CapacityExceeded, "restored store exceeds the source bound");
  }
  streams_[stream.first] = stream.second;
  return Status::success();
}

void EvidenceStore::restore_totals(std::uint64_t accepted,
                                   std::uint64_t fenced,
                                   std::uint64_t duplicates,
                                   std::uint64_t gaps,
                                   std::uint64_t resets,
                                   std::uint64_t wraps) {
  total_accepted_ = accepted;
  total_fenced_ = fenced;
  duplicates_ = duplicates;
  revision_gaps_ = gaps;
  counter_resets_ = resets;
  counter_wraps_ = wraps;
}

void EvidenceStore::clear() {
  slots_.clear();
  streams_.clear();
  total_accepted_ = 0;
  total_fenced_ = 0;
  duplicates_ = 0;
  revision_gaps_ = 0;
  counter_resets_ = 0;
  counter_wraps_ = 0;
  fence_counts_.clear();
}

std::string format_slot_view(const SlotKey& slot, const SlotView& view) {
  TextFields fields(' ');
  fields.add("link", slot.link.to_string());
  fields.add("metric", slot.key.to_string());
  fields.add("accepted", view.accepted);
  fields.add("fenced", view.fenced);
  fields.add("completeness", to_string(view.completeness));
  fields.add("has-gap", view.has_gap);
  fields.add("retained", static_cast<std::uint64_t>(view.retained));
  return fields.str();
}

}  // namespace linkobs
