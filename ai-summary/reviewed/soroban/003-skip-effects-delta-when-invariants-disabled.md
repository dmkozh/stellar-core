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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete lifecycle of `LedgerTxnDelta` in the parallel apply path: construction in `setEffectsDeltaFromSuccessfulTx` on worker threads (ParallelApplyUtils.cpp:790-829), storage in `TxEffects::mDelta` (ParallelApplyStage.h:54), and consumption in `checkAllTxBundleInvariants` (LedgerManagerImpl.cpp:2473-2514) on the main thread. Confirmed that `TxEffects::getDelta()` has exactly one production caller: the invariant check in `checkAllTxBundleInvariants`. Also confirmed that `INVARIANT_CHECKS` defaults to `{}` (Config.cpp:350) and `mEnabled` is only populated via `enableInvariantsFromConfig()` (ApplicationImpl.cpp:1604-1608), which iterates the empty config list. In test configs, invariants ARE enabled via a regex matching all invariants (test.cpp:530-531).

### Code Paths Examined

- `src/main/Config.cpp:350` — `INVARIANT_CHECKS = {}` defaults to empty
- `src/main/ApplicationImpl.cpp:1604-1608` — `enableInvariantsFromConfig()` only enables invariants from config
- `src/invariant/InvariantManagerImpl.h:26` — `mEnabled` is a `vector<shared_ptr<Invariant>>`, empty by default
- `src/invariant/InvariantManagerImpl.cpp:143-170` — `checkOnOperationApply` iterates `mEnabled`; no-op when empty
- `src/transactions/TransactionFrame.cpp:2241-2244` — unconditionally calls `setEffectsDeltaFromSuccessfulTx` for successful parallel txs
- `src/transactions/ParallelApplyUtils.cpp:790-829` — builds delta with `make_shared` allocations per entry
- `src/transactions/ParallelApplyStage.h:33-36` — `getDelta()` returns the stored delta
- `src/ledger/LedgerManagerImpl.cpp:2488-2498` — only production consumer of `getDelta()`, passes to `checkOnOperationApply`
- `src/ledger/LedgerManagerImpl.cpp:2511-2512` — `maybeSetRefundableFeeMeta` is always needed, independent of delta
- `src/test/test.cpp:530-531` — test config enables all invariants via `{"(?!EventsAreConsistentWithEntryDiffs).*"}`

### Findings

The inefficiency is **confirmed real**: `setEffectsDeltaFromSuccessfulTx` constructs a `LedgerTxnDelta` with heap allocations (`make_shared<InternalLedgerEntry>`) for every modified entry in every successful Soroban TX, and this delta is only consumed by invariant checking which is a no-op in production/benchmark configs.

The fix is **correct and safe**: guarding with a flag derived from `mEnabled.empty()` would skip all delta construction when no invariants are enabled. The `maybeSetRefundableFeeMeta` call in `checkAllTxBundleInvariants` is independent and would continue working. No method exists yet to query `mEnabled.empty()` via the public API — a `hasEnabledInvariants()` method would need to be added to `InvariantManager`.

**Severity downgraded to Informational**: The estimated per-thread savings of ~1-2ms represent approximately 1-2% of the parallel phase wall time for a 3200 SAC / T=8 benchmark scenario (where the parallel phase is likely 50-150ms). This is well below the 5% threshold for Low severity. The optimization would save approximately 16,000 `make_shared` heap allocations and `LedgerEntry` deep copies per ledger, which is measurable in profiling but unlikely to produce a visible improvement in end-to-end benchmark numbers.

### PoC Guidance

- **Target code**: 
  1. Add `bool hasEnabledInvariants() const` to `InvariantManager` / `InvariantManagerImpl` (returns `!mEnabled.empty()`)
  2. Pass the flag through `AppConnector` or `Config` to the parallel apply pipeline
  3. Guard `setEffectsDeltaFromSuccessfulTx` call in `TransactionFrame.cpp:2243` with the flag
  4. Guard `setDeltaHeader` + `checkOnOperationApply` block in `LedgerManagerImpl.cpp:2488-2498` with the flag
- **Change description**: Skip construction and consumption of `LedgerTxnDelta` in parallel apply when no invariants are enabled
- **Correctness check**: Run existing test suite (tests enable invariants by default via `INVARIANT_CHECKS = {"(?!EventsAreConsistentWithEntryDiffs).*"}`, so invariant checking is exercised in tests). Also run `[invariant]` tagged tests specifically.
- **Benchmark focus**: Profile `setEffectsDeltaFromSuccessfulTx` time and heap allocation count in SAC 3200 / T=8 scenario. Expect ~1-2ms/thread reduction in parallel phase, ~16K fewer heap allocations per ledger. End-to-end improvement likely <2%.
