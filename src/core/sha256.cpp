#include "linkobs/core/sha256.hpp"

#include "linkobs/core/hash128.hpp"

namespace linkobs {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept {
  return (value >> bits) | (value << (32U - bits));
}

[[nodiscard]] constexpr std::uint32_t load_be32(const std::uint8_t* data) noexcept {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

void store_be32(std::uint8_t* data, std::uint32_t value) noexcept {
  data[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
  data[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  data[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  data[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

}  // namespace

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16U; ++index) {
    schedule[index] = load_be32(block + (index * 4U));
  }
  for (std::size_t index = 16U; index < 64U; ++index) {
    const std::uint32_t s0 = rotate_right(schedule[index - 15U], 7U) ^
                             rotate_right(schedule[index - 15U], 18U) ^
                             (schedule[index - 15U] >> 3U);
    const std::uint32_t s1 = rotate_right(schedule[index - 2U], 17U) ^
                             rotate_right(schedule[index - 2U], 19U) ^
                             (schedule[index - 2U] >> 10U);
    schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64U; ++index) {
    const std::uint32_t sigma1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t sigma0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += static_cast<std::uint64_t>(size);
  std::size_t offset = 0;
  while (offset < size && buffered_ < buffer_.size()) {
    buffer_[buffered_] = bytes[offset];
    ++buffered_;
    ++offset;
  }
  if (buffered_ == buffer_.size()) {
    compress(buffer_.data());
    buffered_ = 0;
  }
  while ((size - offset) >= buffer_.size()) {
    compress(bytes + offset);
    offset += buffer_.size();
  }
  while (offset < size) {
    buffer_[buffered_] = bytes[offset];
    ++buffered_;
    ++offset;
  }
}

Sha256Digest Sha256::finish() noexcept {
  const std::uint64_t total_bits = total_bytes_ * 8U;
  std::array<std::uint8_t, 8> length_bytes{};
  for (std::size_t index = 0; index < length_bytes.size(); ++index) {
    length_bytes[index] = static_cast<std::uint8_t>((total_bits >> (8U * (7U - index))) & 0xFFU);
  }

  const std::uint8_t pad = 0x80U;
  update(&pad, 1U);
  const std::uint8_t zero = 0x00U;
  while (buffered_ != 56U) {
    update(&zero, 1U);
  }
  update(length_bytes.data(), length_bytes.size());

  Sha256Digest digest{};
  for (std::size_t index = 0; index < state_.size(); ++index) {
    store_be32(digest.data() + (index * 4U), state_[index]);
  }
  return digest;
}

Sha256Digest sha256(const void* data, std::size_t size) noexcept {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

Sha256Digest sha256(std::string_view text) noexcept { return sha256(text.data(), text.size()); }

std::string to_hex(const Sha256Digest& digest) { return to_hex(digest.data(), digest.size()); }

bool parse_sha256_hex(std::string_view text, Sha256Digest& out) noexcept {
  if (text.size() != 64U) {
    return false;
  }
  const auto nibble = [](char character) -> int {
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
  };
  for (std::size_t index = 0; index < 32U; ++index) {
    const int high = nibble(text[index * 2U]);
    const int low = nibble(text[(index * 2U) + 1U]);
    if (high < 0 || low < 0) {
      return false;
    }
    out[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

}  // namespace linkobs
