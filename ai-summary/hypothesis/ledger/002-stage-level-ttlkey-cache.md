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
