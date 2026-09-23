#include <iostream>
#include <string>
#include <vector>

#include "commands.hpp"

#include "linkobs/core/text.hpp"
#include "linkobs/version.hpp"

namespace linkobs::cli {
namespace {

[[nodiscard]] bool is_flag(std::string_view token) {
  return token.size() > 2U && token.rfind("--", 0) == 0 && token.find('=') == std::string_view::npos;
}

}  // namespace

bool Options::has(std::string_view name) const {
  return values.find(std::string{name}) != values.end() ||
         std::find(flags.begin(), flags.end(), std::string{name}) != flags.end();
}

std::optional<std::string> Options::get(std::string_view name) const {
  const auto found = values.find(std::string{name});
  if (found != values.end()) {
    return found->second;
  }
  if (std::find(flags.begin(), flags.end(), std::string{name}) != flags.end()) {
    return std::string{};
  }
  return std::nullopt;
}

std::string Options::get_or(std::string_view name, std::string fallback) const {
  const auto found = values.find(std::string{name});
  return found == values.end() ? std::move(fallback) : found->second;
}

std::uint64_t Options::get_u64(std::string_view name, std::uint64_t fallback) const {
  const auto found = values.find(std::string{name});
  if (found == values.end()) {
    return fallback;
  }
  std::uint64_t value = 0;
  if (!parse_u64_dec(found->second, value)) {
    return fallback;
  }
  return value;
}

std::int64_t Options::get_i64(std::string_view name, std::int64_t fallback) const {
  const auto found = values.find(std::string{name});
  if (found == values.end()) {
    return fallback;
  }
  std::int64_t value = 0;
  if (!parse_i64_dec(found->second, value)) {
    return fallback;
  }
  return value;
}

void print_usage(std::string_view program) {
  std::cout << "Link Observatory " << kVersionString << " - " << kVendorName << "\n";
  std::cout << "usage: " << program << " <command> [options]\n\n";
  std::cout << "commands:\n";
  std::cout << "  ingest    --db PATH [--input FILE|-] [--workers N] [--now-ns N]\n";
  std::cout << "  inspect   --db PATH [--link NAME] [--now-ns N]\n";
  std::cout << "  history   --db PATH [--link NAME] [--limit N]\n";
  std::cout << "  classify  --db PATH [--link NAME] [--now-ns N]\n";
  std::cout << "  explain   --db PATH --link NAME [--now-ns N]\n";
  std::cout << "  export    --db PATH [--link NAME] [--format lor|csv|summary]\n";
  std::cout << "  verify    --db PATH\n";
  std::cout << "  selftest  [--seed N]\n";
  std::cout << "  bench     [--records N] [--links N] [--sources N]\n";
  std::cout << "  serve     --db PATH --listen HOST:PORT [--port-file FILE]\n";
  std::cout << "  send      --connect HOST:PORT [--input FILE|-]\n";
  std::cout << "  version\n";
}

}  // namespace linkobs::cli

int main(int argc, char** argv) {
  using namespace linkobs::cli;

  if (argc < 2) {
    print_usage(argc > 0 ? argv[0] : "linkobs");
    return static_cast<int>(ExitCode::Usage);
  }

  Context context{};
  context.program = argv[0];
  const std::string command = argv[1];

  for (int index = 2; index < argc; ++index) {
    const std::string token = argv[index];
    if (is_flag(token)) {
      std::string name = token.substr(2);
      if (index + 1 < argc && argv[index + 1][0] != '-') {
        context.options.values[name] = argv[index + 1];
        ++index;
      } else {
        context.options.flags.push_back(std::move(name));
      }
    } else if (token.rfind("--", 0) == 0) {
      const std::size_t equals = token.find('=');
      if (equals == std::string::npos || equals < 3U) {
        context.options.flags.push_back(token.substr(2));
      } else {
        context.options.values[token.substr(2, equals - 2U)] = token.substr(equals + 1U);
      }
    } else {
      context.options.positional.push_back(token);
    }
  }

  if (context.options.has("now-ns")) {
    context.now_ns = context.options.get_i64("now-ns", 0);
  }

  if (command == "ingest") {
    return run_ingest(context);
  }
  if (command == "inspect") {
    return run_inspect(context);
  }
  if (command == "history") {
    return run_history(context);
  }
  if (command == "classify") {
    return run_classify(context);
  }
  if (command == "explain") {
    return run_explain(context);
  }
  if (command == "export") {
    return run_export(context);
  }
  if (command == "verify") {
    return run_verify(context);
  }
  if (command == "selftest") {
    return run_selftest(context);
  }
  if (command == "bench") {
    return run_bench(context);
  }
  if (command == "serve") {
    return run_serve(context);
  }
  if (command == "send") {
    return run_send(context);
  }
  if (command == "version" || command == "--version") {
    return run_version(context);
  }

  std::cerr << "unknown command: " << command << "\n";
  print_usage(context.program);
  return static_cast<int>(ExitCode::Usage);
}
