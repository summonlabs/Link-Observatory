// Link Observatory - bounded evidence store.
//
// The store holds, for each (link, metric key), the evidence the runtime is
// willing to reason about. It enforces every ingest fence and every bound. It
// deliberately does not decide link state: that is the classifier's job, and it
// reads the store through a query interface.
//
// The store is not independently synchronised. The observatory owns it and holds
// the observatory lock for every access (docs/locking-audit.md).

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "linkobs/domain/evidence.hpp"
#include "linkobs/domain/link.hpp"
#include "linkobs/domain/observation.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/store/counter_tracker.hpp"

namespace linkobs {

struct SlotKey {
  LinkId link{};
  MetricKey key{};

  friend bool operator==(const SlotKey&, const SlotKey&) noexcept = default;
  friend auto operator<=>(const SlotKey&, const SlotKey&) noexcept = default;
};

/// Per-source revision-stream state: the scope currently in force, the highest
/// revision seen in it, and every scope that has already been superseded.
struct StreamState {
  bool has_scope{false};
  SequenceScope scope{};
  bool has_sequence{false};
  Sequence last_sequence{};
  ObservationId last_observation{};
  bool gap_open{false};
  std::vector<SequenceScope> retired_scopes{};
  std::uint64_t accepted{0};
  std::uint64_t fenced{0};
  std::uint64_t gap_count{0};
  std::uint64_t scope_changes{0};
};

struct EvaluationContext {
  TimePoint now{};
  GenerationId current_generation{};
  bool has_generation{false};
};

struct EvaluatedEvidence {
  Evidence evidence{};
  AgeReport age{};
  Freshness freshness{Freshness::Unknown};
  /// Provisional only: derived without conflict analysis. The classifier
  /// recomputes the final confidence after it has compared sources.
  Confidence provisional_confidence{Confidence::Unknown};
  bool scope_current{true};
  bool generation_current{true};
  /// Fresh, in the current scope and in the current generation. Only eligible
  /// evidence may support a positive claim about the present.
  bool eligible{false};
  /// Recorded under a superseded topology generation. Retained, reported, and
  /// never used as current.
  bool historical{false};
};

struct AcceptContext {
  const SourceRecord* source{nullptr};
  GenerationId current_generation{};
  bool family_enabled{true};
};

struct AcceptOutcome {
  bool accepted{false};
  FenceReason fence{FenceReason::None};
  bool duplicate{false};
  bool revision_gap{false};
  bool scope_changed{false};
  bool stale_baseline{false};
  CounterDiscontinuity discontinuity{CounterDiscontinuity::None};
  bool delta_valid{false};
  std::uint64_t delta{0};
  CounterWidth counter_width{CounterWidth::Bits64};
};

struct SlotView {
  MetricKey key{};
  std::uint64_t accepted{0};
  std::uint64_t fenced{0};
  Completeness completeness{Completeness::Unknown};
  std::size_t retained{0};
  /// Instant of the most recent evidence-stream discontinuity, if any.
  TimePoint last_gap_at{};
  bool has_gap{false};
};

/// Serialisable form of a slot, used by the snapshot writer and reader.
struct PersistedSlot {
  SlotKey slot{};
  std::vector<Evidence> history{};
  std::map<SourceId, CounterContinuity> continuity{};
  std::uint64_t accepted{0};
  std::uint64_t fenced{0};
  TimePoint last_gap_at{};
  bool has_gap{false};
  std::uint64_t reset_count{0};
  std::uint64_t wrap_count{0};
};

class EvidenceStore {
 public:
  explicit EvidenceStore(const Policy& policy) : policy_(policy) {}

  /// Applies every ingest fence, then records the observation as evidence.
  [[nodiscard]] AcceptOutcome accept(const Observation& observation,
                                     const AcceptContext& context,
                                     TimePoint now);

