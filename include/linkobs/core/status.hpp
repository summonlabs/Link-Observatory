// Link Observatory - status and result types.
//
// The runtime does not use exceptions for control flow. Fallible operations
// return Status (no value) or Result<T> (value or Status).

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace linkobs {

enum class StatusCode : std::uint8_t {
  Ok = 0,
  InvalidArgument,
  NotFound,
  AlreadyExists,
  OutOfRange,
  CapacityExceeded,
  CorruptData,
  Unsupported,
  Rejected,
  Cancelled,
  Conflict,
  IoError,
  VersionMismatch,
  Internal,
};

[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

/// True for codes that indicate malformed or untrustworthy external input.
[[nodiscard]] constexpr bool is_data_fault(StatusCode code) noexcept {
  return code == StatusCode::CorruptData || code == StatusCode::VersionMismatch ||
         code == StatusCode::OutOfRange || code == StatusCode::InvalidArgument;
}

class Status {
 public:
  Status() noexcept = default;
  Status(StatusCode code, std::string message) noexcept
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] static Status success() noexcept { return Status{}; }
  [[nodiscard]] static Status of(StatusCode code, std::string_view message) {
    return Status{code, std::string{message}};
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] std::string to_string() const;

 private:
  StatusCode code_{StatusCode::Ok};
  std::string message_{};
};

/// A value or the reason it is unavailable.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

  [[nodiscard]] const T& value() const noexcept { return *value_; }
  [[nodiscard]] T& value() noexcept { return *value_; }
  [[nodiscard]] const T* operator->() const noexcept { return &*value_; }

  [[nodiscard]] T value_or(T fallback) const {
    return value_.has_value() ? *value_ : std::move(fallback);
  }

 private:
  std::optional<T> value_{};
  Status status_{};
};

/// Convenience: builds a failure result with a fixed code.
template <class T>
[[nodiscard]] Result<T> failure(StatusCode code, std::string_view message) {
  return Result<T>{Status::of(code, message)};
}

}  // namespace linkobs
