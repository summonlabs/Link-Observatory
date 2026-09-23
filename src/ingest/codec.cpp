#include "linkobs/ingest/codec.hpp"

#include <limits>

#include "linkobs/core/text.hpp"

namespace linkobs {
namespace {

constexpr std::size_t kMaxReportedErrors = 32U;

}  // namespace

ParseOutcome parse_batch(std::string_view text, const Policy& policy) {
  ParseOutcome outcome{};
  std::size_t cursor = 0;
  std::size_t line_number = 0;

  // Iteration stops at the end of the text, so a file that ends with a newline
  // does not produce a phantom trailing line.
  while (cursor < text.size()) {
    const std::size_t end = text.find('\n', cursor);
    const std::string_view raw =
        (end == std::string_view::npos) ? text.substr(cursor) : text.substr(cursor, end - cursor);
    ++line_number;
    const std::string_view line = trim_ascii(raw);
    if (line.empty() || line.front() == '#') {
      outcome.ignored_lines += 1U;
      if (end == std::string_view::npos) {
        break;
      }
      cursor = end + 1U;
      continue;
    }

    if (outcome.batch.records.size() >= policy.limits.max_records_per_batch) {
      outcome.status = Status::of(StatusCode::CapacityExceeded,
                                  "input exceeds the configured records-per-batch bound");
      return outcome;
    }

    const Result<ParsedRecord> parsed = parse_record(line, policy);
    outcome.batch.lines_scanned += 1U;
    if (!parsed.ok()) {
      outcome.rejected += 1U;
      if (outcome.errors.size() < kMaxReportedErrors) {
        std::string message{"line "};
        message.append(to_dec(static_cast<std::uint64_t>(line_number)));
        message.append(": ");
        message.append(parsed.status().to_string());
        outcome.errors.push_back(std::move(message));
      }
    } else {
      outcome.batch.records.push_back(parsed.value());
    }

    if (end == std::string_view::npos) {
      break;
    }
    cursor = end + 1U;
  }

  outcome.status = Status::success();
  return outcome;
}

std::string format_observation_record(std::string_view link_name,
                                      std::string_view source_name,
                                      const Observation& observation) {
  ObservationDecl declaration{};
  declaration.link.assign(link_name);
  declaration.source.assign(source_name);
  declaration.incarnation = observation.provenance.incarnation.to_string();
  declaration.epoch = observation.provenance.epoch.to_string();
  declaration.generation = observation.provenance.generation.to_string();
  declaration.revision = observation.provenance.sequence.value();
  declaration.observed_at = observation.observed_at.nanos();
  declaration.received_at = observation.received_at.nanos();
  declaration.key = observation.key;
  declaration.payload = observation.payload;
  return format_record(declaration);
}

}  // namespace linkobs
