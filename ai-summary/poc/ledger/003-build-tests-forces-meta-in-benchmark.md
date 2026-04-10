# H003: BUILD_TESTS Forces LedgerCloseMeta Collection Even When Benchmark Disables Metadata

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Medium
**Impact**: CPU and memory reduction in apply-load benchmark measurements
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

When the benchmark config sets `METADATA_OUTPUT_STREAM = ""` and `METADATA_DEBUG_LEDGERS = 0`, the apply-load benchmark should not collect per-transaction ledger close metadata. The `ledgerCloseMeta` pointer should be null throughout the apply path, and all conditional meta operations (`pushTxFeeProcessing`, `setTxProcessingMetaAndResultPair`, `getChanges`, `setPostTxApplyFeeProcessing`) should be skipped. The `enableTxMeta` flag should be false, making `TransactionMetaBuilder` operations no-ops.

## Mechanism

The `apply-load` command is gated behind `BUILD_TESTS` (CommandLine.cpp:2069), so the benchmark always runs with a test build. Two BUILD_TESTS overrides force full metadata collection:

**Override 1** (LedgerManagerImpl.cpp:1598-1607): Forces `ledgerCloseMeta` allocation when no meta stream is configured:
```cpp
#ifdef BUILD_TESTS
    if (!ledgerCloseMeta) {
        ledgerCloseMeta = std::make_unique<LedgerCloseMetaFrame>(...);
        ledgerCloseMeta->reserveTxProcessing(applicableTxSet->sizeTxTotal());
        ledgerCloseMeta->populateTxSet(*txSet);
    }
#endif
```

**Override 2** (LedgerManagerImpl.cpp:2646-2650): Forces `enableTxMeta = true` for `TransactionMetaBuilder`:
```cpp
#ifdef BUILD_TESTS
    enableTxMeta = true;
#endif
```

These overrides cause every ledger close in the benchmark to:
1. Allocate `LedgerCloseMetaFrame` and copy the entire TxSet into it via `populateTxSet` (XDR deep copy of all transaction envelopes).
2. For each tx in `processFeesSeqNums`: call `ltxTx.getChanges()` to extract fee processing changes (line 2292). This allocates `LedgerEntryChanges` vectors with XDR `LedgerEntryChange` objects.
3. For each Soroban tx in `processPostTxSetApply`: call `ltxInner.getChanges()` again (line 2854-2855).
4. For each tx: build full `TransactionMeta` in `TransactionMetaBuilder`, finalize it, and store it in the meta frame (line 2590-2596).
5. Populate `mLastLedgerTxMeta` with all tx meta (line 2592, 2601-2603).

With 6400 txs, this is 6400 `getChanges()` calls in fee processing + 6400 in post-processing + 6400 `TransactionMeta` finalization calls + the initial TxSet deep copy.

The fix would be to add a config flag like `APPLY_LOAD_DISABLE_META_FOR_BENCHMARKING` that suppresses the BUILD_TESTS meta overrides in apply-load mode.

## Trigger

Run the apply-load benchmark in any configuration. Profile `getChanges` and `TransactionMetaBuilder::finalize` call frequency and cost. The overhead is present in every apply-load run since apply-load requires BUILD_TESTS.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:1598-1607` — BUILD_TESTS forces meta allocation
- `src/ledger/LedgerManagerImpl.cpp:2646-2650` — BUILD_TESTS forces `enableTxMeta = true`
- `src/ledger/LedgerManagerImpl.cpp:2290-2293` — per-tx `getChanges()` for fee meta
- `src/ledger/LedgerManagerImpl.cpp:2854-2855` — per-tx `getChanges()` for post-apply meta
- `src/ledger/LedgerManagerImpl.cpp:2590-2596` — per-tx `TransactionMeta` finalization
- `src/ledger/LedgerManagerImpl.cpp:2592,2601-2603` — storing meta in `mLastLedgerTxMeta`
- `src/ledger/LedgerCloseMetaFrame.cpp:151-167` — `populateTxSet` deep-copies entire TxSet
- `src/main/CommandLine.cpp:2069` — apply-load behind BUILD_TESTS

## Evidence

1. `apply-load` is behind `#ifdef BUILD_TESTS` (CommandLine.cpp:2069), so it always builds with test code active.
2. Lines 1598-1607 unconditionally create meta when no stream is configured in test builds.
3. Lines 2646-2650 explicitly set `enableTxMeta = true` in test builds, overriding the optimization.
4. The benchmark config `METADATA_OUTPUT_STREAM = ""` intends to disable metadata output, and the comment at line 2644 says "There is no need to populate the transaction meta if we are not going to output it" — but the BUILD_TESTS override negates this intent.
5. `populateTxSet` (LedgerCloseMetaFrame.cpp:151-167) calls `txSet.toXDR(...)` which deep-copies the entire transaction set — with 6400 txs, this is a significant allocation.
6. `LedgerEntryChanges` from `getChanges()` involves allocating XDR vectors with before/after entry states, which has measurable per-tx overhead.

