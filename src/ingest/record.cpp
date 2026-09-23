#include "linkobs/ingest/record.hpp"

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "linkobs/core/text.hpp"

namespace linkobs {
namespace {

constexpr std::size_t kMaxFields = 24U;
constexpr std::size_t kMaxKeyLength = 32U;
constexpr std::size_t kMaxValueLength = 256U;
constexpr std::uint64_t kRecordVersion = 1U;

using FieldMap = std::map<std::string, std::string>;

[[nodiscard]] Result<FieldMap> parse_fields(std::string_view line) {
  FieldMap fields;
  std::size_t cursor = 0;
  while (cursor < line.size()) {
    while (cursor < line.size() && line[cursor] == ' ') {
      ++cursor;
    }
    if (cursor >= line.size()) {
      break;
    }
    const std::size_t end = line.find(' ', cursor);
    const std::string_view token =
        (end == std::string_view::npos) ? line.substr(cursor) : line.substr(cursor, end - cursor);
    const std::size_t equals = token.find('=');
    if (equals == std::string_view::npos) {
      return failure<FieldMap>(StatusCode::InvalidArgument, "field has no '=' separator");
    }
    const std::string_view key = token.substr(0, equals);
    const std::string_view value = token.substr(equals + 1U);
    if (key.empty() || key.size() > kMaxKeyLength) {
      return failure<FieldMap>(StatusCode::InvalidArgument, "field name is empty or too long");
    }
    if (value.size() > kMaxValueLength) {
      return failure<FieldMap>(StatusCode::OutOfRange, "field value exceeds 256 bytes");
    }
    if (!is_ascii_printable(key) || !is_ascii_printable(value)) {
      return failure<FieldMap>(StatusCode::InvalidArgument, "field contains non-printable bytes");
    }
    if (fields.size() >= kMaxFields && fields.find(std::string{key}) == fields.end()) {
      return failure<FieldMap>(StatusCode::CapacityExceeded, "record has too many fields");
    }
    const auto inserted = fields.emplace(std::string{key}, std::string{value});
    if (!inserted.second) {
      return failure<FieldMap>(StatusCode::InvalidArgument, "duplicate field name");
    }
    if (end == std::string_view::npos) {
      break;
    }
    cursor = end + 1U;
  }
  return fields;
}

class FieldReader {
 public:
  explicit FieldReader(const FieldMap& fields) : fields_(fields) {}

  [[nodiscard]] bool has(std::string_view key) const {
    return fields_.find(std::string{key}) != fields_.end();
  }

  [[nodiscard]] Result<std::string> required_token(std::string_view key) {
    const auto found = fields_.find(std::string{key});
    if (found == fields_.end()) {
      return failure<std::string>(StatusCode::InvalidArgument,
                                  std::string{"missing required field '"} + std::string{key} + "'");
    }
    if (!is_token(found->second)) {
      return failure<std::string>(StatusCode::InvalidArgument,
                                  std::string{"field '"} + std::string{key} +
                                      "' is not a valid token");
    }
    used_.insert(found->first);
    return found->second;
  }

  [[nodiscard]] Result<std::string> required_text(std::string_view key, std::size_t max_length) {
    const auto found = fields_.find(std::string{key});
    if (found == fields_.end()) {
      return failure<std::string>(StatusCode::InvalidArgument,
                                  std::string{"missing required field '"} + std::string{key} + "'");
    }
    if (found->second.empty() || found->second.size() > max_length) {
      return failure<std::string>(StatusCode::InvalidArgument,
                                  std::string{"field '"} + std::string{key} +
                                      "' is empty or exceeds its bound");
    }
    used_.insert(found->first);
    return found->second;
  }

  [[nodiscard]] Result<std::uint64_t> required_u64(std::string_view key) {
    const auto found = fields_.find(std::string{key});
    if (found == fields_.end()) {
      return failure<std::uint64_t>(StatusCode::InvalidArgument,
                                    std::string{"missing required field '"} + std::string{key} + "'");
    }
    std::uint64_t value = 0;
    if (!parse_u64_dec(found->second, value)) {
      return failure<std::uint64_t>(StatusCode::InvalidArgument,
                                    std::string{"field '"} + std::string{key} +
                                        "' is not an unsigned decimal integer");
    }
    used_.insert(found->first);
    return value;
  }

