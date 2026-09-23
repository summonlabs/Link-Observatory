#include "linkobs/runtime/observatory.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

#include "linkobs/core/text.hpp"
#include "linkobs/version.hpp"

namespace linkobs {
namespace {

[[nodiscard]] Observation make_observation(const ObservationDecl& declaration) {
  Observation observation{};
  observation.provenance.source = SourceId::from_name(declaration.source);
  observation.provenance.incarnation = IncarnationId::from_name(declaration.incarnation);
  observation.provenance.epoch = EpochId::from_name(declaration.epoch);
  observation.provenance.generation = GenerationId::from_name(declaration.generation);
  observation.provenance.sequence = Sequence{declaration.revision};
  observation.link = derive_link_id(declaration.link);
  observation.key = declaration.key;
  observation.payload = declaration.payload;
  observation.observed_at = TimePoint::from_nanos(declaration.observed_at);
  observation.received_at = TimePoint::from_nanos(declaration.received_at);
  return with_identity(observation);
}

[[nodiscard]] LinkFacts make_link_facts(const LinkDecl& declaration) {
  LinkFacts facts{};
  facts.id = derive_link_id(declaration.name);
  facts.name = declaration.name;
  facts.kind = declaration.kind;
  facts.generation = GenerationId::from_name(declaration.generation);
  facts.provenance = declaration.provenance;
  facts.capacity_known = declaration.capacity_known;
  facts.capacity_in_bps = declaration.capacity_in_bps;
  facts.capacity_out_bps = declaration.capacity_out_bps;
  if (!declaration.local_endpoint.empty()) {
    facts.local_endpoint = EndpointId::from_name(declaration.local_endpoint);
  }
  if (!declaration.remote_endpoint.empty()) {
    facts.remote_endpoint = EndpointId::from_name(declaration.remote_endpoint);
  }
  if (!declaration.local_port.empty()) {
    facts.local_port = PortId::from_name(declaration.local_port);
  }
  if (!declaration.remote_port.empty()) {
    facts.remote_port = PortId::from_name(declaration.remote_port);
  }
  return facts;
}

[[nodiscard]] std::string_view source_name_of(const ParsedRecord& record) {
  switch (record.kind) {
    case ParsedRecord::Kind::Source:
      return record.source.name;
    case ParsedRecord::Kind::Observation:
      return record.observation.source;
    case ParsedRecord::Kind::Link:
    case ParsedRecord::Kind::None:
      break;
  }
  return std::string_view{};
}

}  // namespace

Observatory::Observatory(RuntimeConfig config, std::unique_ptr<Clock> clock)
    : config_(std::move(config)),
      clock_(clock != nullptr ? std::move(clock) : std::make_unique<SystemClock>()),
      runtime_id_(runtime_id_of(config_)),
      incarnation_id_(incarnation_id_of(config_)),
      registry_(config_.policy),
      store_(config_.policy),
      transitions_(config_.policy.limits) {
  if (config_.workers > 0U) {
    workers_ = std::make_unique<WorkerPool>(config_.workers, config_.queue_depth);
  }
}

Observatory::~Observatory() {
  if (started_) {
    const Status stopped = stop();
    (void)stopped;
  }
  if (workers_ != nullptr) {
    workers_->cancel();
  }
}

Status Observatory::start() {
  const Status valid = validate(config_);
  if (!valid.ok()) {
    return valid;
  }
  {
    std::lock_guard<TrackedLock> guard(state_lock_);
    if (started_) {
      return Status::of(StatusCode::AlreadyExists, "observatory is already started");
    }
  }

  if (config_.restore_on_start && !config_.snapshot_path.empty()) {
    const Status loaded = reload();
    if (!loaded.ok() && loaded.code() != StatusCode::NotFound) {
      return loaded;
    }
  }

  if (workers_ != nullptr) {
    const Status started = workers_->start([this](Batch&& batch) {
      std::lock_guard<TrackedLock> guard(state_lock_);
      stats_.batches_processed += 1U;
      for (const ParsedRecord& record : batch.records) {
        const Status applied = apply_record_locked(record);
        (void)applied;
      }
    });
    if (!started.ok()) {
      return started;
    }
  }

  std::lock_guard<TrackedLock> guard(state_lock_);
  started_ = true;
  return Status::success();
}

Status Observatory::stop() {
  if (workers_ != nullptr) {
    workers_->stop();
    std::lock_guard<TrackedLock> guard(state_lock_);
    stats_.abandoned_batches += workers_->abandoned();
  }

  Status persisted = Status::success();
  if (config_.persist_on_stop && !config_.snapshot_path.empty()) {
    const SnapshotWriteResult result = persist();
    persisted = result.status;
  }

  std::lock_guard<TrackedLock> guard(state_lock_);
  started_ = false;
  return persisted;
}

bool Observatory::started() const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  return started_;
}

