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
