# H012: Three metrics walks of all 22 buckets after every addLiveBatch

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: per-ledger CPU in addLiveBatch, serial path only
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

After ingesting a new batch of entries into the BucketList, the system should
not walk all 22 buckets (11 levels × 2) multiple times just to report metrics.
Metrics should be computed lazily or amortized rather than recomputed from
scratch on every ledger close.

## Mechanism

`BucketManager::addLiveBatch` (BucketManager.cpp:1025-1046) calls three full
bucket-list walks after each batch:

1. `getSize()` — walks all 22 buckets, summing file sizes via
   `BucketBase::getSize()` (which returns cached `mSize`).
2. `reportBucketEntryCountMetrics()` — walks all 22 buckets via
   `sumBucketEntryCounters()`, calling `getIndex().getBucketEntryCounters()`
   on each.
3. `reportLiveBucketIndexCacheMetrics()` — walks all 22 buckets via
   `reportCacheMetrics()`.

Each walk involves ~22 `shared_ptr<BucketT const>` copies (getting current
snapshot), plus the actual metric aggregation. All three are pure reporting
walks with no side effects on the apply path.

## Trigger

Run any apply-load benchmark scenario. The three walks execute on every single
ledger close, regardless of whether any metrics consumers are active.

## Target Code

- `src/bucket/BucketManager.cpp:1025-1046` — `addLiveBatch` calling size, entry
  count, and cache metrics walks
- `src/bucket/BucketManager.cpp:1840-1880` — `reportBucketEntryCountMetrics`
  walk
- `src/bucket/BucketManager.cpp:1885-1920` — `reportLiveBucketIndexCacheMetrics`
  walk
- `src/bucket/BucketListBase.cpp:600-630` — `getSize()` summing across levels

## Evidence

Profiling heuristics: 22 buckets × 3 walks × ~100ns per iteration (shared_ptr
copy + getter) = ~6.6μs. The `sumBucketEntryCounters` walk accesses index
metadata which may be in L3 cache but not L1/L2, adding another ~200ns per
bucket = ~4.4μs. Total estimated: ~10-50μs per ledger.

## Anti-Evidence

At ~10-50μs per ledger close, this is <0.1% of a 10-50ms close. The benchmark
config disables Soroban metrics (`EMIT_SOROBAN_TRANSACTION_METRICS=false`) but
NOT bucket metrics, so these walks do execute. However, the absolute cost is
below the Medium severity threshold (>2% or 200μs in a single function).

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The absolute cost of ~10-50μs for three bucket-list walks is negligible against
a 10-50ms ledger close time. Even at the optimistic end (50μs against 10ms
close), this is only 0.5% — below the Medium severity threshold of 2%. The
walks access cached values (`mSize`, `BucketEntryCounters`) that are likely in
L2 cache, so the actual cost trends toward the lower end of the estimate.

### Lesson Learned

Metrics reporting walks that access cached/precomputed values are cheap even
when they traverse all 22 buckets. The overhead is dominated by the
shared_ptr copies per level, which at ~22×100ns = 2.2μs is negligible. Focus
optimization efforts on per-transaction costs (3000+ invocations per ledger)
rather than per-ledger walks (1 invocation per ledger).
