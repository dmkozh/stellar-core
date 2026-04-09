# H001: Serial preParallelApply dominates T=8 Soroban apply

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: High
**Impact**: parallel apply throughput / main-thread bottleneck
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

Once a Soroban tx set has already been partitioned into non-overlapping clusters, the T=8 apply-load benchmark should spend the bulk of its close time inside worker-thread `parallelApply`, not in a long main-thread prelude that walks every transaction one by one. The sequential pre-pass should be limited to the truly shared mutations that must be globally ordered.

## Mechanism

`GlobalParallelApplyLedgerState::preParallelApplyAndCollectModifiedClassicEntries` runs before any worker thread is launched and calls `tx->preParallelApply(...)` for every Soroban transaction in the stage. That path executes `commonPreApply` plus op-level `checkValid`, so the benchmark pays a fully serial per-tx validation/setup cost on the critical path even though apply-load has already prevalidated the generated txs and primed the signature cache. On the `T=8` scenarios this creates an Amdahl-law ceiling: worker parallelism improves host execution, but the main thread still linearly processes all 1600-6400 txs first.

## Trigger

Run `scripts/run_apply_load_matrix.py` and compare `sac,TX=6400,T=1` vs `sac,TX=6400,T=8` (or the `custom_token` / `soroswap` T=8 rows). Profile the main thread during ledger close; expect substantial time in `preParallelApply -> commonPreApply -> checkValid` before apply threads become busy.

## Target Code

- `src/transactions/ParallelApplyUtils.cpp:GlobalParallelApplyLedgerState::preParallelApplyAndCollectModifiedClassicEntries:324-385` - serial pre-pass across every tx in every stage
- `src/transactions/TransactionFrame.cpp:TransactionFrame::commonPreApply:2048-2123` - per-tx validation, fee/resource setup, seqnum/signature processing
- `src/transactions/TransactionFrame.cpp:TransactionFrame::preParallelApply:2126-2176` - serial op-level `checkValid` before worker launch
- `src/simulation/ApplyLoad.cpp:ApplyLoad::generateSacPayments:2136-2148` - benchmark explicitly prevalidates txs and primes the signature cache

## Evidence

The global parallel state constructor calls `preParallelApplyAndCollectModifiedClassicEntries`, and that helper performs a nested `for stage / for txBundle` walk that invokes `preParallelApply` on every Soroban tx before any thread state is created. The benchmark generator comments state that the up-front `checkValid()` pass is used to "prime the signature cache", meaning the close-time pre-pass is not even paying cold-signature cost.

## Anti-Evidence

`preParallelApply` does mutate fee-source accounts and sequence numbers, and the code comment explains it must run before later footprint collection because other tx footprints may read those classic entries. A viable optimization therefore has to preserve deterministic visibility of those shared classic-account updates rather than blindly moving the whole function to worker threads.

---

## Review

**Verdict**: VIABLE
**Severity**: Medium
**Date**: 2026-04-09
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

The `GlobalParallelApplyLedgerState` constructor (ParallelApplyUtils.cpp:300-322) calls `preParallelApplyAndCollectModifiedClassicEntries` which iterates all stages and all txBundles, calling `preParallelApply` on each transaction serially. Each `preParallelApply` invokes `commonPreApply` which creates a nested `LedgerTxn`, runs the full `commonValid` validation chain (including `commonValidPreSeqNum` with footprint dedup via UnorderedSet allocation, Soroban resource checks, source account loading, and cached signature verification), then mutates state via `processSeqNum` and `processSignatures`, commits the LedgerTxn, and finally runs op-level `checkValid`. This entire loop completes before any worker thread is launched via `applySorobanStageClustersInParallel`.

### Code Paths Examined

