// Link Observatory - shared test fixtures.
//
// Fixtures build declarations, never runtime state: every test drives the
// runtime through its public ingest surface, so the tests exercise the same path
// a real source would.

#pragma once

#include <string>
#include <string_view>

#include "linkobs/domain/policy.hpp"
#include "linkobs/ingest/codec.hpp"
#include "linkobs/ingest/record.hpp"
#include "linkobs/runtime/config.hpp"

namespace linkobs::test {

inline constexpr Nanos kBaseInstant = 1700000000000000000LL;

/// The platform temporary directory. Uses the standard library rather than an
/// environment lookup so the tests behave the same everywhere.
[[nodiscard]] std::string temporary_directory();

[[nodiscard]] inline TimePoint instant(Nanos nanos) { return TimePoint::from_nanos(nanos); }

[[nodiscard]] inline SourceDecl make_source(std::string_view name,
                                            SourceAuthority authority = SourceAuthority::Primary,
                                            ProvenanceClass provenance = ProvenanceClass::Synthetic) {
  SourceDecl declaration{};
  declaration.name.assign(name);
  declaration.authority = authority;
  declaration.provenance = provenance;
  return declaration;
}

[[nodiscard]] inline LinkDecl make_link(std::string_view name,
                                        std::uint64_t capacity_in_bps = 100000000000ULL,
                                        std::uint64_t capacity_out_bps = 100000000000ULL,
                                        std::string_view generation = "gen-0") {
  LinkDecl declaration{};
  declaration.name.assign(name);
  declaration.kind = LinkKind::Physical;
  declaration.generation.assign(generation);
  declaration.provenance = ProvenanceClass::Synthetic;
  declaration.capacity_in_bps = capacity_in_bps;
  declaration.capacity_out_bps = capacity_out_bps;
  declaration.capacity_known = capacity_in_bps > 0U || capacity_out_bps > 0U;
  return declaration;
}

struct StreamPosition {
  std::string incarnation{"inc-0"};
  std::string epoch{"epoch-0"};
  std::string generation{"gen-0"};
  std::uint64_t revision{1};
};

[[nodiscard]] inline ObservationDecl make_observation(std::string_view link,
                                                      std::string_view source,
                                                      const MetricKey& key,
                                                      MetricPayload payload,
                                                      const StreamPosition& position,
                                                      Nanos observed_at,
                                                      Nanos received_at) {
  ObservationDecl declaration{};
  declaration.link.assign(link);
  declaration.source.assign(source);
  declaration.incarnation = position.incarnation;
  declaration.epoch = position.epoch;
  declaration.generation = position.generation;
  declaration.revision = position.revision;
  declaration.observed_at = observed_at;
  declaration.received_at = received_at;
  declaration.key = key;
  declaration.payload = std::move(payload);
  return declaration;
}

[[nodiscard]] inline MetricKey oper_key() {
  return MetricKey{MetricFamily::OperState, Direction::Unknown, ErrorClass::None};
}

[[nodiscard]] inline MetricKey admin_key() {
  return MetricKey{MetricFamily::AdminState, Direction::Unknown, ErrorClass::None};
}

[[nodiscard]] inline MetricKey octets_key(Direction direction = Direction::In) {
  return MetricKey{MetricFamily::Octets, direction, ErrorClass::None};
}

[[nodiscard]] inline MetricKey errors_key(Direction direction = Direction::In,
                                          ErrorClass error_class = ErrorClass::Total) {
  return MetricKey{MetricFamily::Errors, direction, error_class};
}

[[nodiscard]] inline MetricKey quality_key(Direction direction = Direction::In) {
  return MetricKey{MetricFamily::SignalQuality, direction, ErrorClass::None};
}

[[nodiscard]] inline MetricKey reported_util_key() {
  return MetricKey{MetricFamily::UtilReported, Direction::In, ErrorClass::None};
}

[[nodiscard]] inline ObservationDecl make_oper(std::string_view link,
                                               std::string_view source,
                                               const StreamPosition& position,
                                               OperStateValue value,
                                               Nanos observed_at,
                                               Nanos received_at) {
  OperStateReading reading{};
  reading.value = value;
  return make_observation(link, source, oper_key(), reading, position, observed_at, received_at);
}

[[nodiscard]] inline ObservationDecl make_admin(std::string_view link,
                                                std::string_view source,
                                                const StreamPosition& position,
                                                AdminStateValue value,
                                                Nanos observed_at,
                                                Nanos received_at) {
  AdminStateReading reading{};
  reading.value = value;
  return make_observation(link, source, admin_key(), reading, position, observed_at, received_at);
}

[[nodiscard]] inline ObservationDecl make_octets(std::string_view link,
                                                 std::string_view source,
                                                 const StreamPosition& position,
                                                 std::uint64_t counter,
                                                 Nanos observed_at,
                                                 Nanos received_at,
                                                 bool reset = false,
                                                 CounterWidth width = CounterWidth::Bits64,
                                                 Direction direction = Direction::In) {
  CounterReading reading{};
  reading.raw = counter;
  reading.width = width;
  reading.reset_declared = reset;
  return make_observation(link, source, octets_key(direction), reading, position, observed_at,
                          received_at);
}

[[nodiscard]] inline ObservationDecl make_errors(std::string_view link,
                                                 std::string_view source,
                                                 const StreamPosition& position,
                                                 std::uint64_t counter,
                                                 Nanos observed_at,
                                                 Nanos received_at,
                                                 Direction direction = Direction::In) {
  CounterReading reading{};
  reading.raw = counter;
  reading.width = CounterWidth::Bits64;
  return make_observation(link, source, errors_key(direction), reading, position, observed_at,
                          received_at);
}

[[nodiscard]] inline ObservationDecl make_quality(std::string_view link,
                                                  std::string_view source,
                                                  const StreamPosition& position,
                                                  std::uint64_t ppb,
                                                  bool degraded,
                                                  Nanos observed_at,
                                                  Nanos received_at) {
  QualityReading reading{};
  reading.value_ppb = ppb;
  reading.degraded_declared = degraded;
  return make_observation(link, source, quality_key(), reading, position, observed_at, received_at);
}

[[nodiscard]] inline ObservationDecl make_reported_util(std::string_view link,
                                                        std::string_view source,
                                                        const StreamPosition& position,
                                                        std::uint64_t ppb,
                                                        Nanos observed_at,
                                                        Nanos received_at) {
  RatioReading reading{};
  reading.ppb = ppb;
  return make_observation(link, source, reported_util_key(), reading, position, observed_at,
                          received_at);
}

[[nodiscard]] inline Batch make_batch(std::initializer_list<ObservationDecl> declarations) {
  Batch batch{};
  for (const ObservationDecl& declaration : declarations) {
    ParsedRecord record{};
    record.kind = ParsedRecord::Kind::Observation;
    record.observation = declaration;
    batch.records.push_back(std::move(record));
  }
  return batch;
}

[[nodiscard]] inline RuntimeConfig make_config(std::string snapshot_path = {},
                                               std::size_t workers = 0U) {
  RuntimeConfig config{};
  config.snapshot_path = std::move(snapshot_path);
  config.workers = workers;
  config.policy = make_default_policy();
  config.incarnation_name = "test-incarnation";
  return config;
}

/// A temporary file path that is removed on destruction.
class TempPath {
 public:
  explicit TempPath(std::string_view stem);
  ~TempPath();

  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  void remove() const;

 private:
  std::string path_;
};

}  // namespace linkobs::test
