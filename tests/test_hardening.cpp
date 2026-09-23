// Second pass adversarial and hardening tests.
//
// These cases were written after the first green run, deliberately looking for
// ways to make the runtime fabricate a value, lose a bound, or hang.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/core/text.hpp"
#include "linkobs/persist/snapshot.hpp"
#include "linkobs/runtime/observatory.hpp"
#include "linkobs/transport/client.hpp"
#include "linkobs/transport/server.hpp"

namespace {

using namespace linkobs;
using namespace linkobs::test;

class Rig {
 public:
  explicit Rig(std::string snapshot_path = {}, std::size_t workers = 0U, Nanos start = kBaseInstant) {
    auto clock = std::make_unique<ManualClock>(instant(start));
    clock_ = clock.get();
    observatory_ =
        std::make_unique<Observatory>(make_config(std::move(snapshot_path), workers), std::move(clock));
    const Status started = observatory_->start();
    (void)started;
  }

  [[nodiscard]] Observatory& observatory() { return *observatory_; }
  [[nodiscard]] ManualClock& clock() { return *clock_; }

  void declare(std::string_view link, std::uint64_t capacity = 1000000000ULL) {
    (void)observatory_->apply_source(make_source("agent-0"));
    (void)observatory_->apply_link(make_link(link, capacity, capacity));
  }

 private:
  ManualClock* clock_{nullptr};
  std::unique_ptr<Observatory> observatory_{};
};

LO_TEST(hardening, a_backwards_observation_never_produces_a_rate) {
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  (void)rig.observatory().submit_observation(
      make_octets("link-a", "agent-0", position, 1000000U, kBaseInstant, kBaseInstant));
  position.revision += 1U;
  // The counter advances but the observation time goes backwards: the interval is
  // not usable, so no rate may be reported.
  (void)rig.observatory().submit_observation(make_octets("link-a", "agent-0", position, 2000000U,
                                                         kBaseInstant - 1000000000LL,
                                                         kBaseInstant));
  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
  LO_REQUIRE(octets != nullptr);
  LO_CHECK(!octets->has_derived);
  LO_CHECK_EQ(octets->derived_ppb, static_cast<std::uint64_t>(0));
  LO_CHECK_EQ(octets->derivation_fault, DerivationFault::NonMonotonicTime);
}

LO_TEST(hardening, the_same_revision_with_different_evidence_is_refused) {
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                             kBaseInstant, kBaseInstant))
               .ok());
  // Same revision, different payload: the stream is ambiguous, so the record is
  // refused rather than replacing the accepted one.
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Down,
                                             kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK_EQ(rig.observatory().fence_count(FenceReason::RevisionReplay),
              static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(rig.observatory().statistics().records_accepted, static_cast<std::uint64_t>(1));
  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  LO_CHECK_EQ(classification->state, LinkState::Healthy);
}

LO_TEST(hardening, an_astronomically_large_delta_reports_overflow_instead_of_wrapping) {
  Rig rig{};
  rig.declare("link-a", 1U);
  StreamPosition position{};
  (void)rig.observatory().submit_observation(
      make_octets("link-a", "agent-0", position, 0U, kBaseInstant, kBaseInstant));
  position.revision += 1U;
  // A petabyte over one nanosecond on a one bit per second link cannot be
  // represented; the runtime must say so rather than wrap around.
  (void)rig.observatory().submit_observation(
      make_octets("link-a", "agent-0", position, 1000000000000000ULL, kBaseInstant + 1,
                  kBaseInstant + 1));
  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
  LO_REQUIRE(octets != nullptr);
  if (octets->has_derived) {
    // If the value is representable it must be enormous, never a small wrapped
    // number, and never negative (which is unrepresentable here).
    LO_CHECK(octets->derived_ppb > 1000000000ULL);
  } else {
    LO_CHECK_EQ(octets->derivation_fault, DerivationFault::Overflow);
    LO_CHECK_EQ(octets->derived_ppb, static_cast<std::uint64_t>(0));
  }
}

LO_TEST(hardening, a_snapshot_larger_than_the_configured_bound_is_refused) {
  TempPath path{"hardening-bound"};
  RuntimeConfig config = make_config(path.path(), 0U);
  config.policy.limits.max_snapshot_bytes = 64U;
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  (void)started;
  (void)observatory.apply_source(make_source("agent-0"));
  (void)observatory.apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL));
  (void)observatory.submit_observation(make_oper("link-a", "agent-0", StreamPosition{},
                                                 OperStateValue::Up, kBaseInstant, kBaseInstant));
  const SnapshotWriteResult written = observatory.persist();
  LO_CHECK(!written.status.ok());
  LO_CHECK_EQ(written.status.code(), StatusCode::CapacityExceeded);
  LO_CHECK(!snapshot_exists(path.path()));
}

