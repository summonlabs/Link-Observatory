#include "linkobs/classify/explain.hpp"

#include "linkobs/core/text.hpp"

namespace linkobs {
namespace {

void append_line(std::string& out, std::string_view key, std::string_view value) {
  out.append(key);
  out.append(": ");
  out.append(value);
  out.push_back('\n');
}

void append_line(std::string& out, std::string_view key, std::uint64_t value) {
  append_line(out, key, std::string_view{to_dec(value)});
}

[[nodiscard]] std::string support_text(SupportState support) {
  return std::string{to_string(support)};
}

}  // namespace

std::string explain(const Classification& classification, const ExplainOptions& options) {
  std::string out;
  out.reserve(2048U);

  append_line(out, "link", classification.link.to_string());
  append_line(out, "state", to_string(classification.state));
  if (options.include_last_known) {
    if (classification.has_last_known) {
      append_line(out, "last-known-state", to_string(classification.last_known_state));
    } else {
      append_line(out, "last-known-state", "none");
    }
  }
  append_line(out, "decided-at", classification.decided_at.nanos());
  append_line(out, "generation", classification.generation.to_string());
  append_line(out, "generation-changes", classification.generation_changes);
  append_line(out, "retired", classification.retired);
  append_line(out, "freshness", to_string(classification.freshness));
  append_line(out, "completeness", to_string(classification.completeness));
  append_line(out, "confidence", to_string(classification.confidence));
  append_line(out, "support", support_text(classification.support));
  append_line(out, "accepted-observations", classification.accepted_total);
  append_line(out, "fenced-observations", classification.fenced_total);
  append_line(out, "flap-events-in-window", classification.flap_events);
  append_line(out, "transitions-retained", static_cast<std::uint64_t>(classification.transitions));
  append_line(out, "transitions-dropped", classification.dropped_transitions);

  if (options.include_families) {
    for (const auto& report : classification.families) {
      out.append("metric ");
      out.append(report.key.to_string());
      out.append(": support=");
      out.append(to_string(report.support));
      out.append(" freshness=");
      out.append(to_string(report.freshness));
      out.append(" completeness=");
      out.append(to_string(report.completeness));
      out.append(" conflict=");
      out.append(to_string(report.conflict));
      out.append(" confidence=");
      out.append(to_string(report.confidence));
      out.append(" contenders=");
      out.append(to_dec(static_cast<std::uint64_t>(report.contenders)));
      out.append(" sources=");
      out.append(to_dec(static_cast<std::uint64_t>(report.sources)));
      out.append(" accepted=");
      out.append(to_dec(report.accepted));
      out.append(" fenced=");
      out.append(to_dec(report.fenced));
      if (report.has_value) {
        out.append(" eligible=true value=");
        out.append(report.value_text);
        out.append(" source=");
        out.append(report.winner_source.to_string());
        out.append(" provenance=");
        out.append(to_string(report.winner_provenance));
        out.append(" observed-at=");
        out.append(to_dec(report.winner_observed_at.nanos()));
        out.append(" received-at=");
        out.append(to_dec(report.winner_received_at.nanos()));
      } else {
        out.append(" eligible=false value=none");
      }
      out.append(" delta-derived=");
      out.append(report.has_delta ? "true" : "false");
      if (report.has_derived) {
        out.append(" derived-utilization-ppb=");
        out.append(to_dec(report.derived_ppb));
      } else {
        out.append(" derived-utilization=none");
      }
      out.append(" derivation-fault=");
      out.append(to_string(report.derivation_fault));
      if (report.has_ratio) {
        out.append(" ratio-ppb=");
        out.append(to_dec(report.ratio_ppb));
      }
      out.append(" counter-resets=");
      out.append(to_dec(report.reset_count));
      out.append(" counter-wraps=");
      out.append(to_dec(report.wrap_count));
      out.push_back('\n');
    }
  }

  if (options.include_rules) {
    for (const auto& rule : classification.rules) {
      out.append("rule ");
      out.append(to_dec(static_cast<std::uint64_t>(static_cast<std::uint32_t>(rule.rule))));
      out.append(" ");
      out.append(rule_name(rule.rule));
      out.append(": fired=");
      out.append(rule.fired ? "true" : "false");
      if (!rule.detail.empty()) {
        out.append(" detail=");
        out.append(rule.detail);
      }
      out.push_back('\n');
    }
  }
  return out;
}

std::string summarize(const Classification& classification) {
  TextFields fields(' ');
  fields.add("link", classification.link.to_string());
  fields.add("state", to_string(classification.state));
  fields.add("freshness", to_string(classification.freshness));
  fields.add("completeness", to_string(classification.completeness));
  fields.add("confidence", to_string(classification.confidence));
  fields.add("generation", classification.generation.to_string());
  fields.add("decided-at", classification.decided_at.nanos());
  fields.add("families", static_cast<std::uint64_t>(classification.families.size()));
  return fields.str();
}

}  // namespace linkobs
