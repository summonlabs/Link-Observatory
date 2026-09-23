#include "linkobs/runtime/config.hpp"

#include "linkobs/core/text.hpp"

namespace linkobs {

Status validate(const RuntimeConfig& config) {
  if (config.workers > config.policy.limits.max_workers) {
    return Status::of(StatusCode::OutOfRange, "worker count exceeds the configured bound");
  }
  if (config.queue_depth == 0U) {
    return Status::of(StatusCode::InvalidArgument, "queue depth must be positive");
  }
  if (config.runtime_name.empty() || config.runtime_name.size() > 128U) {
    return Status::of(StatusCode::InvalidArgument, "runtime name must be 1..128 bytes");
  }
  if (config.incarnation_name.empty() || config.incarnation_name.size() > 128U) {
    return Status::of(StatusCode::InvalidArgument, "incarnation name must be 1..128 bytes");
  }
  if (config.families.enabled == 0U) {
    return Status::of(StatusCode::InvalidArgument, "every metric family is disabled");
  }
  const Status policy_status = validate(config.policy);
  if (!policy_status.ok()) {
    return policy_status;
  }
  return Status::success();
}

RuntimeId runtime_id_of(const RuntimeConfig& config) {
  return RuntimeId::from_name(config.runtime_name);
}

IncarnationId incarnation_id_of(const RuntimeConfig& config) {
  return IncarnationId::from_name(config.incarnation_name);
}

}  // namespace linkobs
