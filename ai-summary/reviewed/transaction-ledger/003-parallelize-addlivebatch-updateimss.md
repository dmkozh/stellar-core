# H003: Parallelize addLiveBatch and updateInMemorySorobanState

**Date**: 2025-07-14
**Subsystem**: transaction-ledger
**Severity**: Low
**Impact**: 4-6% reduction across T=8 scenarios
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

After `getAllEntries` produces the ledger's modified entries, the two major
consumers — `addLiveBatch` (bucket list update) and `updateInMemorySorobanState`
(in-memory state cache update) — should execute in parallel since they are
independent operations on separate data structures. The total wall time should
be `max(addLiveBatch, updateInMemorySorobanState)` instead of their sum.

## Mechanism

In `LedgerManagerImpl::finalizeLedgerTxnChanges` (lines 2952-3058), after
`getAllEntries` populates the entry vectors, the code executes sequentially:

```cpp
ltx.getAllEntries(initEntries, liveEntries, deadEntries);
mApplyState.addAnyContractsToModuleCache(lh.ledgerVersion, initEntries);
mApplyState.addAnyContractsToModuleCache(lh.ledgerVersion, liveEntries);
mApp.getBucketManager().addLiveBatch(app, lh, initEntries, liveEntries, deadEntries);
mApplyState.updateInMemorySorobanState(initEntries, liveEntries, deadEntries, ...);
```

`addLiveBatch` and `updateInMemorySorobanState` are fully independent:
- `addLiveBatch` modifies `BucketListBase` level 0 (sort, merge, serialize,
  write to disk). Estimated ~25ms per ledger.
- `updateInMemorySorobanState` modifies `InMemorySorobanState` hash tables
  (insert/update/delete contract data + TTL entries). Estimated ~28ms per ledger.
- Both take `const&` to the entry vectors — read-only access to shared data.
- They access completely different data structures with no shared mutable state.

Running them serially costs ~53ms. Running them in parallel costs ~28ms
(the slower of the two). Savings: **~25ms** = **4.1% of 612ms** (SAC T=8).

For custom_token T=8 (430ms): ~25ms savings = **5.8%**.
For soroswap T=8 (401ms): ~25ms savings = **6.2%**.

The parallelization is safe because:
1. `addLiveBatch` only accesses `BucketManager` and `BucketListBase`
2. `updateInMemorySorobanState` only accesses `InMemorySorobanState`
3. `addAnyContractsToModuleCache` runs before both (writes module cache)
4. Entry vectors are read-only (const reference parameters)
5. No shared mutable state between the two operations

## Trigger

Run apply-load benchmark with any scenario at T=8. Profile
`finalizeLedgerTxnChanges` to measure time spent in `addLiveBatch` vs
`updateInMemorySorobanState`. The two should show up as sequential blocks
totaling ~50-55ms, with no overlap.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:2952-3058` —
  the serial execution of addLiveBatch then updateInMemorySorobanState
- `src/bucket/BucketListBase.cpp:addBatchInternal:684-797` — bucket list
  update (sort + merge + disk write)
- `src/ledger/InMemorySorobanState.cpp:updateState:536-602` — in-memory
  state update (hash table operations)
- `src/bucket/LiveBucket.cpp:prepareFirstLevel:196-238` — level-0 merge
  (freshInMemoryOnly + mergeInMemory)

## Evidence

1. `addLiveBatch` signature takes all entry vectors by `const&` (see
   `BucketManager::addLiveBatch` declaration). Same for
   `updateInMemorySorobanState` in `InMemorySorobanState::updateState`.
2. The two functions access entirely different subsystems: bucket list
   (BucketListBase, BucketManager) vs. in-memory state cache
   (InMemorySorobanState). No shared data structures.
3. `addLiveBatch` performs significant I/O (bucket file write in
   `mergeInMemory` → `BucketOutputIterator::put`) while
   `updateInMemorySorobanState` is CPU-bound (hash table operations).
   These use different hardware resources, maximizing parallelism benefit.
4. The existing codebase already uses parallel apply infrastructure
   (`applySorobanStageClustersInParallel`, `applyThread`) demonstrating
   that the system is designed for multi-threaded operation.

## Anti-Evidence

1. **Thread spawn overhead.** Spawning a thread for a ~25ms task has
   negligible overhead (~5-10µs). Could use the existing thread pool
   infrastructure from the parallel apply system.
2. **Error handling.** If either operation throws, the exception must be
   caught and propagated correctly. `addLiveBatch` can throw on I/O errors;
   `updateInMemorySorobanState` throws on assertion failures. Standard
   `std::future` exception propagation handles this.
3. **Ordering dependency with `snapshotLedger`.** Both operations must
   complete before `snapshotLedger` computes the bucket list hash. This
   is a join point that's naturally expressed with a thread join/future.get().
4. **If H002 (cache TTL key hash) is also applied**, `updateInMemorySorobanState`
   drops from ~28ms to ~5ms, making parallelization less valuable (saves
   only ~5ms instead of ~25ms). These hypotheses are complementary but with
   diminishing returns when combined.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full execution path from `finalizeLedgerTxnChanges` (line 3098 in `sealLedgerTxnAndStoreInBucketsAndDB`) through `addLiveBatch` (BucketManager.cpp:1026-1046 → BucketListBase::addBatchInternal:684-797) and `updateInMemorySorobanState` (ApplyState wrapper at LedgerManagerImpl.cpp:308-318 → InMemorySorobanState::updateState:536-602). Confirmed both take `const&` to the entry vectors, modify entirely disjoint data structures (BucketListBase levels + BucketManager metrics vs. InMemorySorobanState hash tables + SorobanMetrics), and have no shared mutable state. The only ordering constraint is that both must complete before `snapshotLedger` (line 3104), which is a natural join point.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:3049-3057` — confirmed serial execution: `getAllEntries` → `addAnyContractsToModuleCache` → `addLiveBatch` → `updateInMemorySorobanState`