  [[nodiscard]] Result<std::int64_t> required_i64(std::string_view key) {
    const auto found = fields_.find(std::string{key});
    if (found == fields_.end()) {
      return failure<std::int64_t>(StatusCode::InvalidArgument,
                                   std::string{"missing required field '"} + std::string{key} + "'");
    }
    std::int64_t value = 0;
    if (!parse_i64_dec(found->second, value)) {
      return failure<std::int64_t>(StatusCode::InvalidArgument,
                                   std::string{"field '"} + std::string{key} +
                                       "' is not a signed decimal integer");
    }
    used_.insert(found->first);
    return value;
  }

  [[nodiscard]] Result<std::uint64_t> optional_u64(std::string_view key,
                                                   std::uint64_t fallback) {
    if (!has(key)) {
      return fallback;
    }
    return required_u64(key);
  }

  [[nodiscard]] Result<bool> optional_bool(std::string_view key, bool fallback) {
    if (!has(key)) {
      return fallback;
    }
    const auto found = fields_.find(std::string{key});
    used_.insert(found->first);
    if (found->second == "true") {
      return true;
    }
    if (found->second == "false") {
      return false;
    }
    return failure<bool>(StatusCode::InvalidArgument,
                         std::string{"field '"} + std::string{key} +
                             "' must be true or false");
  }

  /// Marks a key as consumed so that "unknown field" detection is exact.
  void ignore(std::string_view key) {
    if (has(key)) {
      used_.insert(std::string{key});
    }
  }

  [[nodiscard]] Result<std::string> ensure_all_used() const {
    for (const auto& field : fields_) {
      if (used_.find(field.first) == used_.end()) {
        return failure<std::string>(StatusCode::InvalidArgument,
                                    std::string{"unknown field '"} + field.first + "'");
      }
    }
    return std::string{};
  }

