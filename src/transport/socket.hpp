// Link Observatory - private socket helpers.
//
// Not installed and not part of the public API. Isolates the two platform
// socket APIs behind one small surface so the transport code itself stays
// platform neutral.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "linkobs/core/status.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace linkobs::net {

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

/// One process wide socket runtime initialisation.
class SocketRuntime {
 public:
  SocketRuntime();
  ~SocketRuntime();
  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;

  [[nodiscard]] bool ok() const noexcept { return ok_; }

 private:
  bool ok_{false};
};

[[nodiscard]] Status last_socket_error(std::string_view what);

inline void close_socket(SocketHandle handle) noexcept {
  if (handle == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  ::closesocket(handle);
#else
  ::close(handle);
#endif
}

/// True for 127.0.0.0/8, ::1 and the name "localhost".
[[nodiscard]] bool is_loopback_host(std::string_view host) noexcept;

/// Sends every byte or reports why it could not.
[[nodiscard]] Status send_all(SocketHandle socket, std::string_view data);

/// Receives once. Returns false when the peer closed cleanly.
[[nodiscard]] Status receive_once(SocketHandle socket, std::string& buffer, bool& closed);

}  // namespace linkobs::net
