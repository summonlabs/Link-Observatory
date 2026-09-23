// Domain types: identity, metric keys, readings, policy validation, evidence
// ageing and confidence derivation.

#include <limits>
#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/domain/evidence.hpp"
#include "linkobs/domain/link.hpp"
#include "linkobs/domain/observation.hpp"
#include "linkobs/domain/policy.hpp"

namespace {

using namespace linkobs;
using namespace linkobs::test;

LO_TEST(domain, identifiers_are_typed_and_round_trip) {
  const LinkId link = LinkId::from_name("leaf1/eth0");
  const std::string text = link.to_string();
  LO_CHECK(text.rfind("link-", 0) == 0U);
  const Result<LinkId> parsed = LinkId::parse(text);
  LO_REQUIRE(parsed.ok());
  LO_CHECK(parsed.value() == link);

  const Result<SourceId> wrong_kind = SourceId::parse(text);
  LO_CHECK(!wrong_kind.ok());
  LO_CHECK_EQ(wrong_kind.status().code(), StatusCode::InvalidArgument);

  // The kind is part of the hashed namespace, so equal names in different kinds
  // never produce equal identifiers.
  LO_CHECK(!(LinkId::from_name("x").hash() == GenerationId::from_name("x").hash()));
}

LO_TEST(domain, sequence_overflow_is_reported) {
  const Sequence last{std::numeric_limits<std::uint64_t>::max()};
  LO_CHECK(!last.next().ok);
  const Sequence first{0};
  LO_REQUIRE(first.next().ok);
  LO_CHECK_EQ(first.next().value.value(), static_cast<std::uint64_t>(1));
}

LO_TEST(domain, metric_key_spelling_is_canonical) {
  const MetricKey octets{MetricFamily::Octets, Direction::In, ErrorClass::None};
  LO_CHECK_EQ(octets.to_string(), std::string("octets/in"));
  const MetricKey errors{MetricFamily::Errors, Direction::Out, ErrorClass::Crc};
  LO_CHECK_EQ(errors.to_string(), std::string("errors/out/crc"));
  const MetricKey oper = MetricKey{MetricFamily::OperState, Direction::Unknown, ErrorClass::None};
  LO_CHECK_EQ(oper.to_string(), std::string("oper-state"));

  for (const MetricKey& key : {octets, errors, oper}) {
    const Result<MetricKey> parsed = MetricKey::parse(key.to_string());
    LO_REQUIRE(parsed.ok());
    LO_CHECK(parsed.value() == key);
  }
  LO_CHECK(!MetricKey::parse("not-a-family").ok());
  LO_CHECK(!MetricKey::parse("octets/sideways").ok());
  LO_CHECK(!MetricKey::parse("octets/in/extra/crc").ok());
}

LO_TEST(domain, reading_validation_rejects_rather_than_clamps) {
  const Policy policy = make_default_policy();

  LO_CHECK(validate_reading(octets_key(), CounterReading{1U, CounterWidth::Bits64, false}, policy)
               .ok());

  // A 32-bit counter cannot carry a 64-bit sample.
  CounterReading wide{};
  wide.raw = 0x1FFFFFFFFULL;
  wide.width = CounterWidth::Bits32;
  const Status rejected = validate_reading(
      MetricKey{MetricFamily::Octets, Direction::In, ErrorClass::None}, wide, policy);
  LO_CHECK(!rejected.ok());
  LO_CHECK_EQ(rejected.code(), StatusCode::OutOfRange);

  // A quality indicator cannot exceed full scale.
  QualityReading quality{};
  quality.value_ppb = kPpbFullScale + 1U;
  LO_CHECK(!validate_reading(MetricKey{MetricFamily::SignalQuality, Direction::In, ErrorClass::None},
                             quality, policy)
                .ok());

  // A ratio above the accepted maximum is refused.
  RatioReading ratio{};
  ratio.ppb = kPpbAcceptedMax + 1U;
  LO_CHECK(!validate_reading(
               MetricKey{MetricFamily::UtilReported, Direction::In, ErrorClass::None}, ratio, policy)
                .ok());

  // A reading whose alternative does not belong to the family is unsupported.
  LO_CHECK_EQ(validate_reading(MetricKey{MetricFamily::Octets, Direction::In, ErrorClass::None},
                               OperStateReading{OperStateValue::Up}, policy)
                  .code(),
              StatusCode::Unsupported);

  // A zero flap window is not a window.
  FlapReading zero_window{};
  zero_window.events = 1U;
  zero_window.window = Duration::from_nanos(0);
  LO_CHECK(!validate_reading(
               MetricKey{MetricFamily::FlapReport, Direction::Unknown, ErrorClass::None},
               zero_window, policy)
                .ok());
}

LO_TEST(domain, default_policy_is_valid_and_bounds_are_checked) {
  const Policy policy = make_default_policy();
  LO_CHECK(validate(policy).ok());

  Policy broken = policy;
  broken.thresholds.flap_threshold = 0U;
  LO_CHECK(!validate(broken).ok());

  broken = policy;
  broken.freshness[0].usable_within = broken.freshness[0].fresh_within;
  LO_CHECK(!validate(broken).ok());

  broken = policy;
  broken.limits.max_links = 0U;
  LO_CHECK(!validate(broken).ok());

  broken = policy;
  broken.thresholds.saturation_ppb = 0U;
  LO_CHECK(!validate(broken).ok());
}

LO_TEST(domain, freshness_is_dominated_by_received_age) {
  Policy policy = make_default_policy();
  const FreshnessPolicy& window = freshness_policy_for(policy, MetricFamily::Octets);

  Evidence evidence{};
  evidence.observation.observed_at = TimePoint::from_nanos(1000);
  evidence.observation.received_at = TimePoint::from_nanos(1000);

  AgeReport age = age_evidence(evidence, TimePoint::from_nanos(1000));
  LO_CHECK(age.observed_age.is_zero());
  LO_CHECK_EQ(classify_freshness(age, window), Freshness::Fresh);

  // Old by observation time but just received: still not current, because the
  // runtime cannot know what happened in between.
  evidence.observation.observed_at = TimePoint::from_nanos(1000);
  evidence.observation.received_at = TimePoint::from_nanos(1000 + 120000000000LL);
  age = age_evidence(evidence, TimePoint::from_nanos(1000 + 120000000000LL));
  LO_CHECK(age.observed_age.nanos() > 0);
  LO_CHECK_EQ(classify_freshness(age, window), Freshness::Stale);

  // Beyond the usable window is expired, which is never usable.
  age = age_evidence(evidence, TimePoint::from_nanos(1000 + 400000000000LL));
  LO_CHECK_EQ(classify_freshness(age, window), Freshness::Expired);
}

LO_TEST(domain, a_future_timestamp_is_not_extra_fresh) {
  Evidence evidence{};
  evidence.observation.observed_at = TimePoint::from_nanos(5000);
  evidence.observation.received_at = TimePoint::from_nanos(5000);
  const AgeReport age = age_evidence(evidence, TimePoint::from_nanos(1000));
  LO_CHECK(age.backwards_clock);
  LO_CHECK(age.observed_age.is_zero());
  const FreshnessPolicy window = freshness_policy_for(make_default_policy(), MetricFamily::Octets);
  LO_CHECK_EQ(classify_freshness(age, window), Freshness::Fresh);
}

LO_TEST(domain, confidence_follows_authority_freshness_and_conflict) {
  LO_CHECK_EQ(derive_confidence(SourceAuthority::Primary, Freshness::Fresh, ConflictState::None,
                                Completeness::Complete),
              Confidence::High);
  LO_CHECK_EQ(derive_confidence(SourceAuthority::Primary, Freshness::Fresh,
                                ConflictState::Conflicting, Completeness::Complete),
              Confidence::Low);
  LO_CHECK_EQ(derive_confidence(SourceAuthority::Primary, Freshness::Stale, ConflictState::None,
                                Completeness::Complete),
              Confidence::Low);
  LO_CHECK_EQ(derive_confidence(SourceAuthority::Primary, Freshness::Expired, ConflictState::None,
                                Completeness::Complete),
              Confidence::Unknown);
  LO_CHECK_EQ(derive_confidence(SourceAuthority::Secondary, Freshness::Fresh, ConflictState::None,
                                Completeness::Complete),
              Confidence::Medium);
  LO_CHECK_EQ(derive_confidence(SourceAuthority::Primary, Freshness::Fresh, ConflictState::None,
                                Completeness::Incomplete),
              Confidence::Medium);
  LO_CHECK_EQ(derive_confidence(SourceAuthority::Unknown, Freshness::Fresh, ConflictState::None,
                                Completeness::Complete),
              Confidence::Unknown);
}

LO_TEST(domain, observation_identity_covers_every_field) {
  Observation observation{};
  observation.provenance.source = SourceId::from_name("s");
  observation.provenance.incarnation = IncarnationId::from_name("i");
  observation.provenance.epoch = EpochId::from_name("e");
  observation.provenance.generation = GenerationId::from_name("g");
  observation.provenance.sequence = Sequence{1};
  observation.link = LinkId::from_name("l");
  observation.key = MetricKey{MetricFamily::Octets, Direction::In, ErrorClass::None};
  observation.payload = CounterReading{10U, CounterWidth::Bits64, false};
  observation.observed_at = TimePoint::from_nanos(1);
  observation.received_at = TimePoint::from_nanos(2);
  const Observation first = with_identity(observation);
  LO_CHECK(first.id.valid());
  LO_CHECK(with_identity(observation).id == first.id);

  Observation changed = observation;
  changed.provenance.sequence = Sequence{2};
  LO_CHECK(!(with_identity(changed).id == first.id));

  changed = observation;
  changed.observed_at = TimePoint::from_nanos(3);
  LO_CHECK(!(with_identity(changed).id == first.id));

  changed = observation;
  changed.provenance.generation = GenerationId::from_name("g2");
  LO_CHECK(!(with_identity(changed).id == first.id));

  changed = observation;
  changed.payload = CounterReading{11U, CounterWidth::Bits64, false};
  LO_CHECK(!(with_identity(changed).id == first.id));
}

LO_TEST(domain, link_facts_reject_contradictory_capacity) {
  const Policy policy = make_default_policy();
  LinkFacts facts{};
  facts.id = derive_link_id("l");
  facts.name = "l";
  LO_CHECK(validate_link_facts(facts, policy).ok());

  facts.capacity_known = true;
  LO_CHECK(!validate_link_facts(facts, policy).ok());

  facts.capacity_in_bps = 1U;
  LO_CHECK(validate_link_facts(facts, policy).ok());

  facts.capacity_known = false;
  LO_CHECK(!validate_link_facts(facts, policy).ok());

  facts.capacity_in_bps = 0U;
  LO_CHECK(validate_link_facts(facts, policy).ok());
  LO_CHECK(!capacity_for(facts, Direction::In, facts.capacity_in_bps));
}

}  // namespace
