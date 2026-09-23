// Link Observatory - batch parsing of LOR text.
//
// A batch is parsed as a whole so that every malformed line is reported, not
// just the first. Rejected lines are counted and described; they are never
// silently skipped and never turned into default values.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "linkobs/domain/policy.hpp"
#include "linkobs/ingest/record.hpp"

namespace linkobs {

struct Batch {
  std::vector<ParsedRecord> records{};
  std::size_t lines_scanned{0};
  /// Invoked exactly once, after every record in the batch has been applied.
  /// Empty when the caller does not need a completion signal.
  std::function<void()> on_applied{};
};

struct ParseOutcome {
  Status status{};
  Batch batch{};
  std::vector<std::string> errors{};
  std::size_t rejected{0};
  std::size_t ignored_lines{0};

  [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

/// Parses up to limits.max_records_per_batch records. Exceeding the bound is a
/// CapacityExceeded status; the parsed prefix is still returned for reporting.
[[nodiscard]] ParseOutcome parse_batch(std::string_view text, const Policy& policy);

/// Renders one accepted observation back to canonical LOR text. Used by export.
[[nodiscard]] std::string format_observation_record(std::string_view link_name,
                                                    std::string_view source_name,
                                                    const Observation& observation);

}  // namespace linkobs
