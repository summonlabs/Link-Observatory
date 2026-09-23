// End to end: declarations and observations in, classification, explanation,
// persistence and reload out.

#include <algorithm>
#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/classify/explain.hpp"
#include "linkobs/ingest/codec.hpp"
#include "linkobs/runtime/observatory.hpp"

namespace {

using namespace linkobs;
using namespace linkobs::test;

constexpr std::string_view kScenario =
    "# a two link fabric observed by one agent\n"
    "v=1 kind=source name=fabric-agent authority=primary provenance=real\n"
    "v=1 kind=link name=leaf1-eth0 link-kind=physical linkgen=gen-1 provenance=real "
    "capacity-in-bps=1000000000 capacity-out-bps=1000000000\n"
    "v=1 kind=link name=leaf1-eth1 link-kind=physical linkgen=gen-1 provenance=real "
    "capacity-in-bps=1000000000 capacity-out-bps=1000000000\n"
    "v=1 kind=observation link=leaf1-eth0 metric=oper-state source=fabric-agent incarnation=boot-1 "
    "epoch=cfg-1 generation=gen-1 revision=1 observed-at=1700000000000000000 "
    "received-at=1700000000000000000 value=up\n"
    "v=1 kind=observation link=leaf1-eth0 metric=admin-state source=fabric-agent "
    "incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=2 "
    "observed-at=1700000000000000000 received-at=1700000000000000000 value=enabled\n"
    "v=1 kind=observation link=leaf1-eth0 metric=octets/in source=fabric-agent "
    "incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=3 "
    "observed-at=1700000000000000000 received-at=1700000000000000000 counter=0 width=bits64\n"
    "v=1 kind=observation link=leaf1-eth0 metric=octets/in source=fabric-agent "
    "incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=4 "
    "observed-at=1700000001000000000 received-at=1700000001000000000 counter=125000000 "
    "width=bits64\n"
    "v=1 kind=observation link=leaf1-eth1 metric=oper-state source=fabric-agent incarnation=boot-1 "
    "epoch=cfg-1 generation=gen-1 revision=5 observed-at=1700000000000000000 "
    "received-at=1700000000000000000 value=down\n";

[[nodiscard]] std::unique_ptr<Observatory> ingest_scenario(const std::string& snapshot_path) {
  RuntimeConfig config = make_config(snapshot_path, 0U);
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant + 1000000000LL));
  auto observatory = std::make_unique<Observatory>(std::move(config), std::move(clock));
  const Status started = observatory->start();
  (void)started;
  ParseOutcome outcome = parse_batch(kScenario, observatory->config().policy);
  if (outcome.ok()) {
    const Status applied = observatory->submit_batch(std::move(outcome.batch));
    (void)applied;
  }
  return observatory;
}

LO_TEST(integration, the_scenario_ingests_without_rejections) {
  std::unique_ptr<Observatory> observatory = ingest_scenario({});
  const RuntimeStatistics stats = observatory->statistics();
  LO_CHECK_EQ(stats.records_accepted, static_cast<std::uint64_t>(5));
  LO_CHECK_EQ(stats.records_fenced, static_cast<std::uint64_t>(0));
  LO_CHECK_EQ(stats.declarations_applied, static_cast<std::uint64_t>(3));
  LO_CHECK_EQ(observatory->links().size(), static_cast<std::size_t>(2));
  const Status stopped = observatory->stop();
  (void)stopped;
}

LO_TEST(integration, both_links_classify_as_expected) {
  std::unique_ptr<Observatory> observatory = ingest_scenario({});
  const std::optional<Classification> up = observatory->classify("leaf1-eth0");
  LO_REQUIRE(up.has_value());
  // 125000000 bytes in one second on a 1 Gbit/s link is exactly 100%.
  LO_CHECK_EQ(up->state, LinkState::Saturated);

  const std::optional<Classification> down = observatory->classify("leaf1-eth1");
  LO_REQUIRE(down.has_value());
  LO_CHECK_EQ(down->state, LinkState::Down);

  const std::vector<Classification> all = observatory->classify_all();
  LO_CHECK_EQ(all.size(), static_cast<std::size_t>(2));
  const Status stopped = observatory->stop();
  (void)stopped;
}

