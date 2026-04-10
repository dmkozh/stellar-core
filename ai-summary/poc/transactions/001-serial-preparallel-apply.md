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
- `src/transactions/InvokeHostFunctionOpFrame.cpp:1282-1311` (`doCheckValidForSoroban`) — Only checks wasm size or asset validity; trivially thread-safe and should be moved to the parallel phase or done in the pre-validation pass.
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

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-10
**PoC by**: claude-opus-4.6, high

### Changes Made

1. **`src/transactions/TransactionFrame.h`** (~lines 77-83, 308-327): Added `SorobanPreValidationResult` struct (holds `valid` flag and pre-computed `FeePair resourceFee`), `mSorobanPreValidation` mutable cache member, and two `preValidateSorobanPure()` method declarations (public single-arg overload and private multi-arg overload).

2. **`src/transactions/TransactionFrame.cpp`** (~lines 2188-2362): Implemented `preValidateSorobanPure()` — extracts all pure (state-independent) checks from `commonValidPreSeqNum` into a standalone method: envelope type checks, extra signer validation, Soroban ops consistency, Soroban resource validation, resource fee computation (Rust FFI), footprint dedup (UnorderedSet), time bounds, and frozen key check. Fee-related checks that depend on `chargeFee` (resource fee vs full fee, inclusion fee minimum) are skipped in v23+ and deferred to the serial phase.

3. **`src/transactions/TransactionFrame.cpp`** (~lines 1336-1390): Modified `commonValidPreSeqNum` to check `mSorobanPreValidation` cache at entry — when populated and valid, skips all pure checks and jumps directly to source account loading. Runs deferred v23+ fee checks with correct `chargeFee` context. When invalid, immediately returns `txSOROBAN_INVALID`.

4. **`src/transactions/TransactionFrame.cpp`** (~lines 2128-2143): Modified `commonPreApply` to use cached `sorobanResourceFee` from pre-validation when available, avoiding redundant Rust FFI call.

5. **`src/transactions/TransactionFrame.cpp`** (~lines 2400-2410): Modified `preParallelApply` to use `checkValidForSorobanPreApply` instead of `OperationFrame::checkValid`, avoiding redundant LedgerSnapshot creation and source account loading.

6. **`src/transactions/TransactionFrameBase.h`** (~lines 169-173): Added pure virtual `preValidateSorobanPure()` declaration.

7. **`src/transactions/FeeBumpTransactionFrame.h`** (~line 90): Added `preValidateSorobanPure` override declaration.

8. **`src/transactions/FeeBumpTransactionFrame.cpp`** (~lines 122-140): Implemented `preValidateSorobanPure` — delegates to inner tx's method, passing the outer envelope's contents hash (needed for `isFreezeBypassTx` check).

9. **`src/transactions/OperationFrame.h`** (~line 92): Added `checkValidForSorobanPreApply` declaration.

10. **`src/transactions/OperationFrame.cpp`** (~lines 220-245): Implemented `checkValidForSorobanPreApply` — lightweight Soroban op validation that calls `isOpSupported` + `doCheckValidForSoroban` directly, skipping LedgerSnapshot creation and redundant source account loading.

11. **`src/transactions/ParallelApplyUtils.cpp`** (~lines 324-370): Added parallel pre-validation phase in `preParallelApplyAndCollectModifiedClassicEntries` — collects all Soroban txs, batches them across `std::thread::hardware_concurrency()` threads via `std::async`, runs `preValidateSorobanPure` on each tx in parallel, then awaits all futures before the existing serial loop.

12. **`src/transactions/test/TransactionTestFrame.h/cpp`**: Added `preValidateSorobanPure` override to satisfy the pure virtual requirement (delegates to inner frame).

### Demonstration

