# H002: Bulk bucket loads clone and tear down the unresolved-key tree on every prefetch

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: prefetch CPU and allocator pressure
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Bulk bucket lookups should consume a pre-sorted key batch with a cheap
forward-only data structure. The hot `loadLiveKeys()` path should not need to
clone the entire key set and then individually erase found keys from a
red-black tree when the algorithm only ever walks keys in order.

## Mechanism

`loadKeysInternal()` copies the caller's full `std::set<LedgerKey>` into a new
tree (`auto keys = inKeys`), and `loadKeysFromBucket()` erases each resolved key
one-by-one as it finds matches. Apply-load calls `ltx.prefetch()` for fee-source
accounts and tx-apply keys every ledger; even in Soroban-heavy scenarios that
guarantees large classic batches of source-account keys before transaction
execution begins. The result is deterministic allocator churn — thousands of
tree-node allocations and frees per ledger — before the bucket code has even
started the actual indexed reads.

## Trigger

Run the stock apply-load matrix with default prefetch enabled and bucket-backed
state (`!allBucketsInMemory()`). Every ledger's
`prefetchTxSourceIds`/`prefetchTransactionData` path builds a large key set and
feeds it into `loadLiveKeys()`, with the strongest effect in the
`sac,TX=3200` scenarios.

## Target Code

- `src/bucket/BucketListSnapshot.cpp:loadKeysFromBucket:203-277` — resolves keys by erasing them from the mutable `std::set`
- `src/bucket/BucketListSnapshot.cpp:loadKeysInternal:348-381` — clones the input key set before every bulk lookup
- `src/bucket/BucketListSnapshot.h:130-138` — API shape requires a `std::set` and destructive internal copy
- `src/ledger/LedgerTxn.cpp:LedgerTxnRoot::Impl::prefetch:3045-3097` — materializes a `LedgerKeySet` and always calls `loadLiveKeys()` for uncached classic keys
- `src/ledger/LedgerManagerImpl.cpp:prefetchTxSourceIds/prefetchTransactionData:2340-2376` — invokes prefetch on every ledger before apply

## Evidence

The bucket bulk-load algorithm is already strictly forward-only: inside a bucket,
it keeps `currKeyIt` and `indexIter` moving monotonically forward. Nothing in
the search requires tree insertion or arbitrary deletion; the mutable `std::set`
exists only so found keys can be erased and shadowed keys skipped in later
buckets. That makes the full-tree copy in `loadKeysInternal()` and per-hit
`keys.erase(currKeyIt)` pure bookkeeping overhead that scales with batch size.

## Anti-Evidence

If the entry cache is hot or the process is configured with all buckets in
memory, fewer keys reach `loadLiveKeys()`. A replacement data structure also has
to preserve the shadow-elision behavior across buckets, so the fix is more than
just swapping `std::set` for `std::vector`.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete path from `prefetchTxSourceIds`/`prefetchTransactionData` → `LedgerTxnRoot::Impl::prefetch` → `LedgerStateSnapshot::loadLiveKeys` → `SearchableLiveBucketListSnapshot::loadKeys` → `loadKeysInternal` → `loadKeysFromBucket`. Confirmed the `std::set` copy at line 358 (`auto keys = inKeys`) creates O(n) heap-allocated tree nodes, and `keys.erase(currKeyIt)` at line 272 deallocates one node per found key. The algorithm is strictly forward-only — `currKeyIt` and `indexIter` both advance monotonically — so a sorted vector with a boolean mask would eliminate all tree-node allocations.

### Code Paths Examined

