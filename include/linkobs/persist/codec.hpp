// Link Observatory - explicit little-endian binary codec.
//
// Persisted bytes never depend on struct layout, padding, native endianness or
// compiler version: every field is written and read explicitly, so a snapshot
// written by one build is readable by another with the same format version.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "linkobs/core/status.hpp"
#include "linkobs/domain/link.hpp"
#include "linkobs/domain/observation.hpp"
#include "linkobs/store/evidence_store.hpp"
#include "linkobs/store/history.hpp"
#include "linkobs/store/link_registry.hpp"

namespace linkobs {

class ByteWriter {
 public:
  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void bool_value(bool value);
  void raw(const void* data, std::size_t size);
  void bytes(std::string_view value);
  void count(std::size_t value);

  [[nodiscard]] const std::string& data() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

 private:
  std::string buffer_{};
};

class ByteReader {
 public:
  explicit ByteReader(std::string_view data) : data_(data) {}

  [[nodiscard]] bool u8(std::uint8_t& out);
  [[nodiscard]] bool u16(std::uint16_t& out);
  [[nodiscard]] bool u32(std::uint32_t& out);
  [[nodiscard]] bool u64(std::uint64_t& out);
  [[nodiscard]] bool i64(std::int64_t& out);
  [[nodiscard]] bool bool_value(bool& out);
  [[nodiscard]] bool raw(void* out, std::size_t size);
  [[nodiscard]] bool bytes(std::string& out, std::size_t max_size);
  [[nodiscard]] bool count(std::size_t& out, std::size_t max_value);

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] bool exhausted() const noexcept { return offset_ >= data_.size(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  void fail(std::string_view message);

  std::string_view data_{};
  std::size_t offset_{0};
  bool ok_{true};
  Status status_{};
};

// ---------------------------------------------------------------------------
// Domain encoders. Each encode has a matching decode; a mismatch is a
// corrupt-data status, never a partially applied value.
// ---------------------------------------------------------------------------

void encode(ByteWriter& writer, Hash128 value);
[[nodiscard]] bool decode(ByteReader& reader, Hash128& out);

template <class Tag>
void encode(ByteWriter& writer, StrongId<Tag> value) {
  encode(writer, value.hash());
}

template <class Tag>
[[nodiscard]] bool decode(ByteReader& reader, StrongId<Tag>& out) {
  Hash128 hash{};
  if (!decode(reader, hash)) {
    return false;
  }
  out = StrongId<Tag>::from_hash(hash);
  return true;
}

void encode(ByteWriter& writer, Sequence value);
[[nodiscard]] bool decode(ByteReader& reader, Sequence& out);

void encode(ByteWriter& writer, TimePoint value);
[[nodiscard]] bool decode(ByteReader& reader, TimePoint& out);

void encode(ByteWriter& writer, Duration value);
[[nodiscard]] bool decode(ByteReader& reader, Duration& out);

void encode(ByteWriter& writer, const SequenceScope& value);
[[nodiscard]] bool decode(ByteReader& reader, SequenceScope& out);

void encode(ByteWriter& writer, const MetricKey& value);
[[nodiscard]] bool decode(ByteReader& reader, MetricKey& out);

void encode(ByteWriter& writer, const MetricPayload& value);
[[nodiscard]] bool decode(ByteReader& reader, MetricPayload& out);

void encode(ByteWriter& writer, const Provenance& value);
[[nodiscard]] bool decode(ByteReader& reader, Provenance& out);

void encode(ByteWriter& writer, const Observation& value);
[[nodiscard]] bool decode(ByteReader& reader, Observation& out);

void encode(ByteWriter& writer, const Evidence& value);
[[nodiscard]] bool decode(ByteReader& reader, Evidence& out);

void encode(ByteWriter& writer, const CounterReading& value);
[[nodiscard]] bool decode(ByteReader& reader, CounterReading& out);

void encode(ByteWriter& writer, const CounterSample& value);
[[nodiscard]] bool decode(ByteReader& reader, CounterSample& out);

void encode(ByteWriter& writer, const CounterContinuity& value);
[[nodiscard]] bool decode(ByteReader& reader, CounterContinuity& out);

void encode(ByteWriter& writer, const StreamState& value);
[[nodiscard]] bool decode(ByteReader& reader, StreamState& out);

void encode(ByteWriter& writer, const SlotKey& value);
[[nodiscard]] bool decode(ByteReader& reader, SlotKey& out);

void encode(ByteWriter& writer, const PersistedSlot& value);
[[nodiscard]] bool decode(ByteReader& reader, PersistedSlot& out);

void encode(ByteWriter& writer, const LinkFacts& value);
[[nodiscard]] bool decode(ByteReader& reader, LinkFacts& out);

void encode(ByteWriter& writer, const SourceRecord& value);
[[nodiscard]] bool decode(ByteReader& reader, SourceRecord& out);

void encode(ByteWriter& writer, const TransitionEvent& value);
[[nodiscard]] bool decode(ByteReader& reader, TransitionEvent& out);

void encode(ByteWriter& writer, const LinkEntry& value);
[[nodiscard]] bool decode(ByteReader& reader, LinkEntry& out);

void encode(ByteWriter& writer, const LinkGenerationState& value);
[[nodiscard]] bool decode(ByteReader& reader, LinkGenerationState& out);

void encode(ByteWriter& writer, const PersistedLink& value);
[[nodiscard]] bool decode(ByteReader& reader, PersistedLink& out);

}  // namespace linkobs
