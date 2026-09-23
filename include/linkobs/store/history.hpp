// Link Observatory - bounded transition history.
//
// History is what makes a state change explainable afterwards, so it must be
// complete for the window it covers and honest about what it dropped. Every
// buffer is a ring with a hard bound; dropping a record increments a counter
// that is reported, never hidden.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "linkobs/core/strong_id.hpp"
#include "linkobs/domain/enums.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/time/clock.hpp"

namespace linkobs {

struct TransitionEvent {
  TransitionId id{};
  LinkId link{};
  TransitionKind kind{TransitionKind::LinkRegistered};
  TimePoint at{};
  GenerationId generation{};
  SourceId source{};
  std::string from{};
  std::string to{};
  std::uint64_t detail{0};
};

/// Computes the content identity of a transition. Two identical transitions
/// with the same instant collapse to the same identity.
[[nodiscard]] TransitionId compute_transition_id(const TransitionEvent& event);

class TransitionLog {
 public:
  explicit TransitionLog(const Limits& limits) : limits_(limits) {}

  /// Appends an event, assigning its identity. Enforces the per-link bound and
  /// the global bound; anything evicted is counted.
  void record(TransitionEvent event);

  [[nodiscard]] std::vector<TransitionEvent> for_link(LinkId link, std::size_t limit) const;
  [[nodiscard]] std::vector<TransitionEvent> recent(std::size_t limit) const;
  [[nodiscard]] std::size_t total_events() const noexcept { return total_records_; }
  [[nodiscard]] std::uint64_t dropped_events() const noexcept { return dropped_; }
  [[nodiscard]] std::size_t distinct_links() const noexcept { return per_link_.size(); }
  [[nodiscard]] std::size_t events_for(LinkId link) const;

  /// Transitions recorded for a link at or after the given instant, bounded.
  [[nodiscard]] std::size_t count_since(LinkId link, TimePoint since, std::size_t limit) const;

  void clear();

 private:
  Limits limits_;
  std::deque<TransitionEvent> global_{};
  std::map<LinkId, std::deque<TransitionEvent>> per_link_{};
  std::size_t total_records_{0};
  std::uint64_t dropped_{0};
};

/// Deterministic rendering of one transition.
[[nodiscard]] std::string format_transition(const TransitionEvent& event);

}  // namespace linkobs
