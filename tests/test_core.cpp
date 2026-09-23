// Core primitives: hashing, identity, checked arithmetic, text codecs, clocks,
// and the lock order auditor.

#include <algorithm>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "support/harness.hpp"

#include "linkobs/core/checked.hpp"
#include "linkobs/core/hash128.hpp"
#include "linkobs/core/lock_tracker.hpp"
#include "linkobs/core/sha256.hpp"
#include "linkobs/core/strong_id.hpp"
#include "linkobs/core/text.hpp"
#include "linkobs/time/clock.hpp"

namespace {

using namespace linkobs;
using namespace linkobs::test;

LO_TEST(core, sha256_matches_published_vectors) {
  // Independent verification: the published SHA-256 digests of "", "abc" and the
  // 56 byte NIST sample string.
  LO_CHECK_EQ(to_hex(sha256(std::string_view{})),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  LO_CHECK_EQ(to_hex(sha256(std::string_view{"abc"})),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  LO_CHECK_EQ(
      to_hex(sha256(std::string_view{
          "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"})),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

LO_TEST(core, sha256_is_streaming_and_position_independent) {
  const std::string text = "the quick brown fox jumps over the lazy dog";
  const Sha256Digest whole = sha256(text);
  Sha256 streaming;
  for (std::size_t index = 0; index < text.size(); ++index) {
    streaming.update(text.data() + index, 1U);
  }
  LO_CHECK(streaming.finish() == whole);

  Sha256 two_part;
  two_part.update(std::string_view{text}.substr(0, 11));
  two_part.update(std::string_view{text}.substr(11));
  LO_CHECK(two_part.finish() == whole);
}

LO_TEST(core, hash_is_stable_and_order_sensitive) {
  LO_CHECK(hash128("") == hash128(""));
  LO_CHECK(!(hash128("a") == hash128("b")));
  // The 128-bit multiply must not lose the high limb for long inputs.
  const std::string long_input(4096, 'x');
  LO_CHECK(!(hash128(long_input) == hash128(std::string(4096, 'y'))));
  LO_CHECK(!(hash128(long_input).hi == 0U && hash128(long_input).lo == 0U));
}

LO_TEST(core, hash_builder_is_unambiguous) {
  HashBuilder first;
  first.add_str("ab");
  first.add_str("c");
  HashBuilder second;
  second.add_str("a");
  second.add_str("bc");
  LO_CHECK(!(first.finish() == second.finish()));

  HashBuilder integers_first;
  integers_first.add_u32(1U);
  integers_first.add_u32(2U);
  HashBuilder integers_second;
  integers_second.add_u32(2U);
  integers_second.add_u32(1U);
  LO_CHECK(!(integers_first.finish() == integers_second.finish()));
}

LO_TEST(core, hash_hex_round_trip) {
  const Hash128 value = hash128("round-trip");
  const std::string text = to_hex(value);
  LO_CHECK_EQ(text.size(), static_cast<std::size_t>(32));
  Hash128 parsed{};
  LO_CHECK(parse_hex128(text, parsed));
  LO_CHECK(parsed == value);
  LO_CHECK(!parse_hex128(text.substr(0, 31), parsed));
  LO_CHECK(!parse_hex128("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", parsed));
}

LO_TEST(core, checked_arithmetic_reports_overflow) {
  const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  LO_CHECK(checked_add(maximum, 1U).ok == false);
  LO_CHECK(checked_add(maximum - 1U, 1U).value == maximum);
  LO_CHECK(checked_sub(std::uint64_t{0}, std::uint64_t{1}).ok == false);
  LO_CHECK(checked_mul(maximum, 2U).ok == false);
  LO_CHECK(checked_div(4U, 0U).ok == false);
  LO_CHECK(checked_div(4U, 2U).value == 2U);
}

LO_TEST(core, u128_division_is_exact) {
  const U128 product = u128_mul_u64(1000000000000ULL, 1000000000000ULL);
  const Checked<std::uint64_t> quotient = u128_div_u64(product, 1000000000000ULL);
  LO_REQUIRE(quotient.ok);
  LO_CHECK_EQ(quotient.value, 1000000000000ULL);

  // 2^64 * 3 / 2^63 == 6
  const U128 big = u128_mul_u64(0xFFFFFFFFFFFFFFFFULL, 8U);
  const Checked<std::uint64_t> half = u128_div_u128(big, U128{0U, 0x8000000000000000ULL});
  LO_REQUIRE(half.ok);
  LO_CHECK_EQ(half.value, 15ULL);
}

LO_TEST(core, u128_division_at_the_128_bit_boundary) {
  // A denominator above 2^127 exercises the 129-bit intermediate path.
  const U128 numerator{0x4000000000000000ULL, 0U};
  const U128 denominator{0x8000000000000000ULL, 0U};
  const Checked<std::uint64_t> quotient = u128_div_u128(numerator, denominator);
  LO_REQUIRE(quotient.ok);
  LO_CHECK_EQ(quotient.value, 0ULL);

  const U128 equal{0x8000000000000000ULL, 0U};
  const Checked<std::uint64_t> one = u128_div_u128(equal, denominator);
  LO_REQUIRE(one.ok);
  LO_CHECK_EQ(one.value, 1ULL);
}

LO_TEST(core, text_parsers_are_strict) {
  std::uint64_t value = 0;
  LO_CHECK(parse_u64_dec("0", value) && value == 0U);
  LO_CHECK(parse_u64_dec("18446744073709551615", value));
  LO_CHECK(!parse_u64_dec("18446744073709551616", value));
  LO_CHECK(!parse_u64_dec("-1", value));
  LO_CHECK(!parse_u64_dec("1 ", value));
  LO_CHECK(!parse_u64_dec("", value));
  LO_CHECK(!parse_u64_dec("0x10", value));

  std::int64_t signed_value = 0;
  LO_CHECK(parse_i64_dec("-9223372036854775808", signed_value));
  LO_CHECK(signed_value == std::numeric_limits<std::int64_t>::min());
  LO_CHECK(!parse_i64_dec("9223372036854775808", signed_value));
}

LO_TEST(core, decimal_formatting_is_locale_independent) {
  LO_CHECK_EQ(to_dec(static_cast<std::uint64_t>(0)), std::string("0"));
  LO_CHECK_EQ(to_dec(static_cast<std::uint64_t>(18446744073709551615ULL)),
              std::string("18446744073709551615"));
  LO_CHECK_EQ(to_dec(std::numeric_limits<std::int64_t>::min()),
              std::string("-9223372036854775808"));
}

LO_TEST(core, bounded_split_never_loses_input) {
  const std::vector<std::string_view> fields = split_bounded("a,b,c,d", ',', 3U);
  LO_CHECK_EQ(fields.size(), static_cast<std::size_t>(3));
  LO_CHECK_EQ(std::string(fields[2]), std::string("c,d"));
  const std::vector<std::string_view> empty = split_bounded("a", ',', 0U);
  LO_CHECK(empty.empty());
}

LO_TEST(core, manual_clock_is_explicit) {
  ManualClock clock{TimePoint::from_nanos(100)};
  LO_CHECK_EQ(clock.now().nanos(), static_cast<Nanos>(100));
  clock.advance_by(Duration::from_nanos(50));
  LO_CHECK_EQ(clock.now().nanos(), static_cast<Nanos>(150));
  const Checked<Duration> backwards = difference(TimePoint::from_nanos(10),
                                                 TimePoint::from_nanos(20));
  LO_REQUIRE(backwards.ok);
  LO_CHECK(backwards.value.is_negative());
  const Checked<TimePoint> overflow =
      advance(TimePoint::from_nanos(std::numeric_limits<Nanos>::max()), Duration::from_nanos(1));
  LO_CHECK(!overflow.ok);
}

LO_TEST(core, lock_tracker_stays_silent_when_used_correctly) {
  reset_lock_tracking();
  {
    TrackedLock outer{LockClass::ObservatoryState};
    std::lock_guard<TrackedLock> guard(outer);
    TrackedLock inner{LockClass::Diagnostics};
    std::lock_guard<TrackedLock> nested(inner);
  }
  LO_CHECK_EQ(lock_violation_count(), static_cast<std::uint64_t>(0));
}

LO_TEST(core, lock_tracker_detects_reentrancy_and_inversion) {
  reset_lock_tracking();
  {
    LockScope outer{LockClass::Diagnostics};
    LockScope inner{LockClass::ObservatoryState};
    (void)inner;
  }
  LO_CHECK(lock_violation_count() >= 1U);
  reset_lock_tracking();
  {
    LockScope first{LockClass::IngestQueue};
    LockScope second{LockClass::IngestQueue};
    (void)second;
  }
  LO_CHECK(lock_violation_count() >= 1U);
  reset_lock_tracking();
  LO_CHECK_EQ(lock_violation_count(), static_cast<std::uint64_t>(0));
  LO_CHECK(lock_violation_reports().empty());
}

}  // namespace
