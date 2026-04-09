# H002: Level-0 index construction copies every merged entry into a second heap structure

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: Medium
**Impact**: ledger-close serial CPU and allocation pressure
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once `mergeInMemory` has produced a sorted `mergedEntries` vector and stores it inside the new `LiveBucket`, building the lookup structure for that bucket should reuse that data directly. The hot close path should not allocate one `shared_ptr<BucketEntry const>` per entry and duplicate the entire merged level-0 contents into a separate hash set just to support lookups on a bucket that already keeps the entries in order.

## Mechanism

`LiveBucket::mergeInMemory` hands its merged vector to `BucketOutputIterator::getBucket`, which constructs `LiveBucketIndex` from `inMemoryState`; `LiveBucketIndex` then builds an `InMemoryIndex`, and `InMemoryIndex::insert` wraps every `BucketEntry` in a new heap-allocated `shared_ptr` before inserting it into `mEntries`. This creates a second per-entry object graph for every level-0 rebuild even though the `LiveBucket` already owns the original sorted vector, so large write-heavy ledgers pay an avoidable O(n) allocation and copy tax on the main thread.

## Trigger

Run any apply-load model-transaction benchmark with enough write traffic that level 0 contains thousands of entries per close. The cost scales directly with the number of merged level-0 entries and shows up on every ledger, not just spill ledgers.

## Target Code

- `src/bucket/LiveBucket.cpp:564-612` — `mergeInMemory` creates `mergedEntries` and passes them to `getBucket` as `inMemoryState`
- `src/bucket/BucketOutputIterator.cpp:220-242` — `getBucket` constructs a `LiveBucketIndex` from that in-memory state
- `src/bucket/LiveBucketIndex.cpp:84-92` — in-memory-state constructor always builds an `InMemoryIndex`
- `src/bucket/InMemoryIndex.cpp:55-117` — `insert` allocates a new `shared_ptr<BucketEntry const>` per entry and populates auxiliary maps/ranges

## Evidence

The level-0 bucket already keeps `mEntries` specifically so future level-0 merges can avoid file I/O, and those entries are sorted by key. Despite that, the in-memory index path duplicates each entry into `InternalInMemoryBucketEntry(std::make_shared<BucketEntry const>(be))`, meaning the main thread walks the full merged vector again and performs per-entry allocations before the ledger can finish closing.

## Anti-Evidence

Some query helpers currently expect index-owned structures such as type ranges and asset-to-pool mappings, so replacing `InMemoryIndex` with direct vector search is not a one-line change. But level 0 has a much narrower hot-path requirement than long-lived disk buckets, so a lighter-weight level-0 lookup path should still be possible without changing higher-level semantics.
