# H002: HistoryArchiveState Uses Archive-Grade JSON in the Ledger-Close DB Hot Path

**Date**: 2026-04-10
**Subsystem**: database, history, ledger
**Severity**: Informational
**Impact**: CPU + WAL write amplification
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Per-ledger restart metadata persisted to the local database should use a compact
restart-oriented encoding, not a human-readable archive format. The
`kHistoryArchiveState` write in the timed ledger-close path should minimize
string allocation, formatting, and bytes written while remaining losslessly
decodable on startup.

## Mechanism

`storePersistentStateAndLedgerHeaderInDB()` constructs a full
`HistoryArchiveState` and immediately serializes it with `toString()`, which
routes through `std::ostringstream` and `cereal::JSONOutputArchive` before
writing into `storestate.state TEXT`. The same state is only consumed locally by
`fromString()` during restart, so the DB path is paying JSON formatting overhead
and verbose TEXT write amplification on every ledger close for data that does
not need archive-style readability.

## Trigger

Run `scripts/run_apply_load_matrix.py` with `APPLY_LOAD_TIME_WRITES = true`
(the benchmark default). Every closed ledger calls
`storePersistentStateAndLedgerHeaderInDB()` and writes a fresh
`historyarchivestate` row.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:storePersistentStateAndLedgerHeaderInDB:2916-2936` — constructs HAS and immediately serializes it for DB persistence
- `src/history/HistoryArchive.cpp:toString/fromString:136-149,173-179` — local DB persistence uses JSON string round-trip
- `src/main/PersistentState.cpp:updateDb:281-318` — writes the serialized value into `storestate.state`
- `src/ledger/LedgerManagerImpl.cpp:loadLastKnownLedgerInternal:537-541` — restart path only needs to decode the DB value back into HAS
- `src/history/HistoryManagerImpl.cpp:writeCheckpointFile:77-99` — there is already a non-JSON checkpoint-writing path for HAS-adjacent persistence

## Evidence

The hot path does not stream directly to SQLite; it first materializes a JSON
string in memory, then issues a generic `UPDATE storestate`. Startup recovery
only calls `fromString()` on the stored value, so the current DB format is
chosen for implementation convenience rather than because the runtime needs a
human-readable representation there.

## Anti-Evidence

The number of bucket levels is fixed, so this remains an O(1) per-ledger cost,
and the total payload is still small relative to Soroban execution. A viable
change must also preserve backward compatibility for existing databases or
introduce a migration path.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: FAIL — substantially equivalent to fail/005 and fail/013, which already quantified the exact cost this hypothesis targets (HAS JSON serialization + storestate write) at 0.5–2ms per ledger
**Failed At**: reviewer

### Trace Summary

Traced the `toString()` serialization path through `cereal::JSONOutputArchive` and the `updateDb()` SQL write path. The HAS JSON payload contains version, server string, currentLedger, networkPassphrase, and 11 `HistoryStateBucket` entries (each with curr hash, snap hash, and FutureBucket state) plus 11 hot archive buckets — producing roughly 3–5KB of JSON. A compact binary encoding (e.g., XDR or msgpack) would reduce this to roughly 1–2KB, saving ~2–3KB per write. At SQLite page size 4096, this saves zero or one page per UPDATE.

### Code Paths Examined

- `src/history/HistoryArchive.cpp:136-149` — `toString()` uses `std::ostringstream` + `cereal::JSONOutputArchive`; the cereal serialization iterates over `version`, `server`, `currentLedger`, `networkPassphrase`, `currentBuckets[11]`, `hotArchiveBuckets[11]`
- `src/history/HistoryArchive.h:46-58` — Each `HistoryStateBucket` serializes `curr` (64-char hex string), `next` (FutureBucket with state enum + up to 4 hex hashes), `snap` (64-char hex string)
- `src/history/HistoryArchive.h:136-183` — `HistoryArchiveState::serialize()` — const version serializes version, server, currentLedger, optional networkPassphrase, currentBuckets, optional hotArchiveBuckets
- `src/main/PersistentState.cpp:281-318` — `updateDb()` executes `UPDATE storestate SET state = :v WHERE statename = :n`, a single-row UPDATE on a tiny table
- `src/ledger/LedgerManagerImpl.cpp:2900-2947` — `storePersistentStateAndLedgerHeaderInDB()` constructs HAS, calls `has.toString()`, then writes two storestate rows

### Why It Failed

This hypothesis targets the same cost center already quantified by multiple prior investigations:

1. **fail/005** measured the total `storePersistentStateAndLedgerHeaderInDB()` cost at 0.5–2ms per ledger, with JSON serialization specifically at ~50–500μs. Over 200 benchmark ledgers this totals 10–100ms out of ~40,000ms total benchmark time (0.025–0.25%).

2. **fail/013** independently confirmed the same 0.5–2ms total, noting that "moving metadata elsewhere would shift, not eliminate, the write cost."

3. **The proposed format change would save even less than eliminating serialization entirely.** Replacing JSON with a compact binary format would save only the formatting overhead (~50–500μs per ledger), not the SQL UPDATE or WAL fsync costs which dominate. The WAL write savings from a 2–3KB smaller payload are negligible — SQLite writes at page granularity (4096 bytes), so a 3KB vs 5KB value likely hits the same number of pages.

4. **The fix also requires a schema migration path.** Existing databases store JSON in `storestate.state TEXT`. A binary encoding would need either a migration that converts existing values, dual-format detection in `fromString()`, or a new column — adding complexity for sub-0.1% savings.

5. **The cost is O(1) per ledger, not O(n) per transaction.** Against Soroban execution costs that scale with transaction count, this fixed overhead becomes proportionally smaller as the benchmark increases transaction volume.

### Lesson Learned

The HAS serialization and storestate persistence path has now been analyzed from five angles: skip entirely (fail/005), relocate to misc DB (fail/013), lazy SQL transaction (fail/003), benchmark-mode gating (fail/005), and now compact encoding (this investigation). All converge on the same conclusion: the total cost is 0.5–2ms per ledger, too small to produce measurable improvement. The JSON format vs. compact format distinction is irrelevant when the entire serialization is only ~50–500μs. Future hypotheses should not target the storestate persistence path.
