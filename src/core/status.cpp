#include "linkobs/core/status.hpp"

namespace linkobs {

std::string_view to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok:
      return "ok";
    case StatusCode::InvalidArgument:
      return "invalid_argument";
    case StatusCode::NotFound:
      return "not_found";
    case StatusCode::AlreadyExists:
      return "already_exists";
    case StatusCode::OutOfRange:
      return "out_of_range";
    case StatusCode::CapacityExceeded:
      return "capacity_exceeded";
    case StatusCode::CorruptData:
      return "corrupt_data";
    case StatusCode::Unsupported:
      return "unsupported";
    case StatusCode::Rejected:
      return "rejected";
    case StatusCode::Cancelled:
      return "cancelled";
    case StatusCode::Conflict:
      return "conflict";
    case StatusCode::IoError:
      return "io_error";
    case StatusCode::VersionMismatch:
      return "version_mismatch";
    case StatusCode::Internal:
      return "internal";
  }
  return "unknown";
}

std::string Status::to_string() const {
  std::string out{linkobs::to_string(code_)};
  if (!message_.empty()) {
    out.append(": ");
    out.append(message_);
  }
  return out;
}

}  // namespace linkobs
