# H003: Disk-index lookups ignore per-type bounds and scan unrelated Soroban pages

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: bucket lookup CPU in prefetch and point-load paths
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When a bucket index already knows the contiguous file range for each
`LedgerEntryType`, point and bulk lookups should use that metadata to skip
buckets that do not contain the requested type and to narrow searches to the
relevant type slice. ACCOUNT and TRUSTLINE lookups should not binary-search and
bloom-probe pages belonging entirely to CONFIG_SETTING, CONTRACT_CODE, TTL, or
other unrelated entry types.

## Mechanism

`DiskIndex` builds `typeRanges` during index construction, and live buckets
already expose those ranges via `getRangeForType()`, but the lookup path ignores
them. `SearchableBucketListSnapshot::load()` and `loadKeysFromBucket()` call
generic `lookup()`/`scan()`, which invoke `DiskIndex::scan()` over the full
`keysToOffset` vector and then hash the key for the bloom filter. In Soroban
apply-load runs the bucket files are filled with contract-data and TTL pages,
while the prefetch path is still loading classic ACCOUNT/TRUSTLINE keys, so this
extra per-bucket in-memory search work repeats across many irrelevant pages.

## Trigger

Run the stock apply-load benchmark after the Soroban working set has populated
mid/older live buckets. Source-account prefetches and any classic trustline
loads then repeatedly query ACCOUNT/TRUSTLINE keys against buckets whose range
indexes mostly describe Soroban entry types.

## Target Code

- `src/bucket/DiskIndex.cpp:scan:59-85` — always lower-bounds the full `keysToOffset` vector and runs the bloom filter
- `src/bucket/DiskIndex.cpp:164-167,188-191,252-253` — already records `typeRanges` while building the index
- `src/bucket/LiveBucketIndex.cpp:getRangeForType:285-295` — exposes the existing per-type metadata
- `src/bucket/BucketListSnapshot.cpp:getBucketEntry:171-201` — point lookups never consult type ranges before `lookup()`
- `src/bucket/BucketListSnapshot.cpp:loadKeysFromBucket:210-276` — bulk loads likewise call `scan()` without type-based pruning
- `src/bucket/BucketListSnapshot.cpp:scanForEntriesOfType:653-709` — demonstrates the codebase already trusts type ranges for targeted scans

## Evidence

The index builder maintains exact start/end offsets for each `LedgerEntryType`,
and `scanForEntriesOfType()` already uses that information to seek directly to
the right region. Yet ordinary point and bulk lookups do not exploit it at all:
they still binary-search the whole range index and perform bloom probing even
when the bucket lacks the requested type entirely. That is especially wasteful
for apply-load's classic-key prefetches against Soroban-heavy buckets.

## Anti-Evidence

Many buckets will still contain at least some ACCOUNT or TRUSTLINE pages, so the
optimization does not eliminate all index work. Realizing it cleanly may require
augmenting the index with iterator bounds, not just byte offsets, to avoid
introducing another search step that erases part of the win.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full lookup path from `SearchableBucketListSnapshot::load()` through
`getBucketEntry()` → `LiveBucketIndex::lookup()` → `DiskIndex::scan()`. Confirmed
that `scan()` always binary-searches the entire `keysToOffset` vector and probes
the `BinaryFuseFilter` (which allocates via `xdr_to_opaque()`) without consulting
`typeRanges`. Meanwhile, `scanForEntriesOfType()` at line 667 already calls
`bucket->getRangeForType(type)` and skips buckets returning nullopt — proving the
metadata is trusted and available. The optimization of skipping entire buckets
when `getRangeForType(k.type()) == nullopt` is trivially correct.

### Code Paths Examined

