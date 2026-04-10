# H028: Avoid Per-TX `Instant::now()` Timing Calls When Metrics Disabled

**Date**: 2025-07-18
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: CPU / redundant syscalls
**Hypothesis by**: claude-opus-4.6, high

## Expected Behavior

When Soroban metrics are disabled (`DISABLE_SOROBAN_METRICS_FOR_TESTING = true`
in benchmark config), the two `Instant::now()` calls bracketing the host
invocation should be skipped, since `time_nsecs` is only consumed by
`HostFunctionMetrics` (which is disabled) and diagnostic events (which are
also disabled in the benchmark).

## Mechanism

In `invoke_host_function_or_maybe_panic` (soroban_proto_any.rs:441, 459-460),
two `Instant::now()` calls bracket the host invocation to measure execution
time. On Linux, `Instant::now()` calls `clock_gettime(CLOCK_MONOTONIC)` via
the vDSO, costing ~20-40 ns per call. The result `time_nsecs` is passed back
to C++ in `InvokeHostFunctionOutput` and stored in
`mMetrics.mInvokeTimeNsecs` (InvokeHostFunctionOpFrame.cpp:556), which is
only used in `maybePopulateMetricsInDiagnosticEvents` (line 869) — a function
that returns immediately when `ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = false`.

## Trigger

Run SAC @ 3200 TXs. Two `Instant::now()` calls per TX: 3200 × 60 ns = ~192 μs
per ledger. At baseline ~2500 ms (T=1): 0.008%. At T=8 ~350 ms: 0.055%.

## Target Code

- `src/rust/src/soroban_proto_any.rs:441` — `let start_time = Instant::now();`
- `src/rust/src/soroban_proto_any.rs:459-460` — `let stop_time = Instant::now(); let time_nsecs = ...`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:556` — `mMetrics.mInvokeTimeNsecs = out.time_nsecs;`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:869` — only consumer, guarded by diagnostic events check

## Evidence

The timing values are computed unconditionally on the Rust side but their only
consumer on the C++ side (`maybePopulateMetricsInDiagnosticEvents`) has an
early return when diagnostics are disabled. The Rust side cannot know whether
diagnostics are enabled since `enable_diagnostics` controls Soroban host
diagnostics, not the C++ metrics system.

## Anti-Evidence

Two `Instant::now()` calls cost ~40-80 ns total per TX. Over 3200 TXs, total
is ~128-256 μs — well below 0.01% of baseline. Even eliminating them entirely
would be unmeasurable.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2025-07-18
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The per-TX cost of two `Instant::now()` calls (~40-80 ns) is negligible. Total
savings of ~128-256 μs per ledger represent <0.01% of baseline, which is
several orders of magnitude below the benchmark noise floor. Additionally, the
timing data may be useful for internal performance monitoring even when not
published as diagnostic events.

### Lesson Learned

Per-TX timing overhead from `clock_gettime(CLOCK_MONOTONIC)` via vDSO is
extremely cheap (~20-40 ns per call) and not worth optimizing. The total
contribution of timing calls to ledger close time is negligible even at
3200 TX/ledger.
