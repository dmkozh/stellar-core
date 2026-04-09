# H002: Sequential processPostTxSetApply Is a Serial Bottleneck After Parallel Apply

**Date**: 2026-04-09
**Subsystem**: ledger
**Severity**: Medium
**Impact**: Parallelization improvement for T=8 Soroban scenarios
**Hypothesis by**: claude-opus-4-6, high

## Expected Behavior

After parallel Soroban transaction execution completes, the post-processing step (fee refunds, meta collection, result recording) should either be parallelized or have minimal per-transaction cost. The sequential post-processing should not negate a significant fraction of the benefits of parallel execution.

## Mechanism

`processPostTxSetApply()` (LedgerManagerImpl.cpp:2827-2874) runs **sequentially** on the primary apply thread after all parallel Soroban stages complete. For each Soroban transaction, it:

1. Opens a new `LedgerTxn` (line 2844) — copies header, sets up entry tracking.
2. Calls `processPostTxSetApply` on the transaction (line 2845) — which calls `processRefund` to refund unused Soroban fees. This loads and modifies the fee source account.
3. Calls `ledgerCloseMeta->setPostTxApplyFeeProcessing(ltxInner.getChanges(), ...)` (line 2854) — extracts `LedgerEntryChanges` from the LedgerTxn (XDR vector copy).
4. Commits the LedgerTxn (line 2857) — merges entries back to parent.
5. Calls `processResultAndMeta` (line 2862) — records the transaction result and meta into `txResultSet` and `ledgerCloseMeta`.

With 3000-6400 Soroban transactions per ledger, this loop iterates thousands of times sequentially. Each iteration creates and destroys a `LedgerTxn`, which involves: header copy, entry map allocation, parent deactivation, and on commit: entry merge back to parent. The `refundSorobanFee` (TransactionFrame.cpp:2604) loads the fee source account via `stellar::loadAccount`, modifies the balance, and the LedgerTxn bookkeeping wraps this single mutation.

This sequential processing directly limits the speedup of T=8 parallel execution — the parallelized Soroban execution takes time ~T/8, but the sequential post-processing still takes time proportional to the total number of transactions, creating an Amdahl's law bottleneck.

## Trigger

Run any T=8 apply-load benchmark scenario with high transaction counts (sac TX=6400 or custom_token TX=3000). Use Tracy profiling to measure the wall time spent in `processPostTxSetApply` relative to total apply time. The bottleneck should be visible as a single-threaded region after the parallel execution zone.

## Target Code

- `src/ledger/LedgerManagerImpl.cpp:2827-2874` — `processPostTxSetApply` iterates all txs sequentially
- `src/ledger/LedgerManagerImpl.cpp:2844` — per-tx `LedgerTxn` creation
- `src/ledger/LedgerManagerImpl.cpp:2845-2850` — `processPostTxSetApply` on each tx
- `src/ledger/LedgerManagerImpl.cpp:2854-2855` — per-tx `getChanges()` for meta
- `src/ledger/LedgerManagerImpl.cpp:2857` — per-tx commit
- `src/transactions/TransactionFrame.cpp:2581-2587` — `processPostTxSetApply` calls `processRefund`
- `src/transactions/TransactionFrame.cpp:2592-2615` — `processRefund` loads account, modifies balance

## Evidence

1. The loop at line 2839-2867 iterates over ALL stages and ALL txBundles sequentially — no parallelism.
2. Each iteration opens a `LedgerTxn` (line 2844), performs a refund, extracts changes, and commits.
3. With 6400 transactions (sac T=8 scenario), this is 6400 sequential LedgerTxn create-modify-commit cycles.
4. The refund involves `loadAccount` (a LedgerTxn entry lookup + deref) and a balance mutation.
5. The `getChanges()` call extracts a vector of `LedgerEntryChange` objects involving XDR copies.
6. The comment at lines 2869-2872 acknowledges this is only for the parallel path, suggesting it was added as a serialized post-processing step without considering batching.
7. Per Amdahl's law, if parallel execution takes 50ms at T=8 and post-processing takes 20ms, the maximum speedup is limited to 50ms/(50/8 + 20)ms = ~1.9x instead of the ideal ~8x.

## Anti-Evidence

1. `processRefund` is O(1) per tx — a single account balance credit. The LedgerTxn overhead per iteration may be only ~1-5μs, making the total for 6400 txs ~6-32ms.
2. If the parallel Soroban VM execution dominates total time (e.g., >200ms for T=8), then 20-30ms of post-processing is <15% overhead and may not meet the Medium severity threshold.
3. Parallelizing refunds is non-trivial: multiple transactions may share a fee source account, creating write conflicts. The current sequential approach avoids this by design.
4. The meta collection (`getChanges`, `setPostTxApplyFeeProcessing`) is inherently order-dependent since meta is stored in transaction-index order in the `LedgerCloseMeta`.
5. If metadata output is disabled (as in benchmark config), the `getChanges` cost is lower — but BUILD_TESTS forces meta collection anyway.

---

## Review

