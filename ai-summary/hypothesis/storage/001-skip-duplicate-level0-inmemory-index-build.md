# H001: Skip the duplicate `InMemoryIndex` build for level-0 in-memory buckets

**Date**: 2026-04-10
**Subsystem**: storage (bucket)
**Severity**: Low
**Impact**: post-apply commit CPU and memory-bandwidth reduction in level-0 bucket maintenance
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When level-0 merge output is already retained as a sorted in-memory
`std::vector<BucketEntry>` for the next ledger's merge, storage should not
immediately build a second full in-memory representation of the same entries
just to answer lookups. The merged bucket should be queryable without deep-copying
every entry into a separate `InMemoryIndex`.

## Mechanism

`LiveBucket::mergeInMemory(...)` already materializes the full merged output in
`mergedEntries` and passes it to `getBucket(...)` as `inMemoryState`, where the
new `LiveBucket` keeps that vector in `mEntries`. `BucketOutputIterator::getBucket(...)`
then immediately constructs `LiveBucketIndex(bucketManager, *inMemoryState, mMeta)`,
and `InMemoryIndex` walks the same vector again, copying every `BucketEntry`
into `std::shared_ptr<BucketEntry const>` nodes inside an `unordered_set`.

That means every ledger-close level-0 merge pays for two in-memory
representations of the same newest bucket: the sorted `mEntries` vector needed
for the next merge and the duplicated `InMemoryIndex` payload needed for reads.
Because level 0 is rebuilt synchronously on every ledger close, replacing the
duplicate index with direct binary-search / range-scan support over `mEntries`
or a pointer-only lightweight index should reduce serial post-apply work in the
benchmark path.

## Trigger

Run any Soroban apply-load scenario with substantial state churn so level 0
contains thousands of entries every ledger. Each close rebuilds the merged
level-0 bucket, stores `mergedEntries` for the next in-memory merge, and then
rebuilds a second representation of those same entries for indexing.

## Target Code

- `src/bucket/LiveBucket.cpp:mergeInMemory:550-612` — produces `mergedEntries` and passes them as retained in-memory state
- `src/bucket/LiveBucket.h:getInMemoryEntries:171-181` — confirms the bucket already keeps the merged vector
- `src/bucket/BucketOutputIterator.cpp:getBucket:220-228` — unconditionally builds `LiveBucketIndex` from that same `inMemoryState`
- `src/bucket/LiveBucketIndex.cpp:84-91` — the in-memory-state constructor routes to `InMemoryIndex`
- `src/bucket/InMemoryIndex.cpp:55-60` — each indexed entry is deep-copied into `std::shared_ptr<BucketEntry const>`
- `src/bucket/InMemoryIndex.cpp:78-117` — full second pass over the merged vector to build hash/index metadata
- `src/bucket/BucketListSnapshot.cpp:getBucketEntry:171-196` — query path that currently forces all buckets through `getIndex()`

## Evidence

The level-0 path is special: unlike file-based buckets, it already keeps the
full merged entry vector alive specifically so the next ledger can merge against
it in memory. The current code then pays an immediate second pass plus per-entry
heap allocation to build `InMemoryIndex`, even though the backing vector is
sorted and already owned by the bucket. This is hot because `prepareFirstLevel`
uses `mergeInMemory(...)` on every normal ledger close.

## Anti-Evidence

The query layer currently assumes every bucket answers through the `IndexT`
interface, so exploiting `mEntries` directly requires either a new lightweight
index wrapper or in-memory fast paths in `SearchableBucketListSnapshot`. A
vector-based lookup path could also regress workloads dominated by repeated
random lookups if it is not carefully specialized for the small newest buckets.
