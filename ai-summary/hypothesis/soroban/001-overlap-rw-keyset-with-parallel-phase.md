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
