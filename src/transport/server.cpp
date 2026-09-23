#include "linkobs/transport/server.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

#include "linkobs/classify/explain.hpp"
#include "linkobs/core/text.hpp"
#include "linkobs/ingest/codec.hpp"
#include "transport/socket.hpp"

namespace linkobs {
namespace {

/// A listener that keeps failing to accept is broken; retrying forever would
/// spin. This is a failure bound, not a deadline.
constexpr std::size_t kMaxConsecutiveAcceptFailures = 64U;

[[nodiscard]] Status ensure_socket_runtime() {
  static net::SocketRuntime runtime;
  if (!runtime.ok()) {
    return Status::of(StatusCode::IoError, "socket runtime could not be initialised");
  }
  return Status::success();
}

[[nodiscard]] Status write_port_file(const std::string& path, std::uint16_t port) {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream.good()) {
      return Status::of(StatusCode::IoError, "port file could not be created");
    }
    std::string content = to_dec(static_cast<std::uint64_t>(port));
    content.push_back('\n');
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();
    if (!stream.good()) {
      return Status::of(StatusCode::IoError, "port file could not be written");
    }
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    return Status::of(StatusCode::IoError, "port file could not be moved into place");
  }
  return Status::success();
}

[[nodiscard]] std::string error_payload(const Status& status) {
  std::string out{"code="};
  out.append(to_string(status.code()));
  out.append(" message=");
  out.append(status.message());
  return out;
}

[[nodiscard]] std::string state_report(const Observatory& observatory, std::string_view link_name) {
  std::string out;
  if (link_name.empty()) {
    for (const Classification& classification : observatory.classify_all()) {
      out.append(summarize(classification));
      out.push_back('\n');
    }
  } else {
    const std::optional<Classification> classification = observatory.classify(link_name);
    if (!classification.has_value()) {
      out.append("link=");
      out.append(link_name);
      out.append(" state=absent\n");
    } else {
      out.append("name=");
      out.append(link_name);
      out.push_back(' ');
      out.append(summarize(*classification));
      out.push_back('\n');
    }
  }
  const RuntimeStatistics stats = observatory.statistics();
  out.append("runtime accepted=");
  out.append(to_dec(stats.records_accepted));
  out.append(" fenced=");
  out.append(to_dec(stats.records_fenced));
  out.append(" state-changes=");
  out.append(to_dec(stats.state_changes));
  out.push_back('\n');
  return out;
}

}  // namespace

TransportServer::TransportServer(Observatory& observatory, ServerOptions options)
    : observatory_(observatory), options_(std::move(options)) {}

TransportServer::~TransportServer() {
  request_stop();
  for (std::thread& thread : connection_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  if (listener_ != -1) {
    net::close_socket(static_cast<net::SocketHandle>(listener_));
    listener_ = -1;
  }
}

Status TransportServer::listen() {
  const Status ready = ensure_socket_runtime();
  if (!ready.ok()) {
    return ready;
  }
  if (!options_.allow_non_loopback && !net::is_loopback_host(options_.host)) {
    return Status::of(StatusCode::Rejected,
                      "listening beyond loopback requires an explicit opt in");
  }
  if (options_.max_connections == 0U) {
    return Status::of(StatusCode::InvalidArgument, "max_connections must be positive");
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  const std::string service = to_dec(static_cast<std::uint64_t>(options_.port));
  addrinfo* resolved = nullptr;
  if (::getaddrinfo(options_.host.c_str(), service.c_str(), &hints, &resolved) != 0 ||
      resolved == nullptr) {
    return Status::of(StatusCode::InvalidArgument, "listen address could not be resolved");
  }

  net::SocketHandle socket = net::kInvalidSocket;
  for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
    socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket == net::kInvalidSocket) {
      continue;
    }
    int reuse = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 static_cast<int>(sizeof(reuse)));
    if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
        ::listen(socket, static_cast<int>(options_.listen_backlog)) == 0) {
      break;
    }
    net::close_socket(socket);
    socket = net::kInvalidSocket;
  }
  ::freeaddrinfo(resolved);
  if (socket == net::kInvalidSocket) {
    return net::last_socket_error("bind/listen");
  }

  sockaddr_storage address{};
  socklen_t address_length = static_cast<socklen_t>(sizeof(address));
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
    net::close_socket(socket);
    return net::last_socket_error("getsockname");
  }
  if (address.ss_family == AF_INET) {
    bound_port_ = ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
  } else {
    bound_port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
  }

  listener_ = static_cast<std::intptr_t>(socket);
  listening_.store(true, std::memory_order_relaxed);

  if (!options_.port_file.empty()) {
    const Status written = write_port_file(options_.port_file, bound_port_);
    if (!written.ok()) {
      return written;
    }
  }
  return Status::success();
}

