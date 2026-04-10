# H002: Bulk bucket loads keep re-running `lower_bound` instead of merge-walking the sorted range index

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: prefetch CPU and apply-path staging latency
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Bulk `loadLiveKeys()` should exploit the fact that both the unresolved key batch
and `DiskIndex::keysToOffset` are already sorted. For a prefetch batch of
thousands of keys, the bucket code should advance through the range index with a
single forward-only merge walk, not perform a fresh binary search for every key.

## Mechanism

`SearchableBucketListSnapshot::loadKeysFromBucket()` maintains a monotonic
`currKeyIt` and `indexIter`, but still calls `index.scan(indexIter, key)` for
every key. `DiskIndex::scan()` then runs `std::lower_bound(start, end, key, ...)`
again. On large prefetch batches this turns a naturally linear two-pointer walk
into repeated O(log P) range-index searches per key, per bucket. Apply-load
invokes prefetch twice per ledger before execution, so source-account and
classic-footprint batches repeatedly pay this comparator-heavy serial prefix.

## Trigger

Run any apply-load benchmark with default prefetch enabled and bucket-backed
state. The strongest case is large ACCOUNT batches from `prefetchTxSourceIds`
plus any additional classic TRUSTLINE keys in `prefetchTransactionData`, where
thousands of sorted keys are probed against the same disk buckets every ledger.

## Target Code

- `src/bucket/BucketListSnapshot.cpp:210-229` — bulk loader iterates sorted keys but calls `index.scan(...)` for each one
- `src/bucket/DiskIndex.cpp:61-85` — `scan()` performs `std::lower_bound` from the current iterator on every key
- `src/ledger/LedgerTxn.cpp:3045-3097` — `prefetch()` materializes `keysToSearch` and always routes them through `loadLiveKeys(...)`
- `src/ledger/LedgerManagerImpl.cpp:2341-2377` — prefetch runs twice per ledger before apply

## Evidence

The algorithm is already structured like a merge walk: both `currKeyIt` and
`indexIter` only move forward, and `index.scan()` returns the lower bound to
start from on the next key. But instead of incrementally advancing the current
range until it can answer the next key, the code re-enters `lower_bound` for
every key. That means thousands of repeated `LedgerKey` comparisons against the
same `keysToOffset` vector, even though no search ever needs to move backward.

## Anti-Evidence

If the key batch is sparse, the remaining range index is small, or most lookups
resolve from in-memory buckets, the log-factor savings shrink. The optimization
also needs to preserve the existing bloom-filter and iterator semantics for keys
that do fall inside a candidate range.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (related H006 page-reuse hypothesis is distinct)
**Failed At**: reviewer

### Trace Summary

Traced the complete bulk-load path from `loadKeysFromBucket` through `DiskIndex::scan()`. The `scan()` function takes a `start` iterator (the position returned by the previous call) and calls `std::lower_bound(start, mData.keysToOffset.end(), k, lower_bound_pred)`. This means each successive binary search operates only on the remaining (shrinking) portion of the range index. The hypothesis claims this should be replaced by a linear merge walk, but the algorithmic analysis is inverted: when keys are sparse relative to index pages (K << P), binary search from an advancing position is strictly more efficient than a merge walk.

### Code Paths Examined

- `src/bucket/DiskIndex.cpp:61-86` — `scan()` takes `IterT start` and calls `std::lower_bound(start, end, k, pred)`. The search range shrinks with each call since `start` only moves forward. Total comparisons across K keys ≈ K × log(P/K).
- `src/bucket/DiskIndex.cpp:42-45` — `lower_bound_pred` is a lightweight comparator: `indexEntry.first.upperBound < key`, a single `LedgerKey` comparison.
- `src/bucket/BucketListSnapshot.cpp:210-277` — `loadKeysFromBucket` maintains `indexIter` across iterations, passing it as `start` to each `scan()` call. The iterator only advances forward.
- `src/bucket/DiskIndex.h:64,79` — `RangeIndex` is `vector<pair<RangeEntry, streamoff>>`. For large buckets (levels 7-10), this can contain 10K-100K+ entries (one per 16KB page).
- `src/main/Config.cpp:310` — `PREFETCH_BATCH_SIZE = 1000` (default). Split across ~10-20 non-empty buckets, this yields ~50-100 keys per bucket.

### Why It Failed

The hypothesis fundamentally mischaracterizes the algorithmic tradeoff. For the typical workload:

- **K** (keys per bucket) ≈ 50–100 (1000 keys spread across ~10–20 non-empty buckets)
- **P** (range index entries per large bucket) ≈ 10K–100K+

**Current binary-search approach**: Total comparisons ≈ K × log(P/K) ≈ 100 × log(1000) ≈ **1,000 comparisons** per bucket.

**Proposed merge walk**: Total comparisons ≈ P + K ≈ **10K–100K comparisons** per bucket (must examine every range entry between the first and last key).

The binary search from an advancing start position is **10–100× more efficient** than a merge walk when keys are sparse relative to pages. A merge walk is only advantageous when K ≈ P (nearly every page contains a key), which never occurs in the prefetch use case. The proposed "optimization" would actually be a performance regression.

### Lesson Learned

When two sorted sequences have very different sizes (K << P), binary search per element of the smaller sequence (O(K log P)) dominates a merge walk (O(P + K)) because P is the dominant term in the merge cost. The code already uses the advancing-start-position optimization that reduces total binary search cost to O(K log(P/K)), which is near-optimal for this access pattern.
