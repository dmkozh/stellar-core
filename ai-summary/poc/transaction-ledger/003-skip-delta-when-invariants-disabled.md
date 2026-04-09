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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete data flow from `TransactionFrame::applySorobanTransaction` (line 2243) through `ThreadParallelApplyLedgerState::setEffectsDeltaFromSuccessfulTx` (ParallelApplyUtils.cpp:790-829) which builds a `LedgerTxnDelta` with `make_shared<InternalLedgerEntry>` allocations, to the sole consumer `checkAllTxBundleInvariants` (LedgerManagerImpl.cpp:2473-2514), which passes the delta to `InvariantManagerImpl::checkOnOperationApply` (line 143-171). Confirmed that when `mEnabled` is empty (default config: `INVARIANT_CHECKS = {}`), the invariant check iterates nothing. Also confirmed that `setLedgerChangesFromSuccessfulOp` (TransactionMeta.cpp:385-420) independently calls `getLiveEntryOpt` for the same keys, making the delta's lookups redundant when meta is enabled.

### Code Paths Examined

- `src/transactions/TransactionFrame.cpp:2241-2247` — Unconditionally calls `setEffectsDeltaFromSuccessfulTx` then `setLedgerChangesFromSuccessfulOp` for every successful Soroban tx; both iterate `res.getModifiedEntryMap()` and call `getLiveEntryOpt`.
- `src/transactions/ParallelApplyUtils.cpp:790-829` — `setEffectsDeltaFromSuccessfulTx`: for each modified entry, calls `getLiveEntryOpt(lk)`, then does 2x `make_shared<InternalLedgerEntry>` (previous + current), then inserts into `mDelta.entry` unordered_map.
- `src/transactions/ParallelApplyUtils.cpp:699-735` — `getLiveEntryOpt`: checks `mThreadEntryMap` (preloaded hash map, fast), then falls through to `InMemorySorobanState` or `mLCLSnapshot`. For typical Soroban entries this is a hash map hit ~100-200ns.
- `src/transactions/ParallelApplyStage.h:19-56` — `TxEffects` class: owns `LedgerTxnDelta mDelta` alongside `TransactionMetaBuilder mMeta`. The delta is populated on worker threads, consumed on main thread.
- `src/ledger/LedgerManagerImpl.cpp:2473-2514` — `checkAllTxBundleInvariants`: iterates all txBundles in a stage, calls `setDeltaHeader` and `app.checkOnOperationApply` (the delta consumer), then `maybeSetRefundableFeeMeta` (meta-related, not delta-dependent).
- `src/invariant/InvariantManagerImpl.cpp:143-171` — `checkOnOperationApply`: iterates `mEnabled` vector. Empty when `INVARIANT_CHECKS={}` (default).
- `src/main/Config.cpp:350` — `INVARIANT_CHECKS = {}` by default; validators never enable invariants.
- `src/ledger/LedgerManagerImpl.cpp:2645-2649` — In the apply-load benchmark (BUILD_TESTS), `enableTxMeta` is forced true, so `setLedgerChangesFromSuccessfulOp` always runs its own `getLiveEntryOpt` calls — meaning the delta's lookups are fully redundant in the benchmark.
- `src/transactions/TransactionMeta.cpp:385-420` — `setLedgerChangesFromSuccessfulOp`: independently iterates `res.getModifiedEntryMap()`, calls `threadState.getLiveEntryOpt(lk)` for each entry, copies entries into `LedgerEntryChanges` vector (not shared_ptrs).

### Findings

The inefficiency is **real and confirmed**:
1. `setEffectsDeltaFromSuccessfulTx` runs unconditionally on worker threads for every successful Soroban tx.
2. Its sole consumer (`checkOnOperationApply`) is a no-op when invariants are disabled (default).
3. The `getLiveEntryOpt` calls within the delta function are fully redundant with those in `setLedgerChangesFromSuccessfulOp` (which runs immediately after on the same thread when meta is enabled).
4. The fix (guarding on invariant enablement) is clean, correct, and does not affect any other consumer.

