// Link Observatory - link identity and declared facts.
//
// A link is registered, never guessed. Registration carries the operator
// supplied name, the endpoint/port anchors, the declared kind, the capacity used
// for utilization derivation, and the topology generation the facts belong to.
// Observations that reference an unregistered link are refused unless implicit
// registration is explicitly enabled, and implicit links never carry capacity -
// so they can never produce a derived utilization.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "linkobs/core/strong_id.hpp"
#include "linkobs/domain/enums.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/time/clock.hpp"

namespace linkobs {

/// Operator supplied, stable identity of a link. The runtime hashes it into a
/// LinkId; the name itself is retained for reporting.
[[nodiscard]] LinkId derive_link_id(std::string_view name);

struct LinkFacts {
  LinkId id{};
  std::string name{};
  EndpointId local_endpoint{};
  EndpointId remote_endpoint{};
  PortId local_port{};
  PortId remote_port{};
  LinkKind kind{LinkKind::Unknown};
  GenerationId generation{};
  bool capacity_known{false};
  std::uint64_t capacity_in_bps{0};
  std::uint64_t capacity_out_bps{0};
  ProvenanceClass provenance{ProvenanceClass::Unknown};
};

struct LinkEntry {
  LinkFacts facts{};
  TimePoint registered_at{};
  TimePoint last_evidence_at{};
  TimePoint last_accepted_at{};
  std::uint64_t accepted_observations{0};
  std::uint64_t fenced_observations{0};
  bool retired{false};
  bool has_evidence{false};
};

/// Declared capacity for a direction, if the link declares one.
[[nodiscard]] bool capacity_for(const LinkFacts& facts,
                                Direction direction,
                                std::uint64_t& out) noexcept;

[[nodiscard]] Status validate_link_facts(const LinkFacts& facts, const Policy& policy);

/// Deterministic rendering of a link fact set.
[[nodiscard]] std::string format_link_facts(const LinkFacts& facts);

}  // namespace linkobs
