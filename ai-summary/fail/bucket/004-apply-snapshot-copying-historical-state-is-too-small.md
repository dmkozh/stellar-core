# H004: Apply snapshot copying of historical bucket state dominates worker startup

**Date**: 2026-04-09
**Subsystem**: bucket
**Severity**: Low
**Impact**: parallel apply thread startup
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If snapshot-copy overhead were a meaningful bottleneck for apply-load, worker-thread startup should avoid copying unused historical and hot-archive snapshot metadata. In that case, stripping apply-time snapshots down to the current live bucket state would noticeably reduce per-ledger setup cost at `T=8`.

## Mechanism

At first glance, `ApplyLedgerStateSnapshot` looks heavy: it contains both live and hot-archive searchable snapshots plus historical maps, and `ThreadParallelApplyLedgerState` copies the snapshot into every worker. That suggests thread startup might spend significant time copying bucket metadata before any transaction work begins.

## Trigger

Run a multi-threaded apply-load benchmark and inspect the cost of constructing `GlobalParallelApplyLedgerState` and `ThreadParallelApplyLedgerState` for each ledger.

## Target Code

- `src/ledger/LedgerStateSnapshot.cpp:347-415` — `CompleteConstLedgerState` stores current and historical live/hot-archive snapshot data; `LedgerStateSnapshot` wraps both
- `src/transactions/ParallelApplyUtils.cpp:297-305` — global parallel state stores an `ApplyLedgerStateSnapshot`
- `src/transactions/ParallelApplyUtils.cpp:610-618` — each worker thread copy-constructs its own snapshot wrapper
- `src/main/Config.cpp:237-238` — default `QUERY_SNAPSHOT_LEDGERS = 5`

## Evidence

The default apply snapshot does carry more than the current live snapshot: it includes hot-archive data and up to five historical snapshot entries for both bucket lists. Every worker-thread copy therefore duplicates some metadata structures even in benchmarks that never query historical state or hot archive.

## Anti-Evidence

The copied objects are still shallow wrappers around shared immutable state: the maps only contain a handful of `shared_ptr`s, and stream caches are intentionally reset rather than duplicated. There is no per-entry copying here, only a small number of wrapper and map-node copies once per worker thread.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The copy cost is too small to matter relative to transaction execution and bucket I/O: default historical depth is only five ledgers, so each worker copies only a few `shared_ptr`-bearing map entries and wrapper objects once per ledger. That is orders of magnitude smaller than per-entry merge work, per-page XDR decoding, or per-file fsync paths.

### Lesson Learned

For apply-load bucket optimizations, focus on work that scales with entries, pages, or files. Snapshot-wrapper copying looks suspicious because it crosses thread boundaries, but the actual copied payload is tiny compared to the real hot paths.
