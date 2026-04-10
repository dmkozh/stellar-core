# H001: Main-DB Storestate Writes Keep Soroban-Only Ledgers on the SQL Commit Path

**Date**: 2026-04-10
**Subsystem**: database, ledger
**Severity**: Low
**Impact**: main-DB transaction overhead / parallel-apply tail latency
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When a ledger closes without touching any SQL-backed ledger entries, the main
database path should stay idle. In the apply-load SAC, custom-token, and
soroswap scenarios, the ledger-close hot path should not need to check out a
main-DB pool session, begin a main SQL transaction, and commit it merely to
persist restart metadata.

## Mechanism

Today, the only ledger entries still excluded from BucketListDB are `OFFER`
rows, but every close still writes `kHistoryArchiveState` and
`kLastClosedLedgerHeader` through `PersistentState::setMainState`, which binds
those keys to the main `storestate` table. That coupling forces
`LedgerTxnRoot::Impl::addChild()` to open a main-DB SQL transaction even on
Soroban-only ledgers; moving this metadata to a dedicated compact restart store
(for example the misc DB or a separate durable metadata file) would let the
main SQL transaction become conditional on actual `OFFER` mutations.

## Trigger

Run `scripts/run_apply_load_matrix.py` with any built-in scenario on the default
on-disk SQLite database. The workload mutates Soroban state but not `OFFER`
entries, yet the ledger-close path still opens and commits the main SQL
transaction because restart metadata is persisted in the main DB.

## Target Code

- `src/bucket/LiveBucketIndex.cpp:typeNotSupported:22-25` — only `OFFER` remains outside BucketListDB
- `src/main/PersistentState.h:Entry:24-40` — `kLastClosedLedgerHeader` and `kHistoryArchiveState` are classified as main-state entries
- `src/main/PersistentState.cpp:setMainState/updateDb:169-180,281-318` — both metadata values are written through the generic main `storestate` path
- `src/ledger/LedgerManagerImpl.cpp:storePersistentStateAndLedgerHeaderInDB:2900-2940` — every ledger writes those two main-state rows on the root session
- `src/ledger/LedgerTxn.cpp:LedgerTxnRoot::Impl::addChild/commitChild:2816-2830,2958-2959` — main SQL transaction is opened and committed for ledger close

## Evidence

The earlier `fail/003` record showed that the main SQL transaction cannot be
skipped today because these two metadata writes execute on the same root
session. The code now makes the structural blocker explicit: Soroban-ledger
entry persistence is already BucketListDB-backed, but restart metadata is still
hard-wired to the main SQL store.

## Anti-Evidence

This is a broader redesign than a local micro-optimization: startup recovery
currently expects both values in `storestate`, so a viable fix needs a schema or
recovery-path migration. The total gain also depends on how much of the current
main-DB cost is the transaction itself versus the metadata encoding work.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — distinct approach from fail/003, fail/004, fail/005, fail/007 (proposes structural relocation rather than skipping or deferring), but targets the same cost that those investigations already quantified as negligible
**Failed At**: reviewer

### Trace Summary

Traced the complete Soroban-only ledger close SQL path. Confirmed that on such ledgers, the main SQL transaction's only writes are two `UPDATE storestate` rows (HAS and LCLH) via `storePersistentStateAndLedgerHeaderInDB()`. The `commitChild()` entry iterator scans all modified entries but accumulates nothing (no OFFERs). The COMMIT then fsyncs the WAL (synchronous=FULL). The total per-ledger cost of this entire SQL path has been quantified by prior investigations at 0.5–2ms, representing 0.1–2% of Soroban ledger close time. Moving metadata elsewhere would shift, not eliminate, the write cost.

### Code Paths Examined

- `src/ledger/LedgerTxn.cpp:2816-2831` — `LedgerTxnRoot::Impl::addChild()` unconditionally checks out pool session and opens `soci::transaction` for `READ_WRITE_WITH_SQL_TXN` mode
- `src/ledger/LedgerManagerImpl.cpp:2901-2947` — `storePersistentStateAndLedgerHeaderInDB()` constructs HAS from BucketList, serializes to JSON, writes two `UPDATE storestate` rows via `PersistentState::setMainState()` on the pool session
- `src/main/PersistentState.cpp:281-318` — `updateDb()` executes `UPDATE storestate SET state = :v WHERE statename = :n` per metadata key
- `src/ledger/LedgerTxn.cpp:2918-2965` — `commitChild()` iterates all entries via `BulkLedgerEntryChangeAccumulator::accumulate()`, which rejects all non-OFFER types (lines 2877-2894); for Soroban-only ledgers, zero entries are accumulated, zero SQL upserts/deletes occur
- `src/ledger/LedgerTxn.cpp:2958-2959` — `mTransaction->commit()` commits only the two storestate UPDATEs; fsync cost is 50–200μs on NVMe SSD per fail/007 analysis
- `src/database/Database.cpp:163-166` — WAL mode enabled, synchronous=FULL (NORMAL commented out), confirming per-commit fsync
- `src/history/HistoryManagerImpl.cpp:288-343` — `maybeQueueHistoryCheckpoint()` writes to checkpoint file (not SQL), so it does not add SQL work to the transaction

### Why It Failed

The inefficiency is real but its magnitude is too small to produce a measurable benchmark improvement. Prior quantified analyses establish the bounds:

1. **fail/005** estimated total metadata persistence cost at 0.5–2ms per ledger (0.1–2% of 100–500ms Soroban close time), well below the 5% threshold for Low severity.

2. **fail/007** measured the per-commit fsync cost at 50–200μs on NVMe SSD, confirming the transaction overhead itself is sub-millisecond.

3. **Moving metadata to misc DB or a separate file does not eliminate the cost — it shifts it.** The two metadata UPDATEs plus their durable COMMIT must still occur somewhere. If moved to the misc DB, that store would need its own BEGIN + 2 UPDATEs + COMMIT with fsync, yielding near-zero net savings. The only true savings would be the entry iteration (microseconds for ~2000 entries with simple type checks) and the pool session checkout/return (also microseconds).

4. **Atomicity risk for mixed ledgers.** Ledgers that modify both Soroban state and OFFERs would need metadata and OFFER changes to remain crash-consistent. Splitting them across two stores introduces a two-phase commit problem — metadata in misc DB committed but OFFER changes in main DB not committed (or vice versa) — requiring additional coordination for negligible benefit.

5. **Significant implementation scope.** The fix requires schema migration (new misc DB tables or file format), recovery path changes (startup must read from new location), and conditional transaction logic in `addChild()`/`commitChild()` — substantial complexity for a saving that's unmeasurable against ±5–10% benchmark variance.

### Lesson Learned

The main SQL transaction cost for Soroban-only ledgers has now been analyzed from multiple angles (lazy skip in fail/003, session routing in fail/004, benchmark-mode skip in fail/005, fsync tuning in fail/007, and structural relocation here). All analyses converge on the same conclusion: the total SQL path cost per Soroban-only ledger is 0.5–2ms, dominated by the two storestate UPDATEs and their COMMIT fsync. This cost is a fixed O(1)-per-ledger overhead that cannot reach the 5% improvement threshold against the O(n)-per-transaction Soroban execution cost that dominates close time. Further database-subsystem hypotheses should target different cost centers.
