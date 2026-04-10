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