- `src/transactions/ParallelApplyUtils.cpp:324-386` — The serial pre-pass iterates all stages/txBundles, calling `preParallelApply` on each tx, then collects classic entries from footprints. Comment at line 358-362 explains ordering constraint.
- `src/transactions/TransactionFrame.cpp:2049-2123` (`commonPreApply`) — Creates SignatureChecker (uses cached hash), computes Soroban resource fee, opens LedgerTxn+Snapshot, calls `commonValid` (full validation), `processSeqNum` (mutates seq num), `processSignatures` (checks op sigs, removes one-time signers), commits LedgerTxn.
- `src/transactions/TransactionFrame.cpp:2126-2176` (`preParallelApply`) — After `commonPreApply` succeeds, calls `updateSorobanMetrics` (no-op in benchmark), then `mOperations.front()->checkValid(...)`. Comment at line 2160-2163 explicitly says this is serial "to avoid making OperationFrame::checkValid thread safe."
- `src/transactions/TransactionFrame.cpp:1319-1562` (`commonValidPreSeqNum`) — Protocol checks, `validateSorobanOpsConsistency`, `checkSorobanResources`, footprint dedup with UnorderedSet (O(footprint_size) per tx), time/ledger bounds, fee checks, source account load, frozen key check.
- `src/transactions/TransactionFrame.cpp:1666-1774` (`commonValid`) — Calls `commonValidPreSeqNum`, checks seq num, calls `checkAllTransactionSignatures` (cached), checks balance.
- `src/transactions/TransactionFrame.cpp:1565-1581` (`processSeqNum`) — Loads source account via LedgerTxn, updates seq num. This is the core serial mutation.
- `src/transactions/TransactionFrame.cpp:1584-1636` (`processSignatures`) — Creates LedgerSnapshot, checks op signatures, removes one-time signers, checks all signatures used.
- `src/ledger/LedgerManagerImpl.cpp:2534-2553` (`applySorobanStages`) — Constructs `GlobalParallelApplyLedgerState` (triggers the serial pre-pass), then iterates stages calling `applySorobanStage`.
- `src/ledger/LedgerManagerImpl.cpp:2427-2450` (`applySorobanStageClustersInParallel`) — Launches `std::async` threads per cluster; this is the parallel phase that happens AFTER the serial pre-pass.
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1282-1311` (`doCheckValidForSoroban`) — Only checks wasm size or asset validity; trivially thread-safe.
- `src/simulation/ApplyLoad.cpp:2136-2149` — Benchmark primes signature cache and validates txs before timing starts.

### Findings

**The bottleneck is real.** The serial pre-pass in `preParallelApplyAndCollectModifiedClassicEntries` performs substantial per-tx work: LedgerTxn creation/commit, full validation (including UnorderedSet-based footprint dedup), source account loading, cached signature verification, sequence number mutation, and op-level `checkValid`. For 6400 txs, this represents an estimated 200-500ms of serial main-thread time.

**The serial phase mixes parallelizable validation with necessary mutations.** Within `commonPreApply`, only `processSeqNum` and `processSignatures` truly require serial execution (they mutate shared ledger state that subsequent txs must see). The validation work — resource fee computation (pure function at line 2089), `checkSorobanResources` (pure validation at line 1407), footprint dedup (UnorderedSet allocation at lines 1464-1489), signature verification (cached), and op-level `checkValid` (trivially thread-safe per line 2160-2163 comment) — could be separated.

**Existing optimizations don't address this.** Signature cache priming (confirmed in ApplyLoad.cpp:2140) reduces per-signature cost but doesn't eliminate the serial iteration. Hash memoization (confirmed in TransactionFrame::getContentsHash) avoids re-hashing. Soroban metrics are disabled in benchmark. But the structural bottleneck — 6400 serial LedgerTxn cycles with validation — remains.

**Impact estimation**: For SAC payments at TX=6400/T=8, if host execution is ~1-2ms/tx (lightweight transfers), parallel time ≈ 800-1600ms. Serial pre-pass ≈ 200-500ms (30-80μs per tx × 6400). This yields a serial fraction of 15-35% of close time. Reducing the serial phase by 50-60% (moving validation to parallel) would save 100-300ms, improving total close time by 10-19%. This aligns with Medium severity. Profiling may reveal the actual fraction is higher for lightweight workloads, potentially reaching High.

### PoC Guidance

- **Target code**: `src/transactions/TransactionFrame.cpp` (preParallelApply, commonPreApply), `src/transactions/ParallelApplyUtils.cpp` (preParallelApplyAndCollectModifiedClassicEntries)
- **Change description**: Split `preParallelApply` into two phases: (1) A parallel pre-validation pass that computes resource fees, validates Soroban resources, performs footprint dedup, and runs op-level `checkValid` for all txs concurrently; (2) A minimal serial mutation pass that only opens a LedgerTxn, loads the source account, checks seq num, calls `processSeqNum`, `processSignatures`, commits, and records meta. The op-level `checkValid` for `InvokeHostFunctionOpFrame::doCheckValidForSoroban` (line 1282) only checks wasm size or asset validity — trivially thread-safe and should be moved to the parallel phase or done in the pre-validation pass.
- **Correctness check**: Existing tests covering parallel apply: search for `[soroban]` tag tests, `ParallelSorobanLedgerClose` tests, and the apply-load benchmark itself. The key invariant to preserve is that `processSeqNum` must see the committed state from all previously-processed txs (the ordering within the serial loop must be maintained for mutations).
- **Benchmark focus**: Run `scripts/run_apply_load_matrix.py` comparing T=1 vs T=8 for `sac,TX=6400`. The metric to improve is total ledger close time at T=8. Profile the serial pre-pass duration (TracyZone `preParallelApply`) vs. the parallel `parallelApply` duration. Target: 10-20% reduction in T=8 close time for SAC workload.
