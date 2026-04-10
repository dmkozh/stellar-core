# H002: Pre-compute Stage-Level TTL Key Cache to Eliminate Redundant SHA-256

**Date**: 2025-07-14
**Subsystem**: ledger
**Severity**: Low
**Impact**: Serial + parallel apply throughput (5-10% improvement at T=8)
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

Each unique LedgerKey in a stage's transaction footprints should have its
corresponding TTL key (which requires SHA-256 hashing) computed exactly ONCE
per stage, rather than 3-5 times across different phases of the parallel apply
lifecycle.

## Mechanism

`getTTLKey(LedgerKey)` at `src/ledger/LedgerTypeUtils.cpp:30-38` calls
`sha256(xdr::xdr_to_opaque(e))` on every invocation with no caching. The same
LedgerKeys are passed to `getTTLKey` at 5 distinct call sites during a single
stage's apply lifecycle, resulting in massive redundant SHA-256 computation:

1. **`collectClusterFootprintEntriesFromGlobal`** (ParallelApplyUtils.cpp:602)
   — Called SERIALLY in ThreadParallelApplyLedgerState constructor for each
   cluster. For 8 clusters × ~2,000 Soroban entries each = ~16,000 SHA-256
   calls, all serial on the apply thread.

2. **`buildRoTTLSet`** (ParallelApplyUtils.cpp:159) — Called per transaction
   inside `commitChangesFromSuccessfulTx`. ~3,200 txs × ~4 RO Soroban entries
   = ~12,800 SHA-256 calls across 8 threads.

3. **`flushRoTTLBumpsInTxWriteFootprint`** (ParallelApplyUtils.cpp:639) —
   Called per transaction before each apply. ~3,200 txs × ~2 RW Soroban
   entries = ~6,400 SHA-256 calls across 8 threads.

4. **`addReads` in InvokeHostFunctionOpFrame** (InvokeHostFunctionOpFrame.cpp:380)
   — Called per transaction during VM setup. ~3,200 txs × ~5 Soroban entries
   = ~16,000 SHA-256 calls across 8 threads.

5. **`getReadWriteKeysForStage`** (ParallelApplyUtils.cpp:113) — Called SERIALLY
   after parallel apply completes. ~3,200 txs × ~2 RW Soroban entries =
   ~6,400 SHA-256 calls, all serial.

**Total: ~57,600 explicit getTTLKey SHA-256 calls per ledger close.** Of these,
~22,400 are on the serial apply thread (~22ms at 1μs/SHA-256) and ~35,200 are
distributed across 8 threads (~4.4ms wall time). With a stage-level cache, each
unique Soroban key's TTL key is computed once. For ~4,000 unique keys: ~4,000
SHA-256 → ~4ms total, saving ~22ms.

This hypothesis is COMPLEMENTARY to H001 (which addresses HIDDEN SHA-256 inside
unordered_set operations on mContractDataEntries). This addresses EXPLICIT
getTTLKey calls at known call sites.

## Trigger

Run SAC benchmark at T=8 with 3200 transactions. Profile SHA-256 time in
`getTTLKey`. The function will appear as a hot spot called from 5 distinct
stack traces corresponding to the call sites above.

## Target Code

- `src/ledger/LedgerTypeUtils.cpp:30-38` — `getTTLKey` performs SHA-256 on
  every call with no memoization
- `src/transactions/ParallelApplyUtils.cpp:602` —
  `collectClusterFootprintEntriesFromGlobal` calls getTTLKey per Soroban key
  (SERIAL, per cluster)
- `src/transactions/ParallelApplyUtils.cpp:149-162` — `buildRoTTLSet` calls
  getTTLKey per RO Soroban key (per tx, parallel)
- `src/transactions/ParallelApplyUtils.cpp:639` —
  `flushRoTTLBumpsInTxWriteFootprint` calls getTTLKey per RW Soroban key
  (per tx, parallel)
- `src/transactions/InvokeHostFunctionOpFrame.cpp:380` — `addReads` calls
  getTTLKey per Soroban footprint key (per tx, parallel)
- `src/transactions/ParallelApplyUtils.cpp:100-118` —
  `getReadWriteKeysForStage` calls getTTLKey per RW Soroban key (SERIAL)

## Evidence

