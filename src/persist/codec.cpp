#include "linkobs/persist/codec.hpp"

#include <limits>

namespace linkobs {
namespace {

constexpr std::size_t kMaxStringField = 256U;

/// Hard sanity bounds applied while decoding. They stop a corrupt or hostile
/// snapshot from driving an allocation before the store applies its own
/// policy bounds during restore.
constexpr std::size_t kMaxRetiredScopesBound = 64U;
constexpr std::size_t kMaxEvidencePerSlotBound = 1024U;
constexpr std::size_t kMaxSourcesBound = 4096U;

}  // namespace

void ByteWriter::u8(std::uint8_t value) { buffer_.push_back(static_cast<char>(value)); }

void ByteWriter::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>(value & 0xFFU));
  u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void ByteWriter::u32(std::uint32_t value) {
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

void ByteWriter::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void ByteWriter::bool_value(bool value) { u8(value ? 1U : 0U); }

void ByteWriter::raw(const void* data, std::size_t size) {
  if (size == 0U) {
    return;
  }
  buffer_.append(static_cast<const char*>(data), size);
}

void ByteWriter::bytes(std::string_view value) {
  u64(static_cast<std::uint64_t>(value.size()));
  raw(value.data(), value.size());
}

void ByteWriter::count(std::size_t value) { u64(static_cast<std::uint64_t>(value)); }

void ByteReader::fail(std::string_view message) {
  ok_ = false;
  status_ = Status::of(StatusCode::CorruptData, message);
}

bool ByteReader::u8(std::uint8_t& out) {
  if (offset_ >= data_.size()) {
    fail("truncated byte");
    return false;
  }
  out = static_cast<std::uint8_t>(data_[offset_]);
  ++offset_;
  return true;
}

bool ByteReader::u16(std::uint16_t& out) {
  std::uint8_t low = 0;
  std::uint8_t high = 0;
  if (!u8(low) || !u8(high)) {
    return false;
  }
  out = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8U));
  return true;
}

bool ByteReader::u32(std::uint32_t& out) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    std::uint8_t byte = 0;
    if (!u8(byte)) {
      return false;
    }
    value |= (static_cast<std::uint32_t>(byte) << shift);
  }
  out = value;
  return true;
}

bool ByteReader::u64(std::uint64_t& out) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    std::uint8_t byte = 0;
    if (!u8(byte)) {
      return false;
    }
    value |= (static_cast<std::uint64_t>(byte) << shift);
  }
  out = value;
  return true;
}

bool ByteReader::i64(std::int64_t& out) {
  std::uint64_t value = 0;
  if (!u64(value)) {
    return false;
  }
  out = static_cast<std::int64_t>(value);
  return true;
}

bool ByteReader::bool_value(bool& out) {
  std::uint8_t value = 0;
  if (!u8(value)) {
    return false;
  }
  if (value > 1U) {
    fail("invalid boolean encoding");
    return false;
  }
  out = value == 1U;
  return true;
}

bool ByteReader::raw(void* out, std::size_t size) {
  if (size > remaining()) {
    fail("truncated payload");
    return false;
  }
  if (size > 0U) {
    const char* source = data_.data() + offset_;
    auto* destination = static_cast<char*>(out);
    for (std::size_t index = 0; index < size; ++index) {
      destination[index] = source[index];
    }
  }
  offset_ += size;
  return true;
}

bool ByteReader::bytes(std::string& out, std::size_t max_size) {
  std::uint64_t length = 0;
  if (!u64(length)) {
    return false;
  }
  if (length > static_cast<std::uint64_t>(max_size) ||
      length > static_cast<std::uint64_t>(remaining())) {
    fail("string field exceeds its bound");
    return false;
  }
  out.assign(static_cast<std::size_t>(length), '\0');
  if (length > 0U) {
    return raw(out.data(), static_cast<std::size_t>(length));
  }
  return true;
}

bool ByteReader::count(std::size_t& out, std::size_t max_value) {
  std::uint64_t value = 0;
  if (!u64(value)) {
    return false;
  }
  if (value > static_cast<std::uint64_t>(max_value)) {
    fail("collection exceeds its bound");
    return false;
  }
  out = static_cast<std::size_t>(value);
  return true;
}

void encode(ByteWriter& writer, Hash128 value) {
  writer.u64(value.hi);
  writer.u64(value.lo);
}

bool decode(ByteReader& reader, Hash128& out) {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  if (!reader.u64(hi) || !reader.u64(lo)) {
    return false;
  }
  out.hi = hi;
  out.lo = lo;
  return true;
}

