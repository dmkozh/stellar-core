# H011: Lazy Main Pool Creation Spills Into the First Timed Benchmark Ledger

**Date**: 2026-04-10
**Subsystem**: database
**Severity**: Low
**Impact**: p99 latency spike
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If the SQLite connection pool were first created inside the measured
apply-load loop, the benchmark should avoid counting that one-time setup cost in
its timed ledgers. Pool initialization should happen during startup or other
pre-benchmark setup so it does not distort p99 close time.

## Mechanism

`Database::getPool()` is lazy, and `LedgerTxnRoot::Impl::addChild()` calls it
when `parallelLedgerClose()` opens the root SQL transaction. That initially
suggests the first benchmark ledger might pay for `createPool()` to open and
configure all pooled SQLite sessions.

## Trigger

Run any apply-load benchmark scenario on an on-disk SQLite database and compare
the first timed ledger to steady-state ledgers.

## Target Code

- `src/database/Database.cpp:createPool/getPool:706-745` — pool creation opens and configures every entry lazily
- `src/ledger/LedgerTxn.cpp:LedgerTxnRoot::Impl::addChild:2822-2830` — root ledger-close path requests the pool
- `src/simulation/ApplyLoad.cpp:1399-1414` — benchmark setup performs ledger writes and closes a ledger before the timed loop
- `src/simulation/ApplyLoad.cpp:1896-1916` — timed benchmark loop starts only after setup completes

## Evidence

Because `getPool()` is lazy, it is natural to suspect that the first measured
ledger pays the initialization cost, especially since the root ledger-close path
does request the pool on demand.

## Anti-Evidence

`ApplyLoad` setup already creates and commits a root `LedgerTxn` before the
benchmark loop (`LedgerTxn ltx(mApp.getLedgerTxnRoot())` at lines 1404-1408),
which forces `getPool()` and warms the pool before any close time is recorded.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The benchmark prewarms the root ledger-close path during setup, so the lazy pool
creation cost is already outside the measured ledger-close samples.

### Lesson Learned

For apply-load investigations, do not assume "lazy" initialization is timed just
because the production hot path reaches it on demand. The benchmark harness does
substantial pre-run ledger activity that can silently warm database structures
before the measured loop begins.
