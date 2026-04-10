# H004: Move Per-Cluster Thread-State Preload Off the Apply Thread

**Date**: 2026-04-10
**Subsystem**: transaction-ledger (ledger/LedgerManagerImpl, transactions/ParallelApplyUtils)
**Severity**: Low
**Impact**: Serial setup before worker-side Soroban execution
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Per-cluster `ThreadParallelApplyLedgerState` setup should happen on worker
threads, or in a separate parallel preload step, so that cluster initialization
scales with the same thread count as cluster execution.

## Mechanism

`applySorobanStageClustersInParallel` constructs each
`ThreadParallelApplyLedgerState` on the apply thread before launching the async
task. The constructor immediately walks the full cluster footprint in
`collectClusterFootprintEntriesFromGlobal`, performing global-map lookups and
TTL-key derivation before that worker can start useful work. On apply-load's
fixed one-stage / max-cluster topology, this staggers worker startup behind a
serial chain of cluster-preload passes and leaves the last worker waiting on
setup that is logically independent across clusters.

## Trigger

Run `scripts/run_apply_load_matrix.py` with `T=8`, especially `soroswap` and
`custom_token`, and profile time from entering
`applySorobanStageClustersInParallel` until the final worker reaches its first
host call. The hypothesis is strongest when each cluster has many transactions
with non-trivial footprints.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2426-2470` — constructs each `ThreadParallelApplyLedgerState` on the main/apply thread, then launches `std::async`
- `src/transactions/ParallelApplyUtils.cpp:563-608` — `collectClusterFootprintEntriesFromGlobal` walks every tx footprint in the cluster during construction
- `src/transactions/ParallelApplyUtils.cpp:610-623` — constructor eagerly performs the preload
- `src/simulation/ApplyLoad.cpp:2016-2025` — benchmark asserts exactly one stage and maximum cluster parallelism
- `src/simulation/ApplyLoad.cpp:3140-3168` and `src/simulation/TxGenerator.cpp:840-865` — benchmark txs have repeated multi-key footprints that amplify preload work

## Evidence

- The thread-state constructor is invoked before each `std::async` launch, so worker startup is serialized through constructor completion.
- The preload walk touches every read-only and read-write key in the cluster and adds an extra TTL lookup for each Soroban key.
- This setup work is outside the Soroban VM and therefore is a pure scheduler/apply-path cost in the benchmark.

## Anti-Evidence

- Constructor work for earlier clusters overlaps with already-launched workers, so not all of the setup time is exposed end-to-end.
- If the worker-side host execution dominates wall time, this startup stagger may only produce a small single-digit gain.
- Any redesign has to preserve the scope-lifetime guarantees around `globalState` and thread-local state construction.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated
**Failed At**: reviewer

### Trace Summary

Traced the full parallel apply path from `applySorobanStageClustersInParallel`
(LedgerManagerImpl.cpp:2441-2449) through the `ThreadParallelApplyLedgerState`
constructor (ParallelApplyUtils.cpp:679-692) into
`collectClusterFootprintEntriesFromGlobal` (ParallelApplyUtils.cpp:632-677).
Confirmed the serial construction pattern: each cluster's thread state is
constructed on the apply thread before `std::async` launches the worker. The
construction walks all tx footprint keys, calling `getTTLKey` (SHA256) for each
Soroban key and performing hash lookups in the global entry map.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:2441-2449` — serial for loop constructs `ThreadParallelApplyLedgerState` then launches `std::async`; confirmed N clusters are initialized sequentially
- `src/transactions/ParallelApplyUtils.cpp:679-692` — constructor copies LCL snapshot, references in-memory state, then calls `collectClusterFootprintEntriesFromGlobal`
- `src/transactions/ParallelApplyUtils.cpp:632-677` — walks every footprint key in every tx in the cluster; calls `getTTLKey(key)` for every Soroban key (NOT deduplicated), `fetchFromGlobal` is deduplicated via `mThreadEntryMap.find(key)` check
- `src/ledger/LedgerTypeUtils.cpp:31-38` — `getTTLKey`: computes `sha256(xdr::xdr_to_opaque(e))` unconditionally (~700ns per call)
- `src/transactions/ParallelApplyUtils.cpp:298-323` — `GlobalParallelApplyLedgerState` constructor: for first/only stage, global entry map contains only classic entries (accounts/trustlines) from `preParallelApplyAndCollectModifiedClassicEntries`
- `src/bucket/BucketListSnapshot.cpp:85-96` — `SearchableBucketListSnapshot` copy constructor: only copies shared_ptrs and reference_wrappers (~1-5µs), thread-safe for concurrent reads
- `src/ledger/LedgerEntryScope.h:352` — `mActive` is plain `bool` (not atomic); written by `DeactivateScopeGuard` before any thread launch, safe via happens-before
- `src/simulation/ApplyLoad.cpp:3140-3168` — soroswap tx has 10 footprint keys: 5 RO + 5 RW, of which 8 are Soroban entries requiring `getTTLKey`

### Why It Failed

The serial overhead from cluster state construction is too small to produce a
measurable benchmark improvement:

1. **Dominant cost is `getTTLKey` SHA256**, already addressed by viable H010
   (cache-getttlkey-sha256). For soroswap at TX=1000, T=8: 125 txs/cluster ×
   8 Soroban keys × 700ns = ~700µs per cluster. Total serial stagger for 8
   clusters: 7 × 700µs ≈ 4.9ms. Against ~200ms total close time, this is
   ~2.5% — below the 5% Low threshold.

2. **After H010 is implemented**, `getTTLKey` cost becomes negligible. Remaining
   per-cluster construction cost is ~100-200µs (hash lookups, small entry
   copies, snapshot shared_ptr copies). Serial stagger: 7 × 150µs ≈ 1.05ms,
   which is ~0.5% of close time.

3. **Global map is mostly empty for Soroban keys**: For the first (and only,
   in apply-load) stage, the global entry map contains classic entries from
   fee/seqnum processing. Soroban entries (CONTRACT_DATA, CONTRACT_CODE, TTL)
   are NOT in the global map — they are fetched from `InMemorySorobanState`
   during execution. So `fetchFromGlobal` returns early for most lookups,
   only finding shared accounts/trustlines.

4. **Overlap mitigates the stagger**: Workers for clusters 0-6 are already
   executing while later clusters' states are constructed. Only the last
   cluster's construction adds to end-to-end time without any parallel overlap.

### Lesson Learned

When evaluating serial-setup overhead before parallel dispatch, estimate the
per-unit setup cost against the per-unit parallel execution cost. If setup is
<1% of execution time per unit, the serial stagger is negligible even for many
units. Also check whether the dominant cost within the setup (here, SHA256
hashing) is already addressed by another optimization hypothesis.
