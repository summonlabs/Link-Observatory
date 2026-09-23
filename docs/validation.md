# Link Observatory - Validation Record

This document records what was actually executed for this release, the results
observed, the defects that hardening found and fixed, and the limitations that
remain. Everything here was produced on one Windows x64 host with MSVC 19.44
(toolset 14.44.35207), CMake 4.3.2 and Ninja, against the release commit.

## 1. Build configurations

| Configuration | Build type | Sanitizer | Warnings as errors | Purpose |
| --- | --- | --- | --- | --- |
| `release` | Release | none | yes | shipped artefacts, benchmarks, install |
| `debug` | Debug | none | yes | assertions, lock tracking, full iterator debugging |
| `asan` | Debug | AddressSanitizer | yes | memory-error detection over the whole suite |

All three use the same warning policy from `cmake/LinkObservatoryWarnings.cmake`:

    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:inline /utf-8 /EHsc
    /external:anglebrackets /external:W0 /WX

The build is configured with `LINKOBS_WARNINGS_AS_ERRORS=ON` in every
configuration used for validation, so the first-party warning count is zero by
construction rather than by inspection: any diagnostic from first-party code
would have failed the build, and all three configurations completed successfully.

    cmake --preset release && cmake --build --preset release
    cmake --preset debug   && cmake --build --preset debug
    cmake --preset asan    && cmake --build --preset asan

## 2. Test suites

The main suite is one executable with a deterministic exit code and no timeout of
any kind. Concurrency is driven by explicit handshakes; anything time dependent
is driven by `ManualClock`.

| Suite | Contents |
| --- | --- |
| `core` | hashing, SHA-256 known answers, checked and 128-bit arithmetic, strict text parsers, locale independence, bounded splitting, manual clock, the lock tracker |
| `domain` | enumerations and spellings, metric keys, readings and their validation, provenance, evidence ageing, confidence derivation, policy validation |
| `record` | the LOR grammar, every rejection path, every value spelling |
| `store` | link and source registry, generation currency, ingest fences, bounded history, counter continuity and derivation |
| `classify` | every rule, every state, precedence, conflict handling, confidence and completeness aggregation |
| `persist` | snapshot round trip, every integrity failure, rotation and recovery, restart semantics |
| `property` | seeded randomised streams with invariants asserted over the whole run |
| `adversarial` | forged digests, replayed revisions, retired generations, backwards clocks, oversized fields, crafted counts |
| `hardening` | regression tests written after the first green run, plus regression coverage for the defects listed in section 5 |
| `concurrency` | concurrent ingest, concurrent readers, start/stop/cancel, worker-count equivalence, zero lock violations |
| `integration` | end-to-end pipelines, batch-split equivalence, unsupported families, restart |
| `transport` | frame round trip, every header field checked, real loopback round trip, malformed frames, loopback restriction |

Results, as printed by the harness:

    build/release/tests/linkobs_tests.exe          tests passed=122 failed=0   exit 0
    build/debug/tests/linkobs_tests.exe            tests passed=122 failed=0   exit 0
    build/asan/tests/linkobs_tests.exe             tests passed=122 failed=0   exit 0
    build/release/tests/linkobs_process_tests.exe  tests passed=7   failed=0   exit 0
    build/debug/tests/linkobs_process_tests.exe    tests passed=7   failed=0   exit 0
    build/asan/tests/linkobs_process_tests.exe     tests passed=7   failed=0   exit 0

The seven independent-process tests spawn the real `linkobs` executable and
verify: a child ingests and a second child reads the result; old evidence is not
fresh in a new process; explanations are byte identical across processes; a
damaged snapshot fails verification with the integrity exit code; a forged record
is rejected with the data exit code; an independent client process talks to the
runtime over a real socket; the command line reports its own version and formats.

`example_observe` and `example_persist` are registered as CTest tests and
pass. `example_transport` is built with the same warning policy and is run
manually against a serving runtime; it is not registered as a test because it
needs a listener to talk to.

