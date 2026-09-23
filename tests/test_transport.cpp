// Transport: framing, corruption handling and a real loopback round trip.

#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/transport/client.hpp"
#include "linkobs/transport/frame.hpp"
#include "linkobs/transport/server.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using namespace linkobs;
using namespace linkobs::test;

/// Opens a raw TCP connection, sends the bytes produced by the builder, and
/// reports whether the peer closed the connection afterwards. This is the only
/// way to send a frame the runtime's own encoder would never produce.
[[nodiscard]] bool send_raw_and_expect_close(
    std::uint16_t port,
    const std::function<void(std::string&)>& builder) {
#ifdef _WIN32
  WSADATA data{};
  if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return false;
  }
  const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_SOCKET) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  const bool connected =
      ::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
#else
  const int socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket < 0) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  const bool connected =
      ::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
#endif
  if (!connected) {
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
    return false;
  }

  std::string bytes;
  builder(bytes);
  std::size_t sent = 0;
  while (sent < bytes.size()) {
#ifdef _WIN32
    const int written = ::send(socket, bytes.data() + sent,
                               static_cast<int>(bytes.size() - sent), 0);
#else
    const ssize_t written = ::send(socket, bytes.data() + sent, bytes.size() - sent, 0);
#endif
    if (written <= 0) {
      break;
    }
    sent += static_cast<std::size_t>(written);
  }

  // The peer may reply with an error frame before closing, so read until the
  // connection is closed. The iteration bound is a liveness guard only.
  char buffer[256];
  bool closed = false;
  for (int attempt = 0; attempt < 1024; ++attempt) {
#ifdef _WIN32
    const int received = ::recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
    const ssize_t received = ::recv(socket, buffer, sizeof(buffer), 0);
#endif
    if (received == 0) {
      closed = true;
      break;
    }
    if (received < 0) {
      break;
    }
  }
#ifdef _WIN32
  ::closesocket(socket);
#else
  ::close(socket);
#endif
  return closed;
}

LO_TEST(transport, frames_round_trip) {
  Frame frame{};
  frame.type = FrameType::PushRecords;
  frame.payload = "v=1 kind=source name=s authority=primary provenance=real\n";

  std::string encoded;
  LO_REQUIRE(encode_frame(frame, 1024U, encoded).ok());
  LO_CHECK_EQ(encoded.size(), kFrameHeaderSize + frame.payload.size());

  // A frame is decoded only when it is complete.
  const FrameDecodeResult partial = decode_frame(std::string_view{encoded}.substr(0, 4U), 1024U);
  LO_CHECK(partial.ok());
  LO_CHECK(!partial.complete);

  const FrameDecodeResult decoded = decode_frame(encoded, 1024U);
  LO_REQUIRE(decoded.ok());
  LO_CHECK(decoded.complete);
  LO_CHECK_EQ(decoded.consumed, encoded.size());
  LO_CHECK_EQ(decoded.frame.type, FrameType::PushRecords);
  LO_CHECK_EQ(decoded.frame.payload, frame.payload);
}

LO_TEST(transport, every_header_field_is_checked) {
  Frame frame{};
  frame.type = FrameType::Ping;
  frame.payload = "payload";
  std::string encoded;
  LO_REQUIRE(encode_frame(frame, 1024U, encoded).ok());

  std::string bad_magic = encoded;
  bad_magic[0] = static_cast<char>(bad_magic[0] ^ 0x01);
  LO_CHECK_EQ(decode_frame(bad_magic, 1024U).status.code(), StatusCode::CorruptData);

  std::string bad_version = encoded;
  bad_version[4] = static_cast<char>(99);
  LO_CHECK_EQ(decode_frame(bad_version, 1024U).status.code(), StatusCode::VersionMismatch);

  std::string bad_type = encoded;
  bad_type[5] = static_cast<char>(200);
  LO_CHECK_EQ(decode_frame(bad_type, 1024U).status.code(), StatusCode::Unsupported);

  std::string bad_flags = encoded;
  bad_flags[6] = static_cast<char>(1);
  LO_CHECK_EQ(decode_frame(bad_flags, 1024U).status.code(), StatusCode::Unsupported);

  std::string bad_hash = encoded;
  bad_hash[kFrameHeaderSize] = static_cast<char>(bad_hash[kFrameHeaderSize] ^ 0xFF);
  LO_CHECK_EQ(decode_frame(bad_hash, 1024U).status.code(), StatusCode::CorruptData);

  // A declared payload larger than the bound is refused before allocating.
  Frame huge{};
  huge.type = FrameType::PushRecords;
  huge.payload.assign(4096U, 'x');
  std::string oversized;
  LO_CHECK(!encode_frame(huge, 128U, oversized).ok());

  // The same check must hold for a hand built header, which is what a hostile
  // peer would send.
  std::string forged;
  const std::uint32_t magic = 0x31464F4CU;
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    forged.push_back(static_cast<char>((magic >> shift) & 0xFFU));
  }
  forged.push_back(static_cast<char>(1));
  forged.push_back(static_cast<char>(static_cast<std::uint8_t>(FrameType::PushRecords)));
  forged.push_back(static_cast<char>(0));
  forged.push_back(static_cast<char>(0));
  const std::uint32_t declared = 4096U;
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    forged.push_back(static_cast<char>((declared >> shift) & 0xFFU));
  }
  for (unsigned index = 0; index < 8U; ++index) {
    forged.push_back(static_cast<char>(0));
  }
  forged.append(4096U, 'x');
  const FrameDecodeResult bounded = decode_frame(forged, 128U);
  LO_CHECK(!bounded.ok());
  LO_CHECK_EQ(bounded.status.code(), StatusCode::CapacityExceeded);
}

