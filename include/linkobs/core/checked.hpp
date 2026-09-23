// Link Observatory - checked arithmetic.
//
// Every size, count, offset, duration or rate that derives from external input
// (ingested records, snapshots, transport frames, command line arguments) is
// computed with the helpers below. Overflow is reported, never wrapped, and
// never allowed to fabricate a value.

#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace linkobs {

/// Result of a checked arithmetic operation.
template <class T>
struct Checked {
  bool ok{false};
  T value{};

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok; }
};

[[nodiscard]] constexpr Checked<std::uint64_t> checked_add(std::uint64_t lhs,
                                                           std::uint64_t rhs) noexcept {
  Checked<std::uint64_t> result{};
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    return result;
  }
  result.ok = true;
  result.value = lhs + rhs;
  return result;
}

[[nodiscard]] constexpr Checked<std::uint64_t> checked_sub(std::uint64_t lhs,
                                                           std::uint64_t rhs) noexcept {
  Checked<std::uint64_t> result{};
  if (rhs > lhs) {
    return result;
  }
  result.ok = true;
  result.value = lhs - rhs;
  return result;
}

[[nodiscard]] constexpr Checked<std::uint64_t> checked_mul(std::uint64_t lhs,
                                                           std::uint64_t rhs) noexcept {
  Checked<std::uint64_t> result{};
  if (lhs == 0U || rhs == 0U) {
    result.ok = true;
    result.value = 0U;
    return result;
  }
  if (lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
    return result;
  }
  result.ok = true;
  result.value = lhs * rhs;
  return result;
}

[[nodiscard]] constexpr Checked<std::uint64_t> checked_div(std::uint64_t lhs,
                                                           std::uint64_t rhs) noexcept {
  Checked<std::uint64_t> result{};
  if (rhs == 0U) {
    return result;
  }
  result.ok = true;
  result.value = lhs / rhs;
  return result;
}

[[nodiscard]] constexpr Checked<std::int64_t> checked_add(std::int64_t lhs,
                                                          std::int64_t rhs) noexcept {
  Checked<std::int64_t> result{};
  if (rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) {
    return result;
  }
  if (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs) {
    return result;
  }
  result.ok = true;
  result.value = lhs + rhs;
  return result;
}

[[nodiscard]] constexpr Checked<std::int64_t> checked_sub(std::int64_t lhs,
                                                          std::int64_t rhs) noexcept {
  Checked<std::int64_t> result{};
  if (rhs == std::numeric_limits<std::int64_t>::min()) {
    return result;
  }
  return checked_add(lhs, -rhs);
}

/// A minimal unsigned 128-bit value used for intermediate rate arithmetic.
struct U128 {
  std::uint64_t hi{0};
  std::uint64_t lo{0};

  [[nodiscard]] constexpr bool is_zero() const noexcept { return hi == 0U && lo == 0U; }
};

[[nodiscard]] constexpr int compare(U128 lhs, U128 rhs) noexcept {
  if (lhs.hi != rhs.hi) {
    return lhs.hi < rhs.hi ? -1 : 1;
  }
  if (lhs.lo != rhs.lo) {
    return lhs.lo < rhs.lo ? -1 : 1;
  }
  return 0;
}

[[nodiscard]] constexpr U128 u128_from_u64(std::uint64_t value) noexcept {
  return U128{0U, value};
}

[[nodiscard]] constexpr U128 u128_mul_u64(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  const std::uint64_t lhs_low = lhs & 0xFFFFFFFFULL;
  const std::uint64_t lhs_high = lhs >> 32U;
  const std::uint64_t rhs_low = rhs & 0xFFFFFFFFULL;
  const std::uint64_t rhs_high = rhs >> 32U;

  const std::uint64_t low_low = lhs_low * rhs_low;
  const std::uint64_t low_high = lhs_low * rhs_high;
  const std::uint64_t high_low = lhs_high * rhs_low;
  const std::uint64_t high_high = lhs_high * rhs_high;

  const std::uint64_t middle = low_high + (low_low >> 32U) + (high_low & 0xFFFFFFFFULL);
  const std::uint64_t result_lo = (middle << 32U) | (low_low & 0xFFFFFFFFULL);
  const std::uint64_t result_hi = high_high + (high_low >> 32U) + (middle >> 32U);
  return U128{result_hi, result_lo};
}

