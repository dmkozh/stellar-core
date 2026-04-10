# H009: Replace post-seal entry copy amplification with a single extraction pass

**Date**: 2026-04-10
**Subsystem**: storage (ledger, bucket)
**Severity**: Low
**Impact**: post-apply commit CPU and memory-bandwidth reduction
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

After `LedgerTxn` is sealed, the modified-entry set should be materialized once
and then shared across downstream consumers. The commit path should not deep-copy
the same `LedgerEntry` payloads into transient init/live vectors, then deep-copy
them again into `BucketEntry` objects, and then deep-copy Soroban entries again
into the in-memory state cache.

## Mechanism

`finalizeLedgerTxnChanges` calls `ltx.getAllEntries(...)`, which walks the
sealed `mEntry` map and copies every modified ledger entry into
`std::vector<LedgerEntry>` / `std::vector<LedgerKey>`. `addLiveBatch(...)`
immediately re-copies those vectors into `BucketEntry` objects in
`LiveBucket::convertToBucketEntry(...)`, and `updateInMemorySorobanState(...)`
then copies Soroban entries again into heap-owned `shared_ptr<LedgerEntry const>`
records. On Soroban-heavy ledgers, this creates multiple full-object copies of
the same changed entries on the single-threaded commit path after the parallel
execution phase has already finished.

## Trigger

Any apply-load Soroban ledger that modifies thousands of entries. The hot path
is reached on every ledger close after tx application when the code seals the
ledger transaction, emits bucket inputs, and refreshes `InMemorySorobanState`.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:3039-3046` — `getAllEntries(...)` feeds both `addLiveBatch(...)` and `updateInMemorySorobanState(...)`
- `src/ledger/LedgerTxn.cpp:1627-1667` — first full copy out of `mEntry` into init/live/dead vectors
- `src/bucket/LiveBucket.cpp:379-419` — second copy into `BucketEntry` before sort/write
- `src/ledger/InMemorySorobanState.h:224-235` — stored contract-data values are heap-copied into new `shared_ptr`s
- `src/ledger/InMemorySorobanState.cpp:109-110` — updates reinsert a copied `LedgerEntry`
- `src/ledger/InMemorySorobanState.cpp:140-141` — creates insert a copied `LedgerEntry`
- `src/ledger/InMemorySorobanState.cpp:299-301` — contract-code updates also replace with copied `LedgerEntry`

## Evidence

The current code performs at least one full pass to copy every changed ledger
entry out of `LedgerTxn`, then immediately performs more full passes over the
same data for bucket ingestion and in-memory-state refresh. The bucket path only
needs stable post-seal data and the in-memory state only needs durable ownership,
so a dedicated extraction API could build owned `BucketEntry` objects and
long-lived Soroban cache payloads from one traversal of `mEntry` instead of
serially re-copying the same XDR objects.

## Anti-Evidence

Some copying is unavoidable: bucket writes need owned sorted entries, and the
in-memory cache persists across ledgers. The refactor is intrusive because it
changes ownership and lifetime assumptions around a sealed `LedgerTxn`, and it
must preserve the later SQL commit path. The actual benchmark gain depends on
how large modified entries are in the apply-load workloads.
