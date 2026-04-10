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
