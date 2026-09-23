#include "transport/socket.hpp"

#include <cstring>

namespace linkobs::net {

SocketRuntime::SocketRuntime() {
#ifdef _WIN32
  WSADATA data{};
  ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  ok_ = true;
#endif
}

SocketRuntime::~SocketRuntime() {
#ifdef _WIN32
  if (ok_) {
    ::WSACleanup();
  }
#endif
}

Status last_socket_error(std::string_view what) {
#ifdef _WIN32
  const int code = ::WSAGetLastError();
#else
  const int code = errno;
#endif
  std::string message{what};
  message.append(" failed (socket error ");
  message.append(std::to_string(code));
  message.push_back(')');
  return Status::of(StatusCode::IoError, message);
}

bool is_loopback_host(std::string_view host) noexcept {
  if (host == "localhost") {
    return true;
  }
  if (host == "::1" || host == "[::1]") {
    return true;
  }
  if (host.rfind("127.", 0) == 0) {
    return true;
  }
  return false;
}

Status send_all(SocketHandle socket, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t remaining = data.size() - sent;
    const int chunk = static_cast<int>(remaining > 65536U ? 65536U : remaining);
#ifdef _WIN32
    const int written = ::send(socket, data.data() + sent, chunk, 0);
#else
    const ssize_t written = ::send(socket, data.data() + sent, static_cast<std::size_t>(chunk), 0);
#endif
    if (written <= 0) {
      return last_socket_error("send");
    }
    sent += static_cast<std::size_t>(written);
  }
  return Status::success();
}

Status receive_once(SocketHandle socket, std::string& buffer, bool& closed) {
  closed = false;
  char chunk[8192];
#ifdef _WIN32
  const int received = ::recv(socket, chunk, static_cast<int>(sizeof(chunk)), 0);
#else
  const ssize_t received = ::recv(socket, chunk, sizeof(chunk), 0);
#endif
  if (received == 0) {
    closed = true;
    return Status::success();
  }
  if (received < 0) {
    return last_socket_error("recv");
  }
  buffer.append(chunk, static_cast<std::size_t>(received));
  return Status::success();
}

}  // namespace linkobs::net
