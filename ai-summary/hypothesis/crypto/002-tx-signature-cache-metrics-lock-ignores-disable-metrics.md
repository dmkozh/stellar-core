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
