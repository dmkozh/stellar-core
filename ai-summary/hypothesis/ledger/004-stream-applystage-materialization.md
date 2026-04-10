# H004: Stream `ApplyStage` Materialization Instead of Building the Full Parallel Phase Up Front

**Date**: 2026-04-10
**Subsystem**: ledger, transactions
**Severity**: Medium
**Impact**: serial pre-parallel CPU and allocation pressure before Soroban workers begin
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The apply thread should only materialize the bookkeeping required for the
current parallel stage, or at least avoid allocating a full `TxBundle` /
`TxEffects` graph for every Soroban transaction before any worker starts.
Stage execution should begin promptly, with post-apply bookkeeping stored in a
form that does not force the whole parallel phase to stay fully materialized.

## Mechanism

`applyParallelPhase` iterates every stage and every cluster, constructs a
`TxBundle` for every Soroban tx, and appends the resulting `ApplyStage`s into
`applyStages` before calling `applySorobanStages`. Each `TxBundle` heap-owns a
`TxEffects`, and `TxEffects` constructs a `TransactionMetaBuilder`, which
allocates operation-meta state up front. Because `processPostTxSetApply` later
walks the stored `applyStages`, this entire nested object graph stays alive
until after the whole parallel phase has finished.

Streaming one stage at a time, or flattening the retained post-apply data to a
minimal per-tx worklist keyed by tx index, would remove a large serial
allocation burst from the front of the parallel phase and reduce peak memory
pressure. The benchmark test build is particularly sensitive here because tx
meta is forcibly enabled even when metadata output is configured off.

## Trigger

Run `scripts/run_apply_load_matrix.py` in the test build and profile the time
inside `applyParallelPhase` before entering `applySorobanStages`, plus heap
allocation counts attributable to `TxBundle`, `TxEffects`, and
`TransactionMetaBuilder`.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:applyParallelPhase:2710-2766` — builds every `ApplyStage` before any stage is executed
- `src/transactions/ParallelApplyStage.h:TxEffects:22-59` — per-tx effect container allocated for the whole parallel phase
- `src/transactions/ParallelApplyStage.h:TxBundle:64-104` — heap-allocates `TxEffects` in the constructor
- `src/transactions/TransactionMeta.cpp:TransactionMetaBuilder::TransactionMetaBuilder:924-974` — eagerly allocates per-op meta builders and XDR buffers
- `src/ledger/LedgerManagerImpl.cpp:processPostTxSetApply:2828-2874` — current consumer that forces `applyStages` retention after execution
- `src/ledger/LedgerManagerImpl.cpp:2645-2650` — BUILD_TESTS forces tx meta enabled

## Evidence

- `applyParallelPhase` fully materializes `applyStages` and only then calls
  `applySorobanStages`, so none of this construction overlaps with worker
  execution.
- `TxBundle` uses `std::unique_ptr<TxEffects>` instead of embedding effects,
  guaranteeing an extra allocation per Soroban tx.
- `TransactionMetaBuilder` reserves and constructs per-operation meta structures
  at `TxBundle` creation time, not lazily when the worker needs them.
- The retained `applyStages` are only needed because `processPostTxSetApply`
  reuses the nested bundle structure later for refunds and result/meta emission.

## Anti-Evidence

- Soroban benchmark transactions usually have a single operation, so each
  individual `TransactionMetaBuilder` is not very large.
- Refactoring this to a streamed representation would touch both execution and
  post-apply ordering logic, making the change comparatively invasive.
- If profiling shows host execution still dominates overwhelmingly, the serial
  construction burst may land closer to the low end of measurable impact.
