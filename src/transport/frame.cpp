#include "linkobs/transport/frame.hpp"

#include <array>

#include "linkobs/version.hpp"

namespace linkobs {
namespace {

void append_u32(std::string& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

void append_u64(std::string& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
  }
}

[[nodiscard]] std::uint32_t read_u32(std::string_view data, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4U; ++index) {
    value |= (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + index]))
              << (8U * index));
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64(std::string_view data, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    value |= (static_cast<std::uint64_t>(static_cast<unsigned char>(data[offset + index]))
              << (8U * index));
  }
  return value;
}

}  // namespace

std::string_view to_string(FrameType type) noexcept {
  switch (type) {
    case FrameType::Ping:
      return "ping";
    case FrameType::Pong:
      return "pong";
    case FrameType::PushRecords:
      return "push-records";
    case FrameType::PushAck:
      return "push-ack";
    case FrameType::QueryState:
      return "query-state";
    case FrameType::StateReport:
      return "state-report";
    case FrameType::ErrorReport:
      return "error-report";
    case FrameType::Shutdown:
      return "shutdown";
  }
  return "invalid";
}

bool parse_frame_type(std::string_view text, FrameType& out) noexcept {
  static constexpr std::array<std::pair<std::string_view, FrameType>, 8> kNames{{
      {"ping", FrameType::Ping},
      {"pong", FrameType::Pong},
      {"push-records", FrameType::PushRecords},
      {"push-ack", FrameType::PushAck},
      {"query-state", FrameType::QueryState},
      {"state-report", FrameType::StateReport},
      {"error-report", FrameType::ErrorReport},
      {"shutdown", FrameType::Shutdown},
  }};
  for (const auto& entry : kNames) {
    if (entry.first == text) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

std::uint64_t frame_hash(std::string_view data) noexcept {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char raw : data) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

Status encode_frame(const Frame& frame, std::size_t max_payload, std::string& out) {
  if (frame.payload.size() > max_payload) {
    return Status::of(StatusCode::CapacityExceeded, "frame payload exceeds the configured bound");
  }
  out.clear();
  out.reserve(kFrameHeaderSize + frame.payload.size());
  append_u32(out, kFrameMagic);
  out.push_back(static_cast<char>(kTransportProtocolVersion));
  out.push_back(static_cast<char>(static_cast<std::uint8_t>(frame.type)));
  out.push_back(static_cast<char>(frame.flags & 0xFFU));
  out.push_back(static_cast<char>((frame.flags >> 8U) & 0xFFU));
  append_u32(out, static_cast<std::uint32_t>(frame.payload.size()));
  append_u64(out, frame_hash(frame.payload));
  out.append(frame.payload);
  return Status::success();
}

FrameDecodeResult decode_frame(std::string_view buffer, std::size_t max_payload) {
  FrameDecodeResult result{};
  if (buffer.size() < kFrameHeaderSize) {
    result.status = Status::success();
    result.complete = false;
    return result;
  }
  const std::uint32_t magic = read_u32(buffer, 0);
  if (magic != kFrameMagic) {
    result.status = Status::of(StatusCode::CorruptData, "frame magic does not match");
    return result;
  }
  const auto version = static_cast<std::uint8_t>(buffer[4]);
  if (version != static_cast<std::uint8_t>(kTransportProtocolVersion)) {
    result.status = Status::of(StatusCode::VersionMismatch, "frame protocol version mismatch");
    return result;
  }
  const auto type = static_cast<std::uint8_t>(buffer[5]);
  if (type < static_cast<std::uint8_t>(FrameType::Ping) ||
      type > static_cast<std::uint8_t>(FrameType::Shutdown)) {
    result.status = Status::of(StatusCode::Unsupported, "unknown frame type");
    return result;
  }
  const auto flags = static_cast<std::uint16_t>(
      static_cast<std::uint8_t>(buffer[6]) |
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(buffer[7])) << 8U));
  if (flags != 0U) {
    result.status = Status::of(StatusCode::Unsupported, "frame reserved flags are not zero");
    return result;
  }
  const std::uint32_t payload_size = read_u32(buffer, 8);
  if (payload_size > max_payload) {
    result.status = Status::of(StatusCode::CapacityExceeded,
                               "frame payload exceeds the configured bound");
    return result;
  }
  const std::uint64_t expected_hash = read_u64(buffer, 12);
  const std::size_t total = kFrameHeaderSize + static_cast<std::size_t>(payload_size);
  if (buffer.size() < total) {
    result.status = Status::success();
    result.complete = false;
    return result;
  }
  const std::string_view payload = buffer.substr(kFrameHeaderSize, payload_size);
  if (frame_hash(payload) != expected_hash) {
    result.status = Status::of(StatusCode::CorruptData, "frame payload hash mismatch");
    return result;
  }
  result.frame.type = static_cast<FrameType>(type);
  result.frame.flags = flags;
  result.frame.payload.assign(payload);
  result.consumed = total;
  result.complete = true;
  result.status = Status::success();
  return result;
}

}  // namespace linkobs
