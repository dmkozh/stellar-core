# H003: Point lookups pay a range-index binary search on every negative bucket before the bloom filter rejects them

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Informational
**Impact**: parallel apply scalability for classic point loads
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Point bucket lookups should use a point-specific fast path that consults cheap
negative tests first and only binary-searches the range index when a bucket is a
real candidate. When the caller only wants `IndexReturnT` for a single key, it
should not pay iterator-maintenance work that exists for bulk scans.

## Mechanism

`LiveBucketIndex::lookup()` delegates to `mDiskIndex->scan(begin(), k).first`,
so even single-key lookups execute the generic bulk-search routine.
`DiskIndex::scan()` intentionally performs `lower_bound` before the bloom check
so it can return the next iterator. Point loads never use that iterator, but
still pay the binary search on every bucket miss. In parallel apply, the
fallback path for non-Soroban classic keys (`mLCLSnapshot.loadLiveEntry(key)`)
can miss several newer buckets before finding an older trustline, so those
unnecessary range-index searches accumulate under `T=8`.

## Trigger

Run the SAC apply-load scenario with parallel apply enabled, where read-only
classic trustlines can miss thread-local state and fall through to
`loadLiveEntry()`. The effect is strongest once those trustlines live in older
disk-indexed buckets and each lookup must reject several younger buckets first.

## Target Code

- `src/bucket/LiveBucketIndex.cpp:223-233` — point lookup routes through `scan(begin(), k).first`
- `src/bucket/DiskIndex.cpp:61-85` — `scan()` does `lower_bound` before bloom filtering
- `src/bucket/BucketListSnapshot.cpp:315-345` — `load()` loops buckets until a point lookup succeeds
- `src/bucket/BucketListSnapshot.cpp:282-301` — lookup order is newest-to-oldest, so older hits imply multiple earlier misses
- `src/transactions/ParallelApplyUtils.cpp:723-732` — non-Soroban classic keys fall back to `mLCLSnapshot.loadLiveEntry(key)`

## Evidence

The comment in `DiskIndex::scan()` explicitly acknowledges that checking the
bloom filter first would be more efficient, but keeps the current ordering to
return an iterator to the bulk caller. `LiveBucketIndex::lookup()` then reuses
that same API even though the iterator is immediately discarded, so the point
path inherits bulk-search costs it does not need.

## Anti-Evidence

This only helps disk-indexed point loads; in-memory buckets, cache hits, and the
Soroban-heavy `custom_token` / `soroswap` paths get little or no benefit. A
point-specific API would also need to preserve bloom-miss metering and exact
`NOT_FOUND` behavior.
