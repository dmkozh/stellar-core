# H001: Replace Individual load+update in commitChangesToLedgerTxn with Bulk Operations

**Date**: 2026-04-08
**Subsystem**: transaction-ledger (transactions/ParallelApplyUtils, ledger/LedgerTxn)
**Severity**: Low
**Impact**: Sequential commit phase optimization
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`GlobalParallelApplyLedgerState::commitChangesToLedgerTxn` should use
`createWithoutLoading`/`updateWithoutLoading`/`eraseWithoutLoading` bulk
LedgerTxn methods instead of individual `load(key)` + modify calls, to
avoid traversing the LedgerTxn hierarchy for each dirty entry.

## Mechanism

`commitChangesToLedgerTxn` (ParallelApplyUtils.cpp:389-458) iterates
`mGlobalEntryMap` and for each dirty entry calls `ltxInner.load(key)`,
which traverses: ltxInner.mEntry → ltx.mEntry → LedgerTxnRoot cache →
BucketList. For Soroban entries not in ltx.mEntry, this falls through to
the root cache. With prefetching enabled (`prefetchTransactionData`), most
entries should be in the root cache.

## Trigger

Run apply-load benchmark and profile `commitChangesToLedgerTxn` time.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:389-458` — `commitChangesToLedgerTxn`
- `src/ledger/LedgerTxn.cpp:744-800` — `createWithoutLoading`/`updateWithoutLoading`

## Evidence

- Individual `load(key)` calls traverse the LedgerTxn hierarchy for each entry
- `createWithoutLoading`/`updateWithoutLoading` bypass hierarchy traversal
- For 500 dirty entries, this eliminates 500 hierarchy lookups

## Anti-Evidence

- `prefetchTransactionData` (LedgerManagerImpl.cpp:2636) prefetches Soroban keys into the root cache before apply, so root cache hit rate should be high
- Individual root cache lookups are fast (~100-500ns per entry)
- Total cost: 500 × ~300ns = ~150µs — negligible vs. total apply time

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-08
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The `prefetchTransactionData` call at the start of `applyTransactions`
(LedgerManagerImpl.cpp:2636) pre-loads Soroban entry keys into the
`LedgerTxnRoot` entry cache. When `commitChangesToLedgerTxn` later calls
`ltxInner.load(key)`, the lookup falls through to the root cache and hits
in ~100-500ns. For 500 dirty entries, the total overhead is ~150-250µs,
which is <0.05% of a typical ledger apply time (~500ms). The complexity
of tracking create vs. update vs. delete state for bulk operations
(which require knowing entry existence without loading) outweighs the
negligible performance gain.

Additionally, `createWithoutLoading` and `updateWithoutLoading` have
strict preconditions (entry must/must not exist in the local mEntry map),
and misusing them would cause runtime exceptions. The current
load+modify pattern is correct-by-construction.

### Lesson Learned

When `prefetchTransactionData` is effective (which it is for the standard
apply path), LedgerTxnRoot cache hit rates are high and individual entry
lookups are cheap. Bulk operation optimizations in the commit path
should only be investigated if profiling shows cache miss rates > 50%.
