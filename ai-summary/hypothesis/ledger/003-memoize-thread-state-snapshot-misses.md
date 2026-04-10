# H003: Memoize `getLiveEntryOpt` Snapshot Misses in Thread State

**Date**: 2026-04-10
**Subsystem**: ledger, transactions
**Severity**: Low
**Impact**: repeated snapshot/in-memory lookups on worker and post-worker bookkeeping paths
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Within a cluster, once a key has been loaded from the live snapshot or
`InMemorySorobanState`, later reads of the same pre-state key should reuse that
materialized value from thread-local state. The parallel-apply code should not
re-fetch and re-copy the same entry multiple times in one transaction's
host/setup/meta/commit flow.

## Mechanism

`ThreadParallelApplyLedgerState::getLiveEntryOpt` checks `mThreadEntryMap`, but
when the key is absent it reads from `mInMemorySorobanState` or
`mLCLSnapshot.loadLiveEntry(key)` and immediately returns a scoped copy without
memoizing it. The same key can then be reloaded repeatedly by
`TxParallelApplyLedgerState::upsertEntry`, `eraseEntryIfExists`,
`setEffectsDeltaFromSuccessfulTx`, `OperationMetaBuilder::setLedgerChangesFromSuccessfulOp`,
and `commitChangeFromSuccessfulTx`, each of which calls back into
`getLiveEntryOpt`.

Caching first-miss results as clean entries in thread-local state would turn
these repeated snapshot and in-memory lookups into cheap hash-map hits. This is
especially relevant in BUILD_TESTS benchmarks where transaction meta is forced
on, because successful Soroban txs already revisit modified keys again for both
delta and meta construction.

## Trigger

Run `scripts/run_apply_load_matrix.py` with any Soroban scenario in the test
build. Profile repeated calls to `ThreadParallelApplyLedgerState::getLiveEntryOpt`
for the same keys during one successful transaction and compare with a memoized
variant.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:getLiveEntryOpt:687-721` — reads snapshot/in-memory state on misses but does not cache the result
- `src/transactions/ParallelApplyUtils.cpp:commitChangeFromSuccessfulTx:748-773` — reloads pre-state per modified key
- `src/transactions/ParallelApplyUtils.cpp:setEffectsDeltaFromSuccessfulTx:777-815` — reloads previous state again for delta construction
- `src/transactions/TransactionMeta.cpp:setLedgerChangesFromSuccessfulOp:385-452` — reloads previous state again for op-meta changes
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::upsertEntry:893-938` — probes live existence via `getLiveEntryOpt`
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::eraseEntryIfExists:940-963` — probes live existence via `getLiveEntryOpt`
- `src/ledger/LedgerManagerImpl.cpp:2645-2650` — BUILD_TESTS forces tx meta enabled in benchmark builds

## Evidence

- `getLiveEntryOpt` only consults `mThreadEntryMap` for preloaded global keys;
  snapshot/in-memory misses are returned directly and not retained.
- The same key is visibly re-read in multiple later phases of successful tx
  handling: host bookkeeping, delta construction, and op-meta construction.
- `OperationMetaBuilder::setLedgerChangesFromSuccessfulOp` and
  `setEffectsDeltaFromSuccessfulTx` each independently call
  `threadState.getLiveEntryOpt(lk)` for the same modified keys.
- In BUILD_TESTS benchmark binaries, tx meta stays enabled, so the extra
  op-meta readback is present even when metadata streaming is disabled.

## Anti-Evidence

- Some keys may truly be touched only once, reducing the benefit of memoization.
- Making `getLiveEntryOpt` populate a cache changes a logically-const path and
  would require careful scope and thread-safety auditing.
- If `mThreadEntryMap` growth meaningfully harms cache locality, a naive
  memoization strategy could trade one form of overhead for another.
