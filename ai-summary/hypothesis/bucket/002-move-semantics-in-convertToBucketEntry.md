# H002: Avoid deep copies in convertToBucketEntry by accepting move-from vectors

**Date**: 2025-07-21
**Subsystem**: bucket
**Severity**: Low
**Impact**: ledger-close serial CPU, allocation pressure
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When `LedgerEntry` vectors are converted into `BucketEntry` vectors during
level-0 bucket creation, the conversion should move entries rather than copy
them, since the source vectors (`initEntries`, `liveEntries`) are not needed
after the bucket is built.

## Mechanism

`LiveBucket::convertToBucketEntry` (LiveBucket.cpp:379-420) takes its input
vectors as `const&` and deep-copies each entry:

```cpp
for (auto const& e : initEntries) {
    BucketEntry ce;
    ce.type(useInit ? INITENTRY : LIVEENTRY);
    ce.liveEntry() = e;          // deep copy of LedgerEntry
    bucket.push_back(ce);
}
```

For the `freshInMemoryOnly` path (level 0, every ledger), these entries were
just produced by `ltx.getAllEntries()` which itself copies them from the
LedgerTxn EntryMap. The `addLiveBatch` → `addBatchInternal` →
`prepareFirstLevel` → `freshInMemoryOnly` → `convertToBucketEntry` chain
passes them as `const&` throughout, preventing move semantics.

Each `LedgerEntry` contains an `xdr::xvector<uint8_t>` for Soroban
contract data values (CONTRACT_DATA, CONTRACT_CODE) which can be 1-64KB.
Deep-copying these allocates fresh heap buffers. For ~3000 entries per
ledger close, this is ~3000 unnecessary heap allocations and memory copies
that could be eliminated by accepting the vectors by value (or rvalue
reference) and using `std::move`.

The callers in `finalizeLedgerTxnChanges` (LedgerManagerImpl.cpp:2959-3057)
use these vectors for `addAnyContractsToModuleCache` (read-only) and
`updateInMemorySorobanState` (read-only) before passing them to
`addLiveBatch`. By reordering or accepting ownership at the `addLiveBatch`
boundary, the bucket subsystem could move-from the vectors instead of
copying.

## Trigger

Run any apply-load Soroban benchmark (sac, custom_token, soroswap). Every
ledger close calls `convertToBucketEntry` which copies all modified entries.
The impact scales with the number and size of entries modified per ledger.

## Target Code

- `src/bucket/LiveBucket.cpp:379-420` — `convertToBucketEntry`: copies each entry from const& input vectors
- `src/bucket/LiveBucket.cpp:466-498` — `freshInMemoryOnly`: calls convertToBucketEntry with forwarded const& args
- `src/bucket/BucketManager.cpp:1026-1046` — `addLiveBatch`: takes vectors as const&, passes to addBatch
- `src/bucket/LiveBucketList.cpp:14-27` — `addBatch`: forwards const& to addBatchInternal
- `src/ledger/LedgerManagerImpl.cpp:3049-3056` — caller: uses vectors for module cache + soroban state, then addLiveBatch

## Evidence

1. The `convertToBucketEntry` function creates a new `BucketEntry` for each
   input, setting its discriminant and deep-copying the `LedgerEntry` payload.
   XDR union assignment deep-copies all variant data including nested vectors.
2. The vectors originate from `ltx.getAllEntries()` which already deep-copies
   from the EntryMap. The resulting vectors are only read (not modified) by
   `addAnyContractsToModuleCache`, which iterates looking for CONTRACT_CODE
   entries. After `addLiveBatch`, `updateInMemorySorobanState` also only reads.
3. If `addLiveBatch` were the last consumer (or if the API were split so bucket
   creation receives ownership), each LedgerEntry could be moved into its
   BucketEntry wrapper, avoiding the heap allocation for large payloads.

## Anti-Evidence

1. The current call sequence (`addAnyContractsToModuleCache` → `addLiveBatch` →
   `updateInMemorySorobanState`) requires all three to read the same vectors.
   Enabling moves would require either reordering these calls, splitting Soroban
   vs. classic entries earlier, or having `addLiveBatch` be the last consumer.
2. For classic entries (ACCOUNT, TRUSTLINE, OFFER), the `LedgerEntry` is small
   (~200-400 bytes) and the copy cost is minimal. The savings are primarily for
   Soroban entries with large payloads.
3. The `convertToBucketEntry` function also sorts the resulting vector
   (line 412-413). The sort itself moves elements, but the initial copy into
   the vector is the avoidable cost.
4. Given that this is one copy in a chain of 3-4 copies through the level-0
   path, eliminating just this one saves ~25% of total copy overhead per entry
   but may still be below the 5% threshold for ledger close time improvement.
