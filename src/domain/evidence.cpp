#include "linkobs/domain/evidence.hpp"

#include "linkobs/core/text.hpp"

namespace linkobs {

AgeReport age_evidence(const Evidence& evidence, TimePoint now) noexcept {
  AgeReport report{};
  const Checked<Duration> observed = difference(now, evidence.observation.observed_at);
  const Checked<Duration> received = difference(now, evidence.observation.received_at);

  if (observed.ok && observed.value.is_negative()) {
    report.backwards_clock = true;
    report.observed_age = Duration{};
  } else if (observed.ok) {
    report.observed_age = observed.value;
  } else {
    report.backwards_clock = true;
    report.observed_age = Duration{};
  }

  if (received.ok && received.value.is_negative()) {
    report.backwards_clock = true;
    report.received_age = Duration{};
  } else if (received.ok) {
    report.received_age = received.value;
  } else {
    report.backwards_clock = true;
    report.received_age = Duration{};
  }
  return report;
}

Freshness classify_freshness(const AgeReport& age, const FreshnessPolicy& policy) noexcept {
  const Nanos effective = age.observed_age.nanos() > age.received_age.nanos()
                              ? age.observed_age.nanos()
                              : age.received_age.nanos();
  if (effective <= policy.fresh_within.nanos()) {
    return Freshness::Fresh;
  }
  if (effective <= policy.usable_within.nanos()) {
    return Freshness::Stale;
  }
  return Freshness::Expired;
}

Confidence derive_confidence(SourceAuthority authority,
                             Freshness freshness,
                             ConflictState conflict,
                             Completeness completeness) noexcept {
  if (freshness == Freshness::Unknown || freshness == Freshness::Expired) {
    return Confidence::Unknown;
  }
  if (conflict == ConflictState::Conflicting) {
    return Confidence::Low;
  }
  if (freshness == Freshness::Stale) {
    return Confidence::Low;
  }

  Confidence base = Confidence::Unknown;
  switch (authority) {
    case SourceAuthority::Primary:
      base = Confidence::High;
      break;
    case SourceAuthority::Secondary:
      base = Confidence::Medium;
      break;
    case SourceAuthority::Derived:
    case SourceAuthority::Synthetic:
      base = Confidence::Low;
      break;
    case SourceAuthority::Unknown:
      base = Confidence::Unknown;
      break;
  }
  if (completeness == Completeness::Incomplete && base > Confidence::Low) {
    base = static_cast<Confidence>(static_cast<std::uint8_t>(base) - 1U);
  }
  return base;
}

EvidenceId compute_evidence_id(const Observation& observation) {
  HashBuilder builder;
  builder.add_str("evidence-v1");
  builder.add_hash(observation.link.hash());
  builder.add_u8(static_cast<std::uint8_t>(observation.key.family));
  builder.add_u8(static_cast<std::uint8_t>(observation.key.direction));
  builder.add_u8(static_cast<std::uint8_t>(observation.key.error_class));
  builder.add_hash(observation.provenance.source.hash());
  builder.add_hash(observation.provenance.incarnation.hash());
  builder.add_hash(observation.provenance.epoch.hash());
  builder.add_u64(observation.provenance.sequence.value());
  return EvidenceId::from_hash(builder.finish());
}

std::string format_evidence(const Evidence& evidence,
                            Freshness freshness,
                            Confidence confidence) {
  TextFields fields(' ');
  fields.add("evidence", compute_evidence_id(evidence.observation).to_string());
  fields.add("observation", evidence.observation.id.to_string());
  fields.add("link", evidence.observation.link.to_string());
  fields.add("metric", evidence.observation.key.to_string());
  fields.add("source", evidence.observation.provenance.source.to_string());
  fields.add("incarnation", evidence.observation.provenance.incarnation.to_string());
  fields.add("epoch", evidence.observation.provenance.epoch.to_string());
  fields.add("generation", evidence.observation.provenance.generation.to_string());
  fields.add("revision", evidence.observation.provenance.sequence.value());
  fields.add("observed-at", evidence.observation.observed_at.nanos());
  fields.add("received-at", evidence.observation.received_at.nanos());
  fields.add("accepted-at", evidence.accepted_at.nanos());
  fields.add("freshness", to_string(freshness));
  fields.add("confidence", to_string(confidence));
  fields.add("value", format_payload(evidence.observation.payload));
  return fields.str();
}

}  // namespace linkobs
