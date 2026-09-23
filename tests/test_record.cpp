// The LOR record grammar: strict acceptance, strict rejection, canonical output.

#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/ingest/codec.hpp"

namespace {

using namespace linkobs;
using namespace linkobs::test;

LO_TEST(record, valid_records_parse) {
  const Policy policy = make_default_policy();

  const Result<ParsedRecord> source = parse_record(
      "v=1 kind=source name=agent-1 authority=primary provenance=real", policy);
  LO_REQUIRE(source.ok());
  LO_CHECK_EQ(source.value().kind, ParsedRecord::Kind::Source);
  LO_CHECK_EQ(source.value().source.name, std::string("agent-1"));
  LO_CHECK_EQ(source.value().source.authority, SourceAuthority::Primary);
  LO_CHECK_EQ(source.value().source.provenance, ProvenanceClass::Real);

  const Result<ParsedRecord> link = parse_record(
      "v=1 kind=link name=leaf1-eth0 link-kind=physical linkgen=gen-7 provenance=real "
      "capacity-in-bps=400000000000 capacity-out-bps=400000000000 local-port=p1 remote-port=p2",
      policy);
  LO_REQUIRE(link.ok());
  LO_CHECK_EQ(link.value().link.capacity_in_bps, static_cast<std::uint64_t>(400000000000ULL));
  LO_CHECK(link.value().link.capacity_known);
  LO_CHECK_EQ(link.value().link.local_port, std::string("p1"));

  const Result<ParsedRecord> observation = parse_record(
      "v=1 kind=observation link=leaf1-eth0 metric=octets/in source=agent-1 incarnation=inc-9 "
      "epoch=epoch-3 generation=gen-7 revision=42 observed-at=1700000000000000000 "
      "received-at=1700000000000000001 counter=123456 width=bits64",
      policy);
  LO_REQUIRE(observation.ok());
  LO_CHECK_EQ(observation.value().observation.revision, static_cast<std::uint64_t>(42));
  const auto* counter = std::get_if<CounterReading>(&observation.value().observation.payload);
  LO_REQUIRE(counter != nullptr);
  LO_CHECK_EQ(counter->raw, static_cast<std::uint64_t>(123456));

  const Result<ParsedRecord> state = parse_record(
      "v=1 kind=observation link=l metric=oper-state source=s incarnation=i epoch=e generation=g "
      "revision=1 observed-at=1 received-at=1 value=down",
      policy);
  LO_REQUIRE(state.ok());
  const auto* oper = std::get_if<OperStateReading>(&state.value().observation.payload);
  LO_REQUIRE(oper != nullptr);
  LO_CHECK_EQ(oper->value, OperStateValue::Down);
}

LO_TEST(record, comments_and_blank_lines_are_ignored) {
  const Policy policy = make_default_policy();
  LO_CHECK(!parse_record("", policy).ok());
  LO_CHECK(!parse_record("   ", policy).ok());
  LO_CHECK(!parse_record("# a comment", policy).ok());
  LO_CHECK_EQ(count_record_lines("# comment\n\nv=1 kind=source name=s authority=primary "
                                 "provenance=real\n"),
              static_cast<std::size_t>(1));
}

LO_TEST(record, unknown_and_duplicate_fields_are_rejected) {
  const Policy policy = make_default_policy();
  const Result<ParsedRecord> unknown =
      parse_record("v=1 kind=source name=s authority=primary provenance=real mystery=1", policy);
  LO_CHECK(!unknown.ok());
  LO_CHECK(unknown.status().message().find("unknown field") != std::string::npos);

  const Result<ParsedRecord> duplicate =
      parse_record("v=1 kind=source name=s name=t authority=primary provenance=real", policy);
  LO_CHECK(!duplicate.ok());
  LO_CHECK(duplicate.status().message().find("duplicate") != std::string::npos);
}

LO_TEST(record, version_and_kind_are_enforced) {
  const Policy policy = make_default_policy();
  LO_CHECK_EQ(parse_record("v=2 kind=source name=s authority=primary provenance=real", policy)
                  .status()
                  .code(),
              StatusCode::VersionMismatch);
  LO_CHECK(!parse_record("kind=source name=s authority=primary provenance=real", policy).ok());
  LO_CHECK_EQ(parse_record("v=1 kind=nonsense", policy).status().code(), StatusCode::Unsupported);
}

LO_TEST(record, provenance_must_be_declared) {
  const Policy policy = make_default_policy();
  LO_CHECK(!parse_record("v=1 kind=source name=s authority=primary provenance=unknown", policy).ok());
  LO_CHECK(!parse_record("v=1 kind=source name=s authority=primary", policy).ok());
  LO_CHECK(!parse_record("v=1 kind=source name=s authority=sideways provenance=real", policy).ok());
}

LO_TEST(record, size_and_range_bounds_are_enforced) {
  Policy policy = make_default_policy();
  policy.limits.max_record_bytes = 64U;

  const std::string long_line =
      "v=1 kind=source name=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "
      "authority=primary provenance=real";
  LO_CHECK(long_line.size() > policy.limits.max_record_bytes);
  LO_CHECK_EQ(parse_record(long_line, policy).status().code(), StatusCode::OutOfRange);

  Policy generous = make_default_policy();
  LO_CHECK(!parse_record("v=1 kind=observation link=l metric=octets/in source=s incarnation=i "
                         "epoch=e generation=g revision=1 observed-at=1 received-at=1 "
                         "counter=99999999999999999999999 width=bits64",
                         generous)
                 .ok());
  LO_CHECK(!parse_record("v=1 kind=observation link=l metric=octets/in source=s incarnation=i "
                         "epoch=e generation=g revision=1 observed-at=1 received-at=1 "
                         "counter=1 width=bits128",
                         generous)
                 .ok());
  LO_CHECK(!parse_record("v=1 kind=observation link=l metric=octets/in source=s incarnation=i "
                         "epoch=e generation=g revision=1 observed-at=1 received-at=1 "
                         "counter=1",
                         generous)
                 .ok());
}

LO_TEST(record, formatted_records_parse_back) {
  const Policy policy = make_default_policy();
  const SourceDecl source = make_source("agent-9", SourceAuthority::Secondary,
                                        ProvenanceClass::Replayed);
  const Result<ParsedRecord> source_round_trip = parse_record(format_record(source), policy);
  LO_CHECK_MSG(source_round_trip.ok(),
               source_round_trip.status().to_string() + " input=[" + format_record(source) + "]");
  LO_REQUIRE(source_round_trip.ok());
  LO_CHECK_EQ(source_round_trip.value().source.name, source.name);
  LO_CHECK_EQ(source_round_trip.value().source.authority, source.authority);
  LO_CHECK_EQ(source_round_trip.value().source.provenance, source.provenance);

  const LinkDecl link = make_link("leaf2-eth1", 25000000000ULL, 25000000000ULL, "gen-2");
  const Result<ParsedRecord> link_round_trip = parse_record(format_record(link), policy);
  LO_CHECK_MSG(link_round_trip.ok(),
               link_round_trip.status().to_string() + " input=[" + format_record(link) + "]");
  LO_REQUIRE(link_round_trip.ok());
  LO_CHECK_EQ(link_round_trip.value().link.name, link.name);
  LO_CHECK_EQ(link_round_trip.value().link.generation, link.generation);
  LO_CHECK_EQ(link_round_trip.value().link.capacity_in_bps, link.capacity_in_bps);

  const ObservationDecl observation =
      make_octets("leaf2-eth1", "agent-9", StreamPosition{}, 900000U, 10, 11);
  const Result<ParsedRecord> observation_round_trip =
      parse_record(format_record(observation), policy);
  LO_CHECK_MSG(observation_round_trip.ok(),
               observation_round_trip.status().to_string() + " input=[" +
                   format_record(observation) + "]");
  LO_REQUIRE(observation_round_trip.ok());
  LO_CHECK(observation_round_trip.value().observation.link == observation.link);
  LO_CHECK(observation_round_trip.value().observation.key == observation.key);
}

LO_TEST(record, batch_reports_every_bad_line) {
  const Policy policy = make_default_policy();
  const std::string text =
      "# header\n"
      "v=1 kind=source name=s authority=primary provenance=real\n"
      "garbage line\n"
      "v=1 kind=source name=t authority=primary provenance=real extra=1\n"
      "v=1 kind=link name=l link-kind=logical linkgen=g provenance=real\n";

  const ParseOutcome outcome = parse_batch(text, policy);
  LO_CHECK(outcome.ok());
  LO_CHECK_EQ(outcome.batch.records.size(), static_cast<std::size_t>(2));
  LO_CHECK_EQ(outcome.rejected, static_cast<std::size_t>(2));
  LO_CHECK_EQ(outcome.errors.size(), static_cast<std::size_t>(2));
  LO_CHECK_EQ(outcome.ignored_lines, static_cast<std::size_t>(1));
}

LO_TEST(record, batch_bound_is_enforced_not_bypassed) {
  Policy policy = make_default_policy();
  policy.limits.max_records_per_batch = 2U;
  const std::string text =
      "v=1 kind=source name=a authority=primary provenance=real\n"
      "v=1 kind=source name=b authority=primary provenance=real\n"
      "v=1 kind=source name=c authority=primary provenance=real\n";
  const ParseOutcome outcome = parse_batch(text, policy);
  LO_CHECK(!outcome.ok());
  LO_CHECK_EQ(outcome.status.code(), StatusCode::CapacityExceeded);
  LO_CHECK_EQ(outcome.batch.records.size(), static_cast<std::size_t>(2));
}

}  // namespace
