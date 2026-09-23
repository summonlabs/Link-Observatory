// Persistence and restart semantics.

#include <cstdio>
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

[[nodiscard]] std::string read_all(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::string content;
  stream.seekg(0, std::ios::end);
  const std::streamoff length = stream.tellg();
  stream.seekg(0, std::ios::beg);
  content.assign(static_cast<std::size_t>(length), '\0');
  if (length > 0) {
    stream.read(content.data(), length);
  }
  return content;
}

void write_all(const std::string& path, std::string_view data) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(data.data(), static_cast<std::streamsize>(data.size()));
}

/// Runs one runtime generation against a snapshot file.
[[nodiscard]] RuntimeStatistics run_generation(const std::string& path,
                                               std::string_view incarnation,
                                               std::uint64_t revision_base,
                                               Nanos at,
                                               bool down) {
  RuntimeConfig config = make_config(path);
  config.incarnation_name = std::string{incarnation};
  auto clock = std::make_unique<ManualClock>(instant(at));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  (void)started;
  (void)observatory.apply_source(make_source("agent-0"));
  (void)observatory.apply_link(make_link("link-a", 1000000000ULL));

  StreamPosition position{};
  position.revision = revision_base;
  (void)observatory.submit_observation(make_oper("link-a", "agent-0", position,
                                                 down ? OperStateValue::Down : OperStateValue::Up,
                                                 at, at));
  position.revision = revision_base + 1U;
  (void)observatory.submit_observation(make_octets("link-a", "agent-0", position, 1000U, at, at));
  const Status stopped = observatory.stop();
  (void)stopped;
  return observatory.statistics();
}

LO_TEST(persist, round_trip_preserves_evidence_and_declarations) {
  TempPath path{"round-trip"};
  (void)run_generation(path.path(), "inc-a", 1U, kBaseInstant, true);

  const Policy policy = make_default_policy();
  const SnapshotLoadResult loaded = read_snapshot(path.path(), policy);
  LO_REQUIRE(loaded.ok);
  LO_CHECK(loaded.integrity_verified);
  LO_CHECK(!loaded.recovered_from_backup);
  LO_CHECK_EQ(loaded.contents.links.size(), static_cast<std::size_t>(1));
  LO_CHECK_EQ(loaded.contents.sources.size(), static_cast<std::size_t>(1));
  LO_CHECK_EQ(loaded.contents.slots.size(), static_cast<std::size_t>(2));
  LO_CHECK(loaded.contents.total_accepted >= 2U);
}

LO_TEST(persist, a_flipped_payload_byte_is_detected) {
  TempPath path{"corrupt-payload"};
  (void)run_generation(path.path(), "inc-a", 1U, kBaseInstant, false);

  std::string bytes = read_all(path.path());
  LO_REQUIRE(bytes.size() > 100U);
  bytes[bytes.size() - 4U] = static_cast<char>(bytes[bytes.size() - 4U] ^ 0x5A);

  TempPath corrupt{"corrupt-payload-target"};
  write_all(corrupt.path(), bytes);

  const SnapshotLoadResult loaded = read_snapshot(corrupt.path(), make_default_policy());
  LO_CHECK(!loaded.ok);
  LO_CHECK_EQ(loaded.status.code(), StatusCode::CorruptData);
  LO_CHECK_EQ(loaded.contents.links.size(), static_cast<std::size_t>(0));
}

LO_TEST(persist, a_flipped_header_byte_is_detected) {
  TempPath path{"corrupt-header"};
  (void)run_generation(path.path(), "inc-a", 1U, kBaseInstant, false);

  std::string bytes = read_all(path.path());
  bytes[9] = static_cast<char>(bytes[9] ^ 0x01);

  TempPath corrupt{"corrupt-header-target"};
  write_all(corrupt.path(), bytes);
  const SnapshotLoadResult loaded = read_snapshot(corrupt.path(), make_default_policy());
  LO_CHECK(!loaded.ok);
  LO_CHECK_EQ(loaded.status.code(), StatusCode::CorruptData);
}

