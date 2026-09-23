# Persistence

## File header (128 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `FABOBSJ1` |
| 8 | 4 | format version |
| 12 | 4 | header size (128) |
| 16 | 16 | fabric identity |
| 32 | 8 | creation wall clock |
| 40 | 32 | chain seed for the first record of this part |
| 72 | 52 | reserved, zero |
| 124 | 4 | CRC-32C of bytes 0..123 |

## Record header (116 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | payload length |
| 4 | 4 | payload CRC-32C |
| 8 | 1 | record kind (observation, restart marker, snapshot marker) |
| 9 | 3 | flags, zero |
| 12 | 8 | record sequence within the part |
| 20 | 32 | previous chain value |
| 52 | 32 | SHA-256 of the payload |
| 84 | 32 | chain value |

`chain = SHA-256(prev_chain || content_digest || kind || sequence)`

Every payload is the canonical encoding of its structure. Record payloads are
self-describing and versioned, so a future format can be recognised and refused
rather than misread.

## Recovery

Recovery reads parts from oldest to newest and verifies, per record, in this
order:

1. the record header is complete,
2. the payload length is inside the configured bounds,
3. the previous chain value continues the running chain,
4. the payload is complete,
5. the payload CRC-32C matches,
6. the payload SHA-256 matches,
7. the chain value is self-consistent,
8. the payload decodes and validates against the current limits.

The first failure stops recovery. Nothing is repaired, truncated or deleted:
the file is left exactly as it was found, the diagnostics name the offset and
the failing check, and `fabobs verify` exits non-zero.

A short tail is reported as `truncated` with the number of unusable trailing
bytes — everything from the start of the incomplete record to the end of the
part — while a short or damaged record anywhere else is reported as `corrupt`.

A header whose format version is not this build's is **refused**, not guessed
at. A journal written for a different fabric identity is refused rather than
merged.

## Rotation and bounded growth

When appending a record would cross `max_bytes`, the journal rotates: the
oldest part is removed, the remaining parts shift up by one, and the active file
becomes part one. At most `max_files` parts exist, so total persistence
growth is bounded by `max_bytes × max_files`. The chain continues across
rotations, so a record removed by rotation cannot be silently replaced.

## Compaction

`Journal::compact` rewrites the valid prefix into a new part, with a fresh
header and a rebuilt chain. It is the only operation that discards bytes and it
is always explicit.

## The journal is readable while the runtime holds it

On Windows the journal is opened with `_SH_DENYNO`, so inspection tooling can
read it while the runtime is writing. A runtime that locked its own journal
exclusively would block the tooling that exists to look at it.

## Snapshot markers

Every publication appends a snapshot marker carrying the snapshot identity,
generation, epoch and publication time. These are the persisted record of the
snapshot lineage; `fabobs verify` reports how many exist.

A snapshot itself is **not** rebuilt after a restart, and the tool says so
explicitly. Recovered evidence is never presented as fresh, so a rebuilt
snapshot would not have the recorded identity; pretending otherwise would be
worse than reporting the limitation. Snapshot history is therefore per process.
