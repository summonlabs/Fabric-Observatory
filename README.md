# Fabric Observatory

A standalone, vendor-neutral **Fabric OS observational runtime**.

Fabric Observatory owns the *global observational view* of fabric state. It
consumes provenance-bearing observations from adjacent systems and presents
generation-bound state across identity, topology, links, devices, ports, paths,
capacity, reachability, authority, failures, partitions, congestion and
operational state.

It is deliberately **not** a topology discovery engine, a routing engine, a
forwarding control plane, a link or path authority, or a rollout, drain,
maintenance or recovery system. It observes, records what it was told, says
exactly how much it trusts it, and never invents the rest.

    C++20 · CMake ≥ 3.25 · no third-party dependencies · Apache-2.0

---

## What is implemented

### Typed identities and provenance

Every domain object has its own strongly typed identity, and no two kinds are
interchangeable:

| Identity | Kind | Notes |
|---|---|---|
| `FabricId`, `SiteId`, `PodId`, `RackId`, `DeviceId`, `SourceId`, `IncarnationId` | 128-bit | derived deterministically from canonical text with SHA-256 and a domain tag |
| `PortId` | `DeviceId` + index` | a port is only meaningful with respect to its device |
| `LinkId` | ordered port pair | stored in canonical order, so a link observed from either end has one identity |
| `PathId` | hop sequence | content derived: same hops, same path, whoever observed it |
| `EntityId` / `SubjectIdentity` | canonical 128-bit + typed carrier | one-way derivation, never inverted |
| `GenerationId`, `EpochId`, `SourceSequence`, `ClaimRevision`, `HistoryIndex`, `RestartEpoch` | monotonic counters | each with its own type |

Every observation carries: **what** was observed, **by which source**, **for
which generation and epoch**, at **what observation and receive times**, under
**which source incarnation**, and **whether the evidence is fresh, stale,
conflicting, incomplete, unsupported or unknown**.

### Truth states

Six truth states, evaluated in a fixed order, are the only vocabulary in which
the runtime reports fabric state:

| State | Meaning |
|---|---|
| `unknown` | no evidence was accepted at all |
| `unsupported` | evidence exists but the reporting source cannot supply the aspect |
| `conflicting` | fresh evidence disagrees and the runtime will not pick a winner |
| `stale` | current evidence exists but none of it is fresh |
| `incomplete` | fresh evidence agrees but coverage requirements are unmet |
| `known` | fresh, agreed, sufficiently covered evidence |

**Absence of evidence is never positive evidence.** Only `known` may be
treated as an assertion about the fabric. Disagreement is never collapsed:
every retained claim stays visible with its source, authority, sequence,
observation time, receive time and freshness verdict.

### Canonical immutable snapshots

A snapshot is an immutable, canonically ordered, generation-bound view. Its
identity is the SHA-256 of the canonical encoding of everything it asserts, so
two snapshots with the same fabric, generation, epoch, evaluation time, policy
and evidence have the same identity — regardless of arrival order, thread
scheduling or container iteration order.

The digest covers the fabric view and the ingest refusals that explain it. It
deliberately excludes publication bookkeeping (history position and publication
time), so identity is a function of content alone, and it deliberately excludes
per-source refusal counters, so retrying a stream cannot move the published
view.

### Ingestion with explicit fences

Every refusal is a named condition, never a silent drop:

| Fence | When |
|---|---|
| `FencedSequence` | a source's sequence does not advance |
| `FencedDuplicateContent` | byte-identical replay of accepted evidence (idempotent) |
| `FencedIncarnation` | a superseded boot of a source comes back |
| `FencedStaleGeneration` | a generation below the one the source already reported |
| `FencedStaleEpoch` | an epoch below the one the source already reported for that generation |
| `FencedRevision` | a claim revision below the last one seen for that subject and aspect |
| `FencedAuthority` | a claim outside the source's declared aspect scope |
| `ClockInconsistent` | an observation dated beyond the clock-skew tolerance |
| `FabricMismatch` / `Unsupported` / `SourceUnknown` | wrong fabric, unknown schema, unregistered source |

Generation and epoch supersession across *different* sources is applied at
composition time, not at ingest: evidence for a superseded generation is
retained, marked superseded, and can never support an assertion. That is what
makes the accepted evidence set — and therefore the snapshot — independent of
the order in which sources are heard from. A `strict_generation_fence` policy
flag trades that order independence for an immediate refusal, and the choice is
visible in the policy digest recorded in every snapshot.

### Deterministic tooling

`fabobs` provides `ingest`, `snapshot`, `query`, `hierarchy`, `diff`, `history`, `explain`, `sources`, `verify`, `policy`
and a newline delimited JSON protocol on stdio (`serve --stdio`) that can be
driven by an independent process.

### Integrity-checked persistence

The journal is versioned, CRC-32C checked per record, and chained with SHA-256
across records and across rotated parts. Recovery verifies the header checksum,
the magic, the format version, the fabric identity, the chain link, the payload
checksum and the payload digest, and stops at the first bad record. Nothing is
repaired, truncated or deleted automatically; the valid prefix can be rewritten
with an explicit compaction.

Rotated parts are bounded by count and by size, so persistence growth is
bounded.

### Conservative restart

Persisted dynamic evidence never silently becomes fresh. Every record that comes
back from the journal is marked as recovered at a new restart epoch, and its
freshness is capped at `aging` for ever, no matter how recently it was
received. Provenance and values survive byte for byte; freshness degrades
monotonically. Truth states therefore degrade after a restart, and the snapshot
says so.

Snapshot history is per process: a snapshot cannot be rebuilt after a restart
because recovered evidence is never presented as fresh, and the tool reports
that explicitly rather than inventing a diff.

### Bounded everywhere

Workers, queues, payloads, metadata, values, history, result sets, journals,
aggregation windows and evidence records are all bounded, and every bound is
named in `Limits`. Externally derived sizes go through checked arithmetic;
timestamps go through saturating arithmetic, so an extreme value is reported as
"very old" or "far in the future" rather than wrapping into a plausible looking
small number.

### Concurrency

One `std::shared_mutex` guards the evidence and the published snapshot.
Readers take it shared and receive a `shared_ptr<const Snapshot>`; writers take
it exclusively. Locks are never nested in the opposite order, and the runtime
keeps exactly one lock level live at a time. A runtime lock auditor checks
ordering and re-entrancy on every acquisition. The ingest pipeline shards by
source identity, so a source's order is preserved and the accepted evidence set
does not depend on thread scheduling. Shutdown is a real join.

---

## Build

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

Options:

| Option | Default | Effect |
|---|---|---|
| `FABRIC_OBSERVATORY_BUILD_TESTS` | ON (top level) | build the test suite |
| `FABRIC_OBSERVATORY_BUILD_TOOLS` | ON | build `fabobs` |
| `FABRIC_OBSERVATORY_BUILD_EXAMPLES` | ON (top level) | build the examples |
| `FABRIC_OBSERVATORY_BUILD_BENCHMARKS` | ON (top level) | build the benchmarks |
| `FABRIC_OBSERVATORY_WARNINGS_AS_ERRORS` | ON | `/W4 /WX` on MSVC, `-Werror` elsewhere |
| `FABRIC_OBSERVATORY_ENABLE_ASAN` | OFF | build and link with AddressSanitizer |
| `FABRIC_OBSERVATORY_LOCK_AUDIT` | OFF | compile the lock auditor in explicitly |
| `FABRIC_OBSERVATORY_DOWNSTREAM_PROOF` | ON | register the downstream `find_package` proof as a test |

The library is delivered as a static library and exported as
`FabricObservatory::fabric_observatory`.

### Install and use from another project

    cmake --install build --prefix /some/prefix

    find_package(FabricObservatory 1.0 REQUIRED)
    target_link_libraries(my_target PRIVATE FabricObservatory::fabric_observatory)

`consumers/find_package` is an independent project that does exactly this. It
does not use `add_subdirectory`, `FetchContent` or any path inside this
repository; the `downstream_find_package` test installs the runtime into a
staging prefix, configures the consumer against that prefix alone, builds it,
runs it and checks the installed headers and package configuration files.

---

## Tests

Fourteen CTest entries: thirteen suites plus the downstream packaging proof.
There are **no timeouts and no sleeps anywhere** in the suite; every worker is
joined, so a hang is visible as a hang rather than masked by a timer.

| Suite | Covers |
|---|---|
| `unit_identity` | SHA-256 and CRC-32C known vectors, identity derivation and round trips, time formatting, value codec and bounds, JSON strictness, checked arithmetic, status codes |
| `unit_truth` | the truth state table, coverage policy resolution, freshness boundaries, recovered-evidence cap |
| `unit_snapshot` | content-addressed identity, order independence, publication bookkeeping exclusion, ordering, self-update versus conflict, topology parents, statistics |
| `unit_query_diff` | filters, hierarchical queries, cycle and absent-parent handling, diffs, history |
| `unit_persistence` | journal round trip, corruption at every layer, truncation, version and fabric refusal, rotation, compaction, recovery bounds |
| `unit_bounds` | every configured bound, plus bounded history and result sets |
| `integration_ingest` | the complete ingest path and every fence, scope declarations, authority caps, strict generation fencing, disagreement, partial visibility, interleaving independence, causal wording |
| `integration_restart` | conservative restart, lineage across restarts, post-restart fencing, corruption and truncation recovery, journal append without recovery |
| `property_invariants` | 60 seeded randomised workloads: snapshot invariants, interleaving independence, replay idempotence, accounting, monotone freshness, query reproducibility, bounded history |
| `adversarial_input` | malformed and hostile JSON, extreme values, deep and wide nesting, truncated and bit-flipped canonical input, source and subject floods, clock extremes, identifier abuse |
| `concurrency_runtime` | torn-view detection under concurrent readers and writers, pipeline ordering across worker counts, queue bounds, cancellation accounting, real drain shutdown, query and diff during ingest, lock audit |
| `process_transport` | a real child process over real pipes: every operation, one response per request, cross-process identity agreement, restart across processes, hostile unterminated input |
| `e2e_cli` | the tool as an operator uses it: exit codes, deterministic policy output, ingest, snapshot, query, hierarchy, history, explain, sources, verify, corruption reporting |
| `downstream_find_package` | install, independent configure, build, run, and package content |

Build configurations exercised: **Release** and **Debug** (with the lock
auditor) with `/W4 /WX` and zero first-party warnings, and **Debug +
AddressSanitizer** (MSVC `/fsanitize=address`, with the sanitizer runtime copied
next to every binary so the sanitizer is actually active).

---

## Benchmarks

Benchmarks measure completed work: each one counts the units it finished and
folds its results into a checksum, so nothing measured can be optimised away.
Representative Release figures on a 16-core Windows workstation, with 2400
claims over 400 subjects for the snapshot benchmark:

| Benchmark | Work completed | Result |
|---|---|---|
| `bench_snapshot` | 10 full snapshot builds (2400 claims, 800 conflicting aspects) | ≈ 80 ms total, ≈ 124 snapshots/s, identity reproduced exactly |
| `bench_query` | 20 whole-snapshot queries, 50 aspect queries, 50 diffs, 20 hierarchy traversals | sub-millisecond each at 1024 subjects × 8 sources |
| `bench_ingest` | 4096 observations ingested, then published and rebuilt | see the program output for the current machine |
| `bench_persistence` | 4096 journal records appended and recovered | see the program output for the current machine |

Run them directly (`build/bench_snapshot` and so on); they print the completed
count, elapsed time, throughput and checksum.

---

## Examples

| Example | Shows |
|---|---|
| `example_ingest_walkthrough` | building observations by hand, ingesting, and reading the snapshot summary and an explanation |
| `example_embedded_query` | two disagreeing sources, a preserved conflict, and a hierarchical query |
| `example_ndjson_bridge` | the newline delimited JSON protocol in process |

---

## Documentation

| Document | Contents |
|---|---|
| `docs/architecture.md` | layers, ownership, data flow, module map |
| `docs/truth-model.md` | the truth state lattice, freshness, coverage, evidence classification |
| `docs/persistence.md` | journal format, integrity chain, recovery, rotation, compaction |
| `docs/concurrency-audit.md` | lock hierarchy, re-entrancy audit, ownership rules, shutdown |
| `docs/proof-surfaces.md` | REAL / SYNTHETIC / UNSUPPORTED labelling of every claim |
| `docs/cli.md` | the `fabobs` commands and the stdio protocol |

---

## REAL, SYNTHETIC and UNSUPPORTED

This runtime has never been run against a switch, ASIC, RDMA device, InfiniBand
fabric, NVLink domain, multi-host cluster or a real telemetry source. Nothing
here claims otherwise. In short:

* **REAL** — the C++20 runtime, its identities, snapshots, digests, fences,
  persistence, restart behaviour, concurrency behaviour, tooling, packaging and
  every test result reported above.
* **SYNTHETIC** — all fabric content used in tests, examples and benchmarks.
  Every device, link, path and source is generated by the test or benchmark
  itself.
* **UNSUPPORTED** — hardware, vendor protocol and multi-host claims, along with
  any claim that observations in this repository came from real equipment.

See `docs/proof-surfaces.md` for the itemised list.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
