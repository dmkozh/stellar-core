# H009: Use WithoutLoading Methods in commitChangesToLedgerTxn

**Date**: 2025-07-14
**Subsystem**: soroban, ledger
**Severity**: Low
**Impact**: Reduced serial commit time after all stages
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

`commitChangesToLedgerTxn` should write dirty entries from the global map
to the LedgerTxn using the most efficient available methods:
`createWithoutLoading`, `updateWithoutLoading`, `eraseWithoutLoading`.
These bypass the load-then-modify pattern, avoiding LedgerTxnEntry handle
creation and parent chain lookups.

## Mechanism

`commitChangesToLedgerTxn` (ParallelApplyUtils.cpp:389-458) iterates all
dirty entries in the global map. For each entry, it calls `ltxInner.load(key)`
to probe whether the key exists, then either updates or creates. The `load`
call involves: (1) hash map lookup in `mActive`, (2) parent chain search
via `loadEntryFromParent`, (3) `LedgerTxnEntry` RAII handle creation with
`shared_ptr` management.

`updateWithoutLoading` / `createWithoutLoading` (LedgerTxn.cpp:750-797)
skip the handle mechanism and directly insert into the entry map. This
avoids the parent chain search and handle overhead.

With ~5,000 dirty entries after all stages, the savings per entry are:
~100-200ns (handle creation + parent lookup avoidance).
Total: ~0.5-1.0ms.

## Trigger

Run apply-load with 3200 SAC TXs. Profile `commitChangesToLedgerTxn` to
measure per-entry `load` overhead vs. direct `*WithoutLoading` calls.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:commitChangesToLedgerTxn:389-458` — current load-then-modify loop
- `src/ledger/LedgerTxn.cpp:updateWithoutLoading:780-797` — direct entry update
- `src/ledger/LedgerTxn.cpp:createWithoutLoading:750-771` — direct entry creation

## Evidence

- `load` creates a `LedgerTxnEntry` RAII handle that involves `shared_ptr` allocation
- `load` searches the parent chain, which for in-memory parents is a hash map lookup
- `*WithoutLoading` methods skip handle creation and parent search
- ~5,000 dirty entries × ~150ns savings = ~0.75ms

## Anti-Evidence

- The current `load` call serves as a probe: it determines whether to create or update
- Using `*WithoutLoading` requires knowing upfront whether each entry is new or existing
- The global entry map doesn't currently track create-vs-update status
- Adding this tracking requires a new flag in `GlobalParallelApplyEntry` and propagating it through thread commit
- For in-memory parents, `load` is already fast (hash map lookup ~50ns)
- Total savings (~0.5-1ms) are marginal for the implementation complexity

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-14
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The savings are only ~0.5-1ms total across ~5,000 entries, well below the
Low severity threshold. The `load` calls for in-memory entries are already
cheap (~50-100ns per call), and the `LedgerTxnEntry` handle overhead is
modest. The required changes (tracking create/update status through the
entire global map pipeline) add non-trivial complexity for minimal gain.

### Lesson Learned

`LedgerTxn::load` for in-memory entries is already efficient. The
`*WithoutLoading` methods are primarily useful when the load would hit
disk (SQL queries). For the parallel apply path where all entries are
in-memory, the overhead difference between `load` and `*WithoutLoading`
is small enough to not be worth the added complexity.
