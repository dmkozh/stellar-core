# H001: InMemoryIndex type-erasure forces heap allocation and virtual dispatch on every lookup

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Medium
**Impact**: per-transaction bucket list lookup CPU for classic entries during apply
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

Looking up a `LedgerKey` in the level-0 in-memory bucket index should be a
simple hash-table probe with no heap allocation. The index already holds all
entries in memory; a point lookup should cost roughly one hash computation, one
pointer comparison, and one equality check — all inline, with no dynamic memory
or virtual dispatch.

## Mechanism

`InMemoryBucketState` stores entries in an
`unordered_set<InternalInMemoryBucketEntry>`. Because C++17 `unordered_set`
does not support heterogeneous lookup, the code uses a type-erasure wrapper:
each entry is stored as `unique_ptr<ValueEntry>` (which itself holds a
`shared_ptr<BucketEntry const>`), and each lookup constructs a temporary
`InternalInMemoryBucketEntry(searchKey)` that allocates a
`unique_ptr<QueryKey>` on the heap.

This means every single lookup into the level-0 index pays:

1. **Heap allocation**: `std::make_unique<QueryKey>(searchKey)` — allocates and
   constructs a `QueryKey` wrapping a copy of the `LedgerKey`.
2. **Virtual dispatch on `hash()`**: calls `QueryKey::hash()` through
   `AbstractEntry*`, which copies the `LedgerKey` out via `copyKey()` and then
   hashes it with `std::hash<LedgerKey>{}`.
3. **Virtual dispatch on `operator==()`**: on a hash-bucket match, the set
   calls `AbstractEntry::operator==()`, which calls `copyKey()` on *both* the
   query and the stored entry — two full `LedgerKey` copies — just to compare
   them.
4. **Heap deallocation**: the temporary `InternalInMemoryBucketEntry` is
   destroyed, freeing the `QueryKey`.

For the apply-load benchmark at `sac,TX=3200`, each ledger has ~3200
source-account lookups that check level 0 first (the most recently modified
entries reside there). At T=8, each of 8 threads performs its share of these
lookups. The per-lookup overhead is ~150-300ns (alloc + virtual calls + dealloc),
totaling ~0.5-1.0ms per ledger across all threads. Against a serial level-0
merge time of ~5-15ms, this is a 5-10% overhead on the lookup side.

## Trigger

Run any apply-load scenario (`sac`, `custom_token`, or `soroswap`) with enough
transactions that thousands of classic source-account lookups hit the level-0
bucket per ledger. The overhead shows up in both T=1 and T=8 scenarios.

## Target Code

- `src/bucket/InMemoryIndex.h:26-133` — `InternalInMemoryBucketEntry` with
  type-erased `unique_ptr<AbstractEntry>` wrapper, virtual `hash()`,
  `operator==()`, `copyKey()`
- `src/bucket/InMemoryIndex.h:147-153` — `InMemoryBucketState` typedef of
  `unordered_set<InternalInMemoryBucketEntry>`
- `src/bucket/InMemoryIndex.cpp:56-61` — `insert()` allocates
  `shared_ptr<BucketEntry const>` per entry
- `src/bucket/InMemoryIndex.cpp:64-76` — `scan()` constructs temporary
  `InternalInMemoryBucketEntry(searchKey)` per lookup
- `src/bucket/LiveBucketIndex.cpp:236-239` — `lookup()` delegates to
  `mInMemoryIndex->scan()` for level-0 buckets

## Evidence

The current design explicitly acknowledges the C++17 limitation:
`InMemoryIndex.h:19-25` comments "C++20 allows heterogeneous lookup in
unordered_set, so we can simplify this class once we upgrade." The
`InternalInMemoryBucketEntry` class exists solely to work around this
limitation, and the workaround introduces per-lookup heap allocation and virtual
dispatch.

The file-based `DiskIndex::scan` path (DiskIndex.cpp:61-86) uses no such
wrapper — it does a `std::lower_bound` on a sorted vector with inline
comparators. The in-memory path is paradoxically MORE expensive per-lookup than
the disk-index path (excluding the actual disk I/O).

## Anti-Evidence

The InMemoryIndex is only used for level-0 buckets (small buckets below the
`BUCKETLIST_DB_INDEX_CUTOFF`). For large DiskIndex buckets, this overhead does
not apply. The actual per-lookup cost (~200ns) is small compared to disk I/O
(~1-10μs per page read), so the improvement mainly benefits workloads where most
lookups hit level 0. Additionally, replacing `unordered_set` with
`unordered_map<LedgerKey, shared_ptr<BucketEntry const>>` requires ensuring the
key extraction from stored entries is consistent, and the auxiliary data
(AssetPoolIDMap, type ranges) would need separate handling.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete lookup path from `SearchableBucketListSnapshot::getBucketEntry()` → `LiveBucketIndex::lookup()` → `InMemoryIndex::scan()` → `InMemoryBucketState::scan()` → `unordered_set::find(InternalInMemoryBucketEntry(searchKey))`. Confirmed the heap allocation in the `InternalInMemoryBucketEntry(LedgerKey const&)` constructor (line 111-113 of InMemoryIndex.h), which calls `std::make_unique<QueryKey>(ledgerKey)`. The `operator==` at line 37-40 does call `copyKey()` on both sides, producing two temporary `LedgerKey` values per equality comparison. The `ValueEntry::copyKey()` calls `getBucketLedgerKey(*entry)` (types.h:147-160), which constructs a new `LedgerKey` via `LedgerEntryKey(be.liveEntry())`. The inefficiency is real and confirmed.

