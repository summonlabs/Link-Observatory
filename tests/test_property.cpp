// Property and seeded randomized tests.
//
// Each property is stated as an invariant that must hold for every generated
// stream, not as an expected output for one hand written input. The generator is
// seeded and fully deterministic, so a failure is reproducible from the seed
// printed in the failure message.

#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/classify/explain.hpp"
#include "linkobs/core/text.hpp"
#include "linkobs/runtime/observatory.hpp"

namespace {

using namespace linkobs;
using namespace linkobs::test;

class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : state_(seed) {}
  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  }
  [[nodiscard]] std::uint64_t bounded(std::uint64_t limit) noexcept {
    return limit == 0U ? 0U : next() % limit;
  }

 private:
  std::uint64_t state_;
};

/// One generated counter sample, retained so a property can recompute the
/// expected derivation independently of the runtime.
struct Sample {
  std::uint64_t revision{0};
  std::uint64_t counter{0};
  Nanos observed{0};
  bool reset{false};
  std::string epoch{};
  std::string generation{};
};

struct Generation {
  std::vector<ObservationDecl> records{};
  std::vector<Sample> samples{};
  std::uint64_t seed{0};
  std::uint64_t capacity_bps{0};
};

/// Generates a stream that mixes forward progress, declared and undeclared
/// resets and revision gaps, all inside one incarnation, epoch and generation,
/// so that the expected derivation is unambiguous. Scope and generation changes
/// are covered by their own tests.
///
/// Counters are 64-bit and strictly increasing except at an explicit reset, so
/// the expected derivation is unambiguous and can be recomputed independently.
[[nodiscard]] Generation generate(std::uint64_t seed, std::size_t count) {
  SplitMix64 random{seed};
  Generation generation{};
  generation.seed = seed;
  generation.capacity_bps = 1000000000ULL;

  StreamPosition position{};
  std::uint64_t counter = 0;
  Nanos observed = kBaseInstant;

  for (std::size_t index = 0; index < count; ++index) {
    const std::uint64_t shape = random.bounded(100U);
    position.revision += 1U;
    observed += 1000000 + static_cast<Nanos>(random.bounded(1000000U));

    bool reset = false;
    if (shape < 78U) {
      counter += 1000U + random.bounded(100000U);
    } else if (shape < 88U) {
      counter = random.bounded(1000U);
      reset = true;
    } else {
      position.revision += 3U;  // revision gap
    }

    Sample sample{};
    sample.revision = position.revision;
    sample.counter = counter;
    sample.observed = observed;
    sample.reset = reset;
    sample.epoch = position.epoch;
    sample.generation = position.generation;
    generation.samples.push_back(sample);

    generation.records.push_back(make_octets("link-0", "agent-0", position, counter, observed,
                                             observed, reset, CounterWidth::Bits64));
  }
  return generation;
}

[[nodiscard]] std::unique_ptr<Observatory> make_rig(std::size_t workers = 0U) {
  RuntimeConfig config = make_config({}, workers);
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  auto observatory = std::make_unique<Observatory>(std::move(config), std::move(clock));
  const Status started = observatory->start();
  (void)started;
  (void)observatory->apply_source(make_source("agent-0"));
  (void)observatory->apply_link(make_link("link-0", 1000000000ULL, 1000000000ULL));
  return observatory;
}

/// The utilization the runtime must report, recomputed here with floating point
/// arithmetic: an independent method reaching the same answer as the runtime's
/// exact integer arithmetic, within a two part per billion tolerance.
[[nodiscard]] bool expected_utilization(const Generation& generation,
                                        std::uint64_t& out_ppb,
                                        bool& expect_contiguous) {
  expect_contiguous = false;
  if (generation.samples.size() < 2U) {
    return false;
  }
  const Sample& previous = generation.samples[generation.samples.size() - 2U];
  const Sample& current = generation.samples.back();
  if (previous.generation != current.generation || previous.epoch != current.epoch) {
    return false;
  }
  if (current.revision != previous.revision + 1U) {
    return false;
  }
  if (current.reset) {
    return false;
  }
  if (current.counter < previous.counter) {
    return false;
  }
  const long double window_ns = static_cast<long double>(current.observed - previous.observed);
  if (window_ns <= 0.0L) {
    return false;
  }
  expect_contiguous = true;
  const long double delta = static_cast<long double>(current.counter - previous.counter);
  const long double ppb = (delta * 8.0L * 1.0e18L) /
                          (static_cast<long double>(generation.capacity_bps) * window_ns);
  out_ppb = static_cast<std::uint64_t>(ppb);
  return true;
}

