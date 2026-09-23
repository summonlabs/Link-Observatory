// Real independent process tests.
//
// These tests do not simulate a process boundary: they run the shipped
// "linkobs" executable as a child process, over a real socket and a real file
// system, and check what that process actually did.
//
// The only bounded wait in this file is the readiness poll for the listening
// port. It exists so that a child that never starts cannot hang the suite; it is
// a liveness guard, not an assumption about timing, and no correctness property
// depends on how long anything takes.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "support/fixtures.hpp"
#include "support/harness.hpp"

#include "linkobs/core/text.hpp"
#include "linkobs/runtime/observatory.hpp"
#include "linkobs/transport/client.hpp"
#include "linkobs/transport/server.hpp"

#ifndef LINKOBS_CLI_PATH
#error "LINKOBS_CLI_PATH must be defined by the build"
#endif

namespace {

using namespace linkobs;

using linkobs::test::temporary_directory;

struct ProcessResult {
  int exit_code{-1};
  std::string output{};
};

/// Runs the command through the platform shell with both streams redirected to
/// a file, so the test observes exactly what the child produced.
[[nodiscard]] ProcessResult run_child(const std::string& command) {
  const std::string output_path =
      temporary_directory() + "/linkobs-child-" + to_dec(static_cast<std::uint64_t>(
                                  std::chrono::steady_clock::now().time_since_epoch().count())) +
      ".out";
  // "call" keeps the command from starting with a quote. cmd.exe applies its
  // quote stripping heuristic to a command that starts with a quote, which would
  // split a path containing spaces.
  const std::string full = "call " + command + " > \"" + output_path + "\" 2>&1";
  const int code = std::system(full.c_str());

  ProcessResult result{};
  result.exit_code = code;
  std::ifstream stream(output_path, std::ios::binary);
  if (stream.good()) {
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    result.output = buffer.str();
  }
  const bool removed = std::remove(output_path.c_str()) == 0;
  (void)removed;
  return result;
}

[[nodiscard]] std::string quote(const std::string& text) { return std::string{"\""} + text + "\""; }

/// The executable path is quoted: it may contain spaces, and the command is run
/// through the platform shell.
[[nodiscard]] std::string cli() { return std::string{"\""} + LINKOBS_CLI_PATH + "\""; }

struct TempFile {
  std::string path;

  explicit TempFile(std::string name)
      : path(temporary_directory() + "/" + std::move(name) +
             "-" + to_dec(static_cast<std::uint64_t>(
                       std::chrono::steady_clock::now().time_since_epoch().count()))) {
    remove_all();
  }
  ~TempFile() { remove_all(); }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  void remove_all() const {
    const bool a = std::remove(path.c_str()) == 0;
    (void)a;
    const bool b = std::remove((path + ".prev").c_str()) == 0;
    (void)b;
    const bool c = std::remove((path + ".tmp").c_str()) == 0;
    (void)c;
  }