LO_TEST(transport, a_real_loopback_round_trip_works) {
  RuntimeConfig config = make_config({}, 1U);
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  LO_REQUIRE(started.ok());

  ServerOptions options{};
  options.host = "127.0.0.1";
  options.port = 0U;
  options.connection_limit = 1U;
  options.persist_on_close = false;

  TransportServer server(observatory, options);
  const Status listening = server.listen();
  LO_REQUIRE(listening.ok());
  LO_CHECK(server.bound_port() != 0U);

  std::thread server_thread([&server]() {
    const Status served = server.serve();
    (void)served;
  });

  TransportClient client{};
  const Status connected = client.connect("127.0.0.1", server.bound_port());
  LO_REQUIRE(connected.ok());

  std::string response;
  LO_CHECK(client.ping(response).ok());
  LO_CHECK_EQ(response, std::string("ok"));

  const std::string records =
      "v=1 kind=source name=wire-agent authority=primary provenance=real\n"
      "v=1 kind=link name=wire-link link-kind=physical linkgen=gen-1 provenance=real "
      "capacity-in-bps=1000000000\n"
      "v=1 kind=observation link=wire-link metric=oper-state source=wire-agent incarnation=boot-1 "
      "epoch=cfg-1 generation=gen-1 revision=1 observed-at=1700000000000000000 "
      "received-at=1700000000000000000 value=up\n";
  LO_CHECK(client.push_records(records, response).ok());
  LO_CHECK_EQ(response, std::string("records=1 rejected=0"));

  LO_CHECK(client.query_state("wire-link", response).ok());
  LO_CHECK(response.find("name=wire-link") != std::string::npos);
  LO_CHECK(response.find("state=healthy") != std::string::npos);

  LO_CHECK(client.shutdown(response).ok());
  LO_CHECK_EQ(response, std::string("shutdown"));
  client.close();
  server_thread.join();

  const ServerStatistics stats = server.statistics();
  LO_CHECK_EQ(stats.connections_accepted, static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(stats.pushes, static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(stats.queries, static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(stats.shutdowns, static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(stats.protocol_errors, static_cast<std::uint64_t>(0));

  const Status stopped = observatory.stop();
  (void)stopped;
}

LO_TEST(transport, a_malformed_frame_terminates_the_connection_without_killing_the_server) {
  RuntimeConfig config = make_config({}, 1U);
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  LO_REQUIRE(started.ok());

  ServerOptions options{};
  options.host = "127.0.0.1";
  options.port = 0U;
  options.connection_limit = 2U;
  options.persist_on_close = false;

  TransportServer server(observatory, options);
  LO_REQUIRE(server.listen().ok());
  std::thread server_thread([&server]() {
    const Status served = server.serve();
    (void)served;
  });

  {
    // A hostile peer sends a syntactically valid frame with a corrupted payload.
    // The server must close that connection and keep serving.
    const bool closed = send_raw_and_expect_close(server.bound_port(), [](std::string& bytes) {
      Frame frame{};
      frame.type = FrameType::Ping;
      frame.payload = "hello";
      (void)encode_frame(frame, 1024U, bytes);
      bytes[kFrameHeaderSize] = static_cast<char>(bytes[kFrameHeaderSize] ^ 0x7F);
    });
    LO_CHECK(closed);
  }

  {
    TransportClient client{};
    LO_REQUIRE(client.connect("127.0.0.1", server.bound_port()).ok());
    std::string response;
    LO_CHECK(client.ping(response).ok());
    LO_CHECK(client.shutdown(response).ok());
    client.close();
  }

  server_thread.join();
  LO_CHECK_EQ(server.statistics().connections_accepted, static_cast<std::uint64_t>(2));
  const Status stopped = observatory.stop();
  (void)stopped;
}

LO_TEST(transport, the_server_refuses_to_listen_beyond_loopback_without_opt_in) {
  RuntimeConfig config = make_config({}, 0U);
  auto clock = std::make_unique<ManualClock>(instant(kBaseInstant));
  Observatory observatory(std::move(config), std::move(clock));

  ServerOptions options{};
  options.host = "0.0.0.0";
  options.port = 0U;
  options.allow_non_loopback = false;
  TransportServer server(observatory, options);
  const Status listening = server.listen();
  LO_CHECK(!listening.ok());
  LO_CHECK_EQ(listening.code(), StatusCode::Rejected);
}

}  // namespace