TimePoint Observatory::now() const { return clock_->now(); }

Status Observatory::apply_source(const SourceDecl& declaration) {
  std::lock_guard<TrackedLock> guard(state_lock_);
  ParsedRecord record{};
  record.kind = ParsedRecord::Kind::Source;
  record.source = declaration;
  return apply_record_locked(record);
}

Status Observatory::apply_link(const LinkDecl& declaration) {
  std::lock_guard<TrackedLock> guard(state_lock_);
  ParsedRecord record{};
  record.kind = ParsedRecord::Kind::Link;
  record.link = declaration;
  return apply_record_locked(record);
}

Status Observatory::apply_batch(const Batch& batch) {
  {
    std::lock_guard<TrackedLock> guard(state_lock_);
    stats_.batches_processed += 1U;
    for (const ParsedRecord& record : batch.records) {
      const Status applied = apply_record_locked(record);
      (void)applied;
    }
  }
  if (batch.on_applied) {
    batch.on_applied();
  }
  return Status::success();
}

Status Observatory::submit_batch(Batch batch) {
  {
    std::lock_guard<TrackedLock> guard(state_lock_);
    if (!started_) {
      return Status::of(StatusCode::Cancelled, "observatory is not started");
    }
  }

  if (workers_ == nullptr || config_.workers == 0U) {
    return apply_batch(batch);
  }

  std::map<std::size_t, Batch> routed;
  std::vector<ParsedRecord> declarations;
  for (ParsedRecord& record : batch.records) {
    if (record.kind == ParsedRecord::Kind::Observation) {
      const std::size_t route = workers_->route_of(source_name_of(record));
      routed[route].records.push_back(std::move(record));
    } else {
      declarations.push_back(std::move(record));
    }
  }

  // The caller's completion fires only after every part of the batch has been
  // applied, whether the parts run inline or on workers.
  const std::size_t part_count = routed.size() + (declarations.empty() ? 0U : 1U);
  auto remaining = std::make_shared<std::atomic<std::size_t>>(part_count);
  std::function<void()> caller_completion = std::move(batch.on_applied);
  const auto arrive = [remaining, caller_completion]() {
    if (remaining->fetch_sub(1U) == 1U && caller_completion) {
      caller_completion();
    }
  };

  if (part_count == 0U) {
    if (caller_completion) {
      caller_completion();
    }
    return Status::success();
  }

  if (!declarations.empty()) {
    Batch declaration_batch{};
    declaration_batch.records = std::move(declarations);
    declaration_batch.on_applied = arrive;
    const Status applied = apply_batch(declaration_batch);
    if (!applied.ok()) {
      return applied;
    }
  }

  for (auto& entry : routed) {
    entry.second.on_applied = arrive;
    const Status submitted = workers_->submit(entry.first, std::move(entry.second));
    if (!submitted.ok()) {
      return submitted;
    }
  }
  return Status::success();
}

Status Observatory::submit_observation(const ObservationDecl& declaration) {
  Batch batch{};
  ParsedRecord record{};
  record.kind = ParsedRecord::Kind::Observation;
  record.observation = declaration;
  batch.records.push_back(std::move(record));
  return submit_batch(std::move(batch));
}

