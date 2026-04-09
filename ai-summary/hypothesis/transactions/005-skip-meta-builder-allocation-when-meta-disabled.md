# H005: Metadata-disabled bundles still allocate TransactionMeta builders

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Low
**Impact**: serial per-tx setup overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

With `METADATA_OUTPUT_STREAM=""` and non-test builds, creating `TxBundle` objects for a parallel Soroban stage should not allocate transaction-meta scaffolding. The metadata-disabled path should be close to storing only the tx pointer, result reference, and tx number.

## Mechanism

`applyTransactions` correctly sets `enableTxMeta` from `ledgerCloseMeta != nullptr`, and the apply-load benchmark template disables metadata output. But `applyParallelPhase` still constructs a `TxEffects` for every tx, and `TxEffects` immediately constructs a `TransactionMetaBuilder` that allocates protocol-specific op-meta vectors and `OperationMetaBuilder` objects even when `metaEnabled` is false. On `sac,TX=6400,...` this serial main-thread work repeats thousands of times per ledger before any worker thread starts.

## Trigger

Run `scripts/run_apply_load_matrix.py` with the stock benchmark config and profile `sac,TX=6400,T=1` or `sac,TX=6400,T=8`. Expect visible setup time in `applyParallelPhase`, `TxBundle` construction, and `TransactionMetaBuilder::TransactionMetaBuilder` despite metadata output being disabled.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:applyTransactions/applyParallelPhase:2641-2668,2710-2738` - computes `enableTxMeta=false` yet still constructs `TxBundle`/`TxEffects` for every tx
- `src/transactions/ParallelApplyStage.h:TxEffects::TxEffects and TxBundle::TxBundle:19-55,61-100` - unconditional `TransactionMetaBuilder` construction
- `src/transactions/TransactionMeta.cpp:TransactionMetaBuilder::TransactionMetaBuilder:924-974` - allocates op-meta vectors/builders even with `metaEnabled=false`
- `docs/apply-load-benchmark-sac.cfg:18-22` - benchmark disables metadata output

## Evidence

The constructor comment says a disabled meta builder should make dependent logic "very cheap", but the constructor still reserves `mOperationMetaBuilders`, resizes the protocol-specific operation-meta vector, and constructs `OperationMetaBuilder` objects for each operation. In the benchmark configuration those allocations are pure setup overhead because `ledgerCloseMeta` is never created.

## Anti-Evidence

This is a smaller opportunity than worker-thread hot-loop issues because each tx here has only one operation and the builder objects are short-lived. The likely impact is therefore lower and concentrated in high-tx-count scenarios such as the SAC benchmark.