void encode(ByteWriter& writer, Sequence value) { writer.u64(value.value()); }

bool decode(ByteReader& reader, Sequence& out) {
  std::uint64_t value = 0;
  if (!reader.u64(value)) {
    return false;
  }
  out = Sequence{value};
  return true;
}

void encode(ByteWriter& writer, TimePoint value) { writer.i64(value.nanos()); }

bool decode(ByteReader& reader, TimePoint& out) {
  std::int64_t value = 0;
  if (!reader.i64(value)) {
    return false;
  }
  out = TimePoint::from_nanos(value);
  return true;
}

void encode(ByteWriter& writer, Duration value) { writer.i64(value.nanos()); }

bool decode(ByteReader& reader, Duration& out) {
  std::int64_t value = 0;
  if (!reader.i64(value)) {
    return false;
  }
  out = Duration::from_nanos(value);
  return true;
}

void encode(ByteWriter& writer, const SequenceScope& value) {
  encode(writer, value.source);
  encode(writer, value.incarnation);
  encode(writer, value.epoch);
}

bool decode(ByteReader& reader, SequenceScope& out) {
  return decode(reader, out.source) && decode(reader, out.incarnation) &&
         decode(reader, out.epoch);
}

void encode(ByteWriter& writer, const MetricKey& value) {
  writer.u8(static_cast<std::uint8_t>(value.family));
  writer.u8(static_cast<std::uint8_t>(value.direction));
  writer.u8(static_cast<std::uint8_t>(value.error_class));
}

bool decode(ByteReader& reader, MetricKey& out) {
  std::uint8_t family = 0;
  std::uint8_t direction = 0;
  std::uint8_t error_class = 0;
  if (!reader.u8(family) || !reader.u8(direction) || !reader.u8(error_class)) {
    return false;
  }
  if (family >= kMetricFamilyCount || direction >= kDirectionCount ||
      error_class >= kErrorClassCount) {
    return false;
  }
  out.family = static_cast<MetricFamily>(family);
  out.direction = static_cast<Direction>(direction);
  out.error_class = static_cast<ErrorClass>(error_class);
  return true;
}

void encode(ByteWriter& writer, const MetricPayload& value) {
  writer.u8(static_cast<std::uint8_t>(value.index()));
  if (const auto* admin = std::get_if<AdminStateReading>(&value)) {
    writer.u8(static_cast<std::uint8_t>(admin->value));
  } else if (const auto* oper = std::get_if<OperStateReading>(&value)) {
    writer.u8(static_cast<std::uint8_t>(oper->value));
  } else if (const auto* counter = std::get_if<CounterReading>(&value)) {
    writer.u64(counter->raw);
    writer.u8(static_cast<std::uint8_t>(counter->width));
    writer.bool_value(counter->reset_declared);
  } else if (const auto* ratio = std::get_if<RatioReading>(&value)) {
    writer.u64(ratio->ppb);
  } else if (const auto* quality = std::get_if<QualityReading>(&value)) {
    writer.u64(quality->value_ppb);
    writer.bool_value(quality->degraded_declared);
  } else if (const auto* flap = std::get_if<FlapReading>(&value)) {
    writer.u64(flap->events);
    encode(writer, flap->window);
  }
}

bool decode(ByteReader& reader, MetricPayload& out) {
  std::uint8_t alternative = 0;
  if (!reader.u8(alternative)) {
    return false;
  }
  switch (alternative) {
    case 0: {
      std::uint8_t value = 0;
      if (!reader.u8(value) || value > static_cast<std::uint8_t>(AdminStateValue::Disabled)) {
        return false;
      }
      AdminStateReading reading{};
      reading.value = static_cast<AdminStateValue>(value);
      out = reading;
      return true;
    }
    case 1: {
      std::uint8_t value = 0;
      if (!reader.u8(value) || value > static_cast<std::uint8_t>(OperStateValue::Degraded)) {
        return false;
      }
      OperStateReading reading{};
      reading.value = static_cast<OperStateValue>(value);
      out = reading;
      return true;
    }
    case 2: {
      CounterReading reading{};
      std::uint8_t width = 0;
      if (!reader.u64(reading.raw) || !reader.u8(width) || width > 1U ||
          !reader.bool_value(reading.reset_declared)) {
        return false;
      }
      reading.width = static_cast<CounterWidth>(width);
      out = reading;
      return true;
    }
    case 3: {
      RatioReading reading{};
      if (!reader.u64(reading.ppb)) {
        return false;
      }
      out = reading;
      return true;
    }
    case 4: {
      QualityReading reading{};
      if (!reader.u64(reading.value_ppb) || !reader.bool_value(reading.degraded_declared)) {
        return false;
      }
      out = reading;
      return true;
    }
    case 5: {
      FlapReading reading{};
      Duration window{};
      if (!reader.u64(reading.events) || !decode(reader, window)) {
        return false;
      }
      reading.window = window;
      out = reading;
      return true;
    }
    default:
      return false;
  }
}

