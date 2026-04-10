# H004: Ledger Close Persists Two Singleton Keys Through Two Full Generic SQL Write Cycles

**Date**: 2026-04-10
**Subsystem**: database, ledger
**Severity**: Informational
**Impact**: SQL dispatch overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The ledger-close hot path should persist its two fixed restart values through a
single specialized write path. Writing `historyarchivestate` and
`lastclosedledgerheader` should not require two separate trips through the
generic key/value `setMainState()` / `updateDb()` machinery on every ledger.

## Mechanism

`storePersistentStateAndLedgerHeaderInDB()` calls `setMainState()` twice in a
row for the same table and session. Each call goes through the generic
`PersistentState::updateDb()` path, which formats SQL text, allocates a fresh
SOCI statement via `Database::getPreparedStatement()`, prepares it, binds the
key/value strings, executes, and tears the statement back down. A dedicated
batched ledger-close metadata write (for example a single-row table or one
multi-value UPSERT) would cut per-ledger SQL dispatch and allocator churn in
half while preserving transactional semantics.

## Trigger

Run `scripts/run_apply_load_matrix.py` with default settings. Every benchmark
ledger executes the back-to-back `setMainState()` calls in
`storePersistentStateAndLedgerHeaderInDB()`.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:storePersistentStateAndLedgerHeaderInDB:2935-2940` — two sequential metadata writes every ledger
- `src/main/PersistentState.cpp:setMainState/updateDb:169-180,281-318` — each write goes through the generic singleton-key update path
- `src/database/Database.cpp:getPreparedStatement:758-765` — every call allocates and prepares a fresh SOCI statement
- `src/main/PersistentState.cpp:kSQLCreateStatement:32-36` — current schema is a generic key/value table rather than a dedicated ledger-close metadata row

## Evidence

The hot path writes the same two singleton keys every ledger close, but the code
treats them as arbitrary key/value entries and pays two independent statement
lifecycles. The database transaction is already open on the root session, so a
batched write path can be introduced without changing correctness boundaries.

## Anti-Evidence

There are only two writes per ledger, so the upside may still be modest relative
to Soroban execution. A more specialized schema or UPSERT path also increases
migration and codepath complexity compared to the current generic helper.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — substantially equivalent to the storestate persistence cost center exhaustively analyzed in fail/005, fail/013, fail/014, and fail/015
**Failed At**: reviewer

### Trace Summary

Traced the two `setMainState()` calls in `storePersistentStateAndLedgerHeaderInDB()` (lines 2935–2940). Each call routes through `PersistentState::updateDb()` (lines 281–318), which calls `Database::getPreparedStatement()` (lines 759–765) to allocate a `shared_ptr<soci::statement>`, call `alloc()` and `prepare(query)`, then binds two parameters and executes `UPDATE storestate SET state = :v WHERE statename = :n`. The hypothesis proposes merging these two statement lifecycles into one. The savings would be one heap allocation (~50ns), one SQLite statement prepare (~1–5μs), and one execute round-trip (~50–200μs) — totaling at most ~200μs per ledger.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:2901-2948` — `storePersistentStateAndLedgerHeaderInDB()`: two sequential `setMainState()` calls at lines 2935 and 2939, both using the same session from `getLedgerTxnRoot().getSession()`
- `src/main/PersistentState.cpp:169-180` — `setMainState()` validates entry type, forwards to `updateDb()` with session and table name
- `src/main/PersistentState.cpp:281-318` — `updateDb()` allocates a fresh SOCI statement via `getPreparedStatement()`, binds key/value, executes UPDATE; falls back to INSERT if no rows affected
- `src/database/Database.cpp:759-765` — `getPreparedStatement()` creates `make_shared<soci::statement>`, calls `alloc()` and `prepare(query)` — this is the "fresh statement" allocation the hypothesis correctly identifies
- `src/ledger/LedgerTxn.cpp:2778-2786` — `getSession()` returns the pool session carrying the active SQL transaction (per fail/004 analysis), so both writes are already within the ledger-close transaction

### Why It Failed

The proposed optimization targets a cost center that has been quantified by five prior investigations to total 0.5–2ms per ledger close:

1. **The total per-ledger savings from batching is ~100–200μs.** Each `updateDb()` call costs roughly the same: one `make_shared` (~50ns), one `prepare()` (~1–5μs for a trivial UPDATE), two `exchange()` binds (~100ns each), and one `execute()` (~50–200μs for SQLite WAL write). Eliminating one of these two calls saves at most half the SQL dispatch overhead — roughly 100–200μs per ledger.

2. **Over 200 benchmark ledgers, total savings: 20–40ms out of ~40,000ms** (0.05–0.1%). This is two orders of magnitude below the 5% threshold for Low severity and completely unmeasurable against ±5–10% benchmark variance.

3. **The SQLite WAL write dominates, and it cannot be batched away.** Even a single multi-value UPDATE still writes to the same WAL pages. The per-commit fsync cost (~50–200μs per fail/007) is paid once regardless of whether there are one or two UPDATEs in the transaction, so the WAL overhead is already amortized.

4. **Prior convergent findings.** fail/005 (skip writes), fail/013 (relocate writes), fail/014 (compact HAS format), and fail/015 (binary header encoding) all independently quantified this cost center at 0.5–2ms total per ledger, concluding it is an O(1) fixed cost that cannot reach measurable thresholds against O(n) Soroban execution costs. This hypothesis proposes saving <50% of an already-negligible cost.

### Lesson Learned

The storestate persistence path (`storePersistentStateAndLedgerHeaderInDB`) has been analyzed from six distinct angles (skip, relocate, compact format, binary encoding, session routing, and now statement batching). All converge: the total cost is 0.5–2ms per ledger, dominated by the WAL writes and commit fsync, not by statement allocation or SQL dispatch overhead. Further micro-optimizations within this function cannot produce measurable benchmark improvements. The statement lifecycle overhead (alloc + prepare + bind) for a trivial single-row UPDATE is single-digit microseconds — negligible even when doubled.
