#include "linkobs/store/counter_tracker.hpp"

#include <limits>

#include "linkobs/core/checked.hpp"

namespace linkobs {
namespace {

[[nodiscard]] constexpr std::uint64_t counter_range(CounterWidth width) noexcept {
  return width == CounterWidth::Bits32 ? 0xFFFFFFFFULL : std::numeric_limits<std::uint64_t>::max();
}

[[nodiscard]] bool same_identity(const CounterSample& lhs, const CounterSample& rhs) noexcept {
  return lhs.scope == rhs.scope && lhs.generation == rhs.generation;
}

}  // namespace

std::uint64_t wrap_detection_threshold(CounterWidth width) noexcept {
  if (width == CounterWidth::Bits32) {
    return 3221225472ULL;  // three quarters of 2^32
  }
  return 13835058055282163712ULL;  // three quarters of 2^64
}

ContinuityOutcome observe_counter(CounterContinuity& state, const CounterSample& sample) noexcept {
  ContinuityOutcome outcome{};

  if (!state.has_latest) {
    outcome.first_sample = true;
  } else {
    const CounterSample& baseline = state.latest;
    if (!same_identity(baseline, sample)) {
      outcome.scope_changed = baseline.scope != sample.scope;
      outcome.generation_changed = baseline.generation != sample.generation;
    } else {
      const bool regressed = sample.sequence.value() <= baseline.sequence.value();
      const bool discontinuity = sample.gap_epoch != baseline.gap_epoch;
      if (regressed || discontinuity) {
        outcome.sequence_gap = true;
        state.gap_count += 1U;
      }
      if (sample.reading.reset_declared) {
        outcome.discontinuity = CounterDiscontinuity::Reset;
      } else if (sample.reading.raw >= baseline.reading.raw) {
        outcome.delta = sample.reading.raw - baseline.reading.raw;
        outcome.delta_valid = !outcome.sequence_gap;
      } else if (baseline.reading.raw >= wrap_detection_threshold(baseline.reading.width)) {
        const std::uint64_t range = counter_range(baseline.reading.width);
        const std::uint64_t head = (range - baseline.reading.raw) + 1U;
        const Checked<std::uint64_t> total = checked_add(head, sample.reading.raw);
        if (total.ok) {
          outcome.delta = total.value;
          outcome.delta_valid = !outcome.sequence_gap;
          outcome.discontinuity = CounterDiscontinuity::Wrap;
        } else {
          outcome.discontinuity = CounterDiscontinuity::Reset;
        }
      } else {
        outcome.discontinuity = CounterDiscontinuity::Reset;
      }
    }
  }

  if (outcome.discontinuity == CounterDiscontinuity::Reset) {
    state.reset_count += 1U;
  } else if (outcome.discontinuity == CounterDiscontinuity::Wrap) {
    state.wrap_count += 1U;
  }
  state.last_discontinuity = outcome.discontinuity;

  if (state.has_latest) {
    state.previous = state.latest;
    state.has_previous = true;
  }
  state.latest = sample;
  state.has_latest = true;
  return outcome;
}

Freshness classify_sample_freshness(const CounterSample& sample,
                                     TimePoint now,
                                     const FreshnessPolicy& policy) noexcept {
  const Checked<Duration> observed = difference(now, sample.observed_at);
  const Checked<Duration> received = difference(now, sample.received_at);
  Nanos effective = 0;
  if (observed.ok && !observed.value.is_negative()) {
    effective = observed.value.nanos();
  }
  if (received.ok && !received.value.is_negative() && received.value.nanos() > effective) {
    effective = received.value.nanos();
  }
  if (effective <= policy.fresh_within.nanos()) {
    return Freshness::Fresh;
  }
  if (effective <= policy.usable_within.nanos()) {
    return Freshness::Stale;
  }
  return Freshness::Expired;
}

DeltaResult derive_delta(const DerivationRequest& request) noexcept {
  DeltaResult result{};

  if (!request.has_previous) {
    result.fault = DerivationFault::MissingPreviousSample;
    return result;
  }
  if (!request.previous_usable) {
    result.fault = DerivationFault::StaleSample;
    return result;
  }
  if (!request.current_usable) {
    result.fault = DerivationFault::StaleSample;
    return result;
  }
  if (!same_identity(request.previous, request.current)) {
    result.fault = DerivationFault::FencedSample;
    return result;
  }

  if (request.current.sequence.value() <= request.previous.sequence.value() ||
      request.current.gap_epoch != request.previous.gap_epoch) {
    result.fault = DerivationFault::RevisionGap;
    return result;
  }

  const Checked<Duration> span = difference(request.current.observed_at,
                                            request.previous.observed_at);
  if (!span.ok) {
    result.fault = DerivationFault::Overflow;
    return result;
  }
  if (span.value.is_negative() || span.value.is_zero()) {
    result.fault = DerivationFault::NonMonotonicTime;
    return result;
  }
  if (span.value.nanos() > request.max_window.nanos()) {
    result.fault = DerivationFault::WindowTooLarge;
    return result;
  }
  result.window = span.value;

  std::uint64_t delta = 0;
  if (request.current.reading.reset_declared) {
    result.discontinuity = CounterDiscontinuity::Reset;
    result.fault = DerivationFault::CounterReset;
    return result;
  }
  if (request.current.reading.raw >= request.previous.reading.raw) {
    delta = request.current.reading.raw - request.previous.reading.raw;
  } else if (request.previous.reading.raw >=
             wrap_detection_threshold(request.previous.reading.width)) {
    const std::uint64_t range = counter_range(request.previous.reading.width);
    const std::uint64_t head = (range - request.previous.reading.raw) + 1U;
    const Checked<std::uint64_t> total = checked_add(head, request.current.reading.raw);
    if (!total.ok) {
      result.fault = DerivationFault::Overflow;
      return result;
    }
    delta = total.value;
    result.discontinuity = CounterDiscontinuity::Wrap;
  } else {
    result.discontinuity = CounterDiscontinuity::Reset;
    result.fault = DerivationFault::CounterReset;
    return result;
  }

  result.delta = delta;
  result.valid = true;
  return result;
}

DerivationResult derive_utilization(const DerivationRequest& request) noexcept {
  DerivationResult result{};

  if (request.capacity_bps == 0U) {
    result.fault = DerivationFault::MissingCapacity;
    return result;
  }

  const DeltaResult delta_result = derive_delta(request);
  if (!delta_result.valid) {
    result.fault = delta_result.fault;
    result.discontinuity = delta_result.discontinuity;
    result.window = delta_result.window;
    return result;
  }

  const Checked<std::uint64_t> bits = checked_mul(delta_result.delta, 8U);
  if (!bits.ok) {
    result.fault = DerivationFault::Overflow;
    return result;
  }
  result.delta_bytes = delta_result.delta;
  result.window = delta_result.window;
  result.discontinuity = delta_result.discontinuity;

  const auto window_ns = static_cast<std::uint64_t>(delta_result.window.nanos());
  const Checked<std::uint64_t> rate = checked_mul_div(bits.value, 1000000000ULL, window_ns);
  if (!rate.ok) {
    result.fault = DerivationFault::Overflow;
    return result;
  }
  result.bits_per_second = rate.value;

  // utilization_ppb = bits * 1e18 / (capacity_bps * window_ns)
  const U128 numerator = u128_mul_u64(bits.value, 1000000000000000000ULL);
  const U128 denominator = u128_mul_u64(request.capacity_bps, window_ns);
  if (denominator.is_zero()) {
    result.fault = DerivationFault::MissingCapacity;
    return result;
  }
  const Checked<std::uint64_t> utilization = u128_div_u128(numerator, denominator);
  if (!utilization.ok) {
    result.fault = DerivationFault::Overflow;
    return result;
  }

  result.utilization_ppb = utilization.value;
  result.valid = true;
  return result;
}

bool ratio_ppb(std::uint64_t numerator, std::uint64_t denominator, std::uint64_t& out) noexcept {
  if (denominator == 0U) {
    return false;
  }
  if (numerator == 0U) {
    out = 0U;
    return true;
  }
  const U128 product = u128_mul_u64(numerator, 1000000000ULL);
  const Checked<std::uint64_t> quotient = u128_div_u64(product, denominator);
  if (!quotient.ok) {
    return false;
  }
  out = quotient.value;
  return true;
}

}  // namespace linkobs
