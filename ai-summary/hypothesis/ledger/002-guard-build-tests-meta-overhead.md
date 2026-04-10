# H002: Guard BUILD_TESTS Meta Creation With Runtime Configuration Check

**Date**: 2025-07-14
**Subsystem**: ledger
**Severity**: Medium
**Impact**: CPU + Memory — eliminates ~30ms of meta tracking overhead per ledger in benchmark builds
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When the benchmark configuration sets `METADATA_OUTPUT_STREAM = ""` (no meta
output), the apply path should NOT create `LedgerCloseMetaFrame`, should NOT
force `enableTxMeta = true`, and should NOT collect per-transaction meta
changes. The overhead of meta tracking should scale with whether meta will
actually be consumed, not with whether the binary was compiled with
`BUILD_TESTS`.

## Mechanism

In `LedgerManagerImpl::applyLedger()` (line 1598–1606), a `#ifdef BUILD_TESTS`
block unconditionally creates `ledgerCloseMeta` even when no meta stream is
configured. This triggers a cascade of overhead:

1. **`populateTxSet(*txSet)`** (line 1605): Deep-copies the entire
   `GeneralizedTransactionSet` XDR structure. For 3200 Soroban transactions,
   this is ~0.6–1.6MB of data copied. Cost: ~1ms.

2. **`enableTxMeta = true`** (line 2649): Forces all per-transaction meta
   tracking. In `processFeesSeqNums`, each of ~3200 transactions calls
   `ltxTx.getChanges()` (line 2292) which seals the LedgerTxn, looks up
   previous entry versions via `getNewestVersion()`, and builds a
   `LedgerEntryChanges` vector. Cost: ~3200 × ~1µs = ~3ms.

3. **`setEffectsDeltaFromSuccessfulTx`** (TransactionFrame.cpp:2243): For each
   successful Soroban tx, allocates `make_shared<InternalLedgerEntry>` for both
   previous and current versions of every modified entry. For ~5–10 entries per
   tx × 3200 txs = ~32K shared_ptr allocations. Cost: ~6–10ms.

4. **`processPostTxSetApply`** (line 2855): Calls `ltxInner.getChanges()` for
   each Soroban tx's refund processing. Cost: ~3200 × ~0.5µs = ~1.5ms.

5. **`mLastLedgerTxMeta` storage** (line 1875): Stores the complete meta frame
   containing all per-tx changes. Memory: ~5–20MB per ledger, immediately
   discarded.

Total estimated overhead: ~15–30ms per ledger, which is ~10–20% of a typical
100–150ms SAC ledger close.

The apply-load benchmark (`run_apply_load_matrix.py`) builds with
`BUILD_TESTS` enabled and sets `METADATA_OUTPUT_STREAM = ""`. It measures
ledger close time including this artificial overhead, producing results that
are ~10–20% slower than production builds.

## Trigger

Run the SAC apply-load benchmark at T=1 or T=8 with 3200 transactions per
ledger. Compare the result against a non-BUILD_TESTS build (production) or
against a patched BUILD_TESTS build that respects `METADATA_OUTPUT_STREAM`.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:1598-1606` — `#ifdef BUILD_TESTS` unconditional meta creation
- `src/ledger/LedgerManagerImpl.cpp:2646-2650` — `#ifdef BUILD_TESTS` forces `enableTxMeta = true`
- `src/ledger/LedgerManagerImpl.cpp:1870-1876` — `#ifdef BUILD_TESTS` stores meta in `mLastLedgerTxMeta`
- `src/ledger/LedgerManagerImpl.cpp:2290-2293` — `pushTxFeeProcessing(ltxTx.getChanges())` guarded by `ledgerCloseMeta`
- `src/transactions/TransactionFrame.cpp:2243-2244` — `setEffectsDeltaFromSuccessfulTx` builds delta with shared_ptr allocs

## Evidence

1. **Explicit bypass of meta check**: Line 1598–1606 creates `ledgerCloseMeta`
   even when `mMetaStream` is null. The non-BUILD_TESTS path (line 1585–1596)
   correctly checks `mMetaStream || mMetaDebugStream` before creating meta.

2. **`enableTxMeta` forced true**: Line 2646–2650 sets `enableTxMeta = true`
   unconditionally in BUILD_TESTS. The non-BUILD_TESTS path (line 2645)
   correctly derives it from `ledgerCloseMeta != nullptr`.

3. **Benchmark config disables meta**: `docs/apply-load-benchmark-sac.cfg`
   sets `METADATA_OUTPUT_STREAM = ""`, indicating the benchmark intends to
   skip meta output. But BUILD_TESTS overrides this intent.

4. **Observable memory waste**: `mLastLedgerTxMeta` (line 1875) accumulates
   all per-tx meta for the entire ledger, then it's only consumed by tests
   that explicitly inspect meta (not by the benchmark).

## Anti-Evidence

1. **Test correctness dependency**: Some tests (not benchmarks) rely on
   `mLastLedgerTxMeta` being populated to inspect transaction effects. The fix
   must preserve meta tracking when tests actually need it. A simple approach:
   add a config flag like `DISABLE_TX_META_FOR_TESTING` or check
   `METADATA_OUTPUT_STREAM` in the BUILD_TESTS block.

2. **Not a production code change**: This optimization affects the benchmark
   measurement tool, not the production code path. In production with meta
   enabled, the overhead is wanted. In production without meta, the overhead
   doesn't exist. The value is making the benchmark more accurately reflect
   production performance.

3. **Partial overlap with config flag**: `DISABLE_SOROBAN_METRICS_FOR_TESTING`
   already exists as a pattern for disabling test-only overhead in benchmarks.
   The same pattern could be applied to meta tracking, but it adds another
   configuration knob that must be documented and maintained.
