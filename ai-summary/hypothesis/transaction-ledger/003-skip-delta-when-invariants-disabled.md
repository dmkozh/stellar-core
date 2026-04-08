# H003: Skip LedgerTxnDelta Construction When Invariant Checking Is Disabled

**Date**: 2026-04-08
**Subsystem**: transaction-ledger (transactions/ParallelApplyUtils, transactions/TransactionFrame)
**Severity**: Low
**Impact**: 5-10% improvement on T=8 scenarios by eliminating unnecessary heap allocations on worker threads
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When no invariants are enabled (typical for validator nodes and the
apply-load benchmark), `setEffectsDeltaFromSuccessfulTx` should be skipped
entirely, and `checkAllTxBundleInvariants` should be a no-op. The
`LedgerTxnDelta` construction — which involves heap allocations of
`shared_ptr<InternalLedgerEntry>` for every modified entry — should not
occur when its sole consumer (invariant checking) is disabled.

## Mechanism

`setEffectsDeltaFromSuccessfulTx` (ParallelApplyUtils.cpp:790-829) runs
on each worker thread after a successful Soroban tx. For each modified
entry, it:
1. Calls `getLiveEntryOpt(lk)` to get the previous state (potentially
   falling through to InMemorySorobanState with SHA256 overhead)
2. Allocates `make_shared<InternalLedgerEntry>(prevLe.value())` — heap
   allocation + copy of entry (100-500+ bytes)
3. Allocates `make_shared<InternalLedgerEntry>(entryOpt.value())` — another
   heap allocation + copy

For a tx modifying 20 entries: 40 heap allocations + 40 entry copies +
20 `getLiveEntryOpt` lookups — all running on the worker thread.

The resulting `LedgerTxnDelta` is consumed ONLY by `checkAllTxBundleInvariants`
(LedgerManagerImpl.cpp:2473-2514), which calls
`app.checkOnOperationApply(...)`. When no invariants are enabled,
`InvariantManagerImpl::checkOnOperationApply` (InvariantManagerImpl.cpp:143)
iterates an empty `mEnabled` vector and does nothing.

The `setLedgerChangesFromSuccessfulOp` call (TransactionMeta.cpp:385) that
handles meta building does NOT use the delta — it independently iterates
`res.getModifiedEntryMap()` and `threadState.getLiveEntryOpt()`. So the
delta is purely for invariant checking.

For 200 txs with 20 modified entries each: 8000 heap allocations + 8000
entry copies. At ~100ns per allocation + ~200ns per copy: ~2.4ms of
worker-thread time. With T=8, the per-thread cost is ~300µs, but the
allocator contention under 8 concurrent threads may amplify this via
malloc lock contention.

## Trigger

Run the apply-load benchmark with T=8 and any Soroban scenario. Compare
total time of `applySorobanStageClustersInParallel` with and without the
delta construction. Measure using Tracy zones or by conditionally
compiling out the delta.

## Target Code

- `src/transactions/TransactionFrame.cpp:2243-2244` — `threadState.setEffectsDeltaFromSuccessfulTx(*res, ...)`: unconditionally called on success
- `src/transactions/ParallelApplyUtils.cpp:790-829` — `setEffectsDeltaFromSuccessfulTx`: builds delta with make_shared allocations
- `src/ledger/LedgerManagerImpl.cpp:2473-2514` — `checkAllTxBundleInvariants`: sole consumer of the delta
- `src/invariant/InvariantManagerImpl.cpp:143-171` — `checkOnOperationApply`: no-op when mEnabled is empty

## Evidence

- `setEffectsDeltaFromSuccessfulTx` (line 803-804, 823-824) does `make_shared<InternalLedgerEntry>` for both previous and current for each modified entry
- `checkAllTxBundleInvariants` (line 2490) passes `txBundle.getEffects().getDelta()` to `checkOnOperationApply`
- `InvariantManagerImpl::checkOnOperationApply` (line 148) iterates `mEnabled` — empty when invariants disabled
- `setLedgerChangesFromSuccessfulOp` (TransactionMeta.cpp:398-401) independently reads `res.getModifiedEntryMap()` without using the delta
- On validator nodes, invariants are typically not enabled, making this dead work

## Anti-Evidence

- Invariants CAN be enabled in some configurations (archiver nodes, test environments)
- The overhead per tx (~12µs for 20 entries) is small relative to VM execution (~2-5ms)
- Removing the delta path entirely would require a way to conditionally enable it, adding complexity
- heap allocator contention may not be significant if using jemalloc/tcmalloc (which stellar-core may use)
- `TxEffects` stores the delta, but `TxEffects` also stores meta — cannot eliminate the object entirely
