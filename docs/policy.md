# Policy defaults

Policy is data. Freshness windows, thresholds, conflict tolerance and every
resource bound live in `linkobs::Policy` and are supplied explicitly; two runs
with the same policy and the same evidence produce the same classification and
the same explanation bytes. `linkobs::validate` rejects a policy that is
internally inconsistent, so an unusable configuration fails at startup instead of
silently changing behaviour.

## Why these numbers

The defaults are deliberately conservative: they favour admitting ignorance over
producing a confident wrong answer.

### Freshness windows

| Family | fresh | usable | Rationale |
| --- | --- | --- | --- |
| `admin-state` | 30s | 300s | A deliberate configuration change is rare; a state older than five minutes must not be presented as current. |
| `oper-state` | 30s | 300s | The most safety relevant signal, so the tightest window. |
| `octets` | 30s | 300s | Traffic counters are normally sampled at seconds; a stale pair cannot support a rate. |
| `errors` | 60s | 600s | Error counters are often sampled more slowly, and a slightly old error count is still informative. |
| `discards` | 60s | 600s | Same as errors. |
| `util-reported` | 30s | 300s | A utilization reading is a claim about right now. |
| `signal-quality` | 60s | 600s | Optical and electrical margins move slowly. |
| `flap-report` | 120s | 1200s | A flap count is meaningful over a window, not at an instant. |

The gap between `fresh` and `usable` is the stale band. Evidence in it is
retained and reported but never supports a claim about the present.

### Thresholds

| Setting | Default | Rationale |
| --- | --- | --- |
| `saturation_ppb` | 900000000 (90%) | A link at 90% of declared capacity has no headroom for a burst. |
| `degraded_quality_ppb` | 500000000 (50%) | Half of the normalized quality scale is the point at which margin is a concern, not a fact. |
| `error_ratio_ppb` | 1000000 (0.1%) | One error per thousand octets is unambiguously a fault. |
| `discard_ratio_ppb` | 10000000 (1%) | Discards are expected under congestion; 1% is not. |
| `conflict_tolerance_ppb` | 50000000 (5%) | Sampling jitter between two sources rarely exceeds a few percent. |
| `flap_threshold` | 4 | Four state changes in a minute is a pattern, not an incident. |
| `flap_window` | 60s | Long enough to see a pattern, short enough to clear. |
| `derivation_window` | 300s | A rate over more than five minutes is an average, not a current utilization. |
| `max_history_window` | 3600s | Bounds a history query and the flap window a source may declare. |

### Bounds

Every bound is enforced, and exceeding one is a reported failure rather than a
silent truncation.

| Bound | Default | Applies to |
| --- | --- | --- |
| `max_links` | 4096 | Registered links. |
| `max_sources` | 256 | Registered sources. |
| `max_evidence_per_key` | 16 | Retained evidence per (link, key). |
| `max_contenders_per_key` | 8 | Sources considered at once for one key. |
| `max_transitions_per_link` | 256 | Transition ring per link. |
| `max_total_transitions` | 65536 | Global transition ring. |
| `max_result_rows` | 4096 | Every query result set. |
| `max_record_bytes` | 4096 | One ingest record. |
| `max_records_per_batch` | 4096 | One parsed batch. |
| `max_ingest_queue_depth` | 8192 | One worker queue. |
| `max_workers` | 16 | Ingest workers. |
| `max_snapshot_bytes` | 64 MiB | Snapshot payload. |
| `max_transport_payload_bytes` | 1 MiB | One frame payload. |
| `max_transport_connections` | 16 | Simultaneous connections. |

## Implicit links

`allow_implicit_links` defaults to `false`: an observation for an unregistered
link is refused. When it is enabled, the runtime creates a placeholder link with
no declared capacity, so such a link can never produce a derived utilization. The
placeholder is visible in the report as `capacity-known=false`.

## Derived policy in the command line tool

The CLI builds a default policy and lets four values be overridden:
`--max-links`, `--workers`, `--queue-depth` and `--runtime-name`. It does not
expose the thresholds as command line options, because a threshold that is
changed per invocation makes two runs incomparable and the explanations
non-reproducible.