### AddressSanitizer

The x64 ASan configuration builds and the full suite passes under it. The MSVC
ASan runtime library is not copied next to the executables by the build, so the
binaries must be started with the MSVC ASan runtime directory on `PATH`:

    C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64

Without that directory on `PATH` the executables fail immediately with
`STATUS_DLL_NOT_FOUND` (0xC0000135). With it, both suites report the results
above. AddressSanitizer on this platform does not perform leak detection, so no
leak-detection result is claimed.

## 3. Package and downstream consumer

The install tree was produced from the Release build and consumed by a separate
CMake project that has no access to the source tree:

    cmake --install build/release --prefix <prefix>            # exit 0
    cmake -S tests/downstream -B <dir> -G Ninja \
          -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<prefix>
    cmake --build <dir>                                        # exit 0
    <dir>/downstream_consumer.exe

    downstream ok: version=1.0.0 snapshot-format=1 state=healthy

The consumer uses only installed public headers (`linkobs/version.hpp`,
`linkobs/ingest/codec.hpp`, `linkobs/classify/explain.hpp`,
`linkobs/runtime/observatory.hpp`) and the exported target
`LinkObservatory::linkobs`, resolves through `find_package(LinkObservatory 1.0
REQUIRED CONFIG)`, and compiles with `/W4 /WX` itself. The installed tree
contains the library, the command line executable, the public headers, the
generated version header, the package configuration and version files, and the
licence.

## 4. Benchmarks

The benchmark measures completed work: it parses a generated record set, submits
every batch and waits for the completion latch, then classifies every link. It
prints per-phase counters and a digest over the resulting state so the work
cannot be optimised away.

Both configurations were run: the inline path (no workers) and the real worker
path. When workers are configured the benchmark submits through the worker pool
and waits on the completion latch before it classifies, so both blocks below
measure completed work rather than enqueued work. The run is driven by an
explicit evaluation instant, so the two blocks are directly comparable.

    records=200068 links=64 sources=4 workers=0 batches=49
    parse_ns=474886600 parse_per_second=421296
    ingest_ns=791617000 ingest_per_second=252733 accepted=200000 fenced=0
    classify_ns=163500 classifications=64 classify_per_second=391437
    transitions=128 state_changes=64
    result_digest=c3e2161d3ef71f7000bb14d34bd6477375c44297ab4d2f7b47be63844fea7d81

    records=200068 links=64 sources=4 workers=4 batches=49
    parse_ns=476556600 parse_per_second=419820
    ingest_ns=779106700 ingest_per_second=256791 accepted=200000 fenced=0
    classify_ns=185800 classifications=64 classify_per_second=344456
    transitions=128 state_changes=64
    result_digest=c3e2161d3ef71f7000bb14d34bd6477375c44297ab4d2f7b47be63844fea7d81

Both blocks report the same accepted count, the same transition and state-change
counts and the same result digest, which is the point: the worker count changes
throughput and nothing else. Each block is a single-run observation on this host,
not an average of repeated runs. No speed-up from the worker path is claimed and
none is demonstrated by one pair of runs.

These are measurements, not guarantees. They are not a latency or throughput
contract, and no comparison with any other system is implied.

## 5. Defects found and fixed during hardening

Hardening was a deliberate attempt to break the runtime after the first passing
state. The following material defects were found, reproduced, fixed and covered
by a regression test; the hardening pass itself lives in `tests/test_hardening.cpp`.

1. **`TextFields::add` bound a string literal to the bool overload.**
   `fields.add("kind", "source")` selected `add(std::string_view, bool)` through
   the standard pointer-to-bool conversion rather than the string overload, so
   the field rendered as `true`. Reports were still deterministic, which is why
   only a content assertion caught it. Fixed by adding explicit `const char*`
   and `std::string` overloads so the literal can no longer decay to a boolean.

