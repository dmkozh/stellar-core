# H002: Parallel Merge Child Does Not Open an Extra SQL Transaction

**Date**: 2026-04-08
**Subsystem**: database
**Severity**: Medium
**Impact**: parallel apply overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If `GlobalParallelApplyLedgerState::commitChangesToLedgerTxn()` created a fresh
root-level SQL transaction, that would be redundant: the enclosing ledger close
should already have the only SQL transaction needed for offer-table updates.

## Mechanism

At first glance `LedgerTxn ltxInner(ltx)` looks suspicious because the default
constructor mode is `READ_WRITE_WITH_SQL_TXN`. But that mode only matters when
the parent is `LedgerTxnRoot`: `LedgerTxn::Impl` passes the mode to
`mParent.addChild(self, mode)`, while `LedgerTxn::addChild()` ignores the mode
and simply links the child; only `LedgerTxnRoot::Impl::addChild()` actually
checks out a DB session and opens `soci::transaction`.

## Trigger

Run any parallel-apply benchmark and inspect the final merge from
`GlobalParallelApplyLedgerState` back into the outer ledger transaction.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:389-458` — final merge creates `LedgerTxn ltxInner(ltx)` and commits it
- `src/ledger/LedgerTxn.cpp:427-462` — child `LedgerTxn` construction delegates to parent `addChild`
- `src/ledger/LedgerTxn.cpp:2816-2830` — only `LedgerTxnRoot::Impl::addChild()` opens the SQL transaction

## Evidence

The final merge wrapper clearly creates another `LedgerTxn`, so it is a plausible
place to suspect duplicate SQL begin/commit work.

## Anti-Evidence

The parent in this path is another `LedgerTxn`, not `LedgerTxnRoot`, so the mode
is ignored and no extra database transaction is created.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-08
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

Nested `LedgerTxn` children do not open SQL transactions; they only become
additional in-memory delta layers over the already-open outer ledger
transaction.

### Lesson Learned

When tracing database cost in `LedgerTxn`, distinguish carefully between child
construction under `LedgerTxnRoot` and child construction under another
`LedgerTxn`; only the former crosses into SOCI session management.