LO_TEST(persist, truncation_is_detected) {
  TempPath path{"truncated"};
  (void)run_generation(path.path(), "inc-a", 1U, kBaseInstant, false);
  std::string bytes = read_all(path.path());
  bytes.resize(bytes.size() / 2U);

  TempPath truncated{"truncated-target"};
  write_all(truncated.path(), bytes);
  const SnapshotLoadResult loaded = read_snapshot(truncated.path(), make_default_policy());
  LO_CHECK(!loaded.ok);
  LO_CHECK_EQ(loaded.status.code(), StatusCode::CorruptData);
}

LO_TEST(persist, trailing_bytes_are_rejected) {
  TempPath path{"trailing"};
  (void)run_generation(path.path(), "inc-a", 1U, kBaseInstant, false);
  std::string bytes = read_all(path.path());
  bytes.append("extra");

  TempPath extended{"trailing-target"};
  write_all(extended.path(), bytes);
  const SnapshotLoadResult loaded = read_snapshot(extended.path(), make_default_policy());
  LO_CHECK(!loaded.ok);
  LO_CHECK_EQ(loaded.status.code(), StatusCode::CorruptData);
}

LO_TEST(persist, an_unsupported_format_version_is_refused) {
  TempPath path{"version"};
  (void)run_generation(path.path(), "inc-a", 1U, kBaseInstant, false);

  std::string bytes = read_all(path.path());
  LO_REQUIRE(bytes.size() > 88U);
  // Rewrite the format version and re-seal the header so that the version check
  // itself is what rejects the file.
  bytes[8] = static_cast<char>(99);
  bytes[9] = static_cast<char>(0);
  bytes[10] = static_cast<char>(0);
  bytes[11] = static_cast<char>(0);
  const Sha256Digest digest = sha256(std::string_view{bytes}.substr(0, 56U));
  for (std::size_t index = 0; index < digest.size(); ++index) {
    bytes[56U + index] = static_cast<char>(digest[index]);
  }

  TempPath versioned{"version-target"};
  write_all(versioned.path(), bytes);
  const SnapshotLoadResult loaded = read_snapshot(versioned.path(), make_default_policy());
  LO_CHECK(!loaded.ok);
  LO_CHECK_EQ(loaded.status.code(), StatusCode::VersionMismatch);
}

LO_TEST(persist, the_previous_snapshot_is_recovered_when_the_latest_is_damaged) {
  TempPath path{"backup-recovery"};
  (void)run_generation(path.path(), "inc-a", 1U, kBaseInstant, false);
  // A second generation rotates the first snapshot to "<path>.prev".
  (void)run_generation(path.path(), "inc-a", 3U, kBaseInstant + 1000000000LL, false);
  LO_CHECK(snapshot_exists(snapshot_backup_path(path.path())));

  std::string bytes = read_all(path.path());
  bytes[bytes.size() - 1U] = static_cast<char>(bytes[bytes.size() - 1U] ^ 0xFF);
  write_all(path.path(), bytes);

  const SnapshotLoadResult loaded = read_snapshot(path.path(), make_default_policy());
  LO_CHECK_MSG(loaded.ok, loaded.status.to_string());
  LO_REQUIRE(loaded.ok);
  LO_CHECK(loaded.recovered_from_backup);
  LO_CHECK(loaded.primary_failed);
  LO_CHECK(loaded.detail.find("backup") != std::string::npos);
}

