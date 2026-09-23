# Architecture

## Boundary

Fabric Observatory **consumes** provenance-bearing observations and **presents**
generation-bound state. It does not:

* discover topology,
* compute routes,
* mutate forwarding,
* own link, path or device authority,
* perform rollout, drain, maintenance or recovery,
* talk to hardware.

Everything it knows arrived as an observation, and every observation is
attributed. Where the runtime would have to guess, it reports `unknown`,
`incomplete` or `unavailable` instead.

## Layers

    +------------------------------------------------------------------+
    | tooling            fabobs CLI, stdio JSON protocol               |
    +------------------------------------------------------------------+
    | facade             Observatory: evidence, publication, history   |
    +------------------------------------------------------------------+
    | composition        Snapshot::build, evaluate_truth, evaluate_     |
    |                    freshness, query, hierarchy, diff, explain     |
    +------------------------------------------------------------------+
    | domain             Observation, Claim, SourceDescriptor, Policy,  |
    |                    SubjectIdentity, AspectId, Value, TruthState   |
    +------------------------------------------------------------------+
    | foundation         Status/Result, checked arithmetic, SHA-256,    |
    |                    CRC-32C, canonical encoder and decoder, JSON,  |
    |                    bounded Value, time, identities                |
    +------------------------------------------------------------------+
    | persistence        Journal: integrity-checked, rotated, recovery  |
    +------------------------------------------------------------------+

Each layer only depends on the ones below it. Nothing below the facade reads a
clock: `Snapshot::build` takes the evaluation time as an argument, which is
what makes a snapshot a pure function of its inputs.

## Ownership of state

| State | Owner | Lifetime |
|---|---|---|
| Accepted observation records | `Observatory::records_` | bounded by `Limits::max_evidence_records`, oldest evicted first |
| Per-source lineage | `Observatory::sources_` | bounded by `Limits::max_sources` |
| Seen incarnations per source | `Observatory::incarnations_` | bounded by `Limits::max_incarnations_per_source` |
| Claim revisions | `Observatory::revisions_` | bounded by `Limits::max_subjects` |
| Refusals and fenced claims | `Observatory::rejected_`, `fenced_claims_` | bounded by `Limits::max_rejections_retained`, idempotent by observation identity |
| Published snapshot | `Observatory::published_` | one immutable `shared_ptr<const Snapshot>` |
| Snapshot history | `Observatory::history_` | bounded ring, `Limits::max_history_snapshots` |
| Persisted evidence | `Journal` | bounded by part size and part count |

## Data flow

    source process
        |
        | observation: fabric, source, incarnation, sequence, generation,
        |              epoch, observation time, claims, causal references
        v
    Observatory::ingest
        |  1. structural bounds and schema
        |  2. fabric identity, clock skew
        |  3. source registration and declared scope
        |  4. incarnation fence
        |  5. sequence fence / duplicate recognition
        |  6. generation and epoch fences (per source)
        |  7. claim level fences: scope, revision
        |  8. journal append
        v
    accepted evidence  --->  Snapshot::build  --->  immutable snapshot
                                   |                      |
                                   |                      +--> query / hierarchy
                                   |                      +--> diff
                                   |                      +--> explain
                                   |
                                   +--> bounded history ring
                                   +--> journal snapshot marker

## Why composition, not ingestion, applies cross-source supersession

If a stale generation were refused at ingest, the accepted evidence set would
depend on the order in which sources were heard from: whichever source spoke
first would set the bar. Composition-time supersession removes that dependency.
The high-water generation and the current epoch are maximums over the evidence
set, so they are order independent, and evidence below them is retained and
marked rather than discarded — which is also what makes disagreement visible
instead of silently resolved.

A `strict_generation_fence` policy flag restores ingest-time refusal for
deployments that want it. The flag participates in the policy digest recorded in
every snapshot, so the two behaviours can never be confused.

## Determinism

Deterministic identity follows from four rules:

1. Every map that feeds the encoding is ordered, and every vector that feeds it
   is sorted with a total order.
2. Multi-byte integers are big-endian and every variable-length field is length
   prefixed, so the encoding is unambiguous and architecture independent.
3. Nothing derived from an address, a thread identity, a hash-table iteration
   order or a wall clock is ever encoded. Receive times are part of the evidence
   and are therefore encoded — which is why two runs only agree if they agree on
   when they received, and why the CLI offers a manual clock for reproducible
   replay.
4. Values, claims, causal references and metadata are canonicalised on the way
   in, so two observations carrying the same statements in a different order
   have the same identity.

## Failure philosophy

* First-party code does not throw. Fallible operations return `Result<T>` or
  `Status`.
* Internal invariant violations are loud and immediate
  (`FABRIC_OBSERVATORY_CONTRACT`); they are never silent.
* External input is never trusted: it is bounded, canonicalised, and refused
  with a named status.
* Refusals are reported, not hidden. `fabobs` exits non-zero when input was
  refused, and the snapshot carries the refusals that explain its own gaps.
