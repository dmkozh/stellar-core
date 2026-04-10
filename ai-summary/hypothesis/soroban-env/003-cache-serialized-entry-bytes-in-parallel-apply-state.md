# H003: Cache Serialized Entry Bytes in Parallel-Apply State, Not Per Read

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Low
**Impact**: Parallel apply throughput / repeated state marshaling
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Within a parallel-apply cluster, once a ledger entry state has been materialized
for bridge input, repeated reads of that same state should reuse cached encoded
bytes until the entry changes. The bridge should not re-run `xdr_to_opaque` on
the same thread-local entry state every time a later transaction in the same
cluster needs that key again.

## Mechanism

`addReads()` serializes every fetched entry and TTL entry into a fresh `CxxBuf`
for every Soroban transaction. But parallel apply already maintains
stage-spanning thread and tx entry maps (`mThreadEntryMap`, `mTxEntryMap`) and
central mutation points (`upsertEntry`, `eraseEntry`, `commitChangesFromSuccessfulTx`)
that know exactly when an entry's bytes become stale. Attaching a lazy
serialized-byte sidecar to that existing state would let repeated RO keys and
recurrent RW keys (such as a hot pair instance touched by many swaps in the
same cluster) pay the XDR walk once per state transition instead of once per
transactional read.

## Trigger

Run the `soroswap,T=8` benchmark. The load generator intentionally creates one
pair per dependent-tx cluster, so each cluster repeatedly accesses the same
router/pair code and pair-instance state across many swaps. Compare profiles for
`InvokeHostFunctionApplyHelper::addReads()` before and after caching serialized
bytes in the parallel-apply maps.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:360-466` — `addReads()` serializes every live entry and TTL entry with `toCxxBuf(...)`
- `src/transactions/ParallelApplyUtils.cpp:563-608` — cluster footprint materialization already centralizes which keys enter thread-local state
- `src/transactions/ParallelApplyUtils.cpp:700-735` — thread state resolves repeated key lookups
- `src/transactions/ParallelApplyUtils.cpp:738-750` — `upsertEntry()` is a natural invalidation/update point for cached bytes
- `src/transactions/ParallelApplyUtils.cpp:832-843` — successful tx commits propagate repeated-entry state forward within a cluster
- `src/simulation/ApplyLoad.cpp:3134-3168` — `soroswap` swaps repeatedly read shared router/pair code and rewrite the same pair instance inside each cluster

## Evidence

Parallel apply is already structured around reusable per-cluster entry maps, so
the bridge has a persistent place to remember both the current `LedgerEntry`
value and any cached encoded representation of it. The `soroswap` benchmark
intentionally concentrates traffic onto one pair per cluster, which means the
same pair instance and shared code/instance keys are marshaled over and over in
the measured path even though the cluster-local state already exists in memory.

## Anti-Evidence

The previously rejected read-only-only cache showed that large code-entry
serialization is still dominated by memcpy into owned buffers, so this idea only
becomes interesting if the repeated mutable keys also matter. The cache would
need careful invalidation on every `upsertEntry` / `eraseEntry` to avoid stale
bridge input, and the total win may still stay near the low single-digit range
if the dominant reused entries are large `opaque_vec<>` code blobs rather than
small mutable instances.