LO_TEST(hardening, persisting_while_ingesting_produces_a_loadable_snapshot) {
  TempPath path{"hardening-concurrent-persist"};
  Rig rig{path.path(), 3U};
  rig.declare("link-a");
  for (std::size_t link = 0; link < 3U; ++link) {
    (void)rig.observatory().apply_link(
        make_link("link-" + to_dec(link), 1000000000ULL, 1000000000ULL));
  }

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> persists{0};
  std::thread writer([&rig, &stop, &persists]() {
    while (!stop.load(std::memory_order_relaxed)) {
      const SnapshotWriteResult result = rig.observatory().persist();
      if (result.status.ok()) {
        persists.fetch_add(1U, std::memory_order_relaxed);
      }
    }
  });

  // Handshake: the writer must have completed at least one persist before the
  // ingest load starts, so the test cannot pass because the thread never ran.
  for (int attempt = 0; attempt < 10000000 && persists.load() == 0U; ++attempt) {
    std::this_thread::yield();
  }
  LO_CHECK(persists.load() > 0U);

  for (std::size_t index = 0; index < 600U; ++index) {
    StreamPosition position{};
    position.revision = index + 1U;
    const std::string link = "link-" + to_dec(index % 3U);
    (void)rig.observatory().submit_observation(
        make_oper(link, "agent-0", position,
                  (index % 2 == 0) ? OperStateValue::Up : OperStateValue::Down,
                  kBaseInstant + static_cast<Nanos>(index) * 1000000LL,
                  kBaseInstant + static_cast<Nanos>(index) * 1000000LL));
  }

  stop.store(true, std::memory_order_relaxed);
  writer.join();
  const Status stopped = rig.observatory().stop();
  (void)stopped;

  // Whatever was captured must be a complete, verifiable snapshot.
  const SnapshotLoadResult loaded = read_snapshot(path.path(), make_default_policy());
  LO_CHECK_MSG(loaded.ok, loaded.status.to_string());
  LO_REQUIRE(loaded.ok);
  LO_CHECK(loaded.integrity_verified);
  LO_CHECK(persists.load() > 0U);
}

LO_TEST(hardening, repeated_restart_cycles_stay_loadable) {
  TempPath path{"hardening-restart-cycles"};
  for (std::uint64_t cycle = 1; cycle <= 6U; ++cycle) {
    RuntimeConfig config = make_config(path.path(), 0U);
    config.incarnation_name = std::string{"cycle-"} + to_dec(cycle);
    auto clock = std::make_unique<ManualClock>(instant(kBaseInstant + static_cast<Nanos>(cycle) *
                                                                                 1000000000LL));
    Observatory observatory(std::move(config), std::move(clock));
    const Status started = observatory.start();
    LO_CHECK_MSG(started.ok(), started.to_string());
    LO_REQUIRE(started.ok());
    (void)observatory.apply_source(make_source("agent-0"));
    (void)observatory.apply_link(make_link("link-a", 1000000000ULL, 1000000000ULL));
    StreamPosition position{};
    position.revision = cycle;
    (void)observatory.submit_observation(
        make_oper("link-a", "agent-0", position,
                  (cycle % 2U == 0U) ? OperStateValue::Up : OperStateValue::Down,
                  kBaseInstant + static_cast<Nanos>(cycle) * 1000000000LL,
                  kBaseInstant + static_cast<Nanos>(cycle) * 1000000000LL));
    const Status stopped = observatory.stop();
    LO_CHECK(stopped.ok());

    const SnapshotLoadResult loaded = read_snapshot(path.path(), make_default_policy());
    LO_CHECK_MSG(loaded.ok, loaded.status.to_string());
    LO_REQUIRE(loaded.ok);
  }
  // Six cycles must have produced exactly one primary and one backup.
  LO_CHECK(snapshot_exists(path.path()));
  LO_CHECK(snapshot_exists(snapshot_backup_path(path.path())));
}