However, **the severity is downgraded to Informational** because:
- The per-tx delta overhead (~12-22µs for 20 entries) is 0.6-1.1% of typical VM execution (~2ms). Even with 8-thread malloc contention (2-3x amplification), this reaches only 1.5-3.3%.
- In the apply-load benchmark, `enableTxMeta` is always true (BUILD_TESTS forces it at LedgerManagerImpl.cpp:2649), so `setLedgerChangesFromSuccessfulOp` runs its own `getLiveEntryOpt` lookups regardless — only the `make_shared` allocations and unordered_map insertions are saved.
- For 200 txs × 20 entries: savings are ~2.4ms of serial allocation time. With T=8, each thread saves ~300µs out of ~50ms+ parallel phase wall time — well below the 5% threshold for Low.
- The claimed 5-10% improvement would require either (a) very lightweight Soroban txs with many modified entries or (b) severe malloc contention, neither of which is typical of the benchmark scenarios.

### PoC Guidance

- **Target code**:
  - `src/transactions/TransactionFrame.cpp:2243-2244` — Guard the `setEffectsDeltaFromSuccessfulTx` call with an `enableInvariantChecks` bool parameter (passed from `applySorobanStage` through `applySorobanStageClustersInParallel` → `applyThread` → `applySorobanTransaction`).
  - `src/ledger/LedgerManagerImpl.cpp:2488-2498` — Guard the `setDeltaHeader` + `checkOnOperationApply` block inside `checkAllTxBundleInvariants` with a similar check (e.g., `!config.INVARIANT_CHECKS.empty()` or a pre-computed bool from `InvariantManager::getEnabledInvariants().empty()`).
  - Note: `maybeSetRefundableFeeMeta` on line 2511-2512 must NOT be guarded — it is meta-related, not invariant-related.
- **Change description**: Thread a `bool enableInvariantChecks` (derived from `!config.INVARIANT_CHECKS.empty()`) through the parallel apply call chain. When false, skip delta construction on worker threads and skip the invariant check loop on the main thread.
- **Correctness check**: All existing invariant tests explicitly enable invariants via `INVARIANT_CHECKS` config, so they will continue to construct deltas. Run `[invariant]` tagged tests plus the Soroban parallel apply tests (`[soroban]`, `[tx]`).
- **Benchmark focus**: Measure `applySorobanStageClustersInParallel` wall time with and without the guard. Expected improvement: <5% (Informational). Focus on scenarios with high entry-count txs (20+ modified entries) for maximum signal.

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-09
**PoC by**: claude-opus-4-6, high

### Changes Made

- **`src/transactions/TransactionFrame.cpp:2243-2250`** — Wrapped the `threadState.setEffectsDeltaFromSuccessfulTx()` call in `parallelApply()` with a guard checking `!config.INVARIANT_CHECKS.empty()`. When invariants are disabled (default), the expensive `LedgerTxnDelta` construction with `make_shared<InternalLedgerEntry>` heap allocations is skipped entirely on worker threads. The subsequent `setLedgerChangesFromSuccessfulOp` call (which builds tx meta independently) remains unconditional.

- **`src/ledger/LedgerManagerImpl.cpp:2478-2516`** — Added a `bool const checkInvariants = !config.INVARIANT_CHECKS.empty()` guard at the top of `checkAllTxBundleInvariants()`. The invariant check block (`setDeltaHeader` + `checkOnOperationApply`) is now conditional on this flag. The `maybeSetRefundableFeeMeta` call remains unconditional as it is meta-related, not invariant-related.

### Demonstration

When `INVARIANT_CHECKS` is empty (the default for validators and benchmarks), the optimization eliminates all `make_shared<InternalLedgerEntry>` heap allocations and `getLiveEntryOpt` lookups performed by `setEffectsDeltaFromSuccessfulTx` on worker threads. For a ledger with 200 Soroban txs each modifying 20 entries, this saves ~8000 heap allocations and ~8000 entry copies across all worker threads, reducing allocator contention under T=8 parallel execution. When invariants ARE enabled (test configs), the code path is unchanged — delta construction and invariant checking proceed as before.

### Test Results

- All 40 `[invariant]` tests pass (40,677 assertions) — confirms invariant checking still works when enabled via config
- All 109 `[soroban]` tests pass (3,478,645 assertions) — confirms parallel Soroban apply is unbroken
- All 124 `[tx]` tests pass (558,956 assertions) — confirms transaction processing is correct
- Full `make check` suite passes (all partitions)