### Code Paths Examined

- `src/bucket/InMemoryIndex.h:103-113` — `InternalInMemoryBucketEntry` stores `unique_ptr<AbstractEntry> impl`; the `LedgerKey` constructor allocates `make_unique<QueryKey>` on every lookup
- `src/bucket/InMemoryIndex.h:29-41` — `AbstractEntry::operator==()` calls `copyKey()` on both sides, producing two by-value `LedgerKey` temporaries per equality check
- `src/bucket/InMemoryIndex.h:61-64` — `ValueEntry::hash()` calls `getBucketLedgerKey(*entry)` which returns `LedgerKey` by value (one copy per hash of stored entry)
- `src/bucket/InMemoryIndex.cpp:64-76` — `InMemoryBucketState::scan()` constructs the temporary wrapper, calls `mEntries.find()`, destroys it
- `src/bucket/LiveBucketIndex.cpp:236-239` — `lookup()` delegates to `mInMemoryIndex->scan()` for InMemoryIndex buckets
- `src/bucket/BucketListSnapshot.cpp:171-201` — `getBucketEntry()` calls `bucket->getIndex().lookup(k)` for every bucket in the loop
- `src/bucket/BucketListSnapshot.cpp:221-257` — `loadKeysFromBucket()` calls `index.scan()` per key in bulk loads
- `src/bucket/LiveBucketIndex.cpp:29-38` — `getPageSize()` shows cutoff is `BUCKETLIST_DB_INDEX_CUTOFF * 1024 * 1024` bytes; default is 20 MB (Config.cpp:178)
- `src/util/types.h:147-160` — `getBucketLedgerKey()` returns `LedgerKey` by value, involving `LedgerEntryKey()` construction for LIVE/INIT entries

### Findings

The inefficiency is real and well-characterized:

1. **Heap allocation confirmed**: Every `scan()` call constructs `InternalInMemoryBucketEntry(searchKey)` → `make_unique<QueryKey>(searchKey)` → one heap alloc + one `LedgerKey` copy into QueryKey. On destruction, one heap dealloc. Cost: ~40-80ns on modern allocators.

2. **Virtual dispatch confirmed**: `hash()` and `operator==()` are virtual calls through `AbstractEntry*`. The `operator==` body (`copyKey() == other.copyKey()`) creates two temporary `LedgerKey` objects per equality comparison. For Soroban CONTRACT_DATA keys (which contain `SCVal`), this copy is non-trivial.

3. **InMemoryIndex applies to level-0 and possibly level-1 buckets** (those under 20 MB, the default `BUCKETLIST_DB_INDEX_CUTOFF`). Level 0 contains entries from the last 4 ledgers and is always small.

4. **Impact is real but small relative to total benchmark time**: Per-lookup overhead is ~60-150ns. With thousands of lookups per ledger hitting InMemoryIndex buckets, total overhead is ~0.5-1.5ms per ledger. However, total ledger close time in apply-load benchmarks is 50-200ms (dominated by Soroban execution), making this ~0.5-2% of total time — well below the 5% threshold for "Low" severity.

5. **Fix is straightforward and safe**: Replace `unordered_set<InternalInMemoryBucketEntry>` with `unordered_map<LedgerKey, shared_ptr<BucketEntry const>>`. The memory overhead of storing the LedgerKey separately is negligible for small buckets (which is exactly when InMemoryIndex is used). The code comment at line 19-25 already acknowledges this is a C++17 workaround. The `AssetPoolIDMap` and `BucketEntryCounters` are maintained separately and unaffected.

**Severity downgrade from Medium to Informational**: The hypothesis correctly identifies the inefficiency but overestimates its benchmark impact. The "5-10% overhead on the lookup side" is not equivalent to 5-10% of benchmark throughput. BucketList lookups are a small fraction of total ledger close time in Soroban-dominated workloads.

### PoC Guidance

- **Target code**: `src/bucket/InMemoryIndex.h` (replace `InternalInMemoryBucketEntry` and `InMemoryBucketState`), `src/bucket/InMemoryIndex.cpp` (update `insert()` and `scan()`)
- **Change description**: Replace `unordered_set<InternalInMemoryBucketEntry, InternalInMemoryBucketEntryHash>` with `unordered_map<LedgerKey, IndexPtrT>` (where `IndexPtrT = shared_ptr<BucketEntry const>`). Eliminate the entire `InternalInMemoryBucketEntry` class hierarchy. `insert()` becomes `mEntries.emplace(getBucketLedgerKey(be), make_shared<BucketEntry const>(be))`. `scan()` becomes `auto it = mEntries.find(searchKey); return it != end ? IndexReturnT(it->second) : IndexReturnT()`. Iterator interface (`begin()`/`end()`) needs updating to match the new container type.
- **Correctness check**: Existing BucketIndex tests (`BucketIndexTests.cpp`) cover InMemoryIndex lookup, insert, and type range queries. The `[bucket]` test tag runs the full suite. BUILD_TESTS `operator==` on `InMemoryBucketState` and `InMemoryIndex` will need updating for the new container type.
- **Benchmark focus**: Improvement will be micro-level (~0.5-1.5ms per ledger). Unlikely to show measurable change in apply-load benchmark median/p99 times. A micro-benchmark or Tracy profiling comparing per-lookup latency before/after would be more appropriate for validation.
