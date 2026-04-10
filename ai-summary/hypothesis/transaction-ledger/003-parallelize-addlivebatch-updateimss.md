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
