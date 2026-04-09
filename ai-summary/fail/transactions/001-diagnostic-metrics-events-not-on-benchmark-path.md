# H001: Diagnostic metrics event emission is not on the benchmark path

**Date**: 2026-04-09
**Subsystem**: transactions
**Severity**: Low
**Impact**: diagnostic event emission
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

The apply-load benchmark should not spend measurable time building Soroban diagnostic output or synthetic metrics events when the benchmark config has diagnostics disabled. In the stock benchmark configuration, those helper paths should short-circuit before allocating or appending any diagnostic events.

## Mechanism

I considered whether `maybePopulateOutputDiagnosticEvents` / `maybePopulateMetricsInDiagnosticEvents` were inflating host-function cost. The actual behavior matches the expected behavior: both paths immediately return when `ENABLE_SOROBAN_DIAGNOSTIC_EVENTS` is false, and the benchmark template explicitly sets that flag false while also disabling Soroban metrics for testing.

## Trigger

Run `scripts/run_apply_load_matrix.py` with `docs/apply-load-benchmark-sac.cfg` or the matrix script defaults.

## Target Code

- `src/transactions/InvokeHostFunctionOpFrame.cpp:maybePopulateOutputDiagnosticEvents:87-102` - early-return on disabled diagnostics
- `src/transactions/InvokeHostFunctionOpFrame.cpp:InvokeHostFunctionApplyHelper::maybePopulateMetricsInDiagnosticEvents:831-881` - early-return on disabled diagnostics
- `docs/apply-load-benchmark-sac.cfg:13-22,49-50` - benchmark disables metrics and diagnostic events

## Evidence

The helper functions both check `cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS` before doing any event decoding or synthetic metric-event creation. The benchmark template sets `DISABLE_SOROBAN_METRICS_FOR_TESTING = true`, `METADATA_OUTPUT_STREAM = ""`, and `ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = false`, so this path is dormant in the measured workload.

## Anti-Evidence

If a developer intentionally turns diagnostics on, the path would become hot immediately and could be worth optimizing. That is not the target benchmark configuration for this objective.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-09
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The benchmark config already disables the feature behind the suspected overhead, and the implementation has explicit early returns before any expensive event-building work happens.

### Lesson Learned

For apply-load optimization work, always confirm the template config first; many seemingly expensive event and metrics paths are compiled in but disabled at runtime.
