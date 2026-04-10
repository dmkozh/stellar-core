# H003: Replace getReadWriteKeysForStage with Entry-Level RO TTL Flag

**Date**: 2025-07-14
**Subsystem**: ledger
**Severity**: Low
**Impact**: Serial post-parallel-apply throughput (5-10% improvement at T=8)
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

After parallel execution completes, the serial `commitChangesFromThreads`
function should be able to distinguish read-only TTL bumps from read-write
modifications WITHOUT reconstructing a full set of all RW keys in the stage.
The information about whether an entry was modified via RO TTL bump path or
RW modification path is already known at the point of modification inside
each worker thread.

## Mechanism

`commitChangesFromThreads` (ParallelApplyUtils.cpp:546-559) calls
`getReadWriteKeysForStage(stage)` which iterates ALL transactions in the stage,
extracts their RW footprint entries, and calls `getTTLKey(lk)` for each Soroban
entry to build an `unordered_set<LedgerKey>`. This set is then passed to
`commitChangeFromThread` → `maybeMergeRoTTLBumps` where it's used solely to
check whether a given TTL key is in the RW set (i.e., whether a TTL change
came from a RW modification or a RO TTL bump).

The cost of `getReadWriteKeysForStage`:
- Iterates ~3,200 transactions × ~2 RW Soroban keys = ~6,400 keys
- Calls `getTTLKey(lk)` per Soroban key = ~6,400 SHA-256 computations
- Inserts ~12,800 keys (original + TTL) into unordered_set
- Total: ~6,400 SHA-256 at ~1μs each + ~12,800 hash insertions at ~200ns each
  = ~9ms serial

This entire function can be eliminated by tracking the RO/RW provenance of
TTL entries at the point where they're written in the worker threads. During
`flushRoTTLBumpsInTxWriteFootprint` and `flushRemainingRoTTLBumps`, the code
already KNOWS which entries are RO TTL bumps (that's the entire purpose of
these functions). A simple flag on `ThreadParallelApplyEntry` or a separate
set of "RO TTL bump" keys per thread would carry this information through to
`commitChangeFromThread` without needing the global readWriteSet.

## Trigger

Run SAC benchmark at T=8. Profile the serial gap between parallel execution
completing and `commitChangesToLedgerTxn` starting. The
`getReadWriteKeysForStage` call will appear as a ~9ms serial bottleneck in
this gap, inside `commitChangesFromThreads`.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:99-118` — `getReadWriteKeysForStage`
  iterates all txs and calls getTTLKey per RW Soroban key (serial bottleneck)
- `src/transactions/ParallelApplyUtils.cpp:546-559` —
  `commitChangesFromThreads` calls getReadWriteKeysForStage then passes set
  to each thread's commit
- `src/transactions/ParallelApplyUtils.cpp:480-507` — `maybeMergeRoTTLBumps`
  checks `readWriteSet.find(key)` to determine if a TTL entry is an RO bump
  (the ONLY consumer of the readWriteSet)
- `src/transactions/ParallelApplyUtils.cpp:509-542` — `commitChangeFromThread`
  passes readWriteSet through to maybeMergeRoTTLBumps
- `src/transactions/ParallelApplyUtils.cpp:625-660` —
  `flushRoTTLBumpsInTxWriteFootprint` — where the code ALREADY knows an
  entry is being flushed from RO TTL bump storage into the thread entry map
- `src/transactions/ParallelApplyUtils.cpp:662-685` —
  `flushRemainingRoTTLBumps` — remaining RO TTL bumps flushed at cluster end

## Evidence

1. **Single consumer**: The readWriteSet is ONLY used in `maybeMergeRoTTLBumps`
   (line 497) for a single `readWriteSet.find(key)` check. The entire
   ~12,800-entry set is built just to answer a yes/no question per dirty TTL
   entry during commit.

2. **Information already available at source**: In
   `flushRoTTLBumpsInTxWriteFootprint` (lines 626-660) and
   `flushRemainingRoTTLBumps` (lines 662-685), the code moves entries from
   `mRoTTLBumps` into `mThreadEntryMap` via `upsertEntry`. At this exact point,
   the code knows the entry originated as an RO TTL bump. A flag or tag could
   be set here.

3. **Clean separation**: The `ThreadParallelApplyEntry` struct already has a
   dirty flag (`mIsDirty`) to track modification state. Adding an
   `mIsRoTTLBump` flag is a natural extension of this pattern.

4. **The `mRoTTLBumps` map itself** (used within a worker thread during tx
   processing) already maintains per-key RO TTL bump state. The information
   is discarded after flush rather than being carried through to the commit.

## Anti-Evidence

1. The flag approach changes the invariant from "check against the stage's RW
   footprint" to "check the entry's provenance flag." These must be
   semantically equivalent. Edge cases to verify: an entry that starts as an
   RO TTL bump but is later overwritten by a RW modification in a subsequent
   tx in the same cluster. In this case, `flushRoTTLBumpsInTxWriteFootprint`
   moves the RO bump into the thread map, and the later RW modification calls
   `upsertEntry` which overwrites it — the flag must be cleared on RW
   overwrite.

2. If H002 (stage-level TTL key cache) is implemented, `getReadWriteKeysForStage`
   becomes ~3ms (no SHA-256, just cache lookups + set construction), reducing
   this hypothesis's value from ~9ms to ~6ms savings. Still LOW severity
   independently.

3. This is a targeted optimization for a single serial function. The
   implementation is straightforward but the testing must verify semantic
   equivalence for all entry provenance paths (RO bump, RW modify, RW delete,
   RO bump then RW overwrite).