- `src/bucket/DiskIndex.cpp:scan:59-86` — binary-searches full `keysToOffset`, then probes bloom filter; no type filtering
- `src/bucket/DiskIndex.cpp:119-130` — `getRangeForType()` returns nullopt when type absent; already implemented and working
- `src/bucket/DiskIndex.cpp:164-253` — index construction builds `typeRanges` from file offsets during bucket scan
- `src/bucket/LiveBucketIndex.cpp:223-240` — `lookup()` delegates to DiskIndex::scan or InMemoryIndex::scan; no type check
- `src/bucket/LiveBucketIndex.cpp:242-257` — `scan()` similarly delegates without type check
- `src/bucket/BucketListSnapshot.cpp:169-201` — `getBucketEntry()` checks isEmpty() then calls `lookup()` directly
- `src/bucket/BucketListSnapshot.cpp:210-276` — `loadKeysFromBucket()` iterates keys calling `scan()` without type filter
- `src/bucket/BucketListSnapshot.cpp:653-709` — `scanForEntriesOfType()` uses `getRangeForType()` to skip buckets; proves pattern is safe
- `src/util/BinaryFuseFilter.cpp:34-40` — `contains()` allocates via `xdr_to_opaque(key)` on every probe
- `src/ledger/LedgerTxn.cpp:3061-3068` — prefetch explicitly rejects Soroban keys; only classic ACCOUNT/TRUSTLINE

### Findings

**The inefficiency is real but the practical impact in apply-load benchmarks is
very limited.** Here's why:

1. **The "skip entire bucket" optimization is trivially correct**: Adding a
   `getRangeForType(k.type()) == nullopt` check before `lookup()`/`scan()` would
   skip the binary search + bloom probe entirely. Cost: one `std::map::find()`
   (~20ns). Savings per skipped probe: binary search (~100-200ns for log2(N)
   LedgerKey comparisons) + bloom probe allocation (~100ns for `xdr_to_opaque`
   heap alloc + SipHash).

2. **ACCOUNT entries exist in nearly all buckets during apply-load**: Every Soroban
   transaction modifies its source account (sequence number bump), so ACCOUNT
   entries flow into level 0 every ledger and propagate through all levels. The
   type-range check would almost never return nullopt for ACCOUNT lookups.

3. **The "narrow the search range" approach is questionable**: `typeRanges` stores
   file offsets, not iterator positions into `keysToOffset`. Converting file
   offsets to iterator bounds requires additional binary searches on offsets,
   which may erase the savings from the narrower key search range.

4. **Savings estimate**: ~5000 lookups/ledger × 22 buckets × ~5% prunable × ~250ns
   = ~1.4ms/ledger. At ~200ms ledger close time, this is <1% — below measurement
   threshold.

5. **Secondary benefit**: Reducing unnecessary `xdr_to_opaque` allocations from
   bloom probes reduces malloc contention under parallel apply (T=8), but this
   is a second-order effect.

**Severity downgrade rationale**: The hypothesis claims Low severity (5-10%
improvement), but the actual impact is Informational because ACCOUNT entries
(the dominant prefetch target) exist in essentially all buckets, so type pruning
rarely triggers. The optimization is correct and trivially implementable but
unlikely to produce measurable benchmark improvement.

### PoC Guidance

- **Target code**: `src/bucket/LiveBucketIndex.cpp` — `lookup()` (line 223-240) and `scan()` (line 242-257)
- **Change description**: Add an early-return check at the top of `lookup()` and `scan()`: if `mDiskIndex` is active, call `mDiskIndex->getRangeForType(k.type())`; if nullopt, return NOT_FOUND immediately (for lookup) or `{IndexReturnT(), start}` (for scan). For `InMemoryIndex`, the same check via `mInMemoryIndex->getRangeForType()`. This mirrors the pattern already used in `scanForEntriesOfType()` at BucketListSnapshot.cpp:667.
- **Correctness check**: Existing bucket index tests (`[bucket]`, `[bucketindex]`) cover lookup paths. Run `"[bucket]"` and `"[bucketindex]"` test tags. Also verify `scanForEntriesOfType` tests still pass since it already uses this pattern.
- **Benchmark focus**: Measure bloom lookup meter and bloom miss meter rates. The optimization should reduce bloom lookups (fewer probes). Measure with `apply-load` at T=8 with mixed classic+Soroban workload. Expected improvement: <1% on wall-clock time; reduction in bloom probe count is the primary observable.
