# H004: Parallel apply still builds invariant deltas with invariants disabled

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Medium
**Impact**: per-entry allocation overhead / unnecessary bookkeeping
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If `INVARIANT_CHECKS` is empty, successful Soroban transactions should commit their ledger changes without materializing `LedgerTxnDelta` copies solely for invariant evaluation. The apply path should avoid both worker-side delta construction and the follow-up main-thread invariant walk when there is nothing enabled to consume that data.

## Mechanism

`TransactionFrame::parallelApply` always calls `threadState.setEffectsDeltaFromSuccessfulTx(...)` on success, and that function allocates `InternalLedgerEntry` snapshots for previous/current values of every modified key. Later `LedgerManagerImpl::checkAllTxBundleInvariants` still iterates every tx bundle and calls `app.checkOnOperationApply(...)`, even though the benchmark config leaves `INVARIANT_CHECKS` at its default empty value and `InvariantManagerImpl` will just loop over an empty vector. Gating the delta build and invariant pass on "any invariant enabled" would remove avoidable allocations in workers and a useless serial walk on the main thread.

## Trigger

Run any apply-load benchmark with the default template config and profile allocations in `setEffectsDeltaFromSuccessfulTx` plus main-thread time in `checkAllTxBundleInvariants`. This should reproduce on all six matrix scenarios because none of them enable invariants.

## Target Code

- `src/transactions/TransactionFrame.cpp:TransactionFrame::parallelApply:2191-2247` - always populates tx effects for successful parallel txs
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::setEffectsDeltaFromSuccessfulTx:790-829` - allocates per-entry previous/current snapshots
- `src/ledger/LedgerManagerImpl.cpp:LedgerManagerImpl::checkAllTxBundleInvariants:2474-2513` - serial invariant walk over every tx bundle
- `src/ledger/LedgerManagerImpl.cpp:LedgerManagerImpl::applySorobanStage:2516-2532` - invariant pass is unconditional in the stage flow
- `src/main/Config.cpp:Config defaults:338-351` - `INVARIANT_CHECKS = {}`
- `src/main/ApplicationImpl.cpp:ApplicationImpl::enableInvariantsFromConfig:1604-1610` - only configured names are enabled

## Evidence

The benchmark template does not set `INVARIANT_CHECKS`, and config defaults initialize it to an empty vector. Despite that, the parallel apply path still constructs `LedgerTxnDelta` data for each successful tx and still invokes the invariant manager pass afterward; with no enabled invariants, that work has no consumer in the benchmarked configuration.

## Anti-Evidence

This optimization must not affect tests or deployments that deliberately enable invariants, and any interface used by metadata or post-apply logic has to remain available on those paths. The opportunity only exists because apply-load runs with the default empty invariant set.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — duplicate of ai-summary/reviewed/transaction-ledger/003-skip-delta-when-invariants-disabled.md
**Failed At**: reviewer

### Trace Summary

This hypothesis describes the exact same mechanism as H003 ("Skip LedgerTxnDelta Construction When Invariant Checking Is Disabled"), which was already reviewed as VIABLE (Informational) in `ai-summary/reviewed/transaction-ledger/003-skip-delta-when-invariants-disabled.md`. Both identify: (1) unconditional `setEffectsDeltaFromSuccessfulTx` on worker threads building `LedgerTxnDelta` with `make_shared<InternalLedgerEntry>` allocations, (2) sole consumption by `checkAllTxBundleInvariants` → `InvariantManagerImpl::checkOnOperationApply` which no-ops when `INVARIANT_CHECKS` is empty.

### Code Paths Examined

- `src/transactions/TransactionFrame.cpp:2241-2247` — Same unconditional `setEffectsDeltaFromSuccessfulTx` call identified in H003
- `src/transactions/ParallelApplyUtils.cpp:790-829` — Same delta construction with `make_shared` allocations identified in H003
- `src/ledger/LedgerManagerImpl.cpp:2473-2514` — Same `checkAllTxBundleInvariants` consumer identified in H003
- `src/invariant/InvariantManagerImpl.cpp:143-171` — Same empty `mEnabled` iteration identified in H003

### Why It Failed

This is a duplicate of H003 which was already reviewed and marked VIABLE (Informational) with full PoC guidance. The target code, mechanism, proposed fix, and impact analysis are substantially identical. H003 additionally provided a more thorough severity analysis showing the actual impact is below the 5% Low threshold.

### Lesson Learned

Cross-subsystem directory naming varies from skill names (e.g., "transaction-ledger" vs "transactions"). Duplicate checks should scan all subsystem directories under `reviewed/` and `fail/`, not just the exact subsystem name from the hypothesis.
