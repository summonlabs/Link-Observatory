// Example: connect to a running runtime and query link state.
//
// Usage: transport_client <host> <port> [link-name]
// With no arguments the program explains how to start a server instead of
// pretending to have one.

#include <cstdint>
#include <iostream>
#include <string>

#include "linkobs/core/text.hpp"
#include "linkobs/transport/client.hpp"

int main(int argc, char** argv) {
  using namespace linkobs;

  if (argc < 3) {
    std::cout << "usage: transport_client <host> <port> [link-name]\n"
                 "start a runtime with: linkobs serve --db state.lkos "
                 "--listen 127.0.0.1:9100 --port-file port.txt\n";
    return 0;
  }

  std::uint64_t port = 0;
  if (!parse_u64_dec(argv[2], port) || port > 65535U) {
    std::cerr << "invalid port\n";
    return 1;
  }

  TransportClient client{};
  const Status connected = client.connect(argv[1], static_cast<std::uint16_t>(port));
  if (!connected.ok()) {
    std::cerr << "connect: " << connected.to_string() << "\n";
    return 2;
  }

  std::string response;
  const Status pinged = client.ping(response);
  if (!pinged.ok()) {
    std::cerr << "ping: " << pinged.to_string() << "\n";
    return 3;
  }
  std::cout << "ping=" << response << "\n";

  const std::string link = argc > 3 ? argv[3] : std::string{};
  const Status queried = client.query_state(link, response);
  if (!queried.ok()) {
    std::cerr << "query: " << queried.to_string() << "\n";
    return 4;
  }
  std::cout << response;
  return 0;
}
