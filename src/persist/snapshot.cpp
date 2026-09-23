#include "linkobs/persist/snapshot.hpp"

#include <cstdio>
#include <fstream>
#include <string_view>

#include "linkobs/core/sha256.hpp"
#include "linkobs/core/text.hpp"
#include "linkobs/persist/codec.hpp"
#include "linkobs/version.hpp"

namespace linkobs {
namespace {

constexpr std::string_view kMagic = "LKOBSNAP";
constexpr std::uint32_t kMinimumReaderVersion = 1U;

[[nodiscard]] Status read_file(const std::string& path, std::size_t max_bytes, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) {
    return Status::of(StatusCode::NotFound, "snapshot file could not be opened");
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff length = stream.tellg();
  if (length < 0) {
    return Status::of(StatusCode::IoError, "snapshot file size could not be determined");
  }
  const auto size = static_cast<std::uint64_t>(length);
  if (size > static_cast<std::uint64_t>(max_bytes)) {
    return Status::of(StatusCode::CapacityExceeded, "snapshot exceeds the configured size bound");
  }
  stream.seekg(0, std::ios::beg);
  out.assign(static_cast<std::size_t>(size), '\0');
  if (size > 0U) {
    stream.read(out.data(), static_cast<std::streamsize>(size));
    if (!stream.good() && !stream.eof()) {
      return Status::of(StatusCode::IoError, "snapshot read failed");
    }
  }
  return Status::success();
}

[[nodiscard]] Status write_file(const std::string& path, std::string_view data) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.good()) {
    return Status::of(StatusCode::IoError, "snapshot file could not be created");
  }
  if (!data.empty()) {
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
  }
  stream.flush();
  if (!stream.good()) {
    return Status::of(StatusCode::IoError, "snapshot write failed");
  }
  return Status::success();
}

[[nodiscard]] bool remove_file(const std::string& path) {
  return std::remove(path.c_str()) == 0;
}

[[nodiscard]] Status rename_file(const std::string& from, const std::string& to) {
  if (std::rename(from.c_str(), to.c_str()) != 0) {
    return Status::of(StatusCode::IoError, "snapshot rename failed");
  }
  return Status::success();
}

[[nodiscard]] bool file_exists(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return stream.good();
}

struct Header {
  std::uint32_t format_version{0};
  std::uint32_t minimum_reader{0};
  std::uint64_t payload_size{0};
  Sha256Digest payload_digest{};
};

[[nodiscard]] std::string encode_header(const Header& header) {
  ByteWriter writer;
  writer.raw(kMagic.data(), kMagic.size());
  writer.u32(header.format_version);
  writer.u32(header.minimum_reader);
  writer.u64(header.payload_size);
  writer.raw(header.payload_digest.data(), header.payload_digest.size());
  const Sha256Digest digest = sha256(writer.data());
  writer.raw(digest.data(), digest.size());
  return writer.data();
}

}  // namespace

std::string snapshot_backup_path(const std::string& path) { return path + ".prev"; }

std::string snapshot_temporary_path(const std::string& path) { return path + ".tmp"; }

SnapshotWriteResult write_snapshot(const std::string& path,
                                   const SnapshotContents& contents,
                                   const Policy& policy) {
  SnapshotWriteResult result{};

  ByteWriter payload;
  payload.u32(kSnapshotFormatVersion);
  encode(payload, contents.runtime);
  encode(payload, contents.incarnation);
  encode(payload, contents.saved_at);
  payload.u64(contents.acceptance_index);
  payload.count(contents.links.size());
  for (const PersistedLink& link : contents.links) {
    encode(payload, link);
  }
  payload.count(contents.sources.size());
  for (const SourceRecord& source : contents.sources) {
    encode(payload, source);
  }
  payload.count(contents.slots.size());
  for (const PersistedSlot& slot : contents.slots) {
    encode(payload, slot);
  }
  payload.count(contents.streams.size());
  for (const auto& stream : contents.streams) {
    encode(payload, stream.first);
    encode(payload, stream.second);
  }
  payload.count(contents.transitions.size());
  for (const TransitionEvent& event : contents.transitions) {
    encode(payload, event);
  }
  payload.u64(contents.total_accepted);
  payload.u64(contents.total_fenced);
  payload.u64(contents.duplicates);
  payload.u64(contents.revision_gaps);
  payload.u64(contents.counter_resets);
  payload.u64(contents.counter_wraps);
  payload.u64(contents.transitions_dropped);

  if (payload.size() > policy.limits.max_snapshot_bytes) {
    result.status = Status::of(StatusCode::CapacityExceeded,
                               "encoded snapshot exceeds the configured size bound");
    return result;
  }

  Header header{};
  header.format_version = kSnapshotFormatVersion;
  header.minimum_reader = kMinimumReaderVersion;
  header.payload_size = static_cast<std::uint64_t>(payload.size());
  header.payload_digest = sha256(payload.data());

  std::string container = encode_header(header);
  container.append(payload.data());

  const std::string temporary = snapshot_temporary_path(path);
  const Status written = write_file(temporary, container);
  if (!written.ok()) {
    result.status = written;
    return result;
  }

  const std::string backup = snapshot_backup_path(path);
  if (file_exists(path)) {
    // Best effort: a missing or unremovable backup must not fail the write, and
    // the rename below replaces it atomically anyway.
    const bool removed = remove_file(backup);
    (void)removed;
    const Status rotated = rename_file(path, backup);
    if (!rotated.ok()) {
      const bool cleaned = remove_file(temporary);
      (void)cleaned;
      result.status = rotated;
      return result;
    }
  }
  const Status moved = rename_file(temporary, path);
  if (!moved.ok()) {
    result.status = moved;
    return result;
  }

  result.status = Status::success();
  result.bytes_written = container.size();
  result.payload_digest = to_hex(header.payload_digest);
  return result;
}

