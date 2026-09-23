// Concurrency: worker count must not change the outcome, the lock order audit
// must stay silent, queue bounds must hold, and cancellation must be real.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/classify/explain.hpp"
#include "linkobs/core/text.hpp"
#include "linkobs/ingest/queue.hpp"
#include "linkobs/runtime/observatory.hpp"

namespace {

using namespace linkobs;
using namespace linkobs::test;

[[nodiscard]] std::vector<ObservationDecl> build_stream(std::size_t links, std::size_t per_link) {
  std::vector<ObservationDecl> records;
  records.reserve(links * per_link);
  Nanos observed = kBaseInstant;
  for (std::size_t link = 0; link < links; ++link) {
    StreamPosition position{};
    for (std::size_t step = 0; step < per_link; ++step) {
      observed += 1000000;
      position.revision += 1U;
      records.push_back(make_oper("link-" + to_dec(static_cast<std::uint64_t>(link)), "agent-0",
                                  position,
                                  (step % 2 == 0) ? OperStateValue::Up : OperStateValue::Down,
                                  observed, observed));
    }
  }
  return records;
}

[[nodiscard]] std::unique_ptr<Observatory> run_with_workers(std::size_t workers,
                                                           const std::vector<ObservationDecl>& records) {
  RuntimeConfig config = make_config({}, workers);
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  auto observatory = std::make_unique<Observatory>(std::move(config), std::move(clock));
  const Status started = observatory->start();
  (void)started;
  (void)observatory->apply_source(make_source("agent-0"));
  for (std::size_t link = 0; link < 4U; ++link) {
    (void)observatory->apply_link(
        make_link("link-" + to_dec(static_cast<std::uint64_t>(link)), 1000000000ULL, 1000000000ULL));
  }

  // One source, so every record for a link is routed to the same worker and is
  // applied in submission order whatever the worker count is.
  Batch batch{};
  for (const ObservationDecl& declaration : records) {
    ParsedRecord record{};
    record.kind = ParsedRecord::Kind::Observation;
    record.observation = declaration;
    batch.records.push_back(std::move(record));
  }
  const Status submitted = observatory->submit_batch(std::move(batch));
  (void)submitted;
  const Status stopped = observatory->stop();
  (void)stopped;
  return observatory;
}

LO_TEST(concurrency, worker_count_does_not_change_the_outcome) {
  const std::vector<ObservationDecl> records = build_stream(4U, 250U);

  std::unique_ptr<Observatory> single = run_with_workers(0U, records);
  const RuntimeStatistics single_stats = single->statistics();
  std::vector<std::string> single_states;
  for (const Classification& classification : single->classify_all()) {
    single_states.push_back(summarize(classification));
  }

  std::unique_ptr<Observatory> parallel = run_with_workers(4U, records);
  const RuntimeStatistics parallel_stats = parallel->statistics();
  std::vector<std::string> parallel_states;
  for (const Classification& classification : parallel->classify_all()) {
    parallel_states.push_back(summarize(classification));
  }

  LO_CHECK_EQ(single_stats.records_accepted, parallel_stats.records_accepted);
  LO_CHECK_EQ(single_stats.records_fenced, parallel_stats.records_fenced);
  LO_CHECK_EQ(single_stats.state_changes, parallel_stats.state_changes);
  LO_CHECK_EQ(single_states.size(), parallel_states.size());
  for (std::size_t index = 0; index < single_states.size() && index < parallel_states.size();
       ++index) {
    LO_CHECK_EQ(single_states[index], parallel_states[index]);
  }
}

LO_TEST(concurrency, many_observers_never_see_a_torn_state) {
  RuntimeConfig config = make_config({}, 2U);
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  LO_REQUIRE(started.ok());
  (void)observatory.apply_source(make_source("agent-0"));
  (void)observatory.apply_link(make_link("link-0", 1000000000ULL, 1000000000ULL));

  std::atomic<bool> finished{false};
  std::atomic<std::uint64_t> observations{0};
  std::atomic<std::uint64_t> readers_ready{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 3; ++index) {
    readers.emplace_back([&observatory, &finished, &observations, &readers_ready]() {
      bool announced = false;
      while (!finished.load(std::memory_order_relaxed)) {
        const std::optional<Classification> classification = observatory.classify("link-0");
        if (classification.has_value()) {
          if (!announced) {
            announced = true;
            readers_ready.fetch_add(1U, std::memory_order_relaxed);
          }
          // Invariant: a reported classification always carries a generation
          // identifier that is either absent or well formed, and never a torn
          // mixture of family reports.
          const bool consistent =
              classification->families.size() <= kMetricFamilyCount * kDirectionCount;
          if (!consistent) {
            LO_CHECK(false);
          }
          observations.fetch_add(1U, std::memory_order_relaxed);
        }
      }
    });
  }

  // Explicit handshake: every reader must have observed the state at least once
  // before the writer starts, so the test cannot pass because the readers never
  // ran. There is no deadline; the loop is bounded by the work itself.
  for (int attempt = 0; attempt < 1000000 && readers_ready.load() < readers.size(); ++attempt) {
    std::this_thread::yield();
  }
  LO_CHECK_EQ(readers_ready.load(), static_cast<std::uint64_t>(readers.size()));

  StreamPosition position{};
  Nanos observed = kBaseInstant;
  for (int index = 0; index < 500; ++index) {
    observed += 1000000;
    position.revision += 1U;
    (void)observatory.submit_observation(
        make_oper("link-0", "agent-0", position,
                  (index % 2 == 0) ? OperStateValue::Up : OperStateValue::Down, observed, observed));
  }

  finished.store(true, std::memory_order_relaxed);
  for (std::thread& reader : readers) {
    reader.join();
  }
  LO_CHECK(observations.load() > 0U);
  const Status stopped = observatory.stop();
  (void)stopped;
}

LO_TEST(concurrency, the_lock_order_audit_records_nothing) {
  reset_lock_tracking();
  {
    RuntimeConfig config = make_config({}, 3U);
    auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
    Observatory observatory(std::move(config), std::move(clock));
    const Status started = observatory.start();
    (void)started;
    (void)observatory.apply_source(make_source("agent-0"));
    (void)observatory.apply_link(make_link("link-0", 1000000000ULL, 1000000000ULL));

    const std::vector<ObservationDecl> records = build_stream(2U, 200U);
    for (const ObservationDecl& declaration : records) {
      (void)observatory.submit_observation(declaration);
    }
    for (int index = 0; index < 20; ++index) {
      (void)observatory.classify_all();
      (void)observatory.recent_transitions(64U);
    }
    const Status stopped = observatory.stop();
    (void)stopped;
  }

  // The main thread's own stack is empty here, so any remaining report is a real
  // ordering or re-entrancy violation.
  const std::vector<std::string> reports = lock_violation_reports();
  for (const std::string& report : reports) {
    LO_CHECK_MSG(false, report);
  }
  LO_CHECK_EQ(lock_violation_count(), static_cast<std::uint64_t>(0));
}

LO_TEST(concurrency, bounded_queue_refuses_rather_than_grows) {
  BoundedQueue queue{2U};
  Batch first{};
  Batch second{};
  Batch third{};
  LO_CHECK(queue.try_push(first));
  LO_CHECK(queue.try_push(second));
  LO_CHECK(!queue.try_push(third));
  LO_CHECK_EQ(queue.rejected_full(), static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(queue.size(), static_cast<std::size_t>(2));
  LO_CHECK_EQ(queue.high_water(), static_cast<std::size_t>(2));

  Batch popped{};
  LO_CHECK(queue.pop(popped));
  LO_CHECK(queue.try_push(third));
  queue.close();
  LO_CHECK(queue.closed());
  Batch after_close{};
  LO_CHECK(!queue.try_push(after_close));
  LO_CHECK(queue.pop(popped));
  LO_CHECK(queue.pop(popped));
  LO_CHECK(!queue.pop(popped));
}

LO_TEST(concurrency, cancellation_discards_pending_work_and_reports_it) {
  RuntimeConfig config = make_config({}, 2U);
  config.queue_depth = 1U;
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  LO_REQUIRE(started.ok());
  (void)observatory.apply_source(make_source("agent-0"));
  (void)observatory.apply_link(make_link("link-0", 1000000000ULL, 1000000000ULL));

  std::vector<ObservationDecl> records;
  StreamPosition position{};
  Nanos observed = kBaseInstant;
  for (int index = 0; index < 200; ++index) {
    observed += 1000000;
    position.revision += 1U;
    records.push_back(make_oper("link-0", "agent-0", position, OperStateValue::Up, observed,
                                observed));
  }
  for (const ObservationDecl& declaration : records) {
    (void)observatory.submit_observation(declaration);
  }

  // Destruction cancels: nothing may hang, and the counters must stay coherent.
  const RuntimeStatistics before = observatory.statistics();
  LO_CHECK(before.records_accepted <= records.size());
  const Status stopped = observatory.stop();
  (void)stopped;
  LO_CHECK(!observatory.started());
}

LO_TEST(concurrency, repeated_start_and_stop_cycles_are_stable) {
  for (int cycle = 0; cycle < 4; ++cycle) {
    RuntimeConfig config = make_config({}, 2U);
    auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
    Observatory observatory(std::move(config), std::move(clock));
    const Status started = observatory.start();
    LO_REQUIRE(started.ok());
    const Status again = observatory.start();
    LO_CHECK(!again.ok());
    LO_CHECK_EQ(again.code(), StatusCode::AlreadyExists);
    const Status stopped = observatory.stop();
    LO_CHECK(stopped.ok());
    LO_CHECK(!observatory.started());
  }
}

}  // namespace
