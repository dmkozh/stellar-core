# H014: Skip BucketList Metric Reporting in `addLiveBatch` When Metrics Disabled

**Date**: 2026-04-10
**Subsystem**: ledger (BucketManager)
**Severity**: Informational
**Impact**: Eliminate per-ledger BucketList metric aggregation
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `DISABLE_SOROBAN_METRICS_FOR_TESTING` is enabled (as in the benchmark),
`addLiveBatch` should skip the `reportBucketEntryCountMetrics()` and
`reportLiveBucketIndexCacheMetrics()` calls at BucketManager.cpp:1044-1045,
since these metric-reporting functions iterate all 22 BucketList levels.

## Mechanism

After `addBatch` in `addLiveBatch` (BucketManager.cpp:1026-1046), two
metric-reporting functions are called unconditionally:

1. `reportBucketEntryCountMetrics()` (lines 1840-1872): calls
   `sumBucketEntryCounters()` which iterates all 11 levels × 2 buckets,
   summing `BucketEntryCounters` maps. Then updates medida counters.

2. `reportLiveBucketIndexCacheMetrics()` (lines 353-396): iterates all
   11 levels × 2 buckets, calling `getIndexCacheSize()` and
   `getBucketEntryCounters()`, computing cache size estimates.

These run per ledger close even when metrics are disabled for benchmarking.

## Trigger

Profile `addLiveBatch` and measure time in the two reporting functions.

## Target Code

- `src/bucket/BucketManager.cpp:addLiveBatch:1044-1045` — unconditional metric reporting
- `src/bucket/BucketManager.cpp:reportBucketEntryCountMetrics:1840-1872` — iterates all levels
- `src/bucket/BucketManager.cpp:reportLiveBucketIndexCacheMetrics:353-396` — iterates all levels

## Evidence

- Both functions iterate all 22 non-empty buckets
- `DISABLE_SOROBAN_METRICS_FOR_TESTING` does not gate these calls
- Called every ledger close

## Anti-Evidence

- `getBucketEntryCounters()` returns a pre-computed cached struct per bucket — the iteration cost is ~22 map operations
- `getIndexCacheSize()` is a trivial accessor
- Total cost: ~22 × ~5-10 entry types per counter map = ~220 operations at ~10ns each = ~2.2μs
- This is negligible compared to total close time

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The bucket entry counters and index cache sizes are pre-computed cached
values — `getBucketEntryCounters()` returns a reference to a stored struct,
not a live computation. The aggregation across 22 buckets involves summing
~10 entry-type counters per bucket = ~220 integer additions + hash map
lookups, totaling ~2-5μs. This is 0.0001-0.005% of close time. The cost
is entirely negligible.

### Lesson Learned

BucketList metric reporting (entry counts, index cache sizes) uses
pre-computed per-bucket counters, making the per-ledger aggregation cost
trivial (~2-5μs for 22 buckets). Only metric COLLECTION (timers,
histograms with per-tx resolution) is expensive enough to warrant
conditional gating.
