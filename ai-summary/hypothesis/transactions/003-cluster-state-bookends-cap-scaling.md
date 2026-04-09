# H003: Cluster state import and merge cap T=8 scaling

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Medium
**Impact**: parallel apply throughput / main-thread bottleneck
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

After the tx-set builder has already formed `ApplyStage` clusters, the remaining stage bookkeeping should avoid repeated whole-stage and whole-cluster footprint scans on the critical path. Importing thread state and merging results back should reuse precomputed cluster/stage key sets rather than re-hashing every tx footprint each ledger close.

## Mechanism

Each `ThreadParallelApplyLedgerState` constructor walks every tx in the cluster and every key in both RO and RW footprints to populate `mThreadEntryMap`. After worker threads finish, `commitChangesFromThreads` calls `getReadWriteKeysForStage(stage)`, which rescans the entire stage again to build a deduped RW set before serially iterating every thread map and merging it back into global state. Because apply-load asserts there is exactly one maximally parallel stage, these setup/teardown passes are unavoidable bookends around the worker phase and can materially limit the upside of `T=8`, especially on write-heavy ledgers timed with `APPLY_LOAD_TIME_WRITES=true`.

## Trigger

Run the benchmark with `APPLY_LOAD_TIME_WRITES=true` (the default template) and compare `custom_token,TX=3000,T=8` or `sac,TX=6400,T=8` under a profiler. Expect main-thread time in `collectClusterFootprintEntriesFromGlobal`, `getReadWriteKeysForStage`, and `commitChangesFromThreads` even when worker threads are otherwise scaling.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:getReadWriteKeysForStage:99-117` - rescans the full stage to rebuild the RW key set
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::collectClusterFootprintEntriesFromGlobal:562-608` - rescans every tx footprint to seed per-thread maps
- `src/transactions/ParallelApplyUtils.cpp:GlobalParallelApplyLedgerState::commitChangesFromThreads:546-560` - serial stage merge after workers complete
- `src/ledger/LedgerManagerImpl.cpp:LedgerManagerImpl::applySorobanStageClustersInParallel:2426-2470` - constructs per-cluster thread state on the main thread before launching work
- `src/simulation/ApplyLoad.cpp:ApplyLoad::benchmarkModelTxTpsSingleLedger:2016-2025` - benchmark expects one stage and max clusters, so stage bookends are directly on the measured path

## Evidence

The code computes stage- and cluster-level key sets lazily during apply rather than storing them on `ApplyStage` / `Cluster` when the tx-set builder already has the same information. The merge path also stays fully serial even though clusters are disjoint by construction, so the more the worker phase speeds up, the more this fixed bookkeeping shows up in end-to-end close time.

## Anti-Evidence

The import/merge passes are also where scope ownership, TTL-bump semantics, and restore tracking are enforced, so some amount of ordered bookkeeping is required. A valid optimization therefore needs to preserve those semantics while reducing redundant footprint scans and hash-table churn.
