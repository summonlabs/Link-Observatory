#include "linkobs/store/link_registry.hpp"

#include <algorithm>

#include "linkobs/core/text.hpp"

namespace linkobs {
namespace {

constexpr std::size_t kMaxRetiredGenerations = 32U;

}  // namespace

Status LinkRegistry::register_source(SourceRecord record) {
  if (!record.id.valid()) {
    return Status::of(StatusCode::InvalidArgument, "source record has no identity");
  }
  if (record.name.empty() || record.name.size() > 128U) {
    return Status::of(StatusCode::InvalidArgument, "source name must be 1..128 bytes");
  }
  if (record.provenance == ProvenanceClass::Unknown) {
    return Status::of(StatusCode::InvalidArgument,
                      "source must declare whether it supplies real, synthetic or replayed evidence");
  }
  const auto existing = sources_.find(record.id);
  if (existing != sources_.end()) {
    if (existing->second.authority != record.authority ||
        existing->second.provenance != record.provenance) {
      return Status::of(StatusCode::Conflict,
                        "source is already registered with different authority or provenance");
    }
    existing->second.name = record.name;
    existing->second.retired = false;
    existing->second.registered = true;
    return Status::success();
  }
  if (sources_.size() >= policy_.limits.max_sources) {
    return Status::of(StatusCode::CapacityExceeded, "source registry is full");
  }
  record.registered = true;
  sources_.emplace(record.id, std::move(record));
  return Status::success();
}

Status LinkRegistry::retire_source(SourceId id) {
  const auto found = sources_.find(id);
  if (found == sources_.end()) {
    return Status::of(StatusCode::NotFound, "source is not registered");
  }
  found->second.retired = true;
  return Status::success();
}

Status LinkRegistry::register_link(const LinkFacts& facts, TimePoint now) {
  const Status valid = validate_link_facts(facts, policy_);
  if (!valid.ok()) {
    return valid;
  }
  const auto existing = links_.find(facts.id);
  if (existing != links_.end()) {
    if (existing->second.entry.retired) {
      return Status::of(StatusCode::Conflict, "link is retired");
    }
    LinkFacts merged = facts;
    if (!merged.generation.valid()) {
      merged.generation = existing->second.entry.facts.generation;
    }
    if (merged.generation != existing->second.entry.facts.generation) {
      // A re-registration under a new generation is a topology change, not a
      // silent edit: it goes through the same fencing path as an observation.
      const GenerationObservation observation =
          observe_generation(facts.id, merged.generation, now);
      if (observation.behind) {
        return Status::of(StatusCode::Conflict,
                          "link re-registration names a retired topology generation");
      }
    }
    existing->second.entry.facts = std::move(merged);
    return Status::success();
  }
  if (links_.size() >= policy_.limits.max_links) {
    return Status::of(StatusCode::CapacityExceeded, "link registry is full");
  }
  LinkState state{};
  state.entry.facts = facts;
  state.entry.registered_at = now;
  if (facts.generation.valid()) {
    state.generations.has_generation = true;
    state.generations.current_generation = facts.generation;
  }
  links_.emplace(facts.id, std::move(state));
  return Status::success();
}

Status LinkRegistry::retire_link(LinkId id, TimePoint now) {
  (void)now;
  const auto found = links_.find(id);
  if (found == links_.end()) {
    return Status::of(StatusCode::NotFound, "link is not registered");
  }
  found->second.entry.retired = true;
  return Status::success();
}

Status LinkRegistry::ensure_implicit_link(LinkId id, std::string_view name, TimePoint now) {
  if (!policy_.allow_implicit_links) {
    return Status::of(StatusCode::NotFound, "link is not registered");
  }
  const auto existing = links_.find(id);
  if (existing != links_.end()) {
    if (existing->second.entry.retired) {
      return Status::of(StatusCode::Conflict, "link is retired");
    }
    return Status::success();
  }
  LinkFacts facts{};
  facts.id = id;
  facts.name.assign(name.substr(0U, 128U));
  facts.kind = LinkKind::Unknown;
  facts.capacity_known = false;
  facts.provenance = ProvenanceClass::Unknown;
  const Status status = register_link(facts, now);
  if (status.ok()) {
    const auto created = links_.find(id);
    if (created != links_.end()) {
      created->second.entry.facts.provenance = ProvenanceClass::Unknown;
    }
  }
  return status;
}

const SourceRecord* LinkRegistry::find_source(SourceId id) const {
  const auto found = sources_.find(id);
  return found == sources_.end() ? nullptr : &found->second;
}

const LinkEntry* LinkRegistry::find_link(LinkId id) const {
  const auto found = links_.find(id);
  return found == links_.end() ? nullptr : &found->second.entry;
}

GenerationObservation LinkRegistry::observe_generation(LinkId id,
                                                       GenerationId generation,
                                                       TimePoint now) {
  (void)now;
  GenerationObservation observation{};
  observation.current = generation;
  const auto found = links_.find(id);
  if (found == links_.end()) {
    observation.behind = true;
    return observation;
  }
  LinkState& state = found->second;
  LinkGenerationState& generations = state.generations;
  if (!generations.has_generation) {
    generations.has_generation = true;
    generations.current_generation = generation;
    observation.changed = true;
    observation.previous = GenerationId{};
    return observation;
  }
  if (generations.current_generation == generation) {
    return observation;
  }
  const bool retired =
      std::find(generations.retired_generations.begin(), generations.retired_generations.end(),
                generation) != generations.retired_generations.end();
  if (retired) {
    observation.behind = true;
    return observation;
  }
  generations.retired_generations.push_back(generations.current_generation);
  while (generations.retired_generations.size() > kMaxRetiredGenerations) {
    generations.retired_generations.erase(generations.retired_generations.begin());
  }
  observation.changed = true;
  observation.previous = generations.current_generation;
  generations.current_generation = generation;
  generations.generation_changes += 1U;
  return observation;
}

GenerationId LinkRegistry::current_generation(LinkId id) const {
  const auto found = links_.find(id);
  if (found == links_.end() || !found->second.generations.has_generation) {
    return GenerationId{};
  }
  return found->second.generations.current_generation;
}

LinkGenerationState LinkRegistry::generation_state(LinkId id) const {
  const auto found = links_.find(id);
  return found == links_.end() ? LinkGenerationState{} : found->second.generations;
}

std::vector<PersistedLink> LinkRegistry::persisted_links() const {
  std::vector<PersistedLink> out;
  out.reserve(links_.size());
  for (const auto& item : links_) {
    PersistedLink link{};
    link.facts = item.second.entry.facts;
    link.entry = item.second.entry;
    link.generations = item.second.generations;
    out.push_back(std::move(link));
  }
  return out;
}

Status LinkRegistry::restore(const PersistedLink& link) {
  const Status valid = validate_link_facts(link.facts, policy_);
  if (!valid.ok()) {
    return Status::of(StatusCode::CorruptData, "persisted link facts are invalid");
  }
  if (links_.size() >= policy_.limits.max_links && links_.find(link.facts.id) == links_.end()) {
    return Status::of(StatusCode::CapacityExceeded, "restored registry exceeds the link bound");
  }
  if (link.generations.retired_generations.size() > kMaxRetiredGenerations) {
    return Status::of(StatusCode::CapacityExceeded,
                      "persisted link exceeds the retired generation bound");
  }
  LinkState state{};
  state.entry = link.entry;
  state.entry.facts = link.facts;
  state.generations = link.generations;
  links_[link.facts.id] = std::move(state);
  return Status::success();
}

void LinkRegistry::note_registered(LinkId id, TimePoint now) {
  const auto found = links_.find(id);
  if (found == links_.end()) {
    return;
  }
  found->second.entry.registered_at = now;
}

bool LinkRegistry::generation_retired(LinkId id, GenerationId generation) const {
  const auto found = links_.find(id);
  if (found == links_.end()) {
    return false;
  }
  const auto& retired = found->second.generations.retired_generations;
  return std::find(retired.begin(), retired.end(), generation) != retired.end();
}

std::size_t LinkRegistry::retired_generation_count(LinkId id) const {
  const auto found = links_.find(id);
  return found == links_.end() ? 0U : found->second.generations.retired_generations.size();
}

void LinkRegistry::note_accepted(LinkId id, TimePoint observed_at, TimePoint accepted_at) {
  const auto found = links_.find(id);
  if (found == links_.end()) {
    return;
  }
  LinkEntry& entry = found->second.entry;
  entry.accepted_observations += 1U;
  if (!entry.has_evidence || observed_at > entry.last_evidence_at) {
    entry.last_evidence_at = observed_at;
  }
  if (!entry.has_evidence || accepted_at > entry.last_accepted_at) {
    entry.last_accepted_at = accepted_at;
  }
  entry.has_evidence = true;
}

void LinkRegistry::note_fenced(LinkId id) {
  const auto found = links_.find(id);
  if (found == links_.end()) {
    return;
  }
  found->second.entry.fenced_observations += 1U;
}

std::vector<LinkEntry> LinkRegistry::links() const {
  std::vector<LinkEntry> out;
  out.reserve(links_.size());
  for (const auto& item : links_) {
    out.push_back(item.second.entry);
  }
  return out;
}

std::vector<SourceRecord> LinkRegistry::sources() const {
  std::vector<SourceRecord> out;
  out.reserve(sources_.size());
  for (const auto& item : sources_) {
    out.push_back(item.second);
  }
  return out;
}

}  // namespace linkobs