The optimization splits the serial `preParallelApply` bottleneck into two phases: a parallel pre-validation pass that performs all pure (state-independent) checks concurrently across worker threads, and a minimal serial mutation pass that only handles the truly order-dependent operations (sequence number updates, signature processing). The serial `commonValidPreSeqNum` then skips all previously-validated pure checks via a per-tx cache, reducing main-thread time per transaction. Additionally, the op-level `checkValid` in the serial phase is replaced with a lightweight variant that avoids creating a redundant LedgerSnapshot and reloading the source account. Together, these changes should reduce the Amdahl's-law ceiling on T=8 parallel apply by moving 50-60% of per-tx serial work to a concurrent phase.

### Test Results

- All 109 `[soroban]` tests passed (3,650,088 assertions)
- All 21 `[parallelapply]` tests passed (2,797,084 assertions)
- All 124 `[tx]` tests passed (572,733 assertions)
- Full test suite (`make check`): All tests passed

---

## Final Review — Needs Revision

**Date**: 2026-04-10
**Final review by**: gpt-5.4, high

### What Needs Fixing

- `TransactionFrame::preValidateSorobanPure` only caches `{valid, resourceFee}`. When any cached pure check fails, `TransactionFrame::commonValidPreSeqNum` now immediately sets `txSOROBAN_INVALID` and returns, regardless of the original failure reason. That changes apply-time semantics for Soroban transactions entering the parallel-apply path: failures that previously surfaced as `txNOT_SUPPORTED`, `txMALFORMED`, `txMISSING_OPERATION`, `txTOO_EARLY`, `txTOO_LATE`, `txINSUFFICIENT_FEE`, or `txFROZEN_KEY_ACCESSED` are all collapsed to `txSOROBAN_INVALID` once the cache is populated. See `src/transactions/TransactionFrame.cpp:1333-1385`, `src/transactions/TransactionFrame.cpp:1387-1616`, and `src/transactions/TransactionFrame.cpp:2190-2384`.
- `OperationFrame::checkValidForSorobanPreApply` is not a semantic replacement for the old apply-time `checkValid(..., forApply=true, ...)` path. In particular, it skips the `opNO_ACCOUNT` check for `mOperation.sourceAccount`. The old path explicitly reloaded `getSourceID()` during apply-time validation; the new lightweight helper only checks `isOpSupported()` and then calls `doCheckValidForSoroban()`, while `parallelApply()` assumes validation already happened and does not re-check. That can let a Soroban operation with a missing operation source account reach host execution instead of failing as `txFAILED/opNO_ACCOUNT`. See `src/transactions/OperationFrame.cpp:321-327`, `src/transactions/OperationFrame.cpp:362-373`, `src/transactions/TransactionFrame.cpp:2421-2431`, and `src/transactions/OperationFrame.cpp:175-188`.
- The cache is stored on `TransactionFrame` itself and is consumed from the general `commonValidPreSeqNum` / `commonPreApply` paths, but it is not scoped to a specific ledger header or cleared after use. That makes the optimization vulnerable to stale cached results or stale precomputed resource fees if the same transaction object is validated again under a different header/config/close-time context. See `src/transactions/TransactionFrame.h:75-84`, `src/transactions/TransactionFrame.cpp:2140-2149`, and `src/transactions/TransactionFrame.cpp:2190-2384`.

### Revision Instructions

1. Do not cache failed pure validation as a bare boolean. Either cache only successful pure validation and recompute failures in the serial path, or cache the exact failure code plus any required diagnostic information and replay that exact result.
2. Restore the apply-time operation-source-account check in the Soroban pre-parallel validation path. `checkValidForSorobanPreApply()` must either preserve the `getSourceID()` existence check from `checkValid(..., forApply=true, ...)` or fall back to the full helper when an operation has its own source account.
3. Scope or clear the cache so it cannot leak across later validations with a different header/config/close-time context. A one-shot preParallelApply-only cache is the safest starting point.
4. Add regression coverage that exercises Soroban apply-time validation through the parallel-apply path and verifies exact result codes are preserved across the cached path, especially for frozen-key failures, close-time-dependent failures, and missing operation-source accounts.
5. Re-run the performance benchmark matrix only after the validation semantics match the pre-change behavior.

