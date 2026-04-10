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
