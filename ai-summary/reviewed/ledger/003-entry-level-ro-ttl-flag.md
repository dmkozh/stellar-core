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

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete flow from `getReadWriteKeysForStage` (lines 99-118) through `commitChangesFromThreads` (546-559) → `commitChangeFromThread` (509-529) → `maybeMergeRoTTLBumps` (479-507). Confirmed that `readWriteSet` has exactly one consumer: the `readWriteSet.find(key) == readWriteSet.end()` check at line 497. Verified that `getTTLKey` (LedgerTypeUtils.cpp:31-38) performs `sha256(xdr::xdr_to_opaque(e))` for each RW Soroban key — confirmed SHA-256 + XDR serialization cost. Traced the provenance information through `flushRoTTLBumpsInTxWriteFootprint` (625-660) and `flushRemainingRoTTLBumps` (662-685) and verified the flag approach preserves correctness via clustering guarantees.

### Code Paths Examined

- `src/transactions/ParallelApplyUtils.cpp:99-118` — `getReadWriteKeysForStage` iterates all txs, calls `getTTLKey(lk)` which does SHA-256 per Soroban RW key, builds unordered_set. Confirmed single call site at line 555.
- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey(LedgerKey)` confirmed: `sha256(xdr::xdr_to_opaque(e))` — XDR serialization + SHA-256 hash per call.
- `src/transactions/ParallelApplyUtils.cpp:479-507` — `maybeMergeRoTTLBumps` uses `readWriteSet.find(key)` at line 497 — confirmed only consumer of the readWriteSet.
- `src/transactions/ParallelApplyUtils.cpp:625-660` — `flushRoTTLBumpsInTxWriteFootprint` moves entries from `mRoTTLBumps` to `mThreadEntryMap` via `upsertEntry` — confirmed the code knows provenance at write time.
- `src/transactions/ParallelApplyUtils.cpp:662-685` — `flushRemainingRoTTLBumps` flushes remaining RO TTL bumps at cluster end — same provenance knowledge.
- `src/transactions/ParallelApplyUtils.cpp:738-750` — `ThreadParallelApplyLedgerState::upsertEntry` always creates `ThreadParallelApplyEntry::dirty(entry)` — RW overwrites naturally replace the entry without the RO flag.
- `src/transactions/TransactionFrameBase.h:56-79` — `ParallelApplyEntry<S>` struct has `mLedgerEntry` and `mIsDirty`. `rescope()` at line 74-78 preserves `mIsDirty` — adding `mIsRoTTLBump` would be preserved identically.

### Findings

**The inefficiency exists and is confirmed.** `getReadWriteKeysForStage` performs ~6,400 SHA-256 computations (via `getTTLKey` → `sha256(xdr::xdr_to_opaque(e))`) plus ~12,800 hash-set insertions on the serial commit path. The `readWriteSet` has exactly one consumer at line 497.

**The flag approach is semantically equivalent to the readWriteSet approach.** Key insight: if a TTL key is in any tx's RW footprint, ALL txs touching that key (whether RO bump or RW) are clustered together by `ParallelTxSetBuilder`. This means:
- If `newEntry.mIsRoTTLBump == true`, no tx in any cluster has this key in its RW footprint → the key is NOT in the stage's readWriteSet → merge with max is correct.
- If `newEntry.mIsRoTTLBump == false`, this entry was modified by RW → the key IS in the stage's readWriteSet → no merge (overwrite).
- The "RO bump then RW overwrite" edge case (anti-evidence #1) is safe: `flushRoTTLBumpsInTxWriteFootprint` writes the bump with `mIsRoTTLBump=true`, then the RW tx's `commitChangeFromSuccessfulTx` calls `upsertEntry` which overwrites with `mIsRoTTLBump=false`. But this key can't appear in another thread (clustering forces all touches into one cluster), so the merge path is never triggered for it.

**Only the NEW entry's flag needs checking.** The old entry in the global map may come from a previous stage's RW write (not flagged as RO). This is correct because the readWriteSet is also stage-specific — it only reflects the current stage's RW footprint.

**Estimated savings:** ~5-9ms per stage on the serial commit path (SHA-256 cost dominates). At T=8 with SAC benchmark (3,200 txs), this is a meaningful fraction of serial overhead.

### PoC Guidance

- **Target code**:
  - `src/transactions/TransactionFrameBase.h:56-79` — Add `bool mIsRoTTLBump = false;` to `ParallelApplyEntry<S>`. Update `clean()`, `dirty()` factory methods to default it to `false`. Add a `dirtyRoTTLBump()` factory method that sets both flags. Update `rescope()` to preserve the flag.
  - `src/transactions/ParallelApplyUtils.cpp:625-660` — In `flushRoTTLBumpsInTxWriteFootprint`, after `upsertEntry`, mark the entry as RO TTL bump by using a new `upsertRoTTLBumpEntry` helper or setting the flag after insert.
  - `src/transactions/ParallelApplyUtils.cpp:662-685` — Same in `flushRemainingRoTTLBumps`.
  - `src/transactions/ParallelApplyUtils.cpp:479-507` — In `maybeMergeRoTTLBumps`, replace `readWriteSet.find(key) == readWriteSet.end()` with `newEntry.mIsRoTTLBump` (checking only the new entry).
  - `src/transactions/ParallelApplyUtils.cpp:509-542` — Remove `readWriteSet` parameter from `commitChangeFromThread`.
  - `src/transactions/ParallelApplyUtils.cpp:531-543` — Remove `readWriteSet` parameter from `commitChangesFromThread`.
  - `src/transactions/ParallelApplyUtils.cpp:546-559` — Remove `getReadWriteKeysForStage` call from `commitChangesFromThreads`.
  - `src/transactions/ParallelApplyUtils.cpp:99-118` — Delete `getReadWriteKeysForStage` function entirely.
- **Correctness check**: Existing parallel apply tests (tag `[soroban]`, specifically `InvokeHostFunctionTests` with parallel apply scenarios) cover the RO TTL bump merge logic. Run the full `[soroban]` test tag. Also verify with `[tx]` tag for broader coverage.
- **Benchmark focus**: SAC benchmark at T=8 (3,200 txs). The serial gap between parallel execution completing and `commitChangesToLedgerTxn` should shrink by ~5-9ms. Look for improvement in median ledger close time. Tracy profiling of the `commitChangesFromThreads` zone should show the `getReadWriteKeysForStage` zone eliminated.
