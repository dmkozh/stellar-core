# H002: Sequential processPostTxSetApply Is a Serial Bottleneck After Parallel Apply

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Medium
**Impact**: Parallelization improvement for T=8 Soroban scenarios
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

After parallel Soroban transaction execution completes, the post-processing step (fee refunds, meta collection, result recording) should either be parallelized or have minimal per-transaction cost. The sequential post-processing should not negate a significant fraction of the benefits of parallel execution.

## Mechanism

`processPostTxSetApply()` (LedgerManagerImpl.cpp:2827-2874) runs **sequentially** on the primary apply thread after all parallel Soroban stages complete. For each Soroban transaction, it:

1. Opens a new `LedgerTxn` (line 2844) — copies header, sets up entry tracking.
2. Calls `processPostTxSetApply` on the transaction (line 2845) — which calls `processRefund` to refund unused Soroban fees. This loads and modifies the fee source account.
3. Calls `ledgerCloseMeta->setPostTxApplyFeeProcessing(ltxInner.getChanges(), ...)` (line 2854) — extracts `LedgerEntryChanges` from the LedgerTxn (XDR vector copy).
4. Commits the LedgerTxn (line 2857) — merges entries back to parent.
5. Calls `processResultAndMeta` (line 2862) — records the transaction result and meta into `txResultSet` and `ledgerCloseMeta`.

With 3000-6400 Soroban transactions per ledger, this loop iterates thousands of times sequentially. Each iteration creates and destroys a `LedgerTxn`, which involves: header copy, entry map allocation, parent deactivation, and on commit: entry merge back to parent. The `refundSorobanFee` (TransactionFrame.cpp:2604) loads the fee source account via `stellar::loadAccount`, modifies the balance, and the LedgerTxn bookkeeping wraps this single mutation.

This sequential processing directly limits the speedup of T=8 parallel execution — the parallelized Soroban execution takes time ~T/8, but the sequential post-processing still takes time proportional to the total number of transactions, creating an Amdahl's law bottleneck.

## Trigger

Run any T=8 apply-load benchmark scenario with high transaction counts (sac TX=6400 or custom_token TX=3000). Use Tracy profiling to measure the wall time spent in `processPostTxSetApply` relative to total apply time. The bottleneck should be visible as a single-threaded region after the parallel execution zone.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2827-2874` — `processPostTxSetApply` iterates all txs sequentially
- `src/ledger/LedgerManagerImpl.cpp:2844` — per-tx `LedgerTxn` creation
- `src/ledger/LedgerManagerImpl.cpp:2845-2850` — `processPostTxSetApply` on each tx
- `src/ledger/LedgerManagerImpl.cpp:2854-2855` — per-tx `getChanges()` for meta
- `src/ledger/LedgerManagerImpl.cpp:2857` — per-tx commit
- `src/transactions/TransactionFrame.cpp:2581-2587` — `processPostTxSetApply` calls `processRefund`
- `src/transactions/TransactionFrame.cpp:2592-2615` — `processRefund` loads account, modifies balance

## Evidence

1. The loop at line 2839-2867 iterates over ALL stages and ALL txBundles sequentially — no parallelism.
2. Each iteration opens a `LedgerTxn` (line 2844), performs a refund, extracts changes, and commits.
3. With 6400 transactions (sac T=8 scenario), this is 6400 sequential LedgerTxn create-modify-commit cycles.
4. The refund involves `loadAccount` (a LedgerTxn entry lookup + deref) and a balance mutation.
5. The `getChanges()` call extracts a vector of `LedgerEntryChange` objects involving XDR copies.
6. The comment at lines 2869-2872 acknowledges this is only for the parallel path, suggesting it was added as a serialized post-processing step without considering batching.
7. Per Amdahl's law, if parallel execution takes 50ms at T=8 and post-processing takes 20ms, the maximum speedup is limited to 50ms/(50/8 + 20)ms = ~1.9x instead of the ideal ~8x.

## Anti-Evidence

1. `processRefund` is O(1) per tx — a single account balance credit. The LedgerTxn overhead per iteration may be only ~1-5μs, making the total for 6400 txs ~6-32ms.
2. If the parallel Soroban VM execution dominates total time (e.g., >200ms for T=8), then 20-30ms of post-processing is <15% overhead and may not meet the Medium severity threshold.
3. Parallelizing refunds is non-trivial: multiple transactions may share a fee source account, creating write conflicts. The current sequential approach avoids this by design.
4. The meta collection (`getChanges`, `setPostTxApplyFeeProcessing`) is inherently order-dependent since meta is stored in transaction-index order in the `LedgerCloseMeta`.
5. If metadata output is disabled (as in benchmark config), the `getChanges` cost is lower — but BUILD_TESTS forces meta collection anyway.
