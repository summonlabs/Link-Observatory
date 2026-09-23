#include <chrono>
#include <iostream>
#include <thread>

#include "common.hpp"

#include "linkobs/ingest/codec.hpp"
#include "linkobs/transport/client.hpp"
#include "linkobs/transport/server.hpp"

namespace linkobs::cli {
namespace {

[[nodiscard]] bool parse_endpoint(std::string_view text,
                                  std::string& host,
                                  std::uint16_t& port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0U || colon + 1U >= text.size()) {
    return false;
  }
  host.assign(text.substr(0, colon));
  std::uint64_t parsed = 0;
  if (!parse_u64_dec(text.substr(colon + 1U), parsed) || parsed > 65535U) {
    return false;
  }
  port = static_cast<std::uint16_t>(parsed);
  return true;
}

}  // namespace

int run_serve(const Context& context) {
  int exit_code = static_cast<int>(ExitCode::Ok);
  const std::string db = require_db(context, exit_code);
  if (exit_code != static_cast<int>(ExitCode::Ok)) {
    return exit_code;
  }
  const std::optional<std::string> listen = context.options.get("listen");
  if (!listen.has_value()) {
    std::cerr << "error: --listen HOST:PORT is required\n";
    return static_cast<int>(ExitCode::Usage);
  }

  ServerOptions options{};
  if (!parse_endpoint(*listen, options.host, options.port)) {
    std::cerr << "error: --listen must be HOST:PORT\n";
    return static_cast<int>(ExitCode::Usage);
  }
  options.allow_non_loopback = context.options.has("allow-non-loopback");
  options.port_file = context.options.get_or("port-file", "");
  options.max_connections =
      static_cast<std::size_t>(context.options.get_u64("max-connections", 8U));
  options.connection_limit =
      static_cast<std::size_t>(context.options.get_u64("connection-limit", 0U));
  options.max_payload_bytes =
      static_cast<std::size_t>(context.options.get_u64("max-payload", 1024U * 1024U));

  RuntimeConfig config = make_config(context, db, true, false);
  config.workers = static_cast<std::size_t>(context.options.get_u64("workers", 1U));
  Observatory observatory(std::move(config), make_clock(context));
  const Status started = observatory.start();
  if (!started.ok() && started.code() != StatusCode::NotFound) {
    std::cerr << "error: " << started.to_string() << "\n";
    return exit_code_for(started.code());
  }

  TransportServer server(observatory, options);
  const Status listening = server.listen();
  if (!listening.ok()) {
    std::cerr << "error: " << listening.to_string() << "\n";
    const Status stopped = observatory.stop();
    (void)stopped;
    return exit_code_for(listening.code());
  }
  std::cout << "listening " << server.bound_address() << "\n" << std::flush;

  const Status served = server.serve();
  const Status stopped = observatory.stop();
  const ServerStatistics stats = server.statistics();
  std::cout << "connections=" << stats.connections_accepted
            << " refused=" << stats.connections_refused << " frames=" << stats.frames_received
            << " pushes=" << stats.pushes << " queries=" << stats.queries
            << " shutdowns=" << stats.shutdowns << " protocol-errors=" << stats.protocol_errors
            << "\n";
  if (!served.ok()) {
    std::cerr << "error: " << served.to_string() << "\n";
    return exit_code_for(served.code());
  }
  if (!stopped.ok()) {
    std::cerr << "error: " << stopped.to_string() << "\n";
    return exit_code_for(stopped.code());
  }
  return static_cast<int>(ExitCode::Ok);
}

int run_send(const Context& context) {
  const std::optional<std::string> connect = context.options.get("connect");
  if (!connect.has_value()) {
    std::cerr << "error: --connect HOST:PORT is required\n";
    return static_cast<int>(ExitCode::Usage);
  }
  std::string host;
  std::uint16_t port = 0;
  if (!parse_endpoint(*connect, host, port)) {
    std::cerr << "error: --connect must be HOST:PORT\n";
    return static_cast<int>(ExitCode::Usage);
  }

  TransportClient client{};
  const Status connected = client.connect(host, port);
  if (!connected.ok()) {
    std::cerr << "error: " << connected.to_string() << "\n";
    return exit_code_for(connected.code());
  }

  std::string response;
  const Status pinged = client.ping(response);
  if (!pinged.ok()) {
    std::cerr << "error: " << pinged.to_string() << "\n";
    return exit_code_for(pinged.code());
  }
  std::cout << "ping=" << response << "\n";

  const std::string input_path = context.options.get_or("input", "");
  if (!input_path.empty()) {
    const std::optional<std::string> text = read_text_input(input_path, kMaxInputBytes);
    if (!text.has_value()) {
      std::cerr << "error: input could not be read\n";
      return static_cast<int>(ExitCode::Io);
    }
    const Status pushed = client.push_records(*text, response);
    std::cout << "push=" << response << "\n";
    if (!pushed.ok()) {
      std::cerr << "error: " << pushed.to_string() << "\n";
      return exit_code_for(pushed.code());
    }
  }

  const std::string link = context.options.get_or("link", "");
  const Status queried = client.query_state(link, response);
  if (!queried.ok()) {
    std::cerr << "error: " << queried.to_string() << "\n";
    return exit_code_for(queried.code());
  }
  std::cout << response;

  if (context.options.has("shutdown")) {
    const Status shut = client.shutdown(response);
    if (!shut.ok()) {
      std::cerr << "error: " << shut.to_string() << "\n";
      return exit_code_for(shut.code());
    }
    std::cout << "shutdown=" << response << "\n";
  }
  return static_cast<int>(ExitCode::Ok);
}

}  // namespace linkobs::cli