[[nodiscard]] constexpr Checked<std::uint64_t> u128_div_u64(U128 dividend,
                                                            std::uint64_t divisor) noexcept {
  Checked<std::uint64_t> result{};
  if (divisor == 0U) {
    return result;
  }
  std::uint64_t quotient = 0;
  std::uint64_t remainder = 0;
  for (int bit = 127; bit >= 0; --bit) {
    if (remainder > (std::numeric_limits<std::uint64_t>::max() >> 1U)) {
      return result;  // Would overflow: quotient cannot be represented.
    }
    remainder = (remainder << 1U) | ((bit >= 64) ? ((dividend.hi >> (bit - 64)) & 1U)
                                                 : ((dividend.lo >> bit) & 1U));
    if (remainder >= divisor) {
      remainder -= divisor;
      if (bit < 64) {
        quotient |= (1ULL << bit);
      } else {
        return result;  // Quotient needs more than 64 bits.
      }
    }
  }
  result.ok = true;
  result.value = quotient;
  return result;
}

[[nodiscard]] constexpr U128 u128_sub(U128 lhs, U128 rhs) noexcept {
  U128 result{};
  result.lo = lhs.lo - rhs.lo;
  const std::uint64_t borrow = (lhs.lo < rhs.lo) ? 1U : 0U;
  result.hi = lhs.hi - rhs.hi - borrow;
  return result;
}

[[nodiscard]] constexpr U128 u128_shl1(U128 value) noexcept {
  return U128{(value.hi << 1U) | (value.lo >> 63U), value.lo << 1U};
}

[[nodiscard]] constexpr bool u128_high_bit(U128 value) noexcept { return (value.hi >> 63U) != 0U; }

/// Divides a 128-bit numerator by a 128-bit denominator.
///
/// Fails when the divisor is zero or when the quotient does not fit into 64
/// bits. Used for utilization derivation, where both the traffic volume and the
/// capacity-times-window product can exceed 64 bits.
[[nodiscard]] constexpr Checked<std::uint64_t> u128_div_u128(U128 numerator,
                                                             U128 denominator) noexcept {
  Checked<std::uint64_t> result{};
  if (denominator.is_zero()) {
    return result;
  }
  U128 remainder{0U, 0U};
  std::uint64_t quotient = 0;
  for (int bit = 127; bit >= 0; --bit) {
    const bool carry = u128_high_bit(remainder);
    remainder = u128_shl1(remainder);
    if (bit >= 64) {
      remainder.lo |= ((numerator.hi >> (bit - 64)) & 1U);
    } else {
      remainder.lo |= ((numerator.lo >> bit) & 1U);
    }

    bool subtract = carry;
    if (!subtract && compare(remainder, denominator) >= 0) {
      subtract = true;
    }
    if (subtract) {
      remainder = u128_sub(remainder, denominator);
      if (bit >= 64) {
        return result;  // Quotient would not fit into 64 bits.
      }
      quotient |= (1ULL << bit);
    }
  }
  result.ok = true;
  result.value = quotient;
  return result;
}

/// Computes (lhs * rhs) / divisor with a 128-bit intermediate product.
[[nodiscard]] constexpr Checked<std::uint64_t> checked_mul_div(std::uint64_t lhs,
                                                               std::uint64_t rhs,
                                                               std::uint64_t divisor) noexcept {
  if (divisor == 0U) {
    return Checked<std::uint64_t>{};
  }
  return u128_div_u64(u128_mul_u64(lhs, rhs), divisor);
}

/// Widens a signed value that is known to be non-negative.
[[nodiscard]] constexpr Checked<std::uint64_t> to_unsigned(std::int64_t value) noexcept {
  Checked<std::uint64_t> result{};
  if (value < 0) {
    return result;
  }
  result.ok = true;
  result.value = static_cast<std::uint64_t>(value);
  return result;
}

/// Narrows an unsigned value that must fit into a signed 64-bit value.
[[nodiscard]] constexpr Checked<std::int64_t> to_signed(std::uint64_t value) noexcept {
  Checked<std::int64_t> result{};
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return result;
  }
  result.ok = true;
  result.value = static_cast<std::int64_t>(value);
  return result;
}

}  // namespace linkobs