1. **Five distinct call sites** all call `getTTLKey` on overlapping sets of
   LedgerKeys from the same stage's transaction footprints. The overlap is
   near-total: a key in the RW footprint gets hashed in sites 1, 3, 4, and 5;
   a key in the RO footprint gets hashed in sites 1, 2, and 4.

2. **Serial cluster initialization is a bottleneck**: The constructor of
   `ThreadParallelApplyLedgerState` (lines 610-623) runs on the apply thread
   for ALL clusters sequentially (inside the loop at
   LedgerManagerImpl.cpp:2441-2449). The `collectClusterFootprintEntriesFromGlobal`
   call at line 622 accounts for ~16,000 SHA-256 calls in this serial phase.

3. **The fix is straightforward**: Build an `UnorderedMap<LedgerKey, LedgerKey>`
   cache once at stage construction (before cluster init loop), pass it by
   const-reference to all consumers. The cache is immutable after construction
   and can be shared across threads without synchronization.

4. **getTTLKey is pure/deterministic**: Same input always produces same output,
   making it safe to cache without invalidation concerns.

## Anti-Evidence

1. The cache would consume ~4,000 entries × ~200 bytes/entry = ~800KB per stage.
   This is negligible compared to the overall apply state memory footprint.

2. Building the cache requires one pass over all footprint entries (~8,000
   entries), which costs ~8,000 SHA-256 + hash map insertions = ~12ms. This
   front-loads the cost but the net savings from eliminating ~49,600 redundant
   SHA-256 calls is ~46ms, for a net benefit of ~34ms.

3. The fix requires threading the cache through 5 different call sites, which
   increases interface complexity. The InvokeHostFunctionOpFrame::addReads path
   is particularly tricky as it's called through the LedgerAccessHelper
   abstraction.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated (complementary to H001 cached-ttl-key-hash-in-contract-data-map which targets internal unordered_set hashing, not explicit getTTLKey call sites)

### Trace Summary

Traced the complete parallel apply lifecycle from `LedgerManagerImpl::applySorobanStageClustersInParallel` through all 5 claimed call sites. Confirmed that `getTTLKey` at `LedgerTypeUtils.cpp:31-37` calls `sha256(xdr::xdr_to_opaque(e))` with no caching. All 5 call sites exist at the stated line numbers and are invoked per-key per-transaction or per-key per-cluster as claimed. The serial cluster initialization loop at `LedgerManagerImpl.cpp:2441-2449` constructs each `ThreadParallelApplyLedgerState` sequentially, confirming that site 1 (~14,400 SHA-256 calls) runs entirely on the apply thread. Additionally found 3 more `getTTLKey` calls in `InvokeHostFunctionOpFrame::recordStorageChanges` (lines 671, 701) and `handleArchivedEntry` (line 1063) that the hypothesis didn't count, though the latter is rare (restoration path only).

### Code Paths Examined

- `src/ledger/LedgerTypeUtils.cpp:31-37` — Confirmed: `getTTLKey(LedgerKey)` calls `sha256(xdr::xdr_to_opaque(e))` on every invocation, no caching
- `src/transactions/ParallelApplyUtils.cpp:592-607` — Confirmed: `collectClusterFootprintEntriesFromGlobal` iterates all RO+RW keys per cluster, calls `getTTLKey` per Soroban entry (line 602)
- `src/transactions/ParallelApplyUtils.cpp:148-161` — Confirmed: `buildRoTTLSet` builds fresh `UnorderedSet<LedgerKey>` per transaction via `getTTLKey` per RO Soroban key (line 159)
- `src/transactions/ParallelApplyUtils.cpp:626-659` — Confirmed: `flushRoTTLBumpsInTxWriteFootprint` calls `getTTLKey` per RW Soroban key (line 639) before each tx apply
- `src/transactions/InvokeHostFunctionOpFrame.cpp:360-502` — Confirmed: `addReads` calls `getTTLKey` per Soroban key (line 380) during footprint validation
- `src/transactions/InvokeHostFunctionOpFrame.cpp:664-705` — Found: `recordStorageChanges` calls `getTTLKey` at lines 671 and 701 for created/erased entry validation (not counted in hypothesis)
- `src/transactions/ParallelApplyUtils.cpp:99-118` — Confirmed: `getReadWriteKeysForStage` calls `getTTLKey` per RW Soroban key (line 113), runs serially after parallel phase
- `src/ledger/LedgerManagerImpl.cpp:2441-2449` — Confirmed: cluster init loop is sequential; each `ThreadParallelApplyLedgerState` constructor is called serially before async launch
- `src/transactions/ParallelApplyUtils.cpp:831-843` — Confirmed: `commitChangesFromSuccessfulTx` calls `buildRoTTLSet` per tx in parallel
- `src/transactions/ParallelApplyUtils.cpp:545-559` — Confirmed: `commitChangesFromThreads` calls `getReadWriteKeysForStage` serially after all threads complete

