# H001: Disable BUILD_TESTS Tx-Meta Capture on the Apply-Load Path

**Date**: 2026-04-10
**Subsystem**: transaction-ledger
**Severity**: High
**Impact**: Benchmark-only instrumentation overhead in ledger apply
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When `ledgerCloseMeta` is not being emitted and no test harness has explicitly
requested last-ledger tx metadata, apply-load should keep transaction metadata
disabled even in `BUILD_TESTS` builds. The benchmark should not pay per-tx
`TransactionMetaBuilder` construction, ledger-change materialization, finalize,
and deep-copy costs for `mLastLedgerTxMeta` that nobody reads.

## Mechanism

`LedgerManagerImpl::applyTransactions` forces `enableTxMeta = true` under
`BUILD_TESTS` even when `ledgerCloseMeta == nullptr`. That eagerly constructs a
full `TransactionMetaBuilder` inside every `TxBundle`, drives
`setLedgerChangesFromSuccessfulOp` during parallel apply, and finally deep-copies
every finalized meta object into `mLastLedgerTxMeta`. The apply-load benchmark
disables metadata output and `src/simulation` does not consume
`getLastClosedLedgerTxMeta`, so this is likely pure benchmark overhead on the
Soroban apply path.

## Trigger

Build a `BUILD_TESTS` binary and run `scripts/run_apply_load_matrix.py`.
Compare the current path against a build that only enables tx meta capture when
`ledgerCloseMeta` is present or an explicit test/debug knob requests
`mLastLedgerTxMeta`. The signal should be strongest on `sac,TX=3200,T=8` and
`custom_token,TX=1600,T=8`, where thousands of successful Soroban txs each pay
the metadata path.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2555-2604` — finalizes each tx meta and copies it into `mLastLedgerTxMeta`
- `src/ledger/LedgerManagerImpl.cpp:2641-2650` — `BUILD_TESTS` unconditionally forces `enableTxMeta = true`
- `src/transactions/ParallelApplyStage.h:22-25,64-70` — every `TxBundle` eagerly allocates `TxEffects(TransactionMetaBuilder, LedgerTxnDelta)`
- `src/transactions/TransactionMeta.cpp:924-974` — `TransactionMetaBuilder` allocates per-op meta vectors/builders up front

## Evidence

- `applyTransactions` explicitly overrides the runtime decision and turns tx meta
  on for all `BUILD_TESTS` runs, even when no metadata stream is being emitted.
- `processResultAndMeta` finalizes every meta object and stores it in
  `mLastLedgerTxMeta`; `applyLedger` also clears that vector every ledger under
  `BUILD_TESTS` (`src/ledger/LedgerManagerImpl.cpp:1465-1468`), confirming the
  benchmark path repeatedly repopulates it.
- `getLastClosedLedgerTxMeta` exists only behind `BUILD_TESTS`
  (`src/ledger/LedgerManagerImpl.cpp:866-870`), and code search shows its
  consumers live in transaction test helpers/tests rather than `src/simulation`.

## Anti-Evidence

- Some `BUILD_TESTS` executions genuinely need `mLastLedgerTxMeta`, so the fix
  must be an explicit opt-in/opt-out gate, not a blanket removal.
- If `ledgerCloseMeta` is being emitted, the real metadata path is required and
  only the extra test-copying work can be removed.
- Part of the benchmark gain may overlap with other meta-path cleanups, so the
  incremental win needs measurement.

---

## Review

**Verdict**: VIABLE
**Severity**: Medium
**Date**: 2026-04-10
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Under `BUILD_TESTS`, four separate code paths force full metadata capture even when no stream is emitting and no test consumer reads it: (1) `LedgerCloseMetaFrame` is force-created at `LedgerManagerImpl.cpp:1598-1607` when `!mMetaStream && !mMetaDebugStream`, (2) `enableTxMeta` is forced `true` at line 2649, (3) per-tx `TransactionMetaBuilder` construction/finalization and deep-copy into `mLastLedgerTxMeta` at lines 2591-2603, and (4) the entire `LedgerCloseMetaFrame` is deep-copied into `mLastLedgerCloseMeta` at line 1761. The apply-load benchmark disables metadata output (`METADATA_OUTPUT_STREAM=""`), and `src/simulation` never calls `getLastClosedLedgerTxMeta()` — all consumers are in `src/transactions/test/` and `src/test/TxTests.cpp`.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:1598-1607` — Under BUILD_TESTS, `ledgerCloseMeta` is always force-constructed with `reserveTxProcessing(sizeTxTotal())` + `populateTxSet(*txSet)`, even when no meta stream exists. This means `enableTxMeta = ledgerCloseMeta != nullptr` at line 2645 would already be `true` even without the explicit override at line 2649.
- `src/ledger/LedgerManagerImpl.cpp:2645-2650` — `enableTxMeta` override is actually redundant given the force-creation above, but both paths exist.
- `src/ledger/LedgerManagerImpl.cpp:2735-2738` — Each `TxBundle` in the parallel path allocates `TxEffects(enableTxMeta=true, ...)`, triggering `TransactionMetaBuilder` construction.
- `src/transactions/TransactionMeta.cpp:924-974` — `TransactionMetaBuilder` constructor allocates per-op meta vectors (`OperationMeta` or `OperationMetaV2`), creates `OperationMetaBuilder` objects with enabled `OpEventManager` (enabled for Soroban since `isSoroban=true`), `TxEventManager`, and `DiagnosticEventManager` (disabled due to `ENABLE_SOROBAN_DIAGNOSTIC_EVENTS=false` in benchmark config).
- `src/transactions/TransactionMeta.cpp:384-452` — `setLedgerChangesFromSuccessfulOp` runs on worker threads for each successful Soroban tx. When `mEnabled=true`, iterates all modified entries, reads previous state from `threadState.getLiveEntryOpt(lk)`, constructs full `LedgerEntryChanges` records (CREATED/UPDATED/REMOVED), and calls `processOpLedgerEntryChanges`. With `mEnabled=false`, returns immediately.
- `src/ledger/LedgerManagerImpl.cpp:2555-2604` — `processResultAndMeta` calls `txMetaBuilder.finalize(...)` then deep-copies the `TransactionMeta` XDR into `mLastLedgerTxMeta` and moves it into `ledgerCloseMeta`. For 3200 SAC txs, this is 3200 finalize+copy operations running sequentially on the main thread.
- `src/ledger/LedgerManagerImpl.cpp:1755-1762` — The *entire* `LedgerCloseMetaFrame` (containing all 3200 tx metas, the tx set, etc.) is deep-copied into `mLastLedgerCloseMeta`. This single copy is potentially the largest single cost — multiple MB of XDR.
- `src/transactions/EventManager.cpp:236-246` — `OpEventManager` for Soroban txs is enabled when `metaEnabled=true` (regardless of `EMIT_CLASSIC_EVENTS`), so contract events are buffered even though nobody reads them.
- `src/ledger/LedgerManagerImpl.cpp:866-870` — `getLastClosedLedgerTxMeta()` is BUILD_TESTS-only; consumers confirmed exclusively in test files.

