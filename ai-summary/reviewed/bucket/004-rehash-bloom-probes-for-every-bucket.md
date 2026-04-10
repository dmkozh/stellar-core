# H004: The same lookup key is re-encoded and re-hashed for every bucket-level bloom probe

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Informational
**Impact**: bucket lookup CPU in prefetch and classic fallback loads
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When a single `LedgerKey` is probed against multiple bucket filters, the bucket
code should compute that key's bloom digest once and reuse it across all bucket
probes for the lookup. Re-encoding the same key for every bucket should not be
part of the steady-state miss path.

## Mechanism

`BinaryFuseFilter::contains()` always runs `xdr::xdr_to_opaque(key)` and SipHash
on the supplied `LedgerKey`. `DiskIndex::scan()` calls `contains(k)` for every
bucket probe, and `SearchableBucketListSnapshot::{load,loadKeysFromBucket}` can
probe the same key against many buckets until it is found or exhausted. In
apply-load, source-account prefetch and classic trustline fallback loads often
check several younger buckets first, so the exact same key encoding and SipHash
work is repeated across buckets even though the digest is bucket-independent.

## Trigger

Run any apply-load scenario with bucket-backed state and default prefetch. Every
source-account key that is absent from younger buckets before being found in an
older one will recompute the same bloom digest multiple times in the same
ledger.

## Target Code

- `src/util/BinaryFuseFilter.cpp:34-39` — `contains()` always allocates/encodes and SipHashes the full `LedgerKey`
- `src/bucket/DiskIndex.cpp:75-76` — every candidate bucket probe calls `filter->contains(k)`
- `src/bucket/BucketListSnapshot.cpp:221-229` — bulk loader reuses unresolved keys across bucket levels
- `src/bucket/BucketListSnapshot.cpp:326-344` — point loader likewise probes the same key through multiple buckets
- `src/ledger/LedgerTxn.cpp:3090-3097` — prefetch feeds large repeated key batches into `loadLiveKeys(...)`

## Evidence

The hash seed lives in the filter, but the input digest depends only on the
`LedgerKey`, so the expensive `xdr_to_opaque + SipHash24` step is reusable
across every bucket that shares the same filter seed format. Today that work is
buried inside `contains()`, which means the digest is rebuilt for each probe
instead of once per lookup.

## Anti-Evidence

If lookups usually hit in very young buckets, the repeated-probe count is low.
This also only attacks the bloom-filter portion of the miss path; large range
index searches or page reads may still dominate unless combined with other
lookup-path reductions.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (H009 in fail/bucket addressed per-probe allocation only, not cross-bucket digest caching; this is a distinct optimization vector)

### Trace Summary

Traced the complete bloom filter probe path from `BinaryFuseFilter::contains()` through `DiskIndex::scan()` and into the two lookup loops (`load()` and `loadKeysFromBucket()`). Confirmed that every bucket filter in a process shares the same `mHashSeed` (from `shortHash::getShortHashInitKey()`, a per-process random key), meaning the `uint64_t` SipHash digest for a given `LedgerKey` is identical across all bucket probes. The `xdr_to_opaque` serialization and SipHash computation are repeated per-probe despite producing the same result. However, the bloom filter check occurs AFTER the range-index binary search (per a deliberate design choice documented in DiskIndex.cpp:66-69), so it only fires on keys that land within a candidate range entry, limiting the total repetition count.

### Code Paths Examined

