# H015: Diagnostic-Event Bridge Batching For Apply-Load

**Date**: 2026-04-10
**Subsystem**: crypto, rust
**Severity**: Low
**Impact**: disabled-path bridge cleanup
**Hypothesis by**: gpt-5.4, high

## Expected Behavior

An apply-load optimization should target work that actually runs during the
benchmark. If diagnostic events are disabled in the benchmark configs, then
batching or optimizing their bridge representation should not be treated as a
viable apply-load performance finding.

## Mechanism

Rust represents diagnostic events as `Vec<RustBuf>`, and `encode_diagnostic_events`
would allocate one buffer per encoded event if diagnostics were active. But the
benchmark configs set `ENABLE_SOROBAN_DIAGNOSTIC_EVENTS = false`, so successful
apply-load runs keep `diagnostic_events` empty and C++ returns early from
`maybePopulateOutputDiagnosticEvents`.

## Trigger

Run any default apply-load scenario with the stock benchmark configs and inspect
the diagnostic-event path. The collection stays empty on the success path and
the C++ consumer immediately returns without decoding anything.

## Target Code

- `docs/apply-load-benchmark-sac.cfg:50` — benchmark disables Soroban diagnostic events
- `docs/apply-load-benchmark-token.cfg:44` — token benchmark config also disables diagnostic events
- `src/rust/src/soroban_proto_any.rs:248-259` — diagnostic encoding path uses `Vec<RustBuf>`
- `src/rust/src/soroban_proto_any.rs:522-555` — failure diagnostics are only added when `enable_diagnostics` is true
- `src/transactions/InvokeHostFunctionOpFrame.cpp:141-156` — C++ skips output diagnostic-event decoding when diagnostics are disabled

## Evidence

The benchmark configs explicitly disable diagnostic events, and the C++ consumer
guards its decode loop with the same config flag. On the common successful
apply-load path, the bridge work here is an empty-vector fast path.

## Anti-Evidence

This code could matter in debugging runs or failure-heavy workloads with
diagnostics enabled, and batching it might still be a sensible cleanup. It just
does not match the measured apply-load objective.

---

## Review

**Verdict**: NOT_VIABLE
**Date**: 2026-04-10
**Failed At**: hypothesis
**Novelty**: PASS — not previously investigated

### Why It Failed

The benchmark disables the feature entirely, so optimizing its representation
cannot improve benchmark throughput. Any change here would be dead-code cleanup
for apply-load, not a performance finding.

### Lesson Learned

On the bridge return path, only `contract_events` and `modified_ledger_entries`
are worth pursuing for apply-load. `diagnostic_events` must be screened against
the benchmark config first or the investigation becomes immediately out of
scope.