  /// Evidence held for one slot, newest first, evaluated against the context.
  [[nodiscard]] std::vector<EvaluatedEvidence> contenders(LinkId link,
                                                          const MetricKey& key,
                                                          const EvaluationContext& context) const;

  [[nodiscard]] Completeness completeness(LinkId link, const MetricKey& key, TimePoint now) const;
  [[nodiscard]] std::optional<SlotView> slot_view(LinkId link,
                                                  const MetricKey& key,
                                                  TimePoint now) const;
  [[nodiscard]] const CounterContinuity* continuity(LinkId link,
                                                    const MetricKey& key,
                                                    SourceId source) const;
  [[nodiscard]] const StreamState* stream_state(SourceId source) const;
  [[nodiscard]] std::vector<MetricKey> keys_for(LinkId link) const;
  [[nodiscard]] std::vector<LinkId> links_with_evidence() const;
  [[nodiscard]] std::size_t evidence_count(LinkId link) const;
  [[nodiscard]] std::size_t slot_count() const noexcept { return slots_.size(); }

  [[nodiscard]] std::uint64_t total_accepted() const noexcept { return total_accepted_; }
  [[nodiscard]] std::uint64_t total_fenced() const noexcept { return total_fenced_; }
  [[nodiscard]] std::uint64_t fence_count(FenceReason reason) const;
  [[nodiscard]] std::uint64_t duplicate_count() const noexcept { return duplicates_; }
  [[nodiscard]] std::uint64_t revision_gap_count() const noexcept { return revision_gaps_; }
  [[nodiscard]] std::uint64_t counter_reset_count() const noexcept { return counter_resets_; }
  [[nodiscard]] std::uint64_t counter_wrap_count() const noexcept { return counter_wraps_; }

  [[nodiscard]] std::vector<PersistedSlot> persisted_slots() const;
  [[nodiscard]] std::vector<std::pair<SourceId, StreamState>> persisted_streams() const;
  [[nodiscard]] const std::map<FenceReason, std::uint64_t>& fence_counts() const noexcept {
    return fence_counts_;
  }

  /// Restores persisted state. Only the snapshot loader calls this, and only
  /// after the payload has been integrity checked. Restored evidence keeps its
  /// original timestamps, so it cannot become fresh merely by being reloaded.
  [[nodiscard]] Status restore(const PersistedSlot& slot);
  [[nodiscard]] Status restore_stream(const std::pair<SourceId, StreamState>& stream);
  void restore_totals(std::uint64_t accepted,
                      std::uint64_t fenced,
                      std::uint64_t duplicates,
                      std::uint64_t gaps,
                      std::uint64_t resets,
                      std::uint64_t wraps);

  void clear();

 private:
  struct Slot {
    MetricKey key{};
    std::vector<Evidence> history{};
    std::map<SourceId, CounterContinuity> continuity{};
    std::uint64_t accepted{0};
    std::uint64_t fenced{0};
    TimePoint last_gap_at{};
    bool has_gap{false};
    std::uint64_t reset_count{0};
    std::uint64_t wrap_count{0};
  };

  [[nodiscard]] Slot& slot_for(LinkId link, const MetricKey& key);
  [[nodiscard]] StreamState& stream_for(SourceId source);
  void count_fence(FenceReason reason);
  [[nodiscard]] FenceReason fence_stream(const StreamState& state,
                                         SequenceScope scope,
                                         SourceId source) const;

  Policy policy_;
  std::map<SlotKey, Slot> slots_{};
  std::map<SourceId, StreamState> streams_{};
  std::uint64_t total_accepted_{0};
  std::uint64_t total_fenced_{0};
  std::uint64_t duplicates_{0};
  std::uint64_t revision_gaps_{0};
  std::uint64_t counter_resets_{0};
  std::uint64_t counter_wraps_{0};
  std::map<FenceReason, std::uint64_t> fence_counts_{};
};

[[nodiscard]] std::string format_slot_view(const SlotKey& slot, const SlotView& view);

}  // namespace linkobs
