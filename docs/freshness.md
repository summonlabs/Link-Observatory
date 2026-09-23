# Freshness, confidence and fencing

## Freshness is computed, never stored

Every piece of evidence carries the instant the source observed it and the
instant this runtime received it. Freshness is computed at evaluation time as the
worse of the two ages:

```
effective_age = max(now - observed_at, now - received_at)
fresh         when effective_age <= fresh_within
stale         when effective_age <= usable_within
expired       otherwise
```

Received age is included because a sample that was observed long ago but only
just arrived tells the runtime nothing about the interval in between. An
observation dated in the future is not treated as extra fresh: its age is clamped
to zero and the record is flagged `backwards_clock`.

Freshness is never persisted. A snapshot stores timestamps, so a restart
recomputes every age against the running clock and old evidence cannot become
current merely by being reloaded.

## Confidence

Confidence is derived, deterministic, and never asserted by a source:

| Condition | Confidence |
| --- | --- |
| freshness `unknown` or `expired` | `unknown` |
| conflicting | `low` |
| stale | `low` |
| fresh, authority `primary` | `high` |
| fresh, authority `secondary` | `medium` |
| fresh, authority `derived` or `synthetic` | `low` |
| fresh, authority `unknown` | `unknown` |
| completeness `incomplete` | one step down |

The link level confidence is the weakest confidence among the supported families
that hold a value. One weak family is enough to lower the whole link, which is
the conservative direction.

Authority and provenance class come from source registration, never from the
record payload. A source cannot promote itself, and evidence keeps the standing
it was admitted under: re-registering a source does not retroactively upgrade
history.

## Distinct outcomes

The runtime keeps five outcomes apart, in the report and in the explanation:

| Outcome | Where it appears |
| --- | --- |
| `unknown` | no value, no support, or an unclassifiable situation |
| `stale` | a value exists but is past `fresh_within` |
| `expired` | a value exists but is past `usable_within`; usable as history only |
| `incomplete` | a revision gap inside the derivation window; derived values unavailable |
| `unsupported` | no configured source can supply the family |

A family that has never held evidence reports `support=unsupported`,
`freshness=unknown` and no value. It is never reported as zero.

## Fencing

Fencing is applied before evidence is stored. A fenced observation never mutates
current evidence; it is counted, attributed to a reason, and dropped.

| Reason | Trigger |
| --- | --- |
| `unknown-source` | The source is not registered, or is retired. |
| `unknown-link` | The link is not registered and implicit links are disabled. |
| `unsupported-family` | The family is disabled in configuration. |
| `unsupported-provenance` | The source did not declare real, synthetic or replayed. |
| `invalid-value` | The reading is outside its accepted range. |
| `duplicate-observation` | Byte-identical content at the same revision. |
| `revision-replay` | A lower revision, or a different payload at the same revision. |
| `epoch-behind` | A scope that was already superseded. |
| `incarnation-behind` | A source incarnation that was already superseded. |
| `generation-behind` | A topology generation that was already retired. |
| `authority-downgrade` | A lower-authority source trying to replace newer higher-authority evidence. |
| `store-full` | The bounded evidence store is at capacity. |

A revision gap is not a fence: the record is accepted and the affected slot is
marked `incomplete` for `derivation_window`, which makes derived values
unavailable without discarding the accepted samples.

## Counter continuity

Sequence numbers are scoped to (source, incarnation, epoch). Two samples of the
same key are contiguous when the sequence number strictly increased and the
stream lost no revision between them. The stream carries a gap epoch, incremented
every time a revision is missing, and two samples are contiguous exactly when
their gap epochs are equal.

This matters because a real source reports several metric families under one
revision counter: consecutive samples of one family are therefore not consecutive
revisions, and a rule based on "revision plus one" would make a multi-family
stream impossible to derive from.

A backwards step in a counter is classified as a wrap only when the previous
value sits in the top quarter of the declared counter width. Otherwise it is a
reset. A reset never yields a delta: it yields "no derivable value". Ambiguity
always resolves against the fabrication of traffic.

## Restart

A restart is an identity change for derivation purposes:

* evidence keeps its original timestamps, so it ages naturally,
* scopes, retired scopes and the highest revision per source are restored, so a
  replayed record is still fenced,
* topology generation currency is restored, so an old topology is still fenced,
* counter baselines are restored, so a read-only process can still report a
  utilization that the evidence supports.

Restoring a baseline cannot resurrect a stale value. A derived rate is recomputed
only when both samples are fresh, in the current scope and generation, strictly
ordered in the stream and inside the derivation window; the window, the scope, the
generation, the revision and the gap epoch are all stored with the samples. A rate
whose basis has expired is reported as a derivation fault, never as a value.

The totals that describe the evidence (observations accepted, observations
fenced, transitions recorded) are restored with it, so a report printed by a later
process is consistent with the evidence it shows. The per-reason breakdown of
refusals and the classification history are session statistics and restart at
zero; accepted evidence and the transition log are persisted.
