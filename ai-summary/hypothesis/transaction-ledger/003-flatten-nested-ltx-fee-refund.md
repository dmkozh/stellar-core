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