Status Observatory::apply_record_locked(const ParsedRecord& record) {
  switch (record.kind) {
    case ParsedRecord::Kind::Source: {
      SourceRecord source{};
      source.id = SourceId::from_name(record.source.name);
      source.name = record.source.name;
      source.authority = record.source.authority;
      source.provenance = record.source.provenance;
      const Status status = registry_.register_source(source);
      if (!status.ok()) {
        stats_.declarations_rejected += 1U;
        return status;
      }
      if (record.source.retired) {
        const Status retired = registry_.retire_source(source.id);
        if (!retired.ok()) {
          stats_.declarations_rejected += 1U;
          return retired;
        }
      }
      stats_.declarations_applied += 1U;
      return Status::success();
    }
    case ParsedRecord::Kind::Link: {
      const LinkFacts facts = make_link_facts(record.link);
      const bool existed = registry_.find_link(facts.id) != nullptr;
      const TimePoint instant = clock_->now();
      const Status status = registry_.register_link(facts, instant);
      if (!status.ok()) {
        stats_.declarations_rejected += 1U;
        return status;
      }
      stats_.declarations_applied += 1U;
      const GenerationObservation observation =
          registry_.observe_generation(facts.id, facts.generation, instant);
      if (!existed) {
        TransitionEvent event{};
        event.link = facts.id;
        event.kind = TransitionKind::LinkRegistered;
        event.at = instant;
        event.generation = facts.generation;
        event.to = facts.name;
        transitions_.record(std::move(event));
        stats_.transitions_recorded += 1U;
      }
      if (observation.changed && existed) {
        TransitionEvent event{};
        event.link = facts.id;
        event.kind = TransitionKind::GenerationChanged;
        event.at = instant;
        event.generation = facts.generation;
        event.from = observation.previous.to_string();
        event.to = facts.generation.to_string();
        transitions_.record(std::move(event));
        stats_.transitions_recorded += 1U;
      }
      return Status::success();
    }
    case ParsedRecord::Kind::Observation:
      apply_observation_locked(record.observation);
      return Status::success();
    case ParsedRecord::Kind::None:
      return Status::success();
  }
  return Status::success();
}

void Observatory::apply_observation_locked(const ObservationDecl& declaration) {
  const TimePoint instant = clock_->now();
  const Observation observation = make_observation(declaration);
  stats_.records_submitted += 1U;

  const SourceRecord* source = registry_.find_source(observation.provenance.source);
  if (source == nullptr || source->retired) {
    AcceptContext context{};
    context.source = source;
    context.current_generation = registry_.current_generation(observation.link);
    context.family_enabled = config_.families.is_enabled(observation.key.family);
    const AcceptOutcome outcome = store_.accept(observation, context, instant);
    if (!outcome.accepted) {
      stats_.records_fenced += 1U;
    }
    return;
  }

  if (!config_.families.is_enabled(observation.key.family)) {
    AcceptContext context{};
    context.source = source;
    context.current_generation = registry_.current_generation(observation.link);
    context.family_enabled = false;
    const AcceptOutcome outcome = store_.accept(observation, context, instant);
    if (!outcome.accepted) {
      // The store already counted this fence; only the runtime total is added.
      stats_.records_fenced += 1U;
    }
    return;
  }

  if (registry_.find_link(observation.link) == nullptr) {
    const Status ensured = registry_.ensure_implicit_link(
        observation.link, observation.link.to_string(), instant);
    if (!ensured.ok()) {
      ++own_fence_counts_[FenceReason::UnknownLink];
      stats_.records_fenced += 1U;
      return;
    }
  }

  const GenerationObservation generation = registry_.observe_generation(
      observation.link, observation.provenance.generation, instant);
  if (generation.behind) {
    ++own_fence_counts_[FenceReason::GenerationBehind];
    stats_.records_fenced += 1U;
    registry_.note_fenced(observation.link);
    return;
  }
  if (generation.changed) {
    TransitionEvent event{};
    event.link = observation.link;
    event.kind = TransitionKind::GenerationChanged;
    event.at = instant;
    event.generation = observation.provenance.generation;
    event.source = observation.provenance.source;
    event.from = generation.previous.to_string();
    event.to = observation.provenance.generation.to_string();
    transitions_.record(std::move(event));
    stats_.transitions_recorded += 1U;
  }

  AcceptContext context{};
  context.source = source;
  context.current_generation = registry_.current_generation(observation.link);
  context.family_enabled = true;
  const AcceptOutcome outcome = store_.accept(observation, context, instant);
  if (!outcome.accepted) {
    stats_.records_fenced += 1U;
    registry_.note_fenced(observation.link);
    return;
  }

  stats_.records_accepted += 1U;
  registry_.note_accepted(observation.link, observation.observed_at, instant);

  if (outcome.discontinuity == CounterDiscontinuity::Reset) {
    TransitionEvent event{};
    event.link = observation.link;
    event.kind = TransitionKind::CounterReset;
    event.at = instant;
    event.generation = observation.provenance.generation;
    event.source = observation.provenance.source;
    event.to = observation.key.to_string();
    transitions_.record(std::move(event));
    stats_.transitions_recorded += 1U;
  } else if (outcome.discontinuity == CounterDiscontinuity::Wrap) {
    TransitionEvent event{};
    event.link = observation.link;
    event.kind = TransitionKind::CounterWrap;
    event.at = instant;
    event.generation = observation.provenance.generation;
    event.source = observation.provenance.source;
    event.to = observation.key.to_string();
    transitions_.record(std::move(event));
    stats_.transitions_recorded += 1U;
  }

  if (outcome.revision_gap) {
    TransitionEvent event{};
    event.link = observation.link;
    event.kind = TransitionKind::RevisionGap;
    event.at = instant;
    event.generation = observation.provenance.generation;
    event.source = observation.provenance.source;
    event.detail = observation.provenance.sequence.value();
    transitions_.record(std::move(event));
    stats_.transitions_recorded += 1U;
  }

  refresh_classification_locked(observation.link, instant);
}

