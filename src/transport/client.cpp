#include "linkobs/transport/client.hpp"

#include "transport/socket.hpp"

namespace linkobs {
namespace {

[[nodiscard]] Status ensure_socket_runtime() {
  static net::SocketRuntime runtime;
  if (!runtime.ok()) {
    return Status::of(StatusCode::IoError, "socket runtime could not be initialised");
  }
  return Status::success();
}

}  // namespace

TransportClient::TransportClient(ClientOptions options) : options_(options) {}

TransportClient::~TransportClient() { close(); }

Status TransportClient::connect(std::string_view host, std::uint16_t port) {
  close();
  const Status ready = ensure_socket_runtime();
  if (!ready.ok()) {
    return ready;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  const std::string service = std::to_string(static_cast<unsigned>(port));
  const std::string host_text{host};
  addrinfo* resolved = nullptr;
  if (::getaddrinfo(host_text.c_str(), service.c_str(), &hints, &resolved) != 0 ||
      resolved == nullptr) {
    return Status::of(StatusCode::InvalidArgument, "connect address could not be resolved");
  }

  net::SocketHandle socket = net::kInvalidSocket;
  Status last = Status::of(StatusCode::IoError, "no usable address");
  for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
    socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket == net::kInvalidSocket) {
      last = net::last_socket_error("socket");
      continue;
    }
    if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      break;
    }
    last = net::last_socket_error("connect");
    net::close_socket(socket);
    socket = net::kInvalidSocket;
  }
  ::freeaddrinfo(resolved);
  if (socket == net::kInvalidSocket) {
    return last;
  }
  socket_ = static_cast<std::intptr_t>(socket);
  buffer_.clear();
  return Status::success();
}

void TransportClient::close() {
  if (socket_ != -1) {
    net::close_socket(static_cast<net::SocketHandle>(socket_));
    socket_ = -1;
  }
  buffer_.clear();
}

Status TransportClient::receive_frame(Frame& response) {
  while (true) {
    const FrameDecodeResult decoded = decode_frame(buffer_, options_.max_payload_bytes);
    if (!decoded.ok()) {
      return decoded.status;
    }
    if (decoded.complete) {
      buffer_.erase(0, decoded.consumed);
      response = decoded.frame;
      return Status::success();
    }
    if (buffer_.size() > options_.max_response_bytes) {
      return Status::of(StatusCode::CapacityExceeded, "response exceeds the configured bound");
    }
    bool closed = false;
    const Status received = net::receive_once(static_cast<net::SocketHandle>(socket_), buffer_,
                                              closed);
    if (!received.ok()) {
      return received;
    }
    if (closed) {
      return Status::of(StatusCode::IoError, "peer closed the connection before replying");
    }
  }
}

Status TransportClient::exchange(const Frame& request, Frame& response) {
  if (socket_ == -1) {
    return Status::of(StatusCode::Cancelled, "client is not connected");
  }
  std::string encoded;
  const Status status = encode_frame(request, options_.max_payload_bytes, encoded);
  if (!status.ok()) {
    return status;
  }
  const Status sent = net::send_all(static_cast<net::SocketHandle>(socket_), encoded);
  if (!sent.ok()) {
    return sent;
  }
  return receive_frame(response);
}

Status TransportClient::ping(std::string& response) {
  Frame reply{};
  const Status status = exchange(Frame{FrameType::Ping, 0, {}}, reply);
  if (!status.ok()) {
    return status;
  }
  if (reply.type != FrameType::Pong) {
    return Status::of(StatusCode::Unsupported, "unexpected reply to ping");
  }
  response = reply.payload;
  return Status::success();
}

Status TransportClient::push_records(std::string_view records, std::string& response) {
  Frame reply{};
  Frame request{};
  request.type = FrameType::PushRecords;
  request.payload.assign(records);
  const Status status = exchange(request, reply);
  if (!status.ok()) {
    return status;
  }
  response = reply.payload;
  if (reply.type == FrameType::ErrorReport) {
    return Status::of(StatusCode::Rejected, reply.payload);
  }
  if (reply.type != FrameType::PushAck) {
    return Status::of(StatusCode::Unsupported, "unexpected reply to push");
  }
  return Status::success();
}

Status TransportClient::query_state(std::string_view link_name, std::string& response) {
  Frame reply{};
  Frame request{};
  request.type = FrameType::QueryState;
  request.payload.assign(link_name);
  const Status status = exchange(request, reply);
  if (!status.ok()) {
    return status;
  }
  if (reply.type == FrameType::ErrorReport) {
    return Status::of(StatusCode::Rejected, reply.payload);
  }
  if (reply.type != FrameType::StateReport) {
    return Status::of(StatusCode::Unsupported, "unexpected reply to query");
  }
  response = reply.payload;
  return Status::success();
}

Status TransportClient::shutdown(std::string& response) {
  Frame reply{};
  const Status status = exchange(Frame{FrameType::Shutdown, 0, {}}, reply);
  if (!status.ok()) {
    return status;
  }
  response = reply.payload;
  return Status::success();
}

}  // namespace linkobs
