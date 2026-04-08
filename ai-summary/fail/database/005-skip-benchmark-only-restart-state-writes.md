# H003: Benchmark Mode Still Persists Restart-Only Database State Every Ledger

**Date**: 2026-04-08
**Subsystem**: database
**Severity**: Low
**Impact**: benchmark-only durability I/O
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

`apply-load` benchmark mode should avoid serializing and persisting restart-only
database state that the harness never consumes during the timed run. For the
one-shot benchmark command, ledger-close timing should not include repeated
`HistoryArchiveState` JSON serialization and `storestate` durability writes if
those writes are only needed for crash recovery or later restart.

## Mechanism

The benchmark harness runs each scenario in a fresh temporary directory, parses
only the timing log, and then discards the working tree. Even so,
`storePersistentStateAndLedgerHeaderInDB()` serializes `HistoryArchiveState` to
JSON and updates both `kHistoryArchiveState` and `kLastClosedLedgerHeader`
every ledger, which adds fixed serialization and DB-write cost unrelated to the
Soroban apply work being benchmarked.

## Trigger

Run `scripts/run_apply_load_matrix.py`. Each scenario is executed in a new
`TemporaryDirectory`, `stellar-core apply-load` emits timing logs, and the
script copies out only the scenario log and parsed percentiles.

## Target Code

- `scripts/run_apply_load_matrix.py:338-367` — benchmark runs in a temporary directory and consumes only log output
- `src/main/CommandLine.cpp:1838-1883` — `runApplyLoad()` constructs a dedicated benchmark harness
- `src/ledger/LedgerManagerImpl.cpp:2891-2930` — every ledger serializes HAS and updates two `storestate` rows
- `src/history/HistoryArchive.cpp:137-149` — `HistoryArchiveState::toString()` performs full JSON serialization for the DB write

## Evidence

Nothing in the benchmark harness reads the persisted `storestate` rows back
during the measured run; the script advances through 200 ledgers and harvests
only close-time metrics from the log. This makes the restart-durability writes a
fixed tax on every benchmark ledger that is orthogonal to the model
transaction's actual apply cost.

## Anti-Evidence

The benchmark intentionally leaves `APPLY_LOAD_TIME_WRITES = true`, so some
amount of persistence cost is supposed to remain in-scope. A viable fix likely
needs to be explicitly benchmark-mode-gated rather than changing normal
validator ledger-close behavior.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (related to fail/003 and fail/004 but distinct claim about benchmark-mode skipping)
**Failed At**: reviewer

### Trace Summary

The hypothesis correctly identifies that `storePersistentStateAndLedgerHeaderInDB()`
performs unnecessary persistence writes during benchmark runs. However, tracing the
full cost reveals the overhead is negligible: `has.toString()` serializes ~3-5KB of
JSON (22 bucket hash entries), followed by two single-row SQL UPDATEs on the
`storestate` table via `PersistentState::updateDb()`. The total per-ledger cost is
estimated at 0.5-2ms, representing 0.1-2% of a typical 100-500ms Soroban ledger close.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:2891-2937` — `storePersistentStateAndLedgerHeaderInDB()`: constructs HAS from BucketList (lines 2906-2923), serializes to JSON via `has.toString()` (line 2926), writes two `storestate` rows (lines 2925-2930)
- `src/history/HistoryArchive.cpp:137-149` — `has.toString()` uses cereal JSONOutputArchive to serialize bucket hashes; output is ~3-5KB
- `src/history/HistoryArchive.cpp:530-565` — HAS constructor iterates 11 BucketList levels, extracting hash hex strings and future states
- `src/main/PersistentState.cpp:280-319` — `updateDb()` prepares and executes a single `UPDATE storestate SET state = :v WHERE statename = :n` per call
- `src/ledger/LedgerManagerImpl.cpp:3091-3098` — The HAS returned by `storePersistentStateAndLedgerHeaderInDB()` is consumed by `advanceApplySnapshotAndMakeLedgerState()`, meaning HAS construction cannot be skipped — only the JSON serialization and DB writes could be eliminated
- `src/simulation/ApplyLoad.cpp:1958-1962` — Benchmark timer selection: `APPLY_LOAD_TIME_WRITES=true` uses `{"ledger","ledger","close"}` which includes these writes in timing

### Why It Failed

The inefficiency exists but is not in a hot enough path to produce a measurable
improvement. The combined cost of JSON serialization (~50-500µs) and two SQL
UPDATEs (~100-500µs each) totals approximately 0.5-2ms per ledger close. For
benchmark ledger close times of 100-500ms (with 200 Soroban transactions), this
represents 0.1-2% of total close time — well below the 5% threshold for Low
severity and unmeasurable given typical benchmark variance of ±5-10%. Additionally,
the fix cannot simply skip `storePersistentStateAndLedgerHeaderInDB()` entirely
because the returned `HistoryArchiveState` is required by
`advanceApplySnapshotAndMakeLedgerState()` for in-memory state progression. The
optimization would require splitting the function (construct HAS vs. persist HAS)
and adding a benchmark-mode config flag, adding complexity for negligible benefit.

### Lesson Learned

When evaluating per-ledger-close overhead in the benchmark, distinguish between
costs that scale with transaction count (Soroban execution, BucketList updates)
and fixed per-ledger costs (persistent state writes). Fixed costs that are O(1)
per ledger and sub-millisecond are unlikely to be measurable against the much
larger O(n) transaction-processing costs that dominate close time.