### Findings

1. **All 5 call sites confirmed.** The hypothesis accurately identifies the locations, call frequencies, and serial/parallel nature of each site.

2. **Three additional call sites found** in `InvokeHostFunctionOpFrame.cpp` (lines 671, 701, 1063) that could also benefit from the cache, though they represent a smaller fraction of total calls.

3. **SHA-256 cost estimate is conservative.** `getTTLKey` uses `sha256(xdr::xdr_to_opaque(e))` which involves both XDR serialization to a heap-allocated `std::vector<uint8_t>` AND SHA-256 computation. The XDR allocation overhead adds ~200ns per call on top of the SHA-256 cost. Using `xdrSha256` (which streams XDR directly into the hash state without intermediate allocation) could reduce per-call cost to ~400ns, but the cache approach eliminates both costs entirely.

4. **Cache construction cost is lower than estimated.** The hypothesis estimates ~12ms for cache construction (~8,000 SHA-256). With dedup via `try_emplace`, only ~4,000 unique keys need hashing = ~3-4ms. This improves the net savings.

5. **Threading the cache is feasible.** The `GlobalParallelApplyLedgerState` already holds stage-wide data and is passed to `ThreadParallelApplyLedgerState` constructors. The cache can be owned by `GlobalParallelApplyLedgerState` and passed by const-ref. For `InvokeHostFunctionOpFrame::addReads`, the cache can flow through `ParallelLedgerAccessHelper` which already holds a reference to `ThreadParallelApplyLedgerState`.

6. **No correctness concerns.** `getTTLKey` is a pure function — same LedgerKey always produces the same TTL LedgerKey. The cache is immutable after construction and safely shared across threads without synchronization.

### PoC Guidance

- **Target code**:
  - `src/transactions/ParallelApplyUtils.h` — Add `UnorderedMap<LedgerKey, LedgerKey> mTTLKeyCache` member to `GlobalParallelApplyLedgerState`; add a `getTTLKeyFromCache(LedgerKey)` accessor. Add const-ref to `ThreadParallelApplyLedgerState`.
  - `src/transactions/ParallelApplyUtils.cpp` — Build cache in `GlobalParallelApplyLedgerState` constructor by iterating all stage footprint keys. Modify `collectClusterFootprintEntriesFromGlobal`, `buildRoTTLSet`, `flushRoTTLBumpsInTxWriteFootprint`, `getReadWriteKeysForStage` to use the cache instead of calling `getTTLKey`.
  - `src/transactions/InvokeHostFunctionOpFrame.cpp` — Modify `addReads` and `recordStorageChanges` to accept/use the TTL key cache via `ParallelLedgerAccessHelper`. The `LedgerAccessHelper` base class's `addReads` can accept an optional cache parameter (nullptr for non-parallel path).
- **Change description**: Build an immutable `UnorderedMap<LedgerKey, LedgerKey>` mapping all Soroban footprint keys to their TTL keys once per stage in `GlobalParallelApplyLedgerState`. Pass by const-ref to all 5+ call sites. Replace each `getTTLKey(lk)` call with a cache lookup. For the `addReads` path, add the cache reference to `ParallelLedgerAccessHelper` and pass it to `InvokeHostFunctionParallelApplyHelper`.
- **Correctness check**: Existing tests in `src/transactions/test/ParallelApplyTest.cpp` (especially the parallel apply test cases) and `src/transactions/test/InvokeHostFunctionTests.cpp` cover all affected code paths. Run `"[tx]"` and `"[soroban]"` test tags.
- **Benchmark focus**: Run SAC at T=8 with 3200 txs. Measure total stage apply time (serial cluster init + parallel apply + serial commit). Expect ~10-15ms reduction in serial portion and ~2-3ms reduction in parallel wall time, for a total ~12-18ms savings per ledger close (~5-10% improvement on SAC T=8).