void replay(Observatory& observatory, const Generation& generation) {
  for (const ObservationDecl& declaration : generation.records) {
    const Status submitted = observatory.submit_observation(declaration);
    if (!submitted.ok()) {
      LO_CHECK_MSG(false, "ingest refused a generated record");
      return;
    }
  }
}

LO_TEST(property, a_derived_utilization_matches_an_independent_recomputation) {
  for (std::uint64_t seed = 1; seed <= 24U; ++seed) {
    const Generation generation = generate(seed, 200U);
    std::unique_ptr<Observatory> observatory = make_rig();
    replay(*observatory, generation);

    const std::optional<Classification> classification = observatory->classify("link-0");
    LO_REQUIRE(classification.has_value());
    const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
    LO_REQUIRE(octets != nullptr);

    std::uint64_t expected = 0;
    bool contiguous = false;
    const bool derivable = expected_utilization(generation, expected, contiguous);

    if (derivable) {
      LO_CHECK(octets->has_delta);
      LO_CHECK(octets->has_derived);
      const std::uint64_t actual = octets->derived_ppb;
      const std::uint64_t difference = actual > expected ? actual - expected : expected - actual;
      LO_CHECK_MSG(difference <= 2U, std::string{"expected="} + to_dec(expected) +
                                         " actual=" + to_dec(actual));
    } else {
      // No utilization may be invented when the samples are not contiguous, the
      // scope changed, or a reset intervened.
      LO_CHECK_EQ(octets->derived_ppb, static_cast<std::uint64_t>(0));
      LO_CHECK(!octets->has_derived);
    }
    for (const FamilyReport& report : classification->families) {
      if (!report.has_derived) {
        LO_CHECK_EQ(report.derived_ppb, static_cast<std::uint64_t>(0));
      }
    }
    const Status stopped = observatory->stop();
    (void)stopped;
  }
}

LO_TEST(property, replaying_the_same_stream_twice_is_deterministic) {
  for (std::uint64_t seed = 1; seed <= 12U; ++seed) {
    const Generation generation = generate(seed, 120U);

    std::unique_ptr<Observatory> first = make_rig();
    replay(*first, generation);
    const std::string first_explain =
        first->classify("link-0").has_value()
            ? explain(*first->classify("link-0"), ExplainOptions{})
            : std::string{"absent"};
    const RuntimeStatistics first_stats = first->statistics();
    const Status first_stop = first->stop();
    (void)first_stop;

    std::unique_ptr<Observatory> second = make_rig();
    replay(*second, generation);
    const std::string second_explain =
        second->classify("link-0").has_value()
            ? explain(*second->classify("link-0"), ExplainOptions{})
            : std::string{"absent"};
    const RuntimeStatistics second_stats = second->statistics();
    const Status second_stop = second->stop();
    (void)second_stop;

    LO_CHECK_EQ(first_explain, second_explain);
    LO_CHECK_EQ(first_stats.records_accepted, second_stats.records_accepted);
    LO_CHECK_EQ(first_stats.records_fenced, second_stats.records_fenced);
    LO_CHECK_EQ(first_stats.state_changes, second_stats.state_changes);
  }
}

LO_TEST(property, a_fence_never_mutates_state) {
  for (std::uint64_t seed = 30; seed <= 42U; ++seed) {
    const Generation generation = generate(seed, 80U);
    std::unique_ptr<Observatory> observatory = make_rig();
    replay(*observatory, generation);
    const RuntimeStatistics before = observatory->statistics();

    // Replay every record verbatim. Every one of them is now a duplicate or an
    // out of order revision, so nothing may be accepted.
    for (const ObservationDecl& declaration : generation.records) {
      const Status submitted = observatory->submit_observation(declaration);
      (void)submitted;
    }
    const RuntimeStatistics after = observatory->statistics();
    LO_CHECK_EQ(after.records_accepted, before.records_accepted);
    LO_CHECK_EQ(after.state_changes, before.state_changes);
    LO_CHECK(after.records_fenced >= before.records_fenced + generation.records.size());

    const Status stopped = observatory->stop();
    (void)stopped;
  }
}

