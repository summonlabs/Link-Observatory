// Downstream consumer smoke test.
//
// Exercises the installed public API only: no internal header, no source tree
// include path, no build directory artifact.

#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "linkobs/classify/explain.hpp"
#include "linkobs/ingest/codec.hpp"
#include "linkobs/runtime/observatory.hpp"
#include "linkobs/version.hpp"

namespace {

[[nodiscard]] int run() {
  using namespace linkobs;

  RuntimeConfig config{};
  config.workers = 0U;
  config.policy = make_default_policy();
  config.incarnation_name = "downstream";
  auto clock = std::make_unique<ManualClock>(TimePoint::from_nanos(1700000000000000000LL));
  Observatory observatory(std::move(config), std::move(clock));

  const Status started = observatory.start();
  if (!started.ok()) {
    std::cerr << "start failed: " << started.to_string() << "\n";
    return 1;
  }

  const std::string scenario =
      "v=1 kind=source name=downstream-agent authority=primary provenance=synthetic\n"
      "v=1 kind=link name=downstream-link link-kind=physical linkgen=gen-1 "
      "provenance=synthetic capacity-in-bps=100000000000\n"
      "v=1 kind=observation link=downstream-link metric=oper-state source=downstream-agent "
      "incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=1 "
      "observed-at=1700000000000000000 received-at=1700000000000000000 value=up\n";

  ParseOutcome parsed = parse_batch(scenario, observatory.config().policy);
  if (!parsed.ok()) {
    std::cerr << "parse failed: " << parsed.status.to_string() << "\n";
    return 2;
  }
  const Status applied = observatory.apply_batch(parsed.batch);
  if (!applied.ok()) {
    std::cerr << "apply failed: " << applied.to_string() << "\n";
    return 3;
  }

  const std::optional<Classification> classification = observatory.classify("downstream-link");
  if (!classification.has_value()) {
    std::cerr << "no classification\n";
    return 4;
  }
  if (classification->state != LinkState::Healthy) {
    std::cerr << "unexpected state: " << to_string(classification->state) << "\n";
    return 5;
  }

  const std::string text = explain(*classification, ExplainOptions{});
  if (text.find("state: healthy") == std::string::npos) {
    std::cerr << "explanation missing the state line\n";
    return 6;
  }

  const Status stopped = observatory.stop();
  if (!stopped.ok()) {
    std::cerr << "stop failed: " << stopped.to_string() << "\n";
    return 7;
  }

  std::cout << "downstream ok: version=" << kVersionString
            << " snapshot-format=" << kSnapshotFormatVersion
            << " state=" << to_string(classification->state) << "\n";
  return 0;
}

}  // namespace

int main() { return run(); }
