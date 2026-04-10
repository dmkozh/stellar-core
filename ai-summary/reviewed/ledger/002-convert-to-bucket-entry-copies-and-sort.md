# H002: `convertToBucketEntry` Deep Copies All Entries + Sorts with Expensive SCVal Comparator

**Date**: 2026-04-10
**Subsystem**: ledger (LiveBucket)
**Severity**: Medium
**Impact**: Eliminate ~3-10ms of unnecessary copying and sorting per ledger close
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `addLiveBatch` prepares level-0 bucket entries via `convertToBucketEntry`
(LiveBucket.cpp:380-420), the function should use move semantics to transfer
`LedgerEntry` objects into `BucketEntry` wrappers rather than deep-copying
them. Similarly, the `mergeInMemory` merge output lambda
(LiveBucket.cpp:585-588) should move entries rather than copying from the
merge algorithm output. Since the entry vectors from `getAllEntries` are
consumed only once (by `addLiveBatch`) and the merge output entries are
accumulated once, move semantics are safe and eliminate redundant deep copies
of large Soroban CONTRACT_DATA and CONTRACT_CODE entries.

## Mechanism

The level-0 bucket update path involves multiple full-entry copies:

**Copy 1 — `convertToBucketEntry` (LiveBucket.cpp:390-410):**
```cpp
for (auto const& e : initEntries)   // const ref prevents move
{
    BucketEntry ce;
    ce.type(useInit ? INITENTRY : LIVEENTRY);
    ce.liveEntry() = e;              // deep copy of LedgerEntry
    bucket.push_back(ce);            // another copy into vector
}
```
Each `LedgerEntry` (200-1000+ bytes for CONTRACT_DATA with SCVal keys) is
copied into a `BucketEntry` wrapper. For ~10K entries, this is 2-10MB of
unnecessary data copying.

**Copy 2 — `mergeInMemory` output (LiveBucket.cpp:585-588):**
```cpp
std::function<void(BucketEntry const&)> putFunc =
    [&mergedEntries](BucketEntry const& entry) {
        mergedEntries.emplace_back(entry);   // copies entry
    };
```
The merge algorithm emits entries via const reference, and the lambda copies
each entry into `mergedEntries`. For old+new ~20K entries, this is another
4-20MB of data copying.

**Sort overhead (LiveBucket.cpp:412-418):**
```cpp
BucketEntryIdCmp<LiveBucket> cmp;
std::sort(bucket.begin(), bucket.end(), cmp);
```
The sort uses `BucketEntryIdCmp` which, for CONTRACT_DATA entries, calls
`LedgerEntryIdCmp::lexCompare` over `SCAddress + SCVal + ContractDataDurability`.
For ~10K Soroban entries (SAC benchmark), this is O(10K × 14) ≈ 140K
comparisons. Each comparison involving `SCVal` (nested XDR union) may cost
100-300ns, totaling 14-42ms for the sort alone.

However, much of the sort time may be dominated by cache misses from the
large entry sizes rather than comparison cost. Move semantics would reduce
the entry size in the vector (by leaving moved-from entries in a
default/empty state), potentially improving cache behavior during sort.

Combined, the copies and sort cost an estimated 3-10ms per ledger close.

## Trigger

Run `scripts/run_apply_load_matrix.py` for the SAC scenario (TX=3200).
Profile `LiveBucket::convertToBucketEntry` and `LiveBucket::mergeInMemory`.
The copy and sort overhead should be visible in Tracy or perf as a
significant fraction of the `addLiveBatch` path.

## Target Code

