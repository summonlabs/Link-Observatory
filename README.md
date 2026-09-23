# Link Observatory

A standalone, vendor neutral Fabric OS runtime for observing link health,
utilization, errors, degradation and transitions, and for interpreting them
historically.

Link Observatory is a C++20 library plus a command line runtime. It ingests
telemetry from independent sources, fences anything it cannot justify, stores
bounded evidence, reports a deterministic health state with a deterministic
explanation, and persists everything with versioned, integrity-checked recovery.

Everything in this README is implemented and verified by tests in this
repository. Nothing is aspirational.

## Systems boundary

Link Observatory owns **observation and historical interpretation** of link
health, utilization, errors, degradation and transitions.

It does not, and by design will not:

* decide route eligibility,
* own or assert link authority,
* repair links,
* program ports,
* infer a failure from missing evidence.

Those are separate systems. This runtime reports what the evidence supports and
says so explicitly when the evidence supports nothing.

## What it guarantees

| Guarantee | How it is enforced and proved |
| --- | --- |
| Missing telemetry is never zero | A family with no evidence reports `unsupported` / `unknown` with no value. A derivation with no capacity, no baseline, a reset or a gap reports a fault and `derived-utilization=none`. |
| Stale counters do not create current utilization | Freshness is recomputed against an explicit instant, never persisted, and a derivation requires both samples to be fresh. |
| Resets do not fabricate negative traffic | Deltas are unsigned and reset paths yield "not derivable". A backward step far from the counter ceiling is a reset, not a wrap. |
| Identity changes fence prior evidence | Epochs, incarnations and topology generations are tracked, superseded ones are retained, and replays from them are refused. |
| Restart does not make old observations fresh | Timestamps are stored and ages are recomputed against the running clock on load. A derived rate is recomputed only from samples that are still fresh, still contiguous and still inside the derivation window. |
| Deterministic policy and explanations | No floating point, no unordered iteration, no implicit clock, no locale. `explain` output is byte identical across runs and across processes. |
| Bounded everything | Workers, queues, payloads, records, history, result sets, snapshot size and aggregation windows all have hard bounds that fail loudly. |
| Real cancellation and shutdown | Queues close and wake every waiter; the transport stop path unblocks a blocked `accept`; there are no timeouts anywhere. |

## Build

Requirements: CMake 3.21+, a C++20 compiler, and threads. On Windows, MSVC
17.10+ with the Visual Studio generator or a developer prompt.

```sh
# From a developer prompt with the compiler on PATH
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

CMake presets are provided: `release`, `debug` and `asan`.

The library builds with `/W4 /WX /permissive-` on MSVC and
`-Wall -Wextra -Werror` and a strict warning set elsewhere. First party warning
count is zero in every configuration that is built here.

Options: `LINKOBS_BUILD_APPS`, `LINKOBS_BUILD_TESTS`, `LINKOBS_BUILD_BENCHMARKS`,
`LINKOBS_BUILD_EXAMPLES`, `LINKOBS_WARNINGS_AS_ERRORS`, `LINKOBS_LOCK_TRACKING`,
`LINKOBS_SANITIZE`, `LINKOBS_ENABLE_ANALYZE`.

## Quick start

```sh
linkobs ingest --db state.lkos --input records.lor --now-ns 1700000000000000000
linkobs inspect --db state.lkos
linkobs explain --db state.lkos --link leaf1-eth0
linkobs history --db state.lkos --link leaf1-eth0 --limit 20
```

A record file is line oriented text. One record is exactly one line; blank lines
and lines starting with `#` are ignored. Each record below is a single physical
line.

```text
# declarations first
v=1 kind=source name=fabric-agent authority=primary provenance=real
v=1 kind=link name=leaf1-eth0 link-kind=physical linkgen=gen-1 provenance=real capacity-in-bps=100000000000 capacity-out-bps=100000000000

# then observations
v=1 kind=observation link=leaf1-eth0 metric=oper-state source=fabric-agent incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=1 observed-at=1700000000000000000 received-at=1700000000000000000 value=up
v=1 kind=observation link=leaf1-eth0 metric=octets/in source=fabric-agent incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=2 observed-at=1700000000000000000 received-at=1700000000000000000 counter=0 width=bits64
v=1 kind=observation link=leaf1-eth0 metric=octets/in source=fabric-agent incarnation=boot-1 epoch=cfg-1 generation=gen-1 revision=3 observed-at=1700000001000000000 received-at=1700000001000000000 counter=125000000 width=bits64
```

The parser is strict: the version is mandatory, unknown and duplicate fields are
rejected, every field is length bounded and every number is range checked. A
malformed line is reported with its line number and counted; it is never skipped
silently and never replaced with a default.

The three observations above describe a link carrying 125000000 bytes in one
second on a 100 Gbit/s link, which is one percent of capacity, so the reported
state is `healthy`. Reduce `capacity-in-bps` to `1000000000` and the same records
describe a fully saturated link.

## Tooling