void encode(ByteWriter& writer, const Provenance& value) {
  encode(writer, value.source);
  encode(writer, value.incarnation);
  encode(writer, value.epoch);
  encode(writer, value.generation);
  encode(writer, value.sequence);
}

bool decode(ByteReader& reader, Provenance& out) {
  return decode(reader, out.source) && decode(reader, out.incarnation) &&
         decode(reader, out.epoch) && decode(reader, out.generation) &&
         decode(reader, out.sequence);
}

void encode(ByteWriter& writer, const Observation& value) {
  encode(writer, value.id);
  encode(writer, value.provenance);
  encode(writer, value.link);
  encode(writer, value.key);
  encode(writer, value.payload);
  encode(writer, value.observed_at);
  encode(writer, value.received_at);
}

bool decode(ByteReader& reader, Observation& out) {
  return decode(reader, out.id) && decode(reader, out.provenance) && decode(reader, out.link) &&
         decode(reader, out.key) && decode(reader, out.payload) &&
         decode(reader, out.observed_at) && decode(reader, out.received_at);
}

void encode(ByteWriter& writer, const Evidence& value) {
  encode(writer, value.observation);
  encode(writer, value.accepted_at);
  writer.u8(static_cast<std::uint8_t>(value.authority));
  writer.u8(static_cast<std::uint8_t>(value.provenance));
}

bool decode(ByteReader& reader, Evidence& out) {
  std::uint8_t authority = 0;
  std::uint8_t provenance = 0;
  if (!decode(reader, out.observation) || !decode(reader, out.accepted_at) ||
      !reader.u8(authority) || !reader.u8(provenance)) {
    return false;
  }
  if (authority > static_cast<std::uint8_t>(SourceAuthority::Primary) ||
      provenance > static_cast<std::uint8_t>(ProvenanceClass::Replayed)) {
    return false;
  }
  out.authority = static_cast<SourceAuthority>(authority);
  out.provenance = static_cast<ProvenanceClass>(provenance);
  return true;
}

void encode(ByteWriter& writer, const CounterReading& value) {
  writer.u64(value.raw);
  writer.u8(static_cast<std::uint8_t>(value.width));
  writer.bool_value(value.reset_declared);
}

bool decode(ByteReader& reader, CounterReading& out) {
  std::uint8_t width = 0;
  if (!reader.u64(out.raw) || !reader.u8(width) || width > 1U ||
      !reader.bool_value(out.reset_declared)) {
    return false;
  }
  out.width = static_cast<CounterWidth>(width);
  return true;
}

void encode(ByteWriter& writer, const CounterSample& value) {
  encode(writer, value.reading);
  encode(writer, value.observed_at);
  encode(writer, value.received_at);
  encode(writer, value.sequence);
  encode(writer, value.scope);
  encode(writer, value.generation);
  writer.u64(value.gap_epoch);
}

bool decode(ByteReader& reader, CounterSample& out) {
  return decode(reader, out.reading) && decode(reader, out.observed_at) &&
         decode(reader, out.received_at) && decode(reader, out.sequence) &&
         decode(reader, out.scope) && decode(reader, out.generation) &&
         reader.u64(out.gap_epoch);
}

void encode(ByteWriter& writer, const CounterContinuity& value) {
  encode(writer, value.latest);
  encode(writer, value.previous);
  writer.bool_value(value.has_latest);
  writer.bool_value(value.has_previous);
  writer.u8(static_cast<std::uint8_t>(value.last_discontinuity));
  writer.u64(value.reset_count);
  writer.u64(value.wrap_count);
  writer.u64(value.gap_count);
}

bool decode(ByteReader& reader, CounterContinuity& out) {
  std::uint8_t discontinuity = 0;
  if (!decode(reader, out.latest) || !decode(reader, out.previous) ||
      !reader.bool_value(out.has_latest) || !reader.bool_value(out.has_previous) ||
      !reader.u8(discontinuity) || discontinuity > static_cast<std::uint8_t>(CounterDiscontinuity::Wrap) ||
      !reader.u64(out.reset_count) || !reader.u64(out.wrap_count) || !reader.u64(out.gap_count)) {
    return false;
  }
  out.last_discontinuity = static_cast<CounterDiscontinuity>(discontinuity);
  return true;
}

