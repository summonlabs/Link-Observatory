// Link Observatory - ingest and query transport server.
//
// The server is a real TCP listener. It is loopback-only unless the caller
// explicitly allows otherwise, because a runtime that observes infrastructure
// must not silently expose an ingest port to a network.
//
// Behaviour that is deliberately bounded:
//   * at most max_connections connections are handled at once; further
//     connections are accepted and closed immediately, and counted,
//   * every frame payload is bounded,
//   * the read buffer for a connection is bounded,
//   * shutdown is explicit: a Shutdown frame, a connection limit, or a stop
//     request from another thread. There is no timeout based termination.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "linkobs/runtime/observatory.hpp"
#include "linkobs/transport/frame.hpp"

namespace linkobs {

struct ServerOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  bool allow_non_loopback{false};
  std::size_t max_connections{8U};
  std::size_t max_payload_bytes{1024U * 1024U};
  std::size_t max_read_buffer_bytes{4U * 1024U * 1024U};
  std::size_t listen_backlog{8U};
  /// When non-empty, the bound port is written here once listening.
  std::string port_file{};
  /// Stop after this many accepted connections. Zero means no limit.
  std::size_t connection_limit{0U};
  bool persist_on_close{true};
};

struct ServerStatistics {
  std::uint64_t connections_accepted{0};
  std::uint64_t connections_refused{0};
  std::uint64_t connections_closed{0};
  std::uint64_t frames_received{0};
  std::uint64_t frames_sent{0};
  std::uint64_t pushes{0};
  std::uint64_t queries{0};
  std::uint64_t pings{0};
  std::uint64_t shutdowns{0};
  std::uint64_t protocol_errors{0};
  std::uint64_t records_submitted{0};
  std::uint64_t records_rejected{0};
  std::uint64_t payload_bytes_received{0};
};

class TransportServer {
 public:
  TransportServer(Observatory& observatory, ServerOptions options);
  ~TransportServer();

  TransportServer(const TransportServer&) = delete;
  TransportServer& operator=(const TransportServer&) = delete;

  /// Binds and starts listening. Writes the port file when configured.
  [[nodiscard]] Status listen();

  /// Serves until shutdown is requested. Blocks.
  [[nodiscard]] Status serve();

  /// Asks the accept loop to stop. Safe to call from another thread.
  void request_stop();

  [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }
  [[nodiscard]] ServerStatistics statistics() const;
  [[nodiscard]] std::string bound_address() const;

 private:
  void handle_connection(std::intptr_t socket);
  void wake_accept_loop();

  Observatory& observatory_;
  ServerOptions options_;
  std::intptr_t listener_{-1};
  std::uint16_t bound_port_{0};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> listening_{false};
  std::atomic<std::uint64_t> active_connections_{0};
  std::atomic<std::uint64_t> total_connections_{0};
  ServerStatistics stats_{};
  mutable TrackedLock stats_lock_{LockClass::Transport};
  std::vector<std::thread> connection_threads_{};
};

}  // namespace linkobs