std::string TransportServer::bound_address() const {
  std::string out{options_.host};
  out.push_back(':');
  out.append(to_dec(static_cast<std::uint64_t>(bound_port_)));
  return out;
}

void TransportServer::request_stop() {
  stop_requested_.store(true, std::memory_order_relaxed);
  wake_accept_loop();
}

void TransportServer::wake_accept_loop() {
  // The accept loop blocks in accept() with no deadline, by design: the runtime
  // never terminates work on a timer. A stop request therefore has to unblock it
  // explicitly. Connecting to the listener once is portable, immediate and does
  // not depend on the platform's behaviour when a socket is closed from another
  // thread.
  if (!listening_.load(std::memory_order_relaxed) || listener_ == -1) {
    return;
  }
  if (!net::is_loopback_host(options_.host)) {
    return;
  }
  const std::string service = to_dec(static_cast<std::uint64_t>(bound_port_));
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  addrinfo* resolved = nullptr;
  if (::getaddrinfo(options_.host.c_str(), service.c_str(), &hints, &resolved) != 0 ||
      resolved == nullptr) {
    return;
  }
  for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
    const net::SocketHandle socket =
        ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (socket == net::kInvalidSocket) {
      continue;
    }
    if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      net::close_socket(socket);
      break;
    }
    net::close_socket(socket);
  }
  ::freeaddrinfo(resolved);
}