## Anti-Evidence

1. The meta collection overhead may be small relative to Soroban VM execution time. Need profiling to quantify.
2. `TransactionMetaBuilder` with `enableTxMeta = true` may still be lightweight if operations are mostly move-semantics based.
3. The benchmark is designed for relative comparisons (before/after a change). Since meta overhead is constant across runs, it doesn't affect the validity of A/B comparisons. However, it does affect absolute throughput numbers and the ceiling for optimizations.
4. Removing meta collection would break test infrastructure that relies on `mLastLedgerTxMeta` being populated. A conditional override keyed to apply-load mode would be cleaner.
5. The `DISABLE_SOROBAN_METRICS_FOR_TESTING = true` config already disables Soroban-specific metrics, suggesting the team is aware of measurement overhead. Metadata may be next.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full apply-load benchmark path from `CommandLine.cpp:2069` through `LedgerManagerImpl::applyLedger()`. Confirmed both BUILD_TESTS overrides exist exactly as described: line 1598-1607 forces `ledgerCloseMeta` allocation with `populateTxSet` deep copy, and line 2646-2650 forces `enableTxMeta = true`. This causes per-tx `getChanges()` calls in fee processing (line 2292), per-tx `finalize()` in `processResultAndMeta` (lines 2590-2602), per-operation `setLedgerChangesFromSuccessfulOp` on parallel Soroban threads (TransactionMeta.cpp:385-452), and per-Soroban-tx `getChanges()` in `processPostTxSetApply` (line 2854-2855). All benchmark configs explicitly set `METADATA_OUTPUT_STREAM = ""` and `METADATA_DEBUG_LEDGERS = 0`, confirming intent to disable meta.

### Code Paths Examined

- `src/main/CommandLine.cpp:2061-2073` — `apply-load` command is within `#ifdef BUILD_TESTS` block, confirmed
- `src/ledger/LedgerManagerImpl.cpp:1580-1607` — `ledgerCloseMeta` creation: normally only when `mMetaStream || mMetaDebugStream` (line 1581), but BUILD_TESTS override at 1598-1607 forces creation unconditionally
- `src/ledger/LedgerManagerImpl.cpp:2645-2650` — `enableTxMeta` set to `ledgerCloseMeta != nullptr` (line 2645), then unconditionally overridden to `true` by BUILD_TESTS (line 2649)
- `src/ledger/LedgerManagerImpl.cpp:2288-2293` — Per-tx `getChanges()` in `processFeesSeqNums`: guarded by `if (ledgerCloseMeta)`, which is always true due to override 1
- `src/ledger/LedgerManagerImpl.cpp:2555-2604` — `processResultAndMeta`: when `ledgerCloseMeta` is non-null, calls `txMetaBuilder.finalize()` (line 2590) and stores in `mLastLedgerTxMeta` (line 2592); even in the else branch, BUILD_TESTS forces `finalize()` + storage (lines 2600-2603)
- `src/transactions/TransactionMeta.cpp:385-452` — `OperationMetaBuilder::setLedgerChangesFromSuccessfulOp`: when `mEnabled=true`, iterates all modified entries, looks up previous state from `threadState`, constructs full `LedgerEntryChanges` with XDR deep copies. This runs on parallel Soroban worker threads.
- `src/transactions/TransactionMeta.cpp:1112-1120` — `maybePushChanges`: when `mEnabled=true`, calls `changesLtx.getChanges()` and appends to dest
- `src/ledger/LedgerTxn.cpp:1355-1400` — `getChanges()`: iterates all entries in the LedgerTxn, for each non-INIT entry calls `mParent.getNewestVersion(key)` to get previous state, creates XDR `LedgerEntryChange` objects with deep copies of before/after entries
- `src/ledger/LedgerCloseMetaFrame.cpp:151-167` — `populateTxSet`: calls `txSet.toXDR()` which deep-copies all transaction envelopes
- `docs/apply-load-benchmark-sac.cfg:18-22` — Benchmark config explicitly disables both meta output and debug meta