- `src/bucket/BucketManager.cpp:addLiveBatch:1026-1046` — confirmed no `mBucketMutex` acquisition; modifies only `mLiveBucketList`, `mBucketAddLiveBatch`, `mBucketLiveObjectInsertBatch`, `mLiveBucketListSizeCounter`, and bucket entry count metrics — all BucketManager-owned members
- `src/bucket/BucketManager.h:317-320` — confirmed all entry parameters are `const&`
- `src/ledger/LedgerManagerImpl.cpp:ApplyState::updateInMemorySorobanState:308-318` — wrapper calls `assertWritablePhase()` then `mInMemorySorobanState.updateState(...)` with `mMetrics.mSorobanMetrics`
- `src/ledger/InMemorySorobanState.cpp:updateState:536-602` — iterates entry vectors, creates/updates/deletes in `mContractDataEntries`/`mContractCodeEntries` hash tables, calls `checkUpdateInvariants` and `reportMetrics`
- `src/ledger/LedgerManagerImpl.cpp:ApplyState::assertWritablePhase:1088-1093` — calls `threadInvariant()` which asserts current thread is main or apply thread
- `src/ledger/LedgerManagerImpl.cpp:threadInvariant:275-285` — the thread-affinity check that would need to be addressed for parallelization
- `src/ledger/LedgerManagerImpl.cpp:sealLedgerTxnAndStoreInBucketsAndDB:3098-3109` — confirmed `finalizeLedgerTxnChanges` returns before `snapshotLedger` is called (natural join point)

### Findings

**Independence confirmed**: The two operations have zero shared mutable state. `addLiveBatch` operates on `BucketManager`'s `mLiveBucketList` and associated metrics. `updateInMemorySorobanState` operates on `ApplyState`'s `mInMemorySorobanState` and `mMetrics.mSorobanMetrics`. The only shared input is the const entry vectors.

**Thread invariant obstacle (fixable)**: `ApplyState::updateInMemorySorobanState` calls `assertWritablePhase()` → `threadInvariant()`, which enforces the caller is the main or apply thread. A spawned helper thread would fail this assertion. Two clean fix approaches:
1. Call `mInMemorySorobanState.updateState(...)` directly (bypassing the `ApplyState` wrapper), which has no thread assertions, from a lambda in `std::async`.
2. Relax the thread invariant for helper threads spawned by the apply thread.

**Resource complementarity**: `addLiveBatch` is I/O-bound (writes bucket files via `BucketOutputIterator::put`), while `updateInMemorySorobanState` is CPU-bound (hash table operations with SHA256 for TTL key derivation). This means they genuinely use different hardware resources, maximizing parallelism benefit.

**No LedgerTxn threading constraints**: Unlike H024 (which was rejected for LedgerTxn thread-affinity constraints), this hypothesis operates entirely on sealed data — the LedgerTxn is already sealed by `getAllEntries` before either operation runs. No LedgerTxn access occurs in either parallelized code path.

**Metrics thread safety**: BucketManager metrics (`mBucketAddLiveBatch`, `mBucketLiveObjectInsertBatch`) and SorobanMetrics (`mContractCodeStateSize`, etc.) are separate metric objects. Medida counters use atomic operations. TracyPlot calls are thread-safe by design. No metric contention expected.

### PoC Guidance

- **Target code**: `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges`, specifically lines 3053-3056. Replace the serial calls with `std::async` launching `addLiveBatch` on a separate thread, then running `updateInMemorySorobanState` on the current thread, then calling `future.get()` to join.
- **Change description**: In `finalizeLedgerTxnChanges`, after `addAnyContractsToModuleCache`, launch `addLiveBatch` via `std::async(std::launch::async, ...)`. Call `mInMemorySorobanState.updateState(...)` directly (bypassing `ApplyState` wrapper to avoid thread invariant assertion) on the current thread. Then call `future.get()` before returning. This preserves the join-before-`snapshotLedger` invariant.
- **Correctness check**: All existing tests that exercise ledger close should pass. Key tests: `[tx]`, `[ledger]`, `[bucket]`, `[soroban]` tags. Also run the parallel apply tests to verify no interference.
- **Benchmark focus**: Apply-load benchmark at T=8 for SAC, custom_token, and soroswap scenarios. Expected improvement: 4-6% (median close time). Profile `finalizeLedgerTxnChanges` with Tracy to verify that the two operations now overlap.
