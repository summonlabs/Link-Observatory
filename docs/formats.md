# Link Observatory - Formats

Three formats are frozen by this release: the LOR record grammar, the snapshot
container, and the transport frame. Frozen means the spellings below are part of
the interface. A spelling is never reused with a different meaning, and an
unrecognised spelling is rejected with a typed status rather than defaulted,
ignored or clamped.

Version constants live in `linkobs/version.hpp`: `kSnapshotFormatVersion`,
`kTransportProtocolVersion` and `kRecordFormatVersion` are all `1` for this
release.

## 1. LOR record grammar

A record is one line of `key=value` fields separated by single spaces. Blank
lines and lines whose first non-space character is `#` are ignored. The grammar
is versioned by a mandatory leading field `v=1`.

    v=1 kind=source name=<token> authority=<level> provenance=<class> [retired=<bool>]

    v=1 kind=link name=<token> link-kind=<kind> linkgen=<token> provenance=<class>
        [capacity-in-bps=<n>] [capacity-out-bps=<n>]
        [local-endpoint=<token>] [remote-endpoint=<token>]
        [local-port=<token>] [remote-port=<token>]

    v=1 kind=observation link=<token> source=<token> incarnation=<token>
        epoch=<token> generation=<token> revision=<n>
        observed-at=<ns> received-at=<ns> metric=<key> <value fields>

### 1.1 Field reference

| Field | Applies to | Type | Required | Notes |
| --- | --- | --- | --- | --- |
| `v` | all | unsigned decimal | yes | must be exactly `1` |
| `kind` | all | token | yes | `source`, `link` or `observation` |
| `name` | source, link | token | yes | operator-supplied identity; hashed into a typed id |
| `authority` | source | token | yes | see 1.3 |
| `provenance` | source, link | token | yes | see 1.3; `unknown` is rejected for a source |
| `retired` | source | `true`/`false` | no | default `false` |
| `link-kind` | link | token | yes | see 1.3 |
| `linkgen` | link | token | yes | topology generation name |
| `capacity-in-bps` | link | unsigned decimal | no | presence makes capacity known for that direction |
| `capacity-out-bps` | link | unsigned decimal | no | as above |
| `local-endpoint`, `remote-endpoint` | link | token | no | endpoint anchors |
| `local-port`, `remote-port` | link | token | no | port anchors |
| `link` | observation | token | yes | must name a declared link |
| `source` | observation | token | yes | must name a declared source |
| `incarnation` | observation | token | yes | source process incarnation |
| `epoch` | observation | token | yes | source configuration epoch |
| `generation` | observation | token | yes | topology generation the reading belongs to |
| `revision` | observation | unsigned decimal | yes | monotonic within (source, incarnation, epoch) |
| `observed-at` | observation | signed decimal ns | yes | source observation instant |
| `received-at` | observation | signed decimal ns | yes | local receive instant |
| `metric` | observation | metric key | yes | see 1.2 |

### 1.2 Metric keys and value fields

A metric key is `<family>[/<direction>][/<error-class>]`. The optional parts are
present exactly when they carry information.

| Family spelling | Direction accepted | Error class accepted | Value fields |
| --- | --- | --- | --- |
| `admin-state` | no | no | `value=unknown|enabled|disabled` |
| `oper-state` | no | no | `value=unknown|up|down|degraded` |
| `octets` | yes | no | `counter=<n> width=bits32|bits64 [reset=<bool>]` |
| `errors` | yes | yes | `counter=<n> width=bits32|bits64 [reset=<bool>]` |
| `discards` | yes | yes | `counter=<n> width=bits32|bits64 [reset=<bool>]` |
| `util-reported` | yes | no | `ppb=<n>` |
| `signal-quality` | yes | no | `ppb=<n> [degraded=<bool>]` |
| `flap-report` | no | no | `events=<n> window-ns=<n>` |

Direction spellings are `unknown`, `in`, `out` and `both`. Error class
spellings are `none`, `total`, `crc`, `symbol`, `framing`, `encoding`,
`congestion` and `other`. A direction on a family that has no direction, or an
error class on a family that has no classes, is rejected.

Range checks are applied at parse time, before anything is stored:

* `util-reported ppb` is accepted up to 2 000 000 000 (100 % is 1 000 000 000;
  the headroom exists so a source may report oversubscription without the runtime
  silently clamping it),
* `signal-quality ppb` is accepted up to 1 000 000 000,
* a `bits32` counter whose value exceeds 0xFFFFFFFF is rejected as out of range,
* `window-ns` must be positive and no longer than the configured history window.

### 1.3 Value spellings

| Enumeration | Accepted spellings |
| --- | --- |
| source authority | `unknown`, `synthetic`, `derived`, `secondary`, `primary` |
| provenance class | `unknown`, `real`, `synthetic`, `replayed` |
| link kind | `unknown`, `physical`, `logical`, `aggregate` |
| counter width | `bits32`, `bits64` |
| administrative state | `unknown`, `enabled`, `disabled` (plus `up`/`down` as synonyms for `enabled`/`disabled`) |
| operational state | `unknown`, `up`, `down`, `degraded` |
| boolean | `true`, `false` |

### 1.4 Strictness rules

* the version field is mandatory and must match exactly,
* unknown keys are rejected, never ignored: a typo must not silently drop a value,
* duplicate keys are rejected,
* field names are limited to 32 bytes and field values to 256 bytes, and both
  must be printable ASCII,
* a record is limited by `Limits::max_record_bytes` (default 4096) and a batch
  by `Limits::max_records_per_batch` (default 4096),
* a record may carry at most 24 fields,
* token fields are limited to `[A-Za-z0-9_.:-]` and 128 bytes,
* the parser never clamps, repairs or defaults a malformed value, and it never
  infers a zero from a missing field,
