# H004: Persisting every disk index eagerly adds cold write amplification that fights apply-path I/O

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: Medium
**Impact**: background merge write amplification and p99 close time
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Merge outputs should only pay the cost of durable on-disk index serialization when that persisted index is actually needed before the process exits or restarts. During apply-load, where the process keeps running and merge workers immediately hand the in-memory index to the adopted bucket, background merges should not also serialize, fsync, and durable-rename a second index artifact for every large merged bucket.

## Mechanism

Once a bucket crosses the disk-index cutoff, `BucketOutputIterator::getBucket` calls `createIndex`, and `DiskIndex` immediately persists itself via `saveToDisk`, which writes a temporary index file, fsyncs it on close, and durable-renames it into the bucket directory. That extra index file is cold redundancy during the benchmark: the newly adopted bucket already carries the live index in memory, but the worker still performs a second durable write path that competes with ledger-close reads and bucket-file writes.

## Trigger

Run a long enough apply-load benchmark for live-bucket merges to produce buckets above the 20 MB disk-index cutoff, especially at `T=8` where worker-thread merge I/O can overlap with apply-thread bucket reads. This should show up most clearly in higher-percentile ledgers once upper levels start spilling.

## Target Code

- `src/main/Config.cpp:176-179,201` — disk indexes are enabled by default and XDR fsync is enabled by default
- `src/bucket/LiveBucketIndex.cpp:28-39,51-69` — buckets at or above the cutoff switch to `DiskIndex`
- `src/bucket/BucketOutputIterator.cpp:220-235` — merge output eagerly creates an index before adoption
- `src/bucket/DiskIndex.cpp:325-372` — `saveToDisk` writes, fsyncs, and durable-renames a separate index file
- `src/bucket/FutureBucket.cpp:406-459` — this work runs inside background merge tasks while apply continues

## Evidence

The persisted index is not needed for the current process to use the merged bucket: `adoptFileAsBucket` stores the already-built index inside the returned `Bucket` object. Yet large merge outputs still perform a second file-creation path for the serialized index, including another durable rename in the same directory, so each big merge writes both the bucket payload and a separate durable index payload before the benchmark ever benefits from that persisted copy.

## Anti-Evidence

This optimization only matters once merged buckets are large enough to use `DiskIndex`; small buckets stay on `InMemoryIndex` and never call `saveToDisk`. Persisted indexes materially improve restart behavior, so the likely fix is lazy or deferred persistence rather than removing on-disk indexes entirely.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete path from `FutureBucket::startMerge()` through background worker thread → `BucketT::merge()` → `BucketOutputIterator::getBucket()` → `createIndex<BucketT>()` → `LiveBucketIndex` constructor → `DiskIndex<LiveBucket>` constructor → `saveToDisk()`. Confirmed that when `BUCKETLIST_DB_PERSIST_INDEX` is true (the default) and a bucket exceeds the 20MB `BUCKETLIST_DB_INDEX_CUTOFF`, the DiskIndex constructor eagerly serializes the entire index to a temp file, fsyncs it, and durable-renames it into the bucket directory — all on the background merge worker thread. The in-memory index is immediately available and stored in the adopted bucket via `adoptFileAsBucket`, so the persisted copy is pure restart-recovery redundancy during runtime.

### Code Paths Examined

- `src/bucket/FutureBucket.cpp:406-459` — Background merge task construction; `doFsync` derived from `!DISABLE_XDR_FSYNC` (default: true)
- `src/bucket/BucketOutputIterator.cpp:168-250` — `getBucket()` closes the bucket file (with fsync), computes hash, checks for existing index, falls through to `createIndex()` at line 233 when no existing index found
- `src/bucket/BucketIndexUtils.cpp:30-51` — `createIndex()` constructs a new `BucketT::IndexT` (LiveBucketIndex for LiveBucket)
- `src/bucket/LiveBucketIndex.cpp:41-69` — Constructor checks `getPageSize()`; if bucket >= 20MB cutoff, creates `DiskIndex<LiveBucket>`
- `src/bucket/DiskIndex.cpp:185-299` — Constructor builds range index, bloom filter, type ranges, then at line 296-298 calls `saveToDisk()` if `BUCKETLIST_DB_PERSIST_INDEX` is true
- `src/bucket/DiskIndex.cpp:325-372` — `saveToDisk()`: writes temp file via `OutputFileStream` (with fsync), then `renameBucketDirFile()` for durable rename
- `src/main/Config.cpp:177-179,201` — `BUCKETLIST_DB_INDEX_CUTOFF = 20` (MB), `BUCKETLIST_DB_PERSIST_INDEX = true`, `DISABLE_XDR_FSYNC = false`

### Findings

The inefficiency is real: every DiskIndex-sized merge output pays for a second durable file write (temp file + fsync + rename) for the persisted index, even though the in-memory index is immediately usable. However, the practical impact is significantly lower than claimed:

1. **Background thread, not apply path**: The saveToDisk call runs on background worker threads inside the merge task, not on the main ledger-close thread. The only way it affects benchmark timings is through I/O contention (competing for disk bandwidth with main-thread bucket reads).

2. **Bucket file fsync dominates**: The bucket file itself is also written with fsync enabled (via `OutputFileStream` with `doFsync=true` in `BucketOutputIterator`). The bucket file is much larger than its index file (tens to hundreds of MB vs a few MB), so the incremental I/O from index persistence is a small fraction of total merge I/O.

3. **Config flag already exists**: `BUCKETLIST_DB_PERSIST_INDEX` can be set to `false` to disable this entirely. The benchmark configs (`docs/apply-load-benchmark-sac.cfg`) do not set this flag, so the trivial "fix" for benchmarks is a one-line config change.

4. **Standard benchmark may not trigger it**: With `APPLY_LOAD_BL_SIMULATED_LEDGERS = 0` and only 100 benchmark ledgers, BucketList sizes may barely cross the 20MB threshold depending on transaction volume and entry deduplication.

Severity downgraded from Medium to Informational because the improvement is theoretical — the background-thread index fsync adds ~1-10ms per large merge, which is small relative to merge cost and doesn't directly block the apply path.

### PoC Guidance

- **Target code**: No code changes required for initial validation. Test with config change `BUCKETLIST_DB_PERSIST_INDEX = false` in benchmark configs.
- **Change description**: Add `BUCKETLIST_DB_PERSIST_INDEX = false` to `docs/apply-load-benchmark-sac.cfg` and equivalent configs. For a code-level optimization, modify `DiskIndex` constructor (DiskIndex.cpp:296-298) to skip `saveToDisk()` during merge and instead persist lazily (e.g., on `BucketManager::forgetUnreferencedBuckets()` or on shutdown).
- **Correctness check**: Existing bucket tests (`[bucket]` tag) cover index creation and merge paths. Disabling persistence should not affect any test that doesn't explicitly check for index files on disk.
- **Benchmark focus**: Compare p99 ledger close time with `BUCKETLIST_DB_PERSIST_INDEX = true` vs `false` using the `LIMIT_BASED` mode (which pre-generates a BucketList), not the `benchmark` mode (which starts with empty BucketList). Use at least `APPLY_LOAD_BL_SIMULATED_LEDGERS = 1000` to ensure large enough buckets. Expected improvement: likely unmeasurable (<1%) for median, possibly small p99 reduction if I/O contention from background fsyncs is significant on the test hardware.