LO_TEST(property, freshness_degrades_monotonically_as_time_advances) {
  const Generation generation = generate(7U, 40U);
  RuntimeConfig config = make_config({}, 0U);
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  ManualClock* manual = clock.get();
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  (void)started;
  (void)observatory.apply_source(make_source("agent-0"));
  (void)observatory.apply_link(make_link("link-0", 1000000000ULL, 1000000000ULL));
  replay(observatory, generation);

  const auto rank = [](Freshness value) {
    switch (value) {
      case Freshness::Fresh:
        return 3;
      case Freshness::Stale:
        return 2;
      case Freshness::Expired:
        return 1;
      case Freshness::Unknown:
        return 0;
    }
    return 0;
  };

  const std::optional<Classification> start = observatory.classify("link-0");
  LO_REQUIRE(start.has_value());
  LO_CHECK_EQ(start->freshness, Freshness::Fresh);

  int previous_rank = rank(start->freshness);
  for (std::uint64_t step = 0; step < 20U; ++step) {
    manual->advance_by(Duration::from_seconds(60));
    const std::optional<Classification> classification = observatory.classify("link-0");
    LO_REQUIRE(classification.has_value());
    const int current_rank = rank(classification->freshness);
    LO_CHECK(current_rank <= previous_rank);
    previous_rank = current_rank;
  }
  // After twenty minutes of virtual time the evidence is expired. The state is
  // then an explicit admission of ignorance, never a stale positive claim.
  LO_CHECK_EQ(previous_rank, 1);
  const std::optional<Classification> expired = observatory.classify("link-0");
  LO_REQUIRE(expired.has_value());
  LO_CHECK_EQ(expired->freshness, Freshness::Expired);
  LO_CHECK(!expired->any_eligible);
  LO_CHECK(!(expired->state == LinkState::Healthy));
  LO_CHECK(!(expired->state == LinkState::Saturated));
  LO_CHECK(!(expired->state == LinkState::Erroring));
  // This stream never reported an operational state, so the runtime says so
  // explicitly rather than borrowing confidence from the traffic counters.
  LO_CHECK(expired->decisive_unsupported);
  LO_CHECK_EQ(expired->state, LinkState::Unknown);
  const Status stopped = observatory.stop();
  (void)stopped;
}

LO_TEST(property, bounded_buffers_never_grow_past_their_bound) {
  Policy policy = make_default_policy();
  policy.limits.max_evidence_per_key = 5U;
  policy.limits.max_transitions_per_link = 8U;

  RuntimeConfig config = make_config({}, 0U);
  config.policy = policy;
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  (void)started;
  (void)observatory.apply_source(make_source("agent-0"));
  (void)observatory.apply_link(make_link("link-0", 1000000000ULL, 1000000000ULL));

  StreamPosition position{};
  Nanos observed = kBaseInstant;
  for (int index = 0; index < 400; ++index) {
    position.revision += 1U;
    observed += 1000000;
    (void)observatory.submit_observation(
        make_oper("link-0", "agent-0", position,
                  (index % 2 == 0) ? OperStateValue::Up : OperStateValue::Down, observed, observed));
  }

  std::size_t retained = 0;
  for (const SlotView& view : observatory.slot_views(derive_link_id("link-0"))) {
    LO_CHECK(view.retained <= 5U);
    retained += view.retained;
  }
  LO_CHECK(retained > 0U);
  LO_CHECK(retained <= 5U);
  // Contenders are reported per source, newest first, and are bounded too.
  LO_CHECK(observatory.link_evidence(derive_link_id("link-0"), 1000U).size() <= 8U);
  LO_CHECK(observatory.transitions(derive_link_id("link-0"), 1000U).size() <= 8U);
  const Status stopped = observatory.stop();
  (void)stopped;
}

LO_TEST(property, unknown_is_never_reported_as_zero_utilization) {
  // A link with no traffic evidence at all must not report utilization 0.
  std::unique_ptr<Observatory> observatory = make_rig();
  StreamPosition position{};
  (void)observatory->submit_observation(
      make_oper("link-0", "agent-0", position, OperStateValue::Up, kBaseInstant, kBaseInstant));
  const std::optional<Classification> classification = observatory->classify("link-0");
  LO_REQUIRE(classification.has_value());
  // The traffic family is always reported, and with no evidence it is
  // Unsupported rather than a confident zero.
  const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
  LO_REQUIRE(octets != nullptr);
  LO_CHECK(!octets->has_value);
  LO_CHECK(!octets->has_delta);
  LO_CHECK(!octets->has_derived);
  LO_CHECK_EQ(octets->derived_ppb, static_cast<std::uint64_t>(0));
  LO_CHECK_EQ(octets->support, SupportState::Unsupported);
  LO_CHECK_EQ(octets->freshness, Freshness::Unknown);
  LO_CHECK_EQ(octets->completeness, Completeness::Unknown);
  const Status stopped = observatory->stop();
  (void)stopped;
}

}  // namespace
