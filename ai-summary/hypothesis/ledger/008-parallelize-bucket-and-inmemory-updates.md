# H008: Sequential BucketList and InMemorySorobanState Updates Could Run in Parallel

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Medium
**Impact**: Parallelization of post-seal commit path, reducing ledger close time for all Soroban scenarios
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

After the LedgerTxn is sealed and `getAllEntries()` produces the init/live/dead entry vectors, the subsequent consumers of these vectors — `addLiveBatch` (BucketList update), `addHotArchiveBatch`, `updateInMemorySorobanState`, and `addAnyContractsToModuleCache` — should execute with maximum parallelism since they operate on independent data structures. The init/live/dead vectors are immutable (const references) at this point and can be safely shared across threads.

## Mechanism

`finalizeLedgerTxnChanges()` (LedgerManagerImpl.cpp:3039-3047) performs three sequential operations after sealing the LedgerTxn:

```
getAllEntries(initEntries, liveEntries, deadEntries);        // seal + extract
addAnyContractsToModuleCache(lh.ledgerVersion, initEntries); // scan for CONTRACT_CODE
addAnyContractsToModuleCache(lh.ledgerVersion, liveEntries); // scan for CONTRACT_CODE  
addLiveBatch(mApp, lh, initEntries, liveEntries, deadEntries); // BucketList update
updateInMemorySorobanState(init, live, dead, lh, config);      // in-memory cache update
```

These operations are independent:
- `addLiveBatch` modifies the `LiveBucketList` (writing serialized entries to bucket files on disk, potentially triggering merges)
- `updateInMemorySorobanState` modifies the `InMemorySorobanState` (updating in-memory hash maps for contract data/code/TTL entries)
- `addAnyContractsToModuleCache` modifies the `SorobanModuleCache` (compiling new Wasm contracts)

All three read the same `initEntries`/`liveEntries`/`deadEntries` vectors via const references. None of them modify these vectors. They write to completely separate data structures with no shared state.

By running `addLiveBatch` and `updateInMemorySorobanState` in parallel (e.g., via `std::async`), the total wall time would be `max(addLiveBatch_time, updateInMemorySorobanState_time)` instead of `addLiveBatch_time + updateInMemorySorobanState_time`.

For a SAC benchmark with 3000 transactions and ~15,000-20,000 modified entries, `addLiveBatch` may take 5-20ms (serializing entries into bucket output) and `updateInMemorySorobanState` may take 2-10ms (hash map updates for Soroban entries only). Parallelizing these saves ~2-10ms per ledger.

## Trigger

Run SAC or custom_token apply-load benchmarks at T=1 and T=8. Profile the `finalizeLedgerTxnChanges` Tracy zone. Measure the individual durations of `addLiveBatch` and `updateInMemorySorobanState` (both have ZoneScoped or timer metrics). The sum of these two is the potential savings.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:3040-3046` — Sequential execution of `getAllEntries`, `addAnyContractsToModuleCache`, `addLiveBatch`, `updateInMemorySorobanState`
- `src/bucket/BucketManager.cpp:1026-1046` — `addLiveBatch` implementation (BucketList serialization)
- `src/ledger/InMemorySorobanState.cpp:535-602` — `updateState` implementation (in-memory hash map updates)
- `src/ledger/LedgerManagerImpl.cpp:3147-3176` — `addAnyContractsToModuleCache` (Wasm compilation for new contracts)

## Evidence

1. `addLiveBatch` takes `const&` vectors (LiveBucketList.cpp:17-19), confirming no mutation of input data.
2. `updateInMemorySorobanState` takes `const&` vectors (InMemorySorobanState.cpp:537-539), confirming no mutation of input data.
3. `LiveBucketList` and `InMemorySorobanState` are entirely separate data structures with no shared state. `LiveBucketList` is owned by `BucketManager`; `InMemorySorobanState` is owned by `ApplyState`.
4. The `addLiveBatch` path involves serialization and I/O (writing to bucket files), making it CPU+I/O bound. `updateInMemorySorobanState` is CPU+memory bound (hash map operations). These have different resource bottlenecks, making parallelization especially effective.
5. The entries vectors are local to `finalizeLedgerTxnChanges` and their lifetime extends past both operations, so sharing across threads is safe.
6. Tracy profiling zones already exist for both operations (`BucketManager::addLiveBatch` has `mBucketAddLiveBatch.TimeScope()` at BucketManager.cpp:1038; `updateState` is called from `ApplyState::updateInMemorySorobanState` which has `assertWritablePhase()`).

## Anti-Evidence

1. **Thread safety of BucketManager**: `addLiveBatch` may access shared BucketManager state (metric counters, bucket list caches). Running it on a separate thread while the apply thread continues other work requires verifying no concurrent access to BucketManager from the apply thread.
2. **Ordering constraints**: The `storePersistentStateAndLedgerHeaderInDB` call at line 3095 needs the BucketList to be updated (via `snapshotLedger` at line 3094). If `addLiveBatch` runs in parallel and isn't complete by line 3094, the snapshot would be stale. The parallel path would need a synchronization point before `snapshotLedger`.
3. **`addAnyContractsToModuleCache` dependency**: Module cache compilation must complete before `updateInMemorySorobanState` if the module cache is consulted during the update. Looking at the code, `updateState` doesn't consult the module cache, so this is not a constraint.
4. **Phase assertions**: `ApplyState::updateInMemorySorobanState` calls `assertWritablePhase()` which checks we're in the correct phase. Running on a different thread may violate this assertion if the phase check is thread-affine.
5. **Complexity vs. benefit**: If `addLiveBatch` takes 15ms and `updateInMemorySorobanState` takes 5ms, parallelization saves only 5ms. This may not meet the Medium threshold depending on total ledger close time.
