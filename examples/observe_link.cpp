// Example: observe one link, print its classification and its explanation.

#include <iostream>
#include <memory>
#include <string>

#include "linkobs/classify/explain.hpp"
#include "linkobs/ingest/codec.hpp"
#include "linkobs/runtime/observatory.hpp"

int main() {
  using namespace linkobs;

  RuntimeConfig config{};
  config.workers = 0U;
  config.policy = make_default_policy();
  config.incarnation_name = "example";

  auto clock = std::make_unique<ManualClock>(TimePoint::from_nanos(1700000000000000000LL));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  if (!started.ok()) {
    std::cerr << "start: " << started.to_string() << "\n";
    return 1;
  }

  const std::string records =
      "v=1 kind=source name=example-agent authority=primary provenance=synthetic\n"
      "v=1 kind=link name=leaf1-eth0 link-kind=physical linkgen=gen-1 provenance=synthetic "
      "capacity-in-bps=100000000000\n"
      "v=1 kind=observation link=leaf1-eth0 metric=oper-state source=example-agent "
      "incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=1 "
      "observed-at=1700000000000000000 received-at=1700000000000000000 value=up\n"
      "v=1 kind=observation link=leaf1-eth0 metric=octets/in source=example-agent "
      "incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=2 "
      "observed-at=1700000000000000000 received-at=1700000000000000000 counter=0 width=bits64\n"
      "v=1 kind=observation link=leaf1-eth0 metric=octets/in source=example-agent "
      "incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=3 "
      "observed-at=1700000001000000000 received-at=1700000001000000000 counter=1250000000 "
      "width=bits64\n";

  ParseOutcome parsed = parse_batch(records, observatory.config().policy);
  if (!parsed.ok()) {
    std::cerr << "parse: " << parsed.status.to_string() << "\n";
    return 2;
  }
  const Status applied = observatory.apply_batch(parsed.batch);
  if (!applied.ok()) {
    std::cerr << "apply: " << applied.to_string() << "\n";
    return 3;
  }

  const std::optional<Classification> classification = observatory.classify("leaf1-eth0");
  if (!classification.has_value()) {
    std::cerr << "no classification\n";
    return 4;
  }
  std::cout << explain(*classification, ExplainOptions{});

  const Status stopped = observatory.stop();
  return stopped.ok() ? 0 : 5;
}
