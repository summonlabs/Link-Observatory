# Link Observatory - Architecture

Link Observatory is a standalone, vendor-neutral Fabric OS runtime. It owns the
observation and historical interpretation of link health, utilization, errors,
degradation and transitions. It is a library plus a command line tool; it is not
a control plane.

## Scope of the runtime

What the runtime does:

* accepts link and source declarations and link telemetry observations,
* validates, fences and stores that evidence under hard bounds,
* derives rates and utilization from cumulative counters when, and only when,
  the derivation is defensible,
* classifies link state deterministically from the evidence it holds,
* keeps bounded transition history and produces deterministic explanations,
* persists and restores its state under integrity checking.

What the runtime deliberately does not do, and has no code path for:

| Excluded responsibility | Why it is absent |
| --- | --- |
| Route eligibility | No routing table, no next-hop notion and no path object exists anywhere in the tree. A classification is a report about a link, never an input to forwarding. |
| Link authority | The runtime never commands a state. There is no setter for administrative or operational state, no port object and no fabric write path. Declared facts come from the operator, observed facts come from sources. |
| Link repair | There is no retry, reset, bounce or re-enable operation. A link that is reported down is reported down. |
| Port programming | No register, lane, PHY or serdes concept is modelled, because modelling one without hardware access would be a fabricated capability. |
| Inferring failure from missing evidence | Absence of evidence produces Unknown, Unsupported or Stale, never Down. The only path to Down is a fresh, eligible reading that says down or disabled. |

The last row is the defining property of the design and is enforced structurally:
rule R-800 (no evidence) and rules R-810/R-820 (unsupported or incomplete decisive
families) select Unknown, and the "down" rules require an eligible reading.

## Layer map

| Layer | Directory | Owns | Depends on |
| --- | --- | --- | --- |
| core | include/linkobs/core, src/core | Content hashing (FNV-1a 128), SHA-256, hexadecimal codecs, checked arithmetic including 128-bit intermediates, status/result types, strongly typed content-addressed identities, deterministic text formatting, the lock order auditor | nothing |
| time | include/linkobs/time, src/time | Instant and duration value types, the Clock interface, SystemClock, ManualClock, checked instant arithmetic | core |
| domain | include/linkobs/domain, src/domain | Enumerations with frozen spellings, metric identity and typed readings, observations and provenance, source records, link facts, evidence ageing and confidence, policy and resource bounds | core, time |
| store | include/linkobs/store, src/store | Link and source registry with topology generation currency, bounded evidence store with every ingest fence, bounded transition history, counter continuity and rate derivation | domain |
| classify | include/linkobs/classify, src/classify | The rule set, the deterministic state selection, per-family reports, and the canonical explanation renderer | domain, store |
| persist | include/linkobs/persist, src/persist | The explicit little-endian codec and the versioned, digest-checked snapshot container with rotation and recovery | domain, store |
| ingest | include/linkobs/ingest, src/ingest | The LOR record grammar, batch parsing with per-line error reporting, the bounded queue and the one-shot batch completion latch | domain |
| runtime | include/linkobs/runtime, src/runtime | Configuration and validation, the bounded worker pool, and the Observatory facade that owns all runtime state | classify, ingest, persist, store |
| transport | include/linkobs/transport, src/transport | Wire framing, the loopback-first TCP server and the client | runtime |
| apps | apps/linkobs | The command line surface: ingest, inspect, history, classify, explain, export, verify, selftest, bench, serve, send, version | runtime, transport |

## Dependency direction

    apps  ->  transport  ->  runtime  ->  classify  ->  store  ->  domain  ->  time  ->  core
                                       ->  persist   ->  store
                                       ->  ingest    ->  domain

The direction is strictly downward. No layer includes a header from a layer above
it, so there are no cycles and no hidden back edges:

* core depends on nothing first-party.
* domain depends on core and time only; it contains no I/O, no threads and no
  clocks other than the injected Clock interface.
* store depends on domain. It performs no I/O: persistence is a separate layer
  that reads the store's serialisable views.
* classify depends on domain and store and is otherwise a pure function of its
  inputs; it never reads a clock directly.
* persist depends on domain and store. It is the only layer that touches files.
* ingest depends on domain. It is the only layer that parses untrusted text.
* runtime depends on classify, ingest, persist and store, and owns them.
* transport depends on runtime and owns sockets, nothing else.
* apps depend on runtime and transport.

## Ownership and concurrency contract

The runtime has exactly one piece of mutable shared state and exactly one lock
that guards it.

* `Observatory` owns a `LinkRegistry`, an `EvidenceStore` and a
  `TransitionLog` by value.
* A single `TrackedLock` of class `LockClass::ObservatoryState` (`state_lock_`)
  guards all three. Every read and every mutation of registry, evidence and
  history goes through `Observatory` with that lock held.
* `LinkRegistry`, `EvidenceStore` and `TransitionLog` are **not**
  independently synchronised. They contain no mutex. Their headers state this
  explicitly, and their public methods are only correct when the caller holds
  the observatory lock. This is the ownership statement the locking audit rests
  on: there is no second lock that could be taken in the opposite order, because
  there is no second lock in the state path at all.
