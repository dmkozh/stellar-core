# H003: Skip Effects Delta Construction When Invariants Are Disabled

**Date**: 2025-07-14
**Subsystem**: soroban, ledger
**Severity**: Low-Medium
**Impact**: Reduced per-TX parallel phase work; eliminates wasted allocations
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When invariant checking is disabled (no invariants in `mEnabled`),
`setEffectsDeltaFromSuccessfulTx` should not be called, because the
`LedgerTxnDelta` it builds is only consumed by
`checkAllTxBundleInvariants` → `InvariantManagerImpl::checkOnOperationApply`,
which iterates the empty `mEnabled` list and does nothing.

## Mechanism

For every successful Soroban TX in the parallel phase,
`setEffectsDeltaFromSuccessfulTx` (ParallelApplyUtils.cpp:790-829) iterates
all modified entries and for each one:

1. Calls `getLiveEntryOpt(lk)` — hash map lookup in `mThreadEntryMap` (~50ns)
2. Creates `make_shared<InternalLedgerEntry>(prevLe.value())` for
   `entryDelta.previous` — heap allocation + LedgerEntry deep copy (~150-500ns
   depending on entry size)
3. Creates `make_shared<InternalLedgerEntry>(entryOpt.value())` for
   `entryDelta.current` — same cost
4. Calls `effects.setDeltaEntry(lk, entryDelta)` — hash map emplace into
   `mDelta.entry` (~100ns)

Total per entry: ~450-1150ns. With 3200 TXs × ~5 modified entries each =
~16,000 entries across all threads.

At T=8, each thread processes ~400 TXs × 5 entries = 2000 entries, costing
~1-2ms per thread of pure parallel-phase work. This work is entirely
wasted when invariants are disabled because `checkOnOperationApply`
(InvariantManagerImpl.cpp:143-170) iterates `mEnabled` which is empty.

The fix: pass a flag (derived from `InvariantManager::hasEnabledInvariants()`
or similar) through the parallel apply pipeline, and guard the
`setEffectsDeltaFromSuccessfulTx` call. Also guard
`checkAllTxBundleInvariants` (which iterates all TXs and calls
`setDeltaHeader` per TX even when invariants are disabled).

## Trigger

Run apply-load benchmark with T=8 and 3200 SAC transactions. With
invariants disabled (default for benchmark/validator configs), the
entire delta construction is wasted work. Profile
`setEffectsDeltaFromSuccessfulTx` to measure the wasted time.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:setEffectsDeltaFromSuccessfulTx:790-829` — builds unused delta
- `src/transactions/TransactionFrame.cpp:parallelApply:2241` — unconditionally calls setEffectsDelta
- `src/ledger/LedgerManagerImpl.cpp:checkAllTxBundleInvariants:2473-2514` — consumes delta only for invariants
- `src/invariant/InvariantManagerImpl.cpp:checkOnOperationApply:143-170` — iterates empty mEnabled list
- `src/transactions/ParallelApplyStage.h:TxEffects:19-55` — stores the delta

## Evidence

- `TxEffects::getDelta()` is called ONLY in `checkAllTxBundleInvariants` (LedgerManagerImpl.cpp:2493) for the parallel apply path
- `checkOnOperationApply` iterates `mEnabled` invariants — when empty, the entire delta is unused
- `setEffectsDeltaFromSuccessfulTx` performs per-entry hash map lookups + `make_shared` heap allocations + `LedgerEntry` deep copies — all hot-path work on the parallel threads
- The benchmark config does not enable extra invariants
- Validators typically run with default invariants only; some configs disable all invariants
- Similar pattern exists: `setLedgerChangesFromSuccessfulOp` already has an internal `if (!mEnabled)` guard (TransactionMeta.cpp:390-393) for metadata

## Anti-Evidence

- Some invariants may be enabled by default even without explicit config (need to verify default `mEnabled` set)
- If invariants ARE enabled, skipping the delta would cause invariant checking to fail silently
- Adding a flag through the pipeline adds plumbing complexity
- The wall-clock savings at T=8 are ~1-2ms per thread of parallel work, which might not significantly reduce overall ledger time if the parallel phase is dominated by Soroban VM execution
- The `checkAllTxBundleInvariants` function also sets `maybeSetRefundableFeeMeta` (line 2511-2512) which IS needed regardless of invariants — so the function cannot be entirely skipped, only the `checkOnOperationApply` call and delta construction
