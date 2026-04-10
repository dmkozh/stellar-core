# H022: Batch processFeesSeqNums Into Single LedgerTxn When Meta Disabled

**Date**: 2025-07-14
**Subsystem**: ledger
**Severity**: Low
**Impact**: CPU — eliminates ~3200 LedgerTxn constructor/destructor/commit cycles
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When transaction metadata is not needed (no meta stream configured), the
`processFeesSeqNums` function should process all transaction fees and sequence
number increments in a single parent `LedgerTxn`, avoiding the overhead of
creating and committing ~3200 child LedgerTxn instances. Each child LedgerTxn
adds ~0.5–1.5µs of overhead (constructor, map operations, commit merging).
For 3200 transactions, the total savings would be ~1.5–4.5ms per ledger.

## Mechanism

In `processFeesSeqNums` (line 2255), a `LedgerTxn ltxTx(ltx)` is created
per transaction to isolate per-tx changes. This isolation serves two purposes:
1. **Meta capture**: `ltxTx.getChanges()` (line 2292) extracts per-tx fee
   changes for `ledgerCloseMeta`. This requires per-tx LedgerTxn isolation.
2. **Rollback safety**: If fee charging fails, per-tx changes can be rolled
   back without affecting other transactions.

When meta is disabled (`ledgerCloseMeta == nullptr`), purpose (1) is moot.
For purpose (2), fee charging on a consensus-validated transaction set should
never fail — the source account balance was validated during nomination.
Batching all fees into the parent LedgerTxn would eliminate the overhead.

## Trigger

Run SAC apply-load benchmark at T=1 with 3200 transactions per ledger and
`METADATA_OUTPUT_STREAM = ""`.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2255-2295` — per-tx LedgerTxn in processFeesSeqNums loop

## Evidence

1. Each LedgerTxn constructor initializes an EntryMap, copies the header, and
   sets up thread-affinity tracking (~100–200ns).
2. Each commit merges entries into the parent map (~200–400ns).
3. 3200 iterations × ~0.5–1.5µs = ~1.5–4.5ms overhead.

## Anti-Evidence

1. Per-tx isolation provides defensive rollback even for "impossible" failures.
2. The savings (~3ms) represent only ~2–3% of a typical 100–150ms close.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-14
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated (H020 covered parallelization of processPostTxSetApply, not batching of processFeesSeqNums)

### Why It Failed

Two independent reasons prevent this from being viable for the apply-load
benchmark objective:

1. **BUILD_TESTS forces meta on**: The apply-load benchmark is built with
   `BUILD_TESTS`, which forces `ledgerCloseMeta` to always be non-null
   (line 1598–1606) and `enableTxMeta = true` (line 2646–2650). Therefore,
   `getChanges()` IS called for every tx, and per-tx LedgerTxn isolation IS
   required for meta capture. The batching optimization has no effect on the
   benchmark.

2. **Savings too small for production**: In a hypothetical production build
   without meta, the savings (~1.5–4.5ms for 3200 txs) represent only ~2–3%
   of ledger close time, which falls below the Low severity threshold (5–10%).

### Lesson Learned

BUILD_TESTS's unconditional meta tracking means optimizations that rely on
"meta disabled" as a precondition will NOT improve benchmark numbers. The
meta overhead itself should be addressed directly (see H002 hypothesis for
guarding BUILD_TESTS meta creation).
