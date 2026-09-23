// Link Observatory - the runtime facade.
//
// The observatory is the only owner of registry, evidence store and transition
// history. It applies declarations and observations, fences replayed or
// superseded input, records transitions, persists and restores. It does not
// decide route eligibility, does not own link authority, does not repair links
// and does not program ports: it observes and interprets.
//
// Concurrency contract (proved in docs/locking-audit.md and by the concurrency
// suite):
//   * one lock class, ObservatoryState, guards all mutable runtime state,
//   * queue locks are never held while the state lock is taken and vice versa,
//   * observations are routed to a worker by a deterministic function of the
//     source name, so records from one source are always applied in submission
//     order, whatever the worker count,
//   * declarations are applied synchronously, so a caller that submits
//     declarations before observations always sees the declarations first.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "linkobs/classify/classifier.hpp"
#include "linkobs/core/lock_tracker.hpp"
#include "linkobs/ingest/codec.hpp"
#include "linkobs/persist/snapshot.hpp"
#include "linkobs/runtime/config.hpp"
#include "linkobs/runtime/worker.hpp"
#include "linkobs/store/evidence_store.hpp"
#include "linkobs/store/history.hpp"
#include "linkobs/store/link_registry.hpp"

namespace linkobs {

class Observatory {
 public:
  explicit Observatory(RuntimeConfig config, std::unique_ptr<Clock> clock = nullptr);
  ~Observatory();

  Observatory(const Observatory&) = delete;
  Observatory& operator=(const Observatory&) = delete;

  /// Validates the configuration, restores a snapshot when configured, and
  /// starts the ingest workers.
  [[nodiscard]] Status start();

  /// Stops the workers (draining pending work) and persists when configured.
  [[nodiscard]] Status stop();

  [[nodiscard]] bool started() const;

  // -- declarations ---------------------------------------------------------

  [[nodiscard]] Status apply_source(const SourceDecl& declaration);
  [[nodiscard]] Status apply_link(const LinkDecl& declaration);

  /// Applies every record in the batch synchronously, in order. Declarations and
  /// observations may be mixed.
  [[nodiscard]] Status apply_batch(const Batch& batch);

  /// Submits a batch. Declarations are applied synchronously; observations are
  /// routed to workers when workers are configured, otherwise applied inline.
  [[nodiscard]] Status submit_batch(Batch batch);

  /// Convenience for a single observation.
  [[nodiscard]] Status submit_observation(const ObservationDecl& declaration);

  // -- queries --------------------------------------------------------------

  [[nodiscard]] std::vector<LinkEntry> links() const;
  [[nodiscard]] std::vector<SourceRecord> sources() const;
  [[nodiscard]] std::optional<LinkEntry> find_link(std::string_view name) const;

  [[nodiscard]] std::vector<Classification> classify_all() const;
  [[nodiscard]] std::optional<Classification> classify(LinkId link) const;
  [[nodiscard]] std::optional<Classification> classify(std::string_view link_name) const;

  [[nodiscard]] std::vector<TransitionEvent> transitions(LinkId link, std::size_t limit) const;
  [[nodiscard]] std::vector<TransitionEvent> recent_transitions(std::size_t limit) const;
  [[nodiscard]] std::vector<EvaluatedEvidence> evidence_for(LinkId link,
                                                           const MetricKey& key) const;
  [[nodiscard]] std::vector<MetricKey> metrics_for(LinkId link) const;
  [[nodiscard]] std::vector<SlotView> slot_views(LinkId link) const;
  [[nodiscard]] std::vector<Evidence> link_evidence(LinkId link, std::size_t limit) const;

  // -- operations -----------------------------------------------------------

  /// Writes a snapshot now. Returns the write result.
  [[nodiscard]] SnapshotWriteResult persist();
  /// Re-reads the configured snapshot, replacing in-memory state.
  [[nodiscard]] Status reload();

  [[nodiscard]] RuntimeStatistics statistics() const;
  [[nodiscard]] std::uint64_t fence_count(FenceReason reason) const;
  [[nodiscard]] bool restored_from_snapshot() const;
  [[nodiscard]] bool loaded_from_backup() const;
  [[nodiscard]] std::string load_detail() const;

  [[nodiscard]] const RuntimeConfig& config() const noexcept { return config_; }
  [[nodiscard]] RuntimeId runtime_id() const noexcept { return runtime_id_; }
  [[nodiscard]] IncarnationId incarnation_id() const noexcept { return incarnation_id_; }
  [[nodiscard]] TimePoint now() const;

 private:
  [[nodiscard]] Status apply_record_locked(const ParsedRecord& record);
  void apply_observation_locked(const ObservationDecl& declaration);
  void refresh_classification_locked(LinkId link, TimePoint now);
  [[nodiscard]] Status restore_from(const SnapshotContents& contents);
  [[nodiscard]] SnapshotContents collect_snapshot() const;

  RuntimeConfig config_;
  std::unique_ptr<Clock> clock_;
  RuntimeId runtime_id_{};
  IncarnationId incarnation_id_{};
  LinkRegistry registry_;
  EvidenceStore store_;
  TransitionLog transitions_;
  std::unique_ptr<WorkerPool> workers_;

  mutable TrackedLock state_lock_{LockClass::ObservatoryState};
  std::map<LinkId, LinkState> observed_states_{};
  std::map<LinkId, std::string> last_oper_text_{};
  std::map<LinkId, std::string> last_admin_text_{};
  std::map<FenceReason, std::uint64_t> own_fence_counts_{};
  RuntimeStatistics stats_{};
  bool started_{false};
  bool restored_{false};
  bool loaded_from_backup_{false};
  std::string load_detail_{};
};

}  // namespace linkobs
