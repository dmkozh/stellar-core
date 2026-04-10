# H001: Overlap readWrite Key Set Construction With Parallel Phase

**Date**: 2025-07-14
**Subsystem**: soroban, ledger
**Severity**: Low-Medium
**Impact**: Serial bottleneck reduction for T=8 parallel apply
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

After `applySorobanStageClustersInParallel` completes the parallel phase,
`commitChangesFromThreads` should merge thread results into the global map
with minimal serial overhead. The `readWriteSet` needed for TTL merge
decisions should already be available, having been computed during idle time
while threads were running.

## Mechanism

`getReadWriteKeysForStage` (ParallelApplyUtils.cpp:100-116) rebuilds an
`unordered_set<LedgerKey>` from scratch at the start of
`commitChangesFromThreads`. This iterates every TX in the stage, collecting
all read-write keys and their TTL counterparts. For 3200 SAC TXs × ~5 RW
keys each = ~16,000 keys, plus ~16,000 TTL keys = ~32,000 hash set
insertions. Each `CONTRACT_DATA` key insertion requires
`shortHash::xdrComputeHash` (LedgerHashUtils.h:178-184) which serializes
the `SCVal` key to XDR and computes SipHash over it.

This serial work happens AFTER the parallel phase completes — blocking the
pipeline. But the set only depends on the `ApplyStage` definition (the TX
footprints), which is immutable and available before the parallel phase
begins. The computation can be moved to overlap with the parallel phase by
starting it on the apply thread (or a dedicated async task) while worker
threads execute transactions.

In `applySorobanStage` (LedgerManagerImpl.cpp:2517-2532):
```cpp
auto threadStates = applySorobanStageClustersInParallel(...); // parallel phase
checkAllTxBundleInvariants(...);                               // serial
globalParState.commitChangesFromThreads(app, threadStates, stage); // serial, builds RW set
```

The RW set could be computed during the parallel phase (or precomputed once
per stage before launching threads) and passed to `commitChangesFromThreads`.

## Trigger

Run the apply-load benchmark with T=8 (8 clusters, 8 threads) and 3200 SAC
transactions. Profile the serial gap between parallel phase completion and
`commitChangesFromThreads` finishing. The RW key set construction shows up
as `getReadWriteKeysForStage` in profiling.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:getReadWriteKeysForStage:100-116` — builds the expensive hash set
- `src/transactions/ParallelApplyUtils.cpp:commitChangesFromThreads:546-560` — calls getReadWriteKeysForStage on the serial path
- `src/ledger/LedgerManagerImpl.cpp:applySorobanStage:2517-2532` — orchestration point where overlap could be introduced
- `src/ledger/LedgerHashUtils.h:hash<LedgerKey>::operator():136-202` — CONTRACT_DATA hash includes xdrComputeHash

## Evidence

- `getReadWriteKeysForStage` iterates ALL TXs in the stage, computing expensive hashes for CONTRACT_DATA keys via `xdrComputeHash`
- The function is called on the serial path (apply thread) after the parallel phase completes
- The stage object is immutable and available before the parallel phase starts
- With 32,000 key insertions, each requiring LedgerKey hashing (including SCVal serialization for CONTRACT_DATA), estimated cost is 4-12ms per stage
- This is pure serial overhead that directly reduces T=8 parallelism efficiency

## Anti-Evidence

- If stages have few TXs (e.g., soroswap with 1000 TXs), the set is smaller and the cost is proportionally lower (~1-4ms)
- The actual time depends on SCVal key complexity — simple keys (Symbol) are cheap to hash
- Adding async computation introduces complexity and potential lifetime issues with the stage reference
- A simpler alternative (precompute before launching threads) adds latency to thread launch but avoids concurrency complexity

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — substantially overlaps with 007-parallelize-commitchangesfromthreads
**Failed At**: reviewer

### Trace Summary

Traced `getReadWriteKeysForStage` (ParallelApplyUtils.cpp:99-118) and the SAC transfer TX footprint generation (TxGenerator.cpp:738-812). The hypothesis incorrectly claims "~5 RW keys per TX" and "32,000 hash set insertions". Each SAC transfer TX has only 2 RW keys (1 ACCOUNT + 1 CONTRACT_DATA balance), yielding ~9,600 set insertions (3200 ACCOUNT + 3200 CONTRACT_DATA + 3200 TTL). The `xdrComputeHash` for CONTRACT_DATA keys is a streaming SipHash24 over ~70 bytes (no allocation), costing ~50-100ns per call. Total function cost is ~300-600µs, not the claimed 4-12ms. This serial overhead is <1% of a 50ms ledger close — far below the 5% threshold for Low severity.

### Code Paths Examined

- `ParallelApplyUtils.cpp:getReadWriteKeysForStage:99-118` — iterates all TXs in stage, inserts RW keys + TTL counterparts into unordered_set
- `TxGenerator.cpp:invokeSACPayment:738-812` — each SAC TX has 2 RW keys: 1 ACCOUNT key + 1 CONTRACT_DATA (balance) key, not ~5 as claimed
- `LedgerHashUtils.h:hash<LedgerKey>::operator():136-202` — CONTRACT_DATA hashing uses streaming `xdrComputeHash` (SipHash24, no allocation), ACCOUNT and TTL hashing is just `hash<uint256>`
- `ShortHash.h:xdrComputeHash:49-55` — streams XDR archive through `SipHash24` without allocating intermediate buffer; cost ~50-100ns for a small SCVal
- `LedgerManagerImpl.cpp:applySorobanStage:2517-2532` — confirmed serial sequence: parallel phase → invariant checks → commitChangesFromThreads (calls getReadWriteKeysForStage)
- `ApplyLoad.cpp:2018-2023` — benchmark asserts 1 stage per ledger, so getReadWriteKeysForStage is called once

### Why It Failed

1. **Cost estimates are inflated 10-20×**: The hypothesis claims 32,000 insertions at 4-12ms. Actual: ~9,600 insertions at ~300-600µs. Each SAC TX has 2 RW keys (not ~5), and `xdrComputeHash` is a streaming SipHash (not XDR serialization to a buffer).

2. **Substantially duplicates rejected H007**: Fail file `007-parallelize-commitchangesfromthreads.md` already analyzed this exact serial section (`getReadWriteKeysForStage` + `commitChangesFromThreads`) and concluded the serial overhead is ~50-200µs per stage (<0.5% of ledger time). While H007 analyzed a smaller TX count (~96 TXs), the conclusion that this serial section is lightweight extends to 3200 TXs — the cost scales linearly and remains sub-millisecond.

3. **Below measurability threshold**: Even with the most generous estimate of ~600µs saved, this is ~1% of a 50ms ledger. The benchmark noise floor makes sub-1% improvements unmeasurable, and it's well below the 5% threshold for Low severity.

### Lesson Learned

When estimating per-TX key counts for footprint-related cost analysis, inspect the actual TX generation code (e.g., `TxGenerator::invokeSACPayment`) rather than guessing. SAC transfers have only 2 RW keys, not 5. Also, `xdrComputeHash` uses streaming SipHash without allocation — it is far cheaper than serializing to a buffer and hashing.
