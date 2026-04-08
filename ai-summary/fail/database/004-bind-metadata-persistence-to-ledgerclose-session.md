# H002: Metadata Persistence Escapes the Open ledgerClose SQL Transaction

**Date**: 2026-04-08
**Subsystem**: database
**Severity**: Medium
**Impact**: write amplification / parallel apply scalability
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Per-ledger persistence of `kHistoryArchiveState` and
`kLastClosedLedgerHeader` should reuse the SQL session that is already open for
the enclosing ledger-close transaction. In parallel-ledger-close mode, those
writes should piggyback on the already-open `ledgerClose` session instead of
creating separate autocommit work on the root main-thread session.

## Mechanism

`sealLedgerTxnAndStoreInBucketsAndDB()` calls
`storePersistentStateAndLedgerHeaderInDB()` before the outer `ltx.commit()`, so
there is already an active ledger-close SQL transaction. But
`storePersistentStateAndLedgerHeaderInDB()` fetches
`mApp.getLedgerTxnRoot().getSession()` directly and performs two `setMainState()`
updates there; under `parallelLedgerClose()` the outer transaction is on a
different pooled `ledgerClose` session, so these metadata writes fall out of the
existing transaction and become extra commits.

## Trigger

Run any apply-load benchmark scenario on the default on-disk SQLite database.
`PARALLEL_LEDGER_APPLY` is forced on by `runApplyLoad()`, which makes
`parallelLedgerClose()` true and routes the outer ledger `LedgerTxn` through a
pooled `ledgerClose` session.

## Target Code

- `src/main/Config.cpp:2615-2618` — `parallelLedgerClose()` is true whenever parallel apply is enabled on a non-memory DB
- `src/ledger/LedgerTxn.cpp:2822-2830` — root ledger close uses pooled `ledgerClose` session + SQL transaction
- `src/ledger/LedgerManagerImpl.cpp:1717-1720,1826` — metadata persistence happens before the outer ledger transaction commits
- `src/ledger/LedgerManagerImpl.cpp:2891-2930` — persistence helper pulls `getLedgerTxnRoot().getSession()` and performs two `setMainState()` writes
- `src/main/PersistentState.cpp:168-180,281-319` — each `setMainState()` becomes an `UPDATE` statement on that session

## Evidence

The call order shows the metadata writes are issued while the outer ledger-close
transaction is still pending, but they are not passed the outer session. In the
benchmark configuration this means every closed ledger pays at least two extra
storestate writes that cannot be batched with the already-open `ledgerClose`
transaction.

## Anti-Evidence

When `parallelLedgerClose()` is false, the root session and the outer
transaction session are the same object, so the issue collapses to a no-op.
This hypothesis is specifically about the parallel-ledger-close path exercised
by apply-load.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (related to fail/003 but distinct claim)
**Failed At**: reviewer

### Trace Summary

The hypothesis claims that `storePersistentStateAndLedgerHeaderInDB()` uses a
different SQL session than the one carrying the open ledger-close transaction,
causing the metadata writes to "fall out" as separate autocommit operations.
Tracing the actual code path shows this is incorrect: `LedgerTxnRoot::Impl::getSession()`
returns the pool session (`*mSession`) whenever it is set, which is exactly the
session that `addChild()` created and opened the `soci::transaction` on.
Therefore the metadata writes are already inside the existing transaction.

### Code Paths Examined

- `src/ledger/LedgerTxn.cpp:2816-2831` — `LedgerTxnRoot::Impl::addChild()` creates `mSession` from pool (line 2826-2827) and opens `soci::transaction` on `getSession()` (line 2829-2830)
- `src/ledger/LedgerTxn.cpp:2778-2786` — `LedgerTxnRoot::Impl::getSession()` returns `*mSession` when set (line 2781-2783), falls back to `mApp.getDatabase().getSession()` only when `mSession` is null
- `src/ledger/LedgerManagerImpl.cpp:3051-3099` — `sealLedgerTxnAndStoreInBucketsAndDB()` calls `storePersistentStateAndLedgerHeaderInDB()` at line 3095, while the child `LedgerTxn` is still active (so `mSession` is still set)
- `src/ledger/LedgerManagerImpl.cpp:2891-2930` — `storePersistentStateAndLedgerHeaderInDB()` calls `mApp.getLedgerTxnRoot().getSession()` at line 2897, which returns the pool session
- `src/main/PersistentState.cpp:168-180` — `setMainState()` receives the session by reference and forwards it to `updateDb()`
- `src/main/PersistentState.cpp:280-319` — `updateDb()` executes `UPDATE storestate` on the passed session, which is the pool session with the active transaction

### Why It Failed

The inefficiency does not exist. The hypothesis incorrectly assumes that
`getLedgerTxnRoot().getSession()` returns the main database session rather than
the pool session. In reality, `LedgerTxnRoot::Impl::getSession()` checks
`if (mSession)` first and returns `*mSession` — the pool session created by
`addChild()` — which is the same session carrying the open `soci::transaction`.
The two `setMainState()` writes for `kHistoryArchiveState` and
`kLastClosedLedgerHeader` execute within this transaction and are committed
together with the ledger entry changes when `ltx.commit()` is called at line 1826.
There are no extra autocommit writes.

### Lesson Learned

`LedgerTxnRoot::getSession()` is a single unified accessor that returns the pool
session when one is active (parallel ledger close) or the main database session
otherwise. Code that calls `getLedgerTxnRoot().getSession()` automatically
participates in whatever SQL transaction is currently open on the root, regardless
of whether it's a pooled or main-thread session. Don't assume that "fetching the
session from root" means "using a different session than the transaction."
