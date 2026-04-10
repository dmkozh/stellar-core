# H002: Reserve Stage And Thread Parallel-Apply Hash Tables From Known Cluster Footprints

**Date**: 2026-04-10
**Subsystem**: crypto, transactions
**Severity**: Medium
**Impact**: serial pre-execution rehashing that reduces parallel-apply scalability
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Before a stage starts executing in parallel, the bookkeeping containers built
from that stage's and cluster's already-known footprints should be pre-sized to
their approximate final cardinality. The apply thread and worker startup paths
should spend their time loading entries, not repeatedly rehashing large
`LedgerKey` sets and maps assembled from those footprints.

## Mechanism

`getReadWriteKeysForStage()` starts from an empty `std::unordered_set`, then
inserts every stage read-write key plus every Soroban TTL key. Likewise each
`ThreadParallelApplyLedgerState` starts with an empty `mThreadEntryMap` and
fills it in `collectClusterFootprintEntriesFromGlobal()` by probing the whole
cluster footprint, including TTL companions. In the SAC benchmark a single tx
contributes 101 explicit write keys and roughly 101 associated TTL keys, so
stage- and cluster-level containers can absorb thousands of inserts before the
first useful host execution. Every growth wave rehashes raw `LedgerKey`s on a
mostly serial path, which is especially harmful at `T=8` because it erodes the
parallel section's effective speedup.

## Trigger

Run SAC apply-load with `T=8` and sample `getReadWriteKeysForStage`,
`collectClusterFootprintEntriesFromGlobal`, allocator activity, and
`std::hash<LedgerKey>` before worker threads get into steady-state host
execution. Compare against a build that pre-counts footprint keys and calls
`reserve()` on the stage read-write set and per-thread entry map before bulk
insertions.

## Target Code

- `docs/apply-load-benchmark-sac.cfg:31-38` — benchmark drives batched SAC transfers with a 100-destination batch size
- `src/simulation/ApplyLoad.cpp:2069-2113` — SAC benchmark txs are generated in batched form
- `src/simulation/TxGenerator.cpp:1480-1512` — each batch-transfer tx contributes 101 read-write keys before TTL expansion
- `src/transactions/ParallelApplyUtils.cpp:99-117` — `getReadWriteKeysForStage()` bulk-inserts stage keys and TTL keys into an empty set
- `src/transactions/ParallelApplyUtils.cpp:545-559` — `commitChangesFromThreads()` rebuilds that stage-level set every stage
- `src/transactions/ParallelApplyUtils.h:103-112` — per-thread state owns `mThreadEntryMap` and `mRoTTLBumps`
- `src/transactions/ParallelApplyUtils.cpp:563-607` — thread startup bulk-loads cluster footprint entries into `mThreadEntryMap`
- `src/transactions/ParallelApplyUtils.h:211-221` — stage-to-stage propagation also relies on `mGlobalEntryMap`
- `src/transactions/ParallelApplyUtils.cpp:333-355` — global map classic preloads also insert into an unreserved map
- `src/ledger/LedgerHashUtils.h:178-184` — contract-data key hashes are expensive enough that rehashing them repeatedly is non-trivial

## Evidence

The final cardinality of these containers is predictable from the stage and
cluster footprints before the first insert occurs. The current code does not
reserve capacity for the stage set, the thread entry map, or the global entry
map, even though the benchmark intentionally feeds them repeated large batched
SAC write footprints and their TTL companions. That means the rehash work is
fully paid on the apply/setup path instead of being avoided with one sizing
decision.

## Anti-Evidence

This benefit depends on the cluster actually carrying large footprints; it will
be much smaller in `custom_token` transfer and `soroswap` swap workloads, which
touch only a handful of write keys per tx. The strongest signal should therefore
be the parallel SAC benchmark, not every scenario uniformly.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — distinct from reviewed H001 (tx-level `mTxEntryMap`) and reviewed H002-prehash (hash caching). This targets stage-level and thread-level containers.

### Trace Summary

Traced the stage/thread container lifecycle for SAC T=8. `run_apply_load_matrix.py` sets `APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS = thread_count`, so at T=8 there are 8 clusters with ~4 actual txs each. `getReadWriteKeysForStage()` builds a `std::unordered_set<LedgerKey>` from empty to ~6464 entries (32 txs × 101 RW keys × 2 for TTL companions) on the serial commit path. Each `mThreadEntryMap` grows from empty to ~808 entries. Additionally, `mGlobalEntryMap` grows from ~32 classic entries to ~6000+ during `commitChangeFromThread()` calls. None of these containers are reserved.

### Code Paths Examined

