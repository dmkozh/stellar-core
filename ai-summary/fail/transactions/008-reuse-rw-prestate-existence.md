# H003: RW writeback re-probes key existence even though `addReads` already discovered the prestate

**Date**: 2026-04-10
**Subsystem**: transactions
**Severity**: Low
**Impact**: worker-side state lookup duplication during host writeback
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once `addReads` has loaded each read-write footprint key and determined whether the prestate entry exists, the writeback phase should reuse that fact when classifying host outputs as creates vs. updates. The hot path should not query the same thread/live state again for every returned modified entry just to rediscover whether the key existed moments earlier.

## Mechanism

`InvokeHostFunctionApplyHelper::addReads` already performs `getLedgerEntryOpt(lk)` and, for Soroban entries, TTL lookups while preparing the host input buffers. Later, `recordStorageChanges` feeds each returned entry back through `upsertLedgerEntry`, and `TxParallelApplyLedgerState::upsertEntry` calls `getLiveEntryOpt(key).readInScope(*this).has_value()` again to decide whether the change is a logical create. In batched SAC this duplicates state probes for hundreds of newly-created balance and TTL entries per transaction.

## Trigger

Run `scripts/run_apply_load_matrix.py`, especially `sac,TX=6400,T=8` with the default batch size of 100. Profile `InvokeHostFunctionApplyHelper::recordStorageChanges` and `TxParallelApplyLedgerState::upsertEntry`; expect a second wave of thread-map/live-snapshot lookups for keys that were already classified during `addReads`.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::addReads:360-466` — preloads each footprint key and already learns whether the entry exists
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::recordStorageChanges:616-658` — calls `upsertLedgerEntry` for every returned entry
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::upsertEntry:975-1019` — re-reads live state to classify create vs. update
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::getLiveEntryOpt:954-973` — lookup path through tx map, thread map, in-memory state, or snapshot
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSacPayments:2069-2148` — benchmark creates large batched footprints with many fresh destination balances

## Evidence

The add-reads phase must look at the live state for every RW key anyway to populate `mLedgerEntryCxxBufs` and TTL buffers. That existence information is then discarded, and the writeback path asks the same state hierarchy again inside `upsertEntry`; for the benchmark's unique SAC destinations, most of those second lookups just rediscover "this key did not exist" before inserting the new balance/TTL entry.

## Anti-Evidence

The optimization is weaker on workloads where most RW keys already exist and the second lookup stays hot in the thread map, such as some steady-state pool and trustline updates. Any cache also has to be scoped to the apply helper or keyed by RW footprint index so it stays valid across autorestore and live-bucket restore cases.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the full parallel-apply path from `addReads` through `invokeHostFunction` to `recordStorageChanges` → `upsertEntry`. For the SAC benchmark's new entries (the primary claim), `addReads` only looks up TTL keys (lines 398-430), NOT the entry keys themselves — when the TTL is absent, `sorobanEntryLive` stays false and the `getLedgerEntryOpt(lk)` call at line 468 is skipped (`!isSorobanEntry(lk) || sorobanEntryLive` evaluates to false). So the `getLiveEntryOpt` in `upsertEntry` is the FIRST lookup for these entry keys, not a duplicate. For existing entries where true duplication exists, all lookups resolve in O(1) hash map hits in `mThreadEntryMap`.

### Code Paths Examined

- `src/transactions/InvokeHostFunctionOpFrame.cpp:addReads:387-523` — For new Soroban entries: checks TTL key (line 403), finds absent, checks hot archive (line 447), finds absent, then SKIPS entry-key lookup at line 466-468 because `sorobanEntryLive` is false
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::upsertEntry:907-951` — Calls `getLiveEntryOpt(key)` on the entry key; for new entries this is the first (not second) lookup for that key
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::getLiveEntryOpt:886-904` — Checks mTxEntryMap (empty), falls through to `mThreadState.getLiveEntryOpt` which checks mThreadEntryMap then mInMemorySorobanState — all O(1) hash map operations
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::getLiveEntryOpt:700-735` — For Soroban keys not in thread map, calls `mInMemorySorobanState.get(key)` which is a simple hash map lookup (line 211-216 in InMemorySorobanState.cpp)
- `src/transactions/ParallelApplyUtils.cpp:collectClusterFootprintEntriesFromGlobal:563-607` — Only copies existing entries from global map; new entries are not pre-populated

### Why It Failed

The hypothesis's core mechanism is inaccurate for the claimed hot case (new SAC entries):

1. **New entries are NOT double-probed for the entry key.** `addReads` only checks TTL keys for new entries; the entry key itself is first looked up in `upsertEntry`. Different keys are queried at each stage.

2. **For existing entries where duplication exists, the cost is negligible.** The second lookup hits `mThreadEntryMap` (an in-memory hash map) in O(1). At ~10ns per lookup, even 200 entries per tx × 6400 txs = ~13ms total — well under 0.1% of benchmark time.

3. **All Soroban key lookups are in-memory.** The `InMemorySorobanState::isInMemoryType` check (line 725) routes all CONTRACT_DATA, CONTRACT_CODE, and TTL lookups through in-memory hash maps, never touching disk/snapshot I/O. Hash map misses for non-existent keys cost ~5-10ns each.

4. **The `upsertEntry` return value is only used for a correctness assertion** (the created-keys TTL check at lines 684-698 in recordStorageChanges), not for any hot-path optimization that would benefit from caching.

### Lesson Learned

When analyzing `addReads` for duplication, carefully distinguish which keys are actually looked up at each phase. For non-existent Soroban entries, `addReads` only probes the TTL key to determine liveness — it deliberately skips loading the entry key when the TTL is absent. This means claimed "duplication" of entry-key lookups is actually a first-time lookup. Also, all Soroban state in the parallel-apply path is served from in-memory hash maps (InMemorySorobanState, mThreadEntryMap), so even redundant lookups have negligible cost.
