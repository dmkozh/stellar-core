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

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full level-0 bucket creation path from `finalizeLedgerTxnChanges` (LedgerManagerImpl.cpp:2952) through `addLiveBatch` → `addBatch` → `addBatchInternal` → `prepareFirstLevel` → `freshInMemoryOnly` → `convertToBucketEntry`. Confirmed that `const&` propagates through the entire chain, forcing deep copies in `convertToBucketEntry`. The fix requires reordering `updateInMemorySorobanState` before `addLiveBatch` in `finalizeLedgerTxnChanges`, which is safe since these two operations act on independent subsystems (InMemorySorobanState vs BucketList). The inefficiency is real but the measurable impact is likely below the 5% threshold for benchmark scenarios.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges:2952-3058` — Call ordering: `addAnyContractsToModuleCache` (read-only) → `addLiveBatch` (deep-copies) → `updateInMemorySorobanState` (read-only + copies Soroban entries into internal map). Reordering last two is safe.
- `src/bucket/BucketManager.cpp:addLiveBatch:1026-1046` — Takes `const&` vectors, forwards to `LiveBucketList::addBatch`
- `src/bucket/LiveBucketList.cpp:addBatch:15-27` — Forwards `const&` to `addBatchInternal`
- `src/bucket/BucketListBase.cpp:addBatchInternal:684-797` — Uses variadic `const&` template parameter pack, forwards to `prepareFirstLevel`
- `src/bucket/BucketListBase.cpp:prepareFirstLevel:196-238` — Calls `LiveBucket::freshInMemoryOnly` with forwarded `const&` args
- `src/bucket/LiveBucket.cpp:freshInMemoryOnly:466-498` — Calls `convertToBucketEntry` with `const&`, result vector is moved into `make_shared<LiveBucket>`
- `src/bucket/LiveBucket.cpp:convertToBucketEntry:379-420` — **The bottleneck**: iterates all three input vectors, deep-copies each `LedgerEntry` into a new `BucketEntry` via `ce.liveEntry() = e`. Sorts result and checks uniqueness.
- `src/ledger/InMemorySorobanState.cpp:updateState:536-600` — Takes `const&`, iterates entries, copies Soroban types into internal `InternalContractDataMapEntry` which does `make_shared<LedgerEntry const>(ledgerEntry)` (another deep copy, line 228 of header). No ordering dependency on `addLiveBatch`.

### Findings

The inefficiency is confirmed: `convertToBucketEntry` unconditionally deep-copies every `LedgerEntry` into `BucketEntry` wrappers via XDR union assignment. The fix is mechanically sound — swapping `updateInMemorySorobanState` before `addLiveBatch` allows the latter to take ownership via move. However, the practical impact is limited by several factors:

1. **Entry size distribution**: In typical SAC benchmarks, CONTRACT_DATA entries for token balances are small (~200-500 bytes). Only CONTRACT_CODE entries (wasm blobs) are large, and those are infrequently modified (deploy, not transfer). Most entries per ledger close are ACCOUNT/TRUSTLINE/CONTRACT_DATA with modest payloads.

2. **Copy chain**: This is one of 3-4 copies per entry through the level-0 path (getAllEntries → convertToBucketEntry → mergeInMemory → sort). Eliminating one copy saves ~25% of copy overhead but that's a fraction of total ledger close work.

3. **InMemorySorobanState also copies**: `updateInMemorySorobanState` does `make_shared<LedgerEntry const>(ledgerEntry)` for every Soroban entry it stores. A more comprehensive optimization would use shared_ptr ownership from the start, but that's a larger refactor.

4. **deadEntries are LedgerKey**: The `deadEntries` vector contains `LedgerKey` (small, ~50 bytes), so moves provide negligible benefit there.

### PoC Guidance

- **Target code**:
  - `src/ledger/LedgerManagerImpl.cpp:finalizeLedgerTxnChanges` — Swap lines 3053-3054 (`addLiveBatch`) with lines 3055-3056 (`updateInMemorySorobanState`) so `addLiveBatch` is the last consumer
  - `src/bucket/BucketManager.cpp:addLiveBatch` — Change `std::vector<LedgerEntry> const&` params to `std::vector<LedgerEntry>` (by value)
  - `src/bucket/LiveBucketList.cpp:addBatch` — Same signature change
  - `src/bucket/BucketListBase.cpp:addBatchInternal` — Change variadic `VectorT const&...` to `VectorT&&...` or by-value
  - `src/bucket/BucketListBase.cpp:prepareFirstLevel` — Same propagation
  - `src/bucket/LiveBucket.cpp:freshInMemoryOnly` — Accept vectors by value
  - `src/bucket/LiveBucket.cpp:convertToBucketEntry` — Accept vectors by value, use `std::move(e)` in loops: `ce.liveEntry() = std::move(e);`
- **Change description**: Propagate move semantics from `finalizeLedgerTxnChanges` down to `convertToBucketEntry` for the level-0 in-memory path. Requires call reordering in LedgerManagerImpl so `addLiveBatch` is last.
- **Correctness check**: Existing bucket tests (`[bucket]` tag), especially level-0 merge tests and `BucketListIsConsistentWithDatabase` tests, cover this path. Also run `[ledger]` tests to verify `finalizeLedgerTxnChanges` reordering is safe.
- **Benchmark focus**: Apply-load `sac` scenario at T=1 with 3200 TX. Measure ledger close time (serial path). Improvement likely <5% — may not be distinguishable from noise. A custom micro-benchmark of `convertToBucketEntry` with large CONTRACT_DATA entries (~10KB each) would show a clearer signal.