### Findings

**Both BUILD_TESTS overrides are confirmed and operate exactly as hypothesized.**

The overhead has three components:

1. **Per-ledger overhead**: `populateTxSet` deep-copies the entire TxSet (all 3000 transaction envelopes in the SAC benchmark config). This is a one-time XDR serialization of potentially megabytes of data.

2. **Per-tx overhead on the main apply thread**: 
   - `processFeesSeqNums` calls `getChanges()` per tx (line 2292) — ~2-4 XDR entry copies per tx for fee/seqnum changes
   - `processPostTxSetApply` calls `getChanges()` per Soroban tx (line 2854-2855) — additional entry copies
   - `processResultAndMeta` calls `finalize()` per tx (line 2590) — collects all operation metas, events, etc.
   - `mLastLedgerTxMeta.emplace_back()` stores a copy of the full `TransactionMeta` per tx (line 2592)

3. **Per-operation overhead on parallel Soroban threads**: `setLedgerChangesFromSuccessfulOp` (TransactionMeta.cpp:385-452) iterates ALL modified entries per operation, looks up previous state from the thread-local snapshot, and constructs `LedgerEntryChanges` with deep XDR copies. For a SAC transfer batch with 100 transfers, each tx modifies ~200+ entries (sender/receiver balances + TTLs). This runs on the parallel worker threads, adding to per-thread CPU time and memory pressure.

**Estimated overhead with 3000 txs**: 
- 3000 × `getChanges()` in fee processing ≈ 6000-12000 XDR entry copies
- 3000 × `setLedgerChangesFromSuccessfulOp` with ~200 modified entries each ≈ 600K-1.2M XDR entry copies (on parallel threads)
- 3000 × `getChanges()` in post-apply ≈ varies
- 3000 × `finalize()` + `emplace_back()` for TransactionMeta storage
- 1 × `populateTxSet` deep copy of 3000 envelopes

The parallel-thread overhead is the most significant component because it increases per-thread work, reduces effective parallelism, and increases memory pressure. The config precedent of `DISABLE_SOROBAN_METRICS_FOR_TESTING = true` confirms the team actively addresses benchmark measurement overhead.

### PoC Guidance

- **Target code**: `src/ledger/LedgerManagerImpl.cpp` — modify the two BUILD_TESTS blocks at lines 1598-1607 and 2646-2650 to check an additional condition (e.g., a config flag like `DISABLE_TX_META_FOR_TESTING` or check `mApp.getConfig().APPLY_LOAD_MODE != ApplyLoadMode::NONE`). Also guard the `mLastLedgerTxMeta` storage at lines 2592 and 2600-2603.
- **Change description**: Add a config flag (or reuse the apply-load mode check) that suppresses the BUILD_TESTS meta overrides when running in benchmark mode. When the flag is set: (1) don't force `ledgerCloseMeta` allocation at line 1598-1607, (2) don't override `enableTxMeta` at line 2646-2650, (3) skip `mLastLedgerTxMeta` storage at lines 2592 and 2600-2603. The simplest approach is to add `DISABLE_TX_META_FOR_TESTING = true` to the benchmark configs and check it alongside the BUILD_TESTS guards.
- **Correctness check**: All existing tests should continue to pass since the flag defaults to `false`. The apply-load benchmark tests should also pass since they don't inspect `mLastLedgerTxMeta`. Run `make check` to verify. Specifically verify that test cases that use `getLastLedgerTxMeta()` still work (they won't set the new flag).
- **Benchmark focus**: Run the SAC benchmark (`apply-load` with `apply-load-benchmark-sac.cfg`) before and after the change. The primary metric is median ledger close time. Expect 5-10% improvement with 3000 txs at T=1 (single cluster). The improvement may be more visible at T=1 than T=8 since meta collection is serial overhead that doesn't parallelize. Also measure memory high-water mark — expect reduction from eliminated XDR allocations.

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-10
**PoC by**: claude-opus-4-6, high

