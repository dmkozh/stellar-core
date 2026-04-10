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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (H009 targeted bloom allocation cost; H015 targeted bulk merge-walk vs binary search; H003-reviewed targeted type-range pruning — all distinct mechanisms)

### Trace Summary

Traced the complete point-lookup path: `SearchableBucketListSnapshot::load()` → `getBucketEntry()` → `LiveBucketIndex::lookup()` → `DiskIndex::scan(begin(), k).first`. Confirmed that `scan()` at DiskIndex.cpp:61-86 performs `std::lower_bound` over the entire `keysToOffset` vector (up to ~100K entries for large buckets) before checking the bloom filter, and that `lookup()` at LiveBucketIndex.cpp:233 discards the iterator. The code comment at DiskIndex.cpp:66-69 explicitly acknowledges the inefficiency. A dedicated `pointLookup()` method that checks the bloom filter first would skip ~17 LedgerKey comparisons per negative bucket on large buckets.

### Code Paths Examined

- `src/bucket/DiskIndex.cpp:59-86` — `scan()` does `lower_bound(start, end, k, pred)` then checks `mData.filter->contains(k)`. Comment at line 66-69 explicitly notes bloom-first would be more efficient.
- `src/bucket/DiskIndex.h:137` — `scan()` is the only lookup method; no point-specific alternative exists.
- `src/bucket/LiveBucketIndex.cpp:223-240` — `lookup()` calls `mDiskIndex->scan(mDiskIndex->begin(), k).first`, discarding the iterator (`.second`).
- `src/bucket/LiveBucketIndex.cpp:242-257` — `scan()` (bulk path) correctly uses the returned iterator for sequential advancement.
- `src/bucket/BucketListSnapshot.cpp:170-201` — `getBucketEntry()` calls `bucket->getIndex().lookup(k)`, routing through point path.
- `src/bucket/BucketListSnapshot.cpp:314-346` — `load()` iterates all buckets via `loopAllBuckets()`, calling `getBucketEntry()` on each until found.
- `src/util/BinaryFuseFilter.cpp:33-40` — `contains()` allocates via `xdr_to_opaque(key)` (~100-200ns) + SipHash + 3 filter probes.
- `src/transactions/ParallelApplyUtils.cpp:699-735` — `getLiveEntryOpt()` falls through to `mLCLSnapshot.loadLiveEntry(key)` for classic keys not in thread-local state.
- `src/transactions/ParallelApplyUtils.cpp:563-608` — `collectClusterFootprintEntriesFromGlobal()` only pre-populates Soroban footprint keys, not source accounts or other classic keys.

### Findings

**The inefficiency is real and the fix is correct, but practical impact on current benchmarks is limited.**

1. **The inefficiency exists and is explicitly documented**: The comment at DiskIndex.cpp:66-69 says: "This may be slightly less efficient than checking the bloom filter first, but the filter's primary purpose is to avoid disk lookups, not to avoid in-memory index search." The optimization would add a `pointLookup()` method that reverses this order for point lookups.

2. **Per-miss savings are real but modest**: For large buckets (levels 5-10), `keysToOffset` has ~10K-100K entries, so `lower_bound` performs ~14-17 `LedgerKey` comparisons per call. Each comparison involves type-discriminant + key-field comparison (~20-50ns). Total binary search cost: ~300-850ns. The bloom filter check (`xdr_to_opaque` + SipHash + 3 probes) costs ~200-350ns. On a negative lookup (bloom rejects), the proposed change saves the binary search entirely.

3. **Bloom filter rejection rate is near-100% for negatives**: `BinaryFuseFilter16` has a theoretical false positive rate of ~1/65536 ≈ 0.0015%. In practice, nearly every negative lookup will have the bloom filter correctly reject it, so the binary search would be skipped in ~99.998% of miss cases.

4. **Point-lookup frequency in apply-load is limited**: In parallel apply, `getLiveEntryOpt()` falls through to `loadLiveEntry()` only for classic keys not in `mThreadEntryMap` and not in `InMemorySorobanState`. Since `collectClusterFootprintEntriesFromGlobal()` pre-populates all Soroban footprint keys, and Soroban keys use `InMemorySorobanState`, the fallback to `loadLiveEntry()` is primarily for classic keys like source accounts not explicitly in the Soroban footprint. This limits the number of point lookups per ledger to a moderate count.

5. **Impact estimate**: Assuming ~500-2000 point lookups per ledger × ~10 bucket misses per lookup × ~500ns saved per miss = ~2.5-10ms per ledger. At ~200ms ledger close time, this is 1-5%. Realistically, with prefetched entries and cache hits reducing the fallback count, the actual savings are likely <1%.

6. **Architectural value**: Beyond raw performance, this creates a proper separation between point and bulk lookup APIs. The bulk path (`scan()`) legitimately needs the iterator for sequential advancement; the point path does not. Separating them makes the code cleaner and more intentional.

### PoC Guidance

- **Target code**: `src/bucket/DiskIndex.h` and `src/bucket/DiskIndex.cpp` — add a `pointLookup(LedgerKey const& k) const -> IndexReturnT` method. `src/bucket/LiveBucketIndex.cpp:223-240` — change `lookup()` to call `mDiskIndex->pointLookup(k)` instead of `mDiskIndex->scan(begin(), k).first`.
- **Change description**: Add `DiskIndex::pointLookup()` that checks `mData.filter->contains(k)` before `std::lower_bound`. If bloom rejects, return `IndexReturnT()` immediately. If bloom accepts, perform the binary search and range check as before. Mark `mBloomLookupMeter` in all cases. Update `LiveBucketIndex::lookup()` to call this new method. Keep `scan()` unchanged for the bulk path.
- **Correctness check**: Run `"[bucket]"` and `"[bucketindex]"` test tags. The change does not alter any observable behavior — the same keys return the same results. The only difference is the internal ordering of bloom check vs binary search for point lookups. Also verify metering: `mBloomLookupMeter` must still be marked on every call, and `markBloomMiss()` is still called by `getEntryAtOffset()` when a bloom false positive causes a fruitless disk read.
- **Benchmark focus**: Measure `bl.bloom.lookup` and `bl.bloom.miss` meter rates (should be unchanged). Primary observable: reduced CPU time in `DiskIndex::scan` Tracy zone during point lookups. Expected wall-clock improvement: <1% on apply-load benchmarks due to limited point-lookup volume. The optimization is primarily a code-quality improvement with a small performance benefit.