LO_TEST(persist, a_restored_runtime_reports_the_totals_it_restored) {
  TempPath path{"restored-totals"};
  (void)run_generation(path.path(), "inc-a", 1U, kBaseInstant, false);

  const Policy policy = make_default_policy();
  const SnapshotLoadResult loaded = read_snapshot(path.path(), policy);
  LO_REQUIRE(loaded.ok);

  RuntimeConfig config = make_config(path.path());
  config.incarnation_name = "inc-b";
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant + 1000000000LL));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  LO_REQUIRE(started.ok());
  LO_CHECK(observatory.restored_from_snapshot());

  // A report printed by a later process must be consistent with the evidence it
  // shows: the totals that describe the evidence are restored with it.
  const RuntimeStatistics stats = observatory.statistics();
  LO_CHECK_EQ(stats.records_accepted, loaded.contents.total_accepted);
  LO_CHECK_EQ(stats.records_fenced, loaded.contents.total_fenced);
  LO_CHECK_EQ(stats.transitions_recorded,
              static_cast<std::uint64_t>(loaded.contents.transitions.size()));
  LO_CHECK(stats.records_accepted > 0U);

  // Counters that describe this process rather than the observed system stay at
  // zero: nothing has been written or cancelled in this incarnation yet.
  LO_CHECK_EQ(stats.snapshot_writes, static_cast<std::uint64_t>(0));
  LO_CHECK_EQ(stats.cancellations, static_cast<std::uint64_t>(0));

  const Status stopped = observatory.stop();
  (void)stopped;
}

LO_TEST(persist, a_missing_snapshot_is_not_an_error_but_is_reported) {
  TempPath path{"missing"};
  const SnapshotLoadResult loaded = read_snapshot(path.path(), make_default_policy());
  LO_CHECK(!loaded.ok);
  LO_CHECK_EQ(loaded.status.code(), StatusCode::NotFound);
}

LO_TEST(persist, restart_does_not_make_old_evidence_fresh) {
  TempPath path{"restart-freshness"};
  (void)run_generation(path.path(), "inc-a", 1U, kBaseInstant, true);

  // The second process starts a long time later. The evidence is the same
  // evidence, so it must not be usable.
  RuntimeConfig config = make_config(path.path());
  config.incarnation_name = "inc-b";
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant + 5000000000000LL));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  LO_CHECK_MSG(started.ok(), started.to_string() + " | " + observatory.load_detail());
  LO_REQUIRE(started.ok());
  LO_CHECK(observatory.restored_from_snapshot());

  const std::optional<Classification> classification = observatory.classify("link-a");
  LO_REQUIRE(classification.has_value());
  LO_CHECK_EQ(classification->state, LinkState::Stale);
  // An hour is beyond the usable window, so the evidence is expired rather than
  // merely stale: it may be read as history, never as a current value.
  LO_CHECK_EQ(classification->freshness, Freshness::Expired);
  LO_CHECK(classification->has_last_known);
  LO_CHECK_EQ(classification->last_known_state, LinkState::Down);
  const Status stopped = observatory.stop();
  (void)stopped;
}

LO_TEST(persist, a_rate_is_derived_after_a_restart_when_the_samples_are_still_usable) {
  TempPath path{"restart-derivation"};
  RuntimeConfig first_config = make_config(path.path());
  first_config.incarnation_name = "inc-a";
  {
    auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
    Observatory observatory(std::move(first_config), std::move(clock));
    const Status started = observatory.start();
    (void)started;
    (void)observatory.apply_source(make_source("agent-0"));
    (void)observatory.apply_link(make_link("link-a", 1000000000ULL));
    StreamPosition position{};
    (void)observatory.submit_observation(make_octets("link-a", "agent-0", position, 1000000U,
                                                     kBaseInstant, kBaseInstant));
    position.revision = 2U;
    (void)observatory.submit_observation(make_octets("link-a", "agent-0", position, 2000000U,
                                                     kBaseInstant + 1000000000LL,
                                                     kBaseInstant + 1000000000LL));
    const Status stopped = observatory.stop();
    (void)stopped;
  }

  RuntimeConfig second_config = make_config(path.path());
  second_config.incarnation_name = "inc-b";
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant + 1000000000LL));
  Observatory observatory(std::move(second_config), std::move(clock));
  const Status started = observatory.start();
  LO_REQUIRE(started.ok());

  // The two samples and their continuity travel with the evidence. The interval
  // is one second, both samples are fresh, the stream lost no revision and the
  // link declares a capacity, so the rate is exactly as valid after the restart
  // as before it. Recomputing it is not the same as making evidence fresh.
  const std::optional<Classification> classification = observatory.classify("link-a");
  LO_REQUIRE(classification.has_value());
  const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
  LO_REQUIRE(octets != nullptr);
  LO_CHECK_MSG(octets->has_derived, to_string(octets->derivation_fault));
  LO_CHECK(octets->has_derived);
  // 1000000 bytes over one second on a 1 Gbit/s link is 0.8 percent.
  LO_CHECK_EQ(octets->derived_ppb, static_cast<std::uint64_t>(8000000));
  const Status stopped = observatory.stop();
  (void)stopped;
}

