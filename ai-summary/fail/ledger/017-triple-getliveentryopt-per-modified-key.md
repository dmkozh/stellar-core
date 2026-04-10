# H017: Triple getLiveEntryOpt Lookup Per Modified Key in Parallel Apply

**Date**: 2025-07-14
**Subsystem**: ledger
**Severity**: Informational
**Impact**: Parallel apply throughput
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

For each key modified by a successful Soroban transaction during parallel apply,
the "previous value" of the entry should be retrieved ONCE and reused across
all consumers that need it (delta construction, metadata generation, and commit
logic), rather than being independently looked up 3 times.

## Mechanism

When a Soroban transaction succeeds, three sequential functions each call
`getLiveEntryOpt(key)` to retrieve the previous value of each modified key:

1. `setEffectsDeltaFromSuccessfulTx` (ParallelApplyUtils.cpp:797) — builds
   the invariant delta by comparing previous vs. new entries
2. `setLedgerChangesFromSuccessfulOp` (TransactionMeta.cpp:406) — builds
   operation metadata with before/after entry pairs
3. `commitChangeFromSuccessfulTx` (ParallelApplyUtils.cpp:765) — performs
   the actual commit, needs previous value to determine RO TTL merge behavior

All three run on the same worker thread, for the same key, within the same
transaction processing. The first call (if the key hasn't been seen before in
this cluster) may fall through to `InMemorySorobanState::get()`, but subsequent
calls for the same key will hit the thread-local `mThreadEntryMap` cache.

For SAC benchmark: ~3,200 txs × 4 modified keys per tx × 2 extra lookups =
~25,600 extra hash map lookups. At ~100ns per thread-local hash map lookup:
~2.6ms total across 8 threads = ~0.3ms wall time.

## Trigger

Profile parallel apply with SAC benchmark at T=8. Count `getLiveEntryOpt`
calls per unique key per transaction.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:790-829` —
  `setEffectsDeltaFromSuccessfulTx` (first lookup at line 797)
- `src/transactions/TransactionMeta.cpp:390-461` —
  `setLedgerChangesFromSuccessfulOp` (second lookup)
- `src/transactions/ParallelApplyUtils.cpp:761-787` —
  `commitChangeFromSuccessfulTx` (third lookup at line 765)

## Evidence

The pattern is clearly visible: three distinct functions, each independently
calling `getLiveEntryOpt(key)` for the same key within the same tx processing
sequence. A refactor to return previous values from the commit function and
pass them to delta/meta builders would eliminate 2 of 3 lookups.

## Anti-Evidence

After the first transaction in a cluster touches a key, subsequent lookups
for that key hit the thread-local `mThreadEntryMap` (O(1) hash map lookup,
~100ns). The extra cost is only significant for the FIRST transaction per key,
where the fallthrough to InMemorySorobanState involves SHA-256 (covered by
H001). With H001 implemented, the per-lookup cost would be uniformly ~100ns.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-14
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The total wall-time impact is ~0.3ms (2.6ms across 8 threads), which is well
below the Low severity threshold (5% of close time = ~7.5-15ms at T=8). Even
in the worst case (all first-touch lookups falling through to
InMemorySorobanState with SHA-256), the impact is ~8ms wall time — and this
cost is already attributed to H001 (ValueEntry SHA-256 recomputation). The
triple-lookup pattern is a code quality issue rather than a performance
bottleneck.

### Lesson Learned

Hash map lookups in thread-local maps are ~100ns. Even at 25,600 extra lookups,
the total cost is negligible. Focus performance investigations on operations
that are 100x+ more expensive per call (SHA-256 ~1μs, heap allocations ~50ns,
XDR serialization ~500ns) rather than hash map lookups.
