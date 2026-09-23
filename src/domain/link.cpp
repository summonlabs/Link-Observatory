#include "linkobs/domain/link.hpp"

#include "linkobs/core/text.hpp"

namespace linkobs {

LinkId derive_link_id(std::string_view name) { return LinkId::from_name(name); }

bool capacity_for(const LinkFacts& facts, Direction direction, std::uint64_t& out) noexcept {
  if (!facts.capacity_known) {
    return false;
  }
  switch (direction) {
    case Direction::In:
      if (facts.capacity_in_bps == 0U) {
        return false;
      }
      out = facts.capacity_in_bps;
      return true;
    case Direction::Out:
      if (facts.capacity_out_bps == 0U) {
        return false;
      }
      out = facts.capacity_out_bps;
      return true;
    case Direction::Both:
    case Direction::Unknown:
      return false;
  }
  return false;
}

Status validate_link_facts(const LinkFacts& facts, const Policy& policy) {
  (void)policy;
  if (!facts.id.valid()) {
    return Status::of(StatusCode::InvalidArgument, "link facts have no link identity");
  }
  if (facts.name.empty()) {
    return Status::of(StatusCode::InvalidArgument, "link facts have no name");
  }
  if (facts.name.size() > 128U) {
    return Status::of(StatusCode::OutOfRange, "link name exceeds 128 bytes");
  }
  if (facts.capacity_known) {
    if (facts.capacity_in_bps == 0U && facts.capacity_out_bps == 0U) {
      return Status::of(StatusCode::InvalidArgument,
                        "link declares known capacity but both directions are zero");
    }
  } else if (facts.capacity_in_bps != 0U || facts.capacity_out_bps != 0U) {
    return Status::of(StatusCode::InvalidArgument,
                      "link has capacity values but does not declare capacity as known");
  }
  return Status::success();
}

std::string format_link_facts(const LinkFacts& facts) {
  TextFields fields(' ');
  fields.add("link", facts.id.to_string());
  fields.add("name", facts.name);
  fields.add("kind", to_string(facts.kind));
  fields.add("generation", facts.generation.to_string());
  fields.add("capacity-known", facts.capacity_known);
  fields.add("capacity-in-bps", facts.capacity_in_bps);
  fields.add("capacity-out-bps", facts.capacity_out_bps);
  fields.add("provenance", to_string(facts.provenance));
  if (facts.local_endpoint.valid()) {
    fields.add("local-endpoint", facts.local_endpoint.to_string());
  }
  if (facts.remote_endpoint.valid()) {
    fields.add("remote-endpoint", facts.remote_endpoint.to_string());
  }
  if (facts.local_port.valid()) {
    fields.add("local-port", facts.local_port.to_string());
  }
  if (facts.remote_port.valid()) {
    fields.add("remote-port", facts.remote_port.to_string());
  }
  return fields.str();
}

}  // namespace linkobs