- `src/transactions/ParallelApplyUtils.cpp:99-117` — `getReadWriteKeysForStage()` creates empty `std::unordered_set<LedgerKey>`, inserts all stage RW keys + TTL companions. No `reserve()`.
- `src/transactions/ParallelApplyUtils.cpp:545-559` — `commitChangesFromThreads()` calls `getReadWriteKeysForStage()` once per stage (serial), then iterates threads calling `commitChangeFromThread()` which emplaces into `mGlobalEntryMap` (also unreserved for Soroban entries).
- `src/transactions/ParallelApplyUtils.cpp:510-528` — `commitChangeFromThread()` calls `mGlobalEntryMap.emplace(key, ...)` for each dirty entry, growing the global map from ~32 classic entries toward ~6000+.
- `src/transactions/ParallelApplyUtils.cpp:563-607` — `collectClusterFootprintEntriesFromGlobal()`: for each footprint key, does `mThreadEntryMap.find()` + `globalEntryMap.find()` + conditional `mThreadEntryMap.emplace()`. Grows from empty to ~808 entries per thread.
- `src/ledger/LedgerHashUtils.h:178-184` — `CONTRACT_DATA` hash: `std::hash<SCAddress>` → `shortHash::computeHash` (mutex + SipHash on 8 bytes), then `shortHash::xdrComputeHash(SCVal key)` → `XDRShortHasher` (mutex + XDR serialize ~64 bytes + SipHash). ~100-150ns per hash.
- `src/util/HashOfHash.cpp:12-18` — `std::hash<uint256>` calls `shortHash::computeHash` on first 8 bytes, acquiring `gKeyMutex` each time. Used by TTL key hashing.
- `scripts/run_apply_load_matrix.py:269` — confirms `APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS` is set to `thread_count` (=8 for T=8).

### Findings

**The inefficiency is real but small.** For SAC T=8 (TX=3200, batch=100, 32 actual txs, 8 clusters):

1. **Stage set** (`getReadWriteKeysForStage`): ~6464 entries from empty. Starting from default bucket count (~1), std::unordered_set rehashes at ~13 thresholds. Total rehash overhead (geometric series): ~6500 extra hash computations × ~135ns avg = ~0.88ms. This is on the serial commit path, once per stage.

2. **Global map** (`mGlobalEntryMap`): grows from ~32 classic entries to ~6000+ during `commitChangeFromThread` calls. Rehash overhead: ~0.85ms on the serial path.

3. **Thread maps** (`mThreadEntryMap`): ~808 entries per thread from empty. Rehash overhead: ~0.11ms per thread. With 8 parallel threads, wall-clock is ~0.11ms.

**Total estimated savings: ~1.8-2.2ms per ledger.** Against estimated SAC T=8 close times of ~80-120ms, this is approximately **1.5-2.5%** — well below the 5% "Low" threshold.

**The fix is trivially correct.** Add `reserve()` calls before bulk insertion loops:
- `getReadWriteKeysForStage()`: count keys first or estimate from stage tx count × avg footprint
- `collectClusterFootprintEntriesFromGlobal()`: count cluster footprint keys
- `mGlobalEntryMap`: estimate from total Soroban footprint across stages

**No correctness constraints violated.** `reserve()` only affects internal capacity, not map semantics. No ownership, thread safety, or API contract issues.

**Diminishing value if reviewed H002-prehash is implemented.** If `LedgerKey` hashes are cached (per reviewed/002-prehash-parallel-apply-ledger-keys.md), rehashing becomes a trivial O(1) per-element operation, reducing the benefit of capacity pre-planning to just memory allocation avoidance (~8 alloc/dealloc cycles per container).

**Hypothesis severity downgraded from Medium to Informational.** The claimed "Medium" (10-20% improvement) overstates the impact. The serial-path overhead is ~2ms against ~100ms close times. The claim about "eroding T=8 speedup" is partially valid for thread setup but the per-thread overhead is only ~0.11ms.

### PoC Guidance

- **Target code**:
  1. `src/transactions/ParallelApplyUtils.cpp:99-117` — in `getReadWriteKeysForStage()`, add a pre-count loop to sum `footprint.readWrite.size() * 2` across stage txs, then `res.reserve(count)` before the insertion loop
  2. `src/transactions/ParallelApplyUtils.cpp:563-607` — in `collectClusterFootprintEntriesFromGlobal()`, pre-count cluster footprint keys (RW + RO + TTL companions), then `mThreadEntryMap.reserve(count)` before the fetch loop
  3. `src/transactions/ParallelApplyUtils.cpp:510-528` — for `mGlobalEntryMap`, reserve before the commit phase using total dirty entry count across threads
- **Change description**: Add `reserve()` calls to stage set, thread entry map, and global entry map before bulk insertions. Estimate capacity from known footprint sizes.
- **Correctness check**: Run `[tx]` and `[soroban]` tagged tests. This is a capacity-only change with no semantic effect.
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` SAC T=8 scenario. Expected improvement: ~1.5-2.5% on median close time — likely within noise. Best combined with reviewed H001 (tx-level reserve) for cumulative benefit.