LO_TEST(hardening, the_transport_bounds_a_stalled_peer_by_connection_count) {
  // The transport has no idle timeout by design: it never terminates work on a
  // deadline. The resource that bounds a stalled peer is therefore the
  // connection limit, and exceeding it must be refused rather than queued.
  Rig rig{};
  ServerOptions options{};
  options.host = "127.0.0.1";
  options.port = 0U;
  options.max_connections = 1U;
  options.persist_on_close = false;
  TransportServer server(rig.observatory(), options);
  LO_REQUIRE(server.listen().ok());
  std::thread server_thread([&server]() {
    const Status served = server.serve();
    (void)served;
  });

  // The first client holds the only connection slot for as long as it stays
  // connected. A second client reaches the listener but is closed immediately,
  // which is what the bound means.
  TransportClient first{};
  LO_REQUIRE(first.connect("127.0.0.1", server.bound_port()).ok());
  std::string response;
  LO_CHECK(first.ping(response).ok());

  TransportClient second{};
  const Status second_connected = second.connect("127.0.0.1", server.bound_port());
  LO_CHECK(second_connected.ok());
  // The server never reads from the refused socket, so no reply can arrive.
  LO_CHECK(!second.ping(response).ok());
  second.close();

  // The first client is still served, and a shutdown frame ends the server.
  LO_CHECK(first.ping(response).ok());
  LO_CHECK(first.shutdown(response).ok());
  first.close();
  server_thread.join();

  const ServerStatistics stats = server.statistics();
  LO_CHECK(stats.connections_accepted >= 1U);
  LO_CHECK_EQ(stats.shutdowns, static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(stats.protocol_errors, static_cast<std::uint64_t>(0));
}

LO_TEST(hardening, a_record_at_the_size_bound_is_accepted_and_one_over_is_not) {
  Policy policy = make_default_policy();
  policy.limits.max_record_bytes = 128U;

  // A record whose trimmed length is exactly at the bound is accepted; one byte
  // more is refused. The name is a token, so the split is unambiguous.
  const std::string prefix = "v=1 kind=source name=";
  const std::string suffix = " authority=primary provenance=real";
  LO_REQUIRE(prefix.size() + suffix.size() < 128U);
  const std::size_t name_length = 128U - prefix.size() - suffix.size();

  const std::string exactly = prefix + std::string(name_length, 'a') + suffix;
  const std::string over = prefix + std::string(name_length + 1U, 'a') + suffix;
  LO_CHECK_EQ(exactly.size(), static_cast<std::size_t>(128));

  const Result<ParsedRecord> accepted = parse_record(exactly, policy);
  LO_CHECK_MSG(accepted.ok(), accepted.status().to_string());
  LO_CHECK(accepted.ok());
  LO_CHECK_EQ(parse_record(over, policy).status().code(), StatusCode::OutOfRange);
}

LO_TEST(hardening, a_link_whose_capacity_is_unknown_never_reports_saturation) {
  Rig rig{};
  rig.declare("link-a", 0U);
  StreamPosition position{};
  (void)rig.observatory().submit_observation(make_oper("link-a", "agent-0", position,
                                                       OperStateValue::Up, kBaseInstant,
                                                       kBaseInstant));
  position.revision += 1U;
  (void)rig.observatory().submit_observation(
      make_octets("link-a", "agent-0", position, 0U, kBaseInstant, kBaseInstant));
  position.revision += 1U;
  (void)rig.observatory().submit_observation(make_octets("link-a", "agent-0", position,
                                                         1000000000000ULL,
                                                         kBaseInstant + 1000000LL,
                                                         kBaseInstant + 1000000LL));
  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  LO_CHECK(!(classification->state == LinkState::Saturated));
  const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
  LO_REQUIRE(octets != nullptr);
  LO_CHECK(!octets->has_derived);
  LO_CHECK_EQ(octets->derivation_fault, DerivationFault::MissingCapacity);
}

LO_TEST(hardening, a_source_that_reports_a_quality_reading_beyond_full_scale_is_refused) {
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  (void)rig.observatory().apply_source(make_source("agent-0"));
  const Status submitted = rig.observatory().submit_observation(
      make_quality("link-a", "agent-0", position, kPpbFullScale + 1U, false, kBaseInstant,
                   kBaseInstant));
  LO_CHECK(submitted.ok());
  LO_CHECK_EQ(rig.observatory().fence_count(FenceReason::InvalidValue),
              static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(rig.observatory().statistics().records_accepted, static_cast<std::uint64_t>(0));
}

}  // namespace