- `src/bucket/BucketListSnapshot.cpp:loadKeysInternal:350-382` — Line 358 copies the entire `std::set<LedgerKey, LedgerEntryIdCmp>` via `auto keys = inKeys`. This allocates one red-black tree node per key (each requiring a heap allocation containing the `LedgerKey` plus tree metadata).
- `src/bucket/BucketListSnapshot.cpp:loadKeysFromBucket:210-277` — Line 272 `keys.erase(currKeyIt)` deallocates one tree node per found key and rebalances the tree. Lines 221-228 confirm strictly forward-only iteration: `currKeyIt` starts at `keys.begin()` and only advances via `++currKeyIt` or `erase`.
- `src/ledger/LedgerTxn.cpp:LedgerTxnRoot::Impl::prefetch:3045-3100` — Lines 3083-3094 filter out cached keys into a `LedgerKeySet keysToSearch`, then line 3096 calls `loadLiveKeys(keysToSearch, "prefetch")`. The entry cache (`mEntryCache.exists(key, false)`) reduces the set size after warmup.
- `src/ledger/LedgerManagerImpl.cpp:prefetchTxSourceIds:2340-2357` — Collects one ACCOUNT key per unique transaction source. For `sac,TX=3200` that's up to ~3200 keys before cache filtering.
- `src/ledger/LedgerManagerImpl.cpp:prefetchTransactionData:2360-2377` — Collects apply-time keys via `insertKeysForTxApply`. Also up to ~3200 keys before cache filtering.
- `src/ledger/LedgerStateSnapshot.cpp:loadLiveKeys:445-450` — Pass-through to `SearchableLiveBucketListSnapshot::loadKeys`.
- `src/bucket/BucketListSnapshot.cpp:loadKeys:446-453` — Wraps `loadKeysInternal` with a timer.

### Findings

The inefficiency is confirmed: `loadKeysInternal` performs a full red-black tree copy, and `loadKeysFromBucket` destructively erases found keys from that copy. Since the algorithm is forward-only, a sorted `std::vector<LedgerKey>` with a parallel `std::vector<bool>` (or equivalent marker) would replace O(n) tree-node allocations with a single contiguous allocation + memcpy, and replace O(log n) erase-with-rebalance per found key with O(1) flag-set.

**Severity downgrade rationale (Low → Informational):**

1. **Entry cache reduces exposure after warmup.** The `LedgerTxnRoot::Impl::prefetch` function (line 3084) skips keys already in `mEntryCache`. After the first ledger in apply-load, source accounts from repeated transactions are likely cached. The set reaching `loadKeys` shrinks significantly.

2. **Absolute cost is small relative to ledger close time.** Even with a worst-case 3200-key set copy + erase (no cache filtering), the cost is ~0.5–1ms on modern hardware (3200 malloc/free cycles + tree rebalancing). Ledger close times in the apply-load benchmarks are 200–400ms, placing this overhead at ~0.1–0.5% of total.

3. **Precedent from fail/bucket/006.** A more impactful optimization in the same `loadKeysFromBucket` function — eliminating redundant 16KB page reads for same-page keys — was downgraded to Informational by the reviewer and ultimately REJECTED at final review because benchmarks showed no net improvement. The set copy/erase overhead is strictly less impactful than redundant I/O syscalls + 16KB memcpy.

4. **Prefetch executes only twice per ledger.** Unlike per-transaction operations, the two prefetch calls (source IDs + transaction data) produce a fixed per-ledger cost that does not scale with parallelism.

### PoC Guidance

- **Target code**: `src/bucket/BucketListSnapshot.cpp:loadKeysInternal` (line 358) and `src/bucket/BucketListSnapshot.cpp:loadKeysFromBucket` (lines 210–277); `src/bucket/BucketListSnapshot.h` (lines 130–138) for the function signature.
- **Change description**: Replace the `std::set<LedgerKey, LedgerEntryIdCmp>` working copy with a `std::vector<LedgerKey>` (sorted, copied from the input set via range construction) plus a `std::vector<bool> found` bitmap. In `loadKeysFromBucket`, iterate with an index that skips `found[i] == true` entries. When a key is found, set `found[i] = true` instead of erasing. In `loadKeysInternal`, check `std::all_of(found.begin(), found.end(), identity)` for early termination instead of `keys.empty()`. The `loadKeysFromBucket` signature changes from `std::set<LedgerKey>&` to something like `(std::vector<LedgerKey> const&, std::vector<bool>&, ...)`.
- **Correctness check**: Existing tests in `src/bucket/test/BucketIndexTests.cpp` (`[bucketindex]` tag) and `src/bucket/test/BucketListTests.cpp` (`[bucket]` tag) cover bulk loads. The `[bucket]` test suite should pass unchanged.
- **Benchmark focus**: Measure total `loadKeys` time via the existing medida timer (`getBulkLoadTimer`). Expect modest allocator-pressure reduction but no measurable improvement (< 1%) on apply-load benchmark scenarios. The strongest signal would come from `sac,TX=3200,T=1` with a cold entry cache, but even that scenario's prefetch phase is a small fraction of total close time.
