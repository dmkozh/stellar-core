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
