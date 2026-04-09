# H003: BUILD_TESTS Forces LedgerCloseMeta Collection Even When Benchmark Disables Metadata

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Medium
**Impact**: CPU and memory reduction in apply-load benchmark measurements
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When the benchmark config sets `METADATA_OUTPUT_STREAM = ""` and `METADATA_DEBUG_LEDGERS = 0`, the apply-load benchmark should not collect per-transaction ledger close metadata. The `ledgerCloseMeta` pointer should be null throughout the apply path, and all conditional meta operations (`pushTxFeeProcessing`, `setTxProcessingMetaAndResultPair`, `getChanges`, `setPostTxApplyFeeProcessing`) should be skipped. The `enableTxMeta` flag should be false, making `TransactionMetaBuilder` operations no-ops.

## Mechanism

The `apply-load` command is gated behind `BUILD_TESTS` (CommandLine.cpp:2069), so the benchmark always runs with a test build. Two BUILD_TESTS overrides force full metadata collection:

**Override 1** (LedgerManagerImpl.cpp:1598-1607): Forces `ledgerCloseMeta` allocation when no meta stream is configured:
```cpp
#ifdef BUILD_TESTS
    if (!ledgerCloseMeta) {
        ledgerCloseMeta = std::make_unique<LedgerCloseMetaFrame>(...);
        ledgerCloseMeta->reserveTxProcessing(applicableTxSet->sizeTxTotal());
        ledgerCloseMeta->populateTxSet(*txSet);
    }
#endif
```

**Override 2** (LedgerManagerImpl.cpp:2646-2650): Forces `enableTxMeta = true` for `TransactionMetaBuilder`:
```cpp
#ifdef BUILD_TESTS
    enableTxMeta = true;
#endif
```

These overrides cause every ledger close in the benchmark to:
1. Allocate `LedgerCloseMetaFrame` and copy the entire TxSet into it via `populateTxSet` (XDR deep copy of all transaction envelopes).
2. For each tx in `processFeesSeqNums`: call `ltxTx.getChanges()` to extract fee processing changes (line 2292). This allocates `LedgerEntryChanges` vectors with XDR `LedgerEntryChange` objects.
3. For each Soroban tx in `processPostTxSetApply`: call `ltxInner.getChanges()` again (line 2854-2855).
4. For each tx: build full `TransactionMeta` in `TransactionMetaBuilder`, finalize it, and store it in the meta frame (line 2590-2596).
5. Populate `mLastLedgerTxMeta` with all tx meta (line 2592, 2601-2603).

With 6400 txs, this is 6400 `getChanges()` calls in fee processing + 6400 in post-processing + 6400 `TransactionMeta` finalization calls + the initial TxSet deep copy.

The fix would be to add a config flag like `APPLY_LOAD_DISABLE_META_FOR_BENCHMARKING` that suppresses the BUILD_TESTS meta overrides in apply-load mode.

## Trigger

Run the apply-load benchmark in any configuration. Profile `getChanges` and `TransactionMetaBuilder::finalize` call frequency and cost. The overhead is present in every apply-load run since apply-load requires BUILD_TESTS.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:1598-1607` — BUILD_TESTS forces meta allocation
- `src/ledger/LedgerManagerImpl.cpp:2646-2650` — BUILD_TESTS forces `enableTxMeta = true`
- `src/ledger/LedgerManagerImpl.cpp:2290-2293` — per-tx `getChanges()` for fee meta
- `src/ledger/LedgerManagerImpl.cpp:2854-2855` — per-tx `getChanges()` for post-apply meta
- `src/ledger/LedgerManagerImpl.cpp:2590-2596` — per-tx `TransactionMeta` finalization
- `src/ledger/LedgerManagerImpl.cpp:2592,2601-2603` — storing meta in `mLastLedgerTxMeta`
- `src/ledger/LedgerCloseMetaFrame.cpp:151-167` — `populateTxSet` deep-copies entire TxSet
- `src/main/CommandLine.cpp:2069` — apply-load behind BUILD_TESTS

## Evidence

1. `apply-load` is behind `#ifdef BUILD_TESTS` (CommandLine.cpp:2069), so it always builds with test code active.
2. Lines 1598-1607 unconditionally create meta when no stream is configured in test builds.
3. Lines 2646-2650 explicitly set `enableTxMeta = true` in test builds, overriding the optimization.
4. The benchmark config `METADATA_OUTPUT_STREAM = ""` intends to disable metadata output, and the comment at line 2644 says "There is no need to populate the transaction meta if we are not going to output it" — but the BUILD_TESTS override negates this intent.
5. `populateTxSet` (LedgerCloseMetaFrame.cpp:151-167) calls `txSet.toXDR(...)` which deep-copies the entire transaction set — with 6400 txs, this is a significant allocation.
6. `LedgerEntryChanges` from `getChanges()` involves allocating XDR vectors with before/after entry states, which has measurable per-tx overhead.

## Anti-Evidence

1. The meta collection overhead may be small relative to Soroban VM execution time. Need profiling to quantify.
2. `TransactionMetaBuilder` with `enableTxMeta = true` may still be lightweight if operations are mostly move-semantics based.
3. The benchmark is designed for relative comparisons (before/after a change). Since meta overhead is constant across runs, it doesn't affect the validity of A/B comparisons. However, it does affect absolute throughput numbers and the ceiling for optimizations.
4. Removing meta collection would break test infrastructure that relies on `mLastLedgerTxMeta` being populated. A conditional override keyed to apply-load mode would be cleaner.
5. The `DISABLE_SOROBAN_METRICS_FOR_TESTING = true` config already disables Soroban-specific metrics, suggesting the team is aware of measurement overhead. Metadata may be next.
