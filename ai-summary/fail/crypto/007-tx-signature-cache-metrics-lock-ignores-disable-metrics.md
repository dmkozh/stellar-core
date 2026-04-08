# H002: Tx Signature-Cache Metrics Stay On In Metrics-Off Benchmarks

**Date**: 2026-04-08
**Subsystem**: crypto
**Severity**: Medium
**Impact**: lock contention and wasted metrics work
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

When `run_apply_load_matrix.py` runs with `DISABLE_SOROBAN_METRICS_FOR_TESTING=true`, the check-valid/apply path should not pay per-signature synchronization costs for metrics that are only flushed into meters later. Disabling benchmark metrics should remove both Soroban metric updates and tx-signature-cache metric bookkeeping from the critical path.

## Mechanism

The benchmark driver sets `disable_metrics=True` by default and writes `DISABLE_SOROBAN_METRICS_FOR_TESTING` into the config, and `TransactionFrame::updateSorobanMetrics` honors that flag. But `SignatureChecker` still calls `updateTxSigCacheMetrics` after each Ed25519 verification result, and that helper always locks `gCheckValidOrApplyTxSigCacheMetricsMutex` while incrementing global counters that are later flushed by `ApplicationImpl::syncOwnMetrics`.

## Trigger

Run any default apply-load scenario, especially the `T=8` ones, with `DISABLE_SOROBAN_METRICS_FOR_TESTING=true`. Profile lock contention or samples in `SignatureChecker::updateTxSigCacheMetrics` and compare against a build that disables tx signature-cache metrics during benchmark runs.

## Target Code

- `scripts/run_apply_load_matrix.py:37` - `disable_metrics` defaults to `True`
- `scripts/run_apply_load_matrix.py:269-275` - benchmark config writes `DISABLE_SOROBAN_METRICS_FOR_TESTING`
- `src/transactions/TransactionFrame.cpp:1085-1092` - Soroban metrics respect the disable flag
- `src/transactions/SignatureChecker.cpp:117-135` - `checkSignature` updates tx-cache metrics on each Ed25519 result
- `src/transactions/SignatureChecker.cpp:185-204` - `updateTxSigCacheMetrics` locks a global mutex
- `src/main/ApplicationImpl.cpp:1309-1326` - counters are only consumed later during metric sync

## Evidence

`mTrackCacheMetrics` defaults to `true`, and the normal check-valid/apply constructors do not disable it. The code therefore keeps a global-lock metrics path alive even in the same benchmark configurations that explicitly skip Soroban metric updates and timers.

## Anti-Evidence

Only Ed25519 and signed-payload verifications that reach the cache path increment these counters; malformed signatures or hint mismatches return `NO_LOOKUP`. If benchmark transactions mostly short-circuit before the cache path, the contention will be smaller.

---

## Review

**Verdict**: VIABLE
**Severity**: Informational
**Date**: 2026-04-08
**Reviewed by**: claude-opus-4-6, high
**Novelty**: PASS — not previously investigated

### Trace Summary

Traced the full per-signature verification path from `SignatureChecker::checkSignature` through `SignatureUtils::verify` → `PubKeyUtils::verifySig` and back to `updateTxSigCacheMetrics`. Confirmed that `mTrackCacheMetrics` defaults to `true` and is never set to `false` in the apply path, even when `DISABLE_SOROBAN_METRICS_FOR_TESTING` is enabled. The only callers of `disableCacheMetricsTracking()` are `Peer.cpp:93` (overlay) and `FuzzerImpl.cpp:938` (fuzzer) — neither is the transaction apply path.

### Code Paths Examined

