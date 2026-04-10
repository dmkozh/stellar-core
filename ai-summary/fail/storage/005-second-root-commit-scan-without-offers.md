# H010: Skip the second full `mEntry` scan in `LedgerTxnRoot::commitChild` when no offers changed

**Date**: 2026-04-10
**Subsystem**: storage (ledger)
**Severity**: Low
**Impact**: post-apply commit CPU reduction
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

In Soroban-only ledgers with no `OFFER` mutations, the SQL commit path should
not need to rescan the entire modified-entry set after bucket extraction has
already walked it once. Ideally the root commit would operate in O(number of
offer changes), which is zero in the benchmarked workloads.

## Mechanism

`LedgerTxn::Impl::getAllEntries(...)` already walks every modified entry in
`mEntry` to extract init/live/dead vectors for bucket ingestion. Afterwards,
`LedgerTxnRoot::Impl::commitChild(...)` walks the same iterator again and
`BulkLedgerEntryChangeAccumulator::accumulate(...)` rejects every non-`OFFER`
entry because only offers are SQL-backed. That looks like a second wasted pass
over the same post-apply state.

## Trigger

Run Soroban apply-load workloads with many modified ledger entries but no offer
changes. The root commit path will still execute the second iterator walk even
though `bulkApply(...)` never sends anything to SQL offer helpers.

## Target Code

- `src/ledger/LedgerTxn.cpp:1627-1667` — first full pass in `getAllEntries(...)`
- `src/ledger/LedgerTxn.cpp:2877-2894` — `BulkLedgerEntryChangeAccumulator::accumulate(...)` rejects every non-offer entry
- `src/ledger/LedgerTxn.cpp:2918-2959` — second full pass in `LedgerTxnRoot::Impl::commitChild(...)`

## Evidence

The code clearly performs two independent full traversals of modified entries:
one for bucket extraction and one for SQL-offer accumulation. In Soroban-heavy
ledgers the second pass contributes no SQL work because `bulkApply(...)` never
sees buffered offers.

## Anti-Evidence

The second pass is extremely lightweight. `accumulate(...)` does little more
than two enum checks and returns false; it does not deep-copy entries or touch
the database unless an offer is encountered. Compared to the real costs on this
path — entry copying, bucket sorting/writing, in-memory Soroban-state updates,
and transaction commit — the saved work is probably too small to move the
apply-load benchmark materially.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

This path removes only a branch-heavy iterator walk with no significant copying
or I/O. Even across thousands of modified entries, that is unlikely to clear the
5% benchmark threshold needed for a storage optimization worth pursuing.

### Lesson Learned

On the storage apply path, the meaningful wins come from eliminating deep XDR
copies, hashing, serialization, or disk traffic. A second pass that only checks
types is visible in code review but too cheap to matter in the benchmark.
