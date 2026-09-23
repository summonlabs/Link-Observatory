#include <algorithm>
#include <iostream>

#include "common.hpp"

#include "linkobs/classify/explain.hpp"
#include "linkobs/domain/evidence.hpp"
#include "linkobs/ingest/codec.hpp"
#include "linkobs/store/history.hpp"

namespace linkobs::cli {
namespace {

struct Loaded {
  int exit_code{static_cast<int>(ExitCode::Ok)};
  std::string message{};
  std::unique_ptr<Observatory> observatory{};
};

[[nodiscard]] Loaded load(const Context& context) {
  Loaded loaded{};
  const std::string db = require_db(context, loaded.exit_code);
  if (loaded.exit_code != static_cast<int>(ExitCode::Ok)) {
    return loaded;
  }
  RuntimeConfig config = make_config(context, db, true, false);
  config.workers = 0U;
  loaded.observatory = std::make_unique<Observatory>(std::move(config), make_clock(context));
  const Status started = loaded.observatory->start();
  if (!started.ok() && started.code() != StatusCode::NotFound) {
    loaded.exit_code = exit_code_for(started.code());
    loaded.message = started.to_string();
  }
  return loaded;
}

[[nodiscard]] std::string state_line(const LinkEntry& entry,
                                     const Classification& classification) {
  TextFields fields(' ');
  fields.add("link", entry.facts.name.empty() ? entry.facts.id.to_string() : entry.facts.name);
  fields.add("id", entry.facts.id.to_string());
  fields.add("state", to_string(classification.state));
  fields.add("freshness", to_string(classification.freshness));
  fields.add("completeness", to_string(classification.completeness));
  fields.add("confidence", to_string(classification.confidence));
  fields.add("generation", classification.generation.to_string());
  fields.add("accepted", entry.accepted_observations);
  fields.add("fenced", entry.fenced_observations);
  return fields.str();
}

}  // namespace

int run_inspect(const Context& context) {
  Loaded loaded = load(context);
  if (loaded.exit_code != static_cast<int>(ExitCode::Ok)) {
    std::cerr << "error: " << loaded.message << "\n";
    return loaded.exit_code;
  }
  Observatory& observatory = *loaded.observatory;

  std::vector<LinkEntry> links = observatory.links();
  std::sort(links.begin(), links.end(), [](const LinkEntry& lhs, const LinkEntry& rhs) {
    return lhs.facts.id < rhs.facts.id;
  });
  const std::optional<std::string> only = context.options.get("link");
  const LinkId filter = only.has_value() ? derive_link_id(*only) : LinkId{};

  std::cout << "runtime " << observatory.runtime_id().to_string()
            << " restored=" << (observatory.restored_from_snapshot() ? "true" : "false")
            << " detail=" << observatory.load_detail() << "\n";

  for (const LinkEntry& entry : links) {
    if (only.has_value() && !(entry.facts.id == filter)) {
      continue;
    }
    const std::optional<Classification> classification = observatory.classify(entry.facts.id);
    if (!classification.has_value()) {
      continue;
    }
    std::cout << state_line(entry, *classification) << "\n";
    for (const SlotView& view : observatory.slot_views(entry.facts.id)) {
      std::cout << "  slot " << format_slot_view(SlotKey{entry.facts.id, view.key}, view) << "\n";
    }
  }

  const RuntimeStatistics stats = observatory.statistics();
  std::cout << "totals accepted=" << stats.records_accepted << " fenced=" << stats.records_fenced
            << " declarations=" << stats.declarations_applied
            << " transitions=" << stats.transitions_recorded
            << " state-changes=" << stats.state_changes << "\n";
  return static_cast<int>(ExitCode::Ok);
}

int run_history(const Context& context) {
  Loaded loaded = load(context);
  if (loaded.exit_code != static_cast<int>(ExitCode::Ok)) {
    std::cerr << "error: " << loaded.message << "\n";
    return loaded.exit_code;
  }
  Observatory& observatory = *loaded.observatory;
  const std::uint64_t limit = context.options.get_u64("limit", 64U);
  const std::optional<std::string> only = context.options.get("link");

  if (only.has_value()) {
    for (const TransitionEvent& event :
         observatory.transitions(derive_link_id(*only), static_cast<std::size_t>(limit))) {
      std::cout << format_transition(event) << "\n";
    }
  } else {
    for (const TransitionEvent& event :
         observatory.recent_transitions(static_cast<std::size_t>(limit))) {
      std::cout << format_transition(event) << "\n";
    }
  }
  return static_cast<int>(ExitCode::Ok);
}

int run_classify(const Context& context) {
  Loaded loaded = load(context);
  if (loaded.exit_code != static_cast<int>(ExitCode::Ok)) {
    std::cerr << "error: " << loaded.message << "\n";
    return loaded.exit_code;
  }
  Observatory& observatory = *loaded.observatory;
  const std::optional<std::string> only = context.options.get("link");

  if (only.has_value()) {
    const std::optional<Classification> classification = observatory.classify(*only);
    if (!classification.has_value()) {
      std::cerr << "error: unknown link\n";
      return static_cast<int>(ExitCode::Data);
    }
    std::cout << summarize(*classification) << "\n";
    return static_cast<int>(ExitCode::Ok);
  }

  std::vector<Classification> all = observatory.classify_all();
  std::sort(all.begin(), all.end(), [](const Classification& lhs, const Classification& rhs) {
    return lhs.link < rhs.link;
  });
  for (const Classification& classification : all) {
    std::cout << summarize(classification) << "\n";
  }
  return static_cast<int>(ExitCode::Ok);
}

int run_explain(const Context& context) {
  Loaded loaded = load(context);
  if (loaded.exit_code != static_cast<int>(ExitCode::Ok)) {
    std::cerr << "error: " << loaded.message << "\n";
    return loaded.exit_code;
  }
  const std::optional<std::string> only = context.options.get("link");
  if (!only.has_value()) {
    std::cerr << "error: --link NAME is required\n";
    return static_cast<int>(ExitCode::Usage);
  }
  Observatory& observatory = *loaded.observatory;
  const std::optional<Classification> classification = observatory.classify(*only);
  if (!classification.has_value()) {
    std::cerr << "error: unknown link\n";
    return static_cast<int>(ExitCode::Data);
  }
  ExplainOptions options{};
  options.include_families = !context.options.has("no-families");
  options.include_rules = !context.options.has("no-rules");
  std::cout << explain(*classification, options);
  return static_cast<int>(ExitCode::Ok);
}

int run_export(const Context& context) {
  Loaded loaded = load(context);
  if (loaded.exit_code != static_cast<int>(ExitCode::Ok)) {
    std::cerr << "error: " << loaded.message << "\n";
    return loaded.exit_code;
  }
  Observatory& observatory = *loaded.observatory;
  const std::string format = context.options.get_or("format", "summary");
  const std::optional<std::string> only = context.options.get("link");
  const LinkId filter = only.has_value() ? derive_link_id(*only) : LinkId{};

  std::vector<LinkEntry> links = observatory.links();
  std::sort(links.begin(), links.end(), [](const LinkEntry& lhs, const LinkEntry& rhs) {
    return lhs.facts.id < rhs.facts.id;
  });

  if (format == "summary") {
    for (const LinkEntry& entry : links) {
      if (only.has_value() && !(entry.facts.id == filter)) {
        continue;
      }
      const std::optional<Classification> classification = observatory.classify(entry.facts.id);
      if (classification.has_value()) {
        std::cout << summarize(*classification) << "\n";
      }
    }
    return static_cast<int>(ExitCode::Ok);
  }

  if (format == "lor" || format == "csv") {
    if (format == "csv") {
      std::cout << "link,metric,source,incarnation,epoch,generation,revision,observed_at,"
                   "received_at,value\n";
    }
    for (const LinkEntry& entry : links) {
      if (only.has_value() && !(entry.facts.id == filter)) {
        continue;
      }
      for (const Evidence& evidence : observatory.link_evidence(entry.facts.id, 4096U)) {
        const Observation& observation = evidence.observation;
        if (format == "lor") {
          std::cout << format_observation_record(
                           entry.facts.name, observation.provenance.source.to_string(), observation)
                    << "\n";
        } else {
          std::cout << entry.facts.name << ',' << observation.key.to_string() << ','
                    << observation.provenance.source.to_string() << ','
                    << observation.provenance.incarnation.to_string() << ','
                    << observation.provenance.epoch.to_string() << ','
                    << observation.provenance.generation.to_string() << ','
                    << observation.provenance.sequence.value() << ','
                    << observation.observed_at.nanos() << ',' << observation.received_at.nanos()
                    << ',' << format_payload(observation.payload) << "\n";
        }
      }
    }
    return static_cast<int>(ExitCode::Ok);
  }

  std::cerr << "error: unknown export format '" << format << "'\n";
  return static_cast<int>(ExitCode::Usage);
}

}  // namespace linkobs::cli
