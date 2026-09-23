#include "linkobs/core/hash128.hpp"

#include <array>

namespace linkobs {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_nibble(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return (character - 'a') + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return (character - 'A') + 10;
  }
  return -1;
}

}  // namespace

std::string to_hex(const std::uint8_t* data, std::size_t size) {
  std::string out;
  out.reserve(size * 2U);
  for (std::size_t index = 0; index < size; ++index) {
    const std::uint8_t byte = data[index];
    out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[byte & 0x0FU]);
  }
  return out;
}

std::string to_hex(Hash128 value) {
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>((value.hi >> (8U * (7U - index))) & 0xFFU);
    bytes[8 + index] = static_cast<std::uint8_t>((value.lo >> (8U * (7U - index))) & 0xFFU);
  }
  return to_hex(bytes.data(), bytes.size());
}

bool parse_hex128(std::string_view text, Hash128& out) noexcept {
  if (text.size() != 32U) {
    return false;
  }
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  for (std::size_t index = 0; index < 32U; ++index) {
    const int nibble = hex_nibble(text[index]);
    if (nibble < 0) {
      return false;
    }
    if (index < 16U) {
      hi = (hi << 4U) | static_cast<std::uint64_t>(nibble);
    } else {
      lo = (lo << 4U) | static_cast<std::uint64_t>(nibble);
    }
  }
  out.hi = hi;
  out.lo = lo;
  return true;
}

}  // namespace linkobs
