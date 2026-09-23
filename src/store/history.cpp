#include "linkobs/store/history.hpp"

#include "linkobs/core/text.hpp"

namespace linkobs {

TransitionId compute_transition_id(const TransitionEvent& event) {
  HashBuilder builder;
  builder.add_str("transition-v1");
  builder.add_hash(event.link.hash());
  builder.add_u8(static_cast<std::uint8_t>(event.kind));
  builder.add_i64(event.at.nanos());
  builder.add_hash(event.generation.hash());
  builder.add_hash(event.source.hash());
  builder.add_str(event.from);
  builder.add_str(event.to);
  builder.add_u64(event.detail);
  return TransitionId::from_hash(builder.finish());
}

void TransitionLog::record(TransitionEvent event) {
  if (event.from.size() > 64U) {
    event.from.resize(64U);
  }
  if (event.to.size() > 64U) {
    event.to.resize(64U);
  }
  event.id = compute_transition_id(event);

  auto& bucket = per_link_[event.link];
  bucket.push_back(event);
  while (bucket.size() > limits_.max_transitions_per_link) {
    bucket.pop_front();
    dropped_ += 1U;
  }

  global_.push_back(std::move(event));
  ++total_records_;
  while (global_.size() > limits_.max_total_transitions) {
    global_.pop_front();
    dropped_ += 1U;
  }
}

std::vector<TransitionEvent> TransitionLog::for_link(LinkId link, std::size_t limit) const {
  std::vector<TransitionEvent> out;
  const auto found = per_link_.find(link);
  if (found == per_link_.end()) {
    return out;
  }
  const std::size_t bound = limit < limits_.max_result_rows ? limit : limits_.max_result_rows;
  const auto& bucket = found->second;
  const std::size_t start = bucket.size() > bound ? bucket.size() - bound : 0U;
  out.reserve(bucket.size() - start);
  for (std::size_t index = start; index < bucket.size(); ++index) {
    out.push_back(bucket[index]);
  }
  return out;
}

std::vector<TransitionEvent> TransitionLog::recent(std::size_t limit) const {
  std::vector<TransitionEvent> out;
  const std::size_t bound = limit < limits_.max_result_rows ? limit : limits_.max_result_rows;
  const std::size_t start = global_.size() > bound ? global_.size() - bound : 0U;
  out.reserve(global_.size() - start);
  for (std::size_t index = start; index < global_.size(); ++index) {
    out.push_back(global_[index]);
  }
  return out;
}

std::size_t TransitionLog::events_for(LinkId link) const {
  const auto found = per_link_.find(link);
  return found == per_link_.end() ? 0U : found->second.size();
}

std::size_t TransitionLog::count_since(LinkId link,
                                       TimePoint since,
                                       std::size_t limit) const {
  const auto found = per_link_.find(link);
  if (found == per_link_.end()) {
    return 0U;
  }
  std::size_t count = 0;
  const auto& bucket = found->second;
  for (auto it = bucket.rbegin(); it != bucket.rend(); ++it) {
    if (it->at < since) {
      break;
    }
    ++count;
    if (count >= limit) {
      break;
    }
  }
  return count;
}

void TransitionLog::clear() {
  global_.clear();
  per_link_.clear();
  total_records_ = 0;
  dropped_ = 0;
}

std::string format_transition(const TransitionEvent& event) {
  TextFields fields(' ');
  fields.add("transition", event.id.to_string());
  fields.add("link", event.link.to_string());
  fields.add("kind", to_string(event.kind));
  fields.add("at", event.at.nanos());
  fields.add("generation", event.generation.to_string());
  if (event.source.valid()) {
    fields.add("source", event.source.to_string());
  }
  if (!event.from.empty()) {
    fields.add("from", event.from);
  }
  if (!event.to.empty()) {
    fields.add("to", event.to);
  }
  if (event.detail != 0U) {
    fields.add("detail", event.detail);
  }
  return fields.str();
}

}  // namespace linkobs
