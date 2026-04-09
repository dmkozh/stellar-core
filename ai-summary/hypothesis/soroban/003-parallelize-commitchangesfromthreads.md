# H003: Sequential commitChangesFromThreads Is a Serial Bottleneck Between Stages

**Date**: 2026-04-09
**Subsystem**: soroban (ledger parallel apply)
**Severity**: Medium
**Impact**: Parallelization / T=8 throughput
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

After parallel threads complete a stage's clusters, the merge-back of thread
results into the global state should be as fast as possible, since it runs
serially on the apply thread and blocks the start of the next stage. Ideally,
thread state commit should either be parallelized or minimized in scope.

## Mechanism

`GlobalParallelApplyLedgerState::commitChangesFromThreads()` (ParallelApplyUtils.cpp:
546-559) runs after all parallel threads complete. It iterates every thread's
`mThreadEntryMap` sequentially, calling `commitChangeFromThread()` for each
key-value pair. This involves:

1. **`getReadWriteKeysForStage()`** (line 555): Iterates ALL transactions in the
   stage to build a `std::unordered_set<LedgerKey>` of RW keys. This allocates
   and populates a hash set proportional to the total RW footprint of the entire
   stage.

2. **Per-thread iteration** (lines 556-558): For each thread, iterates its
   entire `mThreadEntryMap`, calling `commitChangeFromThread()` which does
   map lookups and optional RO TTL merges.

3. **`checkAllTxBundleInvariants()`** (called at line 2529 in
   `applySorobanStage` before `commitChangesFromThreads`): Iterates ALL
   transactions in the stage, calling `setEffectsDeltaFromSuccessfulTx()` for
   each successful tx. This calls `getLiveEntryOpt()` for EVERY modified key
   to fetch the pre-state, creating `shared_ptr<InternalLedgerEntry>` copies.

The sequential work between stages is: `checkAllTxBundleInvariants` +
`commitChangesFromThreads` + `getReadWriteKeysForStage`. For a stage with 8
clusters × ~12 txs each = ~96 txs, each touching ~6 keys, this processes
~576 entries sequentially with hash map operations and shared_ptr copies.

Estimated sequential overhead: ~100-500µs per stage depending on footprint
sizes. With 2-4 stages, this is ~200-2000µs of serial work between parallel
phases. Against a 50-100ms ledger, this is 0.2-4%.

The key optimization is that `getReadWriteKeysForStage()` rebuilds the RW key
set from scratch each time. This set could be precomputed once during
`applyParallelPhase` construction and reused. Additionally,
`checkAllTxBundleInvariants` could be partially parallelized (the per-tx delta
computation is independent).

## Trigger

Run `scripts/run_apply_load_matrix.py` with T=8 scenarios. Profile
`commitChangesFromThreads`, `checkAllTxBundleInvariants`, and
`getReadWriteKeysForStage`. If the hypothesis is correct, these sequential
sections between stages will show as a measurable fraction of total apply time,
especially visible in traces showing idle parallel threads.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:commitChangesFromThreads:546-559` — sequential merge of all thread results
- `src/transactions/ParallelApplyUtils.cpp:getReadWriteKeysForStage:100-118` — rebuilds RW key set per stage
- `src/transactions/ParallelApplyUtils.cpp:setEffectsDeltaFromSuccessfulTx:790-829` — re-fetches pre-state for all modified keys
- `src/ledger/LedgerManagerImpl.cpp:applySorobanStage:2517-2532` — sequential: clusters → invariants → commit
- `src/ledger/LedgerManagerImpl.cpp:checkAllTxBundleInvariants:2474-2513` — sequential per-tx invariant checks

## Evidence

1. `commitChangesFromThreads` is called with `ZoneScoped`, indicating it's already recognized as a profiling hotspot.
2. `getReadWriteKeysForStage` rebuilds an `unordered_set` by iterating all txs in the stage. This is O(total_RW_footprint) work that could be precomputed.
3. `setEffectsDeltaFromSuccessfulTx` calls `getLiveEntryOpt(lk)` for every modified key, which traverses `mThreadEntryMap → mInMemorySorobanState → mLCLSnapshot`. Each call potentially does 1-3 hash map lookups and a deep copy.
4. The sequential section blocks the start of the next stage's parallel execution.

## Anti-Evidence

1. For simple benchmarks with only 1 stage, there's no inter-stage serial
   bottleneck — only the final commit matters.
2. `checkAllTxBundleInvariants` is needed for correctness; parallelizing it
   requires ensuring thread-safe access to effects data.
3. The `getReadWriteKeysForStage` cost is O(N) in total RW keys, which is
   modest for typical footprints (~6 keys/tx × 96 txs = ~576 keys). Hash set
   construction for 576 keys is ~10-20µs.
4. The dominant cost is likely the per-tx `setEffectsDeltaFromSuccessfulTx`
   which creates `shared_ptr<InternalLedgerEntry>` copies. Optimizing this
   requires changing how deltas are represented.
