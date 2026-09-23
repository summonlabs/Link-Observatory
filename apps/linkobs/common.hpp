// Link Observatory - shared helpers for the command line tool.

#pragma once

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "commands.hpp"

#include "linkobs/core/text.hpp"
#include "linkobs/runtime/observatory.hpp"
#include "linkobs/time/clock.hpp"

namespace linkobs::cli {

inline constexpr std::size_t kMaxInputBytes = 64U * 1024U * 1024U;

[[nodiscard]] inline std::unique_ptr<Clock> make_clock(const Context& context) {
  if (context.now_ns.has_value()) {
    return std::make_unique<ManualClock>(TimePoint::from_nanos(*context.now_ns));
  }
  return std::make_unique<SystemClock>();
}

[[nodiscard]] inline RuntimeConfig make_config(const Context& context,
                                               std::string db_path,
                                               bool restore,
                                               bool persist_on_stop) {
  RuntimeConfig config{};
  config.snapshot_path = std::move(db_path);
  config.restore_on_start = restore;
  config.persist_on_stop = persist_on_stop;
  config.workers = static_cast<std::size_t>(context.options.get_u64("workers", 0U));
  config.queue_depth = static_cast<std::size_t>(context.options.get_u64("queue-depth", 1024U));
  config.runtime_name = context.options.get_or("runtime-name", "link-observatory");
  config.incarnation_name = context.options.get_or("incarnation", "incarnation-0");
  config.policy = make_default_policy();
  const std::uint64_t max_links = context.options.get_u64("max-links", 0U);
  if (max_links != 0U) {
    config.policy.limits.max_links = static_cast<std::size_t>(max_links);
  }
  return config;
}

[[nodiscard]] inline int exit_code_for(StatusCode code) {
  switch (code) {
    case StatusCode::Ok:
      return static_cast<int>(ExitCode::Ok);
    case StatusCode::InvalidArgument:
      return static_cast<int>(ExitCode::Usage);
    case StatusCode::OutOfRange:
    case StatusCode::CapacityExceeded:
    case StatusCode::AlreadyExists:
    case StatusCode::Conflict:
    case StatusCode::Rejected:
      return static_cast<int>(ExitCode::Data);
    case StatusCode::NotFound:
    case StatusCode::IoError:
      return static_cast<int>(ExitCode::Io);
    case StatusCode::CorruptData:
    case StatusCode::VersionMismatch:
      return static_cast<int>(ExitCode::Integrity);
    case StatusCode::Unsupported:
      return static_cast<int>(ExitCode::Unsupported);
    case StatusCode::Cancelled:
    case StatusCode::Internal:
      return static_cast<int>(ExitCode::Internal);
  }
  return static_cast<int>(ExitCode::Internal);
}

[[nodiscard]] inline std::string require_db(const Context& context, int& exit_code) {
  const std::optional<std::string> path = context.options.get("db");
  if (!path.has_value() || path->empty()) {
    std::cerr << "error: --db PATH is required\n";
    exit_code = static_cast<int>(ExitCode::Usage);
    return {};
  }
  return *path;
}

[[nodiscard]] inline std::optional<std::string> read_text_input(const std::string& path,
                                                                std::size_t max_bytes) {
  if (path == "-") {
    std::string content;
    std::string line;
    while (std::getline(std::cin, line)) {
      if (content.size() + line.size() + 1U > max_bytes) {
        return std::nullopt;
      }
      content.append(line);
      content.push_back('\n');
    }
    return content;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) {
    return std::nullopt;
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff length = stream.tellg();
  if (length < 0 || static_cast<std::uint64_t>(length) > max_bytes) {
    return std::nullopt;
  }
  stream.seekg(0, std::ios::beg);
  std::string content(static_cast<std::size_t>(length), '\0');
  if (length > 0) {
    stream.read(content.data(), length);
  }
  return content;
}

}  // namespace linkobs::cli
