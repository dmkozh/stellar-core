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