void encode(ByteWriter& writer, const StreamState& value) {
  writer.bool_value(value.has_scope);
  encode(writer, value.scope);
  writer.bool_value(value.has_sequence);
  encode(writer, value.last_sequence);
  encode(writer, value.last_observation);
  writer.bool_value(value.gap_open);
  writer.count(value.retired_scopes.size());
  for (const SequenceScope& scope : value.retired_scopes) {
    encode(writer, scope);
  }
  writer.u64(value.accepted);
  writer.u64(value.fenced);
  writer.u64(value.gap_count);
  writer.u64(value.scope_changes);
}

bool decode(ByteReader& reader, StreamState& out) {
  std::size_t retired = 0;
  if (!reader.bool_value(out.has_scope) || !decode(reader, out.scope) ||
      !reader.bool_value(out.has_sequence) || !decode(reader, out.last_sequence) ||
      !decode(reader, out.last_observation) || !reader.bool_value(out.gap_open) ||
      !reader.count(retired, kMaxRetiredScopesBound)) {
    return false;
  }
  out.retired_scopes.clear();
  for (std::size_t index = 0; index < retired; ++index) {
    SequenceScope scope{};
    if (!decode(reader, scope)) {
      return false;
    }
    out.retired_scopes.push_back(scope);
  }
  return reader.u64(out.accepted) && reader.u64(out.fenced) && reader.u64(out.gap_count) &&
         reader.u64(out.scope_changes);
}

void encode(ByteWriter& writer, const SlotKey& value) {
  encode(writer, value.link);
  encode(writer, value.key);
}

bool decode(ByteReader& reader, SlotKey& out) {
  return decode(reader, out.link) && decode(reader, out.key);
}

void encode(ByteWriter& writer, const PersistedSlot& value) {
  encode(writer, value.slot);
  writer.count(value.history.size());
  for (const Evidence& evidence : value.history) {
    encode(writer, evidence);
  }
  writer.count(value.continuity.size());
  for (const auto& entry : value.continuity) {
    encode(writer, entry.first);
    encode(writer, entry.second);
  }
  writer.u64(value.accepted);
  writer.u64(value.fenced);
  encode(writer, value.last_gap_at);
  writer.bool_value(value.has_gap);
  writer.u64(value.reset_count);
  writer.u64(value.wrap_count);
}

bool decode(ByteReader& reader, PersistedSlot& out) {
  std::size_t history = 0;
  std::size_t continuity = 0;
  if (!decode(reader, out.slot) || !reader.count(history, kMaxEvidencePerSlotBound)) {
    return false;
  }
  out.history.clear();
  for (std::size_t index = 0; index < history; ++index) {
    Evidence evidence{};
    if (!decode(reader, evidence)) {
      return false;
    }
    out.history.push_back(std::move(evidence));
  }
  if (!reader.count(continuity, kMaxSourcesBound)) {
    return false;
  }
  out.continuity.clear();
  for (std::size_t index = 0; index < continuity; ++index) {
    SourceId source{};
    CounterContinuity state{};
    if (!decode(reader, source) || !decode(reader, state)) {
      return false;
    }
    out.continuity.emplace(source, state);
  }
  return reader.u64(out.accepted) && reader.u64(out.fenced) &&
         decode(reader, out.last_gap_at) && reader.bool_value(out.has_gap) &&
         reader.u64(out.reset_count) && reader.u64(out.wrap_count);
}

void encode(ByteWriter& writer, const LinkFacts& value) {
  encode(writer, value.id);
  writer.bytes(value.name);
  encode(writer, value.local_endpoint);
  encode(writer, value.remote_endpoint);
  encode(writer, value.local_port);
  encode(writer, value.remote_port);
  writer.u8(static_cast<std::uint8_t>(value.kind));
  encode(writer, value.generation);
  writer.bool_value(value.capacity_known);
  writer.u64(value.capacity_in_bps);
  writer.u64(value.capacity_out_bps);
  writer.u8(static_cast<std::uint8_t>(value.provenance));
}

bool decode(ByteReader& reader, LinkFacts& out) {
  std::uint8_t kind = 0;
  std::uint8_t provenance = 0;
  if (!decode(reader, out.id) || !reader.bytes(out.name, kMaxStringField) ||
      !decode(reader, out.local_endpoint) || !decode(reader, out.remote_endpoint) ||
      !decode(reader, out.local_port) || !decode(reader, out.remote_port) ||
      !reader.u8(kind) || kind > static_cast<std::uint8_t>(LinkKind::Aggregate) ||
      !decode(reader, out.generation) || !reader.bool_value(out.capacity_known) ||
      !reader.u64(out.capacity_in_bps) || !reader.u64(out.capacity_out_bps) ||
      !reader.u8(provenance) ||
      provenance > static_cast<std::uint8_t>(ProvenanceClass::Replayed)) {
    return false;
  }
  out.kind = static_cast<LinkKind>(kind);
  out.provenance = static_cast<ProvenanceClass>(provenance);
  return true;
}