void Observatory::refresh_classification_locked(LinkId link, TimePoint instant) {
  ClassificationInput input{};
  input.link = link;
  input.entry = registry_.find_link(link);
  input.current_generation = registry_.current_generation(link);
  input.generation_changes = registry_.generation_state(link).generation_changes;
  input.policy = &config_.policy;
  input.store = &store_;
  input.transitions = &transitions_;
  input.now = instant;

  const Classification current = linkobs::classify(input);
  stats_.classifications += 1U;

  const auto previous = observed_states_.find(link);
  if (previous == observed_states_.end()) {
    TransitionEvent event{};
    event.link = link;
    event.kind = TransitionKind::ClassificationChanged;
    event.at = instant;
    event.generation = current.generation;
    event.from = "none";
    event.to = std::string{to_string(current.state)};
    transitions_.record(std::move(event));
    stats_.transitions_recorded += 1U;
    observed_states_.emplace(link, current.state);
    stats_.state_changes += 1U;
  } else if (previous->second != current.state) {
    TransitionEvent event{};
    event.link = link;
    event.kind = TransitionKind::ClassificationChanged;
    event.at = instant;
    event.generation = current.generation;
    event.from = std::string{to_string(previous->second)};
    event.to = std::string{to_string(current.state)};
    transitions_.record(std::move(event));
    stats_.transitions_recorded += 1U;
    previous->second = current.state;
    stats_.state_changes += 1U;
  }

  const auto track = [this, link, instant](MetricFamily family,
                                           std::map<LinkId, std::string>& tracker,
                                           TransitionKind kind,
                                           const Classification& classification) {
    const FamilyReport* report = classification.find(family, Direction::Unknown);
    const std::string text = (report != nullptr && report->has_value) ? report->value_text
                                                                     : std::string{"none"};
    const auto found = tracker.find(link);
    if (found == tracker.end()) {
      tracker.emplace(link, text);
      return;
    }
    if (found->second == text) {
      return;
    }
    TransitionEvent event{};
    event.link = link;
    event.kind = kind;
    event.at = instant;
    event.generation = classification.generation;
    event.from = found->second;
    event.to = text;
    transitions_.record(std::move(event));
    stats_.transitions_recorded += 1U;
    found->second = text;
  };

  track(MetricFamily::OperState, last_oper_text_, TransitionKind::OperStateChanged, current);
  track(MetricFamily::AdminState, last_admin_text_, TransitionKind::AdminStateChanged, current);
}

std::vector<LinkEntry> Observatory::links() const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  return registry_.links();
}

std::vector<SourceRecord> Observatory::sources() const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  return registry_.sources();
}

std::optional<LinkEntry> Observatory::find_link(std::string_view name) const {
  const LinkId id = derive_link_id(name);
  std::lock_guard<TrackedLock> guard(state_lock_);
  const LinkEntry* entry = registry_.find_link(id);
  if (entry == nullptr) {
    return std::nullopt;
  }
  return *entry;
}