* Queue locks (`LockClass::IngestQueue`) guard only the bounded queues and the
  batch completion latch. They are never nested with the state lock: a producer
  releases the queue lock before the item is applied, and a consumer releases it
  before the handler runs.
* The transport statistics lock (`LockClass::Transport`) guards only the
  server's counters and is held for single counter updates, never across a wait.

The full argument, including the lock order, every acquisition site, the wait and
join analysis and the batch completion latch, is in `docs/locking-audit.md`.

## Data flow

Ingest path:

    LOR text
      -> ingest::parse_batch                 (strict per-line grammar, bounded)
      -> Batch                               (records + optional completion)
      -> Observatory::submit_batch
           declarations                      applied synchronously under the state lock
           observations                      routed by source name hash to a worker
      -> Observatory::apply_batch             (state lock held)
           -> LinkRegistry                   declarations, source authority, generation currency
           -> EvidenceStore::accept          every ingest fence, then evidence and continuity
           -> classify                       re-evaluate the affected link
           -> TransitionLog                  record the transition if the state changed
      -> BatchCompletion::signal              fires after the state lock is released

Query path (all under the same lock, all returning copies):

    Observatory::links / sources / find_link
    Observatory::classify / classify_all        -> Classification
    Observatory::transitions / recent_transitions
    Observatory::evidence_for / metrics_for / slot_views / link_evidence

Persistence path:

    Observatory::collect_snapshot  -> SnapshotContents -> persist::write_snapshot
    persist::read_snapshot         -> SnapshotContents -> Observatory::restore_from

Routing by source name is what makes worker count a throughput knob and nothing
else: every record from one source is applied by one worker in submission order,
whatever the configured worker count.

## Determinism

Determinism is a design constraint, not an aspiration:

* no floating point is used in any decision. Ratios are integer parts per
  billion, computed with checked 128-bit intermediates.
* no clock is read implicitly. Everything that needs "now" takes it from the
  injected `Clock`; `ManualClock` makes time an ordinary input in tests.
* no `std::unordered_map` iteration reaches an output. Maps are ordered, and
  result sets are sorted by a typed key before they are returned.
* content identity, transition identity, explanation text and exported records
  are all derived from canonical, length-prefixed encodings rather than from
  addresses, pointers or locale-dependent formatting.
* the rule evaluation records every rule, fired or not, in ascending rule
  identifier order, so an explanation is a function of the inputs alone.

## Where each boundary requirement lives

| Requirement | Implementation |
| --- | --- |
| Typed link identity | `LinkId` derived from an operator name by `derive_link_id` (domain/link.hpp) |
| Typed endpoint and port identities | `EndpointId`, `PortId` in `LinkFacts` (domain/link.hpp) |
| Typed source identity | `SourceId`, `SourceRecord` (domain/observation.hpp) |
| Generation, epoch and incarnation | `Provenance` and `SequenceScope` (domain/observation.hpp) |
| Revision | `Sequence`, scoped to (source, incarnation, epoch) |
| Administrative and operational state | `AdminStateReading`, `OperStateReading` (domain/metric.hpp) |
| Utilization | `RatioReading` for reported values; `derive_utilization` for derived ones |
| Throughput | `DerivationResult::bits_per_second` |
| Errors and discards | Counter families keyed by `ErrorClass` (domain/metric.hpp) |
| Signal and degradation indicators | `QualityReading` with a declared degraded flag |
| Flap events | `FlapReading` plus counted admin/oper transitions in the log |
| Freshness by metric family | `FreshnessPolicy` per family (domain/policy.hpp), applied by `classify_freshness` |
| Confidence by metric family | `derive_confidence` and per-family `FamilyReport::confidence` |
| Deterministic link states | `classify` and the `RuleId` set (classify/classifier.hpp) |
| Bounded historical transitions | `TransitionLog` (store/history.hpp) |
| Counter reset and wrap handling | `observe_counter`, `wrap_detection_threshold` (store/counter_tracker.hpp) |
| Source conflicts | Highest-authority comparison in `classify`; `ConflictState` |
| Topology generation correlation | `LinkRegistry::observe_generation` and generation fencing in the store |
| Persistence and restart semantics | persist/snapshot.hpp plus `Observatory::restore_from` |
| Ingest, inspect, history, classify, explain, export tooling | apps/linkobs |

## Failure and refusal model

Every refusal is typed and counted. `FenceReason` enumerates why an observation
was not admitted (unknown source, unknown link, unsupported family, invalid
value, duplicate, revision replay, revision gap, epoch behind, incarnation
behind, generation behind, authority downgrade, oversized payload, undeclared
provenance). A refused observation never mutates current evidence. The counters
are reported by `Observatory::fence_count` and by `Statistics`.

The runtime distinguishes five states of knowledge that are routinely conflated
elsewhere, and keeps them distinct from first ingest to final report:

| State | Meaning |
| --- | --- |
| Unknown | There is no evidence, or the evidence that exists cannot support a claim. |
| Stale | Evidence exists and was admitted, but it is not usable for a present-tense claim. |
| Conflicting | Two sources of comparable authority disagree beyond the configured tolerance. |
| Incomplete | The revision stream has a gap inside the derivation window, so derived quantities are unavailable. |
| Unsupported | No configured source supplies the family at all. This is a statement about configuration, never about the link. |
