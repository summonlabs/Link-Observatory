// Link Observatory - strongly typed, content addressed identities.
//
// Every important domain object is identified by a distinct type. Two
// identifiers of different kinds are not comparable, not convertible and not
// interchangeable, which turns "generation passed where an epoch was expected"
// into a compile error instead of a field bug.

#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "linkobs/core/checked.hpp"
#include "linkobs/core/hash128.hpp"
#include "linkobs/core/status.hpp"

namespace linkobs {

/// Tag types. Each one names a distinct identity kind and fixes its textual
/// prefix; the prefix is part of the persisted and printed representation.
struct EndpointTag {
  static constexpr std::string_view kKind = "endpoint";
};
struct PortTag {
  static constexpr std::string_view kKind = "port";
};
struct LinkTag {
  static constexpr std::string_view kKind = "link";
};
struct SourceTag {
  static constexpr std::string_view kKind = "source";
};
struct GenerationTag {
  static constexpr std::string_view kKind = "generation";
};
struct EpochTag {
  static constexpr std::string_view kKind = "epoch";
};
struct IncarnationTag {
  static constexpr std::string_view kKind = "incarnation";
};
struct ObservationTag {
  static constexpr std::string_view kKind = "observation";
};
struct EvidenceTag {
  static constexpr std::string_view kKind = "evidence";
};
struct RuntimeTag {
  static constexpr std::string_view kKind = "runtime";
};
struct TransitionTag {
  static constexpr std::string_view kKind = "transition";
};
struct SnapshotTag {
  static constexpr std::string_view kKind = "snapshot";
};

template <class Tag>
class StrongId {
 public:
  constexpr StrongId() noexcept = default;

  [[nodiscard]] static constexpr StrongId from_hash(Hash128 hash) noexcept {
    return StrongId{hash};
  }

  /// Derives an identity from a caller supplied name within this kind's
  /// namespace. The hash covers the kind, so identical names in different kinds
  /// never collide.
  [[nodiscard]] static StrongId from_name(std::string_view name) {
    HashBuilder builder;
    builder.add_str(Tag::kKind);
    builder.add_str(name);
    return StrongId{builder.finish()};
  }

  [[nodiscard]] static Result<StrongId> parse(std::string_view text) {
    const std::size_t separator = text.find('-');
    if (separator == std::string_view::npos) {
      return failure<StrongId>(StatusCode::InvalidArgument, "identifier has no kind prefix");
    }
    if (text.substr(0, separator) != Tag::kKind) {
      return failure<StrongId>(StatusCode::InvalidArgument, "identifier kind mismatch");
    }
    Hash128 hash{};
    if (!parse_hex128(text.substr(separator + 1U), hash)) {
      return failure<StrongId>(StatusCode::InvalidArgument,
                               "identifier body is not 32 hexadecimal digits");
    }
    return StrongId{hash};
  }

  [[nodiscard]] constexpr Hash128 hash() const noexcept { return hash_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return hash_.is_zero(); }
  [[nodiscard]] constexpr bool valid() const noexcept { return !hash_.is_zero(); }

  /// Canonical textual form: "<kind>-<32 lowercase hexadecimal digits>".
  [[nodiscard]] std::string to_string() const {
    std::string out{Tag::kKind};
    out.push_back('-');
    out.append(linkobs::to_hex(hash_));
    return out;
  }

  friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;
  friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;

 private:
  explicit constexpr StrongId(Hash128 hash) noexcept : hash_(hash) {}

  Hash128 hash_{};
};

using EndpointId = StrongId<EndpointTag>;
using PortId = StrongId<PortTag>;
using LinkId = StrongId<LinkTag>;
using SourceId = StrongId<SourceTag>;
using GenerationId = StrongId<GenerationTag>;
using EpochId = StrongId<EpochTag>;
using IncarnationId = StrongId<IncarnationTag>;
using ObservationId = StrongId<ObservationTag>;
using EvidenceId = StrongId<EvidenceTag>;
using RuntimeId = StrongId<RuntimeTag>;
using TransitionId = StrongId<TransitionTag>;
using SnapshotId = StrongId<SnapshotTag>;

/// Monotonic per-source revision counter.
///
/// A sequence number is scoped to (source, incarnation, epoch). The runtime
/// never compares sequence numbers across scopes and never invents values to
/// fill a gap.
class Sequence {
 public:
  constexpr Sequence() noexcept = default;
  explicit constexpr Sequence(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_initial() const noexcept { return value_ == 0U; }

  /// Successor, or an empty result when the counter would overflow.
  [[nodiscard]] constexpr Checked<Sequence> next() const noexcept {
    Checked<Sequence> result{};
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
      return result;
    }
    result.ok = true;
    result.value = Sequence{value_ + 1U};
    return result;
  }

  friend constexpr bool operator==(Sequence, Sequence) noexcept = default;
  friend constexpr auto operator<=>(Sequence, Sequence) noexcept = default;

 private:
  std::uint64_t value_{0};
};

}  // namespace linkobs