Status TransportServer::serve() {
  if (listener_ == -1) {
    return Status::of(StatusCode::Internal, "server is not listening");
  }
  std::size_t consecutive_failures = 0;
  while (!stop_requested_.load(std::memory_order_relaxed)) {
    if (options_.connection_limit != 0U &&
        total_connections_.load(std::memory_order_relaxed) >= options_.connection_limit) {
      break;
    }
    sockaddr_storage peer{};
    socklen_t peer_length = static_cast<socklen_t>(sizeof(peer));
    const net::SocketHandle accepted =
        ::accept(static_cast<net::SocketHandle>(listener_), reinterpret_cast<sockaddr*>(&peer),
                 &peer_length);
    if (accepted == net::kInvalidSocket) {
      if (stop_requested_.load(std::memory_order_relaxed)) {
        break;
      }
      ++consecutive_failures;
      if (consecutive_failures >= kMaxConsecutiveAcceptFailures) {
        break;
      }
      continue;
    }
    consecutive_failures = 0;
    if (stop_requested_.load(std::memory_order_relaxed)) {
      // The connection that woke the loop is not a client.
      net::close_socket(accepted);
      break;
    }
    total_connections_.fetch_add(1U, std::memory_order_relaxed);
    if (active_connections_.load(std::memory_order_relaxed) >= options_.max_connections) {
      {
        std::lock_guard<TrackedLock> guard(stats_lock_);
        stats_.connections_refused += 1U;
      }
      net::close_socket(accepted);
      continue;
    }
    active_connections_.fetch_add(1U, std::memory_order_relaxed);
    {
      std::lock_guard<TrackedLock> guard(stats_lock_);
      stats_.connections_accepted += 1U;
    }
    connection_threads_.emplace_back([this, accepted]() {
      handle_connection(static_cast<std::intptr_t>(accepted));
      active_connections_.fetch_sub(1U, std::memory_order_relaxed);
      std::lock_guard<TrackedLock> guard(stats_lock_);
      stats_.connections_closed += 1U;
    });
    // Finished threads are reclaimed in bounded batches so that a long running
    // server cannot accumulate thread handles without limit.
    if (connection_threads_.size() >= options_.max_connections * 4U) {
      for (std::thread& thread : connection_threads_) {
        if (thread.joinable()) {
          thread.join();
        }
      }
      connection_threads_.clear();
    }
  }
  for (std::thread& thread : connection_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  connection_threads_.clear();
  listening_.store(false, std::memory_order_relaxed);
  if (options_.persist_on_close) {
    const SnapshotWriteResult result = observatory_.persist();
    if (!result.status.ok() && result.status.code() != StatusCode::InvalidArgument) {
      return result.status;
    }
  }
  return Status::success();
}

void TransportServer::handle_connection(std::intptr_t raw_socket) {
  const auto socket = static_cast<net::SocketHandle>(raw_socket);
  std::string buffer;
  bool closed = false;
  bool finish = false;

  while (!closed && !finish) {
    const Status received = net::receive_once(socket, buffer, closed);
    if (!received.ok()) {
      break;
    }
    if (buffer.size() > options_.max_read_buffer_bytes) {
      std::lock_guard<TrackedLock> guard(stats_lock_);
      stats_.protocol_errors += 1U;
      break;
    }
    while (!finish) {
      const FrameDecodeResult decoded = decode_frame(buffer, options_.max_payload_bytes);
      if (!decoded.ok()) {
        std::lock_guard<TrackedLock> guard(stats_lock_);
        stats_.protocol_errors += 1U;
        const std::string payload = error_payload(decoded.status);
        std::string encoded;
        if (encode_frame(Frame{FrameType::ErrorReport, 0, payload}, options_.max_payload_bytes,
                         encoded)
                .ok()) {
          const Status sent = net::send_all(socket, encoded);
          (void)sent;
        }
        finish = true;
        break;
      }
      if (!decoded.complete) {
        break;
      }
      buffer.erase(0, decoded.consumed);
      {
        std::lock_guard<TrackedLock> guard(stats_lock_);
        stats_.frames_received += 1U;
        stats_.payload_bytes_received += decoded.frame.payload.size();
      }

      Frame response{};
      response.type = FrameType::Pong;
      switch (decoded.frame.type) {
        case FrameType::Ping: {
          {
            std::lock_guard<TrackedLock> guard(stats_lock_);
            stats_.pings += 1U;
          }
          response.payload = "ok";
          break;
        }
        case FrameType::PushRecords: {
          ParseOutcome outcome = parse_batch(decoded.frame.payload, observatory_.config().policy);
          std::size_t submitted = 0;
          if (outcome.ok()) {
            // Wait until the records are in the store before acknowledging, so a
            // client that queries immediately after a push sees its own write.
            auto completion = std::make_shared<BatchCompletion>();
            outcome.batch.on_applied = [completion]() { completion->signal(); };
            const Status applied = observatory_.submit_batch(std::move(outcome.batch));
            if (!applied.ok()) {
              response.type = FrameType::ErrorReport;
              response.payload = error_payload(applied);
              break;
            }
            completion->wait();
            submitted = 1U;
          }
          response.type = FrameType::PushAck;
          response.payload = std::string{"records="} +
                             to_dec(static_cast<std::uint64_t>(submitted)) + " rejected=" +
                             to_dec(static_cast<std::uint64_t>(outcome.rejected));
          for (const std::string& error : outcome.errors) {
            response.payload.push_back('\n');
            response.payload.append(error);
          }
          {
            std::lock_guard<TrackedLock> guard(stats_lock_);
            stats_.pushes += 1U;
            stats_.records_submitted += submitted;
            stats_.records_rejected += outcome.rejected;
          }
          break;
        }
        case FrameType::QueryState: {
          {
            std::lock_guard<TrackedLock> guard(stats_lock_);
            stats_.queries += 1U;
          }
          response.type = FrameType::StateReport;
          response.payload = state_report(observatory_, trim_ascii(decoded.frame.payload));
          break;
        }
        case FrameType::Shutdown: {
          {
            std::lock_guard<TrackedLock> guard(stats_lock_);
            stats_.shutdowns += 1U;
          }
          response.type = FrameType::Pong;
          response.payload = "shutdown";
          request_stop();
          finish = true;
          break;
        }
        case FrameType::Pong:
        case FrameType::PushAck:
        case FrameType::StateReport:
        case FrameType::ErrorReport: {
          std::lock_guard<TrackedLock> guard(stats_lock_);
          stats_.protocol_errors += 1U;
          response.type = FrameType::ErrorReport;
          response.payload = error_payload(Status::of(
              StatusCode::Unsupported, "client sent a frame type that only a server may send"));
          break;
        }
      }

      std::string encoded;
      const Status status =
          encode_frame(response, options_.max_payload_bytes, encoded);
      if (!status.ok()) {
        std::lock_guard<TrackedLock> guard(stats_lock_);
        stats_.protocol_errors += 1U;
        break;
      }
      const Status sent = net::send_all(socket, encoded);
      if (!sent.ok()) {
        break;
      }
      {
        std::lock_guard<TrackedLock> guard(stats_lock_);
        stats_.frames_sent += 1U;
      }
      if (finish) {
        break;
      }
    }
  }
  net::close_socket(socket);
}

ServerStatistics TransportServer::statistics() const {
  std::lock_guard<TrackedLock> guard(stats_lock_);
  return stats_;
}

}  // namespace linkobs