### Findings

The inefficiency is real and multi-layered:

1. **Per-tx overhead on worker threads** (`setLedgerChangesFromSuccessfulOp`): For each successful Soroban tx, iterates all modified entries (~3-5 for SAC), reads previous state from thread entry map, and constructs full `LedgerEntryChanges`. This is pure waste when meta is not consumed. Runs in parallel but adds per-tx overhead to the critical path.

2. **Per-tx sequential overhead** (`processResultAndMeta`): For each of 3200 txs, finalize the `TransactionMetaBuilder` (assembles events, operation metas), then deep-copy the `TransactionMeta` XDR into `mLastLedgerTxMeta`. This runs sequentially on the main thread.

3. **Final mega-copy** (line 1761): The entire `LedgerCloseMetaFrame` — containing all 3200 tx metas plus the tx set — is deep-copied into `mLastLedgerCloseMeta`. For 3200 SAC txs with ~2-5 KB meta each, this is a ~6-16 MB deep copy of a single XDR structure.

4. **Force-created `ledgerCloseMeta`** (line 1598-1607): Even under `METADATA_OUTPUT_STREAM=""`, the `LedgerCloseMetaFrame` is constructed, populated, and maintained throughout apply. This adds overhead beyond just the tx meta.

Severity downgraded from High to **Medium**: While the overhead is undeniably real and the fix is straightforward, the Soroban host function execution (even for simple SAC transfers) likely dominates per-tx cost. The meta overhead is estimated at 10-20% of total close time for the SAC T=8 scenario, with the final mega-copy and sequential finalization being the largest contributors. The actual impact needs benchmarking to confirm.

Note: The hypothesis mechanism description is slightly imprecise — it says `enableTxMeta = true` is forced "even when `ledgerCloseMeta == nullptr`", but under BUILD_TESTS, `ledgerCloseMeta` is *never* nullptr due to the force-creation at line 1598-1607. The `enableTxMeta` override at line 2649 is actually redundant. The real root cause is the force-creation of `ledgerCloseMeta` at line 1598-1607.

### PoC Guidance

- **Target code**: 
  - `src/ledger/LedgerManagerImpl.cpp:1598-1607` — Guard the force-creation of `ledgerCloseMeta` behind a new config flag or test-only knob (e.g., `ENABLE_TEST_TX_META_CAPTURE` defaulting to `true`, set to `false` in benchmark configs)
  - `src/ledger/LedgerManagerImpl.cpp:2646-2650` — Remove or guard the `enableTxMeta = true` override (redundant if ledgerCloseMeta creation is guarded)
  - `src/ledger/LedgerManagerImpl.cpp:2591-2603` — The `mLastLedgerTxMeta` copies become dead code when the above is disabled
  - `src/ledger/LedgerManagerImpl.cpp:1755-1762` — The `mLastLedgerCloseMeta` deep copy likewise
  - `docs/apply-load-benchmark-sac.cfg` and `docs/apply-load-benchmark-token.cfg` — Add `ENABLE_TEST_TX_META_CAPTURE=false` (or whatever the knob is named)
- **Change description**: Add a `Config` boolean (e.g., `DISABLE_TX_META_FOR_TESTING` defaulting to `false`) that, when set to `true`, skips the BUILD_TESTS force-creation of `ledgerCloseMeta` and the `enableTxMeta` override. This preserves all existing test functionality (tests don't set the flag) while eliminating the overhead in benchmarks.
- **Correctness check**: All existing tests that use `getLastClosedLedgerTxMeta()` must still pass without the flag set. The flag should only be set in benchmark configs. Run the full test suite without the flag, and the apply-load benchmark with the flag.
- **Benchmark focus**: Compare `sac,TX=3200,T=8` and `custom_token,TX=1600,T=8` median close times with and without the flag. Expect 10-20% improvement on SAC (where host execution is lightweight relative to meta overhead), and 5-15% on custom_token (where host execution is heavier).