| Command | Purpose |
| --- | --- |
| `ingest` | Parse, fence and apply a record file, then persist. |
| `inspect` | Link inventory, per-family slots and runtime totals. |
| `classify` | The state line for one link or every link. |
| `explain` | The full deterministic rule trace and evidence report. |
| `history` | Bounded transition history. |
| `export` | `summary`, `csv` or `lor` evidence export. |
| `verify` | Snapshot integrity check with an explicit exit code. |
| `serve` | Real TCP listener for push and query, loopback by default. |
| `send` | Client for a running runtime. |
| `selftest` | In-process invariant check. |
| `bench` | Completed-work benchmark. |
| `version` | Product and format versions. |

Exit codes are stable: 0 ok, 1 usage, 2 data, 3 io, 4 integrity, 5 unsupported,
6 internal.

## Using the library

```cpp
#include "linkobs/ingest/codec.hpp"
#include "linkobs/runtime/observatory.hpp"

linkobs::RuntimeConfig config{};
config.policy = linkobs::make_default_policy();
config.snapshot_path = "state.lkos";
config.workers = 2;

linkobs::Observatory observatory(config);   // inject a Clock for determinism
observatory.start();

linkobs::ParseOutcome parsed = linkobs::parse_batch(text, observatory.config().policy);
observatory.submit_batch(std::move(parsed.batch));
observatory.stop();
```

The runtime is installable and exported:

```sh
cmake --install build/release --prefix /some/prefix
```

```cmake
find_package(LinkObservatory 1.0 REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE LinkObservatory::linkobs)
```

`tests/downstream` is an independent CMake project that does exactly this and is
part of the validated release.

## Persistence

A snapshot is a versioned container with a SHA-256 digest over the header and
another over the payload. Writing is atomic: a temporary file, a rotation of the
previous snapshot to `<path>.prev`, then a rename into place. Reading fails closed
on a wrong magic, a wrong digest, a truncated file, a length mismatch, trailing
bytes or an unsupported version, and falls back to the previous snapshot exactly
once, reporting that it did so.

After a reload nothing is fresh because it was reloaded: ages are recomputed
against the running clock. Counter baselines are restored, because a rate is a
property of the two samples and not of the observing process; the window, the
scope, the topology generation, the revision order and the stream gap epoch all
travel with the samples, so a derived value is exactly as justified after a
restart as before it, and an expired one is not recomputed at all.

## Transport

`linkobs serve` is a real TCP listener with length-prefixed, hash-checked frames.
It is loopback-only unless `--allow-non-loopback` is given explicitly. A push is
acknowledged only after its records have been applied, so a client that queries
immediately after a push sees its own write. A malformed frame terminates that
connection and leaves the server running.

## Testing

```sh
ctest --test-dir build/release --output-on-failure
```

| Suite | Contents |
| --- | --- |
| `linkobs.unit` | 120 tests: unit, integration, property, seeded randomized, adversarial, hardening, concurrency and restart. |
| `linkobs.process` | 7 independent-process tests: real child processes, real files, real loopback sockets. |
| `linkobs.bench` | Completed-work benchmark. |
| `linkobs.example.*` | The shipped examples run as tests. |

No test uses a timeout. Time-dependent behaviour is driven by an injected
`ManualClock`; concurrency is driven by explicit handshakes; the one bounded wait
in the process tests is a readiness poll that exists so a child which never
starts cannot hang the suite.

## Benchmarks

```sh
linkobs bench --records 200000 --links 64 --sources 4 --workers 0
linkobs bench --records 200000 --links 64 --sources 4 --workers 4
```

Observed on the development host (single run each, not a controlled measurement):

| Configuration | Ingest throughput | Accepted | Fenced |
| --- | --- | --- | --- |
| single worker | ~76k records/s | 200000 | 0 |
| four workers | ~182k records/s | 200000 | 0 |

The benchmark prints a digest over the completed classifications, so the measured
work cannot be optimised away and two runs can be compared for equality.

## Real, synthetic and unsupported

**REAL** — verified here by executed tests: the runtime library, the command line
tool, in-process behaviour, restart and recovery, the independent-process tests,
the loopback TCP transport, the Release and Debug builds, the AddressSanitizer
build, and the downstream `find_package` consumer.

**SYNTHETIC** — all benchmark and property input is generated by this
repository's own generator, and every such record declares
`provenance=synthetic`. No synthetic data is ever presented as real, and the
provenance class travels with the evidence into the report.

**UNSUPPORTED** — not implemented and never claimed: real switch, ASIC, RDMA,
InfiniBand or NVLink hardware; vendor telemetry protocols or MIBs; multi-host
network fabrics; snapshot authentication or signing; AddressSanitizer leak
detection on this platform.

See `docs/proof-surfaces.md` for the full table.

## Documentation

* `docs/architecture.md` — layers, ownership and the concurrency contract.
* `docs/states.md` — the nine states, rule precedence and derived utilization.
* `docs/freshness.md` — freshness, confidence, fencing and restart semantics.
* `docs/formats.md` — record grammar, snapshot container and wire framing.
* `docs/policy.md` — every default and why it has that value.
* `docs/locking-audit.md` — the deadlock and lock re-entrancy audit.
* `docs/proof-surfaces.md` — REAL, SYNTHETIC and UNSUPPORTED.
* `docs/validation.md` — what was run, what was found, what is limited.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
