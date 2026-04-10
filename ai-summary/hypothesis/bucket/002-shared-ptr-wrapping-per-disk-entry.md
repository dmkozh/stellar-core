# H002: getEntryAtOffset unconditionally wraps every disk-loaded entry in shared_ptr

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: bulk-load and point-load CPU for entries in disk-indexed buckets
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When a bucket entry is loaded from disk for a bulk or point lookup, and the
random-eviction cache is disabled (the benchmark default), the entry should be
returned to the caller without heap-allocating a `shared_ptr` wrapper. The
caller (`loadKeysFromBucket`) immediately extracts `liveEntry()` and discards
the wrapper, so the allocation serves no purpose when caching is off.

## Mechanism

`SearchableBucketListSnapshot::getEntryAtOffset` (BucketListSnapshot.cpp:157)
performs the following sequence for every entry found in a disk-backed bucket:

1. XDR-decode the bucket page into a stack-local `BucketEntry be`.
2. `std::make_shared<BucketEntry const>(be)` — heap-allocate and deep-copy the
   entire entry into a `shared_ptr`.
3. `bucket->getIndex().maybeAddToCache(entry)` — with default config
   (`BUCKETLIST_DB_MEMORY_FOR_CACHING = 0`), this is a no-op: `shouldUseCache()`
   returns false immediately.
4. Return `{entry, false}` — the `shared_ptr` is passed back through
   `getBucketEntry` to the caller.

In `loadKeysFromBucket` (line 263), the caller does
`result.push_back(entryOp->liveEntry())`, which deep-copies the `LedgerEntry`
out of the `BucketEntry`, then lets the `shared_ptr` die — freeing the heap
object immediately.

So each disk-loaded entry pays: XDR decode (necessary) → `shared_ptr` alloc +
BucketEntry deep copy (wasted) → `liveEntry()` copy (necessary) →
`shared_ptr` dealloc (wasted). The `shared_ptr` wrapping exists solely to
support the cache-hit return path (`IndexReturnState::CACHE_HIT`), but when
caching is disabled, no lookup ever takes that path.

For the `sac,TX=3200` benchmark after source accounts have settled into
disk-indexed levels (levels 1+), each ledger's prefetch loads ~3200
source-account entries from disk buckets. Each unnecessary `shared_ptr` costs
~100-200ns (heap alloc + deep copy + atomic refcount + dealloc), totaling
~320-640μs per ledger. Against a 10-50ms close, this is ~1-6%.

## Trigger

Run any apply-load scenario with disk-backed buckets (default config with
`BUCKETLIST_DB_MEMORY_FOR_CACHING = 0`) long enough that source accounts spill
out of level 0 into disk-indexed levels. The `prefetchTxSourceIds` path will
then load entries through `getEntryAtOffset` with wasted `shared_ptr` wrapping.

## Target Code

- `src/bucket/BucketListSnapshot.cpp:139-164` — `getEntryAtOffset` creates
  `std::make_shared<BucketEntry const>(be)` for every disk-read entry
- `src/bucket/BucketListSnapshot.cpp:231-250` — `loadKeysFromBucket` calls
  `getEntryAtOffset` per key hit, then immediately extracts `liveEntry()`
- `src/bucket/BucketListSnapshot.cpp:170-201` — `getBucketEntry` returns the
  `shared_ptr` through `getEntryAtOffset`
- `src/bucket/LiveBucketIndex.cpp:200-221` — `getCachedEntry` is a no-op when
  `shouldUseCache()` returns false (cache disabled)
- `src/main/Config.cpp:177` — default `BUCKETLIST_DB_MEMORY_FOR_CACHING = 0`

## Evidence

The apply-load benchmark config uses the default `BUCKETLIST_DB_MEMORY_FOR_CACHING = 0`,
which means `LiveBucketIndex::shouldUseCache()` always returns false, and
`maybeAddToCache()` always returns immediately without storing anything. The
`shared_ptr<BucketEntry const>` wrapping therefore has zero consumers: it is
allocated, passed through two function returns, copied once via `liveEntry()`,
and immediately freed.

The point-load path (`load()`) has the same issue: `getBucketEntry` returns a
`shared_ptr`, then `bucketEntryToLoadResult(be)` extracts the load result,
and the `shared_ptr` is discarded.

## Anti-Evidence

When caching IS enabled (production validators with
`BUCKETLIST_DB_MEMORY_FOR_CACHING > 0`), the `shared_ptr` wrapping is needed
for cache insertion. Any optimization must preserve this path. One approach is
to template the lookup path on a `CacheEnabled` bool, or to return a
`std::variant<BucketEntry, shared_ptr<BucketEntry const>>` and only allocate
the `shared_ptr` when caching is active. The fix is straightforward but touches
a frequently-used interface, so care is needed to avoid regressions.
