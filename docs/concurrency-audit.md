# Concurrency and lock audit

## Lock hierarchy

| Level | Lock | Guards |
|---|---|---|
| 0 | `IngestPipeline::Shard::mutex` | one shard's bounded queue |
| 0 | `IngestPipeline::counters_mutex_` | pipeline counters |
| 1 | `Observatory::state_mutex_` (`std::shared_mutex`) | accepted evidence, source lineage, published snapshot, history |
| 2 | the journal's file handle | append and recovery |

Two rules make a deadlock impossible to construct rather than merely unlikely:

1. A lock of level *N* is only ever acquired while holding locks of strictly
   lower level.
2. **The runtime never holds two locks at the same time.** The pipeline releases
   its shard lock before calling into the observatory, `stats()` reads the
   counters and the shard peaks in two separate critical sections, and no lock is
   held across a call that could take another.

Consequence: there is no cycle to find, because there is no pair of held locks.

`Observatory` uses a single `std::shared_mutex`. Readers take it shared and
receive a `shared_ptr<const Snapshot>`; writers take it exclusively. No
operation takes the same lock twice, and every lock in the runtime is
non-recursive, so an accidental re-entry is a self-deadlock in waiting rather
than a subtle bug.

## The runtime auditor

`LockOrderGuard` is created immediately after a lock is acquired and destroyed
immediately before it is released. On every acquisition it checks, against
thread-local state:

* **re-entrancy** — the same thread acquiring a level it already holds is a
  contract violation, reported and aborted;
* **ordering** — acquiring a level less than or equal to a level already held is
  a contract violation;
* **depth** — nesting deeper than the auditor supports is a contract violation.

`lock_audit_report()` returns acquisitions, observed maximum depth, and both
violation counters. The report is deterministic: it never contains thread
identities or timings.

The auditor is compiled in by default (it costs a thread-local increment per
lock) and `FABRIC_OBSERVATORY_LOCK_AUDIT=ON` makes the intent explicit. The
Debug configuration in the validation runs turns it on, and
`concurrency_runtime` asserts that the report is clean and that the observed
maximum depth is at most 2 across the whole concurrent suite.

## Ingest pipeline

Observations are sharded by source identity:

    shard = hash(source) % worker_count

The hash depends only on the identity — never on an address, a thread identity or
the time of day. Each worker consumes exactly one shard, so:

* a source's observations are processed in submission order, which is what its
  sequence fence requires;
* different sources proceed in parallel;
* the accepted evidence set does not depend on thread scheduling or on the
  worker count, which `concurrency_runtime` verifies by running the same
  stream with 1, 2, 3 and 8 workers and comparing the resulting snapshot
  identity.

The queue is bounded per shard. `submit` reports `QueueFull` rather than
blocking or growing, so back pressure is explicit and the caller decides what to
do. A shard being full does not block another shard.

## Shutdown

`IngestPipeline::shutdown` is a real join: it sets the stop flag, wakes every
worker, joins every thread, and only then clears the thread list. `Drain`
processes everything that was submitted; `Discard` discards what is queued and
counts exactly what it discarded, so nothing disappears without being accounted
for. `submit` after shutdown is refused with `ShuttingDown`.

## No torn views

The published snapshot is immutable and reached through a
`shared_ptr<const Snapshot>`, so a reader can never observe a partially applied
update. `concurrency_runtime` proves this actively rather than by argument:
concurrent readers repeatedly recompute the digest of the snapshot they hold and
compare it with the digest the snapshot advertises, and repeatedly check that
`SnapshotId` agrees with the digest. Any torn view would be caught as a digest
mismatch; the test asserts the count of mismatches is exactly zero.

The same suite asserts that after all writers have joined, the published
snapshot is identical to the one a single-threaded replay of the same evidence
produces.

## Re-entrancy audit result

| Question | Answer |
|---|---|
| Is any lock recursive? | No. Every lock is `std::mutex` or `std::shared_mutex`. |
| Can a thread take the state lock twice? | No. Every acquisition site is reached at most once per call path, and the auditor aborts if one repeats. |
| Is a lock ever held across a call that takes another lock? | No. `publish_locked` is called with the state lock held and takes only the journal handle, which is level 2. |
| Does the pipeline hold a shard lock while calling into the observatory? | No. The item is moved out of the queue and the lock released first. |
| Does `stats()` nest the counters lock inside the shard locks? | No. Two separate critical sections. |
| Can shutdown be called twice? | Yes, and the second call is a no-op. |
| Is there any timeout, sleep or polling loop anywhere in the runtime or the tests? | No. Workers block on a condition variable with a predicate, shutdown joins, and tests join. |
