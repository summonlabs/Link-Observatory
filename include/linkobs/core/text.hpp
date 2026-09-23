// Link Observatory - deterministic text primitives.
//
// The runtime never formats numbers through locale-aware or floating point
// machinery: every textual representation is produced here so that byte-for-byte
// equality of reports and explanations is a testable property.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace linkobs {

/// Locale independent unsigned decimal formatting.
[[nodiscard]] std::string to_dec(std::uint64_t value);
/// Locale independent signed decimal formatting.
[[nodiscard]] std::string to_dec(std::int64_t value);

/// Every other integral width routes to one of the two functions above. Without
/// this, a std::size_t argument is ambiguous on a 32-bit target, where size_t is
/// neither of the two exact types.
template <class T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, std::uint64_t> &&
           !std::is_same_v<T, std::int64_t>)
[[nodiscard]] std::string to_dec(T value) {
  if constexpr (std::is_signed_v<T>) {
    return to_dec(static_cast<std::int64_t>(value));
  } else {
    return to_dec(static_cast<std::uint64_t>(value));
  }
}
[[nodiscard]] std::string to_hex_u64(std::uint64_t value);

/// Strict parsers. They reject empty input, signs (unless noted), whitespace,
/// trailing characters and any value outside the destination range.
[[nodiscard]] bool parse_u64_dec(std::string_view text, std::uint64_t& out) noexcept;
[[nodiscard]] bool parse_i64_dec(std::string_view text, std::int64_t& out) noexcept;
[[nodiscard]] bool parse_u32_dec(std::string_view text, std::uint32_t& out) noexcept;
[[nodiscard]] bool parse_u64_hex(std::string_view text, std::uint64_t& out) noexcept;

[[nodiscard]] bool is_ascii_printable(std::string_view text) noexcept;
[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept;

/// Splits on a single character. Empty fields are preserved. The number of
/// produced fields is bounded by max_fields; when the bound is reached the
/// remainder of the input is placed in the final field so that no data is lost
/// silently.
[[nodiscard]] std::vector<std::string_view> split_bounded(std::string_view text,
                                                          char separator,
                                                          std::size_t max_fields);

/// True when the text is a syntactically valid token for identifiers, metric
/// names and enum spellings: [A-Za-z0-9_.:-] and non-empty.
[[nodiscard]] bool is_token(std::string_view text) noexcept;

/// Appends "key=value" style fields with a separator, in caller-defined order.
class TextFields {
 public:
  explicit TextFields(char separator) noexcept : separator_(separator) {}

  void add(std::string_view key, std::string_view value);
  /// Without this overload a string literal would bind to the bool overload,
  /// silently rendering "key=1" instead of "key=value".
  void add(std::string_view key, const char* value);
  void add(std::string_view key, const std::string& value);
  void add(std::string_view key, std::uint64_t value);
  void add(std::string_view key, std::int64_t value);
  void add(std::string_view key, bool value);

  [[nodiscard]] const std::string& str() const noexcept { return buffer_; }
  [[nodiscard]] std::string take() { return std::move(buffer_); }
  [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }

 private:
  void begin_field();

  char separator_;
  bool has_field_{false};
  std::string buffer_{};
};

}  // namespace linkobs
