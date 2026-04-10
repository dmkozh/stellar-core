# H002: Skip Fsync for Level-0 In-Memory Bucket Merge Output

**Date**: 2025-07-22
**Subsystem**: transaction-ledger (LiveBucket, BucketOutputIterator)
**Severity**: Medium
**Impact**: Reduced I/O latency in ledger close critical path
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When the level-0 in-memory bucket merge completes, the merged entries are both
kept in memory (for the next merge) and written to disk (for crash recovery).
The disk write should complete as quickly as possible since it is on the
ledger-close critical path. For level 0 specifically — which is rebuilt every
single ledger close (~5 seconds) — the fsync is disproportionately expensive
relative to its recovery value.

## Mechanism

`LiveBucket::mergeInMemory` (line 599-612) creates a `LiveBucketOutputIterator`
with `doFsync = !app.getConfig().DISABLE_XDR_FSYNC` (default: true). This
iterator serializes every merged entry to disk via `writeOne()` and calls
`fs::flushFileChanges()` (fsync) on close. For a typical 3200-tx Soroban
ledger with ~16,000 surviving entries after merge (~3.2MB of XDR data), the
fsync blocks for ~1-5ms depending on the storage device.

Level 0 is unique: it is the ONLY level that merges every ledger, and its
result is always kept in memory via the `mEntries` field. The on-disk file
serves only as a crash-recovery fallback. Since level 0 is rebuilt on the very
next ledger close (5 seconds later), the crash recovery window is minimal.
On recovery, stellar-core detects hash mismatches and replays from the last
consistent state.

The fix: pass `doFsync=false` specifically for level-0 in-memory merges in
`prepareFirstLevel`. The in-memory entries are authoritative; the disk file
is best-effort. Higher-level bucket merges continue to fsync normally.

## Trigger

Every ledger close that processes Soroban transactions triggers a level-0
in-memory merge:

1. `addLiveBatch` → `addBatchInternal` → `prepareFirstLevel`
2. `prepareFirstLevel` calls `mergeInMemory` with `doFsync=true`
3. `mergeInMemory` writes ~3.2MB to disk and fsyncs

In the apply-load benchmark with 3200 SAC txs at T=8, this fsync is on the
critical path of every ledger close.

## Target Code

- `src/bucket/LiveBucket.cpp:mergeInMemory:550-613` — creates BucketOutputIterator with doFsync
- `src/bucket/BucketListBase.cpp:addBatchInternal:693-810` — passes `doFsync` from config
- `src/bucket/BucketListBase.cpp:prepareFirstLevel:196-238` — level 0 specific merge path
- `src/bucket/BucketOutputIterator.cpp:30-32` — BucketOutputIterator stores doFsync
- `src/util/XDRStream.h:317-320` — fsync on close: `if (mFsyncOnClose) { fs::flushFileChanges(...); }`

## Evidence

1. Level 0 merges every ledger (confirmed by `levelShouldSpill` logic in `addBatchInternal`). The level-0 bucket file has the shortest lifespan of any bucket.
2. The merged result is kept in memory (`mEntries` field, line 612). Subsequent merges use the in-memory entries directly (`hasInMemoryEntries()` check at line 215). The disk file is not read during normal operation.
3. `DISABLE_XDR_FSYNC` exists as a config option, suggesting fsync skipping is an accepted pattern. But it's all-or-nothing; there's no per-level control.
4. The fsync cost (~1-5ms) is a fixed latency on every ledger close. For a T=8 benchmark close of ~50ms, this is 2-10% — directly measurable.
5. On crash recovery, stellar-core already handles missing/corrupt buckets by rebuilding from the last known good state (bucket hash verification on startup).

## Anti-Evidence

1. Skipping fsync introduces a small crash-recovery risk: if the machine loses power between the write and the next ledger close, the bucket file may be corrupt. Recovery would require replaying one additional ledger.
2. When level 0 spills (every 2 ledgers), its `curr` becomes `snap` and feeds into level-1 merge. If the file wasn't fsynced and the machine crashes mid-merge, level-1 could read corrupt data. However, level-1 merges happen asynchronously and the file would typically be flushed to disk by the OS within seconds.
3. The `doFsync` flag is read from `DISABLE_XDR_FSYNC` in `addBatchInternal` (line 788), which is a global config. Adding per-level fsync control requires API changes through `prepareFirstLevel` and `mergeInMemory`.
