# H011: Diagnostic Event Encoding Is Not Hot in Apply-Load Benchmark Mode

**Date**: 2026-04-10
**Subsystem**: soroban-env
**Severity**: Informational
**Impact**: CPU / diagnostics
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

If diagnostic-event bridging were a meaningful optimization target for
apply-load, the benchmark success path would need to spend measurable time
encoding or decoding diagnostic events for each Soroban invocation.

## Mechanism

The suspected waste was that `encode_diagnostic_events()` is called
unconditionally in the Rust bridge output path, so benchmark runs might still be
serializing `DiagnosticEvent` XDR even when diagnostics are nominally disabled.

## Trigger

Run the standard apply-load matrix with the stock benchmark configs and inspect
the success path through `invoke_host_function_or_maybe_panic()`.

## Target Code

- `docs/apply-load-benchmark-sac.cfg:18-20,50` — metrics off, metadata off, diagnostics off for SAC benchmark
- `docs/apply-load-benchmark-token.cfg:18-20,44` — same for custom-token / soroswap template
- `src/rust/src/soroban_proto_any.rs:248-258` — `encode_diagnostic_events()`
- `src/rust/src/soroban_proto_any.rs:431-467` — diagnostics vector starts empty and is only logged/returned afterward
- `src/rust/src/soroban_proto_any.rs:498-516` — success path still calls `encode_diagnostic_events(&diagnostic_events)`
- `src/transactions/InvokeHostFunctionOpFrame.cpp:87-101` — C++ decoding of returned diagnostic events is also config-gated

## Evidence

The Rust bridge does call `encode_diagnostic_events()` on the success path, and
the C++ side has a matching diagnostic-event decode hook. At first glance that
looks like avoidable work in benchmark mode because the benchmark config turns
diagnostics off.

## Anti-Evidence

With `ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = false`, the benchmark success path does
not populate `diagnostic_events`, so `encode_diagnostic_events()` iterates an
empty vector and performs no XDR serialization. The C++ side also returns early
before decoding any output diagnostics, and the benchmark asserts successful
transactions, so the failure-only diagnostic path is not exercised.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The apparent overhead disappears under the actual benchmark configuration:
diagnostics are disabled in both benchmark config templates, so the success path
returns an empty diagnostic-event vector and the encode/decode helpers do
effectively no work.

### Lesson Learned

For apply-load optimization work, verify whether diagnostics are merely compiled
in or actually populated by the benchmark config. Unconditional helper calls are
not useful targets if the associated vectors are empty on the measured path.
