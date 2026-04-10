# H001: Reserve `mTxEntryMap` From The Transaction Footprint Before Parallel Apply

**Date**: 2026-04-10
**Subsystem**: crypto, transactions
**Severity**: High
**Impact**: repeated `LedgerKey` rehashing and allocator churn in per-tx dirty-entry tracking
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The per-transaction dirty-entry map in parallel apply should allocate enough
capacity up front from the transaction's known Soroban footprint, so that a
single invoke-host execution pays roughly one hash and one insertion per dirty
key. Batched SAC transfers should not repeatedly grow and rehash the tx-local
map while they materialize every modified contract-data entry and its TTL
companion.

## Mechanism

`TxParallelApplyLedgerState` constructs `mTxEntryMap` empty and never reserves
capacity before `upsertEntry()` and `eraseEntryIfExists()` start filling it.
The benchmarked SAC batch-transfer path uses `APPLY_LOAD_BATCH_SAC_COUNT = 100`
and therefore puts 101 read-write keys into the footprint for each tx (the
batch contract's balance plus 100 destination balances); because persistent
Soroban writes also carry TTL entries, the tx-local dirty map can grow toward
~200 entries per tx. Every growth step of an empty `unordered_map` rehashes all
previously inserted `LedgerKey`s, and for `CONTRACT_DATA` those rehashes call
`shortHash::xdrComputeHash` on the `SCVal("Balance", address)` key again.

## Trigger

Run the default SAC apply-load benchmark with `APPLY_LOAD_BATCH_SAC_COUNT = 100`
(especially `T=8`) and sample `TxParallelApplyLedgerState::upsertEntry`,
`unordered_map` rehash/allocation activity, and `std::hash<LedgerKey>` during
parallel apply. Compare against a build that reserves `mTxEntryMap` from the
read-write footprint size plus expected TTL companions before any restore or
host-write insertion happens.

## Target Code

- `docs/apply-load-benchmark-sac.cfg:31-38` — benchmark config uses batched SAC transfers with `APPLY_LOAD_BATCH_SAC_COUNT = 100`
- `src/simulation/ApplyLoad.cpp:2069-2113` — SAC benchmark generation builds batch-transfer txs when the batch size is >1
- `src/simulation/TxGenerator.cpp:1480-1512` — each batch-transfer tx inserts 101 read-write keys before TTL companions are added during apply
- `src/transactions/ParallelApplyUtils.h:292-303` — `TxParallelApplyLedgerState` owns `mTxEntryMap` but has no capacity planning hook
- `src/transactions/ParallelApplyUtils.cpp:876-883` — tx state constructor leaves `mTxEntryMap` empty
- `src/transactions/ParallelApplyUtils.cpp:907-950` — `upsertEntry()` repeatedly `insert_or_assign`s into the growing map
- `src/transactions/ParallelApplyUtils.cpp:954-967` — delete markers also grow the same map
- `src/transactions/ParallelApplyUtils.cpp:1009-1018` — map is only moved out after the tx finishes, so all rehashing stays on the hot path
- `src/ledger/LedgerHashUtils.h:178-184` — `CONTRACT_DATA` hashing reserializes the `SCVal` key via `shortHash::xdrComputeHash`

## Evidence

The transaction already knows an upper bound for dirty entries before apply
starts: its Soroban read-write footprint. In the batched SAC benchmark that is
101 keys per tx before accounting for TTL entries, and the apply path adds those
TTL companions into the same tx-local dirty map. There is no `reserve()` call
on `mTxEntryMap`, so the map grows from zero through multiple rehash rounds
while hashing the same contract-data keys again and again.

## Anti-Evidence

This should be much less visible in `custom_token` and `soroswap`, whose
benchmark footprints are far smaller per tx. The hypothesis is strongest for
the SAC benchmark because batching inflates one tx's write set enough for
rehash waves to become material.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full parallel apply path for `InvokeHostFunctionOpFrame`. The `TxParallelApplyLedgerState::mTxEntryMap` (`UnorderedMap<LedgerKey, TxParApplyLedgerEntryOpt>`, i.e. `std::unordered_map` with `RandHasher`) is constructed empty in `ParallelLedgerAccessHelper`'s constructor (line 242). After Soroban host invocation, `recordStorageChanges()` iterates `out.modified_ledger_entries` and calls `upsertLedgerEntry()` for each (~200 entries for SAC batch), triggering ~8 rehash rounds. Each rehash recomputes hashes for all existing keys, including the expensive `shortHash::xdrComputeHash` path for `CONTRACT_DATA` `SCVal` keys. The RW footprint size is readily available in the same constructor via `mOpFrame.mResources.footprint.readWrite`.

### Code Paths Examined

- `src/transactions/ParallelApplyUtils.cpp:876-883` — `TxParallelApplyLedgerState` constructor: `mTxEntryMap` default-constructed (empty, ~1 bucket)
- `src/transactions/ParallelApplyUtils.cpp:239-246` — `ParallelLedgerAccessHelper` constructor: `mTxState(threadState)` — no reserve opportunity
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1001-1196` — `InvokeHostFunctionParallelApplyHelper`: has access to `mResources.footprint.readWrite` and `mTxState` (inherited from `ParallelLedgerAccessHelper`)
- `src/transactions/InvokeHostFunctionOpFrame.cpp:610-659` — `recordStorageChanges()`: loops over `out.modified_ledger_entries`, calls `upsertLedgerEntry(lk, le)` for each — this is the hot insertion loop
- `src/transactions/InvokeHostFunctionOpFrame.cpp:614-615` — `createdAndModifiedKeys` and `createdKeys` are also local `UnorderedSet<LedgerKey>` that grow from empty (additional rehash overhead)
- `src/transactions/ParallelApplyUtils.cpp:907-950` — `TxParallelApplyLedgerState::upsertEntry()`: calls `getLiveEntryOpt(key)` (which does `mTxEntryMap.find(key)`) then `mTxEntryMap.insert_or_assign()` — two hash operations per insert
- `src/ledger/LedgerHashUtils.h:178-184` — `CONTRACT_DATA` hash calls `shortHash::xdrComputeHash(lk.contractData().key)` which creates an `XDRShortHasher` (acquires `gKeyMutex`), serializes the `SCVal`, and computes SipHash
- `src/transactions/TransactionFrameBase.h:52` — `TxModifiedEntryMap` is `UnorderedMap<LedgerKey, TxParApplyLedgerEntryOpt>` = `std::unordered_map` with `RandHasher<LedgerKey>`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:312-313` — nearby code already reserves `mLedgerEntryCxxBufs` and `mTtlEntryCxxBufs` from `footprintLength` — precedent for capacity planning in this code path

### Findings

**The inefficiency is real.** For SAC batch-transfer with `APPLY_LOAD_BATCH_SAC_COUNT=100`, each tx has 101 RW footprint keys. The host returns ~202 modified entries (101 contract data + 101 TTL). Starting from an empty `std::unordered_map` (default ~1 bucket), inserting 202 entries triggers ~8 rehash rounds (at bucket thresholds ~1, 3, 7, 17, 37, 67, 131, 263). Each rehash recomputes hashes for all previously inserted keys. Total extra hash operations from rehashing: ~1+3+7+17+37+67+131 ≈ 263 — comparable to the 202 insertions themselves.

**The rehash cost is non-trivial for `CONTRACT_DATA` keys.** Each `CONTRACT_DATA` hash involves `XDRShortHasher` construction (mutex acquire ~10-20ns), XDR serialization of the `SCVal` key through `XDRHasher::operator()` (~50-100ns), and SipHash computation (~10-20ns). At ~100ns per CONTRACT_DATA rehash and ~40ns per TTL rehash: ~130 × 100ns + ~133 × 40ns ≈ 18μs per tx just from rehash hashes.

**Additional allocator churn.** Each rehash allocates a new bucket array and deallocates the old one (~8 allocations per tx). More importantly, `recordStorageChanges()` also creates two local `UnorderedSet<LedgerKey>` (`createdAndModifiedKeys`, `createdKeys`) that experience the same growth pattern, roughly doubling the rehash overhead.

**The fix is simple and correct.** Add a `reserveEntryMap(size_t n)` method to `TxParallelApplyLedgerState` that calls `mTxEntryMap.reserve(n)`. Call it from `InvokeHostFunctionParallelApplyHelper` constructor with `mResources.footprint.readWrite.size() * 2` (accounting for TTL companions). This exactly matches the existing pattern at lines 312-313 where `mLedgerEntryCxxBufs` and `mTtlEntryCxxBufs` are reserved from footprint length.

**Impact estimate.** For SAC at TX=3200, T=8: ~3200 txs × ~36μs rehash overhead per tx / 8 threads ≈ 14ms per ledger wall clock. Against estimated ledger close times of 200-400ms, this is 3-7%. The improvement is SAC-specific; `custom_token` (~6 entries) and `soroswap` (~10-20 entries) have footprints too small for rehashing to matter. Downgrading from High to Low — real but modest improvement for one benchmark scenario.

### PoC Guidance

- **Target code**: 
  1. `src/transactions/ParallelApplyUtils.h` — add `void reserveEntryMap(size_t capacity);` to `TxParallelApplyLedgerState` public interface
  2. `src/transactions/ParallelApplyUtils.cpp` — implement as `mTxEntryMap.reserve(capacity);`
  3. `src/transactions/InvokeHostFunctionOpFrame.cpp` ~line 1182 — in `InvokeHostFunctionParallelApplyHelper` constructor, after `ParallelLedgerAccessHelper(threadState, ledgerInfo)`, add `mTxState.reserveEntryMap(mResources.footprint.readWrite.size() * 2);`
  4. Optionally: in `recordStorageChanges()` ~line 614, add `createdAndModifiedKeys.reserve(out.modified_ledger_entries.size());` for the local sets
  5. Similarly for `ExtendFootprintTTLParallelApplyHelper` and `RestoreFootprintParallelApplyHelper` (though their footprints are small)
- **Change description**: Pre-allocate hash map buckets for the per-tx dirty-entry map using the known RW footprint size × 2, eliminating all rehash rounds during `recordStorageChanges()`
- **Correctness check**: Run `[tx]` tagged tests and `[soroban]` tagged tests — the change only affects internal capacity, not map semantics. Also run `[ledger]` tests that exercise parallel apply.
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` with SAC scenarios at T=8. Expected improvement: 3-7% on SAC median/p99 ledger close time. Negligible effect on `custom_token` and `soroswap`.
