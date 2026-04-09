# H003: Empty hot-archive ledgers still create meta-only buckets and merge work

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: Medium
**Impact**: per-ledger I/O, hashing, and merge churn
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If a ledger archives no entries and restores no hot-archive entries, the hot-archive bucket list should stay unchanged and skip bucket construction entirely. A no-op archival delta should not create files, indexes, or background merges.

## Mechanism

`finalizeLedgerTxnChanges` calls `addHotArchiveBatch` even when both input vectors are empty. `HotArchiveBucketList::addBatch` unconditionally reaches `BucketLevel<HotArchiveBucket>::prepareFirstLevel`, which calls `HotArchiveBucket::fresh`; the `BucketOutputIterator` constructor always emits a `HOT_ARCHIVE_METAENTRY`, so an empty archival delta becomes a non-empty meta-only bucket file. That file then goes through adopt/index/merge machinery despite carrying no archived or restored ledger entries.

## Trigger

Run apply-load with the benchmark Soroban upgrade config, which sets `minPersistentTTL` / `minTemporaryTTL` to ~1e9 and is explicitly trying to avoid archival. On those ledgers, `archivedEntries` and `restoredHotArchiveKeys` stay empty, but hot-archive bucket maintenance still runs every close.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:2998-3001` — unconditional `addHotArchiveBatch` call
- `src/bucket/HotArchiveBucketList.cpp:addBatch:11-23` — no empty-input fast path
- `src/bucket/BucketListBase.cpp:BucketLevel<HotArchiveBucket>::prepareFirstLevel:243-253` — always constructs a fresh bucket
- `src/bucket/HotArchiveBucket.cpp:fresh:16-43` — creates bucket output even for empty vectors
- `src/bucket/BucketOutputIterator.cpp:25-73,181-193` — writes metadata entry and treats meta-only output as a real bucket

## Evidence

Unlike live level-0 batches, hot archive has no in-memory empty fast path. `HotArchiveBucket::convertToBucketEntry` can return an empty vector, but `BucketOutputIterator` still buffers and writes the metadata entry, so `getBucket()` does not hit the `mObjectsPut == 0` empty-bucket branch.

## Anti-Evidence

If archival or restoration is actually happening, the batch is necessary. Hash-based bucket deduplication may also cap some disk-retention fallout, so the measurable win depends on how much time is spent building and indexing these meta-only buckets before dedup kicks in.
