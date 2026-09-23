# The truth model

## Evidence classification

Every accepted claim is retained with its full provenance and classified:

| Field | Meaning |
|---|---|
| `source`, `incarnation`, `sequence` | who said it, in which run of that process, at which point in its stream |
| `authority` | the strongest authority the source declared: `none`, `inferred`, `reported`, `corroborated`, `authoritative` |
| `observed_at` | when the source says it observed the fact |
| `received_at` | when this runtime received the observation |
| `freshness` | `fresh`, `aging` or `stale` at the snapshot's evaluation time |
| `superseded_generation` | evidence for a generation below the observed high water |
| `superseded_epoch` | evidence for an epoch below the current epoch of the high-water generation |
| `superseded_incarnation` | evidence from a superseded run of the same source |
| `superseded_by_later_sequence` | the same source made a later statement about the same aspect |
| `fenced`, `fence` | the claim was refused at ingest, with the reason |
| `recovered`, `recovery_epoch` | the evidence came back from persistence |

A claim is **current** when none of the superseded flags is set and it is not
fenced. A claim is **assertive** when it is current, supported by its source, and
fresh. Only assertive claims can support a positive statement.

## Freshness

Freshness is a pure function of three inputs: the evidence reference time, the
snapshot's evaluation time, and the freshness policy.

    age = evaluation_time - min(observed_at, received_at)

    age <= fresh_window          -> fresh
    age <= aging_window          -> aging
    otherwise                    -> stale

Three deliberate choices:

* **The earlier of observation and receive time governs.** A delayed observation
  can never look fresher than the fact it reports, and a source whose clock lags
  is treated conservatively rather than generously.
* **The subtraction saturates.** Timestamps are externally supplied; an extreme
  value is reported as "very old" rather than wrapping into a plausible looking
  small one.
* **Recovered evidence is capped at `aging` unless the policy says otherwise.**
  A restart can never resurrect a fresh assertion. The default is to cap; an
  operator who wants the other behaviour must say so explicitly, and the choice
  is part of the policy digest recorded in every snapshot.

## Truth evaluation

The evaluation order is fixed and total. The first rule that applies wins:

1. **unknown** — no claims at all. Absence of evidence is not evidence of
   absence, and the runtime says so rather than guessing.
2. **unsupported** — claims exist, but every one of them declares that its
   source cannot supply the aspect.
3. **conflicting** — two or more distinct values are asserted by *assertive*
   claims. Both survive; the runtime does not pick a winner and does not
   synthesise a majority.
4. **stale** — claims exist but none is assertive.
5. **incomplete** — assertive claims agree, but the coverage requirement for the
   aspect is unmet.
6. **known** — assertive claims agree and coverage is satisfied. Only here does
   the snapshot carry an agreed value.

Disagreement between assertive claims is reported as a conflict; disagreement
with a *non-assertive* claim is not, because superseded, stale or fenced
evidence cannot contradict current evidence — it is simply older or refused.

### Coverage

Coverage requirements come from a three-level resolution, highest priority
first:

1. an explicit override in the policy,
2. the well-known aspect descriptor,
3. the policy's default coverage.

| Aspect | Fresh sources required | Authority required |
|---|---|---|
| `identity.labels` | 1 | none |
| `topology.parent` | 1 | none |
| `link.state` | 1 | reported |
| `device.model` | 1 | none |
| `port.state` | 1 | reported |
| `path.state` | 2 | reported |
| `capacity.bandwidth_bps` | 1 | reported |
| `reachability.state` | 2 | corroborated |
| `authority.owner` | 1 | authoritative |
| `failure.state` | 1 | reported |
| `partition.membership` | 1 | reported |
| `congestion.level` | 1 | reported |
| `operational.admin_state` | 1 | reported |
| `operational.health` | 1 | none |

Aspect identifiers are `<domain>.<name>`, where the domain must be one of the
thirteen known domains. Names outside the well-known table are accepted when
`reject_unknown_aspects` is false (the default), which is what makes the
runtime vendor neutral; they resolve to the default coverage.

## Subjects and hierarchy

A subject exists in a snapshot **only because something was asserted about it**.
A parent named by a `topology.parent` claim is not materialised as a subject;
instead the child reports `parent_absent`, and the entity appears in the
hierarchy result's `unattached` list. The runtime never invents an entity.

An entity is **unattached** when the runtime cannot place it: it has no observed
parent, or its parent is not present in this snapshot. `Fabobs hierarchy`
reports both cases by name rather than hiding them.

Cycles in observed containment cannot be entered from a root and are therefore
never traversed; the traversal is iterative, depth-bounded and reports what it
limited.

## Subject-level truth

A subject carries the **least informative** truth state across its aspects.
Consumers that need one word for a subject get the pessimistic one.

## Causality

Observations may carry references to antecedent observations. The vocabulary is
deliberately weaker than causation:

| Strength | Meaning |
|---|---|
| `correlates-with` | recorded correlation only |
| `temporally-precedes` | recorded temporal ordering only |
| `contributory-evidence` | recorded contributing evidence only |

Every rendered line ends with "no causal relationship is asserted". The runtime
never claims that one event caused another, and no output omits the disclaimer.
