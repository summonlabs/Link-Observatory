#include "linkobs/core/text.hpp"

#include <limits>

namespace linkobs {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

[[nodiscard]] bool is_digit(char character) noexcept {
  return character >= '0' && character <= '9';
}

}  // namespace

std::string to_dec(std::uint64_t value) {
  if (value == 0U) {
    return std::string{"0"};
  }
  char buffer[20]{};
  std::size_t index = 0;
  while (value != 0U) {
    buffer[index] = static_cast<char>('0' + static_cast<char>(value % 10U));
    value /= 10U;
    ++index;
  }
  std::string out;
  out.reserve(index);
  while (index > 0U) {
    --index;
    out.push_back(buffer[index]);
  }
  return out;
}

std::string to_dec(std::int64_t value) {
  if (value >= 0) {
    return to_dec(static_cast<std::uint64_t>(value));
  }
  const std::uint64_t magnitude =
      static_cast<std::uint64_t>(-(value + 1)) + 1U;  // Safe for INT64_MIN.
  std::string out{"-"};
  out.append(to_dec(magnitude));
  return out;
}

std::string to_hex_u64(std::uint64_t value) {
  if (value == 0U) {
    return std::string{"0"};
  }
  char buffer[16]{};
  std::size_t index = 0;
  while (value != 0U) {
    buffer[index] = kHexDigits[static_cast<std::size_t>(value & 0x0FU)];
    value >>= 4U;
    ++index;
  }
  std::string out;
  out.reserve(index);
  while (index > 0U) {
    --index;
    out.push_back(buffer[index]);
  }
  return out;
}

bool parse_u64_dec(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 20U) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (!is_digit(character)) {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      return false;
    }
    value = (value * 10U) + digit;
  }
  out = value;
  return true;
}

bool parse_u32_dec(std::string_view text, std::uint32_t& out) noexcept {
  std::uint64_t wide = 0;
  if (!parse_u64_dec(text, wide) || wide > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  out = static_cast<std::uint32_t>(wide);
  return true;
}

bool parse_i64_dec(std::string_view text, std::int64_t& out) noexcept {
  if (text.empty()) {
    return false;
  }
  bool negative = false;
  if (text.front() == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  std::uint64_t magnitude = 0;
  if (!parse_u64_dec(text, magnitude)) {
    return false;
  }
  const std::uint64_t limit = negative
                                  ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U
                                  : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (magnitude > limit) {
    return false;
  }
  if (negative) {
    out = (magnitude == limit) ? std::numeric_limits<std::int64_t>::min()
                               : -static_cast<std::int64_t>(magnitude);
  } else {
    out = static_cast<std::int64_t>(magnitude);
  }
  return true;
}

bool parse_u64_hex(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 16U) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    std::uint64_t digit = 0;
    if (character >= '0' && character <= '9') {
      digit = static_cast<std::uint64_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digit = static_cast<std::uint64_t>(character - 'a') + 10U;
    } else if (character >= 'A' && character <= 'F') {
      digit = static_cast<std::uint64_t>(character - 'A') + 10U;
    } else {
      return false;
    }
    if (value > (std::numeric_limits<std::uint64_t>::max() >> 4U)) {
      return false;
    }
    value = (value << 4U) | digit;
  }
  out = value;
  return true;
}

bool is_ascii_printable(std::string_view text) noexcept {
  for (const char character : text) {
    const auto raw = static_cast<unsigned char>(character);
    if (raw < 0x20U || raw > 0x7EU) {
      return false;
    }
  }
  return true;
}

std::string_view trim_ascii(std::string_view text) noexcept {
  std::size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::vector<std::string_view> split_bounded(std::string_view text,
                                            char separator,
                                            std::size_t max_fields) {
  std::vector<std::string_view> fields;
  if (max_fields == 0U) {
    return fields;
  }
  std::size_t start = 0;
  while (true) {
    if (fields.size() + 1U == max_fields) {
      fields.push_back(text.substr(start));
      return fields;
    }
    const std::size_t position = text.find(separator, start);
    if (position == std::string_view::npos) {
      fields.push_back(text.substr(start));
      return fields;
    }
    fields.push_back(text.substr(start, position - start));
    start = position + 1U;
  }
}

bool is_token(std::string_view text) noexcept {
  if (text.empty() || text.size() > 128U) {
    return false;
  }
  for (const char character : text) {
    const bool allowed = (character >= 'a' && character <= 'z') ||
                         (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9') || character == '_' ||
                         character == '.' || character == ':' || character == '-';
    if (!allowed) {
      return false;
    }
  }
  return true;
}

void TextFields::begin_field() {
  if (has_field_) {
    buffer_.push_back(separator_);
  }
  has_field_ = true;
}

void TextFields::add(std::string_view key, std::string_view value) {
  begin_field();
  buffer_.append(key);
  buffer_.push_back('=');
  buffer_.append(value);
}

void TextFields::add(std::string_view key, const char* value) {
  add(key, std::string_view{value});
}

void TextFields::add(std::string_view key, const std::string& value) {
  add(key, std::string_view{value});
}

void TextFields::add(std::string_view key, std::uint64_t value) {
  begin_field();
  buffer_.append(key);
  buffer_.push_back('=');
  buffer_.append(to_dec(value));
}

void TextFields::add(std::string_view key, std::int64_t value) {
  begin_field();
  buffer_.append(key);
  buffer_.push_back('=');
  buffer_.append(to_dec(value));
}

void TextFields::add(std::string_view key, bool value) {
  add(key, std::string_view{value ? "true" : "false"});
}

}  // namespace linkobs
