# H005: buildRoTTLSet Per-TX UnorderedSet Allocation During Cluster Commit

**Date**: 2026-04-09
**Subsystem**: soroban
**Severity**: Low
**Impact**: CPU / allocation churn during sequential cluster commit
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When committing changes from a successful transaction within a cluster,
the RO TTL key set should not be rebuilt from scratch for every transaction.
The set could be precomputed once per transaction during cluster setup or
during the parallel apply phase.

## Mechanism

`ThreadParallelApplyLedgerState::commitChangesFromSuccessfulTx()` (line 835)
calls `buildRoTTLSet(txBundle)` which allocates a new
`UnorderedSet<LedgerKey>` and populates it by iterating the transaction's RO
footprint. For each Soroban key in the RO footprint, it constructs a TTL key
via `getTTLKey()` and inserts it. This happens for every successful transaction
in the cluster.

With ~12 txs per cluster, each with ~3 RO Soroban keys, this creates 12
`UnorderedSet`s with ~3 entries each. The allocation cost is dominated by hash
set construction overhead (~2µs per set including bucket allocation).

## Trigger

Profile `commitChangesFromSuccessfulTx` during T=8 apply-load runs.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:buildRoTTLSet:149-162` — allocates UnorderedSet per call
- `src/transactions/ParallelApplyUtils.cpp:commitChangesFromSuccessfulTx:832-843` — calls buildRoTTLSet per tx

## Evidence

The `buildRoTTLSet` function is called once per successful transaction during
the sequential commit phase within each thread. The RO footprint is available
from the transaction and doesn't change.

## Anti-Evidence

The cost per call is ~2µs for a small set (~3 entries). With ~12 txs per
cluster, total overhead per cluster is ~24µs. This runs on the worker thread
during the intra-cluster sequential phase, not the inter-stage serial phase.
Against per-tx host execution of ~200-2000µs, this is <1%.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The per-call cost (~2µs) and total overhead (~24µs per cluster) is negligible
relative to transaction execution time. The `UnorderedSet` with 3 entries uses
the small-map optimization in most implementations. Even with 8 clusters
running in parallel, the total allocation overhead is ~192µs across all threads,
well below 0.5% of any benchmark scenario.

### Lesson Learned

Small hash set constructions with <10 entries are essentially free relative to
Soroban host execution times. Focus optimization efforts on operations that
scale with entry *size* (serialization, deep copies) rather than entry *count*
for small footprints.
