# H002: Bulk bucket loads clone and tear down the unresolved-key tree on every prefetch

**Date**: 2026-04-10
**Subsystem**: bucket
**Severity**: Low
**Impact**: prefetch CPU and allocator pressure
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Bulk bucket lookups should consume a pre-sorted key batch with a cheap
forward-only data structure. The hot `loadLiveKeys()` path should not need to
clone the entire key set and then individually erase found keys from a
red-black tree when the algorithm only ever walks keys in order.

## Mechanism

`loadKeysInternal()` copies the caller's full `std::set<LedgerKey>` into a new
tree (`auto keys = inKeys`), and `loadKeysFromBucket()` erases each resolved key
one-by-one as it finds matches. Apply-load calls `ltx.prefetch()` for fee-source
accounts and tx-apply keys every ledger; even in Soroban-heavy scenarios that
guarantees large classic batches of source-account keys before transaction
execution begins. The result is deterministic allocator churn — thousands of
tree-node allocations and frees per ledger — before the bucket code has even
started the actual indexed reads.

## Trigger

Run the stock apply-load matrix with default prefetch enabled and bucket-backed
state (`!allBucketsInMemory()`). Every ledger's
`prefetchTxSourceIds`/`prefetchTransactionData` path builds a large key set and
feeds it into `loadLiveKeys()`, with the strongest effect in the
`sac,TX=3200` scenarios.

## Target Code

- `src/bucket/BucketListSnapshot.cpp:loadKeysFromBucket:203-277` — resolves keys by erasing them from the mutable `std::set`
- `src/bucket/BucketListSnapshot.cpp:loadKeysInternal:348-381` — clones the input key set before every bulk lookup
- `src/bucket/BucketListSnapshot.h:130-138` — API shape requires a `std::set` and destructive internal copy
- `src/ledger/LedgerTxn.cpp:LedgerTxnRoot::Impl::prefetch:3045-3097` — materializes a `LedgerKeySet` and always calls `loadLiveKeys()` for uncached classic keys
- `src/ledger/LedgerManagerImpl.cpp:prefetchTxSourceIds/prefetchTransactionData:2340-2376` — invokes prefetch on every ledger before apply

## Evidence

The bucket bulk-load algorithm is already strictly forward-only: inside a bucket,
it keeps `currKeyIt` and `indexIter` moving monotonically forward. Nothing in
the search requires tree insertion or arbitrary deletion; the mutable `std::set`
exists only so found keys can be erased and shadowed keys skipped in later
buckets. That makes the full-tree copy in `loadKeysInternal()` and per-hit
`keys.erase(currKeyIt)` pure bookkeeping overhead that scales with batch size.

## Anti-Evidence

If the entry cache is hot or the process is configured with all buckets in
memory, fewer keys reach `loadLiveKeys()`. A replacement data structure also has
to preserve the shadow-elision behavior across buckets, so the fix is more than
just swapping `std::set` for `std::vector`.
