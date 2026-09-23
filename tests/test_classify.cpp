// Classification: every state is reachable, precedence is fixed, and absence of
// evidence is never converted into a positive claim.

#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/classify/explain.hpp"
#include "linkobs/runtime/observatory.hpp"

namespace {

using namespace linkobs;
using namespace linkobs::test;

struct Scenario {
  Observatory observatory;
  ManualClock* clock{nullptr};

  explicit Scenario(std::size_t workers = 0U)
      : observatory(make_config({}, workers), std::make_unique<ManualClock>(instant(kBaseInstant))) {
    clock = static_cast<ManualClock*>(nullptr);
  }
};

/// Builds an observatory with a manual clock the test can steer.
class Rig {
 public:
  explicit Rig(std::size_t workers = 0U) {
    auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
    clock_ = clock.get();
    observatory_ = std::make_unique<Observatory>(make_config({}, workers), std::move(clock));
    const Status started = observatory_->start();
    (void)started;
  }

  [[nodiscard]] Observatory& observatory() { return *observatory_; }
  [[nodiscard]] ManualClock& clock() { return *clock_; }

  void advance(Duration delta) { clock_->advance_by(delta); }

  void declare(std::string_view link, std::uint64_t capacity = 100000000000ULL) {
    const Status source = observatory_->apply_source(make_source("agent-0"));
    (void)source;
    const Status declared = observatory_->apply_link(make_link(link, capacity, capacity));
    (void)declared;
  }

  [[nodiscard]] LinkState state_of(std::string_view link) {
    const std::optional<Classification> classification = observatory_->classify(link);
    return classification.has_value() ? classification->state : LinkState::Unknown;
  }

 private:
  ManualClock* clock_{nullptr};
  std::unique_ptr<Observatory> observatory_{};
};

LO_TEST(classify, a_link_without_evidence_is_unknown_not_healthy) {
  Rig rig{};
  rig.declare("link-a");
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Unknown);
  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  LO_CHECK(!classification->any_evidence);
  LO_CHECK_EQ(classification->freshness, Freshness::Unknown);
}

LO_TEST(classify, evidence_for_an_unregistered_link_is_fenced) {
  Rig rig{};
  (void)rig.observatory().apply_source(make_source("agent-0"));
  const Status submitted = rig.observatory().submit_observation(
      make_oper("ghost", "agent-0", StreamPosition{}, OperStateValue::Up, kBaseInstant,
                kBaseInstant));
  LO_CHECK(submitted.ok());
  LO_CHECK_EQ(rig.observatory().fence_count(FenceReason::UnknownLink),
              static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(rig.observatory().links().size(), static_cast<std::size_t>(0));
}

LO_TEST(classify, healthy_requires_fresh_up_evidence) {
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                             kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Healthy);
}

LO_TEST(classify, absence_of_operational_evidence_is_not_healthy) {
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  // Octets alone do not say the link is up.
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 1000U, kBaseInstant,
                                               kBaseInstant))
               .ok());
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Unknown);
  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  LO_CHECK(classification->decisive_unsupported);
}

LO_TEST(classify, down_and_disabled_map_to_down) {
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Down,
                                             kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Down);

  Rig admin_rig{};
  admin_rig.declare("link-b");
  LO_CHECK(admin_rig.observatory()
               .submit_observation(make_admin("link-b", "agent-0", StreamPosition{},
                                              AdminStateValue::Disabled, kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK_EQ(admin_rig.state_of("link-b"), LinkState::Down);
}

LO_TEST(classify, stale_evidence_is_stale_not_down) {
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Down,
                                             kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Down);

  // Well beyond the usable window: the runtime no longer knows, and says so.
  rig.advance(Duration::from_seconds(1000));
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Stale);
  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  LO_CHECK(classification->has_last_known);
  LO_CHECK_EQ(classification->last_known_state, LinkState::Down);
}

LO_TEST(classify, saturation_comes_from_a_valid_derivation_only) {
  Rig rig{};
  rig.declare("link-a", 1000000000ULL);  // 1 Gbit/s
  StreamPosition position{};

  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                             kBaseInstant, kBaseInstant))
               .ok());
  position.revision += 1U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 0U, kBaseInstant,
                                               kBaseInstant))
               .ok());
  position.revision += 1U;
  // 1 Gbit/s for one second is 125000000 bytes: exactly 100%.
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 125000000ULL,
                                               kBaseInstant + 1000000000LL,
                                               kBaseInstant + 1000000000LL))
               .ok());
  rig.advance(Duration::from_nanos(1000000000LL));

  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  LO_CHECK_EQ(classification->state, LinkState::Saturated);
  const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
  LO_REQUIRE(octets != nullptr);
  LO_CHECK(octets->has_derived);
  LO_CHECK_EQ(octets->derived_ppb, kPpbFullScale);
}

