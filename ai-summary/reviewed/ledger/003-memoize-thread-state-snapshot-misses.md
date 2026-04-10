# H003: Memoize `getLiveEntryOpt` Snapshot Misses in Thread State

**Date**: 2026-04-10
**Subsystem**: ledger, transactions
**Severity**: Low
**Impact**: repeated snapshot/in-memory lookups on worker and post-worker bookkeeping paths
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Within a cluster, once a key has been loaded from the live snapshot or
`InMemorySorobanState`, later reads of the same pre-state key should reuse that
materialized value from thread-local state. The parallel-apply code should not
re-fetch and re-copy the same entry multiple times in one transaction's
host/setup/meta/commit flow.

## Mechanism

`ThreadParallelApplyLedgerState::getLiveEntryOpt` checks `mThreadEntryMap`, but
when the key is absent it reads from `mInMemorySorobanState` or
`mLCLSnapshot.loadLiveEntry(key)` and immediately returns a scoped copy without
memoizing it. The same key can then be reloaded repeatedly by
`TxParallelApplyLedgerState::upsertEntry`, `eraseEntryIfExists`,
`setEffectsDeltaFromSuccessfulTx`, `OperationMetaBuilder::setLedgerChangesFromSuccessfulOp`,
and `commitChangeFromSuccessfulTx`, each of which calls back into
`getLiveEntryOpt`.

Caching first-miss results as clean entries in thread-local state would turn
these repeated snapshot and in-memory lookups into cheap hash-map hits. This is
especially relevant in BUILD_TESTS benchmarks where transaction meta is forced
on, because successful Soroban txs already revisit modified keys again for both
delta and meta construction.

## Trigger

Run `scripts/run_apply_load_matrix.py` with any Soroban scenario in the test
build. Profile repeated calls to `ThreadParallelApplyLedgerState::getLiveEntryOpt`
for the same keys during one successful transaction and compare with a memoized
variant.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:getLiveEntryOpt:699-735` — reads snapshot/in-memory state on misses but does not cache the result
- `src/transactions/ParallelApplyUtils.cpp:commitChangeFromSuccessfulTx:761-787` — reloads pre-state per modified key
- `src/transactions/ParallelApplyUtils.cpp:setEffectsDeltaFromSuccessfulTx:790-828` — reloads previous state again for delta construction
- `src/transactions/TransactionMeta.cpp:setLedgerChangesFromSuccessfulOp:390-461` — reloads previous state again for op-meta changes
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::upsertEntry:907-951` — probes live existence via `getLiveEntryOpt`
- `src/transactions/ParallelApplyUtils.cpp:TxParallelApplyLedgerState::eraseEntryIfExists:953-967` — probes live existence via `getLiveEntryOpt`
- `src/ledger/LedgerManagerImpl.cpp:2645-2650` — BUILD_TESTS forces tx meta enabled in benchmark builds

## Evidence

- `getLiveEntryOpt` only consults `mThreadEntryMap` for preloaded global keys;
  snapshot/in-memory misses are returned directly and not retained.
- The same key is visibly re-read in multiple later phases of successful tx
  handling: host bookkeeping, delta construction, and op-meta construction.
- `OperationMetaBuilder::setLedgerChangesFromSuccessfulOp` and
  `setEffectsDeltaFromSuccessfulTx` each independently call
  `threadState.getLiveEntryOpt(lk)` for the same modified keys.
- In BUILD_TESTS benchmark binaries, tx meta stays enabled, so the extra
  op-meta readback is present even when metadata streaming is disabled.

## Anti-Evidence

- Some keys may truly be touched only once, reducing the benefit of memoization.
- Making `getLiveEntryOpt` populate a cache changes a logically-const path and
  would require careful scope and thread-safety auditing.
- If `mThreadEntryMap` growth meaningfully harms cache locality, a naive
  memoization strategy could trade one form of overhead for another.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full parallel Soroban apply path from `applyThread` → `parallelApply` → `commitChangesFromSuccessfulTx`. For each successful tx, every modified key triggers `ThreadParallelApplyLedgerState::getLiveEntryOpt` 2–3 times in the post-execution phase (`setEffectsDeltaFromSuccessfulTx`, `setLedgerChangesFromSuccessfulOp` when meta is enabled, and `commitChangeFromSuccessfulTx`). The global entry map (`preParallelApplyAndCollectModifiedClassicEntries`, line 325) explicitly skips Soroban entries (`if (isSorobanEntry(lk)) continue;`), so ALL Soroban keys miss the `mThreadEntryMap` and fall through to `InMemorySorobanState::get()` + deep copy each time.

### Code Paths Examined