LO_TEST(integration, explanations_are_stable_and_complete) {
  std::unique_ptr<Observatory> observatory = ingest_scenario({});
  const std::optional<Classification> classification = observatory->classify("leaf1-eth0");
  LO_REQUIRE(classification.has_value());

  ExplainOptions options{};
  const std::string first = explain(*classification, options);
  const std::string second = explain(*classification, options);
  LO_CHECK_EQ(first, second);

  LO_CHECK(first.find("link: ") != std::string::npos);
  LO_CHECK(first.find("state: saturated") != std::string::npos);
  LO_CHECK(first.find("metric octets/in:") != std::string::npos);
  LO_CHECK(first.find("rule 500 saturated-derived: fired=true") != std::string::npos);
  LO_CHECK(first.find("freshness: fresh") != std::string::npos);
  // The explanation never contains an address, a pointer or a locale decimal.
  LO_CHECK(first.find("0x") == std::string::npos);
  const Status stopped = observatory->stop();
  (void)stopped;
}

LO_TEST(integration, a_scenario_survives_a_full_restart_cycle) {
  TempPath path{"integration-cycle"};
  {
    std::unique_ptr<Observatory> observatory = ingest_scenario(path.path());
    const Status stopped = observatory->stop();
    LO_REQUIRE(stopped.ok());
  }
  {
    RuntimeConfig config = make_config(path.path(), 0U);
    config.incarnation_name = "second-process";
    auto clock = std::make_unique<ManualClock>(instant(kBaseInstant + 1000000000LL));
    Observatory observatory(std::move(config), std::move(clock));
    const Status started = observatory.start();
    LO_REQUIRE(started.ok());
    LO_CHECK(observatory.restored_from_snapshot());

    // The evidence is restored, so the history is intact...
    const std::optional<Classification> down = observatory.classify("leaf1-eth1");
    LO_REQUIRE(down.has_value());
    LO_CHECK(down->has_last_known);
    LO_CHECK_EQ(down->last_known_state, LinkState::Down);
    LO_CHECK_EQ(observatory.links().size(), static_cast<std::size_t>(2));

    // ...and so is the derived utilization, because the two samples, their
    // window, their scope and their stream continuity all travel with the
    // evidence. 125000000 bytes in one second on a 1 Gbit/s link is full scale.
    const std::optional<Classification> up = observatory.classify("leaf1-eth0");
    LO_REQUIRE(up.has_value());
    const FamilyReport* octets = up->find(MetricFamily::Octets, Direction::In);
    LO_REQUIRE(octets != nullptr);
    LO_CHECK(octets->has_derived);
    LO_CHECK_EQ(octets->derived_ppb, kPpbFullScale);
    LO_CHECK_EQ(up->state, LinkState::Saturated);

    const Status stopped = observatory.stop();
    (void)stopped;
  }
}

LO_TEST(integration, the_same_records_in_a_different_batch_split_give_the_same_result) {
  const auto run = [](std::size_t chunk) {
    RuntimeConfig config = make_config({}, 0U);
    auto clock = std::make_unique<ManualClock>(instant(kBaseInstant + 1000000000LL));
    Observatory observatory(std::move(config), std::move(clock));
    const Status started = observatory.start();
    (void)started;

    ParseOutcome outcome = parse_batch(kScenario, observatory.config().policy);
    Batch& batch = outcome.batch;
    for (std::size_t index = 0; index < batch.records.size(); index += chunk) {
      Batch slice{};
      const std::size_t end = std::min(index + chunk, batch.records.size());
      for (std::size_t cursor = index; cursor < end; ++cursor) {
        slice.records.push_back(batch.records[cursor]);
      }
      const Status applied = observatory.apply_batch(slice);
      (void)applied;
    }
    return summarize(*observatory.classify("leaf1-eth0"));
  };
  LO_CHECK_EQ(run(1U), run(2U));
  LO_CHECK_EQ(run(2U), run(64U));
}

LO_TEST(integration, unsupported_metric_families_stay_unsupported) {
  std::unique_ptr<Observatory> observatory = ingest_scenario({});
  const std::optional<Classification> classification = observatory->classify("leaf1-eth1");
  LO_REQUIRE(classification.has_value());
  // Nothing in the scenario supplies octets for the second link, so the family
  // is reported as Unsupported. It is not reported as zero traffic.
  const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
  LO_REQUIRE(octets != nullptr);
  LO_CHECK_EQ(octets->support, SupportState::Unsupported);
  LO_CHECK(!octets->has_value);
  LO_CHECK(!octets->has_delta);
  LO_CHECK_EQ(octets->freshness, Freshness::Unknown);
  LO_CHECK_EQ(octets->derived_ppb, static_cast<std::uint64_t>(0));
  const Status stopped = observatory->stop();
  (void)stopped;
}

}  // namespace
