// Link Observatory - wire framing.
//
// Every message is a length prefixed frame:
//
//   magic        u32  'LOF1' little endian
//   version      u8   protocol version
//   type         u8   frame type
//   flags        u16  reserved, must be zero
//   payload size u32  exact payload length
//   payload hash u64  FNV-1a 64 of the payload bytes
//   payload      payload size bytes
//
// A frame whose magic, version, reserved flags, length bound or payload hash is
// wrong is rejected. The decoder never resynchronises by guessing: a bad frame
// terminates the connection, because a desynchronised stream is not a stream the
// runtime can reason about.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "linkobs/core/status.hpp"

namespace linkobs {

enum class FrameType : std::uint8_t {
  Ping = 1,
  Pong = 2,
  PushRecords = 3,
  PushAck = 4,
  QueryState = 5,
  StateReport = 6,
  ErrorReport = 7,
  Shutdown = 8,
};

[[nodiscard]] std::string_view to_string(FrameType type) noexcept;
[[nodiscard]] bool parse_frame_type(std::string_view text, FrameType& out) noexcept;

inline constexpr std::size_t kFrameHeaderSize = 20U;
inline constexpr std::uint32_t kFrameMagic = 0x31464F4CU;  // 'L','O','F','1'

struct Frame {
  FrameType type{FrameType::Ping};
  std::uint16_t flags{0};
  std::string payload{};
};

/// Serialises a frame. Fails when the payload exceeds max_payload.
[[nodiscard]] Status encode_frame(const Frame& frame, std::size_t max_payload, std::string& out);

struct FrameDecodeResult {
  Status status{};
  bool complete{false};
  std::size_t consumed{0};
  Frame frame{};

  [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

/// Decodes one frame from the front of a buffer.
///
/// complete == false with an Ok status means "not enough bytes yet"; the caller
/// should read more and retry with the same buffer.
[[nodiscard]] FrameDecodeResult decode_frame(std::string_view buffer, std::size_t max_payload);

/// FNV-1a 64, used for the frame payload digest.
[[nodiscard]] std::uint64_t frame_hash(std::string_view data) noexcept;

}  // namespace linkobs