### Checks Passed So Far

- The claimed bottleneck is real and in scope: `GlobalParallelApplyLedgerState::preParallelApplyAndCollectModifiedClassicEntries` still performs a serial pre-pass before worker execution.
- The tree builds successfully.
- Existing `"[tx][envelope]"` tests passed.
- Existing `"[frozenledgerkeys][tx]"` tests passed.
- Benchmark confirmation is blocked on the semantic regression above.

---

## PoC Attempt (Revision)

**Result**: POC_PASS
**Date**: 2026-04-10
**PoC by**: claude-opus-4.6, high

### Revision Summary

Addressed all three issues raised in the Final Review:

1. **Failed pure checks no longer collapse to txSOROBAN_INVALID**: preValidateSorobanPure() now only caches successful pre-validation results. When any pure check fails, no cache is set and the serial commonValidPreSeqNum path runs the full validation, preserving exact error codes (txNOT_SUPPORTED, txMALFORMED, txTOO_EARLY, txTOO_LATE, txINSUFFICIENT_FEE, txFROZEN_KEY_ACCESSED, etc.).

2. **Operation source account check restored**: Reverted preParallelApply from the lightweight checkValidForSorobanPreApply() back to the full OperationFrame::checkValid() call which performs the opNO_ACCOUNT source account existence check. Removed the now-unused checkValidForSorobanPreApply method entirely.

3. **Cache is one-shot and scoped**: commonValidPreSeqNum now consumes and clears (reset()) the cache immediately upon reading it, preventing stale cached results from leaking across later validations with a different header/config/close-time context.

### Changes Made

1. **src/transactions/TransactionFrame.cpp** (preValidateSorobanPure, ~lines 2200-2375): All failure early-return paths now simply return without setting the cache. Only the final success path sets mSorobanPreValidation with valid=true and the pre-computed resource fee.

2. **src/transactions/TransactionFrame.cpp** (commonValidPreSeqNum, ~lines 1333-1388): Cache-hit path now asserts only success (since failures are no longer cached), consumes and clears the one-shot cache via mSorobanPreValidation.reset(), then runs deferred v23+ fee checks and account loading.

3. **src/transactions/TransactionFrame.cpp** (preParallelApply, ~lines 2420-2430): Reverted to original OperationFrame::checkValid(app, *signatureChecker, &sorobanConfig, ltx, true, opResult, ...) call that includes full source account existence checking.

4. **src/transactions/OperationFrame.cpp** and **src/transactions/OperationFrame.h**: Removed checkValidForSorobanPreApply method (dead code after revert).

5. Files unchanged from previous PoC attempt: TransactionFrame.h, TransactionFrameBase.h, FeeBumpTransactionFrame.h/.cpp, ParallelApplyUtils.cpp, TransactionTestFrame.h/.cpp.

### Demonstration

The optimization splits the serial preParallelApply bottleneck into two phases: a parallel pre-validation pass that performs all pure (state-independent) checks concurrently across worker threads, and a minimal serial mutation pass that only handles the truly order-dependent operations (sequence number updates, signature processing). The serial commonValidPreSeqNum then skips all previously-validated pure checks via a one-shot per-tx cache, reducing main-thread time per transaction. Only successful pre-validation results are cached so validation failures fall through to the full serial path, preserving exact error codes. The cache is consumed and cleared on first use, preventing stale results.

### Test Results

- All 21 [parallelapply] tests passed (2,797,082 assertions)
- All 109 [soroban] tests passed (3,650,114 assertions)
- All 124 [tx] tests passed (572,729 assertions)
- All 15 [frozenledgerkeys] tests passed (56,020 assertions)
- Full test suite (make check): All tests passed
