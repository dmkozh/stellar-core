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