LO_TEST(classify, a_missing_capacity_prevents_a_saturation_claim) {
  Rig rig{};
  rig.declare("link-a", 0U);
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 0U, kBaseInstant,
                                               kBaseInstant))
               .ok());
  position.revision += 1U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 125000000ULL,
                                               kBaseInstant + 1000000000LL,
                                               kBaseInstant + 1000000000LL))
               .ok());
  rig.advance(Duration::from_nanos(1000000000LL));
  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  LO_CHECK(!(classification->state == LinkState::Saturated));
  const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
  LO_REQUIRE(octets != nullptr);
  LO_CHECK(!octets->has_derived);
  LO_CHECK_EQ(octets->derivation_fault, DerivationFault::MissingCapacity);
}

LO_TEST(classify, a_counter_reset_does_not_fabricate_traffic) {
  Rig rig{};
  rig.declare("link-a", 1000000000ULL);
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 1000000000ULL,
                                               kBaseInstant, kBaseInstant))
               .ok());
  position.revision += 1U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 5U,
                                               kBaseInstant + 1000000000LL,
                                               kBaseInstant + 1000000000LL))
               .ok());
  rig.advance(Duration::from_nanos(1000000000LL));

  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  LO_CHECK(!(classification->state == LinkState::Saturated));
  const FamilyReport* octets = classification->find(MetricFamily::Octets, Direction::In);
  LO_REQUIRE(octets != nullptr);
  LO_CHECK(!octets->has_derived);
  LO_CHECK_EQ(octets->derivation_fault, DerivationFault::CounterReset);
  LO_CHECK_EQ(octets->derived_ppb, static_cast<std::uint64_t>(0));
}

LO_TEST(classify, conflicting_authorities_are_reported_as_conflicting) {
  Rig rig{};
  rig.declare("link-a");
  (void)rig.observatory().apply_source(make_source("agent-1", SourceAuthority::Primary));
  (void)rig.observatory().apply_source(make_source("agent-2", SourceAuthority::Primary));

  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-1", StreamPosition{},
                                             OperStateValue::Up, kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-2", StreamPosition{},
                                             OperStateValue::Down, kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Conflicting);
}

LO_TEST(classify, a_lower_authority_disagreement_is_not_a_conflict) {
  Rig rig{};
  rig.declare("link-a");
  (void)rig.observatory().apply_source(make_source("agent-1", SourceAuthority::Primary));
  (void)rig.observatory().apply_source(make_source("agent-2", SourceAuthority::Secondary));

  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-1", StreamPosition{},
                                             OperStateValue::Up, kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-2", StreamPosition{},
                                             OperStateValue::Down, kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Healthy);
}

LO_TEST(classify, corroborating_sources_are_recorded_as_corroborated) {
  Rig rig{};
  rig.declare("link-a");
  (void)rig.observatory().apply_source(make_source("agent-1", SourceAuthority::Primary));
  (void)rig.observatory().apply_source(make_source("agent-2", SourceAuthority::Primary));
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-1", StreamPosition{},
                                             OperStateValue::Up, kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-2", StreamPosition{},
                                             OperStateValue::Up, kBaseInstant, kBaseInstant))
               .ok());
  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  const FamilyReport* oper = classification->find(MetricFamily::OperState, Direction::Unknown);
  LO_REQUIRE(oper != nullptr);
  LO_CHECK_EQ(oper->conflict, ConflictState::Corroborated);
  LO_CHECK_EQ(classification->state, LinkState::Healthy);
}

LO_TEST(classify, quality_degradation_and_error_ratio_degrade_a_link) {
  Rig rig{};
  rig.declare("link-a", 1000000000ULL);
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                             kBaseInstant, kBaseInstant))
               .ok());
  position.revision += 1U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_quality("link-a", "agent-0", position, 100000000ULL, false,
                                                kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Degraded);
}

LO_TEST(classify, an_error_ratio_above_threshold_is_erroring) {
  Rig rig{};
  rig.declare("link-a", 1000000000ULL);
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                             kBaseInstant, kBaseInstant))
               .ok());
  position.revision += 1U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 0U, kBaseInstant,
                                               kBaseInstant))
               .ok());
  position.revision += 1U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_errors("link-a", "agent-0", position, 0U, kBaseInstant,
                                               kBaseInstant))
               .ok());
  position.revision += 1U;
  const Nanos later = kBaseInstant + 1000000000LL;
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 100000000ULL, later,
                                               later))
               .ok());
  position.revision += 1U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_errors("link-a", "agent-0", position, 100000U, later, later))
               .ok());
  rig.advance(Duration::from_nanos(1000000000LL));
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Erroring);
}

LO_TEST(classify, flapping_is_detected_from_observed_transitions) {
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  // Five observations alternating up and down: four observed transitions within
  // the flap window, and a final state of up.
  for (int index = 0; index < 5; ++index) {
    const OperStateValue value = (index % 2 == 0) ? OperStateValue::Up : OperStateValue::Down;
    const Nanos at = kBaseInstant + (static_cast<Nanos>(index) * 1000000000LL);
    LO_CHECK(rig.observatory()
                 .submit_observation(make_oper("link-a", "agent-0", position, value, at, at))
                 .ok());
    position.revision += 1U;
  }
  rig.advance(Duration::from_seconds(5));
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Flapping);
}

