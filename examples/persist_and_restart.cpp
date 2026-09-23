// Example: persist evidence, restart, and show that old evidence is not fresh.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "linkobs/classify/explain.hpp"
#include "linkobs/ingest/codec.hpp"
#include "linkobs/runtime/observatory.hpp"

namespace {

constexpr std::string_view kRecords =
    "v=1 kind=source name=example-agent authority=primary provenance=synthetic\n"
    "v=1 kind=link name=leaf1-eth0 link-kind=physical linkgen=gen-1 provenance=synthetic "
    "capacity-in-bps=100000000000\n"
    "v=1 kind=observation link=leaf1-eth0 metric=oper-state source=example-agent "
    "incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=1 "
    "observed-at=1700000000000000000 received-at=1700000000000000000 value=down\n";

[[nodiscard]] std::string temporary_path() {
  std::error_code error;
  const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
  const std::filesystem::path path =
      (error ? std::filesystem::path{"."} : directory) / "linkobs-example.lkos";
  return path.string();
}

}  // namespace

int main() {
  using namespace linkobs;
  const std::string path = temporary_path();
  (void)std::remove(path.c_str());
  (void)std::remove((path + ".prev").c_str());

  {
    RuntimeConfig config{};
    config.snapshot_path = path;
    config.policy = make_default_policy();
    config.incarnation_name = "first-process";
    auto clock = std::make_unique<ManualClock>(TimePoint::from_nanos(1700000000000000000LL));
    Observatory observatory(std::move(config), std::move(clock));
    const Status started = observatory.start();
    if (!started.ok()) {
      std::cerr << "start: " << started.to_string() << "\n";
      return 1;
    }
    ParseOutcome parsed = parse_batch(kRecords, observatory.config().policy);
    const Status applied = observatory.apply_batch(parsed.batch);
    if (!applied.ok()) {
      std::cerr << "apply: " << applied.to_string() << "\n";
      return 2;
    }
    const Status stopped = observatory.stop();
    if (!stopped.ok()) {
      std::cerr << "stop: " << stopped.to_string() << "\n";
      return 3;
    }
    std::cout << "first process: evidence written to " << path << "\n";
  }

  {
    RuntimeConfig config{};
    config.snapshot_path = path;
    config.policy = make_default_policy();
    config.incarnation_name = "second-process";
    // An hour later.
    auto clock = std::make_unique<ManualClock>(TimePoint::from_nanos(1700003600000000000LL));
    Observatory observatory(std::move(config), std::move(clock));
    const Status started = observatory.start();
    if (!started.ok()) {
      std::cerr << "restart: " << started.to_string() << "\n";
      return 4;
    }
    const std::optional<Classification> classification = observatory.classify("leaf1-eth0");
    if (!classification.has_value()) {
      std::cerr << "no classification\n";
      return 5;
    }
    std::cout << "second process: restored="
              << (observatory.restored_from_snapshot() ? "true" : "false")
              << " state=" << to_string(classification->state)
              << " freshness=" << to_string(classification->freshness)
              << " last-known=" << to_string(classification->last_known_state) << "\n";
    const Status stopped = observatory.stop();
    (void)stopped;
  }

  (void)std::remove(path.c_str());
  (void)std::remove((path + ".prev").c_str());
  return 0;
}
