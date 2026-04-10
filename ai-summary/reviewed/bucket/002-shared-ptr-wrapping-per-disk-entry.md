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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete lookup path from `getEntryAtOffset()` through `getBucketEntry()` to both callers: `load()` (point lookup) and `loadKeysFromBucket()` (bulk prefetch). Confirmed that `readPage()` deserializes into a stack-local `BucketEntry be`, which is then copy-constructed into a `make_shared<BucketEntry const>(be)` on line 157. The `maybeAddToCache()` call on line 158 acquires a shared lock on `mCacheMutex` in `shouldUseCache()` and returns false when `mCache` is null (cache disabled). Both callers immediately extract data from the shared_ptr and discard it: `loadKeysFromBucket` does `entryOp->liveEntry()` (line 263); `load()` calls `bucketEntryToLoadResult(be)` which itself creates a second `make_shared<LedgerEntry>(be->liveEntry())`.

### Code Paths Examined

- `src/bucket/BucketListSnapshot.cpp:139-164` — `getEntryAtOffset()`: confirmed `make_shared<BucketEntry const>(be)` copies from stack-local `be`; no `std::move` is used
- `src/bucket/BucketListSnapshot.cpp:210-277` — `loadKeysFromBucket()`: each `FILE_OFFSET` hit calls `getEntryAtOffset()`, extracts `liveEntry()`, and the shared_ptr dies at end of loop iteration
- `src/bucket/BucketListSnapshot.cpp:314-346` — `load()`: calls `getBucketEntry()` → `getEntryAtOffset()`, then `bucketEntryToLoadResult(be)` creates a SECOND shared_ptr (`make_shared<LedgerEntry>(be->liveEntry())`), making the first shared_ptr pure overhead
- `src/bucket/LiveBucketIndex.cpp:218-230` — `shouldUseCache()`: acquires `SharedLockShared` on `mCacheMutex` (an atomic operation), checks `mCache != nullptr`, returns false when cache disabled — this lock is acquired per entry even when caching is off
- `src/bucket/LiveBucket.cpp:bucketEntryToLoadResult` — creates `make_shared<LedgerEntry>(be->liveEntry())`, adding a second heap allocation on the point-lookup path
- `src/util/XDRStream.h:readPage:180-240` — deserializes entries into `out` parameter by reference; `be` is a complete stack-local value after return, suitable for `std::move`

### Findings

The inefficiency is confirmed and well-characterized:

1. **Unnecessary heap allocation**: Every disk-loaded entry creates a `shared_ptr<BucketEntry const>` via `make_shared`. When cache is disabled (`BUCKETLIST_DB_MEMORY_FOR_CACHING = 0`, the benchmark and common default), the shared_ptr is never stored in the cache and is discarded by callers within the same function scope.

2. **Unnecessary deep copy**: `make_shared<BucketEntry const>(be)` copy-constructs from `be` because `be` is passed as an lvalue. Even without eliminating the shared_ptr, `std::move(be)` would avoid the deep copy since `be` is not used after this point. XDR-generated types have implicit move constructors that efficiently transfer vector/string members.

3. **Point lookup path has double allocation**: `load()` → `getBucketEntry()` → `getEntryAtOffset()` creates shared_ptr #1 (`BucketEntry`), then `bucketEntryToLoadResult()` creates shared_ptr #2 (`LedgerEntry` from `be->liveEntry()`). The first shared_ptr is never consumed — it exists only as a vehicle to call `liveEntry()`.

4. **Per-entry lock overhead**: `maybeAddToCache()` calls `shouldUseCache()` which acquires a `SharedLockShared` on `mCacheMutex` per entry, even when caching is permanently disabled. This adds ~10-20ns per entry in atomic operations.

**Severity downgrade from Low to Informational:**

The per-entry cost is ~100-200ns, and the hypothesis claim of ~3200 entries per ledger from disk-indexed buckets is plausible for long-running SAC benchmarks. However, the total overhead of ~320-640μs per ledger represents only ~1-3% of total ledger close time. The closely related H006 (page reuse optimization on the same code path) was benchmarked and showed no net improvement on apply-load scenarios despite targeting a potentially larger inefficiency (redundant 16KB page reads). This strongly suggests that optimizations to the per-entry overhead in this code path are below the noise floor of the apply-load benchmark.

The simplest partial fix (`std::move(be)` in `getEntryAtOffset`) is trivially correct and avoids the deep copy (~20-50ns per entry), but does not eliminate the allocation/deallocation overhead (~80-160ns). The full fix (variant return type or compile-time cache enable/disable) is more impactful but requires coordinated changes across `getEntryAtOffset`, `getBucketEntry`, `loadKeysFromBucket`, `load`, and `bucketEntryToLoadResult`.

### PoC Guidance

- **Target code**: `src/bucket/BucketListSnapshot.cpp:getEntryAtOffset` (line 157), `src/bucket/BucketListSnapshot.cpp:loadKeysFromBucket` (lines 231-270), `src/bucket/BucketListSnapshot.cpp:getBucketEntry` (lines 170-201), `src/bucket/LiveBucket.cpp:bucketEntryToLoadResult`
- **Change description**: Two approaches, from simplest to most impactful:
  1. **Minimal**: Change line 157 from `make_shared<BucketEntry const>(be)` to `make_shared<BucketEntry const>(std::move(be))`. This avoids the deep copy (~20-50ns/entry) while preserving the current interface. Safe because `be` is not used after this point.
  2. **Full**: Have `getEntryAtOffset` return `std::optional<BucketEntry>` when cache is disabled (or unconditionally), and only wrap in `shared_ptr` at the `maybeAddToCache` call site when `shouldUseCache()` is true. `loadKeysFromBucket` can then extract `liveEntry()` directly from the optional. `getBucketEntry` would need a variant or template return type. The `load()` path's `bucketEntryToLoadResult` would construct its result `shared_ptr<LedgerEntry>` directly from the by-value `BucketEntry` without the intermediate shared_ptr.
- **Correctness check**: Existing `[bucketindex]` and `[bucket]` test suites cover all affected paths. Key tests: bulk load via `loadKeys`, point load via `load`, and cache-enabled scenarios (if any test sets `BUCKETLIST_DB_MEMORY_FOR_CACHING > 0`).
- **Benchmark focus**: Likely below apply-load noise floor (~1-3% of ledger close). A micro-benchmark or Tracy profile comparing `getEntryAtOffset` latency before/after would be more appropriate for validation. If combined with other per-entry optimizations (e.g., H001 InMemoryIndex type-erasure), the cumulative effect may become measurable.