- `src/bucket/LiveBucket.cpp:convertToBucketEntry:380-420` — copies all entries into BucketEntry wrappers + sorts
- `src/bucket/LiveBucket.cpp:freshInMemoryOnly:467-498` — calls `convertToBucketEntry` (const ref interface prevents moves)
- `src/bucket/LiveBucket.cpp:mergeInMemory:584-588` — merge output lambda copies entries
- `src/bucket/LiveBucket.cpp:mergeInMemory:549-613` — entire merge function with multiple copy points
- `src/bucket/LedgerCmp.h:LedgerEntryIdCmp:90-96` — CONTRACT_DATA comparison involving `lexCompare` on `SCAddress + SCVal`
- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:3043-3044` — calls `addLiveBatch` with const ref vectors

## Evidence

1. `convertToBucketEntry` receives vectors by `const&` (line 381-383) and uses copy assignment `ce.liveEntry() = e` (lines 394, 401, 408). If the interface accepted rvalue references, entries could be moved.
2. The `mergeInMemory` putFunc lambda at line 585-588 takes `BucketEntry const&` and copies via `emplace_back(entry)`. The merge algorithm could be modified to provide move semantics for entries that are consumed (not shadowed).
3. `freshInMemoryOnly` (line 467-498) already keeps entries in memory only (no disk write), but still copies them via `convertToBucketEntry`. The in-memory-only path should especially benefit from move semantics.
4. For the SAC benchmark, the vast majority of entries are CONTRACT_DATA with complex `SCVal` keys. The `LedgerEntryIdCmp` comparator for CONTRACT_DATA does `lexCompare(contract, contract, key, key, durability, durability)` (LedgerCmp.h:92-95), where `key` comparison involves deep `SCVal` traversal.
5. The `std::sort` for ~10K entries with 200-1000 byte elements causes significant data movement. `std::sort` uses introsort which does O(n log n) swaps — each swap moves 200-1000+ bytes of data.
6. H009 (reviewed/ledger/009) already identified that `getAllEntries` copies entries out of the sealed LedgerTxn; this hypothesis extends that observation to the DOWNSTREAM consumer of those entries.

## Anti-Evidence

1. The `addLiveBatch` interface accepts `const&` vectors because the same vectors are also passed to `updateInMemorySorobanState` (line 3045-3046). Changing to move semantics would require processing the in-memory state update BEFORE `addLiveBatch`, or making a copy for one consumer.
2. H008 (reviewed/ledger/008) proposes parallelizing `addLiveBatch` with `updateInMemorySorobanState`. If H008 is implemented, both consumers run concurrently and neither can move from the shared vectors. However, H008 could pass ownership of one copy to each consumer if the vectors are duplicated before parallelization.
3. Modern compilers and CPUs handle sequential memory copies efficiently; the actual cost may be lower than the theoretical 2-10MB estimate due to cache line prefetching and SIMD copy operations.
4. The sort is required by the BucketList merge algorithm (which expects sorted input). An alternative would be to maintain entries in sorted order earlier in the pipeline, but this would complicate the LedgerTxn extraction path.
5. If the total `addLiveBatch` time is only 5-15ms out of ~100-200ms total ledger close, this is a 2.5-15% improvement — at the boundary of Medium severity.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete level-0 bucket update path from `finalizeLedgerTxnChanges` (LedgerManagerImpl.cpp:3043) → `addLiveBatch` (BucketManager.cpp:1026) → `addBatchInternal` (BucketListBase.cpp:684) → `prepareFirstLevel` (BucketListBase.cpp:229) → `freshInMemoryOnly` (LiveBucket.cpp:467) → `convertToBucketEntry` (LiveBucket.cpp:379). Confirmed the double-copy pattern (copy into local `BucketEntry ce`, then copy into vector via `push_back`). Also traced the `mergeInMemory` path (LiveBucket.cpp:549) confirming the `putFunc` lambda copies entries from `MemoryMergeInput` (BucketMergeAdapter.h:106) which holds `const&` to input vectors. However, the hypothesis significantly overestimates the sort comparison cost and mischaracterizes `std::sort`'s swap behavior.

### Code Paths Examined

- `src/bucket/LiveBucket.cpp:379-420` — `convertToBucketEntry`: confirmed double-copy pattern. Line 394 `ce.liveEntry() = e` copies from const ref (unavoidable without interface change). Line 395 `bucket.push_back(ce)` copies named local into vector (avoidable with `std::move(ce)` or direct `emplace_back` construction).
- `src/bucket/LiveBucket.cpp:467-498` — `freshInMemoryOnly`: takes `const&` vectors, calls `convertToBucketEntry`, then moves the result vector into the new bucket at line 497 via `std::move(entries)`.
- `src/bucket/LiveBucket.cpp:549-613` — `mergeInMemory`: `MemoryMergeInput` at line 584 holds `const&` to old/new entry vectors. `putFunc` at line 585-588 takes `BucketEntry const&`, copies via `emplace_back`. Changing this requires modifying the merge algorithm's `putFunc` interface or changing `MemoryMergeInput` to support move-from-source semantics — significant refactoring.
- `src/bucket/BucketMergeAdapter.h:106-146` — `MemoryMergeInput<LiveBucket>`: stores `const&` to entry vectors. All accessors (`getOldEntry`, `getNewEntry`) return `const&`. Move semantics would require non-const storage and tracking of consumed entries.
- `src/bucket/LedgerCmp.h:90-96` — `LedgerEntryIdCmp` for CONTRACT_DATA: calls `lexCompare(contract, contract, key, key, durability, durability)`. For SAC entries (all same contract), `SCAddress` comparisons always tie (equal addresses), requiring fallthrough to `SCVal` key comparison every time.
- `lib/xdrpp/xdrpp/types.h:1012-1027` — XDR union `operator<=>`: compares discriminant first, then dispatches to active field via `_xdr_with_mem_ptr`. For SAC balance keys (e.g. SCV_ADDRESS or SCV_I128 variants), the inner comparison is ~20-40ns, not 100-300ns as claimed.
- `src/ledger/LedgerManagerImpl.cpp:3039-3046` — `finalizeLedgerTxnChanges`: confirmed `initEntries`/`liveEntries`/`deadEntries` are consumed by four operations in sequence (`addAnyContractsToModuleCache` ×2, `addLiveBatch`, `updateInMemorySorobanState`), all taking `const&`. Moving from input vectors requires reordering consumers and ensuring `addLiveBatch` runs last.

### Findings

The hypothesis identifies **real but overstated inefficiencies**:

1. **Double-copy in `convertToBucketEntry` is real and trivially fixable.** Each entry is copied into a local `BucketEntry ce`, then copied again via `bucket.push_back(ce)`. Replacing with `bucket.push_back(std::move(ce))` or using `auto& be = bucket.emplace_back(); be.type(...); be.liveEntry() = e;` eliminates the second copy. For ~10K entries at ~200 bytes (SAC balance entries), this saves ~2MB of memcpy → approximately 0.3-0.5ms at modern memory bandwidth.

2. **First copy (`ce.liveEntry() = e`) is NOT easily avoidable.** The input vectors are shared with `updateInMemorySorobanState` and `addAnyContractsToModuleCache`. Moving from them requires reordering `finalizeLedgerTxnChanges` so `addLiveBatch` runs last, then changing signatures across `BucketManager::addLiveBatch` → `LiveBucketList::addBatch` → `BucketLevel::prepareFirstLevel` → `LiveBucket::freshInMemoryOnly` → `convertToBucketEntry` to accept rvalue-ref vectors. This is a non-trivial interface change for ~0.5-1ms additional savings.

3. **Sort comparison cost is overestimated.** The hypothesis claims 100-300ns per comparison; actual cost for SAC entries is ~50-80ns (SCAddress equality check ~20ns via 32-byte memcmp, SCVal key comparison ~20-40ns for simple variants like SCV_ADDRESS/SCV_I128, durability enum ~1ns). For 140K comparisons: ~7-11ms, not 14-42ms. Furthermore, `std::sort` already uses move semantics for element swaps (since C++11), so the data movement during sort is already O(pointer-swaps), not O(entry-size) copies as implied.

4. **`mergeInMemory` copy requires merge algorithm refactoring.** The `putFunc` interface uses `BucketEntry const&` because it's shared between `MemoryMergeInput` and `FileMergeInput` paths. Supporting moves requires either a separate code path for memory merges or a new `putFunc` overload that accepts `BucketEntry&&`. The merged entries vector receives ~10-20K entries, so the copy cost is ~1-3ms. This is the largest single optimization in the hypothesis but the hardest to implement.

5. **Severity downgrade from Medium to Informational:** The easy fix (eliminate `push_back` copy) saves ~0.3-0.5ms. The moderate fix (move from input vectors) saves ~0.5-1ms more. The hard fix (merge algorithm moves) saves ~1-3ms more. Total potential: ~2-4.5ms on a ~100-200ms ledger close = 1-4.5%. None of the individual fixes exceed 5% improvement, and even all combined fall short of the 5% threshold for Low severity.

### PoC Guidance

- **Target code**: `src/bucket/LiveBucket.cpp`, function `convertToBucketEntry` (lines 390-410)
- **Change description**: Replace the three loops' pattern of `BucketEntry ce; ce.type(...); ce.XXXEntry() = e; bucket.push_back(ce);` with either (a) `bucket.push_back(std::move(ce))` after each assignment, or (b) `auto& be = bucket.emplace_back(); be.type(...); be.XXXEntry() = e;` to construct directly in the vector. Option (b) is cleaner — it eliminates both the temporary and the copy into the vector.
- **Correctness check**: Existing bucket tests under `[bucket]` tag cover `convertToBucketEntry` via `freshInMemoryOnly` and `mergeInMemory`. The `BucketListTests.cpp` and `BucketListIsConsistentWithDatabaseTests.cpp` test suites exercise the full addBatch/merge path.
- **Benchmark focus**: Run SAC apply-load at T=1. Measure `bucket.addLiveBatch` timer metric before/after. Expected improvement: 0.3-0.5ms reduction from the `push_back` fix alone (<1% of total close time). Profile `convertToBucketEntry` and `mergeInMemory` separately to distinguish copy vs. sort vs. merge costs.
