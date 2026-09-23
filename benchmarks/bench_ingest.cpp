// Link Observatory benchmark.
//
// Measures completed work, not intentions: every number reported here counts
// records that were fully parsed, accepted and (where applicable) classified.
// The final digest is printed so that the measured work cannot be optimised away
// and so that two runs can be compared for equality.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "linkobs/classify/explain.hpp"
#include "linkobs/core/sha256.hpp"
#include "linkobs/core/text.hpp"
#include "linkobs/ingest/codec.hpp"
#include "linkobs/runtime/observatory.hpp"

namespace {

using namespace linkobs;

class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : state_(seed) {}
  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  }
  [[nodiscard]] std::uint64_t bounded(std::uint64_t limit) noexcept {
    return limit == 0U ? 0U : next() % limit;
  }

 private:
  std::uint64_t state_;
};

[[nodiscard]] std::uint64_t option(int argc,
                                   char** argv,
                                   std::string_view name,
                                   std::uint64_t fallback) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string_view{argv[index]} == name) {
      std::uint64_t value = 0;
      if (parse_u64_dec(argv[index + 1], value)) {
        return value;
      }
    }
  }
  return fallback;
}

[[nodiscard]] std::string build_scenario(std::uint64_t records,
                                         std::uint64_t links,
                                         std::uint64_t sources,
                                         std::uint64_t seed) {
  SplitMix64 random{seed};
  std::string text;
  text.reserve(static_cast<std::size_t>(records) * 170U);

  for (std::uint64_t index = 0; index < sources; ++index) {
    text.append("v=1 kind=source name=bench-source-");
    text.append(to_dec(index));
    text.append(" authority=primary provenance=synthetic\n");
  }
  for (std::uint64_t index = 0; index < links; ++index) {
    text.append("v=1 kind=link name=bench-link-");
    text.append(to_dec(index));
    text.append(" link-kind=physical linkgen=gen-0 provenance=synthetic "
                "capacity-in-bps=100000000000\n");
  }

  const std::size_t slot_count =
      static_cast<std::size_t>(links) * static_cast<std::size_t>(sources);
  std::vector<std::uint64_t> octets(slot_count, 0U);
  std::vector<std::uint64_t> revision(static_cast<std::size_t>(sources), 0U);
  std::int64_t observed = 1700000000000000000LL;
  for (std::uint64_t index = 0; index < records; ++index) {
    const std::uint64_t link = index % links;
    const std::uint64_t source = index % sources;
    const std::size_t slot = static_cast<std::size_t>(link * sources + source);
    octets[slot] += 1000000U + random.bounded(1000000U);
    revision[static_cast<std::size_t>(source)] += 1U;
    observed += 1000000;

    text.append("v=1 kind=observation link=bench-link-");
    text.append(to_dec(link));
    text.append(" metric=octets/in source=bench-source-");
    text.append(to_dec(source));
    text.append(" incarnation=inc-0 epoch=epoch-0 generation=gen-0 revision=");
    text.append(to_dec(revision[static_cast<std::size_t>(source)]));
    text.append(" observed-at=");
    text.append(to_dec(observed));
    text.append(" received-at=");
    text.append(to_dec(observed));
    text.append(" counter=");
    text.append(to_dec(octets[slot]));
    text.append(" width=bits64\n");
  }
  return text;
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t records = option(argc, argv, "--records", 200000U);
  const std::uint64_t links = option(argc, argv, "--links", 64U);
  const std::uint64_t sources = option(argc, argv, "--sources", 4U);
  const std::uint64_t workers = option(argc, argv, "--workers", 0U);
  const std::uint64_t seed = option(argc, argv, "--seed", 7U);

  if (records == 0U || links == 0U || sources == 0U) {
    std::cerr << "records, links and sources must be positive\n";
    return 2;
  }
  if (links > 100000U || sources > 100000U || records > 100000000U) {
    std::cerr << "benchmark parameters exceed the supported range\n";
    return 2;
  }

  const std::string scenario = build_scenario(records, links, sources, seed);
  const Policy policy = make_default_policy();

  const std::size_t batch_bound = policy.limits.max_records_per_batch;
  std::vector<std::string> chunks;
  {
    std::size_t cursor = 0;
    std::size_t lines = 0;
    std::size_t start = 0;
    while (cursor < scenario.size()) {
      const std::size_t end = scenario.find('\n', cursor);
      const std::size_t next = (end == std::string::npos) ? scenario.size() : end + 1U;
      ++lines;
      if (lines >= batch_bound) {
        chunks.push_back(scenario.substr(start, next - start));
        start = next;
        lines = 0;
      }
      if (end == std::string::npos) {
        break;
      }
      cursor = next;
    }
    if (start < scenario.size()) {
      chunks.push_back(scenario.substr(start));
    }
  }

  const auto parse_start = std::chrono::steady_clock::now();
  std::vector<Batch> batches;
  std::size_t parsed_records = 0;
  for (const std::string& chunk : chunks) {
    ParseOutcome parsed = parse_batch(chunk, policy);
    if (!parsed.ok()) {
      std::cerr << "parse failed: " << parsed.status.to_string() << "\n";
      return 2;
    }
    parsed_records += parsed.batch.records.size();
    batches.push_back(std::move(parsed.batch));
  }
  const auto parse_end = std::chrono::steady_clock::now();

  RuntimeConfig config{};
  config.workers = static_cast<std::size_t>(workers);
  config.queue_depth = 4096U;
  config.policy = policy;
  config.incarnation_name = "bench";
  auto clock = std::make_unique<ManualClock>(TimePoint::from_nanos(1700000000000000000LL));
  ManualClock* manual = clock.get();
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  if (!started.ok()) {
    std::cerr << "start failed: " << started.to_string() << "\n";
    return 3;
  }

  // With workers configured the benchmark exercises the real worker path and
  // waits for each batch to be applied, so the measurement covers completed work
  // rather than work that was merely queued.
  const bool asynchronous = workers > 0U;
  const auto ingest_start = std::chrono::steady_clock::now();
  for (const Batch& source_batch : batches) {
    if (!asynchronous) {
      const Status applied = observatory.apply_batch(source_batch);
      if (!applied.ok()) {
        std::cerr << "apply failed: " << applied.to_string() << "\n";
        return 4;
      }
      continue;
    }
    Batch batch = source_batch;
    auto completion = std::make_shared<BatchCompletion>();
    batch.on_applied = [completion]() { completion->signal(); };
    const Status submitted = observatory.submit_batch(std::move(batch));
    if (!submitted.ok()) {
      std::cerr << "submit failed: " << submitted.to_string() << "\n";
      return 4;
    }
    completion->wait();
  }
  const auto ingest_end = std::chrono::steady_clock::now();

  manual->advance_by(Duration::from_millis(10));
  const auto classify_start = std::chrono::steady_clock::now();
  const std::vector<Classification> classifications = observatory.classify_all();
  const auto classify_end = std::chrono::steady_clock::now();

  const Status stopped = observatory.stop();
  if (!stopped.ok()) {
    std::cerr << "stop failed: " << stopped.to_string() << "\n";
    return 5;
  }
  const RuntimeStatistics stats = observatory.statistics();

  std::string fingerprint;
  for (const Classification& classification : classifications) {
    fingerprint.append(summarize(classification));
    fingerprint.push_back('\n');
  }
  const std::string digest = to_hex(sha256(fingerprint));

  const auto parse_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(parse_end - parse_start).count();
  const auto ingest_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(ingest_end - ingest_start).count();
  const auto classify_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(classify_end - classify_start).count();

  const auto rate = [](std::int64_t nanos, std::size_t count) -> double {
    if (nanos <= 0 || count == 0U) {
      return 0.0;
    }
    return static_cast<double>(count) * 1e9 / static_cast<double>(nanos);
  };

  std::cout << "records=" << parsed_records << " links=" << links << " sources=" << sources
            << " workers=" << workers << " batches=" << batches.size() << "\n";
  std::cout << "parse_ns=" << parse_ns << " parse_per_second="
            << static_cast<std::uint64_t>(rate(parse_ns, parsed_records)) << "\n";
  std::cout << "ingest_ns=" << ingest_ns << " ingest_per_second="
            << static_cast<std::uint64_t>(rate(ingest_ns, parsed_records))
            << " accepted=" << stats.records_accepted << " fenced=" << stats.records_fenced
            << "\n";
  std::cout << "classify_ns=" << classify_ns << " classifications=" << classifications.size()
            << " classify_per_second="
            << static_cast<std::uint64_t>(rate(classify_ns, classifications.size())) << "\n";
  std::cout << "transitions=" << stats.transitions_recorded
            << " state_changes=" << stats.state_changes << "\n";
  std::cout << "result_digest=" << digest << "\n";
  return 0;
}
