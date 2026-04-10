# H011: Skip Invariant Delta Construction Using InvariantManager Runtime State

**Date**: 2026-04-10
**Subsystem**: ledger (LedgerManagerImpl), transactions (ParallelApplyUtils)
**Severity**: Low
**Impact**: Eliminate per-tx `make_shared` heap allocations on worker threads when invariants are disabled at runtime
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When no invariants are enabled at runtime (as determined by the
InvariantManager's `mEnabled` set), `setEffectsDeltaFromSuccessfulTx` on
worker threads should be skipped entirely, and `checkAllTxBundleInvariants`
should skip the invariant-check block. The `LedgerTxnDelta` — which involves
heap-allocating `shared_ptr<InternalLedgerEntry>` for every modified entry's
before and after state — should not be constructed when its sole consumer
(invariant checking) has nothing enabled.

## Mechanism

A prior hypothesis (`fail/transaction-ledger/011`) identified this exact
inefficiency but was rejected because its PoC guarded the optimization with
`config.INVARIANT_CHECKS.empty()`, which does not reflect runtime state —
invariants can be enabled programmatically after config load (e.g., in test
harnesses via `InvariantManager::enableInvariant()`).

This hypothesis proposes the correct guard: query the InvariantManager's
actual runtime-enabled set. Specifically:

1. Add `bool InvariantManager::hasEnabledInvariants() const` that returns
   `!mEnabled.empty()`.
2. At the start of `applySorobanStage` (LedgerManagerImpl.cpp:2517),
   capture `bool checkInvariants = app.getInvariantManager().hasEnabledInvariants()`.
3. Pass this flag through to worker threads (via `ParallelLedgerInfo` or
   similar). On workers, skip `setEffectsDeltaFromSuccessfulTx` when false.
4. In `checkAllTxBundleInvariants` (LedgerManagerImpl.cpp:2473), skip the
   invariant block when the flag is false. Keep `maybeSetRefundableFeeMeta`
   unconditional.

The invariant-enabled set is stable during ledger application — invariant
registration/enabling happens during application setup, not during tx
processing. A check at stage-apply time accurately reflects whether any
invariant checking will occur.

For SAC benchmark with TX=6400: each successful Soroban tx calls
`setEffectsDeltaFromSuccessfulTx` which does `make_shared<InternalLedgerEntry>`
twice per modified entry (once for previous state, once for current state).
With ~200 modified entries per tx: 400 heap allocations × ~100ns + 400 entry
copies × ~200ns = ~120μs per tx × 6400 txs = ~768ms of delta-construction
overhead across all threads. Under T=8, per-thread cost is ~96ms, but
allocator contention under 8 concurrent threads amplifies this via malloc
lock contention.

## Trigger

Run `scripts/run_apply_load_matrix.py` with any scenario (invariants are
not enabled in benchmark configs). Profile `setEffectsDeltaFromSuccessfulTx`
time on worker threads. Compare total close time with and without the delta
construction skip.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:setEffectsDeltaFromSuccessfulTx:790-829` — builds LedgerTxnDelta with `make_shared` per modified entry
- `src/ledger/LedgerManagerImpl.cpp:checkAllTxBundleInvariants:2473-2514` — sole consumer of delta via `app.checkOnOperationApply`
- `src/invariant/InvariantManagerImpl.cpp:checkOnOperationApply:143-171` — iterates `mEnabled` (empty when invariants disabled)
- `src/invariant/InvariantManagerImpl.h` — add `hasEnabledInvariants()` method
- `src/ledger/LedgerManagerImpl.cpp:applySorobanStage:2517-2532` — capture invariant state before launching workers

## Evidence

- `fail/transaction-ledger/011` confirmed the inefficiency is real and the PoC passed all tests — it was rejected solely because the guard condition was wrong (`config.INVARIANT_CHECKS.empty()` vs runtime state)
- `InvariantManagerImpl::checkOnOperationApply` (line 148) iterates `mEnabled` — empty when invariants disabled → entire function is a no-op
- `setEffectsDeltaFromSuccessfulTx` (lines 803-804, 823-824) does `make_shared<InternalLedgerEntry>` — confirmed heap allocations on worker threads
- `InvariantManagerImpl::enableInvariant` is called during app setup (not during ledger apply) — the enabled set is stable during apply
- No invariants are enabled in the apply-load benchmark config — the delta construction is 100% wasted
- The `setLedgerChangesFromSuccessfulOp` meta-building call (TransactionMeta.cpp:398) independently reads the modified entry map WITHOUT using the delta — meta is not affected by skipping delta construction

## Anti-Evidence

- The prior hypothesis was rejected at final-review stage, and a reviewer might consider this a duplicate despite the different guard
- Adding a `hasEnabledInvariants()` method and threading the flag through to workers adds modest complexity
- Invariants could theoretically be enabled at runtime after the check, though this never happens during apply
- The impact estimate (~768ms total across all threads) may be optimistic — actual per-allocation cost depends on allocator (jemalloc/tcmalloc may be faster than ~100ns)
- Profiling may show the delta construction is a smaller fraction than estimated if VM execution dominates

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not a duplicate; the prior investigation (`fail/transaction-ledger/011`) was rejected because it used `config.INVARIANT_CHECKS.empty()` as the guard, which doesn't reflect runtime state. This hypothesis proposes the correct guard: `InvariantManager::hasEnabledInvariants()` checking the actual `mEnabled` vector.

### Trace Summary

Traced the complete code path from `TransactionFrame::parallelApply` (TransactionFrame.cpp:2241-2244) through `ThreadParallelApplyLedgerState::setEffectsDeltaFromSuccessfulTx` (ParallelApplyUtils.cpp:790-829), which builds a `LedgerTxnDelta` with `make_shared<InternalLedgerEntry>` allocations for each modified entry's previous and current state. The delta's sole consumer is `checkAllTxBundleInvariants` (LedgerManagerImpl.cpp:2474-2514), which calls `AppConnector::checkOnOperationApply` (AppConnector.cpp:77-84), which delegates to `InvariantManagerImpl::checkOnOperationApply` (InvariantManagerImpl.cpp:142-171) — a no-op when `mEnabled` is empty. Confirmed that `enableInvariant()` is only called during startup (`ApplicationImpl::enableInvariantsFromConfig`, line 1604-1609), in test setup, and in fuzzer setup — never during ledger apply. The `mEnabled` vector is therefore stable during the entire apply phase, making a pre-apply check safe.

### Code Paths Examined

- `src/invariant/InvariantManagerImpl.h:26` — `mEnabled` is `std::vector<std::shared_ptr<Invariant>>`, the authoritative runtime set
- `src/invariant/InvariantManagerImpl.cpp:215-276` — `enableInvariant()` modifies `mEnabled` via `push_back`; only called during setup
- `src/main/ApplicationImpl.cpp:1604-1609` — `enableInvariantsFromConfig()` calls `enableInvariant` for each config entry; called at line 323 during app construction
- `src/invariant/OrderBookIsNotCrossed.cpp:187-193` — `registerAndEnableInvariant()` calls `enableInvariant` — only from test code (line 117, 196 in tests) and fuzzer (FuzzerImpl.cpp:1540)
- `src/invariant/InvariantManagerImpl.cpp:142-171` — `checkOnOperationApply` iterates `mEnabled`; empty vector = no work
- `src/transactions/ParallelApplyUtils.cpp:790-829` — `setEffectsDeltaFromSuccessfulTx`: 2× `make_shared<InternalLedgerEntry>` per modified entry + `getLiveEntryOpt` lookup per entry
- `src/transactions/TransactionFrame.cpp:2241-2246` — Unconditionally calls `setEffectsDeltaFromSuccessfulTx` then `setLedgerChangesFromSuccessfulOp` for each successful Soroban tx on worker threads
- `src/ledger/LedgerManagerImpl.cpp:2474-2514` — `checkAllTxBundleInvariants`: sole consumer of delta, also calls `maybeSetRefundableFeeMeta` (meta-related, must remain unconditional)
- `src/ledger/LedgerManagerImpl.cpp:2380-2417` — `applyThread`: entry point for worker threads; `parallelApply` is called at line 2400-2402
- `src/ledger/LedgerManagerImpl.cpp:2517-2532` — `applySorobanStage`: orchestrates cluster apply then invariant check; natural place to capture the invariant-enabled flag via `mApp.getInvariantManager()`
- `src/invariant/InvariantManager.h:42` — `getEnabledInvariants()` exists but returns `vector<string>` (allocates); a new `hasEnabledInvariants()` returning bool is cleaner
- `src/main/AppConnector.h` — Does NOT expose `getInvariantManager()`; flag must be captured via `LedgerManagerImpl::mApp` (type `Application&`) and threaded through

### Findings

The inefficiency is **real and the proposed fix is correct**:

1. `setEffectsDeltaFromSuccessfulTx` runs unconditionally on worker threads for every successful Soroban tx, performing 2× `make_shared<InternalLedgerEntry>` plus `getLiveEntryOpt` per modified entry.
2. Its sole consumer (`checkOnOperationApply`) iterates `mEnabled` — a complete no-op when empty (default for validators and benchmarks).
3. The proposed guard (`InvariantManager::hasEnabledInvariants()`) checks the actual runtime `mEnabled` vector — directly fixing the rejection reason for the prior attempt which used the config-level `INVARIANT_CHECKS`.
4. `mEnabled` is only modified during app startup/test setup, never during ledger apply — the check is safe and accurate.
5. `setLedgerChangesFromSuccessfulOp` (meta building) independently calls `getLiveEntryOpt` for the same keys, making the delta's lookups fully redundant when meta is enabled.
6. `AppConnector` does not expose `getInvariantManager()`, so the flag must be captured via `LedgerManagerImpl::mApp` in `applySorobanStage` and threaded through `ParallelLedgerInfo` or a similar mechanism to worker threads.

**Severity downgraded to Informational** for the same reasons as the prior review:
- Per-tx delta overhead (~12-22μs for ~20 modified entries) is ~0.6-1.1% of typical VM execution (~2ms). The hypothesis's claim of "~200 modified entries per tx" is unrealistic for SAC transfers (which modify ~3-5 entries per tx).
- With T=8, each thread saves ~300μs out of ~50ms+ parallel phase wall time — well below the 5% threshold for Low severity.
- The savings are real but the magnitude is too small to produce a measurable benchmark improvement.

### PoC Guidance

- **Target code**:
  - `src/invariant/InvariantManager.h` — Add `virtual bool hasEnabledInvariants() const = 0;`
  - `src/invariant/InvariantManagerImpl.h` — Add `bool hasEnabledInvariants() const override;`
  - `src/invariant/InvariantManagerImpl.cpp` — Implement as `return !mEnabled.empty();`
  - `src/ledger/LedgerManagerImpl.cpp:applySorobanStage` (line 2517) — Capture `bool checkInvariants = mApp.getInvariantManager().hasEnabledInvariants();` and pass through `ParallelLedgerInfo` or as a separate parameter to `applySorobanStageClustersInParallel` → `applyThread` → `TransactionFrame::parallelApply`.
  - `src/transactions/TransactionFrame.cpp:2241-2244` — Guard `setEffectsDeltaFromSuccessfulTx` with the `checkInvariants` flag.
  - `src/ledger/LedgerManagerImpl.cpp:checkAllTxBundleInvariants` (line 2478-2504) — Guard the `setDeltaHeader` + `checkOnOperationApply` block with `checkInvariants`. Keep `maybeSetRefundableFeeMeta` (line 2511-2512) unconditional.
- **Change description**: Thread a `bool checkInvariants` (derived from `InvariantManager::hasEnabledInvariants()`) through the parallel apply call chain. When false, skip delta construction on worker threads and skip the invariant check loop on the main thread.
- **Correctness check**: All existing invariant tests explicitly enable invariants (via config or programmatic `enableInvariant()`), so `hasEnabledInvariants()` returns true and the delta path remains active. Run `[invariant]` tagged tests plus `[soroban]` and `[tx]` tagged tests.
- **Benchmark focus**: Measure `applySorobanStageClustersInParallel` wall time with and without the guard. Expected improvement: <5% (Informational). Focus on T=8 scenarios with SAC workload for maximum signal from allocator contention reduction.
