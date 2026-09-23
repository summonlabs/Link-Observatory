// Link Observatory - SHA-256 for stored-evidence integrity.
//
// Content identity uses the 128-bit FNV-1a hash in hash128.hpp (fast, stable,
// sufficient for addressing). Stored evidence is additionally protected by a
// cryptographic digest so that truncation, bit rot and deliberate mutation of a
// snapshot are all detected. SHA-256 is implemented here in full so the runtime
// has no external dependency and no platform crypto provider to trust.
//
// Limitation, stated explicitly: a digest detects modification. It does not
// authenticate the writer. Snapshots are not signed.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace linkobs {

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256 {
 public:
  Sha256() noexcept = default;

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }

  /// Finalises the digest. The object must not be updated afterwards.
  [[nodiscard]] Sha256Digest finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                     0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_{0};
  std::size_t buffered_{0};
};

[[nodiscard]] Sha256Digest sha256(std::string_view text) noexcept;
[[nodiscard]] Sha256Digest sha256(const void* data, std::size_t size) noexcept;
[[nodiscard]] std::string to_hex(const Sha256Digest& digest);
[[nodiscard]] bool parse_sha256_hex(std::string_view text, Sha256Digest& out) noexcept;

}  // namespace linkobs