std::vector<Classification> Observatory::classify_all() const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  std::vector<Classification> out;
  const TimePoint instant = clock_->now();
  for (const LinkId link : store_.links_with_evidence()) {
    ClassificationInput input{};
    input.link = link;
    input.entry = registry_.find_link(link);
    input.current_generation = registry_.current_generation(link);
    input.generation_changes = registry_.generation_state(link).generation_changes;
    input.policy = &config_.policy;
    input.store = &store_;
    input.transitions = &transitions_;
    input.now = instant;
    out.push_back(linkobs::classify(input));
  }
  return out;
}

std::optional<Classification> Observatory::classify(LinkId link) const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  if (registry_.find_link(link) == nullptr && store_.slot_count() == 0U) {
    return std::nullopt;
  }
  const TimePoint instant = clock_->now();
  ClassificationInput input{};
  input.link = link;
  input.entry = registry_.find_link(link);
  input.current_generation = registry_.current_generation(link);
  input.generation_changes = registry_.generation_state(link).generation_changes;
  input.policy = &config_.policy;
  input.store = &store_;
  input.transitions = &transitions_;
  input.now = instant;
  return linkobs::classify(input);
}

std::optional<Classification> Observatory::classify(std::string_view link_name) const {
  return classify(derive_link_id(link_name));
}

std::vector<TransitionEvent> Observatory::transitions(LinkId link, std::size_t limit) const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  return transitions_.for_link(link, limit);
}

std::vector<TransitionEvent> Observatory::recent_transitions(std::size_t limit) const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  return transitions_.recent(limit);
}

std::vector<EvaluatedEvidence> Observatory::evidence_for(LinkId link,
                                                         const MetricKey& key) const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  EvaluationContext context{};
  context.now = clock_->now();
  context.current_generation = registry_.current_generation(link);
  context.has_generation = context.current_generation.valid();
  return store_.contenders(link, key, context);
}

std::vector<MetricKey> Observatory::metrics_for(LinkId link) const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  return store_.keys_for(link);
}

std::vector<SlotView> Observatory::slot_views(LinkId link) const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  std::vector<SlotView> out;
  const TimePoint instant = clock_->now();
  for (const MetricKey& key : store_.keys_for(link)) {
    const auto view = store_.slot_view(link, key, instant);
    if (view.has_value()) {
      out.push_back(*view);
    }
  }
  return out;
}

std::vector<Evidence> Observatory::link_evidence(LinkId link, std::size_t limit) const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  const std::size_t bound = limit < config_.policy.limits.max_result_rows
                                ? limit
                                : config_.policy.limits.max_result_rows;
  std::vector<Evidence> out;
  for (const MetricKey& key : store_.keys_for(link)) {
    const std::vector<EvaluatedEvidence> contenders = store_.contenders(
        link, key, EvaluationContext{clock_->now(), registry_.current_generation(link),
                                     registry_.current_generation(link).valid()});
    for (const auto& candidate : contenders) {
      if (out.size() >= bound) {
        return out;
      }
      out.push_back(candidate.evidence);
    }
  }
  return out;
}

SnapshotContents Observatory::collect_snapshot() const {
  SnapshotContents contents{};
  contents.format_version = kSnapshotFormatVersion;
  contents.runtime = runtime_id_;
  contents.incarnation = incarnation_id_;
  contents.saved_at = clock_->now();
  contents.links = registry_.persisted_links();
  contents.sources = registry_.sources();
  contents.slots = store_.persisted_slots();
  contents.streams = store_.persisted_streams();
  contents.transitions = transitions_.recent(config_.policy.limits.max_total_transitions);
  contents.total_accepted = store_.total_accepted();
  contents.total_fenced = store_.total_fenced();
  contents.duplicates = store_.duplicate_count();
  contents.revision_gaps = store_.revision_gap_count();
  contents.counter_resets = store_.counter_reset_count();
  contents.counter_wraps = store_.counter_wrap_count();
  contents.transitions_dropped = transitions_.dropped_events();
  return contents;
}

SnapshotWriteResult Observatory::persist() {
  std::lock_guard<TrackedLock> guard(state_lock_);
  if (config_.snapshot_path.empty()) {
    SnapshotWriteResult result{};
    result.status = Status::of(StatusCode::InvalidArgument, "no snapshot path is configured");
    return result;
  }
  const SnapshotContents contents = collect_snapshot();
  const SnapshotWriteResult result =
      write_snapshot(config_.snapshot_path, contents, config_.policy);
  if (result.status.ok()) {
    stats_.snapshot_writes += 1U;
  }
  return result;
}