- `src/util/BinaryFuseFilter.cpp:34-39` — `contains()` calls `xdr::xdr_to_opaque(key)` (heap allocation + XDR serialization) then `SipHash24(mHashSeed.data()).update(keybuf).digest()` on every invocation. No caching of the serialized bytes or digest.
- `src/crypto/ShortHash.cpp:28-35` — `getShortHashInitKey()` returns the global per-process `gKey`. All DiskIndex constructors call this (DiskIndex.cpp:162), so all filters in a process start with the same seed.
- `src/bucket/DiskIndex.cpp:59-86` — `scan()` performs `lower_bound` first (line 71), then checks `mData.filter->contains(k)` (line 76). The bloom filter is a secondary guard against disk I/O, not the primary index mechanism.
- `src/bucket/DiskIndex.cpp:256-266` — Filter construction retry loop: on rare `std::out_of_range`, `seed[0]++` modifies the seed. This means a tiny fraction of filters could have a different `mHashSeed`, preventing blind digest reuse without a seed check.
- `src/bucket/BucketListSnapshot.cpp:315-346` — Point lookup `load()`: calls `getBucketEntry(bucket, k)` per bucket via `loopAllBuckets()`. Each call goes through `lookup()` → `scan()` → `contains()`. Same key is re-serialized and re-hashed per bucket.
- `src/bucket/BucketListSnapshot.cpp:210-277` — Bulk lookup `loadKeysFromBucket()`: calls `index.scan(indexIter, *currKeyIt)` per key per bucket. Same per-key overhead.
- `lib/binaryfusefilter.h:535-543` — `binary_fuse_t::contain(uint64_t key)` performs a SECOND SipHash (`sip_hash24(key, Seed)`) internally. This internal hash uses the filter's structural `Seed`, not `mHashSeed`. Only the outer `mHashSeed`-based digest is reusable across buckets.

### Findings

**The inefficiency is real.** Every bucket probe for the same `LedgerKey` repeats:
1. `xdr::xdr_to_opaque(key)` — heap-allocates a `vector<uint8_t>` and serializes the key (~40-80 bytes for typical keys).
2. `SipHash24` initialization, update, and digest — ~20-30ns for this data volume.

All filters in a normal process share the same `mHashSeed` (from `getShortHashInitKey()`), making the resulting `uint64_t` digest identical across bucket probes. The rare filter-construction retry (which increments `seed[0]`) is the only case where seeds can differ.

**Impact is correctly assessed as Informational.** For a point lookup that probes N buckets (typically 5-10 for keys in older levels), N-1 redundant `xdr_to_opaque + SipHash` computations are performed, costing ~200-250ns each. For 1000-key prefetch batches with ~5 average probes per key, total waste is ~1ms — roughly 0.1-0.5% of ledger close time. This is below the measurement threshold for apply-load benchmarks.

**Distinction from H009:** H009 proposed eliminating the per-probe `xdr_to_opaque` heap allocation (making each individual probe cheaper). H004 proposes caching the complete `xdr_to_opaque + SipHash` result across bucket probes for the same key (eliminating N-1 of N total computations). These address different waste patterns. H009's lesson ("prioritize removing whole probes over polishing individual probes") remains valid, and H004's Informational severity is consistent with that lesson.

### PoC Guidance

- **Target code**: `src/util/BinaryFuseFilter.cpp` (add `containsDigest(uint64_t digest)` method); `src/bucket/DiskIndex.cpp:scan()` (add overload accepting pre-computed digest); `src/bucket/BucketListSnapshot.cpp:load()` and `loadKeysFromBucket()` (pre-compute digest once per key, pass to all bucket probes).
- **Change description**: (1) Add `BinaryFuseFilter::containsDigest(uint64_t)` that calls `mFilter.contain(digest)` directly. (2) Add `DiskIndex::scanWithDigest(IterT, LedgerKey, uint64_t)` that uses the pre-computed digest instead of calling `contains()`. (3) In `load()`, compute digest once via `xdr_to_opaque + SipHash24(seed)` before entering the bucket loop, passing it to each `getBucketEntry` call. (4) In `loadKeysFromBucket()`, similarly pre-compute per-key digests. Note: the seed must match the filter's `mHashSeed`; for the rare retry case, a fallback to the full `contains()` path is needed if the cached seed doesn't match.
- **Correctness check**: `[bucketindex]` tests (1M+ assertions) cover lookup correctness. `[bucket]` tests cover BucketList integrity. Both suites should pass unchanged since the optimization only changes how the digest is computed, not what value it produces.
- **Benchmark focus**: Measure `loadKeys()` bulk latency with 1000+ keys distributed across many bucket levels. Watch total CPU time in `BinaryFuseFilter::contains` and `xdr_to_opaque`. Expect <1% improvement on overall apply-load benchmarks; potentially measurable in microbenchmarks of the bloom probe path in isolation.
