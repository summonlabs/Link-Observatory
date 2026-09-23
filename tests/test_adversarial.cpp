// Adversarial ingest: replays, superseded identities, authority downgrades and
// hostile persistence payloads.

#include <fstream>
#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/core/sha256.hpp"
#include "linkobs/persist/snapshot.hpp"
#include "linkobs/runtime/observatory.hpp"

namespace {

using namespace linkobs;
using namespace linkobs::test;

[[nodiscard]] std::unique_ptr<Observatory> rig(std::string snapshot_path = {}) {
  RuntimeConfig config = make_config(std::move(snapshot_path), 0U);
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  auto observatory = std::make_unique<Observatory>(std::move(config), std::move(clock));
  const Status started = observatory->start();
  (void)started;
  return observatory;
}

LO_TEST(adversarial, an_exact_duplicate_is_fenced_and_counted) {
  std::unique_ptr<Observatory> observatory = rig();
  (void)observatory->apply_source(make_source("agent-0"));
  (void)observatory->apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL));

  StreamPosition position{};
  const ObservationDecl declaration =
      make_oper("link-a", "agent-0", position, OperStateValue::Up, kBaseInstant, kBaseInstant);
  LO_CHECK(observatory->submit_observation(declaration).ok());
  LO_CHECK(observatory->submit_observation(declaration).ok());
  LO_CHECK_EQ(observatory->fence_count(FenceReason::DuplicateObservation),
              static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(observatory->statistics().records_accepted, static_cast<std::uint64_t>(1));
}

LO_TEST(adversarial, a_replayed_older_revision_is_fenced) {
  std::unique_ptr<Observatory> observatory = rig();
  (void)observatory->apply_source(make_source("agent-0"));
  (void)observatory->apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL));

  StreamPosition position{};
  position.revision = 10U;
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                              kBaseInstant, kBaseInstant))
               .ok());
  position.revision = 4U;
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Down,
                                              kBaseInstant + 1000, kBaseInstant + 1000))
               .ok());
  LO_CHECK_EQ(observatory->fence_count(FenceReason::RevisionReplay),
              static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(observatory->statistics().records_accepted, static_cast<std::uint64_t>(1));
}

LO_TEST(adversarial, a_superseded_epoch_is_fenced) {
  std::unique_ptr<Observatory> observatory = rig();
  (void)observatory->apply_source(make_source("agent-0"));
  (void)observatory->apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL));

  StreamPosition position{};
  position.epoch = "epoch-0";
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                              kBaseInstant, kBaseInstant))
               .ok());
  position.epoch = "epoch-1";
  position.revision = 20U;
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                              kBaseInstant + 1000, kBaseInstant + 1000))
               .ok());
  // The old epoch is now retired; replaying it must be refused even though its
  // revision is higher than anything seen in that epoch.
  position.epoch = "epoch-0";
  position.revision = 99U;
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Down,
                                              kBaseInstant + 2000, kBaseInstant + 2000))
               .ok());
  LO_CHECK_EQ(observatory->fence_count(FenceReason::EpochBehind), static_cast<std::uint64_t>(1));
}

LO_TEST(adversarial, a_superseded_incarnation_is_fenced) {
  std::unique_ptr<Observatory> observatory = rig();
  (void)observatory->apply_source(make_source("agent-0"));
  (void)observatory->apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL));

  StreamPosition position{};
  position.incarnation = "inc-0";
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                              kBaseInstant, kBaseInstant))
               .ok());
  position.incarnation = "inc-1";
  position.revision = 1U;
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                              kBaseInstant + 1000, kBaseInstant + 1000))
               .ok());
  position.incarnation = "inc-0";
  position.revision = 50U;
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Down,
                                              kBaseInstant + 2000, kBaseInstant + 2000))
               .ok());
  LO_CHECK_EQ(observatory->fence_count(FenceReason::IncarnationBehind),
              static_cast<std::uint64_t>(1));
}

LO_TEST(adversarial, a_superseded_topology_generation_is_fenced) {
  std::unique_ptr<Observatory> observatory = rig();
  (void)observatory->apply_source(make_source("agent-0"));
  (void)observatory->apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL, "gen-0"));

  StreamPosition position{};
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                              kBaseInstant, kBaseInstant))
               .ok());
  position.generation = "gen-1";
  position.revision += 1U;
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                              kBaseInstant + 1000, kBaseInstant + 1000))
               .ok());
  position.generation = "gen-0";
  position.revision += 1U;
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Down,
                                              kBaseInstant + 2000, kBaseInstant + 2000))
               .ok());
  LO_CHECK_EQ(observatory->fence_count(FenceReason::GenerationBehind),
              static_cast<std::uint64_t>(1));
}

LO_TEST(adversarial, evidence_from_a_superseded_generation_is_not_current) {
  std::unique_ptr<Observatory> observatory = rig();
  (void)observatory->apply_source(make_source("agent-0"));
  (void)observatory->apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL, "gen-0"));

  StreamPosition position{};
  // A down state recorded under generation 0.
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Down,
                                              kBaseInstant, kBaseInstant))
               .ok());
  // A topology change moves the link to generation 1 without any new state.
  position.generation = "gen-1";
  position.revision += 1U;
  LO_CHECK(observatory
               ->submit_observation(make_octets("link-a", "agent-0", position, 1000U,
                                                kBaseInstant + 1000, kBaseInstant + 1000))
               .ok());

  const std::optional<Classification> classification = observatory->classify("link-a");
  LO_REQUIRE(classification.has_value());
  // The old "down" evidence is history now, and cannot decide the current state.
  LO_CHECK(!(classification->state == LinkState::Down));
}

