# H003: Preload Shared RO In-Memory Entries Once Per Cluster

**Date**: 2026-04-10
**Subsystem**: soroban
**Severity**: Low
**Impact**: Parallel apply CPU / repeated deep copies from in-memory state
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Within a parallel-apply cluster, immutable Soroban read-only entries that are
shared across many transactions should be copied from `InMemorySorobanState` at
most once per thread state. Repeated transactions in the same cluster should hit
the thread-local entry map instead of deep-copying the same contract code or
instance entry on every `addReads()` call.

## Mechanism

`collectClusterFootprintEntriesFromGlobal` only preloads keys already present in
the global entry map, which mainly covers prior writes and modified classic
entries. Shared RO Soroban entries that live only in `InMemorySorobanState`
remain absent from `mThreadEntryMap`, so every transaction misses the thread map
and `getLiveEntryOpt` falls through to `InMemorySorobanState::get()` and
`std::make_optional(*res)`. In `custom_token` and `soroswap`, hundreds of
transactions in a cluster reuse the same code/instance keys, so these deep
copies repeat many times before serialization even begins.

## Trigger

Run apply-load `custom_token` or `soroswap` with `T=8` and profile
`ThreadParallelApplyLedgerState::getLiveEntryOpt` plus `addReads`. If the
hypothesis is correct, a meaningful share of pre-host time will be in
`std::make_optional(*res)` for RO contract code / instance entries that are
identical across many transactions in the same cluster.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::collectClusterFootprintEntriesFromGlobal:563-608` — preloads only keys already resident in the global map
- `src/transactions/ParallelApplyUtils.cpp:ThreadParallelApplyLedgerState::getLiveEntryOpt:700-734` — falls through to `InMemorySorobanState::get()` and deep-copies missing RO entries
- `src/ledger/InMemorySorobanState.cpp:InMemorySorobanState::get:205-236` — returns shared immutable entries that are copied again by callers
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::addReads:360-466` — consumes those copied entries immediately for bridge marshaling
- `src/simulation/TxGenerator.cpp:invokeTokenTransfer:840-845` — every custom-token TX reuses the same instance read-only keys
- `src/simulation/ApplyLoad.cpp:2962-2985` — soroswap deposit path reuses router/factory/pair and SAC RO keys across many TXs

## Evidence

The thread-state preload path already exists; it just ignores RO entries that
are only present in `InMemorySorobanState`. The benchmark generators make the
sharing pattern explicit: custom-token transactions all read the same contract
code + instance, and soroswap transactions read the same router/factory/pair
code and instance keys repeatedly. That creates a cluster-local reuse
opportunity without needing cross-ledger invalidation.

## Anti-Evidence

This only removes the repeated deep copy, not the subsequent XDR serialization,
so its ceiling is lower than a full serialized-entry cache. Preloading more RO
entries into `mThreadEntryMap` also increases thread-state setup work and memory
footprint, so the win depends on clusters being large enough that the repeated
copies dominate the one-time preload cost.
