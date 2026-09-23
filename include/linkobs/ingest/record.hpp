// Link Observatory - the LOR ingest record grammar.
//
// A record is one line of "key=value" fields separated by single spaces:
//
//   v=1 kind=source name=<token> authority=<level> provenance=<class> [retired=true]
//   v=1 kind=link name=<name> link-kind=<kind> linkgen=<name> provenance=<class>
//       [capacity-in-bps=<n>] [capacity-out-bps=<n>]
//       [local-endpoint=<name>] [remote-endpoint=<name>]
//       [local-port=<name>] [remote-port=<name>]
//   v=1 kind=observation link=<name> metric=<key> source=<name> incarnation=<name>
//       epoch=<name> generation=<name> revision=<n> observed-at=<ns> received-at=<ns>
//       <value fields>
//
// Value fields by metric family:
//   admin-state      value=unknown|enabled|disabled
//   oper-state       value=unknown|up|down|degraded
//   octets           counter=<n> width=bits32|bits64 [reset=true]
//   errors           counter=<n> width=bits32|bits64 [reset=true]
//   discards         counter=<n> width=bits32|bits64 [reset=true]
//   util-reported    ppb=<n>
//   signal-quality   ppb=<n> [degraded=true]
//   flap-report      events=<n> window-ns=<n>
//
// Rules that make the grammar safe to accept untrusted text:
//   * the version field is mandatory and must match exactly,
//   * unknown keys are rejected, never ignored: a typo must not silently drop a
//     value,
//   * duplicate keys are rejected,
//   * every field is length bounded and every numeric field is range checked,
//   * the parser never clamps, repairs or defaults a malformed value,
//   * comments start with '#' and blank lines are ignored.
//
// The runtime does not decide what a field means from its name alone. Reads are
// strict: a value that does not parse is an error, not a zero.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "linkobs/core/status.hpp"
#include "linkobs/domain/link.hpp"
#include "linkobs/domain/metric.hpp"
#include "linkobs/domain/observation.hpp"
#include "linkobs/domain/policy.hpp"
#include "linkobs/time/clock.hpp"

namespace linkobs {

struct SourceDecl {
  std::string name{};
  SourceAuthority authority{SourceAuthority::Unknown};
  ProvenanceClass provenance{ProvenanceClass::Unknown};
  bool retired{false};

  [[nodiscard]] SourceId id() const { return SourceId::from_name(name); }
};

struct LinkDecl {
  std::string name{};
  LinkKind kind{LinkKind::Unknown};
  std::string generation{};
  ProvenanceClass provenance{ProvenanceClass::Unknown};
  bool capacity_known{false};
  std::uint64_t capacity_in_bps{0};
  std::uint64_t capacity_out_bps{0};
  std::string local_endpoint{};
  std::string remote_endpoint{};
  std::string local_port{};
  std::string remote_port{};
};

struct ObservationDecl {
  std::string link{};
  std::string source{};
  std::string incarnation{};
  std::string epoch{};
  std::string generation{};
  std::uint64_t revision{0};
  Nanos observed_at{0};
  Nanos received_at{0};
  MetricKey key{};
  MetricPayload payload{};
};

struct ParsedRecord {
  enum class Kind : std::uint8_t { None = 0, Source = 1, Link = 2, Observation = 3 };

  Kind kind{Kind::None};
  SourceDecl source{};
  LinkDecl link{};
  ObservationDecl observation{};
};

/// Parses one line. The status is Ok only when the record is fully valid.
[[nodiscard]] Result<ParsedRecord> parse_record(std::string_view line, const Policy& policy);

/// Renders a declaration back to canonical LOR text. Field order is fixed, so
/// the output is byte stable.
[[nodiscard]] std::string format_record(const SourceDecl& declaration);
[[nodiscard]] std::string format_record(const LinkDecl& declaration);
[[nodiscard]] std::string format_record(const ObservationDecl& declaration);

/// Number of lines in the input that are neither blank nor comments.
[[nodiscard]] std::size_t count_record_lines(std::string_view text) noexcept;

}  // namespace linkobs
