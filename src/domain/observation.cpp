#include "linkobs/domain/observation.hpp"

#include "linkobs/core/text.hpp"

namespace linkobs {

SequenceScope scope_of(const Provenance& provenance) noexcept {
  return SequenceScope{provenance.source, provenance.incarnation, provenance.epoch};
}

ObservationId compute_observation_id(const Observation& observation) {
  HashBuilder builder;
  builder.add_str("observation-v1");
  builder.add_hash(observation.provenance.source.hash());
  builder.add_hash(observation.provenance.incarnation.hash());
  builder.add_hash(observation.provenance.epoch.hash());
  builder.add_hash(observation.provenance.generation.hash());
  builder.add_u64(observation.provenance.sequence.value());
  builder.add_hash(observation.link.hash());
  builder.add_u8(static_cast<std::uint8_t>(observation.key.family));
  builder.add_u8(static_cast<std::uint8_t>(observation.key.direction));
  builder.add_u8(static_cast<std::uint8_t>(observation.key.error_class));
  builder.add_u8(static_cast<std::uint8_t>(observation.payload.index()));
  if (const auto* counter = std::get_if<CounterReading>(&observation.payload)) {
    builder.add_u64(counter->raw);
    builder.add_u8(static_cast<std::uint8_t>(counter->width));
    builder.add_bool(counter->reset_declared);
  } else if (const auto* ratio = std::get_if<RatioReading>(&observation.payload)) {
    builder.add_u64(ratio->ppb);
  } else if (const auto* quality = std::get_if<QualityReading>(&observation.payload)) {
    builder.add_u64(quality->value_ppb);
    builder.add_bool(quality->degraded_declared);
  } else if (const auto* flap = std::get_if<FlapReading>(&observation.payload)) {
    builder.add_u64(flap->events);
    builder.add_i64(flap->window.nanos());
  } else if (const auto* admin = std::get_if<AdminStateReading>(&observation.payload)) {
    builder.add_u8(static_cast<std::uint8_t>(admin->value));
  } else if (const auto* oper = std::get_if<OperStateReading>(&observation.payload)) {
    builder.add_u8(static_cast<std::uint8_t>(oper->value));
  }
  builder.add_i64(observation.observed_at.nanos());
  builder.add_i64(observation.received_at.nanos());
  return ObservationId::from_hash(builder.finish());
}

Observation with_identity(Observation observation) {
  observation.id = compute_observation_id(observation);
  return observation;
}

Status validate_observation(const Observation& observation, const Policy& policy) {
  if (!observation.provenance.source.valid()) {
    return Status::of(StatusCode::InvalidArgument, "observation has no source identity");
  }
  if (!observation.provenance.incarnation.valid()) {
    return Status::of(StatusCode::InvalidArgument, "observation has no source incarnation");
  }
  if (!observation.provenance.epoch.valid()) {
    return Status::of(StatusCode::InvalidArgument, "observation has no source epoch");
  }
  if (!observation.link.valid()) {
    return Status::of(StatusCode::InvalidArgument, "observation has no link identity");
  }
  const Status reading = validate_reading(observation.key, observation.payload, policy);
  if (!reading.ok()) {
    return reading;
  }
  return Status::success();
}

std::string format_observation(const Observation& observation) {
  TextFields fields(' ');
  fields.add("observation", observation.id.to_string());
  fields.add("link", observation.link.to_string());
  fields.add("metric", observation.key.to_string());
  fields.add("source", observation.provenance.source.to_string());
  fields.add("incarnation", observation.provenance.incarnation.to_string());
  fields.add("epoch", observation.provenance.epoch.to_string());
  fields.add("generation", observation.provenance.generation.to_string());
  fields.add("revision", observation.provenance.sequence.value());
  fields.add("observed-at", observation.observed_at.nanos());
  fields.add("received-at", observation.received_at.nanos());
  fields.add("value", format_payload(observation.payload));
  return fields.str();
}

}  // namespace linkobs
