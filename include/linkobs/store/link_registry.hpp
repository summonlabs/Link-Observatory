// Link Observatory - link and source registry.
//
// The registry is the only place that decides whether a link or a source is
// known. It also owns topology generation currency: observing a generation that
// has never been seen makes it current and retires the previous one, while
// observing a generation that was already retired is refused. Both directions
// fence prior evidence, which is what makes a topology change safe.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "linkobs/domain/link.hpp"
#include "linkobs/domain/observation.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/store/history.hpp"

namespace linkobs {

struct GenerationObservation {
  bool changed{false};
  bool behind{false};
  GenerationId previous{};
  GenerationId current{};
};

/// Which topology generation is current for a link, and which ones have already
/// been superseded.
///
/// Superseded generations are retained so that a replayed record from an old
/// topology is fenced instead of being mistaken for a new topology change. The
/// list is bounded, and the eviction order is oldest first.
struct LinkGenerationState {
  GenerationId current_generation{};
  bool has_generation{false};
  std::vector<GenerationId> retired_generations{};
  std::uint64_t generation_changes{0};
};

/// Everything about a link that survives a restart.
struct PersistedLink {
  LinkFacts facts{};
  LinkEntry entry{};
  LinkGenerationState generations{};
};

/// Registry state. Not independently synchronised: the observatory owns it and
/// holds the observatory lock for every access. See docs/locking-audit.md.
class LinkRegistry {
 public:
  explicit LinkRegistry(const Policy& policy) : policy_(policy) {}

  [[nodiscard]] Status register_source(SourceRecord record);
  [[nodiscard]] Status retire_source(SourceId id);
  [[nodiscard]] Status register_link(const LinkFacts& facts, TimePoint now);
  [[nodiscard]] Status retire_link(LinkId id, TimePoint now);
  [[nodiscard]] Status ensure_implicit_link(LinkId id, std::string_view name, TimePoint now);

  [[nodiscard]] const SourceRecord* find_source(SourceId id) const;
  [[nodiscard]] const LinkEntry* find_link(LinkId id) const;

  /// Records the observation of a topology generation for a link.
  [[nodiscard]] GenerationObservation observe_generation(LinkId id,
                                                         GenerationId generation,
                                                         TimePoint now);

  [[nodiscard]] GenerationId current_generation(LinkId id) const;
  [[nodiscard]] bool generation_retired(LinkId id, GenerationId generation) const;
  [[nodiscard]] std::size_t retired_generation_count(LinkId id) const;
  [[nodiscard]] LinkGenerationState generation_state(LinkId id) const;

  /// Updates the last-evidence bookkeeping for a link.
  void note_accepted(LinkId id, TimePoint observed_at, TimePoint accepted_at);
  void note_fenced(LinkId id);
  void note_registered(LinkId id, TimePoint now);

  [[nodiscard]] std::vector<LinkEntry> links() const;
  [[nodiscard]] std::vector<SourceRecord> sources() const;
  [[nodiscard]] std::vector<PersistedLink> persisted_links() const;
  [[nodiscard]] std::size_t link_count() const noexcept { return links_.size(); }
  [[nodiscard]] std::size_t source_count() const noexcept { return sources_.size(); }

  /// Restores a persisted link, including its generation fencing state. Only the
  /// snapshot loader calls this.
  [[nodiscard]] Status restore(const PersistedLink& link);

  [[nodiscard]] const Policy& policy() const noexcept { return policy_; }

 private:
  struct LinkState {
    LinkEntry entry{};
    LinkGenerationState generations{};
  };

  Policy policy_;
  std::map<LinkId, LinkState> links_{};
  std::map<SourceId, SourceRecord> sources_{};
};

}  // namespace linkobs
