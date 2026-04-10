# H007: Set PRAGMA synchronous=NORMAL in WAL Mode to Eliminate Per-Commit fsync

**Date**: 2026-04-10
**Subsystem**: database
**Severity**: Informational
**Impact**: I/O overhead reduction
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

In WAL mode, `PRAGMA synchronous=NORMAL` skips the fsync syscall on each
SQL COMMIT while still providing durability against application crashes (but
not OS crashes). Since the apply-load benchmark is a one-shot harness that
discards its working directory, full OS-crash durability is unnecessary.
Setting `synchronous=NORMAL` should eliminate one fsync per ledger close.

## Mechanism

`DatabaseConfigureSessionOp::doSqliteSpecificOperation` sets
`PRAGMA journal_mode = WAL` but leaves `synchronous` at the default `FULL`
(the NORMAL line is commented out at line 166). In WAL mode with `FULL`,
every `soci::transaction::commit()` forces an fsync of the WAL file. With
`NORMAL`, the COMMIT completes without an fsync — the WAL data is still
written to the OS page cache but not forced to stable storage.

## Trigger

Run `scripts/run_apply_load_matrix.py` with default benchmark configs.
Each ledger close does exactly one SQL COMMIT (line 2959 of LedgerTxn.cpp),
which triggers an fsync under `synchronous=FULL`.

## Target Code

- `src/database/Database.cpp:163-166` — WAL mode set, synchronous=NORMAL commented out
- `src/ledger/LedgerTxn.cpp:2958-2959` — SOCI commit triggers fsync

## Evidence

The commented-out line `// mSession << "PRAGMA synchronous = NORMAL";`
shows this was already considered. The benchmark writes only 2 small UPDATE
storestate rows per commit, so the fsync dominates the commit I/O cost
(fsync takes 50-200μs on NVMe SSD).

## Anti-Evidence

On NVMe SSD, each fsync costs only ~50-200μs. Over 200 benchmark ledgers,
total fsync overhead is ~10-40ms, which is <0.1% of a typical 40-second
benchmark run (200 ledgers × 200ms). Well below the 5% threshold for Low
severity and unmeasurable given ±5-10% benchmark variance.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The per-commit fsync from `synchronous=FULL` adds only ~50-200μs per
ledger close on NVMe SSD. Total overhead across a 200-ledger benchmark
is 10-40ms out of ~40,000ms total runtime (<0.1%). This is orders of
magnitude below the 5% minimum threshold for a Low-severity finding.
Even on slow storage where fsync takes 5-50ms, this represents only
1-10% of total benchmark time — at the low end this is still below
threshold, and the benchmark is designed to run on fast storage.

### Lesson Learned

The SQL path in Soroban apply-load is minimal: only 2 UPDATE storestate
rows + 1 COMMIT per ledger. Individual per-commit tuning (fsync policy,
statement caching, transaction mode) cannot produce measurable improvement
because the entire SQL cost per ledger is already sub-millisecond on modern
hardware. Database optimizations for this benchmark must target structural
changes (eliminating the SQL path entirely) rather than micro-tuning.