### Changes Made

1. **`src/main/Config.h`** (line ~551-555): Added `bool DISABLE_TX_META_FOR_TESTING` config flag declaration, with documentation comment explaining its purpose.

2. **`src/main/Config.cpp`** (line ~175): Initialized `DISABLE_TX_META_FOR_TESTING = false` in the Config constructor defaults, immediately after `DISABLE_SOROBAN_METRICS_FOR_TESTING`.

3. **`src/main/Config.cpp`** (line ~1183-1186): Added config file parser entry for `DISABLE_TX_META_FOR_TESTING`, following the same pattern as `DISABLE_SOROBAN_METRICS_FOR_TESTING`.

4. **`src/ledger/LedgerManagerImpl.cpp`** (line ~1598-1608): Modified the first BUILD_TESTS override to check `!mApp.getConfig().DISABLE_TX_META_FOR_TESTING` before forcing `ledgerCloseMeta` allocation. When the flag is set, the `ledgerCloseMeta` pointer remains null (as intended by `METADATA_OUTPUT_STREAM = ""`), skipping `populateTxSet` deep copy and all downstream per-tx `getChanges()` calls guarded by `if (ledgerCloseMeta)`.

5. **`src/ledger/LedgerManagerImpl.cpp`** (line ~2646-2653): Modified the second BUILD_TESTS override to check `!mApp.getConfig().DISABLE_TX_META_FOR_TESTING` before forcing `enableTxMeta = true`. When the flag is set, `enableTxMeta` remains false (since `ledgerCloseMeta` is null), making all `TransactionMetaBuilder` operations no-ops including `setLedgerChangesFromSuccessfulOp` on parallel worker threads.

6. **`src/ledger/LedgerManagerImpl.cpp`** (line ~2592, 2601-2603): Guarded both `mLastLedgerTxMeta.emplace_back()` calls with `!mApp.getConfig().DISABLE_TX_META_FOR_TESTING` to avoid storing per-tx `TransactionMeta` copies when benchmarking.

7. **`docs/apply-load-benchmark-sac.cfg`** (line ~19-20): Added `DISABLE_TX_META_FOR_TESTING = true` to the SAC benchmark config.

8. **`docs/apply-load-benchmark-token.cfg`** (line ~19-20): Added `DISABLE_TX_META_FOR_TESTING = true` to the custom token benchmark config.

### Demonstration

When `DISABLE_TX_META_FOR_TESTING = true` is set in benchmark configs, the three BUILD_TESTS metadata overrides are suppressed. This eliminates: (1) the per-ledger `populateTxSet` deep copy of all transaction envelopes, (2) per-tx `getChanges()` XDR deep copies in fee processing and post-apply paths, (3) per-operation `setLedgerChangesFromSuccessfulOp` XDR deep copies on parallel Soroban worker threads, and (4) per-tx `TransactionMeta` finalization and storage. For a 3000-tx SAC benchmark, this avoids hundreds of thousands of XDR entry copies and significant memory allocations, reducing both CPU time and memory pressure during the measured benchmark path.

### Test Results

All tests passed: `make check` with `NUM_PARTITIONS=$(nproc)` completed successfully — "All 2 tests passed" (the two test partitions: `selftest-nopg` and `check-nondet`, which together cover the full C++ and Rust test suites). The flag defaults to `false`, so all existing tests that rely on `getLastClosedLedgerTxMeta()` (EventTests, InvokeHostFunctionTests, TxTests) continue to work unchanged.