**Verdict**: VIABLE
**Severity**: Low
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the complete `processPostTxSetApply` loop (LedgerManagerImpl.cpp:2827-2874) and confirmed it runs sequentially for every Soroban tx from the parallel phase. Each iteration creates TWO nested `LedgerTxn` objects: one outer (`ltxInner` at line 2844) and one inner within `refundSorobanFee` (TransactionFrame.cpp:1061). Each LedgerTxn construction copies the `LedgerHeader` via `make_unique<LedgerHeader>(mParent.getHeader())` (LedgerTxn.cpp:431), a ~300-500 byte struct with heap allocation. For 6400 transactions, this is 12,800 LedgerTxn create/destroy cycles with header copies, heap allocations, and entry map operations. The refund itself is trivial (single account balance credit), but the LedgerTxn bookkeeping overhead dominates per-iteration cost.

### Code Paths Examined

- `src/ledger/LedgerManagerImpl.cpp:2827-2874` — Confirmed sequential loop over all stages and txBundles; no parallelism or batching
- `src/ledger/LedgerManagerImpl.cpp:2844` — Per-tx `LedgerTxn ltxInner(ltx)` creation
- `src/transactions/TransactionFrame.cpp:1045-1083` — `refundSorobanFee` creates ANOTHER nested `LedgerTxn ltx(ltxOuter)` at line 1061, loads account, modifies balance, commits — adding a second level of LedgerTxn overhead per iteration
- `src/ledger/LedgerTxn.cpp:427-438` — LedgerTxn constructor: `make_unique<LedgerHeader>(mParent.getHeader())` heap allocation + copy, plus `mParent.addChild()` call
- `src/ledger/LedgerTxn.cpp:587-610` — `commitChild`: copies child header again, iterates all entries (1-2 per refund), calls `updateEntry` per entry
- `src/ledger/LedgerTxn.cpp:1355-1400` — `getChanges()`: iterates entries, calls `getNewestVersion` (O(1) parent hash map lookup), copies LedgerEntry for state+updated pairs; only called when `ledgerCloseMeta` is non-null
- `src/ledger/LedgerManagerImpl.cpp:2556-2597` — `processResultAndMeta`: copies tx hash and result XDR, calls `txMetaBuilder.finalize()` only when meta enabled, emplaces into `txResultSet.results`
- `src/transactions/FeeBumpTransactionFrame.cpp:220-227` — Confirmed fee-bump path delegates to inner tx's `processRefund` with the fee-bump source account

### Findings

1. **Double LedgerTxn nesting confirmed**: Each tx incurs TWO LedgerTxn create/commit/destroy cycles — one in `processPostTxSetApply` and one inside `refundSorobanFee`. This doubles the overhead vs. what a naive reading suggests.

2. **Per-iteration cost estimate**: ~3-5μs without meta (header copy × 2, heap alloc/dealloc × 2, hash map lookups, entry merge). For 6400 txs: ~20-32ms total.

3. **Benchmark meta disabled mitigates one cost**: When `ledgerCloseMeta` is null (benchmark config), `getChanges()` and `setPostTxApplyFeeProcessing` are skipped. However, in `BUILD_TESTS` builds, `enableTxMeta` is forced true, so `txMetaBuilder.finalize()` still runs. The `processResultAndMeta` cost without meta streaming is low (~hash copy + XDR result copy + vector emplace).

4. **Amdahl's law impact is real but bounded**: With T=8 parallel execution taking ~100-200ms for 6400 SAC txs, 25ms of sequential post-processing represents ~12-25% of parallel execution time. However, this is ONE of several sequential bottlenecks (processFeesSeqNums, preParallelApply, sealing are also sequential), so eliminating this alone won't achieve the full theoretical T=8 speedup.

5. **Batching is the viable optimization**: True parallelization is impractical (shared fee source accounts, order-dependent meta). Instead, batching all refunds into a single LedgerTxn would eliminate ~12,800 LedgerTxn create/destroy cycles. The refunds can be applied sequentially within one LedgerTxn, and per-tx change records can be computed manually (before/after balance snapshots) rather than via `getChanges()`.

6. **Severity downgrade from Medium to Low**: The ~20-30ms savings from batching represents ~5-10% of total apply time (which includes multiple sequential phases beyond this one). This is meaningful but below the 10-20% threshold for Medium.

### PoC Guidance

- **Target code**: `src/ledger/LedgerManagerImpl.cpp:2827-2874` (the `processPostTxSetApply` method) and `src/transactions/TransactionFrame.cpp:1045-1083` (`refundSorobanFee`)
- **Change description**: Refactor the loop to open a single `LedgerTxn` for the entire batch of refunds rather than one per transaction. Within the single LedgerTxn, for each tx: (1) load the fee source account (reusing if already loaded), (2) snapshot the balance before, (3) apply `addBalance` for the refund, (4) build `LedgerEntryChanges` manually from the before/after state, (5) call `setPostTxApplyFeeProcessing` with the manual changes. Commit once at the end. An alternative simpler approach: eliminate the inner LedgerTxn inside `refundSorobanFee` by inlining the refund logic directly into the outer loop's LedgerTxn, halving the LedgerTxn overhead.
- **Correctness check**: Existing parallel Soroban tests (tag `[soroban]`, especially those testing fee refunds and fee-bump transactions with Soroban) should cover this path. Run `"[tx][soroban]"` and `"[txsetapply]"` test tags.
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` with `sac,TX=6400,T=8`. Measure the wall time of the `processPostTxSetApply` Tracy zone before and after the change. Expect ~40-60% reduction in that zone's duration (~10-15ms savings), translating to ~3-5% improvement in total ledger close time.