- `src/transactions/ParallelApplyUtils.cpp:GlobalParallelApplyLedgerState::preParallelApplyAndCollectModifiedClassicEntries:325-386` — confirms only classic entries are loaded into the global map; Soroban entries skipped at line 338
- `src/transactions/ParallelApplyUtils.cpp:collectClusterFootprintEntriesFromGlobal:563-608` — thread map populated only from global map; Soroban entries absent
- `src/transactions/ParallelApplyUtils.cpp:getLiveEntryOpt:699-735` — on miss, reads InMemorySorobanState or snapshot, deep-copies via `std::make_optional(*res)` at line 734, does NOT cache
- `src/transactions/ParallelApplyUtils.cpp:setEffectsDeltaFromSuccessfulTx:790-828` — calls `getLiveEntryOpt(lk)` per modified key (redundant call #1)
- `src/transactions/TransactionMeta.cpp:setLedgerChangesFromSuccessfulOp:390-461` — calls `threadState.getLiveEntryOpt(lk)` per modified key (redundant call #2, only when meta enabled)
- `src/transactions/ParallelApplyUtils.cpp:commitChangeFromSuccessfulTx:761-787` — calls `getLiveEntryOpt(key)` per modified key (redundant call #3); then finally inserts dirty value via `upsertEntry`/`eraseEntry`
- `src/ledger/InMemorySorobanState.cpp:get:205-236` — returns `shared_ptr<LedgerEntry const>` from hash table; cheap lookup but the caller performs a deep copy
- `src/transactions/TransactionFrame.cpp:2241-2247` — confirms ordering: `setEffectsDelta` and `setLedgerChanges` both run before `commitChangesFromSuccessfulTx`
- `src/ledger/LedgerManagerImpl.cpp:2400-2406` — confirms `parallelApply` returns before `commitChangesFromSuccessfulTx`

### Findings

**The inefficiency is real and the mechanism is correctly identified.** For a successful Soroban tx modifying N keys, `ThreadParallelApplyLedgerState::getLiveEntryOpt` is called 2–3 times per key after the first lookup (2 in production without meta, 3 in BUILD_TESTS with meta). Each call performs a hash lookup in `InMemorySorobanState` (cheap) plus a deep copy of the `LedgerEntry` via `std::make_optional(*res)` (moderately expensive for CONTRACT_DATA entries).

**The proposed fix is architecturally sound.** `ThreadParallelApplyEntry` already has a `clean`/`dirty` distinction used by `collectClusterFootprintEntriesFromGlobal`. Caching snapshot results as clean entries in `mThreadEntryMap` would be consistent with existing patterns. The `mThreadEntryMap` is per-thread, so no thread-safety issues. The `flushRoTTLBumpsInTxWriteFootprint` flow is compatible: it reads the cached clean value then upserts a dirty bumped value, correctly overwriting the cache.

**Severity downgrade rationale:** The per-call overhead is dominated by the deep copy cost. For typical SAC benchmark entries (~200–500 bytes average across CONTRACT_DATA and TTL entries), the total redundant copying is approximately 5–10 MB per ledger at 3200 txs. At modern memcpy throughput (~10–20 GB/s), this amounts to ~0.5–1 ms per ledger. With apply phase times of ~100–500 ms in typical benchmarks, the improvement is <1–2% — below the Low threshold of 5–10%. In production builds without meta streaming, one of the redundant calls is skipped, further reducing impact.

**Implementation note:** `getLiveEntryOpt` is currently `const`. Caching would require either (a) making the cache `mutable`, (b) using a separate mutable cache member, or (c) restructuring to make the function non-const. Option (a) with a `mutable UnorderedMap<LedgerKey, OptionalEntryT>` or reusing `mThreadEntryMap` with clean flag is simplest.

### PoC Guidance

- **Target code**: `src/transactions/ParallelApplyUtils.cpp` — `ThreadParallelApplyLedgerState::getLiveEntryOpt` (lines 699–735)
- **Change description**: After the snapshot/InMemorySorobanState lookup succeeds (or returns null), insert the result as a clean entry into `mThreadEntryMap` (requires making it `mutable` or adding a separate mutable cache). Subsequent calls for the same key will hit the thread-local map instead of repeating the snapshot lookup and deep copy.
- **Correctness check**: Run `"[tx]"` and `"[soroban]"` test tags. The `flushRoTTLBumpsInTxWriteFootprint` and `commitChangeFromSuccessfulTx` paths should continue to work correctly since they upsert dirty values that override cached clean values. Verify that `eraseEntry` properly overrides cached entries.
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` with SAC scenario at T=8 and compare median and p99 ledger close times. Expected improvement is small (<2%) — may need statistical testing across multiple runs to measure. A profiler comparison counting InMemorySorobanState::get calls before/after would more clearly demonstrate the reduction.
