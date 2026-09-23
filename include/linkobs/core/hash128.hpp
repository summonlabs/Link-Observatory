// Link Observatory - deterministic content hashing.
//
// All identifiers in the runtime are content addressed through a single,
// fully specified 128-bit hash. The hash must be:
//   * deterministic across processes, builds, platforms and standard libraries,
//   * independent of the C++ standard library implementation (no std::hash),
//   * stable across releases, because identifiers are persisted.
//
// Algorithm: FNV-1a, 128-bit variant, applied to a length-prefixed byte stream.
//   offset basis 0x6c62272e07bb014262b821756295c58d
//   prime        0x0000000001000000000000000000013b

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace linkobs {

/// A 128-bit content hash. Default constructed value is all zero ("no identity").
struct Hash128 {
  std::uint64_t hi{0};
  std::uint64_t lo{0};

  [[nodiscard]] constexpr bool is_zero() const noexcept { return hi == 0 && lo == 0; }

  friend constexpr bool operator==(const Hash128&, const Hash128&) noexcept = default;
  friend constexpr auto operator<=>(const Hash128&, const Hash128&) noexcept = default;
};

/// FNV-1a 128 over a contiguous byte range.
[[nodiscard]] Hash128 hash128_bytes(const void* data, std::size_t size) noexcept;

/// FNV-1a 128 over a string view.
[[nodiscard]] Hash128 hash128(std::string_view data) noexcept;

/// Continues an existing FNV-1a 128 state over more bytes.
[[nodiscard]] Hash128 hash128_extend(Hash128 seed, std::string_view data) noexcept;

/// Canonical textual form: exactly 32 lowercase hexadecimal characters.
[[nodiscard]] std::string to_hex(Hash128 value);
[[nodiscard]] std::string to_hex(const std::uint8_t* data, std::size_t size);

/// Strict parser: exactly 32 hexadecimal characters, case insensitive.
[[nodiscard]] bool parse_hex128(std::string_view text, Hash128& out) noexcept;

/// Incremental, unambiguous hashing of structured values.
///
/// Every field is length delimited so that no two distinct field sequences can
/// produce the same byte stream (no concatenation ambiguity). Integers are
/// encoded little endian, fixed width. Strings are encoded as an 8-byte length
/// followed by raw bytes.
class HashBuilder {
 public:
  HashBuilder() noexcept = default;

  void add_bytes(std::string_view bytes) noexcept;
  void add_raw(const void* data, std::size_t size) noexcept;
  void add_str(std::string_view text) noexcept;
  void add_u8(std::uint8_t value) noexcept;
  void add_bool(bool value) noexcept;
  void add_u32(std::uint32_t value) noexcept;
  void add_u64(std::uint64_t value) noexcept;
  void add_i64(std::int64_t value) noexcept;
  void add_hash(Hash128 value) noexcept;

  [[nodiscard]] Hash128 finish() const noexcept { return state_; }
  [[nodiscard]] const Hash128& state() const noexcept { return state_; }

 private:
  Hash128 state_{0x6c62272e07bb0142ULL, 0x62b821756295c58dULL};
};

}  // namespace linkobs