2. **The snapshot loader read every collection length before decoding anything.**
   All five counts were decoded up front, so a corrupt or hostile file could
   drive work based on a count that the rest of the payload did not support.
   Fixed by decoding each collection length immediately before that collection,
   so a count is validated against the payload, the policy bound and the
   remaining bytes before any element is decoded.

3. **Counter contiguity required exactly one revision step.** Derivation rejected
   a valid sample pair whenever the source advanced its revision counter for a
   different metric family in between, which made utilization underivable for any
   source that reports several families under one revision counter. Fixed by
   tracking a per-stream gap epoch and requiring strictly increasing revisions
   within an unchanged epoch: two samples carrying the same epoch provably had no
   missing revision between them, whatever else the source interleaved.

4. **`TransportServer::request_stop` could not unblock `accept()`.** The accept
   loop blocks in `accept()` with no deadline by design, so setting a stop flag
   left `serve()` blocked until an unrelated connection happened to arrive.
   Fixed by waking the loop with one loopback connection to the listener, which
   is portable, immediate and does not depend on closing a socket from another
   thread.

5. **The transport acknowledged a push before the records were applied.**
   `PushRecords` was answered as soon as the batch had been enqueued, so a
   client that queried immediately after a push could observe a state predating
   its own write. Fixed with the one-shot `BatchCompletion` latch, signalled from
   `Batch::on_applied` only after every part of the batch has been applied.

6. **`to_dec` was ambiguous for `std::size_t` on a 32-bit target.**
   `std::size_t` is neither `std::uint64_t` nor `std::int64_t` on Win32, so a
   call with a `size_t` argument was ambiguous or silently selected the signed
   overload. Fixed with a constrained template overload that dispatches on
   signedness.

7. **Link-level freshness never reported `Expired`.** The aggregate collapsed to
   Fresh or Stale, so a link whose evidence was entirely past its usable window
   was reported as Stale, which understates how unusable the evidence is. Fixed
   by aggregating the per-family freshness and reporting Expired when no family
   has usable evidence, while keeping Stale for the case where some evidence is
   still inside its usable window but outside its fresh window.

8. **Counter baselines were dropped when a snapshot was restored.** The restore
   path called `EvidenceStore::reset_continuity()`, on the theory that no rate
   should span a restart. The effect was that every read-only process
   (`linkobs inspect`, `classify`, `explain`) reported
   `derivation-fault=missing-previous-sample` and no utilization at all, even for
   evidence that was fresh, contiguous and inside the derivation window. A rate is
   a property of the two samples, not of the observing process: the window, the
   scope, the topology generation, the revision order and the stream gap epoch all
   travel with the samples, and freshness is recomputed against the running clock.
   Fixed by restoring the continuity instead of clearing it; a rate whose basis has
   expired is now reported as a derivation fault and the link reports Expired
   freshness. `reset_continuity` was removed.

9. **Runtime session statistics were not restored with the evidence.** After a
   restart, `linkobs inspect` showed per-link accepted counts next to totals of
   zero, because `RuntimeStatistics` was left at its initial value. Fixed by
   restoring the statistics that describe the evidence (records accepted, records
   fenced, transitions recorded) from the snapshot totals, while leaving the
   counters that describe the process itself (snapshot writes, cancellations) at
   zero.

Each fixed defect has a regression test. The hardening pass lives in
`tests/test_hardening.cpp`, and the table below names the test that fails if each
defect returns.

