// Link Observatory - transport client.
//
// Used by the command line tool and by the integration tests to exercise the
// runtime across a real process boundary over a real socket.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "linkobs/core/status.hpp"
#include "linkobs/transport/frame.hpp"

namespace linkobs {

struct ClientOptions {
  std::size_t max_payload_bytes{1024U * 1024U};
  std::size_t max_response_bytes{4U * 1024U * 1024U};
};

class TransportClient {
 public:
  explicit TransportClient(ClientOptions options = {});
  ~TransportClient();

  TransportClient(const TransportClient&) = delete;
  TransportClient& operator=(const TransportClient&) = delete;

  [[nodiscard]] Status connect(std::string_view host, std::uint16_t port);
  [[nodiscard]] bool connected() const noexcept { return socket_ != -1; }
  void close();

  [[nodiscard]] Status ping(std::string& response);
  [[nodiscard]] Status push_records(std::string_view records, std::string& response);
  [[nodiscard]] Status query_state(std::string_view link_name, std::string& response);
  [[nodiscard]] Status shutdown(std::string& response);

 private:
  [[nodiscard]] Status exchange(const Frame& request, Frame& response);
  [[nodiscard]] Status receive_frame(Frame& response);

  ClientOptions options_{};
  std::intptr_t socket_{-1};
  std::string buffer_{};
};

}  // namespace linkobs
