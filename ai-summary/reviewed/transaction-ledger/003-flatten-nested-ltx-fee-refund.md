# H003: Flatten Nested LedgerTxn in processPostTxSetApply Fee Refund Path

**Date**: 2025-07-22
**Subsystem**: transaction-ledger (TransactionFrame, LedgerManagerImpl)
**Severity**: Low
**Impact**: Reduced LedgerTxn overhead in sequential post-apply phase
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When processing Soroban fee refunds after parallel apply, each transaction's
refund should require at most ONE LedgerTxn create/commit cycle. The refund
operation (load account, add balance, adjust feePool) is simple and does not
need nested isolation.

## Mechanism

`processPostTxSetApply` (LedgerManagerImpl.cpp:2827-2874) iterates all 3200
parallel-phase transactions sequentially. For each tx:

1. Creates `LedgerTxn ltxInner(ltx)` — **LedgerTxn #1**
2. Calls `processPostTxSetApply` → `processRefund` → `refundSorobanFee`
3. Inside `refundSorobanFee` (TransactionFrame.cpp:1045-1083): creates
   `LedgerTxn ltx(ltxOuter)` — **LedgerTxn #2** (nested inside #1)
4. Loads header and account, modifies balance and feePool
5. Commits **#2** into **#1**
6. Back in the outer loop: optionally captures changes, commits **#1**

This creates 3200 × 2 = 6400 LedgerTxn create/commit cycles for what is
essentially a simple balance update per transaction.