Status Observatory::restore_from(const SnapshotContents& contents) {
  registry_ = LinkRegistry{config_.policy};
  store_ = EvidenceStore{config_.policy};
  transitions_.clear();
  observed_states_.clear();
  last_oper_text_.clear();
  last_admin_text_.clear();

  for (const SourceRecord& source : contents.sources) {
    SourceRecord copy = source;
    const bool retired = source.retired;
    copy.retired = false;
    const Status status = registry_.register_source(copy);
    if (!status.ok()) {
      return Status::of(StatusCode::CorruptData, "snapshot source record could not be restored");
    }
    if (retired) {
      const Status retired_status = registry_.retire_source(copy.id);
      if (!retired_status.ok()) {
        return retired_status;
      }
    }
  }
  for (const PersistedLink& link : contents.links) {
    const Status status = registry_.restore(link);
    if (!status.ok()) {
      return status;
    }
  }
  for (const PersistedSlot& slot : contents.slots) {
    const Status status = store_.restore(slot);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& stream : contents.streams) {
    const Status status = store_.restore_stream(stream);
    if (!status.ok()) {
      return status;
    }
  }
  for (const TransitionEvent& event : contents.transitions) {
    transitions_.record(event);
  }
  store_.restore_totals(contents.total_accepted, contents.total_fenced, contents.duplicates,
                        contents.revision_gaps, contents.counter_resets, contents.counter_wraps);

  // Session statistics that describe the evidence are restored too, so a report
  // printed by a later process is consistent with the evidence it shows. Counters
  // that describe this process only (snapshot writes, cancellations) stay at
  // zero, because they are not claims about the observed system.
  stats_.records_accepted = contents.total_accepted;
  stats_.records_fenced = contents.total_fenced;
  stats_.transitions_recorded = static_cast<std::uint64_t>(contents.transitions.size());

  // Counter baselines are restored, not dropped. A rate is a property of the two
  // samples, not of the observing process: the window, the scope, the generation,
  // the revision order and the stream gap epoch all travel with the samples, and
  // freshness is recomputed against the running clock. Restoring the baseline
  // therefore cannot make anything fresh, and dropping it would make every
  // read-only process unable to report a utilization at all.
  return Status::success();
}

Status Observatory::reload() {
  if (config_.snapshot_path.empty()) {
    return Status::of(StatusCode::InvalidArgument, "no snapshot path is configured");
  }
  const SnapshotLoadResult loaded = read_snapshot(config_.snapshot_path, config_.policy);

  std::lock_guard<TrackedLock> guard(state_lock_);
  if (!loaded.ok) {
    if (loaded.status.code() == StatusCode::NotFound) {
      restored_ = false;
      load_detail_ = "no snapshot present";
      return loaded.status;
    }
    restored_ = false;
    load_detail_ = loaded.detail;
    return loaded.status;
  }

  const Status restored = restore_from(loaded.contents);
  if (!restored.ok()) {
    restored_ = false;
    load_detail_ = std::string{"snapshot could not be applied: "} + restored.to_string();
    return restored;
  }
  restored_ = true;
  loaded_from_backup_ = loaded.recovered_from_backup;
  load_detail_ = loaded.recovered_from_backup ? loaded.detail : std::string{"snapshot loaded"};
  stats_.snapshot_reads += 1U;
  if (loaded.recovered_from_backup) {
    stats_.recoveries += 1U;
  }
  return Status::success();
}

RuntimeStatistics Observatory::statistics() const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  return stats_;
}

std::uint64_t Observatory::fence_count(FenceReason reason) const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  std::uint64_t total = store_.fence_count(reason);
  const auto found = own_fence_counts_.find(reason);
  if (found != own_fence_counts_.end()) {
    total += found->second;
  }
  return total;
}

bool Observatory::restored_from_snapshot() const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  return restored_;
}

bool Observatory::loaded_from_backup() const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  return loaded_from_backup_;
}

std::string Observatory::load_detail() const {
  std::lock_guard<TrackedLock> guard(state_lock_);
  return load_detail_;
}

}  // namespace linkobs
