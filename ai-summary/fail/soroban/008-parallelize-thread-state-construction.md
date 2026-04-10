# H008: Parallelize Thread State Construction

**Date**: 2025-07-14
**Subsystem**: soroban, ledger
**Severity**: Low
**Impact**: Serial bottleneck reduction for T=8 parallel apply
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`ThreadParallelApplyLedgerState` construction should be parallelizable
since it only reads from the shared global map. Moving construction
inside the `std::async` lambda (alongside `applyThread`) would let
multiple thread states be built concurrently instead of serially.

## Mechanism

In `applySorobanStageClustersInParallel` (LedgerManagerImpl.cpp:2441-2449),
thread states are constructed serially on the apply thread before launching
each async task. Each constructor calls
`collectClusterFootprintEntriesFromGlobal` (ParallelApplyUtils.cpp:563-608)
which iterates all footprint keys in the cluster (~4000 keys for 400 TXs)
and looks them up in the global map via hash map find operations.

With 8 clusters, this is 8 × ~4000 hash map lookups done serially, totaling
~32,000 lookups at ~50ns each = ~1.6ms of serial setup time.

## Trigger

Profile `applySorobanStageClustersInParallel` with T=8 to measure the gap
between function entry and the first async thread launch.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:applySorobanStageClustersInParallel:2441-2449` — serial construction loop
- `src/transactions/ParallelApplyUtils.cpp:collectClusterFootprintEntriesFromGlobal:563-608` — per-cluster setup
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState:610-623` — constructor

## Evidence

- Thread states are constructed sequentially in a loop before launching async tasks
- `collectClusterFootprintEntriesFromGlobal` only READS from the global map (no writes)
- The global map is not modified during construction, so concurrent reads are safe
- With 8 clusters, serializing construction adds ~1.6ms before any thread starts work

## Anti-Evidence

- `collectClusterFootprintEntriesFromGlobal` has an assertion: `threadIsMain() || threadIsType(APPLY)`
- Async worker threads spawned by `std::async` are NOT registered in `mThreadTypes` (ApplicationImpl.cpp:201-206)
- Calling `threadIsType` on an unregistered thread triggers `releaseAssert(it != mThreadTypes.end())` — a crash
- Registering async threads requires synchronizing access to `mThreadTypes` map and adds significant complexity
- `scopeAdoptEntryOptFrom` calls during construction involve `LedgerEntryScope` operations that may have thread-safety requirements
- Savings (~1.6ms) are modest compared to implementation complexity

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-14
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The thread type assertion system (`threadIsMain() || threadIsType(APPLY)`) in
`collectClusterFootprintEntriesFromGlobal` prevents running this code on
async worker threads. The thread type registry (`mThreadTypes`) only tracks
named threads (main, overlay, apply), not ad-hoc `std::async` threads.
Calling `threadIsType` on an unregistered thread crashes via `releaseAssert`.
Fixing this requires either relaxing the assertion (risky — it exists to
catch incorrect thread usage) or adding thread registration for async
workers (complex, requires synchronization).

### Lesson Learned

The thread type assertion system (`Application::threadIsType`) prevents
running certain state management code on unregistered threads. Any
parallelization hypothesis that moves code to async worker threads must
verify the target code doesn't have thread type assertions.