- `src/transactions/SignatureChecker.h:56` — `mTrackCacheMetrics` defaults to `true`
- `src/transactions/SignatureChecker.cpp:186-204` — `updateTxSigCacheMetrics` acquires `gCheckValidOrApplyTxSigCacheMetricsMutex` and increments counters; returns early only if `mTrackCacheMetrics` is false
- `src/transactions/SignatureChecker.cpp:117-135` — `checkSignature` calls `updateTxSigCacheMetrics` after each Ed25519 and signed-payload verification
- `src/transactions/TransactionFrame.cpp:2066` — apply path constructs `SignatureChecker` with default `mTrackCacheMetrics = true`; no code subsequently disables it based on `DISABLE_SOROBAN_METRICS_FOR_TESTING`
- `src/crypto/SecretKey.cpp:448-495` — `PubKeyUtils::verifySig` acquires `gVerifySigCacheMutex` 1–2 times per call (cache check + optional insert), performing BLAKE2 cache key computation and the actual Ed25519 verify on miss
- `src/overlay/Peer.cpp:93` — only non-test call site of `disableCacheMetricsTracking()`
- `src/main/ApplicationImpl.cpp:1309-1326` — `syncOwnMetrics` flushes both verify cache counters and tx-valid sig cache counters

### Findings

The inconsistency is confirmed: `DISABLE_SOROBAN_METRICS_FOR_TESTING` skips Soroban-specific metrics (timers, byte counters) but does not skip tx signature cache metrics tracking. The fix is trivial — call `signatureChecker->disableCacheMetricsTracking()` when the flag is set.

However, the actual performance impact is negligible and far below the claimed "Medium" severity:

1. **Lock cost is tiny**: The metrics mutex (`gCheckValidOrApplyTxSigCacheMetricsMutex`) is held only to increment 1–2 `uint64_t` counters (~10–30ns uncontended). With T=8, contention adds at most ~100–500ns per acquisition.

2. **Dominated by verify cache mutex**: Each signature verification already acquires `gVerifySigCacheMutex` 1–2 times (cache lookup, optional insert). This mutex has identical contention characteristics and is NOT disabled by any metrics flag. The metrics mutex is strictly additive.

3. **Dominated by actual verify**: On a cache miss, the Ed25519 signature verification itself costs ~50–100µs, making the ~100ns metrics lock overhead <0.2% of the verification cost.

4. **Low call frequency**: Benchmark transactions typically have 1 signature each. At T=8, each thread processes one signature per transaction — contention probability on a ~10ns critical section is extremely low.

5. **Fix doesn't address the larger bottleneck**: Even if the metrics lock is removed, the `gVerifySigCacheMutex` remains and has the same (or worse) contention profile.

Downgraded from Medium to **Informational**: the inconsistency is real and the fix is correct, but the benchmark improvement would be unmeasurable (<0.1%).

### PoC Guidance

- **Target code**: `src/transactions/TransactionFrame.cpp` — after constructing `SignatureChecker` in the apply path (~line 2066), add a check for `config.DISABLE_SOROBAN_METRICS_FOR_TESTING` and call `signatureChecker->disableCacheMetricsTracking()` if true. Same for the `checkValid` path (~line 1904).
- **Change description**: When `DISABLE_SOROBAN_METRICS_FOR_TESTING` is set, disable tx signature cache metrics tracking to be consistent with other metrics-off behavior.
- **Correctness check**: Existing tests for `SignatureChecker`, `TransactionFrame::checkValid`, and `TransactionFrame::apply` cover these paths. The change only affects metrics counters, not signature validation logic.
- **Benchmark focus**: Compare apply-load T=8 scenarios with and without the fix. The expected improvement is < 0.1% — likely within measurement noise. The value is consistency, not performance.

---

## PoC Attempt

**Result**: POC_PASS
**Date**: 2026-04-08
**PoC by**: claude-opus-4.6, high

### Changes Made

- `src/transactions/TransactionFrame.cpp` lines 1908–1911 (checkValid path): After constructing the `SignatureChecker` on the stack, added a check for `app.getConfig().DISABLE_SOROBAN_METRICS_FOR_TESTING` that calls `signatureChecker.disableCacheMetricsTracking()` when the flag is set.