The inner LedgerTxn (#2) in `refundSorobanFee` exists to handle error cases
(account merged, liabilities preventing refund) — on error, the function
returns 0 without committing, rolling back partial state. However, these
error cases are rare (in the benchmark: never), and the rollback could be
handled by checking preconditions before modifying state.

The fix: refactor `refundSorobanFee` to accept the caller's LedgerTxn directly.
Check for the account's existence and liability clearance BEFORE loading the
header. If preconditions fail, return 0 without having modified any state.
This eliminates 3200 LedgerTxn create/commit cycles.

## Trigger

Any Soroban ledger close with fee refunds. In the apply-load benchmark:
- 3200 Soroban txs, each with a fee refund
- Each refund creates a nested LedgerTxn (#2) just for isolation
- 3200 unnecessary LedgerTxn create + commit = ~1-2ms overhead

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:processPostTxSetApply:2827-2874` — outer loop creating LedgerTxn #1 per tx
- `src/transactions/TransactionFrame.cpp:refundSorobanFee:1045-1083` — inner LedgerTxn #2 for error isolation
- `src/transactions/TransactionFrame.cpp:processRefund:2592-2615` — calls refundSorobanFee with ltxOuter

## Evidence

1. The nested LedgerTxn pattern (2 levels per refund) is visible at lines 2844 (outer) and 1061 (inner). Each LedgerTxn construction involves unordered_map initialization and parent registration.
2. LedgerTxn commit involves merging child entries into parent — for the inner LedgerTxn with ~2 modified entries (header + account), this is a hash map merge of 2 entries × 3200 txs = 6400 entry merges that could be avoided.
3. The error cases in `refundSorobanFee` (account merged at line 1066-1069, liabilities at line 1072-1076) are precondition checks. They can be performed before modifying state by loading the account read-only first.
4. In the benchmark, all 3200 txs have unique source accounts that exist and have no liabilities. The error paths are never taken, making the nested LedgerTxn pure overhead.

## Anti-Evidence

1. Each LedgerTxn create/commit cycle for a small transaction (2 entries: header + account) costs ~300ns. Total for 3200: ~0.96ms. This is <1% of a 100ms ledger close. The impact is measurable but small.
2. The nested LedgerTxn pattern provides clean error isolation without needing to manually track and revert state. Removing it adds complexity to the error handling logic.
3. The `processRefund` function is also called by `FeeBumpTransactionFrame` with a different fee source. Any refactoring must preserve this interface.
4. When `ledgerCloseMeta` is non-null (meta enabled), `ltxInner.getChanges()` captures per-tx fee changes. This requires a per-tx LedgerTxn (#1) regardless. Only #2 is eliminable.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete call chain from `LedgerManagerImpl::processPostTxSetApply` (line 2844) through `TransactionFrame::processPostTxSetApply` (line 2586) → `processRefund` (line 2604) → `refundSorobanFee` (line 1061). Confirmed the nested LedgerTxn #2 exists solely for error isolation. Verified that `addBalance` (TransactionUtils.cpp:592-623) does NOT modify state on failure — it computes `newBalance` locally and only writes to `acc.balance` on the success path (line 622). This means the error rollback provided by #2 is unnecessary since no state needs reverting on the error paths.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:processPostTxSetApply:2828-2874` — Confirmed outer loop creates LedgerTxn #1 per tx (line 2844), calls `processPostTxSetApply`, captures meta via `getChanges()` (line 2855), then commits.
- `src/transactions/TransactionFrame.cpp:processPostTxSetApply:2581-2587` — Thin wrapper that calls `processRefund` with `ltx` (which is the caller's #1).
- `src/transactions/TransactionFrame.cpp:processRefund:2592-2615` — Calls `refundSorobanFee` then emits fee event. Passes ltxOuter (which is #1) directly.
- `src/transactions/TransactionFrame.cpp:refundSorobanFee:1045-1083` — Creates nested LedgerTxn #2 (line 1061). Error paths at lines 1066-1069 (account merged) and 1072-1076 (liabilities) return 0 without committing.
- `src/transactions/TransactionUtils.cpp:addBalance:592-623` — For ACCOUNT type: computes newBalance locally (line 602), checks overflow (line 603), checks liabilities (lines 611-619), only writes `acc.balance = newBalance` (line 622) on success. Does NOT mutate state on failure.
- `src/transactions/FeeBumpTransactionFrame.cpp:processPostTxSetApply:220-227` — Also calls `mInnerTx->processRefund` with `getFeeSourceID()`. Same pattern, same nested #2 overhead.
- `src/ledger/LedgerTxn.cpp:Impl::Impl:427-438` — Constructor copies LedgerHeader (`make_unique<LedgerHeader>`) and calls `addChild`. For empty parent (#1 with no active entries), `addChild` iterates empty `mActive` set.
- `src/ledger/LedgerTxn.cpp:Impl::commitChild:588-618` — Copies child header again, iterates child entries via `updateEntry`, merges into parent. For 2 entries (header + account), this is a small map merge.

### Findings

**The inefficiency is real:** Each `refundSorobanFee` call creates a nested LedgerTxn (#2) that involves:
1. **Construction**: LedgerHeader deep copy (~200-300 bytes), `addChild` call (deactivates parent's active entries — empty at this point), unordered_map default initialization for mEntry/mActive.
2. **Commit**: Another LedgerHeader copy, iteration over ~2 child entries with hash map insertions into parent, worst-best-offer map merge (empty), restored entries merge (empty).

**The fix is correct:** The error paths in `refundSorobanFee` do not leave modified state:
- Account-merged path (line 1066-1069): `loadHeader` and `loadAccount` only load entries; no mutations occur before the null check.
- Liability path (line 1072-1076): `addBalance` returns false WITHOUT writing `acc.balance` — the local `newBalance` is discarded.
- Therefore, eliminating #2 and working directly on the parent LedgerTxn is safe — on error, no state needs reverting.

**Meta capture is unaffected:** `ltxInner.getChanges()` at line 2855 captures changes from #1. Whether those changes were made directly in #1 or committed from a child #2 is irrelevant — `getChanges()` sees the same result either way.

**FeeBumpTransactionFrame compatibility:** `FeeBumpTransactionFrame::processPostTxSetApply` (line 220-227) calls `mInnerTx->processRefund(app, ltx, getFeeSourceID(), ...)`. The refactored `refundSorobanFee` would work identically — it just operates on the passed LedgerTxn directly instead of creating a child.

**Impact is very small:** Per-tx overhead of ~300-500ns for the unnecessary LedgerTxn #2 create/commit. For 3200 txs: ~1-1.6ms. On a benchmark ledger close of ~100-200ms, this is <1.5%. This falls below the Low threshold (5-10%) and is best classified as Informational.

### PoC Guidance

- **Target code**: `src/transactions/TransactionFrame.cpp:refundSorobanFee:1045-1083` — Remove the nested `LedgerTxn ltx(ltxOuter)` at line 1061. Change all references to `ltx` to use `ltxOuter` directly. The function signature already accepts `AbstractLedgerTxn& ltxOuter`, so callers need no change.
- **Change description**: Remove lines 1061 and 1080 (`LedgerTxn ltx(ltxOuter)` and `ltx.commit()`). Replace `ltx.loadHeader()` with `ltxOuter.loadHeader()` and `loadAccount(ltx, ...)` with `loadAccount(ltxOuter, ...)`. The error-path semantics are preserved because `addBalance` does not mutate on failure and `loadAccount` returning null means no entry was created.
- **Correctness check**: Existing tests covering `refundSorobanFee` include Soroban transaction tests with fee refunds. The `[soroban]` and `[tx]` test tags cover this path. Also test with `FeeBumpTransactionFrame` to ensure the fee-bump refund path still works.
- **Benchmark focus**: Measure the post-tx-set-apply phase time (ZoneScoped in `processPostTxSetApply`). Expected improvement: ~1-1.5ms on the SAC benchmark with 3200 Soroban txs. This may not be visible above noise in end-to-end median/p99 metrics but should be detectable in Tracy flame graphs or micro-benchmarks of the post-apply phase.
