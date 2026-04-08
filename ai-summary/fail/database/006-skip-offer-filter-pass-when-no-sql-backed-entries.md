# H004: Root Commit Re-Scans Every Dirty Entry to Discover There Are No SQL Writes

**Date**: 2026-04-08
**Subsystem**: database
**Severity**: Medium
**Impact**: post-apply CPU overhead
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If a ledger did not modify any SQL-backed entry types, the root commit path
should skip the SQL staging pass entirely. In the Soroban apply-load scenarios,
post-apply commit should not linearly walk every dirty `CONTRACT_DATA`, `TTL`,
`ACCOUNT`, and `TRUSTLINE` entry just to prove that none of them are `OFFER`s.

## Mechanism

`LedgerTxnRoot::Impl::commitChild()` iterates the full child delta and feeds
each entry into `BulkLedgerEntryChangeAccumulator::accumulate()`. That
accumulator immediately drops every non-`OFFER` entry because
`LiveBucketIndex::typeNotSupported()` returns true only for `OFFER`, so
Soroban-heavy ledgers pay an O(changed-entries) serialized CPU pass whose only
purpose is maintaining the legacy SQL offer table.

## Trigger

Run the apply-load benchmark with SAC, custom-token, or soroswap model
transactions. These scenarios mutate large Soroban footprints and fee-source
classic entries but do not create or edit `OFFER`s, so the commit scan becomes
all reject-path and no useful SQL work.

## Target Code

- `src/ledger/LedgerTxn.cpp:2877-2894` — `BulkLedgerEntryChangeAccumulator::accumulate()` rejects all non-SQL-backed entries
- `src/ledger/LedgerTxn.cpp:2918-2959` — root commit loops the entire child delta and bulk-applies only offer buffers
- `src/bucket/LiveBucketIndex.cpp:22-25` — only `OFFER` is excluded from BucketListDB
- `scripts/run_apply_load_matrix.py:71-101` — benchmark scenarios are Soroban model transactions rather than offer-book workloads

## Evidence

The scan sits on the serialized root commit path, after transaction execution is
done and before the ledger transaction commits. Because the benchmark's model
transactions are Soroban-only, the pass produces empty offer buffers but still
touches every dirty entry in the ledger delta.

## Anti-Evidence

If a ledger includes actual `OFFER` mutations, the pass is required with the
current design. A safe optimization needs either a cheap "SQL-backed entry was
touched" bit or a separate incremental buffer of offer changes built while the
child `LedgerTxn` is mutated.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (related but distinct from H003 which covered the SQL transaction overhead, not the iteration loop)
**Failed At**: reviewer

### Trace Summary

Traced the full `commitChild()` loop in `LedgerTxnRoot::Impl`. The loop iterates
over the child `EntryMap` (an `UnorderedMap<InternalLedgerKey, LedgerEntryPtr>`)
via `EntryIteratorImpl`, which wraps a `const_iterator`. Each iteration performs:
(1) virtual dispatch to `key()` returning a reference to the map key, (2) an
integer comparison on `InternalLedgerKey::mType`, (3) for LEDGER_ENTRY types, a
second integer comparison on the XDR `LedgerKey` discriminant, (4) virtual
dispatch for `advance()` doing `++mIter`, and (5) `bulkApply()` which checks two
`vector::size()` calls against the threshold (both vectors are empty). The entire
per-iteration cost is a few virtual dispatches and integer comparisons on
cache-local data — no allocations, copies, serialization, or I/O.

### Code Paths Examined

- `src/ledger/LedgerTxn.cpp:2918-2959` — `commitChild()` while-loop iterates the full delta
- `src/ledger/LedgerTxn.cpp:2877-2894` — `accumulate()` does two cheap type checks and returns false for all non-OFFER entries
- `src/ledger/LedgerTxn.cpp:2897-2915` — `bulkApply()` checks two empty vectors per iteration (no-op when no offers)
- `src/ledger/LedgerTxn.cpp:2647-2650` — `EntryIteratorImpl::advance()` is just `++mIter` on UnorderedMap iterator
- `src/ledger/LedgerTxn.cpp:2677-2679` — `EntryIteratorImpl::key()` returns `mIter->first` (reference, no copy)
- `src/ledger/InternalLedgerEntry.h:40` — `InternalLedgerKey::mType` is a direct field, integer comparison
- `src/bucket/LiveBucketIndex.cpp:22-25` — `typeNotSupported()` is a single `== OFFER` comparison

### Why It Failed

The inefficiency exists only in a trivial sense — the loop runs O(n) but each
iteration has a negligible constant factor. Per-iteration work amounts to ~3
virtual dispatches and ~3 integer comparisons, all on cache-local data with
no allocations, copies, or I/O. For a typical Soroban ledger delta of hundreds
to low thousands of entries, this loop costs sub-microsecond to low single-digit
microseconds — orders of magnitude below the cost of the SQL transaction commit
(line 2959), BucketList updates, or Soroban execution. The loop is not in a
meaningfully hot path relative to total ledger close cost, and eliminating it
would produce no measurable improvement in any benchmark scenario. The proposed
fixes (tracking an "SQL-backed entry touched" flag across the LedgerTxn
hierarchy, or building a separate incremental offer buffer during mutation) both
add non-trivial complexity for sub-microsecond savings.

### Lesson Learned

When evaluating O(n) scans for optimization, the constant factor matters as much
as n. A loop that does only virtual dispatch and integer comparisons on
cache-local data with no allocations or I/O is effectively free for n < 10,000.
Focus optimization effort on loops that do per-element allocation, serialization,
I/O, or cache-unfriendly access patterns.
