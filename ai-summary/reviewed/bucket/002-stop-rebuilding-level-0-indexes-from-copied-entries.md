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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

I traced the complete level-0 ledger-close path: `addBatchInternal` → `prepareFirstLevel` → `LiveBucket::mergeInMemory` → `BucketOutputIterator::getBucket` → `LiveBucketIndex(bm, inMemoryState, metadata)` → `InMemoryIndex` constructor → `InMemoryBucketState::insert` per entry. The insert path at InMemoryIndex.cpp:58-59 does `mEntries.insert(InternalInMemoryBucketEntry(std::make_shared<BucketEntry const>(be)))`, which heap-allocates a copy of each BucketEntry plus a shared_ptr control block, a unique_ptr<ValueEntry> wrapper (InMemoryIndex.h:103,107), and an unordered_set node — three heap allocations per entry — while the bucket's `mEntries` vector already holds the same data in sorted order. This occurs synchronously on the main thread every ledger close.

### Code Paths Examined

- `src/bucket/BucketListBase.cpp:196-238` (`prepareFirstLevel`) — Confirmed level 0 always takes the in-memory merge path when `curr->hasInMemoryEntries()` is true (the normal case after the first ledger).
- `src/bucket/LiveBucket.cpp:549-613` (`mergeInMemory`) — Creates `mergedEntries` vector, writes to disk via `BucketOutputIterator::put()`, then calls `getBucket(bucketManager, nullptr, make_unique<vector>(move(mergedEntries)))`. The vector is moved into the unique_ptr, so the bucket owns it.
- `src/bucket/BucketOutputIterator.cpp:214-242` (`getBucket`) — Checks for an existing indexed bucket by hash; if none found and `inMemoryState` is non-null, constructs `LiveBucketIndex(bm, *inMemoryState, mMeta)`. For write-heavy workloads where entries change every ledger, the existing-bucket check rarely hits.
- `src/bucket/LiveBucketIndex.cpp:84-92` — In-memory-state constructor unconditionally creates `InMemoryIndex(bm, inMemoryState, metadata)`.
- `src/bucket/InMemoryIndex.cpp:78-117` — Constructor iterates all entries calling `processEntry`, which calls `mInMemoryState.insert(be)`.
- `src/bucket/InMemoryIndex.cpp:55-61` (`InMemoryBucketState::insert`) — **The duplication site**: `std::make_shared<BucketEntry const>(be)` copies each entry into a new heap allocation.
- `src/bucket/InMemoryIndex.h:26-133` (`InternalInMemoryBucketEntry`) — Each entry is wrapped in `unique_ptr<AbstractEntry>` → `ValueEntry` → `IndexPtrT` (shared_ptr<BucketEntry const>). Three levels of indirection per entry.
- `src/bucket/LiveBucket.cpp:467-498` (`freshInMemoryOnly`) — The "level -1" snap bucket created without index, confirming only the merge result gets indexed.
- `src/bucket/LiveBucket.h:39,172-181` — Confirms `mEntries` (unique_ptr<vector<BucketEntry>>) persists on the bucket for future merges, existing alongside the duplicated index data.

### Findings

The inefficiency is confirmed and real. Per entry in the merged level-0 bucket, the index construction performs:

1. **`std::make_shared<BucketEntry const>(be)`** — Full deep copy of the XDR BucketEntry data (varies from ~200 bytes for accounts to ~4KB+ for contract data) plus 16-byte shared_ptr control block.
2. **`std::make_unique<ValueEntry>(entry)`** — ~32-byte heap allocation for the polymorphic wrapper.
3. **`unordered_set::insert`** — Node allocation (~40 bytes) plus hash computation via `std::hash<LedgerKey>{}(getBucketLedgerKey(*entry))`.

For a write-heavy benchmark with ~1000 entries in level 0, this is ~3000 heap allocations and ~1-4MB of copied data per ledger close. The bucket already holds the same entries in its `mEntries` sorted vector.

However, the overall severity is **Informational** rather than Medium because:
- Level 0 typically contains entries from only 2 ledgers (sizeOfCurr(0) = 2), bounding the entry count
- Ledger close time is dominated by transaction application (Soroban host execution), which dwarfs the index construction cost
- The 3000 allocations likely take 0.5-2ms, against a total ledger close of 100-500ms in benchmarks
- The `InMemoryIndex` metadata (AssetPoolIDMap, BucketEntryCounters, typeRanges) still needs to be computed regardless — only the unordered_set population can be eliminated

The fix is correct in principle: the sorted vector already supports O(log n) binary search (n ≈ hundreds to low thousands, so ~10 comparisons), which could replace the O(1) hash lookup with negligible query-time regression while eliminating all per-entry allocations on the close path.

### PoC Guidance

- **Target code**: `src/bucket/InMemoryIndex.cpp` (constructor at line 78), `src/bucket/InMemoryIndex.h` (`InMemoryBucketState` class), `src/bucket/LiveBucketIndex.cpp` (constructor at line 84)
- **Change description**: Create a lightweight level-0-specific index variant that stores a `const vector<BucketEntry>*` (non-owning pointer to the bucket's `mEntries`) instead of building an `unordered_set`. Implement `scan()` via `std::lower_bound` on the sorted vector using `BucketEntryIdCmp`. Continue computing `AssetPoolIDMap`, `BucketEntryCounters`, and `typeRanges` during the single iteration pass but skip the per-entry heap allocation. The `IndexReturnT` for cache hits would need to return a pointer into the vector rather than a `shared_ptr`, which may require adjusting the `IndexPtrT` handling in `SearchableBucketListSnapshot::getBucketEntry()`.
- **Correctness check**: Existing tests `[bucket]` and `[bucketindex]` cover point lookups and bulk loads through level-0 buckets. Run `"[bucket]"` and `"[bucketindex]"` test tags. Also verify `BucketListDB` tests in `BucketIndexTests.cpp` (lines 773-828 exercise in-memory index paths with `BUCKETLIST_DB_INDEX_CUTOFF = 0`).
- **Benchmark focus**: Run apply-load benchmarks (sac and custom_token scenarios at T=1 and T=8) and measure: (1) per-ledger close time (median and p99), (2) heap allocation rate via `MALLOC_STATS` or Tracy allocator profiling. Expect reduced allocation pressure but likely <5% improvement in wall-clock ledger close time.
