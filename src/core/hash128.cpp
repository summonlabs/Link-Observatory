#include "linkobs/core/hash128.hpp"

#include <array>

namespace linkobs {
namespace {

/// The FNV-1a 128-bit prime is 2^88 + 0x13B, so it is stored as its low limb.
constexpr std::uint64_t kFnvPrimeLow = 0x000000000000013BULL;
constexpr std::uint64_t kFnvOffsetHi = 0x6c62272e07bb0142ULL;
constexpr std::uint64_t kFnvOffsetLo = 0x62b821756295c58dULL;

struct Mul128 {
  std::uint64_t hi{0};
  std::uint64_t lo{0};
};

/// Full 64x64 -> 128 bit unsigned multiply built from 32-bit limbs only.
[[nodiscard]] constexpr Mul128 mul_u64_full(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  const std::uint64_t lhs_low = lhs & 0xFFFFFFFFULL;
  const std::uint64_t lhs_high = lhs >> 32U;
  const std::uint64_t rhs_low = rhs & 0xFFFFFFFFULL;
  const std::uint64_t rhs_high = rhs >> 32U;

  const std::uint64_t partial = lhs_low * rhs_low;
  const std::uint64_t middle = (lhs_high * rhs_low) + (partial >> 32U);
  std::uint64_t folded = middle & 0xFFFFFFFFULL;
  const std::uint64_t middle_high = middle >> 32U;
  folded += lhs_low * rhs_high;

  Mul128 result{};
  result.hi = (lhs_high * rhs_high) + middle_high + (folded >> 32U);
  result.lo = (folded << 32U) | (partial & 0xFFFFFFFFULL);
  return result;
}

/// state = state * prime, modulo 2^128, using 64-bit limbs only.
void fnv_multiply(std::uint64_t& hi, std::uint64_t& lo) noexcept {
  const Mul128 low_product = mul_u64_full(lo, kFnvPrimeLow);
  const std::uint64_t high_addend = mul_u64_full(hi, kFnvPrimeLow).lo;
  const std::uint64_t product_hi = low_product.hi + high_addend;

  const std::uint64_t shifted_lo = lo << 24U;
  const std::uint64_t shifted_hi = lo >> 40U;

  const std::uint64_t sum_lo = shifted_lo + low_product.lo;
  const std::uint64_t carry = (sum_lo < shifted_lo) ? 1U : 0U;

  lo = sum_lo;
  hi = shifted_hi + product_hi + carry;
}

}  // namespace

Hash128 hash128_bytes(const void* data, std::size_t size) noexcept {
  Hash128 state{kFnvOffsetHi, kFnvOffsetLo};
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t index = 0; index < size; ++index) {
    state.lo ^= static_cast<std::uint64_t>(bytes[index]);
    fnv_multiply(state.hi, state.lo);
  }
  return state;
}

Hash128 hash128(std::string_view data) noexcept {
  return hash128_bytes(data.data(), data.size());
}

Hash128 hash128_extend(Hash128 seed, std::string_view data) noexcept {
  Hash128 state = seed;
  for (const char raw : data) {
    state.lo ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
    fnv_multiply(state.hi, state.lo);
  }
  return state;
}

void HashBuilder::add_raw(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t index = 0; index < size; ++index) {
    state_.lo ^= static_cast<std::uint64_t>(bytes[index]);
    fnv_multiply(state_.hi, state_.lo);
  }
}

void HashBuilder::add_bytes(std::string_view bytes) noexcept {
  add_u64(static_cast<std::uint64_t>(bytes.size()));
  add_raw(bytes.data(), bytes.size());
}

void HashBuilder::add_str(std::string_view text) noexcept { add_bytes(text); }

void HashBuilder::add_u8(std::uint8_t value) noexcept {
  state_.lo ^= static_cast<std::uint64_t>(value);
  fnv_multiply(state_.hi, state_.lo);
}

void HashBuilder::add_bool(bool value) noexcept { add_u8(value ? 1U : 0U); }

void HashBuilder::add_u32(std::uint32_t value) noexcept {
  std::array<std::uint8_t, 4> buffer{};
  for (std::size_t index = 0; index < buffer.size(); ++index) {
    buffer[index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
  }
  add_raw(buffer.data(), buffer.size());
}

void HashBuilder::add_u64(std::uint64_t value) noexcept {
  std::array<std::uint8_t, 8> buffer{};
  for (std::size_t index = 0; index < buffer.size(); ++index) {
    buffer[index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
  }
  add_raw(buffer.data(), buffer.size());
}

void HashBuilder::add_i64(std::int64_t value) noexcept {
  add_u64(static_cast<std::uint64_t>(value));
}

void HashBuilder::add_hash(Hash128 value) noexcept {
  add_u64(value.hi);
  add_u64(value.lo);
}

}  // namespace linkobs