* `parse_batch` reports every rejected line with its line number and reason; a
  rejected line is counted, not silently skipped.

## 2. Snapshot container

All integers are little endian and written field by field, so the file does not
depend on struct layout, padding, native endianness or compiler version.

### 2.1 Header (88 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `LKOBSNAP` |
| 8 | 4 | container format version (u32) |
| 12 | 4 | minimum reader version (u32) |
| 16 | 8 | payload size in bytes (u64) |
| 24 | 32 | SHA-256 of the payload |
| 56 | 32 | SHA-256 of bytes 0..55 of the header |
| 88 | payload size | payload |

### 2.2 Payload

| Order | Field |
| --- | --- |
| 1 | inner format version (u32), must equal the container format version |
| 2 | runtime identity (128-bit content hash) |
| 3 | incarnation identity (128-bit content hash) |
| 4 | save instant (i64 nanoseconds) |
| 5 | acceptance index (u64) |
| 6 | link facts collection |
| 7 | source record collection |
| 8 | evidence slot collection |
| 9 | revision stream collection |
| 10 | transition collection |
| 11 | totals: accepted, fenced, duplicates, revision gaps, counter resets, counter wraps, transitions dropped (7 x u64) |

Every collection is prefixed by a u64 element count, which is checked against the
policy bound and against the remaining bytes before a single element is decoded.

Each evidence slot carries its link and metric key, its bounded evidence history,
its per-source counter continuity, its acceptance and refusal counters, the
instant of its most recent stream discontinuity and the discontinuity flag.

### 2.3 Writing and rotation

1. the container is serialised and checked against `Limits::max_snapshot_bytes`,
2. it is written to `<path>.tmp`,
3. if `<path>` exists it is renamed to `<path>.prev` (any older `.prev` is
   removed first),
4. `<path>.tmp` is renamed to `<path>`.

The previously good snapshot is therefore never overwritten in place. A crash
between any two steps leaves either the old file, the old backup, or a stale
temporary file that the reader ignores.

### 2.4 Reading and recovery

The reader fails closed. Any of the following makes the file unusable:

* wrong magic, or a file shorter than the header,
* header digest mismatch,
* unsupported container format version, or a minimum reader version newer than
  this build,
* payload size above the configured bound, or a payload length that does not
  match the file size,
* payload digest mismatch,
* truncated payload, inner version mismatch, a collection count above its bound,
  an invalid enumeration value, or trailing bytes after the final total.

When the primary file fails, the reader tries `<path>.prev` exactly once. A
successful fallback is reported through `SnapshotLoadResult::recovered_from_backup`
and `primary_failed`; a failed fallback reports both reasons. Nothing is ever
partially applied: a snapshot is fully decoded or not applied at all.

Restoring never restores time. Freshness is recomputed against the running clock
at every query, so reloading a snapshot cannot make old evidence current. Counter
continuity is restored together with the evidence, because a rate is a property
of the two samples and not of the observing process: the window, the scope, the
topology generation, the revision order and the stream gap epoch all travel with
the samples. A baseline that is no longer usable yields a derivation fault
instead of a rate.

## 3. Transport framing

### 3.1 Frame layout (20 byte header)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic (u32) `0x31464F4C`, the bytes `L` `O` `F` `1` |
| 4 | 1 | protocol version (u8) |
| 5 | 1 | frame type (u8) |
| 6 | 2 | flags (u16), reserved, must be zero |
| 8 | 4 | payload size in bytes (u32) |
| 12 | 8 | FNV-1a 64 digest of the payload |
| 20 | payload size | payload |

### 3.2 Frame types

| Value | Name | Direction | Payload |
| --- | --- | --- | --- |
| 1 | `Ping` | client to server | empty |
| 2 | `Pong` | server to client | `ok` |
| 3 | `PushRecords` | client to server | LOR text |
| 4 | `PushAck` | server to client | counters and per-line errors |
| 5 | `QueryState` | client to server | link name |
| 6 | `StateReport` | server to client | classification summary |
| 7 | `ErrorReport` | server to client | status code and message |
| 8 | `Shutdown` | client to server | empty |

### 3.3 Decoding rules

* a buffer that does not yet hold a whole frame decodes as "incomplete" with an
  Ok status, and the caller reads more and retries,
* a wrong magic, an unsupported version, non-zero reserved flags, a payload
  length above the bound, or a payload digest mismatch is a hard error,
* on a hard error the connection is closed. The decoder never resynchronises by
  scanning for the next magic: a desynchronised stream is not a stream the
  runtime can reason about,
* payloads are bounded by `ServerOptions::max_payload_bytes` and
  `ClientOptions::max_payload_bytes` (default 1 MiB) and responses by
  `ClientOptions::max_response_bytes` (default 4 MiB),
* a connection's read buffer is bounded by
  `ServerOptions::max_read_buffer_bytes` (default 4 MiB),
* a `PushRecords` acknowledgement is sent only after the records have actually
  been applied, so a client that queries immediately after a push observes its
  own write.

## 4. Bounds

The defaults below are compiled in as `Limits` defaults and can be replaced by
supplying a different policy. They are enforced at parse, at ingest, at
derivation, at query and at persistence; exceeding a bound is a reported failure,
never a silent truncation.

| Bound | Default |
| --- | --- |
| links | 4096 |
| sources | 256 |
| evidence retained per (link, metric key) | 16 |
| contenders retained per key | 8 |
| transitions per link | 256 |
| transitions in total | 65536 |
| rows in a result set | 4096 |
| bytes in a record | 4096 |
| records in a batch | 4096 |
| ingest queue depth | 8192 |
| workers | 16 |
| snapshot bytes | 64 MiB |
| transport payload bytes | 1 MiB |
| transport connections | 16 |
