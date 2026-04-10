# H009: Eliminating BinaryFuseFilter probe allocations alone is too narrow

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: bucket negative-lookup micro-overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Bloom-filter probes should hash `LedgerKey` values without allocating a fresh
opaque buffer on every call. A negative lookup should not need a heap allocation
just to feed bytes into SipHash.

## Mechanism

`BinaryFuseFilter::contains()` converts the queried `LedgerKey` to a new
`std::vector<uint8_t>` via `xdr::xdr_to_opaque(key)` before hashing it. Since
`DiskIndex::scan()` calls `contains()` for every probed bucket, repeated misses
can create avoidable heap churn on the lookup path.

## Trigger

Run any workload that performs many negative bucket probes for classic keys, such
as prefetching source accounts or trustlines that are not found in the youngest
buckets.

## Target Code

- `src/util/BinaryFuseFilter.cpp:contains:33-39` — allocates and copies via `xdr_to_opaque(key)` on every probe
- `src/bucket/DiskIndex.cpp:scan:61-85` — invokes the filter for every indexed lookup
- `src/bucket/BucketListSnapshot.cpp:getBucketEntry/loadKeysFromBucket:171-201,210-276` — hot lookup callers that fan this out across buckets

## Evidence

The probe path definitely allocates: `xdr::xdr_to_opaque(key)` materializes a
new buffer before SipHash runs, and nothing in `DiskIndex::scan()` caches or
reuses that opaque representation across probes. On paper, a no-allocation hash
API or stack-backed encoder would remove those allocations.

## Anti-Evidence

The allocation is only one part of the miss path. Each probe also performs a
range-index search, and many apply-load lookups resolve in early buckets rather
than fanning out across all levels. Without reducing the number of bucket probes
themselves, this stays a micro-optimization.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The per-probe allocation is real, but fixing it in isolation attacks too small a
fraction of the total lookup cost. A broader change that skips entire probes
(for example by using type-based pruning) is more likely to matter than merely
making the existing probes a bit cheaper.

### Lesson Learned

On bucket lookups, prioritize optimizations that remove whole bucket probes or
whole page reads before polishing the cost of an individual negative probe. The
allocation is a symptom; the dominant lever is probe count.
