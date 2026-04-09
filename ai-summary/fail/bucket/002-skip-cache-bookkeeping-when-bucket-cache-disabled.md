# H002: Live-bucket cache-disabled runs still pay two full bookkeeping passes per ledger

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: Low
**Impact**: per-ledger CPU overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When `BUCKETLIST_DB_MEMORY_FOR_CACHING` is zero, ledger close should skip cache initialization and cache-size reporting work entirely. With caching disabled, `addLiveBatch` should only update the bucket list itself, not re-sum account sizes or walk every bucket to report cache occupancy that must remain zero.

## Mechanism

`BucketManager::addLiveBatch` always calls both `LiveBucketList::maybeInitializeCaches` and `BucketManager::reportLiveBucketIndexCacheMetrics`. Even though `LiveBucketIndex::maybeInitializeCache` eventually returns immediately when the cache budget is zero, `maybeInitializeCaches` first computes `sumBucketEntryCounters()` across all levels, and `reportLiveBucketIndexCacheMetrics` does another full bucket-list walk every ledger. Apply-load uses the default config where caching is disabled, so this is pure bookkeeping overhead on the hot close path.

## Trigger

Run apply-load with default config (`BUCKETLIST_DB_MEMORY_FOR_CACHING = 0`) on any nontrivial live bucket list. Every committed ledger pays these two traversals after `addBatch`.

## Target Code

- `src/bucket/BucketManager.cpp:addLiveBatch:1026-1046` — unconditional cache init + reporting hooks
- `src/bucket/LiveBucketList.cpp:sumBucketEntryCounters/maybeInitializeCaches:29-68` — full bucket-list traversal before the zero-budget early return inside individual indexes
- `src/bucket/BucketManager.cpp:reportLiveBucketIndexCacheMetrics:353-390` — second unconditional traversal
- `src/main/Config.cpp:177-179` — defaults cache budget to zero

## Evidence

The fast-path guard for disabled caching lives inside `LiveBucketIndex::maybeInitializeCache`, after `LiveBucketList::maybeInitializeCaches` has already aggregated total account bytes across every curr/snap bucket. `reportLiveBucketIndexCacheMetrics` then repeats another full scan even though the expected result is zero cache entries and zero estimated bytes.

## Anti-Evidence

When operators explicitly enable bucket caching, both passes may be justified. The improvement here is limited by the small fixed number of levels, so it is likely smaller than optimizations that remove disk reads or background merges.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the complete call chain from `BucketManager::addLiveBatch()` through `LiveBucketList::addBatch()` → `maybeInitializeCaches()` → `sumBucketEntryCounters()`, and separately through `reportBucketEntryCountMetrics()` (which also calls `sumBucketEntryCounters()`) and `reportLiveBucketIndexCacheMetrics()`. Confirmed that when `BUCKETLIST_DB_MEMORY_FOR_CACHING = 0`, three traversals of the BucketList occur per ledger close, but each is extremely cheap — purely in-memory operations on cached data.

### Code Paths Examined

- `src/bucket/BucketManager.cpp:1026-1046` — `addLiveBatch()` calls `addBatch()`, `reportBucketEntryCountMetrics()`, and `reportLiveBucketIndexCacheMetrics()`
- `src/bucket/LiveBucketList.cpp:29-68` — `sumBucketEntryCounters()` loops 22 buckets (11 levels × curr+snap), calling `getBucketEntryCounters()` which returns `const&` to cached `mData.counters` in the index — no disk I/O
- `src/bucket/LiveBucketList.cpp:48-67` — `maybeInitializeCaches()` calls `sumBucketEntryCounters()` then loops 22 buckets again
- `src/bucket/LiveBucketIndex.cpp:95-124` — `maybeInitializeCache()`: checks `mInMemoryIndex` (return), acquires shared lock to check `mCache` (return), then checks `maxBucketListBytesToCache == 0` (return). Three cheap early-exit paths.
- `src/bucket/BucketManager.cpp:353-396` — `reportLiveBucketIndexCacheMetrics()` loops 22 buckets calling `getIndexCacheSize()`, which calls `shouldUseCache()` → shared lock + null check → returns 0
- `src/bucket/BucketManager.cpp:1840-1872` — `reportBucketEntryCountMetrics()` calls `sumBucketEntryCounters()` a second time (redundant with the one in `maybeInitializeCaches`)
- `src/bucket/DiskIndex.h:155-159` — `getBucketEntryCounters()` returns `mData.counters` by const reference — a trivial accessor
- `src/bucket/BucketUtils.cpp:377-387` — `BucketEntryCounters` constructor allocates two `std::map` with 11 entries each; `operator+=` iterates 11 entries per map

### Why It Failed

The inefficiency exists but is **not in a hot path in any meaningful sense**. Each "traversal" iterates at most 22 non-empty buckets. Per bucket, the operations are:

1. `getBucketEntryCounters()`: returns a `const&` from `DiskIndex::mData.counters` — a single pointer dereference, zero computation.
2. `maybeInitializeCache()`: at most 2 branch checks + 1 shared lock acquire/release before early exit at `maxBucketListBytesToCache == 0`.
3. `getIndexCacheSize()`: 1 shared lock + null check → returns 0.

The `BucketEntryCounters` default constructor allocates two `std::map` with 11 entries (node allocations), and `operator+=` iterates 22 map entries per call. `sumBucketEntryCounters()` calls this ~22 times, so roughly 484 map iterations + 1 map construction per call. This is called twice (once in `maybeInitializeCaches`, once in `reportBucketEntryCountMetrics`).

Total estimated cost: ~2-5 microseconds per ledger close. A single ledger close in apply-load takes milliseconds to tens of milliseconds (transaction application, bucket creation, disk writes, snapshot updates). The ratio is roughly 0.01-0.05% — well below any measurable benchmark threshold, let alone the 5% minimum for "Low" severity.

While the redundant `sumBucketEntryCounters()` call could theoretically be deduplicated, and the cache-disabled path could short-circuit earlier, the absolute savings (~1-2μs) would never appear in any benchmark.

### Lesson Learned

When evaluating bucket traversal overhead, note that the BucketList has a fixed, small number of levels (11 levels × 2 = 22 buckets max). Operations that are O(levels) rather than O(entries) are effectively constant-time and negligible compared to per-entry or I/O-bound work in the ledger close path. The `std::map`-based `BucketEntryCounters` aggregation is the most expensive part of these traversals, but with only 11 enum values per map and 22 buckets, the total work is still trivially small.
