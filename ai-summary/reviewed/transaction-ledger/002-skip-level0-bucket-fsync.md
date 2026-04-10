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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete level-0 merge path from `addBatchInternal` → `prepareFirstLevel` → `mergeInMemory` → `BucketOutputIterator` → `XDROutputFileStream::close()` → `fs::flushFileChanges()` → `fsync()`. Confirmed the fsync is real, on every ledger close, and inside the benchmark-measured path (when `APPLY_LOAD_TIME_WRITES=true`). The benchmark configs do NOT set `DISABLE_XDR_FSYNC`, so fsync is active during benchmarks. However, the hypothesis only addresses the bucket file fsync — `adoptFileAsBucket` also calls `durableRename` (directory fsync) via `renameBucketDirFile`, and the index file may also fsync — neither of which is addressed by this change.

### Code Paths Examined

- `src/bucket/BucketListBase.cpp:addBatchInternal:788` — `doFsync = !app.getConfig().DISABLE_XDR_FSYNC` (confirmed: always true unless globally disabled)
- `src/bucket/BucketListBase.cpp:prepareFirstLevel:196-238` — passes `doFsync` to `mergeInMemory` at line 236; the in-memory path (line 215-237) is the common case
- `src/bucket/LiveBucket.cpp:mergeInMemory:549-613` — creates `LiveBucketOutputIterator(... doFsync)` at line 599-601; merged entries kept in-memory via `mEntries` at line 610-612
- `src/bucket/BucketOutputIterator.cpp:getBucket:169-250` — calls `mOut.close()` at line 181 (triggers fsync); then creates index and calls `adoptFileAsBucket`
- `src/util/XDRStream.h:close:308-327` — `if (mFsyncOnClose) { fs::flushFileChanges(getHandle()); }` at lines 317-319
- `src/util/Fs.cpp:flushFileChanges:224-236` — calls `fsync(fd)` in a retry loop
- `src/bucket/BucketManager.cpp:renameBucketDirFile:430-443` — when `!DISABLE_XDR_FSYNC`, calls `fs::durableRename` which does `rename()` + `fsync(dir_fd)` — this is a SECOND fsync not addressed by the hypothesis
- `src/bucket/DiskIndex.cpp:350` — index save also uses `OutputFileStream(ctx, !bm.getConfig().DISABLE_XDR_FSYNC)` — potential THIRD fsync if DiskIndex is used
- `docs/apply-load-benchmark-sac.cfg` — confirmed: `DISABLE_XDR_FSYNC` is NOT set, so fsync is active in benchmarks
- `src/simulation/ApplyLoad.cpp:1958-1962` — when `APPLY_LOAD_TIME_WRITES=true`, timing uses `{"ledger", "ledger", "close"}` which includes bucket merge and fsync

### Findings

1. **The inefficiency is real**: `fsync()` is called on the level-0 bucket file on every single ledger close. This is the highest-frequency fsync in the bucket system.

2. **The fix is correct and safe**: Passing `doFsync=false` to `mergeInMemory` for level-0 is safe because:
   - Merged entries remain authoritative in memory (`mEntries` field)
   - Level-1 background merges read the file from the OS page cache (not affected by lack of fsync)
   - Level-0 is rebuilt every ledger, so the crash recovery window is ~5 seconds
   - On crash recovery, bucket hash verification detects and handles corruption

3. **The fix is incomplete**: The hypothesis only eliminates one of up to three fsyncs in the level-0 merge path:
   - ✅ Bucket file fsync (`XDROutputFileStream::close`) — addressed
   - ❌ Directory fsync in `durableRename` (`adoptFileAsBucket` → `renameBucketDirFile`) — NOT addressed; this is controlled by `DISABLE_XDR_FSYNC` globally, not by the `doFsync` parameter
   - ❌ Index file fsync (if `DiskIndex` is created) — NOT addressed

4. **Estimated impact is below the Low threshold (5-10%)**:
   - File fsync for ~3.2MB on typical SSD: ~0.5-2ms
   - Total close time in benchmark: ~50ms
   - Expected improvement: 1-4% of total close time
   - The remaining directory fsync (~0.1-0.5ms) and potential index fsync are still present

5. **Severity downgraded from Medium to Informational**: The hypothesis claims 2-10% (Medium), but realistic improvement from the file fsync alone is 1-4%, which is below the 5% threshold for Low severity. A complete approach that also eliminates the directory and index fsyncs could potentially reach the Low threshold.

### PoC Guidance

- **Target code**: `src/bucket/BucketListBase.cpp:prepareFirstLevel` — change the `doFsync` argument passed to `mergeInMemory` at line 236 from the parameter value to `false` (or a hardcoded `false` specifically for the in-memory merge path)
- **Change description**: In `prepareFirstLevel` for `LiveBucket`, pass `doFsync=false` to `mergeInMemory()` instead of the config-derived value. The in-memory entries are authoritative; the disk file is best-effort for crash recovery. For a more complete optimization, also consider skipping `durableRename` in `adoptFileAsBucket` for level-0 buckets (requires plumbing a flag through `BucketOutputIterator::getBucket` → `adoptFileAsBucket` → `renameBucketDirFile`)
- **Correctness check**: Existing bucket tests cover level-0 merges: `[bucket]` tag tests, particularly those testing `addBatch` and merge behavior. The test suite already sets `DISABLE_XDR_FSYNC=true` (see `src/test/test.cpp:579`), so correctness is already validated without fsync. The concern is crash recovery behavior which is not unit-testable.
- **Benchmark focus**: Measure `{"ledger", "ledger", "close"}` timer median and p99 with `APPLY_LOAD_TIME_WRITES=true`. Expected improvement: 1-4% reduction in median close time for SAC 3200-tx T=8 scenario. If the improvement is below noise, try also eliminating the `durableRename` fsync for level-0 buckets to increase the signal.
