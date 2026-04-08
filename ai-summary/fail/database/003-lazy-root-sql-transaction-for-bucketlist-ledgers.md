# H001: Lazy Root SQL Transaction for BucketListDB-Only Ledgers

**Date**: 2026-04-08
**Subsystem**: database
**Severity**: Medium
**Impact**: parallel apply overhead / write-path CPU+I/O
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When a ledger closes without touching any SQL-backed ledger-entry types, the
root `LedgerTxn` should not acquire a database pool session or open a SOCI SQL
transaction at all. In the apply-load SAC, custom-token, and soroswap
benchmarks, ledger mutations should stay on the BucketListDB / in-memory
Soroban path unless an `OFFER` row is actually created, updated, or deleted.

## Mechanism

`LedgerManagerImpl` creates the top-level ledger `LedgerTxn` eagerly, and
`LedgerTxnRoot::Impl::addChild()` immediately opens a pooled session plus a SQL
transaction whenever `parallelLedgerClose()` is enabled. But
`LiveBucketIndex::typeNotSupported()` says only `OFFER` still lives in SQL, so
Soroban-only apply-load ledgers pay session checkout and begin/commit overhead
even when the SQL side stays completely idle.

## Trigger

Run `scripts/run_apply_load_matrix.py` against the default on-disk benchmark
configs (`sac`, `custom_token`, `soroswap`, `T=1` or `T=8`). Each ledger close
constructs a root `LedgerTxn`, but the workload mutates Soroban
`CONTRACT_DATA`/`CONTRACT_CODE`/`TTL` state rather than `OFFER`s.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:1478,1639-1720,1826` — top-level ledger close keeps a root `LedgerTxn` open across apply/commit
- `src/ledger/LedgerTxn.cpp:2816-2830` — `LedgerTxnRoot::Impl::addChild()` eagerly checks out `getPool()` and starts `soci::transaction`
- `src/bucket/LiveBucketIndex.cpp:22-25` — only `OFFER` is still marked unsupported by BucketListDB
- `src/ledger/LedgerTxn.cpp:2877-2894,2918-2959` — SQL-side commit path only accumulates offer changes

## Evidence

The benchmark harness always enables `PARALLEL_LEDGER_APPLY`, and
`Config::parallelLedgerClose()` only requires that plus a non-memory database,
so the eager pooled-session path is active for the benchmark. The root commit
path itself confirms that only `OFFER` changes are ever emitted to SQL, making
the transaction setup pure overhead on Soroban-only ledgers.

## Anti-Evidence

Classic ledgers that actually modify `OFFER` entries still need the existing SQL
transaction semantics, so the optimization has to be conditional rather than a
global removal of root SQL transactions.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

The hypothesis claims the SQL transaction opened by `LedgerTxnRoot::Impl::addChild()`
is pure overhead on Soroban-only ledgers because only OFFER entries go through the
SQL commit path. However, tracing the full ledger close path reveals that the pool
session and its enclosing SQL transaction are actively used for critical persistent
state writes (HistoryArchiveState and LastClosedLedgerHeader) on every ledger close,
regardless of whether any OFFERs are modified.

### Code Paths Examined

- `src/ledger/LedgerTxn.cpp:2816-2830` — `LedgerTxnRoot::Impl::addChild()` checks out pool session and opens `soci::transaction` when `parallelLedgerClose()` is true
- `src/ledger/LedgerTxn.cpp:2779-2786` — `LedgerTxnRoot::Impl::getSession()` returns `*mSession` (the pool session) when it exists
- `src/ledger/LedgerManagerImpl.cpp:1718` — `sealLedgerTxnAndStoreInBucketsAndDB()` called before `ltx.commit()`
- `src/ledger/LedgerManagerImpl.cpp:3095` — calls `storePersistentStateAndLedgerHeaderInDB(lh, true)`
- `src/ledger/LedgerManagerImpl.cpp:2891-2937` — `storePersistentStateAndLedgerHeaderInDB()` obtains session via `mApp.getLedgerTxnRoot().getSession()` (which returns the pool session), then executes SQL UPDATEs for `kHistoryArchiveState` (line 2925-2926) and `kLastClosedLedgerHeader` (line 2929-2930)
- `src/main/PersistentState.cpp:281-296` — `updateDb()` executes a real `UPDATE storestate SET state = :v WHERE statename = :n` SQL statement on the pool session
- `src/ledger/LedgerTxn.cpp:2958-2959` — `mTransaction->commit()` commits these writes

### Why It Failed

The hypothesis's core claim — that "the SQL side stays completely idle" for
Soroban-only ledgers — is false. While `BulkLedgerEntryChangeAccumulator::accumulate()`
correctly filters out non-OFFER entries (so no ledger entry SQL upserts/deletes occur),
the same pool session and SQL transaction are actively used by
`storePersistentStateAndLedgerHeaderInDB()` to write the HistoryArchiveState and
LastClosedLedgerHeader into the `storestate` table. These writes happen on every
single ledger close and are essential for crash recovery. The SQL transaction
protects their atomicity and cannot be removed or made lazy without restructuring
how persistent state is stored during ledger close.

### Lesson Learned

When evaluating SQL transaction overhead in the ledger close path, don't only
examine `BulkLedgerEntryChangeAccumulator` (which handles ledger entry types).
The same root `LedgerTxn` session is also used by `storePersistentStateAndLedgerHeaderInDB()`
for persistent state writes that occur unconditionally on every ledger close.
The SQL transaction wraps more than just OFFER upserts/deletes.