LO_TEST(classify, a_currently_down_link_is_reported_down_even_while_it_flaps) {
  // Precedence is fixed and documented: a link that is down right now is
  // reported down. Flapping describes the history that produced that state.
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  for (int index = 0; index < 6; ++index) {
    const OperStateValue value = (index % 2 == 0) ? OperStateValue::Up : OperStateValue::Down;
    const Nanos at = kBaseInstant + (static_cast<Nanos>(index) * 1000000000LL);
    (void)rig.observatory().submit_observation(
        make_oper("link-a", "agent-0", position, value, at, at));
    position.revision += 1U;
  }
  rig.advance(Duration::from_seconds(5));
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Down);
  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  const RuleEvaluation* flap_rule = nullptr;
  for (const RuleEvaluation& rule : classification->rules) {
    if (rule.rule == RuleId::Flapping) {
      flap_rule = &rule;
    }
  }
  LO_REQUIRE(flap_rule != nullptr);
  LO_CHECK(flap_rule->fired);
  LO_CHECK(flap_rule->detail.find("observed transitions=") != std::string::npos);
}

LO_TEST(classify, a_reported_flap_count_is_used_when_the_source_supplies_it) {
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                             kBaseInstant, kBaseInstant))
               .ok());
  position.revision += 1U;
  FlapReading reading{};
  reading.events = 9U;
  reading.window = Duration::from_seconds(60);
  LO_CHECK(rig.observatory()
               .submit_observation(make_observation(
                   "link-a", "agent-0",
                   MetricKey{MetricFamily::FlapReport, Direction::Unknown, ErrorClass::None}, reading,
                   position, kBaseInstant, kBaseInstant))
               .ok());
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Flapping);
}

LO_TEST(classify, incomplete_decisive_evidence_is_unknown_with_a_reason) {
  Rig rig{};
  rig.declare("link-a");
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Up,
                                             kBaseInstant, kBaseInstant))
               .ok());
  position.revision += 1U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 1000U, kBaseInstant,
                                               kBaseInstant))
               .ok());
  // A revision gap is a hole in the stream, not a link fault.
  position.revision += 5U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 2000U,
                                               kBaseInstant + 1000000000LL,
                                               kBaseInstant + 1000000000LL))
               .ok());
  rig.advance(Duration::from_nanos(1000000000LL));

  const std::optional<Classification> classification = rig.observatory().classify("link-a");
  LO_REQUIRE(classification.has_value());
  LO_CHECK_EQ(classification->state, LinkState::Unknown);
  LO_CHECK(classification->decisive_incomplete);
  LO_CHECK_EQ(classification->completeness, Completeness::Incomplete);
}

LO_TEST(classify, down_takes_precedence_over_saturation) {
  Rig rig{};
  rig.declare("link-a", 1000000000ULL);
  StreamPosition position{};
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 0U, kBaseInstant,
                                               kBaseInstant))
               .ok());
  position.revision += 1U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_octets("link-a", "agent-0", position, 125000000ULL,
                                               kBaseInstant + 1000000000LL,
                                               kBaseInstant + 1000000000LL))
               .ok());
  position.revision += 1U;
  LO_CHECK(rig.observatory()
               .submit_observation(make_oper("link-a", "agent-0", position, OperStateValue::Down,
                                             kBaseInstant + 1000000000LL,
                                             kBaseInstant + 1000000000LL))
               .ok());
  rig.advance(Duration::from_nanos(1000000000LL));
  LO_CHECK_EQ(rig.state_of("link-a"), LinkState::Down);
}

LO_TEST(classify, explanation_is_byte_identical_across_runs) {
  const auto build = []() {
    Rig rig{};
    rig.declare("link-a", 1000000000ULL);
    StreamPosition position{};
    (void)rig.observatory().submit_observation(make_oper("link-a", "agent-0", position,
                                                         OperStateValue::Up, kBaseInstant,
                                                         kBaseInstant));
    position.revision += 1U;
    (void)rig.observatory().submit_observation(make_octets("link-a", "agent-0", position, 0U,
                                                           kBaseInstant, kBaseInstant));
    position.revision += 1U;
    (void)rig.observatory().submit_observation(
        make_octets("link-a", "agent-0", position, 125000000ULL, kBaseInstant + 1000000000LL,
                    kBaseInstant + 1000000000LL));
    rig.advance(Duration::from_nanos(1000000000LL));
    return explain(*rig.observatory().classify("link-a"), ExplainOptions{});
  };
  const std::string first = build();
  const std::string second = build();
  LO_CHECK_EQ(first, second);
  LO_CHECK(first.find("state: saturated") != std::string::npos);
}

}  // namespace
