#include "linkobs/domain/metric.hpp"

#include <limits>

#include "linkobs/core/checked.hpp"
#include "linkobs/core/text.hpp"

namespace linkobs {

std::string_view to_string(AdminStateValue value) noexcept {
  switch (value) {
    case AdminStateValue::Unknown:
      return "unknown";
    case AdminStateValue::Enabled:
      return "enabled";
    case AdminStateValue::Disabled:
      return "disabled";
  }
  return "invalid";
}

std::string_view to_string(OperStateValue value) noexcept {
  switch (value) {
    case OperStateValue::Unknown:
      return "unknown";
    case OperStateValue::Up:
      return "up";
    case OperStateValue::Down:
      return "down";
    case OperStateValue::Degraded:
      return "degraded";
  }
  return "invalid";
}

bool parse_admin_state(std::string_view text, AdminStateValue& out) noexcept {
  if (text == "unknown") {
    out = AdminStateValue::Unknown;
    return true;
  }
  if (text == "enabled" || text == "up") {
    out = AdminStateValue::Enabled;
    return true;
  }
  if (text == "disabled" || text == "down") {
    out = AdminStateValue::Disabled;
    return true;
  }
  return false;
}

bool parse_oper_state(std::string_view text, OperStateValue& out) noexcept {
  if (text == "unknown") {
    out = OperStateValue::Unknown;
    return true;
  }
  if (text == "up") {
    out = OperStateValue::Up;
    return true;
  }
  if (text == "down") {
    out = OperStateValue::Down;
    return true;
  }
  if (text == "degraded") {
    out = OperStateValue::Degraded;
    return true;
  }
  return false;
}

std::string MetricKey::to_string() const {
  std::string out{linkobs::to_string(family)};
  if (is_directional_family(family) && direction != Direction::Unknown) {
    out.push_back('/');
    out.append(linkobs::to_string(direction));
  }
  if (family == MetricFamily::Errors && error_class != ErrorClass::None) {
    out.push_back('/');
    out.append(linkobs::to_string(error_class));
  }
  if (family == MetricFamily::Discards && error_class != ErrorClass::None) {
    out.push_back('/');
    out.append(linkobs::to_string(error_class));
  }
  return out;
}

Result<MetricKey> MetricKey::parse(std::string_view text) {
  MetricKey key{};
  std::size_t cursor = 0;
  std::size_t part = 0;
  while (cursor <= text.size()) {
    const std::size_t next = text.find('/', cursor);
    const std::string_view token =
        (next == std::string_view::npos) ? text.substr(cursor) : text.substr(cursor, next - cursor);
    if (token.empty()) {
      return failure<MetricKey>(StatusCode::InvalidArgument, "empty metric key component");
    }
    if (part == 0U) {
      if (!parse_metric_family(token, key.family)) {
        return failure<MetricKey>(StatusCode::Unsupported, "unknown metric family");
      }
    } else if (part == 1U) {
      if (!parse_direction(token, key.direction)) {
        return failure<MetricKey>(StatusCode::Unsupported, "unknown metric direction");
      }
    } else if (part == 2U) {
      if (!parse_error_class(token, key.error_class)) {
        return failure<MetricKey>(StatusCode::Unsupported, "unknown error class");
      }
    } else {
      return failure<MetricKey>(StatusCode::InvalidArgument, "metric key has too many components");
    }
    ++part;
    if (next == std::string_view::npos) {
      break;
    }
    cursor = next + 1U;
  }
  if (part == 0U) {
    return failure<MetricKey>(StatusCode::InvalidArgument, "empty metric key");
  }
  return key;
}

bool payload_matches_family(MetricFamily family, const MetricPayload& payload) noexcept {
  switch (family) {
    case MetricFamily::AdminState:
      return std::holds_alternative<AdminStateReading>(payload);
    case MetricFamily::OperState:
      return std::holds_alternative<OperStateReading>(payload);
    case MetricFamily::Octets:
    case MetricFamily::Errors:
    case MetricFamily::Discards:
      return std::holds_alternative<CounterReading>(payload);
    case MetricFamily::UtilReported:
      return std::holds_alternative<RatioReading>(payload);
    case MetricFamily::SignalQuality:
      return std::holds_alternative<QualityReading>(payload);
    case MetricFamily::FlapReport:
      return std::holds_alternative<FlapReading>(payload);
  }
  return false;
}

namespace {

[[nodiscard]] Status reject(std::string_view message) {
  return Status::of(StatusCode::InvalidArgument, message);
}

[[nodiscard]] bool error_class_allowed(MetricFamily family, ErrorClass value) noexcept {
  if (family == MetricFamily::Errors) {
    return true;
  }
  if (family == MetricFamily::Discards) {
    return value == ErrorClass::None || value == ErrorClass::Total ||
           value == ErrorClass::Congestion || value == ErrorClass::Other;
  }
  return value == ErrorClass::None;
}

}  // namespace

Status validate_reading(const MetricKey& key, const MetricPayload& payload, const Policy& policy) {
  if (!payload_matches_family(key.family, payload)) {
    return Status::of(StatusCode::Unsupported,
                      "reading alternative does not belong to the declared metric family");
  }
  if (!error_class_allowed(key.family, key.error_class)) {
    return reject("error class is not defined for this metric family");
  }
  if (key.direction != Direction::Unknown && !is_directional_family(key.family)) {
    return reject("direction is not defined for this metric family");
  }

  if (const auto* admin = std::get_if<AdminStateReading>(&payload)) {
    (void)admin;
    return Status::success();
  }
  if (const auto* oper = std::get_if<OperStateReading>(&payload)) {
    (void)oper;
    return Status::success();
  }
  if (const auto* counter = std::get_if<CounterReading>(&payload)) {
    if (counter->width == CounterWidth::Bits32 &&
        counter->raw > std::numeric_limits<std::uint32_t>::max()) {
      return Status::of(StatusCode::OutOfRange,
                        "32-bit counter sample exceeds the declared counter width");
    }
    return Status::success();
  }
  if (const auto* ratio = std::get_if<RatioReading>(&payload)) {
    if (ratio->ppb > kPpbAcceptedMax) {
      return Status::of(StatusCode::OutOfRange, "reported ratio exceeds the accepted maximum");
    }
    return Status::success();
  }
  if (const auto* quality = std::get_if<QualityReading>(&payload)) {
    if (quality->value_ppb > kPpbFullScale) {
      return Status::of(StatusCode::OutOfRange, "quality indicator exceeds full scale");
    }
    if (key.direction == Direction::Both) {
      return reject("quality indicator is not defined for a bidirectional reading");
    }
    return Status::success();
  }
  if (const auto* flap = std::get_if<FlapReading>(&payload)) {
    if (flap->window.nanos() <= 0) {
      return reject("flap window must be positive");
    }
    if (flap->window.nanos() > policy.thresholds.max_history_window.nanos()) {
      return Status::of(StatusCode::OutOfRange, "flap window exceeds the maximum history window");
    }
    return Status::success();
  }
  return Status::of(StatusCode::Unsupported, "unrecognised reading alternative");
}

std::string format_payload(const MetricPayload& payload) {
  if (const auto* admin = std::get_if<AdminStateReading>(&payload)) {
    return std::string{to_string(admin->value)};
  }
  if (const auto* oper = std::get_if<OperStateReading>(&payload)) {
    return std::string{to_string(oper->value)};
  }
  if (const auto* counter = std::get_if<CounterReading>(&payload)) {
    std::string out{"raw="};
    out.append(to_dec(counter->raw));
    out.append(" width=");
    out.append(to_string(counter->width));
    out.append(" declared-reset=");
    out.append(counter->reset_declared ? "true" : "false");
    return out;
  }
  if (const auto* ratio = std::get_if<RatioReading>(&payload)) {
    return std::string{"ppb="} + to_dec(ratio->ppb);
  }
  if (const auto* quality = std::get_if<QualityReading>(&payload)) {
    return std::string{"ppb="} + to_dec(quality->value_ppb) +
           " declared-degraded=" + (quality->degraded_declared ? "true" : "false");
  }
  if (const auto* flap = std::get_if<FlapReading>(&payload)) {
    return std::string{"events="} + to_dec(flap->events) +
           " window-ns=" + to_dec(flap->window.nanos());
  }
  return std::string{"none"};
}

bool payloads_are_comparable(const MetricPayload& lhs, const MetricPayload& rhs) noexcept {
  return lhs.index() == rhs.index();
}

bool payload_difference_ppb(const MetricPayload& lhs,
                            const MetricPayload& rhs,
                            std::uint64_t& out) noexcept {
  if (const auto* left = std::get_if<RatioReading>(&lhs)) {
    const auto* right = std::get_if<RatioReading>(&rhs);
    if (right == nullptr) {
      return false;
    }
    const std::uint64_t low = left->ppb < right->ppb ? left->ppb : right->ppb;
    const std::uint64_t high = left->ppb < right->ppb ? right->ppb : left->ppb;
    out = high - low;
    return true;
  }
  if (const auto* left = std::get_if<QualityReading>(&lhs)) {
    const auto* right = std::get_if<QualityReading>(&rhs);
    if (right == nullptr) {
      return false;
    }
    const std::uint64_t low = left->value_ppb < right->value_ppb ? left->value_ppb : right->value_ppb;
    const std::uint64_t high =
        left->value_ppb < right->value_ppb ? right->value_ppb : left->value_ppb;
    out = high - low;
    return true;
  }
  return false;
}

}  // namespace linkobs
