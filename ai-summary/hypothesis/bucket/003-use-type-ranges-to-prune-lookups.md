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
