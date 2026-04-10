# H018: Parallelize Per-TX Output Processing (recordStorageChanges + collectEvents + finalizeSuccess)

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Medium
**Impact**: CPU / parallelization
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

After `invoke_host_function` returns from Rust, the C++ output processing
(recordStorageChanges, collectEvents, finalizeSuccess) could be overlapped
with the next TX's input preparation or Rust invocation, using a pipeline
or producer-consumer pattern to utilize CPU cores that would otherwise be idle.

## Mechanism

Within a cluster, TXs are processed serially in `applyThread()`:
```cpp
for (auto const& txBundle : cluster) {
    auto res = txBundle.getTx()->parallelApply(...);
    if (res) {
        threadState->commitChangesFromSuccessfulTx(*res, txBundle);
    }
}
```

Each TX's `parallelApply()` calls: `addReads()` → `invokeHostFunction()` (FFI
to Rust) → `recordStorageChanges()` → `collectEvents()` →
`consumeRefundableResources()` → `finalizeSuccess()`.

If the output processing for TX_N could overlap with the input preparation
for TX_N+1, we'd hide the output processing latency. Estimated per-TX output
processing: ~3-8μs (recordStorageChanges ~2-5μs, collectEvents ~0.5-1.5μs,
finalizeSuccess ~1-2μs).

## Trigger

Run the apply-load benchmark with T=8 (multi-threaded). Each thread processes
a cluster serially, so pipelining within a thread could improve per-thread
throughput.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2386-2412` — `applyThread` serial TX loop
- `src/transactions/InvokeHostFunctionOpFrame.cpp:610-829` — output processing methods
- `src/transactions/ParallelApplyUtils.cpp:832-843` — `commitChangesFromSuccessfulTx`

## Evidence

The output processing methods (recordStorageChanges, collectEvents,
finalizeSuccess) are CPU-bound (XDR decode, hash computation, map operations)
and don't depend on the next TX's input data. In principle, they could run
on a separate thread.

## Anti-Evidence

1. `recordStorageChanges` calls `upsertLedgerEntry` which modifies the
   thread-local `mThreadEntryMap`. The next TX's `addReads` calls
   `getLedgerEntryOpt` which reads from the SAME `mThreadEntryMap`. These are
   data-dependent: TX_N+1's reads may depend on TX_N's writes (that's why
   they're in the same cluster — they have readWrite footprint overlap).
2. `commitChangesFromSuccessfulTx` updates the thread state that subsequent
   TXs read from. This creates a strict ordering dependency.
3. Even `collectEvents` modifies `mMetrics` which is used for resource tracking
   across TXs.
4. The output processing (~3-8μs) is small relative to the Rust host execution
   (~50-150μs per TX). Hiding ~5% of the per-TX time through pipelining would
   yield ~5% × (3-8μs / 130-200μs) ≈ ~1-4% improvement per thread.
5. Threading overhead (synchronization, cache line bouncing) would likely
   consume much of the savings for such a short pipeline stage.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The fundamental blocker is data dependency: within a cluster, TX_N+1's
`addReads()` reads from `mThreadEntryMap` which is modified by TX_N's
`recordStorageChanges()` and `commitChangesFromSuccessfulTx()`. These TXs
are in the same cluster BECAUSE they have overlapping readWrite footprints.
The next TX cannot begin input preparation until the previous TX's output
has been committed to the shared thread state.

Even if we could isolate the non-state-modifying parts (like collectEvents
and hash computation), the pipeline stage would be ~1-2μs, which is too short
to justify the synchronization overhead of a producer-consumer queue.

### Lesson Learned

Within-cluster TX ordering is semantically required due to readWrite footprint
overlap. Pipelining within a cluster is fundamentally limited by these data
dependencies. The only way to increase intra-cluster parallelism would be to
break TXs into finer-grained independent stages, which would require
significant architectural changes to the apply framework. Cross-cluster
parallelism (which already exists via std::async per cluster) is the correct
level of parallelization for the current design.