LO_TEST(adversarial, a_lower_authority_source_cannot_overwrite_newer_higher_authority_evidence) {
  std::unique_ptr<Observatory> observatory = rig();
  (void)observatory->apply_source(make_source("primary-agent", SourceAuthority::Primary));
  (void)observatory->apply_source(make_source("secondary-agent", SourceAuthority::Secondary));
  (void)observatory->apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL));

  StreamPosition position{};
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "primary-agent", position, OperStateValue::Up,
                                              kBaseInstant + 1000, kBaseInstant + 1000))
               .ok());
  LO_CHECK(observatory
               ->submit_observation(make_oper("link-a", "secondary-agent", position,
                                              OperStateValue::Down, kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK_EQ(observatory->fence_count(FenceReason::AuthorityDowngrade),
              static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(observatory->statistics().records_accepted, static_cast<std::uint64_t>(1));
}

LO_TEST(adversarial, an_unregistered_source_is_fenced) {
  std::unique_ptr<Observatory> observatory = rig();
  (void)observatory->apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL));
  (void)observatory->submit_observation(make_oper("link-a", "ghost", StreamPosition{},
                                                  OperStateValue::Up, kBaseInstant, kBaseInstant));
  LO_CHECK_EQ(observatory->fence_count(FenceReason::UnknownSource),
              static_cast<std::uint64_t>(1));
}

LO_TEST(adversarial, a_disabled_family_is_fenced_explicitly) {
  RuntimeConfig config = make_config({}, 0U);
  config.families.disable(MetricFamily::Octets);
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  (void)started;
  (void)observatory.apply_source(make_source("agent-0"));
  (void)observatory.apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL));
  (void)observatory.submit_observation(
      make_octets("link-a", "agent-0", StreamPosition{}, 10U, kBaseInstant, kBaseInstant));
  LO_CHECK_EQ(observatory.fence_count(FenceReason::UnsupportedFamily),
              static_cast<std::uint64_t>(1));
}

LO_TEST(adversarial, a_source_without_a_declared_provenance_is_refused) {
  std::unique_ptr<Observatory> observatory = rig();
  SourceRecord record{};
  record.id = SourceId::from_name("opaque");
  record.name = "opaque";
  record.authority = SourceAuthority::Primary;
  record.provenance = ProvenanceClass::Unknown;
  LO_CHECK(!observatory->apply_source(SourceDecl{"opaque", SourceAuthority::Primary,
                                                 ProvenanceClass::Unknown, false})
                 .ok());
  (void)record;
  LO_CHECK_EQ(observatory->sources().size(), static_cast<std::size_t>(0));
}

LO_TEST(adversarial, a_snapshot_with_a_plausible_but_wrong_digest_is_refused) {
  TempPath path{"hostile"};
  {
    RuntimeConfig config = make_config(path.path(), 0U);
    auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
    Observatory observatory(std::move(config), std::move(clock));
    const Status started = observatory.start();
    (void)started;
    (void)observatory.apply_source(make_source("agent-0"));
    (void)observatory.apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL));
    (void)observatory.submit_observation(make_oper("link-a", "agent-0", StreamPosition{},
                                                   OperStateValue::Up, kBaseInstant, kBaseInstant));
    const Status stopped = observatory.stop();
    (void)stopped;
  }

  std::ifstream stream(path.path(), std::ios::binary);
  std::string bytes;
  stream.seekg(0, std::ios::end);
  const std::streamoff length = stream.tellg();
  stream.seekg(0, std::ios::beg);
  bytes.assign(static_cast<std::size_t>(length), '\0');
  stream.read(bytes.data(), length);
  stream.close();

  // Change one payload byte and re-seal the header so only the payload digest
  // stands between the attacker and the runtime.
  bytes[100] = static_cast<char>(bytes[100] ^ 0x01);
  const Sha256Digest header_digest = sha256(std::string_view{bytes}.substr(0, 56U));
  for (std::size_t index = 0; index < header_digest.size(); ++index) {
    bytes[56U + index] = static_cast<char>(header_digest[index]);
  }
  std::ofstream out(path.path(), std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();

  const SnapshotLoadResult loaded = read_snapshot(path.path(), make_default_policy());
  LO_CHECK(!loaded.ok);
  LO_CHECK_EQ(loaded.status.code(), StatusCode::CorruptData);
}

LO_TEST(adversarial, an_oversized_snapshot_is_refused_before_allocation) {
  TempPath path{"oversized"};
  std::string bytes(4096U, 'x');
  bytes[0] = 'L';
  bytes[1] = 'K';
  bytes[2] = 'O';
  bytes[3] = 'B';
  bytes[4] = 'S';
  bytes[5] = 'N';
  bytes[6] = 'A';
  bytes[7] = 'P';
  {
    std::ofstream out(path.path(), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  Policy policy = make_default_policy();
  policy.limits.max_snapshot_bytes = 128U;
  const SnapshotLoadResult loaded = read_snapshot(path.path(), policy);
  LO_CHECK(!loaded.ok);
  LO_CHECK_EQ(loaded.status.code(), StatusCode::CapacityExceeded);
}

}  // namespace
