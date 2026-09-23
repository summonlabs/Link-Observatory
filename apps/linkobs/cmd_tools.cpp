#include <chrono>
#include <iostream>
#include <memory>
#include <vector>

#include "common.hpp"

#include "linkobs/classify/explain.hpp"
#include "linkobs/core/sha256.hpp"
#include "linkobs/ingest/codec.hpp"
#include "linkobs/version.hpp"

namespace linkobs::cli {
namespace {

/// Deterministic splitmix64, used only to generate benchmark and self test
/// input. It is never used for any decision the runtime makes.
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

[[nodiscard]] std::string build_scenario(std::uint64_t records,
                                         std::uint64_t links,
                                         std::uint64_t sources,
                                         std::uint64_t seed) {
  SplitMix64 random{seed};
  std::string text;
  text.reserve(static_cast<std::size_t>(records) * 160U);

  for (std::uint64_t index = 0; index < sources; ++index) {
    text.append("v=1 kind=source name=source-");
    text.append(to_dec(index));
    text.append(" authority=primary provenance=synthetic\n");
  }
  for (std::uint64_t index = 0; index < links; ++index) {
    text.append("v=1 kind=link name=link-");
    text.append(to_dec(index));
    text.append(" link-kind=physical linkgen=gen-0 provenance=synthetic capacity-in-bps=100000000000"
                " capacity-out-bps=100000000000\n");
  }

  // Octet counters are per (link, source). Revisions are per source: a
  // sequence number belongs to the source stream, not to a link.
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

    text.append("v=1 kind=observation link=link-");
    text.append(to_dec(link));
    text.append(" metric=octets/in source=source-");
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

int run_version(const Context& context) {
  (void)context;
  std::cout << "product=" << kProductName << " version=" << kVersionString
            << " vendor=" << kVendorName << "\n";
  std::cout << "snapshot-format=" << kSnapshotFormatVersion
            << " transport-protocol=" << kTransportProtocolVersion
            << " record-format=" << kRecordFormatVersion << "\n";
  std::cout << "copyright=\"" << kCopyrightNotice << "\"\n";
  return static_cast<int>(ExitCode::Ok);
}

int run_verify(const Context& context) {
  int exit_code = static_cast<int>(ExitCode::Ok);
  const std::string db = require_db(context, exit_code);
  if (exit_code != static_cast<int>(ExitCode::Ok)) {
    return exit_code;
  }
  const Policy policy = make_default_policy();
  const SnapshotLoadResult loaded = read_snapshot(db, policy);
  if (!loaded.ok) {
    std::cout << "verify=FAIL status=" << to_string(loaded.status.code())
              << " detail=\"" << loaded.detail << "\"\n";
    return exit_code_for(loaded.status.code());
  }
  std::size_t evidence = 0;
  for (const PersistedSlot& slot : loaded.contents.slots) {
    evidence += slot.history.size();
  }
  std::cout << "verify=OK integrity=true bytes=" << loaded.bytes_read
            << " format=" << loaded.contents.format_version << " links="
            << loaded.contents.links.size() << " sources=" << loaded.contents.sources.size()
            << " slots=" << loaded.contents.slots.size()
            << " evidence=" << evidence
            << " transitions=" << loaded.contents.transitions.size()
            << " recovered-from-backup=" << (loaded.recovered_from_backup ? "true" : "false")
            << "\n";
  if (loaded.recovered_from_backup) {
    std::cout << "recovery=\"" << loaded.detail << "\"\n";
    return static_cast<int>(ExitCode::Integrity);
  }
  return static_cast<int>(ExitCode::Ok);
}

int run_selftest(const Context& context) {
  const std::uint64_t seed = context.options.get_u64("seed", 1U);
  int failures = 0;

  const auto check = [&failures](std::string_view name, bool condition) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << "\n";
    if (!condition) {
      ++failures;
    }
  };

  const std::string scenario = build_scenario(8U, 1U, 1U, seed);
  RuntimeConfig config{};
  config.workers = 0U;
  config.policy = make_default_policy();
  config.incarnation_name = "selftest";
  auto clock = std::make_unique<ManualClock>(TimePoint::from_nanos(1700000000000000000LL));
  ManualClock* manual = clock.get();
  Observatory observatory(config, std::move(clock));

  const Status started = observatory.start();
  check("runtime-starts", started.ok());

  ParseOutcome parsed = parse_batch(scenario, observatory.config().policy);
  check("scenario-parses", parsed.ok() && parsed.rejected == 0U);

  manual->set(TimePoint::from_nanos(1700000000000000000LL));
  const Status applied = observatory.apply_batch(parsed.batch);
  check("scenario-applies", applied.ok());

  manual->advance_by(Duration::from_seconds(120));
  const std::optional<Classification> classification = observatory.classify("link-0");
  check("classification-present", classification.has_value());
  if (classification.has_value()) {
    check("no-utilization-without-capacity-derivation",
          classification->state == LinkState::Stale ||
              classification->state == LinkState::Unknown);
  }

  const RuntimeStatistics stats = observatory.statistics();
  check("records-accepted", stats.records_accepted == 8U);
  check("no-fences", stats.records_fenced == 0U);

  std::cout << (failures == 0 ? "selftest=PASS" : "selftest=FAIL") << " failures=" << failures
            << "\n";
  return failures == 0 ? static_cast<int>(ExitCode::Ok)
                       : static_cast<int>(ExitCode::Internal);
}

int run_bench(const Context& context) {
  const std::uint64_t records = context.options.get_u64("records", 200000U);
  const std::uint64_t links = context.options.get_u64("links", 64U);
  const std::uint64_t sources = context.options.get_u64("sources", 4U);
  const std::uint64_t seed = context.options.get_u64("seed", 7U);

  if (records == 0U || links == 0U || sources == 0U) {
    std::cerr << "error: --records, --links and --sources must be positive\n";
    return static_cast<int>(ExitCode::Usage);
  }
  if (links > 100000U || sources > 100000U || records > 100000000U) {
    std::cerr << "error: benchmark parameters exceed the supported range\n";
    return static_cast<int>(ExitCode::Usage);
  }

  const std::string scenario = build_scenario(records, links, sources, seed);
  const Policy policy = make_default_policy();

  // The ingest bound is respected rather than bypassed: the scenario is split
  // into batches that the runtime would accept from a real source.
  const std::size_t batch_bound = policy.limits.max_records_per_batch;
  std::vector<std::string> chunks;
  {
    std::size_t cursor = 0;
    std::size_t lines = 0;
    std::size_t start = 0;
    while (cursor < scenario.size()) {
      const std::size_t end = scenario.find('\n', cursor);
      const std::size_t next = (end == std::string::npos) ? scenario.size() : end + 1U;
      if (scenario[cursor] != '#') {
        ++lines;
      }
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

  std::vector<Batch> batches;
  batches.reserve(chunks.size());
  const auto parse_start = std::chrono::steady_clock::now();
  std::size_t parsed_records = 0;
  for (const std::string& chunk : chunks) {
    ParseOutcome parsed = parse_batch(chunk, policy);
    if (!parsed.ok()) {
      std::cerr << "error: benchmark scenario did not parse: " << parsed.status.to_string()
                << "\n";
      return static_cast<int>(ExitCode::Data);
    }
    parsed_records += parsed.batch.records.size();
    batches.push_back(std::move(parsed.batch));
  }
  const auto parse_end = std::chrono::steady_clock::now();

  RuntimeConfig config{};
  config.workers = static_cast<std::size_t>(context.options.get_u64("workers", 1U));
  config.queue_depth = static_cast<std::size_t>(context.options.get_u64("queue-depth", 4096U));
  config.policy = policy;
  config.incarnation_name = "bench";
  auto clock = std::make_unique<ManualClock>(TimePoint::from_nanos(1700000000000000000LL));
  ManualClock* manual = clock.get();
  Observatory observatory(config, std::move(clock));
  const Status started = observatory.start();
  if (!started.ok()) {
    std::cerr << "error: " << started.to_string() << "\n";
    return exit_code_for(started.code());
  }

  // With --workers the benchmark drives the real worker path and waits for each
  // batch to be applied, so it measures completed work and not queue depth.
  const bool asynchronous = config.workers > 0U;
  const auto ingest_start = std::chrono::steady_clock::now();
  for (const Batch& source_batch : batches) {
    if (!asynchronous) {
      const Status applied = observatory.apply_batch(source_batch);
      if (!applied.ok()) {
        std::cerr << "error: " << applied.to_string() << "\n";
        return exit_code_for(applied.code());
      }
      continue;
    }
    Batch batch = source_batch;
    auto completion = std::make_shared<BatchCompletion>();
    batch.on_applied = [completion]() { completion->signal(); };
    const Status submitted = observatory.submit_batch(std::move(batch));
    if (!submitted.ok()) {
      std::cerr << "error: " << submitted.to_string() << "\n";
      return exit_code_for(submitted.code());
    }
    completion->wait();
  }
  const auto ingest_end = std::chrono::steady_clock::now();

  manual->advance_by(Duration::from_millis(10));
  const auto classify_start = std::chrono::steady_clock::now();
  const std::vector<Classification> classifications = observatory.classify_all();
  const auto classify_end = std::chrono::steady_clock::now();

  const Status stopped = observatory.stop();
  const RuntimeStatistics stats = observatory.statistics();

  // A checksum over completed work, printed so that the measured work cannot be
  // optimised away and so that two runs can be compared.
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

  const auto per_record = [](std::int64_t nanos, std::size_t count) -> double {
    return count == 0U ? 0.0 : static_cast<double>(nanos) / static_cast<double>(count);
  };

  std::cout << "records=" << parsed_records << " links=" << links << " sources=" << sources
            << " workers=" << config.workers << "\n";
  std::cout << "parse_ns=" << parse_ns << " parse_ns_per_record="
            << static_cast<std::uint64_t>(per_record(parse_ns, parsed_records)) << "\n";
  std::cout << "ingest_ns=" << ingest_ns << " ingest_ns_per_record="
            << static_cast<std::uint64_t>(per_record(ingest_ns, parsed_records))
            << " ingest_records_per_second="
            << static_cast<std::uint64_t>(per_record(ingest_ns, parsed_records) > 0.0
                                              ? 1e9 / per_record(ingest_ns, parsed_records)
                                              : 0.0)
            << "\n";
  std::cout << "classify_ns=" << classify_ns << " classifications=" << classifications.size()
            << "\n";
  std::cout << "accepted=" << stats.records_accepted << " fenced=" << stats.records_fenced
            << " transitions=" << stats.transitions_recorded << "\n";
  std::cout << "result_digest=" << digest << "\n";
  (void)stopped;
  return static_cast<int>(ExitCode::Ok);
}

}  // namespace linkobs::cli