 private:
  const FieldMap& fields_;
  std::set<std::string> used_{};
};

[[nodiscard]] Result<ParsedRecord> parse_source_record(FieldReader& reader) {
  ParsedRecord record{};
  record.kind = ParsedRecord::Kind::Source;
  auto name = reader.required_token("name");
  if (!name.ok()) {
    return failure<ParsedRecord>(name.status().code(), name.status().message());
  }
  record.source.name = name.value();

  auto authority = reader.required_token("authority");
  if (!authority.ok()) {
    return failure<ParsedRecord>(authority.status().code(), authority.status().message());
  }
  if (!parse_source_authority(authority.value(), record.source.authority)) {
    return failure<ParsedRecord>(StatusCode::Unsupported, "unknown source authority level");
  }

  auto provenance = reader.required_token("provenance");
  if (!provenance.ok()) {
    return failure<ParsedRecord>(provenance.status().code(), provenance.status().message());
  }
  if (!parse_provenance_class(provenance.value(), record.source.provenance)) {
    return failure<ParsedRecord>(StatusCode::Unsupported, "unknown provenance class");
  }
  if (record.source.provenance == ProvenanceClass::Unknown) {
    return failure<ParsedRecord>(StatusCode::InvalidArgument,
                                 "source must declare a real, synthetic or replayed provenance");
  }

  auto retired = reader.optional_bool("retired", false);
  if (!retired.ok()) {
    return failure<ParsedRecord>(retired.status().code(), retired.status().message());
  }
  record.source.retired = retired.value();

  auto leftover = reader.ensure_all_used();
  if (!leftover.ok()) {
    return failure<ParsedRecord>(leftover.status().code(), leftover.status().message());
  }
  return record;
}

[[nodiscard]] Result<ParsedRecord> parse_link_record(FieldReader& reader) {
  ParsedRecord record{};
  record.kind = ParsedRecord::Kind::Link;
  auto name = reader.required_token("name");
  if (!name.ok()) {
    return failure<ParsedRecord>(name.status().code(), name.status().message());
  }
  record.link.name = name.value();

  auto kind = reader.required_token("link-kind");
  if (!kind.ok()) {
    return failure<ParsedRecord>(kind.status().code(), kind.status().message());
  }
  if (!parse_link_kind(kind.value(), record.link.kind)) {
    return failure<ParsedRecord>(StatusCode::Unsupported, "unknown link kind");
  }

  auto generation = reader.required_token("linkgen");
  if (!generation.ok()) {
    return failure<ParsedRecord>(generation.status().code(), generation.status().message());
  }
  record.link.generation = generation.value();

  auto provenance = reader.required_token("provenance");
  if (!provenance.ok()) {
    return failure<ParsedRecord>(provenance.status().code(), provenance.status().message());
  }
  if (!parse_provenance_class(provenance.value(), record.link.provenance)) {
    return failure<ParsedRecord>(StatusCode::Unsupported, "unknown provenance class");
  }

  auto capacity_in = reader.optional_u64("capacity-in-bps", 0U);
  if (!capacity_in.ok()) {
    return failure<ParsedRecord>(capacity_in.status().code(), capacity_in.status().message());
  }
  auto capacity_out = reader.optional_u64("capacity-out-bps", 0U);
  if (!capacity_out.ok()) {
    return failure<ParsedRecord>(capacity_out.status().code(), capacity_out.status().message());
  }
  record.link.capacity_in_bps = capacity_in.value();
  record.link.capacity_out_bps = capacity_out.value();
  record.link.capacity_known = capacity_in.value() > 0U || capacity_out.value() > 0U;

  const std::array<std::pair<std::string_view, std::string*>, 4> optional_names{{
      {"local-endpoint", &record.link.local_endpoint},
      {"remote-endpoint", &record.link.remote_endpoint},
      {"local-port", &record.link.local_port},
      {"remote-port", &record.link.remote_port},
  }};
  for (const auto& entry : optional_names) {
    if (!reader.has(entry.first)) {
      continue;
    }
    auto value = reader.required_token(entry.first);
    if (!value.ok()) {
      return failure<ParsedRecord>(value.status().code(), value.status().message());
    }
    *entry.second = value.value();
  }

  auto leftover = reader.ensure_all_used();
  if (!leftover.ok()) {
    return failure<ParsedRecord>(leftover.status().code(), leftover.status().message());
  }
  return record;
}

[[nodiscard]] Result<ParsedRecord> parse_observation_record(FieldReader& reader,
                                                            const Policy& policy) {
  ParsedRecord record{};
  record.kind = ParsedRecord::Kind::Observation;
  ObservationDecl& declaration = record.observation;

  const std::array<std::pair<std::string_view, std::string*>, 5> names{{
      {"link", &declaration.link},
      {"source", &declaration.source},
      {"incarnation", &declaration.incarnation},
      {"epoch", &declaration.epoch},
      {"generation", &declaration.generation},
  }};
  for (const auto& entry : names) {
    auto value = reader.required_token(entry.first);
    if (!value.ok()) {
      return failure<ParsedRecord>(value.status().code(), value.status().message());
    }
    *entry.second = value.value();
  }

  auto revision = reader.required_u64("revision");
  if (!revision.ok()) {
    return failure<ParsedRecord>(revision.status().code(), revision.status().message());
  }
  declaration.revision = revision.value();

  auto observed_at = reader.required_i64("observed-at");
  if (!observed_at.ok()) {
    return failure<ParsedRecord>(observed_at.status().code(), observed_at.status().message());
  }
  declaration.observed_at = observed_at.value();

  auto received_at = reader.required_i64("received-at");
  if (!received_at.ok()) {
    return failure<ParsedRecord>(received_at.status().code(), received_at.status().message());
  }
  declaration.received_at = received_at.value();

  // A metric key is "<family>[/<direction>][/<error-class>]", so it contains
  // '/' and is validated by the key parser rather than by the token rule.
  auto metric = reader.required_text("metric", 64U);
  if (!metric.ok()) {
    return failure<ParsedRecord>(metric.status().code(), metric.status().message());
  }
  auto key = MetricKey::parse(metric.value());
  if (!key.ok()) {
    return failure<ParsedRecord>(key.status().code(), key.status().message());
  }
  declaration.key = key.value();

  switch (declaration.key.family) {
    case MetricFamily::AdminState:
    case MetricFamily::OperState: {
      auto value = reader.required_token("value");
      if (!value.ok()) {
        return failure<ParsedRecord>(value.status().code(), value.status().message());
      }
      if (declaration.key.family == MetricFamily::AdminState) {
        AdminStateValue parsed{};
        if (!parse_admin_state(value.value(), parsed)) {
          return failure<ParsedRecord>(StatusCode::Unsupported, "unknown administrative state");
        }
        declaration.payload = AdminStateReading{parsed};
      } else {
        OperStateValue parsed{};
        if (!parse_oper_state(value.value(), parsed)) {
          return failure<ParsedRecord>(StatusCode::Unsupported, "unknown operational state");
        }
        declaration.payload = OperStateReading{parsed};
      }
      break;
    }
    case MetricFamily::Octets:
    case MetricFamily::Errors:
    case MetricFamily::Discards: {
      auto counter = reader.required_u64("counter");
      if (!counter.ok()) {
        return failure<ParsedRecord>(counter.status().code(), counter.status().message());
      }
      auto width_token = reader.required_token("width");
      if (!width_token.ok()) {
        return failure<ParsedRecord>(width_token.status().code(), width_token.status().message());
      }
      CounterWidth width{};
      if (!parse_counter_width(width_token.value(), width)) {
        return failure<ParsedRecord>(StatusCode::Unsupported, "unknown counter width");
      }
      auto reset = reader.optional_bool("reset", false);
      if (!reset.ok()) {
        return failure<ParsedRecord>(reset.status().code(), reset.status().message());
      }
      CounterReading reading{};
      reading.raw = counter.value();
      reading.width = width;
      reading.reset_declared = reset.value();
      declaration.payload = reading;
      break;
    }
    case MetricFamily::UtilReported: {
      auto ppb = reader.required_u64("ppb");
      if (!ppb.ok()) {
        return failure<ParsedRecord>(ppb.status().code(), ppb.status().message());
      }
      RatioReading reading{};
      reading.ppb = ppb.value();
      declaration.payload = reading;
      break;
    }
    case MetricFamily::SignalQuality: {
      auto ppb = reader.required_u64("ppb");
      if (!ppb.ok()) {
        return failure<ParsedRecord>(ppb.status().code(), ppb.status().message());
      }
      auto degraded = reader.optional_bool("degraded", false);
      if (!degraded.ok()) {
        return failure<ParsedRecord>(degraded.status().code(), degraded.status().message());
      }
      QualityReading reading{};
      reading.value_ppb = ppb.value();
      reading.degraded_declared = degraded.value();
      declaration.payload = reading;
      break;
    }
    case MetricFamily::FlapReport: {
      auto events = reader.required_u64("events");
      if (!events.ok()) {
        return failure<ParsedRecord>(events.status().code(), events.status().message());
      }
      auto window = reader.required_i64("window-ns");
      if (!window.ok()) {
        return failure<ParsedRecord>(window.status().code(), window.status().message());
      }
      FlapReading reading{};
      reading.events = events.value();
      reading.window = Duration::from_nanos(window.value());
      declaration.payload = reading;
      break;
    }
  }

  const Status reading_valid = validate_reading(declaration.key, declaration.payload, policy);
  if (!reading_valid.ok()) {
    return failure<ParsedRecord>(reading_valid.code(), reading_valid.message());
  }

  auto leftover = reader.ensure_all_used();
  if (!leftover.ok()) {
    return failure<ParsedRecord>(leftover.status().code(), leftover.status().message());
  }
  return record;
}

}  // namespace

Result<ParsedRecord> parse_record(std::string_view line, const Policy& policy) {
  const std::string_view trimmed = trim_ascii(line);
  if (trimmed.empty() || trimmed.front() == '#') {
    return failure<ParsedRecord>(StatusCode::InvalidArgument, "line is empty or a comment");
  }
  if (trimmed.size() > policy.limits.max_record_bytes) {
    return failure<ParsedRecord>(StatusCode::OutOfRange, "record exceeds the configured byte bound");
  }

  auto fields = parse_fields(trimmed);
  if (!fields.ok()) {
    return failure<ParsedRecord>(fields.status().code(), fields.status().message());
  }
  FieldReader reader{fields.value()};

  auto version = reader.required_u64("v");
  if (!version.ok()) {
    return failure<ParsedRecord>(version.status().code(), version.status().message());
  }
  if (version.value() != kRecordVersion) {
    return failure<ParsedRecord>(StatusCode::VersionMismatch,
                                 "record version is not supported by this build");
  }

  auto kind = reader.required_token("kind");
  if (!kind.ok()) {
    return failure<ParsedRecord>(kind.status().code(), kind.status().message());
  }
  if (kind.value() == "source") {
    return parse_source_record(reader);
  }
  if (kind.value() == "link") {
    return parse_link_record(reader);
  }
  if (kind.value() == "observation") {
    return parse_observation_record(reader, policy);
  }
  return failure<ParsedRecord>(StatusCode::Unsupported, "unknown record kind");
}

std::string format_record(const SourceDecl& declaration) {
  TextFields fields(' ');
  fields.add("v", static_cast<std::uint64_t>(1));
  fields.add("kind", "source");
  fields.add("name", declaration.name);
  fields.add("authority", to_string(declaration.authority));
  fields.add("provenance", to_string(declaration.provenance));
  if (declaration.retired) {
    fields.add("retired", true);
  }
  return fields.str();
}

std::string format_record(const LinkDecl& declaration) {
  TextFields fields(' ');
  fields.add("v", static_cast<std::uint64_t>(1));
  fields.add("kind", "link");
  fields.add("name", declaration.name);
  fields.add("link-kind", to_string(declaration.kind));
  fields.add("linkgen", declaration.generation);
  fields.add("provenance", to_string(declaration.provenance));
  if (declaration.capacity_in_bps > 0U) {
    fields.add("capacity-in-bps", declaration.capacity_in_bps);
  }
  if (declaration.capacity_out_bps > 0U) {
    fields.add("capacity-out-bps", declaration.capacity_out_bps);
  }
  if (!declaration.local_endpoint.empty()) {
    fields.add("local-endpoint", declaration.local_endpoint);
  }
  if (!declaration.remote_endpoint.empty()) {
    fields.add("remote-endpoint", declaration.remote_endpoint);
  }
  if (!declaration.local_port.empty()) {
    fields.add("local-port", declaration.local_port);
  }
  if (!declaration.remote_port.empty()) {
    fields.add("remote-port", declaration.remote_port);
  }
  return fields.str();
}

std::string format_record(const ObservationDecl& declaration) {
  TextFields fields(' ');
  fields.add("v", static_cast<std::uint64_t>(1));
  fields.add("kind", "observation");
  fields.add("link", declaration.link);
  fields.add("source", declaration.source);
  fields.add("incarnation", declaration.incarnation);
  fields.add("epoch", declaration.epoch);
  fields.add("generation", declaration.generation);
  fields.add("revision", declaration.revision);
  fields.add("observed-at", declaration.observed_at);
  fields.add("received-at", declaration.received_at);
  fields.add("metric", declaration.key.to_string());
  if (const auto* admin = std::get_if<AdminStateReading>(&declaration.payload)) {
    fields.add("value", to_string(admin->value));
  } else if (const auto* oper = std::get_if<OperStateReading>(&declaration.payload)) {
    fields.add("value", to_string(oper->value));
  } else if (const auto* counter = std::get_if<CounterReading>(&declaration.payload)) {
    fields.add("counter", counter->raw);
    fields.add("width", to_string(counter->width));
    if (counter->reset_declared) {
      fields.add("reset", true);
    }
  } else if (const auto* ratio = std::get_if<RatioReading>(&declaration.payload)) {
    fields.add("ppb", ratio->ppb);
  } else if (const auto* quality = std::get_if<QualityReading>(&declaration.payload)) {
    fields.add("ppb", quality->value_ppb);
    if (quality->degraded_declared) {
      fields.add("degraded", true);
    }
  } else if (const auto* flap = std::get_if<FlapReading>(&declaration.payload)) {
    fields.add("events", flap->events);
    fields.add("window-ns", flap->window.nanos());
  }
  return fields.str();
}

std::size_t count_record_lines(std::string_view text) noexcept {
  std::size_t count = 0;
  std::size_t cursor = 0;
  while (cursor <= text.size()) {
    const std::size_t end = text.find('\n', cursor);
    const std::string_view line =
        (end == std::string_view::npos) ? text.substr(cursor) : text.substr(cursor, end - cursor);
    const std::string_view trimmed = trim_ascii(line);
    if (!trimmed.empty() && trimmed.front() != '#') {
      ++count;
    }
    if (end == std::string_view::npos) {
      break;
    }
    cursor = end + 1U;
  }
  return count;
}

}  // namespace linkobs