void encode(ByteWriter& writer, const SourceRecord& value) {
  encode(writer, value.id);
  writer.bytes(value.name);
  writer.u8(static_cast<std::uint8_t>(value.authority));
  writer.u8(static_cast<std::uint8_t>(value.provenance));
  writer.bool_value(value.registered);
  writer.bool_value(value.retired);
}

bool decode(ByteReader& reader, SourceRecord& out) {
  std::uint8_t authority = 0;
  std::uint8_t provenance = 0;
  if (!decode(reader, out.id) || !reader.bytes(out.name, kMaxStringField) ||
      !reader.u8(authority) || !reader.u8(provenance) ||
      authority > static_cast<std::uint8_t>(SourceAuthority::Primary) ||
      provenance > static_cast<std::uint8_t>(ProvenanceClass::Replayed) ||
      !reader.bool_value(out.registered) || !reader.bool_value(out.retired)) {
    return false;
  }
  out.authority = static_cast<SourceAuthority>(authority);
  out.provenance = static_cast<ProvenanceClass>(provenance);
  return true;
}

void encode(ByteWriter& writer, const TransitionEvent& value) {
  encode(writer, value.id);
  encode(writer, value.link);
  writer.u8(static_cast<std::uint8_t>(value.kind));
  encode(writer, value.at);
  encode(writer, value.generation);
  encode(writer, value.source);
  writer.bytes(value.from);
  writer.bytes(value.to);
  writer.u64(value.detail);
}

bool decode(ByteReader& reader, TransitionEvent& out) {
  std::uint8_t kind = 0;
  if (!decode(reader, out.id) || !decode(reader, out.link) || !reader.u8(kind) ||
      kind > static_cast<std::uint8_t>(TransitionKind::StaleEvidence) ||
      !decode(reader, out.at) || !decode(reader, out.generation) ||
      !decode(reader, out.source) || !reader.bytes(out.from, kMaxStringField) ||
      !reader.bytes(out.to, kMaxStringField) || !reader.u64(out.detail)) {
    return false;
  }
  out.kind = static_cast<TransitionKind>(kind);
  return true;
}

void encode(ByteWriter& writer, const LinkEntry& value) {
  encode(writer, value.facts);
  encode(writer, value.registered_at);
  encode(writer, value.last_evidence_at);
  encode(writer, value.last_accepted_at);
  writer.u64(value.accepted_observations);
  writer.u64(value.fenced_observations);
  writer.bool_value(value.retired);
  writer.bool_value(value.has_evidence);
}

bool decode(ByteReader& reader, LinkEntry& out) {
  return decode(reader, out.facts) && decode(reader, out.registered_at) &&
         decode(reader, out.last_evidence_at) && decode(reader, out.last_accepted_at) &&
         reader.u64(out.accepted_observations) && reader.u64(out.fenced_observations) &&
         reader.bool_value(out.retired) && reader.bool_value(out.has_evidence);
}

void encode(ByteWriter& writer, const LinkGenerationState& value) {
  encode(writer, value.current_generation);
  writer.bool_value(value.has_generation);
  writer.count(value.retired_generations.size());
  for (const GenerationId& generation : value.retired_generations) {
    encode(writer, generation);
  }
  writer.u64(value.generation_changes);
}

bool decode(ByteReader& reader, LinkGenerationState& out) {
  std::size_t retired = 0;
  if (!decode(reader, out.current_generation) || !reader.bool_value(out.has_generation) ||
      !reader.count(retired, kMaxRetiredScopesBound)) {
    return false;
  }
  out.retired_generations.clear();
  for (std::size_t index = 0; index < retired; ++index) {
    GenerationId generation{};
    if (!decode(reader, generation)) {
      return false;
    }
    out.retired_generations.push_back(generation);
  }
  return reader.u64(out.generation_changes);
}

void encode(ByteWriter& writer, const PersistedLink& value) {
  encode(writer, value.facts);
  encode(writer, value.entry);
  encode(writer, value.generations);
}

bool decode(ByteReader& reader, PersistedLink& out) {
  return decode(reader, out.facts) && decode(reader, out.entry) && decode(reader, out.generations);
}

}  // namespace linkobs