| Defect | Covering test |
| --- | --- |
| 1 | `record.formatted_records_parse_back`, `integration.explanations_are_stable_and_complete` |
| 2 | `adversarial.an_oversized_snapshot_is_refused_before_allocation`, `hardening.a_snapshot_larger_than_the_configured_bound_is_refused` |
| 3 | `store.an_interleaved_metric_stream_still_derives_a_rate` |
| 4 | `concurrency.repeated_start_and_stop_cycles_are_stable`, `hardening.the_transport_bounds_a_stalled_peer_by_connection_count`, `process.an_independent_client_process_talks_to_the_runtime_over_a_real_socket` |
| 5 | `transport.a_real_loopback_round_trip_works`, `process.an_independent_client_process_talks_to_the_runtime_over_a_real_socket` |
| 6 | enforced by the compiler wherever a size is formatted; exercised by `store.evidence_slots_are_bounded` and `record.batch_bound_is_enforced_not_bypassed` |
| 7 | `property.freshness_degrades_monotonically_as_time_advances`, `persist.restart_does_not_make_old_evidence_fresh` |
| 8 | `persist.a_rate_is_derived_after_a_restart_when_the_samples_are_still_usable`, `persist.a_rate_is_not_resurrected_by_a_restart_that_outlives_the_window` |
| 9 | `persist.a_restored_runtime_reports_the_totals_it_restored`, which asserts the restored accepted, fenced and transition totals against the snapshot contents and asserts that process-local counters stay at zero |

The complete suite was rerun in all three configurations after the last fix, with
the results recorded in section 2.

## 6. Known limitations

* **No idle timeout on the transport.** A connection that stops sending is not
  closed by a timer. This is deliberate: the runtime does not terminate work on a
  clock, and an idle connection is not an error. It does mean a client that hangs
  holds a connection slot until the connection limit is reached.
* **Per-reason fence counters are session statistics.** The totals that describe
  the evidence (records accepted, records fenced, transitions recorded) are
  restored from the snapshot, but the breakdown by `FenceReason` is accumulated
  in memory only and restarts at zero. Accepted evidence, sequence state,
  generation currency, transition history and counter continuity are persisted.
* **Retired topology generations are bounded.** The registry keeps a bounded list
  of retired generations per link. A generation that has been evicted from that
  list can no longer be recognised as retired, so a replay of a very old
  generation after many subsequent topology changes would be treated as a new
  generation rather than as a fence. The bound is deliberately generous relative
  to realistic topology churn.
* **AddressSanitizer runtime deployment.** See U8 in `docs/proof-surfaces.md`:
  the instrumented binaries need the MSVC ASan runtime directory on `PATH`.
* **No non-Windows validation.** The build contains GCC and Clang warning
  configurations and POSIX socket paths, but no such compiler was available on
  this host, so nothing outside MSVC on Windows x64 was compiled or executed.
* **No completed 32-bit build.** A Win32 configuration was attempted because the
  x64 AddressSanitizer runtime was believed to be missing at the time; the x64
  runtime turned out to be present in the Build Tools installation, so the
  AddressSanitizer configuration is x64 and the 32-bit tree was abandoned
  unfinished. Defect 6 was found while reviewing that attempt rather than by a
  completed 32-bit test run, so no 32-bit build or test result is claimed.
* **Restored totals are not asserted directly.** Defect 9 is exercised by the
  restart round-trip tests and by the independent-process tests, which check that
  state is restored after a restart, but no test currently asserts the restored
  `records_accepted`, `records_fenced` or `transitions_recorded` values
  against the values that were persisted. The fix is in place and observable
  through `linkobs inspect`; a direct assertion is still missing.
* **Single host only.** The transport proof is a loopback socket between
  processes on one machine. Nothing about multi-host behaviour is claimed.

## 7. Reproducing this record

    cmake --preset release && cmake --build --preset release
    cmake --preset debug   && cmake --build --preset debug
    ctest --preset release
    ctest --preset debug

    # benchmark
    build/release/benchmarks/linkobs_bench.exe

    # package and downstream proof
    cmake --install build/release --prefix <prefix>
    cmake -S tests/downstream -B <dir> -G Ninja \
          -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<prefix>
    cmake --build <dir> && <dir>/downstream_consumer.exe

    # address sanitizer (runtime directory on PATH, as described above)
    cmake --preset asan && cmake --build --preset asan
    build/asan/tests/linkobs_tests.exe