namespace {

[[nodiscard]] SnapshotLoadResult load_once(const std::string& path, const Policy& policy) {
  SnapshotLoadResult result{};

  std::string raw;
  const std::size_t max_container =
      policy.limits.max_snapshot_bytes + 128U;
  const Status read = read_file(path, max_container, raw);
  if (!read.ok()) {
    result.status = read;
    result.detail = read.to_string();
    return result;
  }
  result.bytes_read = raw.size();
  if (raw.size() < 88U) {
    result.status = Status::of(StatusCode::CorruptData, "snapshot is shorter than its header");
    result.detail = result.status.to_string();
    return result;
  }
  if (std::string_view{raw.data(), kMagic.size()} != kMagic) {
    result.status = Status::of(StatusCode::CorruptData, "snapshot magic does not match");
    result.detail = result.status.to_string();
    return result;
  }

  ByteReader header_reader{std::string_view{raw.data(), 88U}};
  std::array<std::uint8_t, 8> magic{};
  if (!header_reader.raw(magic.data(), magic.size())) {
    result.status = Status::of(StatusCode::CorruptData, "snapshot header is truncated");
    result.detail = result.status.to_string();
    return result;
  }
  Header header{};
  if (!header_reader.u32(header.format_version) ||
      !header_reader.u32(header.minimum_reader) ||
      !header_reader.u64(header.payload_size) ||
      !header_reader.raw(header.payload_digest.data(), header.payload_digest.size())) {
    result.status = Status::of(StatusCode::CorruptData, "snapshot header fields are truncated");
    result.detail = result.status.to_string();
    return result;
  }
  Sha256Digest stored_header_digest{};
  if (!header_reader.raw(stored_header_digest.data(), stored_header_digest.size())) {
    result.status = Status::of(StatusCode::CorruptData, "snapshot header digest is truncated");
    result.detail = result.status.to_string();
    return result;
  }
  const Sha256Digest computed_header_digest =
      sha256(std::string_view{raw.data(), 56U});
  if (computed_header_digest != stored_header_digest) {
    result.status = Status::of(StatusCode::CorruptData, "snapshot header digest mismatch");
    result.detail = result.status.to_string();
    return result;
  }
  if (header.format_version != kSnapshotFormatVersion) {
    result.status = Status::of(StatusCode::VersionMismatch,
                               "snapshot format version is not supported by this build");
    result.detail = result.status.to_string();
    return result;
  }
  if (header.minimum_reader > kSnapshotFormatVersion) {
    result.status = Status::of(StatusCode::VersionMismatch,
                               "snapshot requires a newer reader than this build");
    result.detail = result.status.to_string();
    return result;
  }
  if (header.payload_size > static_cast<std::uint64_t>(policy.limits.max_snapshot_bytes)) {
    result.status = Status::of(StatusCode::CapacityExceeded,
                               "snapshot payload exceeds the configured size bound");
    result.detail = result.status.to_string();
    return result;
  }
  if (header.payload_size != static_cast<std::uint64_t>(raw.size() - 88U)) {
    result.status = Status::of(StatusCode::CorruptData, "snapshot payload length mismatch");
    result.detail = result.status.to_string();
    return result;
  }

  const std::string_view payload_view{raw.data() + 88U,
                                      static_cast<std::size_t>(header.payload_size)};
  const Sha256Digest computed_payload_digest = sha256(payload_view);
  if (computed_payload_digest != header.payload_digest) {
    result.status = Status::of(StatusCode::CorruptData, "snapshot payload digest mismatch");
    result.detail = result.status.to_string();
    return result;
  }
  result.integrity_verified = true;

  ByteReader reader{payload_view};
  SnapshotContents& contents = result.contents;
  std::uint32_t inner_version = 0;
  std::size_t links = 0;
  std::size_t sources = 0;
  std::size_t slots = 0;
  std::size_t streams = 0;
  std::size_t transitions = 0;

  // Each collection length is read immediately before its records, exactly as
  // the writer emits them: length, records, length, records, ...
  const bool header_ok = reader.u32(inner_version) &&
                         inner_version == kSnapshotFormatVersion &&
                         decode(reader, contents.runtime) &&
                         decode(reader, contents.incarnation) &&
                         decode(reader, contents.saved_at) &&
                         reader.u64(contents.acceptance_index) &&
                         reader.count(links, policy.limits.max_links);
  if (!header_ok) {
    result.status = reader.status().ok()
                        ? Status::of(StatusCode::CorruptData, "snapshot payload header is invalid")
                        : reader.status();
    result.detail = result.status.to_string();
    result.integrity_verified = false;
    return result;
  }

  contents.format_version = inner_version;
  contents.links.clear();
  for (std::size_t index = 0; index < links; ++index) {
    PersistedLink link{};
    if (!decode(reader, link)) {
      result.status = Status::of(StatusCode::CorruptData, "snapshot link record is invalid");
      result.detail = result.status.to_string();
      return result;
    }
    contents.links.push_back(std::move(link));
  }
  if (!reader.count(sources, policy.limits.max_sources)) {
    result.status = reader.status();
    result.detail = result.status.to_string();
    return result;
  }
  contents.sources.clear();
  for (std::size_t index = 0; index < sources; ++index) {
    SourceRecord source{};
    if (!decode(reader, source)) {
      result.status = Status::of(StatusCode::CorruptData, "snapshot source record is invalid");
      result.detail = result.status.to_string();
      return result;
    }
    contents.sources.push_back(std::move(source));
  }
  if (!reader.count(slots, policy.limits.max_links * kMetricFamilyCount * kDirectionCount *
                                kErrorClassCount)) {
    result.status = reader.status();
    result.detail = result.status.to_string();
    return result;
  }
  contents.slots.clear();
  for (std::size_t index = 0; index < slots; ++index) {
    PersistedSlot slot{};
    if (!decode(reader, slot)) {
      result.status = Status::of(StatusCode::CorruptData, "snapshot evidence slot is invalid");
      result.detail = result.status.to_string();
      return result;
    }
    contents.slots.push_back(std::move(slot));
  }
  if (!reader.count(streams, policy.limits.max_sources)) {
    result.status = reader.status();
    result.detail = result.status.to_string();
    return result;
  }
  contents.streams.clear();
  for (std::size_t index = 0; index < streams; ++index) {
    SourceId source{};
    StreamState state{};
    if (!decode(reader, source) || !decode(reader, state)) {
      result.status = Status::of(StatusCode::CorruptData, "snapshot stream record is invalid");
      result.detail = result.status.to_string();
      return result;
    }
    contents.streams.emplace_back(source, state);
  }
  if (!reader.count(transitions, policy.limits.max_total_transitions)) {
    result.status = reader.status();
    result.detail = result.status.to_string();
    return result;
  }
  contents.transitions.clear();
  for (std::size_t index = 0; index < transitions; ++index) {
    TransitionEvent event{};
    if (!decode(reader, event)) {
      result.status = Status::of(StatusCode::CorruptData, "snapshot transition record is invalid");
      result.detail = result.status.to_string();
      return result;
    }
    contents.transitions.push_back(std::move(event));
  }

  const bool totals_ok =
      reader.u64(contents.total_accepted) && reader.u64(contents.total_fenced) &&
      reader.u64(contents.duplicates) && reader.u64(contents.revision_gaps) &&
      reader.u64(contents.counter_resets) && reader.u64(contents.counter_wraps) &&
      reader.u64(contents.transitions_dropped);
  if (!totals_ok) {
    result.status = Status::of(StatusCode::CorruptData, "snapshot totals are truncated");
    result.detail = result.status.to_string();
    return result;
  }
  if (!reader.exhausted()) {
    result.status = Status::of(StatusCode::CorruptData, "snapshot has trailing bytes");
    result.detail = result.status.to_string();
    return result;
  }

  result.ok = true;
  result.status = Status::success();
  return result;
}

}  // namespace

SnapshotLoadResult read_snapshot(const std::string& path, const Policy& policy) {
  SnapshotLoadResult primary = load_once(path, policy);
  if (primary.ok) {
    return primary;
  }

  const std::string backup = snapshot_backup_path(path);
  if (backup == path || !file_exists(backup)) {
    return primary;
  }

  SnapshotLoadResult fallback = load_once(backup, policy);
  if (!fallback.ok) {
    // Both copies failed: report the primary failure, and note the fallback.
    primary.detail.append("; backup also unusable: ");
    primary.detail.append(fallback.status.to_string());
    return primary;
  }
  fallback.primary_failed = true;
  fallback.recovered_from_backup = true;
  fallback.detail = std::string{"primary snapshot unusable ("} + primary.status.to_string() +
                    "); recovered from backup";
  return fallback;
}

bool snapshot_exists(const std::string& path) { return file_exists(path); }

}  // namespace linkobs
