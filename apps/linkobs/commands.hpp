// Link Observatory - command line surface.
//
// Every command is deterministic given the same inputs and the same explicit
// evaluation instant. Nothing prints a wall clock time, an address, a locale
// dependent number or a host specific path.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace linkobs::cli {

/// Exit codes are part of the interface and are stable.
enum class ExitCode : int {
  Ok = 0,
  Usage = 1,
  Data = 2,
  Io = 3,
  Integrity = 4,
  Unsupported = 5,
  Internal = 6,
};

struct Options {
  std::map<std::string, std::string> values{};
  std::vector<std::string> flags{};
  std::vector<std::string> positional{};

  [[nodiscard]] bool has(std::string_view name) const;
  [[nodiscard]] std::optional<std::string> get(std::string_view name) const;
  [[nodiscard]] std::string get_or(std::string_view name, std::string fallback) const;
  [[nodiscard]] std::uint64_t get_u64(std::string_view name, std::uint64_t fallback) const;
  [[nodiscard]] std::int64_t get_i64(std::string_view name, std::int64_t fallback) const;
};

struct Context {
  std::string program{};
  Options options{};
  /// Explicit evaluation instant. When unset the system clock is used.
  std::optional<std::int64_t> now_ns{};
};

[[nodiscard]] int run_ingest(const Context& context);
[[nodiscard]] int run_inspect(const Context& context);
[[nodiscard]] int run_history(const Context& context);
[[nodiscard]] int run_classify(const Context& context);
[[nodiscard]] int run_explain(const Context& context);
[[nodiscard]] int run_export(const Context& context);
[[nodiscard]] int run_verify(const Context& context);
[[nodiscard]] int run_selftest(const Context& context);
[[nodiscard]] int run_bench(const Context& context);
[[nodiscard]] int run_serve(const Context& context);
[[nodiscard]] int run_send(const Context& context);
[[nodiscard]] int run_version(const Context& context);

/// Reads a text file, or standard input when the path is "-".
[[nodiscard]] std::optional<std::string> read_text_input(const std::string& path,
                                                         std::size_t max_bytes);

/// Builds a runtime configuration from the common options.
struct ConfiguredRuntime {
  int error{0};
  std::string message{};
  std::map<std::string, std::string> unused{};
};

void print_usage(std::string_view program);

}  // namespace linkobs::cli