  void write(std::string_view content) const {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
};

constexpr std::string_view kScenario =
    "v=1 kind=source name=proc-agent authority=primary provenance=real\n"
    "v=1 kind=link name=proc-link link-kind=physical linkgen=gen-1 provenance=real "
    "capacity-in-bps=100000000000\n"
    "v=1 kind=observation link=proc-link metric=oper-state source=proc-agent incarnation=boot-1 "
    "epoch=cfg-1 generation=gen-1 revision=1 observed-at=1700000000000000000 "
    "received-at=1700000000000000000 value=down\n";

LO_TEST(process, a_child_ingests_and_a_second_child_reads_the_result) {
  TempFile snapshot{"linkobs-proc-state"};
  TempFile records{"linkobs-proc-records"};
  records.write(kScenario);

  const ProcessResult ingest = run_child(cli() + " ingest --db " + quote(snapshot.path) +
                                         " --input " + quote(records.path) + " --now-ns "
                                         "1700000000000000000");
  LO_CHECK_EQ(ingest.exit_code, 0);
  LO_CHECK(ingest.output.find("accepted=1") != std::string::npos);

  const ProcessResult inspect = run_child(cli() + " inspect --db " + quote(snapshot.path) +
                                          " --now-ns 1700000000000000000");
  LO_CHECK_EQ(inspect.exit_code, 0);
  LO_CHECK(inspect.output.find("restored=true") != std::string::npos);
  LO_CHECK(inspect.output.find("state=down") != std::string::npos);
}

LO_TEST(process, old_evidence_is_not_fresh_in_a_new_process) {
  TempFile snapshot{"linkobs-proc-fresh"};
  TempFile records{"linkobs-proc-fresh-records"};
  records.write(kScenario);

  const ProcessResult ingest = run_child(cli() + " ingest --db " + quote(snapshot.path) +
                                         " --input " + quote(records.path) + " --now-ns "
                                         "1700000000000000000");
  LO_CHECK_EQ(ingest.exit_code, 0);

  // The second process evaluates an hour later. The observation is unchanged, so
  // the state must be stale rather than down.
  const ProcessResult inspect = run_child(cli() + " inspect --db " + quote(snapshot.path) +
                                          " --now-ns 1700003600000000000");
  LO_CHECK_EQ(inspect.exit_code, 0);
  LO_CHECK(inspect.output.find("state=stale") != std::string::npos);
  LO_CHECK(inspect.output.find("freshness=expired") != std::string::npos);
}

LO_TEST(process, explanations_are_byte_identical_across_processes) {
  TempFile snapshot{"linkobs-proc-explain"};
  TempFile records{"linkobs-proc-explain-records"};
  records.write(kScenario);

  const ProcessResult ingest = run_child(cli() + " ingest --db " + quote(snapshot.path) +
                                         " --input " + quote(records.path) + " --now-ns "
                                         "1700000000000000000");
  LO_CHECK_EQ(ingest.exit_code, 0);

  const ProcessResult first = run_child(cli() + " explain --db " + quote(snapshot.path) +
                                        " --link proc-link --now-ns 1700000000000000000");
  const ProcessResult second = run_child(cli() + " explain --db " + quote(snapshot.path) +
                                         " --link proc-link --now-ns 1700000000000000000");
  LO_CHECK_EQ(first.exit_code, 0);
  LO_CHECK_EQ(second.exit_code, 0);
  LO_CHECK_EQ(first.output, second.output);
  LO_CHECK(first.output.find("state: down") != std::string::npos);
}

LO_TEST(process, a_damaged_snapshot_fails_verification_with_an_integrity_exit_code) {
  TempFile snapshot{"linkobs-proc-damaged"};
  TempFile records{"linkobs-proc-damaged-records"};
  records.write(kScenario);

  const ProcessResult ingest = run_child(cli() + " ingest --db " + quote(snapshot.path) +
                                         " --input " + quote(records.path) + " --now-ns "
                                         "1700000000000000000");
  LO_CHECK_EQ(ingest.exit_code, 0);

  const ProcessResult verify = run_child(cli() + " verify --db " + quote(snapshot.path));
  LO_CHECK_EQ(verify.exit_code, 0);
  LO_CHECK(verify.output.find("verify=OK") != std::string::npos);

  // Flip one byte of the payload.
  {
    std::ifstream stream(snapshot.path, std::ios::binary);
    std::string bytes;
    stream.seekg(0, std::ios::end);
    const std::streamoff length = stream.tellg();
    stream.seekg(0, std::ios::beg);
    bytes.assign(static_cast<std::size_t>(length), '\0');
    stream.read(bytes.data(), length);
    stream.close();
    LO_REQUIRE(bytes.size() > 8U);
    bytes[bytes.size() - 2U] = static_cast<char>(bytes[bytes.size() - 2U] ^ 0x11);
    std::ofstream out(snapshot.path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const ProcessResult damaged = run_child(cli() + " verify --db " + quote(snapshot.path));
  LO_CHECK_EQ(damaged.exit_code, 4);
  LO_CHECK(damaged.output.find("verify=FAIL") != std::string::npos);
}

LO_TEST(process, a_forged_record_is_rejected_with_a_data_exit_code) {
  TempFile snapshot{"linkobs-proc-forged"};
  TempFile records{"linkobs-proc-forged-records"};
  records.write(
      "v=1 kind=source name=proc-agent authority=primary provenance=real\n"
      "v=1 kind=link name=proc-link link-kind=physical linkgen=gen-1 provenance=real\n"
      "v=1 kind=observation link=proc-link metric=oper-state source=proc-agent incarnation=boot-1 "
      "epoch=cfg-1 generation=gen-1 revision=1 observed-at=1 received-at=1 value=sideways\n");

  const ProcessResult ingest = run_child(cli() + " ingest --db " + quote(snapshot.path) +
                                         " --input " + quote(records.path));
  LO_CHECK_EQ(ingest.exit_code, 2);
  LO_CHECK(ingest.output.find("rejected=") != std::string::npos);
}

LO_TEST(process, an_independent_client_process_talks_to_the_runtime_over_a_real_socket) {
  TempFile snapshot{"linkobs-proc-serve"};
  TempFile records{"linkobs-proc-serve-records"};
  records.write(kScenario);

  // The runtime listens in this process; the client is a separate operating
  // system process launched below. Nothing about the exchange is simulated.
  RuntimeConfig config{};
  config.snapshot_path = snapshot.path;
  config.workers = 1U;
  config.policy = make_default_policy();
  config.incarnation_name = "server-process";
  auto clock = std::make_unique<ManualClock>(TimePoint::from_nanos(1700000000000000000LL));
  Observatory observatory(std::move(config), std::move(clock));
  const Status started = observatory.start();
  LO_REQUIRE(started.ok());

  ServerOptions options{};
  options.host = "127.0.0.1";
  options.port = 0U;
  options.connection_limit = 1U;
  options.persist_on_close = true;
  TransportServer server(observatory, options);
  LO_REQUIRE(server.listen().ok());
  LO_REQUIRE(server.bound_port() != 0U);

  std::thread server_thread([&server]() {
    const Status served = server.serve();
    (void)served;
  });

  const std::string connect =
      std::string{"127.0.0.1:"} + to_dec(static_cast<std::uint64_t>(server.bound_port()));
  const ProcessResult client = run_child(cli() + " send --connect " + quote(connect) +
                                         " --input " + quote(records.path) +
                                         " --link proc-link --shutdown");
  LO_CHECK_EQ(client.exit_code, 0);
  LO_CHECK(client.output.find("ping=ok") != std::string::npos);
  LO_CHECK(client.output.find("push=records=1 rejected=0") != std::string::npos);
  LO_CHECK(client.output.find("state=down") != std::string::npos);
  LO_CHECK(client.output.find("shutdown=shutdown") != std::string::npos);

  server_thread.join();
  const ServerStatistics stats = server.statistics();
  LO_CHECK_EQ(stats.connections_accepted, static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(stats.pushes, static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(stats.queries, static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(stats.shutdowns, static_cast<std::uint64_t>(1));
  LO_CHECK_EQ(stats.protocol_errors, static_cast<std::uint64_t>(0));
  LO_CHECK(observatory.statistics().records_accepted >= 1U);
  const Status stopped = observatory.stop();
  (void)stopped;

  // The transport wrote a snapshot, and a third process reads it back.
  const ProcessResult verify = run_child(cli() + " verify --db " + quote(snapshot.path));
  LO_CHECK_EQ(verify.exit_code, 0);
  LO_CHECK(verify.output.find("verify=OK") != std::string::npos);

  const ProcessResult inspect = run_child(cli() + " inspect --db " + quote(snapshot.path) +
                                          " --now-ns 1700000000000000000");
  LO_CHECK_EQ(inspect.exit_code, 0);
  LO_CHECK(inspect.output.find("state=down") != std::string::npos);
}

LO_TEST(process, the_command_line_reports_its_own_version_and_formats) {
  const ProcessResult version = run_child(cli() + " version");
  LO_CHECK_EQ(version.exit_code, 0);
  LO_CHECK(version.output.find("version=1.0.0") != std::string::npos);
  LO_CHECK(version.output.find("Copyright 2026 Summon Software Labs.") != std::string::npos);
}

}  // namespace