LO_TEST(persist, a_rate_is_not_resurrected_by_a_restart_that_outlives_the_window) {
  TempPath path{"restart-derivation-stale"};
  RuntimeConfig first_config = make_config(path.path());
  first_config.incarnation_name = "inc-a";
  {
    auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
    Observatory observatory(std::move(first_config), std::move(clock));
    const Status started = observatory.start();
    (void)started;
    (void)observatory.apply_source(make_source("agent-0"));
    (void)observatory.apply_link(make_link("link-a", 1000000000ULL));
    StreamPosition position{};
    (void)observatory.submit_observation(make_octets("link-a", "agent-0", position, 1000000U,
                                                     kBaseInstant, kBaseInstant));
    position.revision = 2U;
    (void)observatory.submit_observation(make_octets("link-a", "agent-0", position, 2000000U,
                                                     kBaseInstant + 1000000000LL,
                                                     kBaseInstant + 1000000000LL));
    const Status stopped = observatory.stop();
    (void)stopped;
  }

  RuntimeConfig second_config = make_config(path.path());
  second_config.incarnation_name = "inc-b";
  // Ten minutes later the baseline is far outside the derivation window.
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant + 600000000000LL));
  Observatory observatory(std::move(second_config), std::move(clock));
  const Status started = observatory.start();
  LO_REQUIRE(started.ok());
  const std::optional<Classification> classification = observatory.classify("link-a");
  LO_REQUIRE(classification.has_value());
  const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
  LO_REQUIRE(octets != nullptr);
  LO_CHECK(!octets->has_derived);
  LO_CHECK_EQ(octets->derived_ppb, static_cast<std::uint64_t>(0));
  LO_CHECK_EQ(classification->freshness, Freshness::Expired);
  LO_CHECK(!classification->any_eligible);
  // The persisted stream never reported an operational state, so the runtime
  // admits it does not know rather than reporting a stale positive claim.
  LO_CHECK(!(classification->state == LinkState::Healthy));
  LO_CHECK(!(classification->state == LinkState::Saturated));
  const Status stopped = observatory.stop();
  (void)stopped;
}

LO_TEST(persist, revision_fencing_survives_a_restart) {
  TempPath path{"restart-fencing"};
  (void)run_generation(path.path(), "inc-a", 10U, kBaseInstant, false);

  RuntimeConfig config = make_config(path.path());
  config.incarnation_name = "inc-b";
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant + 1000000000LL));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  LO_REQUIRE(started.ok());

  // Revision 5 was already applied before the restart. Replaying it must be
  // refused even though the process is new.
  StreamPosition position{};
  position.revision = 5U;
  (void)observatory.submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                                 kBaseInstant + 1000000000LL,
                                                 kBaseInstant + 1000000000LL));
  LO_CHECK_EQ(observatory.fence_count(FenceReason::RevisionReplay),
              static_cast<std::uint64_t>(1));
  const Status stopped = observatory.stop();
  (void)stopped;
}

}  // namespace
