# Link states

Link Observatory reports one of nine states for a link. The state is a pure
function of the policy, the link facts, the accepted evidence and the recorded
transitions, evaluated at an explicit instant. No clock is read inside the
classifier, no floating point is used, and no iteration order is unspecified, so
the same inputs always produce the same state and the same explanation bytes.

## The states

| State | Meaning |
| --- | --- |
| `down` | A fresh source reports the operational state down, or the administrative state disabled. |
| `conflicting` | Two sources of equal, top authority disagree beyond the configured tolerance. |
| `flapping` | The observed state changed at least `flap_threshold` times inside `flap_window`, or a source reported that many flaps. |
| `erroring` | The derived error ratio is at or above `error_ratio_ppb`. |
| `saturated` | Reported or derived utilization is at or above `saturation_ppb`. |
| `degraded` | A degradation indicator: reported quality at or below `degraded_quality_ppb`, a declared degraded operational state, or a discard ratio at or above `discard_ratio_ppb`. |
| `stale` | Evidence exists, but none of it is usable now. |
| `unknown` | No evidence at all, a required family has no source, a decisive family has a revision gap, or no rule justified a more specific state. |
| `healthy` | A fresh operational state of up, and no adverse indicator. |

## Precedence

Precedence is fixed and is part of the contract, not an implementation detail:

```
unknown (no evidence)
  > conflicting
  > down
  > flapping
  > erroring
  > saturated
  > degraded
  > unknown (unsupported or incomplete decisive evidence)
  > stale
  > healthy
```

Two consequences are deliberate:

* A link that is down right now is reported `down`, even if it flapped on the
  way there. Flapping describes the history that produced the present state and
  is reported by the rule trace, not by the state name.
* `unknown` appears twice because "I have nothing" and "I cannot justify a claim
  with what I have" are different situations. The explanation distinguishes them:
  the first has `rule 800 no-evidence: fired=true`, the second
  `rule 810 unsupported-decisive` or `rule 820 incomplete-decisive`.

## Rules

Every rule is evaluated on every classification, fired or not, and appears in the
explanation in ascending rule identifier order. Rule identifiers are stable and
are never renumbered.

| Id | Rule | Fires when |
| --- | --- | --- |
| 100 | `oper-down` | The winning operational state is `down`. |
| 110 | `admin-disabled` | The winning administrative state is `disabled`. |
| 200 | `source-conflict` | Any family is in conflict. |
| 300 | `flapping` | Observed state transitions in the flap window reach the threshold. |
| 310 | `source-flap-report` | A source reported at least the threshold number of flaps. |
| 400 | `erroring` | The derived error ratio reaches the threshold. |
| 500 | `saturated-derived` | A derived utilization reaches the threshold. |
| 510 | `saturated-reported` | A reported utilization reaches the threshold. |
| 600 | `degraded-quality` | A quality indicator is at or below the threshold, or was declared degraded. |
| 610 | `degraded-discards` | The derived discard ratio reaches the threshold. |
| 620 | `degraded-oper-state` | The winning operational state is `degraded`. |
| 700 | `stale-evidence` | Accepted evidence exists but none of it is eligible. |
| 800 | `no-evidence` | No evidence has ever been accepted for the link. |
| 810 | `unsupported-decisive` | A required family has no source. |
| 820 | `incomplete-decisive` | A decisive family has a revision gap inside the derivation window. |
| 900 | `healthy` | Operational state up, and no adverse indicator. |

## Eligibility

Evidence is eligible only when it is fresh, inside the scope currently in force
for its source, and inside the generation currently in force for its link. Only
eligible evidence can support a claim about the present. Evidence that is
accepted but not eligible is still retained, reported, and used for history and
conflict reporting.

## Winner selection

For each metric key the runtime selects one winning reading among the eligible
contenders, in this order:

1. highest source authority,
2. latest observation time,
3. latest receive time,
4. lowest source identifier.

The ordering is total, so the winner never depends on insertion order.

## Conflict

Conflict is evaluated among the eligible contenders that share the highest
authority:

* a single top-authority source yields `insufficient` (no corroboration),
* two or more top-authority sources that agree yield `corroborated`,
* two or more that disagree beyond `conflict_tolerance_ppb` yield `conflicting`.

A disagreement with a strictly lower authority source is not a conflict: it is
recorded and it loses the winner selection.

Discrete readings (administrative and operational state) disagree when their
values differ and neither is `unknown`. Continuous readings (reported utilization
and quality) disagree when their relative difference exceeds the tolerance.
Counter families are compared on their derived utilization over the same window,
never on raw counter values: two sources have independent baselines, so comparing
raw counters would report a conflict that does not exist.

## Derived utilization

Utilization is derived from a cumulative octet counter over a direction and a
declared capacity:

```
utilization_ppb = delta_bytes * 8 * 10^18 / (capacity_bps * window_ns)
```

The computation is exact integer arithmetic with a 128-bit intermediate. It is
performed only when all of the following hold:

* the link declares a non-zero capacity for that direction,
* two samples exist for the same source, key, scope and generation,
* the sequence number strictly increased and the source stream lost no revision
  between them (the gap epoch is unchanged),
* the window is positive and no longer than `derivation_window`,
* neither sample triggered a counter reset,
* the destination range is representable.

Any other case yields `derived-utilization=none` together with a derivation
fault, never a zero. `0` means "measured, and it was zero".

## Flap counting

Only observed administrative and operational state changes count towards
flapping, plus source-reported flap counts. Classification changes are recorded
as their own transition kind and never count: otherwise a link whose evidence
quality changed would look like a flapping link.
