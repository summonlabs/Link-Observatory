// Link Observatory - versioned, integrity checked persistence.
//
// Layout (all integers little endian):
//
//   magic             8 bytes  "LKOBSNAP"
//   format version    u32      container format understood by the writer
//   minimum reader    u32      oldest reader that may load this payload
//   payload size      u64      exact payload length in bytes
//   payload sha256    32 bytes digest of the payload
//   header sha256     32 bytes digest of the preceding 56 bytes
//   payload           payload size bytes of codec output
//
// Reading is conservative in every direction:
//   * a wrong magic, a wrong header digest, a short file, a wrong length, a
//     wrong payload digest or an unknown format version all fail closed,
//   * the writer never overwrites the last good snapshot in place: it writes a
//     temporary file, then rotates the previous file to "<path>.prev", then
//     moves the temporary into place,
//   * the reader falls back to "<path>.prev" only when the primary file fails
//     integrity, and reports that it did so,
//   * nothing is ever partially applied: a snapshot is either fully decoded or
//     not applied at all.
//
// Time is not restored. Freshness is always recomputed against the running
// clock, so a snapshot can never make old evidence current.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "linkobs/core/status.hpp"
#include "linkobs/domain/link.hpp"
#include "linkobs/domain/observation.hpp"
#include "linkobs/store/evidence_store.hpp"
#include "linkobs/store/history.hpp"
#include "linkobs/store/link_registry.hpp"

namespace linkobs {

struct SnapshotContents {
  std::uint32_t format_version{0};
  RuntimeId runtime{};
  IncarnationId incarnation{};
  TimePoint saved_at{};
  std::uint64_t acceptance_index{0};

  std::vector<PersistedLink> links{};
  std::vector<SourceRecord> sources{};
  std::vector<PersistedSlot> slots{};
  std::vector<std::pair<SourceId, StreamState>> streams{};
  std::vector<TransitionEvent> transitions{};

  std::uint64_t total_accepted{0};
  std::uint64_t total_fenced{0};
  std::uint64_t duplicates{0};
  std::uint64_t revision_gaps{0};
  std::uint64_t counter_resets{0};
  std::uint64_t counter_wraps{0};
  std::uint64_t transitions_dropped{0};
};

[[nodiscard]] std::string snapshot_backup_path(const std::string& path);
[[nodiscard]] std::string snapshot_temporary_path(const std::string& path);

struct SnapshotWriteResult {
  Status status{};
  std::size_t bytes_written{0};
  std::string payload_digest{};
};

/// Writes atomically: temporary file, rotate previous, rename into place.
[[nodiscard]] SnapshotWriteResult write_snapshot(const std::string& path,
                                                 const SnapshotContents& contents,
                                                 const Policy& policy);

struct SnapshotLoadResult {
  bool ok{false};
  Status status{};
  bool recovered_from_backup{false};
  bool integrity_verified{false};
  bool primary_failed{false};
  std::string detail{};
  std::size_t bytes_read{0};
  SnapshotContents contents{};
};

/// Reads and fully verifies a snapshot. Never returns partially decoded data.
[[nodiscard]] SnapshotLoadResult read_snapshot(const std::string& path, const Policy& policy);

/// True when a well formed snapshot exists at the path.
[[nodiscard]] bool snapshot_exists(const std::string& path);

}  // namespace linkobs
