// Counter continuity, utilization derivation, evidence store fencing and bounds.

#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/store/counter_tracker.hpp"
#include "linkobs/store/evidence_store.hpp"
#include "linkobs/store/history.hpp"

namespace {

using namespace linkobs;
using namespace linkobs::test;

[[nodiscard]] CounterSample sample(std::uint64_t raw,
                                   Sequence sequence,
                                   Nanos observed,
                                   CounterWidth width = CounterWidth::Bits64,
                                   bool reset = false) {
  CounterSample value{};
  value.reading.raw = raw;
  value.reading.width = width;
  value.reading.reset_declared = reset;
  value.observed_at = TimePoint::from_nanos(observed);
  value.received_at = TimePoint::from_nanos(observed);
  value.sequence = sequence;
  value.scope = SequenceScope{SourceId::from_name("s"), IncarnationId::from_name("i"),
                              EpochId::from_name("e")};
  value.generation = GenerationId::from_name("g");
  return value;
}

LO_TEST(store, forward_delta_is_exact) {
  CounterContinuity continuity{};
  const ContinuityOutcome first = observe_counter(continuity, sample(1000U, Sequence{1}, 10));
  LO_CHECK(first.first_sample);
  LO_CHECK(!first.delta_valid);


  const ContinuityOutcome second = observe_counter(continuity, sample(1600U, Sequence{2}, 20));
  LO_CHECK(second.delta_valid);
  LO_CHECK_EQ(second.delta, static_cast<std::uint64_t>(600));
  LO_CHECK_EQ(second.discontinuity, CounterDiscontinuity::None);
}

LO_TEST(store, counter_reset_never_produces_a_delta) {
  CounterContinuity continuity{};
  (void)observe_counter(continuity, sample(900000U, Sequence{1}, 10));
  const ContinuityOutcome reset = observe_counter(continuity, sample(5U, Sequence{2}, 20));
  LO_CHECK_EQ(reset.discontinuity, CounterDiscontinuity::Reset);
  LO_CHECK(!reset.delta_valid);
  LO_CHECK_EQ(reset.delta, static_cast<std::uint64_t>(0));
  LO_CHECK_EQ(continuity.reset_count, static_cast<std::uint64_t>(1));

  DerivationRequest request{};
  request.previous = continuity.previous;
  request.current = continuity.latest;
  request.has_previous = true;
  request.capacity_bps = 1000000000ULL;
  request.max_window = Duration::from_seconds(60);
  const DerivationResult derived = derive_utilization(request);
  LO_CHECK(!derived.valid);
  LO_CHECK_EQ(derived.fault, DerivationFault::CounterReset);
  LO_CHECK_EQ(derived.utilization_ppb, static_cast<std::uint64_t>(0));
}

LO_TEST(store, declared_reset_is_honoured) {
  CounterContinuity continuity{};
  (void)observe_counter(continuity, sample(10U, Sequence{1}, 10));
  const ContinuityOutcome outcome = observe_counter(continuity, sample(5000U, Sequence{2}, 20,
                                                                      CounterWidth::Bits64, true));
  LO_CHECK_EQ(outcome.discontinuity, CounterDiscontinuity::Reset);
  LO_CHECK(!outcome.delta_valid);
  LO_CHECK_EQ(outcome.delta, static_cast<std::uint64_t>(0));
}

LO_TEST(store, a_wrap_near_the_top_of_a_32_bit_counter_is_recovered) {
  CounterContinuity continuity{};
  const std::uint64_t near_top = 0xFFFFFF00ULL;
  (void)observe_counter(continuity, sample(near_top, Sequence{1}, 10, CounterWidth::Bits32));
  const ContinuityOutcome outcome =
      observe_counter(continuity, sample(0x100ULL, Sequence{2}, 20, CounterWidth::Bits32));
  LO_CHECK_EQ(outcome.discontinuity, CounterDiscontinuity::Wrap);
  LO_CHECK(outcome.delta_valid);
  // (2^32 - 1 - near_top) + 1 + 0x100 == 0x200
  LO_CHECK_EQ(outcome.delta, static_cast<std::uint64_t>(0x200));
  LO_CHECK_EQ(continuity.wrap_count, static_cast<std::uint64_t>(1));
}

LO_TEST(store, an_ambiguous_backward_step_resolves_to_reset) {
  // Far from the top of the counter width, a backward step is a reset. Losing a
  // delta is acceptable; inventing one is not.
  CounterContinuity continuity{};
  (void)observe_counter(continuity, sample(1000U, Sequence{1}, 10, CounterWidth::Bits32));
  const ContinuityOutcome outcome =
      observe_counter(continuity, sample(500U, Sequence{2}, 20, CounterWidth::Bits32));
  LO_CHECK_EQ(outcome.discontinuity, CounterDiscontinuity::Reset);
  LO_CHECK(!outcome.delta_valid);
}

LO_TEST(store, derivation_refuses_what_it_cannot_justify) {
  CounterContinuity continuity{};
  (void)observe_counter(continuity, sample(1000U, Sequence{1}, 10));
  (void)observe_counter(continuity, sample(2000U, Sequence{2}, 10 + 1000000000LL));

  DerivationRequest request{};
  request.previous = continuity.previous;
  request.current = continuity.latest;
  request.has_previous = true;
  request.max_window = Duration::from_seconds(60);

  request.capacity_bps = 0U;
  LO_CHECK_EQ(derive_utilization(request).fault, DerivationFault::MissingCapacity);

  request.capacity_bps = 100000000000ULL;
  request.previous_usable = false;
  LO_CHECK_EQ(derive_utilization(request).fault, DerivationFault::StaleSample);

  request.previous_usable = true;
  // Same instant, advanced revision: the samples are ordered but the window is
  // empty, which is not a usable interval.
  request.current = request.previous;
  request.current.sequence = Sequence{request.previous.sequence.value() + 1U};
  LO_CHECK_EQ(derive_utilization(request).fault, DerivationFault::NonMonotonicTime);

  // A discontinuity in the source stream between the two samples makes the
  // interval unusable even though both samples are individually valid.
  request.current = sample(2000U, Sequence{3}, 10 + 1000000000LL);
  request.current.gap_epoch = request.previous.gap_epoch + 1U;
  LO_CHECK_EQ(derive_utilization(request).fault, DerivationFault::RevisionGap);

  // A sample that would move the baseline backwards is refused outright.
  request.current = sample(2000U, Sequence{1}, 10 + 1000000000LL);
  LO_CHECK_EQ(derive_utilization(request).fault, DerivationFault::RevisionGap);
  request.current = sample(2000U, Sequence{2}, 10 + 1000000000LL);

  request.current = sample(2000U, Sequence{2}, 10 + 4000000000000LL);
  LO_CHECK_EQ(derive_utilization(request).fault, DerivationFault::WindowTooLarge);

  request.current = sample(2000U, Sequence{3}, 10 + 1000000000LL);
  const DerivationResult valid = derive_utilization(request);
  LO_CHECK_MSG(valid.valid, to_string(valid.fault));
  LO_REQUIRE(valid.valid);
  // 1000 bytes over one second on a 100 Gbit/s link is 80 parts per billion.
  LO_CHECK_EQ(valid.delta_bytes, static_cast<std::uint64_t>(1000));
  LO_CHECK_EQ(valid.bits_per_second, static_cast<std::uint64_t>(8000));
  LO_CHECK_EQ(valid.utilization_ppb, static_cast<std::uint64_t>(80));
}

LO_TEST(store, an_interleaved_metric_stream_still_derives_a_rate) {
  // A real source reports several families under one revision counter. The
  // samples of one family are therefore not consecutive revisions, and a rate
  // must still be derivable: what matters is that no revision was lost.
  CounterContinuity continuity{};
  (void)observe_counter(continuity, sample(1000U, Sequence{1}, 10));
  (void)observe_counter(continuity, sample(2000U, Sequence{4}, 10 + 1000000000LL));

  DerivationRequest request{};
  request.previous = continuity.previous;
  request.current = continuity.latest;
  request.has_previous = true;
  request.capacity_bps = 100000000000ULL;
  request.max_window = Duration::from_seconds(60);
  const DerivationResult derived = derive_utilization(request);
  LO_CHECK_MSG(derived.valid, to_string(derived.fault));
  LO_REQUIRE(derived.valid);
  LO_CHECK_EQ(derived.delta_bytes, static_cast<std::uint64_t>(1000));

  // The same interval with a lost revision is not derivable.
  request.current.gap_epoch = 1U;
  LO_CHECK_EQ(derive_utilization(request).fault, DerivationFault::RevisionGap);
}

LO_TEST(store, utilization_never_exceeds_full_scale_for_a_bounded_counter) {
  CounterContinuity continuity{};
  (void)observe_counter(continuity, sample(0U, Sequence{1}, 0));
  (void)observe_counter(continuity, sample(12500000000ULL, Sequence{2}, 1000000000LL));

  DerivationRequest request{};
  request.previous = continuity.previous;
  request.current = continuity.latest;
  request.has_previous = true;
  request.capacity_bps = 100000000000ULL;
  request.max_window = Duration::from_seconds(60);
  const DerivationResult derived = derive_utilization(request);
  LO_REQUIRE(derived.valid);
  // 12.5 GB in one second on a 100 Gbit/s link: full scale.
  LO_CHECK_EQ(derived.utilization_ppb, kPpbFullScale);
}

LO_TEST(store, ratio_helper_reports_when_undefined) {
  std::uint64_t ratio = 0;
  LO_CHECK(!ratio_ppb(1U, 0U, ratio));
  LO_CHECK(ratio_ppb(0U, 10U, ratio));
  LO_CHECK_EQ(ratio, static_cast<std::uint64_t>(0));
  LO_CHECK(ratio_ppb(1U, 1000000000ULL, ratio));
  LO_CHECK_EQ(ratio, static_cast<std::uint64_t>(1));
}

LO_TEST(store, evidence_slots_are_bounded) {
  Policy policy = make_default_policy();
  policy.limits.max_evidence_per_key = 3U;
  EvidenceStore store{policy};

  SourceRecord source{};
  source.id = SourceId::from_name("s");
  source.name = "s";
  source.authority = SourceAuthority::Primary;
  source.provenance = ProvenanceClass::Real;
  source.registered = true;

  AcceptContext context{};
  context.source = &source;
  context.family_enabled = true;

  for (std::uint64_t revision = 1; revision <= 10U; ++revision) {
    Observation observation{};
    observation.provenance.source = SourceId::from_name("s");
    observation.provenance.incarnation = IncarnationId::from_name("i");
    observation.provenance.epoch = EpochId::from_name("e");
    observation.provenance.generation = GenerationId::from_name("g");
    observation.provenance.sequence = Sequence{revision};
    observation.link = LinkId::from_name("l");
    observation.key = octets_key();
    observation.payload = CounterReading{revision * 100U, CounterWidth::Bits64, false};
    observation.observed_at = TimePoint::from_nanos(static_cast<Nanos>(revision));
    observation.received_at = observation.observed_at;
    const Observation identified = with_identity(observation);
    const AcceptOutcome outcome = store.accept(
        identified, context, TimePoint::from_nanos(static_cast<Nanos>(revision)));
    LO_CHECK(outcome.accepted);
  }

  const auto view = store.slot_view(LinkId::from_name("l"), octets_key(), TimePoint::from_nanos(10));
  LO_REQUIRE(view.has_value());
  LO_CHECK_EQ(view->retained, static_cast<std::size_t>(3));
  LO_CHECK_EQ(store.evidence_count(LinkId::from_name("l")), static_cast<std::size_t>(3));
}

LO_TEST(store, transition_log_is_bounded_and_counts_drops) {
  Limits limits{};
  limits.max_transitions_per_link = 4U;
  limits.max_total_transitions = 6U;
  TransitionLog log{limits};

  for (std::uint64_t index = 0; index < 12U; ++index) {
    TransitionEvent event{};
    event.link = LinkId::from_name("l");
    event.kind = TransitionKind::OperStateChanged;
    event.at = TimePoint::from_nanos(static_cast<Nanos>(index));
    log.record(event);
  }
  LO_CHECK_EQ(log.events_for(LinkId::from_name("l")), static_cast<std::size_t>(4));
  LO_CHECK(log.dropped_events() >= 6U);
  LO_CHECK(log.recent(64U).size() <= 6U);

  const std::size_t counted =
      log.count_since(LinkId::from_name("l"), TimePoint::from_nanos(100), 64U);
  LO_CHECK_EQ(counted, static_cast<std::size_t>(0));
}

}  // namespace
