# H005: Double getLiveEntryOpt Lookups for Every Modified Key in Parallel Apply

**Date**: 2026-04-09
**Subsystem**: soroban (transactions / parallel apply)
**Severity**: Low
**Impact**: CPU / redundant hash map lookups in worker threads
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

After a successful parallel Soroban transaction, the pre-state of each modified
key should be fetched only once and shared between the delta computation
(`setEffectsDeltaFromSuccessfulTx`) and the meta builder
(`setLedgerChangesFromSuccessfulOp`). Currently, both functions independently
call `getLiveEntryOpt(lk)` for every modified key.

## Mechanism

In `TransactionFrame::parallelApply()` (TransactionFrame.cpp:2241-2246), after
a successful parallel apply, two functions are called sequentially:

1. `threadState.setEffectsDeltaFromSuccessfulTx(*res, ledgerInfo, effects)` —
   iterates every key in `res.getModifiedEntryMap()`, calls
   `getLiveEntryOpt(lk)` (line 797 in ParallelApplyUtils.cpp), and creates
   `shared_ptr<InternalLedgerEntry>` copies.

2. `opMeta.setLedgerChangesFromSuccessfulOp(threadState, *res, ledgerSeq)` —
   iterates the same keys, calls `threadState.getLiveEntryOpt(lk)` AGAIN
   (line 401 in TransactionMeta.cpp), and builds `LedgerEntryChanges`.

Each `getLiveEntryOpt` call traverses: `mThreadEntryMap` lookup → (miss) →
`InMemorySorobanState::get()` → `shared_ptr` dereference → deep copy
(`std::make_optional(*res)` at ParallelApplyUtils.cpp:734). For CONTRACT_CODE
entries (~10-25KB), the deep copy is ~3-5µs.

With ~6 modified keys per Soroban tx, the double-lookup costs ~12 extra hash
map lookups and 6 extra deep copies per tx. However, note that
`setLedgerChangesFromSuccessfulOp` returns early when `!mEnabled` (line 390-393
of TransactionMeta.cpp). In the **benchmark configuration**, meta output is
disabled (`METADATA_OUTPUT_STREAM = ""`), so the second function is a no-op.

This means the double-lookup only happens in **production** (where meta is
enabled), not in the benchmark. The optimization would improve production
performance but would NOT be measurable in the apply-load benchmark.

## Trigger

Run a production-like apply with meta output enabled (NOT the benchmark config).
Profile `setEffectsDeltaFromSuccessfulTx` and `setLedgerChangesFromSuccessfulOp`
to see the duplicate `getLiveEntryOpt` calls.

## Target Code

- `src/transactions/TransactionFrame.cpp:2243-2246` — calls both delta and meta functions sequentially
- `src/transactions/ParallelApplyUtils.cpp:setEffectsDeltaFromSuccessfulTx:790-829` — first getLiveEntryOpt per key (line 797)
- `src/transactions/TransactionMeta.cpp:setLedgerChangesFromSuccessfulOp:385-452` — second getLiveEntryOpt per key (line 401)

## Evidence

Both functions iterate the same `res.getModifiedEntryMap()` and call
`threadState.getLiveEntryOpt(lk)` for every key. The pre-state from the first
call could be cached (e.g., in a map from key → `shared_ptr<LedgerEntry const>`)
and passed to the second function.

## Anti-Evidence

1. **Not measurable in benchmark**: The benchmark disables metadata, so
   `setLedgerChangesFromSuccessfulOp` is a no-op. The optimization only
   helps production deployments, not apply-load scores.
2. After `commitChangesFromSuccessfulTx` runs, the thread entry map IS updated
   with the new values, so the second lookup for already-committed keys would
   find them in `mThreadEntryMap` (fast path), not in `InMemorySorobanState`.
   This reduces the cost of the duplicate lookup significantly.
3. Refactoring to share pre-state between two different functions adds coupling
   and complexity.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The optimization cannot produce a measurable improvement in the apply-load
benchmark because the second lookup (`setLedgerChangesFromSuccessfulOp`) is
disabled when metadata output is off. The benchmark explicitly disables metadata.
Furthermore, after `commitChangesFromSuccessfulTx` runs at line 2406 of
`LedgerManagerImpl.cpp`, the thread entry map IS populated with the committed
values, so any subsequent `getLiveEntryOpt` calls hit the fast `mThreadEntryMap`
path rather than deep-copying from `InMemorySorobanState`.

### Lesson Learned

When evaluating optimizations for the apply-load benchmark, always check whether
the affected code path is disabled in the benchmark configuration
(`docs/apply-load-benchmark-sac.cfg`). Meta-related code paths are no-ops when
`METADATA_OUTPUT_STREAM = ""`.