- `src/transactions/TransactionFrame.cpp` lines 2077–2080 (apply/commonPreApply path): After constructing the `SignatureChecker` via `std::make_unique`, added the same check calling `signatureChecker->disableCacheMetricsTracking()` when `DISABLE_SOROBAN_METRICS_FOR_TESTING` is set.

### Demonstration

When `DISABLE_SOROBAN_METRICS_FOR_TESTING` is enabled, the tx signature cache metrics mutex (`gCheckValidOrApplyTxSigCacheMetricsMutex`) is no longer acquired on every signature verification in checkValid and apply paths. This makes the metrics-off behavior consistent: both Soroban metrics and tx signature cache metrics are now skipped together, eliminating unnecessary per-signature synchronization in benchmark configurations.

### Test Results

All 124 tests in the `[tx]` tag passed (572,146 assertions). All 16 tests in the `[crypto]` tag passed (15,262 assertions). No regressions detected.

---

## Final Review

**Verdict**: REJECTED
**Date**: 2026-04-08
**Final review by**: gpt-5.4, high
**Failed At**: final-review

### Adversarial Analysis

1. **Exercises claimed inefficiency**: PASS — the code change disables `SignatureChecker::updateTxSigCacheMetrics()` in both the `checkValid` and apply flows exactly where the hypothesis identified the extra mutex acquisition.
2. **Realistic preconditions**: PASS — `scripts/run_apply_load_matrix.py` sets `DISABLE_SOROBAN_METRICS_FOR_TESTING=true`, so the reviewed benchmark path does exercise the new branch.
3. **Inefficiency vs by-design**: PASS — these counters are only flushed later into metrics, so skipping them under an explicit metrics-off config does not change signature-validation semantics.
4. **Benchmark impact / severity**: FAIL — the independent benchmark run is mixed and net-unconvincing. Against `ai-summary/baseline.csv`, the highest-signature SAC scenarios regressed materially (`sac,TX=6400,T=1`: median **-8.23%**, p95 **-9.00%**, p99 **-7.96%**; `sac,TX=6400,T=8`: median **-7.40%**). The relevant parallel `custom_token,TX=3000,T=8` scenario also regressed across all metrics (median **-7.09%**, p95 **-8.52%**, p99 **-8.98%**). Some `soroswap` numbers improved (`T=8`: median **+9.03%**, p95 **+8.82%**, p99 **+5.50%**), but not in a pattern consistent with a fixed per-signature lock cost.
5. **In scope**: PASS — this is a C++ apply-path change in the benchmarked codebase and does not require soroban-env-host internal changes.
6. **Benchmark methodology**: PASS — stellar-core was rebuilt, the full existing test suite completed successfully, and the project benchmark tool was run exactly as required: `python3 scripts/run_apply_load_matrix.py --stellar-core-bin ./src/stellar-core --build-tag finalreview-txsig-metrics`, producing `/home/devbox/apply-load/finalreview-txsig-metrics-20260408-223713/results.csv`.
7. **Alternative explanations / attribution**: FAIL — this optimization removes a tiny per-signature cost, so any real win should correlate with signature volume. The data shows the opposite pattern: the most signature-dense SAC scenarios regress, while the largest gains appear in `soroswap`, the heaviest and lowest-transaction-count workload where signature bookkeeping should be least important. That mismatch strongly suggests normal benchmark variance or unrelated effects, not causal improvement from the change.
8. **Novelty**: PASS — no duplicate of this exact finding was identified in the existing crypto success/fail set.

### Rejection Reason

The code change is behavior-safe and does remove the claimed metrics mutex from the benchmark path, but the authoritative apply-load benchmark does not support it as a performance optimization. The observed wins are inconsistent with the mechanism and are outweighed by regressions in the scenarios that should benefit most from reduced per-signature overhead.

### Failed Checks

- 4
- 7
